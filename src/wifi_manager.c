#include "wifi_manager.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>

#define DNS_QUERY_TIMEOUT_MS        10000
#define WIFI_LED_PIN                6

static wifi_manager_state_t g_wifi_state = {0};
static char g_ssid[64] = {0};
static char g_password[64] = {0};
static bool wifi_wait_for_connection(uint32_t timeout_ms);
static void wifi_check_full_connection(void);


bool wifi_manager_init(void)
{
    printf("WiFi Manager: Initializing CYW43 chip...\n");
    
    /* Initialize WiFi LED */
    gpio_init(WIFI_LED_PIN);
    gpio_set_dir(WIFI_LED_PIN, GPIO_OUT);
    gpio_put(WIFI_LED_PIN, 0);

    /* Initialize CYW43 in threadsafe background mode */
    if (cyw43_arch_init() != 0)
    {
        printf("WiFi Manager: ERROR - CYW43 initialization failed!\n");
        g_wifi_state.status = WIFI_STATUS_FAILED;
        gpio_put(WIFI_LED_PIN, 0);
        return false;
    }

    /* Enable WiFi station mode */
    cyw43_arch_enable_sta_mode();

    /* Initialize state */
    g_wifi_state.is_initialized = true;
    g_wifi_state.is_connected = false;
    g_wifi_state.is_fully_connected = false;
    g_wifi_state.reconnect_pending = false;
    g_wifi_state.status = WIFI_STATUS_DISCONNECTED;
    g_wifi_state.last_check_time = 0;
    g_wifi_state.disconnect_time = 0;
    g_wifi_state.connect_attempts = 0;

    printf("WiFi Manager: CYW43 initialized successfully\n");
    return true;
}

/**
 * @brief Connect to WiFi network
 */
bool wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (!g_wifi_state.is_initialized)
    {
        printf("WiFi Manager: ERROR - Not initialized!\n");
        return false;
    }

    if (ssid == NULL || password == NULL)
    {
        printf("WiFi Manager: ERROR - Invalid credentials!\n");
        return false;
    }

    /* Store credentials for reconnection */
    strncpy(g_ssid, ssid, sizeof(g_ssid) - 1);
    g_ssid[sizeof(g_ssid) - 1] = '\0';
    strncpy(g_password, password, sizeof(g_password) - 1);
    g_password[sizeof(g_password) - 1] = '\0';

    printf("WiFi Manager: Connecting to '%s'...\n", ssid);
    g_wifi_state.status = WIFI_STATUS_CONNECTING;
    g_wifi_state.connect_attempts++;

    /* Attempt WiFi connection */
    int result = cyw43_arch_wifi_connect_timeout_ms(
        ssid,
        password,
        CYW43_AUTH_WPA2_AES_PSK,
        timeout_ms
    );

    if (result != 0)
    {
        printf("WiFi Manager: Connection failed! Error: %d\n", result);
        g_wifi_state.status = WIFI_STATUS_FAILED;
        g_wifi_state.is_connected = false;
        gpio_put(WIFI_LED_PIN, 0);  /* LED OFF on failure */
        return false;
    }

    /* Wait for link to stabilize (only link up, DHCP is separate) */
    if (!wifi_wait_for_connection(5000))
    {
        printf("WiFi Manager: Link stabilization failed!\n");
        g_wifi_state.status = WIFI_STATUS_FAILED;
        g_wifi_state.is_connected = false;
        gpio_put(WIFI_LED_PIN, 0);  /* LED OFF on failure */
        return false;
    }

    g_wifi_state.is_connected = true;
    g_wifi_state.status = WIFI_STATUS_CONNECTED;
    g_wifi_state.reconnect_pending = false;
    gpio_put(WIFI_LED_PIN, 1);  /* LED ON when connected */

    return true;
}

wifi_status_t wifi_manager_check_status(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (!g_wifi_state.is_initialized)
    {
        return WIFI_STATUS_DISCONNECTED;
    }

    /* Rate limit status checks */
    if (now - g_wifi_state.last_check_time < WIFI_CHECK_INTERVAL_MS)
    {
        return g_wifi_state.status;
    }

    g_wifi_state.last_check_time = now;

    /* Check hardware link status */
    int link_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

    if (link_status != CYW43_LINK_UP)
    {
        /* Connection lost */
        if (g_wifi_state.is_connected)
        {
            printf("WiFi Manager: Connection lost!\n");
            g_wifi_state.is_connected = false;
            g_wifi_state.is_fully_connected = false;
            g_wifi_state.disconnect_time = now;
            g_wifi_state.reconnect_pending = true;
            g_wifi_state.status = WIFI_STATUS_DISCONNECTED;
            gpio_put(WIFI_LED_PIN, 0);  /* LED OFF when disconnected */
        }
    }
    else
    {
        /* Connection restored */
        if (!g_wifi_state.is_connected)
        {
            printf("WiFi Manager: Connection restored!\n");
            g_wifi_state.is_connected = true;
            g_wifi_state.reconnect_pending = false;
            g_wifi_state.status = WIFI_STATUS_CONNECTED;
            gpio_put(WIFI_LED_PIN, 1);  /* LED ON when connected */
        }
    }

    return g_wifi_state.status;
}

