#include <stdio.h>
#include <string.h>

#include "tusb.h"
#include "usbscpi/usbscpi.h"
#include "usbscpi_tinyusb.h"
#include "ble_scan_handler.h"
#include "ble_conn_handler.h"
#include "nrfx_power.h"
#include "nrfx_clock.h"
#include "app_error.h"
#include "nrf_soc.h"
#include "nrf_sdh_soc.h"

/* ---------- Static buffers (no dynamic allocation) ---------- */
static uint8_t s_storage[2048];
static char    s_line[96];
/* io_buf doubles as the SYST:HELP:DESC? render buffer. With the scan + connect +
   pair command set and three workflows the rendered line-record text is ~2 KiB,
   so size it well above both that and the 256-byte mtu. */
static uint8_t s_io[4096];

/* ---------- USB TX callback via TinyUSB glue ---------- */
static int usb_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user; (void)eom;
    return usbscpi_tinyusb_tx(NULL, data, len, true);
}

/* ---------- BLE SCPI command callbacks ---------- */
static scpi_result_t cmd_ble_scan_start(scpi_t *ctx) {
    (void)ctx;
    ble_scan_start();
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_scan_stop(scpi_t *ctx) {
    (void)ctx;
    ble_scan_stop();
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_scan_state(scpi_t *ctx) {
    return SCPI_ResultUInt32(ctx, ble_scan_is_scanning() ? 1 : 0);
}

static scpi_result_t cmd_ble_scan_count(scpi_t *ctx) {
    return SCPI_ResultUInt32(ctx, ble_scan_count());
}

/* Format scan result #index as a CSV row and return it, or push a range error.
 * Shared by BLE:SCAN:RESult? and the ESP32-dialect BLE:SCAN? alias. */
static scpi_result_t scan_row_reply(scpi_t *ctx, uint32_t index) {
    ble_scan_result_t r;
    if (!ble_scan_get_result((uint16_t)index, &r)) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }

    /* Format: "AA:BB:CC:DD:EE:FF,-67,DeviceName,C" where the trailing field is
     * "C" for a connectable (pairable) advertiser or "N" otherwise. Hosts use it
     * to offer only pairable devices to BLE:CONNect/BLE:CPAIR. */
    char buf[80];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X,%d,%s,%c",
             r.addr[5], r.addr[4], r.addr[3], r.addr[2], r.addr[1], r.addr[0],
             r.rssi, r.name[0] ? r.name : "(unknown)",
             r.adv_type == 0 ? 'C' : 'N');
    return SCPI_ResultCharacters(ctx, buf, strlen(buf));
}

static scpi_result_t cmd_ble_scan_result(scpi_t *ctx) {
    uint32_t index = 0;
    if (SCPI_ParamUInt32(ctx, &index, TRUE) != TRUE) return SCPI_RES_ERR;
    return scan_row_reply(ctx, index);
}

static scpi_result_t cmd_ble_scan_clear(scpi_t *ctx) {
    (void)ctx;
    ble_scan_clear();
    return SCPI_RES_OK;
}

/* ---------- ESP32-dialect scan aliases (for the unmodified iotsploit-ui) ---------- */
/* The iotsploit-ui BLE panel drives scans as a timed operation:
 *   BLE:SCAN <secs>  ->  poll BLE:SCAN:DONE?  ->  BLE:SCAN? <index>
 * The nRF's native interface is start/stop with a BLE:SCAN:STATe? poll, so these
 * three aliases bridge the dialect without any host/UI change. They are kept out
 * of the descriptor on purpose: the descriptor advertises the native ble-scan
 * workflow, and the UI does not read the descriptor. */

