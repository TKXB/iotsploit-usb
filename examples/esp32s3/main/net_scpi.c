#include "net_scpi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "usbscpi_socket.h"
#include "wifi_scan.h"

static const char *TAG = "netscpi";

#define NET_SCPI_PORT 5025

/* Storage for the TCP context. Separate from the USB context's buffers by
 * necessity, not by preference — see net_scpi.h. */
static uint8_t s_net_storage[2048];
static char    s_net_line[256];
static uint8_t s_net_io[4096];

static usbscpi_config_t     s_net_cfg;
static usbscpi_t           *s_net_dev;
static const scpi_command_t *s_net_commands;

static char s_ssid[33];
static char s_pass[65];

static void net_scpi_task(void *arg) {
    (void)arg;

    /* wifi_scan_init() owns esp_wifi_init(); it runs from scan_init_task, so
     * wait for it rather than racing it. */
    while (!wifi_scan_ready()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (wifi_sta_connect(s_ssid, s_pass) != 0) {
        ESP_LOGE(TAG, "sta connect request failed; not starting listener");
        vTaskDelete(NULL);
        return;
    }

    char ip[16] = {0};
    while (wifi_sta_ip(ip, sizeof(ip)) != 0) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGI(TAG, "SCPI/TCP listening on %s:%d", ip, NET_SCPI_PORT);

    /* Binds every interface: on a station that is the one Wi-Fi netif. The
     * argument is explicit rather than defaulted so the exposure is visible. */
    if (usbscpi_socket_serve(s_net_dev, "0.0.0.0", NET_SCPI_PORT) != 0) {
        ESP_LOGE(TAG, "listen on port %d failed", NET_SCPI_PORT);
    }
    vTaskDelete(NULL);
}

int net_scpi_start(const usbscpi_config_t *tmpl,
                   const scpi_command_t *commands,
                   const char *ssid,
                   const char *password) {
    if (!tmpl || !ssid) {
        return -1;
    }

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", password ? password : "");
    s_net_commands = commands;

    /* Everything device-specific comes from the USB context's config; only the
     * transport-specific fields differ. */
    s_net_cfg = *tmpl;
    s_net_cfg.usb_tx       = usbscpi_socket_tx;
    s_net_cfg.line_buf     = s_net_line;
    s_net_cfg.line_buf_len = sizeof(s_net_line);
    s_net_cfg.io_buf       = s_net_io;
    s_net_cfg.io_buf_len   = sizeof(s_net_io);
    /* A socket is not limited to a USB endpoint's 64/512 bytes. This caps
     * SYSTem:HELP:HEADers? and block reads, so the USB context's 256 would
     * needlessly truncate them here. */
    s_net_cfg.mtu          = 4096;

    s_net_dev = usbscpi_init(s_net_storage, sizeof(s_net_storage), &s_net_cfg);
    if (!s_net_dev) {
        ESP_LOGE(TAG, "usbscpi_init failed for the TCP context");
        return -1;
    }
    if (s_net_commands && usbscpi_register(s_net_dev, s_net_commands) != USBSCPI_OK) {
        ESP_LOGE(TAG, "usbscpi_register failed for the TCP context");
        return -1;
    }

    if (xTaskCreate(net_scpi_task, "net_scpi", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "net_scpi task creation failed");
        return -1;
    }
    return 0;
}
