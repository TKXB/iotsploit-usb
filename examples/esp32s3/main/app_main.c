#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"     /* usb_new_phy */
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "soc/soc_caps.h"
#include "tusb.h"
#include "usbscpi/usbscpi.h"
#include "usbscpi_tinyusb.h"
#include "usbscpi_socket.h"
#include "net_scpi.h"
#include "wifi_scan.h"
#include "ble_scan.h"
#include "ble_conn.h"
#include "usb_frame.h"
#include "usbscpi_stream.h"

static const char *TAG = "scpi";

/* ---------- 静态缓冲(等价 pico2,避免动态分配) ---------- */
static uint8_t s_storage[4096];
static char    s_line[512];
static usbscpi_t *s_usb_dev;
/* See the note in net_scpi.c: the descriptor is all-or-nothing, so this is
 * sized for it rather than for the largest data read. */
static uint8_t s_io[8192];

/* ---------- ADC oneshot 句柄 ---------- */
static adc_oneshot_unit_handle_t s_adc1;
/* ADC1 channels configured so far, one bit each. A channel is configured the
 * first time it is read; reading one that never was returns garbage. */
static uint32_t s_adc1_configured;

/* Configure ADC1 channel `ch` if it is not yet. 12 dB attenuation reads the
 * full 0..~3.1 V range. Returns 0, or -1 for a channel ADC1 does not have. */
static int adc_channel(uint32_t ch) {
    if (ch >= SOC_ADC_CHANNEL_NUM(ADC_UNIT_1)) return -1;
    if (s_adc1_configured & (1u << ch)) return 0;
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(s_adc1, (adc_channel_t)ch, &c) != ESP_OK) return -1;
    s_adc1_configured |= 1u << ch;
    return 0;
}

static void adc_setup(void) {
    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&u, &s_adc1);
    /* 预配置通道0(GPIO1 on esp32s3),DATA:READ? 用它 */
    (void)adc_channel(0);
}

/* ---------- SCPI 发送回调:走 glue 缓冲 IN 路径 ---------- */
static int usb_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user; (void)eom;
    return usbscpi_tinyusb_tx(NULL, data, len, true);
}

/* ---------- DATA:READ? 数据源:连续采 ADC1 ch0 ---------- */
static size_t adc_avail(void *user) { (void)user; return (size_t)-1; }  /* 永远有数据 */

static size_t adc_read_cb(void *user, uint8_t *buf, size_t len) {
    (void)user;
    size_t n = len / 2;               /* 每样本 2 字节(little-endian) */
    for (size_t i = 0; i < n; i++) {
        int raw = 0;
        adc_oneshot_read(s_adc1, ADC_CHANNEL_0, &raw);
        uint16_t v = (uint16_t)raw;
        buf[i*2]   = (uint8_t)(v & 0xFF);
        buf[i*2+1] = (uint8_t)(v >> 8);
    }
    return n * 2;
}

/* ---------- 业务命令(每个 3–5 行,仅 HAL 不同) ---------- */
static scpi_result_t cmd_gpio_set(scpi_t *ctx) {
    uint32_t pin, val;
    if (SCPI_ParamUInt32(ctx, &pin, TRUE) != TRUE) return SCPI_RES_ERR;
    if (SCPI_ParamUInt32(ctx, &val, TRUE) != TRUE) return SCPI_RES_ERR;
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)pin, val != 0);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_gpio_get(scpi_t *ctx) {
    uint32_t pin;
    if (SCPI_ParamUInt32(ctx, &pin, TRUE) != TRUE) return SCPI_RES_ERR;
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    SCPI_ResultUInt32(ctx, (uint32_t)gpio_get_level((gpio_num_t)pin));
    return SCPI_RES_OK;
}

/* ADC? [channel]: raw count from ADC1 `channel` (default 0). On the ESP32-S3,
 * ADC1 channel n is GPIO n+1. */
