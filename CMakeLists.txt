cmake_minimum_required(VERSION 3.13)

# Initialize project
project(combined_usb_project C CXX ASM)

# Import Pico SDK
include(pico_sdk_import.cmake)

pico_sdk_init()

# Add TinyUSB and FatFS (they come with Pico SDK)
set(PICO_EXTRAS_PATH ${CMAKE_SOURCE_DIR}/lib/pico_extras CACHE STRING "Path to Pico Extras")
include_directory(${PICO_SDK_PATH}/lib/tinyusb/src)
include_directory(${PICO_SDK_PATH}/lib/fatfs)

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
    hardware_pio
    hardware_dma
    pico_multicore
    hardware_flash
)

# Include directories
target_include_directories(combined_usb_project PRIVATE src)

# Compile definitions
target_compile_definitions(combined_usb_project PRIVATE
    CFG_TUD_MSC=1
    CFG_TUD_HID=1
)