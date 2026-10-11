#include "wifi_scan.h"

#include <stdio.h>
#include <string.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "wifi";

#define WIFI_SCAN_MAX_AP 20

static wifi_ap_record_t s_aps[WIFI_SCAN_MAX_AP];
static volatile size_t  s_ap_count;
static volatile int     s_done;     /* set in event ctx, read in USB ctx */

/* SCPI, Wi-Fi events and the deadline timer share this owner. */
static volatile int s_wifi_ready;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static bool s_scan_running;
static wifi_config_t s_sta_config;
static wifi_sta_result_t s_result = { .state = "IDLE", .result = "NONE" };
static const char *s_after_disconnect;
static int64_t s_deadline;
enum { STA_OFFLINE, STA_SWITCHING, STA_CONNECTING, STA_DHCP, STA_ONLINE, STA_DRAINING };
static int s_phase;

static void finish(const char *result) {
    s_phase = STA_OFFLINE;
    s_result.state = strcmp(result, "STOPPED") == 0 ? "IDLE" : "FAILED";
    s_result.result = result;
    s_result.ip[0] = 0;
    s_deadline = 0;
    if (strcmp(result, "STOPPED") == 0) s_result.reason = 0;
}

static void begin_connect(void) {
    s_phase = STA_CONNECTING;
    s_result.state = "RUNNING";
    s_result.result = "NONE";
    s_result.reason = 0;
    s_result.ip[0] = 0;
    s_deadline = esp_timer_get_time() + 15000000;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_result.reason = err;
        finish(err == ESP_ERR_WIFI_PASSWORD ? "AUTH_FAILED" : "ERROR");
    }
}

/* STA_STOP acknowledges cancellation even before association. Restart only
 * after that event has drained the old connection from the Wi-Fi owner. */
static int disconnect_for(const char *result) {
    s_after_disconnect = result;
    s_phase = STA_DRAINING;
    s_result.state = "RUNNING";
    s_result.ip[0] = 0;
    s_deadline = 0;
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        s_result.reason = err;
        finish("ERROR");
        return -1;
    }
    return 0;
}

static void deadline_handler(void *arg) {
    (void)arg;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_deadline && esp_timer_get_time() >= s_deadline) {
        disconnect_for(s_phase == STA_DHCP ? "DHCP_TIMEOUT" : "TIMEOUT");
    }
    xSemaphoreGive(s_mutex);
}

static void scan_done_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint16_t num = WIFI_SCAN_MAX_AP;
    if (esp_wifi_scan_get_ap_records(&num, s_aps) != ESP_OK) num = 0;
    s_ap_count = num;
    s_done = 1;
    s_scan_running = false;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "wifi scan done: %u APs", (unsigned)num);
}

static void sta_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data) {
    (void)arg; (void)base;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (id == WIFI_EVENT_STA_STOP &&
        (s_phase == STA_SWITCHING || s_phase == STA_DRAINING)) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) {
            s_result.reason = err;
            finish("ERROR");
        } else if (s_phase == STA_SWITCHING) begin_connect();
        else finish(s_after_disconnect);
    } else if (id == WIFI_EVENT_STA_CONNECTED && s_phase == STA_CONNECTING) {
        s_phase = STA_DHCP;
        s_deadline = esp_timer_get_time() + 15000000;
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = data;
        s_result.ip[0] = 0;
        if (s_phase == STA_ONLINE) {
            /* A successful credential is reused once on link loss. A failed
             * reconnect is terminal; never cycle the host's password list. */
            begin_connect();
        } else if (s_phase == STA_CONNECTING || s_phase == STA_DHCP) {
            s_result.reason = e->reason;
            const char *result = "DISCONNECTED";
            if (e->reason == WIFI_REASON_NO_AP_FOUND) result = "AP_NOT_FOUND";
            else if (e->reason == WIFI_REASON_AUTH_FAIL ||
                     e->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                     e->reason == WIFI_REASON_HANDSHAKE_TIMEOUT) result = "AUTH_FAILED";
            finish(result);
            ESP_LOGW(TAG, "sta attempt failed: reason=%u", (unsigned)e->reason);
        }
    }
    xSemaphoreGive(s_mutex);
}

