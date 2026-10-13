# Wi-Fi scan/association backend and its WLAN:* SCPI jobs, shared by the
# ESP32-S3 examples. The example defines wlan_scpi_allowed() (wlan_scpi.h).
idf_component_register(
    SRCS "wifi_scan.c" "wlan_scpi.c"
    INCLUDE_DIRS "."
    REQUIRES iotsploit-usb esp_wifi esp_netif esp_event esp_timer nvs_flash)
