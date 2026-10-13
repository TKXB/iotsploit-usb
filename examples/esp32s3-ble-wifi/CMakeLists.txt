cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.1.0")
# WLAN jobs shared with esp32s3.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/wlan")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_ble_wifi)
