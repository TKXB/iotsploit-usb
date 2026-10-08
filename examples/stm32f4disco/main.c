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
 * CAN1:   PD0 (RX) / PD1 (TX), AF9.   CAN2: PB12 (RX) / PB13 (TX), AF9.
 *         Each pair goes to an external transceiver (e.g. SN65HVD230).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/can.h>
#include <libopencm3/cm3/nvic.h>

#include "tusb.h"
#include "usbscpi/ring_buffer.h"
#include "usbscpi/usbscpi.h"
#include "usbscpi_tinyusb.h"

const char *board_serial(void); /* usb_descriptors.c */

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

/* ---------- CAN (bxCAN1 + bxCAN2) ----------
 * Bus numbers on the SCPI side are 1 and 2. Received frames are queued by the
 * RX0 ISRs into one ring and drained by CAN:RECV?. Both ISRs run at the same
 * NVIC priority, so they never preempt each other and the ring keeps a single
 * producer at a time. */

typedef struct {
    uint32_t id;
    uint8_t  bus;
    uint8_t  ext;
    uint8_t  rtr;
    uint8_t  len;
    uint8_t  data[8];
} can_frame_rec_t;  /* 16 bytes: the ring size stays a whole number of them */

static const uint32_t can_ports[] = { CAN1, CAN2 };
/* CAN2 owns filter banks from CAN_FMR.CAN2SB (reset value 14) upwards. */
static const uint32_t can_filter_nr[] = { 0, 14 };

static uint8_t           s_can_ring_mem[64 * sizeof(can_frame_rec_t)];
static usbscpi_ring_t    s_can_ring;
static volatile uint32_t s_can_dropped;  /* written only by the RX ISRs */
static bool              s_can_open[2];

static void can_rx_drain(uint8_t bus) {
    uint32_t port = can_ports[bus - 1u];
    while (can_fifo_pending(port, 0)) {
        can_frame_rec_t rec;
        bool ext, rtr;
        uint8_t fmi;
        uint16_t ts;
        memset(&rec, 0, sizeof(rec));
        can_receive(port, 0, true, &rec.id, &ext, &rtr, &fmi, &rec.len,
                    rec.data, &ts);
        rec.bus = bus;
        rec.ext = ext ? 1u : 0u;
        rec.rtr = rtr ? 1u : 0u;
        if (rec.len > sizeof(rec.data)) rec.len = sizeof(rec.data);  /* DLC 9-15 = 8 bytes */
        /* Whole records only: a partial one would misalign every later read. */
        if (usbscpi_ring_free(&s_can_ring) < sizeof(rec)) {
            s_can_dropped++;
        } else {
            usbscpi_ring_write(&s_can_ring, (const uint8_t *)&rec, sizeof(rec));
        }
    }
}

void can1_rx0_isr(void) { can_rx_drain(1); }
void can2_rx0_isr(void) { can_rx_drain(2); }

static bool can_param_bus(scpi_t *ctx, uint32_t *bus) {
    if (SCPI_ParamUInt32(ctx, bus, TRUE) != TRUE) return false;
    if (*bus < 1u || *bus > 2u) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return false;
    }
    return true;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* CAN:OPEN <bus>,<bitrate>
 * APB1 = 42 MHz; 1 + 11 + 2 = 14 tq per bit, sample point 85.7 %. */
