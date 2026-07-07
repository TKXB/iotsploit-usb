#include "ble_spp_security.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble.spp";

#define BLE_SPP_DEVICE_NAME      "iotsploit-spp-sec"
#define BLE_SVC_SPP_UUID16       0xABF0   /* SPP service UUID     */
#define BLE_SVC_SPP_CHR_UUID16   0xABF1   /* SPP data char UUID   */

/* Defined in NimBLE's store/config module; not declared in a public header. */
void ble_store_config_init(void);

/* All NimBLE callbacks run on the host task; SCPI accessors run on the USB
 * task. State is shared through volatiles + simple flags, matching the
 * lock-free style used elsewhere in this repo. A single connection is tracked. */
static volatile int      s_ready;          /* host stack synced, ok to advertise */
static volatile int      s_conn_state = BLE_SPP_DISCONNECTED;
static volatile int      s_pair_state = BLE_SPP_PAIR_IDLE;
static volatile int      s_adv_on;         /* advertising intent (re-arm after disconnect) */
static volatile uint16_t s_conn_handle;
static volatile uint32_t s_disp_passkey;   /* passkey we generated (DISPLAY)  */
static volatile uint32_t s_numcmp_val;     /* number to compare (NUMCMP)      */
static volatile int      s_last_status;    /* last GAP connect/disc/enc status */

static uint8_t  s_own_addr_type;
static uint16_t s_spp_val_handle;          /* SPP characteristic value handle */

static int gap_event_cb(struct ble_gap_event *event, void *arg);

/* ---------- SPP GATT service ----------
 * A single primary service (0xABF0) with one read/write/notify characteristic
 * (0xABF1). The data path is intentionally minimal: this example exists to
 * demonstrate connection security, so the access callback only logs. */
static int spp_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        ESP_LOGI(TAG, "spp chr read");
        break;
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        ESP_LOGI(TAG, "spp chr write len=%u", (unsigned)OS_MBUF_PKTLEN(ctxt->om));
        break;
    default:
        break;
    }
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLE_SVC_SPP_UUID16),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = BLE_UUID16_DECLARE(BLE_SVC_SPP_CHR_UUID16),
                .access_cb  = spp_chr_access_cb,
                .val_handle = &s_spp_val_handle,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                              BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};

static int gatt_svr_init(void) {
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        return rc;
    }
    return ble_gatts_add_svcs(s_gatt_svcs);
}

/* ---------- Advertising ----------
 * General discoverable, undirected connectable, with the SPP service UUID and
 * complete device name so a central can find and identify us. */
