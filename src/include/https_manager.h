#ifndef HTTPS_MANAGER_H
#define HTTPS_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Forward declarations for lwIP types
struct altcp_tls_config;
struct altcp_pcb;

// HTTPS connection states
typedef enum {
    HTTPS_STATE_IDLE,
    HTTPS_STATE_DNS_RESOLVING,
    HTTPS_STATE_CONNECTING,
    HTTPS_STATE_CONNECTED,
    HTTPS_STATE_SENDING,
    HTTPS_STATE_RECEIVING,
    HTTPS_STATE_COMPLETE,
    HTTPS_STATE_ERROR
} https_state_t;

// HTTPS configuration structure
typedef struct {
    const char* hostname;
    const char* webhook_token;
    uint16_t port;
    
    // TLS configuration
    const uint8_t* ca_cert;
    size_t ca_cert_len;
    
    // mTLS configuration (optional)
    bool enable_mtls;
    const uint8_t* client_cert;
    size_t client_cert_len;
    void* atecc_pk_context;  // mbedtls_pk_context* if using ATECC
    
    // LED indicators (optional, 0 = disabled)
    uint8_t dns_led_pin;
    uint8_t mtls_led_pin;
    
    // Timeouts
    uint32_t operation_timeout_ms;
} https_config_t;

// Data structure for POST requests
typedef struct {
    uint32_t sample;
    uint32_t timestamp;
    const char* device;
    float cpu;
    float memory;
    float disk;
    float net_in;
    float net_out;
    int processes;
} https_post_data_t;

/**
 * Initialize HTTPS manager with configuration
 * @param config HTTPS configuration structure
 * @return true if initialization successful, false otherwise
 */
bool https_manager_init(const https_config_t* config);

/**
 * Deinitialize HTTPS manager and cleanup resources
 */
void https_manager_deinit(void);

/**
 * Send POST request with JSON data
 * @param data Data to send in POST request
 * @return true if request initiated successfully, false otherwise
 */
bool https_manager_post_json(const https_post_data_t* data);

/**
 * Check if HTTPS manager is currently busy with an operation
 * @return true if busy, false if idle
 */
bool https_manager_is_busy(void);

/**
 * Get current HTTPS state
 * @return Current state
 */
https_state_t https_manager_get_state(void);

/**
 * Get number of bytes received in last operation
 * @return Bytes received
 */
uint16_t https_manager_get_bytes_received(void);

/**
 * Abort current operation and cleanup
 */
void https_manager_abort(void);

/**
 * Task function - handles timeouts and cleanup
 * Should be called periodically from main loop
 */
void https_manager_task(void);

#endif // HTTPS_MANAGER_H