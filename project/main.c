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

// Multicore, run wifi and sd card separately
#include "pico/multicore.h"

#include "pico/cyw43_arch.h"

#include "lwip/dns.h"
#include "lwip/altcp_tls.h"
#include "altcp_tls_mbedtls_structs.h"
#include "lwip/prot/iana.h"
#include "mbedtls/ssl.h"
#include "mbedtls/check_config.h"
#include "picohttps.h"


#define WIFI_SSID "Zzz"
#define WIFI_PASSWORD "i6b22krm"

#define HID_BUTTON_PIN 20
#define WEBHOOK_BUTTON_PIN 22

// SD Card LED status
// Pin 6 = Wifi status
// Pin 7 = Handshake connection to web server (not implemented)
// Pin 8 = SD Card status
// Pin 9 = Error Status pin eg. wifi error = (Pin 6 + Pin 9) or wifi & Handshake error = (Pin 6 + Pin 7 + Pin 9)

// 6 currently SD card
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
void check_webhook_button(void);
void core1_entry(void);

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

// Sequence of operations on boot
// 1. Wifi Successful
// 2. ATEC Chip ???  (not ready yet later implement)
// 3. SD card MSC successfully loaded
// 4. trigger HID to open file
// 5. Python file returns json data back with CDC serial
// 6. Send HTTPS POST request to web monitoring server. (Every 5 seconds)

// have GP20 as HID button
// have GP22 as webhook.site button


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
    
    printf("IP Address: %lu.%lu.%lu.%lu\n",
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
    int link_status = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);

    if (link_status == CYW43_LINK_UP)
    {
        if (!wifi_connected)
        {
            printf("WiFi connection restored!\n");
            wifi_connected = true;
        }
    }
    else
    {
        if (wifi_connected)
        {
            printf("WiFi connection lost! Attempting to reconnect...\n");
            wifi_connected = false;
        }

        // Try to reconnect
        try_wifi_connect();
    }
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
            printf("\n>>> GP22 Button Pressed! <<<\n");  // Changed from GP21
            send_https_post();  // Changed function name
            debounce_time = now;
        }
    }

    last_button_state = current_state;
}


// [------------------------------------------------------------------------- HTTPS Helper Functions -------------------------------------------------------------------------]

// Resolve hostname using DNS
bool resolve_hostname(ip_addr_t* ipaddr) {
    ipaddr->addr = IPADDR_ANY;
    
    cyw43_arch_lwip_begin();
    err_t dns_err = dns_gethostbyname(
        PICOHTTPS_HOSTNAME,
        ipaddr,
        NULL,  // No callback needed for synchronous approach
        NULL
    );
    cyw43_arch_lwip_end();
    
    if (dns_err == ERR_INPROGRESS) {
        // Wait for DNS resolution
        int retries = 50;  // 5 seconds timeout
        while (ipaddr->addr == IPADDR_ANY && retries-- > 0) {
            sleep_ms(100);
        }
        
        if (ipaddr->addr == IPADDR_NONE || ipaddr->addr == IPADDR_ANY) {
            printf("DNS resolution failed\n");
            return false;
        }
    } else if (dns_err != ERR_OK) {
        printf("DNS error: %d\n", dns_err);
        return false;
    }
    
    return true;
}

// Free ALTCP PCB
void altcp_free_pcb(struct altcp_pcb* pcb) {
    cyw43_arch_lwip_begin();
    err_t err = altcp_close(pcb);
    cyw43_arch_lwip_end();
    
    int retries = 10;
    while (err != ERR_OK && retries-- > 0) {
        sleep_ms(PICOHTTPS_ALTCP_CONNECT_POLL_INTERVAL);
        cyw43_arch_lwip_begin();
        err = altcp_close(pcb);
        cyw43_arch_lwip_end();
    }
}

// Free ALTCP TLS config
void altcp_free_config(struct altcp_tls_config* config) {
    cyw43_arch_lwip_begin();
    altcp_tls_free_config(config);
    cyw43_arch_lwip_end();
}

// Free callback argument
void altcp_free_arg(struct altcp_callback_arg* arg) {
    if (arg) {
        free(arg);
    }
}

// ALTCP callbacks
void callback_altcp_err(void* arg, err_t err) {
    printf("Connection error: %d\n", err);
    
    if (arg) {
        struct altcp_callback_arg* cb_arg = (struct altcp_callback_arg*)arg;
        if (cb_arg->config) {
            altcp_free_config(cb_arg->config);
        }
        altcp_free_arg(cb_arg);
    }
}

err_t callback_altcp_poll(void* arg, struct altcp_pcb* pcb) {
    (void)arg;
    (void)pcb;
    return ERR_OK;
}

err_t callback_altcp_sent(void* arg, struct altcp_pcb* pcb, u16_t len) {
    (void)pcb;
    ((struct altcp_callback_arg*)arg)->acknowledged = len;
    return ERR_OK;
}

