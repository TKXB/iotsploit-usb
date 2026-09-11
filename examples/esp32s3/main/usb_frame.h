#ifndef USB_FRAME_H
#define USB_FRAME_H

#include <stdint.h>

/*
 * Envelope for the vendor bulk-IN pipe (interface 1, EP 0x82).
 *
 * That pipe carries device log text. Adding records to it means framing BOTH
 * kinds, because a reader can no longer tell one from the other by position.
 *
 * OFF AT BOOT. An existing host reads this endpoint as raw text with no
 * negotiation (host/rust/src/usbtmc_raw.rs), so a device that framed
 * unconditionally would garble every old host. Framing is switched on only by
 * SYSTem:STReam:FRAMing 1, which only a host that understands it will send.
 *
 *   [u8 type][u8 rsv][u16 len]  payload        little-endian len
 *
 * Records are written before log bytes each round and log bytes are dropped
 * first when the FIFO is short: logs are diagnostics, records are the
 * measurement, and losing the measurement to a chatty ESP_LOGx would be
 * backwards.
 */

#define USB_FRAME_HDR_LEN 4u
#define USB_FRAME_TYPE_LOG 0x01u
#define USB_FRAME_TYPE_REC 0x02u

/* Largest log chunk emitted per round once framing is on. Bounds how long a
 * burst of logging can delay the record path. */
#define USB_FRAME_LOG_CHUNK 256u

#endif /* USB_FRAME_H */
