#include <pico/stdlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <bsp/board.h>
#include <tusb.h>
#include "hardware/gpio.h"
#include "hid_config.h"

#include "pico/unique_id.h"

#define BUTTON_PIN 20
#define LED_PIN 7

// Buffer for receiving data
#define RX_BUFFER_SIZE 512
char rx_buffer[RX_BUFFER_SIZE];
int rx_index = 0;

// Health data structure
typedef struct {
    float cpu;
    float memory;
    float disk;
    float cpu_temp;
    float net_in;
    float net_out;
    int processes;
    uint32_t timestamp;
    bool valid;
} health_data_t;

health_data_t current_health = {0};
uint32_t last_data_time = 0;
uint32_t sample_count = 0;

// Function prototypes
void hid_task(void);
void led_blinking_task(void);
void process_json_data(char* json);
void display_compact_status(void);

int main(void)
{
    board_init();
    tusb_init();
    printf("\033[2J\033[H");
    tud_init(BOARD_TUD_RHPORT);
    stdio_init_all();

    // Buttons
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);
    
    // Pins
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    printf("HID+Storage ready, button on GP%d\n", BUTTON_PIN);

    
    while (true)
    {
        tud_task();
        // led_blinking_task();
        hid_task();

        // Check for incoming character
        int c = getchar_timeout_us(0);
        
        if(c != PICO_ERROR_TIMEOUT) {
            if(c == '\r' || c == '\n') {
                // End of line
                rx_buffer[rx_index] = '\0';
                
                if(rx_index > 0) {
                    // Only process JSON data (starts with '{')
                    if(rx_buffer[0] == '{') {
                        process_json_data(rx_buffer);
                    }
                }
                
                // Reset buffer
                rx_index = 0;
            }
            else if(rx_index < RX_BUFFER_SIZE - 1) {
                // Add to buffer
                rx_buffer[rx_index++] = c;
            }
        }
        
        tight_loop_contents();
    }

    return 0;
}

void tud_mount_cb(void) { printf("USB Device mounted\n"); }
void tud_umount_cb(void) { printf("USB Device unmounted\n"); }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; }
void tud_resume_cb(void) { printf("USB resumed\n"); }

//--------------------------------------------------------------------+
// USB HID - FASTER TYPING
//--------------------------------------------------------------------+

typedef struct
{
    uint8_t modifier;
    uint8_t key;
} key_action_t;

#define MAX_SEQ 512
static key_action_t sequence[MAX_SEQ];
static int seq_len = 0;

// Helper to add key with minimal delays
static inline void add_key(uint8_t mod, uint8_t key, int delay_count) {
    if (seq_len < MAX_SEQ - 2) {
        sequence[seq_len++] = (key_action_t){mod, key};
        sequence[seq_len++] = (key_action_t){0, 0}; // release
        // Minimal delays between keys
        for (int i = 0; i < delay_count; i++) {
            if (seq_len < MAX_SEQ) sequence[seq_len++] = (key_action_t){0, 0};
        }
    }
}

static void build_sequence(void)
{
    seq_len = 0;

    // Win+R
    add_key(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_R, 8);

    // Type: cmd (faster, less delays)
    add_key(0, HID_KEY_C, 1);
    add_key(0, HID_KEY_M, 1);
    add_key(0, HID_KEY_D, 1);

    // Enter
    add_key(0, HID_KEY_ENTER, 25); // Increased delay for CMD to fully open

    // Try drives D, E, F, G
    char drives[] = "DEFG";
    for (int d = 0; d < 4; d++)
    {
        // Drive letter (uppercase)
        add_key(KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_A + (drives[d] - 'A'), 1);
        
        // Colon
        add_key(KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_SEMICOLON, 1);
        
        // Backslash
        add_key(0, HID_KEY_BACKSLASH, 1);
        
        // Type: health_cdc.exe (faster)
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

        // Enter and short wait
        add_key(0, HID_KEY_ENTER, 3);
    }

    // Wait for program
    for (int i = 0; i < 15; i++) {
        if (seq_len < MAX_SEQ) sequence[seq_len++] = (key_action_t){0, 0};
    }

    // UAC - Tab Tab Enter
    add_key(0, HID_KEY_TAB, 4);
    add_key(0, HID_KEY_TAB, 4);
    add_key(0, HID_KEY_ENTER, 0);

    printf("Sequence built: %d keys\n", seq_len);
}

