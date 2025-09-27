#include "pico/stdlib.h"
#include "tusb.h"
#include "hid_test.h"
#include "msd_handler.h"

int main() {
    stdio_init_all();
    
    // Wait for USB connection
    while (!tud_cdc_connected()) {
        sleep_ms(100);
    }
    
    printf("=== Combined HID + Mass Storage Device ===\n");
    
    // Initialize both services
    hid_test_init();
    msd_init();
    
    // Initialize TinyUSB
    tusb_init();
    
    printf("Device ready - will appear as both HID keyboard and mass storage\n");
    
    while (true) {
        tud_task();  // TinyUSB device task
        
        // Run HID functionality
        hid_test_task();
        
        // Run MSD functionality  
        // msd_task(); // Uncomment if you add periodic tasks
        
        tight_loop_contents();
    }
    
    return 0;
}

// USB device callbacks
void tud_mount_cb(void) {
    printf("USB: Device mounted\n");
}

void tud_umount_cb(void) {
    printf("USB: Device unmounted\n");
}

void tud_suspend_cb(bool remote_wakeup_en) {
    printf("USB: Suspended\n");
}

void tud_resume_cb(void) {
    printf("USB: Resumed\n");
}