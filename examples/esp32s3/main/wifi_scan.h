#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up NVS + netif + Wi-Fi in STA mode and register the scan-done event.
 * Safe to call once at startup, before USB/SCPI init. */
void wifi_scan_init(void);

/* 1 once Wi-Fi STA has finished coming up (init done), else 0. */
bool wifi_scan_ready(void);

/* Kick off a non-blocking scan of all channels. Returns 0 on success.
 * Results are captured asynchronously in the WIFI_EVENT_SCAN_DONE handler. */
int wifi_scan_start(void);

/* 1 once the most recent scan has finished and results are ready, else 0. */
int wifi_scan_done(void);

/* Number of APs captured by the last completed scan. */
size_t wifi_scan_count(void);

/* ---- STA association (used by the SCPI-over-TCP transport) ---- */

/* Associate with `ssid`/`password` and keep retrying on disconnect.
 * Non-blocking: use wifi_sta_ip() to learn when an address has been assigned.
 * Returns 0 if the request was accepted. */
int wifi_sta_connect(const char *ssid, const char *password);

/* 1 once the station holds an IPv4 lease, else 0. */
int wifi_sta_has_ip(void);

/* Copy the station's dotted-quad address into out. Returns 0 on success, -1 if
 * there is no lease yet. */
int wifi_sta_ip(char *out, size_t out_len);

/* Format AP #index as a CSV row into out:
 *   "<ssid>",<rssi>,<channel>,<authmode>,<bssid>
 * Returns 0 on success, -1 if index is out of range. */
int wifi_scan_get(size_t index, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_SCAN_H */
