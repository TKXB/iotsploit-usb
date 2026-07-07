/**
 * @file radio_ble.c
 * @brief Bare-metal nRF52840 RADIO driver for BLE advertising-channel sniff/inject.
 *
 * This is the "radio core" the butterfly-over-USBTMC plan keeps from butterfly:
 * the firmware owns the raw RADIO peripheral directly (no SoftDevice), which is
 * why this example drops the SoftDevice-based nRF demo. Only the BLE
 * advertising-channel RX/TX path is implemented here — enough to prove the
 * timed-capture → text-row fetch workflow end-to-end. Other butterfly protocols
 * (802.15.4, ESB, …) would plug in as additional radio configurations.
 */

#include "radio_ble.h"

#include <string.h>

#include "nrf.h"

/* BLE advertising physical-channel constants (Core spec, Vol 6, Part B). */
#define BLE_ACCESS_ADDR   0x8E89BED6UL
#define BLE_CRC_POLY      0x00065BUL   /* x^24 + x^10 + x^9 + x^6 + x^4 + x^3 + x + 1 */
#define BLE_CRC_INIT      0x555555UL

static uint8_t s_pkt[RADIO_BLE_PDU_MAX + 2]; /* S0 + LENGTH + payload, radio DMA target */
static uint8_t s_channel = 37;

/* Advertising channel index -> RF frequency (MHz above 2400). */
static uint8_t chan_to_freq(uint8_t ch) {
    switch (ch) {
        case 37: return 2;
        case 38: return 26;
        case 39: return 80;
        default: return 2;
    }
}

/* The RADIO needs the high-frequency crystal oscillator. Idempotent — TinyUSB's
 * nRF driver also starts HFCLK, and starting an already-running clock is a no-op. */
static void hfclk_start(void) {
    NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_HFCLKSTART = 1;
    while (NRF_CLOCK->EVENTS_HFCLKSTARTED == 0) {
        /* wait for the crystal to ramp up */
    }
}

void radio_ble_init(void) {
    hfclk_start();

    NRF_RADIO->POWER = 1;
    NRF_RADIO->MODE = (RADIO_MODE_MODE_Ble_1Mbit << RADIO_MODE_MODE_Pos);

    /* 1-byte S0, 8-bit LENGTH, no S1: the received PDU header lands in
     * s_pkt[0], the length in s_pkt[1], and the payload follows. */
    NRF_RADIO->PCNF0 = (1UL << RADIO_PCNF0_S0LEN_Pos) |
                       (8UL << RADIO_PCNF0_LFLEN_Pos) |
                       (0UL << RADIO_PCNF0_S1LEN_Pos);

    NRF_RADIO->PCNF1 = ((uint32_t)RADIO_BLE_PDU_MAX << RADIO_PCNF1_MAXLEN_Pos) |
                       (0UL << RADIO_PCNF1_STATLEN_Pos) |
                       (3UL << RADIO_PCNF1_BALEN_Pos) |   /* 3 base + 1 prefix = 4-byte AA */
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

    /* Access address 0x8E89BED6 split into BASE0 (low 3 bytes) + PREFIX0 (MSB). */
    NRF_RADIO->BASE0     = (BLE_ACCESS_ADDR << 8) & 0xFFFFFF00UL;
    NRF_RADIO->PREFIX0   = (BLE_ACCESS_ADDR >> 24) & 0xFFUL;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1; /* listen on logical address 0 */

    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCPOLY = BLE_CRC_POLY;
    NRF_RADIO->CRCINIT = BLE_CRC_INIT;

    NRF_RADIO->TIFS = 150;
    NRF_RADIO->PACKETPTR = (uint32_t)s_pkt;

    radio_ble_set_channel(s_channel);
}

void radio_ble_set_channel(uint8_t channel) {
    s_channel = channel;
    NRF_RADIO->FREQUENCY = chan_to_freq(channel);
    /* BLE whitening is seeded with the channel index; the hardware forces bit 6. */
    NRF_RADIO->DATAWHITEIV = channel & 0x3FUL;
}

uint8_t radio_ble_channel(void) {
    return s_channel;
}

/* Spin until the radio reaches the DISABLED state so a fresh RXEN/TXEN is legal. */
static void radio_disable(void) {
    NRF_RADIO->SHORTS = 0;
    if (NRF_RADIO->STATE != RADIO_STATE_STATE_Disabled) {
        NRF_RADIO->EVENTS_DISABLED = 0;
        NRF_RADIO->TASKS_DISABLE = 1;
        while (NRF_RADIO->EVENTS_DISABLED == 0) {
            /* wait for ramp-down */
        }
    }
}

void radio_ble_start_rx(void) {
    radio_disable();

    NRF_RADIO->PACKETPTR = (uint32_t)s_pkt;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_ADDRESS = 0;
    NRF_RADIO->EVENTS_DISABLED = 0;

    /* READY->START begins reception; ADDRESS->RSSISTART samples signal strength;
     * END->DISABLE parks the radio after each PDU so the poll loop can read the
     * buffer without a DMA race before re-arming. */
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |
                        RADIO_SHORTS_ADDRESS_RSSISTART_Msk |
                        RADIO_SHORTS_END_DISABLE_Msk |
                        RADIO_SHORTS_DISABLED_RSSISTOP_Msk;

    NRF_RADIO->TASKS_RXEN = 1;
}

bool radio_ble_packet_ready(void) {
    return NRF_RADIO->EVENTS_END != 0;
}

bool radio_ble_consume(radio_ble_packet_t *out) {
    if (!out || NRF_RADIO->EVENTS_END == 0) {
        return false;
    }
    NRF_RADIO->EVENTS_END = 0;

    out->crc_ok  = (NRF_RADIO->CRCSTATUS == RADIO_CRCSTATUS_CRCSTATUS_CRCOk);
    out->channel = s_channel;
    out->rssi    = -(int8_t)(NRF_RADIO->RSSISAMPLE & 0x7FUL);

    uint8_t payload_len = s_pkt[1];
    if (payload_len > RADIO_BLE_PDU_MAX) {
        payload_len = RADIO_BLE_PDU_MAX;
    }
    out->pdu_len = (uint8_t)(payload_len + 2); /* include the 2-byte header */
    memcpy(out->pdu, s_pkt, out->pdu_len);
    return true;
}

void radio_ble_transmit(const uint8_t *pdu, uint8_t len) {
    if (!pdu || len < 2) {
        return;
    }
    if (len > (uint8_t)(RADIO_BLE_PDU_MAX + 2)) {
        len = (uint8_t)(RADIO_BLE_PDU_MAX + 2);
    }

    radio_disable();
    memcpy(s_pkt, pdu, len);

    NRF_RADIO->PACKETPTR = (uint32_t)s_pkt;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;

    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0) {
        /* wait for the PDU to go out and the radio to ramp down */
    }
}
