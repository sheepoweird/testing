#include <pico/stdlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <bsp/board.h>
#include <tusb.h>
#include "hardware/gpio.h"
#include "hid_config.h"

#include "ff.h"
#include "diskio.h"

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

#define WIFI_SSID "SINGTEL-93F8"
#define WIFI_PASSWORD "9gqksHLu9Eq4"

#define HID_BUTTON_PIN 20
#define WEBHOOK_BUTTON_PIN 21
#define LED_PIN 6
#define RX_BUFFER_SIZE 512
#define DATA_TIMEOUT_MS 10000
#define MAX_SEQ 512

// ============================================
// AUTO-HID TRIGGER CONFIGURATION
// Comment out the line below to DISABLE auto-trigger
#define AUTO_TRIGGER_HID
// ============================================

static FATFS fs;
static bool sd_mounted = false;

char rx_buffer[RX_BUFFER_SIZE];
int rx_index = 0;

// Type definition - MUST come before forward declarations that use it
typedef struct
{
    float cpu;
    float memory;
    float disk;
    float cpu_temp;
    float net_in;
    float net_out;
    int processes;
    bool valid;
} health_data_t;

// Forward Declarations
bool init_sd_card(void);
bool init_wifi(void);
bool try_wifi_connect(void);
void check_wifi_connection(void);
void send_webhook_post(int num_requests);
void check_webhook_button(void);
void core1_entry(void);

// Add these new declarations:
void dns_callback(const char* name, const ip_addr_t* ipaddr, void* arg);
err_t https_connected_callback(void* arg, struct altcp_pcb* tpcb, err_t err);
err_t https_recv_callback(void* arg, struct altcp_pcb* tpcb, struct pbuf* p, err_t err);
void https_err_callback(void* arg, err_t err);

void log_disconnect_event(void);
void hid_task(void);
void led_blinking_task(void);
void process_json_data(char *json);
void display_compact_status(void);

// Global variables
health_data_t current_health = {0};
uint32_t last_data_time = 0;
uint32_t sample_count = 0;
bool is_connected = false;

// HTTPS connection state
typedef struct {
    struct altcp_tls_config* tls_config;
    bool connected;
    bool request_sent;
    uint16_t bytes_received;
} https_state_t;

static https_state_t https_state = {0};

// Add this new variable for inter-core communication:
static volatile bool webhook_trigger = false;

// Auto-trigger variables
static volatile bool wifi_fully_connected = false;  // Set by Core 1 when WiFi has IP
static bool usb_mounted = false;                     // Set when USB mounts
static bool auto_trigger_executed = false;           // Prevents multiple triggers

