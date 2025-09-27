cmake_minimum_required(VERSION 3.13)

# Let VS Code extension handle SDK path
set(PICO_BOARD pico CACHE STRING "Board type")
set(PROGRAM_NAME pico_health_launcher_final)

# Pull in Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(${PROGRAM_NAME})

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Add the no-OS-FatFS-SD library from your src/src folder (nested)
set(SD_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/src")

# Check if SD library exists
if(EXISTS ${SD_LIB_DIR})
    message(STATUS "Found SD library at ${SD_LIB_DIR}")
    
    # Add the specific SD library source files that actually exist in your src/src folder
    set(SD_SOURCES
        "${SD_LIB_DIR}/crash.c"
        "${SD_LIB_DIR}/crc.c"
        "${SD_LIB_DIR}/f_util.c"
        "${SD_LIB_DIR}/FatFsSd.cpp"
        "${SD_LIB_DIR}/ff_stdio.c"
        "${SD_LIB_DIR}/file_stream.c"
        "${SD_LIB_DIR}/glue.c"
        "${SD_LIB_DIR}/my_debug.c"
        "${SD_LIB_DIR}/my_rtc.c"
        "${SD_LIB_DIR}/util.c"
    )
    
    # Create SD library
    add_library(sd_library STATIC ${SD_SOURCES})
    
    # SD library include directories
    target_include_directories(sd_library PUBLIC
        ${SD_LIB_DIR}
        ${CMAKE_CURRENT_LIST_DIR}
    )
    
    # SD library dependencies
    target_link_libraries(sd_library
        pico_stdlib
        hardware_spi
        hardware_gpio
        hardware_dma
    )
    
    # SD library compile definitions
    target_compile_definitions(sd_library PUBLIC
        SD_USE_SPI=1
        FF_USE_MKFS=1
        FF_FS_READONLY=0
        USE_PRINTF=1
        USE_DBG_PRINTF=1
    )
    
    set(HAS_SD_LIB TRUE)
else()
    message(WARNING "SD library not found at ${SD_LIB_DIR}")
    set(HAS_SD_LIB FALSE)
endif()

# Create main executable
add_executable(${PROGRAM_NAME}
    main.c
    usb_descriptors.c
    hw_config.c
    msc_disk.c
)

# Include directories for main executable
target_include_directories(${PROGRAM_NAME} PRIVATE 
    ${CMAKE_CURRENT_LIST_DIR}
)

# Add SD library includes if available
if(HAS_SD_LIB)
    target_include_directories(${PROGRAM_NAME} PRIVATE
        ${SD_LIB_DIR}
    )
endif()

# Link basic libraries
target_link_libraries(${PROGRAM_NAME} PRIVATE
    pico_stdlib
    tinyusb_device
    tinyusb_board
    hardware_gpio
    hardware_spi
)

# Link SD library if available
if(HAS_SD_LIB)
    target_link_libraries(${PROGRAM_NAME} PRIVATE sd_library)
endif()

# Compile definitions
target_compile_definitions(${PROGRAM_NAME} PRIVATE
    PICO_DEFAULT_LED_PIN=25
    USE_PRINTF=1
    USE_DBG_PRINTF=1
)

# Add SD-specific definitions if available
if(HAS_SD_LIB)
    target_compile_definitions(${PROGRAM_NAME} PRIVATE
        SD_USE_SPI=1
        FF_USE_MKFS=1
        FF_FS_READONLY=0
    )
endif()

# Compiler options
target_compile_options(${PROGRAM_NAME} PRIVATE 
    -Wall 
    -Wextra 
    -O2
)

# Enable USB output, disable UART
pico_enable_stdio_usb(${PROGRAM_NAME} 1)
pico_enable_stdio_uart(${PROGRAM_NAME} 0)

# Generate additional output files
pico_add_extra_outputs(${PROGRAM_NAME})

# Build summary
message(STATUS "=== Build Configuration ===")
message(STATUS "Program: ${PROGRAM_NAME}")
message(STATUS "Board: ${PICO_BOARD}")
message(STATUS "SD Library: ${HAS_SD_LIB}")
if(HAS_SD_LIB)
    message(STATUS "Features: Full HID + SD Card + MSC")
else()
    message(STATUS "Features: HID Keyboard only")
endif()
message(STATUS "==========================")