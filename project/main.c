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

#define HID_BUTTON_PIN 20
#define WEBHOOK_BUTTON_PIN 21
#define WIFI_LED_PIN 6
#define RX_BUFFER_SIZE 512
#define DATA_TIMEOUT_MS 20000
#define MAX_SEQ 512

#define WIFI_RECONNECT_DELAY_MS 5000

// MTLS CONFIGURATION
// #define MTLS_ENABLED

// POST CONFIGURATION
#define AUTO_POST_ON_SAMPLE
// Minimum delay between POSTs (milliseconds)
#define MIN_POST_INTERVAL_MS 6000

// SERIAL VERBOSITY CONTROL
#define VERBOSE_SERIAL 0

// AUTO-HID TRIGGER CONFIGURATION
#define AUTO_TRIGGER_HID

static FATFS fs;
static bool sd_mounted = false;
static bool sd_init_attempted = false;

char rx_buffer[RX_BUFFER_SIZE];
int rx_index = 0;

// Type definition
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

// Forward Declarations
bool init_sd_card(void);
bool try_sd_mount(void);
bool init_wifi(void);
bool try_wifi_connect(void);
void check_wifi_connection(void);
void send_webhook_post_with_cleanup(health_data_t* data);
void check_webhook_button(void);
void core1_entry(void);

void dns_callback(const char* name, const ip_addr_t* ipaddr, void* arg);
err_t https_connected_callback(void* arg, struct altcp_pcb* tpcb, err_t err);
err_t https_recv_callback(void* arg, struct altcp_pcb* tpcb, struct pbuf* p, err_t err);
void https_err_callback(void* arg, err_t err);

void log_disconnect_event(void);
void hid_task(void);
void process_json_data(char *json);

// Global variables
health_data_t current_health = {0};
uint32_t last_data_time = 0;
uint32_t sample_count = 0;
bool is_connected = false;

// HTTPS connection state
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

static https_state_t https_state = {0};

// Inter-core communication
static volatile bool webhook_trigger = false;
static volatile bool webhook_in_progress = false;
static uint32_t last_post_time = 0;

// Auto-trigger variables
static volatile bool wifi_fully_connected = false;
static bool usb_mounted = false;
static bool auto_trigger_executed = false;

// Conditional printf for verbose mode
#if VERBOSE_SERIAL
    #define VPRINTF(...) printf(__VA_ARGS__)
#else
    #define VPRINTF(...) ((void)0)
#endif

