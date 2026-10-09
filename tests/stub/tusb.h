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

/* Descriptor definitions used by the board identity regression test. */
#define CFG_TUD_ENDPOINT0_SIZE 64
#define TUSB_DESC_DEVICE 1
#define TUSB_DESC_CONFIG 2
#define TUSB_DESC_STRING 3
#define TUSB_DESC_INTERFACE 4
#define TUSB_DESC_ENDPOINT 5
#define TUSB_CLASS_UNSPECIFIED 0
#define TUSB_CLASS_VENDOR_SPECIFIC 0xFF
#define TUSB_XFER_BULK 2
#define TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP 0x20
#define TUD_USBTMC_PROTOCOL_USB488 1
#define U16_TO_U8S_LE(value) ((value) & 0xFF), (((value) >> 8) & 0xFF)
#define TUD_CONFIG_DESC_LEN 9
#define TUD_VENDOR_DESC_LEN 23
#define TUD_USBTMC_IF_DESCRIPTOR_LEN 9
#define TUD_USBTMC_BULK_DESCRIPTORS_LEN 14
#define TUD_CONFIG_DESCRIPTOR(config, count, str, len, attr, power) \
    9, TUSB_DESC_CONFIG, U16_TO_U8S_LE(len), count, config, str, attr, power / 2
#define TUD_VENDOR_DESCRIPTOR(itf, str, out, in, size) \
    9, TUSB_DESC_INTERFACE, itf, 0, 2, 0xFF, 0, 0, str, \
    TUD_USBTMC_BULK_DESCRIPTORS(out, in, size)
#define TUD_USBTMC_IF_DESCRIPTOR(itf, count, str, protocol) \
    9, TUSB_DESC_INTERFACE, itf, 0, count, 0xFE, 3, protocol, str
#define TUD_USBTMC_BULK_DESCRIPTORS(out, in, size) \
    7, TUSB_DESC_ENDPOINT, out, TUSB_XFER_BULK, U16_TO_U8S_LE(size), 0, \
    7, TUSB_DESC_ENDPOINT, in, TUSB_XFER_BULK, U16_TO_U8S_LE(size), 0

typedef struct {
    uint8_t bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} tusb_desc_device_t;

typedef struct {
    struct { unsigned int EOM : 1; } bmTransferAttributes;
} usbtmc_msg_request_dev_dep_out;

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
