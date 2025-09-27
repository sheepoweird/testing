cmake_minimum_required(VERSION 3.13)

# Set PICO_SDK_PATH before including the import
set(PICO_SDK_PATH "C:/Program Files/Raspberry Pi/Pico SDK v1.5.1/pico-sdk")

# Import Pico SDK
include(pico_sdk_import.cmake)

# Initialize the SDK
pico_sdk_init()

# Initialize project AFTER pico_sdk_init()
project(combined_usb_project C CXX ASM)

# Create executable
add_executable(combined_usb_project
    src/main.c
    src/hid_test.c
    src/msd_handler.c
)

# Enable USB stdio
pico_enable_stdio_usb(combined_usb_project 1)
pico_enable_stdio_uart(combined_usb_project 0)

# Add extra outputs
pico_add_extra_outputs(combined_usb_project)

# Link libraries
target_link_libraries(combined_usb_project 
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_flash
)

# Include directories
target_include_directories(combined_usb_project PRIVATE src)

# TinyUSB configuration
target_compile_definitions(combined_usb_project PRIVATE
    CFG_TUD_MSC=1
    CFG_TUD_HID=1
    PICO_FLASH_SIZE_BYTES=2097152
)