int main(void)
{
    board_init();
    tusb_init();
    stdio_init_all();
    tud_init(BOARD_TUD_RHPORT);
    sleep_ms(10000);

    // Initialize GPIOs
    gpio_init(HID_BUTTON_PIN);
    gpio_set_dir(HID_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(HID_BUTTON_PIN);

    gpio_init(WEBHOOK_BUTTON_PIN);
    gpio_set_dir(WEBHOOK_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(WEBHOOK_BUTTON_PIN);

    gpio_init(WIFI_LED_PIN);
    gpio_set_dir(WIFI_LED_PIN, GPIO_OUT);
    gpio_put(WIFI_LED_PIN, 0);

    // Launch WiFi on Core 1
    multicore_launch_core1(core1_entry);
    sleep_ms(2000);

    // Core 0 main loop
    while (true)
    {
        tud_task();
        hid_task();
        check_webhook_button();

        // Try to mount SD card after USB is connected (only once)
        if (usb_mounted && !sd_init_attempted) {
            sd_init_attempted = true;
            sleep_ms(100);
            if (init_sd_card()) {
                printf("SD card ready for logging\n");
            }
        }

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
    sleep_ms(5000);
    usb_mounted = true;
    sleep_ms(5000);
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

    add_key(0, HID_KEY_E, 4);
    add_key(0, HID_KEY_X, 4);
    add_key(0, HID_KEY_I, 4);
    add_key(0, HID_KEY_T, 4);
    add_key(0, HID_KEY_ENTER, 0);
}

void hid_task(void)
{
    const uint32_t interval_ms = 20;
    static uint32_t last_update = 0;
    static bool hid_running = false;
    static int seq_index = 0;

#ifdef AUTO_TRIGGER_HID
    if (!auto_trigger_executed && wifi_fully_connected && usb_mounted) {
        static uint32_t trigger_start_time = 0;
        
        if (trigger_start_time == 0) {
            trigger_start_time = to_ms_since_boot(get_absolute_time());
            printf("\n*** WIFI + USB READY - 20 second countdown started ***\n");
        }
        
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - trigger_start_time >= 20000) {
            printf("*** AUTO-TRIGGERING HID SEQUENCE ***\n");
            build_sequence();
            hid_running = true;
            seq_index = 0;
            last_update = now;
            auto_trigger_executed = true;
        }
    }
#endif

    // Manual trigger
    static bool last_button_state = true;
    bool current_state = gpio_get(HID_BUTTON_PIN);
    static uint32_t debounce_time = 0;
    uint32_t now_hid = to_ms_since_boot(get_absolute_time());

    if (!current_state && last_button_state) {
        if (now_hid - debounce_time > 200) {
            printf("\n>>> GP20 Button Pressed! Starting HID sequence... <<<\n");
            build_sequence();
            hid_running = true;
            seq_index = 0;
            last_update = now_hid;
            debounce_time = now_hid;
        }
    }
    last_button_state = current_state;

    if (!hid_running)
        return;

    if (!tud_hid_ready())
        return;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_update < interval_ms)
        return;

    last_update = now;

    if (seq_index >= seq_len) {
        hid_running = false;
        printf("HID sequence completed!\n\n");
        return;
    }

    key_action_t action = sequence[seq_index++];

    uint8_t keycode[6] = {0};
    if (action.key != 0) {
        keycode[0] = action.key;
    }

    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, action.modifier, keycode);
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    (void) itf; (void) report_id; (void) report_type; (void) buffer; (void) reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
    (void) itf; (void) report_id; (void) report_type; (void) buffer; (void) bufsize;
}


// [------------------------------------------------------------------------- JSON Processing -------------------------------------------------------------------------]

void process_json_data(char *json)
{
    // Simple JSON parsing
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

    // Minimal serial response
    printf("\r[%3lu] CPU:%5.1f%% MEM:%5.1f%% DSK:%5.1f%%\n",
           sample_count,
           current_health.cpu,
           current_health.memory,
           current_health.disk);
    fflush(stdout);

#ifdef AUTO_POST_ON_SAMPLE
    // Trigger POST for this sample if enough time has passed
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (!webhook_in_progress && (now - last_post_time >= MIN_POST_INTERVAL_MS)) {
        webhook_trigger = true;
        last_post_time = now;
    }
#endif
}

// [------------------------------------------------------------------------- WiFi -------------------------------------------------------------------------]

static bool wifi_connected = false;
static uint32_t last_wifi_check = 0;

bool init_wifi(void)
{
    printf("Core 1: Initializing WiFi...\n");

    if (cyw43_arch_init())
    {
        printf("Core 1: WiFi init FAILED\n");
        return false;
    }

    cyw43_arch_enable_sta_mode();
    printf("Core 1: WiFi STA mode enabled\n");

    if (!try_wifi_connect())
    {
        printf("Core 1: Initial WiFi connection FAILED\n");
        return false;
    }

    printf("*** WIFI FULLY CONNECTED ***\n");
    wifi_fully_connected = true;

    return true;
}

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
        return false;
    }

    printf("WiFi: Connected successfully!\n");

    uint32_t ip = cyw43_state.netif[0].ip_addr.addr;
    printf("WiFi: IP Address: %lu.%lu.%lu.%lu\n",
           ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);

    wifi_connected = true;
    return true;
}

