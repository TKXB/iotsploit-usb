#include "ble_scpi.h"

#include "esp_log.h"

#include "ble_spp_security.h"

static const char *TAG = "blescpi";

/* Storage for the BLE context. Separate from the USB context's buffers by
 * necessity, not by preference — see ble_scpi.h. */
static uint8_t s_ble_storage[4096];
static char    s_ble_line[512];
/* Sized for SYSTem:HELP:DESCription?, which is emitted whole into this buffer
 * and fails outright when it does not fit, like the TCP context in the esp32s3
 * example. */
static uint8_t s_ble_io[8192];

static usbscpi_config_t s_ble_cfg;
static usbscpi_t       *s_ble_dev;

int ble_scpi_start(const usbscpi_config_t *tmpl,
                   const scpi_command_t *commands) {
    if (!tmpl) {
        return -1;
    }

    /* Everything device-specific comes from the USB context's config; only the
     * transport-specific fields differ. */
    s_ble_cfg = *tmpl;
    s_ble_cfg.usb_tx       = ble_spp_scpi_tx;
    s_ble_cfg.line_buf     = s_ble_line;
    s_ble_cfg.line_buf_len = sizeof(s_ble_line);
    s_ble_cfg.io_buf       = s_ble_io;
    s_ble_cfg.io_buf_len   = sizeof(s_ble_io);
    /* ble_spp_scpi_tx() chunks replies to the negotiated ATT MTU, so this only
     * caps SYSTem:HELP:HEADers? and block reads; the USB context's 256 would
     * needlessly truncate them over a link that reassembles a byte stream. */
    s_ble_cfg.mtu          = 8192;

    s_ble_dev = usbscpi_init(s_ble_storage, sizeof(s_ble_storage), &s_ble_cfg);
    if (!s_ble_dev) {
        ESP_LOGE(TAG, "usbscpi_init failed for the BLE context");
        return -1;
    }
    if (commands && usbscpi_register(s_ble_dev, commands) != USBSCPI_OK) {
        ESP_LOGE(TAG, "usbscpi_register failed for the BLE context");
        return -1;
    }

    ble_spp_scpi_bind(s_ble_dev);
    ESP_LOGI(TAG, "SCPI over BLE bound to the SPP characteristic");
    return 0;
}
