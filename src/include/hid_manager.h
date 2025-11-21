#ifndef HID_MANAGER_H
#define HID_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#define HID_MAX_SEQUENCE_LENGTH     512U    /**< Maximum keyboard sequence length */
#define HID_UPDATE_INTERVAL_MS      20U     /**< HID update interval */
#define HID_AUTO_TRIGGER_DELAY_MS   20000U  /**< Auto-trigger delay (20 seconds) */

typedef struct
{
    uint8_t modifier;   /**< Keyboard modifier (shift, ctrl, alt, etc.) */
    uint8_t key;        /**< Key code */
} hid_key_action_t;

typedef enum
{
    HID_STATUS_IDLE = 0,        /**< HID is idle */
    HID_STATUS_RUNNING,         /**< HID sequence is running */
    HID_STATUS_COMPLETE,        /**< HID sequence completed */
    HID_STATUS_WAITING_TRIGGER  /**< Waiting for auto-trigger */
} hid_status_t;

typedef struct
{
    hid_status_t status;        /**< Current HID status */
    bool is_running;            /**< Sequence running flag */
    uint16_t sequence_index;    /**< Current sequence index */
    uint16_t sequence_length;   /**< Total sequence length */
    uint32_t last_update_time;  /**< Last update timestamp */
    bool auto_trigger_executed; /**< Auto-trigger executed flag */
    uint32_t auto_trigger_start_time; /**< Auto-trigger countdown start */
} hid_manager_state_t;

typedef struct
{
    bool enable_auto_trigger;   /**< Enable auto-trigger on WiFi+USB ready */
    uint32_t auto_trigger_delay_ms; /**< Auto-trigger delay in milliseconds */
} hid_config_t;

bool hid_manager_init(const hid_config_t *config);

void hid_manager_build_sequence(void);

bool hid_manager_start_sequence(void);

void hid_manager_stop_sequence(void);

void hid_manager_task(bool wifi_connected, bool usb_mounted);

hid_status_t hid_manager_get_status(void);

const hid_manager_state_t *hid_manager_get_state(void);

bool hid_manager_is_running(void);

void hid_manager_reset(void);

bool hid_manager_add_key(uint8_t modifier, uint8_t key, uint16_t delay_count);

void hid_manager_clear_sequence(void);

uint16_t hid_manager_get_sequence_length(void);

bool hid_manager_auto_trigger_done(void);

uint16_t tud_hid_get_report_cb(uint8_t itf, 
                               uint8_t report_id, 
                               uint8_t report_type, 
                               uint8_t* buffer, 
                               uint16_t reqlen);

void tud_hid_set_report_cb(uint8_t itf, 
                           uint8_t report_id, 
                           uint8_t report_type, 
                           const uint8_t* buffer, 
                           uint16_t bufsize);

#endif /* HID_MANAGER_H */