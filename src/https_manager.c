#include "https_manager.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/gpio.h"

#include "lwip/altcp_tcp.h"
#include "lwip/altcp.h"
#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/altcp_tls.h"
#include "altcp_tls_mbedtls_structs.h"
#include "lwip/prot/iana.h"
#include "lwip/dns.h"
#include "mbedtls/ssl.h"

// Internal state structure
typedef struct {
    https_config_t config;
    https_state_t state;
    bool initialized;
    
    struct altcp_tls_config* tls_config;
    struct altcp_pcb* pcb;
    
    bool connected;
    bool request_sent;
    uint16_t bytes_received;
    
    uint32_t operation_start_time;
    https_post_data_t pending_data;
    
    ip_addr_t resolved_ip;
    bool dns_complete;
} https_manager_state_t;

// For mTLS with ATECC integration
typedef struct {
    mbedtls_ssl_config conf;
    mbedtls_x509_crt *cert;
    mbedtls_x509_crt *cert_chain;
    mbedtls_pk_context *pkey;
} altcp_tls_config_internal_t;

// Global state
static https_manager_state_t g_https_state = {
    .state = HTTPS_STATE_IDLE,
    .initialized = false,
    .tls_config = NULL,
    .pcb = NULL,
    .connected = false,
    .request_sent = false,
    .bytes_received = 0,
    .dns_complete = false
};

// Forward declarations
static void cleanup_connection(void);
static void update_leds(void);
static void dns_callback(const char* name, const ip_addr_t* ipaddr, void* arg);
static err_t https_connected_callback(void* arg, struct altcp_pcb* tpcb, err_t err);
static err_t https_recv_callback(void* arg, struct altcp_pcb* tpcb, struct pbuf* p, err_t err);
static void https_err_callback(void* arg, err_t err);

bool https_manager_init(const https_config_t* config)
{
    if (!config || !config->hostname || !config->ca_cert) {
        printf("HTTPS Manager: Invalid configuration\n");
        return false;
    }

    printf("HTTPS Manager: Initializing...\n");

    // Copy configuration
    g_https_state.config = *config;
    
    // Set default values
    if (g_https_state.config.port == 0) {
        g_https_state.config.port = 443;
    }
    if (g_https_state.config.operation_timeout_ms == 0) {
        g_https_state.config.operation_timeout_ms = 20000;
    }

    // Initialize LED pins if specified
    if (g_https_state.config.dns_led_pin > 0) {
        gpio_init(g_https_state.config.dns_led_pin);
        gpio_set_dir(g_https_state.config.dns_led_pin, GPIO_OUT);
        gpio_put(g_https_state.config.dns_led_pin, 0);
    }
    if (g_https_state.config.mtls_led_pin > 0) {
        gpio_init(g_https_state.config.mtls_led_pin);
        gpio_set_dir(g_https_state.config.mtls_led_pin, GPIO_OUT);
        gpio_put(g_https_state.config.mtls_led_pin, 0);
    }

    g_https_state.initialized = true;
    g_https_state.state = HTTPS_STATE_IDLE;
    
    printf("HTTPS Manager: Initialized for %s:%d\n", 
           g_https_state.config.hostname, 
           g_https_state.config.port);
    
    return true;
}

void https_manager_deinit(void)
{
    cleanup_connection();
    
    if (g_https_state.config.dns_led_pin > 0) {
        gpio_put(g_https_state.config.dns_led_pin, 0);
    }
    if (g_https_state.config.mtls_led_pin > 0) {
        gpio_put(g_https_state.config.mtls_led_pin, 0);
    }

    g_https_state.initialized = false;
    g_https_state.state = HTTPS_STATE_IDLE;
    
    printf("HTTPS Manager: Deinitialized\n");
}

