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
#include "lwip/altcp_tcp.h"
#include "lwip/altcp.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/ip4_addr.h"


#define WIFI_SSID "Zzz"
#define WIFI_PASSWORD "i6b22krm"

#define BUTTON_PIN 20
#define LED_PIN 6
#define RX_BUFFER_SIZE 512
#define DATA_TIMEOUT_MS 10000
#define MAX_SEQ 512

static FATFS fs;
static bool sd_mounted = false;

char rx_buffer[RX_BUFFER_SIZE];
int rx_index = 0;

// Type definition - MUST come before forward declarations that use it
typedef struct {
    float cpu;
    float memory;
    float disk;
    float cpu_temp;
    float net_in;
    float net_out;
    int processes;
    bool valid;
} health_data_t;

// NOW add forward declarations AFTER the typedef
bool init_sd_card(void);
void log_disconnect_event(void);
bool init_wifi(void);
void test_wifi_post(void);
void post_to_website(health_data_t* data);
void hid_task(void);
void led_blinking_task(void);
void process_json_data(char* json);
void display_compact_status(void);

// Global variables
health_data_t current_health = {0};
uint32_t last_data_time = 0;
uint32_t sample_count = 0;
bool is_connected = false;

int main(void)
{
    board_init();
    tusb_init();
    stdio_init_all();
    // added some delay so the host can detect the device properly
    sleep_ms(2000);
    
    // Clear terminal screen 
    printf("\033[2J\033[H");  
    
    tud_init(BOARD_TUD_RHPORT);
    
    // Initialize button input
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);

    // LED for Logging status indication
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);
    
    // Initialize SD card for logging
    if (!init_sd_card()) {
        printf("WARNING: SD card logging disabled\n");
    }

    // Initialize WiFi
    if (init_wifi()) {
        printf("WiFi ready!\n");
        sleep_ms(2000);  // Wait a bit
        test_wifi_post();  // Test POST once at startup
    } else {
        printf("WARNING: WiFi disabled\n");
    }

    while (true)
    {
        tud_task();
        led_blinking_task();
        hid_task();

        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (last_data_time > 0 && (now - last_data_time > DATA_TIMEOUT_MS)) {
            if (current_health.valid || is_connected) {
                // Log disconnect event to SD card
                log_disconnect_event();
                
                sample_count = 0;
                current_health.valid = false;
                is_connected = false;
                printf("\n[DISCONNECTED] Counter reset\n");
            }
        }

        int c = getchar_timeout_us(0);
        
        if(c != PICO_ERROR_TIMEOUT) {
            if(c == '\r' || c == '\n') {
                if(rx_index < RX_BUFFER_SIZE) {
                    rx_buffer[rx_index] = '\0';
                } else {
                    rx_buffer[RX_BUFFER_SIZE - 1] = '\0';
                }
                
                if(rx_index > 0 && rx_buffer[0] == '{') {
                    process_json_data(rx_buffer);
                }
                
                rx_index = 0;
            }
            else if(rx_index < RX_BUFFER_SIZE - 1) {
                rx_buffer[rx_index++] = c;
            }
        }
        
        tight_loop_contents();
    }

    return 0;
}

// [------------------------------------------------------------------------- MSC -------------------------------------------------------------------------]

void tud_mount_cb(void) { }
void tud_umount_cb(void) { }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; }
void tud_resume_cb(void) { }

// [------------------------------------------------------------------------- HID -------------------------------------------------------------------------]

typedef struct {
    uint8_t modifier;
    uint8_t key;
} key_action_t;

static key_action_t sequence[MAX_SEQ];
static int seq_len = 0;

