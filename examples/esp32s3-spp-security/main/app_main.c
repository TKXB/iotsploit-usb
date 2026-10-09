#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"     /* usb_new_phy */
#include "tusb.h"
#include "usbscpi/usbscpi.h"
#include "usbscpi_tinyusb.h"
#include "ble_spp_security.h"

static const char *TAG = "scpi";

/* ---------- Static buffers (no dynamic allocation) ---------- */
static uint8_t s_storage[2048];
static char    s_line[96];
static uint8_t s_io[4096];

/* ---------- SCPI TX callback: goes through the glue buffered IN path ---------- */
static int usb_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user; (void)eom;
    return usbscpi_tinyusb_tx(NULL, data, len, true);
}

/* ---------- BLE SPP advertising / connection commands ---------- */
static scpi_result_t cmd_ble_adv_start(scpi_t *ctx) {
    (void)ctx;
    ESP_LOGI(TAG, "BLE:ADV:STARt");
    return ble_spp_adv_start() == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_adv_stop(scpi_t *ctx) {
    (void)ctx;
    ESP_LOGI(TAG, "BLE:ADV:STOP");
    return ble_spp_adv_stop() == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_conn_state(scpi_t *ctx) {
    SCPI_ResultInt32(ctx, ble_spp_conn_state());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_conn_status(scpi_t *ctx) {
    SCPI_ResultInt32(ctx, ble_spp_last_status());
    return SCPI_RES_OK;
}

/* ---------- BLE pairing / security commands ---------- */
static scpi_result_t cmd_ble_pair(scpi_t *ctx) {
    (void)ctx;
    ESP_LOGI(TAG, "BLE:PAIR");
    return ble_spp_pair_start() == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_pair_state(scpi_t *ctx) {
    SCPI_ResultInt32(ctx, ble_spp_pair_state());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_pair_passkey(scpi_t *ctx) {
    uint32_t pk = 0;
    if (SCPI_ParamUInt32(ctx, &pk, TRUE) != TRUE) return SCPI_RES_ERR;
    ESP_LOGI(TAG, "BLE:PAIR:PASSKey %06u", (unsigned)pk);
    return ble_spp_pair_passkey(pk) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_pair_passkey_q(scpi_t *ctx) {
    uint32_t pk = 0;
    if (ble_spp_pair_passkey_get(&pk) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultUInt32(ctx, pk);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_numcmp(scpi_t *ctx) {
    uint32_t n = 0;
    if (ble_spp_pair_numcmp_get(&n) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultUInt32(ctx, n);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_confirm(scpi_t *ctx) {
    uint32_t accept = 1;
    (void)SCPI_ParamUInt32(ctx, &accept, FALSE);
    ESP_LOGI(TAG, "BLE:PAIR:CONFirm accept=%u", (unsigned)accept);
    return ble_spp_pair_confirm((int)accept) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_sec(scpi_t *ctx) {
    char buf[64];
    if (ble_spp_sec_info(buf, sizeof(buf)) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultCharacters(ctx, buf, strlen(buf));
    return SCPI_RES_OK;
}

/* ---------- BLE:PERiph job (.agents/standards/scpi-commands.md) ----------
 * STARt advertises; once a central connects, the job pairs on that link. The
 * pairing state machine (ble_spp_security.h) is unchanged: STATe? maps it to
 * the standard words and starts pairing on the first poll after a connect, so
 * the whole flow needs no other command. */
static bool s_periph_running;
static bool s_periph_pair_started;
static bool s_periph_failure_reported;

static scpi_result_t cmd_periph_start(scpi_t *ctx) {
    (void)ctx;
    ESP_LOGI(TAG, "BLE:PERiph:STARt");
    /* A central that is already connected (the old ble-security flow) is
     * paired directly; otherwise advertise and wait for one. */
    if (ble_spp_conn_state() != BLE_SPP_CONNECTED && ble_spp_adv_start() != 0) {
        return SCPI_RES_ERR;
    }
    s_periph_running = true;
    s_periph_pair_started = false;
    s_periph_failure_reported = false;
    return SCPI_RES_OK;
}

static scpi_result_t cmd_periph_stop(scpi_t *ctx) {
    (void)ctx;
    s_periph_running = false;
    return ble_spp_adv_stop() == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static const char *periph_state_word(void) {
    if (!s_periph_running) return "IDLE";
    if (ble_spp_conn_state() != BLE_SPP_CONNECTED) return "RUNNING";
    if (!s_periph_pair_started) {
        s_periph_pair_started = true;
        if (ble_spp_pair_start() != 0) return "FAILED";
        return "RUNNING";
    }
    switch (ble_spp_pair_state()) {
    case BLE_SPP_PAIR_INPUT_NEEDED:  return "PASSKEY";
    case BLE_SPP_PAIR_NUMCMP_NEEDED: return "CONFIRM";
    case BLE_SPP_PAIR_DISPLAY_KEY:   return "DISPLAY";
    case BLE_SPP_PAIR_DONE:          return "DONE";
    case BLE_SPP_PAIR_FAILED:        return "FAILED";
    default:                         return "RUNNING";
    }
}

static scpi_result_t cmd_periph_state(scpi_t *ctx) {
    const char *word = periph_state_word();
    if (strcmp(word, "FAILED") == 0 && !s_periph_failure_reported) {
        s_periph_failure_reported = true;
        ESP_LOGW(TAG, "peripheral pairing failed, status %d", ble_spp_last_status());
        usbscpi_queue_error(ctx, SCPI_ERROR_EXECUTION_ERROR);
    }
    SCPI_ResultMnemonic(ctx, word);
    return SCPI_RES_OK;
}

/* ---------- Commands: declared once, described for hosts ---------- */

static const usbscpi_param_desc_t key_params[] = {
    USBSCPI_PARAM("key", "u32", true),
};
static const usbscpi_param_desc_t accept_params[] = {
    USBSCPI_PARAM("accept", "bool", false),
};

/* ALIAS entries are the names from before the command standard. */
#define SPP_COMMANDS(CMD, ALIAS)                                                         \
    CMD("BLE:PERiph:STARt",  cmd_periph_start, "command",                               \
        "Advertise, then pair with the central that connects", USBSCPI_NO_PARAMS, "none") \
    CMD("BLE:PERiph:STOP",   cmd_periph_stop,  "command", "Stop advertising",            \
        USBSCPI_NO_PARAMS, "none")                                                       \
    CMD("BLE:PERiph:STATe?", cmd_periph_state, "query",                                 \
        "IDLE, RUNNING, PASSKEY, CONFIRM, DISPLAY, DONE or FAILED", USBSCPI_NO_PARAMS, "string") \
    CMD("BLE:PAIR:PASSKey",  cmd_ble_pair_passkey, "command", "Enter the passkey shown on the peer", \
        USBSCPI_PARAMS(key_params), "none")                                              \
    CMD("BLE:PAIR:PASSKey?", cmd_ble_pair_passkey_q, "query", "Passkey to enter on the peer", \
        USBSCPI_NO_PARAMS, "u32")                                                        \
    CMD("BLE:PAIR:NUMCmp?",  cmd_ble_numcmp, "query", "Number to compare with the peer", \
        USBSCPI_NO_PARAMS, "u32")                                                        \
    CMD("BLE:PAIR:CONFirm",  cmd_ble_confirm, "command", "Accept (1) or reject (0) the number", \
        USBSCPI_PARAMS(accept_params), "none")                                           \
    CMD("BLE:SEC?",          cmd_ble_sec, "query", "mac,level,encrypted,authenticated,bonded,key_size", \
        USBSCPI_NO_PARAMS, "string")                                                     \
    ALIAS("BLE:ADV:STARt",       cmd_ble_adv_start)                                      \
    ALIAS("BLE:ADV:STOP",        cmd_ble_adv_stop)                                       \
    ALIAS("BLE:CONNect:STATe?",  cmd_ble_conn_state)                                     \
    ALIAS("BLE:CONNect:STATus?", cmd_ble_conn_status)                                    \
    ALIAS("BLE:PAIR",            cmd_ble_pair)                                           \
    ALIAS("BLE:PAIR:STATe?",     cmd_ble_pair_state)
USBSCPI_DEFINE_COMMANDS(spp, SPP_COMMANDS);

static const usbscpi_prompt_desc_t periph_prompts[] = {
    USBSCPI_PROMPT_PASSKEY("BLE:PAIR:PASSKey"),
    USBSCPI_PROMPT_CONFIRM("BLE:PAIR:CONFirm", "BLE:PAIR:NUMCmp?"),
    USBSCPI_PROMPT_DISPLAY("BLE:PAIR:PASSKey?"),
};
static const char *const periph_old_names[] = { "ble-security" };

static const usbscpi_workflow_desc_t spp_workflows[] = {
    { USBSCPI_WF_INTERACTIVE("ble-peripheral", "BLE:PERiph",
                             "Advertise, pair with the central that connects, report security",
                             120000),
      USBSCPI_WF_PROMPTS(periph_prompts),
      USBSCPI_WF_RESULT("BLE:SEC?",
                        "mac:mac,level:u32,encrypted:bool,authenticated:bool,bonded:bool,key_size:u32"),
      USBSCPI_WF_RENAMED_FROM(periph_old_names) },
};

static const usbscpi_descriptor_t s_descriptor = {
    .commands = spp_desc_commands,
    .command_count = USBSCPI_COUNT(spp_desc_commands),
    .workflows = spp_workflows,
    .workflow_count = USBSCPI_COUNT(spp_workflows),
};

/* ---------- TinyUSB USBTMC required callbacks (glue does not provide these) ---------- */
#if CFG_TUD_USBTMC_ENABLE_488
usbtmc_response_capabilities_488_t const *tud_usbtmc_get_capabilities_cb(void) {
    static usbtmc_response_capabilities_488_t caps = {
        .USBTMC_status = USBTMC_STATUS_SUCCESS,
        .bcdUSBTMC = 0x0100,
        .bmDevCapabilities = {0},
        .bcdUSB488 = 0x0100,
        .bmIntfcCapabilities = {0},
        .bmDevCapabilities488 = {0},
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

/* tud_usbtmc_open_cb is provided by the iotsploit-usb TinyUSB glue
 * (it arms the first bulk-OUT read); do not define it here. */
bool tud_usbtmc_initiate_abort_bulk_out_cb(uint8_t *tmcResult) { *tmcResult = USBTMC_STATUS_SUCCESS; return true; }
bool tud_usbtmc_check_abort_bulk_out_cb(usbtmc_check_abort_bulk_rsp_t *rsp) { rsp->USBTMC_status = USBTMC_STATUS_SUCCESS; return true; }
bool tud_usbtmc_initiate_abort_bulk_in_cb(uint8_t *tmcResult) { *tmcResult = USBTMC_STATUS_SUCCESS; return true; }
bool tud_usbtmc_check_abort_bulk_in_cb(usbtmc_check_abort_bulk_rsp_t *rsp) { rsp->USBTMC_status = USBTMC_STATUS_SUCCESS; return true; }
bool tud_usbtmc_initiate_clear_cb(uint8_t *tmcResult) { *tmcResult = USBTMC_STATUS_SUCCESS; return true; }
bool tud_usbtmc_check_clear_cb(usbtmc_get_clear_status_rsp_t *rsp) { rsp->USBTMC_status = USBTMC_STATUS_SUCCESS; return true; }

/* ---------- USB Descriptors (USBTMC only) ---------- */
#ifndef TUD_CONFIG_DESC_LEN
#define TUD_CONFIG_DESC_LEN     9
#endif
#ifndef TUD_INTERFACE_DESC_LEN
#define TUD_INTERFACE_DESC_LEN  9
#endif
#ifndef TUD_ENDPOINT_DESC_LEN
#define TUD_ENDPOINT_DESC_LEN   7
#endif
#ifndef TUD_INTERFACE_DESCRIPTOR
#define TUD_INTERFACE_DESCRIPTOR(itf_num, alt, num_ep, bclass, subclass, protocol, str_idx) \
    9, TUSB_DESC_INTERFACE, itf_num, alt, num_ep, bclass, subclass, protocol, str_idx
#endif
#ifndef TUD_ENDPOINT_DESCRIPTOR
#define TUD_ENDPOINT_DESCRIPTOR(ep_addr, ep_attr, ep_size, ep_interval) \
    7, TUSB_DESC_ENDPOINT, ep_addr, ep_attr, U16_TO_U8S_LE(ep_size), ep_interval
#endif

static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x1209,
    .idProduct          = 0x0001,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};
uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&desc_device; }

enum { ITF_NUM_USBTMC = 0, ITF_NUM_TOTAL };
#define USBTMC_EP_OUT 0x01
#define USBTMC_EP_IN  0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_INTERFACE_DESC_LEN + TUD_ENDPOINT_DESC_LEN * 2)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_INTERFACE_DESCRIPTOR(ITF_NUM_USBTMC, 0, 2, 0xFE, 0x03, 0x01, 0),
    TUD_ENDPOINT_DESCRIPTOR(USBTMC_EP_OUT, TUSB_XFER_BULK, 64, 0),
    TUD_ENDPOINT_DESCRIPTOR(USBTMC_EP_IN,  TUSB_XFER_BULK, 64, 0),
};
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index; return desc_configuration;
}

/* Chip-unique serial number, shared by the USB descriptor and *IDN? so two
 * identical boards can be told apart. */
static const char *board_serial(void) {
    static char serial[13];
    if (serial[0] == '\0') {
        uint8_t mac[6] = { 0 };
        esp_efuse_mac_get_default(mac);
        snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return serial;
}

static uint16_t _desc_str[32];
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    const char *str = NULL; uint8_t n = 0;
    switch (index) {
    case 0: _desc_str[1] = 0x0409; n = 1; break;
    case 1: str = "IoTSploit";              break;
    case 2: str = "ESP32-S3 SPP Security";  break;
    case 3: str = board_serial();  break;
    default: return NULL;
    }
    if (str) while (*str && n < 31) _desc_str[1 + n++] = *str++;
    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    return _desc_str;
}

/* ---------- esp32s3 internal USB PHY bring-up ---------- */
static usb_phy_handle_t s_phy;
static void usb_phy_start(void) {
    usb_phy_config_t c = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,    /* esp32s3 built-in PHY */
        .otg_mode   = USB_OTG_MODE_DEVICE,
        .otg_speed  = USB_PHY_SPEED_FULL,
    };
    usb_new_phy(&c, &s_phy);
}

/* ---------- USB pump task ----------
 * Use tud_task_ext(10ms) instead of the blocking tud_task() so the USB stack is
 * serviced on a bounded cadence even when idle. */
static void usb_task(void *arg) {
    usbscpi_t *dev = (usbscpi_t *)arg;
    for (;;) {
        tud_task_ext(10, false);
        usbscpi_task(dev);
    }
}

/* ---------- BLE init task ----------
 * NimBLE bring-up uses a lot of stack and can take time; run it off the main
 * path so USB (USBTMC) enumeration is never blocked. */
static void ble_init_task(void *arg) {
    (void)arg;
    ble_spp_security_init();
    vTaskDelete(NULL);
}

/* ---------- Status beacon: 10s period, reports adv/conn/pair state ---------- */
static void beacon_task(void *arg) {
    (void)arg;
    for (;;) {
        ESP_LOGI("beacon", "status conn=%d pair=%d",
                 ble_spp_conn_state(), ble_spp_pair_state());
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void) {
    usb_phy_start();
    tusb_init();

    /* *IDN? carries the same chip-unique serial as the USB descriptor. */
    static char idn[64];
    snprintf(idn, sizeof(idn), "IoTSploit,ESP32S3-SPP-SEC,%s,0.1.0", board_serial());

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
    usbscpi_tinyusb_bind(dev);          /* glue takes over the IN/OUT path */
    usbscpi_register(dev, spp_scpi_commands);

    xTaskCreate(usb_task, "usb", 6144, dev, 5, NULL);
    /* Bring up BLE after USB is running so enumeration is not blocked. */
    xTaskCreate(ble_init_task, "ble_init", 12288, NULL, 4, NULL);
    xTaskCreate(beacon_task, "beacon", 2560, NULL, 3, NULL);
}
