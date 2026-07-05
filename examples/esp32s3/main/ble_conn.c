#include "ble_conn.h"
#include "ble_scan.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"

static const char *TAG = "ble.conn";

/* Defined in NimBLE's store/config module; not declared in a public header. */
void ble_store_config_init(void);

/* All NimBLE callbacks run on the host task; SCPI accessors run on the USB
 * task. State is shared through volatiles + simple flags, matching the
 * lock-free style already used by ble_scan.c. A single connection is tracked. */
static volatile int      s_conn_state = BLE_CONN_IDLE;
static volatile int      s_pair_state = BLE_PAIR_IDLE;
static volatile uint16_t s_conn_handle;
static volatile uint32_t s_disp_passkey;   /* passkey we generated (DISPLAY)  */
static volatile uint32_t s_numcmp_val;     /* number to compare (NUMCMP)      */
static volatile int      s_last_status = 0;/* last GAP connect/disc/enc status */
static volatile int      s_auto_pair = 0;  /* start pairing on connect (CPAIR) */

void ble_conn_init(void) {
    ble_hs_cfg.sm_io_cap   = BLE_HS_IO_KEYBOARD_DISPLAY;  /* PC is the keyboard/display via SCPI */
    ble_hs_cfg.sm_bonding  = 1;
    ble_hs_cfg.sm_mitm     = 1;                           /* require authentication -> passkey/numcmp */
    ble_hs_cfg.sm_sc       = 1;                           /* LE Secure Connections */
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* Register the NVS-backed bond store. Without this, store_read_cb is NULL
     * and pairing fails with BLE_HS_ENOTSUP before any SMP packet is sent. */
    ble_store_config_init();
}

static int gap_conn_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_last_status = event->connect.status;
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_conn_state  = BLE_CONN_CONNECTED;
            ESP_LOGI(TAG, "connect ok handle=%u", (unsigned)s_conn_handle);
            /* One-step connect+pair: kick off pairing as soon as we connect. */
            if (s_auto_pair) {
                s_auto_pair  = 0;
                s_pair_state = BLE_PAIR_PROGRESS;
                if (ble_gap_security_initiate(s_conn_handle) != 0) {
                    ESP_LOGE(TAG, "auto-pair: security_initiate failed");
                    s_pair_state = BLE_PAIR_FAILED;
                }
            }
        } else {
            s_conn_state = BLE_CONN_FAILED;
            s_auto_pair  = 0;
            ESP_LOGE(TAG, "connect failed status=%d", event->connect.status);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_last_status = event->disconnect.reason;
        s_conn_state = BLE_CONN_IDLE;
        s_pair_state = BLE_PAIR_IDLE;
        s_auto_pair  = 0;
        ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        s_last_status = event->enc_change.status;
        s_pair_state = (event->enc_change.status == 0) ? BLE_PAIR_DONE : BLE_PAIR_FAILED;
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
            s_pair_state = BLE_PAIR_INPUT_NEEDED;
            ESP_LOGI(TAG, "passkey action: input needed (enter peer passkey)");
            break;
        case BLE_SM_IOACT_DISP:
            /* We display a passkey; peer types it in. Generate + show it. */
            s_disp_passkey = esp_random() % 1000000u;
            io.action  = BLE_SM_IOACT_DISP;
            io.passkey = s_disp_passkey;
            ble_sm_inject_io(event->passkey.conn_handle, &io);
            s_pair_state = BLE_PAIR_DISPLAY_KEY;
            ESP_LOGI(TAG, "passkey action: display key %06u", (unsigned)s_disp_passkey);
            break;
        case BLE_SM_IOACT_NUMCMP:
            /* Both sides display the same number; host confirms the match. */
            s_numcmp_val = event->passkey.params.numcmp;
            s_pair_state = BLE_PAIR_NUMCMP_NEEDED;
            ESP_LOGI(TAG, "passkey action: numcmp %u", (unsigned)s_numcmp_val);
            break;
        default:
            /* OOB and any other action are unsupported in this demo. */
            s_pair_state = BLE_PAIR_FAILED;
            ESP_LOGW(TAG, "passkey action: unsupported action %d", event->passkey.params.action);
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

    default:
        return 0;
    }
}

int ble_conn_start(size_t scan_index) {
    uint8_t val[6], type;
    if (ble_scan_addr(scan_index, val, &type) != 0) {
        ESP_LOGE(TAG, "connect: scan index %u out of range", (unsigned)scan_index);
        return -1;
    }
    ble_addr_t addr = { .type = type };
    memcpy(addr.val, val, 6);

    uint8_t own_type;
    if (ble_hs_id_infer_auto(0, &own_type) != 0) {
        ESP_LOGE(TAG, "connect: id_infer_auto failed");
        return -1;
    }

    ble_gap_disc_cancel();   /* best-effort: a scan must stop before connecting */

    s_conn_state = BLE_CONN_CONNECTING;
    s_pair_state = BLE_PAIR_IDLE;
    s_auto_pair  = 0;   /* plain connect never auto-pairs; ble_connpair_start re-arms */
    ESP_LOGI(TAG, "connecting to %02X:%02X:%02X:%02X:%02X:%02X",
             val[5], val[4], val[3], val[2], val[1], val[0]);
    if (ble_gap_connect(own_type, &addr, 10000, NULL, gap_conn_cb, NULL) != 0) {
        s_conn_state = BLE_CONN_FAILED;
        ESP_LOGE(TAG, "ble_gap_connect failed");
        return -1;
    }
    return 0;
}

