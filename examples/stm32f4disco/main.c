/*
 * STM32F4-Discovery (STM32F407VGT6) USBTMC + SCPI demo.
 *
 * Uses libopencm3 for board init (clocks, GPIO, NVIC) and TinyUSB's DWC2
 * driver for the USB OTG FS peripheral.  The iotsploit-usb core + TinyUSB
 * glue handle the SCPI/USBTMC protocol path.
 *
 * Clock:  HSE 8 MHz -> PLL -> 168 MHz SYSCLK, 48 MHz on PLLQ (USB).
 * USB:    OTG FS full-speed, PA11 (DM) / PA12 (DP), AF10.
 * LEDs:   PD12-PD15 (green/orange/red/blue).
 * Button: PA0 (user).
 */
#include <stdint.h>
#include <string.h>

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/cm3/nvic.h>

#include "tusb.h"
#include "usbscpi/usbscpi.h"
#include "usbscpi_tinyusb.h"

/* ---------- SystemCoreClock (consumed by dwc2_stm32.h) ---------- */
uint32_t SystemCoreClock = 168000000u;

/* ---------- Static buffers (no dynamic allocation) ---------- */
static uint8_t s_storage[2048];
static char    s_line[96];
static uint8_t s_io[2048];

/* ---------- USB TX callback via TinyUSB glue ---------- */
static int usb_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user; (void)eom;
    return usbscpi_tinyusb_tx(NULL, data, len, true);
}

/* ---------- Board-specific SCPI command callbacks ---------- */

/* LED indices 0-3 map to PD12-PD15 (green, orange, red, blue) */
static const uint16_t led_pins[] = { GPIO12, GPIO13, GPIO14, GPIO15 };

