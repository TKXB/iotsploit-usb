cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pico2_usbscpi_demo C CXX ASM)

pico_sdk_init()

set(USBSCPI_BUILD_TINYUSB_GLUE ON CACHE BOOL "" FORCE)
set(USBSCPI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. iotsploit-usb)
# The glue includes tusb.h, which needs TinyUSB and this example's tusb_config.h.
target_link_libraries(usbscpi_tinyusb_glue PUBLIC tinyusb_device)
target_include_directories(usbscpi_tinyusb_glue PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(pico2_usbscpi_demo
    main.c
    usb_descriptors.c
)

target_link_libraries(pico2_usbscpi_demo
    pico_stdlib
    pico_unique_id
    hardware_adc
    hardware_gpio
    tinyusb_device
    tinyusb_board
    usbscpi
    usbscpi_tinyusb_glue
)

target_include_directories(pico2_usbscpi_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}            # tusb_config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../glue
)

pico_add_extra_outputs(pico2_usbscpi_demo)
