#include "https_client.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/altcp_tls.h"
#include "altcp_tls_mbedtls_structs.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/debug.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ATECC608B functions - defined in main.c or atecc module */
extern bool g_atecc_pk_initialized;
extern mbedtls_pk_context g_atecc_pk_ctx;
extern bool init_atecc_pk_context(void);

typedef struct {
    mbedtls_ssl_config conf;
    mbedtls_x509_crt *cert;
    mbedtls_x509_crt *cert_chain;
    mbedtls_pk_context *pkey;
} altcp_tls_config_internal_t;

#define DNS_MAX_RETRIES             10U
#define DNS_RETRY_DELAY_MS          100U
#define TLS_HANDSHAKE_TIMEOUT_MS    100000U
#define TLS_HANDSHAKE_RETRY_DELAY_MS 100U

static https_client_state_t g_https_state = {0};
static https_config_t g_https_config = {0};
static https_response_callback_t g_response_callback = NULL;
static void *g_response_callback_arg = NULL;

/* LED pins for status indication */
static const uint8_t DNS_LED_PIN = 7;
static const uint8_t MTLS_LED_PIN = 8;

static void https_dns_callback(const char *name, const ip_addr_t *ipaddr, void *arg);
static err_t https_connected_callback(void *arg, struct altcp_pcb *tpcb, err_t err);
static err_t https_recv_callback(void *arg, struct altcp_pcb *tpcb, struct pbuf *p, err_t err);
static void https_err_callback(void *arg, err_t err);
static bool https_setup_tls_config(const https_config_t *config);
static bool https_integrate_atecc(void);
static void https_cleanup_connection(void);
static void mbedtls_debug_callback(void *ctx, int level, const char *file, int line, const char *str);

bool https_client_init(void)
{
    printf("HTTPS Client: Initializing...\n");
    
    /* Initialize state */
    memset(&g_https_state, 0, sizeof(https_client_state_t));
    memset(&g_https_config, 0, sizeof(https_config_t));
    
    g_https_state.status = HTTPS_STATUS_IDLE;
    g_https_state.is_connected = false;
    g_https_state.operation_in_progress = false;
    g_https_state.dns_resolved = false;
    
    printf("HTTPS Client: Initialized successfully\n");
    return true;
}

bool https_client_configure(const https_config_t *config)
{
    if (config == NULL)
    {
        printf("HTTPS Client: ERROR - Invalid configuration\n");
        return false;
    }
    
    if (g_https_state.operation_in_progress)
    {
        printf("HTTPS Client: ERROR - Operation in progress\n");
        return false;
    }
    
    /* Store configuration */
    memcpy(&g_https_config, config, sizeof(https_config_t));
    
    /* Set default port if not specified */
    if (g_https_config.port == 0)
    {
        g_https_config.port = HTTPS_DEFAULT_PORT;
    }
    
    printf("HTTPS Client: Configured for %s:%u\n", 
           g_https_config.hostname, 
           g_https_config.port);
    
    return true;
}

bool https_client_post_json(const char *hostname,
                            const char *path,
                            const char *json_body,
                            size_t body_len)
{
    /* Temporary configuration */
    https_config_t temp_config = {
        .hostname = hostname,
        .path = path,
        .port = HTTPS_DEFAULT_PORT,
        .ca_cert = g_https_config.ca_cert,
        .ca_cert_len = g_https_config.ca_cert_len,
        .client_cert = g_https_config.client_cert,
        .client_cert_len = g_https_config.client_cert_len,
        .use_atecc = g_https_config.use_atecc,
        .enable_mtls = g_https_config.enable_mtls
    };
    
    if (!https_client_configure(&temp_config))
    {
        return false;
    }
    
    return https_client_post(path, json_body, body_len);
}

