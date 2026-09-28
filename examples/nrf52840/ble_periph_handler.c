/**
 * @file ble_periph_handler.c
 * @brief BLE peripheral role for nRF52840 (SoftDevice s140).
 *
 * See ble_periph_handler.h for the state model. Everything is driven by
 * SoftDevice events in ble_evt_handler(); the SCPI accessors only read or
 * inject shared volatile state.
 */

#include "ble_periph_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble.h"
#include "ble_gap.h"
#include "ble_gatts.h"
#include "ble_hci.h"
#include "nrf_sdh_ble.h"

/* Must match the conn cfg tag used in ble_scan_init() (ble_scan_handler.c). */
#define APP_BLE_CONN_CFG_TAG   1
#define APP_BLE_OBSERVER_PRIO  3

#define DEVICE_NAME            "IoTSploit-nRF"
#define ADV_INTERVAL           160   /* 100 ms (units of 0.625 ms) */

/* ------------------------------------------------------------------ */
/* Shared state (written in BLE event context, read on the USB task)  */
/* ------------------------------------------------------------------ */

static volatile int      s_state       = BLE_PERIPH_IDLE;
static volatile uint16_t s_conn_handle = BLE_CONN_HANDLE_INVALID;
static volatile int      s_last_status = 0;
static volatile uint32_t s_disp_passkey;
/* A start while a link was up waits for that link's DISCONNECTED. */
static volatile uint16_t s_restart_after = BLE_CONN_HANDLE_INVALID;

/* What the link negotiated. s_have_peer gates BLE:PERiph:SEC?. */
static volatile int  s_have_peer;
static uint8_t       s_peer_addr[6];
static const char   *s_method = "none";
static volatile uint8_t s_sec_mode, s_sec_level, s_key_size;
static volatile uint8_t s_bonded, s_lesc, s_saw_passkey;
static ble_gap_sec_params_t s_peer_params;   /* central's request, from SEC_PARAMS_REQUEST */
static volatile int  s_have_peer_params;

/* Legacy pairing with MITM and bonding; io_caps is set per start. The central
 * decides what it distributes; we offer our LTK and identity. */
