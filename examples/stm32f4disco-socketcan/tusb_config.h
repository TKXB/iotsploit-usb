#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

/* ---- MCU / OS ---- */
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU            OPT_MCU_STM32F4
#endif
#define CFG_TUSB_OS             OPT_OS_NONE
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

/* ---- DWC2 driver: slave mode (no DMA on F4 Discovery FS) ---- */
#define CFG_TUD_DWC2_SLAVE_ENABLE  1
#define CFG_TUD_DWC2_DMA_ENABLE    0

/* ---- Device ---- */
#define CFG_TUD_ENABLED         1
#define CFG_TUD_ENDPOINT0_SIZE  64

/* ---- Classes: USBTMC + vendor 0 (gs_usb SocketCAN) + vendor 1 (CAN stream) ---- */
#define CFG_TUD_USBTMC          1
#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          2

/* ---- USBTMC (USB488, full-speed bulk 64B) ---- */
#define CFG_TUD_USBTMC_ENABLE_488   1
#define CFG_TUD_USBTMC_BULK_EPSIZE  64

/* ---- Vendor: gs_usb sends one 20-byte frame per transfer (gs_poll() waits
 * for an empty FIFO, so the larger TX buffer does not batch them); the stream
 * queues several 36-byte records ahead of the endpoint. ---- */
#define CFG_TUD_VENDOR_EPSIZE       64
#define CFG_TUD_VENDOR_RX_BUFSIZE   64
#define CFG_TUD_VENDOR_TX_BUFSIZE   512

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

#endif
