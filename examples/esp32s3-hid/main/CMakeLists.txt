idf_component_register(
    SRCS "app_main.c" "duck_keymap.c" "duckscript.c" "duck_runner.c" "duck_store.c"
    INCLUDE_DIRS "."
    REQUIRES iotsploit-usb tinyusb usb esp_app_format esp_timer
             esp_wifi esp_netif esp_event nvs_flash lwip)

# Expose tusb_config.h to the TinyUSB component
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
