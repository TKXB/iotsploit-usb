#include "usbscpi_nimble.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "scpi.ble";

/* Defined in NimBLE's store/config module; not declared in a public header. */
void ble_store_config_init(void);

/* 7d7e000n-ce83-4a46-ac8a-2e014163ef3e, little-endian as NimBLE stores it. */
#define SCPI_UUID(n) BLE_UUID128_DECLARE(0x3e, 0xef, 0x63, 0x41, 0x01, 0x2e, 0x8a, 0xac, \
                                         0x46, 0x4a, 0x83, 0xce, (n), 0x00, 0x7e, 0x7d)
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x3e, 0xef, 0x63, 0x41, 0x01, 0x2e, 0x8a, 0xac,
                     0x46, 0x4a, 0x83, 0xce, 0x01, 0x00, 0x7e, 0x7d);

/* Received bytes wait here for the worker. 16 x 128 bytes holds the longest
 * attribute write (512) several times over. */
#define RX_CHUNK 128
#define RX_DEPTH 16
/* How long a congested notification waits before it is retried. */
#define TX_RETRY_MS 20

typedef struct {
    uint32_t session;
    uint16_t len;
    uint8_t data[RX_CHUNK];
} rx_item_t;

static usbscpi_t        *s_ctx;
static QueueHandle_t     s_rx;
static SemaphoreHandle_t s_tx_room;   /* given when a notification went out or the link dropped */
static uint8_t           s_own_addr_type;
static uint16_t          s_tx_handle;

/* Written on the NimBLE host task, read by the worker. s_session changes on
 * every connect and disconnect, so bytes and replies from one connection can
 * never reach the next. */
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint32_t s_session;
static volatile bool     s_subscribed;
static volatile bool     s_secure;     /* encrypted with an LE Secure Connections bond */
static struct ble_npl_event s_security_event;
static uint16_t s_security_conn;
static uint32_t s_security_session;

/* Worker only: the session whose bytes are being executed. */
static uint32_t s_rx_session;

static int gap_event(struct ble_gap_event *event, void *arg);

/* ---------- RX: host task -> queue -> worker ---------- */

static int rx_access(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!s_secure) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    /* All of the write or none of it: half a command would execute as garbage. */
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if ((UBaseType_t)((len + RX_CHUNK - 1) / RX_CHUNK) > uxQueueSpacesAvailable(s_rx)) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    rx_item_t item = { .session = s_session };
    for (uint16_t off = 0; off < len; off += item.len) {
        item.len = (uint16_t)(len - off < RX_CHUNK ? len - off : RX_CHUNK);
        if (os_mbuf_copydata(ctxt->om, off, item.len, item.data) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        (void)xQueueSend(s_rx, &item, 0);   /* room checked above; one producer */
    }
    return 0;
}

static int tx_access(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;   /* notify only */
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = SCPI_UUID(0x02),
                .access_cb = rx_access,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid       = SCPI_UUID(0x03),
                .access_cb  = tx_access,
                .val_handle = &s_tx_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};

static void worker_task(void *arg) {
    (void)arg;
    rx_item_t item;
    for (;;) {
        if (xQueueReceive(s_rx, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (item.session != s_rx_session) {
            usbscpi_clear(s_ctx);
            s_rx_session = item.session;
        }
        (void)usbscpi_on_rx(s_ctx, item.data, item.len, false);
    }
}

/* ---------- TX: worker -> notifications ---------- */

int usbscpi_nimble_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user;
    (void)eom;   /* a notification stream has no message boundary to signal */
    while (len) {
        uint16_t conn = s_conn;
        if (s_rx_session != s_session || !s_subscribed) {
            return -1;
        }
        uint16_t mtu = ble_att_mtu(conn);
        if (mtu <= 3) {
            return -1;
        }
        size_t n = len < (size_t)(mtu - 3) ? len : (size_t)(mtu - 3);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)n);
        int rc = om ? ble_gatts_notify_custom(conn, s_tx_handle, om) : BLE_HS_ENOMEM;
        if (rc == 0) {
            data += n;
            len -= n;
        } else if (rc == BLE_HS_ENOMEM) {
            (void)xSemaphoreTake(s_tx_room, pdMS_TO_TICKS(TX_RETRY_MS));
        } else {
            return -1;
        }
    }
    return 0;
}

