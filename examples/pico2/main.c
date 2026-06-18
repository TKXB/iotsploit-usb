#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "tusb.h"
#include "usbscpi/usbscpi.h"
#include "usbscpi_tinyusb.h"

static uint8_t s_storage[1024];
static char    s_line[96];
static uint8_t s_io[256];

static int usb_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user;
    (void)eom;
    return usbscpi_tinyusb_tx(NULL, data, len, true);
}

static size_t adc_avail(void *user) {
    (void)user;
    return (size_t)-1;
}

static size_t adc_read_cb(void *user, uint8_t *buf, size_t len) {
    (void)user;
    size_t n = len / 2;
    for (size_t i = 0; i < n; i++) {
        uint16_t v = adc_read();
        buf[i * 2]     = (uint8_t)(v & 0xFF);
        buf[i * 2 + 1] = (uint8_t)(v >> 8);
    }
    return n * 2;
}

static scpi_result_t cmd_gpio_set(scpi_t *ctx) {
    uint32_t pin, val;
    if (SCPI_ParamUInt32(ctx, &pin, 1) != SCPI_RES_OK) return SCPI_RES_ERR;
    if (SCPI_ParamUInt32(ctx, &val, 1) != SCPI_RES_OK) return SCPI_RES_ERR;
    gpio_init((uint)pin);
    gpio_set_dir((uint)pin, GPIO_OUT);
    gpio_put((uint)pin, val != 0);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_gpio_get(scpi_t *ctx) {
    uint32_t pin;
    if (SCPI_ParamUInt32(ctx, &pin, 1) != SCPI_RES_OK) return SCPI_RES_ERR;
    gpio_init((uint)pin);
    gpio_set_dir((uint)pin, GPIO_IN);
    return SCPI_ResultUInt32(ctx, gpio_get((uint)pin));
}

static scpi_result_t cmd_adc_read(scpi_t *ctx) {
    uint32_t ch = 0;
    (void)SCPI_ParamUInt32(ctx, &ch, 0);
    adc_select_input((uint8_t)ch);
    return SCPI_ResultUInt32(ctx, adc_read());
}

static const scpi_command_t demo_commands[] = {
    { "GPIO:SET",    cmd_gpio_set, 0 },
    { "GPIO:GET?",   cmd_gpio_get, 0 },
    { "ADC:READ?",   cmd_adc_read, 0 },
    SCPI_CMD_LIST_END
};

int main(void) {
    stdio_init_all();
    adc_init();
    tusb_init();

    usbscpi_config_t cfg = {
        .usb_tx      = usb_tx,
        .line_buf    = s_line,
        .line_buf_len = sizeof(s_line),
        .max_block_len = 4096,
        .idn         = "IoTSploit,Pico2,0001,0.1.0",
        .data_avail  = adc_avail,
        .data_read   = adc_read_cb,
        .io_buf      = s_io,
        .io_buf_len  = sizeof(s_io),
        .proto       = 1,
        .mtu         = 256,
    };

    usbscpi_t *dev = usbscpi_init(s_storage, sizeof(s_storage), &cfg);
    usbscpi_tinyusb_bind(dev);
    usbscpi_register(dev, demo_commands);

    while (1) {
        tud_task();
    }
}
