#include "usbscpi_tinyusb.h"

#include "tusb.h"

static usbscpi_t *g_usbscpi;

void usbscpi_tinyusb_bind(usbscpi_t *ctx) {
    g_usbscpi = ctx;
}

bool usbscpi_tinyusb_msg_data_cb(void *data, size_t len, bool eom) {
    if (!g_usbscpi) {
        return false;
    }
    return usbscpi_on_rx(g_usbscpi, data, len, eom) == USBSCPI_OK;
}

int usbscpi_tinyusb_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user;
    return tud_usbtmc_transmit_dev_msg_data(data, len, eom, false) ? 0 : -1;
}

bool tud_usbtmc_msg_data_cb(void *data, size_t len, bool transfer_complete) {
    return usbscpi_tinyusb_msg_data_cb(data, len, transfer_complete);
}

void tud_usbtmc_bulkOut_clearFeature_cb(void) {
    if (g_usbscpi) {
        usbscpi_clear(g_usbscpi);
    }
}
