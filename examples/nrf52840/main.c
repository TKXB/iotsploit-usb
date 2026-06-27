#include <stdio.h>
#include <string.h>

#include "tusb.h"
#include "usbscpi/usbscpi.h"
#include "usbscpi_tinyusb.h"
#include "ble_scan_handler.h"
#include "nrfx_power.h"
#include "nrfx_clock.h"
#include "app_error.h"
#include "nrf_soc.h"
#include "nrf_sdh_soc.h"

/* ---------- Static buffers (no dynamic allocation) ---------- */
static uint8_t s_storage[2048];
static char    s_line[96];
/* io_buf doubles as the SYST:HELP:DESC? render buffer; the descriptor text is
   ~0.75 KiB, so size it well above the 256-byte mtu. */
static uint8_t s_io[1024];

/* ---------- USB TX callback via TinyUSB glue ---------- */
static int usb_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user; (void)eom;
    return usbscpi_tinyusb_tx(NULL, data, len, true);
}

/* ---------- BLE SCPI command callbacks ---------- */
static scpi_result_t cmd_ble_scan_start(scpi_t *ctx) {
    (void)ctx;
    ble_scan_start();
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_scan_stop(scpi_t *ctx) {
    (void)ctx;
    ble_scan_stop();
    return SCPI_RES_OK;
}

static scpi_result_t cmd_ble_scan_state(scpi_t *ctx) {
    return SCPI_ResultUInt32(ctx, ble_scan_is_scanning() ? 1 : 0);
}

static scpi_result_t cmd_ble_scan_count(scpi_t *ctx) {
    return SCPI_ResultUInt32(ctx, ble_scan_count());
}

static scpi_result_t cmd_ble_scan_result(scpi_t *ctx) {
    uint32_t index = 0;
    if (SCPI_ParamUInt32(ctx, &index, TRUE) != TRUE) return SCPI_RES_ERR;

    ble_scan_result_t r;
    if (!ble_scan_get_result((uint16_t)index, &r)) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }

    /* Format: "AA:BB:CC:DD:EE:FF,-67,DeviceName" */
    char buf[80];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X,%d,%s",
             r.addr[5], r.addr[4], r.addr[3], r.addr[2], r.addr[1], r.addr[0],
             r.rssi, r.name[0] ? r.name : "(unknown)");
    return SCPI_ResultCharacters(ctx, buf, strlen(buf));
}

static scpi_result_t cmd_ble_scan_clear(scpi_t *ctx) {
    (void)ctx;
    ble_scan_clear();
    return SCPI_RES_OK;
}

static const scpi_command_t ble_commands[] = {
    { "BLE:SCAN:START",   cmd_ble_scan_start,  0 },
    { "BLE:SCAN:STOP",    cmd_ble_scan_stop,    0 },
    { "BLE:SCAN:STATe?",  cmd_ble_scan_state,   0 },
    { "BLE:SCAN:COUNt?",  cmd_ble_scan_count,   0 },
    { "BLE:SCAN:RESult?", cmd_ble_scan_result,  0 },
    { "BLE:SCAN:CLEar",   cmd_ble_scan_clear,   0 },
    SCPI_CMD_LIST_END
};

/* ---------- Descriptor metadata (SYSTem:HELP:DESCription?) ---------- */
/* The device is the single source of truth for its command/workflow metadata:
   the host fetches this over SYST:HELP:DESC? instead of carrying a local copy. */

static const usbscpi_param_desc_t desc_ble_scan_result_params[] = {
    { "index", "u32", true },
};

static const usbscpi_command_desc_t desc_commands[] = {
    { "BLE:SCAN:START",  "command", "Start BLE scanning",
      NULL, 0, NULL },
    { "BLE:SCAN:STOP",   "command", "Stop BLE scanning",
      NULL, 0, NULL },
    { "BLE:SCAN:STATe?", "query",   "1 = scanning, 0 = idle/finished",
      NULL, 0, "bool" },
    { "BLE:SCAN:COUNt?", "query",   "Number of BLE devices found",
      NULL, 0, "u32" },
    { "BLE:SCAN:RESult?", "query",  "Get scan result by index (addr,rssi,name)",
      desc_ble_scan_result_params, 1, "string" },
    { "BLE:SCAN:CLEar",  "command", "Clear scan results",
      NULL, 0, NULL },
};

