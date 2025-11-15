#include <pico/stdlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <bsp/board.h>
#include <tusb.h>
#include "hardware/gpio.h"
#include "hid_config.h"

#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
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
#include "https_config.h"

#include "hardware/i2c.h"
#include "cryptoauthlib.h"
#include "atca_basic.h"
#include "atca_mbedtls_wrap.h"
#include <mbedtls/error.h>
#include "mbedtls/ecdsa.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include <mbedtls/debug.h>

// Headers
#include "msc_manager.h"
#include "hid_manager.h"


#define MBEDTLS_ECDSA_SIGN_ALT

#define HID_BUTTON_PIN 20
#define WIFI_LED_PIN 6
#define DNS_LED_PIN 7
#define MTLS_LED_PIN 8


// ATECC Configuration
#define ATECC_BUTTON_PIN 22
#define I2C_BUS_ID      i2c0
#define I2C_SDA_PIN     4
#define I2C_SCL_PIN     5
#define I2C_BAUDRATE    100000

#define TARGET_SLOT 0
#define ECC_PUB_KEY_SIZE    64
#define ECC_SIGNATURE_SIZE  64
#define DIGEST_SIZE         32
#define RNG_SIZE           32

#define RX_BUFFER_SIZE 512
#define MAX_SEQ 512

#define DATA_TIMEOUT_MS 20000
#define WIFI_RECONNECT_DELAY_MS 5000

// MTLS CONFIGURATION
#define MTLS_ENABLED

// POST CONFIGURATION
#define AUTO_POST_ON_SAMPLE
#define MIN_POST_INTERVAL_MS 6000

// AUTO-HID TRIGGER CONFIGURATION
#define AUTO_TRIGGER_HID

typedef struct
{
    float cpu;
    float memory;
    float disk;
    float net_in;
    float net_out;
    int processes;
    bool valid;
} health_data_t;

typedef struct {
    struct altcp_tls_config* tls_config;
    struct altcp_pcb* pcb;
    bool connected;
    bool request_sent;
    bool operation_in_progress;
    uint16_t bytes_received;
    uint32_t operation_start_time;
    health_data_t pending_data;
} https_state_t;

typedef struct {
    mbedtls_ssl_config conf; //ca cert
    mbedtls_x509_crt *cert; // ca cert
    mbedtls_x509_crt *cert_chain;//client cert
    mbedtls_pk_context *pkey; //private  key we injecting
}altcp_tls_config_internal_t;
// GLOBAL VARIABLES

// Serial buffer
char rx_buffer[RX_BUFFER_SIZE];
int rx_index = 0;

// Health data
health_data_t current_health = {0};
uint32_t last_data_time = 0;
uint32_t sample_count = 0;
bool is_connected = false;

// ATECC
uint8_t g_public_key[ECC_PUB_KEY_SIZE] = {0};
uint8_t g_signature[ECC_SIGNATURE_SIZE] = {0};
const char* DEVICE_CN = "PICO_W_CLIENT";

ATCAIfaceCfg cfg_atecc608_pico = {
    .iface_type = ATCA_I2C_IFACE,
    .devtype    = ATECC608B,
    .atcai2c = {
        .address = 0xC0 >> 1,
        .bus     = 0,
        .baud    = I2C_BAUDRATE
    },
    .wake_delay = 1500,
    .rx_retries = 20,
    .cfg_data   = NULL
};



// HTTPS state
static https_state_t https_state = {0};

// Inter-core communication
static volatile bool webhook_trigger = false;
static volatile bool webhook_in_progress = false;
static uint32_t last_post_time = 0;

// Auto-trigger variables
static volatile bool wifi_fully_connected = false;
static bool usb_mounted = false;
static bool auto_trigger_executed = false;

// WiFi state
static bool wifi_connected = false;
static bool reconnect_pending = false;
static uint32_t last_wifi_check = 0;
static uint32_t wifi_disconnect_time = 0;
static bool cyw43_initialized = false;

//mtls state
static bool g_atecc_pk_initialized = false;
static mbedtls_pk_context g_atecc_pk_ctx;
bool init_atecc_pk_context(void);



// [------------------------------------------------------------------------- ATECC608B -------------------------------------------------------------------------]

void print_hex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02X", data[i]);
    }
}

ATCA_STATUS atecc_is_alive() {
    uint8_t rev_id[4];
    return atcab_info(rev_id);
}

