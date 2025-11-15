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
#include "hid_manager.h"
#include "json_processor.h"
#include "wifi_manager.h"
#include "https_manager.h"


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

#define MBEDTLS_ECDSA_SIGN_ALT

#define TARGET_SLOT 0
#define ECC_PUB_KEY_SIZE    64
#define ECC_SIGNATURE_SIZE  64
#define DIGEST_SIZE         32
#define RNG_SIZE           32

#define MAX_SEQ 512

#define DATA_TIMEOUT_MS 20000

// MTLS CONFIGURATION
#define MTLS_ENABLED

// POST CONFIGURATION
#define AUTO_POST_ON_SAMPLE
#define MIN_POST_INTERVAL_MS 6000

// AUTO-HID TRIGGER CONFIGURATION
#define AUTO_TRIGGER_HID

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

// Inter-core communication
static volatile bool webhook_trigger = false;
static volatile bool webhook_in_progress = false;
static uint32_t last_post_time = 0;

// Auto-trigger variables
static volatile bool wifi_fully_connected = false;
static bool usb_mounted = false;
static bool auto_trigger_executed = false;

//mtls state
bool g_atecc_pk_initialized = false;
mbedtls_pk_context g_atecc_pk_ctx;
bool init_atecc_pk_context(void);

// [------------------------------------------------------------------------- ATECC608B - Testing -------------------------------------------------------------------------]

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
            const health_data_t* health = json_processor_get_health_data();
            if (!health->valid) {
                printf("❌ No health data available — generating test data\n");
                json_processor_generate_test_data();
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

int atecc_pk_sign_wrapper(void *ctx,
                          mbedtls_md_type_t md_alg,
                          const unsigned char *hash,
                          size_t hash_len,
                          unsigned char *sig,
                          size_t sig_size,
                          size_t *sig_len_out,
                          int (*f_rng)(void *, unsigned char *, size_t),
                          void *p_rng);



// [------------------------------------------------------------------------- ATECC608B - Signing -------------------------------------------------------------------------]

int atca_mbedtls_ecdsa_sign(const mbedtls_mpi* data, mbedtls_mpi* r, mbedtls_mpi* s,
                            const unsigned char* msg, size_t msg_len)
{
    (void)data;  // Unused — ATECC uses slot instead

    if (!msg || msg_len != 32 || !r || !s) {
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

// [------------------------------------------------------------------------- HTTPS - POST -------------------------------------------------------------------------]

void trigger_webhook_post(health_data_t* data)
{
    (void)data;  // Unused - data is fetched later in Core 1
    
    // Set flag for Core 1 to pick up
    if (!webhook_in_progress) {
        webhook_trigger = true;
    }
}

void send_webhook_post(health_data_t* data)
{
    if (https_manager_is_busy()) {
        printf("HTTPS busy, skipping\n");
        return;
    }

    webhook_in_progress = true;

    // Prepare POST data
    https_post_data_t post_data = {
        .sample = json_processor_get_sample_count(),
        .timestamp = to_ms_since_boot(get_absolute_time()),
        .device = "Pico-W",
        .cpu = data->cpu,
        .memory = data->memory,
        .disk = data->disk,
        .net_in = data->net_in,
        .net_out = data->net_out,
        .processes = data->processes
    };

    // Send POST request
    bool success = https_manager_post_json(&post_data);
    
    if (success) {
        printf("POST successful!\n");
    } else {
        printf("POST failed\n");
    }

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

void core1_entry(void)
{
    sleep_ms(1000);

    // Configure WiFi manager
    wifi_config_t wifi_cfg = {
        .ssid = WIFI_SSID,
        .password = WIFI_PASSWORD,
        .reconnect_delay_ms = WIFI_RECONNECT_DELAY_MS,
        .connection_timeout_ms = 30000,
        .led_pin = WIFI_LED_PIN  // GP6
    };

    // Initialize WiFi manager
    bool wifi_init_success = false;
    int attempt_count = 0;

    // Keep trying to initialize and connect
    while (!wifi_init_success)
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

        // Initialize WiFi manager
        if (!wifi_manager_init(&wifi_cfg)) {
            attempt_count++;
            continue;
        }

        // Attempt connection
        wifi_init_success = wifi_manager_connect();
        attempt_count++;
        
        if (!wifi_init_success) {
            // Fast blink during failed attempt
            for (int i = 0; i < 5; i++) {
                gpio_put(WIFI_LED_PIN, 1);
                sleep_ms(100);
                gpio_put(WIFI_LED_PIN, 0);
                sleep_ms(100);
            }
            
            // Deinit before retry
            wifi_manager_deinit();
        }
    }

    printf("Core 1: WiFi connected after %d attempts!\n", attempt_count);
    gpio_put(WIFI_LED_PIN, 1);  // SOLID ON when connected
    
    // Set flag for auto-HID trigger
    wifi_fully_connected = true;

    // Core 1 main loop
    while (true)
    {
        wifi_manager_poll();
        wifi_manager_task();
        
        wifi_fully_connected = wifi_manager_is_fully_connected();
        
        wifi_state_t state = wifi_manager_get_state();
        if (state == WIFI_STATE_CONNECTED)
        {
            // Solid ON when connected
            gpio_put(WIFI_LED_PIN, 1);
        }
        else if (state == WIFI_STATE_RECONNECTING)
        {
            // Slow blink while waiting to reconnect
            uint32_t now = to_ms_since_boot(get_absolute_time());
            gpio_put(WIFI_LED_PIN, ((now / 500) % 2) == 0);
        }
        else
        {
            // Fast blink during connection attempt
            uint32_t now = to_ms_since_boot(get_absolute_time());
            gpio_put(WIFI_LED_PIN, ((now / 100) % 2) == 0);
        }
        
        // Handle webhook trigger
        if (webhook_trigger && wifi_manager_is_connected() && !webhook_in_progress)
        {
            webhook_trigger = false;
            const health_data_t* health = json_processor_get_health_data();
            if (health->valid) {
                health_data_t data_copy = *health;
                send_webhook_post(&data_copy);
            }
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
    i2c_init(I2C_BUS_ID, I2C_BAUDRATE);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    ATCA_STATUS status = atcab_init(&cfg_atecc608_pico);
    if (status != ATCA_SUCCESS) {
        printf("❌ CryptoAuthLib init failed: %d, Continuing without ATECC...\n", status);
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

    // Build the sequence once at startup
    hid_manager_build_sequence();

    
    https_config_t https_cfg = {
        .hostname = WEBHOOK_HOSTNAME,
        .webhook_token = WEBHOOK_TOKEN,
        .port = 443,
        
        .ca_cert = CA_CERT,
        .ca_cert_len = sizeof(CA_CERT),
        
    #ifdef MTLS_ENABLED
        .enable_mtls = true,
        .client_cert = CLIENT_CERT,
        .client_cert_len = sizeof(CLIENT_CERT),
        .atecc_pk_context = g_atecc_pk_initialized ? &g_atecc_pk_ctx : NULL,
    #else
        .enable_mtls = false,
        .client_cert = NULL,
        .client_cert_len = 0,
        .atecc_pk_context = NULL,
    #endif
        
        .dns_led_pin = DNS_LED_PIN,   // GP7
        .mtls_led_pin = MTLS_LED_PIN, // GP8
        
        .operation_timeout_ms = DATA_TIMEOUT_MS
    };
    
    if (!https_manager_init(&https_cfg)) {
        printf("HTTPS Manager initialization failed\n");
    }

    // Launch WiFi on Core 1
    multicore_launch_core1(core1_entry);
    sleep_ms(2000);


    
    // Initialize JSON Processor
    json_processor_config_t json_cfg = {
    #ifdef AUTO_POST_ON_SAMPLE
        .enable_auto_post = true,
        .min_post_interval_ms = MIN_POST_INTERVAL_MS,
        .on_post_trigger = trigger_webhook_post,
    #else
        .enable_auto_post = false,
        .min_post_interval_ms = 0,
        .on_post_trigger = NULL,
    #endif
        .on_data_received = NULL
    };

    if (!json_processor_init(&json_cfg))
    {
        printf("JSON Processor initialization failed\n");
    }

    // Core 0 main loop
    while (true)
    {
        tud_task();
        hid_manager_task(wifi_fully_connected, msc_manager_is_mounted());
        check_atecc_button();
        https_manager_task();

        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT)
        {
            json_processor_process_char(c);
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