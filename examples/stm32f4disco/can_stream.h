#ifndef STM32F4DISCO_CAN_STREAM_H
#define STM32F4DISCO_CAN_STREAM_H

/*
 * CAN data plane over the USB vendor pipe, shared by examples/stm32f4disco and
 * examples/stm32f4disco-socketcan.
 *
 * SCPI configures (CAN:OPEN) and starts/stops the capture
 * (SYSTem:STReam:STARt / :STOP); every received frame is then sent as one
 * record on the stream interface's bulk-IN endpoint, in the envelope of
 * examples/esp32s3/main/usb_frame.h: [u8 type=0x02][u8 0][u16 len LE] record.
 * There is no snapshot and nothing to poll: a capture lasts until STOP and is
 * as long as the host keeps reading. Framing is off until the host sends
 * SYSTem:STReam:FRAMing 1, the same handshake as the ESP32-S3.
 *
 * Include once, from main.c, after <libopencm3/...>, tusb.h and
 * usbscpi/ring_buffer.h, with CAN_STREAM_VENDOR set to the TinyUSB vendor
 * instance of the stream interface. The RX ISR calls can_stream_push() while
 * can_stream_running(); the main loop calls can_stream_pump().
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/systick.h>

#ifndef CAN_STREAM_VENDOR
#error "define CAN_STREAM_VENDOR (TinyUSB vendor instance of the stream interface)"
#endif

/* Record: little-endian, 32 bytes. Field names and the first five fields
 * match examples/can/can_rec.h; classic CAN only, so data is 8 bytes, and
 * `bus` says which controller (1 or 2) received it. */
#define CAN_STREAM_VERSION 1u
#define CAN_STREAM_F_EFF   0x01u /* 29-bit identifier */
#define CAN_STREAM_F_RTR   0x02u /* remote request    */

typedef struct {
    uint64_t ts_us;    /* microseconds since boot, taken in the RX ISR */
    uint64_t dropped;  /* records lost to a full ring since STARt, at capture */
    uint32_t can_id;   /* identifier, flag bits masked off */
    uint8_t  len;      /* 0..8 */
    uint8_t  flags;    /* CAN_STREAM_F_* */
    uint8_t  bus;      /* 1 = CAN1, 2 = CAN2 */
    uint8_t  rsv;
    uint8_t  data[8];
} can_stream_rec_t;

typedef char can_stream_rec_is_32_bytes[sizeof(can_stream_rec_t) == 32u ? 1 : -1];

#define CAN_STREAM_FIELDS \
    "ts_us:u64:us,dropped:u64,can_id:u32:hex,len:u8,flags:u8,bus:u8,rsv:u8,data:bytes8"

#define CAN_STREAM_FRAME_HDR 4u
#define CAN_STREAM_FRAME_REC 0x02u

/* 256 records: about 30 ms of a saturated 1 Mbit/s bus, which covers the
 * gaps between USB transfers. A full ring drops new frames and counts them. */
static uint8_t           s_stream_mem[256 * sizeof(can_stream_rec_t)];
static usbscpi_ring_t    s_stream_ring;
static volatile bool     s_stream_running;
static volatile bool     s_stream_framing;
/* Written only by the RX ISRs, which share one priority and never nest. */
static volatile uint64_t s_stream_count;
static volatile uint64_t s_stream_dropped;

/* ---- Timestamps: SysTick at 1 kHz plus its down-counter for microseconds.
 * SysTick runs above the CAN and USB interrupts, so s_ms is current inside
 * the RX ISR and the re-read below catches a tick between the two reads. */
static volatile uint32_t s_ms;

void sys_tick_handler(void) { s_ms++; }

static uint64_t can_stream_now_us(void) {
    uint32_t ms, val;
    do {
        ms  = s_ms;
        val = systick_get_value();
    } while (ms != s_ms);
    uint32_t reload = systick_get_reload();
    return (uint64_t)ms * 1000u +
           (uint64_t)(reload - val) * 1000u / (reload + 1u);
}

static void can_stream_init(void) {
    usbscpi_ring_init(&s_stream_ring, s_stream_mem, sizeof(s_stream_mem));
    systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
    systick_set_reload(rcc_ahb_frequency / 1000u - 1u);
    systick_clear();
    /* Priority 0 is the highest; the CAN and USB interrupts go below it. */
    nvic_set_priority(NVIC_SYSTICK_IRQ, 0);
    systick_interrupt_enable();
    systick_counter_enable();
}

static bool can_stream_running(void) { return s_stream_running; }

/* RX ISR only. Whole records or nothing: a partial one would misalign every
 * later read. */
static void can_stream_push(uint8_t bus, uint32_t id, bool ext, bool rtr,
                            uint8_t len, const uint8_t *data) {
    can_stream_rec_t r;
    memset(&r, 0, sizeof(r));
    r.ts_us   = can_stream_now_us();
    r.can_id  = id;
    r.len     = len > 8u ? 8u : len;
    r.flags   = (uint8_t)((ext ? CAN_STREAM_F_EFF : 0u) | (rtr ? CAN_STREAM_F_RTR : 0u));
    r.bus     = bus;
    if (!rtr) memcpy(r.data, data, r.len);
    if (usbscpi_ring_free(&s_stream_ring) < sizeof(r)) {
        s_stream_dropped++;
        return;
    }
    r.dropped = s_stream_dropped;
    usbscpi_ring_write(&s_stream_ring, (const uint8_t *)&r, sizeof(r));
    s_stream_count++;
}

/* Main loop. Sends queued records while the host listens; with nobody
 * listening (unmounted, or framing off) it discards them, which is not a drop:
 * the counter measures loss from a capture someone is reading. */
