#include "wifi_scan.h"

#include <stdio.h>
#include <string.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "wifi";

#define WIFI_SCAN_MAX_AP 20

static wifi_ap_record_t s_aps[WIFI_SCAN_MAX_AP];
static volatile size_t  s_ap_count;
static volatile int     s_done;     /* set in event ctx, read in USB ctx */

/* STA association state. Written in the event-loop task, read from SCPI ctx. */
static volatile int s_wifi_ready;   /* wifi_scan_init() has finished */
static volatile int s_sta_has_ip;
static char         s_sta_ip[16];
static int          s_sta_want_connect;

/* Runs in the default event-loop task once the driver finishes scanning. */
static void scan_done_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    uint16_t num = WIFI_SCAN_MAX_AP;
    if (esp_wifi_scan_get_ap_records(&num, s_aps) != ESP_OK) {
        ESP_LOGE(TAG, "scan done: get_ap_records failed");
        num = 0;
    }
    s_ap_count = num;
    s_done = 1;
    ESP_LOGI(TAG, "wifi scan done: %u APs", (unsigned)num);
}

/* Keep the association up: the driver does not retry on its own, and a
 * WIFI:SCAN sweeps every channel and will knock the station off. */
static void sta_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_has_ip = 0;
        if (s_sta_want_connect) {
            esp_wifi_connect();
        }
    }
}

static void got_ip_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data) {
    (void)arg; (void)base; (void)id;
    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
    s_sta_has_ip = 1;
    ESP_LOGI(TAG, "sta got ip %s", s_sta_ip);
}

int wifi_sta_connect(const char *ssid, const char *password) {
    if (!ssid || !ssid[0]) {
        return -1;
    }
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wc.sta.password, password, sizeof(wc.sta.password) - 1);
    }

    s_sta_want_connect = 1;
    s_sta_has_ip = 0;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sta set_config failed: 0x%x", (unsigned)err);
        return -1;
    }
    ESP_LOGI(TAG, "sta connecting to \"%s\"", ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "sta connect failed: 0x%x", (unsigned)err);
        return -1;
    }
    return 0;
}

int wifi_sta_has_ip(void) {
    return s_sta_has_ip;
}

int wifi_sta_ip(char *out, size_t out_len) {
    if (!out || !out_len || !s_sta_has_ip) {
        return -1;
    }
    snprintf(out, out_len, "%s", s_sta_ip);
    return 0;
}

void wifi_scan_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs init err=0x%x, erasing and re-init", (unsigned)err);
        nvs_flash_erase();
        nvs_flash_init();
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: 0x%x", (unsigned)err);
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: 0x%x", (unsigned)err);
    }
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                        &scan_done_handler, NULL, NULL);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                        &sta_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &got_ip_handler, NULL, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    s_wifi_ready = 1;
    ESP_LOGI(TAG, "wifi STA started, ready");
}

bool wifi_scan_ready(void) {
    return s_wifi_ready != 0;
}

int wifi_scan_start(void) {
    s_done = 0;
    s_ap_count = 0;
    wifi_scan_config_t cfg = { 0 };   /* all channels, active scan */
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi scan start failed: 0x%x", (unsigned)err);
        return -1;
    }
    ESP_LOGI(TAG, "wifi scan start");
    return 0;
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