int ble_conn_state(void) {
    return s_conn_state;
}

int ble_connpair_start(size_t scan_index) {
    /* ble_conn_start() clears s_auto_pair, so arm it after a successful start.
     * The async CONNECT event arrives later on the host task, by which point
     * the flag is set, and the handler initiates pairing on the fresh link. */
    ESP_LOGI(TAG, "connect+pair: scan index %u", (unsigned)scan_index);
    int rc = ble_conn_start(scan_index);
    if (rc == 0) {
        s_auto_pair = 1;
        ESP_LOGI(TAG, "connect+pair: auto-pair armed on connect");
    } else {
        ESP_LOGE(TAG, "connect+pair: ble_conn_start failed");
    }
    return rc;
}

int ble_connpair_state(void) {
    /* Connection phase first; once connected, report the pairing phase. */
    switch (s_conn_state) {
    case BLE_CONN_IDLE:       return BLE_CP_IDLE;
    case BLE_CONN_CONNECTING: return BLE_CP_CONNECTING;
    case BLE_CONN_FAILED:     return BLE_CP_FAILED;
    default:                  break;  /* BLE_CONN_CONNECTED */
    }
    switch (s_pair_state) {
    case BLE_PAIR_IDLE:
    case BLE_PAIR_PROGRESS:      return BLE_CP_PAIRING;
    case BLE_PAIR_INPUT_NEEDED:  return BLE_CP_PASSKEY;
    case BLE_PAIR_NUMCMP_NEEDED: return BLE_CP_NUMCMP;
    case BLE_PAIR_DISPLAY_KEY:   return BLE_CP_DISPLAY;
    case BLE_PAIR_DONE:          return BLE_CP_DONE;
    default:                     return BLE_CP_FAILED;  /* BLE_PAIR_FAILED */
    }
}

int ble_conn_last_status(void) {
    return s_last_status;
}

int ble_conn_disconnect(void) {
    if (s_conn_state != BLE_CONN_CONNECTED) {
        ESP_LOGW(TAG, "disconnect: not connected (state=%d)", s_conn_state);
        return -1;
    }
    int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0) {
        ESP_LOGE(TAG, "disconnect: ble_gap_terminate failed rc=%d", rc);
        return -1;
    }
    ESP_LOGI(TAG, "disconnecting (host-initiated)");
    return 0;
}

int ble_pair_start(void) {
    if (s_conn_state != BLE_CONN_CONNECTED) {
        ESP_LOGE(TAG, "pair: not connected (state=%d)", s_conn_state);
        return -1;
    }
    s_pair_state = BLE_PAIR_PROGRESS;
    int rc = ble_gap_security_initiate(s_conn_handle);
    s_last_status = rc;                 /* capture synchronous SM error for diagnostics */
    if (rc != 0) {
        s_pair_state = BLE_PAIR_FAILED;
        ESP_LOGE(TAG, "pair: security_initiate failed rc=%d", rc);
        return -1;
    }
    ESP_LOGI(TAG, "pair: security initiated");
    return 0;
}

int ble_pair_state(void) {
    return s_pair_state;
}

int ble_pair_passkey(uint32_t passkey) {
    if (s_pair_state != BLE_PAIR_INPUT_NEEDED) {
        ESP_LOGW(TAG, "passkey: not in input-needed state (state=%d)", s_pair_state);
        return -1;
    }
    struct ble_sm_io io = { .action = BLE_SM_IOACT_INPUT, .passkey = passkey };
    if (ble_sm_inject_io(s_conn_handle, &io) != 0) {
        ESP_LOGE(TAG, "passkey: inject_io failed");
        return -1;
    }
    s_pair_state = BLE_PAIR_PROGRESS;
    ESP_LOGI(TAG, "passkey: injected %06u", (unsigned)passkey);
    return 0;
}

int ble_pair_passkey_get(uint32_t *out) {
    if (!out || s_pair_state != BLE_PAIR_DISPLAY_KEY) {
        return -1;
    }
    *out = s_disp_passkey;
    return 0;
}

int ble_pair_numcmp_get(uint32_t *out) {
    if (!out || s_pair_state != BLE_PAIR_NUMCMP_NEEDED) {
        return -1;
    }
    *out = s_numcmp_val;
    return 0;
}

int ble_pair_confirm(int accept) {
    if (s_pair_state != BLE_PAIR_NUMCMP_NEEDED) {
        ESP_LOGW(TAG, "confirm: not in numcmp-needed state (state=%d)", s_pair_state);
        return -1;
    }
    struct ble_sm_io io = { .action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = accept ? 1 : 0 };
    if (ble_sm_inject_io(s_conn_handle, &io) != 0) {
        ESP_LOGE(TAG, "confirm: inject_io failed");
        return -1;
    }
    s_pair_state = accept ? BLE_PAIR_PROGRESS : BLE_PAIR_FAILED;
    ESP_LOGI(TAG, "numcmp: %s", accept ? "accepted" : "rejected");
    return 0;
}

int ble_sec_info(char *out, size_t out_len) {
    if (s_conn_state != BLE_CONN_CONNECTED) {
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
     * key_size==16 is used as an L4 proxy. This misreports a legacy pairing
     * that also negotiated a 16-byte key as L4. To distinguish strictly,
     * record whether SC was used in the BLE_GAP_EVENT_ENC_CHANGE handler
     * (ble_gap_conn_find -> desc, or the SMP result) and key off that instead. */
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