bool https_client_post(const char *path,
                       const char *json_body,
                       size_t body_len)
{
    if (g_https_state.operation_in_progress)
    {
        printf("HTTPS Client: Operation already in progress\n");
        return false;
    }
    
    if (g_https_config.hostname == NULL || path == NULL || json_body == NULL)
    {
        printf("HTTPS Client: ERROR - Invalid parameters\n");
        return false;
    }
    
    printf("HTTPS Client: Starting POST to %s%s\n", g_https_config.hostname, path);
    
    g_https_state.operation_in_progress = true;
    g_https_state.operation_start_time = to_ms_since_boot(get_absolute_time());
    g_https_state.status = HTTPS_STATUS_RESOLVING_DNS;
    g_https_state.bytes_received = 0;
    
    /* Reset LEDs */
    gpio_put(DNS_LED_PIN, 0);
    gpio_put(MTLS_LED_PIN, 0);
    
    /* Step 1: DNS Resolution */
    printf("HTTPS Client: Resolving DNS for %s...\n", g_https_config.hostname);
    
    err_t dns_err = https_client_resolve_dns(g_https_config.hostname,
                                             &g_https_state.server_ip,
                                             HTTPS_DNS_TIMEOUT_MS);
    
    if (dns_err != ERR_OK)
    {
        printf("HTTPS Client: DNS resolution failed\n");
        gpio_put(DNS_LED_PIN, 0);
        g_https_state.status = HTTPS_STATUS_ERROR;
        g_https_state.operation_in_progress = false;
        return false;
    }
    
    gpio_put(DNS_LED_PIN, 1);
    g_https_state.dns_resolved = true;
    printf("HTTPS Client: DNS resolved to %s\n", ip4addr_ntoa(&g_https_state.server_ip));
    
    /* Step 2: Setup TLS configuration */
    g_https_state.status = HTTPS_STATUS_CONNECTING;
    
    if (!https_setup_tls_config(&g_https_config))
    {
        printf("HTTPS Client: TLS configuration failed\n");
        gpio_put(MTLS_LED_PIN, 0);
        g_https_state.status = HTTPS_STATUS_ERROR;
        g_https_state.operation_in_progress = false;
        return false;
    }
    
    /* Step 3: Create new PCB */
    g_https_state.pcb = altcp_tls_new(g_https_state.tls_config, IPADDR_TYPE_V4);
    
    if (g_https_state.pcb == NULL)
    {
        printf("HTTPS Client: Failed to create PCB\n");
        gpio_put(MTLS_LED_PIN, 0);
        https_cleanup_connection();
        g_https_state.status = HTTPS_STATUS_ERROR;
        g_https_state.operation_in_progress = false;
        return false;
    }
    
    /* Step 4: Set SNI hostname */
    int mbedtls_err = mbedtls_ssl_set_hostname(
        &(((altcp_mbedtls_state_t*)(g_https_state.pcb->state))->ssl_context),
        g_https_config.hostname
    );
    
    if (mbedtls_err != 0)
    {
        printf("HTTPS Client: Failed to set SNI hostname\n");
        gpio_put(MTLS_LED_PIN, 0);
        https_cleanup_connection();
        g_https_state.status = HTTPS_STATUS_ERROR;
        g_https_state.operation_in_progress = false;
        return false;
    }
    
    /* Step 5: Set callbacks */
    g_https_state.is_connected = false;
    g_https_state.request_sent = false;
    
    altcp_arg(g_https_state.pcb, &g_https_state);
    altcp_err(g_https_state.pcb, https_err_callback);
    altcp_recv(g_https_state.pcb, https_recv_callback);
    
    printf("HTTPS Client: Connecting to %s:443...\n", g_https_config.hostname);
    
    /* Step 6: Connect */
    err_t connect_err = altcp_connect(g_https_state.pcb,
                                     &g_https_state.server_ip,
                                     g_https_config.port,
                                     https_connected_callback);
    
    if (connect_err != ERR_OK)
    {
        printf("HTTPS Client: Connection failed: %d\n", connect_err);
        gpio_put(MTLS_LED_PIN, 0);
        https_cleanup_connection();
        g_https_state.status = HTTPS_STATUS_ERROR;
        g_https_state.operation_in_progress = false;
        return false;
    }
    
    /* Step 7: Wait for TLS handshake */
    uint32_t timeout = 0;
    while (!g_https_state.is_connected && timeout < (TLS_HANDSHAKE_TIMEOUT_MS / TLS_HANDSHAKE_RETRY_DELAY_MS))
    {
        cyw43_arch_poll();
        sleep_ms(TLS_HANDSHAKE_RETRY_DELAY_MS);
        timeout++;
    }
    
    if (!g_https_state.is_connected)
    {
        printf("HTTPS Client: TLS handshake timeout\n");
        gpio_put(MTLS_LED_PIN, 0);
        https_cleanup_connection();
        g_https_state.status = HTTPS_STATUS_ERROR;
        g_https_state.operation_in_progress = false;
        return false;
    }
    
    /* Step 8: Build and send HTTP request */
    g_https_state.status = HTTPS_STATUS_SENDING;
    
    char request[HTTPS_MAX_REQUEST_SIZE];
    int req_len = snprintf(request, sizeof(request),
                          "POST %s HTTP/1.1\r\n"
                          "Host: %s\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "\r\n"
                          "%s",
                          path, g_https_config.hostname, body_len, json_body);
    
    if (req_len < 0 || req_len >= (int)sizeof(request))
    {
        printf("HTTPS Client: Request too large\n");
        https_cleanup_connection();
        g_https_state.status = HTTPS_STATUS_ERROR;
        g_https_state.operation_in_progress = false;
        return false;
    }
    
    printf("HTTPS Client: Sending request (%d bytes)...\n", req_len);
    
    err_t write_err = altcp_write(g_https_state.pcb, request, req_len, TCP_WRITE_FLAG_COPY);
    
    if (write_err != ERR_OK)
    {
        printf("HTTPS Client: Write failed: %d\n", write_err);
        https_cleanup_connection();
        g_https_state.status = HTTPS_STATUS_ERROR;
        g_https_state.operation_in_progress = false;
        return false;
    }
    
    altcp_output(g_https_state.pcb);
    g_https_state.request_sent = true;
    g_https_state.status = HTTPS_STATUS_RECEIVING;
    
    /* Wait for response */
    uint32_t response_timeout = 0;
    uint32_t max_response_retries = HTTPS_REQUEST_TIMEOUT_MS / 100;
    
    while (g_https_state.status == HTTPS_STATUS_RECEIVING && 
           response_timeout < max_response_retries)
    {
        cyw43_arch_poll();
        sleep_ms(100);
        response_timeout++;
    }
    
    printf("HTTPS Client: Request complete (%u bytes received)\n", 
           g_https_state.bytes_received);
    
    /* Cleanup */
    https_cleanup_connection();
    
    g_https_state.status = HTTPS_STATUS_COMPLETE;
    g_https_state.operation_in_progress = false;
    
    return true;
}

