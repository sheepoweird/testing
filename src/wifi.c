/**
 * @file    wifi_manager.c
 * @brief   WiFi connection management implementation
 * @author  Team IS-02
 * @date    2025-11-08
 */

/*******************************************************************************
 * INCLUDES
 ******************************************************************************/
#include "wifi.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include <stdio.h>
#include <string.h>

/*******************************************************************************
 * PRIVATE CONSTANTS
 ******************************************************************************/
#define MAX_CONNECTION_RETRIES      3U
#define DNS_QUERY_TIMEOUT_MS        10000U

/*******************************************************************************
 * PRIVATE VARIABLES
 ******************************************************************************/
static wifi_manager_state_t g_wifi_state = {0};
static char g_ssid[64] = {0};
static char g_password[64] = {0};

/*******************************************************************************
 * PRIVATE FUNCTION PROTOTYPES
 ******************************************************************************/
static bool wifi_wait_for_connection(uint32_t timeout_ms);
static bool wifi_test_dns(void);
static void wifi_update_led_status(void);

/*******************************************************************************
 * PUBLIC FUNCTION IMPLEMENTATIONS
 ******************************************************************************/

/**
 * @brief Initialize WiFi hardware and CYW43 chip
 */
bool wifi_manager_init(void)
{
    printf("WiFi Manager: Initializing CYW43 chip...\n");

    /* Initialize CYW43 in threadsafe background mode */
    if (cyw43_arch_init() != 0)
    {
        printf("WiFi Manager: ERROR - CYW43 initialization failed!\n");
        g_wifi_state.status = WIFI_STATUS_FAILED;
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
        return false;
    }

    /* Wait for link to stabilize */
    if (!wifi_wait_for_connection(5000))
    {
        printf("WiFi Manager: Link stabilization failed!\n");
        g_wifi_state.status = WIFI_STATUS_FAILED;
        g_wifi_state.is_connected = false;
        return false;
    }

    printf("WiFi Manager: WiFi connected successfully!\n");
    g_wifi_state.is_connected = true;
    g_wifi_state.status = WIFI_STATUS_CONNECTED;
    g_wifi_state.reconnect_pending = false;

    /* Test DNS resolution */
    if (wifi_test_dns())
    {
        printf("WiFi Manager: DNS resolution successful\n");
        g_wifi_state.is_fully_connected = true;
        printf("*** WiFi Manager: FULLY CONNECTED ***\n");
    }
    else
    {
        printf("WiFi Manager: WARNING - DNS resolution failed\n");
        g_wifi_state.is_fully_connected = false;
    }

    return true;
}

/**
 * @brief Check and monitor WiFi connection status
 */
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

            /* Re-test DNS */
            if (wifi_test_dns())
            {
                g_wifi_state.is_fully_connected = true;
            }
        }
    }

    return g_wifi_state.status;
}

/**
 * @brief Handle WiFi reconnection logic
 */
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

    printf("WiFi Manager: Attempting reconnection (attempt %lu)...\n", 
           g_wifi_state.connect_attempts + 1);

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

/**
 * @brief Get current WiFi connection status
 */
bool wifi_manager_is_connected(void)
{
    return g_wifi_state.is_connected;
}

/**
 * @brief Get full connection status
 */
bool wifi_manager_is_fully_connected(void)
{
    return g_wifi_state.is_fully_connected;
}

/**
 * @brief Set full connection status
 */
void wifi_manager_set_fully_connected(bool connected)
{
    g_wifi_state.is_fully_connected = connected;
}

/**
 * @brief Get WiFi manager state
 */
const wifi_manager_state_t *wifi_manager_get_state(void)
{
    return &g_wifi_state;
}

/**
 * @brief Disconnect from WiFi network
 */
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

/**
 * @brief Deinitialize WiFi hardware
 */
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
 * @brief Poll WiFi hardware
 */
void wifi_manager_poll(void)
{
    if (g_wifi_state.is_initialized)
    {
        cyw43_arch_poll();
    }
}

/*******************************************************************************
 * PRIVATE FUNCTION IMPLEMENTATIONS
 ******************************************************************************/

/**
 * @brief Wait for WiFi connection to stabilize
 * 
 * @param[in] timeout_ms    Timeout in milliseconds
 * @return true if connection stable, false on timeout
 */
static bool wifi_wait_for_connection(uint32_t timeout_ms)
{
    uint32_t start = to_ms_since_boot(get_absolute_time());
    
    while ((to_ms_since_boot(get_absolute_time()) - start) < timeout_ms)
    {
        int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        
        if (status == CYW43_LINK_UP)
        {
            return true;
        }
        
        sleep_ms(100);
    }
    
    return false;
}

    /**
     * @brief Test DNS resolution
     * 
     * @return true if DNS works, false otherwise
     */
static bool wifi_test_dns(void)
{
    ip_addr_t resolved_addr;
    const char *test_hostname = "google.com";
    
    printf("WiFi Manager: Testing DNS resolution for '%s'...\n", test_hostname);
    
    err_t err = dns_gethostbyname(test_hostname, &resolved_addr, NULL, NULL);
    
    if (err == ERR_OK)
    {
        printf("WiFi Manager: DNS test successful (cached)\n");
        return true;
    }
    else if (err == ERR_INPROGRESS)
    {
        /* Wait for DNS resolution */
        uint32_t start = to_ms_since_boot(get_absolute_time());
        
        while ((to_ms_since_boot(get_absolute_time()) - start) < DNS_QUERY_TIMEOUT_MS)
        {
            cyw43_arch_poll();
            
            err = dns_gethostbyname(test_hostname, &resolved_addr, NULL, NULL);
            
            if (err == ERR_OK)
            {
                printf("WiFi Manager: DNS test successful\n");
                return true;
            }
            
            sleep_ms(100);
        }
        
        printf("WiFi Manager: DNS test timeout\n");
        return false;
    }
    else
    {
        printf("WiFi Manager: DNS test failed with error %d\n", err);
        return false;
    }
}