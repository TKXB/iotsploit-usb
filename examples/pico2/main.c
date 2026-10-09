#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "tusb.h"
#include "usbscpi/usbscpi.h"
#include "usbscpi_tinyusb.h"

const char *board_serial(void); /* usb_descriptors.c */

static uint8_t s_storage[2048];
static char    s_line[96];
/* Also holds the SYSTem:HELP:DESCription? reply, so it must exceed the
 * descriptor text (about 360 bytes). */
static uint8_t s_io[1024];

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
    if (SCPI_ParamUInt32(ctx, &pin, TRUE) != TRUE) return SCPI_RES_ERR;
    if (SCPI_ParamUInt32(ctx, &val, TRUE) != TRUE) return SCPI_RES_ERR;
    gpio_init((uint)pin);
    gpio_set_dir((uint)pin, GPIO_OUT);
    gpio_put((uint)pin, val != 0);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_gpio_get(scpi_t *ctx) {
    uint32_t pin;
    if (SCPI_ParamUInt32(ctx, &pin, TRUE) != TRUE) return SCPI_RES_ERR;
    gpio_init((uint)pin);
    gpio_set_dir((uint)pin, GPIO_IN);
    SCPI_ResultUInt32(ctx, gpio_get((uint)pin));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_adc_read(scpi_t *ctx) {
    uint32_t ch = 0;
    (void)SCPI_ParamUInt32(ctx, &ch, FALSE);
    adc_select_input((uint8_t)ch);
    SCPI_ResultUInt32(ctx, adc_read());
    return SCPI_RES_OK;
}

/* Settings (.agents/standards/scpi-commands.md): GPIO <pin>,<value> writes and
 * GPIO? <pin> reads; ADC? <channel> reads. Declared once; the old names stay as
 * undescribed aliases. */
static const usbscpi_param_desc_t gpio_set_params[] = {
    USBSCPI_PARAM("pin", "u32", true),
    USBSCPI_PARAM("value", "bool", true),
};
static const usbscpi_param_desc_t pin_params[] = {
    USBSCPI_PARAM("pin", "u32", true),
};
static const usbscpi_param_desc_t adc_params[] = {
    USBSCPI_PARAM("channel", "u32", false),
};

#define PICO_COMMANDS(CMD, ALIAS)                                             \
    CMD("GPIO",  cmd_gpio_set, "command", "Set a GPIO output level",          \
        USBSCPI_PARAMS(gpio_set_params), "none")                             \
    CMD("GPIO?", cmd_gpio_get, "query",   "Read a GPIO input level",          \
        USBSCPI_PARAMS(pin_params), "u32")                                   \
    CMD("ADC?",  cmd_adc_read, "query",   "Read the ADC; channel defaults to 0", \
        USBSCPI_PARAMS(adc_params), "u32")                                   \
    ALIAS("GPIO:SET",  cmd_gpio_set)                                         \
    ALIAS("GPIO:GET?", cmd_gpio_get)                                         \
    ALIAS("ADC:READ?", cmd_adc_read)
USBSCPI_DEFINE_COMMANDS(pico, PICO_COMMANDS);

static const usbscpi_descriptor_t s_descriptor = {
    .commands      = pico_desc_commands,
    .command_count = USBSCPI_COUNT(pico_desc_commands),
};

/* ---------- TinyUSB USBTMC callbacks the application must provide ----------
 * The glue (usbscpi_tinyusb.c) implements the data-path callbacks
 * (open, msg_data, msgBulkIn_*, get_stb, clearFeature). These remaining ones are
 * strong symbols required by TinyUSB's usbtmc_device.c and have no default, so
 * every USBTMC application has to define them or the link fails. */
#if CFG_TUD_USBTMC_ENABLE_488
usbtmc_response_capabilities_488_t const *tud_usbtmc_get_capabilities_cb(void) {
    static const usbtmc_response_capabilities_488_t caps = {
        .USBTMC_status = USBTMC_STATUS_SUCCESS,
        .bcdUSBTMC = 0x0100,
        .bmDevCapabilities = { 0 },
        .bcdUSB488 = 0x0100,
        .bmIntfcCapabilities = { 0 },
        .bmDevCapabilities488 = { 0 },
    };
    return &caps;
}
#else
usbtmc_response_capabilities_t const *tud_usbtmc_get_capabilities_cb(void) {
    static const usbtmc_response_capabilities_t caps = {
        .USBTMC_status = USBTMC_STATUS_SUCCESS,
        .bcdUSBTMC = 0x0100,
    };
    return &caps;
}
#endif

bool tud_usbtmc_initiate_abort_bulk_out_cb(uint8_t *tmcResult) {
    *tmcResult = USBTMC_STATUS_SUCCESS;
    return true;
}
bool tud_usbtmc_check_abort_bulk_out_cb(usbtmc_check_abort_bulk_rsp_t *rsp) {
    rsp->USBTMC_status = USBTMC_STATUS_SUCCESS;
    return true;
}
bool tud_usbtmc_initiate_abort_bulk_in_cb(uint8_t *tmcResult) {
    *tmcResult = USBTMC_STATUS_SUCCESS;
    return true;
}
bool tud_usbtmc_check_abort_bulk_in_cb(usbtmc_check_abort_bulk_rsp_t *rsp) {
    rsp->USBTMC_status = USBTMC_STATUS_SUCCESS;
    return true;
}
bool tud_usbtmc_initiate_clear_cb(uint8_t *tmcResult) {
    *tmcResult = USBTMC_STATUS_SUCCESS;
    return true;
}
bool tud_usbtmc_check_clear_cb(usbtmc_get_clear_status_rsp_t *rsp) {
    rsp->USBTMC_status = USBTMC_STATUS_SUCCESS;
    return true;
}

int main(void) {
    stdio_init_all();
    adc_init();
    tusb_init();

    /* *IDN? carries the same chip-unique serial as the USB descriptor. */
    static char idn[64];
    snprintf(idn, sizeof(idn), "IoTSploit,Pico2,%s,0.1.0", board_serial());

    usbscpi_config_t cfg = {
        .usb_tx      = usb_tx,
        .line_buf    = s_line,
        .line_buf_len = sizeof(s_line),
        .max_block_len = 4096,
        .idn         = idn,
        .data_avail  = adc_avail,
        .data_read   = adc_read_cb,
        .io_buf      = s_io,
        .io_buf_len  = sizeof(s_io),
        .proto       = 1,
        .mtu         = 256,
        .descriptor  = &s_descriptor,
    };

    usbscpi_t *dev = usbscpi_init(s_storage, sizeof(s_storage), &cfg);
    usbscpi_tinyusb_bind(dev);
    usbscpi_register(dev, pico_scpi_commands);

    while (1) {
        tud_task();
    }
}
