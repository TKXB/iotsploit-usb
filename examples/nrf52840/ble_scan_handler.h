#ifndef BLE_SCAN_HANDLER_H
#define BLE_SCAN_HANDLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SCAN_RESULTS 32
#define BLE_NAME_MAX_LEN 31

typedef struct {
    uint8_t  addr[6];       /* BLE MAC address (little-endian on nRF) */
    uint8_t  addr_type;     /* 0=public, 1=random */
    int8_t   rssi;          /* signal strength in dBm */
    char     name[BLE_NAME_MAX_LEN + 1]; /* device name from adv data */
    uint8_t  adv_type;      /* 0=connectable, 1=non-connectable, 2=scannable */
} ble_scan_result_t;

/**
 * Initialize SoftDevice (s140) + BLE stack + scanning module.
 * Must be called before any other ble_scan_* function.
 * @return 0 on success, non-zero on error.
 */
int ble_scan_init(void);

/** Start BLE scanning. */
void ble_scan_start(void);

/** Stop BLE scanning. */
void ble_scan_stop(void);

/** Clear all stored scan results. */
void ble_scan_clear(void);

/** Get number of devices found so far. */
uint16_t ble_scan_count(void);

/** Get scan result by index.
 * @param index  Zero-based index into results.
 * @param out     Pointer to output struct.
 * @return true on success, false if index out of range.
 */
bool ble_scan_get_result(uint16_t index, ble_scan_result_t *out);

/** Check if currently scanning. */
bool ble_scan_is_scanning(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_SCAN_HANDLER_H */
