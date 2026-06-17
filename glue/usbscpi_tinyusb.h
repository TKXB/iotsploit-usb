#ifndef USBSCPI_TINYUSB_H
#define USBSCPI_TINYUSB_H

#include "usbscpi/usbscpi.h"

#ifdef __cplusplus
extern "C" {
#endif

void usbscpi_tinyusb_bind(usbscpi_t *ctx);
bool usbscpi_tinyusb_msg_data_cb(void *data, size_t len, bool eom);
int usbscpi_tinyusb_tx(void *user, const uint8_t *data, size_t len, bool eom);

#ifdef __cplusplus
}
#endif

#endif