/* ---------- GAP ---------- */

static void advertise(void) {
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);

    /* The 128-bit UUID leaves no room for a name in the advertisement. */
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    const char *name = ble_svc_gap_device_name();
    rsp.name = (uint8_t *)name;
    rsp.name_len = (uint8_t)strlen(name);
    rsp.name_is_complete = 1;
    if (rc == 0) {
        rc = ble_gap_adv_rsp_set_fields(&rsp);
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (rc == 0) {
        rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising failed rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as \"%s\"", name);
    }
}

/* True if this link's peer has a persisted LE Secure Connections bond. */
static bool bonded_with_sc(uint16_t conn, ble_addr_t *peer) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) != 0) {
        return false;
    }
    *peer = desc.peer_id_addr;
    struct ble_store_key_sec key;
    memset(&key, 0, sizeof(key));
    key.peer_addr = desc.peer_id_addr;
    struct ble_store_value_sec value;
    return desc.sec_state.encrypted && ble_store_read_peer_sec(&key, &value) == 0 && value.sc;
}

/* NimBLE persists a newly paired bond after returning from ENC_CHANGE.
 * Check it on the host event queue once that processing has completed. */
static void check_security(struct ble_npl_event *event) {
    (void)event;
    if (s_security_session != s_session || s_security_conn != s_conn) {
        return;
    }
    ble_addr_t peer;
    memset(&peer, 0, sizeof(peer));
    if (bonded_with_sc(s_security_conn, &peer)) {
        s_secure = true;
        ESP_LOGI(TAG, "link secured");
        return;
    }
    ESP_LOGW(TAG, "refusing link: not an LE Secure Connections bond");
    (void)ble_store_util_delete_peer(&peer);
    (void)ble_gap_terminate(s_security_conn, BLE_ERR_AUTH_FAIL);
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            advertise();
            return 0;
        }
        s_subscribed = false;
        s_secure = false;
        s_conn = event->connect.conn_handle;
        s_session++;
        ESP_LOGI(TAG, "connected handle=%u", (unsigned)s_conn);
        /* Ask for encryption now rather than on the first refused write. */
        (void)ble_gap_security_initiate(event->connect.conn_handle);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_secure = false;
        s_session++;
        (void)xSemaphoreGive(s_tx_room);
        ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
        advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.status == 0) {
            s_security_conn = event->enc_change.conn_handle;
            s_security_session = s_session;
            ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_security_event);
            return 0;
        }
        ESP_LOGW(TAG, "refusing link: status=%d, not an LE Secure Connections bond",
                 event->enc_change.status);
        (void)ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The host forgot its bond: drop ours and pair again. */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            (void)ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_handle) {
            s_subscribed = event->subscribe.cur_notify;
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        (void)xSemaphoreGive(s_tx_room);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%u", (unsigned)event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void) {
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    advertise();
}

static void on_reset(int reason) {
    ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason);
}

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();   /* returns only after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

int usbscpi_nimble_start(usbscpi_t *ctx, const char *device_name) {
    if (!ctx || !device_name || s_ctx) {
        return -1;
    }
    s_ctx = ctx;
    s_rx = xQueueCreate(RX_DEPTH, sizeof(rx_item_t));
    s_tx_room = xSemaphoreCreateBinary();
    if (!s_rx || !s_tx_room || nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed");
        return -1;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    /* Headless: Just Works over LE Secure Connections, bonded. */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_svcs);
    }
    if (rc == 0) {
        rc = ble_svc_gap_device_name_set(device_name);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup failed rc=%d", rc);
        return -1;
    }
    ble_store_config_init();
    ble_npl_event_init(&s_security_event, check_security, NULL);

    if (xTaskCreate(worker_task, "scpi_ble", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "worker task creation failed");
        return -1;
    }
    nimble_port_freertos_init(host_task);
    return 0;
}
