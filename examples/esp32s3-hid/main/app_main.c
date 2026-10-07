#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_private/usb_phy.h"     /* usb_new_phy */
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "tusb.h"
#include "usbscpi/usbscpi.h"
#include "usbscpi_socket.h"

#include "duck_runner.h"
#include "duck_store.h"

/*
 * esp32s3-hid, HID-only + TCP profile.
 *
 * The single USB port is a HID keyboard presented to the target computer.
 * SCPI control (the DUCK:* contract) arrives only over Wi-Fi TCP :5025 — there
 * is no USBTMC. The app owns Wi-Fi initialisation explicitly; no BLE/Wi-Fi
 * scan task is involved. Recovery/provisioning is over UART or reflashing,
 * since USBTMC is absent.
 *
 * SCPI handlers run on the socket-serve task; HID submission and the runner
 * poll run on the USB task. They share only application state (the runner
 * start handoff and the upload store), guarded by one short-held mutex. No
 * script runs at boot or on reconnect.
 */

static const char *TAG = "duck";

/* Wi-Fi credentials for the control transport. A dedicated controller network
 * is assumed; the raw SCPI listener does not authenticate clients. */
#ifndef NET_SCPI_SSID
#define NET_SCPI_SSID "My Hotspot"
#endif
#ifndef NET_SCPI_PASS
#define NET_SCPI_PASS "great password"
#endif
#define NET_SCPI_PORT 5025

/* Core buffers (static; no allocation after init). The socket context emits
 * the whole descriptor into io_buf, so it is sized for that, not for a script. */
static uint8_t s_storage[2048];
static char    s_line[256];
static uint8_t s_io[8192];
static char    s_idn[96];

static duck_runner_t s_runner;
static duck_store_t  s_store;
static SemaphoreHandle_t s_lock;   /* guards the store + the run handoff */

/* Run handoff: a SCPI handler (socket task) snapshots the script here and
 * raises the pending flag; the USB task starts the runner from it. */
static char    s_runbuf[DUCK_SCRIPT_MAX];
static size_t  s_runlen;
static volatile bool s_start_pending;

/* Milestone-1 fixed demo: harmless, visible, self-terminating. */
static const char s_demo[] =
    "REM iotsploit esp32s3-hid demo\n"
    "STRING iotsploit esp32s3-hid HID demo\n"
    "ENTER\n";

/* ---------- HID backend: the runner's report sink (USB task) ---------- */
static int hid_submit(void *user, uint8_t modifier, const uint8_t keys[6]) {
    (void)user;
    if (!tud_mounted()) return -1;        /* detached: delivery error */
    if (!tud_hid_ready()) return 0;       /* endpoint busy: retry later */
    bool ok = tud_hid_keyboard_report(0, modifier, (uint8_t *)keys);
    return ok ? 1 : 0;
}
static bool hid_ready(void *user) {
    (void)user;
    return tud_mounted() && tud_hid_ready();
}
static uint32_t now_ms(void *user) {
    (void)user;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---------- SCPI send: socket glue (socket task) ---------- */
/* usb_tx is usbscpi_socket_tx directly (set in the config below). */

/* ---------- DATA:WRITE block -> staging upload (socket task) ---------- */
static int on_block_begin(void *user, size_t total_len) {
    (void)user;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = duck_store_block_begin(&s_store, 1 /*TCP owner*/, total_len);
    xSemaphoreGive(s_lock);
    return rc;
}
static int on_block_data(void *user, const uint8_t *data, size_t len) {
    (void)user;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = duck_store_block_data(&s_store, data, len);
    xSemaphoreGive(s_lock);
    return rc;
}
static int on_block_end(void *user, size_t total_len) {
    (void)user;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = duck_store_block_end(&s_store, total_len);
    xSemaphoreGive(s_lock);
    return rc;
}

/* ---------- DUCK:* SCPI commands (socket task) ---------- */
static scpi_result_t cmd_duck_run(scpi_t *ctx) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = (duck_runner_state(&s_runner) == DUCK_RUNNING) || s_start_pending;
    bool receiving = (duck_store_upload_state(&s_store) == DUCK_UP_RECEIVING);
    if (busy || receiving) {
        xSemaphoreGive(s_lock);
        SCPI_ErrorPush(ctx, busy ? SCPI_ERROR_EXECUTION_ERROR
                                 : SCPI_ERROR_SETTINGS_CONFLICT);
        return SCPI_RES_ERR;
    }
    size_t len = 0;
    const char *script = duck_store_committed(&s_store, &len);
    if (!script) { script = s_demo; len = sizeof(s_demo) - 1; }
    memcpy(s_runbuf, script, len);
    s_runlen = len;
    s_start_pending = true;               /* USB task starts it */
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "DUCK:RUN queued, %u bytes", (unsigned)len);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_duck_stop(scpi_t *ctx) {
    (void)ctx;
    duck_runner_stop(&s_runner);          /* sets a volatile flag; idempotent */
    return SCPI_RES_OK;
}

