#ifndef BLE_SCAN_H
#define BLE_SCAN_H

#include <stddef.h>
#include <stdint.h>

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

#ifdef __cplusplus
}
#endif

#endif /* BLE_SCAN_H */
