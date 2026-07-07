#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#include "sdkconfig.h"

/* ---- MCU / OS ---- */
#define CFG_TUSB_MCU              OPT_MCU_ESP32S3
#define CFG_TUSB_OS              OPT_OS_FREERTOS
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

/* ---- Device ---- */
#define CFG_TUD_ENABLED          1
#define CFG_TUD_ENDPOINT0_SIZE  64

/* ---- Classes: USBTMC only (this demo's control path) ---- */
#define CFG_TUD_USBTMC           1
#define CFG_TUD_CDC              0
#define CFG_TUD_MSC              0
#define CFG_TUD_HID              0
#define CFG_TUD_MIDI             0
#define CFG_TUD_VENDOR           0

/* ---- USBTMC buffers (Full-Speed bulk 64B) ---- */
#define CFG_TUD_USBTMC_ENABLE_488          1
#define CFG_TUD_USBTMC_BULK_EPSIZE        64

/* ESP32-S3 DMA alignment requirements */
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN     __attribute__((aligned(4)))

#endif
