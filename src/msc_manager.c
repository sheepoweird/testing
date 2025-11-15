#include "msc_manager.h"
#include <stdio.h>
#include <tusb.h>

typedef struct {
    bool is_initialized;
    bool is_mounted;
    void (*mount_callback)(void);
    void (*unmount_callback)(void);
} msc_manager_state_t;

static msc_manager_state_t msc_state = {
    .is_initialized = false,
    .is_mounted = false,
    .mount_callback = NULL,
    .unmount_callback = NULL
};

void tud_mount_cb(void) 
{
    msc_state.is_mounted = true;
    
    if (msc_state.mount_callback) {
        msc_state.mount_callback();
    }
}

void tud_umount_cb(void) 
{
    msc_state.is_mounted = false;
    
    if (msc_state.unmount_callback) {
        msc_state.unmount_callback();
    }
}

void tud_suspend_cb(bool remote_wakeup_en) 
{ 
    (void)remote_wakeup_en;
    // Could add suspend handling here if needed
}

void tud_resume_cb(void) 
{
    // required TinyUSB callback even though we dont use it
}

bool msc_manager_init(const msc_config_t *config)
{
    if (msc_state.is_initialized) {
        return true;
    }
    
    if (!config) {
        return false;
    }
    
    // Store callbacks
    msc_state.mount_callback = config->on_mount;
    msc_state.unmount_callback = config->on_unmount;
    
    // Reset mount state
    msc_state.is_mounted = false;
    msc_state.is_initialized = true;

    return true;
}

bool msc_manager_is_mounted(void)
{
    return msc_state.is_mounted;
}

const char* msc_manager_get_status_string(void)
{
    return msc_state.is_mounted ? "MOUNTED" : "UNMOUNTED";
}