void atecc_extract_pubkey() {
    ATCA_STATUS status;

    printf("\n======================================================\n");
    printf("=== ATECC PUBLIC KEY EXTRACTION (GP22) ===\n");
    printf("======================================================\n");

    if (atecc_is_alive() != ATCA_SUCCESS) {
        printf("❌ HARDWARE ERROR: ATECC608B is unresponsive.\n");
        return;
    }

    printf("Extracting Public Key from Slot %d...\n", TARGET_SLOT);
    status = atcab_get_pubkey(TARGET_SLOT, g_public_key);

    if (status != ATCA_SUCCESS) {
        printf("❌ FAILED: Could not read public key. Status: %d\n", status);
        return;
    }

    printf("✅ SUCCESS: Public Key extracted:\n");
    printf("\"PUBLIC_KEY\": \"");
    print_hex(g_public_key, ECC_PUB_KEY_SIZE);
    printf("\"\n");
    printf("======================================================\n");
}

void hardware_rng_test() {
    ATCA_STATUS status;
    uint8_t random_data[RNG_SIZE];

    printf("\n======================================================\n");
    printf("=== HARDWARE RNG TEST ===\n");
    printf("======================================================\n");

    if (atecc_is_alive() != ATCA_SUCCESS) {
        printf("❌ HARDWARE ERROR: ATECC608B is unresponsive.\n");
        return;
    }

    memset(random_data, 0, RNG_SIZE);
    status = atcab_random(random_data);

    if (status != ATCA_SUCCESS) {
        printf("❌ FAILED: atcab_random failed! Status: 0x%02X\n", status);
    } else {
        printf("✅ SUCCESS: 32-byte Hardware Random Number:\n");
        printf("\"RANDOM_DATA\": \"");
        print_hex(random_data, RNG_SIZE);
        printf("\"\n");
    }

    printf("======================================================\n");
}

void check_atecc_button(void)
{
    static bool last_button_state = true;
    static uint32_t debounce_time = 0;

    bool current_state = gpio_get(ATECC_BUTTON_PIN);
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (!current_state && last_button_state)
    {
        if (now - debounce_time > 200)
        {
            printf("\n>>> GP22 Button Pressed! <<<\n");
            atecc_extract_pubkey();
            if (!current_health.valid) {
                printf("❌ No health data available — generating test data\n");
                generate_test_health_data();
            }
            debounce_time = now;
        }
    }

    last_button_state = current_state;
}
static void mbedtls_debug( void *ctx, int level, const char *file, int line, const char *str){
    ((void) ctx);
    ((void) level);
    printf("%s:%04d: %s", file, line, str);
}

///To DELETE
void my_debug(void *ctx, int level, const char *file, int line, const char *str)
{
    printf("[mbedTLS] %s:%d: %s\n", file, line, str);
}
/**
 * @brief Create test health data for manual POST testing
 */
void generate_test_health_data(void)
{
    current_health.cpu        = 23.4f;
    current_health.memory     = 58.7f;
    current_health.disk       = 72.1f;
    current_health.net_in     = 102.5f;
    current_health.net_out    = 88.3f;
    current_health.processes  = 47;
    current_health.valid      = true;

    sample_count = 1;
    last_data_time = to_ms_since_boot(get_absolute_time());

    printf("✅ Test health data generated:\n");
    printf("   CPU: %.1f%%, MEM: %.1f%%, DISK: %.1f%%\n", 
           current_health.cpu, current_health.memory, current_health.disk);
    printf("   NET ↓: %.1f KB/s, ↑: %.1f KB/s, PROC: %d\n", 
           current_health.net_in, current_health.net_out, current_health.processes);
}

int atecc_pk_sign_wrapper(void *ctx,
                          mbedtls_md_type_t md_alg,
                          const unsigned char *hash,
                          size_t hash_len,
                          unsigned char *sig,
                          size_t sig_size,
                          size_t *sig_len_out,
                          int (*f_rng)(void *, unsigned char *, size_t),
                          void *p_rng);

///

// [------------------------------------------------------------------------- JSON PROCESSING -------------------------------------------------------------------------]

void process_json_data(char *json)
{
    char *cpu_pos = strstr(json, "\"cpu\":");
    char *mem_pos = strstr(json, "\"memory\":");
    char *disk_pos = strstr(json, "\"disk\":");
    char *net_in_pos = strstr(json, "\"net_in\":");
    char *net_out_pos = strstr(json, "\"net_out\":");
    char *proc_pos = strstr(json, "\"processes\":");

    if (!is_connected) {
        is_connected = true;
        printf("[CONNECTED] Starting sample counter\n");
    }

    if (cpu_pos) current_health.cpu = atof(cpu_pos + 6);
    if (mem_pos) current_health.memory = atof(mem_pos + 10);
    if (disk_pos) current_health.disk = atof(disk_pos + 7);
    if (net_in_pos) current_health.net_in = atof(net_in_pos + 10);
    if (net_out_pos) current_health.net_out = atof(net_out_pos + 11);
    if (proc_pos) current_health.processes = atoi(proc_pos + 13);

    current_health.valid = true;
    last_data_time = to_ms_since_boot(get_absolute_time());
    sample_count++;

    printf("\r[%3lu] CPU:%5.1f%% MEM:%5.1f%% DSK:%5.1f%%\n",
           sample_count,
           current_health.cpu,
           current_health.memory,
           current_health.disk);
    fflush(stdout);

#ifdef AUTO_POST_ON_SAMPLE
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (!webhook_in_progress && (now - last_post_time >= MIN_POST_INTERVAL_MS)) {
        webhook_trigger = true;
        last_post_time = now;
    }
#endif
}

