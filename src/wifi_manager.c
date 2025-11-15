#include "wifi_manager.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/gpio.h"

// Internal state structure
typedef struct {
    wifi_config_t config;
    wifi_state_t state;
    bool initialized;
    bool cyw43_initialized;
    uint32_t last_check_time;
    uint32_t disconnect_time;
    bool reconnect_pending;
} wifi_manager_state_t;

// Global state
static wifi_manager_state_t g_wifi_state = {
    .state = WIFI_STATE_DISCONNECTED,
    .initialized = false,
    .cyw43_initialized = false,
    .last_check_time = 0,
    .disconnect_time = 0,
    .reconnect_pending = false
};

// Forward declarations
static void update_led_status(void);
static void print_link_status(int link_status);

bool wifi_manager_init(const wifi_config_t* config)
{
    if (!config || !config->ssid || !config->password) {
        printf("WiFi Manager: Invalid configuration\n");
        return false;
    }

    printf("WiFi Manager: Initializing...\n");

    // Copy configuration
    g_wifi_state.config = *config;
    
    // Set default values if not specified
    if (g_wifi_state.config.reconnect_delay_ms == 0) {
        g_wifi_state.config.reconnect_delay_ms = 5000;
    }
    if (g_wifi_state.config.connection_timeout_ms == 0) {
        g_wifi_state.config.connection_timeout_ms = 30000;
    }

    // Initialize LED if specified
    if (g_wifi_state.config.led_pin > 0) {
        gpio_init(g_wifi_state.config.led_pin);
        gpio_set_dir(g_wifi_state.config.led_pin, GPIO_OUT);
        gpio_put(g_wifi_state.config.led_pin, 0);
    }

    // Deinitialize previous instance if exists
    if (g_wifi_state.cyw43_initialized) {
        printf("WiFi Manager: Deinitializing previous instance...\n");
        cyw43_arch_deinit();
        g_wifi_state.cyw43_initialized = false;
        sleep_ms(1000);
    }

    // Initialize CYW43
    if (cyw43_arch_init()) {
        printf("WiFi Manager: CYW43 initialization FAILED\n");
        g_wifi_state.state = WIFI_STATE_ERROR;
        update_led_status();
        return false;
    }

    g_wifi_state.cyw43_initialized = true;
    cyw43_arch_enable_sta_mode();
    printf("WiFi Manager: STA mode enabled\n");

    g_wifi_state.initialized = true;
    g_wifi_state.state = WIFI_STATE_DISCONNECTED;
    
    return true;
}

void wifi_manager_deinit(void)
{
    if (g_wifi_state.cyw43_initialized) {
        cyw43_arch_deinit();
        g_wifi_state.cyw43_initialized = false;
    }

    if (g_wifi_state.config.led_pin > 0) {
        gpio_put(g_wifi_state.config.led_pin, 0);
    }

    g_wifi_state.initialized = false;
    g_wifi_state.state = WIFI_STATE_DISCONNECTED;
    
    printf("WiFi Manager: Deinitialized\n");
}

bool wifi_manager_connect(void)
{
    if (!g_wifi_state.initialized) {
        printf("WiFi Manager: Not initialized\n");
        return false;
    }

    printf("WiFi Manager: Connecting to '%s'...\n", g_wifi_state.config.ssid);
    g_wifi_state.state = WIFI_STATE_CONNECTING;
    update_led_status();

    // Print current link status
    int link_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    printf("WiFi Manager: Current link status: ");
    print_link_status(link_status);

    // Attempt connection
    int connect_result = cyw43_arch_wifi_connect_timeout_ms(
        g_wifi_state.config.ssid,
        g_wifi_state.config.password,
        CYW43_AUTH_WPA2_AES_PSK,
        g_wifi_state.config.connection_timeout_ms
    );

    if (connect_result != 0) {
        printf("WiFi Manager: Connection FAILED (error %d)\n", connect_result);
        g_wifi_state.state = WIFI_STATE_DISCONNECTED;
        update_led_status();
        return false;
    }

    printf("WiFi Manager: Connected successfully!\n");

    // Print IP address
    uint32_t ip = cyw43_state.netif[0].ip_addr.addr;
    printf("WiFi Manager: IP Address: %lu.%lu.%lu.%lu\n",
           ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);

    g_wifi_state.state = WIFI_STATE_CONNECTED;
    g_wifi_state.reconnect_pending = false;
    update_led_status();
    
    return true;
}

