#ifndef SDK_CONFIG_H
#define SDK_CONFIG_H

/* =================================================================
 * Minimal sdk_config.h for nRF52840 BLE scanner + TinyUSB USBTMC
 * Based on nRF5 SDK ble_app_blinky_c example config
 * ================================================================= */

/* ---- SoftDevice handler (SDH) ---- */
#define NRF_SDH_ENABLED                 1
#define NRF_SDH_BLE_ENABLED             1
#define NRF_SDH_SOC_ENABLED             1

#define NRF_SDH_BLE_CENTRAL_LINK_COUNT  1
#define NRF_SDH_BLE_PERIPHERAL_LINK_COUNT 0
#define NRF_SDH_BLE_TOTAL_LINK_COUNT    1

#define NRF_SDH_BLE_GAP_EVENT_LENGTH    6
#define NRF_SDH_BLE_GATT_MAX_MTU_SIZE  23
#define NRF_SDH_BLE_GATTS_ATTR_TAB_SIZE 1408
#define NRF_SDH_BLE_GAP_DATA_LENGTH    27
#define NRF_SDH_BLE_VS_UUID_COUNT      1
#define NRF_SDH_BLE_SERVICE_CHANGED    0

/* Clock source */
#define NRF_SDH_CLOCK_LF_SRC            1
#define NRF_SDH_CLOCK_LF_RC_CTIV        0
#define NRF_SDH_CLOCK_LF_RC_TEMP_CTIV   0
#define NRF_SDH_CLOCK_LF_ACCURACY       7

/* Observer priority levels */
#define NRF_SDH_REQ_OBSERVER_PRIO_LEVELS   2
#define NRF_SDH_STATE_OBSERVER_PRIO_LEVELS 2
#define NRF_SDH_STACK_OBSERVER_PRIO_LEVELS 2
#define NRF_SDH_SOC_OBSERVER_PRIO_LEVELS   2
#define NRF_SDH_BLE_OBSERVER_PRIO_LEVELS   4

/* Observer priorities (individual) */
#define NRF_SDH_BLE_STACK_OBSERVER_PRIO  0
#define NRF_SDH_SOC_STACK_OBSERVER_PRIO 0
#define CLOCK_CONFIG_STATE_OBSERVER_PRIO 0
#define POWER_CONFIG_STATE_OBSERVER_PRIO 0

/* ---- BLE scan module ---- */
#define NRF_BLE_SCAN_ENABLED            1
#define NRF_BLE_SCAN_FILTER_ENABLE     1
#define NRF_BLE_SCAN_NAME_CNT          0
#define NRF_BLE_SCAN_NAME_MAX_LEN      32
#define NRF_BLE_SCAN_SHORT_NAME_CNT    0
#define NRF_BLE_SCAN_SHORT_NAME_MAX_LEN 32
#define NRF_BLE_SCAN_ADDRESS_CNT       0
#define NRF_BLE_SCAN_UUID_CNT          0
#define NRF_BLE_SCAN_APPEARANCE_CNT    0
#define NRF_BLE_SCAN_BUFFER            33
#define NRF_BLE_SCAN_OBSERVER_PRIO     1
#define NRF_BLE_SCAN_MIN_CONNECTION_INTERVAL 7.5
#define NRF_BLE_SCAN_MAX_CONNECTION_INTERVAL 30
#define NRF_BLE_SCAN_SLAVE_LATENCY     0
#define NRF_BLE_SCAN_CONN_SUP_TIMEOUT  MSEC_TO_UNITS(4000, UNIT_10_MS)
#define NRF_BLE_SCAN_SCAN_INTERVAL     160
#define NRF_BLE_SCAN_SCAN_WINDOW       80
#define NRF_BLE_SCAN_SCAN_DURATION     0
#define NRF_BLE_SCAN_SUPERVISION_TIMEOUT 4000

/* ---- nrfx drivers ---- */
#define NRFX_USBD_ENABLED              0   /* TinyUSB owns USB, not Nordic USBD */
/* ---- Legacy power driver (nrf_drv_power) for USB VBUS events ---- */
#define POWER_ENABLED                   1
#define POWER_CONFIG_IRQ_PRIORITY       7
#define POWER_CONFIG_DEFAULT_DCDCEN     0
#define POWER_CONFIG_DEFAULT_DCDCENHV   0
#define POWER_CONFIG_SOC_OBSERVER_PRIO  0
#define CLOCK_CONFIG_IRQ_PRIORITY       7

#define NRFX_POWER_ENABLED              1
#define NRFX_POWER_CONFIG_IRQ_PRIORITY  7
#define NRFX_CLOCK_ENABLED              1
#define NRFX_CLOCK_CONFIG_IRQ_PRIORITY  7
#define NRFX_CLOCK_CONFIG_LF_SRC        1   /* External crystal */
#define NRFX_GPIO_ENABLED               1
#define NRFX_GPIOTE_ENABLED             1
#define NRFX_GPIOTE_CONFIG_NUM_OF_LOW_POWER_EVENTS 1
#define NRFX_GPIOTE_CONFIG_IRQ_PRIORITY  6
#define NRFX_SYSTICK_ENABLED            1
#define NRFX_RTC_ENABLED                1
#define NRFX_RTC0_ENABLED               1
#define NRFX_RTC1_ENABLED               1
#define NRFX_UART_ENABLED               0
#define NRFX_UARTE_ENABLED              0

/* ---- App libraries ---- */
#define APP_TIMER_ENABLED               1
#define APP_TIMER_V2                    1
#define APP_TIMER_V2_RTC1_ENABLED       1
#define APP_TIMER_CONFIG_RTC_FREQUENCY  0
#define APP_TIMER_CONFIG_IRQ_PRIORITY  7

/* ---- Logging (disabled to reduce code size) ---- */
#define NRF_LOG_ENABLED                 0
#define NRF_LOG_BACKEND_RTT_ENABLED     0
#define NRF_LOG_BACKEND_UART_ENABLED    0
#define NRF_LOG_DEFAULT_LEVEL           0

/* ---- Section variables ---- */
#define NRF_SECTION_ITER_ENABLED        1

/* ---- Misc ---- */
#define NRF_STRERROR_ENABLED            0
#define NRF_BALLOC_ENABLED              1
#define NRF_FPRINTF_ENABLED             0
#define NRF_MEMOBJ_ENABLED              0
#define NRF_RINGBUF_ENABLED             0
#define NRF_ATOMIC_ENABLED              1
#define NRF_ATFIFO_ENABLED              0
#define APP_UTIL_PLATFORM_ENABLED        1

#endif /* SDK_CONFIG_H */
