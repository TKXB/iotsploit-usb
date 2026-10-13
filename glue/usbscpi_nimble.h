#ifndef USBSCPI_NIMBLE_H
#define USBSCPI_NIMBLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usbscpi/usbscpi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NimBLE (ESP-IDF) glue for iotsploit-usb: SCPI over a BLE GATT peripheral.
 *
 * Wire contract, shared with hosts:
 *   service  7d7e0001-ce83-4a46-ac8a-2e014163ef3e (advertised)
 *   RX       7d7e0002-ce83-4a46-ac8a-2e014163ef3e  host writes command bytes,
 *            write with response, encryption required
 *   TX       7d7e0003-ce83-4a46-ac8a-2e014163ef3e  device notifies response bytes
 * Both directions are byte streams, exactly like TCP: a write or notification
 * is a fragment, not a message. Commands end with '\n'; responses keep normal
 * SCPI text and definite-length block framing. A host subscribes to TX before
 * sending, and keeps one query outstanding. Responses are dropped while the
 * host is not subscribed.
 *
 * Security: LE Secure Connections Just Works, bonded. A link is used only once
 * it is encrypted with an LE Secure Connections bond; anything else is
 * disconnected and its bond deleted. Just Works stops passive eavesdropping,
 * not an active man in the middle, and any central in range may pair while the
 * device advertises.
 *
 * Like the socket glue this is one context per transport (usbscpi_socket.h):
 * the BLE context needs its own storage, line_buf, io_buf and usb_tx. A worker
 * task owns the context; NimBLE callbacks only copy received bytes into a
 * bounded queue, and a write that does not fit is refused with an ATT error so
 * the host retries it. A new connection clears the context, so a host that
 * disconnected mid-command cannot poison the next session.
 */

/* Use this as usbscpi_config_t.usb_tx for the BLE context. Sends notifications
 * of at most ATT_MTU - 3 bytes, waiting for buffers when the stack is
 * congested. Returns 0, or -1 if the link carrying the command is gone or the
 * host is not subscribed. */
int usbscpi_nimble_tx(void *user, const uint8_t *data, size_t len, bool eom);

/* Bring up NimBLE, the GATT service and the worker, then advertise as
 * `device_name` from sync onwards and again after every disconnect. NVS must
 * already be initialised (bonds are stored there). Call once. Returns 0, or
 * -1 if any part failed to start. */
int usbscpi_nimble_start(usbscpi_t *ctx, const char *device_name);

#ifdef __cplusplus
}
#endif

#endif /* USBSCPI_NIMBLE_H */