static const usbscpi_workflow_desc_t desc_workflows[] = {
    {
        .name = "ble-scan",
        .type = "trigger_poll_fetch",
        .summary = "Scan for BLE devices",
        .trigger_cmd = "BLE:SCAN:START",
        .done_query = "BLE:SCAN:STATe?",
        .done_value = "0",
        .count_query = "BLE:SCAN:COUNt?",
        .fetch_query = "BLE:SCAN:RESult?",
        .state_query = NULL,
        .success_value = NULL,
        .failed_values = NULL,
        .failed_value_count = 0,
        .timeout_ms = 30000,
        .poll_ms = 500,
    },
};

static const usbscpi_descriptor_t s_descriptor = {
    .commands = desc_commands,
    .command_count = sizeof(desc_commands) / sizeof(desc_commands[0]),
    .workflows = desc_workflows,
    .workflow_count = sizeof(desc_workflows) / sizeof(desc_workflows[0]),
};

/* ---------- TinyUSB USBTMC required callbacks ---------- */
#if CFG_TUD_USBTMC_ENABLE_488
usbtmc_response_capabilities_488_t const *tud_usbtmc_get_capabilities_cb(void) {
    static usbtmc_response_capabilities_488_t caps = {
        .USBTMC_status = USBTMC_STATUS_SUCCESS,
        .bcdUSBTMC = 0x0100,
        .bmDevCapabilities = { 0 },
        .bcdUSB488 = 0x0100,
        .bmIntfcCapabilities = { 0 },
        .bmDevCapabilities488 = { 0 },
    };
    return &caps;
}
#else
usbtmc_response_capabilities_t const *tud_usbtmc_get_capabilities_cb(void) {
    static usbtmc_response_capabilities_t caps = {
        .USBTMC_status = USBTMC_STATUS_SUCCESS,
        .bcdUSBTMC = 0x0100,
    };
    return &caps;
}
#endif

bool tud_usbtmc_msgBulkOut_start_cb(usbtmc_msg_request_dev_dep_out const *msg) {
    (void)msg; return true;
}
bool tud_usbtmc_initiate_abort_bulk_out_cb(uint8_t *tmcResult) {
    *tmcResult = USBTMC_STATUS_SUCCESS; return true;
}
bool tud_usbtmc_check_abort_bulk_out_cb(usbtmc_check_abort_bulk_rsp_t *rsp) {
    rsp->USBTMC_status = USBTMC_STATUS_SUCCESS; return true;
}
bool tud_usbtmc_initiate_abort_bulk_in_cb(uint8_t *tmcResult) {
    *tmcResult = USBTMC_STATUS_SUCCESS; return true;
}
bool tud_usbtmc_check_abort_bulk_in_cb(usbtmc_check_abort_bulk_rsp_t *rsp) {
    rsp->USBTMC_status = USBTMC_STATUS_SUCCESS; return true;
}
bool tud_usbtmc_initiate_clear_cb(uint8_t *tmcResult) {
    *tmcResult = USBTMC_STATUS_SUCCESS; return true;
}
bool tud_usbtmc_check_clear_cb(usbtmc_get_clear_status_rsp_t *rsp) {
    rsp->USBTMC_status = USBTMC_STATUS_SUCCESS; return true;
}

/* nrfx_clock event sink. The POWER and CLOCK peripherals share one IRQ, so the
   combined nrfx_power_clock_irq_handler also runs nrfx_clock_irq_handler. The
   TinyUSB nRF driver starts HFCLK via the raw HAL, which sets the HFCLKSTARTED
   event; without an initialized nrfx_clock handler that path calls a NULL
   callback and HardFaults. A no-op handler safely absorbs the event. */
static void clock_event_handler(nrfx_clock_evt_type_t event) { (void)event; }

/* ---------- nRF USB VBUS power events -> TinyUSB ---------- */
/* nRF52840 USBD is powered by the USB regulator; TinyUSB only enables the
   peripheral when it receives these VBUS events. SoftDevice is currently
   disabled, so nrfx_power drives the USB regulator directly. */
extern void tusb_hal_nrf_power_event(uint32_t event);

static void power_usb_event_handler(nrfx_power_usb_evt_t event) {
    switch (event) {
        case NRFX_POWER_USB_EVT_DETECTED: tusb_hal_nrf_power_event(0); break; /* DETECTED */
        case NRFX_POWER_USB_EVT_REMOVED:  tusb_hal_nrf_power_event(1); break; /* REMOVED  */
        case NRFX_POWER_USB_EVT_READY:    tusb_hal_nrf_power_event(2); break; /* READY    */
        default: break;
    }
}

