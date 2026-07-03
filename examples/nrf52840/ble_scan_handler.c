/**
 * @file ble_scan_handler.c
 * @brief BLE scan handler for nRF52840 — wraps nRF5 SDK SoftDevice + nrf_ble_scan.
 *
 * Captures advertising reports and stores deduplicated results in a static ring.
 */

#include "ble_scan_handler.h"
#include <string.h>
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_sdh_soc.h"
#include "ble.h"
#include "ble_gap.h"
#include "ble_advdata.h"
#include "nrf_ble_scan.h"
#include "app_error.h"

/* ------------------------------------------------------------------ */
/* SoftDevice configuration                                           */
/* ------------------------------------------------------------------ */

#define APP_BLE_CONN_CFG_TAG        1

/* Scan parameters */
#define SCAN_INTERVAL               0x00A0   /* 100 ms (units of 0.625 ms) */
#define SCAN_WINDOW                 0x0050   /* 50 ms */
#define SCAN_TIMEOUT                0x0000   /* No timeout (scan until stopped) */

/* ------------------------------------------------------------------ */
/* Scan module instance                                               */
/* ------------------------------------------------------------------ */

NRF_BLE_SCAN_DEF(m_scan);

/* ------------------------------------------------------------------ */
/* Results storage                                                    */
/* ------------------------------------------------------------------ */

static ble_scan_result_t s_results[MAX_SCAN_RESULTS];
static volatile uint16_t s_result_count = 0;
static volatile bool     s_scanning     = false;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Parse advertising data for the Complete Local Name or Shortened Local Name. */
static void extract_name(const ble_gap_evt_adv_report_t *p_adv, char *name_out, size_t name_len)
{
    memset(name_out, 0, name_len);

    const uint8_t *p_data = p_adv->data.p_data;
    uint16_t       data_len = p_adv->data.len;
    uint16_t       i = 0;

    while (i + 1 < data_len) {
        uint8_t field_len = p_data[i];
        uint8_t field_type = p_data[i + 1];

        if (field_len == 0)
            break;

        /* BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME (0x09) */
        if (field_type == 0x09 && (i + 2 + field_len - 1) <= data_len) {
            size_t copy_len = field_len - 1;
            if (copy_len >= name_len)
                copy_len = name_len - 1;
            memcpy(name_out, &p_data[i + 2], copy_len);
            name_out[copy_len] = '\0';
            return;
        }

        /* BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME (0x08) */
        if (field_type == 0x08 && (i + 2 + field_len - 1) <= data_len) {
            size_t copy_len = field_len - 1;
            if (copy_len >= name_len)
                copy_len = name_len - 1;
            memcpy(name_out, &p_data[i + 2], copy_len);
            name_out[copy_len] = '\0';
            return;
        }

        i += field_len + 1;
    }
}

/** Check if a BLE address is already in the results list. */
static int find_duplicate(const uint8_t *addr)
{
    for (uint16_t i = 0; i < s_result_count; i++) {
        if (memcmp(s_results[i].addr, addr, 6) == 0)
            return (int)i;
    }
    return -1;
}

/** Store or update a scan result from an advertising report. */
static void store_adv_report(const ble_gap_evt_adv_report_t *p_adv)
{
    /* Copy address (nRF stores it as 6 bytes in little-endian order) */
    const uint8_t *addr = p_adv->peer_addr.addr;
    uint8_t        addr_type = p_adv->peer_addr.addr_type;

    /* Check for duplicate */
    int dup_idx = find_duplicate(addr);
    if (dup_idx >= 0) {
        /* Update RSSI only */
        s_results[dup_idx].rssi = p_adv->rssi;
        return;
    }

    /* Buffer full — overwrite oldest (ring behaviour) */
    uint16_t idx;
    if (s_result_count < MAX_SCAN_RESULTS) {
        idx = s_result_count;
        s_result_count++;
    } else {
        idx = 0; /* Simple wrap: overwrite index 0 */
    }

    memcpy(s_results[idx].addr, addr, 6);
    s_results[idx].addr_type = addr_type;
    s_results[idx].rssi      = p_adv->rssi;
    s_results[idx].adv_type  = (p_adv->type.connectable) ? 0 : 1;
    extract_name(p_adv, s_results[idx].name, BLE_NAME_MAX_LEN + 1);
}

