#ifndef WLAN_SCPI_H
#define WLAN_SCPI_H

#include <stdbool.h>

#include "usbscpi/usbscpi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The WLAN:SCAN and WLAN:CONNect jobs over wifi_scan.h, shared by every ESP32
 * example that offers them. Add WLAN_COMMANDS to the example's command list and
 * WLAN_WORKFLOWS to its workflow array.
 *
 * Scanning and association take the radio away from whatever link rides on
 * Wi-Fi, so which context may start them is the example's decision: it defines
 * wlan_scpi_allowed(). `scan` is true for WLAN:SCAN:STARt, false for
 * WLAN:CONNect:STARt, :STOP and :CLEar. A refusal is reported as
 * SCPI_ERROR_SETTINGS_CONFLICT. */
bool wlan_scpi_allowed(scpi_t *ctx, bool scan);

scpi_result_t wlan_cmd_scan_start(scpi_t *ctx);
scpi_result_t wlan_cmd_scan_state(scpi_t *ctx);
scpi_result_t wlan_cmd_scan_done(scpi_t *ctx);
scpi_result_t wlan_cmd_scan_count(scpi_t *ctx);
scpi_result_t wlan_cmd_scan_fetch(scpi_t *ctx);
scpi_result_t wlan_cmd_connect_start(scpi_t *ctx);
scpi_result_t wlan_cmd_connect_stop(scpi_t *ctx);
scpi_result_t wlan_cmd_connect_state(scpi_t *ctx);
scpi_result_t wlan_cmd_connect_count(scpi_t *ctx);
scpi_result_t wlan_cmd_connect_fetch(scpi_t *ctx);
scpi_result_t wlan_cmd_connect_clear(scpi_t *ctx);

extern const usbscpi_param_desc_t wlan_scan_fetch_params[1];
extern const usbscpi_param_desc_t wlan_connect_params[2];
extern const usbscpi_param_desc_t wlan_connect_fetch_params[1];

#define WLAN_COMMANDS(CMD, ALIAS)                                                         \
    CMD("WLAN:SCAN:STARt",   wlan_cmd_scan_start, "command", "Scan for Wi-Fi access points", \
        USBSCPI_NO_PARAMS, "none")                                                        \
    CMD("WLAN:SCAN:STATe?",  wlan_cmd_scan_state, "query",   "IDLE, RUNNING or DONE",     \
        USBSCPI_NO_PARAMS, "string")                                                      \
    CMD("WLAN:SCAN:COUNt?",  wlan_cmd_scan_count, "query",   "Access points found",       \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("WLAN:SCAN:FETCh?",  wlan_cmd_scan_fetch, "query",   "Access point by index",     \
        USBSCPI_PARAMS(wlan_scan_fetch_params), "string")                                 \
    CMD("WLAN:CONNect:STARt", wlan_cmd_connect_start, "command", "Try one SSID/password", \
        USBSCPI_PARAMS(wlan_connect_params), "none")                                      \
    CMD("WLAN:CONNect:STOP", wlan_cmd_connect_stop, "command", "Cancel or disconnect",    \
        USBSCPI_NO_PARAMS, "none")                                                        \
    CMD("WLAN:CONNect:STATe?", wlan_cmd_connect_state, "query", "IDLE, RUNNING, DONE or FAILED", \
        USBSCPI_NO_PARAMS, "string")                                                      \
    CMD("WLAN:CONNect:COUNt?", wlan_cmd_connect_count, "query", "Completed attempt results", \
        USBSCPI_NO_PARAMS, "u32")                                                         \
    CMD("WLAN:CONNect:FETCh?", wlan_cmd_connect_fetch, "query", "result,ip,reason; index 0", \
        USBSCPI_PARAMS(wlan_connect_fetch_params), "string")                              \
    CMD("WLAN:CONNect:CLEar", wlan_cmd_connect_clear, "command", "Clear an offline result", \
        USBSCPI_NO_PARAMS, "none")

#define WLAN_WORKFLOWS                                                                    \
    { USBSCPI_WF_ACQUIRE("wifi-connect", "WLAN:CONNect", "Try one Wi-Fi credential",      \
                         "result:string,ip:string,reason:i32", 35000) },                  \
    { USBSCPI_WF_ACQUIRE("wifi-scan", "WLAN:SCAN", "Scan for Wi-Fi access points",        \
                         "ssid:string,rssi:i32:dbm,channel:u32,authmode:string,bssid:mac", \
                         15000) }

#ifdef __cplusplus
}
#endif

#endif /* WLAN_SCPI_H */
