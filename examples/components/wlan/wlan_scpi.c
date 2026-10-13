#include "wlan_scpi.h"

#include <string.h>

#include "esp_log.h"

#include "wifi_scan.h"

static const char *TAG = "scpi";

const usbscpi_param_desc_t wlan_scan_fetch_params[1] = {
    USBSCPI_PARAM_PICK("index", "WLAN:SCAN:COUNt?", "WLAN:SCAN:FETCh?"),
};
const usbscpi_param_desc_t wlan_connect_params[2] = {
    USBSCPI_PARAM("ssid", "string", true),
    USBSCPI_PARAM("password", "string", true),
};
const usbscpi_param_desc_t wlan_connect_fetch_params[1] = {
    USBSCPI_PARAM_PICK("index", "WLAN:CONNect:COUNt?", "WLAN:CONNect:FETCh?"),
};

static bool allowed(scpi_t *ctx, bool scan) {
    if (wlan_scpi_allowed(ctx, scan)) return true;
    SCPI_ErrorPush(ctx, SCPI_ERROR_SETTINGS_CONFLICT);
    return false;
}

/* ---------- WLAN:SCAN (trigger, poll, fetch by row) ---------- */
static bool s_wlan_started;

scpi_result_t wlan_cmd_scan_start(scpi_t *ctx) {
    if (!allowed(ctx, true)) return SCPI_RES_ERR;
    ESP_LOGI(TAG, "WLAN:SCAN");
    if (wifi_scan_start() != 0) return SCPI_RES_ERR;
    s_wlan_started = true;
    return SCPI_RES_OK;
}

scpi_result_t wlan_cmd_scan_state(scpi_t *ctx) {
    SCPI_ResultMnemonic(ctx, !s_wlan_started ? "IDLE" : wifi_scan_done() ? "DONE" : "RUNNING");
    return SCPI_RES_OK;
}

scpi_result_t wlan_cmd_scan_done(scpi_t *ctx) {
    SCPI_ResultBool(ctx, wifi_scan_done());
    return SCPI_RES_OK;
}

scpi_result_t wlan_cmd_scan_count(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, (uint32_t)wifi_scan_count());
    return SCPI_RES_OK;
}

scpi_result_t wlan_cmd_scan_fetch(scpi_t *ctx) {
    uint32_t idx = 0;
    char buf[128];
    if (SCPI_ParamUInt32(ctx, &idx, TRUE) != TRUE) return SCPI_RES_ERR;
    if (wifi_scan_get(idx, buf, sizeof(buf)) != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    SCPI_ResultCharacters(ctx, buf, strlen(buf));   /* bare CSV: ResultText would quote it */
    return SCPI_RES_OK;
}

/* ---------- WLAN:CONNect (one bounded association attempt) ---------- */
static bool s_wlan_conn_failure_reported;

/* Quoted ASCII or a length-delimited UTF-8 block; never truncate credentials. */
static bool wlan_credential(scpi_t *ctx, char *out, size_t capacity, size_t *len) {
    scpi_parameter_t param;
    if (!SCPI_Parameter(ctx, &param, TRUE)) return false;
    const char *text = param.ptr;
    size_t size = param.len;
    char quote = 0;
    if (param.type == SCPI_TOKEN_SINGLE_QUOTE_PROGRAM_DATA ||
        param.type == SCPI_TOKEN_DOUBLE_QUOTE_PROGRAM_DATA) {
        quote = *text++;
        size -= 2;
    } else if (param.type != SCPI_TOKEN_ARBITRARY_BLOCK_PROGRAM_DATA) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_TYPE_ERROR);
        return false;
    }
    *len = 0;
    for (size_t i = 0; i < size; i++) {
        if (*len + 1 >= capacity) {
            SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
            return false;
        }
        out[(*len)++] = text[i];
        if (quote && text[i] == quote) i++;
    }
    out[*len] = 0;
    return true;
}

scpi_result_t wlan_cmd_connect_start(scpi_t *ctx) {
    if (!allowed(ctx, false)) return SCPI_RES_ERR;
    char ssid[33], password[64];
    size_t ssid_len, password_len;
    if (!wlan_credential(ctx, ssid, sizeof(ssid), &ssid_len) ||
        !wlan_credential(ctx, password, sizeof(password), &password_len))
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

scpi_result_t wlan_cmd_connect_stop(scpi_t *ctx) {
    if (!allowed(ctx, false)) return SCPI_RES_ERR;
    if (wifi_sta_stop() != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

scpi_result_t wlan_cmd_connect_clear(scpi_t *ctx) {
    if (!allowed(ctx, false)) return SCPI_RES_ERR;
    if (wifi_sta_clear() != 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_SETTINGS_CONFLICT);
        return SCPI_RES_ERR;
    }
    s_wlan_conn_failure_reported = false;
    return SCPI_RES_OK;
}

scpi_result_t wlan_cmd_connect_state(scpi_t *ctx) {
    wifi_sta_result_t result;
    wifi_sta_result(&result);
    if (strcmp(result.state, "FAILED") == 0 && !s_wlan_conn_failure_reported) {
        s_wlan_conn_failure_reported = true;
        usbscpi_queue_error(ctx, SCPI_ERROR_EXECUTION_ERROR);
    }
    SCPI_ResultMnemonic(ctx, result.state);
    return SCPI_RES_OK;
}

scpi_result_t wlan_cmd_connect_count(scpi_t *ctx) {
    wifi_sta_result_t result;
    wifi_sta_result(&result);
    SCPI_ResultUInt32(ctx, strcmp(result.result, "NONE") != 0);
    return SCPI_RES_OK;
}

scpi_result_t wlan_cmd_connect_fetch(scpi_t *ctx) {
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