int main(void)
{
    board_init();
    tusb_init();
    stdio_init_all();
    tud_init(BOARD_TUD_RHPORT);
    sleep_ms(10000);

    printf("\033[2J\033[H");
    printf("=== System Starting ===\n");
    printf("Core 0: Handling USB, SD, HID\n");

    // Initialize GPIOs
    gpio_init(HID_BUTTON_PIN);
    gpio_set_dir(HID_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(HID_BUTTON_PIN);

    gpio_init(WEBHOOK_BUTTON_PIN);
    gpio_set_dir(WEBHOOK_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(WEBHOOK_BUTTON_PIN);

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    printf("Core 0: GPIO initialized\n");

    // Initialize SD card on Core 0
    if (!init_sd_card())
    {
        printf("WARNING: SD card logging disabled\n");
    }

    // Launch WiFi on Core 1 - THIS IS THE KEY!
    printf("Core 0: Launching WiFi on Core 1...\n");
    multicore_launch_core1(core1_entry);

    sleep_ms(2000); // Give Core 1 time to init WiFi

#ifdef AUTO_TRIGGER_HID
    printf("\n*** AUTO-TRIGGER ENABLED ***\n");
    printf("Waiting for WiFi connection + USB mount, then 15 sec delay...\n");
#else
    printf("\n*** AUTO-TRIGGER DISABLED ***\n");
#endif

    printf("\nCore 0: System ready - entering main loop\n");

    // Core 0 main loop - USB, HID, SD card
    while (true)
    {
        tud_task();
        led_blinking_task();
        hid_task();
        check_webhook_button(); // Webhook button check stays on Core 0

        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (last_data_time > 0 && (now - last_data_time > DATA_TIMEOUT_MS))
        {
            if (current_health.valid || is_connected)
            {
                log_disconnect_event();
                sample_count = 0;
                current_health.valid = false;
                is_connected = false;
                printf("\n[DISCONNECTED] Counter reset\n");
            }
        }

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

// [------------------------------------------------------------------------- MSC -------------------------------------------------------------------------]

void tud_mount_cb(void) 
{
    usb_mounted = true;
    printf("*** USB MOUNTED ***\n");
}

void tud_umount_cb(void) 
{
    usb_mounted = false;
}

void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; }
void tud_resume_cb(void) {}

// [------------------------------------------------------------------------- HID -------------------------------------------------------------------------]

typedef struct
{
    uint8_t modifier;
    uint8_t key;
} key_action_t;

static key_action_t sequence[MAX_SEQ];
static int seq_len = 0;

static inline void add_key(uint8_t mod, uint8_t key, int delay_count)
{
    if (seq_len >= MAX_SEQ - 2)
        return;
    sequence[seq_len++] = (key_action_t){mod, key};
    sequence[seq_len++] = (key_action_t){0, 0};
    for (int i = 0; i < delay_count && seq_len < MAX_SEQ; i++)
    {
        sequence[seq_len++] = (key_action_t){0, 0};
    }
}

static void build_sequence(void)
{
    seq_len = 0;

    add_key(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_R, 8);
    add_key(0, HID_KEY_C, 1);
    add_key(0, HID_KEY_M, 1);
    add_key(0, HID_KEY_D, 1);
    add_key(0, HID_KEY_ENTER, 40);

    char drives[] = "DEFG";
    for (int d = 0; d < 4; d++)
    {
        add_key(KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_A + (drives[d] - 'A'), 1);
        add_key(KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_SEMICOLON, 1);
        add_key(0, HID_KEY_BACKSLASH, 1);
        add_key(0, HID_KEY_H, 0);
        add_key(0, HID_KEY_E, 0);
        add_key(0, HID_KEY_A, 0);
        add_key(0, HID_KEY_L, 0);
        add_key(0, HID_KEY_T, 0);
        add_key(0, HID_KEY_H, 0);
        add_key(KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_MINUS, 0);
        add_key(0, HID_KEY_C, 0);
        add_key(0, HID_KEY_D, 0);
        add_key(0, HID_KEY_C, 0);
        add_key(0, HID_KEY_PERIOD, 0);
        add_key(0, HID_KEY_E, 0);
        add_key(0, HID_KEY_X, 0);
        add_key(0, HID_KEY_E, 1);
        add_key(0, HID_KEY_ENTER, 3);
    }

    for (int i = 0; i < 15; i++)
    {
        if (seq_len < MAX_SEQ)
            sequence[seq_len++] = (key_action_t){0, 0};
    }

    add_key(0, HID_KEY_TAB, 4);
    add_key(0, HID_KEY_TAB, 4);
    add_key(0, HID_KEY_ENTER, 0);
}

void hid_task(void)
{
    const uint32_t interval_ms = 20;
    static uint32_t start_ms = 0;
    static int state = 0;
    static int seq_index = 0;
    static uint32_t auto_trigger_start_time = 0;

    if (board_millis() - start_ms < interval_ms)
        return;
    start_ms = board_millis();

    uint32_t const btn = !gpio_get(HID_BUTTON_PIN);

#ifdef AUTO_TRIGGER_HID
    // Auto-trigger: Wait for BOTH WiFi (with IP) AND USB mount, then 15 second delay
    if (wifi_fully_connected && usb_mounted && !auto_trigger_executed && state == 0)
    {
        if (auto_trigger_start_time == 0)
        {
            // Both are ready, start countdown
            auto_trigger_start_time = board_millis();
            printf("\n*** WIFI + USB READY - 15 second countdown started ***\n");
        }
        else if ((board_millis() - auto_trigger_start_time) >= 15000)
        {
            // 15 seconds passed, trigger HID
            printf("\n*** AUTO-TRIGGERING HID SEQUENCE ***\n");
            if (tud_hid_ready())
            {
                build_sequence();
                seq_index = 0;
                state = 1;
                auto_trigger_executed = true;
            }
        }
    }
#endif

    switch (state)
    {
    case 0:
        if (btn && tud_hid_ready())
        {
            build_sequence();
            seq_index = 0;
            state = 1;
        }
        break;

    case 1:
        if (tud_hid_ready() && seq_index < seq_len)
        {
            key_action_t act = sequence[seq_index++];

            if (act.key || act.modifier)
            {
                uint8_t keys[6] = {act.key, 0, 0, 0, 0, 0};
                tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, act.modifier, keys);
            }
            else
            {
                tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, NULL);
            }
        }
        else if (seq_index >= seq_len)
        {
            state = 2;
        }
        break;

    case 2:
        if (tud_hid_ready())
        {
            tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, NULL);
            state = 0;
        }
        break;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

// [------------------------------------------------------------------------- LED Blinking -------------------------------------------------------------------------]

enum
{
    BLINK_NOT_MOUNTED = 250,
    BLINK_MOUNTED = 1000,
    BLINK_SUSPENDED = 2500,
};

static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

void led_blinking_task(void)
{
    static uint32_t start_ms = 0;
    static bool led_state = false;

    if (board_millis() - start_ms < blink_interval_ms)
        return;
    start_ms = board_millis();

    board_led_write(led_state);
    led_state = !led_state;
}

// [------------------------------------------------------------------------- WiFi -------------------------------------------------------------------------]

static bool wifi_connected = false;
static uint32_t last_wifi_check = 0;
static const uint32_t WIFI_CHECK_INTERVAL_MS = 10000;  // Check every 10 seconds instead of 5

bool init_wifi(void)
{
    printf("WiFi: Initializing CYW43 arch...\n");

    if (cyw43_arch_init())
    {
        printf("WiFi: CYW43 arch init FAILED\n");
        return false;
    }

    printf("WiFi: CYW43 arch initialized\n");
    cyw43_arch_enable_sta_mode();
    printf("WiFi: Station mode enabled\n");

    return try_wifi_connect();
}

bool try_wifi_connect(void)
{
    printf("WiFi: Connecting to SSID: %s\n", WIFI_SSID);

    int result = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID,
        WIFI_PASSWORD,
        CYW43_AUTH_WPA2_AES_PSK,
        30000);

    if (result == 0)
    {
        printf("WiFi: Connected successfully!\n");
        wifi_connected = true;

        // Get and print IP address
        struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
        printf("WiFi: IP Address: %s\n", ip4addr_ntoa(netif_ip4_addr(netif)));

        // Signal that WiFi is FULLY ready (has IP address)
        wifi_fully_connected = true;
        printf("*** WIFI FULLY CONNECTED ***\n");

        return true;
    }
    else
    {
        printf("WiFi: Connection failed (error: %d)\n", result);
        wifi_connected = false;
        return false;
    }
}

void check_wifi_connection(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (now - last_wifi_check < WIFI_CHECK_INTERVAL_MS)
    {
        return;
    }

    last_wifi_check = now;

    int status = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);

    bool was_connected = wifi_connected;
    
    // Only mark as disconnected if truly down/failed
    if (status == CYW43_LINK_DOWN || status == CYW43_LINK_FAIL || 
        status == CYW43_LINK_NONET || status == CYW43_LINK_BADAUTH)
    {
        wifi_connected = false;
    }
    else if (status == CYW43_LINK_UP)
    {
        wifi_connected = true;
    }
    // Ignore transient states like NOIP, JOIN

    // Handle reconnection
    if (!was_connected && wifi_connected)
    {
        printf("WiFi: Reconnected\n");
    }
    else if (was_connected && !wifi_connected)
    {
        printf("WiFi: Connection lost, attempting reconnect...\n");
        wifi_fully_connected = false;  // Reset the fully connected flag
        try_wifi_connect();
    }
    else if (!wifi_connected && !was_connected)
    {
        // Never connected or still disconnected - keep trying
        printf("WiFi: Not connected, retrying...\n");
        try_wifi_connect();
    }
}