/* ------------------------------------------------------------------ */
/* nrf_ble_scan event handler                                         */
/* ------------------------------------------------------------------ */

static void scan_evt_handler(scan_evt_t const *p_scan_evt)
{
    switch (p_scan_evt->scan_evt_id) {
    case NRF_BLE_SCAN_EVT_NOT_FOUND: {
        /* No filter match — but we got an advertising report.
         * The adv report pointer is in p_not_found. */
        const ble_gap_evt_adv_report_t *p_adv = p_scan_evt->params.p_not_found;
        if (p_adv) {
            store_adv_report(p_adv);
        }
        break;
    }
    case NRF_BLE_SCAN_EVT_SCAN_TIMEOUT:
        s_scanning = false;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                        */
/* ------------------------------------------------------------------ */

/* Note: No manual NRF_SDH_BLE_OBSERVER needed here. The NRF_BLE_SCAN_DEF
 * macro in ble_scan_handler.h already registers an observer that routes
 * BLE events to nrf_ble_scan_on_ble_evt. The SDK's nrf_sdh_ble.c also
 * registers its own stack observer for event polling. Adding another
 * observer here would be redundant and referencing nrf_sdh_ble_evts_poll
 * (which is static in nrf_sdh_ble.c) would be a link error. */

int ble_scan_init(void)
{
    ret_code_t err;

    /* Enable SoftDevice handler */
    err = nrf_sdh_enable_request();
    if (err != NRF_SUCCESS) return -1;

    /* Configure BLE stack */
    uint32_t ram_start = 0;
    err = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start);
    if (err != NRF_SUCCESS) return -1;

    err = nrf_sdh_ble_enable(&ram_start);
    if (err != NRF_SUCCESS) return -1;

    /* Configure scanning parameters */
    static ble_gap_scan_params_t const m_scan_params = {
        .active        = 0x01,             /* Active scanning */
        .interval      = SCAN_INTERVAL,
        .window        = SCAN_WINDOW,
        .timeout       = SCAN_TIMEOUT,
        .filter_policy = BLE_GAP_SCAN_FP_ACCEPT_ALL,
    };

    nrf_ble_scan_init_t scan_init = {
        .p_scan_param   = &m_scan_params,
        .connect_if_match = false,
        .p_conn_param   = NULL,
        .conn_cfg_tag   = APP_BLE_CONN_CFG_TAG,
    };

    err = nrf_ble_scan_init(&m_scan, &scan_init, scan_evt_handler);
    if (err != NRF_SUCCESS) return -1;

    s_result_count = 0;
    s_scanning = false;

    return 0;
}

void ble_scan_start(void)
{
    ret_code_t err = nrf_ble_scan_start(&m_scan);
    if (err == NRF_SUCCESS) {
        s_scanning = true;
    }
}

void ble_scan_start_timed(uint16_t secs)
{
    /* Clamp so secs*100 (the SoftDevice's 10 ms timeout units) fits in the
       uint16_t timeout field; 0 would scan forever, defeating the auto-stop. */
    if (secs == 0)   secs = 5;
    if (secs > 600)  secs = 600;

    ble_gap_scan_params_t params = {
        .active        = 0x01,
        .interval      = SCAN_INTERVAL,
        .window        = SCAN_WINDOW,
        .timeout       = (uint16_t)(secs * 100),  /* units of 10 ms */
        .filter_policy = BLE_GAP_SCAN_FP_ACCEPT_ALL,
    };
    if (nrf_ble_scan_params_set(&m_scan, &params) != NRF_SUCCESS) {
        return;
    }

    ret_code_t err = nrf_ble_scan_start(&m_scan);
    if (err == NRF_SUCCESS) {
        s_scanning = true;
    }
}

void ble_scan_stop(void)
{
    nrf_ble_scan_stop();
    s_scanning = false;
}

void ble_scan_clear(void)
{
    s_result_count = 0;
}

uint16_t ble_scan_count(void)
{
    return s_result_count;
}

bool ble_scan_get_result(uint16_t index, ble_scan_result_t *out)
{
    if (index >= s_result_count || !out)
        return false;
    *out = s_results[index];
    return true;
}

bool ble_scan_is_scanning(void)
{
    return s_scanning;
}