err_t callback_altcp_recv(void* arg, struct altcp_pcb* pcb, struct pbuf* buf, err_t err) {
    (void)arg;
    
    if (err == ERR_OK && buf) {
        // Print received data
        struct pbuf* p = buf;
        while (p != NULL) {
            printf("%.*s", p->len, (char*)p->payload);
            p = p->next;
        }
        
        altcp_recved(pcb, buf->tot_len);
        pbuf_free(buf);
    } else if (err == ERR_ABRT) {
        if (buf) pbuf_free(buf);
        return ERR_ABRT;
    }
    
    return ERR_OK;
}

err_t callback_altcp_connect(void* arg, struct altcp_pcb* pcb, err_t err) {
    (void)pcb;
    if (err == ERR_OK) {
        ((struct altcp_callback_arg*)arg)->connected = true;
    }
    return ERR_OK;
}

// Establish HTTPS connection
bool connect_to_host(ip_addr_t* ipaddr, struct altcp_pcb** pcb) {
    // Create TLS config with CA certificate
    u8_t ca_cert[] = PICOHTTPS_CA_ROOT_CERT;
    
    cyw43_arch_lwip_begin();
    struct altcp_tls_config* config = altcp_tls_create_config_client(
        ca_cert,
        sizeof(ca_cert)
    );
    cyw43_arch_lwip_end();
    
    if (!config) {
        printf("Failed to create TLS config\n");
        return false;
    }
    
    // Create ALTCP PCB with TLS
    cyw43_arch_lwip_begin();
    *pcb = altcp_tls_new(config, IPADDR_TYPE_V4);
    cyw43_arch_lwip_end();
    
    if (!(*pcb)) {
        printf("Failed to create TLS PCB\n");
        altcp_free_config(config);
        return false;
    }
    
    // Set SNI hostname
    cyw43_arch_lwip_begin();
    int mbedtls_err = mbedtls_ssl_set_hostname(
        &(((altcp_mbedtls_state_t*)((*pcb)->state))->ssl_context),
        PICOHTTPS_HOSTNAME
    );
    cyw43_arch_lwip_end();
    
    if (mbedtls_err) {
        printf("Failed to set SNI hostname: %d\n", mbedtls_err);
        altcp_free_pcb(*pcb);
        altcp_free_config(config);
        return false;
    }
    
    // Allocate callback argument
    struct altcp_callback_arg* arg = malloc(sizeof(*arg));
    if (!arg) {
        printf("Failed to allocate callback arg\n");
        altcp_free_pcb(*pcb);
        altcp_free_config(config);
        return false;
    }
    
    arg->config = config;
    arg->connected = false;
    arg->acknowledged = 0;
    
    // Set callbacks
    cyw43_arch_lwip_begin();
    altcp_arg(*pcb, arg);
    altcp_err(*pcb, callback_altcp_err);
    altcp_poll(*pcb, callback_altcp_poll, PICOHTTPS_ALTCP_IDLE_POLL_INTERVAL);
    altcp_sent(*pcb, callback_altcp_sent);
    altcp_recv(*pcb, callback_altcp_recv);
    cyw43_arch_lwip_end();
    
    // Connect
    cyw43_arch_lwip_begin();
    err_t conn_err = altcp_connect(*pcb, ipaddr, LWIP_IANA_PORT_HTTPS, callback_altcp_connect);
    cyw43_arch_lwip_end();
    
    if (conn_err == ERR_OK) {
        // Wait for connection
        int retries = 100;  // 10 seconds timeout
        while (!arg->connected && retries-- > 0) {
            sleep_ms(PICOHTTPS_ALTCP_CONNECT_POLL_INTERVAL);
        }
        
        if (!arg->connected) {
            printf("Connection timeout\n");
            altcp_free_pcb(*pcb);
            altcp_free_config(config);
            altcp_free_arg(arg);
            return false;
        }
    } else {
        printf("Connection failed: %d\n", conn_err);
        altcp_free_pcb(*pcb);
        altcp_free_config(config);
        altcp_free_arg(arg);
        return false;
    }
    
    return true;
}

