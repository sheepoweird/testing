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

#define WIFI_SSID "Anthrooo"
#define WIFI_PASSWORD "abcd1234"

#define HID_BUTTON_PIN 20
#define WEBHOOK_BUTTON_PIN 21
#define LED_PIN 6
#define RX_BUFFER_SIZE 512
#define DATA_TIMEOUT_MS 10000
#define MAX_SEQ 512

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
bool establish_mtls_connection(void);
void send_post_request(void);
void send_webhook_post(void);
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
    struct altcp_pcb* pcb;  // Persistent PCB for keeping connection alive
    ip_addr_t server_ip;
    bool connected;
    bool handshake_complete;
    bool request_sent;
    uint16_t bytes_received;
} https_state_t;

static https_state_t https_state = {0};

// Add this new variable for inter-core communication:
static volatile bool webhook_trigger = false;

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

void tud_mount_cb(void) {}
void tud_umount_cb(void) {}
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

    if (board_millis() - start_ms < interval_ms)
        return;
    start_ms = board_millis();

    uint32_t const btn = !gpio_get(HID_BUTTON_PIN);

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
                uint8_t kc[6] = {act.key, 0, 0, 0, 0, 0};
                int pack_count = 1;

                while (pack_count < 6 && seq_index < seq_len &&
                       sequence[seq_index].key &&
                       sequence[seq_index].modifier == act.modifier)
                {
                    kc[pack_count] = sequence[seq_index].key;
                    pack_count++;
                    seq_index++;
                }

                tud_hid_keyboard_report(REPORT_ID_KEYBOARD, act.modifier, kc);
            }
            else
            {
                tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);
            }
        }
        else if (seq_index >= seq_len)
        {
            state = 2;
        }
        break;

    case 2:
        if (!btn)
            state = 0;
        break;
    }
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)instance;
    (void)report;
    (void)len;
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

// [------------------------------------------------------------------------- CDC -------------------------------------------------------------------------]

void process_json_data(char *json)
{
    if (!json || strlen(json) == 0)
        return;

    uint32_t receive_time = to_ms_since_boot(get_absolute_time());
    health_data_t new_data = {0};

    char *cpu_ptr = strstr(json, "\"cpu\":");
    char *mem_ptr = strstr(json, "\"memory\":");
    char *disk_ptr = strstr(json, "\"disk\":");
    char *temp_ptr = strstr(json, "\"cpu_temp\":");
    char *net_in_ptr = strstr(json, "\"net_in\":");
    char *net_out_ptr = strstr(json, "\"net_out\":");
    char *proc_ptr = strstr(json, "\"processes\":");

    if (cpu_ptr)
        new_data.cpu = atof(cpu_ptr + 6);
    if (mem_ptr)
        new_data.memory = atof(mem_ptr + 9);
    if (disk_ptr)
        new_data.disk = atof(disk_ptr + 7);
    if (temp_ptr)
    {
        new_data.cpu_temp = atof(temp_ptr + 11);
        if (strstr(temp_ptr + 11, "null"))
            new_data.cpu_temp = 0;
    }
    if (net_in_ptr)
        new_data.net_in = atof(net_in_ptr + 9);
    if (net_out_ptr)
        new_data.net_out = atof(net_out_ptr + 10);
    if (proc_ptr)
        new_data.processes = atoi(proc_ptr + 12);

    if (cpu_ptr && mem_ptr && disk_ptr)
    {
        current_health = new_data;
        current_health.valid = true;
        is_connected = true;

        uint32_t time_since_last = (last_data_time > 0) ? (receive_time - last_data_time) : 0;
        last_data_time = receive_time;

        sample_count++;
        if (sample_count > 9999)
            sample_count = 1;

        display_compact_status();

        if (time_since_last > 0)
        {
            printf("[TIMING] Gap since last sample: %lu ms\n", time_since_last);
        }
    }
}

void display_compact_status(void)
{
    printf("[%04lu] CPU=%5.1f%% | RAM=%5.1f%% | DISK=%5.1f%% | TEMP=%5.1fC | NET=D%6.1f U%6.1f KB/s | PROC=%d\n",
           sample_count,
           current_health.cpu,
           current_health.memory,
           current_health.disk,
           current_health.cpu_temp,
           current_health.net_in,
           current_health.net_out,
           current_health.processes);
}

// [------------------------------------------------------------------------- LED -------------------------------------------------------------------------]

