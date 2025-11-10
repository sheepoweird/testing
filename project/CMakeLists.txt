# == DO NEVER EDIT THE NEXT LINES for Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.0.0)
set(toolchainVersion 13_2_Rel1)
set(picotoolVersion 2.0.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13...3.27)

set(PICO_SDK_PATH "C:/Users/aydil/.pico-sdk/sdk/2.2.0")

set(PICO_BOARD pico_w CACHE STRING "Board type")

set(PROGRAM_NAME HID_MSC_CDC_LOG_WIFI_HTTPS)

# Pull in Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(${PROGRAM_NAME})

pico_sdk_init()

add_subdirectory(../src build)
add_subdirectory(../include/cryptoauthlib/lib build_cryptoauth) # ← atecc

add_executable(${PROGRAM_NAME}
  hw_config.c
  main.c
  msc_disk.c
  usb_descriptors.c
  hal_pico_i2c.c #atecc
)
target_link_libraries(${PROGRAM_NAME} PRIVATE
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
    pico_stdlib
    pico_unique_id
    tinyusb_additions
    tinyusb_board
    tinyusb_device
    pico_cyw43_arch_lwip_threadsafe_background
    pico_multicore
    pico_lwip_mbedtls
    pico_mbedtls
    hardware_i2c            # ← atecc
    cryptoauth              # ← atecc
)

target_include_directories(${PROGRAM_NAME} PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/header
  ${CMAKE_CURRENT_LIST_DIR} 
  
  ${CMAKE_CURRENT_LIST_DIR}/../include/cryptoauthlib/lib            # ← atecc
  ${CMAKE_CURRENT_LIST_DIR}/../include/cryptoauthlib/lib/hal        # ← atecc
  ${CMAKE_CURRENT_LIST_DIR}/../include/cryptoauthlib/lib/calib      # ← atecc
  ${CMAKE_CURRENT_LIST_DIR}/../include/cryptoauthlib/lib/crypto     # ← atecc
  ${CMAKE_CURRENT_LIST_DIR}/../include/cryptoauthlib/lib/atcacert   # ← atecc
)

# https://datasheets.raspberrypi.com/pico/raspberry-pi-pico-c-sdk.pdf
target_compile_definitions(${PROGRAM_NAME} PUBLIC
    PARAM_ASSERTIONS_ENABLE_ALL=1 
    PICO_MALLOC_PANIC=1
    PICO_USE_STACK_GUARDS=1

    USE_PRINTF
    USE_DBG_PRINTF
    #ateccdef
    ATCA_HAL_I2C=1              
    ATCA_ATECC608_SUPPORT=1     
    ATCA_PRINTF=1
)

#target_compile_options(${PROGRAM_NAME} PRIVATE -Werror -Wall -Wextra -Wnull-dereference)
target_compile_options(${PROGRAM_NAME} PUBLIC 
    -Wall 
    -Wextra 
    -Wshadow 
    -Wstack-usage=2048 
    -fanalyzer 
)

target_link_options(${PROGRAM_NAME} PRIVATE -Wl,--print-memory-usage)

pico_enable_stdio_usb(${PROGRAM_NAME} 1)
pico_add_extra_outputs(${PROGRAM_NAME})
