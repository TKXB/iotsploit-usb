#ifndef USBSCPI_SOCKET_H
#define USBSCPI_SOCKET_H

#include <stdint.h>

#include "usbscpi/usbscpi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BSD-socket glue for iotsploit-usb: raw SCPI over TCP (port 5025 by
 * convention). Builds against glibc on Linux and against lwIP on ESP-IDF.
 *
 * Unlike the TinyUSB glue, responses are sent inline: usbscpi_socket_tx() is
 * called synchronously from inside usbscpi_on_rx() and writes straight to the
 * client socket. There is no deferred IN path and no MAV bit, because a socket
 * has no equivalent of USBTMC's REQUEST_DEV_DEP_MSG_IN handshake.
 *
 * IMPORTANT — one context per transport. usbscpi_config_t carries exactly one
 * usb_tx callback, so a context can only reply on one transport. A device that
 * serves both USB and TCP needs two usbscpi_t instances, each with its own
 * storage, line_buf, io_buf and usb_tx. Sharing one context would route TCP
 * replies out over USB, and would let the two transports corrupt each other's
 * line buffer and block-assembly state.
 */

/* Use this as usbscpi_config_t.usb_tx for the socket context. Writes to the
 * currently connected client. Returns 0 on success, -1 if there is no client or
 * the write failed. */
int usbscpi_socket_tx(void *user, const uint8_t *data, size_t len, bool eom);

/* Bind `bind_addr`:`port`, then accept and serve clients one at a time.
 *
 * Blocks; run it in its own FreeRTOS task or thread. Returns non-zero only if
 * the listening socket could not be created or bound — a client disconnecting
 * is normal and returns the loop to accept().
 *
 * `bind_addr` is required and has no default: choosing "0.0.0.0" should be a
 * visible decision at the call site, because a network listener exposes the
 * whole SCPI command surface to anything that can reach the interface.
 *
 * usbscpi_clear() is called on every disconnect so a client that dies mid-block
 * cannot poison the next session. */
int usbscpi_socket_serve(usbscpi_t *ctx, const char *bind_addr, uint16_t port);

/* Non-zero while a client is connected. Lets an application avoid radio or
 * power operations that would drop the link carrying the command. */
int usbscpi_socket_client_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* USBSCPI_SOCKET_H */
