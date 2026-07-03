/**
 * @file ble_conn_handler.c
 * @brief BLE central connect + pair for nRF52840 (SoftDevice s140).
 *
 * See ble_conn_handler.h for the state model and the security-level scope
 * (legacy pairing, levels 1..3). Pairing is driven entirely by SoftDevice GAP
 * events handled in ble_evt_handler(); the SCPI accessors only read/inject
 * shared volatile state.
 */

#include "ble_conn_handler.h"
#include "ble_scan_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble.h"
#include "ble_gap.h"
#include "ble_hci.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "app_error.h"

/* Must match the conn cfg tag used in ble_scan_init() (ble_scan_handler.c). */
#define APP_BLE_CONN_CFG_TAG   1
#define APP_BLE_OBSERVER_PRIO  3

/* ------------------------------------------------------------------ */
/* Shared state (written in BLE event context, read on the USB task)  */
/* ------------------------------------------------------------------ */

static volatile int      s_conn_state  = BLE_CONN_IDLE;
static volatile int      s_pair_state  = BLE_PAIR_IDLE;
static volatile uint16_t s_conn_handle = BLE_CONN_HANDLE_INVALID;
static volatile uint32_t s_disp_passkey;        /* passkey we display (DISPLAY_KEY) */
static volatile int      s_last_status = 0;     /* last GAP/SMP status/reason       */
static volatile int      s_auto_pair   = 0;     /* initiate pairing on connect      */

/* Negotiated security, captured from BLE_GAP_EVT_CONN_SEC_UPDATE. */
static volatile uint8_t  s_sec_level    = 0;    /* 1..4, 0 = none/unknown */
static volatile uint8_t  s_sec_key_size = 0;
static uint8_t           s_peer_addr[6];        /* connected peer, for BLE:SEC?     */

/* BLE:AUTO scan/select phase. */
static volatile int  s_auto_phase = BLE_AUTO_IDLE;
static char          s_auto_filter[BLE_NAME_MAX_LEN + 1];

/* ------------------------------------------------------------------ */
/* SMP configuration                                                  */
/* ------------------------------------------------------------------ */

/* Legacy pairing, authenticated (MITM), no persistent bonding. IO caps are
 * KeyboardDisplay: the host acts as keyboard/display over SCPI. lesc=0 keeps
 * this free of an ECDH backend (see header). */
static ble_gap_sec_params_t const m_sec_params = {
    .bond          = 0,
    .mitm          = 1,
    .lesc          = 0,
    .keypress      = 0,
    .io_caps       = BLE_GAP_IO_CAPS_KEYBOARD_DISPLAY,
    .oob           = 0,
    .min_key_size  = 7,
    .max_key_size  = 16,
    .kdist_own     = { .enc = 1, .id = 1 },
    .kdist_peer    = { .enc = 1, .id = 1 },
};

/* Storage the SoftDevice fills with distributed keys during pairing. Required
 * even without bonding; the keys are discarded when the link drops. */
static ble_gap_enc_key_t   m_own_enc, m_peer_enc;
static ble_gap_id_key_t    m_own_id,  m_peer_id;
static ble_gap_sec_keyset_t m_keyset = {
    .keys_own  = { .p_enc_key = &m_own_enc,  .p_id_key = &m_own_id,  .p_sign_key = NULL, .p_pk = NULL },
    .keys_peer = { .p_enc_key = &m_peer_enc, .p_id_key = &m_peer_id, .p_sign_key = NULL, .p_pk = NULL },
};

/* Connection + scan parameters used by sd_ble_gap_connect(). */
static ble_gap_scan_params_t const m_connect_scan_params = {
    .active        = 1,
    .interval      = 0x00A0,   /* 100 ms */
    .window        = 0x0050,   /* 50 ms  */
    .timeout       = 0x00C8,   /* 2 s connect attempt (units of 10 ms) */
    .filter_policy = BLE_GAP_SCAN_FP_ACCEPT_ALL,
    .scan_phys     = BLE_GAP_PHY_1MBPS,
};

static ble_gap_conn_params_t const m_conn_params = {
    .min_conn_interval = 0x0018,  /* 30 ms  (units of 1.25 ms) */
    .max_conn_interval = 0x0028,  /* 50 ms                     */
    .slave_latency     = 0,
    .conn_sup_timeout  = 0x0190,  /* 4 s (units of 10 ms)      */
};

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Reset all pairing-related state to idle (keeps no active connection). */
static void reset_state(void)
{
    s_conn_state   = BLE_CONN_IDLE;
    s_pair_state   = BLE_PAIR_IDLE;
    s_conn_handle  = BLE_CONN_HANDLE_INVALID;
    s_auto_pair    = 0;
    s_sec_level    = 0;
    s_sec_key_size = 0;
    s_auto_phase   = BLE_AUTO_IDLE;
}

