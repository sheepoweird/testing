#include "pico/stdlib.h"
#include "tusb.h"
#include "hid_handler.h"
#include "msd_handler.h"

int main() {
    stdio_init_all();
    
    // Initialize both HID and MSD
    hid_init();
    msd_init();
    
    // Initialize TinyUSB
    tusb_init();
    
    printf("Combined USB HID + Mass Storage Device Ready\n");
    
    while (true) {
        tud_task();  // TinyUSB device task
        
        // Handle HID functionality
        hid_task();
        
        // Handle MSD functionality  
        msd_task();
        
        // Example: Send HID report every second
        static uint32_t last_report = 0;
        if (time_us_32() - last_report > 1000000) {
            uint8_t report_data[] = {0x01, 0x02, 0x03, 0x04};
            hid_send_report(report_data, sizeof(report_data));
            last_report = time_us_32();
        }
        
        tight_loop_contents();
    }
    
    return 0;
}

// TinyUSB callbacks
void tud_mount_cb(void) {
    printf("USB Device Mounted\n");
}

void tud_umount_cb(void) {
    printf("USB Device Unmounted\n");
}

void tud_suspend_cb(bool remote_wakeup_en) {
    printf("USB Suspended\n");
}

void tud_resume_cb(void) {
    printf("USB Resumed\n");
}

// Combined HID + MSD descriptor
enum {
    ITF_NUM_HID = 0,
    ITF_NUM_MSC,
    ITF_NUM_TOTAL
};

#define EPNUM_HID   0x01
#define EPNUM_MSC   0x02

uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID)
};

const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0xCafe,
    .idProduct          = 0x4000,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const desc_configuration[] = {
    // Configuration descriptor
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, 128, 0x80, 100),
    
    // HID interface
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 5),
    
    // MSC interface
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC, 64, 0x00),
};

const char* string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },  // 0: is supported language is English (0x0409)
    "Raspberry Pi",                 // 1: Manufacturer
    "Pico HID+MSD Composite",       // 2: Product
    "123456",                       // 3: Serials
};

// TinyUSB callbacks
const uint8_t* tud_descriptor_device_cb(void) {
    return (uint8_t const*) &desc_device;
}

const uint8_t* tud_descriptor_configuration_cb(uint8_t index) {
    return desc_configuration;
}

const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    static uint16_t desc_str[32];
    uint8_t len;
    
    if (index == 0) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        desc_str[0] = 2 | (0 << 8);
        return desc_str;
    }
    
    const char* str = string_desc_arr[index];
    if (!str) return NULL;
    
    len = strlen(str);
    if (len > 31) len = 31;
    
    desc_str[0] = (len * 2 + 2) | (3 << 8);
    for (uint8_t i = 0; i < len; i++) {
        desc_str[i + 1] = str[i];
    }
    
    return desc_str;
}