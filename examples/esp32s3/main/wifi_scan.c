#include "wifi_scan.h"

#include <stdio.h>
#include <string.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#define WIFI_SCAN_MAX_AP 20

static wifi_ap_record_t s_aps[WIFI_SCAN_MAX_AP];
static volatile size_t  s_ap_count;
static volatile int     s_done;     /* set in event ctx, read in USB ctx */

/* Runs in the default event-loop task once the driver finishes scanning. */
static void scan_done_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    uint16_t num = WIFI_SCAN_MAX_AP;
    if (esp_wifi_scan_get_ap_records(&num, s_aps) != ESP_OK) {
        num = 0;
    }
    s_ap_count = num;
    s_done = 1;
}

void wifi_scan_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                        &scan_done_handler, NULL, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
}

int wifi_scan_start(void) {
    s_done = 0;
    s_ap_count = 0;
    wifi_scan_config_t cfg = { 0 };   /* all channels, active scan */
    return esp_wifi_scan_start(&cfg, false) == ESP_OK ? 0 : -1;
}

int wifi_scan_done(void) {
    return s_done;
}

size_t wifi_scan_count(void) {
    return s_ap_count;
}

int wifi_scan_get(size_t index, char *out, size_t out_len) {
    if (index >= s_ap_count || !out) {
        return -1;
    }
    const wifi_ap_record_t *ap = &s_aps[index];
    const uint8_t *b = ap->bssid;
    snprintf(out, out_len,
             "\"%s\",%d,%u,%u,%02X:%02X:%02X:%02X:%02X:%02X",
             (const char *)ap->ssid, ap->rssi, ap->primary, ap->authmode,
             b[0], b[1], b[2], b[3], b[4], b[5]);
    return 0;
}