static scpi_result_t cmd_can_open(scpi_t *ctx) {
    uint32_t bus, bitrate, brp;
    if (!can_param_bus(ctx, &bus)) return SCPI_RES_ERR;
    if (SCPI_ParamUInt32(ctx, &bitrate, TRUE) != TRUE) return SCPI_RES_ERR;
    switch (bitrate) {
    case 1000000: brp = 3;  break;
    case 500000:  brp = 6;  break;
    case 250000:  brp = 12; break;
    case 125000:  brp = 24; break;
    default:
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }

    uint32_t port = can_ports[bus - 1u];
    s_can_open[bus - 1u] = false;
    if (can_init(port, false, true, false, false, false, false,
                 CAN_BTR_SJW_1TQ, CAN_BTR_TS1_11TQ, CAN_BTR_TS2_2TQ, brp,
                 false, false) != 0) {
        /* Usually no transceiver: RX never sees 11 recessive bits. */
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    /* Accept everything into FIFO0. */
    can_filter_id_mask_32bit_init(can_filter_nr[bus - 1u], 0, 0, 0, true);
    can_enable_irq(port, CAN_IER_FMPIE0);
    s_can_open[bus - 1u] = true;
    return SCPI_RES_OK;
}

/* CAN:SEND <bus>,<id>,"<hex data>"  e.g. CAN:SEND 1,#H123,"DEADBEEF"
 * An id above 0x7FF is sent as a 29-bit extended frame. */
static scpi_result_t cmd_can_send(scpi_t *ctx) {
    uint32_t bus, id;
    const char *hex;
    size_t hlen;
    uint8_t data[8];

    if (!can_param_bus(ctx, &bus)) return SCPI_RES_ERR;
    if (SCPI_ParamUInt32(ctx, &id, TRUE) != TRUE) return SCPI_RES_ERR;
    if (SCPI_ParamCharacters(ctx, &hex, &hlen, FALSE) != TRUE) {
        if (SCPI_ParamErrorOccurred(ctx)) return SCPI_RES_ERR;
        hlen = 0;  /* data omitted: zero-length frame */
    }
    if (id > 0x1FFFFFFFu || (hlen % 2u) != 0u || hlen > 2u * sizeof(data)) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    for (size_t i = 0; i < hlen; i += 2) {
        int hi = hex_nibble(hex[i]), lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            SCPI_ErrorPush(ctx, SCPI_ERROR_INVALID_STRING_DATA);
            return SCPI_RES_ERR;
        }
        data[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    if (!s_can_open[bus - 1u] ||
        can_transmit(can_ports[bus - 1u], id, id > 0x7FFu, false,
                     (uint8_t)(hlen / 2u), data) < 0) {
        /* Not opened, or all three TX mailboxes still pending. */
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

/* CAN:RECV? -> "<bus>,<id hex>,<ext>,<rtr>,<len>,<data hex>", or an empty
 * response when nothing is queued. */
static scpi_result_t cmd_can_recv(scpi_t *ctx) {
    can_frame_rec_t rec;
    char line[64];
    int n = 0;
    if (usbscpi_ring_count(&s_can_ring) >= sizeof(rec)) {
        usbscpi_ring_read(&s_can_ring, (uint8_t *)&rec, sizeof(rec));
        n = snprintf(line, sizeof(line), "%u,0x%lX,%u,%u,%u,",
                     rec.bus, (unsigned long)rec.id, rec.ext, rec.rtr, rec.len);
        for (uint8_t i = 0; !rec.rtr && i < rec.len; i++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%02X", rec.data[i]);
        }
    }
    SCPI_ResultCharacters(ctx, line, (size_t)n);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_can_count(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, (uint32_t)(usbscpi_ring_count(&s_can_ring) /
                                      sizeof(can_frame_rec_t)));
    return SCPI_RES_OK;
}

/* CAN:STATe? <bus> -> "<open>,<tec>,<rec>,<busoff>,<rx dropped>" */
static scpi_result_t cmd_can_state(scpi_t *ctx) {
    uint32_t bus;
    char line[48];
    if (!can_param_bus(ctx, &bus)) return SCPI_RES_ERR;
    uint32_t esr = CAN_ESR(can_ports[bus - 1u]);
    int n = snprintf(line, sizeof(line), "%u,%lu,%lu,%u,%lu",
                     s_can_open[bus - 1u] ? 1u : 0u,
                     (unsigned long)((esr >> 16) & 0xFFu),
                     (unsigned long)((esr >> 24) & 0xFFu),
                     (esr & CAN_ESR_BOFF) ? 1u : 0u,
                     (unsigned long)s_can_dropped);
    SCPI_ResultCharacters(ctx, line, (size_t)n);
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

static const usbscpi_param_desc_t desc_can_bus_params[] = {
    { "bus", "u32", true, NULL, NULL },
};
static const usbscpi_param_desc_t desc_can_open_params[] = {
    { "bus",     "u32", true, NULL, NULL },
    { "bitrate", "u32", true, NULL, NULL },
};
static const usbscpi_param_desc_t desc_can_send_params[] = {
    { "bus",  "u32",    true, NULL, NULL },
    { "id",   "u32",    true, NULL, NULL },
    { "data", "string", false, NULL, NULL },
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
    { "CAN:OPEN",     "command", "Start CAN bus 1 (PD0/PD1) or 2 (PB12/PB13) at 125k/250k/500k/1M",
      desc_can_open_params,    2, "none" },
    { "CAN:SEND",     "command", "Send one frame; id > 0x7FF is extended, data is hex",
      desc_can_send_params,    3, "none" },
    { "CAN:RECV?",    "query",   "Pop one frame: bus,id,ext,rtr,len,data (empty if none)",
      NULL, 0, "string" },
    { "CAN:COUNt?",   "query",   "Received frames waiting",
      NULL, 0, "u32"  },
    { "CAN:STATe?",   "query",   "open,tec,rec,busoff,rx_dropped",
      desc_can_bus_params,     1, "string" },
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

    { "CAN:OPEN",    cmd_can_open,       0 },
    { "CAN:SEND",    cmd_can_send,       0 },
    { "CAN:RECV?",   cmd_can_recv,       0 },
    { "CAN:COUNt?",  cmd_can_count,      0 },
    { "CAN:STATe?",  cmd_can_state,      0 },
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
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_GPIOD);
    rcc_periph_clock_enable(RCC_OTGFS);
    /* CAN2 is a slave of CAN1 (shared filters): CAN1 clock is always needed. */
    rcc_periph_clock_enable(RCC_CAN1);
    rcc_periph_clock_enable(RCC_CAN2);

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

    /* CAN1: PD0 (RX), PD1 (TX); CAN2: PB12 (RX), PB13 (TX); all AF9.
     * Pull-up keeps RX recessive when no transceiver is fitted. */
    gpio_mode_setup(GPIOD, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO0 | GPIO1);
    gpio_set_af(GPIOD, GPIO_AF9, GPIO0 | GPIO1);
    gpio_mode_setup(GPIOB, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO12 | GPIO13);
    gpio_set_af(GPIOB, GPIO_AF9, GPIO12 | GPIO13);

    usbscpi_ring_init(&s_can_ring, s_can_ring_mem, sizeof(s_can_ring_mem));
    nvic_enable_irq(NVIC_CAN1_RX0_IRQ);
    nvic_enable_irq(NVIC_CAN2_RX0_IRQ);

    /* Enable USB OTG FS interrupt */
    nvic_enable_irq(NVIC_OTG_FS_IRQ);
}

/* ---------- Main ---------- */
int main(void) {
    board_init();

    tusb_init();

    /* *IDN? carries the same chip-unique serial as the USB descriptor. */
    static char idn[64];
    snprintf(idn, sizeof(idn), "IoTSploit,STM32F4-Disco,%s,0.1.0", board_serial());

    usbscpi_config_t cfg = {
        .usb_tx        = usb_tx,
        .line_buf      = s_line,
        .line_buf_len  = sizeof(s_line),
        .max_block_len = 4096,
        .idn           = idn,
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
