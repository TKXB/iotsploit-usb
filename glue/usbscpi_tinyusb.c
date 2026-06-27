#include "usbscpi_tinyusb.h"

#include <string.h>

#include "tusb.h"

/*
 * Size of the buffered SCPI/UDS response. One USBTMC IN message is held at a
 * time; SCPI responses are short, so 512 bytes is generous. If a single
 * response can exceed this, raise the macro (TinyUSB fragments the IN transfer
 * internally, so the whole buffer is handed off in one transmit call).
 *
 * Must be >= mtu + 8 (e.g. 264 for mtu=256).
 */
#ifndef USBSCPI_TINYUSB_TX_BUF_SIZE
#define USBSCPI_TINYUSB_TX_BUF_SIZE 4096u
#endif

/* IEEE 488.2 status byte bits */
#define USBSCPI_STB_MAV (0x10u) /* Message Available */
#define USBSCPI_STB_SRQ (0x40u) /* Service Request   */

static usbscpi_t *g_usbscpi;

/* Buffered device->host response (filled in the OUT path, sent in the IN path) */
static uint8_t  s_tx_buf[USBSCPI_TINYUSB_TX_BUF_SIZE];
static volatile uint32_t s_tx_len;     /* bytes accumulated for the next message */
static volatile uint32_t s_tx_sent;    /* bytes already delivered to the host */
static volatile uint32_t s_tx_chunk;   /* bytes handed to the in-flight transmit */
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

/* Interface opened (host issued SET_CONFIGURATION): arm the *first* bulk-OUT
 * read so the very first host write is actually received. Without this the OUT
 * endpoint stays un-armed and every host write times out (-110); enumeration
 * still succeeds, which makes the failure look like a data-path bug.
 *
 * The glue owns this so consumers cannot forget it: it already re-arms the OUT
 * endpoint after every receive / IN-complete / clear below, and this closes the
 * remaining gap at open time. The TinyUSB USBTMC driver opens the endpoint but
 * leaves the first bus read to the application. */
void tud_usbtmc_open_cb(uint8_t interface_id) {
    (void)interface_id;
    tud_usbtmc_start_bus_read();
}

/* Bulk-OUT data: feed the component, then re-arm the OUT endpoint. */
bool tud_usbtmc_msg_data_cb(void *data, size_t len, bool transfer_complete) {
    if (g_usbscpi) {
        usbscpi_on_rx(g_usbscpi, data, len, transfer_complete);
    }
    tud_usbtmc_start_bus_read();
    return true;
}

/* Host requested IN data: we are now in STATE_TX_REQUESTED, so transmit is
 * legal here. Send up to the host's requested TransferSize from the buffered
 * response (TinyUSB fragments each chunk across USB packets internally; the
 * buffer must stay valid until the IN completes). A response larger than the
 * host's per-request TransferSize is delivered over several IN requests, with
 * EOM set only on the final chunk -- this is what USBTMC requires and what the
 * host's reassembly relies on. Without honoring TransferSize the device could
 * declare more bytes than the host expects in a single read, leaving the IN
 * endpoint half-drained and blocking all further communication.
 *
 * If there is no pending response (e.g. a spurious host read without a prior
 * command), send a minimal dummy message so TinyUSB does not get stuck in
 * STATE_TX_REQUESTED, which would block all future OUT transfers. */
bool tud_usbtmc_msgBulkIn_request_cb(usbtmc_msg_request_dev_dep_in const *request) {
    if (s_tx_pending && s_tx_len > s_tx_sent) {
        uint32_t remaining = s_tx_len - s_tx_sent;
        uint32_t chunk = remaining;
        /* request->TransferSize is the most the host is willing to accept in
         * this transfer; clamp to it (0 means "no limit" in practice). */
        if (request->TransferSize != 0u && chunk > request->TransferSize) {
            chunk = request->TransferSize;
        }
        bool eom = (s_tx_sent + chunk >= s_tx_len);
        s_tx_chunk = chunk;
        s_tx_in_flight = true;
        return tud_usbtmc_transmit_dev_msg_data(s_tx_buf + s_tx_sent, chunk, eom, false);
    }
    /* No data ready: send a minimal response so TinyUSB does not get stuck
     * in STATE_TX_REQUESTED.  This happens when a host utility does a read
     * without first sending a command (e.g. `head -c 200`). */
    s_tx_chunk = 0u;
    s_tx_in_flight = true;
    static const uint8_t dummy = '\n';
    return tud_usbtmc_transmit_dev_msg_data(&dummy, 1u, true, false);
}

/* IN transfer finished: advance the sent offset. Only once the whole buffered
 * response has been delivered do we release the buffer and clear MAV; until
 * then we keep it pending for the host's next IN request. Re-arm OUT either way
 * so the host can issue the next REQUEST_DEV_DEP_MSG_IN. */
bool tud_usbtmc_msgBulkIn_complete_cb(void) {
    s_tx_sent += s_tx_chunk;
    s_tx_chunk = 0u;
    s_tx_in_flight = false;
    if (s_tx_sent >= s_tx_len) {
        s_tx_len = 0u;
        s_tx_sent = 0u;
        s_tx_pending = false;
        s_status &= (uint8_t)~USBSCPI_STB_MAV;
    }
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
    s_tx_sent = 0u;
    s_tx_chunk = 0u;
    s_tx_pending = false;
    s_tx_in_flight = false;
    s_status = 0u;
    tud_usbtmc_start_bus_read();
}

/* IN endpoint clear (e.g. host abort): reset TX state so future responses
 * can be queued even if tud_usbtmc_msgBulkIn_complete_cb was never called. */
void tud_usbtmc_bulkIn_clearFeature_cb(void) {
    s_tx_len = 0u;
    s_tx_sent = 0u;
    s_tx_chunk = 0u;
    s_tx_pending = false;
    s_tx_in_flight = false;
    s_status &= (uint8_t)~USBSCPI_STB_MAV;
}
