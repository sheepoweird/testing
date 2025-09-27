cmake_minimum_required(VERSION 3.13)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(pico_hid_test)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# rest of your project
add_executable(pico_hid_test
    main.c
    usb_descriptors.c
)

# Make sure TinyUSB can find tusb_config.h
target_include_directories(pico_hid_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# pull in common dependencies
target_link_libraries(pico_hid_test 
    pico_stdlib
    tinyusb_device 
    tinyusb_board
)

# enable usb output, disable uart output
pico_enable_stdio_usb(pico_hid_test 1)
pico_enable_stdio_uart(pico_hid_test 0)

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(pico_hid_test)