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

/* ---- Classes:USBTMC + Vendor(日志输出) ---- */
#define CFG_TUD_USBTMC           1   /* ★ 本 demo 的核心,esp_tinyusb 没有这一项 */
#define CFG_TUD_CDC              0
#define CFG_TUD_MSC              0
#define CFG_TUD_HID              0
#define CFG_TUD_MIDI             0
#define CFG_TUD_VENDOR           1   /* 第二接口:设备日志(bulk-IN 0x82) */

/* ---- USBTMC 缓冲(Full-Speed bulk 64B) ---- */
#define CFG_TUD_USBTMC_ENABLE_488          1
#define CFG_TUD_USBTMC_BULK_EPSIZE        64

/* ---- Vendor(日志)缓冲 ----
 * 只用 IN 方向传日志;OUT 端点存在但主机不写(TUD_VENDOR_DESCRIPTOR 强制两端点)。
 * TX FIFO 放大到 4KB,吸收突发日志;RX 保持最小。 */
#define CFG_TUD_VENDOR_EPSIZE             64
#define CFG_TUD_VENDOR_TX_BUFSIZE       4096
#define CFG_TUD_VENDOR_RX_BUFSIZE         64

/* ESP32-S3 DMA 对齐要求 */
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN     __attribute__((aligned(4)))

#endif