static void can_stream_pump(void) {
    const uint8_t idx = CAN_STREAM_VENDOR;
    if (!tud_vendor_n_mounted(idx) || !s_stream_framing) {
        size_t n = usbscpi_ring_count(&s_stream_ring);
        if (n) usbscpi_ring_advance(&s_stream_ring, n);
        return;
    }
    bool wrote = false;
    while (usbscpi_ring_count(&s_stream_ring) >= sizeof(can_stream_rec_t) &&
           tud_vendor_n_write_available(idx) >=
               CAN_STREAM_FRAME_HDR + sizeof(can_stream_rec_t)) {
        uint8_t frame[CAN_STREAM_FRAME_HDR + sizeof(can_stream_rec_t)] = {
            CAN_STREAM_FRAME_REC, 0u, (uint8_t)sizeof(can_stream_rec_t), 0u
        };
        usbscpi_ring_read(&s_stream_ring, frame + CAN_STREAM_FRAME_HDR,
                          sizeof(can_stream_rec_t));
        tud_vendor_n_write(idx, frame, sizeof(frame));
        wrote = true;
    }
    if (wrote) tud_vendor_n_write_flush(idx);
}

/* A host that vanished (unplug, crash) cannot send STOP; the next one must
 * find the stream idle and unframed rather than mid-capture. */
static void can_stream_reset(void) {
    s_stream_running = false;
    s_stream_framing = false;
}

/* A 64-bit counter the ISRs write, read without tearing. */
static uint64_t can_stream_read_u64(const volatile uint64_t *v) {
    uint64_t a, b;
    do { a = *v; b = *v; } while (a != b);
    return a;
}

/* ---- SCPI: SYSTem:STReam:*, the same headers as the ESP32-S3 ---- */

/* Defined by main.c: whether SCPI owns at least one opened bus. */
static bool can_stream_any_bus_open(void);

static scpi_result_t cmd_stream_start(scpi_t *ctx) {
    if (!can_stream_any_bus_open()) {
        /* Nothing would ever arrive: CAN:OPEN a bus first. */
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    s_stream_running = false;  /* the ISRs stop writing the counters */
    s_stream_count   = 0;
    s_stream_dropped = 0;
    s_stream_running = true;
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_stop(scpi_t *ctx) {
    (void)ctx;
    s_stream_running = false;  /* records already queued are still sent */
    return SCPI_RES_OK;
}

/* "<running>,<attached>": attached means the host turned framing on and the
 * stream interface is configured, i.e. records are being delivered. */
static scpi_result_t cmd_stream_state(scpi_t *ctx) {
    char buf[8];
    int n = snprintf(buf, sizeof(buf), "%d,%d", s_stream_running ? 1 : 0,
                     (s_stream_framing && tud_vendor_n_mounted(CAN_STREAM_VENDOR)) ? 1 : 0);
    SCPI_ResultCharacters(ctx, buf, (size_t)n);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_count(scpi_t *ctx) {
    SCPI_ResultUInt64(ctx, can_stream_read_u64(&s_stream_count));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_dropped(scpi_t *ctx) {
    SCPI_ResultUInt64(ctx, can_stream_read_u64(&s_stream_dropped));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_format(scpi_t *ctx) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "ver=%u,stride=%u,fields=%s",
                     (unsigned)CAN_STREAM_VERSION, (unsigned)sizeof(can_stream_rec_t),
                     CAN_STREAM_FIELDS);
    SCPI_ResultCharacters(ctx, buf, (size_t)n);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_framing(scpi_t *ctx) {
    uint32_t on = 0;
    if (SCPI_ParamUInt32(ctx, &on, TRUE) != TRUE) return SCPI_RES_ERR;
    s_stream_framing = on != 0u;
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_framing_q(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, s_stream_framing ? 1u : 0u);
    return SCPI_RES_OK;
}

static const usbscpi_param_desc_t stream_framing_params[] = {
    USBSCPI_PARAM("value", "bool", true),
};

/* Entries for the board's USBSCPI_DEFINE_COMMANDS list. No PORT?: this board
 * has no network, and a host finds the data plane through FORMat?. */
#define CAN_STREAM_COMMANDS(CMD)                                                   \
    CMD("SYSTem:STReam:STARt", cmd_stream_start, "command",                        \
        "Stream received CAN frames until STOP; CAN:OPEN a bus first",            \
        USBSCPI_NO_PARAMS, "none")                                                 \
    CMD("SYSTem:STReam:STOP", cmd_stream_stop, "command", "Stop the capture",       \
        USBSCPI_NO_PARAMS, "none")                                                 \
    CMD("SYSTem:STReam:STATe?", cmd_stream_state, "query", "running,attached",      \
        USBSCPI_NO_PARAMS, "string")                                               \
    CMD("SYSTem:STReam:COUNt?", cmd_stream_count, "query",                          \
        "Frames captured since STARt", USBSCPI_NO_PARAMS, "u64")                   \
    CMD("SYSTem:STReam:DROPped?", cmd_stream_dropped, "query",                      \
        "Frames lost to a full buffer since STARt", USBSCPI_NO_PARAMS, "u64")      \
    CMD("SYSTem:STReam:FORMat?", cmd_stream_format, "query",                        \
        "Record version, stride, schema", USBSCPI_NO_PARAMS, "string")             \
    CMD("SYSTem:STReam:FRAMing", cmd_stream_framing, "command",                     \
        "Frame the USB vendor pipe (the host sends this)",                         \
        USBSCPI_PARAMS(stream_framing_params), "none")                             \
    CMD("SYSTem:STReam:FRAMing?", cmd_stream_framing_q, "query", "USB framing state", \
        USBSCPI_NO_PARAMS, "u32")

#endif
