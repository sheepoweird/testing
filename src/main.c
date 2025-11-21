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
#include "json_processor.h"
#include "wifi_manager.h"
#include "https_manager.h"


#define MBEDTLS_ECDSA_SIGN_ALT

#define WIFI_LED_PIN 6
#define DNS_LED_PIN 7
#define MTLS_LED_PIN 8


// ATECC Configuration
#define I2C_BUS_ID      i2c0
#define I2C_SDA_PIN     4
#define I2C_SCL_PIN     5
#define I2C_BAUDRATE    100000

#define TARGET_SLOT 0
#define ECC_PUB_KEY_SIZE    64
#define ECC_SIGNATURE_SIZE  64

#define DATA_TIMEOUT_MS 20000
#define WIFI_RECONNECT_DELAY_MS 5000

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

// mTLS state
static bool g_atecc_pk_initialized = false;
static mbedtls_pk_context g_atecc_pk_ctx;
bool init_atecc_pk_context(void);

// [------------------------------------------------------------------------- ATECC608B - Signing -------------------------------------------------------------------------]

int atca_mbedtls_ecdsa_sign(const mbedtls_mpi* data, mbedtls_mpi* r, mbedtls_mpi* s,
                            const unsigned char* msg, size_t msg_len)
{
    (void)data;

    if (!msg || msg_len != 32 || !r || !s) {
        return MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
    }

    uint8_t signature[64];
    ATCA_STATUS status = atcab_sign(TARGET_SLOT, msg, signature);

    if (status != ATCA_SUCCESS) {
        printf("❌ ATECC sign failed: 0x%02X\n", status);
        return MBEDTLS_ERR_PK_ALLOC_FAILED;
    }

    int ret = mbedtls_mpi_read_binary(r, signature, 32);
    if (ret != 0) {
        return ret;
    }

    ret = mbedtls_mpi_read_binary(s, signature + 32, 32);
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
    ATCA_STATUS status;
    
    uint8_t hash[32];
    status = atcab_random(hash);
    if (blen != 32) {
            printf("❌ Expected 32-byte hash, got %zu\n", blen);
        return MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
    }
    memcpy(hash, buf, 32);
    
    uint8_t signature[64];
    status = atcab_sign(TARGET_SLOT, hash, signature);
    
    if (status != ATCA_SUCCESS) {
            printf("❌ ATECC sign failed: 0x%02X\n", status);
    }
    
    int ret = mbedtls_mpi_read_binary(r, signature, 32);
    if (ret != 0) {
        return ret;
    }
    
    ret = mbedtls_mpi_read_binary(s, signature + 32, 32);
    if (ret != 0) {
        return ret;
    }
    
    return 0;
}

// [------------------------------------------------------------------------- HTTPS - POST -------------------------------------------------------------------------]

void trigger_webhook_post(health_data_t* data)
{
    (void)data;
    
    if (!webhook_in_progress) {
        webhook_trigger = true;
    }
}

void send_webhook_post(health_data_t* data)
{
    if (https_manager_is_busy()) {
        return;
    }

    webhook_in_progress = true;

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

    https_manager_post_json(&post_data);

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
    }
    return true;
}

// [------------------------------------------------------------------------- Core 1 - WiFi Handler -------------------------------------------------------------------------]

void core1_entry(void)
{
    sleep_ms(1000);

    wifi_config_t wifi_cfg = {
        .ssid = WIFI_SSID,
        .password = WIFI_PASSWORD,
        .reconnect_delay_ms = WIFI_RECONNECT_DELAY_MS,
        .connection_timeout_ms = 30000,
        .led_pin = WIFI_LED_PIN
    };

    bool wifi_init_success = false;
    int attempt_count = 0;

    while (!wifi_init_success)
    {
        if (attempt_count > 0)
        {
            printf("Core 1: Retry attempt %d in %d seconds...\n", 
                   attempt_count + 1, 
                   WIFI_RECONNECT_DELAY_MS / 1000);
            
            int blink_cycles = WIFI_RECONNECT_DELAY_MS / 500;
            for (int i = 0; i < blink_cycles; i++) {
                gpio_put(WIFI_LED_PIN, 1);
                sleep_ms(250);
                gpio_put(WIFI_LED_PIN, 0);
                sleep_ms(250);
            }
        }

        if (!wifi_manager_init(&wifi_cfg)) {
            attempt_count++;
            continue;
        }

        wifi_init_success = wifi_manager_connect();
        attempt_count++;
        
        if (!wifi_init_success) {
            for (int i = 0; i < 5; i++) {
                gpio_put(WIFI_LED_PIN, 1);
                sleep_ms(100);
                gpio_put(WIFI_LED_PIN, 0);
                sleep_ms(100);
            }
            
            wifi_manager_deinit();
        }
    }

    printf("Core 1: WiFi connected after %d attempts!\n", attempt_count);
    gpio_put(WIFI_LED_PIN, 1);
    
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
    gpio_init(WIFI_LED_PIN);
    gpio_set_dir(WIFI_LED_PIN, GPIO_OUT);
    gpio_put(WIFI_LED_PIN, 0);

    gpio_init(DNS_LED_PIN);
    gpio_set_dir(DNS_LED_PIN, GPIO_OUT);
    gpio_put(DNS_LED_PIN, 0);

    gpio_init(MTLS_LED_PIN);
    gpio_set_dir(MTLS_LED_PIN, GPIO_OUT);
    gpio_put(MTLS_LED_PIN, 0);

    // ATECC608B Initialization
    i2c_init(I2C_BUS_ID, I2C_BAUDRATE);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    ATCA_STATUS status = atcab_init(&cfg_atecc608_pico);
    if (status != ATCA_SUCCESS) {
        printf("CryptoAuthLib init failed: %d\n", status);
    } else {
        printf("ATECC608B initialized\n");
        
        if (!init_atecc_pk_context()){
            printf("ATECC PK context initialization failed\n");
        }
    }

    msc_config_t msc_cfg = {
        .enable_mount_callbacks = true,
        .on_mount = NULL,
        .on_unmount = NULL
    };
    
    msc_manager_init(&msc_cfg);

    hid_config_t hid_cfg = {
        .enable_auto_trigger = true,
        .auto_trigger_delay_ms = 20000
    };
    
    hid_manager_init(&hid_cfg);

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
        .dns_led_pin = DNS_LED_PIN,
        .mtls_led_pin = MTLS_LED_PIN,
        .operation_timeout_ms = DATA_TIMEOUT_MS
    };
    
    https_manager_init(&https_cfg);

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

    json_processor_init(&json_cfg);

    // Core 0 main loop
    while (true)
    {
        tud_task();
        hid_manager_task(wifi_fully_connected, msc_manager_is_mounted());
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