https_status_t https_client_get_status(void)
{
    return g_https_state.status;
}

const https_client_state_t *https_client_get_state(void)
{
    return &g_https_state;
}

bool https_client_is_busy(void)
{
    return g_https_state.operation_in_progress;
}

void https_client_abort(void)
{
    if (g_https_state.operation_in_progress)
    {
        printf("HTTPS Client: Aborting operation\n");
        https_cleanup_connection();
        g_https_state.status = HTTPS_STATUS_IDLE;
        g_https_state.operation_in_progress = false;
    }
}

void https_client_deinit(void)
{
    https_client_abort();
    memset(&g_https_state, 0, sizeof(https_client_state_t));
    memset(&g_https_config, 0, sizeof(https_config_t));
    printf("HTTPS Client: Deinitialized\n");
}

err_t https_client_resolve_dns(const char *hostname,
                               ip_addr_t *ip_addr,
                               uint32_t timeout_ms)
{
    if (hostname == NULL || ip_addr == NULL)
    {
        return ERR_ARG;
    }
    
    ip_addr->addr = 0;
    
    err_t dns_err = dns_gethostbyname(hostname, ip_addr, https_dns_callback, ip_addr);
    
    if (dns_err == ERR_INPROGRESS)
    {
        /* Wait for DNS resolution */
        uint32_t start_time = to_ms_since_boot(get_absolute_time());
        
        while (ip_addr->addr == 0)
        {
            uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - start_time;
            
            if (elapsed >= timeout_ms)
            {
                return ERR_TIMEOUT;
            }
            
            cyw43_arch_poll();
            sleep_ms(DNS_RETRY_DELAY_MS);
        }
        
        return ERR_OK;
    }
    else if (dns_err == ERR_OK)
    {
        /* Already cached */
        return ERR_OK;
    }
    
    return dns_err;
}