static void got_ip_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data) {
    (void)arg; (void)base;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (id == IP_EVENT_STA_GOT_IP && s_phase == STA_DHCP) {
        const ip_event_got_ip_t *e = data;
        snprintf(s_result.ip, sizeof(s_result.ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_phase = STA_ONLINE;
        s_result.state = "DONE";
        s_result.result = "CONNECTED";
        s_result.reason = 0;
        s_deadline = 0;
        ESP_LOGI(TAG, "sta got ip %s", s_result.ip);
    } else if (id == IP_EVENT_STA_LOST_IP && s_phase == STA_ONLINE) {
        s_phase = STA_DHCP;
        s_result.state = "RUNNING";
        s_result.result = "NONE";
        s_result.ip[0] = 0;
        s_deadline = esp_timer_get_time() + 15000000;
    }
    xSemaphoreGive(s_mutex);
}

int wifi_sta_connect(const char *ssid, const char *password) {
    if (!s_wifi_ready || !ssid || !ssid[0] || strlen(ssid) > 32 ||
        !password || strlen(password) > 63) return -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_scan_running || (s_phase != STA_OFFLINE && s_phase != STA_ONLINE)) {
        xSemaphoreGive(s_mutex);
        return -2;
    }
    memset(&s_sta_config, 0, sizeof(s_sta_config));
    memcpy(s_sta_config.sta.ssid, ssid, strlen(ssid));
    memcpy(s_sta_config.sta.password, password, strlen(password));
    s_result.state = "RUNNING";
    s_result.result = "NONE";
    s_result.reason = 0;
    s_result.ip[0] = 0;
    if (s_phase == STA_ONLINE) {
        s_phase = STA_SWITCHING;
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK) {
            s_result.reason = err;
            finish("ERROR");
        }
    } else begin_connect();
    xSemaphoreGive(s_mutex);
    return 0;
}

int wifi_sta_stop(void) {
    if (!s_wifi_ready) return -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int rc = 0;
    if (s_phase == STA_OFFLINE) finish("STOPPED");
    else if (s_phase == STA_DRAINING) s_after_disconnect = "STOPPED";
    else if (s_phase == STA_SWITCHING) {
        s_phase = STA_DRAINING;
        s_after_disconnect = "STOPPED";
    } else rc = disconnect_for("STOPPED");
    xSemaphoreGive(s_mutex);
    return rc;
}

int wifi_sta_clear(void) {
    if (!s_wifi_ready) return -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int rc = s_phase == STA_OFFLINE ? 0 : -2;
    if (!rc) {
        s_result.state = "IDLE";
        s_result.result = "NONE";
        s_result.reason = 0;
        s_result.ip[0] = 0;
    }
    xSemaphoreGive(s_mutex);
    return rc;
}

void wifi_sta_result(wifi_sta_result_t *out) {
    if (!s_wifi_ready) {
        *out = (wifi_sta_result_t){ .state = "IDLE", .result = "NONE" };
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_result;
    xSemaphoreGive(s_mutex);
}

int wifi_sta_ip(char *out, size_t out_len) {
    wifi_sta_result_t result;
    wifi_sta_result(&result);
    if (!out || out_len < sizeof(result.ip) || !result.ip[0]) return -1;
    memcpy(out, result.ip, sizeof(result.ip));
    return 0;
}

void wifi_scan_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                                       &scan_done_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                       &sta_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_STOP,
                                                       &sta_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                                       &sta_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       &got_ip_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                                       &got_ip_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    const esp_timer_create_args_t timer_args = { .callback = deadline_handler, .name = "wifi_attempt" };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 250000));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_ready = 1;
    ESP_LOGI(TAG, "wifi STA started, ready");
}

bool wifi_scan_ready(void) {
    return s_wifi_ready != 0;
}

int wifi_scan_start(void) {
    if (!s_wifi_ready) return -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_scan_running || (s_phase != STA_OFFLINE && s_phase != STA_ONLINE)) {
        xSemaphoreGive(s_mutex);
        return -2;
    }
    s_done = 0;
    s_ap_count = 0;
    wifi_scan_config_t cfg = { 0 };   /* all channels, active scan */
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "wifi scan start failed: 0x%x", (unsigned)err);
        return -1;
    }
    s_scan_running = true;
    xSemaphoreGive(s_mutex);
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