static scpi_result_t cmd_adc_read(scpi_t *ctx) {
    uint32_t ch = 0;
    (void)SCPI_ParamUInt32(ctx, &ch, FALSE);
    if (adc_channel(ch) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    int raw = 0;
    if (adc_oneshot_read(s_adc1, (adc_channel_t)ch, &raw) != ESP_OK) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultUInt32(ctx, (uint32_t)raw);
    return SCPI_RES_OK;
}

/* ---------- WiFi 扫描命令(异步触发 → 轮询 → 逐行取) ---------- */
static scpi_result_t cmd_wlan_scan(scpi_t *ctx) {
    /* esp_wifi_scan_start() sweeps every channel, taking the radio off the
     * channel the station is associated on. Over USB that is free, because the
     * link is independent of the radio; over TCP the scan would drop the very
     * connection carrying this command. Refuse rather than strand the caller —
     * the scan is still available over USB. */
    if (usbscpi_socket_client_connected()) {
        ESP_LOGW(TAG, "WLAN:SCAN refused: would drop the TCP session");
        SCPI_ErrorPush(ctx, SCPI_ERROR_SETTINGS_CONFLICT);
        return SCPI_RES_ERR;
    }
    ESP_LOGI(TAG, "WLAN:SCAN");
    return wifi_scan_start() == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_wlan_done(scpi_t *ctx) {
    SCPI_ResultBool(ctx, wifi_scan_done());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_wlan_count(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, (uint32_t)wifi_scan_count());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_wlan_get(scpi_t *ctx) {
    uint32_t idx = 0;
    char buf[96];
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    if (wifi_scan_get(idx, buf, sizeof(buf)) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    SCPI_ResultCharacters(ctx, buf, strlen(buf));   /* 裸 CSV,不要 ResultText(会加引号) */
    return SCPI_RES_OK;
}

/* Wi-Fi provisioning changes the interface that carries TCP SCPI. Only the
 * independent USB context may start, stop or clear an association job. */
static bool s_wlan_conn_failure_reported;

static bool wlan_usb_only(scpi_t *ctx) {
    if (ctx->user_context == s_usb_dev) return true;
    SCPI_ErrorPush(ctx, SCPI_ERROR_SETTINGS_CONFLICT);
    return false;
}

static scpi_result_t cmd_wlan_connect_start(scpi_t *ctx) {
    if (!wlan_usb_only(ctx)) return SCPI_RES_ERR;
    /* libscpi unescapes doubled quotes. Size for the encoded maximum too,
     * then validate the decoded byte length instead of truncating it. */
    char ssid[67], password[129];
    size_t ssid_len, password_len;
    if (!SCPI_ParamCopyText(ctx, ssid, sizeof(ssid), &ssid_len, TRUE) ||
        !SCPI_ParamCopyText(ctx, password, sizeof(password), &password_len, TRUE))
        return SCPI_RES_ERR;
    if (!ssid_len || ssid_len > 32 || password_len > 63 ||
        strlen(ssid) != ssid_len || strlen(password) != password_len ||
        strpbrk(ssid, "\r\n") || strpbrk(password, "\r\n")) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    int rc = wifi_sta_connect(ssid, password);
    memset(password, 0, sizeof(password));
    if (rc != 0) {
        SCPI_ErrorPush(ctx, rc == -2 ? SCPI_ERROR_SETTINGS_CONFLICT : SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    s_wlan_conn_failure_reported = false;
    return SCPI_RES_OK;
}

static scpi_result_t cmd_wlan_connect_stop(scpi_t *ctx) {
    if (!wlan_usb_only(ctx)) return SCPI_RES_ERR;
    if (wifi_sta_stop() != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_wlan_connect_clear(scpi_t *ctx) {
    if (!wlan_usb_only(ctx)) return SCPI_RES_ERR;
    if (wifi_sta_clear() != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_SETTINGS_CONFLICT);
        return SCPI_RES_ERR;
    }
    s_wlan_conn_failure_reported = false;
    return SCPI_RES_OK;
}

static scpi_result_t cmd_wlan_connect_state(scpi_t *ctx) {
    wifi_sta_result_t result;
    wifi_sta_result(&result);
    if (strcmp(result.state, "FAILED") == 0 && !s_wlan_conn_failure_reported) {
        s_wlan_conn_failure_reported = true;
        usbscpi_queue_error(ctx, SCPI_ERROR_EXECUTION_ERROR);
    }
    SCPI_ResultMnemonic(ctx, result.state);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_wlan_connect_count(scpi_t *ctx) {
    wifi_sta_result_t result;
    wifi_sta_result(&result);
    SCPI_ResultUInt32(ctx, strcmp(result.result, "NONE") != 0);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_wlan_connect_fetch(scpi_t *ctx) {
    uint32_t index;
    if (!SCPI_ParamUInt32(ctx, &index, TRUE)) return SCPI_RES_ERR;
    wifi_sta_result_t result;
    wifi_sta_result(&result);
    if (index || strcmp(result.result, "NONE") == 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    SCPI_ResultMnemonic(ctx, result.result);
    SCPI_ResultText(ctx, result.ip);
    SCPI_ResultInt32(ctx, result.reason);
    return SCPI_RES_OK;
}

/* ---------- BLE 扫描命令 ---------- */
static scpi_result_t cmd_ble_scan(scpi_t *ctx) {
    uint32_t secs = 5;
    (void)SCPI_ParamUInt32(ctx, &secs, FALSE);
    ESP_LOGI(TAG, "BLE:SCAN secs=%u", (unsigned)secs);
    return ble_scan_start(secs) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_done(scpi_t *ctx) {
    SCPI_ResultBool(ctx, ble_scan_done());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_count(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, (uint32_t)ble_scan_count());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_get(scpi_t *ctx) {
    uint32_t idx = 0;
    char buf[96];
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    if (ble_scan_get(idx, buf, sizeof(buf)) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    SCPI_ResultCharacters(ctx, buf, strlen(buf));
    return SCPI_RES_OK;
}

/* ---------- BLE 连接 / 配对命令 ---------- */
static scpi_result_t cmd_ble_conn(scpi_t *ctx) {
    uint32_t idx = 0;
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    ESP_LOGI(TAG, "BLE:CONNect idx=%u", (unsigned)idx);
    return ble_conn_start((size_t)idx) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_conn_state(scpi_t *ctx) {
    SCPI_ResultInt32(ctx, ble_conn_state());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_conn_status(scpi_t *ctx) {
    SCPI_ResultInt32(ctx, ble_conn_last_status());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_disconn(scpi_t *ctx) {
    (void)ctx;
    ESP_LOGI(TAG, "BLE:DISConnect");
    return ble_conn_disconnect() == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_connpair(scpi_t *ctx) {
    uint32_t idx = 0;
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    ESP_LOGI(TAG, "BLE:CPAIR idx=%u", (unsigned)idx);
    return ble_connpair_start((size_t)idx) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_connpair_state(scpi_t *ctx) {
    SCPI_ResultInt32(ctx, ble_connpair_state());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_pair(scpi_t *ctx) {
    (void)ctx;
    ESP_LOGI(TAG, "BLE:PAIR");
    return ble_pair_start() == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_pair_state(scpi_t *ctx) {
    SCPI_ResultInt32(ctx, ble_pair_state());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_pair_passkey(scpi_t *ctx) {
    uint32_t pk = 0;
    if (SCPI_ParamUInt32(ctx, &pk, TRUE) != TRUE) return SCPI_RES_ERR;
    ESP_LOGI(TAG, "BLE:PAIR:PASSKey %06u", (unsigned)pk);
    return ble_pair_passkey(pk) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
}

static scpi_result_t cmd_ble_pair_passkey_q(scpi_t *ctx) {
    uint32_t pk = 0;
    if (ble_pair_passkey_get(&pk) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultUInt32(ctx, pk);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_numcmp(scpi_t *ctx) {
    uint32_t n = 0;
    if (ble_pair_numcmp_get(&n) != 0) {
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
    return ble_pair_confirm((int)accept) == 0 ? SCPI_RES_OK : SCPI_RES_ERR;
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

/* ---- BLE RSSI data plane control ----------------------------------------
 * The ble-scan workflow keeps one sample per device and so cannot answer
 * "how did this RSSI move". These stream every advertisement report instead;
 * both surfaces coexist. */

/* Framing is off at boot and only this turns it on. An existing host reads the
 * vendor pipe as raw text with no negotiation, so a device that framed
 * unconditionally would garble it; only a host that understands the envelope
 * sends this. Enabling it also routes records to the USB ring instead of the
 * TCP one — one ring, one consumer. */
static scpi_result_t cmd_stream_framing(scpi_t *ctx) {
    uint32_t on = 0;
    if (!SCPI_ParamUInt32(ctx, &on, TRUE)) {
        return SCPI_RES_ERR;
    }
    ble_stream_usb_mode_set(on ? 1 : 0);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_framing_q(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, (uint32_t)ble_stream_usb_mode());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_port(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, 5026);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_format(scpi_t *ctx) {
    char buf[256];
    snprintf(buf, sizeof buf, "ver=1,stride=%u,fields=%s",
             (unsigned)ble_stream_stride(), ble_stream_fields());
    SCPI_ResultCharacters(ctx, buf, strlen(buf));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_start(scpi_t *ctx) {
    if (ble_stream_scan_start() != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    ble_stream_enable(1);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_stop(scpi_t *ctx) {
    (void)ctx;
    ble_stream_scan_stop();
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_count(scpi_t *ctx) {
    SCPI_ResultUInt64(ctx, ble_stream_count());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_dropped(scpi_t *ctx) {
    SCPI_ResultUInt64(ctx, ble_stream_dropped());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_torn(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, (uint32_t)usbscpi_stream_torn());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_state(scpi_t *ctx) {
    char buf[64];
    snprintf(buf, sizeof buf, "%d,%d",
             ble_stream_enabled(), usbscpi_stream_attached());
    SCPI_ResultCharacters(ctx, buf, strlen(buf));
    return SCPI_RES_OK;
}

/* ---------- Jobs (.agents/standards/scpi-commands.md) ----------
 * WLAN:SCAN, BLE:SCAN and BLE:CONNect are jobs: STARt, then STATe? reports one
 * word. These handlers map the scanners' done flags and the BLE connect/pair
 * state machines (ble_conn.h) onto those words; the machines are unchanged. */

static bool s_wlan_started;
static bool s_ble_started;

static scpi_result_t cmd_wlan_start(scpi_t *ctx) {
    scpi_result_t r = cmd_wlan_scan(ctx);
    if (r == SCPI_RES_OK) s_wlan_started = true;
    return r;
}

static scpi_result_t cmd_wlan_state(scpi_t *ctx) {
    SCPI_ResultMnemonic(ctx, !s_wlan_started ? "IDLE" : wifi_scan_done() ? "DONE" : "RUNNING");
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_start(scpi_t *ctx) {
    scpi_result_t r = cmd_ble_scan(ctx);
    if (r == SCPI_RES_OK) s_ble_started = true;
    return r;
}

static scpi_result_t cmd_ble_state(scpi_t *ctx) {
    SCPI_ResultMnemonic(ctx, !s_ble_started ? "IDLE" : ble_scan_done() ? "DONE" : "RUNNING");
    return SCPI_RES_OK;
}

/* BLE:CONNect:STARt <index>[,<pair>]: connect to a scan result and, unless
 * pair is 0, pair in the same job. */
static bool s_conn_pair;
static bool s_conn_failure_reported;

static scpi_result_t cmd_ble_connect_start(scpi_t *ctx) {
    uint32_t idx = 0, pair = 1;
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    (void)SCPI_ParamUInt32(ctx, &pair, FALSE);
    ESP_LOGI(TAG, "BLE:CONNect:STARt idx=%u pair=%u", (unsigned)idx, (unsigned)pair);
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
        case BLE_CP_NUMCMP:  return "CONFIRM";
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

static scpi_result_t cmd_ble_connect_state(scpi_t *ctx) {
    const char *word = connect_state_word();
    /* FAILED carries a reason in the error queue, once per attempt; the GAP
     * status code is in the device log. */
    if (strcmp(word, "FAILED") == 0 && !s_conn_failure_reported) {
        s_conn_failure_reported = true;
        ESP_LOGW(TAG, "BLE connect failed, GAP status %d", ble_conn_last_status());
        usbscpi_queue_error(ctx, SCPI_ERROR_EXECUTION_ERROR);
    }
    SCPI_ResultMnemonic(ctx, word);
    return SCPI_RES_OK;
}

/* ---------- Commands: declared once, described for hosts ---------- */

static const usbscpi_param_desc_t gpio_set_params[] = {
    USBSCPI_PARAM("pin", "u32", true),
    USBSCPI_PARAM("value", "bool", true),
};
static const usbscpi_param_desc_t pin_params[] = {
    USBSCPI_PARAM("pin", "u32", true),
};
static const usbscpi_param_desc_t adc_params[] = {
    USBSCPI_PARAM("channel", "u32", false),
};
static const usbscpi_param_desc_t wlan_connect_params[] = {
    USBSCPI_PARAM("ssid", "string", true),
    USBSCPI_PARAM("password", "string", true),
};
static const usbscpi_param_desc_t wlan_connect_fetch_params[] = {
    USBSCPI_PARAM_PICK("index", "WLAN:CONNect:COUNt?", "WLAN:CONNect:FETCh?"),
};
static const usbscpi_param_desc_t ble_scan_params[] = {
    USBSCPI_PARAM("duration", "u32", false),
};
static const usbscpi_param_desc_t wlan_fetch_params[] = {
    USBSCPI_PARAM_PICK("index", "WLAN:SCAN:COUNt?", "WLAN:SCAN:FETCh?"),
};
static const usbscpi_param_desc_t ble_fetch_params[] = {
    USBSCPI_PARAM_PICK("index", "BLE:SCAN:COUNt?", "BLE:SCAN:FETCh?"),
};
static const usbscpi_param_desc_t ble_connect_params[] = {
    USBSCPI_PARAM_PICK("index", "BLE:SCAN:COUNt?", "BLE:SCAN:FETCh?"),
    USBSCPI_PARAM("pair", "bool", false),
};
static const usbscpi_param_desc_t key_params[] = {
    USBSCPI_PARAM("key", "u32", true),
};
static const usbscpi_param_desc_t accept_params[] = {
    USBSCPI_PARAM("accept", "bool", false),
};
static const usbscpi_param_desc_t framing_params[] = {
    USBSCPI_PARAM("value", "bool", true),
};

/* The data plane is driven entirely by the SYSTem:STReam commands, so they
 * are described too. ALIAS entries are the names from before the command
 * standard: they still work for older hosts and the UI, but are not described. */
#define ESP_COMMANDS(CMD, ALIAS)                                                          \
    CMD("SYSTem:STReam:STARt",   cmd_stream_start,   "command", "Start the BLE RSSI capture", \
        USBSCPI_NO_PARAMS, "none")                                                        \
    CMD("SYSTem:STReam:STOP",    cmd_stream_stop,    "command", "Stop the capture",       \
        USBSCPI_NO_PARAMS, "none")                                                        \
    CMD("SYSTem:STReam:STATe?",  cmd_stream_state,   "query",   "enabled,attached",       \
        USBSCPI_NO_PARAMS, "string")                                                      \
    CMD("SYSTem:STReam:COUNt?",  cmd_stream_count,   "query",   "Reports captured",       \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("SYSTem:STReam:DROPped?", cmd_stream_dropped, "query",  "Reports lost to overflow", \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("SYSTem:STReam:TORN?",   cmd_stream_torn,    "query",   "Records cut by a reconnect", \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("SYSTem:STReam:PORT?",   cmd_stream_port,    "query",   "Data-plane TCP port",    \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("SYSTem:STReam:FORMat?", cmd_stream_format,  "query",   "Record version, stride, schema", \
        USBSCPI_NO_PARAMS, "string")                                                      \
    CMD("SYSTem:STReam:FRAMing", cmd_stream_framing, "command", "Frame the USB vendor pipe", \
        USBSCPI_PARAMS(framing_params), "none")                                           \
    CMD("SYSTem:STReam:FRAMing?", cmd_stream_framing_q, "query", "USB framing state",     \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("GPIO",              cmd_gpio_set,   "command", "Set a GPIO output level",        \
        USBSCPI_PARAMS(gpio_set_params), "none")                                          \
    CMD("GPIO?",             cmd_gpio_get,   "query",   "Read a GPIO input level",        \
        USBSCPI_PARAMS(pin_params), "u32")                                                \
    CMD("ADC?",              cmd_adc_read,   "query",   "Raw ADC1 count; channel n is GPIO n+1", \
        USBSCPI_PARAMS(adc_params), "u32")                                                \
    CMD("WLAN:SCAN:STARt",   cmd_wlan_start, "command", "Scan for Wi-Fi access points (USB only)", \
        USBSCPI_NO_PARAMS, "none")                                                        \
    CMD("WLAN:SCAN:STATe?",  cmd_wlan_state, "query",   "IDLE, RUNNING or DONE",          \
        USBSCPI_NO_PARAMS, "string")                                                      \
    CMD("WLAN:SCAN:COUNt?",  cmd_wlan_count, "query",   "Access points found",            \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("WLAN:SCAN:FETCh?",  cmd_wlan_get,   "query",   "Access point by index",          \
        USBSCPI_PARAMS(wlan_fetch_params), "string")                                      \
    CMD("WLAN:CONNect:STARt", cmd_wlan_connect_start, "command", "Try one SSID/password (USB only)", \
        USBSCPI_PARAMS(wlan_connect_params), "none")                                       \
    CMD("WLAN:CONNect:STOP", cmd_wlan_connect_stop, "command", "Cancel or disconnect (USB only)", \
        USBSCPI_NO_PARAMS, "none")                                                        \
    CMD("WLAN:CONNect:STATe?", cmd_wlan_connect_state, "query", "IDLE, RUNNING, DONE or FAILED", \
        USBSCPI_NO_PARAMS, "string")                                                      \
    CMD("WLAN:CONNect:COUNt?", cmd_wlan_connect_count, "query", "Completed attempt results", \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("WLAN:CONNect:FETCh?", cmd_wlan_connect_fetch, "query", "result,ip,reason; index 0", \
        USBSCPI_PARAMS(wlan_connect_fetch_params), "string")                               \
    CMD("WLAN:CONNect:CLEar", cmd_wlan_connect_clear, "command", "Clear an offline result (USB only)", \
        USBSCPI_NO_PARAMS, "none")                                                        \
    CMD("BLE:SCAN:STARt",    cmd_ble_start,  "command", "Scan for BLE devices for N seconds (default 5)", \
        USBSCPI_PARAMS(ble_scan_params), "none")                                          \
    CMD("BLE:SCAN:STATe?",   cmd_ble_state,  "query",   "IDLE, RUNNING or DONE",          \
        USBSCPI_NO_PARAMS, "string")                                                      \
    CMD("BLE:SCAN:COUNt?",   cmd_ble_count,  "query",   "BLE devices found",              \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("BLE:SCAN:FETCh?",   cmd_ble_get,    "query",   "BLE device by index",            \
        USBSCPI_PARAMS(ble_fetch_params), "string")                                       \
    CMD("BLE:CONNect:STARt", cmd_ble_connect_start, "command",                           \
        "Connect to a scanned device and pair (pair=0 to only connect)",                  \
        USBSCPI_PARAMS(ble_connect_params), "none")                                       \
    CMD("BLE:CONNect:STATe?", cmd_ble_connect_state, "query",                            \
        "IDLE, RUNNING, PASSKEY, CONFIRM, DISPLAY, DONE or FAILED",                       \
        USBSCPI_NO_PARAMS, "string")                                                      \
    CMD("BLE:DISConnect",    cmd_ble_disconn, "command", "Drop the BLE connection",       \
        USBSCPI_NO_PARAMS, "none")                                                        \
    CMD("BLE:PAIR:PASSKey",  cmd_ble_pair_passkey, "command", "Enter the passkey shown on the peer", \
        USBSCPI_PARAMS(key_params), "none")                                               \
    CMD("BLE:PAIR:PASSKey?", cmd_ble_pair_passkey_q, "query", "Passkey to enter on the peer", \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("BLE:PAIR:NUMCmp?",  cmd_ble_numcmp, "query",   "Number to compare with the peer", \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("BLE:PAIR:CONFirm",  cmd_ble_confirm, "command", "Accept (1) or reject (0) the number", \
        USBSCPI_PARAMS(accept_params), "none")                                            \
    CMD("BLE:SEC?",          cmd_ble_sec,    "query",   "mac,level,encrypted,authenticated,bonded,key_size", \
        USBSCPI_NO_PARAMS, "string")                                                      \
    ALIAS("GPIO:SET",            cmd_gpio_set)                                            \
    ALIAS("GPIO:GET?",           cmd_gpio_get)                                            \
    ALIAS("ADC:READ?",           cmd_adc_read)                                            \
    ALIAS("WLAN:SCAN",           cmd_wlan_start)                                          \
    ALIAS("WLAN:SCAN:DONE?",     cmd_wlan_done)                                           \
    ALIAS("WLAN:SCAN?",          cmd_wlan_get)                                            \
    ALIAS("BLE:SCAN",            cmd_ble_start)                                           \
    ALIAS("BLE:SCAN:DONE?",      cmd_ble_done)                                            \
    ALIAS("BLE:SCAN?",           cmd_ble_get)                                             \
    ALIAS("BLE:CONNect",         cmd_ble_conn)                                            \
    ALIAS("BLE:CONNect:STATus?", cmd_ble_conn_status)                                     \
    ALIAS("BLE:CPAIR",           cmd_ble_connpair)                                        \
    ALIAS("BLE:CPAIR:STATe?",    cmd_ble_connpair_state)                                  \
    ALIAS("BLE:PAIR",            cmd_ble_pair)                                            \
    ALIAS("BLE:PAIR:STATe?",     cmd_ble_pair_state)
USBSCPI_DEFINE_COMMANDS(esp, ESP_COMMANDS);

/* ---------- Workflows ---------- */

static const usbscpi_prompt_desc_t connect_prompts[] = {
    USBSCPI_PROMPT_PASSKEY("BLE:PAIR:PASSKey"),
    USBSCPI_PROMPT_CONFIRM("BLE:PAIR:CONFirm", "BLE:PAIR:NUMCmp?"),
    USBSCPI_PROMPT_DISPLAY("BLE:PAIR:PASSKey?"),
};
static const char *const connect_old_names[] = { "ble-connect-pair" };

static const usbscpi_workflow_desc_t esp_workflows[] = {
    { USBSCPI_WF_ACQUIRE("wifi-connect", "WLAN:CONNect", "Try one Wi-Fi credential over USB",
                         "result:string,ip:string,reason:i32", 35000) },
    { USBSCPI_WF_ACQUIRE("wifi-scan", "WLAN:SCAN", "Scan for Wi-Fi access points",
                         "ssid:string,rssi:i32:dbm,channel:u32,authmode:string,bssid:mac",
                         15000) },
    { USBSCPI_WF_ACQUIRE("ble-scan", "BLE:SCAN", "Scan for BLE devices",
                         "addr:mac,rssi:i32:dbm,name:string,adv_type:string", 30000) },
    { USBSCPI_WF_INTERACTIVE("ble-connect", "BLE:CONNect",
                             "Connect to a scanned device and pair", 45000),
      USBSCPI_WF_PROMPTS(connect_prompts),
      USBSCPI_WF_RESULT("BLE:SEC?",
                        "mac:mac,level:u32,encrypted:bool,authenticated:bool,bonded:bool,key_size:u32"),
      USBSCPI_WF_RENAMED_FROM(connect_old_names) },
};

static const usbscpi_descriptor_t s_descriptor = {
    .commands = esp_desc_commands,
    .command_count = USBSCPI_COUNT(esp_desc_commands),
    .workflows = esp_workflows,
    .workflow_count = USBSCPI_COUNT(esp_workflows),
};

/* ---------- TinyUSB USBTMC 必须回调(glue 未提供的补 stub) ---------- */
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

/* ---------- USB Descriptors (merged from usb_descriptors.c to ensure linking) ---------- */
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

enum { ITF_NUM_USBTMC = 0, ITF_NUM_VENDOR, ITF_NUM_TOTAL };
#define USBTMC_EP_OUT 0x01
#define USBTMC_EP_IN  0x81
/* Vendor 日志接口:IN 0x82 传日志;OUT 0x02 存在但不使用
 * (TUD_VENDOR_DESCRIPTOR 强制两端点,主机永不写入)。 */
#define LOG_EP_OUT    0x02
#define LOG_EP_IN     0x82
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_INTERFACE_DESC_LEN + TUD_ENDPOINT_DESC_LEN * 2 \
                          + TUD_VENDOR_DESC_LEN)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_INTERFACE_DESCRIPTOR(ITF_NUM_USBTMC, 0, 2, 0xFE, 0x03, 0x01, 0),
    TUD_ENDPOINT_DESCRIPTOR(USBTMC_EP_OUT, TUSB_XFER_BULK, 64, 0),
    TUD_ENDPOINT_DESCRIPTOR(USBTMC_EP_IN,  TUSB_XFER_BULK, 64, 0),
    /* 第二接口:vendor-specific,承载设备日志流 */
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 4, LOG_EP_OUT, LOG_EP_IN, 64),
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
    case 1: str = "IoTSploit";       break;
    case 2: str = "ESP32-S3 USBTMC"; break;
    case 3: str = board_serial();  break;
    case 4: str = "IoTSploit Log";   break;
    default: return NULL;
    }
    if (str) while (*str && n < 31) _desc_str[1 + n++] = *str++;
    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    return _desc_str;
}

/* ---------- esp32s3 内置 PHY 起步 ---------- */
static usb_phy_handle_t s_phy;
static void usb_phy_start(void) {
    usb_phy_config_t c = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,    /* esp32s3 内置 PHY */
        .otg_mode   = USB_OTG_MODE_DEVICE,
        .otg_speed  = USB_PHY_SPEED_FULL,
    };
    usb_new_phy(&c, &s_phy);
}

/* ---------- 设备日志 -> Vendor bulk-IN(0x82) ----------
 * ESP-IDF 的 ESP_LOGx 全部经由一个 vprintf 钩子。我们把日志字节喂进一个
 * FreeRTOS stream buffer(非阻塞,满则丢),再由 usb_task 排空到 TinyUSB 的
 * vendor FIFO。绝不在任意任务/ISR 上下文里直接调用 TinyUSB —— 只在 USB 任务里
 * 调 tud_vendor_write(),从而不阻塞主循环也不破坏枚举。同时把日志转发给原始
 * vprintf(UART),本地串口调试照常可用。 */
static StreamBufferHandle_t s_log_sb;
static vprintf_like_t       s_prev_vprintf;

static int log_tee_vprintf(const char *fmt, va_list ap) {
    /* 先转发到原始输出(UART),消费一份 va_list */
    va_list ap_uart;
    va_copy(ap_uart, ap);
    int n = s_prev_vprintf ? s_prev_vprintf(fmt, ap_uart) : 0;
    va_end(ap_uart);

    /* 再格式化一份进 stream buffer;stream buffer 不是 ISR 安全的,ISR 上下文跳过 */
    if (s_log_sb && !xPortInIsrContext()) {
        char buf[256];
        va_list ap_usb;
        va_copy(ap_usb, ap);
        int m = vsnprintf(buf, sizeof(buf), fmt, ap_usb);
        va_end(ap_usb);
        if (m > 0) {
            size_t len = (m < (int)sizeof(buf)) ? (size_t)m : sizeof(buf) - 1;
            (void)xStreamBufferSend(s_log_sb, buf, len, 0); /* 满则丢,不阻塞 */
        }
    }
    return n;
}

/* 把 stream buffer 里的日志排空到 vendor FIFO。只在 USB 任务里调用。
 * 只取当前 FIFO 能容纳的字节,避免写不下时丢字节;未挂载时丢弃积压。 */
/* Write one framed message. Only called after checking the FIFO can take the
 * whole thing: a half-written envelope would desynchronise the reader for
 * everything after it. */
static void usb_frame_write(uint8_t type, const uint8_t *payload, uint16_t len) {
    uint8_t hdr[USB_FRAME_HDR_LEN] = {
        type, 0u, (uint8_t)(len & 0xFFu), (uint8_t)(len >> 8)
    };
    tud_vendor_write(hdr, sizeof hdr);
    if (len) {
        tud_vendor_write(payload, len);
    }
}

/* Drain records ahead of log text, because logs are diagnostics and records
 * are the measurement — losing the measurement to a chatty ESP_LOGx would be
 * backwards. Returns the FIFO space left for logs. */
static uint32_t usb_pump_records(void) {
    usbscpi_ring_t *ring = ble_stream_usb_ring();
    const size_t stride = ble_stream_stride();
    uint8_t rec[128];
    if (stride > sizeof rec) {
        return tud_vendor_write_available();  /* cannot happen; fail safe */
    }
    for (;;) {
        uint32_t space = tud_vendor_write_available();
        if (space < USB_FRAME_HDR_LEN + stride) return space;
        if (usbscpi_ring_count(ring) < stride) return space;
        if (usbscpi_ring_read(ring, rec, stride) != stride) return space;
        usb_frame_write(USB_FRAME_TYPE_REC, rec, (uint16_t)stride);
    }
}

static void usb_vendor_pump(void) {
    if (!s_log_sb) return;
    if (!tud_vendor_mounted()) {
        uint8_t junk[64];
        while (xStreamBufferReceive(s_log_sb, junk, sizeof(junk), 0) > 0) { }
        return;
    }

    const int framed = ble_stream_usb_mode();
    uint32_t space = framed ? usb_pump_records() : tud_vendor_write_available();

    /* Log text. Unframed this is the original raw path, byte for byte, which
     * is what an old host still expects. */
    uint32_t budget = framed ? USB_FRAME_LOG_CHUNK : UINT32_MAX;
    for (;;) {
        if (space == 0 || budget == 0) break;
        uint8_t buf[64];
        uint32_t room = framed
            ? (space > USB_FRAME_HDR_LEN ? space - USB_FRAME_HDR_LEN : 0u)
            : space;
        if (room == 0) break;
        uint32_t want = room < sizeof(buf) ? room : (uint32_t)sizeof(buf);
        if (want > budget) want = budget;
        size_t got = xStreamBufferReceive(s_log_sb, buf, want, 0);
        if (got == 0) break;                   /* no more log text queued */
        if (framed) {
            usb_frame_write(USB_FRAME_TYPE_LOG, buf, (uint16_t)got);
        } else {
            tud_vendor_write(buf, got);
        }
        budget -= (uint32_t)got;
        space = tud_vendor_write_available();
    }
    tud_vendor_flush();
}

/* ---------- 状态信标:10 秒周期,报告 wifi/ble/连接状态 ----------
 * 取代原来的 1 Hz 心跳;tag=\"beacon\" 可过滤,SCPI 路径不受影响。 */
static void beacon_task(void *arg) {
    (void)arg;
    for (;;) {
        ESP_LOGI("beacon", "status wifi_aps=%u ble_devs=%u conn=%d pair=%d",
                 (unsigned)wifi_scan_count(),
                 (unsigned)ble_scan_count(),
                 ble_conn_state(),
                 ble_pair_state());
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/* ---------- USB 泵任务 ----------
 * 用有界超时的 tud_task_ext(10ms) 取代会永久阻塞的 tud_task():
 * OPT_OS_FREERTOS 下 tud_task() = tud_task_ext(UINT32_MAX),没有 USB 事件时
 * 会一直阻塞,导致其后的 usb_log_pump() 只有在总线有流量时才跑 —— 空闲时日志
 * 永远排不进 vendor FIFO,主机在 0x82 上读不到任何东西。给 10ms 上限后,即便
 * 没有 USB 事件也会周期性醒来把积压日志刷进 IN 端点。 */
static void usb_task(void *arg) {
    usbscpi_t *dev = (usbscpi_t *)arg;
    for (;;) {
        tud_task_ext(10, false); /* 驱动 tinyusb;最多阻塞 10ms 便返回 */
        usbscpi_task(dev);       /* 处理 core 的延迟工作(无则 no-op) */
        usb_vendor_pump();       /* records + log text -> vendor bulk-IN (0x82) */
    }
}

/* ---------- 无线初始化任务 ----------
 * WiFi + NimBLE 起步占用较多栈,且可能耗时;放到独立任务里,使 USB(USBTMC)
 * 枚举不被其阻塞。即便扫描初始化卡住,USB 仍可响应。 */
static void scan_init_task(void *arg) {
    (void)arg;
    wifi_scan_init();     /* NVS + netif + Wi-Fi STA */
    ble_scan_init();      /* NimBLE controller + host task */
    ble_conn_init();      /* Security Manager config (KeyboardDisplay, MITM, SC) */
    vTaskDelete(NULL);
}

void app_main(void) {
    adc_setup();

    /* 安装日志钩子:ESP_LOGx 同时走 UART 和 USB vendor bulk-IN(0x82)。
     * 在建任务前安装,尽早捕获启动日志。 */
    s_log_sb = xStreamBufferCreate(4096, 1);
    s_prev_vprintf = esp_log_set_vprintf(log_tee_vprintf);

    usb_phy_start();
    tusb_init();

    /* *IDN? carries the same chip-unique serial as the USB descriptor. */
    static char idn[64];
    snprintf(idn, sizeof(idn), "IoTSploit,ESP32S3,%s,%s", board_serial(), esp_app_get_description()->version);

    usbscpi_config_t cfg = {
        .usb_tx        = usb_tx,
        .line_buf      = s_line,
        .line_buf_len  = sizeof(s_line),
        /* From the glue, not a literal: a block the glue cannot buffer is
         * rejected in the IN path and the query just never answers. */
        .max_block_len = usbscpi_tinyusb_tx_capacity(),
        .idn           = idn,
        .data_avail    = adc_avail,
        .data_read     = adc_read_cb,
        .io_buf        = s_io,
        .io_buf_len    = sizeof(s_io),
        .proto         = 1,
        .mtu           = 256,
        .descriptor    = &s_descriptor,
    };

    usbscpi_t *dev = usbscpi_init(s_storage, sizeof(s_storage), &cfg);
    ESP_ERROR_CHECK(dev ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(usbscpi_register(dev, esp_scpi_commands));
    s_usb_dev = dev;
    usbscpi_tinyusb_bind(dev);          /* glue 接管 IN/OUT 路径 */

    /* Second context for SCPI over TCP: same commands and descriptor, its own
     * buffers and its own usb_tx. See net_scpi.h for why it cannot be shared. */
    net_scpi_start(&cfg, esp_scpi_commands);

    xTaskCreate(usb_task, "usb", 6144, dev, 5, NULL);
    /* USB 起来后再异步初始化无线,避免阻塞枚举 */
    xTaskCreate(scan_init_task, "scan_init", 12288, NULL, 4, NULL);
    /* 状态信标:10 秒周期向设备日志流输出实时状态 */
    xTaskCreate(beacon_task, "beacon", 2560, NULL, 3, NULL);
}
