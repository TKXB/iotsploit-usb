#include "usbscpi/ring_buffer.h"
#include "usbscpi/usbscpi.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char tx[512];
    size_t tx_len;
    uint8_t block[512];
    size_t block_len;
    size_t begin_len;
    size_t end_len;
    usbscpi_ring_t ring;
    uint8_t ring_storage[16];
} fixture_t;

static int tx_cb(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)eom;
    fixture_t *f = (fixture_t *)user;
    assert(f->tx_len + len < sizeof(f->tx));
    memcpy(f->tx + f->tx_len, data, len);
    f->tx_len += len;
    f->tx[f->tx_len] = '\0';
    return 0;
}

static int block_begin_cb(void *user, size_t total_len) {
    fixture_t *f = (fixture_t *)user;
    f->begin_len = total_len;
    return 0;
}

static int block_data_cb(void *user, const uint8_t *data, size_t len) {
    fixture_t *f = (fixture_t *)user;
    assert(f->block_len + len <= sizeof(f->block));
    memcpy(f->block + f->block_len, data, len);
    f->block_len += len;
    return 0;
}

static int block_end_cb(void *user, size_t total_len) {
    fixture_t *f = (fixture_t *)user;
    f->end_len = total_len;
    return 0;
}

static size_t data_free_cb(void *user) {
    fixture_t *f = (fixture_t *)user;
    return usbscpi_ring_free(&f->ring);
}

static scpi_result_t custom_cmd(scpi_t *ctx) {
    return SCPI_ResultUInt32(ctx, 3300);
}

static const scpi_command_t custom_commands[] = {
    { "MEASure:VOLTage?", custom_cmd, 0 },
    SCPI_CMD_LIST_END
};

static usbscpi_t *make_device(fixture_t *f, uint8_t *storage, size_t storage_len, char *line, size_t line_len) {
    memset(f, 0, sizeof(*f));
    assert(usbscpi_ring_init(&f->ring, f->ring_storage, sizeof(f->ring_storage)) == 0);
    usbscpi_config_t cfg = {
        .usb_tx = tx_cb,
        .on_block_begin = block_begin_cb,
        .on_block_data = block_data_cb,
        .on_block_end = block_end_cb,
        .data_free = data_free_cb,
        .user = f,
        .line_buf = line,
        .line_buf_len = line_len,
        .max_block_len = sizeof(f->block),
        .idn = "Test,USBSCPI,SN1,0.1.0",
    };
    usbscpi_t *dev = usbscpi_init(storage, storage_len, &cfg);
    assert(dev);
    return dev;
}

static void test_idn_and_custom_command(void) {
    fixture_t f;
    uint8_t storage[1024];
    char line[96];
    usbscpi_t *dev = make_device(&f, storage, sizeof(storage), line, sizeof(line));
    assert(usbscpi_register(dev, custom_commands) == SCPI_RES_OK);

    assert(usbscpi_on_rx(dev, "*IDN?\n", 6, true) == USBSCPI_OK);
    assert(strstr(f.tx, "Test,USBSCPI,SN1,0.1.0") != NULL);

    f.tx_len = 0;
    f.tx[0] = '\0';
    assert(usbscpi_on_rx(dev, "MEAS:VOLT?\n", 11, true) == USBSCPI_OK);
    assert(strcmp(f.tx, "3300\n") == 0);
}

static void test_binary_block_split_and_special_bytes(void) {
    fixture_t f;
    uint8_t storage[1024];
    char line[96];
    usbscpi_t *dev = make_device(&f, storage, sizeof(storage), line, sizeof(line));

    const uint8_t p1[] = ":DATA:WRI";
    const uint8_t p2[] = "TE #210";
    const uint8_t p3[] = { 0x00, 0x0a, 0x0d, '#', 0xff, 'A', 'B', 'C', 'D', 'E', '\n' };
    assert(usbscpi_on_rx(dev, p1, sizeof(p1) - 1, false) == USBSCPI_OK);
    assert(usbscpi_on_rx(dev, p2, sizeof(p2) - 1, false) == USBSCPI_OK);
    assert(usbscpi_on_rx(dev, p3, sizeof(p3), true) == USBSCPI_OK);

    assert(f.begin_len == 10);
    assert(f.end_len == 10);
    assert(f.block_len == 10);
    assert(f.block[0] == 0x00);
    assert(f.block[1] == 0x0a);
    assert(f.block[3] == '#');
    assert(f.block[4] == 0xff);
    assert(f.block[9] == 'E');
}

static void test_error_queue_and_free_query(void) {
    fixture_t f;
    uint8_t storage[1024];
    char line[96];
    usbscpi_t *dev = make_device(&f, storage, sizeof(storage), line, sizeof(line));

    assert(usbscpi_on_rx(dev, "BAD:CMD?\n", 9, true) != USBSCPI_OK);
    assert(usbscpi_on_rx(dev, "SYST:ERR?\n", 10, true) == USBSCPI_OK);
    assert(strstr(f.tx, "-113") != NULL);

    f.tx_len = 0;
    f.tx[0] = '\0';
    assert(usbscpi_on_rx(dev, "DATA:FREE?\n", 11, true) == USBSCPI_OK);
    assert(strcmp(f.tx, "16\n") == 0);
}

static void test_ring_buffer(void) {
    uint8_t backing[8];
    uint8_t out[8];
    usbscpi_ring_t ring;
    const uint8_t input[] = { 1, 2, 3, 4, 5 };
    assert(usbscpi_ring_init(&ring, backing, sizeof(backing)) == 0);
    assert(usbscpi_ring_write(&ring, input, sizeof(input)) == sizeof(input));
    assert(usbscpi_ring_count(&ring) == 5);
    assert(usbscpi_ring_read(&ring, out, 3) == 3);
    assert(out[0] == 1 && out[2] == 3);
    assert(usbscpi_ring_free(&ring) == 6);
}

int main(void) {
    test_idn_and_custom_command();
    test_binary_block_split_and_special_bytes();
    test_error_queue_and_free_query();
    test_ring_buffer();
    puts("usbscpi tests passed");
    return 0;
}