// [------------------------------------------------------------------------- HTTPS -------------------------------------------------------------------------]

void dns_callback(const char* name, const ip_addr_t* ipaddr, void* arg)
{
    if (ipaddr) {
        ip_addr_t* result = (ip_addr_t*)arg;
        *result = *ipaddr;
        gpio_put(DNS_LED_PIN, 1);
        printf("DNS resolved: %s\n", ip4addr_ntoa(ipaddr));
    } else {
        gpio_put(DNS_LED_PIN, 0);
        printf("DNS resolution failed\n");
    }
}

err_t https_connected_callback(void* arg, struct altcp_pcb* tpcb, err_t err)
{
    https_state_t* state = (https_state_t*)arg;
    
    if (err == ERR_OK) {
        state->connected = true;
        gpio_put(MTLS_LED_PIN, 1);
        printf("TLS handshake complete!\n");
    } else {
        gpio_put(MTLS_LED_PIN, 0);
        printf("Connection failed: %d\n", err);
    }
    
    return ERR_OK;
}

err_t https_recv_callback(void* arg, struct altcp_pcb* tpcb, struct pbuf* p, err_t err)
{
    https_state_t* state = (https_state_t*)arg;
    
    if (p == NULL) {
        altcp_close(tpcb);
        state->pcb=NULL;
        state->connected=false;
        state->operation_in_progress=false;
        webhook_in_progress=false;
        printf("Connection closed by server\n");
        return ERR_OK;
    }
    
    state->bytes_received += p->tot_len;
    
    altcp_recved(tpcb, p->tot_len);
    altcp_close(tpcb);
    pbuf_free(p);
    if (https_state.pcb != NULL) {
        altcp_close(https_state.pcb);
        https_state.pcb = NULL;
    }
    if (https_state.tls_config != NULL) {
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
    }
    return ERR_OK;
}

void https_err_callback(void* arg, err_t err)
{
    printf("Connection error: %d\n", err);
    https_state_t* state = (https_state_t*)arg;
    state->connected = false;
    gpio_put(MTLS_LED_PIN, 0);
}

// [------------------------------------------------------------------------- HTTPS -------------------------------------------------------------------------]

int atca_mbedtls_ecdsa_sign(const mbedtls_mpi* data, mbedtls_mpi* r, mbedtls_mpi* s,
                            const unsigned char* msg, size_t msg_len)
{
    (void)data;  // Unused — ATECC uses slot instead

    if (!msg || msg_len != 32 || !r || !s) {
        return MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
    }

    printf("📝 ATECC hardware signing called (slot %d)\n", TARGET_SLOT);

    uint8_t signature[64];
    ATCA_STATUS status = atcab_sign(TARGET_SLOT, msg, signature);

    if (status != ATCA_SUCCESS) {
        printf("❌ ATECC sign failed: 0x%02X\n", status);
        return MBEDTLS_ERR_PK_ALLOC_FAILED;
    }

    printf("✅ ATECC signature generated\n");

    int ret = mbedtls_mpi_read_binary(r, signature, 32);
    if (ret != 0) {
        printf("❌ Failed to read R: -0x%04x\n", -ret);
        return ret;
    }

    ret = mbedtls_mpi_read_binary(s, signature + 32, 32);
    if (ret != 0) {
        printf("❌ Failed to read S: -0x%04x\n", -ret);
        return ret;
    }
    return ret;
}