void hid_task(void)
{
    const uint32_t interval_ms = 20; // Faster - 20ms instead of 25ms
    static uint32_t start_ms = 0;

    enum
    {
        ST_IDLE,
        ST_SENDING,
        ST_DONE
    };
    static int state = ST_IDLE;
    static int seq_index = 0;

    if (board_millis() - start_ms < interval_ms)
        return;
    start_ms = board_millis();

    uint32_t const btn = !gpio_get(BUTTON_PIN);

    switch (state)
    {
    case ST_IDLE:
        if (btn && tud_hid_ready())
        {
            build_sequence();
            seq_index = 0;
            state = ST_SENDING;
        }
        break;

    case ST_SENDING:
        if (tud_hid_ready() && seq_index < seq_len)
        {
            key_action_t act = sequence[seq_index++];

            if (act.key || act.modifier)
            {
                // Pack multiple keys when possible (look ahead for non-modifier keys)
                uint8_t kc[6] = {act.key, 0, 0, 0, 0, 0};
                int pack_count = 1;
                
                // Try to pack more keys (same modifier, look ahead)
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
            state = ST_DONE;
            printf("Done\n");
        }
        break;

    case ST_DONE:
        if (!btn)
            state = ST_IDLE;
        break;
    }
}

// USB HID callbacks with unused parameters marked
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

// Simple JSON parser for our specific format
void process_json_data(char* json) {
    health_data_t new_data = {0};
    
    // Very simple JSON parsing
    char* cpu_ptr = strstr(json, "\"cpu\":");
    char* mem_ptr = strstr(json, "\"memory\":");
    char* disk_ptr = strstr(json, "\"disk\":");
    char* temp_ptr = strstr(json, "\"cpu_temp\":");
    char* net_in_ptr = strstr(json, "\"net_in\":");
    char* net_out_ptr = strstr(json, "\"net_out\":");
    char* proc_ptr = strstr(json, "\"processes\":");
    char* time_ptr = strstr(json, "\"timestamp\":");
    
    if(cpu_ptr) new_data.cpu = atof(cpu_ptr + 6);
    if(mem_ptr) new_data.memory = atof(mem_ptr + 9);
    if(disk_ptr) new_data.disk = atof(disk_ptr + 7);
    if(temp_ptr) {
        new_data.cpu_temp = atof(temp_ptr + 11);
        // Handle null case
        if(strstr(temp_ptr + 11, "null")) new_data.cpu_temp = 0;
    }
    if(net_in_ptr) new_data.net_in = atof(net_in_ptr + 9);
    if(net_out_ptr) new_data.net_out = atof(net_out_ptr + 10);
    if(proc_ptr) new_data.processes = atoi(proc_ptr + 12);
    if(time_ptr) new_data.timestamp = atol(time_ptr + 12);
    
    // Validate data
    if(cpu_ptr && mem_ptr && disk_ptr) {
        current_health = new_data;
        current_health.valid = true;
        last_data_time = to_ms_since_boot(get_absolute_time());
        sample_count++;
        
        display_compact_status();
    }
}

void display_compact_status(void) {
    // Display in compact format similar to original Python UI
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
    // Add status in future rn it just blinks infinitely lol
    for(int i = 0; i < 3; i++) {
        gpio_put(LED_PIN, 1);
        sleep_ms(100);
        gpio_put(LED_PIN, 0);
        sleep_ms(100);
    }
}