void led_blinking_task(void)
{
    static uint32_t last_toggle = 0;
    uint32_t now = board_millis();

    if (is_connected)
    {
        if (now - last_toggle >= 100)
        {
            gpio_put(LED_PIN, !gpio_get(LED_PIN));
            last_toggle = now;
        }
    }
    else
    {
        gpio_put(LED_PIN, 0);
    }
}

// [------------------------------------------------------------------------- Wifi -------------------------------------------------------------------------]

static bool wifi_connected = false;
static uint32_t last_wifi_check = 0;
#define WIFI_CHECK_INTERVAL_MS 10000 // Check every 10 seconds

bool init_wifi(void)
{
    printf("=== Starting WiFi Initialization ===\n");

    if (cyw43_arch_init())
    {
        printf("ERROR: WiFi init failed\n");
        return false;
    }
    printf("WiFi chip initialized\n");

    cyw43_arch_enable_sta_mode();

    // Reduce WiFi power to avoid current spikes
    cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);

    printf("Station mode enabled\n");

    return try_wifi_connect();
}

bool try_wifi_connect(void)
{
    printf("Connecting to: %s\n", WIFI_SSID);
    printf("Please wait...\n");

    int result = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID,
        WIFI_PASSWORD,
        CYW43_AUTH_WPA2_AES_PSK,
        30000  // 30 second timeout
    );

    // Check detailed status
    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    printf("Link status after connect: %d\n", status);
    
    if (result != 0)
    {
        printf("ERROR: Failed to connect (error code: %d)\n", result);
        
        // Detailed error codes
        switch(result) {
            case PICO_ERROR_TIMEOUT:
                printf("  -> Timeout: Check if SSID '%s' exists and is in range\n", WIFI_SSID);
                break;
            case PICO_ERROR_GENERIC:
                printf("  -> Generic error: Check password\n");
                break;
            default:
                printf("  -> Unknown error\n");
                break;
        }
        
        wifi_connected = false;
        return false;
    }

    printf("=== WiFi Connected Successfully! ===\n");

    // Print IP address
    extern cyw43_t cyw43_state;
    uint32_t ip = cyw43_state.netif[0].ip_addr.addr;
    
    if (ip == 0) {
        printf("ERROR: Connected but no IP address assigned!\n");
        wifi_connected = false;
        return false;
    }
    
    printf("IP Address: %d.%d.%d.%d\n",
           ip & 0xFF,
           (ip >> 8) & 0xFF,
           (ip >> 16) & 0xFF,
           (ip >> 24) & 0xFF);

    wifi_connected = true;
    last_wifi_check = to_ms_since_boot(get_absolute_time());
    return true;
}

void check_wifi_connection(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Only check periodically
    if (now - last_wifi_check < WIFI_CHECK_INTERVAL_MS)
    {
        return;
    }

    last_wifi_check = now;

    // Check if we're still connected
    int link_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

    // Only reconnect if truly disconnected (not just "no packets")
    if (link_status == CYW43_LINK_DOWN || link_status == CYW43_LINK_FAIL || link_status == CYW43_LINK_NONET)
    {
        if (wifi_connected)
        {
            printf("WiFi connection lost! Attempting to reconnect...\n");
            wifi_connected = false;
        }

        // Try to reconnect
        try_wifi_connect();
    }
    else if (link_status == CYW43_LINK_UP)
    {
        if (!wifi_connected)
        {
            printf("WiFi connection restored!\n");
        }
        wifi_connected = true;
    }
}

// [------------------------------------------------------------------------- HTTPS Callbacks -------------------------------------------------------------------------]

void dns_callback(const char* name, const ip_addr_t* ipaddr, void* arg)
{
    if (ipaddr) {
        printf("DNS resolved: %s -> %s\n", name, ip4addr_ntoa(ipaddr));
        *((ip_addr_t*)arg) = *ipaddr;
    } else {
        printf("DNS resolution failed for %s\n", name);
        ((ip_addr_t*)arg)->addr = 0;
    }
}

err_t https_connected_callback(void* arg, struct altcp_pcb* tpcb, err_t err)
{
    (void)arg;
    
    if (err != ERR_OK) {
        printf("HTTPS connection failed: %d\n", err);
        return err;
    }

    printf("HTTPS connection established!\n");
    https_state.connected = true;
    
    return ERR_OK;
}

err_t https_recv_callback(void* arg, struct altcp_pcb* tpcb, struct pbuf* p, err_t err)
{
    (void)arg;
    
    if (err != ERR_OK || p == NULL) {
        if (p) pbuf_free(p);
        return err;
    }

    // Print response
    struct pbuf* current = p;
    while (current != NULL) {
        printf("%.*s", current->len, (char*)current->payload);
        https_state.bytes_received += current->len;
        current = current->next;
    }

    altcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    
    return ERR_OK;
}