int mbedtls_ecdsa_sign(mbedtls_ecp_group *grp, 
                        mbedtls_mpi *r, 
                        mbedtls_mpi *s,
                        const mbedtls_mpi *d, 
                        const unsigned char *buf, 
                        size_t blen,
                        int (*f_rng)(void *, unsigned char *, size_t), 
                        void *p_rng) {
        fflush(stdout);
        ATCA_STATUS status;
        printf("🚨🚨🚨 MBEDTLS_ECDSA_SIGN CALLED! 🚨🚨🚨\n");
        printf("bettter be called");
        printf("Buffer length: %zu\n", blen);
        
        // Convert the MPI hash to a 32-byte buffer for ATECC
        uint8_t hash[32];
        status = atcab_random(hash);///hello
        if (blen != 32) {
            printf("❌ Expected 32-byte hash, got %zu\n", blen);
            return MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
        }
        memcpy(hash, buf, 32);
        
        // Call ATECC hardware to sign
        uint8_t signature[64];
        status = atcab_sign(TARGET_SLOT, hash, signature);
        
        if (status != ATCA_SUCCESS) {
            printf("❌ ATECC sign failed: 0x%02X\n", status);
        }
        
        // Convert ATECC signature (R||S) to mbedTLS MPIs
        int ret = mbedtls_mpi_read_binary(r, signature, 32);
        if (ret != 0) {
            printf("❌ Failed to read R: -0x%04x\n", -ret);
            return ret;
        }
        
        ret = mbedtls_mpi_read_binary(s, signature + 32, 32);
        if (ret != 0) {
            printf("❌ Failed to read S: -0x%04x\n", -ret);
            return ret;
        }
        
    printf("✅ ATECC signature successful!\n");
    return 0;
}



