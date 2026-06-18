cmake_minimum_required(VERSION 3.16)

if(DEFINED IDF_TARGET)
    idf_component_register(
        SRCS "src/usbscpi.c" "src/scpi_compat.c"
        INCLUDE_DIRS "include"
    )
    return()
endif()

project(iotsploit_usb C)

option(USBSCPI_BUILD_TESTS "Build iotsploit-usb host tests" ON)
option(USBSCPI_BUILD_HELPERS "Build iotsploit-usb optional helper library" ON)
option(USBSCPI_BUILD_TINYUSB_GLUE "Build iotsploit-usb optional TinyUSB glue" OFF)

add_library(usbscpi STATIC
    src/usbscpi.c
    src/scpi_compat.c
)
target_include_directories(usbscpi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(usbscpi PUBLIC c_std_99)

if(MSVC)
    target_compile_options(usbscpi PRIVATE /W4)
else()
    target_compile_options(usbscpi PRIVATE -Wall -Wextra -Werror)
endif()

if(USBSCPI_BUILD_HELPERS)
    add_library(usbscpi_helpers STATIC helpers/ring_buffer.c)
    target_include_directories(usbscpi_helpers PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(usbscpi_helpers PUBLIC c_std_99)
endif()

if(USBSCPI_BUILD_TINYUSB_GLUE)
    add_library(usbscpi_tinyusb_glue STATIC glue/usbscpi_tinyusb.c)
    target_link_libraries(usbscpi_tinyusb_glue PUBLIC usbscpi)
    target_compile_features(usbscpi_tinyusb_glue PUBLIC c_std_99)
endif()

if(USBSCPI_BUILD_TESTS)
    enable_testing()
    add_executable(test_usbscpi tests/test_usbscpi.c)
    target_link_libraries(test_usbscpi PRIVATE usbscpi usbscpi_helpers)
    add_test(NAME test_usbscpi COMMAND test_usbscpi)

    # Host test for the TinyUSB glue, built against a minimal TinyUSB stub
    # (tests/stub/tusb.h). Regression coverage for the USBTMC IN-response
    # sequencing / -110 fix; no real TinyUSB required.
    add_executable(test_glue_tinyusb
        tests/test_glue_tinyusb.c
        glue/usbscpi_tinyusb.c
    )
    target_include_directories(test_glue_tinyusb PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/glue
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/stub
    )
    target_link_libraries(test_glue_tinyusb PRIVATE usbscpi)
    add_test(NAME test_glue_tinyusb COMMAND test_glue_tinyusb)
endif()
