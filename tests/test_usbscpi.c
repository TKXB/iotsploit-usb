#include "usbscpi/ring_buffer.h"
#include "usbscpi/usbscpi.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char tx[2048];
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
    SCPI_ResultUInt32(ctx, 3300);
    return SCPI_RES_OK;
}

static uint32_t gpio_pin = 0;
static uint32_t gpio_val = 0;

static scpi_result_t custom_gpio_set(scpi_t *ctx) {
    if (SCPI_ParamUInt32(ctx, &gpio_pin, TRUE) != TRUE) return SCPI_RES_ERR;
    if (SCPI_ParamUInt32(ctx, &gpio_val, TRUE) != TRUE) return SCPI_RES_ERR;
    return SCPI_RES_OK;
}

static scpi_result_t custom_gpio_get(scpi_t *ctx) {
    if (SCPI_ParamUInt32(ctx, &gpio_pin, TRUE) != TRUE) return SCPI_RES_ERR;
    SCPI_ResultUInt32(ctx, gpio_val);
    return SCPI_RES_OK;
}

static uint8_t block_data_buf[64];
static size_t block_data_len = 0;

static scpi_result_t custom_block_cmd(scpi_t *ctx) {
    SCPI_ResultArbitraryBlock(ctx, block_data_buf, block_data_len);
    return SCPI_RES_OK;
}

static const scpi_command_t custom_commands[] = {
    { "MEASure:VOLTage?", custom_cmd, 0 },
    { "GPIO:SET", custom_gpio_set, 0 },
    { "GPIO:GET?", custom_gpio_get, 0 },
    { "BLOCk:TEST?", custom_block_cmd, 0 },
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
    uint8_t storage[2048];
    char line[96];
    usbscpi_t *dev = make_device(&f, storage, sizeof(storage), line, sizeof(line));
    assert(usbscpi_register(dev, custom_commands) == USBSCPI_OK);

    assert(usbscpi_on_rx(dev, "*IDN?\n", 6, true) == USBSCPI_OK);
    assert(strstr(f.tx, "Test,USBSCPI,SN1,0.1.0") != NULL);

    f.tx_len = 0;
    f.tx[0] = '\0';
    assert(usbscpi_on_rx(dev, "MEAS:VOLT?\n", 11, true) == USBSCPI_OK);
    assert(strcmp(f.tx, "3300\n") == 0);
}

static void test_param_parsing_and_arbitrary_block(void) {
    fixture_t f;
    uint8_t storage[2048];
    char line[96];
    usbscpi_t *dev = make_device(&f, storage, sizeof(storage), line, sizeof(line));
    assert(usbscpi_register(dev, custom_commands) == USBSCPI_OK);

    /* GPIO:SET 5,1 */
    f.tx_len = 0;
    f.tx[0] = '\0';
    assert(usbscpi_on_rx(dev, "GPIO:SET 5,1\n", 13, true) == USBSCPI_OK);
    assert(gpio_pin == 5);
    assert(gpio_val == 1);

    /* GPIO:GET? 5 */
    f.tx_len = 0;
    f.tx[0] = '\0';
    assert(usbscpi_on_rx(dev, "GPIO:GET? 5\n", 12, true) == USBSCPI_OK);
    assert(strcmp(f.tx, "1\n") == 0);

    /* Arbitrary block with embedded zeros and newlines */
    block_data_len = 10;
    for (size_t i = 0; i < block_data_len; i++) {
        block_data_buf[i] = (uint8_t)(0xFF ^ i);
    }
    f.tx_len = 0;
    f.tx[0] = '\0';
    assert(usbscpi_on_rx(dev, "BLOC:TEST?\n", 12, true) == USBSCPI_OK);
    /* Expected: #210<10 bytes>\n */
    assert(f.tx_len == 4 + 10 + 1); /* "#210" + payload + "\n" */
    assert(f.tx[0] == '#');
    assert(f.tx[1] == '2');
    assert(f.tx[2] == '1');
    assert(f.tx[3] == '0');
    assert(memcmp(&f.tx[4], block_data_buf, 10) == 0);
    /* f.tx is a char[]; cast through uint8_t so the high-bit payload byte
     * (0xF6) compares equal on platforms where char is signed (x86). */
    assert((uint8_t)f.tx[4 + 10 - 1] == block_data_buf[9]);
    assert(f.tx[4 + 10] == '\n');
}

static void test_binary_block_split_and_special_bytes(void) {
    fixture_t f;
    uint8_t storage[2048];
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
    uint8_t storage[2048];
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

static size_t test_data_avail_val = 0;
static size_t test_data_avail(void *user) {
    (void)user;
    return test_data_avail_val;
}

static size_t test_data_read(void *user, uint8_t *buf, size_t len) {
    (void)user;
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(i & 0xFF);
    }
    return len;
}