static scpi_result_t cmd_duck_state(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, (uint32_t)duck_runner_state(&s_runner));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_duck_line(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, duck_runner_line(&s_runner));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_duck_err(scpi_t *ctx) {
    char buf[96];
    snprintf(buf, sizeof buf, "%u,\"%s\"",
             (unsigned)duck_runner_line(&s_runner), duck_runner_error(&s_runner));
    SCPI_ResultCharacters(ctx, buf, strlen(buf));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_duck_up_start(scpi_t *ctx) {
    uint32_t len = 0;
    if (SCPI_ParamUInt32(ctx, &len, TRUE) != TRUE) return SCPI_RES_ERR;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = duck_store_reserve(&s_store, 1 /*TCP owner*/, (size_t)len);
    xSemaphoreGive(s_lock);
    if (rc != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_SETTINGS_CONFLICT);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_duck_up_abort(scpi_t *ctx) {
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    duck_store_abort(&s_store, 1);
    xSemaphoreGive(s_lock);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_duck_up_state(scpi_t *ctx) {
    static const char *const names[] = { "none", "receiving", "ready", "error" };
    duck_upload_state_t st = duck_store_upload_state(&s_store);
    const char *s = names[(int)st];
    SCPI_ResultCharacters(ctx, s, strlen(s));
    return SCPI_RES_OK;
}

static const scpi_command_t duck_commands[] = {
    { "DUCK:RUN",            cmd_duck_run,      0 },
    { "DUCK:STOP",           cmd_duck_stop,     0 },
    { "DUCK:STATe?",         cmd_duck_state,    0 },
    { "DUCK:LINE?",          cmd_duck_line,     0 },
    { "DUCK:ERR?",           cmd_duck_err,      0 },
    { "DUCK:UPLoad:STARt",   cmd_duck_up_start, 0 },
    { "DUCK:UPLoad:ABORt",   cmd_duck_up_abort, 0 },
    { "DUCK:UPLoad:STATe?",  cmd_duck_up_state, 0 },
    SCPI_CMD_LIST_END
    /* *IDN? / SYST:CAP? / SYST:HELP:HEAD? / SYST:ERR? from the core.
     * DATA:WRITE is intercepted by the core and routed to on_block_*. */
};

/* ---------- Descriptor metadata (SYSTem:HELP:DESCription?) ---------- */
static const usbscpi_param_desc_t desc_up_start_params[] = {
    { "length", "u32", true },
};
static const usbscpi_command_desc_t desc_commands[] = {
    { "DUCK:RUN",           "command", "Run the committed script, or the demo",
      NULL, 0, "none" },
    { "DUCK:STOP",          "command", "Request cancellation",
      NULL, 0, "none" },
    { "DUCK:STATe?",        "query",   "0 idle 1 running 2 done 3 error 4 cancelled",
      NULL, 0, "u32" },
    { "DUCK:LINE?",         "query",   "Current/last 1-based script line; 0 before a run",
      NULL, 0, "u32" },
    { "DUCK:ERR?",          "query",   "line,\"diagnostic\" for the last run",
      NULL, 0, "string" },
    { "DUCK:UPLoad:STARt",  "command", "Reserve an upload of <length> bytes",
      desc_up_start_params, 1, "none" },
    { "DUCK:UPLoad:ABORt",  "command", "Discard a staged upload",
      NULL, 0, "none" },
    { "DUCK:UPLoad:STATe?", "query",   "none/receiving/ready/error",
      NULL, 0, "string" },
    { "DATA:WRITE",         "block",   "Deliver the reserved script block",
      NULL, 0, "none" },
};

static const char *const desc_duck_failed[] = { "3", "4" };

static const usbscpi_workflow_desc_t desc_workflows[] = {
    {
        .name = "duck-run",
        .type = "trigger_poll_interactive",
        .summary = "Run a HID keyboard script and poll to completion",
        .trigger_cmd = "DUCK:RUN",
        .state_query = "DUCK:STATe?",
        .success_value = "2",
        .failed_values = desc_duck_failed,
        .failed_value_count = 2,
        .timeout_ms = 70000,
        .poll_ms = 100,
    },
};

static const usbscpi_descriptor_t s_descriptor = {
    .commands = desc_commands,
    .command_count = sizeof(desc_commands) / sizeof(desc_commands[0]),
    .workflows = desc_workflows,
    .workflow_count = sizeof(desc_workflows) / sizeof(desc_workflows[0]),
};

/* ---------- HID TinyUSB callbacks ---------- */
static const uint8_t desc_hid_report[] = { TUD_HID_REPORT_DESC_KEYBOARD() };

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
    /* Keyboard LED output report: safely ignored. */
}

/* ---------- USB descriptors: HID keyboard only ---------- */
#ifndef TUD_CONFIG_DESC_LEN
#define TUD_CONFIG_DESC_LEN     9
#endif

static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x1209,
    .idProduct          = 0x0003,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};
uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&desc_device; }

