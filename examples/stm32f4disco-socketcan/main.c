/*
 * STM32F4-Discovery (STM32F407VGT6) SocketCAN adapter + USBTMC/SCPI.
 *
 * examples/stm32f4disco plus a gs_usb (candleLight) vendor interface, so the
 * Linux gs_usb driver presents CAN1/CAN2 as can0/can1. Board files (CMSIS
 * shim, linker script, OpenOCD config) are shared with that example.
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
 *         Driven by SCPI (CAN:*) or, through the gs_usb interface, by Linux
 *         SocketCAN as can0/can1.
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

/* Vendor instance 0 is gs_usb (interface 0); the CAN stream is instance 1. */
#define CAN_STREAM_VENDOR 1
#include "can_stream.h"

const char *board_serial(void); /* usb_descriptors.c */

/* ---------- SystemCoreClock (consumed by dwc2_stm32.h) ---------- */
uint32_t SystemCoreClock = 168000000u;

/* ---------- Static buffers (no dynamic allocation) ---------- */
static uint8_t s_storage[2048];
static char    s_line[96];
/* Also holds the SYSTem:HELP:DESCription? reply, about 2 KB with the stream
 * commands, so it has room to spare. */
static uint8_t s_io[4096];

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
 * Each controller has one owner: SCPI (CAN:OPEN) or the Linux gs_usb driver
 * (SocketCAN, interface MODE start). Bus numbers on the SCPI side are 1 and 2;
 * gs_usb channels are 0 and 1. The RX0 ISRs queue each frame into its owner's
 * ring: gs_poll() drains SocketCAN's; SCPI's go to the stream (can_stream.h)
 * while SYSTem:STReam is running, and otherwise to the ring CAN:RECV? drains.
 * Both ISRs run at the same NVIC priority, so they never preempt each other and
 * each ring keeps a single producer at a time. */

enum { CAN_OWNER_NONE, CAN_OWNER_SCPI, CAN_OWNER_GS };

/* Linux gs_usb host frame for classic CAN. Little-endian, like the core. */
typedef struct {
    uint32_t echo_id;  /* host's TX tag; GS_ECHO_ID_RX for received frames */
    uint32_t can_id;   /* Linux can_id: bit 31 extended, bit 30 RTR */
    uint8_t  can_dlc;
    uint8_t  channel;
    uint8_t  flags;
    uint8_t  reserved;
    uint8_t  data[8];
} gs_host_frame_t;  /* 20 bytes: the transfer size gs_usb uses without timestamps */

#define GS_ECHO_ID_RX      0xFFFFFFFFu
#define GS_CAN_EFF_FLAG    0x80000000u
#define GS_CAN_RTR_FLAG    0x40000000u
#define GS_FLAG_OVERFLOW   0x01u

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
/* Set to or from CAN_OWNER_GS only with that controller's FMPIE0 masked. */
static volatile uint8_t  s_can_owner[2];

/* gs_usb receive queue: whole 20-byte frames, allowed to wrap. */
static uint8_t           s_gs_ring_mem[2048];
static usbscpi_ring_t    s_gs_ring;
static volatile uint32_t s_gs_dropped[2];  /* written only by the RX ISRs */

