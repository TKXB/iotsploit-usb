#ifndef RADIO_BLE_H
#define RADIO_BLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum BLE advertising-channel PDU payload (bytes). The on-air PDU is a
 * 2-byte header (S0 + LENGTH) followed by up to 37 payload bytes. */
#define RADIO_BLE_PDU_MAX  37

/* One captured (or to-be-transmitted) advertising-channel PDU. `pdu` holds the
 * whole PDU including the 2-byte header, so `pdu_len` is 2..(RADIO_BLE_PDU_MAX+2). */
typedef struct {
    uint8_t channel;                    /* BLE advertising channel index (37/38/39) */
    int8_t  rssi;                       /* signal strength in dBm (negative) */
    bool    crc_ok;                     /* CRC matched */
    uint8_t pdu_len;                    /* bytes valid in `pdu` (header + payload) */
    uint8_t pdu[RADIO_BLE_PDU_MAX + 2];
} radio_ble_packet_t;

/**
 * Bring up the nRF52840 RADIO peripheral for bare-metal BLE advertising-channel
 * reception (1 Mbit/s, access address 0x8E89BED6, BLE whitening + CRC). This is
 * butterfly's radio-ownership model: no SoftDevice — the firmware drives the
 * radio directly. Starts the HFXO crystal, which the radio requires. Call once
 * before any other radio_ble_* function.
 */
void radio_ble_init(void);

/** Select the advertising channel (37, 38, or 39). Safe to call while idle. */
void radio_ble_set_channel(uint8_t channel);

/** Return the currently selected advertising channel. */
uint8_t radio_ble_channel(void);

/** Arm the receiver for one PDU on the current channel. The radio disables
 * itself after each received PDU (END->DISABLE short); call again to re-arm. */
void radio_ble_start_rx(void);

/** True if a PDU has been received since the last radio_ble_start_rx(). */
bool radio_ble_packet_ready(void);

/**
 * Copy the most recently received PDU into `out` and clear the ready flag.
 * @return true if a PDU was consumed, false if none was pending.
 */
bool radio_ble_consume(radio_ble_packet_t *out);

/**
 * Transmit a raw advertising-channel PDU (`pdu` = 2-byte header + payload,
 * `len` = total bytes) once on the current channel, then return to a disabled
 * state. Blocking; used by BLE:INJect. Leaves RX disabled — the caller re-arms.
 */
void radio_ble_transmit(const uint8_t *pdu, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_BLE_H */