// Send HTTPS POST request
bool send_request(struct altcp_pcb* pcb, const char* body) {
    int body_len = strlen(body);
    
    char request[512];
    int req_len = snprintf(request, sizeof(request),
        "POST /6aae6834-a1a9-4ea8-8518-7c821c2b0fee HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        PICOHTTPS_HOSTNAME, body_len, body
    );
    
    if (req_len <= 0 || req_len >= sizeof(request)) {
        printf("Request buffer overflow\n");
        return false;
    }
    
    ((struct altcp_callback_arg*)(pcb->arg))->acknowledged = 0;
    
    cyw43_arch_lwip_begin();
    err_t write_err = altcp_write(pcb, request, req_len, TCP_WRITE_FLAG_COPY);
    cyw43_arch_lwip_end();
    
    if (write_err != ERR_OK) {
        printf("Write error: %d\n", write_err);
        return false;
    }
    
    cyw43_arch_lwip_begin();
    err_t output_err = altcp_output(pcb);
    cyw43_arch_lwip_end();
    
    if (output_err != ERR_OK) {
        printf("Output error: %d\n", output_err);
        return false;
    }
    
    // Wait for acknowledgment
    int retries = 50;  // 5 seconds timeout
    while (((struct altcp_callback_arg*)(pcb->arg))->acknowledged == 0 && retries-- > 0) {
        sleep_ms(PICOHTTPS_HTTP_RESPONSE_POLL_INTERVAL);
    }
    
    if (((struct altcp_callback_arg*)(pcb->arg))->acknowledged != req_len) {
        printf("Incomplete acknowledgment\n");
        return false;
    }
    
    return true;
}

// [------------------------------------------------------------------------- Core 1 - WiFi Handler -------------------------------------------------------------------------]

void core1_entry(void)
{
    printf("Core 1: Starting WiFi on separate core\n");

    // Initialize WiFi on Core 1
    sleep_ms(1000);

    if (init_wifi())
    {
        printf("Core 1: WiFi ready!\n");
    }
    else
    {
        printf("Core 1: WiFi failed\n");
    }

    // Core 1 main loop - handle WiFi tasks
    while (true)
    {
        check_wifi_connection();
        sleep_ms(100); // Don't hog the core
    }
}

// [------------------------------------------------------------------------- HTTPS POST -------------------------------------------------------------------------]

void send_https_post(void) {
    if (!wifi_connected) {
        printf("Cannot send POST - WiFi not connected\n");
        return;
    }

    printf("\n=== Sending HTTPS POST to webhook.site ===\n");

    // Resolve hostname
    ip_addr_t server_ip;
    printf("Resolving %s...\n", PICOHTTPS_HOSTNAME);
    if (!resolve_hostname(&server_ip)) {
        printf("Failed to resolve hostname\n");
        return;
    }
    
    cyw43_arch_lwip_begin();
    char* ip_str = ipaddr_ntoa(&server_ip);
    cyw43_arch_lwip_end();
    printf("Resolved to: %s\n", ip_str);

    // Establish HTTPS connection
    struct altcp_pcb* pcb = NULL;
    printf("Connecting to https://%s:443...\n", ip_str);
    if (!connect_to_host(&server_ip, &pcb)) {
        printf("Failed to establish HTTPS connection\n");
        return;
    }
    printf("HTTPS connection established!\n");

    // Create JSON payload
    char json_body[256];
    int body_len;
    
    if (current_health.valid) {
        body_len = snprintf(json_body, sizeof(json_body),
            "{\"button_pressed\":true,\"timestamp\":%lu,\"device\":\"Pico-W\",\"samples\":%lu,\"cpu\":%.1f,\"memory\":%.1f,\"disk\":%.1f,\"temp\":%.1f}",
            to_ms_since_boot(get_absolute_time()), 
            sample_count,
            current_health.cpu,
            current_health.memory,
            current_health.disk,
            current_health.cpu_temp);
    } else {
        body_len = snprintf(json_body, sizeof(json_body),
            "{\"button_pressed\":true,\"timestamp\":%lu,\"device\":\"Pico-W\",\"samples\":%lu,\"status\":\"no_data\"}",
            to_ms_since_boot(get_absolute_time()), 
            sample_count);
    }

    printf("Payload: %s\n", json_body);

    // Send HTTPS POST request
    printf("Sending HTTPS POST request...\n");
    if (!send_request(pcb, json_body)) {
        printf("Failed to send request\n");
        
        // Error blink pattern
        for (int i = 0; i < 3; i++) {
            gpio_put(LED_PIN, 1);
            sleep_ms(300);
            gpio_put(LED_PIN, 0);
            sleep_ms(300);
        }
    } else {
        printf("Request sent successfully!\n");
        
        // Success blink pattern
        for (int i = 0; i < 8; i++) {
            gpio_put(LED_PIN, 1);
            sleep_ms(80);
            gpio_put(LED_PIN, 0);
            sleep_ms(80);
        }
    }

    // Wait for response
    printf("Waiting for response...\n");
    sleep_ms(5000);

    // Cleanup
    if (pcb) {
        struct altcp_callback_arg* arg = (struct altcp_callback_arg*)(pcb->arg);
        altcp_free_pcb(pcb);
        if (arg) {
            if (arg->config) {
                altcp_free_config(arg->config);
            }
            altcp_free_arg(arg);
        }
    }

    printf("=== HTTPS POST complete ===\n\n");
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