enum { ITF_NUM_HID = 0, ITF_NUM_TOTAL };
#define HID_EP_IN 0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(desc_hid_report), HID_EP_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
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
    case 1: str = "IoTSploit";          break;
    case 2: str = "ESP32-S3 HID";       break;
    case 3: str = "0003";               break;
    case 4: str = "IoTSploit Keyboard"; break;
    default: return NULL;
    }
    if (str) while (*str && n < 31) _desc_str[1 + n++] = *str++;
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return _desc_str;
}

void tud_umount_cb(void) {
    /* Detach mid-run: discard queued reports and end with a delivery error.
     * A new RUN is required after reconnect; scripts never resume. */
    duck_runner_detach(&s_runner);
}

/* ---------- esp32s3 internal PHY ---------- */
static usb_phy_handle_t s_phy;
static void usb_phy_start(void) {
    usb_phy_config_t c = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,
        .otg_mode   = USB_OTG_MODE_DEVICE,
        .otg_speed  = USB_PHY_SPEED_FULL,
    };
    usb_new_phy(&c, &s_phy);
}

/* ---------- Wi-Fi STA (owned here, not a scan task) ---------- */
static volatile bool s_got_ip;
static char s_ip[16];

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_got_ip = false;
        esp_wifi_connect();                 /* retry association */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_got_ip = true;
    }
}

static void wifi_sta_start(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL));

    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", NET_SCPI_SSID);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", NET_SCPI_PASS);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---------- network control task ---------- */
static void net_task(void *arg) {
    usbscpi_t *dev = (usbscpi_t *)arg;
    wifi_sta_start();

    int waited = 0;
    while (!s_got_ip && waited < 30000) {   /* bounded association timeout */
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    if (!s_got_ip) {
        ESP_LOGE(TAG, "Wi-Fi association timed out; SCPI listener not started");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "SCPI/TCP listening on %s:%d (control is UNAUTHENTICATED)",
             s_ip, NET_SCPI_PORT);
    if (usbscpi_socket_serve(dev, "0.0.0.0", NET_SCPI_PORT) != 0) {
        ESP_LOGE(TAG, "listen on port %d failed", NET_SCPI_PORT);
    }
    vTaskDelete(NULL);
}

/* ---------- USB service + runner pump ----------
 * Drives TinyUSB (HID) and the runner. The runner never blocks: each poll does
 * bounded work and returns on a busy endpoint or unmet deadline. The run
 * handoff from the socket task is consumed here so duck_runner_start and
 * duck_runner_poll stay on one task. */
static void usb_task(void *arg) {
    (void)arg;
    for (;;) {
        tud_task_ext(10, false);
        if (s_start_pending) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_start_pending) {
                duck_runner_start(&s_runner, s_runbuf, s_runlen);
                s_start_pending = false;
            }
            xSemaphoreGive(s_lock);
        }
        duck_runner_poll(&s_runner);
    }
}

void app_main(void) {
    static char boot_id[17];
    snprintf(boot_id, sizeof(boot_id), "%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    snprintf(s_idn, sizeof(s_idn), "IoTSploit,ESP32S3-HID,%02X%02X%02X%02X%02X%02X,%s",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             esp_app_get_description()->version);

    s_lock = xSemaphoreCreateMutex();
    duck_store_init(&s_store);
    duck_runner_cfg_t rcfg = {
        .submit = hid_submit,
        .ready = hid_ready,
        .now_ms = now_ms,
        .user = NULL,
        .max_runtime_ms = 60000,
    };
    duck_runner_init(&s_runner, &rcfg);

    usb_phy_start();
    tusb_init();

    usbscpi_config_t cfg = {
        .usb_tx         = usbscpi_socket_tx,   /* replies go out the TCP socket */
        .on_block_begin = on_block_begin,
        .on_block_data  = on_block_data,
        .on_block_end   = on_block_end,
        .line_buf       = s_line,
        .line_buf_len   = sizeof(s_line),
        .max_block_len  = DUCK_SCRIPT_MAX,     /* the upload contract, not a buffer */
        .idn            = s_idn,
        .boot_id        = boot_id,
        .io_buf         = s_io,
        .io_buf_len     = sizeof(s_io),
        .proto          = 1,
        .mtu            = 8192,
        .descriptor     = &s_descriptor,
    };

    usbscpi_t *dev = usbscpi_init(s_storage, sizeof(s_storage), &cfg);
    usbscpi_register(dev, duck_commands);

    xTaskCreate(usb_task, "usb", 6144, NULL, 5, NULL);
    xTaskCreate(net_task, "net_scpi", 6144, dev, 4, NULL);
    ESP_LOGI(TAG, "esp32s3-hid ready: HID keyboard, SCPI control over Wi-Fi TCP");
}
