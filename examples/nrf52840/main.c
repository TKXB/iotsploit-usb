#include <stdio.h>
#include <string.h>

#include "tusb.h"
#include "usbscpi/usbscpi.h"
#include "usbscpi_tinyusb.h"
#include "ble_scan_handler.h"
#include "ble_conn_handler.h"
#include "ble_periph_handler.h"
#include "nrfx_power.h"
#include "nrfx_clock.h"
#include "app_error.h"
#include "nrf_soc.h"
#include "nrf_sdh_soc.h"

const char *board_serial(void); /* usb_descriptors.c */

/* ---------- Static buffers (no dynamic allocation) ---------- */
static uint8_t s_storage[2048];
static char    s_line[96];
/* io_buf doubles as the SYST:HELP:DESC? render buffer. With the scan, connect,
   pair and peripheral command sets and four workflows the rendered line-record
   text is ~4.3 KiB. The render fails outright once it outgrows io_buf or
   max_block_len, and USB needs the TinyUSB glue's TX buffer to hold the whole
   reply (USBSCPI_TINYUSB_TX_BUF_SIZE, set in the Makefile): keep all three at
   the same size. */
#define DESC_BUF_SIZE 8192
static uint8_t s_io[DESC_BUF_SIZE];

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

static scpi_result_t cmd_ble_scan_count(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, ble_scan_count());
    return SCPI_RES_OK;
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
    SCPI_ResultCharacters(ctx, buf, strlen(buf));
    return SCPI_RES_OK;
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

/* ---------- Old timed-scan aliases ----------
 * Before the command standard the scan was driven as BLE:SCAN <secs> -> poll
 * BLE:SCAN:DONE? -> BLE:SCAN? <index>. These keep that working; the job's
 * BLE:SCAN:STARt reuses cmd_ble_scan_timed directly. */

