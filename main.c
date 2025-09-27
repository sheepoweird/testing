#include <bsp/board.h>
#include <pico/stdlib.h>
#include <stdlib.h>
#include <string.h>
#include <tusb.h>
#include <stdio.h>

#include "hardware/gpio.h"
#include "usb_descriptors.h"
#include "hw_config.h"
#include "ff.h"
#include "diskio.h"

// GPIO pin for button
#define BUTTON_PIN 21

// State tracking
static bool execute_health_test = false;
static bool usb_ready = false;
static bool sd_mounted = false;
static bool health_test_found = false;
static bool health_test_executed = false;

// Function prototypes
void hid_task(void);
void led_blinking_task(void);
void sd_card_task(void);
void trigger_health_test_execution(void);
bool is_usb_hid_ready(void);

/**
 * @brief Main entry point - Combined HID + SD Card functionality
 */
int main(void) {
    // Initialize the board
    board_init();
    
    // Initialize stdio
    stdio_init_all();
    sleep_ms(500);

    // Clear screen and show banner
    printf("\033[2J\033[H");
    printf("=== Pico Health Test Launcher ===\n");
    printf("HID Keyboard + SD Card Auto-Execute\n");
    printf("Monitoring for SD card with health_test.exe...\n\n");

    // Initialize button GPIO
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);

    // Initialize SD card driver
    printf("Initializing SD card driver...\n");
    if (!sd_init_driver()) {
        printf("Warning: SD card driver initialization failed\n");
    }

    // Initialize TinyUSB
    printf("Initializing TinyUSB...\n");
    tud_init(BOARD_TUD_RHPORT);

    printf("Starting main loop...\n");

    while (1) {
        // TinyUSB device task - must be called frequently
        tud_task();
        
        // Check if USB became ready
        if (!usb_ready && is_usb_hid_ready()) {
            usb_ready = true;
            printf("USB HID is ready!\n");
        }
        
        led_blinking_task();
        sd_card_task();
        hid_task();
        
        sleep_ms(1);
    }

    return 0;
}

//--------------------------------------------------------------------+
// USB Ready Check
//--------------------------------------------------------------------+
bool is_usb_hid_ready(void) {
    return tud_mounted() && tud_hid_ready();
}

//--------------------------------------------------------------------+
// SD Card Monitoring Task
//--------------------------------------------------------------------+
void sd_card_task(void) {
    static uint32_t last_check_ms = 0;
    const uint32_t check_interval_ms = 2000; // Check every 2 seconds
    
    if (board_millis() - last_check_ms < check_interval_ms)
        return;
    last_check_ms = board_millis();
    
    static FATFS fs;
    FRESULT fr;
    
    // Try to mount SD card
    fr = f_mount(&fs, "", 1);
    
    if (fr == FR_OK && !sd_mounted) {
        sd_mounted = true;
        health_test_found = false;
        health_test_executed = false;
        printf("SD Card mounted successfully!\n");
        
        // Check for health_test.exe
        FIL file;
        fr = f_open(&file, "health_test.exe", FA_READ);
        if (fr == FR_OK) {
            FSIZE_t file_size = f_size(&file);
            f_close(&file);
            printf("Found health_test.exe (size: %lu bytes)\n", (unsigned long)file_size);
            health_test_found = true;
            
            // If USB HID is ready, trigger execution after short delay
            if (usb_ready && !health_test_executed) {
                printf("USB HID ready - will execute health test in 3 seconds...\n");
                sleep_ms(3000);
                trigger_health_test_execution();
                health_test_executed = true;
            }
        } else {
            printf("health_test.exe not found on SD card (error: %d)\n", fr);
        }
    } else if (fr != FR_OK && sd_mounted) {
        sd_mounted = false;
        health_test_found = false;
        health_test_executed = false;
        printf("SD Card removed or unmounted (error: %d)\n", fr);
    }
    
    // If we found the file but USB wasn't ready before, check again
    if (sd_mounted && health_test_found && !health_test_executed && usb_ready) {
        printf("USB HID now ready - executing health test...\n");
        trigger_health_test_execution();
        health_test_executed = true;
    }
}

//--------------------------------------------------------------------+
// USB Device Callbacks
//--------------------------------------------------------------------+
void tud_mount_cb(void) {
    printf("USB Device mounted!\n");
}