bool https_manager_post_json(const https_post_data_t* data)
{
    if (!g_https_state.initialized) {
        printf("HTTPS Manager: Not initialized\n");
        return false;
    }

    if (g_https_state.state != HTTPS_STATE_IDLE) {
        printf("HTTPS Manager: Busy (state: %d)\n", g_https_state.state);
        return false;
    }

    printf("HTTPS Manager: POST[%lu]...\n", data->sample);
    
    // Save data for later use
    g_https_state.pending_data = *data;
    g_https_state.operation_start_time = to_ms_since_boot(get_absolute_time());
    g_https_state.bytes_received = 0;
    g_https_state.dns_complete = false;
    g_https_state.resolved_ip.addr = 0;
    
    // Reset LEDs
    update_leds();

    // Step 1: DNS Resolution
    g_https_state.state = HTTPS_STATE_DNS_RESOLVING;
    printf("HTTPS Manager: Resolving %s...\n", g_https_state.config.hostname);
    
    err_t dns_err = dns_gethostbyname(
        g_https_state.config.hostname,
        &g_https_state.resolved_ip,
        dns_callback,
        &g_https_state.resolved_ip
    );
    
    if (dns_err == ERR_INPROGRESS) {
        // DNS query in progress, wait for callback
        int timeout = 0;
        while (!g_https_state.dns_complete && timeout < 100) {
            cyw43_arch_poll();
            sleep_ms(100);
            timeout++;
        }
    } else if (dns_err == ERR_OK) {
        // Already cached
        g_https_state.dns_complete = true;
    }

    if (g_https_state.resolved_ip.addr == 0) {
        printf("HTTPS Manager: DNS resolution failed\n");
        g_https_state.state = HTTPS_STATE_ERROR;
        update_leds();
        cleanup_connection();
        return false;
    }

    printf("HTTPS Manager: Resolved to %s\n", ip4addr_ntoa(&g_https_state.resolved_ip));
    if (g_https_state.config.dns_led_pin > 0) {
        gpio_put(g_https_state.config.dns_led_pin, 1);
    }

    // Step 2: Create TLS Config
    g_https_state.state = HTTPS_STATE_CONNECTING;
    
    if (g_https_state.config.enable_mtls && g_https_state.config.client_cert) {
        printf("HTTPS Manager: Configuring mTLS...\n");
        g_https_state.tls_config = altcp_tls_create_config_client_2wayauth(
            g_https_state.config.ca_cert,
            g_https_state.config.ca_cert_len,
            NULL, 0,  // Private key handled separately
            NULL, 0,
            g_https_state.config.client_cert,
            g_https_state.config.client_cert_len
        );
    } else {
        printf("HTTPS Manager: Configuring TLS...\n");
        g_https_state.tls_config = altcp_tls_create_config_client(
            g_https_state.config.ca_cert,
            g_https_state.config.ca_cert_len
        );
    }

    if (!g_https_state.tls_config) {
        printf("HTTPS Manager: TLS config creation failed\n");
        g_https_state.state = HTTPS_STATE_ERROR;
        update_leds();
        cleanup_connection();
        return false;
    }

    // Inject ATECC PK context if mTLS is enabled
    if (g_https_state.config.enable_mtls && g_https_state.config.atecc_pk_context) {
        altcp_tls_config_internal_t* cfg_internal = 
            (altcp_tls_config_internal_t*)g_https_state.tls_config;
        
        if (cfg_internal != NULL) {
            mbedtls_ssl_conf_authmode(&cfg_internal->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            
            // Allocate client cert chain
            if (cfg_internal->cert_chain == NULL) {
                cfg_internal->cert_chain = (mbedtls_x509_crt*)malloc(sizeof(mbedtls_x509_crt));
                if (cfg_internal->cert_chain == NULL) {
                    printf("HTTPS Manager: Failed to allocate cert chain\n");
                    cleanup_connection();
                    return false;
                }
            }
            
            mbedtls_x509_crt_init(cfg_internal->cert_chain);
            
            // Parse client certificate
            int ret = mbedtls_x509_crt_parse(
                cfg_internal->cert_chain,
                g_https_state.config.client_cert,
                g_https_state.config.client_cert_len
            );
            
            if (ret != 0) {
                printf("HTTPS Manager: Failed to parse client cert: %d\n", ret);
                free(cfg_internal->cert_chain);
                cfg_internal->cert_chain = NULL;
                cleanup_connection();
                return false;
            }
            
            // Inject ATECC PK context
            cfg_internal->pkey = (mbedtls_pk_context*)g_https_state.config.atecc_pk_context;
            
            ret = mbedtls_ssl_conf_own_cert(
                &cfg_internal->conf,
                cfg_internal->cert_chain,
                cfg_internal->pkey
            );
            
            if (ret == 0) {
                printf("HTTPS Manager: ATECC PK context injected successfully\n");
            } else {
                printf("HTTPS Manager: ATECC injection failed: -0x%04x\n", -ret);
            }
        }
    }

    // Step 3: Create new PCB
    g_https_state.pcb = altcp_tls_new(g_https_state.tls_config, IPADDR_TYPE_V4);
    
    if (!g_https_state.pcb) {
        printf("HTTPS Manager: PCB creation failed\n");
        g_https_state.state = HTTPS_STATE_ERROR;
        update_leds();
        cleanup_connection();
        return false;
    }

    // Step 4: Set SNI hostname
    int mbedtls_err = mbedtls_ssl_set_hostname(
        &(((altcp_mbedtls_state_t*)(g_https_state.pcb->state))->ssl_context),
        g_https_state.config.hostname
    );

    if (mbedtls_err != 0) {
        printf("HTTPS Manager: SNI setup failed\n");
        g_https_state.state = HTTPS_STATE_ERROR;
        update_leds();
        cleanup_connection();
        return false;
    }

    // Step 5: Set callbacks
    g_https_state.connected = false;
    g_https_state.request_sent = false;
    
    altcp_arg(g_https_state.pcb, &g_https_state);
    altcp_err(g_https_state.pcb, https_err_callback);
    altcp_recv(g_https_state.pcb, https_recv_callback);

    printf("HTTPS Manager: Connecting to %s:%d...\n", 
           g_https_state.config.hostname, 
           g_https_state.config.port);
    
    // Step 6: Connect
    err_t connect_err = altcp_connect(
        g_https_state.pcb,
        &g_https_state.resolved_ip,
        g_https_state.config.port,
        https_connected_callback
    );

    if (connect_err != ERR_OK) {
        printf("HTTPS Manager: Connection failed: %d\n", connect_err);
        g_https_state.state = HTTPS_STATE_ERROR;
        update_leds();
        cleanup_connection();
        return false;
    }

    // Step 7: Wait for TLS handshake
    int timeout = 0;
    while (!g_https_state.connected && timeout < 1000) {
        cyw43_arch_poll();
        sleep_ms(100);
        timeout++;
    }

    if (!g_https_state.connected) {
        printf("HTTPS Manager: TLS handshake timeout\n");
        g_https_state.state = HTTPS_STATE_ERROR;
        update_leds();
        cleanup_connection();
        return false;
    }

    // Step 8: Build and send POST request
    g_https_state.state = HTTPS_STATE_SENDING;
    
    char json_body[256];
    int body_len = snprintf(json_body, sizeof(json_body),
                            "{\"sample\":%lu,\"timestamp\":%lu,\"device\":\"%s\","
                            "\"cpu\":%.1f,\"mem\":%.1f,\"disk\":%.1f,"
                            "\"net_in\":%.1f,\"net_out\":%.1f,\"proc\":%d}",
                            g_https_state.pending_data.sample,
                            g_https_state.pending_data.timestamp,
                            g_https_state.pending_data.device,
                            g_https_state.pending_data.cpu,
                            g_https_state.pending_data.memory,
                            g_https_state.pending_data.disk,
                            g_https_state.pending_data.net_in,
                            g_https_state.pending_data.net_out,
                            g_https_state.pending_data.processes);

    char request[2048];
    int req_len = snprintf(request, sizeof(request),
                           "POST /%s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %d\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "%s",
                           g_https_state.config.webhook_token,
                           g_https_state.config.hostname,
                           body_len,
                           json_body);

    printf("HTTPS Manager: Sending request...\n");

    err_t write_err = altcp_write(g_https_state.pcb, request, req_len, TCP_WRITE_FLAG_COPY);

    if (write_err == ERR_OK) {
        altcp_output(g_https_state.pcb);
        g_https_state.request_sent = true;
        g_https_state.state = HTTPS_STATE_RECEIVING;

        // Wait for response
        for (int i = 0; i < 500; i++) {
            cyw43_arch_poll();
            sleep_ms(100);
        }

        printf("HTTPS Manager: OK (%d bytes)\n", g_https_state.bytes_received);
        g_https_state.state = HTTPS_STATE_COMPLETE;
    } else {
        printf("HTTPS Manager: Write failed: %d\n", write_err);
        g_https_state.state = HTTPS_STATE_ERROR;
    }

    // Cleanup
    cleanup_connection();
    
    return (g_https_state.state == HTTPS_STATE_COMPLETE);
}

bool https_manager_is_busy(void)
{
    return g_https_state.state != HTTPS_STATE_IDLE && 
           g_https_state.state != HTTPS_STATE_COMPLETE &&
           g_https_state.state != HTTPS_STATE_ERROR;
}

https_state_t https_manager_get_state(void)
{
    return g_https_state.state;
}

uint16_t https_manager_get_bytes_received(void)
{
    return g_https_state.bytes_received;
}

void https_manager_abort(void)
{
    printf("HTTPS Manager: Aborting operation\n");
    cleanup_connection();
    g_https_state.state = HTTPS_STATE_IDLE;
}

void https_manager_task(void)
{
    if (!g_https_state.initialized) {
        return;
    }

    // Check for timeout
    if (https_manager_is_busy()) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        uint32_t elapsed = now - g_https_state.operation_start_time;
        
        if (elapsed > g_https_state.config.operation_timeout_ms) {
            printf("HTTPS Manager: Operation timeout (%lu ms)\n", elapsed);
            cleanup_connection();
            g_https_state.state = HTTPS_STATE_ERROR;
        }
    }

    // Auto-cleanup after completion or error
    if (g_https_state.state == HTTPS_STATE_COMPLETE || 
        g_https_state.state == HTTPS_STATE_ERROR) {
        
        static uint32_t cleanup_time = 0;
        uint32_t now = to_ms_since_boot(get_absolute_time());
        
        if (cleanup_time == 0) {
            cleanup_time = now;
        } else if (now - cleanup_time > 1000) {
            // Reset to idle after 1 second
            g_https_state.state = HTTPS_STATE_IDLE;
            cleanup_time = 0;
        }
    }
}

