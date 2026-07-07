#ifndef BLE_SNIFF_HANDLER_H
#define BLE_SNIFF_HANDLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded static capture ring. One query round-trip fetches each record, so this
 * is sized for bursts of tens of PDUs per window (the zero-host-edit constraint);
 * overflow is counted, not buffered. */
#define MAX_SNIFF_RESULTS 64

/**
 * Initialize the BLE advertising sniffer (brings up the bare-metal radio and the
 * microsecond timestamp timer). Call once at startup before any other function.
 */
void ble_sniff_init(void);

/**
 * Start a timed capture window on the given channel. Clears previous results and
 * the dropped counter, then captures for `secs` seconds. Passing channel 0 keeps
 * the currently selected channel.
 * @param secs     Window length in seconds (0 -> default 5, clamped to 600).
 * @param channel  Advertising channel 37/38/39, or 0 to keep the current one.
 */
void ble_sniff_start(uint16_t secs, uint8_t channel);

/** Stop the capture window early. */
void ble_sniff_stop(void);

/**
 * Pump the sniffer: drain any received PDU into the ring and end the window when
 * its time elapses. Call from the main loop.
 */
void ble_sniff_task(void);

/** True while a capture window is active. */
bool ble_sniff_is_active(void);

/** Number of PDUs buffered in the current/last window. */
uint16_t ble_sniff_count(void);

/** Number of PDUs dropped because the ring was full during the window. */
uint32_t ble_sniff_dropped(void);

/** Select the advertising channel while idle (37/38/39). */
void ble_sniff_set_channel(uint8_t channel);

/** Currently selected advertising channel. */
uint8_t ble_sniff_get_channel(void);

/**
 * Format buffered record `index` as a text row for BLE:SNIFf:PACKet?:
 *   "<timestamp_us>,<channel>,<rssi>,<length>,<pdu_hex>"
 * @return number of bytes written (excluding NUL), or 0 if index is out of range.
 */
size_t ble_sniff_format_row(uint16_t index, char *buf, size_t buf_len);

/**
 * Transmit a raw advertising PDU (hex-decoded by the caller) once on the current
 * channel. Used by BLE:INJect.
 * @return 0 on success, non-zero if the sniffer is mid-window or args invalid.
 */
int ble_sniff_inject(const uint8_t *pdu, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif /* BLE_SNIFF_HANDLER_H */
