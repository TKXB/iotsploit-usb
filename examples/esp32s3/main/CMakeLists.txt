idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES iotsploit-usb tinyusb driver esp_adc usb)

# Expose tusb_config.h to the TinyUSB component
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