static ble_gap_sec_params_t m_sec_params = {
    .bond          = 1,
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

/* Keys from the last pairing, in RAM. s_have_keys lets a bonded central
 * re-encrypt with our LTK after a reconnect (SEC_INFO_REQUEST). */
static ble_gap_enc_key_t    m_own_enc, m_peer_enc;
static ble_gap_id_key_t     m_own_id,  m_peer_id;
static ble_gap_sec_keyset_t m_keyset = {
    .keys_own  = { .p_enc_key = &m_own_enc,  .p_id_key = &m_own_id,  .p_sign_key = NULL, .p_pk = NULL },
    .keys_peer = { .p_enc_key = &m_peer_enc, .p_id_key = &m_peer_id, .p_sign_key = NULL, .p_pk = NULL },
};
static volatile int s_have_keys;

/* Advertising set. The SoftDevice reads these buffers while advertising, so
 * they are static and only rewritten while stopped. */
static uint8_t            s_adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
static uint8_t            s_adv_buf[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static ble_gap_adv_data_t s_adv_data;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static const char *io_name(uint8_t io)
{
    switch (io) {
    case BLE_GAP_IO_CAPS_DISPLAY_ONLY:     return "display_only";
    case BLE_GAP_IO_CAPS_DISPLAY_YESNO:    return "display_yesno";
    case BLE_GAP_IO_CAPS_KEYBOARD_ONLY:    return "keyboard_only";
    case BLE_GAP_IO_CAPS_NONE:             return "no_io";
    case BLE_GAP_IO_CAPS_KEYBOARD_DISPLAY: return "keyboard_display";
    default:                               return "unknown";
    }
}

static int has_keyboard(uint8_t io)
{
    return io == BLE_GAP_IO_CAPS_KEYBOARD_ONLY || io == BLE_GAP_IO_CAPS_KEYBOARD_DISPLAY;
}

static int has_yesno(uint8_t io)
{
    return io == BLE_GAP_IO_CAPS_DISPLAY_YESNO || io == BLE_GAP_IO_CAPS_KEYBOARD_DISPLAY;
}

/* The SMP association model (Core spec Vol 3 Part H, 2.3.5.1) from both
 * sides' OOB, MITM, and IO capabilities, used when no passkey event already
 * told us the answer. */
static const char *derive_method(uint8_t lesc)
{
    if (!s_have_peer_params) {
        return "unknown";
    }
    const ble_gap_sec_params_t *p = &s_peer_params;
    if (p->oob && m_sec_params.oob) {
        return "oob";
    }
    if (!p->mitm && !m_sec_params.mitm) {
        return "just_works";
    }
    uint8_t a = p->io_caps, b = m_sec_params.io_caps;
    if (a == BLE_GAP_IO_CAPS_NONE || b == BLE_GAP_IO_CAPS_NONE) {
        return "just_works";
    }
    if (lesc && has_yesno(a) && has_yesno(b)) {
        return "numeric_comparison";
    }
    if (has_keyboard(a) || has_keyboard(b)) {
        return "passkey";
    }
    return "just_works";
}

static void clear_link(void)
{
    s_have_peer        = 0;
    s_have_peer_params = 0;
    s_method           = "none";
    s_sec_mode = s_sec_level = s_key_size = 0;
    s_bonded = s_lesc = s_saw_passkey = 0;
}

static int adv_start(void)
{
    uint32_t err = sd_ble_gap_adv_start(s_adv_handle, APP_BLE_CONN_CFG_TAG);
    if (err != NRF_SUCCESS) {
        s_last_status = (int)err;
        s_state = BLE_PERIPH_FAILED;
        return -1;
    }
    s_state = BLE_PERIPH_ADVERTISING;
    return 0;
}

/* ------------------------------------------------------------------ */
/* SoftDevice BLE event handler                                       */
/* ------------------------------------------------------------------ */

static void on_connected(const ble_gap_evt_t *gap)
{
    if (gap->params.connected.role != BLE_GAP_ROLE_PERIPH) {
        return;  /* a central link: ble_conn_handler.c owns it */
    }
    s_conn_handle = gap->conn_handle;
    s_last_status = 0;
    clear_link();
    memcpy(s_peer_addr, gap->params.connected.peer_addr.addr, 6);
    s_have_peer = 1;
    s_state = BLE_PERIPH_CONNECTED;
    /* Ask the central to pair (SMP Security Request). A central that is already
     * pairing makes this return BUSY, which is harmless. */
    (void)sd_ble_gap_authenticate(s_conn_handle, &m_sec_params);
}

static void on_disconnected(const ble_gap_evt_t *gap)
{
    uint16_t handle = gap->conn_handle;
    if (handle == s_restart_after) {
        s_restart_after = BLE_CONN_HANDLE_INVALID;
        (void)adv_start();
        return;
    }
    if (handle != s_conn_handle) {
        return;
    }
    s_conn_handle = BLE_CONN_HANDLE_INVALID;
    if (s_last_status == 0) {
        s_last_status = gap->params.disconnected.reason;
    }
    /* Keep DONE so the result can still be read; anything short of it failed. */
    if (s_state != BLE_PERIPH_DONE) {
        s_state = BLE_PERIPH_FAILED;
    }
}

static void ble_evt_handler(ble_evt_t const *p_ble_evt, void *p_context)
{
    (void)p_context;
    const ble_gap_evt_t *gap = &p_ble_evt->evt.gap_evt;
    uint16_t id = p_ble_evt->header.evt_id;

    if (id == BLE_GAP_EVT_CONNECTED) {
        on_connected(gap);
        return;
    }
    if (id == BLE_GAP_EVT_DISCONNECTED) {
        on_disconnected(gap);
        return;
    }
    /* Every remaining GAP and GATTS event starts with its conn_handle. */
    if (s_conn_handle == BLE_CONN_HANDLE_INVALID || gap->conn_handle != s_conn_handle) {
        return;
    }

    switch (id) {

    case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
        s_peer_params      = gap->params.sec_params_request.peer_params;
        s_have_peer_params = 1;
        s_state            = BLE_PERIPH_PAIRING;
        s_have_keys        = 0;  /* the keyset is about to be overwritten */
        {
            uint32_t err = sd_ble_gap_sec_params_reply(s_conn_handle,
                               BLE_GAP_SEC_STATUS_SUCCESS, &m_sec_params, &m_keyset);
            if (err != NRF_SUCCESS) {
                s_last_status = (int)err;
                s_state = BLE_PERIPH_FAILED;
            }
        }
        break;

    case BLE_GAP_EVT_SEC_INFO_REQUEST: {
        /* A bonded central re-encrypting. Reply with our LTK only if it is the
         * one we handed out; otherwise the central must pair again. */
        const ble_gap_master_id_t *mid = &gap->params.sec_info_request.master_id;
        int match = s_have_keys && mid->ediv == m_own_enc.master_id.ediv &&
                    memcmp(mid->rand, m_own_enc.master_id.rand, BLE_GAP_SEC_RAND_LEN) == 0;
        (void)sd_ble_gap_sec_info_reply(s_conn_handle,
                                        match ? &m_own_enc.enc_info : NULL, NULL, NULL);
        break;
    }

    case BLE_GAP_EVT_PASSKEY_DISPLAY: {
        char digits[7] = {0};
        memcpy(digits, gap->params.passkey_display.passkey, 6);
        s_disp_passkey = (uint32_t)strtoul(digits, NULL, 10);
        s_saw_passkey  = 1;
        s_state        = BLE_PERIPH_DISPLAY;
        break;
    }

    case BLE_GAP_EVT_AUTH_KEY_REQUEST:
        if (gap->params.auth_key_request.key_type == BLE_GAP_AUTH_KEY_TYPE_PASSKEY) {
            s_saw_passkey = 1;
            s_state       = BLE_PERIPH_PASSKEY;
        }
        break;

    case BLE_GAP_EVT_CONN_SEC_UPDATE:
        s_sec_mode  = gap->params.conn_sec_update.conn_sec.sec_mode.sm;
        s_sec_level = gap->params.conn_sec_update.conn_sec.sec_mode.lv;
        s_key_size  = gap->params.conn_sec_update.conn_sec.encr_key_size;
        /* Encrypted without a pairing on this link: a bonded central
         * re-encrypted with the LTK from an earlier pairing. */
        if (s_state == BLE_PERIPH_CONNECTED && s_sec_level >= 2) {
            s_method = "bonded_reencrypt";
            s_bonded = 1;
            s_state  = BLE_PERIPH_DONE;
        }
        break;

    case BLE_GAP_EVT_AUTH_STATUS: {
        const ble_gap_evt_auth_status_t *st = &gap->params.auth_status;
        s_last_status = st->auth_status;
        if (st->auth_status == BLE_GAP_SEC_STATUS_SUCCESS) {
            s_bonded    = st->bonded;
            s_lesc      = st->lesc;
            s_have_keys = st->bonded;
            s_method    = s_saw_passkey ? "passkey" : derive_method(st->lesc);
            s_state     = BLE_PERIPH_DONE;
        } else {
            s_state = BLE_PERIPH_FAILED;
        }
        break;
    }

    /* Requests a central may send that the SoftDevice leaves to the app;
     * unanswered, they stall the link until it times out. */
    case BLE_GATTS_EVT_SYS_ATTR_MISSING:
        (void)sd_ble_gatts_sys_attr_set(s_conn_handle, NULL, 0, 0);
        break;

    case BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST:
        (void)sd_ble_gatts_exchange_mtu_reply(s_conn_handle, BLE_GATT_ATT_MTU_DEFAULT);
        break;

    case BLE_GAP_EVT_PHY_UPDATE_REQUEST: {
        ble_gap_phys_t const phys = { .tx_phys = BLE_GAP_PHY_AUTO, .rx_phys = BLE_GAP_PHY_AUTO };
        (void)sd_ble_gap_phy_update(s_conn_handle, &phys);
        break;
    }

    case BLE_GAP_EVT_DATA_LENGTH_UPDATE_REQUEST:
        (void)sd_ble_gap_data_length_update(s_conn_handle, NULL, NULL);
        break;

    case BLE_GATTS_EVT_TIMEOUT:
        (void)sd_ble_gap_disconnect(s_conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        break;

    default:
        break;
    }
}

NRF_SDH_BLE_OBSERVER(m_periph_obs, APP_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void ble_periph_init(void)
{
    ble_gap_conn_sec_mode_t name_perm;
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&name_perm);
    (void)sd_ble_gap_device_name_set(&name_perm, (const uint8_t *)DEVICE_NAME,
                                     (uint16_t)strlen(DEVICE_NAME));

    /* Flags (LE General Discoverable, no BR/EDR) + Complete Local Name. */
    size_t name_len = strlen(DEVICE_NAME);
    size_t n = 0;
    s_adv_buf[n++] = 2;
    s_adv_buf[n++] = BLE_GAP_AD_TYPE_FLAGS;
    s_adv_buf[n++] = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
    s_adv_buf[n++] = (uint8_t)(name_len + 1);
    s_adv_buf[n++] = BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME;
    memcpy(&s_adv_buf[n], DEVICE_NAME, name_len);
    n += name_len;

    s_adv_data.adv_data.p_data      = s_adv_buf;
    s_adv_data.adv_data.len         = (uint16_t)n;
    s_adv_data.scan_rsp_data.p_data = NULL;
    s_adv_data.scan_rsp_data.len    = 0;

    ble_gap_adv_params_t params;
    memset(&params, 0, sizeof(params));
    params.properties.type = BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED;
    params.filter_policy   = BLE_GAP_ADV_FP_ANY;
    params.interval        = ADV_INTERVAL;
    params.duration        = 0;  /* until connected or stopped */
    params.primary_phy     = BLE_GAP_PHY_1MBPS;
    (void)sd_ble_gap_adv_set_configure(&s_adv_handle, &s_adv_data, &params);
}

int ble_periph_start(uint8_t io_caps)
{
    if (io_caps > BLE_GAP_IO_CAPS_KEYBOARD_DISPLAY || s_adv_handle == BLE_GAP_ADV_SET_HANDLE_NOT_SET) {
        return -1;
    }
    (void)sd_ble_gap_adv_stop(s_adv_handle);
    m_sec_params.io_caps = io_caps;
    s_last_status = 0;
    clear_link();

    uint16_t old = s_conn_handle;
    s_conn_handle = BLE_CONN_HANDLE_INVALID;
    if (old != BLE_CONN_HANDLE_INVALID &&
        sd_ble_gap_disconnect(old, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION) == NRF_SUCCESS) {
        /* The peripheral slot is still taken; advertise once it frees. */
        s_restart_after = old;
        s_state = BLE_PERIPH_ADVERTISING;
        return 0;
    }
    return adv_start();
}

int ble_periph_stop(void)
{
    (void)sd_ble_gap_adv_stop(s_adv_handle);
    s_restart_after = BLE_CONN_HANDLE_INVALID;
    uint16_t old = s_conn_handle;
    s_conn_handle = BLE_CONN_HANDLE_INVALID;
    if (old != BLE_CONN_HANDLE_INVALID) {
        (void)sd_ble_gap_disconnect(old, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
    }
    s_state = BLE_PERIPH_IDLE;
    return 0;
}

int ble_periph_state(void)
{
    return s_state;
}

int ble_periph_last_status(void)
{
    return s_last_status;
}

int ble_periph_passkey(uint32_t passkey)
{
    if (s_state != BLE_PERIPH_PASSKEY) {
        return -1;
    }
    /* sd_ble_gap_auth_key_reply expects 6 ASCII digits, MSD first. */
    char digits[7];
    snprintf(digits, sizeof(digits), "%06lu", (unsigned long)(passkey % 1000000u));
    if (sd_ble_gap_auth_key_reply(s_conn_handle, BLE_GAP_AUTH_KEY_TYPE_PASSKEY,
                                  (const uint8_t *)digits) != NRF_SUCCESS) {
        return -1;
    }
    s_state = BLE_PERIPH_PAIRING;
    return 0;
}

int ble_periph_passkey_get(uint32_t *out)
{
    if (!out || s_state != BLE_PERIPH_DISPLAY) {
        return -1;
    }
    *out = s_disp_passkey;
    return 0;
}

int ble_periph_sec_info(char *out, size_t out_len)
{
    if (!s_have_peer) {
        return -1;
    }
    const uint8_t *v = s_peer_addr;  /* little-endian: print MSB..LSB */
    snprintf(out, out_len,
             "%02X:%02X:%02X:%02X:%02X:%02X,%s,%d,%d,%d,%d,%d,%d,%d,%s,%d,%d",
             v[5], v[4], v[3], v[2], v[1], v[0],
             s_method, s_lesc, s_sec_mode, s_sec_level,
             s_sec_level >= 2, s_sec_level >= 3, s_bonded, s_key_size,
             s_have_peer_params ? io_name(s_peer_params.io_caps) : "unknown",
             s_have_peer_params ? s_peer_params.bond : 0,
             s_have_peer_params ? s_peer_params.mitm : 0);
    return 0;
}