void tud_umount_cb(void) {
    printf("USB Device unmounted!\n");
    usb_ready = false;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    printf("USB suspended\n");
}

void tud_resume_cb(void) {
    printf("USB resumed\n");
}

//--------------------------------------------------------------------+
// HID Keyboard Implementation
//--------------------------------------------------------------------+
typedef struct {
    uint8_t modifier;
    uint8_t key;
} key_action_t;

// ASCII to HID keycode mapping
static key_action_t ascii_to_hid(char c) {
    key_action_t act = {0, 0};

    if (c >= 'a' && c <= 'z') {
        act.key = HID_KEY_A + (c - 'a');
    } else if (c >= 'A' && c <= 'Z') {
        act.key = HID_KEY_A + (c - 'A');
        act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
    } else if (c >= '0' && c <= '9') {
        act.key = (c == '0') ? HID_KEY_0 : HID_KEY_1 + (c - '1');
    } else {
        switch (c) {
        case ' ': act.key = HID_KEY_SPACE; break;
        case '.': act.key = HID_KEY_PERIOD; break;
        case ',': act.key = HID_KEY_COMMA; break;
        case '/': act.key = HID_KEY_SLASH; break;
        case '\\': act.key = HID_KEY_BACKSLASH; break;
        case '-': act.key = HID_KEY_MINUS; break;
        case '=': act.key = HID_KEY_EQUAL; break;
        case ';': act.key = HID_KEY_SEMICOLON; break;
        case '\'': act.key = HID_KEY_APOSTROPHE; break;
        case '[': act.key = HID_KEY_BRACKET_LEFT; break;
        case ']': act.key = HID_KEY_BRACKET_RIGHT; break;
        
        // Shifted characters
        case '_': act.key = HID_KEY_MINUS; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '+': act.key = HID_KEY_EQUAL; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case ':': act.key = HID_KEY_SEMICOLON; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '"': act.key = HID_KEY_APOSTROPHE; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '{': act.key = HID_KEY_BRACKET_LEFT; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '}': act.key = HID_KEY_BRACKET_RIGHT; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '|': act.key = HID_KEY_BACKSLASH; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '?': act.key = HID_KEY_SLASH; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '>': act.key = HID_KEY_PERIOD; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '<': act.key = HID_KEY_COMMA; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        
        // Numbers with shift
        case '!': act.key = HID_KEY_1; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '@': act.key = HID_KEY_2; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '#': act.key = HID_KEY_3; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '$': act.key = HID_KEY_4; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '%': act.key = HID_KEY_5; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '^': act.key = HID_KEY_6; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '&': act.key = HID_KEY_7; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '*': act.key = HID_KEY_8; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case '(': act.key = HID_KEY_9; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        case ')': act.key = HID_KEY_0; act.modifier = KEYBOARD_MODIFIER_LEFTSHIFT; break;
        }
    }
    return act;
}

#define MAX_SEQ 512
static key_action_t sequence[MAX_SEQ];
static int seq_len = 0;

// Build keyboard sequence for a command string
static void build_sequence(const char *str) {
    seq_len = 0;

    printf("Building sequence for: %.60s...\n", str);

    // Win+R to open Run dialog
    sequence[seq_len++] = (key_action_t){KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_R};
    sequence[seq_len++] = (key_action_t){0, 0}; // release

    // Add delays for system responsiveness
    for (int i = 0; i < 5; i++) {
        sequence[seq_len++] = (key_action_t){0, 0}; 
    }

    // Type each character of the command
    for (const char *p = str; *p && seq_len < MAX_SEQ - 10; p++) {
        key_action_t k = ascii_to_hid(*p);
        if (k.key || k.modifier) {
            sequence[seq_len++] = k;
            sequence[seq_len++] = (key_action_t){0, 0}; // release
        }
    }

    // Delay before pressing Enter
    for (int i = 0; i < 3; i++) {
        sequence[seq_len++] = (key_action_t){0, 0};
    }

    // Press Enter to execute
    sequence[seq_len++] = (key_action_t){0, HID_KEY_ENTER};
    sequence[seq_len++] = (key_action_t){0, 0}; // release
    
    printf("Sequence built with %d key actions\n", seq_len);
}

