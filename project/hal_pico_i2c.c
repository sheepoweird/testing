/**
 * @file hal_pico_i2c.c
 * @brief Unified HAL implementation for CryptoAuthLib on Raspberry Pi Pico
 * 
 * This HAL provides I2C communication, memory management, and timing functions
 * required by CryptoAuthLib for interfacing with ATECC608B.
 */

#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "cryptoauthlib.h"

// I2C Configuration
#define HAL_I2C_INSTANCE    i2c0
#define HAL_I2C_SDA_PIN     4
#define HAL_I2C_SCL_PIN     5
#define HAL_I2C_BAUDRATE    100000  // 100kHz for stability
#define HAL_I2C_DEVICE_ADDR 0x60    // ATECC608B 7-bit address

// ============================================================================
// MEMORY MANAGEMENT FUNCTIONS
// ============================================================================

void* hal_malloc(size_t size) {
    return malloc(size);
}

void hal_free(void* ptr) {
    if (ptr) {
        free(ptr);
    }
}

// ============================================================================
// TIMING FUNCTIONS
// ============================================================================

void hal_delay_ms(uint32_t ms) {
    busy_wait_ms(ms);
}

void hal_delay_us(uint32_t us) {
    busy_wait_us(us);
}

// Alternative names that some parts of CryptoAuthLib may expect
void atca_delay_ms(uint32_t ms) {
    busy_wait_ms(ms);
}

void atca_delay_us(uint32_t us) {
    busy_wait_us(us);
}

// ============================================================================
// I2C HAL IMPLEMENTATION
// ============================================================================

/**
 * @brief Initialize I2C interface for ATECC608B
 */
