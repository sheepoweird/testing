#ifndef HAL_PICO_I2C_H
#define HAL_PICO_I2C_H

#include "cryptoauthlib.h"
#include <stdint.h>
#include <stddef.h>

// HAL function declarations
ATCA_STATUS hal_i2c_init(void* hal, ATCAIfaceCfg* cfg);
ATCA_STATUS hal_i2c_send(void* hal, uint8_t device_address, uint8_t* data, int len);
ATCA_STATUS hal_i2c_receive(void* hal, uint8_t device_address, uint8_t* data, uint16_t len);
ATCA_STATUS hal_i2c_wake(void* hal);
ATCA_STATUS hal_i2c_release(void* hal);
ATCA_STATUS hal_i2c_discover_buses(int* buses_found, int max_buses);
ATCA_STATUS hal_i2c_discover_devices(int bus_num, uint8_t* devices_found, int max_devices);

// Memory management functions
void* hal_malloc(size_t size);
void hal_free(void* ptr);

// Timing functions
void hal_delay_ms(uint32_t ms);
void hal_delay_us(uint32_t us);

// HAL registration function
ATCA_STATUS hal_i2c_register_hal(void);

// External HAL structure declaration
extern const ATCAHAL_t hal_i2c_pico;

#endif // HAL_PICO_I2C_H