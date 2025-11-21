#include "hid_manager.h"
#include "hid_config.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include <stdio.h>
#include <string.h>

static hid_manager_state_t g_hid_state = {0};
static hid_config_t g_hid_config = {0};
static hid_key_action_t g_sequence[HID_MAX_SEQUENCE_LENGTH] = {0};

static void hid_check_auto_trigger(bool wifi_connected, bool usb_mounted);
static void hid_execute_sequence(void);

bool hid_manager_init(const hid_config_t *config)
{
    if (config == NULL)
    {
        printf("HID Manager: Invalid configuration\n");
        return false;
    }
    
    printf("HID Manager: Initializing...\n");
    
    memcpy(&g_hid_config, config, sizeof(hid_config_t));
    
    memset(&g_hid_state, 0, sizeof(hid_manager_state_t));
    g_hid_state.status = HID_STATUS_IDLE;
    g_hid_state.is_running = false;
    g_hid_state.sequence_index = 0;
    g_hid_state.sequence_length = 0;
    g_hid_state.auto_trigger_executed = false;
    g_hid_state.auto_trigger_start_time = 0;
    
    if (g_hid_config.enable_auto_trigger)
    {
        g_hid_state.status = HID_STATUS_WAITING_TRIGGER;
        printf("HID Manager: Auto-trigger enabled (%lu ms delay)\n", 
               g_hid_config.auto_trigger_delay_ms);
    }
    
    printf("HID Manager: Initialized successfully\n");
    return true;
}

void hid_manager_build_sequence(void)
{
    printf("HID Manager: Building keyboard sequence...\n");
    
    hid_manager_clear_sequence();
    
    // Open Run dialog (Win+R) and type "cmd"
    hid_manager_add_key(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_R, 8);
    hid_manager_add_key(0, HID_KEY_C, 1);
    hid_manager_add_key(0, HID_KEY_M, 1);
    hid_manager_add_key(0, HID_KEY_D, 1);
    hid_manager_add_key(0, HID_KEY_ENTER, 60);
    
    // Try find health-cdc.exe on multiple drive letters
    char drives[] = "DEFG";
    for (int d = 0; d < 4; d++)
    {
        hid_manager_add_key(KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_A + (drives[d] - 'A'), 1);
        hid_manager_add_key(KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_SEMICOLON, 1);
        
        // Type "health-cdc.exe"
        hid_manager_add_key(0, HID_KEY_H, 1);
        hid_manager_add_key(0, HID_KEY_E, 0);
        hid_manager_add_key(0, HID_KEY_A, 0);
        hid_manager_add_key(0, HID_KEY_L, 0);
        hid_manager_add_key(0, HID_KEY_T, 0);
        hid_manager_add_key(0, HID_KEY_H, 0);
        hid_manager_add_key(KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_MINUS, 0);
        hid_manager_add_key(0, HID_KEY_C, 0);
        hid_manager_add_key(0, HID_KEY_D, 0);
        hid_manager_add_key(0, HID_KEY_C, 0);
        hid_manager_add_key(0, HID_KEY_PERIOD, 0);
        hid_manager_add_key(0, HID_KEY_E, 0);
        hid_manager_add_key(0, HID_KEY_X, 0);
        hid_manager_add_key(0, HID_KEY_E, 1);
        hid_manager_add_key(0, HID_KEY_ENTER, 3);
    }
    
    // Add delay between attempts
    for (int i = 0; i < 15; i++)
    {
        hid_manager_add_key(0, 0, 0);
    }
    
    // Type "exit" to close CMD
    hid_manager_add_key(0, HID_KEY_E, 4);
    hid_manager_add_key(0, HID_KEY_X, 4);
    hid_manager_add_key(0, HID_KEY_I, 4);
    hid_manager_add_key(0, HID_KEY_T, 4);
    hid_manager_add_key(0, HID_KEY_ENTER, 0);
    
    printf("HID Manager: Sequence built (%u actions)\n", g_hid_state.sequence_length);
}

bool hid_manager_start_sequence(void)
{
    if (g_hid_state.is_running)
    {
        return false;
    }
    
    if (g_hid_state.sequence_length == 0)
    {
        printf("HID Manager: No sequence built\n");
        return false;
    }
    
    printf("HID Manager: Starting sequence...\n");
    
    g_hid_state.is_running = true;
    g_hid_state.status = HID_STATUS_RUNNING;
    g_hid_state.sequence_index = 0;
    g_hid_state.last_update_time = to_ms_since_boot(get_absolute_time());
    
    return true;
}

void hid_manager_stop_sequence(void)
{
    if (g_hid_state.is_running)
    {
        printf("HID Manager: Stopping sequence\n");
        g_hid_state.is_running = false;
        g_hid_state.status = HID_STATUS_IDLE;
    }
}