// [------------------------------------------------------------------------- JSON Processing -------------------------------------------------------------------------]

void process_json_data(char *json)
{
    last_data_time = to_ms_since_boot(get_absolute_time());

    char *cpu_str = strstr(json, "\"cpu\":");
    char *mem_str = strstr(json, "\"memory\":");
    char *disk_str = strstr(json, "\"disk\":");
    char *temp_str = strstr(json, "\"cpu_temp\":");
    char *net_in_str = strstr(json, "\"net_in\":");
    char *net_out_str = strstr(json, "\"net_out\":");
    char *proc_str = strstr(json, "\"processes\":");

    bool all_found = cpu_str && mem_str && disk_str && temp_str &&
                     net_in_str && net_out_str && proc_str;

    if (all_found)
    {
        current_health.cpu = atof(cpu_str + 6);
        current_health.memory = atof(mem_str + 9);
        current_health.disk = atof(disk_str + 7);
        current_health.cpu_temp = atof(temp_str + 11);
        current_health.net_in = atof(net_in_str + 9);
        current_health.net_out = atof(net_out_str + 10);
        current_health.processes = atoi(proc_str + 12);
        current_health.valid = true;

        if (!is_connected)
        {
            printf("\n[CONNECTED] Starting sample counter\n");
            sample_count = 0;
            is_connected = true;
        }

        sample_count++;
        display_compact_status();
    }
}