static inline void add_key(uint8_t mod, uint8_t key, int delay_count) {
    if (seq_len >= MAX_SEQ - 2) return;
    sequence[seq_len++] = (key_action_t){mod, key};
    sequence[seq_len++] = (key_action_t){0, 0};
    for (int i = 0; i < delay_count && seq_len < MAX_SEQ; i++) {
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

    for (int i = 0; i < 15; i++) {
        if (seq_len < MAX_SEQ) sequence[seq_len++] = (key_action_t){0, 0};
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

    uint32_t const btn = !gpio_get(BUTTON_PIN);

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

void process_json_data(char* json) {
    if (!json || strlen(json) == 0) return;
    
    uint32_t receive_time = to_ms_since_boot(get_absolute_time());
    health_data_t new_data = {0};
    
    char* cpu_ptr = strstr(json, "\"cpu\":");
    char* mem_ptr = strstr(json, "\"memory\":");
    char* disk_ptr = strstr(json, "\"disk\":");
    char* temp_ptr = strstr(json, "\"cpu_temp\":");
    char* net_in_ptr = strstr(json, "\"net_in\":");
    char* net_out_ptr = strstr(json, "\"net_out\":");
    char* proc_ptr = strstr(json, "\"processes\":");
    
    if(cpu_ptr) new_data.cpu = atof(cpu_ptr + 6);
    if(mem_ptr) new_data.memory = atof(mem_ptr + 9);
    if(disk_ptr) new_data.disk = atof(disk_ptr + 7);
    if(temp_ptr) {
        new_data.cpu_temp = atof(temp_ptr + 11);
        if(strstr(temp_ptr + 11, "null")) new_data.cpu_temp = 0;
    }
    if(net_in_ptr) new_data.net_in = atof(net_in_ptr + 9);
    if(net_out_ptr) new_data.net_out = atof(net_out_ptr + 10);
    if(proc_ptr) new_data.processes = atoi(proc_ptr + 12);
    
    if(cpu_ptr && mem_ptr && disk_ptr) {
        current_health = new_data;
        current_health.valid = true;
        is_connected = true;
        
        uint32_t time_since_last = (last_data_time > 0) ? (receive_time - last_data_time) : 0;
        last_data_time = receive_time;
        
        sample_count++;
        if(sample_count > 9999) sample_count = 1;
        
        display_compact_status();
        
        if(time_since_last > 0) {
            printf("[TIMING] Gap since last sample: %lu ms\n", time_since_last);
        }
    }
}

void display_compact_status(void) {
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
    
    if (is_connected) {
        if (now - last_toggle >= 100) {
            gpio_put(LED_PIN, !gpio_get(LED_PIN));
            last_toggle = now;
        }
    } else {
        gpio_put(LED_PIN, 0);
    }
}

// [------------------------------------------------------------------------- Wifi -------------------------------------------------------------------------]

bool init_wifi(void) {
    if (cyw43_arch_init()) {
        printf("WiFi init failed\n");
        return false;
    }
    
    cyw43_arch_enable_sta_mode();
    printf("Connecting to WiFi...\n");
    
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, 
                                            CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("Failed to connect\n");
        return false;
    }
    
    printf("WiFi connected!\n");
    return true;
}

// DNS callback helper
static ip_addr_t resolved_addr;
static volatile bool dns_resolved = false;

static void dns_found_callback(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name;
    (void)arg;
    if (addr) {
        resolved_addr = *addr;
        dns_resolved = true;
        printf("DNS resolved: %s\n", ip4addr_ntoa(addr));
    } else {
        printf("DNS resolution failed\n");
    }
}

void test_wifi_post(void) {
    printf("Testing WiFi POST to webhook.site...\n");
    
    // Initialize DNS resolution
    dns_resolved = false;
    
    // Resolve webhook.site
    err_t err = dns_gethostbyname("webhook.site", &resolved_addr, dns_found_callback, NULL);
    
    if (err == ERR_OK) {
        // Already cached
        printf("DNS already cached\n");
        dns_resolved = true;
    } else if (err == ERR_INPROGRESS) {
        // Wait for DNS resolution (background mode handles it automatically)
        printf("Waiting for DNS resolution...\n");
        uint32_t timeout = to_ms_since_boot(get_absolute_time()) + 10000;
        while (!dns_resolved && to_ms_since_boot(get_absolute_time()) < timeout) {
            sleep_ms(100);
            // No need for cyw43_arch_poll() in threadsafe_background mode
        }
    } else {
        printf("DNS query failed with error: %d\n", err);
        return;
    }
    
    if (!dns_resolved) {
        printf("DNS resolution failed or timed out\n");
        return;
    }
    
    printf("Connecting to resolved IP...\n");
    struct altcp_pcb *pcb = altcp_new(NULL);
    if (!pcb) {
        printf("Failed to create TCP connection\n");
        return;
    }
    
    err_t connect_err = altcp_connect(pcb, &resolved_addr, 80, NULL);
    if (connect_err == ERR_OK) {
        printf("Connection initiated...\n");
        
        // Give connection time to establish
        sleep_ms(1000);
        
        char request[512];
        int len = snprintf(request, sizeof(request),
            "POST /6aae6834-a1a9-4ea8-8518-7c821c2b0fee HTTP/1.1\r\n"
            "Host: webhook.site\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 29\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{\"test\":\"hello from pico\"}");
        
        err_t write_err = altcp_write(pcb, request, len, TCP_WRITE_FLAG_COPY);
        if (write_err == ERR_OK) {
            altcp_output(pcb);
            printf("Test POST sent to webhook.site!\n");
            sleep_ms(2000); // Give time for response
        } else {
            printf("Write failed with error: %d\n", write_err);
        }
        
        altcp_close(pcb);
    } else {
        printf("Connection failed with error: %d\n", connect_err);
        altcp_close(pcb);
    }
}

void post_to_website(health_data_t* data) {
    if (!dns_resolved) {
        printf("Cannot post: DNS not resolved\n");
        return;
    }
    
    struct altcp_pcb *pcb = altcp_new(NULL);
    if (!pcb) {
        printf("Failed to create TCP\n");
        return;
    }
    
    if (altcp_connect(pcb, &resolved_addr, 80, NULL) == ERR_OK) {
        sleep_ms(500); // Wait for connection
        
        char json_body[256];
        int body_len = snprintf(json_body, sizeof(json_body),
            "{\"cpu\":%.1f,\"mem\":%.1f,\"disk\":%.1f,\"temp\":%.1f,\"net_in\":%.1f,\"net_out\":%.1f,\"procs\":%d}",
            data->cpu, data->memory, data->disk, data->cpu_temp,
            data->net_in, data->net_out, data->processes);
        
        char request[768];
        int len = snprintf(request, sizeof(request),
            "POST /6aae6834-a1a9-4ea8-8518-7c821c2b0fee HTTP/1.1\r\n"
            "Host: webhook.site\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            body_len, json_body);
        
        if (altcp_write(pcb, request, len, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            altcp_output(pcb);
            printf("Posted health data to webhook.site\n");
        }
        
        sleep_ms(500);
        altcp_close(pcb);
    } else {
        printf("Connection failed\n");
        altcp_close(pcb);
    }
}


// [------------------------------------------------------------------------- SD Read/Write -------------------------------------------------------------------------]

bool init_sd_card(void) {
    // Initialize SD card
    FRESULT fr = f_mount(&fs, "0:", 1);
    if (fr != FR_OK) {
        printf("SD card mount failed: %d\n", fr);
        return false;
    }
    
    sd_mounted = true;
    printf("SD card mounted successfully\n");
    return true;
}

void log_disconnect_event(void) {
    if (!sd_mounted) return;
    
    FIL fil;
    FRESULT fr;
    UINT bytes_written;
    
    // Open file in append mode, create if doesn't exist
    fr = f_open(&fil, "0:/pico_logs.txt", FA_WRITE | FA_OPEN_APPEND);
    if (fr != FR_OK) {
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
    if (fr != FR_OK) {
        printf("Failed to write to log: %d\n", fr);
    } else {
        printf("Logged disconnect event to SD card\n");
    }
    
    // Close file to ensure data is saved
    f_close(&fil);
}