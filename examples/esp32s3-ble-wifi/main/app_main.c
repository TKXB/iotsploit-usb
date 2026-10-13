#include <stdio.h>
#include <stdbool.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_mac.h"

#include "usbscpi/usbscpi.h"
#include "usbscpi_nimble.h"
#include "wifi_scan.h"
#include "wlan_scpi.h"

#define BLE_NAME "iotsploit-wifi"

/* The BLE context's buffers. The descriptor is emitted whole into io_buf, so
 * it is sized for SYSTem:HELP:DESCription? like the TCP context's. */
static uint8_t s_storage[4096];
static char    s_line[512];
static uint8_t s_io[8192];

/* BLE is the only control link and does not ride on Wi-Fi: coexistence keeps
 * it up through a scan or an association, so every WLAN job is allowed. */
bool wlan_scpi_allowed(scpi_t *ctx, bool scan) {
    (void)ctx;
    (void)scan;
    return true;
}

USBSCPI_DEFINE_COMMANDS(app, WLAN_COMMANDS);

static const usbscpi_workflow_desc_t s_workflows[] = {
    WLAN_WORKFLOWS,
};

static const usbscpi_descriptor_t s_descriptor = {
    .commands = app_desc_commands,
    .command_count = USBSCPI_COUNT(app_desc_commands),
    .workflows = s_workflows,
    .workflow_count = USBSCPI_COUNT(s_workflows),
};

void app_main(void) {
    /* NVS first: NimBLE keeps its bonds there. */
    wifi_scan_init();

    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);
    static char idn[80];
    snprintf(idn, sizeof(idn), "IoTSploit,ESP32S3-BLE-WIFI,%02X%02X%02X%02X%02X%02X,%s",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             esp_app_get_description()->version);

    usbscpi_config_t cfg = {
        .usb_tx        = usbscpi_nimble_tx,
        .line_buf      = s_line,
        .line_buf_len  = sizeof(s_line),
        .max_block_len = sizeof(s_io),
        .idn           = idn,
        .io_buf        = s_io,
        .io_buf_len    = sizeof(s_io),
        .proto         = 1,
        /* A logical reply limit, not the ATT MTU: the glue fragments. */
        .mtu           = sizeof(s_io),
        .descriptor    = &s_descriptor,
    };
    usbscpi_t *dev = usbscpi_init(s_storage, sizeof(s_storage), &cfg);
    ESP_ERROR_CHECK(dev ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(usbscpi_register(dev, app_scpi_commands) == USBSCPI_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(usbscpi_nimble_start(dev, BLE_NAME) == 0 ? ESP_OK : ESP_FAIL);
}