static scpi_result_t cmd_ble_scan_timed(scpi_t *ctx) {
    uint32_t secs = 5;                             /* default when omitted */
    (void)SCPI_ParamUInt32(ctx, &secs, FALSE);
    ble_scan_clear();                              /* each BLE:SCAN starts fresh */
    ble_scan_start_timed((uint16_t)(secs > 600 ? 600 : secs));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_scan_done(scpi_t *ctx) {
    /* 1 = scan finished, 0 = still scanning (inverse of BLE:SCAN:STATe?). */
    SCPI_ResultUInt32(ctx, ble_scan_is_scanning() ? 0 : 1);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_scan_query(scpi_t *ctx) {
    uint32_t index = 0;
    if (SCPI_ParamUInt32(ctx, &index, TRUE) != TRUE) return SCPI_RES_ERR;
    return scan_row_reply(ctx, index);
}

/* ---------- BLE connect / pair SCPI command callbacks ---------- */

static scpi_result_t cmd_ble_conn(scpi_t *ctx) {
    uint32_t idx = 0;
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    return ble_conn_start((size_t)idx) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_conn_status(scpi_t *ctx) {
    SCPI_ResultInt32(ctx, ble_conn_last_status());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_cpair(scpi_t *ctx) {
    uint32_t idx = 0;
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    return ble_connpair_start((size_t)idx) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_cpair_state(scpi_t *ctx) {
    SCPI_ResultInt32(ctx, ble_connpair_state());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_auto(scpi_t *ctx) {
    /* Optional name-filter argument; empty means "first connectable device". */
    char filter[BLE_NAME_MAX_LEN + 1] = {0};
    size_t copied = 0;
    (void)SCPI_ParamCopyText(ctx, filter, sizeof(filter), &copied, FALSE);
    return ble_auto_start(copied ? filter : NULL) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_auto_state(scpi_t *ctx) {
    SCPI_ResultInt32(ctx, ble_auto_state());
    return SCPI_RES_OK;
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
    SCPI_ResultInt32(ctx, ble_pair_state());
    return SCPI_RES_OK;
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
    SCPI_ResultCharacters(ctx, buf, strlen(buf));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_sec(scpi_t *ctx) {
    char buf[64];
    if (ble_sec_info(buf, sizeof(buf)) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultCharacters(ctx, buf, strlen(buf));
    return SCPI_RES_OK;
}

/* ---------- BLE peripheral SCPI command callbacks ---------- */

static scpi_result_t cmd_ble_adv_start(scpi_t *ctx) {
    uint32_t io = 4;                               /* KeyboardDisplay when omitted */
    (void)SCPI_ParamUInt32(ctx, &io, FALSE);
    if (io > 4 || ble_periph_start((uint8_t)io) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_adv_stop(scpi_t *ctx) {
    (void)ctx;
    ble_periph_stop();
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_periph_status(scpi_t *ctx) {
    SCPI_ResultInt32(ctx, ble_periph_last_status());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_periph_passkey(scpi_t *ctx) {
    uint32_t key = 0;
    if (SCPI_ParamUInt32(ctx, &key, TRUE) != TRUE) return SCPI_RES_ERR;
    return ble_periph_passkey(key) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_periph_passkey_get(scpi_t *ctx) {
    uint32_t key = 0;
    if (ble_periph_passkey_get(&key) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%06lu", (unsigned long)key);
    SCPI_ResultCharacters(ctx, buf, strlen(buf));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_periph_sec(scpi_t *ctx) {
    char buf[128];
    if (ble_periph_sec_info(buf, sizeof(buf)) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultCharacters(ctx, buf, strlen(buf));
    return SCPI_RES_OK;
}

/* ---------- Jobs (.agents/standards/scpi-commands.md) ----------
 * BLE:SCAN and BLE:CONNect are jobs: STARt, then STATe? reports one word. These
 * map the scanner's is-scanning flag and the connect/pair state machines
 * (ble_conn_handler.h) onto those words; the machines are unchanged. nRF uses
 * legacy pairing, so there is no CONFIRM state. */

static scpi_result_t cmd_ble_scan_timed(scpi_t *ctx);
static bool s_scan_started;

static scpi_result_t cmd_ble_scan_start_job(scpi_t *ctx) {
    scpi_result_t r = cmd_ble_scan_timed(ctx);   /* clear + timed scan */
    if (r == SCPI_RES_OK) s_scan_started = true;
    return r;
}

static scpi_result_t cmd_ble_scan_state_word(scpi_t *ctx) {
    SCPI_ResultMnemonic(ctx, !s_scan_started ? "IDLE" : ble_scan_is_scanning() ? "RUNNING" : "DONE");
    return SCPI_RES_OK;
}

/* BLE:CONNect:STARt <index>[,<pair>]: connect to scan result #index and, unless
 * pair is 0, pair in the same job. Matches the ESP32-S3, so a host drives both
 * boards the same way. */
static bool s_conn_pair;
static bool s_conn_failure_reported;

static scpi_result_t cmd_ble_connect_start(scpi_t *ctx) {
    uint32_t idx = 0, pair = 1;
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    (void)SCPI_ParamUInt32(ctx, &pair, FALSE);
    s_conn_pair = pair != 0;
    s_conn_failure_reported = false;
    int rc = s_conn_pair ? ble_connpair_start((size_t)idx) : ble_conn_start((size_t)idx);
    return rc == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static const char *connect_state_word(void) {
    if (s_conn_pair) {
        switch (ble_connpair_state()) {
        case BLE_CP_CONNECTING:
        case BLE_CP_PAIRING: return "RUNNING";
        case BLE_CP_PASSKEY: return "PASSKEY";
        case BLE_CP_DISPLAY: return "DISPLAY";
        case BLE_CP_DONE:    return "DONE";
        case BLE_CP_FAILED:  return "FAILED";
        default:             return "IDLE";
        }
    }
    switch (ble_conn_state()) {
    case BLE_CONN_CONNECTING: return "RUNNING";
    case BLE_CONN_CONNECTED:  return "DONE";
    case BLE_CONN_FAILED:     return "FAILED";
    default:                  return "IDLE";
    }
}

static scpi_result_t cmd_ble_connect_state_word(scpi_t *ctx) {
    const char *word = connect_state_word();
    if (strcmp(word, "FAILED") == 0 && !s_conn_failure_reported) {
        s_conn_failure_reported = true;
        usbscpi_queue_error(ctx, SCPI_ERROR_EXECUTION_ERROR);
    }
    SCPI_ResultMnemonic(ctx, word);
    return SCPI_RES_OK;
}

/* BLE:PERiph is a job: STARt advertises, STATe? maps the peripheral state
 * machine (ble_periph_handler.h) to the standard words. io picks the pairing
 * I/O capability. */
static bool s_periph_failure_reported;

static scpi_result_t cmd_ble_periph_start(scpi_t *ctx) {
    uint32_t io = 4;
    (void)SCPI_ParamUInt32(ctx, &io, FALSE);
    s_periph_failure_reported = false;
    if (io > 4 || ble_periph_start((uint8_t)io) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_periph_state_word(scpi_t *ctx) {
    const char *word;
    switch (ble_periph_state()) {
    case BLE_PERIPH_ADVERTISING:
    case BLE_PERIPH_CONNECTED:
    case BLE_PERIPH_PAIRING: word = "RUNNING"; break;
    case BLE_PERIPH_PASSKEY: word = "PASSKEY"; break;
    case BLE_PERIPH_DISPLAY: word = "DISPLAY"; break;
    case BLE_PERIPH_DONE:    word = "DONE";    break;
    case BLE_PERIPH_FAILED:  word = "FAILED";  break;
    default:                 word = "IDLE";    break;
    }
    if (strcmp(word, "FAILED") == 0 && !s_periph_failure_reported) {
        s_periph_failure_reported = true;
        usbscpi_queue_error(ctx, SCPI_ERROR_EXECUTION_ERROR);
    }
    SCPI_ResultMnemonic(ctx, word);
    return SCPI_RES_OK;
}

/* ---------- Commands: declared once, described for hosts ---------- */

static const usbscpi_param_desc_t scan_start_params[] = {
    USBSCPI_PARAM("duration", "u32", false),
};
static const usbscpi_param_desc_t scan_fetch_params[] = {
    USBSCPI_PARAM_PICK("index", "BLE:SCAN:COUNt?", "BLE:SCAN:FETCh?"),
};
static const usbscpi_param_desc_t connect_params[] = {
    USBSCPI_PARAM_PICK("index", "BLE:SCAN:COUNt?", "BLE:SCAN:FETCh?"),
    USBSCPI_PARAM("pair", "bool", false),
};
static const usbscpi_param_desc_t key_params[] = {
    USBSCPI_PARAM("key", "u32", true),
};
static const usbscpi_param_desc_t io_params[] = {
    USBSCPI_PARAM("io", "u32", false),
};

/* ALIAS entries keep the names used before the command standard working — the
 * native start/stop scan controls, the ESP32-dialect scan the iotsploit-ui used,
 * and the separate connect/cpair/auto/pair commands. They are not described. */
#define NRF_COMMANDS(CMD, ALIAS)                                                          \
    CMD("BLE:SCAN:STARt",    cmd_ble_scan_start_job, "command",                           \
        "Clear results, then scan for N seconds (default 5)",                             \
        USBSCPI_PARAMS(scan_start_params), "none")                                        \
    CMD("BLE:SCAN:STOP",     cmd_ble_scan_stop,  "command", "Stop scanning",              \
        USBSCPI_NO_PARAMS, "none")                                                        \
    CMD("BLE:SCAN:STATe?",   cmd_ble_scan_state_word, "query", "IDLE, RUNNING or DONE",   \
        USBSCPI_NO_PARAMS, "string")                                                      \
    CMD("BLE:SCAN:COUNt?",   cmd_ble_scan_count, "query", "BLE devices found",            \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("BLE:SCAN:FETCh?",   cmd_ble_scan_result, "query",                                \
        "BLE device by index (addr,rssi,name,conn; conn C=connectable N=not)",            \
        USBSCPI_PARAMS(scan_fetch_params), "string")                                      \
    CMD("BLE:SCAN:CLEar",    cmd_ble_scan_clear, "command", "Forget scan results",        \
        USBSCPI_NO_PARAMS, "none")                                                        \
    CMD("BLE:CONNect:STARt", cmd_ble_connect_start, "command",                            \
        "Connect to a scanned device and pair (pair=0 to only connect)",                  \
        USBSCPI_PARAMS(connect_params), "none")                                           \
    CMD("BLE:CONNect:STATe?", cmd_ble_connect_state_word, "query",                        \
        "IDLE, RUNNING, PASSKEY, DISPLAY, DONE or FAILED", USBSCPI_NO_PARAMS, "string")   \
    CMD("BLE:DISConnect",    cmd_ble_disconnect, "command", "Drop the BLE connection",    \
        USBSCPI_NO_PARAMS, "none")                                                        \
    CMD("BLE:PAIR:PASSKey",  cmd_ble_passkey,    "command", "Enter the passkey shown on the peer", \
        USBSCPI_PARAMS(key_params), "none")                                               \
    CMD("BLE:PAIR:PASSKey?", cmd_ble_passkey_get, "query", "Passkey to enter on the peer", \
        USBSCPI_NO_PARAMS, "string")                                                      \
    CMD("BLE:SEC?",          cmd_ble_sec,        "query",                                 \
        "mac,level,encrypted,authenticated,bonded,key_size", USBSCPI_NO_PARAMS, "string") \
    CMD("BLE:PERiph:STARt",  cmd_ble_periph_start, "command",                             \
        "Advertise, then pair with the central that connects (io: 0-4 I/O capability)",   \
        USBSCPI_PARAMS(io_params), "none")                                                \
    CMD("BLE:PERiph:STOP",   cmd_ble_adv_stop,   "command", "Stop advertising",           \
        USBSCPI_NO_PARAMS, "none")                                                        \
    CMD("BLE:PERiph:STATe?", cmd_ble_periph_state_word, "query",                          \
        "IDLE, RUNNING, PASSKEY, DISPLAY, DONE or FAILED", USBSCPI_NO_PARAMS, "string")   \
    CMD("BLE:PERiph:PASSKey", cmd_ble_periph_passkey, "command",                          \
        "Enter the passkey the central shows", USBSCPI_PARAMS(key_params), "none")        \
    CMD("BLE:PERiph:PASSKey?", cmd_ble_periph_passkey_get, "query",                       \
        "Passkey to enter on the central", USBSCPI_NO_PARAMS, "string")                   \
    CMD("BLE:PERiph:SEC?",   cmd_ble_periph_sec, "query",                                 \
        "mac,method,lesc,sec_mode,sec_level,encrypted,authenticated,bonded,key_size",     \
        USBSCPI_NO_PARAMS, "string")                                                      \
    ALIAS("BLE:SCAN:START",   cmd_ble_scan_start)                                         \
    ALIAS("BLE:SCAN:RESult?", cmd_ble_scan_result)                                        \
    ALIAS("BLE:SCAN",         cmd_ble_scan_timed)                                         \
    ALIAS("BLE:SCAN:DONE?",   cmd_ble_scan_done)                                          \
    ALIAS("BLE:SCAN?",        cmd_ble_scan_query)                                         \
    ALIAS("BLE:CONNect",      cmd_ble_conn)                                               \
    ALIAS("BLE:CONNect:STATus?", cmd_ble_conn_status)                                     \
    ALIAS("BLE:CPAIR",        cmd_ble_cpair)                                              \
    ALIAS("BLE:CPAIR:STATe?", cmd_ble_cpair_state)                                        \
    ALIAS("BLE:AUTO",         cmd_ble_auto)                                               \
    ALIAS("BLE:AUTO:STATe?",  cmd_ble_auto_state)                                         \
    ALIAS("BLE:PAIR",         cmd_ble_pair)                                               \
    ALIAS("BLE:PAIR:STATe?",  cmd_ble_pair_state)                                         \
    ALIAS("BLE:ADV:STARt",    cmd_ble_adv_start)                                          \
    ALIAS("BLE:ADV:STOP",     cmd_ble_adv_stop)                                           \
    ALIAS("BLE:PERiph:STATus?", cmd_ble_periph_status)
USBSCPI_DEFINE_COMMANDS(nrf, NRF_COMMANDS);

/* ---------- Workflows ---------- */

static const usbscpi_prompt_desc_t connect_prompts[] = {
    USBSCPI_PROMPT_PASSKEY("BLE:PAIR:PASSKey"),
    USBSCPI_PROMPT_DISPLAY("BLE:PAIR:PASSKey?"),
};
static const char *const connect_old_names[] = { "ble-connect-pair", "ble-auto" };
static const usbscpi_prompt_desc_t periph_prompts[] = {
    USBSCPI_PROMPT_PASSKEY("BLE:PERiph:PASSKey"),
    USBSCPI_PROMPT_DISPLAY("BLE:PERiph:PASSKey?"),
};

static const usbscpi_workflow_desc_t desc_workflows[] = {
    { USBSCPI_WF_ACQUIRE("ble-scan", "BLE:SCAN", "Scan for BLE devices",
                         "addr:mac,rssi:i32:dbm,name:string,conn:string", 30000) },
    { USBSCPI_WF_INTERACTIVE("ble-connect", "BLE:CONNect",
                             "Connect to a scanned device and pair", 45000),
      USBSCPI_WF_PROMPTS(connect_prompts),
      USBSCPI_WF_RESULT("BLE:SEC?",
                        "mac:mac,level:string,encrypted:bool,authenticated:bool,bonded:bool,key_size:u32"),
      USBSCPI_WF_RENAMED_FROM(connect_old_names) },
    { USBSCPI_WF_INTERACTIVE("ble-peripheral", "BLE:PERiph",
                             "Advertise as a peripheral; report how a phone or PC paired", 120000),
      USBSCPI_WF_PROMPTS(periph_prompts),
      USBSCPI_WF_RESULT("BLE:PERiph:SEC?",
                        "mac:mac,method:string,lesc:bool,sec_mode:u32,sec_level:u32,encrypted:bool,authenticated:bool,bonded:bool,key_size:u32") },
};

static const usbscpi_descriptor_t s_descriptor = {
    .commands = nrf_desc_commands,
    .command_count = USBSCPI_COUNT(nrf_desc_commands),
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
    /* *IDN? carries the same chip-unique serial as the USB descriptor. */
    static char idn[64];
    snprintf(idn, sizeof(idn), "IoTSploit,nRF52840,%s,0.1.0", board_serial());

    usbscpi_config_t cfg = {
        .usb_tx        = usb_tx,
        .line_buf      = s_line,
        .line_buf_len  = sizeof(s_line),
        .max_block_len = DESC_BUF_SIZE,
        .idn           = idn,
        .io_buf        = s_io,
        .io_buf_len    = sizeof(s_io),
        .proto         = 1,
        .mtu           = 256,
        .descriptor    = &s_descriptor,
    };

    usbscpi_t *dev = usbscpi_init(s_storage, sizeof(s_storage), &cfg);
    usbscpi_tinyusb_bind(dev);
    usbscpi_register(dev, nrf_scpi_commands);

    /* BLE/SoftDevice was already initialized at the top of main(). Register the
     * connect/pair BLE observer (harmless if the SoftDevice failed to start —
     * connect/pair commands then simply error). */
    ble_conn_init();
    if (sd_on) {
        ble_periph_init();
    }

    /* 4. Main loop: pump USB + BLE events + the BLE:AUTO scan/select step */
    while (1) {
        tud_task();
        usbscpi_task(dev);
        ble_conn_task();
    }
}
