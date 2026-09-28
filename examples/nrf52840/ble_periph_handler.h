#ifndef BLE_PERIPH_HANDLER_H
#define BLE_PERIPH_HANDLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BLE peripheral role for nRF52840 (SoftDevice s140).
 *
 * Advertises as a connectable device so a phone or PC can connect, asks the
 * central to pair (SMP Security Request), and records how the link was secured:
 * the pairing method, the LE security mode/level, key size, bonding, and the
 * security parameters the central asked for.
 *
 * Like ble_conn_handler.c this uses LEGACY pairing only (no ECDH backend), so a
 * central requesting LE Secure Connections falls back to legacy and numeric
 * comparison is never negotiated. Keys live in RAM: a bond survives a
 * reconnect but not a reset.
 *
 * Runs beside the central role; each handler ignores the other's links.
 * SoftDevice callbacks write shared volatiles, SCPI accessors read them.
 */

/* Peripheral phase (ble_periph_state()). */
enum {
    BLE_PERIPH_IDLE        = 0,
    BLE_PERIPH_ADVERTISING = 1,
    BLE_PERIPH_CONNECTED   = 2,  /* connected, not yet pairing            */
    BLE_PERIPH_PAIRING     = 3,
    BLE_PERIPH_PASSKEY     = 4,  /* type the peer's passkey -> ble_periph_passkey()     */
    BLE_PERIPH_DISPLAY     = 5,  /* show our passkey        -> ble_periph_passkey_get() */
    BLE_PERIPH_DONE        = 6,  /* link encrypted; details in ble_periph_sec_info()    */
    BLE_PERIPH_FAILED      = 7,
};

/* Set the GAP device name. Call once after the SoftDevice is enabled. */
void ble_periph_init(void);

/* Drop any peripheral link and start advertising with the given SMP IO
 * capability (BLE_GAP_IO_CAPS_*, 0..4). Returns 0 on success, -1 on error. */
int ble_periph_start(uint8_t io_caps);

/* Stop advertising and drop the peripheral link. Returns 0. */
int ble_periph_stop(void);

int ble_periph_state(void);

/* Last GAP/SMP status (disconnect reason, auth status, SoftDevice error). */
int ble_periph_last_status(void);

/* Inject the passkey the peer displays (state BLE_PERIPH_PASSKEY). */
int ble_periph_passkey(uint32_t passkey);

/* Read the passkey we display (state BLE_PERIPH_DISPLAY). */
int ble_periph_passkey_get(uint32_t *out);

/* Format the link's security details as a CSV row:
 *   <mac>,<method>,<lesc>,<sec_mode>,<sec_level>,<encrypted>,<authenticated>,
 *   <bonded>,<key_size>,<peer_io>,<peer_bond>,<peer_mitm>
 * Returns 0, or -1 if no central has connected since the last start. */
int ble_periph_sec_info(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* BLE_PERIPH_HANDLER_H */
