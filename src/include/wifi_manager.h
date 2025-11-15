#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// WiFi manager configuration structure
typedef struct {
    const char* ssid;
    const char* password;
    uint32_t reconnect_delay_ms;
    uint32_t connection_timeout_ms;
    uint8_t led_pin;  // Optional LED for status indication (0 = disabled)
} wifi_config_t;

// WiFi connection states
typedef enum {
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_RECONNECTING,
    WIFI_STATE_ERROR
} wifi_state_t;

bool wifi_manager_init(const wifi_config_t* config);

void wifi_manager_deinit(void);

bool wifi_manager_connect(void);

void wifi_manager_task(void);

void wifi_manager_poll(void);

wifi_state_t wifi_manager_get_state(void);

bool wifi_manager_is_connected(void);

bool wifi_manager_is_fully_connected(void);

bool wifi_manager_get_ip_string(char* buffer, size_t buffer_size);

uint32_t wifi_manager_get_ip(void);

bool wifi_manager_reconnect(void);

#endif // WIFI_MANAGER_H
