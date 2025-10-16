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

set(PICO_SDK_PATH "C:/Users/piers/pico-sdk")

# Pull in Pico SDK (must be before project)
include(pico_sdk_import.cmake)

set(PICO_BOARD pico_w CACHE STRING "Board type")

# Set name of the program
set(PROGRAM_NAME HID_MSC_CDC_LOG_WIFI)
project(${PROGRAM_NAME})

pico_sdk_init()

add_subdirectory(../src build)

add_executable(${PROGRAM_NAME}
  hw_config.c
  main.c
  msc_disk.c
  usb_descriptors.c
)

target_link_libraries(${PROGRAM_NAME} PRIVATE
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
    pico_stdlib
    pico_unique_id
    tinyusb_additions
    tinyusb_board
    tinyusb_device
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mbedtls
    pico_mbedtls
    pico_multicore
)

target_include_directories(${PROGRAM_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR} 
    ${CMAKE_CURRENT_LIST_DIR}/include
)

target_compile_definitions(${PROGRAM_NAME} PUBLIC
    PARAM_ASSERTIONS_ENABLE_ALL=1 
    PICO_MALLOC_PANIC=1
    PICO_USE_STACK_GUARDS=1
    SD_USE_SPI=1
    USE_PRINTF
    USE_DBG_PRINTF
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000
)

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