uint16_t https_client_get_bytes_received(void)
{
    return g_https_state.bytes_received;
}

void https_client_reset(void)
{
    https_cleanup_connection();
    g_https_state.status = HTTPS_STATUS_IDLE;
    g_https_state.operation_in_progress = false;
    g_https_state.is_connected = false;
    g_https_state.request_sent = false;
    g_https_state.bytes_received = 0;
    g_https_state.dns_resolved = false;
}

void https_client_set_response_callback(https_response_callback_t callback,
                                       void *user_arg)
{
    g_response_callback = callback;
    g_response_callback_arg = user_arg;
}

static void https_dns_callback(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    if (ipaddr != NULL)
    {
        ip_addr_t *result = (ip_addr_t*)arg;
        *result = *ipaddr;
        gpio_put(DNS_LED_PIN, 1);
        printf("HTTPS Client: DNS resolved %s -> %s\n", name, ip4addr_ntoa(ipaddr));
    }
    else
    {
        gpio_put(DNS_LED_PIN, 0);
        printf("HTTPS Client: DNS resolution failed for %s\n", name);
    }
}

static err_t https_connected_callback(void *arg, struct altcp_pcb *tpcb, err_t err)
{
    https_client_state_t *state = (https_client_state_t*)arg;
    
    if (err == ERR_OK)
    {
        state->is_connected = true;
        gpio_put(MTLS_LED_PIN, 1);
        printf("HTTPS Client: TLS handshake complete!\n");
    }
    else
    {
        gpio_put(MTLS_LED_PIN, 0);
        printf("HTTPS Client: Connection failed with error: %d\n", err);
    }
    
    return ERR_OK;
}

static err_t https_recv_callback(void *arg, struct altcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    https_client_state_t *state = (https_client_state_t*)arg;
    
    if (p == NULL)
    {
        /* Connection closed by server */
        printf("HTTPS Client: Connection closed by server\n");
        state->is_connected = false;
        state->status = HTTPS_STATUS_COMPLETE;
        return ERR_OK;
    }
    
    /* Update bytes received */
    state->bytes_received += p->tot_len;
    
    /* Call user callback if registered */
    if (g_response_callback != NULL)
    {
        g_response_callback((const uint8_t*)p->payload, p->tot_len, g_response_callback_arg);
    }
    
    /* Acknowledge received data */
    altcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    
    return ERR_OK;
}

static void https_err_callback(void *arg, err_t err)
{
    printf("HTTPS Client: Connection error: %d\n", err);
    
    https_client_state_t *state = (https_client_state_t*)arg;
    state->is_connected = false;
    state->status = HTTPS_STATUS_ERROR;
    gpio_put(MTLS_LED_PIN, 0);
}

static bool https_setup_tls_config(const https_config_t *config)
{
    if (config == NULL)
    {
        return false;
    }
    
    printf("HTTPS Client: Setting up TLS configuration...\n");
    
    /* Create TLS configuration */
    if (config->enable_mtls && config->client_cert != NULL)
    {
        /* Mutual TLS configuration */
        printf("HTTPS Client: Configuring mTLS...\n");
        
        g_https_state.tls_config = altcp_tls_create_config_client_2wayauth(
            config->ca_cert, config->ca_cert_len,
            NULL, 0,  /* Private key handled by ATECC */
            NULL, 0,
            config->client_cert, config->client_cert_len
        );
    }
    else
    {
        /* Standard TLS configuration */
        printf("HTTPS Client: Configuring standard TLS...\n");
        
        g_https_state.tls_config = altcp_tls_create_config_client(
            config->ca_cert, config->ca_cert_len
        );
    }
    
    if (g_https_state.tls_config == NULL)
    {
        printf("HTTPS Client: Failed to create TLS config\n");
        return false;
    }
    
    /* Integrate ATECC608B if enabled */
    if (config->use_atecc && config->enable_mtls)
    {
        if (!https_integrate_atecc())
        {
            printf("HTTPS Client: WARNING - ATECC integration failed, using software crypto\n");
            /* Continue anyway - will fall back to software keys */
        }
    }
    
    printf("HTTPS Client: TLS configuration complete\n");
    return true;
}

