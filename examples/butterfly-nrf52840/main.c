/**
 * @file main.c
 * @brief butterfly-over-USBTMC firmware entry point (nRF52840, Path A / TinyUSB).
 *
 * Drives a bare-metal BLE advertising sniffer (radio_ble + ble_sniff_handler)
 * over the iotsploit-usb core: USBTMC transport + SCPI command path. butterfly's
 * native transport (USB CDC-ACM + the WHAD protobuf protocol) is dropped; the
 * device is driven by SCPI, and capture is modeled as a bounded
 * trigger_poll_fetch window so the existing descriptor-driven host + UI run it
 * with zero host-side changes. No SoftDevice — the radio is owned bare-metal.
 */

#include <stdio.h>
#include <string.h>

#include "tusb.h"
#include "usbscpi/usbscpi.h"
#include "usbscpi_tinyusb.h"
#include "ble_sniff_handler.h"
#include "radio_ble.h"
#include "nrfx_power.h"
#include "nrfx_clock.h"
#include "app_error.h"

const char *board_serial(void); /* usb_descriptors.c */

/* ---------- Static buffers (no dynamic allocation) ---------- */
static uint8_t s_storage[2048];
static char    s_line[128];
/* io_buf doubles as the SYST:HELP:DESC? render buffer; the sniff command set +
   one workflow renders well under 2 KiB. */
static uint8_t s_io[4096];

/* ---------- USB TX callback via TinyUSB glue ---------- */
static int usb_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user; (void)eom;
    return usbscpi_tinyusb_tx(NULL, data, len, true);
}

/* ---------- Hex helpers for BLE:INJect ---------- */
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode an even-length hex string into bytes. Returns byte count, or -1 on a
 * malformed/odd-length/oversized input. */