extern uint32_t __isr_vector;  /* application vector table base (0x27000) */

/* USB VBUS power events while the SoftDevice is enabled. POWER is then owned
   by the SoftDevice, so the events arrive as SoC events instead of via
   nrfx_power; forward them to TinyUSB the same way. */
static void usb_soc_evt_handler(uint32_t sys_evt, void *p_context) {
    (void)p_context;
    switch (sys_evt) {
        case NRF_EVT_POWER_USB_DETECTED:    tusb_hal_nrf_power_event(0); break;
        case NRF_EVT_POWER_USB_REMOVED:     tusb_hal_nrf_power_event(1); break;
        case NRF_EVT_POWER_USB_POWER_READY: tusb_hal_nrf_power_event(2); break;
        default: break;
    }
}
NRF_SDH_SOC_OBSERVER(m_usb_soc_obs, 0, usb_soc_evt_handler, NULL);

/* ---------- USBD interrupt -> TinyUSB ---------- */
/* TinyUSB provides dcd_int_handler() but does NOT define the USBD_IRQHandler
   vector; without this the weak Default_Handler (infinite loop) runs on the
   first USB interrupt and the device hangs before enumerating. */
void USBD_IRQHandler(void) {
    tud_int_handler(0);
}

/* ---------- Main ---------- */
int main(void) {
    /* Bring up BLE/SoftDevice first; once enabled it owns CLOCK and POWER, so
       USB clock/power must then be driven through SoftDevice APIs. */
    bool sd_on = (ble_scan_init() == 0);

    if (sd_on) {
        /* SoftDevice owns POWER: subscribe to USB VBUS events as SoC events
           (routed to usb_soc_evt_handler). The current state is kicked after
           tusb_init() below. */
        sd_power_usbdetected_enable(1);
        sd_power_usbpwrrdy_enable(1);
        sd_power_usbremoved_enable(1);
    } else {
        /* No SoftDevice: drive POWER/CLOCK directly via nrfx. nrfx_clock must be
           initialized so the shared POWER_CLOCK IRQ has a valid clock callback
           (TinyUSB raw HFCLK start sets the HFCLKSTARTED event). */
        APP_ERROR_CHECK(nrfx_clock_init(clock_event_handler));
        nrfx_clock_enable();
        static const nrfx_power_config_t pwr_cfg = { 0 };
        APP_ERROR_CHECK(nrfx_power_init(&pwr_cfg));
        static const nrfx_power_usbevt_config_t usbevt_cfg = { .handler = power_usb_event_handler };
        nrfx_power_usbevt_init(&usbevt_cfg);
        nrfx_power_usbevt_enable();
    }

    /* 1. Init TinyUSB (USBTMC device) first — USB works even if BLE fails */
    tusb_init();

    /* If the SoftDevice is up and VBUS was already present at boot, the
       DETECTED/READY edges predate our subscription, so kick TinyUSB now
       (must be after tusb_init so the device stack is ready). */
    if (sd_on) {
        uint32_t usbreg = 0;
        if (sd_power_usbregstatus_get(&usbreg) == NRF_SUCCESS) {
            if (usbreg & POWER_USBREGSTATUS_VBUSDETECT_Msk) tusb_hal_nrf_power_event(0);
            if (usbreg & POWER_USBREGSTATUS_OUTPUTRDY_Msk)  tusb_hal_nrf_power_event(2);
        }
    }

    /* 2. Init usbscpi core */
    usbscpi_config_t cfg = {
        .usb_tx        = usb_tx,
        .line_buf      = s_line,
        .line_buf_len  = sizeof(s_line),
        .max_block_len = 4096,
        .idn           = "IoTSploit,nRF52840,0001,0.1.0",
        .io_buf        = s_io,
        .io_buf_len    = sizeof(s_io),
        .proto         = 1,
        .mtu           = 256,
        .descriptor    = &s_descriptor,
    };

    usbscpi_t *dev = usbscpi_init(s_storage, sizeof(s_storage), &cfg);
    usbscpi_tinyusb_bind(dev);
    usbscpi_register(dev, ble_commands);

    /* BLE/SoftDevice was already initialized at the top of main(). */

    /* 4. Main loop: pump USB + BLE events */
    while (1) {
        tud_task();
        usbscpi_task(dev);
    }
}