void hid_manager_task(bool wifi_connected, bool usb_mounted)
{
    if (g_hid_config.enable_auto_trigger && !g_hid_state.auto_trigger_executed)
    {
        hid_check_auto_trigger(wifi_connected, usb_mounted);
    }
    
    if (g_hid_state.is_running)
    {
        hid_execute_sequence();
    }
}

hid_status_t hid_manager_get_status(void)
{
    return g_hid_state.status;
}

const hid_manager_state_t *hid_manager_get_state(void)
{
    return &g_hid_state;
}

bool hid_manager_is_running(void)
{
    return g_hid_state.is_running;
}

void hid_manager_reset(void)
{
    g_hid_state.is_running = false;
    g_hid_state.status = HID_STATUS_IDLE;
    g_hid_state.sequence_index = 0;
    g_hid_state.auto_trigger_executed = false;
    g_hid_state.auto_trigger_start_time = 0;
}

bool hid_manager_add_key(uint8_t modifier, uint8_t key, uint16_t delay_count)
{
    if (g_hid_state.sequence_length >= HID_MAX_SEQUENCE_LENGTH - 2)
    {
        return false;
    }
    
    g_sequence[g_hid_state.sequence_length].modifier = modifier;
    g_sequence[g_hid_state.sequence_length].key = key;
    g_hid_state.sequence_length++;
    
    g_sequence[g_hid_state.sequence_length].modifier = 0;
    g_sequence[g_hid_state.sequence_length].key = 0;
    g_hid_state.sequence_length++;
    
    for (uint16_t i = 0; i < delay_count && 
         g_hid_state.sequence_length < HID_MAX_SEQUENCE_LENGTH; i++)
    {
        g_sequence[g_hid_state.sequence_length].modifier = 0;
        g_sequence[g_hid_state.sequence_length].key = 0;
        g_hid_state.sequence_length++;
    }
    
    return true;
}

void hid_manager_clear_sequence(void)
{
    memset(g_sequence, 0, sizeof(g_sequence));
    g_hid_state.sequence_length = 0;
    g_hid_state.sequence_index = 0;
}

uint16_t hid_manager_get_sequence_length(void)
{
    return g_hid_state.sequence_length;
}

bool hid_manager_auto_trigger_done(void)
{
    return g_hid_state.auto_trigger_executed;
}

static void hid_check_auto_trigger(bool wifi_connected, bool usb_mounted)
{
    if (!wifi_connected || !usb_mounted)
    {
        g_hid_state.auto_trigger_start_time = 0;
        g_hid_state.status = HID_STATUS_WAITING_TRIGGER;
        return;
    }
    
    if (g_hid_state.auto_trigger_start_time == 0 && 
        g_hid_state.status == HID_STATUS_WAITING_TRIGGER)
    {
        g_hid_state.auto_trigger_start_time = to_ms_since_boot(get_absolute_time());
        printf("HID Manager: %lu second countdown started\n",
               g_hid_config.auto_trigger_delay_ms / 1000);
        return;
    }
    
    if (g_hid_state.auto_trigger_start_time == 0) {
        return;
    }
    
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t elapsed = now - g_hid_state.auto_trigger_start_time;
    
    if (elapsed >= g_hid_config.auto_trigger_delay_ms)
    {
        printf("HID Manager: Auto-triggering sequence\n");
        hid_manager_start_sequence();
        
        g_hid_state.auto_trigger_executed = true;
        g_hid_state.status = HID_STATUS_RUNNING;
    }
}

static void hid_execute_sequence(void)
{
    if (!tud_hid_ready())
    {
        return;
    }
    
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - g_hid_state.last_update_time < HID_UPDATE_INTERVAL_MS)
    {
        return;
    }
    
    g_hid_state.last_update_time = now;
    
    if (g_hid_state.sequence_index >= g_hid_state.sequence_length)
    {
        g_hid_state.is_running = false;
        g_hid_state.status = HID_STATUS_COMPLETE;
        printf("HID Manager: Sequence completed\n");
        return;
    }
    
    hid_key_action_t action = g_sequence[g_hid_state.sequence_index];
    g_hid_state.sequence_index++;
    
    uint8_t keycode[6] = {0};
    if (action.key != 0)
    {
        keycode[0] = action.key;
    }
    
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, action.modifier, keycode);
}

uint16_t tud_hid_get_report_cb(uint8_t itf, 
                               uint8_t report_id, 
                               uint8_t report_type, 
                               uint8_t* buffer, 
                               uint16_t reqlen)
{
    (void)itf;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    
    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, 
                           uint8_t report_id, 
                           uint8_t report_type, 
                           const uint8_t* buffer, 
                           uint16_t bufsize)
{
    (void)itf;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}