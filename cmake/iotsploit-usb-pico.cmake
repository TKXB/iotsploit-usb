# Include this file from a Pico SDK project after pico_sdk_init().
#
#   include(path/to/iotsploit-usb/cmake/iotsploit-usb-pico.cmake)
#   target_link_libraries(your_firmware PRIVATE usbscpi)
#
get_filename_component(IOTSPLOIT_USB_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_subdirectory("${IOTSPLOIT_USB_ROOT}" "${CMAKE_BINARY_DIR}/iotsploit-usb")