void send_webhook_post(health_data_t* data)
{
    if (https_state.operation_in_progress) {
        printf("Operation already in progress, skipping\n");
        return;
    }

    webhook_in_progress = true;
    https_state.operation_in_progress = true;
    https_state.operation_start_time = to_ms_since_boot(get_absolute_time());
    
    https_state.pending_data = *data;
    
    printf("POST[%lu]...\n", sample_count);
    fflush(stdout);

    // Reset LEDs at start of POST
    gpio_put(DNS_LED_PIN, 0);
    gpio_put(MTLS_LED_PIN, 0);

    // Step 1: DNS Resolution
    ip_addr_t server_ip = {0};
    printf("\nResolving %s...\n", WEBHOOK_HOSTNAME);
    
    err_t dns_err = dns_gethostbyname(WEBHOOK_HOSTNAME, &server_ip, dns_callback, &server_ip);
    
    if (dns_err == ERR_INPROGRESS) {
        int timeout = 0;
        while (server_ip.addr == 0 && timeout < 100) {
            cyw43_arch_poll();
            sleep_ms(100);
            timeout++;
        }
    }

    if (server_ip.addr == 0) {
        printf("DNS fail\n");
        gpio_put(DNS_LED_PIN, 0);
        https_state.operation_in_progress = false;
        webhook_in_progress = false;
        return;
    }

    gpio_put(DNS_LED_PIN, 1);
    printf("Resolved to: %s\n", ip4addr_ntoa(&server_ip));

    // Step 2: Create TLS Config
    u8_t ca_cert[] = CA_CERT;
    
#ifdef MTLS_ENABLED
    u8_t client_cert[] = CLIENT_CERT;
    https_state.tls_config = altcp_tls_create_config_client_2wayauth(
        ca_cert, sizeof(ca_cert),
    // client_key, sizeof(client_key), //modify library to check for (!cert) only reomve &&!privkey portion
        NULL, 0,
        NULL, 0,
        client_cert, sizeof(client_cert)
    );
#else
    https_state.tls_config = altcp_tls_create_config_client(
        ca_cert, sizeof(ca_cert)
    );
#endif
    //Integrating ATECC, injection start===============

    if (https_state.tls_config && g_atecc_pk_initialized){
        altcp_tls_config_internal_t* cfg_internal = 
            (altcp_tls_config_internal_t*)https_state.tls_config;
        if (cfg_internal != NULL){
            mbedtls_ssl_conf_dbg(&cfg_internal->conf, mbedtls_debug, NULL);
            mbedtls_ssl_conf_authmode(&cfg_internal->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            
            //Allocate Client cert chain
            if(cfg_internal->cert_chain == NULL){
                cfg_internal->cert_chain = (mbedtls_x509_crt *)malloc(sizeof(mbedtls_x509_crt));
                if(cfg_internal->cert_chain == NULL){
                    printf("config error: failed to allocatememory for cleint cert chain\n");
                    return;
                }
                printf("memory allocated for cert chain\n");
            }
            mbedtls_x509_crt_init(cfg_internal->cert_chain);
            u8_t client_cert_safe[] = CLIENT_CERT "\0";
            size_t cert_len_plus_null = strlen((const char *)client_cert_safe) +1;
            int ret = mbedtls_x509_crt_parse(cfg_internal->cert_chain, client_cert_safe, cert_len_plus_null);
            // After mbedtls_ssl_conf_own_cert:
            printf("mbedtls_ssl_conf_own_cert returned: %d\n", ret);
            if (ret != 0) {
                    printf("❌ Failed to parse client certificate: %d\n", ret);
                    free(cfg_internal->cert_chain);
                    cfg_internal->cert_chain = NULL;
                    return;
                }
                
                // Make sure ATECC PK context is initiated
                if(!init_atecc_pk_context()){
                    printf("❌ Failed to initialize ATECC PK context.\n");
                    return;
                }
                printf("PK info: %s\n", mbedtls_pk_get_name(&g_atecc_pk_ctx));
            printf("PK can sign? %d\n", mbedtls_pk_can_do(&g_atecc_pk_ctx, MBEDTLS_PK_ECKEY));

                cfg_internal->pkey = &g_atecc_pk_ctx; // Inject ATECC PK context
                uint8_t test_hash[32] = {0xAA}; // Simple pattern
                uint8_t test_sig[64];
                size_t test_sig_len = 0;
                ret = mbedtls_ssl_conf_own_cert(&cfg_internal->conf, 
                                        cfg_internal->cert_chain, 
                                        &g_atecc_pk_ctx);
                printf("mbedtls_ssl_conf_own_cert returned: %d\n", ret);

                if (ret == 0) {
                    printf("✅ Successfully configured TLS to use &g_atecc_pk_ctx (points to ATECC chip)\n");
                } else {
                    printf("⚠️  ATECC injection failed: -0x%04x\n", -ret);
                    printf("⚠️  Falling back to software key\n");
                }
        }else{
            printf("atecc not available, using softwrae\n");
        }
        //injection end ======
    }
    // Step 3: Create new PCB
    https_state.pcb = altcp_tls_new(https_state.tls_config, IPADDR_TYPE_V4);

    if (!https_state.pcb) {
        printf("PCB fail\n");
        gpio_put(MTLS_LED_PIN, 0);
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
        https_state.operation_in_progress = false;
        webhook_in_progress = false;
        return;
    }

    // Step 4: Set SNI hostname
    int mbedtls_err = mbedtls_ssl_set_hostname(
        &(((altcp_mbedtls_state_t*)(https_state.pcb->state))->ssl_context),
        WEBHOOK_HOSTNAME
    );

    if (mbedtls_err != 0) {
        printf("SNI fail\n");
        gpio_put(MTLS_LED_PIN, 0);
        altcp_close(https_state.pcb);
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
        https_state.pcb = NULL;
        https_state.operation_in_progress = false;
        webhook_in_progress = false;
        return;  
    }

    // Step 5: Set callbacks
    https_state.connected = false;
    https_state.request_sent = false;
    https_state.bytes_received = 0;
    
    altcp_arg(https_state.pcb, &https_state);
    altcp_err(https_state.pcb, https_err_callback);
    altcp_recv(https_state.pcb, https_recv_callback);

    printf("Connecting to %s:443...\n", WEBHOOK_HOSTNAME);
    
    // Step 6: Connect
    err_t connect_err = altcp_connect(https_state.pcb, &server_ip, 443, https_connected_callback);

    if (connect_err != ERR_OK) {
        printf("Connect fail:%d\n", connect_err);
        gpio_put(MTLS_LED_PIN, 0);
        altcp_close(https_state.pcb);
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
        https_state.pcb = NULL;
        https_state.operation_in_progress = false;
        webhook_in_progress = false;
        return;
    }

    // Step 7: Wait for TLS handshake
    int timeout = 0;
    while (!https_state.connected && timeout < 1000) {
        cyw43_arch_poll();
        sleep_ms(100);
        timeout++;
    }

    if (!https_state.connected) {
        printf("Timeout\n");
        gpio_put(MTLS_LED_PIN, 0);
        altcp_close(https_state.pcb);
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
        https_state.pcb = NULL;
        https_state.operation_in_progress = false;
        webhook_in_progress = false;
        return;
    }

    // Step 8: Build and send request
    char json_body[256];
    int body_len = snprintf(json_body, sizeof(json_body),
                            "{\"sample\":%lu,\"timestamp\":%lu,\"device\":\"Pico-W\","
                            "\"cpu\":%.1f,\"mem\":%.1f,\"disk\":%.1f,"
                            "\"net_in\":%.1f,\"net_out\":%.1f,\"proc\":%d}",
                            sample_count,
                            to_ms_since_boot(get_absolute_time()),
                            https_state.pending_data.cpu,
                            https_state.pending_data.memory,
                            https_state.pending_data.disk,
                            https_state.pending_data.net_in,
                            https_state.pending_data.net_out,
                            https_state.pending_data.processes);

    char request[2048];
    int req_len = snprintf(request, sizeof(request),
                           "POST /%s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %d\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "%s",
                           WEBHOOK_TOKEN, WEBHOOK_HOSTNAME, body_len, json_body);

    printf("Sending request...\n");

    // Make this reuse previous connection
    err_t write_err = altcp_write(https_state.pcb, request, req_len, TCP_WRITE_FLAG_COPY);

    if (write_err == ERR_OK) {
        altcp_output(https_state.pcb);
        https_state.request_sent = true;

        // Wait for response (longer timeout)
        for (int i = 0; i < 500; i++) {
            cyw43_arch_poll();
            sleep_ms(100);
        }

        printf("OK (%db)\n", https_state.bytes_received);
        fflush(stdout);
    } else {
        printf("Write fail:%d\n", write_err);
    }

    // Step 9: CRITICAL - Proper cleanup in correct order
    // Close the connection
    if (https_state.pcb != NULL) {
        altcp_close(https_state.pcb);
        https_state.pcb = NULL;
    }
    
    // Free TLS config
    if (https_state.tls_config != NULL) {
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
    }
    
    // Give lwIP time to clean up
    for (int i = 0; i < 5; i++) {
        cyw43_arch_poll();
        sleep_ms(50);
    }

    https_state.operation_in_progress = false;
    webhook_in_progress = false;
}

bool init_atecc_pk_context(void){
    if (!g_atecc_pk_initialized){
        mbedtls_pk_init(&g_atecc_pk_ctx);
        int ret = mbedtls_pk_setup(&g_atecc_pk_ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));      
        if (ret !=0){
            printf("❌ mbedtls_pk_setup failed: -0x%04x\n", -ret);
            return false;
        }
        g_atecc_pk_initialized = true;

        printf("⚠️  We need to manually hook the signing function atca wrapper\n");
        debug_pk_context("After init", &g_atecc_pk_ctx);

        // Test signing directly with correct signature
        uint8_t test_hash[32] = {0};
        uint8_t test_sig[64];
        size_t sig_len = 0;
        int test_ret = mbedtls_pk_sign(&g_atecc_pk_ctx, MBEDTLS_MD_SHA256, 
                                    test_hash, 32, test_sig, sizeof(test_sig), &sig_len, NULL, NULL);
        printf("Direct PK sign test returned: %d, sig_len: %zu\n", test_ret, sig_len);
    }
    return true;
}