static void gs_rx_queue(const can_frame_rec_t *rec) {
    gs_host_frame_t f;
    f.echo_id  = GS_ECHO_ID_RX;
    f.can_id   = rec->id | (rec->ext ? GS_CAN_EFF_FLAG : 0u) |
                 (rec->rtr ? GS_CAN_RTR_FLAG : 0u);
    f.can_dlc  = rec->len;
    f.channel  = (uint8_t)(rec->bus - 1u);
    f.flags    = 0;
    f.reserved = 0;
    memcpy(f.data, rec->data, sizeof(f.data));
    if (usbscpi_ring_free(&s_gs_ring) < sizeof(f)) {
        s_gs_dropped[f.channel]++;
    } else {
        usbscpi_ring_write(&s_gs_ring, (const uint8_t *)&f, sizeof(f));
    }
}

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
        if (s_can_owner[bus - 1u] == CAN_OWNER_GS) {
            gs_rx_queue(&rec);
            continue;
        }
        if (can_stream_running()) {
            can_stream_push(bus, rec.id, ext, rtr, rec.len, rec.data);
            continue;
        }
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
    if (s_can_owner[bus - 1u] == CAN_OWNER_GS) {
        /* SocketCAN has it: `ip link set canN down` releases it. */
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    s_can_owner[bus - 1u] = CAN_OWNER_NONE;
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
    s_can_owner[bus - 1u] = CAN_OWNER_SCPI;
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
    if (s_can_owner[bus - 1u] != CAN_OWNER_SCPI ||
        can_transmit(can_ports[bus - 1u], id, id > 0x7FFu, false,
                     (uint8_t)(hlen / 2u), data) < 0) {
        /* Not opened by SCPI, or all three TX mailboxes still pending. */
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

/* CAN:STATe? <bus> -> "<owner>,<tec>,<rec>,<busoff>,<rx dropped>"
 * owner: 0 closed, 1 SCPI (CAN:OPEN), 2 SocketCAN (gs_usb). */
static scpi_result_t cmd_can_state(scpi_t *ctx) {
    uint32_t bus;
    char line[48];
    if (!can_param_bus(ctx, &bus)) return SCPI_RES_ERR;
    uint32_t esr = CAN_ESR(can_ports[bus - 1u]);
    int n = snprintf(line, sizeof(line), "%u,%lu,%lu,%u,%lu",
                     (unsigned)s_can_owner[bus - 1u],
                     (unsigned long)((esr >> 16) & 0xFFu),
                     (unsigned long)((esr >> 24) & 0xFFu),
                     (esr & CAN_ESR_BOFF) ? 1u : 0u,
                     (unsigned long)s_can_dropped);
    SCPI_ResultCharacters(ctx, line, (size_t)n);
    return SCPI_RES_OK;
}

/* ---------- SCPI command descriptor (enables SYSTem:HELP:DESCription?) ---------- */

/* SYSTem:STReam:STARt needs a bus to listen on. */
static bool can_stream_any_bus_open(void) {
    return s_can_owner[0] == CAN_OWNER_SCPI || s_can_owner[1] == CAN_OWNER_SCPI;
}

/* Commands declared once (.agents/standards/scpi-commands.md). LED and GPIO are
 * settings; CAN:OPEN/SEND are actions; received frames stream through
 * SYSTem:STReam (can_stream.h) until STOP, so there is no capture job. The
 * per-colour LED headers and LED:ALL collapse into LED <index>,<value>;
 * LED:SET/GET?, BTN? and the pop-style CAN:RECV?/COUNt? stay as undescribed
 * aliases. */
static const usbscpi_param_desc_t led_params[] = {
    USBSCPI_PARAM("led", "u32", true),
    USBSCPI_PARAM("value", "bool", true),
};
static const usbscpi_param_desc_t led_q_params[] = {
    USBSCPI_PARAM("led", "u32", true),
};
static const usbscpi_param_desc_t gpio_set_params[] = {
    USBSCPI_PARAM("pin", "u32", true),
    USBSCPI_PARAM("value", "bool", true),
};
static const usbscpi_param_desc_t pin_params[] = {
    USBSCPI_PARAM("pin", "u32", true),
};
static const usbscpi_param_desc_t can_bus_params[] = {
    USBSCPI_PARAM("bus", "u32", true),
};
static const usbscpi_param_desc_t can_open_params[] = {
    USBSCPI_PARAM("bus", "u32", true),
    USBSCPI_PARAM("bitrate", "u32", true),
};
static const usbscpi_param_desc_t can_send_params[] = {
    USBSCPI_PARAM("bus", "u32", true),
    USBSCPI_PARAM("id", "u32", true),
    USBSCPI_PARAM("data", "string", false),
};

#define STM_COMMANDS(CMD, ALIAS)                                                          \
    CMD("LED",   cmd_led_set, "command", "Set LED by index (0=green,1=orange,2=red,3=blue)", \
        USBSCPI_PARAMS(led_params), "none")                                               \
    CMD("LED?",  cmd_led_get, "query",   "Read LED state by index",                       \
        USBSCPI_PARAMS(led_q_params), "u32")                                              \
    CMD("LED:TOGgle", cmd_led_toggle, "command", "Toggle LED by index",                   \
        USBSCPI_PARAMS(led_q_params), "none")                                             \
    CMD("BUTTon?", cmd_btn, "query", "Read the user button (PA0), 1=pressed",             \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("GPIO",  cmd_gpio_set, "command", "Set a GPIOA pin output level",                 \
        USBSCPI_PARAMS(gpio_set_params), "none")                                          \
    CMD("GPIO?", cmd_gpio_get, "query",   "Read a GPIOA pin input level",                 \
        USBSCPI_PARAMS(pin_params), "u32")                                                \
    CMD("CAN:OPEN", cmd_can_open, "command",                                              \
        "Start CAN bus 1 (PD0/PD1) or 2 (PB12/PB13) at 125k/250k/500k/1M",                \
        USBSCPI_PARAMS(can_open_params), "none")                                          \
    CMD("CAN:SEND", cmd_can_send, "command", "Send one frame; id > 0x7FF is extended, data is hex", \
        USBSCPI_PARAMS(can_send_params), "none")                                          \
    CMD("CAN:STATe?", cmd_can_state, "query", "owner(0 closed,1 SCPI,2 SocketCAN),tec,rec,busoff,rx_dropped",           \
        USBSCPI_PARAMS(can_bus_params), "string")                                         \
    CAN_STREAM_COMMANDS(CMD)                                                              \
    ALIAS("LED:SET",    cmd_led_set)                                                      \
    ALIAS("LED:GET?",   cmd_led_get)                                                      \
    ALIAS("BTN?",       cmd_btn)                                                          \
    ALIAS("CAN:RECV?",  cmd_can_recv)                                                     \
    ALIAS("CAN:COUNt?", cmd_can_count)
USBSCPI_DEFINE_COMMANDS(stm, STM_COMMANDS);

static const usbscpi_descriptor_t s_descriptor = {
    .commands = stm_desc_commands,
    .command_count = USBSCPI_COUNT(stm_desc_commands),
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

/* ---------- SocketCAN: Linux gs_usb (candleLight) protocol ----------
 * Vendor interface 0. The kernel's gs_usb driver binds to it after
 *   echo 1209 0001 ff > /sys/bus/usb/drivers/gs_usb/new_id
 * and exposes the two controllers as can0 (CAN1) and can1 (CAN2). Control
 * requests carry the channel in wValue; frames travel one per bulk transfer.
 * Every frame the host sends must come back on bulk IN with its echo_id once
 * it has left the controller, or the driver runs out of TX slots. */

enum {
    GS_REQ_HOST_FORMAT   = 0,
    GS_REQ_BITTIMING     = 1,
    GS_REQ_MODE          = 2,
    GS_REQ_BT_CONST      = 4,
    GS_REQ_DEVICE_CONFIG = 5,
};

#define GS_MODE_RESET        0u
#define GS_MODE_START        1u
#define GS_FEAT_LISTEN_ONLY  (1u << 0)
#define GS_FEAT_LOOP_BACK    (1u << 1)

enum { GS_TX_FREE, GS_TX_SENT, GS_TX_DONE };

typedef struct {
    uint32_t sjw, tseg1, tseg2, brp;
} gs_timing_t;

static const uint32_t mbox_off[3]  = { CAN_MBOX0, CAN_MBOX1, CAN_MBOX2 };
static const uint32_t mbox_tme[3]  = { CAN_TSR_TME0, CAN_TSR_TME1, CAN_TSR_TME2 };
static const uint32_t mbox_rqcp[3] = { CAN_TSR_RQCP0, CAN_TSR_RQCP1, CAN_TSR_RQCP2 };

/* 500 kbit/s from 42 MHz, the same timing CAN:OPEN uses, until BITTIMING. */
static gs_timing_t     s_gs_timing[2] = { { 1, 11, 2, 6 }, { 1, 11, 2, 6 } };
/* One echo slot per TX mailbox: freed only after its echo reaches the host. */
static gs_host_frame_t s_gs_tx[2][3];
static uint8_t         s_gs_tx_state[2][3];
static gs_host_frame_t s_gs_out;        /* host frame waiting for a mailbox */
static bool            s_gs_out_valid;
static uint32_t        s_gs_dropped_seen[2];

static void gs_stop(uint8_t ch) {
    uint32_t port = can_ports[ch];
    can_disable_irq(port, CAN_IER_FMPIE0);
    s_can_owner[ch] = CAN_OWNER_NONE;
    CAN_TSR(port) = CAN_TSR_ABRQ0 | CAN_TSR_ABRQ1 | CAN_TSR_ABRQ2;
    CAN_MCR(port) |= CAN_MCR_INRQ;  /* off the bus */
    memset(s_gs_tx_state[ch], GS_TX_FREE, sizeof(s_gs_tx_state[ch]));
    if (s_gs_out_valid && s_gs_out.channel == ch) s_gs_out_valid = false;
}

/* Takes the controller from SCPI if needed: the host asked for it last. */
static bool gs_start(uint8_t ch, uint32_t flags) {
    const gs_timing_t *t = &s_gs_timing[ch];
    gs_stop(ch);
    /* TXFP: send in host order, as SocketCAN expects. */
    if (can_init(can_ports[ch], false, true, false, false, false, true,
                 (t->sjw - 1u) << CAN_BTR_SJW_SHIFT,
                 (t->tseg1 - 1u) << CAN_BTR_TS1_SHIFT,
                 (t->tseg2 - 1u) << CAN_BTR_TS2_SHIFT, t->brp,
                 (flags & GS_FEAT_LOOP_BACK) != 0u,
                 (flags & GS_FEAT_LISTEN_ONLY) != 0u) != 0) {
        return false;  /* usually no transceiver */
    }
    can_filter_id_mask_32bit_init(can_filter_nr[ch], 0, 0, 0, true);
    s_gs_dropped_seen[ch] = s_gs_dropped[ch];
    s_can_owner[ch] = CAN_OWNER_GS;
    can_enable_irq(can_ports[ch], CAN_IER_FMPIE0);
    return true;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *req) {
    static uint32_t buf[10];  /* largest payload: BT_CONST, 10 words */
    uint16_t ch = req->wValue;

    if (req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE) return false;
    /* gs_usb is interface 0. Refusing the stream interface keeps the gs_usb
     * driver, bound by class 0xFF, from probing it as a third CAN device. */
    if (tu_u16_low(req->wIndex) != 0u) return false;

    if (stage == CONTROL_STAGE_SETUP) {
        switch (req->bRequest) {
        case GS_REQ_BITTIMING:
        case GS_REQ_MODE:
            if (ch >= 2u) return false;
            /* fall through */
        case GS_REQ_HOST_FORMAT:  /* byte order: always little-endian here */
            if (req->wLength > sizeof(buf)) return false;
            return tud_control_xfer(rhport, req, buf, req->wLength);
        case GS_REQ_BT_CONST:
            if (ch >= 2u) return false;
            buf[0] = GS_FEAT_LISTEN_ONLY | GS_FEAT_LOOP_BACK;
            buf[1] = rcc_apb1_frequency;  /* 42 MHz */
            buf[2] = 1; buf[3] = 16;      /* tseg1 */
            buf[4] = 1; buf[5] = 8;       /* tseg2 */
            buf[6] = 4;                   /* sjw max */
            buf[7] = 1; buf[8] = 1024;    /* brp */
            buf[9] = 1;                   /* brp step */
            return tud_control_xfer(rhport, req, buf, 40);
        case GS_REQ_DEVICE_CONFIG:
            buf[0] = 1u << 24;  /* reserved[3], icount = channels - 1 */
            buf[1] = 1;         /* sw_version */
            buf[2] = 1;         /* hw_version */
            return tud_control_xfer(rhport, req, buf, 12);
        default:
            return false;
        }
    }

    if (stage != CONTROL_STAGE_DATA ||
        req->bmRequestType_bit.direction != TUSB_DIR_OUT) {
        return true;
    }
    switch (req->bRequest) {
    case GS_REQ_BITTIMING: {  /* prop_seg, phase_seg1, phase_seg2, sjw, brp */
        if (req->wLength < 20u) return false;
        gs_timing_t t = { buf[3], buf[0] + buf[1], buf[2], buf[4] };
        if (t.tseg1 < 1u || t.tseg1 > 16u || t.tseg2 < 1u || t.tseg2 > 8u ||
            t.sjw < 1u || t.sjw > 4u || t.brp < 1u || t.brp > 1024u) {
            return false;
        }
        s_gs_timing[ch] = t;
        return true;
    }
    case GS_REQ_MODE:  /* mode, flags */
        if (req->wLength < 8u) return false;
        if (buf[0] == GS_MODE_START) return gs_start((uint8_t)ch, buf[1]);
        if (s_can_owner[ch] == CAN_OWNER_GS) gs_stop((uint8_t)ch);
        return true;
    default:
        return true;
    }
}

static void gs_transmit(uint8_t ch, uint8_t mb, const gs_host_frame_t *f) {
    uint32_t port = can_ports[ch], off = mbox_off[mb];
    uint32_t tir = (f->can_id & GS_CAN_EFF_FLAG)
        ? ((f->can_id & 0x1FFFFFFFu) << CAN_TIxR_EXID_SHIFT) | CAN_TIxR_IDE
        : (f->can_id & 0x7FFu) << CAN_TIxR_STID_SHIFT;
    if (f->can_id & GS_CAN_RTR_FLAG) tir |= CAN_TIxR_RTR;
    CAN_TDTxR(port, off) = f->can_dlc > 8u ? 8u : f->can_dlc;
    CAN_TDLxR(port, off) = (uint32_t)f->data[0] | (uint32_t)f->data[1] << 8 |
                           (uint32_t)f->data[2] << 16 | (uint32_t)f->data[3] << 24;
    CAN_TDHxR(port, off) = (uint32_t)f->data[4] | (uint32_t)f->data[5] << 8 |
                           (uint32_t)f->data[6] << 16 | (uint32_t)f->data[7] << 24;
    CAN_TIxR(port, off) = tir | CAN_TIxR_TXRQ;
    s_gs_tx[ch][mb] = *f;
    s_gs_tx_state[ch][mb] = GS_TX_SENT;
}

/* Called from the main loop. Picks its own mailbox rather than using
 * can_transmit(): a mailbox the hardware has emptied may still hold an echo
 * the host has not received, and a new request would clear its RQCP. */
static void gs_poll(void) {
    if (!tud_vendor_mounted()) return;

    for (uint8_t ch = 0; ch < 2u; ch++) {
        if (s_can_owner[ch] != CAN_OWNER_GS) continue;
        uint32_t port = can_ports[ch];
        for (uint8_t mb = 0; mb < 3u; mb++) {
            /* RQCP: the frame went out (bxCAN retries until it does). */
            if (s_gs_tx_state[ch][mb] == GS_TX_SENT &&
                (CAN_TSR(port) & mbox_rqcp[mb])) {
                CAN_TSR(port) = mbox_rqcp[mb];  /* write-1-to-clear */
                s_gs_tx_state[ch][mb] = GS_TX_DONE;
            }
        }
    }

    /* Host -> bus. Frames are fixed-size, so the FIFO stays aligned. */
    if (!s_gs_out_valid && tud_vendor_available() >= sizeof(s_gs_out)) {
        tud_vendor_read(&s_gs_out, sizeof(s_gs_out));
        s_gs_out_valid = true;
    }
    if (s_gs_out_valid) {
        uint8_t ch = s_gs_out.channel;
        if (ch >= 2u || s_can_owner[ch] != CAN_OWNER_GS) {
            s_gs_out_valid = false;  /* channel went down: nothing to echo to */
        } else {
            uint32_t tsr = CAN_TSR(can_ports[ch]);
            for (uint8_t mb = 0; mb < 3u; mb++) {
                if (s_gs_tx_state[ch][mb] == GS_TX_FREE && (tsr & mbox_tme[mb])) {
                    gs_transmit(ch, mb, &s_gs_out);
                    s_gs_out_valid = false;
                    break;
                }
            }
        }
    }

    /* Bus -> host, echoes first. Only into an empty FIFO: two frames in one
     * transfer would overflow the driver's 20-byte receive buffer. */
    if (tud_vendor_write_available() < CFG_TUD_VENDOR_TX_BUFSIZE) return;
    gs_host_frame_t f;
    for (uint8_t ch = 0; ch < 2u; ch++) {
        for (uint8_t mb = 0; mb < 3u; mb++) {
            if (s_gs_tx_state[ch][mb] == GS_TX_DONE) {
                tud_vendor_write(&s_gs_tx[ch][mb], sizeof(f));
                tud_vendor_write_flush();
                s_gs_tx_state[ch][mb] = GS_TX_FREE;
                return;
            }
        }
    }
    while (usbscpi_ring_count(&s_gs_ring) >= sizeof(f)) {
        usbscpi_ring_read(&s_gs_ring, (uint8_t *)&f, sizeof(f));
        if (s_can_owner[f.channel] != CAN_OWNER_GS) continue;  /* queued before stop */
        uint32_t dropped = s_gs_dropped[f.channel];
        if (dropped != s_gs_dropped_seen[f.channel]) {
            f.flags |= GS_FLAG_OVERFLOW;
            s_gs_dropped_seen[f.channel] = dropped;
        }
        tud_vendor_write(&f, sizeof(f));
        tud_vendor_write_flush();
        return;
    }
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
    usbscpi_ring_init(&s_gs_ring, s_gs_ring_mem, sizeof(s_gs_ring_mem));
    /* Below SysTick (priority 0), which timestamps frames in these ISRs. */
    nvic_set_priority(NVIC_CAN1_RX0_IRQ, 1u << 4);
    nvic_set_priority(NVIC_CAN2_RX0_IRQ, 1u << 4);
    nvic_set_priority(NVIC_OTG_FS_IRQ, 1u << 4);
    can_stream_init();
    nvic_enable_irq(NVIC_CAN1_RX0_IRQ);
    nvic_enable_irq(NVIC_CAN2_RX0_IRQ);

    /* Enable USB OTG FS interrupt */
    nvic_enable_irq(NVIC_OTG_FS_IRQ);
}

/* Unplugged or reset by the host: the next host starts from an idle stream. */
void tud_umount_cb(void) { can_stream_reset(); }

/* ---------- Main ---------- */
int main(void) {
    board_init();

    tusb_init();

    /* *IDN? carries the same chip-unique serial as the USB descriptor. */
    static char idn[64];
    snprintf(idn, sizeof(idn), "IoTSploit,STM32F4-Disco-SocketCAN,%s,0.1.0", board_serial());

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
    usbscpi_register(dev, stm_scpi_commands);

    while (1) {
        tud_task();
        usbscpi_task(dev);
        can_stream_pump();
        gs_poll();
    }
}
