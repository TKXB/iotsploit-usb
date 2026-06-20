#include "ble_scan.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

#define BLE_SCAN_MAX_DEV 20
#define BLE_SCAN_NAME_LEN 32

typedef struct {
    ble_addr_t addr;
    int8_t     rssi;
    uint8_t    adv_type;
    char       name[BLE_SCAN_NAME_LEN];
} ble_dev_t;

static ble_dev_t      s_devs[BLE_SCAN_MAX_DEV];
static volatile size_t s_dev_count;
static volatile int    s_done;
static uint8_t         s_own_addr_type;
static volatile bool   s_ready;   /* host stack synced, ok to scan */

/* Store / update a device, deduplicated by address (BLE event-task ctx). */
static void store_device(const ble_addr_t *addr, int8_t rssi, uint8_t adv_type,
                         const struct ble_hs_adv_fields *fields) {
    for (size_t i = 0; i < s_dev_count; i++) {
        if (memcmp(s_devs[i].addr.val, addr->val, 6) == 0) {
            s_devs[i].rssi = rssi;   /* refresh signal strength */
            return;
        }
    }
    if (s_dev_count >= BLE_SCAN_MAX_DEV) {
        return;
    }
    ble_dev_t *d = &s_devs[s_dev_count];
    d->addr = *addr;
    d->rssi = rssi;
    d->adv_type = adv_type;
    d->name[0] = '\0';
    if (fields && fields->name_len) {
        size_t n = fields->name_len;
        if (n >= sizeof(d->name)) {
            n = sizeof(d->name) - 1;
        }
        memcpy(d->name, fields->name, n);
        d->name[n] = '\0';
    }
    s_dev_count++;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
        store_device(&event->disc.addr, event->disc.rssi,
                     event->disc.event_type, &fields);
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_done = 1;
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_ready = true;
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();                 /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

void ble_scan_init(void) {
    if (nimble_port_init() != ESP_OK) {
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
}

int ble_scan_start(unsigned secs) {
    if (!s_ready) {
        return -1;
    }
    s_done = 0;
    s_dev_count = 0;
    struct ble_gap_disc_params p = { 0 };
    p.passive = 1;                     /* listen only, no scan requests */
    int32_t duration = (secs == 0) ? BLE_HS_FOREVER : (int32_t)(secs * 1000);
    return ble_gap_disc(s_own_addr_type, duration, &p, gap_event_cb, NULL) == 0 ? 0 : -1;
}

int ble_scan_done(void) {
    return s_done;
}

size_t ble_scan_count(void) {
    return s_dev_count;
}

int ble_scan_get(size_t index, char *out, size_t out_len) {
    if (index >= s_dev_count || !out) {
        return -1;
    }
    const ble_dev_t *d = &s_devs[index];
    const uint8_t *v = d->addr.val;    /* little-endian: print MSB..LSB */
    snprintf(out, out_len,
             "%02X:%02X:%02X:%02X:%02X:%02X,%d,\"%s\",%u",
             v[5], v[4], v[3], v[2], v[1], v[0],
             d->rssi, d->name, d->adv_type);
    return 0;
}