/* Connect to a raw address. Scanning must be stopped first. When auto_pair is
 * set, pairing is initiated as soon as the connection is established. The flag
 * is armed BEFORE the async connect: BLE events are dispatched from an IRQ that
 * can preempt the caller, so the CONNECTED event may run before this returns. */
static int connect_to(const uint8_t addr[6], uint8_t addr_type, bool auto_pair)
{
    ble_gap_addr_t peer = { .addr_type = addr_type };
    memcpy(peer.addr, addr, 6);

    ble_scan_stop();  /* the SoftDevice cannot scan and connect concurrently */

    s_conn_state   = BLE_CONN_CONNECTING;
    s_pair_state   = BLE_PAIR_IDLE;
    s_auto_pair    = auto_pair ? 1 : 0;
    s_sec_level    = 0;   /* clear any security cached from a prior link */
    s_sec_key_size = 0;
    uint32_t err = sd_ble_gap_connect(&peer, &m_connect_scan_params,
                                      &m_conn_params, APP_BLE_CONN_CFG_TAG);
    if (err != NRF_SUCCESS) {
        s_last_status = (int)err;
        s_conn_state  = BLE_CONN_FAILED;
        s_auto_pair   = 0;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* SoftDevice BLE event handler                                       */
/* ------------------------------------------------------------------ */

static void ble_evt_handler(ble_evt_t const *p_ble_evt, void *p_context)
{
    (void)p_context;
    const ble_gap_evt_t *gap = &p_ble_evt->evt.gap_evt;

    switch (p_ble_evt->header.evt_id) {

    case BLE_GAP_EVT_CONNECTED:
        s_conn_handle = gap->conn_handle;
        s_conn_state  = BLE_CONN_CONNECTED;
        s_last_status = 0;
        memcpy(s_peer_addr, gap->params.connected.peer_addr.addr, 6);
        if (s_auto_pair) {
            s_auto_pair  = 0;
            s_pair_state = BLE_PAIR_PROGRESS;
            uint32_t aerr = sd_ble_gap_authenticate(s_conn_handle, &m_sec_params);
            if (aerr != NRF_SUCCESS) {
                s_last_status = (int)aerr;   /* record why pairing could not start */
                s_pair_state  = BLE_PAIR_FAILED;
                /* Free the single central slot so the next attempt can connect. */
                (void)sd_ble_gap_disconnect(s_conn_handle,
                          BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            }
        }
        break;

    case BLE_GAP_EVT_DISCONNECTED:
        /* Preserve a root-cause code already recorded (e.g. the SMP auth status
         * that triggered our own disconnect); only fall back to the disconnect
         * reason when nothing more specific was captured. */
        if (s_last_status == 0) {
            s_last_status = gap->params.disconnected.reason;
        }
        s_conn_handle = BLE_CONN_HANDLE_INVALID;
        s_auto_pair   = 0;
        /* If pairing already completed, keep the DONE outcome (BLE:SEC? reads the
         * cached security info, not the live handle) so a workflow can still
         * report success. Otherwise surface a clean FAILED terminal state rather
         * than dropping to IDLE — a polling workflow cannot tell IDLE apart from
         * "not started" and would hang until it times out. */
        if (s_pair_state != BLE_PAIR_DONE) {
            s_conn_state = BLE_CONN_FAILED;
            s_pair_state = BLE_PAIR_FAILED;
        }
        break;

    case BLE_GAP_EVT_TIMEOUT:
        if (gap->params.timeout.src == BLE_GAP_TIMEOUT_SRC_CONN) {
            /* Connection establishment timed out (peer not connectable/in range).
             * Record an HCI "connection failed to be established" so BLE:CONNect:STATus?
             * distinguishes this from a pairing rejection. */
            s_last_status = 0x3E;  /* BLE_HCI_CONN_FAILED_TO_BE_ESTABLISHED */
            s_conn_state  = BLE_CONN_FAILED;
            s_pair_state  = BLE_PAIR_FAILED;
            s_auto_pair   = 0;
        }
        break;

    case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
        /* As central, our security parameters were already supplied in
         * sd_ble_gap_authenticate(); reply with NULL params + the keyset. */
        sd_ble_gap_sec_params_reply(s_conn_handle, BLE_GAP_SEC_STATUS_SUCCESS,
                                    NULL, &m_keyset);
        break;

    case BLE_GAP_EVT_AUTH_KEY_REQUEST:
        /* The peer displays a passkey; the host must enter it via SCPI. */
        if (gap->params.auth_key_request.key_type == BLE_GAP_AUTH_KEY_TYPE_PASSKEY) {
            s_pair_state = BLE_PAIR_INPUT_NEEDED;
        }
        break;

    case BLE_GAP_EVT_PASSKEY_DISPLAY: {
        /* We display a passkey (6 ASCII digits); the user types it on the peer.
         * match_request is set only for LESC numeric comparison, which this
         * legacy configuration never negotiates. */
        char digits[7] = {0};
        memcpy(digits, gap->params.passkey_display.passkey, 6);
        s_disp_passkey = (uint32_t)strtoul(digits, NULL, 10);
        s_pair_state   = BLE_PAIR_DISPLAY_KEY;
        break;
    }

    case BLE_GAP_EVT_CONN_SEC_UPDATE:
        s_sec_level    = gap->params.conn_sec_update.conn_sec.sec_mode.lv;
        s_sec_key_size = gap->params.conn_sec_update.conn_sec.encr_key_size;
        break;

    case BLE_GAP_EVT_AUTH_STATUS:
        s_last_status = gap->params.auth_status.auth_status;
        if (gap->params.auth_status.auth_status == BLE_GAP_SEC_STATUS_SUCCESS) {
            s_pair_state = BLE_PAIR_DONE;
        } else {
            s_pair_state = BLE_PAIR_FAILED;
            /* Drop the link so the single central slot is freed; otherwise the
             * next connect fails immediately with the slot still occupied. */
            (void)sd_ble_gap_disconnect(s_conn_handle,
                      BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        }
        break;

    default:
        break;
    }
}

NRF_SDH_BLE_OBSERVER(m_conn_obs, APP_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);

/* ------------------------------------------------------------------ */
/* Public API                                                        */
/* ------------------------------------------------------------------ */

void ble_conn_init(void)
{
    reset_state();
}

int ble_conn_start(size_t scan_index)
{
    ble_scan_result_t r;
    if (!ble_scan_get_result((uint16_t)scan_index, &r)) {
        return -1;
    }
    return connect_to(r.addr, r.addr_type, false);  /* plain connect never auto-pairs */
}

int ble_conn_state(void)
{
    return s_conn_state;
}

int ble_connpair_start(size_t scan_index)
{
    ble_scan_result_t r;
    if (!ble_scan_get_result((uint16_t)scan_index, &r)) {
        return -1;
    }
    return connect_to(r.addr, r.addr_type, true);
}

int ble_connpair_state(void)
{
    switch (s_conn_state) {
    case BLE_CONN_IDLE:       return BLE_CP_IDLE;
    case BLE_CONN_CONNECTING: return BLE_CP_CONNECTING;
    case BLE_CONN_FAILED:     return BLE_CP_FAILED;
    default:                  break;  /* BLE_CONN_CONNECTED */
    }
    switch (s_pair_state) {
    case BLE_PAIR_IDLE:
    case BLE_PAIR_PROGRESS:     return BLE_CP_PAIRING;
    case BLE_PAIR_INPUT_NEEDED: return BLE_CP_PASSKEY;
    case BLE_PAIR_DISPLAY_KEY:  return BLE_CP_DISPLAY;
    case BLE_PAIR_DONE:         return BLE_CP_DONE;
    default:                    return BLE_CP_FAILED;  /* BLE_PAIR_FAILED */
    }
}

int ble_auto_start(const char *filter)
{
    strncpy(s_auto_filter, filter ? filter : "", sizeof(s_auto_filter) - 1);
    s_auto_filter[sizeof(s_auto_filter) - 1] = '\0';

    reset_state();
    ble_scan_clear();
    ble_scan_start_timed(10);   /* bounded so a "nothing found" run can end */
    s_auto_phase = BLE_AUTO_SCANNING;
    return 0;
}

int ble_auto_state(void)
{
    /* Explicit phases short-circuit; only the connect/pair phases are derived. */
    if (s_auto_phase == BLE_AUTO_IDLE)     return BLE_AUTO_IDLE;
    if (s_auto_phase == BLE_AUTO_SCANNING) return BLE_AUTO_SCANNING;
    if (s_auto_phase == BLE_AUTO_FAILED)   return BLE_AUTO_FAILED;

    /* Past selection: mirror the combined connect+pair phase. */
    switch (ble_connpair_state()) {
    case BLE_CP_IDLE:
    case BLE_CP_CONNECTING: return BLE_AUTO_CONNECTING;
    case BLE_CP_PAIRING:    return BLE_AUTO_PAIRING;
    case BLE_CP_PASSKEY:    return BLE_AUTO_PASSKEY;
    case BLE_CP_DISPLAY:    return BLE_AUTO_DISPLAY;
    case BLE_CP_DONE:       return BLE_AUTO_DONE;
    default:                return BLE_AUTO_FAILED;
    }
}

void ble_conn_task(void)
{
    if (s_auto_phase != BLE_AUTO_SCANNING) {
        return;
    }
    /* Let the full (bounded) scan window run first: picking the first advertiser
     * seen often lands on a weak or transient one that fails to connect. */
    if (ble_scan_is_scanning()) {
        return;
    }

    /* Scan finished: choose the strongest connectable (adv_type == 0) device
     * matching the optional name filter — strong, persistent advertisers connect
     * far more reliably than the first one that happened to appear. */
    int      best = -1;
    int8_t   best_rssi = -128;
    uint16_t n = ble_scan_count();
    for (uint16_t i = 0; i < n; i++) {
        ble_scan_result_t r;
        if (!ble_scan_get_result(i, &r)) {
            continue;
        }
        if (r.adv_type != 0) {
            continue;  /* not connectable -> cannot pair */
        }
        if (s_auto_filter[0] && !strstr(r.name, s_auto_filter)) {
            continue;  /* name filter set and does not match */
        }
        if (best < 0 || r.rssi > best_rssi) {
            best = i;
            best_rssi = r.rssi;
        }
    }

    if (best < 0) {
        /* Nothing pairable in range: end as FAILED so the workflow reports a
         * result instead of polling until its own timeout. */
        s_auto_phase = BLE_AUTO_FAILED;
        return;
    }

    ble_scan_result_t chosen;
    ble_scan_get_result((uint16_t)best, &chosen);
    s_auto_phase = BLE_AUTO_CONNECTING;
    connect_to(chosen.addr, chosen.addr_type, true);
}

int ble_conn_last_status(void)
{
    return s_last_status;
}

int ble_conn_disconnect(void)
{
    if (s_conn_state != BLE_CONN_CONNECTED) {
        return -1;
    }
    return sd_ble_gap_disconnect(s_conn_handle,
                                 BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION)
               == NRF_SUCCESS ? 0 : -1;
}

int ble_pair_start(void)
{
    if (s_conn_state != BLE_CONN_CONNECTED) {
        return -1;
    }
    s_pair_state = BLE_PAIR_PROGRESS;
    uint32_t err = sd_ble_gap_authenticate(s_conn_handle, &m_sec_params);
    s_last_status = (int)err;
    if (err != NRF_SUCCESS) {
        s_pair_state = BLE_PAIR_FAILED;
        return -1;
    }
    return 0;
}

int ble_pair_state(void)
{
    return s_pair_state;
}

int ble_pair_passkey(uint32_t passkey)
{
    if (s_pair_state != BLE_PAIR_INPUT_NEEDED) {
        return -1;
    }
    /* sd_ble_gap_auth_key_reply expects 6 ASCII digits, MSD first. */
    uint8_t key[6];
    char digits[7];
    snprintf(digits, sizeof(digits), "%06lu", (unsigned long)(passkey % 1000000u));
    memcpy(key, digits, 6);

    if (sd_ble_gap_auth_key_reply(s_conn_handle, BLE_GAP_AUTH_KEY_TYPE_PASSKEY, key)
            != NRF_SUCCESS) {
        return -1;
    }
    s_pair_state = BLE_PAIR_PROGRESS;
    return 0;
}

int ble_pair_passkey_get(uint32_t *out)
{
    if (!out || s_pair_state != BLE_PAIR_DISPLAY_KEY) {
        return -1;
    }
    *out = s_disp_passkey;
    return 0;
}

int ble_sec_info(char *out, size_t out_len)
{
    if (s_conn_state != BLE_CONN_CONNECTED) {
        return -1;
    }

    int level  = s_sec_level;                    /* SoftDevice LE level 1..4 */
    int ks     = s_sec_key_size;
    int enc    = (level >= 2) ? 1 : 0;
    int auth   = (level >= 3) ? 1 : 0;
    int bonded = m_sec_params.bond ? 1 : 0;

    const uint8_t *v = s_peer_addr;  /* little-endian: print MSB..LSB */
    snprintf(out, out_len,
             "%02X:%02X:%02X:%02X:%02X:%02X,%d,%d,%d,%d,%d",
             v[5], v[4], v[3], v[2], v[1], v[0],
             level, enc, auth, bonded, ks);
    return 0;
}
