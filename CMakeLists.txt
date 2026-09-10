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
option(USBSCPI_BUILD_SOCKET_GLUE "Build iotsploit-usb optional BSD-socket glue" OFF)

# MSVC has no /std:c99, so CMake's c_std_99 silently adds nothing there and the
# daemon's designated initializers fail to compile. /std:c11 needs VS2019 16.8+.
if(MSVC)
    set(USBSCPI_C_STD c_std_11)
    set(USBSCPI_WARNINGS /W4)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    set(USBSCPI_C_STD c_std_99)
    set(USBSCPI_WARNINGS -Wall -Wextra -Werror)
endif()

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
target_compile_features(libscpi PUBLIC ${USBSCPI_C_STD})

add_library(usbscpi STATIC
    src/usbscpi.c
)
target_link_libraries(usbscpi PUBLIC libscpi)
target_include_directories(usbscpi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(usbscpi PUBLIC ${USBSCPI_C_STD})

target_compile_options(usbscpi PRIVATE ${USBSCPI_WARNINGS})

if(USBSCPI_BUILD_HELPERS)
    add_library(usbscpi_helpers STATIC helpers/ring_buffer.c)
    target_include_directories(usbscpi_helpers PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(usbscpi_helpers PUBLIC ${USBSCPI_C_STD})
endif()

if(USBSCPI_BUILD_TINYUSB_GLUE)
    add_library(usbscpi_tinyusb_glue STATIC glue/usbscpi_tinyusb.c)
    target_link_libraries(usbscpi_tinyusb_glue PUBLIC usbscpi)
    target_compile_features(usbscpi_tinyusb_glue PUBLIC ${USBSCPI_C_STD})
endif()

if(USBSCPI_BUILD_SOCKET_GLUE)
    add_library(usbscpi_socket_glue STATIC glue/usbscpi_socket.c)
    target_link_libraries(usbscpi_socket_glue PUBLIC usbscpi)
    target_include_directories(usbscpi_socket_glue PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/glue>
    )
    target_compile_features(usbscpi_socket_glue PUBLIC ${USBSCPI_C_STD})
    target_compile_options(usbscpi_socket_glue PRIVATE ${USBSCPI_WARNINGS})
    if(WIN32)
        target_link_libraries(usbscpi_socket_glue PUBLIC ws2_32)
    endif()

    # Desktop SCPI-over-TCP daemon (Linux, Windows, Pi 5): also the
    # hardware-free test rig for the Rust host.
    add_executable(usbscpi_daemon examples/daemon/main.c)
    target_link_libraries(usbscpi_daemon PRIVATE usbscpi_socket_glue)
    target_compile_options(usbscpi_daemon PRIVATE ${USBSCPI_WARNINGS})
    set_target_properties(usbscpi_daemon PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples/daemon)
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
