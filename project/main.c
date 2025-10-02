#include <pico/stdlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <bsp/board.h>
#include <tusb.h>
#include "hardware/gpio.h"
#include "hid_config.h"

#define BUTTON_PIN 20
#define LED_PIN 9
#define RX_BUFFER_SIZE 512
#define DATA_TIMEOUT_MS 10000
#define MAX_SEQ 512

char rx_buffer[RX_BUFFER_SIZE];
int rx_index = 0;

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

health_data_t current_health = {0};
uint32_t last_data_time = 0;
uint32_t sample_count = 0;
bool is_connected = false;

void hid_task(void);
void led_blinking_task(void);
void process_json_data(char* json);
void display_compact_status(void);

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

    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);
    
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    printf("System ready - GP%d=Button, GP%d=LED\n", BUTTON_PIN, LED_PIN);

    while (true)
    {
        tud_task();
        led_blinking_task();
        hid_task();

        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (last_data_time > 0 && (now - last_data_time > DATA_TIMEOUT_MS)) {
            if (current_health.valid || is_connected) {
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

void tud_mount_cb(void) { }
void tud_umount_cb(void) { }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; }
void tud_resume_cb(void) { }

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