void debug_pk_context(const char* label, mbedtls_pk_context* pk) {
    printf("=== %s ===\n", label);
    printf("PK type: %d\n", mbedtls_pk_get_type(pk));
    printf("PK name: %s\n", mbedtls_pk_get_name(pk));
    
    if (mbedtls_pk_get_type(pk) == MBEDTLS_PK_ECKEY) {
        mbedtls_ecp_keypair* ecp = mbedtls_pk_ec(*pk);
        printf("ECP items(?) %d\n", ecp);
        printf("items %p\n",ecp);
    }
    printf("================\n");
}




// [------------------------------------------------------------------------- Core 1 - WiFi Handler -------------------------------------------------------------------------]

bool try_wifi_connect(void)
{
    printf("Core 1: Connecting to '%s'...\n", WIFI_SSID);

    int link_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    printf("connect status: ");
    if (link_status == CYW43_LINK_DOWN) printf("no ip\n");
    else if (link_status == CYW43_LINK_JOIN) printf("wifi joined\n");
    else if (link_status == CYW43_LINK_NOIP) printf("no ip\n");
    else if (link_status == CYW43_LINK_UP) printf("link up\n");
    else if (link_status == CYW43_LINK_FAIL) printf("failed\n");
    else if (link_status == CYW43_LINK_NONET) printf("no net\n");
    else if (link_status == CYW43_LINK_BADAUTH) printf("bad auth\n");
    else printf("unknown\n");

    int connect_result = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID, WIFI_PASSWORD,
        CYW43_AUTH_WPA2_AES_PSK, 30000);

    if (connect_result != 0)
    {
        printf("WiFi: Connection FAILED (error %d)\n", connect_result);
        wifi_connected = false;
        return false;
    }

    printf("WiFi: Connected successfully!\n");

    uint32_t ip = cyw43_state.netif[0].ip_addr.addr;
    printf("WiFi: IP Address: %lu.%lu.%lu.%lu\n",
           ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);

    wifi_connected = true;
    return true;
}

bool init_wifi(void)
{
    printf("Core 1: Initializing WiFi...\n");

    if (cyw43_initialized)
    {
        printf("Core 1: Deinitializing previous WiFi instance...\n");
        cyw43_arch_deinit();
        cyw43_initialized = false;
        sleep_ms(1000);
    }

    if (cyw43_arch_init())
    {
        printf("Core 1: WiFi init FAILED\n");
        return false;
    }

    cyw43_initialized = true;
    cyw43_arch_enable_sta_mode();
    printf("Core 1: WiFi STA mode enabled\n");

    if (!try_wifi_connect())
    {
        printf("Core 1: Initial WiFi connection FAILED\n");
        cyw43_arch_deinit();
        cyw43_initialized = false;
        return false;
    }

    printf("*** WIFI FULLY CONNECTED ***\n");
    wifi_fully_connected = true;

    return true;
}

