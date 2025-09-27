cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(combined_usb_project C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(combined_usb_project
    src/main.c
    src/hid_handler.c
    src/msd_handler.c
)

pico_enable_stdio_usb(combined_usb_project 1)
pico_enable_stdio_uart(combined_usb_project 0)

pico_add_extra_outputs(combined_usb_project)

target_include_directories(combined_usb_project PRIVATE src)

target_link_libraries(combined_usb_project
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_flash
)

target_compile_definitions(combined_usb_project 
    PRIVATE
    PICO_FLASH_SIZE_BYTES=2097152
)