static scpi_result_t cmd_ble_scan_timed(scpi_t *ctx) {
    uint32_t secs = 5;                             /* default when omitted */
    (void)SCPI_ParamUInt32(ctx, &secs, FALSE);
    ble_scan_clear();                              /* each BLE:SCAN starts fresh */
    ble_scan_start_timed((uint16_t)(secs > 600 ? 600 : secs));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_scan_done(scpi_t *ctx) {
    /* 1 = scan finished, 0 = still scanning (inverse of BLE:SCAN:STATe?). */
    return SCPI_ResultUInt32(ctx, ble_scan_is_scanning() ? 0 : 1);
}

static scpi_result_t cmd_ble_scan_query(scpi_t *ctx) {
    uint32_t index = 0;
    if (SCPI_ParamUInt32(ctx, &index, TRUE) != TRUE) return SCPI_RES_ERR;
    return scan_row_reply(ctx, index);
}

/* nRF uses legacy pairing (no numeric comparison), so these never occur in a
 * normal flow. They exist only so the UI's confirm/numcmp buttons return cleanly
 * instead of raising an undefined-header error if pressed. */
static scpi_result_t cmd_ble_confirm_stub(scpi_t *ctx) {
    (void)ctx;
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_numcmp_stub(scpi_t *ctx) {
    return SCPI_ResultUInt32(ctx, 0);
}

/* ---------- BLE connect / pair SCPI command callbacks ---------- */

static scpi_result_t cmd_ble_conn(scpi_t *ctx) {
    uint32_t idx = 0;
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    return ble_conn_start((size_t)idx) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_conn_state(scpi_t *ctx) {
    return SCPI_ResultInt32(ctx, ble_conn_state());
}

static scpi_result_t cmd_ble_conn_status(scpi_t *ctx) {
    return SCPI_ResultInt32(ctx, ble_conn_last_status());
}

static scpi_result_t cmd_ble_cpair(scpi_t *ctx) {
    uint32_t idx = 0;
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    return ble_connpair_start((size_t)idx) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_cpair_state(scpi_t *ctx) {
    return SCPI_ResultInt32(ctx, ble_connpair_state());
}

static scpi_result_t cmd_ble_auto(scpi_t *ctx) {
    /* Optional name-filter argument; empty means "first connectable device". */
    char filter[BLE_NAME_MAX_LEN + 1] = {0};
    size_t copied = 0;
    (void)SCPI_ParamCopyText(ctx, filter, sizeof(filter), &copied, FALSE);
    return ble_auto_start(copied ? filter : NULL) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_auto_state(scpi_t *ctx) {
    return SCPI_ResultInt32(ctx, ble_auto_state());
}

static scpi_result_t cmd_ble_disconnect(scpi_t *ctx) {
    (void)ctx;
    ble_conn_disconnect();
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_pair(scpi_t *ctx) {
    return ble_pair_start() == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_pair_state(scpi_t *ctx) {
    return SCPI_ResultInt32(ctx, ble_pair_state());
}

static scpi_result_t cmd_ble_passkey(scpi_t *ctx) {
    uint32_t key = 0;
    if (SCPI_ParamUInt32(ctx, &key, TRUE) != TRUE) return SCPI_RES_ERR;
    return ble_pair_passkey(key) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_passkey_get(scpi_t *ctx) {
    uint32_t key = 0;
    if (ble_pair_passkey_get(&key) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%06lu", (unsigned long)key);
    return SCPI_ResultCharacters(ctx, buf, strlen(buf));
}

static scpi_result_t cmd_ble_sec(scpi_t *ctx) {
    char buf[64];
    if (ble_sec_info(buf, sizeof(buf)) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_ResultCharacters(ctx, buf, strlen(buf));
}

static const scpi_command_t ble_commands[] = {
    { "BLE:SCAN:START",   cmd_ble_scan_start,  0 },
    { "BLE:SCAN:STOP",    cmd_ble_scan_stop,    0 },
    { "BLE:SCAN:STATe?",  cmd_ble_scan_state,   0 },
    { "BLE:SCAN:COUNt?",  cmd_ble_scan_count,   0 },
    { "BLE:SCAN:RESult?", cmd_ble_scan_result,  0 },
    { "BLE:SCAN:CLEar",   cmd_ble_scan_clear,   0 },
    /* ESP32-dialect scan aliases for the unmodified iotsploit-ui BLE panel. */
    { "BLE:SCAN",         cmd_ble_scan_timed,   0 },
    { "BLE:SCAN:DONE?",   cmd_ble_scan_done,    0 },
    { "BLE:SCAN?",        cmd_ble_scan_query,   0 },
    { "BLE:CONNect",         cmd_ble_conn,        0 },
    { "BLE:CONNect:STATe?",  cmd_ble_conn_state,  0 },
    { "BLE:CONNect:STATus?", cmd_ble_conn_status, 0 },
    { "BLE:CPAIR",           cmd_ble_cpair,       0 },
    { "BLE:CPAIR:STATe?",    cmd_ble_cpair_state, 0 },
    { "BLE:AUTO",            cmd_ble_auto,        0 },
    { "BLE:AUTO:STATe?",     cmd_ble_auto_state,  0 },
    { "BLE:DISConnect",      cmd_ble_disconnect,  0 },
    { "BLE:PAIR",            cmd_ble_pair,        0 },
    { "BLE:PAIR:STATe?",     cmd_ble_pair_state,  0 },
    { "BLE:PAIR:PASSKey",    cmd_ble_passkey,     0 },
    { "BLE:PAIR:PASSKey?",   cmd_ble_passkey_get, 0 },
    /* Compatibility stubs: nRF legacy pairing has no numeric comparison. */
    { "BLE:PAIR:CONFirm",    cmd_ble_confirm_stub, 0 },
    { "BLE:PAIR:NUMCmp?",    cmd_ble_numcmp_stub,  0 },
    { "BLE:SEC?",            cmd_ble_sec,         0 },
    SCPI_CMD_LIST_END
};

/* ---------- Descriptor metadata (SYSTem:HELP:DESCription?) ---------- */
/* The device is the single source of truth for its command/workflow metadata:
   the host fetches this over SYST:HELP:DESC? instead of carrying a local copy. */

static const usbscpi_param_desc_t desc_ble_scan_result_params[] = {
    { "index", "u32", true },
};
/* The connect/connect-pair index is a row in the BLE scan-results table, so it
 * advertises an options source: hosts populate a picker from BLE:SCAN results
 * (and can offer only connectable "C" rows) instead of asking for a raw index. */
static const usbscpi_param_desc_t desc_ble_conn_params[] = {
    { "index", "u32", true, "BLE:SCAN:COUNt?", "BLE:SCAN:RESult?" },
};
static const usbscpi_param_desc_t desc_ble_passkey_params[] = {
    { "key", "u32", true },
};
static const usbscpi_param_desc_t desc_ble_auto_params[] = {
    { "filter", "string", false },
};

static const usbscpi_command_desc_t desc_commands[] = {
    { "BLE:SCAN:START",  "command", "Start BLE scanning",
      NULL, 0, NULL },
    { "BLE:SCAN:STOP",   "command", "Stop BLE scanning",
      NULL, 0, NULL },
    { "BLE:SCAN:STATe?", "query",   "1 = scanning, 0 = idle/finished",
      NULL, 0, "bool" },
    { "BLE:SCAN:COUNt?", "query",   "Number of BLE devices found",
      NULL, 0, "u32" },
    { "BLE:SCAN:RESult?", "query",  "Get scan result by index (addr,rssi,name,conn) where conn is C=connectable N=not",
      desc_ble_scan_result_params, 1, "string" },
    { "BLE:SCAN:CLEar",  "command", "Clear scan results",
      NULL, 0, NULL },
    { "BLE:CONNect",         "command", "Connect to BLE scan result by index",
      desc_ble_conn_params, 1, NULL },
    { "BLE:CONNect:STATe?",  "query",   "0=idle 1=connecting 2=connected 3=failed",
      NULL, 0, "u32" },
    { "BLE:CONNect:STATus?", "query",   "Last GAP/SMP status code",
      NULL, 0, "u32" },
    { "BLE:CPAIR",           "command", "Connect to BLE scan result by index and pair in one step",
      desc_ble_conn_params, 1, NULL },
    { "BLE:CPAIR:STATe?",    "query",   "0=idle 1=connecting 2=pairing 3=passkey 4=display 5=done 6=failed",
      NULL, 0, "u32" },
    { "BLE:AUTO",            "command", "Scan, pick a connectable device (optional name filter), connect and pair",
      desc_ble_auto_params, 1, NULL },
    { "BLE:AUTO:STATe?",     "query",   "0=idle 1=scanning 2=connecting 3=pairing 4=passkey 5=display 6=done 7=failed",
      NULL, 0, "u32" },
    { "BLE:DISConnect",      "command", "Disconnect active BLE connection",
      NULL, 0, NULL },
    { "BLE:PAIR",            "command", "Initiate BLE pairing on the connection",
      NULL, 0, NULL },
    { "BLE:PAIR:STATe?",     "query",   "0=idle 1=in-progress 2=passkey-needed 3=done 4=failed 5=display-key",
      NULL, 0, "u32" },
    { "BLE:PAIR:PASSKey",    "command", "Enter the 6-digit passkey shown on the peer",
      desc_ble_passkey_params, 1, NULL },
    { "BLE:PAIR:PASSKey?",   "query",   "Get the passkey to enter on the peer",
      NULL, 0, "string" },
    { "BLE:SEC?",            "query",   "Security info: mac,level,encrypted,authenticated,bonded,key_size",
      NULL, 0, "string" },
};

static const char *const desc_cpair_failed[] = { "6" };
static const char *const desc_auto_failed[]  = { "7" };

/* Connect+pair prompts keyed by BLE:CPAIR:STATe? (see enum in ble_conn_handler.h):
 *   3 passkey  -> host types the passkey the peer displays
 *   4 display  -> device shows BLE:PAIR:PASSKey? for the user to enter on the peer */
static const usbscpi_prompt_desc_t desc_cpair_prompts[] = {
    { "3", "passkey", "BLE:PAIR:PASSKey", NULL },
    { "4", "display", NULL,               "BLE:PAIR:PASSKey?" },
};
/* Same prompts for the fully automatic workflow, keyed by BLE:AUTO:STATe?. */
static const usbscpi_prompt_desc_t desc_auto_prompts[] = {
    { "4", "passkey", "BLE:PAIR:PASSKey", NULL },
    { "5", "display", NULL,               "BLE:PAIR:PASSKey?" },
};

static const usbscpi_workflow_desc_t desc_workflows[] = {
    {
        .name = "ble-scan",
        .type = "trigger_poll_fetch",
        .summary = "Scan for BLE devices",
        .trigger_cmd = "BLE:SCAN:START",
        .done_query = "BLE:SCAN:STATe?",
        .done_value = "0",
        .count_query = "BLE:SCAN:COUNt?",
        .fetch_query = "BLE:SCAN:RESult?",
        .state_query = NULL,
        .success_value = NULL,
        .failed_values = NULL,
        .failed_value_count = 0,
        .timeout_ms = 30000,
        .poll_ms = 500,
    },
    {
        .name = "ble-connect-pair",
        .type = "trigger_poll_interactive",
        .summary = "Connect to a scanned BLE device and pair in one step",
        .trigger_cmd = "BLE:CPAIR",
        .state_query = "BLE:CPAIR:STATe?",
        .success_value = "5",
        .failed_values = desc_cpair_failed,
        .failed_value_count = 1,
        .prompts = desc_cpair_prompts,
        .prompt_count = sizeof(desc_cpair_prompts) / sizeof(desc_cpair_prompts[0]),
        .result_query = "BLE:SEC?",
        .timeout_ms = 45000,
        .poll_ms = 200,
    },
    {
        .name = "ble-auto",
        .type = "trigger_poll_interactive",
        .summary = "Scan, find a pairable device, connect, pair, and report security level",
        .trigger_cmd = "BLE:AUTO",
        .state_query = "BLE:AUTO:STATe?",
        .success_value = "6",
        .failed_values = desc_auto_failed,
        .failed_value_count = 1,
        .prompts = desc_auto_prompts,
        .prompt_count = sizeof(desc_auto_prompts) / sizeof(desc_auto_prompts[0]),
        .result_query = "BLE:SEC?",
        .timeout_ms = 60000,
        .poll_ms = 300,
    },
};

static const usbscpi_descriptor_t s_descriptor = {
    .commands = desc_commands,
    .command_count = sizeof(desc_commands) / sizeof(desc_commands[0]),
    .workflows = desc_workflows,
    .workflow_count = sizeof(desc_workflows) / sizeof(desc_workflows[0]),
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

/* nrfx_clock event sink. The POWER and CLOCK peripherals share one IRQ, so the
   combined nrfx_power_clock_irq_handler also runs nrfx_clock_irq_handler. The
   TinyUSB nRF driver starts HFCLK via the raw HAL, which sets the HFCLKSTARTED
   event; without an initialized nrfx_clock handler that path calls a NULL
   callback and HardFaults. A no-op handler safely absorbs the event. */
static void clock_event_handler(nrfx_clock_evt_type_t event) { (void)event; }

/* ---------- nRF USB VBUS power events -> TinyUSB ---------- */
/* nRF52840 USBD is powered by the USB regulator; TinyUSB only enables the
   peripheral when it receives these VBUS events. SoftDevice is currently
   disabled, so nrfx_power drives the USB regulator directly. */
extern void tusb_hal_nrf_power_event(uint32_t event);

static void power_usb_event_handler(nrfx_power_usb_evt_t event) {
    switch (event) {
        case NRFX_POWER_USB_EVT_DETECTED: tusb_hal_nrf_power_event(0); break; /* DETECTED */
        case NRFX_POWER_USB_EVT_REMOVED:  tusb_hal_nrf_power_event(1); break; /* REMOVED  */
        case NRFX_POWER_USB_EVT_READY:    tusb_hal_nrf_power_event(2); break; /* READY    */
        default: break;
    }
}

extern uint32_t __isr_vector;  /* application vector table base (0x27000) */

/* USB VBUS power events while the SoftDevice is enabled. POWER is then owned
   by the SoftDevice, so the events arrive as SoC events instead of via
   nrfx_power; forward them to TinyUSB the same way. */
static void usb_soc_evt_handler(uint32_t sys_evt, void *p_context) {
    (void)p_context;
    switch (sys_evt) {
        case NRF_EVT_POWER_USB_DETECTED:    tusb_hal_nrf_power_event(0); break;
        case NRF_EVT_POWER_USB_REMOVED:     tusb_hal_nrf_power_event(1); break;
        case NRF_EVT_POWER_USB_POWER_READY: tusb_hal_nrf_power_event(2); break;
        default: break;
    }
}
NRF_SDH_SOC_OBSERVER(m_usb_soc_obs, 0, usb_soc_evt_handler, NULL);

/* ---------- USBD interrupt -> TinyUSB ---------- */
/* TinyUSB provides dcd_int_handler() but does NOT define the USBD_IRQHandler
   vector; without this the weak Default_Handler (infinite loop) runs on the
   first USB interrupt and the device hangs before enumerating. */
void USBD_IRQHandler(void) {
    tud_int_handler(0);
}

/* ---------- Main ---------- */
int main(void) {
    /* Bring up BLE/SoftDevice first; once enabled it owns CLOCK and POWER, so
       USB clock/power must then be driven through SoftDevice APIs. */
    bool sd_on = (ble_scan_init() == 0);

    if (sd_on) {
        /* SoftDevice owns POWER: subscribe to USB VBUS events as SoC events
           (routed to usb_soc_evt_handler). The current state is kicked after
           tusb_init() below. */
        sd_power_usbdetected_enable(1);
        sd_power_usbpwrrdy_enable(1);
        sd_power_usbremoved_enable(1);
    } else {
        /* No SoftDevice: drive POWER/CLOCK directly via nrfx. nrfx_clock must be
           initialized so the shared POWER_CLOCK IRQ has a valid clock callback
           (TinyUSB raw HFCLK start sets the HFCLKSTARTED event). */
        APP_ERROR_CHECK(nrfx_clock_init(clock_event_handler));
        nrfx_clock_enable();
        static const nrfx_power_config_t pwr_cfg = { 0 };
        APP_ERROR_CHECK(nrfx_power_init(&pwr_cfg));
        static const nrfx_power_usbevt_config_t usbevt_cfg = { .handler = power_usb_event_handler };
        nrfx_power_usbevt_init(&usbevt_cfg);
        nrfx_power_usbevt_enable();
    }

    /* 1. Init TinyUSB (USBTMC device) first — USB works even if BLE fails */
    tusb_init();

    /* If the SoftDevice is up and VBUS was already present at boot, the
       DETECTED/READY edges predate our subscription, so kick TinyUSB now
       (must be after tusb_init so the device stack is ready). */
    if (sd_on) {
        uint32_t usbreg = 0;
        if (sd_power_usbregstatus_get(&usbreg) == NRF_SUCCESS) {
            if (usbreg & POWER_USBREGSTATUS_VBUSDETECT_Msk) tusb_hal_nrf_power_event(0);
            if (usbreg & POWER_USBREGSTATUS_OUTPUTRDY_Msk)  tusb_hal_nrf_power_event(2);
        }
    }

    /* 2. Init usbscpi core */
    usbscpi_config_t cfg = {
        .usb_tx        = usb_tx,
        .line_buf      = s_line,
        .line_buf_len  = sizeof(s_line),
        .max_block_len = 4096,
        .idn           = "IoTSploit,nRF52840,0001,0.1.0",
        .io_buf        = s_io,
        .io_buf_len    = sizeof(s_io),
        .proto         = 1,
        .mtu           = 256,
        .descriptor    = &s_descriptor,
    };

    usbscpi_t *dev = usbscpi_init(s_storage, sizeof(s_storage), &cfg);
    usbscpi_tinyusb_bind(dev);
    usbscpi_register(dev, ble_commands);

    /* BLE/SoftDevice was already initialized at the top of main(). Register the
     * connect/pair BLE observer (harmless if the SoftDevice failed to start —
     * connect/pair commands then simply error). */
    ble_conn_init();

    /* 4. Main loop: pump USB + BLE events + the BLE:AUTO scan/select step */
    while (1) {
        tud_task();
        usbscpi_task(dev);
        ble_conn_task();
    }
}
