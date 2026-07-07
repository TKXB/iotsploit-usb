/**
 * @file ble_sniff_handler.c
 * @brief Timed BLE advertising capture buffer feeding the SCPI text-row workflow.
 *
 * Mirrors the nRF demo's ble_scan_handler: a bounded static ring populated from
 * the radio RX path, plus a dropped-packet counter and per-record text
 * formatting. Capture is modeled as a bounded window (BLE:SNIFf <secs>) so the
 * existing descriptor-driven trigger_poll_fetch host runs it unchanged.
 */

#include "ble_sniff_handler.h"

#include <stdio.h>
#include <string.h>

#include "nrf.h"
#include "radio_ble.h"

typedef struct {
    uint32_t timestamp_us;              /* capture time, microseconds since boot */
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  pdu_len;                   /* header + payload bytes */
    uint8_t  pdu[RADIO_BLE_PDU_MAX + 2];
} sniff_record_t;

static sniff_record_t   s_records[MAX_SNIFF_RESULTS];
static volatile uint16_t s_count   = 0;
static volatile uint32_t s_dropped = 0;
static volatile bool     s_active  = false;
static uint32_t          s_deadline_us = 0;   /* window end in timer ticks (us) */

/* ---- Microsecond timestamp source (free-running TIMER2 @ 1 MHz) ---- */

static void timer_init(void) {
    NRF_TIMER2->TASKS_STOP  = 1;
    NRF_TIMER2->MODE        = TIMER_MODE_MODE_Timer << TIMER_MODE_MODE_Pos;
    NRF_TIMER2->BITMODE     = TIMER_BITMODE_BITMODE_32Bit << TIMER_BITMODE_BITMODE_Pos;
    NRF_TIMER2->PRESCALER   = 4; /* 16 MHz / 2^4 = 1 MHz -> 1 tick = 1 us */
    NRF_TIMER2->TASKS_CLEAR = 1;
    NRF_TIMER2->TASKS_START = 1;
}

static uint32_t timer_now_us(void) {
    NRF_TIMER2->TASKS_CAPTURE[0] = 1;
    return NRF_TIMER2->CC[0];
}

/* ---- Public API ---- */

void ble_sniff_init(void) {
    timer_init();
    radio_ble_init();
    s_count = 0;
    s_dropped = 0;
    s_active = false;
}

void ble_sniff_start(uint16_t secs, uint8_t channel) {
    if (secs == 0)  secs = 5;
    if (secs > 600) secs = 600;
    if (channel == 37 || channel == 38 || channel == 39) {
        radio_ble_set_channel(channel);
    }

    s_count = 0;
    s_dropped = 0;
    s_deadline_us = timer_now_us() + (uint32_t)secs * 1000000UL;
    s_active = true;

    radio_ble_start_rx();
}

void ble_sniff_stop(void) {
    s_active = false;
}

void ble_sniff_task(void) {
    if (!s_active) {
        return;
    }

    if (radio_ble_packet_ready()) {
        radio_ble_packet_t pkt;
        if (radio_ble_consume(&pkt) && pkt.crc_ok) {
            if (s_count < MAX_SNIFF_RESULTS) {
                sniff_record_t *r = &s_records[s_count];
                r->timestamp_us = timer_now_us();
                r->channel      = pkt.channel;
                r->rssi         = pkt.rssi;
                r->pdu_len      = pkt.pdu_len;
                memcpy(r->pdu, pkt.pdu, pkt.pdu_len);
                s_count++;
            } else {
                s_dropped++;
            }
        }
        /* Re-arm for the next PDU (END->DISABLE parked the radio). */
        radio_ble_start_rx();
    }

    /* Signed comparison handles the 32-bit us counter wrapping (~71 min window
     * max, well beyond the 600 s clamp). */
    if ((int32_t)(timer_now_us() - s_deadline_us) >= 0) {
        s_active = false;
    }
}

bool ble_sniff_is_active(void) {
    return s_active;
}

uint16_t ble_sniff_count(void) {
    return s_count;
}

uint32_t ble_sniff_dropped(void) {
    return s_dropped;
}

void ble_sniff_set_channel(uint8_t channel) {
    if (channel == 37 || channel == 38 || channel == 39) {
        radio_ble_set_channel(channel);
    }
}

uint8_t ble_sniff_get_channel(void) {
    return radio_ble_channel();
}

size_t ble_sniff_format_row(uint16_t index, char *buf, size_t buf_len) {
    if (index >= s_count || !buf || buf_len == 0) {
        return 0;
    }
    const sniff_record_t *r = &s_records[index];

    int n = snprintf(buf, buf_len, "%lu,%u,%d,%u,",
                     (unsigned long)r->timestamp_us, r->channel, r->rssi, r->pdu_len);
    if (n < 0 || (size_t)n >= buf_len) {
        return 0;
    }
    size_t pos = (size_t)n;

    /* Append the PDU as lowercase hex; truncate cleanly if the row MTU is hit. */
    static const char hex[] = "0123456789abcdef";
    for (uint8_t i = 0; i < r->pdu_len; i++) {
        if (pos + 2 >= buf_len) {
            break;
        }
        buf[pos++] = hex[(r->pdu[i] >> 4) & 0xF];
        buf[pos++] = hex[r->pdu[i] & 0xF];
    }
    buf[pos] = '\0';
    return pos;
}

int ble_sniff_inject(const uint8_t *pdu, uint8_t len) {
    if (s_active || !pdu || len < 2) {
        return -1;
    }
    radio_ble_transmit(pdu, len);
    return 0;
}