ATCA_STATUS hal_i2c_init(ATCAIface iface, ATCAIfaceCfg* cfg) {
    printf("HAL: I2C init called\n");
    
    // Initialize I2C hardware
    i2c_init(HAL_I2C_INSTANCE, HAL_I2C_BAUDRATE);
    gpio_set_function(HAL_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(HAL_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(HAL_I2C_SDA_PIN);
    gpio_pull_up(HAL_I2C_SCL_PIN);
    
    printf("HAL: I2C initialized at %d kHz\n", HAL_I2C_BAUDRATE / 1000);
    return ATCA_SUCCESS;
}

/**
 * @brief Post-initialization (optional, currently not needed)
 */
ATCA_STATUS hal_i2c_post_init(ATCAIface iface) {
    printf("HAL: I2C post_init called\n");
    return ATCA_SUCCESS;
}

/**
 * @brief Send data over I2C to ATECC608B
 * 
 * Handles special cases:
 * - Wake sequence (address 0x00/0x01 with 0 bytes)
 * - Word address prepending (0x02/0x03)
 * - Regular command transmission
 */
ATCA_STATUS hal_i2c_send(ATCAIface iface, uint8_t address, uint8_t* data, int len) {
    // printf("HAL: Send to addr 0x%02X, len %d: ", address, len);
    if (len > 0 && data) {
        for (int i = 0; i < len && i < 8; i++) {
            // printf("%02X ", data[i]);
        }
        if (len > 8) printf("...");
    }
    // printf("\n");
    
    // SPECIAL CASE 1: Wake sequence detection
    // CryptoAuthLib sends to 0x00 or 0x01 with 0 bytes to trigger wake
    if ((address == 0x00 || address == 0x01) && len == 0) {
        // printf("HAL: Wake sequence detected - performing GPIO wake\n");
        
        // Execute GPIO-based wake pulse
        gpio_set_function(HAL_I2C_SDA_PIN, GPIO_FUNC_SIO);  // Switch to GPIO
        gpio_set_dir(HAL_I2C_SDA_PIN, GPIO_OUT);             // Set as output
        gpio_put(HAL_I2C_SDA_PIN, 0);                        // Drive SDA LOW
        
        busy_wait_us(80);                                        // Hold low for 80µs
        
        gpio_put(HAL_I2C_SDA_PIN, 1);                        // Release SDA
        gpio_set_function(HAL_I2C_SDA_PIN, GPIO_FUNC_I2C);   // Switch back to I2C
        gpio_pull_up(HAL_I2C_SDA_PIN);
        
        busy_wait_ms(2);                                         // Wait 2ms for wake
        
        // printf("HAL: Wake pulse complete\n");
        return ATCA_SUCCESS;
    }
    
    // Validate parameters for regular transmission
    if (!data || len <= 0) {
        // printf("\nHAL: Bad params\n");
        return ATCA_BAD_PARAM;
    }
    
    // SPECIAL CASE 2: Word address prepending
    // Some CryptoAuthLib operations use 0x02/0x03 as word addresses
    if (address == 0x02 || address == 0x03) {
        // printf("HAL: Detected word address 0x%02X, prepending to command\n", address);
        
        // Create buffer with word address prepended
        uint8_t full_cmd[len + 1];
        full_cmd[0] = address;              // Word address first
        memcpy(full_cmd + 1, data, len);    // Then command data
        
        // printf("HAL: Full command: ");
        for (int i = 0; i < len + 1; i++) {
            // printf("%02X ", full_cmd[i]);
        }
        // printf("\n");
        
        // Send to device address 0x60
        int result = i2c_write_blocking(HAL_I2C_INSTANCE, HAL_I2C_DEVICE_ADDR, 
                                        full_cmd, len + 1, false);
        // printf("HAL: Write result: %d (expected %d)\n", result, len + 1);
        
        return (result == len + 1) ? ATCA_SUCCESS : ATCA_COMM_FAIL;
    }
    
    // REGULAR CASE: Standard I2C transmission
    int result = i2c_write_blocking(HAL_I2C_INSTANCE, address, data, len, false);
    // printf("HAL: Write result: %d (expected %d)\n", result, len);
    
    if (result == len) {
        return ATCA_SUCCESS;
    } else {
        return ATCA_COMM_FAIL;
    }
}

/**
 * @brief Receive data over I2C from ATECC608B
 */
ATCA_STATUS hal_i2c_receive(ATCAIface iface, uint8_t address, uint8_t* data, uint16_t* len) {
    // printf("HAL: Receive from addr 0x%02X, expecting %d bytes\n", address, len ? *len : 0);
    
    if (!data || !len || *len == 0) {
        // printf("HAL: Bad receive params\n");
        return ATCA_BAD_PARAM;
    }
    
    // Perform blocking I2C read
    int result = i2c_read_blocking(HAL_I2C_INSTANCE, address, data, *len, false);
    
    // printf("HAL: Read result: %d bytes: ", result);
    if (result > 0) {
        for (int i = 0; i < result && i < 8; i++) {
            // printf("%02X ", data[i]);
        }
        if (result > 8) printf("...");
    }
    // printf("\n");
    
    if (result == *len) {
        return ATCA_SUCCESS;
    } else {
        *len = (result > 0) ? result : 0;
        return ATCA_COMM_FAIL;
    }
}

/**
 * @brief Control function for special operations (wake, idle, sleep)
 */
ATCA_STATUS hal_i2c_control(ATCAIface iface, uint8_t option, void* param, size_t paramlen) {
    // printf("HAL: Control called with option 0x%02X\n", option);
    
    // Handle wake operation (option 0x01)
    if (option == 0x01) {
        printf("HAL: Performing GPIO wake pulse\n");
        
        gpio_set_function(HAL_I2C_SDA_PIN, GPIO_FUNC_SIO);  // Switch to GPIO
        gpio_set_dir(HAL_I2C_SDA_PIN, GPIO_OUT);             // Set as output
        gpio_put(HAL_I2C_SDA_PIN, 0);                        // Drive SDA LOW
        
        busy_wait_us(80);                                        // Hold low for 80µs
        
        gpio_put(HAL_I2C_SDA_PIN, 1);                        // Release SDA
        gpio_set_function(HAL_I2C_SDA_PIN, GPIO_FUNC_I2C);   // Switch back to I2C
        gpio_pull_up(HAL_I2C_SDA_PIN);
        
        busy_wait_ms(2);                                         // Wait 2ms for wake
        
        printf("HAL: Wake pulse complete\n");
        return ATCA_SUCCESS;
    }
    
    // For other control options, return success
    return ATCA_SUCCESS;
}

/**
 * @brief Release/cleanup I2C interface
 */
ATCA_STATUS hal_i2c_release(void* hal_data) {
    printf("HAL: I2C release called\n");
    // No cleanup needed for Pico I2C
    return ATCA_SUCCESS;
}

/**
 * @brief Discover available I2C buses
 */
ATCA_STATUS hal_i2c_discover_buses(int* buses_found, int max_buses) {
    if (buses_found) {
        *buses_found = 1;  // Only i2c0 is used
    }
    return ATCA_SUCCESS;
}

/**
 * @brief Discover devices on I2C bus
 */
ATCA_STATUS hal_i2c_discover_devices(int bus_num, uint8_t* devices_found, int max_devices) {
    // Simple device discovery - check if ATECC608B responds at 0x60
    if (devices_found && max_devices > 0) {
        uint8_t dummy = 0;
        int result = i2c_write_timeout_us(HAL_I2C_INSTANCE, HAL_I2C_DEVICE_ADDR, 
                                          &dummy, 0, false, 50000);
        if (result >= 0) {
            devices_found[0] = HAL_I2C_DEVICE_ADDR;
            return ATCA_SUCCESS;
        }
    }
    return ATCA_COMM_FAIL;
}