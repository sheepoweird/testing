## Overview
This project implements a secure IoT device using Raspberry Pi Pico W that automatically collects Windows PC health metrics and transmits them to a remote server via HTTPS with mutual TLS (mTLS) authentication. The device uses hardware-based cryptography (ATECC608B) for secure client authentication and operates autonomously without user intervention.

Key Features:
- Automatic HID keyboard automation to launch monitoring application
- USB Mass Storage (MSC) and CDC serial communication
- Hardware-accelerated mTLS with ATECC608B cryptographic chip
- Dual-core processing (Core 0: USB/HID, Core 1: WiFi/HTTPS)
- System health monitoring (CPU, Memory, Disk, Network, Processes)



## Project Folders

src/
Main source code directory

include/
Header files for the project

lib/
Contains external libraries and dependencies

lib/cryptoauthlib/
Microchip ATECC608B cryptographic authentication library

lib/ioStream/
Input/output stream utilities

lib/sd_card/
SD card driver and FAT filesystem (FatFs)

Python exe/
Contains health-cdc.exe Windows application and the python code

Uf2 Files/
Compiled firmware files ready for flashing

Uf2 Files/cryptoauth_pico.uf2
ATECC608B uf2 to lock chip and retrieve keys (1 time use)



## Project Files

main.c
Main program entry, Core 0 logic (USB, HID, CDC serial)

wifi_manager.c / wifi_manager.h
Core 1 WiFi connection and maintenance

webhook_manager.c / webhook_manager.h
HTTPS POST with mTLS using mbedTLS

atecc_alt.c / atecc_alt.h
Hardware signing integration with ATECC608B

hal_pico_i2c.c
Hardware abstraction layer for I2C communication with ATECC608B

hid_manager.c
HID keyboard automation to launch health-cdc.exe

https_client.c
HTTPS client implementation for secure connections

https_manager.c
High-level HTTPS request management

hwi_config.c
Hardware interface configuration

json_processor.c
JSON parsing and processing of health data

lwipopt.h
LwIP TCP/IP stack configuration

mbedtls_config.h
mbedTLS cryptography library configuration

msc_disk.c
USB Mass Storage Class disk implementation

msc_manager.c
USB MSC device management

tusb_config.h
TinyUSB configuration for USB interfaces

usb_descriptors.c
USB device descriptors (MSC, HID, CDC)

health-cdc.exe
Windows application that sends system health data

CMakeLists.txt
Build configuration for Pico SDK

pico_sdk_import.cmake
Pico SDK integration file



## Hardware Requirements

- Raspberry Pi Pico W (RP2040 with WiFi)
- ATECC608B crypto chip (I2C)
  SDA: GP4
  SCL: GP5
- MicroSD Card (FAT32 formatted)
  Contains health-cdc.exe
- USB Cable (for power and data)



## Setup Instructions

### 1. Install Pico SDK
Clone Pico SDK version 2.2.0:

git clone -b 2.2.0 https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk
git submodule update --init

### 2. Configure Project
Edit CMakeLists.txt and set the PICO_SDK_PATH:

set(PICO_SDK_PATH "insert path here")

Example:
set(PICO_SDK_PATH "/home/user/pico-sdk")

### 3. Configure WiFi and Server
Edit config.h with your settings:

WIFI_SSID: Your network name
WIFI_PASSWORD: Your WiFi password
WEBHOOK_HOSTNAME: Your server address

### 4. Prepare SD Card
Format microSD card as FAT32
Copy health-cdc.exe to the root directory of SD card
Insert SD card into Pico W



## Compile Instructions

### Step 1: Go to CMAKE Extension Tab
Set compiler to GCC 10.3.1 arm-none-eabi

### Step 2: Clear Build Folder
Clear build folder in project

### Step 3: Build Project
Click on build button

### Step 4: Flash to Pico W
Hold BOOTSEL button on Pico W
Connect USB cable to PC
Copy embedded_token.uf2 to the mounted Pico drive
Device will reboot automatically



## Running the Project

### Automatic Operation Sequence

1. Boot
   - Core 0 initializes USB (MSC + HID + CDC)
   - Core 1 connects to WiFi
   - LEDs indicate status
2. USB Drive Mounts
   - Windows assigns drive letter
   - health-cdc.exe visible on drive
3. Auto-Launch (after 20 seconds)
   - HID keyboard automation triggers
   - Opens CMD and executes health-cdc.exe
4. Data Collection
   - health-cdc.exe sends JSON every 40 seconds
   - Pico W receives data via CDC serial
5. Secure Transmission
   - POST data to server duration around 40 seconds
   - mTLS authentication with ATECC608B
   - Server validates client certificate



### LED Status Indicators

GP6 (WiFi LED):
  Solid: WiFi connected
  Blinking: WiFi connecting/reconnecting
  Off: WiFi disconnected
GP7 (DNS LED):
  On: DNS resolution successful
GP8 (mTLS LED):
  On: mTLS authentication successful



## Security Features

Hardware-Based Cryptography:
- Private key stored in ATECC608B (never exposed)
- Hardware-accelerated ECC P-256 signing
- Secure key storage (cannot be extracted)
Mutual TLS (mTLS):
- Both client and server authenticate
- Certificate-based device identity
- Encrypted data transmission
Certificate Validation:
- Server certificate verification
- Date/time validation
- CA chain verification



## Known Limitations

1. Windows Only - HID automation targets Windows keyboard layout
2. 2.4GHz WiFi - Pico W does not support 5GHz networks
3. FAT32 Only - SD card must be FAT32 formatted
4. Fixed Drive Letters - HID tries D:, E:, F:, G: only
