#ifndef HTTPS_CLIENT_H
#define HTTPS_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdbool.h>
#include <stdint.h>
#include "lwip/err.h"
#include "lwip/altcp.h"
#include "lwip/ip_addr.h"

/*******************************************************************************
 * CONSTANTS AND MACROS
 ******************************************************************************/
#define HTTPS_DEFAULT_PORT          443U
#define HTTPS_DNS_TIMEOUT_MS        10000U
#define HTTPS_CONNECT_TIMEOUT_MS    100000U
#define HTTPS_REQUEST_TIMEOUT_MS    50000U
#define HTTPS_MAX_REQUEST_SIZE      2048U
#define HTTPS_MAX_JSON_BODY_SIZE    512U

/*******************************************************************************
 * TYPE DEFINITIONS
 ******************************************************************************/

/**
 * @brief HTTPS client status enumeration
 */
typedef enum
{
    HTTPS_STATUS_IDLE = 0,          /**< Client is idle */
    HTTPS_STATUS_RESOLVING_DNS,     /**< Resolving DNS */
    HTTPS_STATUS_CONNECTING,        /**< Connecting to server */
    HTTPS_STATUS_CONNECTED,         /**< Connected, TLS handshake complete */
    HTTPS_STATUS_SENDING,           /**< Sending request */
    HTTPS_STATUS_RECEIVING,         /**< Receiving response */
    HTTPS_STATUS_COMPLETE,          /**< Operation complete */
    HTTPS_STATUS_ERROR              /**< Error occurred */
} https_status_t;

/**
 * @brief HTTPS operation configuration
 */
typedef struct
{
    const char *hostname;           /**< Server hostname */
    const char *path;               /**< Request path (e.g., "/webhook/abc") */
    uint16_t port;                  /**< Server port (usually 443) */
    const uint8_t *ca_cert;         /**< CA certificate (PEM format) */
    size_t ca_cert_len;             /**< CA certificate length */
    const uint8_t *client_cert;     /**< Client certificate (for mTLS, PEM format) */
    size_t client_cert_len;         /**< Client certificate length */
    bool use_atecc;                 /**< Use ATECC608B for signing */
    bool enable_mtls;               /**< Enable mutual TLS */
} https_config_t;

/**
 * @brief HTTPS client state structure
 */
typedef struct
{
    https_status_t status;          /**< Current operation status */
    struct altcp_tls_config *tls_config;  /**< TLS configuration */
    struct altcp_pcb *pcb;          /**< Protocol control block */
    bool is_connected;              /**< Connection established flag */
    bool request_sent;              /**< Request sent flag */
    bool operation_in_progress;     /**< Operation in progress flag */
    uint16_t bytes_received;        /**< Bytes received in response */
    uint32_t operation_start_time;  /**< Operation start timestamp */
    ip_addr_t server_ip;            /**< Resolved server IP address */
    bool dns_resolved;              /**< DNS resolution complete flag */
} https_client_state_t;

/*******************************************************************************
 * PUBLIC FUNCTION PROTOTYPES
 ******************************************************************************/

/**
 * @brief Initialize HTTPS client module
 * 
 * This function must be called before any other HTTPS client operations.
 * 
 * @return true if initialization successful, false otherwise
 */
bool https_client_init(void);

/**
 * @brief Configure HTTPS client for an operation
 * 
 * Sets up the HTTPS client with server details and certificates.
 * 
 * @param[in] config    Pointer to configuration structure
 * 
 * @return true if configuration successful, false otherwise
 */
bool https_client_configure(const https_config_t *config);

/**
 * @brief Send HTTPS POST request with JSON payload
 * 
 * Performs complete HTTPS POST operation including DNS resolution,
 * TLS handshake, request sending, and response handling.
 * 
 * @param[in] hostname      Server hostname
 * @param[in] path          Request path
 * @param[in] json_body     JSON payload string
 * @param[in] body_len      Length of JSON payload
 * 
 * @return true if request sent successfully, false otherwise
 */
bool https_client_post_json(const char *hostname, 
                            const char *path,
                            const char *json_body,
                            size_t body_len);

/**
 * @brief Send pre-configured HTTPS POST request
 * 
 * Uses configuration set by https_client_configure().
 * 
 * @param[in] path          Request path
 * @param[in] json_body     JSON payload string
 * @param[in] body_len      Length of JSON payload
 * 
 * @return true if request sent successfully, false otherwise
 */
bool https_client_post(const char *path,
                       const char *json_body,
                       size_t body_len);

/**
 * @brief Get current HTTPS client status
 * 
 * @return Current client status
 */
https_status_t https_client_get_status(void);

/**
 * @brief Get HTTPS client state
 * 
 * Returns pointer to internal state for monitoring.
 * 
 * @return Pointer to client state (read-only)
 */
const https_client_state_t *https_client_get_state(void);

/**
 * @brief Check if HTTPS operation is in progress
 * 
 * @return true if operation in progress, false otherwise
 */
bool https_client_is_busy(void);

/**
 * @brief Abort current HTTPS operation
 * 
 * Cleanly closes connection and resets state.
 */
void https_client_abort(void);

/**
 * @brief Cleanup and deinitialize HTTPS client
 * 
 * Frees all resources and closes connections.
 */
void https_client_deinit(void);

/**
 * @brief Resolve DNS hostname to IP address
 * 
 * Performs blocking DNS resolution with timeout.
 * 
 * @param[in]  hostname     Hostname to resolve
 * @param[out] ip_addr      Resolved IP address
 * @param[in]  timeout_ms   Timeout in milliseconds
 * 
 * @return ERR_OK if successful, lwIP error code otherwise
 */
err_t https_client_resolve_dns(const char *hostname,
                               ip_addr_t *ip_addr,
                               uint32_t timeout_ms);

/**
 * @brief Get bytes received in last response
 * 
 * @return Number of bytes received
 */
uint16_t https_client_get_bytes_received(void);

/**
 * @brief Reset HTTPS client state
 * 
 * Resets state machine to idle without freeing resources.
 */
void https_client_reset(void);

/*******************************************************************************
 * CALLBACK FUNCTION TYPES
 ******************************************************************************/

/**
 * @brief HTTPS response callback function type
 * 
 * Called when response data is received.
 * 
 * @param[in] data      Response data buffer
 * @param[in] len       Length of response data
 * @param[in] user_arg  User-provided argument
 */
typedef void (*https_response_callback_t)(const uint8_t *data, 
                                          size_t len, 
                                          void *user_arg);

/**
 * @brief Set response callback function
 * 
 * @param[in] callback  Callback function pointer
 * @param[in] user_arg  User argument passed to callback
 */
void https_client_set_response_callback(https_response_callback_t callback,
                                       void *user_arg);

#ifdef __cplusplus
}
#endif

#endif /* HTTPS_CLIENT_H */