void https_err_callback(void* arg, err_t err)
{
    (void)arg;
    printf("HTTPS error callback: %d\n", err);
    
    if (https_state.tls_config) {
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
    }
    
    https_state.connected = false;
}

// [------------------------------------------------------------------------- Webhook POST -------------------------------------------------------------------------]

bool establish_mtls_connection(void)
{
    if (!wifi_connected)
    {
        printf("Cannot establish mTLS - WiFi not connected\n");
        return false;
    }

    printf("\n=== Establishing mTLS Connection ===\n");

    // Reset state
    https_state.connected = false;
    https_state.handshake_complete = false;
    https_state.request_sent = false;
    https_state.bytes_received = 0;

    // Step 1: Resolve DNS
    https_state.server_ip.addr = 0;

    printf("Resolving %s...\n", WEBHOOK_HOSTNAME);

    err_t dns_err = dns_gethostbyname(WEBHOOK_HOSTNAME, &https_state.server_ip, dns_callback, &https_state.server_ip);
    printf("[Stage] DNS result: %d, ip=%s\n", dns_err, ip4addr_ntoa(&https_state.server_ip));

    if (dns_err == ERR_INPROGRESS) {
        // Wait for DNS resolution
        int timeout = 0;
        while (https_state.server_ip.addr == 0 && timeout < 100) {
            sleep_ms(100);
            timeout++;
        }
    }

    if (https_state.server_ip.addr == 0) {
        printf("DNS resolution failed\n");
        return false;
    }

    printf("Resolved to: %s\n", ip4addr_ntoa(&https_state.server_ip));

    // Step 2: Create mTLS configuration
    u8_t ca_cert[] = CA_CERT;
    u8_t client_cert[] = CLIENT_CERT;
    u8_t client_key[] = CLIENT_KEY;

    printf("[Stage] Creating TLS config\n");
    https_state.tls_config = altcp_tls_create_config_client_2wayauth(
        ca_cert, sizeof(ca_cert),
        client_key, sizeof(client_key),
        NULL, 0,
        client_cert, sizeof(client_cert)
    );

    if (!https_state.tls_config) {
        printf("Failed to create TLS config\n");
        return false;
    }

    // Step 3: Create mTLS PCB
    https_state.pcb = altcp_tls_new(https_state.tls_config, IPADDR_TYPE_V4);

    if (!https_state.pcb) {
        printf("Failed to create TLS PCB\n");
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
        return false;
    }

    // Step 4: Set SNI hostname
    int mbedtls_err = mbedtls_ssl_set_hostname(
        &(((altcp_mbedtls_state_t*)(https_state.pcb->state))->ssl_context),
        WEBHOOK_HOSTNAME
    );

    if (mbedtls_err != 0) {
        printf("Failed to set SNI hostname\n");
        altcp_close(https_state.pcb);
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
        https_state.pcb = NULL;
        return false;
    }

    // Step 5: Set up callbacks
    altcp_arg(https_state.pcb, &https_state);
    altcp_err(https_state.pcb, https_err_callback);
    altcp_recv(https_state.pcb, https_recv_callback);

    // Step 6: Connect and perform handshake
    printf("Connecting to %s:443...\n", WEBHOOK_HOSTNAME);

    err_t connect_err = altcp_connect(https_state.pcb, &https_state.server_ip, 443, https_connected_callback);

    if (connect_err != ERR_OK) {
        printf("Connection failed: %d\n", connect_err);
        altcp_close(https_state.pcb);
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
        https_state.pcb = NULL;
        return false;
    }

    // Wait for connection and handshake - poll lwIP to process packets
    int timeout = 0;
    while (!https_state.connected && timeout < 150) {
        cyw43_arch_poll();  // Let lwIP process packets
        sleep_ms(100);
        timeout++;
    }

    if (!https_state.connected) {
        printf("Connection/Handshake timeout\n");
        altcp_close(https_state.pcb);
        https_state.pcb = NULL;
        return false;
    }

    https_state.handshake_complete = true;
    printf("=== mTLS Handshake Complete! Connection ready. ===\n\n");

    return true;
}

