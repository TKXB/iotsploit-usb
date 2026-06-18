#ifndef USBSCPI_TINYUSB_H
#define USBSCPI_TINYUSB_H

#include "usbscpi/usbscpi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TinyUSB glue for iotsploit-usb.
 *
 * This glue owns the *entire* USBTMC bulk-IN response path so that integrators
 * do not have to (and must NOT) re-implement it. The component produces SCPI
 * responses synchronously while handling a bulk-OUT message, but TinyUSB only
 * permits sending bulk-IN data after the host issues REQUEST_DEV_DEP_MSG_IN
 * (i.e. when the class is in STATE_TX_REQUESTED). Sending the reply directly
 * from the OUT path makes tud_usbtmc_transmit_dev_msg_data() fail its
 * TU_VERIFY(state == STATE_TX_REQUESTED) guard, the reply is dropped, and every
 * query read times out (-110).
 *
 * Fix: usbscpi_tinyusb_tx() buffers the response and raises the MAV status bit;
 * the buffered response is actually transmitted from
 * tud_usbtmc_msgBulkIn_request_cb(), which the glue defines.
 *
 * Integrators using this glue MUST NOT also define any of:
 *   tud_usbtmc_msg_data_cb, tud_usbtmc_bulkOut_clearFeature_cb,
 *   tud_usbtmc_bulkIn_clearFeature_cb,
 *   tud_usbtmc_msgBulkIn_request_cb, tud_usbtmc_msgBulkIn_complete_cb,
 *   tud_usbtmc_get_stb_cb
 * (they are provided by the glue; duplicate definitions break linking).
 */

/* Bind the global usbscpi instance the glue drives. Call once after init. */
void usbscpi_tinyusb_bind(usbscpi_t *ctx);

/* Use this as usbscpi_config_t.usb_tx. Buffers the response and raises MAV; it
 * does NOT transmit immediately. Returns 0 on success, -1 if the response would
 * overflow the internal TX buffer (raise USBSCPI_TINYUSB_TX_BUF_SIZE then). */
int usbscpi_tinyusb_tx(void *user, const uint8_t *data, size_t len, bool eom);

/* Queue arbitrary device->host bytes (e.g. an out-of-band UDS reply) through the
 * same buffered IN path. Same contract as usbscpi_tinyusb_tx. */
int usbscpi_tinyusb_queue_response(const uint8_t *data, size_t len);

/* Raise the USB488 SRQ status bit (call from tud_usbtmc_msg_trigger_cb etc.). */
void usbscpi_tinyusb_set_srq(void);

#ifdef __cplusplus
}
#endif

#endif
