#include "ble_scan.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "usbscpi/atomic.h"

static const char *TAG = "ble";

/* ---- RSSI data plane ----------------------------------------------------
 *
 * 16 KiB holds ~273 records. Wi-Fi and BLE already own most of the SRAM, so
 * this is the tradeoff to revisit first if reports are being dropped: at a few
 * thousand reports/second in a busy environment it is well under a second of
 * slack.
 *
 * WHAT THE DROP COUNTER CANNOT SEE: reports discarded by the controller or by
 * NimBLE before gap_event_cb() runs. SocketCAN has SO_RXQ_OVFL for exactly
 * this; NimBLE exposes no equivalent that I could find. So ring overflow is
 * counted honestly and controller-level loss is invisible — weaker than the
 * CAN path, and the schema says so rather than implying otherwise. */
#define BLE_STREAM_RING_BYTES 16384u
#define BLE_STREAM_ADV_MAX    31u

/* 59 bytes of content, which the uint64_t members pad to a 64-byte stride.
 *
 * Note that 64 IS a power of two, unlike the 20- and 88-byte strides used
 * elsewhere. Socket buffer space comes in round binary sizes, so a partial
 * send here tends to land on a record boundary and the transport's
 * realignment path will rarely fire on this device. That is not a correctness
 * problem — realign() handles either case and is covered by tests at strides
 * that do exercise it — but it does mean this build is not the one to trust
 * for exercising it. The ring is also an exact multiple of the stride
 * (16384 / 64 = 256), so records never wrap mid-record either. */
typedef struct {
    uint64_t ts_us;
    uint64_t dropped;      /* running total at capture, in-band */
    uint8_t  addr[6];
    uint8_t  addr_type;
    int8_t   rssi;
    uint8_t  adv_type;
    uint8_t  data_len;
    uint16_t rsv;
    uint8_t  data[BLE_STREAM_ADV_MAX];
} ble_rec_t;

static usbscpi_ring_t s_stream_ring;
static uint8_t        s_stream_store[BLE_STREAM_RING_BYTES];
static size_t         s_stream_on;
static uint64_t       s_stream_count;
static uint64_t       s_stream_dropped;

usbscpi_ring_t *ble_stream_ring(void)   { return &s_stream_ring; }
size_t   ble_stream_stride(void)        { return sizeof(ble_rec_t); }
int      ble_stream_enabled(void)       { return usbscpi_load_acquire(&s_stream_on) != 0; }
void     ble_stream_enable(int on)      { usbscpi_store_release(&s_stream_on, on ? 1u : 0u); }
uint64_t ble_stream_count(void)         { return s_stream_count; }
uint64_t ble_stream_dropped(void)       { return s_stream_dropped; }

const char *ble_stream_fields(void) {
    return "ts_us:u64:us,dropped:u64,addr:mac,addr_type:u8,rssi:i8:dbm,"
           "adv_type:u8,data_len:u8,rsv:u16,data:bytes31";
}

/* Called from gap_event_cb on the NimBLE host task: the single producer. */
static void stream_push(const ble_addr_t *addr, int8_t rssi, uint8_t adv_type,
                        const uint8_t *adv, uint8_t adv_len) {
    if (!ble_stream_enabled()) {
        return;  /* not a drop: the counter measures loss from a running
                  * capture, not time spent idle */
    }
    ble_rec_t r;
    memset(&r, 0, sizeof r);
    r.ts_us     = (uint64_t)esp_timer_get_time();
    r.dropped   = s_stream_dropped;
    memcpy(r.addr, addr->val, 6);
    r.addr_type = addr->type;
    r.rssi      = rssi;
    r.adv_type  = adv_type;
    r.data_len  = adv_len > BLE_STREAM_ADV_MAX ? BLE_STREAM_ADV_MAX : adv_len;
    if (adv && r.data_len) {
        memcpy(r.data, adv, r.data_len);
    }
    /* Capacity check first: usbscpi_ring_write() truncates to fit, and a
     * truncated record desynchronises the consumer for every record after it.
     * Drop whole records or none. */
    if (usbscpi_ring_free(&s_stream_ring) < sizeof r) {
        s_stream_dropped++;
        return;
    }
    usbscpi_ring_write(&s_stream_ring, (const uint8_t *)&r, sizeof r);
    s_stream_count++;
}

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
        const uint8_t *v = event->disc.addr.val;  /* little-endian: MSB..LSB */
        ESP_LOGD(TAG, "disc %02X:%02X:%02X:%02X:%02X:%02X rssi=%d name=\"%.*s\"",
                 v[5], v[4], v[3], v[2], v[1], v[0],
                 event->disc.rssi,
                 fields.name_len ? (int)fields.name_len : 0,
                 fields.name_len ? (const char *)fields.name : "");
        store_device(&event->disc.addr, event->disc.rssi,
                     event->disc.event_type, &fields);
        /* Every report, not just the first per device: the dedup above is what
         * makes the workflow unable to answer RSSI-over-time. */
        stream_push(&event->disc.addr, event->disc.rssi, event->disc.event_type,
                    event->disc.data, event->disc.length_data);
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_done = 1;
        ESP_LOGI(TAG, "ble scan complete: %u devs", (unsigned)s_dev_count);
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_ready = true;
    ESP_LOGI(TAG, "nimble synced, ready");
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();                 /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* Continuous passive scan: duration 0 means "until stopped", which is what a
 * stream wants. The workflow's timed scan stays as it is. */
int ble_stream_scan_start(void) {
    if (!s_ready) {
        ESP_LOGE(TAG, "ble stream start: stack not synced");
        return -1;
    }
    struct ble_gap_disc_params p = { 0 };
    p.passive = 1;
    p.filter_duplicates = 0;   /* duplicates ARE the signal here */
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc (stream) rc=%d", rc);
        return -1;
    }
    return 0;
}

void ble_scan_init(void) {
    if (usbscpi_ring_init(&s_stream_ring, s_stream_store, sizeof s_stream_store) != 0) {
        ESP_LOGE(TAG, "ble stream ring init failed");
    }
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
}

int ble_scan_start(unsigned secs) {
    if (!s_ready) {
        ESP_LOGE(TAG, "ble scan start: stack not synced");
        return -1;
    }
    s_done = 0;
    s_dev_count = 0;
    struct ble_gap_disc_params p = { 0 };
    p.passive = 1;                     /* listen only, no scan requests */
    int32_t duration = (secs == 0) ? BLE_HS_FOREVER : (int32_t)(secs * 1000);
    int rc = ble_gap_disc(s_own_addr_type, duration, &p, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble scan start failed: rc=%d", rc);
        return -1;
    }
    ESP_LOGI(TAG, "ble scan start secs=%u", secs);
    return 0;
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

int ble_scan_addr(size_t index, uint8_t out_val[6], uint8_t *out_type) {
    if (index >= s_dev_count || !out_val) {
        return -1;
    }
    memcpy(out_val, s_devs[index].addr.val, 6);
    if (out_type) {
        *out_type = s_devs[index].addr.type;
    }
    return 0;
}
