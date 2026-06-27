/*
 * Minimal TinyUSB USBTMC stub for host-side unit testing of the glue layer.
 * It models the one behaviour that matters for the -110 fix: the real
 * tud_usbtmc_transmit_dev_msg_data() only succeeds when the class is in
 * STATE_TX_REQUESTED (host has issued the IN request). Calls made from the
 * bulk-OUT (STATE_RCV) context fail, exactly like the real stack.
 */
#ifndef USBSCPI_TEST_STUB_TUSB_H
#define USBSCPI_TEST_STUB_TUSB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define USBTMC_STATUS_SUCCESS 0x01u

typedef struct {
    uint32_t TransferSize;
} usbtmc_msg_request_dev_dep_in;

/* Stub-only observable state (defined in the test TU). */
typedef enum { STUB_STATE_RCV, STUB_STATE_TX_REQUESTED } stub_state_t;

extern stub_state_t stub_state;
extern int          stub_transmit_ok;       /* successful transmit calls */
extern int          stub_transmit_rejected; /* transmit calls refused (wrong state) */
extern int          stub_start_bus_read;    /* start_bus_read calls */
extern uint8_t      stub_last_tx[512];
extern uint32_t     stub_last_tx_len;
extern bool         stub_last_tx_eom;       /* EOM flag of the most recent transmit */

static inline bool tud_usbtmc_transmit_dev_msg_data(const void *data, size_t len,
                                                    bool endOfMessage, bool usingTermChar) {
    (void)usingTermChar;
    if (stub_state != STUB_STATE_TX_REQUESTED) {
        stub_transmit_rejected++;
        return false; /* mirrors TU_VERIFY(state == STATE_TX_REQUESTED) */
    }
    stub_last_tx_len = (uint32_t)len;
    stub_last_tx_eom = endOfMessage;
    if (len > sizeof(stub_last_tx)) {
        len = sizeof(stub_last_tx);
    }
    memcpy(stub_last_tx, data, len);
    stub_transmit_ok++;
    return true;
}

static inline void tud_usbtmc_start_bus_read(void) {
    stub_start_bus_read++;
}

#endif