void check_wifi_connection(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (!cyw43_initialized)
        return;

    if (now - last_wifi_check < 5000)
        return;

    last_wifi_check = now;

    int link_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

    if (link_status != CYW43_LINK_UP)
    {
        if (wifi_connected)
        {
            printf("\nCore 1: WiFi connection lost!\n");
            wifi_connected = false;
            wifi_fully_connected = false;
            wifi_disconnect_time = now;
            reconnect_pending = true;
        }
        else if (reconnect_pending && (now - wifi_disconnect_time >= WIFI_RECONNECT_DELAY_MS))
        {
            printf("Core 1: Attempting reconnection...\n");
            reconnect_pending = false;
            
            if (try_wifi_connect())
            {
                printf("Core 1: WiFi reconnected successfully!\n");
            }
            else
            {
                wifi_disconnect_time = now;
                reconnect_pending = true;
                printf("Core 1: Reconnection failed, will retry...\n");
            }
        }
    }
    else
    {
        if (!wifi_connected)
        {
            wifi_connected = true;
            wifi_fully_connected = true;
            reconnect_pending = false;
            printf("Core 1: WiFi link restored!\n");
        }
    }
}

void core1_entry(void)
{
    printf("Core 1: Starting WiFi on separate core\n");

    sleep_ms(1000);

    bool wifi_init_success = false;
    int attempt_count = 0;

    // Keep trying to connect indefinitely until successful
    while (!wifi_init_success)
    {
        if (attempt_count > 0)
        {
            printf("Core 1: Retry attempt %d in %d seconds...\n", 
                   attempt_count + 1, 
                   WIFI_RECONNECT_DELAY_MS / 1000);
            
            // Blink LED during wait period
            int blink_cycles = WIFI_RECONNECT_DELAY_MS / 500;
            for (int i = 0; i < blink_cycles; i++) {
                gpio_put(WIFI_LED_PIN, 1);
                sleep_ms(250);
                gpio_put(WIFI_LED_PIN, 0);
                sleep_ms(250);
            }
        }

        wifi_init_success = init_wifi();
        attempt_count++;
        
        // Fast blink during connection attempt
        if (!wifi_init_success) {
            for (int i = 0; i < 5; i++) {
                gpio_put(WIFI_LED_PIN, 1);
                sleep_ms(100);
                gpio_put(WIFI_LED_PIN, 0);
                sleep_ms(100);
            }
        }
    }

    printf("Core 1: WiFi connected after %d attempts!\n", attempt_count);
    gpio_put(WIFI_LED_PIN, 1);  // SOLID ON when connected

    // Core 1 main loop
    while (true)
    {
        cyw43_arch_poll();
        check_wifi_connection();
        
        // LED behavior based on connection state
        if (wifi_connected)
        {
            // Solid ON when connected
            gpio_put(WIFI_LED_PIN, 1);
        }
        else if (reconnect_pending)
        {
            // Slow blink while waiting to reconnect
            uint32_t now = to_ms_since_boot(get_absolute_time());
            gpio_put(WIFI_LED_PIN, ((now / 500) % 2) == 0);
        }
        else
        {
            // Fast blink during reconnection attempt
            uint32_t now = to_ms_since_boot(get_absolute_time());
            gpio_put(WIFI_LED_PIN, ((now / 100) % 2) == 0);
        }
        
        // Handle webhook trigger
        if (webhook_trigger && wifi_connected && !webhook_in_progress)
        {
            webhook_trigger = false;
            send_webhook_post(&current_health);
        }
        
        sleep_ms(50);
    }
}

// [------------------------------------------------------------------------- MAIN -------------------------------------------------------------------------]

