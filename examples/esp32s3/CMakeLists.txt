cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "1.2.3")
# WLAN jobs shared with esp32s3-ble-wifi.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/wlan")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_usbscpi_demo)
