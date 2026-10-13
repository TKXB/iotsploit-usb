#ifndef BLE_SCPI_H
#define BLE_SCPI_H

#include "usbscpi/usbscpi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SCPI over BLE on the ESP32-S3, alongside the existing USBTMC transport.
 *
 * This creates a SECOND usbscpi_t. It cannot share the USB one: a context holds
 * exactly one usb_tx callback (usbscpi.h), and every response goes through it
 * (src/usbscpi.c), so a command arriving over BLE on the USB context would send
 * its reply out over USB. The two contexts also need separate line buffers and
 * error queues, because a GATT write that delivers half a command would
 * otherwise leave residue for a USB message to append to.
 *
 * `tmpl` supplies everything device-specific — idn, descriptor, max_block_len —
 * and is copied. The transport-specific fields (usb_tx, line_buf, io_buf, mtu)
 * are overridden with this module's own, and the context is bound to the BLE
 * SPP link (ble_spp_scpi_bind). The BLE stack must be brought up separately
 * (ble_spp_security_init); the transport comes alive once a central connects,
 * subscribes to notifications, and writes commands.
 *
 * Returns 0 on success, -1 on failure.
 */
int ble_scpi_start(const usbscpi_config_t *tmpl,
                   const scpi_command_t *commands);

#ifdef __cplusplus
}
#endif

#endif /* BLE_SCPI_H */
