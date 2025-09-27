#include "pico/stdlib.h"
#include "tusb.h"
#include "hid_test.h"
#include "msd_handler.h"

// USB Configuration
enum {
    ITF_NUM_HID = 0,
    ITF_NUM_MSC,
    ITF_NUM_TOTAL
};

#define EPNUM_HID   0x81
#define EPNUM_MSC   0x02

// HID Report Descriptor
uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

// Configuration Descriptor
uint8_t const desc_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, 0xE0, 0x80, 100),

    // HID interface descriptor
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 5),

    // MSC interface descriptor
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC, 64, 0),
};

// TinyUSB callbacks
uint8_t const * tud_descriptor_device_cb(void) {
    return NULL; // Use default device descriptor
}

uint8_t const * tud_descriptor_configuration_cb(uint8_t index) {
    return (index == 0) ? desc_configuration : NULL;
}

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;
    
    static const char* string_desc_arr[] = {
        (const char[]) { 0x09, 0x04 },  // English
        "Raspberry Pi",                 // Manufacturer
        "Pico HID+MSD",                 // Product
        "123456",                       // Serial
    };
    
    static uint16_t desc_str[32];
    uint8_t len;

    if (index == 0) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        desc_str[0] = 2 | (0 << 8);
        return desc_str;
    }

    const char* str = (index < 4) ? string_desc_arr[index] : NULL;
    if (!str) return NULL;

    len = strlen(str);
    if (len > 31) len = 31;

    desc_str[0] = (len * 2 + 2) | (3 << 8);
    for (uint8_t i = 0; i < len; i++) {
        desc_str[i + 1] = str[i];
    }

    return desc_str;
}

// HID callbacks
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
}

int main() {
    stdio_init_all();
    
    printf("=== Pico HID + Mass Storage ===\n");
    
    // Initialize services
    hid_test_init();
    msd_init();
    
    // Initialize TinyUSB
    tusb_init();
    
    while (true) {
        tud_task(); // TinyUSB device task
        
        // Run HID functionality
        hid_test_task();
        
        tight_loop_contents();
    }
    
    return 0;
}

// USB event callbacks
void tud_mount_cb(void) {
    printf("USB: Device mounted\n");
}

void tud_umount_cb(void) {
    printf("USB: Device unmounted\n");
}