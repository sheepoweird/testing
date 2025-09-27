#include "hid_test.h"
#include "tusb.h"

void hid_test_init() {
    // Initialize any HID related hardware
    printf("HID Test Initialized\n");
}

void hid_test_task() {
    // HID periodic tasks
    static uint32_t last_time = 0;
    
    if (time_us_32() - last_time > 1000000) {
        if (tud_hid_ready()) {
            // Send a simple report periodically
            uint8_t report[] = {0x01, 0x00};
            tud_hid_report(0, report, sizeof(report));
        }
        last_time = time_us_32();
    }
}

void send_hid_report(uint8_t keycode) {
    if (tud_hid_ready()) {
        uint8_t report[] = {keycode, 0x00};
        tud_hid_report(0, report, sizeof(report));
    }
}