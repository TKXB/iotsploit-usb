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

    # Data plane: payload-agnostic record stream, device -> host.
    add_library(usbscpi_stream_glue STATIC glue/usbscpi_stream.c)
    target_link_libraries(usbscpi_stream_glue PUBLIC usbscpi_helpers)
    target_include_directories(usbscpi_stream_glue PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/glue>
    )
    target_compile_features(usbscpi_stream_glue PUBLIC ${USBSCPI_C_STD})
    target_compile_options(usbscpi_stream_glue PRIVATE ${USBSCPI_WARNINGS})
    if(WIN32)
        target_link_libraries(usbscpi_stream_glue PUBLIC ws2_32)
    endif()

    # Desktop SCPI-over-TCP daemon (Linux, Windows, Pi 5): also the
    # hardware-free test rig for the Rust host.
    add_executable(usbscpi_daemon examples/daemon/main.c)
    target_link_libraries(usbscpi_daemon PRIVATE usbscpi_socket_glue)
    target_compile_options(usbscpi_daemon PRIVATE ${USBSCPI_WARNINGS})
    set_target_properties(usbscpi_daemon PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples/daemon)

    # First protocol-specific consumer of the data plane. Linux only: SocketCAN
    # has no equivalent elsewhere. Everything CAN-shaped lives here, never in
    # the glue.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Threads QUIET)
        if(Threads_FOUND)
            add_executable(usbscpi_can examples/can/main.c)
            target_link_libraries(usbscpi_can
                PRIVATE usbscpi_socket_glue usbscpi_stream_glue Threads::Threads)
            target_compile_options(usbscpi_can PRIVATE ${USBSCPI_WARNINGS})
            set_target_properties(usbscpi_can PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples/can)

            # Needs a vcan interface and can-utils; the script skips cleanly
            # when the rig has neither, rather than failing.
            find_package(Python3 COMPONENTS Interpreter QUIET)
            if(USBSCPI_BUILD_TESTS AND Python3_FOUND AND NOT CMAKE_CROSSCOMPILING)
                add_test(NAME test_can_capture
                    COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_can_capture.py
                            $<TARGET_FILE:usbscpi_can>)
                set_tests_properties(test_can_capture PROPERTIES TIMEOUT 180)
            endif()
        endif()
    endif()

    # Synthetic producer for the data plane. No CAN, no hardware: if the glue
    # ever needs protocol knowledge, this stops being able to drive it.
    if(USBSCPI_BUILD_TESTS)
        find_package(Threads QUIET)
        if(Threads_FOUND OR WIN32)
            add_executable(stream_testgen tests/stream_testgen.c)
            target_link_libraries(stream_testgen PRIVATE usbscpi_stream_glue)
            if(NOT WIN32)
                target_link_libraries(stream_testgen PRIVATE Threads::Threads)
            endif()
            target_compile_options(stream_testgen PRIVATE ${USBSCPI_WARNINGS})

            find_package(Python3 COMPONENTS Interpreter QUIET)
            if(Python3_FOUND AND NOT CMAKE_CROSSCOMPILING)
                add_test(NAME test_stream_dataplane
                    COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stream_dataplane.py
                            $<TARGET_FILE:stream_testgen>)
                # Backpressure and idle-CPU assertions need wall-clock room.
                set_tests_properties(test_stream_dataplane PROPERTIES TIMEOUT 120)
            endif()
        endif()
    endif()

    # Protocol smoke suite: real sockets, real response framing, real
    # reconnects — transport behaviour the C unit tests cannot reach. It
    # spawns the daemon itself on a free port. Skipped without Python.
    # CMAKE_CROSSCOMPILING guards the case where the daemon cannot run here.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(USBSCPI_BUILD_TESTS AND Python3_FOUND AND NOT CMAKE_CROSSCOMPILING)
        add_test(NAME test_scpi_tcp_smoke
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/scpi_tcp_smoke.py
                    --daemon $<TARGET_FILE:usbscpi_daemon>)
    endif()
endif()

if(USBSCPI_BUILD_TESTS)
    enable_testing()
    add_executable(test_usbscpi tests/test_usbscpi.c)
    target_link_libraries(test_usbscpi PRIVATE usbscpi usbscpi_helpers)
    add_test(NAME test_usbscpi COMMAND test_usbscpi)

    # Host test for the TinyUSB glue, built against a minimal TinyUSB stub
    # (tests/stub/tusb.h). Regression coverage for the USBTMC IN-response
    # sequencing / -110 fix; no real TinyUSB required.
    # SPSC ring across a real thread boundary. Guards the acquire/release
    # pairing in helpers/ring_buffer.c, which had never run cross-thread.
    # POSIX threads only; the ring's users on Windows are single-threaded today.
    if(NOT WIN32)
        find_package(Threads QUIET)
        if(Threads_FOUND)
            add_executable(test_ring_concurrent tests/test_ring_concurrent.c)
            target_link_libraries(test_ring_concurrent
                PRIVATE usbscpi_helpers Threads::Threads)
            add_test(NAME test_ring_concurrent COMMAND test_ring_concurrent)
        endif()
    endif()

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
