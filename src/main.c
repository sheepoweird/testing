// Libraries
#include <pico/stdlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <bsp/board.h>
#include <tusb.h>
#include "hardware/gpio.h"

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
#include "hid_config.h"
#include "hid_manager.h"
#include "https_config.h"
#include "wifi_manager.h"
#include "https_client.h"


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

#define MBEDTLS_ECDSA_SIGN_ALT

#define TARGET_SLOT 0
#define ECC_PUB_KEY_SIZE    64
#define ECC_SIGNATURE_SIZE  64
#define DIGEST_SIZE         32
#define RNG_SIZE           32

#define RX_BUFFER_SIZE 512
#define MAX_SEQ 512

#define DATA_TIMEOUT_MS 20000

// MTLS CONFIGURATION
// #define MTLS_ENABLED

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

//mtls state
bool g_atecc_pk_initialized = false;
mbedtls_pk_context g_atecc_pk_ctx;
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

void send_webhook_post(health_data_t* data)
{
    if (https_client_is_busy())
    {
        printf("HTTPS: Operation already in progress, skipping\n");
        return;
    }
    
    webhook_in_progress = true;
    
    printf("POST[%lu]...\n", sample_count);
    fflush(stdout);
    
    /* Build JSON payload */
    char json_body[HTTPS_MAX_JSON_BODY_SIZE];
    int body_len = snprintf(json_body, sizeof(json_body),
                           "{\"sample\":%lu,\"timestamp\":%lu,\"device\":\"Pico-W\","
                           "\"cpu\":%.1f,\"mem\":%.1f,\"disk\":%.1f,"
                           "\"net_in\":%.1f,\"net_out\":%.1f,\"proc\":%d}",
                           sample_count,
                           to_ms_since_boot(get_absolute_time()),
                           data->cpu,
                           data->memory,
                           data->disk,
                           data->net_in,
                           data->net_out,
                           data->processes);
    
    if (body_len < 0 || body_len >= (int)sizeof(json_body))
    {
        printf("JSON payload too large\n");
        webhook_in_progress = false;
        return;
    }
    
    /* Build request path */
    char path[128];
    snprintf(path, sizeof(path), "/%s", WEBHOOK_TOKEN);
    
    /* Send HTTPS POST request */
    bool success = https_client_post(path, json_body, body_len);
    
    if (success)
    {
        printf("POST complete: %u bytes received\n", 
               https_client_get_bytes_received());
    }
    else
    {
        printf("POST failed\n");
    }
    
    webhook_in_progress = false;
}