static void test_new_default_commands(void) {
    fixture_t f;
    uint8_t storage[2048];
    char line[96];
    uint8_t io_buf[256];
    usbscpi_config_t cfg = {
        .usb_tx = tx_cb,
        .line_buf = line,
        .line_buf_len = sizeof(line),
        .max_block_len = 4096,
        .idn = "Test,USBSCPI,SN1,0.1.0",
        .data_avail = test_data_avail,
        .data_read = test_data_read,
        .io_buf = io_buf,
        .io_buf_len = sizeof(io_buf),
        .proto = 1,
        .mtu = 256,
        .user = &f,
    };
    usbscpi_t *dev = usbscpi_init(storage, sizeof(storage), &cfg);
    assert(dev);

    /* SYST:CAP? */
    f.tx_len = 0;
    f.tx[0] = '\0';
    assert(usbscpi_on_rx(dev, "SYST:CAP?\n", 11, true) == USBSCPI_OK);
    assert(strstr(f.tx, "proto=1") != NULL);
    assert(strstr(f.tx, "mtu=256") != NULL);

    /* SYST:ERR:COUN? (should be 0 after init) */
    f.tx_len = 0;
    f.tx[0] = '\0';
    assert(usbscpi_on_rx(dev, "SYST:ERR:COUN?\n", 16, true) == USBSCPI_OK);
    assert(strcmp(f.tx, "0\n") == 0);

    /* DATA:COUNt? */
    test_data_avail_val = 42;
    f.tx_len = 0;
    f.tx[0] = '\0';
    assert(usbscpi_on_rx(dev, "DATA:COUNt?\n", 13, true) == USBSCPI_OK);
    assert(strcmp(f.tx, "42\n") == 0);

    /* DATA:READ? 8 */
    f.tx_len = 0;
    f.tx[0] = '\0';
    assert(usbscpi_on_rx(dev, "DATA:READ? 8\n", 13, true) == USBSCPI_OK);
    /* Expected: #28<8 bytes>\n */
    assert(f.tx_len == 3 + 8 + 1); /* "#2" + "8" + payload + "\n" */
    assert(f.tx[0] == '#');
    assert(f.tx[1] == '1');
    assert(f.tx[2] == '8');
    for (size_t i = 0; i < 8; i++) {
        assert((uint8_t)f.tx[3 + i] == (uint8_t)(i & 0xFF));
    }
    assert(f.tx[3 + 8] == '\n');

    /* SYST:HELP:HEAD? */
    f.tx_len = 0;
    f.tx[0] = '\0';
    assert(usbscpi_on_rx(dev, "SYST:HELP:HEAD?\n", 17, true) == USBSCPI_OK);
    assert(f.tx_len > 0);
    assert(strstr(f.tx, "*IDN?") != NULL);
    assert(strstr(f.tx, "SYSTem:ERRor?") != NULL);
}

/* ---- Descriptor (SYSTem:HELP:DESCription?) test data ---- */

static const usbscpi_param_desc_t desc_gpio_params[] = {
    { "pin", "u32", true },
    { "value", "bool", true },
};

static const usbscpi_command_desc_t desc_commands[] = {
    { "GPIO:SET", "command", "Set GPIO output level", desc_gpio_params, 2, "none" },
    { "WLAN:SCAN", "command", "Start Wi-Fi scan", NULL, 0, NULL },
    { "WLAN:SCAN?", "query", "Get scan result by index", NULL, 0, "string" },
};

static const char *const desc_ble_connect_failed[] = { "3" };

static const char *const desc_ble_pair_failed[] = { "5" };

static const usbscpi_prompt_desc_t desc_ble_pair_prompts[] = {
    { "2", "passkey", "BLE:PAIR:PASSKey", NULL },
    { "3", "confirm", "BLE:PAIR:CONFirm", "BLE:PAIR:NUMCmp?" },
    { "6", "display", NULL,               "BLE:PAIR:PASSKey?" },
};

static const usbscpi_workflow_desc_t desc_workflows[] = {
    {
        .name = "wifi-scan",
        .type = "trigger_poll_fetch",
        .summary = "Scan for Wi-Fi access points",
        .trigger_cmd = "WLAN:SCAN",
        .done_query = "WLAN:SCAN:DONE?",
        .done_value = "1",
        .count_query = "WLAN:SCAN:COUNt?",
        .fetch_query = "WLAN:SCAN?",
        .state_query = NULL,
        .success_value = NULL,
        .failed_values = NULL,
        .failed_value_count = 0,
        .timeout_ms = 15000,
        .poll_ms = 250,
    },
    {
        .name = "ble-connect",
        .type = "trigger_poll_interactive",
        .summary = "Connect to a BLE device",
        .trigger_cmd = "BLE:CONNect",
        .done_query = NULL,
        .done_value = NULL,
        .count_query = NULL,
        .fetch_query = NULL,
        .state_query = "BLE:CONNect:STATe?",
        .success_value = "2",
        .failed_values = desc_ble_connect_failed,
        .failed_value_count = 1,
        .timeout_ms = 15000,
        .poll_ms = 200,
    },
    {
        .name = "ble-pair",
        .type = "trigger_poll_interactive",
        .summary = "Pair with the connected BLE device",
        .trigger_cmd = "BLE:PAIR",
        .state_query = "BLE:PAIR:STATe?",
        .success_value = "4",
        .failed_values = desc_ble_pair_failed,
        .failed_value_count = 1,
        .prompts = desc_ble_pair_prompts,
        .prompt_count = 3,
        .timeout_ms = 30000,
        .poll_ms = 200,
    },
};

