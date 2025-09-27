#ifndef _CONFIG_H_
#define _CONFIG_H_

#include "pico/stdlib.h"
#include "tusb.h"

// HID Configuration
#define HID_REPORT_ID 0x01
#define HID_BUFFER_SIZE 64

// MSD Configuration
#define MSD_BLOCK_SIZE 512
#define MSD_BLOCK_COUNT 4096  // 2MB storage

// LED Indicators
#define LED_PIN 25

#endif