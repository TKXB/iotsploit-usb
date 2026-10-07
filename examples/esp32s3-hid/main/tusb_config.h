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

/* ---- Classes: HID keyboard only ----
 * HID-only profile: the single USB port is the keyboard presented to the
 * target computer. SCPI control arrives over Wi-Fi TCP, not USB, so USBTMC and
 * the vendor log interface are both disabled and their descriptors, callbacks
 * and glue are omitted. */
#define CFG_TUD_USBTMC           0
#define CFG_TUD_CDC              0
#define CFG_TUD_MSC              0
#define CFG_TUD_HID              1
#define CFG_TUD_MIDI             0
#define CFG_TUD_VENDOR           0

/* ---- HID buffers ---- */
#define CFG_TUD_HID_EP_BUFSIZE            16

/* ESP32-S3 DMA alignment */
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN     __attribute__((aligned(4)))

#endif
