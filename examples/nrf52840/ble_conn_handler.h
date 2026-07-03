#ifndef BLE_CONN_HANDLER_H
#define BLE_CONN_HANDLER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BLE central connect + pair for nRF52840 (SoftDevice s140).
 *
 * Wraps the raw SoftDevice GAP/SMP APIs so the usbscpi workflow engine can
 * drive: scan -> pick a connectable device -> connect -> pair -> read the
 * negotiated LE security level.
 *
 * Pairing uses LEGACY pairing with MITM (IO caps = KeyboardDisplay), so the
 * reachable security levels are:
 *   1 = no security, 2 = encrypted/unauthenticated (Just Works),
 *   3 = encrypted/authenticated (passkey entry).
 * Level 4 (LE Secure Connections) additionally requires an ECDH backend
 * (nrf_ble_lesc + micro-ecc/nrf_crypto) and is intentionally out of scope for
 * this self-contained demo. The security level is read straight from the
 * SoftDevice (BLE_GAP_EVT_CONN_SEC_UPDATE -> conn_sec.sec_mode.lv), so wiring
 * LESC later requires no change to BLE:SEC? reporting.
 *
 * All SoftDevice callbacks run in the BLE event context; the SCPI accessors run
 * on the USB task. State is shared through volatiles, matching the lock-free
 * style of ble_scan_handler.c. A single connection is tracked.
 */

/* Connection phase (ble_conn_state()). */
enum {
    BLE_CONN_IDLE       = 0,
    BLE_CONN_CONNECTING = 1,
    BLE_CONN_CONNECTED  = 2,
    BLE_CONN_FAILED     = 3,
};

/* Pairing phase (ble_pair_state()). */
enum {
    BLE_PAIR_IDLE         = 0,
    BLE_PAIR_PROGRESS     = 1,
    BLE_PAIR_INPUT_NEEDED = 2,  /* peer displays a passkey; host types it   -> ble_pair_passkey()     */
    BLE_PAIR_DONE         = 3,
    BLE_PAIR_FAILED       = 4,
    BLE_PAIR_DISPLAY_KEY  = 5,  /* we display a passkey; user enters on peer -> ble_pair_passkey_get() */
};

/* Combined connect+pair state (ble_connpair_state()), folds the connection and
 * pairing phases so one interactive workflow drives both (BLE:CPAIR). */
enum {
    BLE_CP_IDLE       = 0,
    BLE_CP_CONNECTING = 1,
    BLE_CP_PAIRING    = 2,  /* connected; pairing in progress    */
    BLE_CP_PASSKEY    = 3,  /* inject passkey   -> BLE:PAIR:PASSKey  */
    BLE_CP_DISPLAY    = 4,  /* show our passkey -> BLE:PAIR:PASSKey? */
    BLE_CP_DONE       = 5,
    BLE_CP_FAILED     = 6,
};

/* Fully automatic scan+select+connect+pair state (ble_auto_state()). Adds a
 * leading scan/select phase to the combined state machine (BLE:AUTO). */
enum {
    BLE_AUTO_IDLE       = 0,
    BLE_AUTO_SCANNING   = 1,  /* scanning + selecting a connectable device */
    BLE_AUTO_CONNECTING = 2,
    BLE_AUTO_PAIRING    = 3,
    BLE_AUTO_PASSKEY    = 4,  /* inject passkey   -> BLE:PAIR:PASSKey  */
    BLE_AUTO_DISPLAY    = 5,  /* show our passkey -> BLE:PAIR:PASSKey? */
    BLE_AUTO_DONE       = 6,
    BLE_AUTO_FAILED     = 7,
};

/* Register the BLE observer and reset state. Call once after ble_scan_init()
 * has enabled the SoftDevice. */
void ble_conn_init(void);

/* Advance the automatic (BLE:AUTO) scan->select->connect step. Must be pumped
 * from the main loop alongside tud_task()/usbscpi_task(); the connect and
 * pairing phases themselves are event-driven and need no polling. */
void ble_conn_task(void);

/* Connect to scan result #index (address + type stored by the last BLE:SCAN).
 * Returns 0 on success, -1 on error. Async: poll ble_conn_state(). */
int ble_conn_start(size_t scan_index);
int ble_conn_state(void);

/* Connect to scan result #index and auto-initiate pairing once connected.
 * Returns 0 on success, -1 on error. Async: poll ble_connpair_state(). */
int ble_connpair_start(size_t scan_index);
int ble_connpair_state(void);

/* Start a scan, pick the first connectable device whose advertised name
 * contains `filter` (or the first connectable device when filter is NULL/empty),
 * connect, and pair. Returns 0 on success, -1 on error. Async: poll
 * ble_auto_state(). */
int ble_auto_start(const char *filter);
int ble_auto_state(void);

/* Last GAP/SMP status code (connect status, disconnect reason, auth status) for
 * diagnostics. 0 means success/none. */
int ble_conn_last_status(void);

/* Drop the current connection. Returns 0 on success. */
int ble_conn_disconnect(void);

/* Initiate pairing on the current connection. Returns 0 on success.
 * Async: poll ble_pair_state(). */
int ble_pair_start(void);
int ble_pair_state(void);

/* Inject the passkey (as a plain integer, e.g. 123456) when the state is
 * BLE_PAIR_INPUT_NEEDED. Returns 0 on success. */
int ble_pair_passkey(uint32_t passkey);

/* Read the passkey we generated when the state is BLE_PAIR_DISPLAY_KEY (the user
 * enters it on the peer). Returns 0 and writes *out, -1 if none. */
int ble_pair_passkey_get(uint32_t *out);

/* Format the current connection's security info as a CSV row into out:
 *   <mac>,<level>,<encrypted>,<authenticated>,<bonded>,<key_size>
 * level is the LE security level (1..4) reported by the SoftDevice. Returns 0
 * on success, -1 if not connected. */
int ble_sec_info(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* BLE_CONN_HANDLER_H */
