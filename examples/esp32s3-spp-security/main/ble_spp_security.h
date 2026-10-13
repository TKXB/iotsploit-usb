#ifndef BLE_SPP_SECURITY_H
#define BLE_SPP_SECURITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usbscpi/usbscpi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up NVS, the NimBLE host stack, the SPP GATT service, and the Security
 * Manager (IO cap = KeyboardDisplay, MITM, LE Secure Connections, bonding).
 * Advertising is NOT started here — it is driven from SCPI (BLE:ADV:STARt).
 * Call once from a task after USB is up. */
void ble_spp_security_init(void);

/* Connection state returned by ble_spp_conn_state(): 0 idle, 1 connected. */
enum {
    BLE_SPP_DISCONNECTED = 0,
    BLE_SPP_CONNECTED    = 1,
};

/* Pairing state returned by ble_spp_pair_state(). Mirrors the interactive
 * NimBLE Security Manager IO actions so a host can drive passkey entry,
 * numeric comparison, or passkey display over SCPI. */
enum {
    BLE_SPP_PAIR_IDLE          = 0,
    BLE_SPP_PAIR_PROGRESS      = 1,
    BLE_SPP_PAIR_INPUT_NEEDED  = 2,  /* host types the peer's passkey -> ble_spp_pair_passkey()   */
    BLE_SPP_PAIR_NUMCMP_NEEDED = 3,  /* host confirms a number        -> ble_spp_pair_confirm()    */
    BLE_SPP_PAIR_DONE          = 4,
    BLE_SPP_PAIR_FAILED        = 5,
    BLE_SPP_PAIR_DISPLAY_KEY   = 6,  /* host shows our passkey on peer -> ble_spp_pair_passkey_get() */
};

/* Start / stop connectable BLE SPP advertising. Return 0 on success, -1 on
 * error (e.g. the host stack has not synced yet). */
int ble_spp_adv_start(void);
int ble_spp_adv_stop(void);

/* 0 idle, 1 connected. */
int ble_spp_conn_state(void);

/* Last GAP status/reason code (connect status, disconnect reason, or
 * encryption-change status) — for diagnostics. 0 means success/none. */
int ble_spp_last_status(void);

/* Initiate pairing/encryption (a Security Request) on the active connection.
 * Returns 0 on success, -1 if not connected. Async: poll ble_spp_pair_state(). */
int ble_spp_pair_start(void);

/* Current pairing state (one of BLE_SPP_PAIR_*). */
int ble_spp_pair_state(void);

/* Inject a 6-digit passkey when state == BLE_SPP_PAIR_INPUT_NEEDED. */
int ble_spp_pair_passkey(uint32_t passkey);

/* Read the passkey we generated when state == BLE_SPP_PAIR_DISPLAY_KEY. */
int ble_spp_pair_passkey_get(uint32_t *out);

/* Read the number to compare when state == BLE_SPP_PAIR_NUMCMP_NEEDED. */
int ble_spp_pair_numcmp_get(uint32_t *out);

/* Confirm (accept != 0) or reject numeric comparison. */
int ble_spp_pair_confirm(int accept);

/* ---- SCPI over BLE transport ----
 *
 * The SPP characteristic (0xABF1) carries a raw SCPI byte stream, exactly as a
 * TCP socket or the USBTMC bulk pipe does: a GATT write from the central is
 * command bytes, and the reply is delivered as GATT notifications on the same
 * characteristic. The central subscribes to notifications (writes the CCCD),
 * then writes commands and reads the notified replies — the Nordic-UART style.
 *
 * Bind the usbscpi_t whose usb_tx is ble_spp_scpi_tx(). The GATT write callback
 * feeds its bytes to usbscpi_on_rx() and the reply is produced synchronously,
 * all on the NimBLE host task — one context, one BLE link, the same
 * single-threaded contract the socket glue relies on. The context is cleared on
 * disconnect so a central that drops mid-block cannot poison the next session.
 *
 * One context per transport: this is a SECOND usbscpi_t, never the USB one. A
 * context holds exactly one usb_tx, so a command arriving over BLE on the USB
 * context would route its reply out over USB (see net_scpi.h in the esp32s3
 * example for the same reasoning on TCP). */
void ble_spp_scpi_bind(usbscpi_t *ctx);

/* usbscpi_config_t.usb_tx for the BLE context. Sends `data` as GATT
 * notifications on the SPP characteristic, chunked to the negotiated ATT MTU.
 * Returns 0 on success, -1 if no central is connected and subscribed, or a
 * notification could not be queued. */
int ble_spp_scpi_tx(void *user, const uint8_t *data, size_t len, bool eom);

/* Format the active connection's security info as a CSV row into out:
 *   <mac>,<level>,<encrypted>,<authenticated>,<bonded>,<key_size>
 * level is the LE security level (1..4). Returns 0 on success, -1 if not
 * connected. */
int ble_spp_sec_info(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* BLE_SPP_SECURITY_H */
