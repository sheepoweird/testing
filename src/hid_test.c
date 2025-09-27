#include "hid_test.h"
#include "tusb.h"
#include <stdio.h>

void hid_test_init(void) {
    printf("HID: Keyboard test initialized\n");
}

void hid_test_task(void) {
    // Example: Send a key press every 5 seconds
    static uint32_t last_time = 0;
    static bool key_pressed = false;
    
    uint32_t current_time = time_us_32() / 1000000; // Convert to seconds
    
    if (current_time - last_time >= 5) {
        if (tud_hid_ready()) {
            if (!key_pressed) {
                // Press 'A' key
                uint8_t keycode[6] = {HID_KEY_A};
                tud_hid_keyboard_report(1, 0, keycode);
                printf("HID: Key A pressed\n");
                key_pressed = true;
            } else {
                // Release all keys
                tud_hid_keyboard_report(1, 0, NULL);
                printf("HID: Keys released\n");
                key_pressed = false;
            }
        }
        last_time = current_time;
    }
}

void send_hid_report(uint8_t keycode) {
    if (tud_hid_ready()) {
        uint8_t keys[6] = {keycode};
        tud_hid_keyboard_report(1, 0, keys);
    }
}

// HID descriptor (simple keyboard)
uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

// HID callbacks
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    // Handle set report if needed
}