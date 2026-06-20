cmake_minimum_required(VERSION 3.16)

if(DEFINED IDF_TARGET)
    file(GLOB LIBSCPI_SRCS "third_party/libscpi/src/*.c")
    idf_component_register(
        SRCS "src/usbscpi.c" ${LIBSCPI_SRCS}
        INCLUDE_DIRS "include" "third_party/libscpi/inc"
        PRIV_INCLUDE_DIRS "third_party/libscpi/src"
    )
    target_compile_definitions(${COMPONENT_LIB} PUBLIC SCPI_USER_CONFIG=1)
    return()
endif()

project(iotsploit_usb C)

option(USBSCPI_BUILD_TESTS "Build iotsploit-usb host tests" ON)
option(USBSCPI_BUILD_HELPERS "Build iotsploit-usb optional helper library" ON)
option(USBSCPI_BUILD_TINYUSB_GLUE "Build iotsploit-usb optional TinyUSB glue" OFF)

# Vendored libscpi: its own static lib so third-party warnings don't trip our
# -Werror; SCPI_USER_CONFIG activates third_party/libscpi/inc/scpi/scpi_user_config.h.
file(GLOB LIBSCPI_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libscpi/src/*.c)
add_library(libscpi STATIC ${LIBSCPI_SRCS})
target_include_directories(libscpi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/libscpi/inc>
)
target_include_directories(libscpi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libscpi/src
)
target_compile_definitions(libscpi PUBLIC SCPI_USER_CONFIG=1)
target_compile_features(libscpi PUBLIC c_std_99)

add_library(usbscpi STATIC
    src/usbscpi.c
)
target_link_libraries(usbscpi PUBLIC libscpi)
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