static int advertise(void) {
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    fields.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(BLE_SVC_SPP_UUID16) };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed rc=%d", rc);
        return rc;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed rc=%d", rc);
        return rc;
    }
    ESP_LOGI(TAG, "advertising as \"%s\"", name);
    return 0;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_last_status = event->connect.status;
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_conn_state  = BLE_SPP_CONNECTED;
            s_pair_state  = BLE_SPP_PAIR_IDLE;
            ESP_LOGI(TAG, "central connected handle=%u", (unsigned)s_conn_handle);
        } else {
            ESP_LOGE(TAG, "connect failed status=%d", event->connect.status);
            /* Nobody connected; keep advertising if that was the intent. */
            if (s_adv_on) {
                advertise();
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_last_status = event->disconnect.reason;
        s_conn_state  = BLE_SPP_DISCONNECTED;
        s_pair_state  = BLE_SPP_PAIR_IDLE;
        ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
        /* Resume advertising so the next central can connect. */
        if (s_adv_on) {
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "conn update status=%d", event->conn_update.status);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "adv complete reason=%d", event->adv_complete.reason);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        s_last_status = event->enc_change.status;
        s_pair_state = (event->enc_change.status == 0) ? BLE_SPP_PAIR_DONE
                                                       : BLE_SPP_PAIR_FAILED;
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "encryption established (pair done)");
        } else {
            ESP_LOGE(TAG, "enc change failed status=%d", event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io = {0};
        switch (event->passkey.params.action) {
        case BLE_SM_IOACT_INPUT:
            /* Peer displays a passkey; host must type it in via SCPI. */
            s_pair_state = BLE_SPP_PAIR_INPUT_NEEDED;
            ESP_LOGI(TAG, "passkey action: input needed (enter peer passkey)");
            break;
        case BLE_SM_IOACT_DISP:
            /* We display a passkey; peer types it in. Generate + show it. */
            s_disp_passkey = esp_random() % 1000000u;
            io.action  = BLE_SM_IOACT_DISP;
            io.passkey = s_disp_passkey;
            ble_sm_inject_io(event->passkey.conn_handle, &io);
            s_pair_state = BLE_SPP_PAIR_DISPLAY_KEY;
            ESP_LOGI(TAG, "passkey action: display key %06u", (unsigned)s_disp_passkey);
            break;
        case BLE_SM_IOACT_NUMCMP:
            /* Both sides display the same number; host confirms the match. */
            s_numcmp_val = event->passkey.params.numcmp;
            s_pair_state = BLE_SPP_PAIR_NUMCMP_NEEDED;
            ESP_LOGI(TAG, "passkey action: numcmp %u", (unsigned)s_numcmp_val);
            break;
        default:
            s_pair_state = BLE_SPP_PAIR_FAILED;
            ESP_LOGW(TAG, "passkey action: unsupported action %d",
                     event->passkey.params.action);
            break;
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* Already bonded: drop the stale bond and re-pair from scratch. */
        ESP_LOGW(TAG, "repeat pairing: deleting stale bond");
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%d cur_notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu update mtu=%d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "id_infer_auto failed");
        return;
    }
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "nimble synced, addr %02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    s_ready = 1;
}

static void on_reset(int reason) {
    ESP_LOGE(TAG, "nimble reset; reason=%d", reason);
    s_ready = 0;
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();                 /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

void ble_spp_security_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return;
    }

    ble_hs_cfg.sync_cb          = on_sync;
    ble_hs_cfg.reset_cb         = on_reset;
    ble_hs_cfg.store_status_cb  = ble_store_util_status_rr;

    /* Security Manager: KeyboardDisplay IO capability (the host is the
     * keyboard/display via SCPI), MITM protection, LE Secure Connections,
     * bonding, and full key distribution so an LTK is exchanged and stored. */
    ble_hs_cfg.sm_io_cap         = BLE_HS_IO_KEYBOARD_DISPLAY;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 1;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    int rc = gatt_svr_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt_svr_init failed rc=%d", rc);
        return;
    }
    rc = ble_svc_gap_device_name_set(BLE_SPP_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "device_name_set failed rc=%d", rc);
    }

    /* Register the NVS-backed bond store. Without this, pairing that persists a
     * bond fails because store_read/write callbacks are NULL. */
    ble_store_config_init();

    nimble_port_freertos_init(host_task);
}

int ble_spp_adv_start(void) {
    if (!s_ready) {
        ESP_LOGE(TAG, "adv start: stack not synced");
        return -1;
    }
    if (s_conn_state == BLE_SPP_CONNECTED) {
        ESP_LOGW(TAG, "adv start: already connected");
        return -1;
    }
    s_adv_on = 1;
    return advertise() == 0 ? 0 : -1;
}

int ble_spp_adv_stop(void) {
    s_adv_on = 0;
    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv stop failed rc=%d", rc);
        return -1;
    }
    ESP_LOGI(TAG, "advertising stopped");
    return 0;
}

int ble_spp_conn_state(void) {
    return s_conn_state;
}

int ble_spp_last_status(void) {
    return s_last_status;
}