int main(void)
{
    board_init();
    tusb_init();
    stdio_init_all();
    tud_init(BOARD_TUD_RHPORT);

    // Initialize GPIOs
    gpio_init(HID_BUTTON_PIN);
    gpio_set_dir(HID_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(HID_BUTTON_PIN);

    gpio_init(WIFI_LED_PIN);
    gpio_set_dir(WIFI_LED_PIN, GPIO_OUT);
    gpio_put(WIFI_LED_PIN, 0);

    gpio_init(DNS_LED_PIN);
    gpio_set_dir(DNS_LED_PIN, GPIO_OUT);
    gpio_put(DNS_LED_PIN, 0);

    gpio_init(MTLS_LED_PIN);
    gpio_set_dir(MTLS_LED_PIN, GPIO_OUT);
    gpio_put(MTLS_LED_PIN, 0);
    
    gpio_init(ATECC_BUTTON_PIN);
    gpio_set_dir(ATECC_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(ATECC_BUTTON_PIN);

    // ATECC608B Initialization
    printf("\n=== Initializing ATECC608B ===\n");
    
    i2c_init(I2C_BUS_ID, I2C_BAUDRATE);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    printf("✅ I2C Initialized at %dkHz\n", I2C_BAUDRATE / 1000);

    ATCA_STATUS status = atcab_init(&cfg_atecc608_pico);
    if (status != ATCA_SUCCESS) {
        printf("❌ CryptoAuthLib init failed: %d\n", status);
        printf("⚠️  Continuing without ATECC...\n");
    } else {
        printf("✅ ATECC608B initialized successfully\n");
        
        if (atecc_is_alive() == ATCA_SUCCESS) {
            printf("✅ ATECC608B communication verified\n");
            if (!init_atecc_pk_context()){
                printf("atecc pk context initialization failed\n");
            }else{
                printf("atecc pk context initialized\n");
            }
        }
    }

    // Initialize MSC Manager
    msc_config_t msc_cfg = {
        .enable_mount_callbacks = true,
        .on_mount = NULL,    // Optional: set custom callback if needed
        .on_unmount = NULL   // Optional: set custom callback if needed
    };
    
    if (!msc_manager_init(&msc_cfg))
    {
        printf("MSC Manager initialization failed\n");
    }

    // Initialize HID Manager
    hid_config_t hid_cfg = {
    #ifdef AUTO_TRIGGER_HID
        .enable_auto_trigger = true,
        .auto_trigger_delay_ms = 20000,  // 20 seconds
    #else
        .enable_auto_trigger = false,
        .auto_trigger_delay_ms = 0,
    #endif
        .enable_manual_trigger = true,
        .trigger_button_pin = HID_BUTTON_PIN  // GP20
    };
    
    if (!hid_manager_init(&hid_cfg))
    {
        printf("HID Manager initialization failed\n");
    }

    // Build the sequence once at startup
    hid_manager_build_sequence();


    // Launch WiFi on Core 1
    multicore_launch_core1(core1_entry);
    sleep_ms(2000);

    // Core 0 main loop
    while (true)
    {
        tud_task();
        hid_manager_task(wifi_fully_connected, msc_manager_is_mounted());
        check_atecc_button();

        int c = getchar_timeout_us(0);

        if (c != PICO_ERROR_TIMEOUT)
        {
            if (c == '\r' || c == '\n')
            {
                if (rx_index < RX_BUFFER_SIZE)
                {
                    rx_buffer[rx_index] = '\0';
                }
                else
                {
                    rx_buffer[RX_BUFFER_SIZE - 1] = '\0';
                }

                if (rx_index > 0 && rx_buffer[0] == '{')
                {
                    process_json_data(rx_buffer);
                }

                rx_index = 0;
            }
            else if (rx_index < RX_BUFFER_SIZE - 1)
            {
                rx_buffer[rx_index++] = c;
            }
        }

        tight_loop_contents();
    }

    return 0;
}


// TODO DO NOT REMOVE COMMENTS

// Blinking Logic 
// Off = Fail
// Blinking = In process
// On = Success

// LED 6 WIFI Connection status ✅
// LED 7 DNS Status ✅
// LED 8 MTLS Status 



// Sequence of operations

// PICO Powers on -> WIFI/SD card runs simultaneously on boot
// WHEN WIFI connects LED 6 Turns on else LED off
// WHEN SD card initializes it appears on WINDOWS PC as a boot drive, if SD card not inserted add LED on 16

// IF statement when WIFI && SD Card Initializes start 20sec countdown to open CMD (this is to give time for Windows to initialize) literally no way of checking on PICO until File explorer appears since no Serial connection.

// Triggers HID to open CMD if Python EXE not exist it just exit cmd 

// Python EXE opens (Takes awhile) Start CDC communication, IF fail exits CMD (usually cause COM PORT occupied)

// EVERY 5 sec sends CDC json data to PICO (always work cause how do u fk up python) 

// Pico receives data and starts POST request 

// IF any steps fail it just retries with new incoming samples

// 1. get DNS if successful LED 7 ON
// 2. MTLS with website successful LED 8 ON
// 3. POST request sent Successful LED 9 ON (if fail usually python shows the error (write error etc)

// End of process

// No ATEC806B yet (will appears between step 1 and 2 to get certs)