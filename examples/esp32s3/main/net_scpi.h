#ifndef NET_SCPI_H
#define NET_SCPI_H

#include "usbscpi/usbscpi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SCPI over TCP on the ESP32-S3, alongside the existing USBTMC transport.
 *
 * This creates a SECOND usbscpi_t. It cannot share the USB one: a context holds
 * exactly one usb_tx callback (usbscpi.h), and every response goes through it
 * (src/usbscpi.c), so a command arriving over TCP on the USB context would send
 * its reply out over USB. The two contexts also need separate line buffers and
 * error queues, because a recv() that delivers half a command would otherwise
 * leave residue for a USB message to append to.
 *
 * `tmpl` supplies everything device-specific — idn, descriptor, data_avail /
 * data_read, max_block_len — and is copied. The transport-specific fields
 * (usb_tx, line_buf, io_buf, mtu) are overridden with this module's own.
 *
 * Association is retried automatically, and the listener starts once the
 * station holds a lease. Returns 0 if the task was created.
 */
int net_scpi_start(const usbscpi_config_t *tmpl,
                   const scpi_command_t *commands,
                   const char *ssid,
                   const char *password);

#ifdef __cplusplus
}
#endif

#endif /* NET_SCPI_H */