int ble_spp_pair_start(void) {
    if (s_conn_state != BLE_SPP_CONNECTED) {
        ESP_LOGE(TAG, "pair: not connected (state=%d)", s_conn_state);
        return -1;
    }
    s_pair_state = BLE_SPP_PAIR_PROGRESS;
    int rc = ble_gap_security_initiate(s_conn_handle);
    s_last_status = rc;                 /* capture synchronous SM error */
    if (rc != 0) {
        s_pair_state = BLE_SPP_PAIR_FAILED;
        ESP_LOGE(TAG, "pair: security_initiate failed rc=%d", rc);
        return -1;
    }
    ESP_LOGI(TAG, "pair: security initiated");
    return 0;
}

int ble_spp_pair_state(void) {
    return s_pair_state;
}

int ble_spp_pair_passkey(uint32_t passkey) {
    if (s_pair_state != BLE_SPP_PAIR_INPUT_NEEDED) {
        ESP_LOGW(TAG, "passkey: not in input-needed state (state=%d)", s_pair_state);
        return -1;
    }
    struct ble_sm_io io = { .action = BLE_SM_IOACT_INPUT, .passkey = passkey };
    if (ble_sm_inject_io(s_conn_handle, &io) != 0) {
        ESP_LOGE(TAG, "passkey: inject_io failed");
        return -1;
    }
    s_pair_state = BLE_SPP_PAIR_PROGRESS;
    ESP_LOGI(TAG, "passkey: injected %06u", (unsigned)passkey);
    return 0;
}

int ble_spp_pair_passkey_get(uint32_t *out) {
    if (!out || s_pair_state != BLE_SPP_PAIR_DISPLAY_KEY) {
        return -1;
    }
    *out = s_disp_passkey;
    return 0;
}

int ble_spp_pair_numcmp_get(uint32_t *out) {
    if (!out || s_pair_state != BLE_SPP_PAIR_NUMCMP_NEEDED) {
        return -1;
    }
    *out = s_numcmp_val;
    return 0;
}

int ble_spp_pair_confirm(int accept) {
    if (s_pair_state != BLE_SPP_PAIR_NUMCMP_NEEDED) {
        ESP_LOGW(TAG, "confirm: not in numcmp-needed state (state=%d)", s_pair_state);
        return -1;
    }
    struct ble_sm_io io = { .action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = accept ? 1 : 0 };
    if (ble_sm_inject_io(s_conn_handle, &io) != 0) {
        ESP_LOGE(TAG, "confirm: inject_io failed");
        return -1;
    }
    s_pair_state = accept ? BLE_SPP_PAIR_PROGRESS : BLE_SPP_PAIR_FAILED;
    ESP_LOGI(TAG, "numcmp: %s", accept ? "accepted" : "rejected");
    return 0;
}

int ble_spp_sec_info(char *out, size_t out_len) {
    if (s_conn_state != BLE_SPP_CONNECTED) {
        return -1;
    }
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn_handle, &desc) != 0) {
        return -1;
    }

    int enc    = desc.sec_state.encrypted;
    int auth   = desc.sec_state.authenticated;
    int bonded = desc.sec_state.bonded;
    int ks     = desc.sec_state.key_size;

    /* LE security level: 1 none, 2 encrypted/unauthenticated (Just Works),
     * 3 encrypted/authenticated, 4 same with a full 128-bit (LE SC) key.
     * NOTE: ble_gap_sec_state has no explicit "secure connections" flag, so
     * key_size==16 is a practical L4 proxy — a legacy pairing that also
     * negotiated a 16-byte key would be misreported as L4. */
    int level = 1;
    if (enc && !auth)      level = 2;
    else if (enc && auth)  level = (ks >= 16) ? 4 : 3;

    const uint8_t *v = desc.peer_id_addr.val;   /* little-endian: print MSB..LSB */
    snprintf(out, out_len,
             "%02X:%02X:%02X:%02X:%02X:%02X,%d,%d,%d,%d,%d",
             v[5], v[4], v[3], v[2], v[1], v[0],
             level, enc, auth, bonded, ks);
    return 0;
}