void display_compact_status(void)
{
    printf("\r[%4lu] CPU:%5.1f%% MEM:%5.1f%% DSK:%5.1f%% T:%4.1fC "
           "N↓%5.1fMB/s ↑%5.1fMB/s P:%3d",
           sample_count,
           current_health.cpu,
           current_health.memory,
           current_health.disk,
           current_health.cpu_temp,
           current_health.net_in,
           current_health.net_out,
           current_health.processes);
    fflush(stdout);
}

// [------------------------------------------------------------------------- HTTPS Callbacks -------------------------------------------------------------------------]

void dns_callback(const char* name, const ip_addr_t* ipaddr, void* arg)
{
    if (ipaddr)
    {
        ip_addr_t* result = (ip_addr_t*)arg;
        *result = *ipaddr;
    }
}

err_t https_connected_callback(void* arg, struct altcp_pcb* tpcb, err_t err)
{
    if (err == ERR_OK)
    {
        https_state_t* state = (https_state_t*)arg;
        state->connected = true;
        printf("TLS handshake complete!\n");
    }
    else
    {
        printf("Connection callback error: %d\n", err);
    }
    return ERR_OK;
}

err_t https_recv_callback(void* arg, struct altcp_pcb* tpcb, struct pbuf* p, err_t err)
{
    https_state_t* state = (https_state_t*)arg;

    if (!p)
    {
        return ERR_OK;
    }

    state->bytes_received += p->tot_len;
    altcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    return ERR_OK;
}

void https_err_callback(void* arg, err_t err)
{
    printf("Connection error: %d\n", err);
    https_state_t* state = (https_state_t*)arg;

    if (state->tls_config)
    {
        altcp_tls_free_config(state->tls_config);
        state->tls_config = NULL;
    }

    state->connected = false;
}

// [------------------------------------------------------------------------- Webhook POST -------------------------------------------------------------------------]

