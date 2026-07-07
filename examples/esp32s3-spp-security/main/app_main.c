#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
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

/* ---------- Descriptor metadata (SYSTem:HELP:DESCription?) ---------- */

static const usbscpi_param_desc_t desc_ble_pair_passkey_params[] = {
    { "key", "string", true },
};
static const usbscpi_param_desc_t desc_ble_pair_confirm_params[] = {
    { "accept", "bool", false },
};

static const usbscpi_command_desc_t desc_commands[] = {
    { "BLE:ADV:STARt",       "command", "Start BLE SPP advertising",
      NULL, 0, NULL },
    { "BLE:ADV:STOP",        "command", "Stop BLE SPP advertising",
      NULL, 0, NULL },
    { "BLE:CONNect:STATe?",  "query",   "0=idle 1=connected",
      NULL, 0, "u32" },
    { "BLE:CONNect:STATus?", "query",   "Last GAP status/reason code",
      NULL, 0, "u32" },
    { "BLE:PAIR",            "command", "Initiate pairing/security on the active connection",
      NULL, 0, NULL },
    { "BLE:PAIR:STATe?",     "query",   "0=idle 1=in-progress 2=passkey-needed 3=numcmp-needed 4=done 5=failed 6=display-key",
      NULL, 0, "u32" },
    { "BLE:PAIR:PASSKey",    "command", "Enter the 6-digit passkey shown on the peer",
      desc_ble_pair_passkey_params, 1, NULL },
    { "BLE:PAIR:PASSKey?",   "query",   "Get the passkey to enter on the peer",
      NULL, 0, "u32" },
    { "BLE:PAIR:NUMCmp?",    "query",   "Get the numeric comparison value",
      NULL, 0, "u32" },
    { "BLE:PAIR:CONFirm",    "command", "Confirm (1) or reject (0) numeric comparison",
      desc_ble_pair_confirm_params, 1, NULL },
    { "BLE:SEC?",            "query",   "Security info: mac,level,encrypted,authenticated,bonded,key_size",
      NULL, 0, "string" },
};

static const char *const desc_ble_pair_failed[] = { "5" };

/* Interactive prompts for the ble-security workflow, keyed by BLE:PAIR:STATe?:
 *   2 passkey-needed  -> user types the passkey the peer shows
 *   3 numcmp-needed   -> user compares BLE:PAIR:NUMCmp? and accepts/rejects
 *   6 display-key     -> device shows BLE:PAIR:PASSKey? for the user to enter on the peer */
static const usbscpi_prompt_desc_t desc_ble_pair_prompts[] = {
    { "2", "passkey", "BLE:PAIR:PASSKey", NULL },
    { "3", "confirm", "BLE:PAIR:CONFirm", "BLE:PAIR:NUMCmp?" },
    { "6", "display", NULL,               "BLE:PAIR:PASSKey?" },
};

static const usbscpi_workflow_desc_t desc_workflows[] = {
    {
        .name = "ble-security",
        .type = "trigger_poll_interactive",
        .summary = "Pair with the connected central and report the security level",
        .trigger_cmd = "BLE:PAIR",
        .state_query = "BLE:PAIR:STATe?",
        .success_value = "4",
        .failed_values = desc_ble_pair_failed,
        .failed_value_count = 1,
        .prompts = desc_ble_pair_prompts,
        .prompt_count = sizeof(desc_ble_pair_prompts) / sizeof(desc_ble_pair_prompts[0]),
        .result_query = "BLE:SEC?",
        .result_fields = "mac:mac,level:u32,encrypted:bool,authenticated:bool,bonded:bool,key_size:u32",
        .timeout_ms = 30000,
        .poll_ms = 200,
    },
};

static const usbscpi_descriptor_t s_descriptor = {
    .commands = desc_commands,
    .command_count = sizeof(desc_commands) / sizeof(desc_commands[0]),
    .workflows = desc_workflows,
    .workflow_count = sizeof(desc_workflows) / sizeof(desc_workflows[0]),
};

static const scpi_command_t demo_commands[] = {
    { "BLE:ADV:STARt",       cmd_ble_adv_start,      0 },
    { "BLE:ADV:STOP",        cmd_ble_adv_stop,       0 },
    { "BLE:CONNect:STATe?",  cmd_ble_conn_state,     0 },
    { "BLE:CONNect:STATus?", cmd_ble_conn_status,    0 },
    { "BLE:PAIR",            cmd_ble_pair,           0 },
    { "BLE:PAIR:STATe?",     cmd_ble_pair_state,     0 },
    { "BLE:PAIR:PASSKey",    cmd_ble_pair_passkey,   0 },
    { "BLE:PAIR:PASSKey?",   cmd_ble_pair_passkey_q, 0 },
    { "BLE:PAIR:NUMCmp?",    cmd_ble_numcmp,         0 },
    { "BLE:PAIR:CONFirm",    cmd_ble_confirm,        0 },
    { "BLE:SEC?",            cmd_ble_sec,            0 },
    SCPI_CMD_LIST_END
    /* *IDN? / SYST:CAP? / SYST:HELP:HEAD? / SYST:ERR? are provided by core */
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
bool tud_usbtmc_msgBulkOut_start_cb(usbtmc_msg_request_dev_dep_out const *msg) { (void)msg; return true; }
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

static uint16_t _desc_str[32];
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    const char *str = NULL; uint8_t n = 0;
    switch (index) {
    case 0: _desc_str[1] = 0x0409; n = 1; break;
    case 1: str = "IoTSploit";              break;
    case 2: str = "ESP32-S3 SPP Security";  break;
    case 3: str = "0001";                   break;
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

    usbscpi_config_t cfg = {
        .usb_tx        = usb_tx,
        .line_buf      = s_line,
        .line_buf_len  = sizeof(s_line),
        .max_block_len = 4096,
        .idn           = "IoTSploit,ESP32S3-SPP-SEC,0001,0.1.0",
        .io_buf        = s_io,
        .io_buf_len    = sizeof(s_io),
        .proto         = 1,
        .mtu           = 256,
        .descriptor    = &s_descriptor,
    };

    usbscpi_t *dev = usbscpi_init(s_storage, sizeof(s_storage), &cfg);
    usbscpi_tinyusb_bind(dev);          /* glue takes over the IN/OUT path */
    usbscpi_register(dev, demo_commands);

    xTaskCreate(usb_task, "usb", 6144, dev, 5, NULL);
    /* Bring up BLE after USB is running so enumeration is not blocked. */
    xTaskCreate(ble_init_task, "ble_init", 12288, NULL, 4, NULL);
    xTaskCreate(beacon_task, "beacon", 2560, NULL, 3, NULL);
}