void wifi_manager_task(void)
{
    if (!g_wifi_state.initialized || !g_wifi_state.cyw43_initialized) {
        return;
    }

    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Rate limit checks to every 5 seconds
    if (now - g_wifi_state.last_check_time < 5000) {
        return;
    }

    g_wifi_state.last_check_time = now;

    // Check link status
    int link_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

    if (link_status != CYW43_LINK_UP) {
        // Connection lost
        if (g_wifi_state.state == WIFI_STATE_CONNECTED) {
            printf("\nWiFi Manager: Connection lost!\n");
            g_wifi_state.state = WIFI_STATE_DISCONNECTED;
            g_wifi_state.disconnect_time = now;
            g_wifi_state.reconnect_pending = true;
            update_led_status();
        }
        // Handle reconnection
        else if (g_wifi_state.reconnect_pending && 
                 (now - g_wifi_state.disconnect_time >= g_wifi_state.config.reconnect_delay_ms)) {
            printf("WiFi Manager: Attempting reconnection...\n");
            g_wifi_state.reconnect_pending = false;
            g_wifi_state.state = WIFI_STATE_RECONNECTING;
            update_led_status();
            
            if (wifi_manager_connect()) {
                printf("WiFi Manager: Reconnected successfully!\n");
            } else {
                g_wifi_state.disconnect_time = now;
                g_wifi_state.reconnect_pending = true;
                printf("WiFi Manager: Reconnection failed, will retry...\n");
            }
        }
    } else {
        // Connection restored
        if (g_wifi_state.state != WIFI_STATE_CONNECTED) {
            g_wifi_state.state = WIFI_STATE_CONNECTED;
            g_wifi_state.reconnect_pending = false;
            update_led_status();
            printf("WiFi Manager: Link restored!\n");
        }
    }
}

void wifi_manager_poll(void)
{
    if (g_wifi_state.cyw43_initialized) {
        cyw43_arch_poll();
    }
}

wifi_state_t wifi_manager_get_state(void)
{
    return g_wifi_state.state;
}

bool wifi_manager_is_connected(void)
{
    return g_wifi_state.state == WIFI_STATE_CONNECTED;
}

bool wifi_manager_is_fully_connected(void)
{
    return g_wifi_state.initialized && 
           g_wifi_state.cyw43_initialized && 
           g_wifi_state.state == WIFI_STATE_CONNECTED;
}

bool wifi_manager_get_ip_string(char* buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return false;
    }

    if (!wifi_manager_is_connected()) {
        buffer[0] = '\0';
        return false;
    }

    uint32_t ip = cyw43_state.netif[0].ip_addr.addr;
    snprintf(buffer, buffer_size, "%lu.%lu.%lu.%lu",
             ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    
    return true;
}

uint32_t wifi_manager_get_ip(void)
{
    if (!wifi_manager_is_connected()) {
        return 0;
    }
    
    return cyw43_state.netif[0].ip_addr.addr;
}

bool wifi_manager_reconnect(void)
{
    if (!g_wifi_state.initialized) {
        return false;
    }

    printf("WiFi Manager: Forcing reconnection...\n");
    
    // Reset reconnection flags
    g_wifi_state.reconnect_pending = false;
    g_wifi_state.disconnect_time = 0;
    
    return wifi_manager_connect();
}

// Internal helper functions

static void update_led_status(void)
{
    if (g_wifi_state.config.led_pin == 0) {
        return;
    }

    switch (g_wifi_state.state) {
        case WIFI_STATE_CONNECTED:
            // Solid ON when connected
            gpio_put(g_wifi_state.config.led_pin, 1);
            break;
            
        case WIFI_STATE_DISCONNECTED:
        case WIFI_STATE_CONNECTING:
        case WIFI_STATE_RECONNECTING:
        case WIFI_STATE_ERROR:
            // OFF for error/disconnected states
            // (blinking will be handled in main loop if needed)
            gpio_put(g_wifi_state.config.led_pin, 0);
            break;
    }
}

static void print_link_status(int link_status)
{
    switch (link_status) {
        case CYW43_LINK_DOWN:
            printf("LINK_DOWN\n");
            break;
        case CYW43_LINK_JOIN:
            printf("LINK_JOIN (WiFi joined)\n");
            break;
        case CYW43_LINK_NOIP:
            printf("LINK_NOIP (No IP)\n");
            break;
        case CYW43_LINK_UP:
            printf("LINK_UP\n");
            break;
        case CYW43_LINK_FAIL:
            printf("LINK_FAIL\n");
            break;
        case CYW43_LINK_NONET:
            printf("LINK_NONET\n");
            break;
        case CYW43_LINK_BADAUTH:
            printf("LINK_BADAUTH\n");
            break;
        default:
            printf("UNKNOWN (%d)\n", link_status);
            break;
    }
}