void send_webhook_post(int num_requests)
{
    printf("Start POST...\n");
    fflush(stdout);
    if (!wifi_connected) {
        printf("Cannot send POST - WiFi not connected\n");
        return;
    }

    printf("\n=== Sending HTTPS POST to webhook.site ===\n");

    // Reset state
    https_state.connected = false;
    https_state.request_sent = false;
    https_state.bytes_received = 0;

    // Step 1-6: DNS + TLS Handshake (ONCE)
    ip_addr_t server_ip;
    server_ip.addr = 0;

    printf("Resolving %s...\n", WEBHOOK_HOSTNAME);
    err_t dns_err = dns_gethostbyname(WEBHOOK_HOSTNAME, &server_ip, dns_callback, &server_ip);

    if (dns_err == ERR_INPROGRESS) {
        int timeout = 0;
        while (server_ip.addr == 0 && timeout < 100) {
            sleep_ms(100);
            timeout++;
        }
    }

    if (server_ip.addr == 0) {
        printf("DNS resolution failed\n");
        return;
    }

    printf("Resolved to: %s\n", ip4addr_ntoa(&server_ip));

    u8_t ca_cert[] = CA_CERT;
    u8_t client_cert[] = CLIENT_CERT;
    u8_t client_key[] = CLIENT_KEY;

    https_state.tls_config = altcp_tls_create_config_client_2wayauth(
        ca_cert, sizeof(ca_cert),
        client_key, sizeof(client_key),
        NULL, 0,
        client_cert, sizeof(client_cert)
    );

    if (!https_state.tls_config) {
        printf("Failed to create TLS config\n");
        return;
    }

    struct altcp_pcb* pcb = altcp_tls_new(https_state.tls_config, IPADDR_TYPE_V4);

    if (!pcb) {
        printf("Failed to create TLS PCB\n");
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
        return;
    }

    int mbedtls_err = mbedtls_ssl_set_hostname(
        &(((altcp_mbedtls_state_t*)(pcb->state))->ssl_context),
        WEBHOOK_HOSTNAME
    );

    if (mbedtls_err != 0) {
        printf("Failed to set SNI hostname\n");
        altcp_close(pcb);
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
        return;
    }

    altcp_arg(pcb, &https_state);
    altcp_err(pcb, https_err_callback);
    altcp_recv(pcb, https_recv_callback);

    printf("Connecting to %s:443...\n", WEBHOOK_HOSTNAME);
    
    err_t connect_err = altcp_connect(pcb, &server_ip, 443, https_connected_callback);

    if (connect_err != ERR_OK) {
        printf("Connection failed: %d\n", connect_err);
        altcp_close(pcb);
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
        return;
    }

    int timeout = 0;
    while (!https_state.connected && timeout < 150) {
        cyw43_arch_poll();
        sleep_ms(100);
        timeout++;
    }

    if (!https_state.connected) {
        printf("Connection timeout\n");
        altcp_close(pcb);
        return;
    }

    // Step 7: Send multiple POST requests over the SAME connection
    for (int req_num = 0; req_num < num_requests; req_num++) {
        printf("\n--- Sending request %d/%d ---\n", req_num + 1, num_requests);
        
        https_state.bytes_received = 0;
        https_state.request_sent = false;

        char json_body[256];
        int body_len = snprintf(json_body, sizeof(json_body),
                                "{\"button\":\"GP21 pressed\",\"timestamp\":%lu,\"device\":\"Pico-W\",\"request\":%d}",
                                to_ms_since_boot(get_absolute_time()), req_num + 1);

        char request[512];
        int req_len = snprintf(request, sizeof(request),
                               "POST /%s HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %d\r\n"
                               "Connection: keep-alive\r\n"
                               "\r\n"
                               "%s",
                               WEBHOOK_TOKEN, WEBHOOK_HOSTNAME, body_len, json_body);

        printf("Sending HTTPS request...\n");
        
        err_t write_err = altcp_write(pcb, request, req_len, TCP_WRITE_FLAG_COPY);

        if (write_err == ERR_OK) {
            altcp_output(pcb);
            
            printf("Request sent! Waiting for response...\n");
            https_state.request_sent = true;

            // Wait for response
            for (int i = 0; i < 30; i++) {
                cyw43_arch_poll();
                sleep_ms(100);
            }

            printf("Received %d bytes\n", https_state.bytes_received);
        } else {
            printf("Failed to send request: %d\n", write_err);
            break;
        }

        sleep_ms(100);
    }

    // Blink LED to confirm all requests sent
    for (int i = 0; i < 6; i++) {
        gpio_put(LED_PIN, 1);
        sleep_ms(50);
        gpio_put(LED_PIN, 0);
        sleep_ms(50);
    }

    // Cleanup
    altcp_close(pcb);

    printf("=== HTTPS POST complete (%d requests sent) ===\n\n", num_requests);
}


