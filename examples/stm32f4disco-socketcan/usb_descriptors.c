#include <libopencm3/stm32/desig.h>

#include "tusb.h"
#include "usbscpi/usbscpi.h"
#include "can_stream_itf.h"

/* ------------------------------------------------------------------
 * USB Device Descriptor
 * ------------------------------------------------------------------ */
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x1209,
    .idProduct          = 0x0001,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

/* ------------------------------------------------------------------
 * USB Configuration Descriptor: gs_usb (SocketCAN) + USBTMC + CAN stream
 * gs_usb must be interface 0 with bulk IN 0x81 / OUT 0x02: Linux sends its
 * control requests with wIndex 0, and kernels before endpoint discovery
 * hard-code those two endpoint numbers. The CAN stream (can_stream.h) takes
 * the remaining endpoints: OTG FS has four per direction, EP0 included.
 * ------------------------------------------------------------------ */
enum {
    ITF_NUM_GS_USB = 0,
    ITF_NUM_USBTMC,
    ITF_NUM_STREAM,
    ITF_NUM_TOTAL
};

#define GS_USB_EP_IN  0x81
#define GS_USB_EP_OUT 0x02
#define USBTMC_EP_OUT 0x03
#define USBTMC_EP_IN  0x83
#define STREAM_EP_OUT 0x01  /* unused: the host never writes the stream */
#define STREAM_EP_IN  0x82

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + \
                          TUD_USBTMC_IF_DESCRIPTOR_LEN + TUD_USBTMC_BULK_DESCRIPTORS_LEN + \
                          CAN_STREAM_DESC_LEN)

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_GS_USB, 4, GS_USB_EP_OUT, GS_USB_EP_IN, 64),
    TUD_USBTMC_IF_DESCRIPTOR(ITF_NUM_USBTMC, 2, 0, TUD_USBTMC_PROTOCOL_USB488),
    TUD_USBTMC_BULK_DESCRIPTORS(USBTMC_EP_OUT, USBTMC_EP_IN, 64),
    CAN_STREAM_DESCRIPTOR(ITF_NUM_STREAM, 5, STREAM_EP_OUT, STREAM_EP_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

/* ------------------------------------------------------------------
 * USB String Descriptors
 * ------------------------------------------------------------------ */
/* Chip-unique serial number, shared by the USB descriptor and *IDN? so two
 * identical boards can be told apart. */
const char *board_serial(void) {
    static char serial[25];
    if (serial[0] == '\0') {
        desig_get_unique_id_as_string(serial, sizeof(serial));
    }
    return serial;
}

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t language_id) {
    (void)language_id;
    const char *str = NULL;
    uint8_t chr_count = 0;

    switch (index) {
    case 0:
        _desc_str[1] = 0x0409;  /* English (US) */
        chr_count = 1;
        break;
    case 1: str = "IoTSploit";            break;
    case 2: str = "STM32F4-Disco SocketCAN"; break;
    case 3: str = board_serial();         break;
    case 4: str = "gs_usb CAN";           break;
    case 5: str = "IoTSploit CAN stream"; break;
    default: return NULL;
    }

    if (str) {
        while (*str && chr_count < 31) {
            _desc_str[1 + chr_count++] = *str++;
        }
    }

    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);
    return _desc_str;
}