static scpi_result_t cmd_led_set(scpi_t *ctx) {
    uint32_t idx, val;
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    if (SCPI_ParamUInt32(ctx, &val, TRUE) != TRUE) return SCPI_RES_ERR;
    if (idx >= 4) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    if (val) {
        gpio_set(GPIOD, led_pins[idx]);
    } else {
        gpio_clear(GPIOD, led_pins[idx]);
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_led_get(scpi_t *ctx) {
    uint32_t idx;
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    if (idx >= 4) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    SCPI_ResultUInt32(ctx, (gpio_get(GPIOD, led_pins[idx]) != 0) ? 1 : 0);
    return SCPI_RES_OK;
}

/* ---- Named LED commands (tag = led_pins[] index) ----
 * SCPI short-form mnemonics follow IEEE 488.2 convention:
 *   GREen (PD12), ORAnge (PD13), RED (PD14), BLUe (PD15) */
static scpi_result_t cmd_led_named_set(scpi_t *ctx) {
    uint32_t val;
    if (SCPI_ParamUInt32(ctx, &val, TRUE) != TRUE) return SCPI_RES_ERR;
    int32_t idx = SCPI_CmdTag(ctx);
    if (val) gpio_set(GPIOD, led_pins[idx]);
    else     gpio_clear(GPIOD, led_pins[idx]);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_led_named_get(scpi_t *ctx) {
    int32_t idx = SCPI_CmdTag(ctx);
    SCPI_ResultUInt32(ctx, (gpio_get(GPIOD, led_pins[idx]) != 0) ? 1 : 0);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_led_all_set(scpi_t *ctx) {
    uint32_t val;
    if (SCPI_ParamUInt32(ctx, &val, TRUE) != TRUE) return SCPI_RES_ERR;
    if (val) gpio_set(GPIOD, GPIO12 | GPIO13 | GPIO14 | GPIO15);
    else     gpio_clear(GPIOD, GPIO12 | GPIO13 | GPIO14 | GPIO15);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_led_all_get(scpi_t *ctx) {
    uint32_t state = 0;
    if (gpio_get(GPIOD, GPIO12)) state |= (1u << 0);  /* green  */
    if (gpio_get(GPIOD, GPIO13)) state |= (1u << 1);  /* orange */
    if (gpio_get(GPIOD, GPIO14)) state |= (1u << 2);  /* red    */
    if (gpio_get(GPIOD, GPIO15)) state |= (1u << 3);  /* blue   */
    SCPI_ResultUInt32(ctx, state);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_led_toggle(scpi_t *ctx) {
    uint32_t idx;
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    if (idx >= 4) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    gpio_toggle(GPIOD, led_pins[idx]);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_btn(scpi_t *ctx) {
    /* User button on PA0: active high (pressed = 1) */
    SCPI_ResultUInt32(ctx, (gpio_get(GPIOA, GPIO0) != 0) ? 1 : 0);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_gpio_set(scpi_t *ctx) {
    uint32_t pin, val;
    if (SCPI_ParamUInt32(ctx, &pin, TRUE) != TRUE) return SCPI_RES_ERR;
    if (SCPI_ParamUInt32(ctx, &val, TRUE) != TRUE) return SCPI_RES_ERR;
    if (pin > 15) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    /* Use GPIOA for generic GPIO commands (pins 0-15) */
    uint16_t mask = (uint16_t)(1u << pin);
    gpio_mode_setup(GPIOA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, mask);
    gpio_set_output_options(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, mask);
    if (val) {
        gpio_set(GPIOA, mask);
    } else {
        gpio_clear(GPIOA, mask);
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_gpio_get(scpi_t *ctx) {
    uint32_t pin;
    if (SCPI_ParamUInt32(ctx, &pin, TRUE) != TRUE) return SCPI_RES_ERR;
    if (pin > 15) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    uint16_t mask = (uint16_t)(1u << pin);
    gpio_mode_setup(GPIOA, GPIO_MODE_INPUT, GPIO_PUPD_NONE, mask);
    SCPI_ResultUInt32(ctx, (gpio_get(GPIOA, mask) != 0) ? 1 : 0);
    return SCPI_RES_OK;
}

/* ---------- SCPI command descriptor (enables SYSTem:HELP:DESCription?) ---------- */

static const usbscpi_param_desc_t desc_led_set_params[] = {
    { "index", "u32",  true },
    { "value", "bool", true },
};
static const usbscpi_param_desc_t desc_led_get_params[] = {
    { "index", "u32", true },
};
static const usbscpi_param_desc_t desc_led_val_params[] = {
    { "value", "bool", true },
};
static const usbscpi_param_desc_t desc_led_toggle_params[] = {
    { "index", "u32", true },
};
static const usbscpi_param_desc_t desc_gpio_set_params[] = {
    { "pin",   "u32",  true },
    { "value", "bool", true },
};
static const usbscpi_param_desc_t desc_gpio_get_params[] = {
    { "pin", "u32", true },
};

static const usbscpi_command_desc_t desc_commands[] = {
    { "LED:SET",      "command", "Set LED by index (0=green,1=orange,2=red,3=blue)",
      desc_led_set_params,     2, "none" },
    { "LED:GET?",     "query",   "Read LED state by index",
      desc_led_get_params,     1, "u32"  },
    { "LED:GREen",    "command", "Set green LED (PD12)",
      desc_led_val_params,     1, "none" },
    { "LED:GREen?",   "query",   "Read green LED state",
      NULL, 0, "u32"  },
    { "LED:ORAnge",   "command", "Set orange LED (PD13)",
      desc_led_val_params,     1, "none" },
    { "LED:ORAnge?",  "query",   "Read orange LED state",
      NULL, 0, "u32"  },
    { "LED:RED",      "command", "Set red LED (PD14)",
      desc_led_val_params,     1, "none" },
    { "LED:RED?",     "query",   "Read red LED state",
      NULL, 0, "u32"  },
    { "LED:BLUe",     "command", "Set blue LED (PD15)",
      desc_led_val_params,     1, "none" },
    { "LED:BLUe?",    "query",   "Read blue LED state",
      NULL, 0, "u32"  },
    { "LED:ALL",      "command", "Set all LEDs on or off",
      desc_led_val_params,     1, "none" },
    { "LED:ALL?",     "query",   "Read all LED states as bitmask (bit0=green..bit3=blue)",
      NULL, 0, "u32"  },
    { "LED:TOGgle",   "command", "Toggle LED by index",
      desc_led_toggle_params,  1, "none" },
    { "BTN?",         "query",   "Read user button (PA0), 1=pressed",
      NULL, 0, "u32"  },
    { "GPIO:SET",     "command", "Set GPIOA pin output level",
      desc_gpio_set_params,    2, "none" },
    { "GPIO:GET?",    "query",   "Read GPIOA pin input level",
      desc_gpio_get_params,    1, "u32"  },
};

static const usbscpi_descriptor_t s_descriptor = {
    .commands       = desc_commands,
    .command_count  = sizeof(desc_commands) / sizeof(desc_commands[0]),
    .workflows      = NULL,
    .workflow_count = 0,
};

static const scpi_command_t demo_commands[] = {
    /* Numeric LED control (index 0-3: green, orange, red, blue) */
    { "LED:SET",     cmd_led_set,        0 },
    { "LED:GET?",    cmd_led_get,        0 },

    /* Named LED control — tag encodes led_pins[] index */
    { "LED:GREen",   cmd_led_named_set,  0 },  /* PD12 */
    { "LED:GREen?",  cmd_led_named_get,  0 },
    { "LED:ORAnge",  cmd_led_named_set,  1 },  /* PD13 */
    { "LED:ORAnge?", cmd_led_named_get,  1 },
    { "LED:RED",     cmd_led_named_set,  2 },  /* PD14 */
    { "LED:RED?",    cmd_led_named_get,  2 },
    { "LED:BLUe",    cmd_led_named_set,  3 },  /* PD15 */
    { "LED:BLUe?",   cmd_led_named_get,  3 },

    /* Bulk LED control */
    { "LED:ALL",     cmd_led_all_set,    0 },
    { "LED:ALL?",    cmd_led_all_get,    0 },
    { "LED:TOGgle",  cmd_led_toggle,     0 },

    { "BTN?",        cmd_btn,            0 },
    { "GPIO:SET",    cmd_gpio_set,       0 },
    { "GPIO:GET?",   cmd_gpio_get,       0 },
    SCPI_CMD_LIST_END
};

/* ---------- TinyUSB USBTMC callbacks the application must provide ----------
 * The glue (usbscpi_tinyusb.c) implements the data-path callbacks.  These
 * remaining ones are strong symbols required by TinyUSB's usbtmc_device.c
 * and have no default, so every USBTMC application must define them or the
 * link fails. */
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
    static usbtmc_response_capabilities_t caps = {
        .USBTMC_status = USBTMC_STATUS_SUCCESS,
        .bcdUSBTMC = 0x0100,
    };
    return &caps;
}
#endif

bool tud_usbtmc_msgBulkOut_start_cb(usbtmc_msg_request_dev_dep_out const *msg) {
    (void)msg; return true;
}
bool tud_usbtmc_initiate_abort_bulk_out_cb(uint8_t *tmcResult) {
    *tmcResult = USBTMC_STATUS_SUCCESS; return true;
}
bool tud_usbtmc_check_abort_bulk_out_cb(usbtmc_check_abort_bulk_rsp_t *rsp) {
    rsp->USBTMC_status = USBTMC_STATUS_SUCCESS; return true;
}
bool tud_usbtmc_initiate_abort_bulk_in_cb(uint8_t *tmcResult) {
    *tmcResult = USBTMC_STATUS_SUCCESS; return true;
}
bool tud_usbtmc_check_abort_bulk_in_cb(usbtmc_check_abort_bulk_rsp_t *rsp) {
    rsp->USBTMC_status = USBTMC_STATUS_SUCCESS; return true;
}
bool tud_usbtmc_initiate_clear_cb(uint8_t *tmcResult) {
    *tmcResult = USBTMC_STATUS_SUCCESS; return true;
}
bool tud_usbtmc_check_clear_cb(usbtmc_get_clear_status_rsp_t *rsp) {
    rsp->USBTMC_status = USBTMC_STATUS_SUCCESS; return true;
}

/* ---------- USB OTG FS interrupt -> TinyUSB ----------
 * libopencm3's vector table names this handler otg_fs_isr. */
void otg_fs_isr(void) {
    tud_int_handler(0);
}

/* ---------- Board init ---------- */
static void board_init(void) {
    /* Clock: 8 MHz HSE -> PLL -> 168 MHz, 48 MHz PLLQ for USB */
    rcc_clock_setup_pll(&rcc_hse_8mhz_3v3[RCC_CLOCK_3V3_168MHZ]);
    SystemCoreClock = 168000000u;

    /* Enable peripheral clocks */
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOD);
    rcc_periph_clock_enable(RCC_OTGFS);

    /* LEDs: PD12-PD15 push-pull output */
    gpio_mode_setup(GPIOD, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
                    GPIO12 | GPIO13 | GPIO14 | GPIO15);
    gpio_set_output_options(GPIOD, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ,
                            GPIO12 | GPIO13 | GPIO14 | GPIO15);

    /* User button: PA0 input */
    gpio_mode_setup(GPIOA, GPIO_MODE_INPUT, GPIO_PUPD_PULLDOWN, GPIO0);

    /* USB OTG FS: PA11 (DM), PA12 (DP) as AF10 */
    gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO11 | GPIO12);
    gpio_set_af(GPIOA, GPIO_AF10, GPIO11 | GPIO12);

    /* Enable USB OTG FS interrupt */
    nvic_enable_irq(NVIC_OTG_FS_IRQ);
}

/* ---------- Main ---------- */
int main(void) {
    board_init();

    tusb_init();

    usbscpi_config_t cfg = {
        .usb_tx        = usb_tx,
        .line_buf      = s_line,
        .line_buf_len  = sizeof(s_line),
        .max_block_len = 4096,
        .idn           = "IoTSploit,STM32F4-Disco,0001,0.1.0",
        .io_buf        = s_io,
        .io_buf_len    = sizeof(s_io),
        .proto         = 1,
        .mtu           = 256,
        .descriptor    = &s_descriptor,
    };

    usbscpi_t *dev = usbscpi_init(s_storage, sizeof(s_storage), &cfg);
    usbscpi_tinyusb_bind(dev);
    usbscpi_register(dev, demo_commands);

    while (1) {
        tud_task();
        usbscpi_task(dev);
    }
}
