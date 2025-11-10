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

#define HTTPS_DEFAULT_PORT          443U
#define HTTPS_DNS_TIMEOUT_MS        10000U
#define HTTPS_CONNECT_TIMEOUT_MS    100000U
#define HTTPS_REQUEST_TIMEOUT_MS    50000U
#define HTTPS_MAX_REQUEST_SIZE      2048U
#define HTTPS_MAX_JSON_BODY_SIZE    512U

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

bool https_client_init(void);

bool https_client_configure(const https_config_t *config);

bool https_client_post_json(const char *hostname, 
                            const char *path,
                            const char *json_body,
                            size_t body_len);

bool https_client_post(const char *path,
                       const char *json_body,
                       size_t body_len);

https_status_t https_client_get_status(void);

const https_client_state_t *https_client_get_state(void);

bool https_client_is_busy(void);

void https_client_abort(void);

void https_client_deinit(void);

err_t https_client_resolve_dns(const char *hostname,
                               ip_addr_t *ip_addr,
                               uint32_t timeout_ms);

uint16_t https_client_get_bytes_received(void);

void https_client_reset(void);

typedef void (*https_response_callback_t)(const uint8_t *data, 
                                          size_t len, 
                                          void *user_arg);

void https_client_set_response_callback(https_response_callback_t callback,
                                       void *user_arg);

#ifdef __cplusplus
}
#endif

#endif /* HTTPS_CLIENT_H */