/*
 * Host-side test for the TinyUSB glue's USBTMC IN-response sequencing
 * (regression test for the -110 "query read times out" bug).
 *
 * It drives the glue exactly the way TinyUSB would: a bulk-OUT message arrives
 * first (STATE_RCV), then the host issues an IN request (STATE_TX_REQUESTED).
 * The fix requires the response to be *buffered* during OUT and only
 * *transmitted* during the IN request.
 */
#include "tusb.h" /* stub */
#include "usbscpi/usbscpi.h"
#include "usbscpi_tinyusb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Definitions for the stub's observable state */
stub_state_t stub_state = STUB_STATE_RCV;
int          stub_transmit_ok = 0;
int          stub_transmit_rejected = 0;
int          stub_start_bus_read = 0;
uint8_t      stub_last_tx[512];
uint32_t     stub_last_tx_len = 0;

/* TinyUSB-provided callbacks the glue defines (no public prototypes upstream) */
bool tud_usbtmc_msg_data_cb(void *data, size_t len, bool transfer_complete);
bool tud_usbtmc_msgBulkIn_request_cb(usbtmc_msg_request_dev_dep_in const *request);
bool tud_usbtmc_msgBulkIn_complete_cb(void);
void tud_usbtmc_bulkIn_clearFeature_cb(void);

static void reset_stub(void) {
    stub_state = STUB_STATE_RCV;
    stub_transmit_ok = 0;
    stub_transmit_rejected = 0;
    stub_start_bus_read = 0;
    stub_last_tx_len = 0;
    memset(stub_last_tx, 0, sizeof(stub_last_tx));
}

static void test_query_buffers_then_transmits_on_in(void) {
    reset_stub();

    static uint8_t storage[1024];
    static char    line[96];
    usbscpi_config_t cfg = {
        .usb_tx = usbscpi_tinyusb_tx,
        .line_buf = line,
        .line_buf_len = sizeof(line),
        .idn = "IoTSploit,Pico2-USBTMC,SN0001,0.1.0",
    };
    usbscpi_t *dev = usbscpi_init(storage, sizeof(storage), &cfg);
    assert(dev);
    usbscpi_tinyusb_bind(dev);

    /* --- Phase 1: bulk-OUT "*IDN?\n" arrives (host is NOT reading yet) --- */
    stub_state = STUB_STATE_RCV;
    char idn_cmd[] = "*IDN?\n";
    bool ok = tud_usbtmc_msg_data_cb(idn_cmd, strlen(idn_cmd), true);
    assert(ok);
    /* The reply must NOT have been transmitted from the OUT path... */
    assert(stub_transmit_ok == 0);
    assert(stub_transmit_rejected == 0); /* glue buffers, never even attempts */
    /* ...and the OUT endpoint must have been re-armed. */
    assert(stub_start_bus_read == 1);

    /* --- Phase 2: host issues the bulk-IN request (STATE_TX_REQUESTED) --- */
    stub_state = STUB_STATE_TX_REQUESTED;
    usbtmc_msg_request_dev_dep_in req = { .TransferSize = 64 };
    bool sent = tud_usbtmc_msgBulkIn_request_cb(&req);
    assert(sent);
    assert(stub_transmit_ok == 1);
    stub_last_tx[stub_last_tx_len] = '\0';
    assert(strstr((char *)stub_last_tx, "IoTSploit,Pico2-USBTMC,SN0001,0.1.0") != NULL);

    /* --- Phase 3: IN transfer completes: buffer released, OUT re-armed --- */
    bool done = tud_usbtmc_msgBulkIn_complete_cb();
    assert(done);
    assert(stub_start_bus_read == 2);

    /* A second IN request with no pending response must NOT resend the old
     * reply. Instead the glue sends a minimal dummy message so TinyUSB does not
     * stay stuck in STATE_TX_REQUESTED -- that stuck state would block every
     * future OUT transfer and time out subsequent reads with -110 (seen when a
     * host utility reads without first sending a command, e.g. `head -c 200`). */
    stub_transmit_ok = 0;
    stub_last_tx_len = 0;
    memset(stub_last_tx, 0, sizeof(stub_last_tx));
    bool again = tud_usbtmc_msgBulkIn_request_cb(&req);
    assert(again);
    assert(stub_transmit_ok == 1);                             /* a dummy was sent */
    assert(stub_last_tx_len == 1);                             /* and it is minimal */
    assert(strstr((char *)stub_last_tx, "IoTSploit") == NULL); /* not the old reply */
    assert(tud_usbtmc_msgBulkIn_complete_cb());

    /* IN-endpoint clear (host abort) must reset TX state so a response queued
     * before the clear is dropped and a later IN sees "no data" again. */
    assert(usbscpi_tinyusb_queue_response((const uint8_t *)"X\n", 2) == 0);
    tud_usbtmc_bulkIn_clearFeature_cb();
    stub_transmit_ok = 0;
    stub_last_tx_len = 0;
    bool after_clear = tud_usbtmc_msgBulkIn_request_cb(&req);
    assert(after_clear);
    /* The queued "X\n" was dropped by the clear, so this IN sends the 1-byte
     * dummy rather than the 2-byte queued response. */
    assert(stub_transmit_ok == 1);
    assert(stub_last_tx_len == 1);
}

int main(void) {
    test_query_buffers_then_transmits_on_in();
    puts("usbscpi tinyusb glue tests passed");
    return 0;
}
