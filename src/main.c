#include "pico/stdlib.h"
#include "tusb.h"
#include "hid_test.h"
#include "msd_handler.h"

int main() {
    stdio_init_all();
    
    // Initialize both services
    hid_test_init();
    msd_init();
    
    // Initialize TinyUSB
    tusb_init();
    
    printf("Starting Combined HID + MSD Device\n");
    
    while (true) {
        tud_task();
        
        // Run HID tasks
        hid_test_task();
        
        // Run MSD tasks  
        msd_task();
        
        tight_loop_contents();
    }
    
    return 0;
}

// USB callbacks
void tud_mount_cb(void) {
    printf("USB mounted\n");
}

void tud_umount_cb(void) {
    printf("USB unmounted\n");
}