void send_post_request(void)
{
    if (!https_state.connected || !https_state.handshake_complete || !https_state.pcb)
    {
        printf("Cannot send POST - mTLS connection not established\n");
        printf("  Connected: %d, Handshake: %d, PCB: %p\n",
               https_state.connected, https_state.handshake_complete, https_state.pcb);

        // Try to re-establish connection
        printf("Attempting to re-establish mTLS connection...\n");
        if (!establish_mtls_connection()) {
            printf("Failed to re-establish connection\n");
            return;
        }
    }

    printf("\n=== Sending POST Request ===\n");

    // Reset response state
    https_state.bytes_received = 0;

    // Prepare JSON body
    char json_body[256];
    int body_len = snprintf(json_body, sizeof(json_body),
                            "{\"button\":\"GP21 pressed\",\"timestamp\":%lu,\"device\":\"Pico-W\"}",
                            to_ms_since_boot(get_absolute_time()));

    // Prepare HTTP POST request with keep-alive
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

    printf("Sending POST data over existing mTLS connection...\n");

    err_t write_err = altcp_write(https_state.pcb, request, req_len, TCP_WRITE_FLAG_COPY);

    if (write_err == ERR_OK) {
        altcp_output(https_state.pcb);

        printf("Request sent! Waiting for response...\n");
        https_state.request_sent = true;

        // Wait for response - poll lwIP
        for (int i = 0; i < 30; i++) {
            cyw43_arch_poll();
            sleep_ms(100);
        }

        // Blink LED to confirm
        for (int i = 0; i < 6; i++) {
            gpio_put(LED_PIN, 1);
            sleep_ms(50);
            gpio_put(LED_PIN, 0);
            sleep_ms(50);
        }

        printf("Received %d bytes\n", https_state.bytes_received);
    } else {
        printf("Failed to send request: %d\n", write_err);

        // Connection might be broken, mark for reconnection
        https_state.connected = false;
        https_state.handshake_complete = false;
    }

    printf("=== POST Request complete ===\n\n");
}

