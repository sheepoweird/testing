#include "hid_handler.h"

static bool hid_connected = false;

void hid_init(void) {
    // Initialize any HID-specific hardware
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
}

void hid_task(void) {
    // Blink LED based on HID connection status
    static uint32_t last_blink = 0;
    
    bool connected = tud_hid_ready();
    if (connected != hid_connected) {
        hid_connected = connected;
        gpio_put(LED_PIN, connected);
    }
    
    if (connected && (time_us_32() - last_blink > 1000000)) {
        gpio_xor_mask(1u << LED_PIN);
        last_blink = time_us_32();
    }
}

void hid_send_report(const uint8_t* data, uint8_t len) {
    if (tud_hid_ready()) {
        tud_hid_report(HID_REPORT_ID, data, len);
    }
}

bool hid_ready(void) {
    return tud_hid_ready();
}

// TinyUSB HID callbacks
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    // Handle incoming HID reports
}