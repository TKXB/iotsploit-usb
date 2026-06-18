#include "usbscpi_tinyusb.h"

#include <string.h>

#include "tusb.h"

/*
 * Size of the buffered SCPI/UDS response. One USBTMC IN message is held at a
 * time; SCPI responses are short, so 512 bytes is generous. If a single
 * response can exceed this, raise the macro (TinyUSB fragments the IN transfer
 * internally, so the whole buffer is handed off in one transmit call).
 */
#ifndef USBSCPI_TINYUSB_TX_BUF_SIZE
#define USBSCPI_TINYUSB_TX_BUF_SIZE 512u
#endif

/* IEEE 488.2 status byte bits */
#define USBSCPI_STB_MAV (0x10u) /* Message Available */
#define USBSCPI_STB_SRQ (0x40u) /* Service Request   */

static usbscpi_t *g_usbscpi;

/* Buffered device->host response (filled in the OUT path, sent in the IN path) */
static uint8_t  s_tx_buf[USBSCPI_TINYUSB_TX_BUF_SIZE];
static volatile uint32_t s_tx_len;     /* bytes accumulated for the next message */
static volatile bool     s_tx_pending; /* a complete response awaits the host read */
static volatile bool     s_tx_in_flight; /* transmit handed to TinyUSB, awaiting completion */
static volatile uint8_t  s_status;     /* USBTMC/USB488 status byte */

void usbscpi_tinyusb_bind(usbscpi_t *ctx) {
    g_usbscpi = ctx;
}

void usbscpi_tinyusb_set_srq(void) {
    s_status |= USBSCPI_STB_SRQ;
}

/* Append response bytes and mark a pending message. Buffer, do NOT transmit:
 * TinyUSB only allows transmit once the host has requested the IN transfer. */
int usbscpi_tinyusb_queue_response(const uint8_t *data, size_t len) {
    if (!data || len == 0u) {
        return 0;
    }
    /* Don't clobber a response that TinyUSB is still streaming out. */
    if (s_tx_in_flight) {
        return -1;
    }
    if ((size_t)s_tx_len + len > sizeof(s_tx_buf)) {
        return -1;
    }
    memcpy(s_tx_buf + s_tx_len, data, len);
    s_tx_len += (uint32_t)len;
    s_tx_pending = true;
    s_status |= USBSCPI_STB_MAV;
    return 0;
}

int usbscpi_tinyusb_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user;
    (void)eom; /* the whole buffered message is sent with EOM at IN time */
    return usbscpi_tinyusb_queue_response(data, len);
}

/* ------------------------------------------------------------------ */
/* TinyUSB USBTMC callbacks                                            */
/* ------------------------------------------------------------------ */

/* Bulk-OUT data: feed the component, then re-arm the OUT endpoint. */
bool tud_usbtmc_msg_data_cb(void *data, size_t len, bool transfer_complete) {
    if (g_usbscpi) {
        usbscpi_on_rx(g_usbscpi, data, len, transfer_complete);
    }
    tud_usbtmc_start_bus_read();
    return true;
}

/* Host requested IN data: we are now in STATE_TX_REQUESTED, so transmit is
 * legal here. Hand the whole buffered response to TinyUSB (it fragments across
 * packets internally; the buffer must stay valid until the IN completes). */
bool tud_usbtmc_msgBulkIn_request_cb(usbtmc_msg_request_dev_dep_in const *request) {
    (void)request;
    if (s_tx_pending && s_tx_len > 0u) {
        s_tx_in_flight = true;
        return tud_usbtmc_transmit_dev_msg_data(s_tx_buf, s_tx_len, true, false);
    }
    /* No data ready: SCPI is strictly write-then-read, so this is a protocol
     * misuse path. Leave the request unanswered (host read times out) rather
     * than asserting on a zero-length transmit. */
    return true;
}

/* IN transfer finished: release the buffer, clear MAV, re-arm OUT. */
bool tud_usbtmc_msgBulkIn_complete_cb(void) {
    s_tx_len = 0u;
    s_tx_pending = false;
    s_tx_in_flight = false;
    s_status &= (uint8_t)~USBSCPI_STB_MAV;
    tud_usbtmc_start_bus_read();
    return true;
}

uint8_t tud_usbtmc_get_stb_cb(uint8_t *tmcResult) {
    uint8_t stb = s_status;
    s_status &= (uint8_t)~USBSCPI_STB_SRQ; /* SRQ is cleared once serviced */
    *tmcResult = USBTMC_STATUS_SUCCESS;
    return stb;
}

void tud_usbtmc_bulkOut_clearFeature_cb(void) {
    if (g_usbscpi) {
        usbscpi_clear(g_usbscpi);
    }
    /* Drop any half-built response and reset state. */
    s_tx_len = 0u;
    s_tx_pending = false;
    s_tx_in_flight = false;
    s_status = 0u;
    tud_usbtmc_start_bus_read();
}
