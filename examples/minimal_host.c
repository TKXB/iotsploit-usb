#include "usbscpi/usbscpi.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static uint8_t usbscpi_storage[2048];
static char line_buf[96];

static int usb_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user;
    (void)eom;
    fwrite(data, 1, len, stdout);
    return 0;
}

static int on_block_begin(void *user, size_t total_len) {
    (void)user;
    printf("block begin: %lu bytes\n", (unsigned long)total_len);
    return 0;
}

static int on_block_data(void *user, const uint8_t *data, size_t len) {
    (void)user;
    (void)data;
    printf("block chunk: %lu bytes\n", (unsigned long)len);
    return 0;
}

static int on_block_end(void *user, size_t total_len) {
    (void)user;
    printf("block end: %lu bytes\n", (unsigned long)total_len);
    return 0;
}

int main(void) {
    usbscpi_config_t cfg = {
        .usb_tx = usb_tx,
        .on_block_begin = on_block_begin,
        .on_block_data = on_block_data,
        .on_block_end = on_block_end,
        .line_buf = line_buf,
        .line_buf_len = sizeof(line_buf),
        .max_block_len = 4096,
        .idn = "Example,USBSCPI,SN0001,0.1.0",
    };

    usbscpi_t *dev = usbscpi_init(usbscpi_storage, sizeof(usbscpi_storage), &cfg);
    if (!dev) {
        return 1;
    }

    usbscpi_on_rx(dev, "*IDN?\n", 6, true);
    usbscpi_on_rx(dev, ":DATA:WRITE #15hello\n", 20, true);
    return 0;
}