void check_wifi_connection(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (now - last_wifi_check < 5000)
        return;

    last_wifi_check = now;

    int link_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

    if (link_status != CYW43_LINK_UP)
    {
        if (wifi_connected)
        {
            printf("\nCore 1: WiFi connection lost! Attempting reconnect...\n");
            wifi_connected = false;
            wifi_fully_connected = false;
        }

        try_wifi_connect();
    }
    else
    {
        if (!wifi_connected)
        {
            wifi_connected = true;
            wifi_fully_connected = true;
            printf("Core 1: WiFi reconnected!\n");
        }
    }
}

// [------------------------------------------------------------------------- HTTPS with Proper Cleanup -------------------------------------------------------------------------]

void dns_callback(const char* name, const ip_addr_t* ipaddr, void* arg)
{
    if (ipaddr) {
        ip_addr_t* result = (ip_addr_t*)arg;
        *result = *ipaddr;
        VPRINTF("DNS resolved: %s\n", ip4addr_ntoa(ipaddr));
    } else {
        VPRINTF("DNS resolution failed\n");
    }
}

err_t https_connected_callback(void* arg, struct altcp_pcb* tpcb, err_t err)
{
    https_state_t* state = (https_state_t*)arg;
    
    if (err == ERR_OK) {
        state->connected = true;
        VPRINTF("TLS handshake complete!\n");
    } else {
        VPRINTF("Connection failed: %d\n", err);
    }
    
    return ERR_OK;
}

err_t https_recv_callback(void* arg, struct altcp_pcb* tpcb, struct pbuf* p, err_t err)
{
    https_state_t* state = (https_state_t*)arg;
    
    if (p == NULL) {
        VPRINTF("Connection closed by server\n");
        return ERR_OK;
    }
    
    state->bytes_received += p->tot_len;
    
    altcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    
    return ERR_OK;
}

void https_err_callback(void* arg, err_t err)
{
    VPRINTF("Connection error: %d\n", err);
    https_state_t* state = (https_state_t*)arg;
    state->connected = false;
}

void send_webhook_post_with_cleanup(health_data_t* data)
{
    if (https_state.operation_in_progress) {
        VPRINTF("Operation already in progress, skipping\n");
        return;
    }

    webhook_in_progress = true;
    https_state.operation_in_progress = true;
    https_state.operation_start_time = to_ms_since_boot(get_absolute_time());
    
    // Store a copy of the data
    https_state.pending_data = *data;
    
    printf("POST[%lu]...\n", sample_count);
    fflush(stdout);

    // Step 1: DNS Resolution
    ip_addr_t server_ip = {0};
    VPRINTF("\nResolving %s...\n", WEBHOOK_HOSTNAME);
    
    err_t dns_err = dns_gethostbyname(WEBHOOK_HOSTNAME, &server_ip, dns_callback, &server_ip);
    
    if (dns_err == ERR_INPROGRESS) {
        int timeout = 0;
        while (server_ip.addr == 0 && timeout < 50) {
            cyw43_arch_poll();
            sleep_ms(100);
            timeout++;
        }
    }

    if (server_ip.addr == 0) {
        printf("DNS fail\n");
        https_state.operation_in_progress = false;
        webhook_in_progress = false;
        return;
    }

    VPRINTF("Resolved to: %s\n", ip4addr_ntoa(&server_ip));

    // Step 2: Create TLS Config (fresh for each connection)
    u8_t ca_cert[] = CA_CERT;
    
#ifdef MTLS_ENABLED
    // mTLS: Mutual authentication with client certificate
    u8_t client_cert[] = CLIENT_CERT;
    u8_t client_key[] = CLIENT_KEY;
    
    https_state.tls_config = altcp_tls_create_config_client_2wayauth(
        ca_cert, sizeof(ca_cert),
        client_key, sizeof(client_key),
        NULL, 0,
        client_cert, sizeof(client_cert)
    );
    // Add MTLS Handshake LED 8

#else
    // Standard TLS: Server authentication only
    https_state.tls_config = altcp_tls_create_config_client(
        ca_cert, sizeof(ca_cert)
    );
#endif

    if (!https_state.tls_config) {
        printf("TLS cfg fail\n");
        https_state.operation_in_progress = false;
        webhook_in_progress = false;
        return;
    }

    // Step 3: Create new PCB
    https_state.pcb = altcp_tls_new(https_state.tls_config, IPADDR_TYPE_V4);

    if (!https_state.pcb) {
        printf("PCB fail\n");
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

    VPRINTF("Connecting to %s:443...\n", WEBHOOK_HOSTNAME);
    
    // Step 6: Connect
    err_t connect_err = altcp_connect(https_state.pcb, &server_ip, 443, https_connected_callback);

    if (connect_err != ERR_OK) {
        printf("Connect fail:%d\n", connect_err);
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
    while (!https_state.connected && timeout < 100) {
        cyw43_arch_poll();
        sleep_ms(100);
        timeout++;
    }

    if (!https_state.connected) {
        printf("Timeout\n");
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

    char request[600];
    int req_len = snprintf(request, sizeof(request),
                           "POST /%s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %d\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "%s",
                           WEBHOOK_TOKEN, WEBHOOK_HOSTNAME, body_len, json_body);

    VPRINTF("Sending request...\n");

    // Make this reuse previous connection
    err_t write_err = altcp_write(https_state.pcb, request, req_len, TCP_WRITE_FLAG_COPY);

    if (write_err == ERR_OK) {
        altcp_output(https_state.pcb);
        https_state.request_sent = true;

        // Wait for response (shorter timeout)
        for (int i = 0; i < 20; i++) {
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
        
        // If WiFi disconnected, attempt to reconnect
        if (!wifi_connected)
        {
            printf("Core 1: WiFi disconnected, attempting reconnection...\n");
            gpio_put(WIFI_LED_PIN, 0);  // Turn off LED during reconnection
            
            // Blink during delay
            int blink_cycles = WIFI_RECONNECT_DELAY_MS / 500;
            for (int i = 0; i < blink_cycles; i++) {
                gpio_put(WIFI_LED_PIN, 1);
                sleep_ms(250);
                gpio_put(WIFI_LED_PIN, 0);
                sleep_ms(250);
            }
            
            // Attempt reconnection
            if (try_wifi_connect()) {
                gpio_put(WIFI_LED_PIN, 1);  // Turn LED back on when connected
            }
        }
        
        // Handle webhook trigger
        if (webhook_trigger && wifi_connected && !webhook_in_progress)
        {
            webhook_trigger = false;
            send_webhook_post_with_cleanup(&current_health);
        }
        
        sleep_ms(50);
    }
}

// [------------------------------------------------------------------------- SD Read/Write -------------------------------------------------------------------------]

bool try_sd_mount(void)
{
    FRESULT fr = f_mount(&fs, "0:", 1);
    if (fr == FR_OK) {
        return true;
    }
    
    sleep_ms(500);
    fr = f_mount(&fs, "0:", 1);
    return (fr == FR_OK);
}

bool init_sd_card(void)
{
    printf("Initializing SD card...\n");
    sleep_ms(100);

    if (try_sd_mount()) {
        sd_mounted = true;
        printf("SD card mounted successfully\n");
        return true;
    }

    printf("SD card mount failed\n");
    return false;
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
        VPRINTF("Failed to open log file: %d\n", fr);
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
        VPRINTF("Failed to write to log: %d\n", fr);
    }
    else
    {
        VPRINTF("Logged disconnect event to SD card\n");
    }

    f_close(&fil);
}




// TODO DO NOT REMOVE COMMENTS

// Blinking Logic 
// Off = Fail
// Blinking = In process
// On = Success

// LED 6 WIFI Connection status
// LED 7 DNS Status
// LED 8 MTLS Status 
// LED 9 Write to server Fail (blinking only else off)

// remove FATFS (no longer being used)



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