// Internal helper functions

static void cleanup_connection(void)
{
    // Close PCB
    if (g_https_state.pcb != NULL) {
        altcp_close(g_https_state.pcb);
        g_https_state.pcb = NULL;
    }
    
    // Free TLS config
    if (g_https_state.tls_config != NULL) {
        altcp_tls_free_config(g_https_state.tls_config);
        g_https_state.tls_config = NULL;
    }
    
    // Give lwIP time to clean up
    for (int i = 0; i < 5; i++) {
        cyw43_arch_poll();
        sleep_ms(50);
    }
    
    g_https_state.connected = false;
    g_https_state.request_sent = false;
}

static void update_leds(void)
{
    // DNS LED
    if (g_https_state.config.dns_led_pin > 0) {
        bool dns_on = (g_https_state.state >= HTTPS_STATE_CONNECTING);
        gpio_put(g_https_state.config.dns_led_pin, dns_on ? 1 : 0);
    }
    
    // mTLS LED
    if (g_https_state.config.mtls_led_pin > 0) {
        bool mtls_on = (g_https_state.state >= HTTPS_STATE_CONNECTED);
        gpio_put(g_https_state.config.mtls_led_pin, mtls_on ? 1 : 0);
    }
}

// Callback functions

static void dns_callback(const char* name, const ip_addr_t* ipaddr, void* arg)
{
    if (ipaddr) {
        ip_addr_t* result = (ip_addr_t*)arg;
        *result = *ipaddr;
        g_https_state.dns_complete = true;
        printf("HTTPS Manager: DNS resolved: %s\n", ip4addr_ntoa(ipaddr));
    } else {
        printf("HTTPS Manager: DNS resolution failed\n");
        g_https_state.dns_complete = true;
    }
}

