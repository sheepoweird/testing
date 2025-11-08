#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#define WIFI_RECONNECT_DELAY_MS     5000   /**< Delay before reconnection attempt */
#define WIFI_CHECK_INTERVAL_MS      5000   /**< Interval for WiFi status check */
#define WIFI_CONNECT_TIMEOUT_MS     30000  /**< Timeout for WiFi connection */
#define WIFI_POLL_INTERVAL_MS       1      /**< Minimal delay for non-blocking poll */

typedef enum
{
    WIFI_STATUS_DISCONNECTED = 0,   /**< WiFi is disconnected */
    WIFI_STATUS_CONNECTING,         /**< WiFi connection in progress */
    WIFI_STATUS_CONNECTED,          /**< WiFi is connected */
    WIFI_STATUS_FAILED,             /**< WiFi connection failed */
    WIFI_STATUS_RECONNECTING        /**< WiFi reconnection in progress */
} wifi_status_t;

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


bool wifi_manager_init(void);

bool wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms);

wifi_status_t wifi_manager_check_status(void);

bool wifi_manager_handle_reconnect(void);

bool wifi_manager_is_connected(void);

bool wifi_manager_is_fully_connected(void);

void wifi_manager_set_fully_connected(bool connected);

const wifi_manager_state_t *wifi_manager_get_state(void);

void wifi_manager_disconnect(void);

void wifi_manager_deinit(void);

void wifi_manager_poll(void);

#endif /* WIFI_MANAGER_H */