void send_webhook_post(void)
{
    if (!wifi_connected)
    {
        printf("Cannot send POST - WiFi not connected\n");
        return;
    }

    printf("\n=== Sending HTTPS POST to webhook.site ===\n");

    // Reset state
    https_state.connected = false;
    https_state.request_sent = false;
    https_state.bytes_received = 0;

    // Step 1: Resolve DNS
    ip_addr_t server_ip;
    server_ip.addr = 0;

    printf("Resolving %s...\n", WEBHOOK_HOSTNAME);

    err_t dns_err = dns_gethostbyname(WEBHOOK_HOSTNAME, &server_ip, dns_callback, &server_ip);
    printf("[Stage] DNS result: %d, ip=%s\n", dns_err, ip4addr_ntoa(&server_ip));

    if (dns_err == ERR_INPROGRESS) {
        // Wait for DNS resolution
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

    // Step 2: Create mTLS configuration    
    u8_t ca_cert[] = CA_CERT;
    u8_t client_cert[] = CLIENT_CERT;
    // static const u8_t client_key[] = CLIENT_KEY;
    u8_t client_key[] = CLIENT_KEY;

    printf("[Stage] Creating TLS config\n");
    // https_state.tls_config = altcp_tls_create_config_client(ca_cert, sizeof(ca_cert)); //code responsible for one way handshake
    https_state.tls_config = altcp_tls_create_config_client_2wayauth(
    ca_cert, sizeof(ca_cert),
    client_key, sizeof(client_key),
    NULL, 0,                          // or password + length if encrypted key
    client_cert, sizeof(client_cert)
    );



    if (!https_state.tls_config) {
        printf("Failed to create TLS config\n");
        return;
    }

    // Step 3: Create mTLS PCB
    struct altcp_pcb* pcb = altcp_tls_new(https_state.tls_config, IPADDR_TYPE_V4);

    if (!pcb) {
        printf("Failed to create TLS PCB\n");
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
        return;
    }

    // Step 4: Set SNI hostname
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

    // Step 5: Set up callbacks
    altcp_arg(pcb, &https_state);
    altcp_err(pcb, https_err_callback);
    altcp_recv(pcb, https_recv_callback);

    // Step 6: Connect
    printf("Connecting to %s:443...\n", WEBHOOK_HOSTNAME);
    
    err_t connect_err = altcp_connect(pcb, &server_ip, 443, https_connected_callback);

    if (connect_err != ERR_OK) {
        printf("Connection failed: %d\n", connect_err);
        altcp_close(pcb);
        altcp_tls_free_config(https_state.tls_config);
        https_state.tls_config = NULL;
        return;
    }

    // Wait for connection - poll lwIP to process packets
    int timeout = 0;
    while (!https_state.connected && timeout < 150) {
        cyw43_arch_poll();  // Let lwIP process packets
        sleep_ms(100);
        timeout++;
    }

    if (!https_state.connected) {
        printf("Connection timeout\n");
        altcp_close(pcb);
        return;
    }

    // Step 7: Send HTTPS POST request
    char json_body[256];
    int body_len = snprintf(json_body, sizeof(json_body),
                            "{\"button\":\"GP21 pressed\",\"timestamp\":%lu,\"device\":\"Pico-W\"}",
                            to_ms_since_boot(get_absolute_time()));

    char request[512];
    int req_len = snprintf(request, sizeof(request),
                           "POST /%s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %d\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "%s",
                           WEBHOOK_TOKEN, WEBHOOK_HOSTNAME, body_len, json_body);

    printf("Sending HTTPS request...\n");
    
    err_t write_err = altcp_write(pcb, request, req_len, TCP_WRITE_FLAG_COPY);

    if (write_err == ERR_OK) {
        altcp_output(pcb);
        
        printf("Request sent! Waiting for response...\n");
        https_state.request_sent = true;

        // Wait for response - poll lwIP
        for (int i = 0; i < 30; i++) {
            cyw43_arch_poll();
            sleep_ms(100);
        }

        // Blink LED to confirm
        for (int i = 0; i < 6; i++) {
            gpio_put(LED_PIN, 1);
            sleep_ms(50);
            gpio_put(LED_PIN, 0);
            sleep_ms(50);
        }

        printf("\nReceived %d bytes\n", https_state.bytes_received);
    } else {
        printf("Failed to send request: %d\n", write_err);
    }

    // Cleanup
    altcp_close(pcb);

    printf("=== HTTPS POST complete ===\n\n");
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
            webhook_trigger = true;  // Signal Core 1 to send
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

    if (init_wifi())
    {
        printf("Core 1: WiFi ready!\n");

        // Automatically establish mTLS connection after WiFi is connected
        printf("Core 1: Establishing mTLS connection automatically...\n");
        if (establish_mtls_connection())
        {
            printf("Core 1: mTLS connection established and ready!\n");
        }
        else
        {
            printf("Core 1: mTLS connection failed - will retry on button press\n");
        }
    }
    else
    {
        printf("Core 1: WiFi failed\n");
    }

    // Core 1 main loop - handle WiFi tasks AND webhook
    bool mtls_established = https_state.handshake_complete;

    while (true)
    {
        check_wifi_connection();

        // If WiFi was reconnected and mTLS is not established, re-establish it
        if (wifi_connected && !https_state.handshake_complete && !mtls_established)
        {
            printf("Core 1: WiFi reconnected, re-establishing mTLS...\n");
            if (establish_mtls_connection())
            {
                printf("Core 1: mTLS connection re-established!\n");
                mtls_established = true;
            }
        }

        // Update flag based on current state
        mtls_established = https_state.handshake_complete;

        // Check if Core 0 requested a webhook POST
        if (webhook_trigger && wifi_connected)
        {
            webhook_trigger = false;  // Clear flag
            send_post_request();  // Use new function that reuses connection
        }

        sleep_ms(50);  // Check more frequently for responsiveness
    }
}

// [------------------------------------------------------------------------- SD Read/Write -------------------------------------------------------------------------]

bool init_sd_card(void)
{
    printf("Initializing SD card...\n");

    // Small delay to ensure SPI bus is stable
    sleep_ms(100);

    // Initialize SD card
    FRESULT fr = f_mount(&fs, "0:", 1);
    if (fr != FR_OK)
    {
        printf("SD card mount failed: %d\n", fr);

        // Try once more after delay
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

    // Open file in append mode, create if doesn't exist
    fr = f_open(&fil, "0:/pico_logs.txt", FA_WRITE | FA_OPEN_APPEND);
    if (fr != FR_OK)
    {
        printf("Failed to open log file: %d\n", fr);
        return;
    }

    // Get timestamp
    uint32_t timestamp = to_ms_since_boot(get_absolute_time());

    // Create log message
    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg),
             "[%lu ms] DISCONNECT - Sample count was %lu\n",
             timestamp, sample_count);

    // Write to file
    fr = f_write(&fil, log_msg, strlen(log_msg), &bytes_written);
    if (fr != FR_OK)
    {
        printf("Failed to write to log: %d\n", fr);
    }
    else
    {
        printf("Logged disconnect event to SD card\n");
    }

    // Close file to ensure data is saved
    f_close(&fil);
}