static err_t https_connected_callback(void* arg, struct altcp_pcb* tpcb, err_t err)
{
    https_manager_state_t* state = (https_manager_state_t*)arg;
    
    if (err == ERR_OK) {
        state->connected = true;
        state->state = HTTPS_STATE_CONNECTED;
        printf("HTTPS Manager: TLS handshake complete!\n");
        
        if (g_https_state.config.mtls_led_pin > 0) {
            gpio_put(g_https_state.config.mtls_led_pin, 1);
        }
    } else {
        printf("HTTPS Manager: Connection failed: %d\n", err);
        state->state = HTTPS_STATE_ERROR;
        
        if (g_https_state.config.mtls_led_pin > 0) {
            gpio_put(g_https_state.config.mtls_led_pin, 0);
        }
    }
    
    return ERR_OK;
}

static err_t https_recv_callback(void* arg, struct altcp_pcb* tpcb, struct pbuf* p, err_t err)
{
    https_manager_state_t* state = (https_manager_state_t*)arg;
    
    if (p == NULL) {
        printf("HTTPS Manager: Connection closed by server\n");
        state->state = HTTPS_STATE_COMPLETE;
        return ERR_OK;
    }
    
    state->bytes_received += p->tot_len;
    
    altcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    
    return ERR_OK;
}

static void https_err_callback(void* arg, err_t err)
{
    printf("HTTPS Manager: Connection error: %d\n", err);
    https_manager_state_t* state = (https_manager_state_t*)arg;
    state->connected = false;
    state->state = HTTPS_STATE_ERROR;
    
    if (g_https_state.config.mtls_led_pin > 0) {
        gpio_put(g_https_state.config.mtls_led_pin, 0);
    }
}