static bool https_integrate_atecc(void)
{
    if (!g_atecc_pk_initialized)
    {
        printf("HTTPS Client: Initializing ATECC PK context...\n");
        
        if (!init_atecc_pk_context())
        {
            printf("HTTPS Client: ATECC PK context initialization failed\n");
            return false;
        }
    }
    
    /* Get internal TLS config structure */
    altcp_tls_config_internal_t *cfg_internal = 
        (altcp_tls_config_internal_t*)g_https_state.tls_config;
    
    if (cfg_internal == NULL)
    {
        printf("HTTPS Client: Invalid TLS config internal structure\n");
        return false;
    }
    
    /* Configure mbedTLS debugging */
    mbedtls_ssl_conf_dbg(&cfg_internal->conf, mbedtls_debug_callback, NULL);
    mbedtls_ssl_conf_authmode(&cfg_internal->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    
    /* Allocate client certificate chain if needed */
    if (cfg_internal->cert_chain == NULL)
    {
        cfg_internal->cert_chain = (mbedtls_x509_crt*)malloc(sizeof(mbedtls_x509_crt));
        
        if (cfg_internal->cert_chain == NULL)
        {
            printf("HTTPS Client: Failed to allocate memory for cert chain\n");
            return false;
        }
        
        printf("HTTPS Client: Allocated memory for cert chain\n");
    }
    
    /* Initialize and parse client certificate */
    mbedtls_x509_crt_init(cfg_internal->cert_chain);
    
    /* Need null-terminated certificate */
    uint8_t *client_cert_safe = (uint8_t*)malloc(g_https_config.client_cert_len + 1);
    if (client_cert_safe == NULL)
    {
        printf("HTTPS Client: Failed to allocate cert buffer\n");
        free(cfg_internal->cert_chain);
        cfg_internal->cert_chain = NULL;
        return false;
    }
    
    memcpy(client_cert_safe, g_https_config.client_cert, g_https_config.client_cert_len);
    client_cert_safe[g_https_config.client_cert_len] = '\0';
    
    int ret = mbedtls_x509_crt_parse(cfg_internal->cert_chain, 
                                     client_cert_safe, 
                                     g_https_config.client_cert_len + 1);
    
    free(client_cert_safe);
    
    if (ret != 0)
    {
        printf("HTTPS Client: Failed to parse client certificate: -0x%04x\n", -ret);
        free(cfg_internal->cert_chain);
        cfg_internal->cert_chain = NULL;
        return false;
    }
    
    printf("HTTPS Client: Client certificate parsed successfully\n");
    
    /* Inject ATECC private key context */
    cfg_internal->pkey = &g_atecc_pk_ctx;
    
    /* Configure SSL to use our certificate and ATECC key */
    ret = mbedtls_ssl_conf_own_cert(&cfg_internal->conf,
                                    cfg_internal->cert_chain,
                                    &g_atecc_pk_ctx);
    
    if (ret == 0)
    {
        printf("HTTPS Client: ✅ Successfully configured TLS with ATECC608B hardware signing\n");
        return true;
    }
    else
    {
        printf("HTTPS Client: ⚠️ ATECC injection failed: -0x%04x\n", -ret);
        printf("HTTPS Client: ⚠️ Falling back to software keys\n");
        return false;
    }
}

static void https_cleanup_connection(void)
{
    printf("HTTPS Client: Cleaning up connection...\n");
    
    /* Close PCB */
    if (g_https_state.pcb != NULL)
    {
        altcp_close(g_https_state.pcb);
        g_https_state.pcb = NULL;
    }
    
    /* Free TLS config */
    if (g_https_state.tls_config != NULL)
    {
        altcp_tls_free_config(g_https_state.tls_config);
        g_https_state.tls_config = NULL;
    }
    
    /* Give lwIP time to cleanup */
    for (int i = 0; i < 5; i++)
    {
        cyw43_arch_poll();
        sleep_ms(50);
    }
    
    g_https_state.is_connected = false;
}

static void mbedtls_debug_callback(void *ctx, int level, 
                                  const char *file, int line, 
                                  const char *str)
{
    ((void)ctx);
    ((void)level);
    printf("mbedTLS [%s:%d]: %s", file, line, str);
}