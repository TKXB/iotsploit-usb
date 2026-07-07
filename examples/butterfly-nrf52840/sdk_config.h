#ifndef SDK_CONFIG_H
#define SDK_CONFIG_H

/* =================================================================
 * Minimal sdk_config.h for the butterfly nRF52840 firmware:
 * bare-metal radio + TinyUSB USBTMC, NO SoftDevice.
 * ================================================================= */

/* ---- nrfx drivers ---- */
#define NRFX_USBD_ENABLED              0   /* TinyUSB owns USB, not Nordic USBD */

/* ---- Legacy/nrfx power + clock (for USB VBUS events + HFCLK) ---- */
#define POWER_ENABLED                   1
#define POWER_CONFIG_IRQ_PRIORITY       7
#define POWER_CONFIG_DEFAULT_DCDCEN     0
#define POWER_CONFIG_DEFAULT_DCDCENHV   0
#define CLOCK_CONFIG_IRQ_PRIORITY       7

#define NRFX_POWER_ENABLED              1
#define NRFX_POWER_CONFIG_IRQ_PRIORITY  7
#define NRFX_CLOCK_ENABLED              1
#define NRFX_CLOCK_CONFIG_IRQ_PRIORITY  7
#define NRFX_CLOCK_CONFIG_LF_SRC        1   /* External crystal */

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
