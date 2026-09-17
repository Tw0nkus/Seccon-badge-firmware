# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(secconbadgefirmware C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(secconbadgefirmware
        main.c
        badge_ctf.c
        badge_files.c
        badge_hardware.c
        badge_settings.c
        badge_tunes.c
        badge_updiprog.c
        updi.c
        updi_devices.c
        updi_uart.c)

pico_set_program_name(secconbadgefirmware "badge")
pico_set_program_version(secconbadgefirmware "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(secconbadgefirmware 0)
pico_enable_stdio_usb(secconbadgefirmware 1)

# Add the standard library to the build
target_link_libraries(secconbadgefirmware
        pico_stdlib
        pico_flash
        pico_sha256
        hardware_i2c
        hardware_pio
        hardware_uart
        tinyusb_device)

target_link_options(secconbadgefirmware PRIVATE
        "LINKER:-T,${CMAKE_CURRENT_LIST_DIR}/flag_partition.ld")
set_property(TARGET secconbadgefirmware APPEND PROPERTY
        LINK_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/flag_partition.ld)

pico_generate_pio_header(secconbadgefirmware
        ${PICO_SDK_PATH}/src/rp2_common/pico_status_led/ws2812.pio)

target_compile_definitions(secconbadgefirmware PRIVATE
        PICO_ENABLE_USB_RESET_VIA_BAUD_RATE=1
        USBD_MANUFACTURER=\"Badge\"
        USBD_PRODUCT=\"Badge\")

# Add the standard include files to the build
set(BADGE_LED_COUNT 32 CACHE STRING "Number of badge LEDs (at least 16 for CTF data)")
target_compile_definitions(secconbadgefirmware PRIVATE BADGE_LED_COUNT=${BADGE_LED_COUNT})

target_include_directories(secconbadgefirmware PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(secconbadgefirmware)