static int hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_cap) {
    if (hex_len % 2 != 0 || hex_len / 2 > out_cap) return -1;
    for (size_t i = 0; i < hex_len; i += 2) {
        int hi = hex_nibble(hex[i]);
        int lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(hex_len / 2);
}

/* ---------- BLE sniff SCPI command callbacks ---------- */

/* BLE:SNIFf [<secs>[,<channel>]] — start a timed advertising-capture window. */
static scpi_result_t cmd_ble_sniff(scpi_t *ctx) {
    uint32_t secs = 5;
    uint32_t channel = 0; /* 0 = keep current channel */
    (void)SCPI_ParamUInt32(ctx, &secs, FALSE);
    (void)SCPI_ParamUInt32(ctx, &channel, FALSE);
    ble_sniff_start((uint16_t)(secs > 600 ? 600 : secs), (uint8_t)channel);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_sniff_stop(scpi_t *ctx) {
    (void)ctx;
    ble_sniff_stop();
    return SCPI_RES_OK;
}

/* 1 = window finished, 0 = still capturing. */
static scpi_result_t cmd_ble_sniff_done(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, ble_sniff_is_active() ? 0 : 1);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_sniff_count(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, ble_sniff_count());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_sniff_packet(scpi_t *ctx) {
    uint32_t index = 0;
    if (SCPI_ParamUInt32(ctx, &index, TRUE) != TRUE) return SCPI_RES_ERR;

    char row[160];
    size_t n = ble_sniff_format_row((uint16_t)index, row, sizeof(row));
    if (n == 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    SCPI_ResultCharacters(ctx, row, n);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_sniff_dropped(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, ble_sniff_dropped());
    return SCPI_RES_OK;
}

/* BLE:CHANnel <n> — select the advertising channel while idle. */
static scpi_result_t cmd_ble_channel_set(scpi_t *ctx) {
    uint32_t ch = 0;
    if (SCPI_ParamUInt32(ctx, &ch, TRUE) != TRUE) return SCPI_RES_ERR;
    if (ch != 37 && ch != 38 && ch != 39) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    ble_sniff_set_channel((uint8_t)ch);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_channel_get(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, ble_sniff_get_channel());
    return SCPI_RES_OK;
}

/* BLE:INJect <hex> — transmit a raw advertising PDU (hex text, no binary block). */
static scpi_result_t cmd_ble_inject(scpi_t *ctx) {
    char hex[2 * (RADIO_BLE_PDU_MAX + 2) + 1] = {0};
    size_t copied = 0;
    if (SCPI_ParamCopyText(ctx, hex, sizeof(hex), &copied, TRUE) != TRUE) {
        return SCPI_RES_ERR;
    }
    uint8_t pdu[RADIO_BLE_PDU_MAX + 2];
    int len = hex_decode(hex, copied, pdu, sizeof(pdu));
    if (len < 2) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_INVALID_STRING_DATA);
        return SCPI_RES_ERR;
    }
    if (ble_sniff_inject(pdu, (uint8_t)len) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

/* BLE:SNIFf is a job (.agents/standards/scpi-commands.md): STARt, then STATe?
 * reports one word, mapping ble_sniff_is_active(). The channel stays a
 * parameter of STARt, as before. */
static scpi_result_t cmd_ble_sniff(scpi_t *ctx);
static bool s_sniff_started;

static scpi_result_t cmd_ble_sniff_start_job(scpi_t *ctx) {
    scpi_result_t r = cmd_ble_sniff(ctx);
    if (r == SCPI_RES_OK) s_sniff_started = true;
    return r;
}

static scpi_result_t cmd_ble_sniff_state(scpi_t *ctx) {
    SCPI_ResultMnemonic(ctx, !s_sniff_started ? "IDLE" : ble_sniff_is_active() ? "RUNNING" : "DONE");
    return SCPI_RES_OK;
}

static const usbscpi_param_desc_t sniff_start_params[] = {
    USBSCPI_PARAM("duration", "u32", false),
    USBSCPI_PARAM("channel", "u32", false),
};
static const usbscpi_param_desc_t sniff_fetch_params[] = {
    USBSCPI_PARAM_PICK("index", "BLE:SNIFf:COUNt?", "BLE:SNIFf:FETCh?"),
};
static const usbscpi_param_desc_t channel_params[] = {
    USBSCPI_PARAM("channel", "u32", true),
};
static const usbscpi_param_desc_t inject_params[] = {
    USBSCPI_PARAM("data", "string", true),
};

/* ALIAS entries are the names from before the command standard. */
#define BUTTERFLY_COMMANDS(CMD, ALIAS)                                                    \
    CMD("BLE:SNIFf:STARt",   cmd_ble_sniff_start_job, "command",                          \
        "Capture advertising PDUs for N seconds (optional channel 37/38/39)",             \
        USBSCPI_PARAMS(sniff_start_params), "none")                                       \
    CMD("BLE:SNIFf:STOP",    cmd_ble_sniff_stop,  "command", "Stop the capture early",    \
        USBSCPI_NO_PARAMS, "none")                                                        \
    CMD("BLE:SNIFf:STATe?",  cmd_ble_sniff_state, "query", "IDLE, RUNNING or DONE",       \
        USBSCPI_NO_PARAMS, "string")                                                      \
    CMD("BLE:SNIFf:COUNt?",  cmd_ble_sniff_count, "query", "Buffered advertising PDUs",   \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("BLE:SNIFf:FETCh?",  cmd_ble_sniff_packet, "query",                               \
        "Captured PDU by index (time_us,channel,rssi,length,pdu_hex)",                    \
        USBSCPI_PARAMS(sniff_fetch_params), "string")                                     \
    CMD("BLE:SNIFf:DROPped?", cmd_ble_sniff_dropped, "query", "PDUs dropped to a full ring", \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("BLE:CHANnel",       cmd_ble_channel_set, "command", "Select advertising channel (37/38/39)", \
        USBSCPI_PARAMS(channel_params), "none")                                           \
    CMD("BLE:CHANnel?",      cmd_ble_channel_get, "query", "Current advertising channel", \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("BLE:INJect",        cmd_ble_inject,     "command", "Transmit a raw advertising PDU (hex)", \
        USBSCPI_PARAMS(inject_params), "none")                                            \
    ALIAS("BLE:SNIFf",       cmd_ble_sniff)                                               \
    ALIAS("BLE:SNIFf:DONE?", cmd_ble_sniff_done)                                          \
    ALIAS("BLE:SNIFf:PACKet?", cmd_ble_sniff_packet)
USBSCPI_DEFINE_COMMANDS(butterfly, BUTTERFLY_COMMANDS);

static const usbscpi_workflow_desc_t desc_workflows[] = {
    { USBSCPI_WF_ACQUIRE("ble-sniff", "BLE:SNIFf",
                         "Capture BLE advertising packets for a timed window",
                         "time:u32:us,channel:u32,rssi:i32:dbm,length:u32,pdu:string", 30000) },
};

static const usbscpi_descriptor_t s_descriptor = {
    .commands = butterfly_desc_commands,
    .command_count = USBSCPI_COUNT(butterfly_desc_commands),
    .workflows = desc_workflows,
    .workflow_count = USBSCPI_COUNT(desc_workflows),
};

/* ---------- TinyUSB USBTMC required callbacks ---------- */
#if CFG_TUD_USBTMC_ENABLE_488
usbtmc_response_capabilities_488_t const *tud_usbtmc_get_capabilities_cb(void) {
    static usbtmc_response_capabilities_488_t caps = {
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

/* nrfx_clock event sink. The POWER and CLOCK peripherals share one IRQ, so the
   combined handler also runs nrfx_clock_irq_handler; a no-op absorbs the
   HFCLKSTARTED event TinyUSB triggers via the raw HAL. */
static void clock_event_handler(nrfx_clock_evt_type_t event) { (void)event; }

/* ---------- nRF USB VBUS power events -> TinyUSB ---------- */
extern void tusb_hal_nrf_power_event(uint32_t event);

static void power_usb_event_handler(nrfx_power_usb_evt_t event) {
    switch (event) {
        case NRFX_POWER_USB_EVT_DETECTED: tusb_hal_nrf_power_event(0); break;
        case NRFX_POWER_USB_EVT_REMOVED:  tusb_hal_nrf_power_event(1); break;
        case NRFX_POWER_USB_EVT_READY:    tusb_hal_nrf_power_event(2); break;
        default: break;
    }
}

/* ---------- USBD interrupt -> TinyUSB ---------- */
/* TinyUSB provides dcd_int_handler() but does not define the USBD_IRQHandler
   vector; without this the weak Default_Handler runs on the first USB interrupt
   and the device hangs before enumerating. */
void USBD_IRQHandler(void) {
    tud_int_handler(0);
}

/* ---------- Main ---------- */
int main(void) {
    /* No SoftDevice: drive POWER/CLOCK directly via nrfx so USB VBUS events reach
       TinyUSB and the shared POWER_CLOCK IRQ has a valid clock callback. */
    APP_ERROR_CHECK(nrfx_clock_init(clock_event_handler));
    nrfx_clock_enable();
    static const nrfx_power_config_t pwr_cfg = { 0 };
    APP_ERROR_CHECK(nrfx_power_init(&pwr_cfg));
    static const nrfx_power_usbevt_config_t usbevt_cfg = { .handler = power_usb_event_handler };
    nrfx_power_usbevt_init(&usbevt_cfg);
    nrfx_power_usbevt_enable();

    /* 1. Init TinyUSB (USBTMC device). */
    tusb_init();

    /* 2. Bring up the bare-metal radio + capture buffer. */
    ble_sniff_init();

    /* 3. Init usbscpi core. */
    /* *IDN? carries the same chip-unique serial as the USB descriptor. */
    static char idn[64];
    snprintf(idn, sizeof(idn), "IoTSploit,butterfly-nRF52840,%s,0.1.0", board_serial());

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
    usbscpi_register(dev, butterfly_scpi_commands);

    /* 4. Main loop: pump USB + drain the capture window. */
    while (1) {
        tud_task();
        usbscpi_task(dev);
        ble_sniff_task();
    }
}
