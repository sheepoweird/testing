#include <pico/stdlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <bsp/board.h>
#include <tusb.h>
#include "hardware/gpio.h"
#include "hid_config.h"

#define BUTTON_PIN 20  

// Function prototypes
void hid_task(void);
void led_blinking_task(void);

int main(void) {
    // Initialize the board
    board_init();
    tusb_init();

    // Clear Screen
    printf("\033[2J\033[H");

    // Initialize TinyUSB
    tud_init(BOARD_TUD_RHPORT);

    // Initialize the standard I/O streams
    stdio_init_all();

    // Initialize button - ADD THIS MISSING PART
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);

    printf("HID+Storage ready, button on GP%d\n", BUTTON_PIN);

    // Run the TinyUSB task loop
    while (true) {
        tud_task();
        led_blinking_task();
        hid_task();
    }
    return 0;
}

// Keep all your existing USB callbacks exactly the same
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void)
{
    printf("USB Device mounted\n");
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
    printf("USB Device unmounted\n");
}

// Invoked when usb bus is suspended
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    printf("USB suspended\n");
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
    printf("USB resumed\n");
}

//--------------------------------------------------------------------+
// USB HID - Keep all your existing HID code
//--------------------------------------------------------------------+

// HID keyboard task
// Map ASCII to HID keycode + modifier
typedef struct
{
    uint8_t modifier;
    uint8_t key;
} key_action_t;

static key_action_t ascii_to_hid(char c)
{
    key_action_t act = {0, 0};

    if (c >= 'a' && c <= 'z')
    {
        act.key = HID_KEY_A + (c - 'a');
    }
    else if (c >= 'A' && c <= 'Z')
    {
        act.key = HID_KEY_A + (c - 'A');
        act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
    }
    else if (c >= '0' && c <= '9')
    {
        act.key = (c == '0') ? HID_KEY_0 : HID_KEY_1 + (c - '1');
    }
    else if (c == '.')
    {
        act.key = HID_KEY_PERIOD;
    }
    else if (c == ',')
    {
        act.key = HID_KEY_COMMA;
    }
    else if (c == '/')
    {
        act.key = HID_KEY_SLASH;
    }
    else if (c == '-')
    {
        act.key = HID_KEY_MINUS;
    }
    else if (c == ':')
    {
        act.key = HID_KEY_SEMICOLON;
        act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
    }
    else if (c == '=')
    {
        act.key = HID_KEY_EQUAL;
    }
    else if (c == '?')
    {
        act.key = HID_KEY_SLASH;
        act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
    }
    else if (c == '_')
    {
        act.key = HID_KEY_MINUS;
        act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
    }
    else
    {
        // Extend: space, punctuation, etc.
        if (c == ' ')
            act.key = HID_KEY_SPACE;
    }
    return act;
}

// Max sequence size
#define MAX_SEQ 128
static key_action_t sequence[MAX_SEQ];
static int seq_len = 0;

// Faster sequence building - send multiple keys at once
static void build_sequence(const char *str)
{
    seq_len = 0;

    // Win+R
    sequence[seq_len++] = (key_action_t){KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_R};
    sequence[seq_len++] = (key_action_t){0, 0}; // release
    
    // Small delay
    sequence[seq_len++] = (key_action_t){0, 0}; 
    sequence[seq_len++] = (key_action_t){0, 0}; 

    // Send characters in chunks
    for (const char *p = str; *p && seq_len < MAX_SEQ - 4; p++)
    {
        key_action_t k = ascii_to_hid(*p);
        if (k.key)
        {
            sequence[seq_len++] = k;
            // Only add release every few characters or for special keys
            if (k.modifier || (seq_len % 3 == 0)) {
                sequence[seq_len++] = (key_action_t){0, 0}; // release
            }
        }
    }

    // Final release and Enter
    sequence[seq_len++] = (key_action_t){0, 0}; // final release
    sequence[seq_len++] = (key_action_t){0, 0}; // delay
    sequence[seq_len++] = (key_action_t){0, HID_KEY_ENTER};
    sequence[seq_len++] = (key_action_t){0, 0}; // release
}

// Much faster HID task
void hid_task(void)
{
    const uint32_t interval_ms = 20; // Much faster - 20ms instead of 50ms
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

    uint32_t const btn = !gpio_get(BUTTON_PIN); // active low

    switch (state)
    {
    case ST_IDLE:
        if (btn && tud_hid_ready())
        {
            build_sequence("cmd /c start https://www.youtube.com/watch?v=dQw4w9WgXcQ"); // Faster test
            seq_index = 0;
            state = ST_SENDING;
            printf("Starting fast sequence\n");
        }
        break;

    case ST_SENDING:
        if (tud_hid_ready() && seq_index < seq_len)
        {
            key_action_t act = sequence[seq_index++];
            
            if (act.key || act.modifier)
            {
                // Send up to 6 keys at once
                uint8_t kc[6] = {act.key};
                
                // Try to pack more keys in one report (look ahead)
                int pack_count = 1;
                while (pack_count < 6 && seq_index < seq_len && 
                       sequence[seq_index].key && !sequence[seq_index].modifier &&
                       sequence[seq_index].modifier == act.modifier)
                {
                    kc[pack_count] = sequence[seq_index].key;
                    pack_count++;
                    seq_index++;
                }
                
                tud_hid_keyboard_report(REPORT_ID_KEYBOARD, act.modifier, kc);
                printf("Sent %d keys\n", pack_count);
            }
            else
            {
                tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);
            }
        }
        else if (seq_index >= seq_len)
        {
            state = ST_DONE;
        }
        break;

    case ST_DONE:
        if (!btn)
            state = ST_IDLE;
        break;

    default:
        state = ST_IDLE;
        break;
    }
}

// Keep all your existing HID callbacks exactly the same
// Invoked when sent REPORT successfully to host
// Application can use this to send the next report
// Note: For composite reports, report[0] is report ID
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)instance;
    (void)report;
    (void)len;
    // Nothing to do here for this simple example
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;

    return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    // Handle keyboard LED indicators
    if (report_type == HID_REPORT_TYPE_OUTPUT)
    {
        // Set keyboard LED e.g Caps Lock, Num Lock, Scroll Lock
        if (report_id == REPORT_ID_KEYBOARD)
        {
            // bufsize should be (at least) 1
            if (bufsize < 1)
                return;

            uint8_t const kbd_leds = buffer[0];

            if (kbd_leds & KEYBOARD_LED_CAPSLOCK)
            {
                // Caps Lock On: disable LED
                board_led_write(false);
                printf("Caps Lock ON\n");
            }
            else
            {
                // Caps Lock Off: enable LED
                board_led_write(true);
                printf("Caps Lock OFF\n");
            }
        }
    }
}

//--------------------------------------------------------------------+
// BLINKING TASK
//--------------------------------------------------------------------+
void led_blinking_task(void)
{
    const uint32_t interval_ms = 1000;
    static uint32_t start_ms = 0;
    static bool led_state = false;

    // Blink every interval ms
    if (board_millis() - start_ms < interval_ms)
        return; // not enough time
    start_ms += interval_ms;

    board_led_write(led_state);
    led_state = 1 - led_state; // toggle
}