bool wifi_manager_handle_reconnect(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (!g_wifi_state.reconnect_pending)
    {
        return false;
    }

    /* Check if enough time has passed since disconnect */
    if ((now - g_wifi_state.disconnect_time) < WIFI_RECONNECT_DELAY_MS)
    {
        return false;
    }

    printf("WiFi Manager: Attempting reconnection...\n");

    g_wifi_state.reconnect_pending = false;
    g_wifi_state.status = WIFI_STATUS_RECONNECTING;

    /* Attempt reconnection with stored credentials */
    if (wifi_manager_connect(g_ssid, g_password, WIFI_CONNECT_TIMEOUT_MS))
    {
        printf("WiFi Manager: Reconnection successful!\n");
        return true;
    }
    else
    {
        printf("WiFi Manager: Reconnection failed, will retry...\n");
        g_wifi_state.disconnect_time = now;
        g_wifi_state.reconnect_pending = true;
        return false;
    }
}

bool wifi_manager_is_connected(void)
{
    return g_wifi_state.is_connected;
}

bool wifi_manager_is_fully_connected(void)
{
    return g_wifi_state.is_fully_connected;
}

void wifi_manager_set_fully_connected(bool connected)
{
    g_wifi_state.is_fully_connected = connected;
}

const wifi_manager_state_t *wifi_manager_get_state(void)
{
    return &g_wifi_state;
}

void wifi_manager_disconnect(void)
{
    if (g_wifi_state.is_initialized && g_wifi_state.is_connected)
    {
        printf("WiFi Manager: Disconnecting...\n");
        cyw43_arch_disable_sta_mode();
        g_wifi_state.is_connected = false;
        g_wifi_state.is_fully_connected = false;
        g_wifi_state.status = WIFI_STATUS_DISCONNECTED;
    }
}

// Deinitialize WiFi hardware
void wifi_manager_deinit(void)
{
    if (g_wifi_state.is_initialized)
    {
        printf("WiFi Manager: Deinitializing...\n");
        wifi_manager_disconnect();
        cyw43_arch_deinit();
        g_wifi_state.is_initialized = false;
        g_wifi_state.status = WIFI_STATUS_DISCONNECTED;
    }
}


/**
 * @brief Checks if the network interface has a valid IP address assigned.
 * This is the definition of "fully connected" (link up + DHCP complete).
 */
static void wifi_check_full_connection(void)
{
    // Check if the link is up
    if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP)
    {
        // FIX 1: Replaced '->' with '.' to resolve "operator applied to struct" error.
        if (cyw43_state.netif[CYW43_ITF_STA].ip_addr.addr != 0) 
        {
            if (!g_wifi_state.is_fully_connected) {
                printf("WiFi Manager: IP address assigned: %s\n", 
                       // FIX 2: Added '&' to pass a pointer to netif_ip4_addr().
                       ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]))); 
            }
            g_wifi_state.is_fully_connected = true;
            return;
        }
    }
    
    g_wifi_state.is_fully_connected = false;
}

// Polling WiFi
void wifi_manager_poll(void)
{
    if (g_wifi_state.is_initialized)
    {
        cyw43_arch_poll();
        
        // Only check for full connection status if the link is up
        if (g_wifi_state.is_connected) {
            wifi_check_full_connection();
        }
    }
}

static bool wifi_wait_for_connection(uint32_t timeout_ms)
{
    uint32_t start = to_ms_since_boot(get_absolute_time());
    
    while ((to_ms_since_boot(get_absolute_time()) - start) < timeout_ms)
    {
        // Poll arch continuously to process link status updates
        cyw43_arch_poll(); 

        int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        
        if (status == CYW43_LINK_UP)
        {
            return true;
        }
        
        // Use minimal sleep to avoid blocking network events
        sleep_ms(WIFI_POLL_INTERVAL_MS);
    }
    
    return false;
}