#ifndef BLE_SCAN_H
#define BLE_SCAN_H

#include <stddef.h>
#include <stdint.h>

#include "usbscpi/ring_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the NimBLE controller + host task. Call once at startup. */
void ble_scan_init(void);

/* Start a GAP discovery for secs seconds (passive scan). Returns 0 on success.
 * Devices are collected asynchronously via the GAP event callback. */
int ble_scan_start(unsigned secs);

/* 1 once the most recent discovery has completed, else 0. */
int ble_scan_done(void);

/* Number of distinct devices seen by the last completed discovery. */
size_t ble_scan_count(void);

/* Format device #index as a CSV row into out:
 *   <addr>,<rssi>,"<name>",<adv_type>
 * Returns 0 on success, -1 if index is out of range. */
int ble_scan_get(size_t index, char *out, size_t out_len);

/* Copy the address (and its type: 0 public, 1 random) of device #index, as
 * needed to open a connection. Returns 0 on success, -1 if out of range. */
int ble_scan_addr(size_t index, uint8_t out_val[6], uint8_t *out_type);

/* ---- RSSI data plane ----------------------------------------------------
 *
 * The workflow above keeps ONE sample per device (store_device() refreshes
 * rssi in place) and caps at BLE_SCAN_MAX_DEV. That answers "what is around",
 * and it structurally cannot answer "how did this RSSI move" — the samples are
 * discarded as they arrive. This surface streams every advertisement report
 * instead, and the two coexist.
 *
 * The producer is the NimBLE GAP callback, which runs on the single host task,
 * so the ring keeps its single-producer/single-consumer contract with no lock.
 */

/* Ring the GAP callback fills. Hand to usbscpi_stream_serve(). */
usbscpi_ring_t *ble_stream_ring(void);

/* Record size, for usbscpi_stream_serve()'s stride argument. */
size_t ble_stream_stride(void);

/* Schema string advertised by SYSTem:STReam:FORMat?, so the host parses
 * generically instead of hardcoding the record layout. */
const char *ble_stream_fields(void);

/* Start/stop filling the ring. Scanning itself is started separately: this
 * only controls whether reports are recorded. */
void     ble_stream_enable(int on);
int      ble_stream_enabled(void);

/* Reports recorded, and reports lost to ring overflow. See the note in
 * ble_scan.c about what this counter can and cannot see. */
uint64_t ble_stream_count(void);
uint64_t ble_stream_dropped(void);

/* Begin a continuous (unlimited-duration) passive scan for streaming. */
int ble_stream_scan_start(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_SCAN_H */
