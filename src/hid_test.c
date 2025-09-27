#include "hid_test.h"
#include "tusb.h"
#include <stdio.h>

void hid_test_init(void) {
    printf("HID: Keyboard test initialized\n");
}

void hid_test_task(void) {
    static uint32_t last_time = 0;
    static bool key_pressed = false;
    static uint8_t key_counter = 0;
    
    uint32_t current_time = time_us_32() / 1000000; // Convert to seconds
    
    if (current_time - last_time >= 3) { // Every 3 seconds
        if (tud_hid_ready()) {
            if (!key_pressed) {
                // Press a key (cycle through A, B, C)
                uint8_t keys[6] = {0};
                keys[0] = HID_KEY_A + (key_counter % 3);
                tud_hid_keyboard_report(1, 0, keys);
                printf("HID: Key %c pressed\n", 'A' + (key_counter % 3));
                key_pressed = true;
                key_counter++;
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