bool init_atecc_pk_context(void)
{
    if (!g_atecc_pk_initialized)
    {
        mbedtls_pk_init(&g_atecc_pk_ctx);
        
        int ret = mbedtls_pk_setup(&g_atecc_pk_ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
        
        if (ret != 0)
        {
            printf("❌ Failed to setup ATECC PK context: -0x%04x\n", -ret);
            return false;
        }
        
        g_atecc_pk_initialized = true;
        printf("✅ ATECC PK context initialized\n");
    }
    
    return true;
}

int mbedtls_ecdsa_sign(mbedtls_ecp_group *grp, 
                       mbedtls_mpi *r, 
                       mbedtls_mpi *s,
                       const mbedtls_mpi *d, 
                       const unsigned char *buf, 
                       size_t blen,
                       int (*f_rng)(void *, unsigned char *, size_t), 
                       void *p_rng) 
{
    (void)grp;    /* Unused - ATECC uses P-256 internally */
    (void)d;      /* Unused - ATECC uses key from slot */
    (void)f_rng;  /* Unused - ATECC has hardware RNG */
    (void)p_rng;  /* Unused */
    
    ATCA_STATUS status;
    
    printf("🚨 MBEDTLS_ECDSA_SIGN: ATECC hardware signing called\n");
    printf("   Buffer length: %zu bytes\n", blen);
    
    /* Verify hash length */
    if (blen != 32) 
    {
        printf("❌ Expected 32-byte hash, got %zu bytes\n", blen);
        return MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
    }
    
    /* Copy hash buffer */
    uint8_t hash[32];
    memcpy(hash, buf, 32);
    
    /* Call ATECC608B hardware to sign the hash */
    uint8_t signature[64];
    status = atcab_sign(TARGET_SLOT, hash, signature);
    
    if (status != ATCA_SUCCESS) 
    {
        printf("❌ ATECC sign failed: 0x%02X\n", status);
        return MBEDTLS_ERR_PK_ALLOC_FAILED;
    }
    
    /* Convert ATECC signature (R||S format) to mbedTLS MPIs */
    int ret = mbedtls_mpi_read_binary(r, signature, 32);
    if (ret != 0) 
    {
        printf("❌ Failed to read signature R component: -0x%04x\n", -ret);
        return ret;
    }
    
    ret = mbedtls_mpi_read_binary(s, signature + 32, 32);
    if (ret != 0) 
    {
        printf("❌ Failed to read signature S component: -0x%04x\n", -ret);
        return ret;
    }
    
    printf("✅ ATECC hardware signature successful!\n");
    return 0;
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

bool init_wifi(void)
{
    printf("\n=== Initializing WiFi ===\n");
    
    /* Initialize WiFi manager */
    if (!wifi_manager_init())
    {
        printf("WiFi Manager: Initialization failed!\n");
        return false;
    }
    
    /* Connect to WiFi network */
    if (!wifi_manager_connect(WIFI_SSID, WIFI_PASSWORD, 30000))
    {
        printf("WiFi Manager: Connection failed!\n");
        return false;
    }
    
    /* Update legacy flag for compatibility */
    wifi_fully_connected = wifi_manager_is_fully_connected();
    
    printf("*** WiFi Manager: FULLY CONNECTED ***\n");
    return true;
}

void check_wifi_connection(void)
{
    wifi_status_t status = wifi_manager_check_status();
    
    /* Handle reconnection if needed */
    const wifi_manager_state_t *state = wifi_manager_get_state();
    if (status == WIFI_STATUS_DISCONNECTED && state->reconnect_pending)
    {
        wifi_manager_handle_reconnect();
    }
}

void core1_entry(void)
{
    printf("Core 1: Starting WiFi on separate core\n");
    sleep_ms(1000);
    
    while (true)
    {
        int attempt_count = 0;

        if (wifi_manager_get_state()->is_initialized) {
            printf("Core 1: Forcing de-init and delay...\n");
            wifi_manager_deinit(); 
            sleep_ms(WIFI_RECONNECT_DELAY_MS);
        }
        
        while (true)
        {
            if (attempt_count > 0)
            {
                printf("Core 1: Retry attempt %d in %d seconds...\n", 
                        attempt_count + 1, 
                        WIFI_RECONNECT_DELAY_MS / 1000);
                
                wifi_manager_deinit(); 
                sleep_ms(WIFI_RECONNECT_DELAY_MS);
            }

            if (!wifi_manager_init()) {
                printf("Core 1: WiFi init FAILED\n");
                attempt_count++;
                continue; // Retry after delay
            }

            if (!wifi_manager_connect(WIFI_SSID, WIFI_PASSWORD, WIFI_CONNECT_TIMEOUT_MS)) {
                printf("Core 1: WiFi connection FAILED\n");
                attempt_count++;
                continue; // Retry after deinit
            }

            // Wait for full connection on Core 1 before breaking the connection loop
            uint32_t start_time = to_ms_since_boot(get_absolute_time());
            const uint32_t max_wait = 10000; // 10 second timeout for DHCP
            
            while (wifi_manager_is_connected() && !wifi_manager_is_fully_connected())
            {
                wifi_manager_poll();
                sleep_ms(100);
                if (to_ms_since_boot(get_absolute_time()) - start_time > max_wait) {
                    printf("Core 1: Timed out waiting for full IP connection (DHCP). Retrying...\n");
                    break;
                }
            }
            
            if (!wifi_manager_is_fully_connected()) {
                 attempt_count++;
                 continue; // Restart the entire connection process
            }

            // Exit the inner connection loop
            printf("Core 1: WiFi fully connected after %d attempts!\n", attempt_count + 1);
            wifi_fully_connected = true; // Set flag to true as we are now fully connected
            break; 
        }

        while (wifi_manager_is_connected()) 
        {
            // a check to ensures Core 0 gets the most recent state.
            wifi_fully_connected = wifi_manager_is_fully_connected(); 
            
            wifi_manager_check_status(); 
            wifi_manager_poll();
            
            if (webhook_trigger && !webhook_in_progress)
            {
                webhook_trigger = false;
                send_webhook_post(&current_health);
            }
            
            sleep_ms(50);
        }
        
        // Connection dropped, ensure flag is false before retrying
        wifi_fully_connected = false;
        printf("Core 1: WiFi connection lost. Retrying...\n");
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

    ATCA_STATUS status = atcab_init(&cfg_atecc608_pico);
    if (status != ATCA_SUCCESS) {
        printf("❌ CryptoAuthLib init failed: %d. Continuing without ATECC...\n", status);
    } else {
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
    
    // HID build the sequence once at startup
    hid_manager_build_sequence();

    // Launch WiFi on Core 1
    multicore_launch_core1(core1_entry);
    sleep_ms(2000);

    // Initialize HTTPS client
    if (!https_client_init())
    {
        printf("HTTPS client initialization failed\n");
    }
    
    // Configure HTTPS client with certificates
    https_config_t https_cfg = {
        .hostname = WEBHOOK_HOSTNAME,
        .port = 443,
        .ca_cert = (const uint8_t*)CA_CERT,
        .ca_cert_len = sizeof(CA_CERT),
    #ifdef MTLS_ENABLED
        .client_cert = (const uint8_t*)CLIENT_CERT,
        .client_cert_len = sizeof(CLIENT_CERT),
        .enable_mtls = true,
        .use_atecc = true,
    #else
        .client_cert = NULL,
        .client_cert_len = 0,
        .enable_mtls = false,
        .use_atecc = false,
    #endif
    };
    
    https_client_configure(&https_cfg);


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