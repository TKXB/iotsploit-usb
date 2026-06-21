#ifndef BLE_CONN_H
#define BLE_CONN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the NimBLE Security Manager (IO cap = KeyboardDisplay, MITM, SC,
 * bonding). Call once after ble_scan_init() has brought up the host stack. */
void ble_conn_init(void);

/* Connection state returned by ble_conn_state(). */
enum {
    BLE_CONN_IDLE       = 0,
    BLE_CONN_CONNECTING = 1,
    BLE_CONN_CONNECTED  = 2,
    BLE_CONN_FAILED     = 3,
};

/* Pairing state returned by ble_pair_state(). */
enum {
    BLE_PAIR_IDLE         = 0,
    BLE_PAIR_PROGRESS     = 1,
    BLE_PAIR_INPUT_NEEDED = 2,  /* host must inject a passkey  -> ble_pair_passkey()   */
    BLE_PAIR_NUMCMP_NEEDED= 3,  /* host must confirm a number  -> ble_pair_confirm()    */
    BLE_PAIR_DONE         = 4,
    BLE_PAIR_FAILED       = 5,
    BLE_PAIR_DISPLAY_KEY  = 6,  /* host must show our passkey  -> ble_pair_passkey_get() */
};

/* Connect to the device at scan result #index (reuses the address + type stored
 * by the last BLE:SCAN). Returns 0 on success, -1 on error. Async: poll
 * ble_conn_state(). */
int ble_conn_start(size_t scan_index);

/* Current connection state (one of BLE_CONN_*). */
int ble_conn_state(void);

/* Last GAP status/reason code (connect status, disconnect reason, or
 * encryption-change status) — for diagnostics. 0 means success/none. */
int ble_conn_last_status(void);

/* Drop the current connection. Returns 0 on success. */
int ble_conn_disconnect(void);

/* Initiate pairing/encryption on the current connection. Returns 0 on success.
 * Async: poll ble_pair_state(). */
int ble_pair_start(void);

/* Current pairing state (one of BLE_PAIR_*). */
int ble_pair_state(void);

/* Inject a 6-digit passkey when state == BLE_PAIR_INPUT_NEEDED. Returns 0 ok. */
int ble_pair_passkey(uint32_t passkey);

/* Read the passkey we generated when state == BLE_PAIR_DISPLAY_KEY (the user
 * enters it on the peer). Returns 0 and writes *out, -1 if none. */
int ble_pair_passkey_get(uint32_t *out);

/* Read the number to compare when state == BLE_PAIR_NUMCMP_NEEDED. */
int ble_pair_numcmp_get(uint32_t *out);

/* Confirm (accept != 0) or reject numeric comparison. Returns 0 ok. */
int ble_pair_confirm(int accept);

/* Format the current connection's security info as a CSV row into out:
 *   <mac>,<level>,<encrypted>,<authenticated>,<bonded>,<key_size>
 * level is the LE security level (1..4). Returns 0 on success, -1 if not
 * connected. */
int ble_sec_info(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* BLE_CONN_H */