void check_webhook_button(void)
{
    static bool last_button_state = true;
    static uint32_t debounce_time = 0;

    bool current_state = gpio_get(WEBHOOK_BUTTON_PIN);
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (!current_state && last_button_state)
    {
        if (now - debounce_time > 200)
        {
            printf("\n>>> GP21 Button Pressed! <<<\n");
            webhook_trigger = true;
            debounce_time = now;
        }
    }

    last_button_state = current_state;
}



// [------------------------------------------------------------------------- Core 1 - WiFi Handler -------------------------------------------------------------------------]

void core1_entry(void)
{
    printf("Core 1: Starting WiFi on separate core\n");

    sleep_ms(1000);

    bool wifi_init_success = false;
    int retry_count = 0;
    const int MAX_RETRIES = 5;

    // Try to connect up to MAX_RETRIES times
    while (!wifi_init_success && retry_count < MAX_RETRIES)
    {
        if (retry_count > 0)
        {
            printf("Core 1: Retry attempt %d/%d in 3 seconds...\n", retry_count + 1, MAX_RETRIES);
            sleep_ms(3000);
        }

        wifi_init_success = init_wifi();
        retry_count++;
    }

    if (wifi_init_success)
    {
        printf("Core 1: WiFi ready!\n");
    }
    else
    {
        printf("Core 1: WiFi failed after %d attempts\n", MAX_RETRIES);
    }

    // Core 1 main loop - handle WiFi tasks AND webhook
    while (true)
    {
        cyw43_arch_poll();  // CRITICAL: Keep WiFi stack alive
        check_wifi_connection();
        
        // Check if Core 0 requested a webhook POST
        if (webhook_trigger && wifi_connected)
        {
            webhook_trigger = false;
            send_webhook_post(50);
        }
        
        sleep_ms(100);  // Poll more frequently for stability
    }
}

// [------------------------------------------------------------------------- SD Read/Write -------------------------------------------------------------------------]

bool init_sd_card(void)
{
    printf("Initializing SD card...\n");

    sleep_ms(100);

    FRESULT fr = f_mount(&fs, "0:", 1);
    if (fr != FR_OK)
    {
        printf("SD card mount failed: %d\n", fr);

        sleep_ms(500);
        fr = f_mount(&fs, "0:", 1);
        if (fr != FR_OK)
        {
            printf("SD card mount failed again: %d\n", fr);
            return false;
        }
    }

    sd_mounted = true;
    printf("SD card mounted successfully\n");
    return true;
}

void log_disconnect_event(void)
{
    if (!sd_mounted)
        return;

    FIL fil;
    FRESULT fr;
    UINT bytes_written;

    fr = f_open(&fil, "0:/pico_logs.txt", FA_WRITE | FA_OPEN_APPEND);
    if (fr != FR_OK)
    {
        printf("Failed to open log file: %d\n", fr);
        return;
    }

    uint32_t timestamp = to_ms_since_boot(get_absolute_time());

    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg),
             "[%lu ms] DISCONNECT - Sample count was %lu\n",
             timestamp, sample_count);

    fr = f_write(&fil, log_msg, strlen(log_msg), &bytes_written);
    if (fr != FR_OK)
    {
        printf("Failed to write to log: %d\n", fr);
    }
    else
    {
        printf("Logged disconnect event to SD card\n");
    }

    f_close(&fil);
}