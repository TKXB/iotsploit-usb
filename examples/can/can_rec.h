#ifndef USBSCPI_EXAMPLE_CAN_REC_H
#define USBSCPI_EXAMPLE_CAN_REC_H

#include <stdint.h>

/*
 * Wire record for the CAN data plane.
 *
 * APPLICATION LEVEL. Nothing in glue/usbscpi_stream.c knows this struct exists,
 * and nothing in it may leak there — the transport moves opaque bytes and
 * learns only `stride`.
 *
 * Little-endian, fixed 88-byte stride, sized for CAN FD so that adding FD
 * support later is not a format migration. Classic frames waste 56 payload
 * bytes, which is nothing against either transport's bandwidth.
 *
 * 88 is deliberately not a power of two. A stride that shares no factor with
 * the round binary sizes socket buffers come in is what makes a partial send
 * land mid-record, which is the case the transport's realignment exists to
 * handle — a power-of-two stride would hide it.
 *
 * `dropped` is the producer's running total AT CAPTURE TIME, carried in-band so
 * a consumer sees the gap where it happened rather than inferring it from a
 * sequence hole. It is 64-bit because a narrow counter wraps and silently
 * under-reports. It counts both ring overflow and kernel receive-queue
 * overflow (SO_RXQ_OVFL) — frames the producer never saw would otherwise
 * vanish with nothing counted at all.
 *
 * Records not produced because no consumer was attached are NOT counted as
 * drops: the counter measures loss from a running capture, not idle time.
 */

#define CAN_REC_STRIDE 88u
#define CAN_REC_VERSION 1u

/* flags */
#define CAN_REC_F_EFF 0x01u /* 29-bit identifier   */
#define CAN_REC_F_RTR 0x02u /* remote request      */
#define CAN_REC_F_ERR 0x04u /* error frame         */
#define CAN_REC_F_FD  0x08u /* CAN FD frame        */
#define CAN_REC_F_BRS 0x10u /* bit-rate switch     */
#define CAN_REC_F_ESI 0x20u /* error state indicator */

typedef struct {
    uint64_t ts_us;    /* kernel arrival time, not userspace dequeue time */
    uint64_t dropped;  /* running total at capture */
    uint32_t can_id;   /* identifier, flag bits masked off */
    uint8_t  len;      /* 0..64 */
    uint8_t  flags;
    uint16_t rsv;
    uint8_t  data[64];
} can_rec_t;

/* The schema the device advertises over SCPI, so a host parses generically
 * instead of hardcoding this layout. Same grammar as the workflow `fields=`
 * spec already used by the descriptor. */
#define CAN_REC_FIELDS \
    "ts_us:u64:us,dropped:u64,can_id:u32:hex,len:u8,flags:u8,rsv:u16,data:bytes64"

#endif