static const usbscpi_descriptor_t test_descriptor = {
    .commands = desc_commands,
    .command_count = 3,
    .workflows = desc_workflows,
    .workflow_count = 3,
};

static void test_descriptor_query(void) {
    fixture_t f;
    uint8_t storage[2048];
    char line[96];
    uint8_t io_buf[1024];
    usbscpi_config_t cfg = {
        .usb_tx = tx_cb,
        .line_buf = line,
        .line_buf_len = sizeof(line),
        .max_block_len = 4096,
        .idn = "Test,USBSCPI,SN1,0.1.0",
        .io_buf = io_buf,
        .io_buf_len = sizeof(io_buf),
        .proto = 1,
        .mtu = 256,
        .descriptor = &test_descriptor,
        .user = &f,
    };
    usbscpi_t *dev = usbscpi_init(storage, sizeof(storage), &cfg);
    assert(dev);

    /* SYST:HELP:DESC? should return an IEEE 488.2 block with line-record text */
    f.tx_len = 0;
    f.tx[0] = '\0';
    assert(usbscpi_on_rx(dev, "SYST:HELP:DESC?\n", 17, true) == USBSCPI_OK);

    /* Response must be a definite-length arbitrary block: #<ndigits><len><payload> */
    assert(f.tx[0] == '#');
    int ndigits = f.tx[1] - '0';
    assert(ndigits >= 1 && ndigits <= 9);

    /* Parse block length */
    char len_str[16] = {0};
    memcpy(len_str, &f.tx[2], (size_t)ndigits);
    size_t block_len = (size_t)atoi(len_str);
    assert(block_len > 0);

    /* Block payload starts after #<ndigits><length> */
    const char *content = &f.tx[2 + ndigits];

    /* Verify line-record content */
    assert(strstr(content, "DEV idn=") != NULL);
    assert(strstr(content, "proto=1") != NULL);
    assert(strstr(content, "mtu=256") != NULL);
    assert(strstr(content, "CMD GPIO:SET") != NULL);
    assert(strstr(content, "kind=command") != NULL);
    assert(strstr(content, "summary=\"Set GPIO output level\"") != NULL);
    assert(strstr(content, "param=pin:u32:req") != NULL);
    assert(strstr(content, "param=value:bool:req") != NULL);
    assert(strstr(content, "returns=none") != NULL);
    assert(strstr(content, "CMD WLAN:SCAN") != NULL);
    assert(strstr(content, "CMD WLAN:SCAN?") != NULL);
    assert(strstr(content, "WF wifi-scan") != NULL);
    assert(strstr(content, "type=trigger_poll_fetch") != NULL);
    assert(strstr(content, "trigger=WLAN:SCAN") != NULL);
    assert(strstr(content, "done=WLAN:SCAN:DONE?:1") != NULL);
    assert(strstr(content, "fetch=WLAN:SCAN?#index") != NULL);
    assert(strstr(content, "timeout_ms=15000") != NULL);
    assert(strstr(content, "poll_ms=250") != NULL);
    /* Verify ble-connect interactive workflow with failed= */
    assert(strstr(content, "WF ble-connect") != NULL);
    assert(strstr(content, "type=trigger_poll_interactive") != NULL);
    assert(strstr(content, "trigger=BLE:CONNect") != NULL);
    assert(strstr(content, "state=BLE:CONNect:STATe?") != NULL);
    assert(strstr(content, "success=2") != NULL);
    assert(strstr(content, "failed=3") != NULL);
    /* Verify ble-pair interactive workflow emits its prompt= records */
    assert(strstr(content, "WF ble-pair") != NULL);
    assert(strstr(content, "prompt=2|passkey|BLE:PAIR:PASSKey") != NULL);
    assert(strstr(content, "prompt=3|confirm|BLE:PAIR:CONFirm|BLE:PAIR:NUMCmp?") != NULL);
    assert(strstr(content, "prompt=6|display||BLE:PAIR:PASSKey?") != NULL);
}

static void test_descriptor_unsupported(void) {
    fixture_t f;
    uint8_t storage[2048];
    char line[96];
    usbscpi_t *dev = make_device(&f, storage, sizeof(storage), line, sizeof(line));
    /* descriptor is NULL by default in make_device */

    f.tx_len = 0;
    f.tx[0] = '\0';
    /* Should push SCPI error (undefined header) and return non-OK */
    assert(usbscpi_on_rx(dev, "SYST:HELP:DESC?\n", 17, true) != USBSCPI_OK);
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
    test_param_parsing_and_arbitrary_block();
    test_binary_block_split_and_special_bytes();
    test_error_queue_and_free_query();
    test_new_default_commands();
    test_descriptor_query();
    test_descriptor_unsupported();
    test_ring_buffer();
    puts("usbscpi tests passed");
    return 0;
}