// Trigger the health test execution
void trigger_health_test_execution(void) {
    // Your command to find the drive with specific volume serial and run health_test.exe
    const char* cmd = "cmd /c \"for /f \"tokens=1\" %d in ('wmic logicaldisk where \"VolumeSerialNumber='4454704C'\" get DeviceID /format:value ^| find \"DeviceID\"') do for /f \"tokens=2 delims==\" %e in (\"%d\") do %e\\health_test.exe\"";
    
    build_sequence(cmd);
    execute_health_test = true;
    printf("*** Health test execution sequence is ready! ***\n");
}

// Main HID task
void hid_task(void) {
    const uint32_t interval_ms = 100; // 100ms between key actions
    static uint32_t start_ms = 0;

    enum {
        ST_IDLE,
        ST_DELAY_BEFORE_SEND,
        ST_SENDING,
        ST_DONE
    };
    static int state = ST_IDLE;
    static int seq_index = 0;
    static uint32_t state_timer = 0;

    if (board_millis() - start_ms < interval_ms)
        return;
    start_ms = board_millis();

    // Check button state (active low)
    uint32_t const btn = !gpio_get(BUTTON_PIN);

    switch (state) {
    case ST_IDLE:
        if (execute_health_test || btn) {
            if (!usb_ready) {
                printf("USB HID not ready - cannot execute sequence\n");
                execute_health_test = false;
                break;
            }
            
            if (!execute_health_test) { // Manual button press
                build_sequence("calc.exe");
                printf("Manual button pressed - opening calculator\n");
            } else {
                printf("Auto-executing health test command!\n");
            }
            
            seq_index = 0;
            state = ST_DELAY_BEFORE_SEND;
            state_timer = board_millis();
            execute_health_test = false; // Clear the flag
            printf("Starting HID sequence transmission...\n");
        }
        break;

    case ST_DELAY_BEFORE_SEND:
        // Wait 1 second before starting to send keys
        if (board_millis() - state_timer > 1000) {
            state = ST_SENDING;
            printf("Beginning key transmission\n");
        }
        break;

    case ST_SENDING:
        if (!usb_ready) {
            printf("USB connection lost during transmission!\n");
            state = ST_DONE;
            break;
        }
        
        if (seq_index < seq_len) {
            key_action_t act = sequence[seq_index++];
            
            if (act.key || act.modifier) {
                uint8_t kc[6] = {act.key};
                if (tud_hid_keyboard_report(REPORT_ID_KEYBOARD, act.modifier, kc)) {
                    // Success
                } else {
                    printf("Failed to send HID report, retrying...\n");
                    seq_index--; // Retry this key
                }
            } else {
                // Release all keys
                tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);
            }
            
            // Show progress every 20 keys
            if (seq_index % 20 == 0) {
                printf("Key sequence progress: %d/%d\n", seq_index, seq_len);
            }
        } else {
            state = ST_DONE;
            printf("*** KEY SEQUENCE COMPLETED! ***\n");
        }
        break;

    case ST_DONE:
        // Wait for button release before returning to idle
        if (!btn) {
            state = ST_IDLE;
            printf("Ready for next command\n");
        }
        break;

    default:
        state = ST_IDLE;
        break;
    }
}

//--------------------------------------------------------------------+
// HID Callbacks
//--------------------------------------------------------------------+
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)instance;
    (void)len;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
    (void)instance;
    
    if (report_type == HID_REPORT_TYPE_OUTPUT && report_id == REPORT_ID_KEYBOARD && bufsize >= 1) {
        uint8_t const kbd_leds = buffer[0];
        
        // Use Caps Lock LED state to control onboard LED
        if (kbd_leds & KEYBOARD_LED_CAPSLOCK) {
            board_led_write(false);
        } else {
            board_led_write(true);
        }
    }
}

//--------------------------------------------------------------------+
// LED Blinking Task with Status Indication
//--------------------------------------------------------------------+
void led_blinking_task(void) {
    static uint32_t start_ms = 0;
    static bool led_state = false;
    
    // Different blink patterns based on status
    uint32_t interval_ms;
    
    if (!usb_ready) {
        interval_ms = 200; // Fast blink when USB not ready
    } else if (sd_mounted && health_test_found && !health_test_executed) {
        interval_ms = 100; // Very fast blink when about to execute
    } else if (sd_mounted) {
        interval_ms = 500; // Medium blink when SD mounted
    } else {
        interval_ms = 1000; // Slow blink when idle
    }

    if (board_millis() - start_ms < interval_ms)
        return;
    start_ms += interval_ms;

    board_led_write(led_state);
    led_state = !led_state;
}