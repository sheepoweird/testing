/**
 * @file    wifi_manager.h
 * @brief   WiFi connection management module for Raspberry Pi Pico W
 * @author  Team IS-02
 * @date    2025-11-08
 * 
 * This module provides WiFi initialization, connection monitoring,
 * and automatic reconnection capabilities using the CYW43 chip.
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * INCLUDES
 ******************************************************************************/
#include <stdbool.h>
#include <stdint.h>

/*******************************************************************************
 * CONSTANTS AND MACROS
 ******************************************************************************/
#define WIFI_RECONNECT_DELAY_MS     5000U   /**< Delay before reconnection attempt */
#define WIFI_CHECK_INTERVAL_MS      5000U   /**< Interval for WiFi status check */
#define WIFI_CONNECT_TIMEOUT_MS     30000U  /**< Timeout for WiFi connection */

/*******************************************************************************
 * TYPE DEFINITIONS
 ******************************************************************************/

/**
 * @brief WiFi connection status enumeration
 */
typedef enum
{
    WIFI_STATUS_DISCONNECTED = 0,   /**< WiFi is disconnected */
    WIFI_STATUS_CONNECTING,         /**< WiFi connection in progress */
    WIFI_STATUS_CONNECTED,          /**< WiFi is connected */
    WIFI_STATUS_FAILED,             /**< WiFi connection failed */
    WIFI_STATUS_RECONNECTING        /**< WiFi reconnection in progress */
} wifi_status_t;

/**
 * @brief WiFi manager state structure
 */
typedef struct
{
    bool is_initialized;            /**< CYW43 initialization flag */
    bool is_connected;              /**< Connection status flag */
    bool is_fully_connected;        /**< Full connection flag (DNS resolved) */
    bool reconnect_pending;         /**< Reconnection pending flag */
    wifi_status_t status;           /**< Current WiFi status */
    uint32_t last_check_time;       /**< Last status check timestamp */
    uint32_t disconnect_time;       /**< Disconnect timestamp */
    uint32_t connect_attempts;      /**< Number of connection attempts */
} wifi_manager_state_t;

/*******************************************************************************
 * PUBLIC FUNCTION PROTOTYPES
 ******************************************************************************/

/**
 * @brief Initialize WiFi hardware and CYW43 chip
 * 
 * This function initializes the CYW43 WiFi chip in threadsafe background mode.
 * Must be called before any other WiFi operations.
 * 
 * @return true if initialization successful, false otherwise
 */
bool wifi_manager_init(void);

/**
 * @brief Connect to WiFi network
 * 
 * Attempts to connect to the configured WiFi network. This function will
 * block until connection succeeds or timeout occurs.
 * 
 * @param[in] ssid          WiFi network SSID
 * @param[in] password      WiFi network password
 * @param[in] timeout_ms    Connection timeout in milliseconds
 * 
 * @return true if connection successful, false otherwise
 */
bool wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms);

/**
 * @brief Check and monitor WiFi connection status
 * 
 * This function should be called periodically to monitor the WiFi connection
 * status and trigger automatic reconnection if needed.
 * 
 * @return Current WiFi status
 */
wifi_status_t wifi_manager_check_status(void);

/**
 * @brief Handle WiFi reconnection logic
 * 
 * Attempts to reconnect to WiFi if disconnected. This function handles
 * the reconnection delay and retry logic.
 * 
 * @return true if reconnection successful, false otherwise
 */
bool wifi_manager_handle_reconnect(void);

/**
 * @brief Get current WiFi connection status
 * 
 * @return true if WiFi is connected, false otherwise
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Get full connection status (including DNS)
 * 
 * @return true if fully connected with DNS resolved, false otherwise
 */
bool wifi_manager_is_fully_connected(void);

/**
 * @brief Set full connection status
 * 
 * Used to indicate that full connection is established (DNS resolved, etc.)
 * 
 * @param[in] connected     Full connection status
 */
void wifi_manager_set_fully_connected(bool connected);

/**
 * @brief Get WiFi manager state
 * 
 * Returns pointer to the internal state structure for monitoring purposes.
 * Should not be modified directly.
 * 
 * @return Pointer to WiFi manager state (read-only)
 */
const wifi_manager_state_t *wifi_manager_get_state(void);

/**
 * @brief Disconnect from WiFi network
 * 
 * Gracefully disconnects from the current WiFi network.
 */
void wifi_manager_disconnect(void);

/**
 * @brief Deinitialize WiFi hardware
 * 
 * Cleans up and deinitializes the CYW43 chip. Should be called before
 * system shutdown or reset.
 */
void wifi_manager_deinit(void);

/**
 * @brief Poll WiFi hardware
 * 
 * Must be called regularly to allow WiFi driver to process events.
 * Typically called in the main loop or WiFi task.
 */
void wifi_manager_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */