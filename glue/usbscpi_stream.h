#ifndef USBSCPI_STREAM_H
#define USBSCPI_STREAM_H

#include <stddef.h>
#include <stdint.h>

#include "usbscpi/ring_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Data plane for continuous acquisition: drains a ring buffer to one TCP
 * consumer, device-to-host, unsolicited.
 *
 * This is the *transport*. It is payload-agnostic by design and must stay so —
 * it moves opaque bytes and never interprets them. The only structure it knows
 * is `stride`, and only because record alignment cannot survive a reconnect
 * without it.
 *
 * SCPI remains the control plane. Nothing here parses commands; a consumer
 * discovers the port and record format over SCPI and then just reads.
 *
 * Threading: exactly one producer thread writes the ring, and
 * usbscpi_stream_serve() is the only reader. That is the SPSC contract
 * helpers/ring_buffer.c is built for.
 */

/* Bind `bind_addr`:`port`, then accept and serve one consumer at a time,
 * draining `ring` to it.
 *
 * Blocks; run it in its own thread. Returns non-zero only if the listening
 * socket could not be created or bound — a consumer disconnecting is normal
 * and returns the loop to accept().
 *
 * `bind_addr` is required and has no default: "0.0.0.0" exposes the record
 * stream to anything that can reach the interface, and that should be a
 * visible decision at the call site.
 *
 * `stride` is the producer's record size in bytes, or 1 for an unstructured
 * byte stream. The producer MUST write whole records only (see
 * usbscpi_ring_write's partial-write behaviour) — this code relies on the ring
 * head always being a multiple of `stride`. */
int usbscpi_stream_serve(usbscpi_ring_t *ring, const char *bind_addr,
                         uint16_t port, size_t stride);

/* Non-zero while a consumer is attached. Lets a producer skip the cost of
 * encoding records nobody will read.
 *
 * Records not produced because nobody was attached are NOT drops: the drop
 * counter measures loss from a running capture, not time spent idle. A
 * consumer must not read a counter jump across an attach as lost data. */
int usbscpi_stream_attached(void);

/* Records discarded by this glue to restore alignment after a consumer
 * vanished part-way through one. Monotonic.
 *
 * This is the only loss the transport can cause, and it is at most one record
 * per disconnect. Ring-overflow loss is detected by the producer and counted
 * there — there is deliberately no API for the producer to report into,
 * because that would be a counter this component cannot maintain. */
size_t usbscpi_stream_torn(void);

#ifdef __cplusplus
}
#endif

#endif /* USBSCPI_STREAM_H */
