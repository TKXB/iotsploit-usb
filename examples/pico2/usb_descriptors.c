#include "tusb.h"
#include "usbscpi/usbscpi.h"

/* ------------------------------------------------------------------
 * USB Device Descriptor
 * ------------------------------------------------------------------ */
tusb_desc_device_t const desc_device = {
    .bLength         = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB          = 0x0200,
    .bDeviceClass    = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor        = 0x1209,
    .idProduct       = 0x0001,
    .bcdDevice       = 0x0100,
    .iManufacturer   = 0x01,
    .iProduct        = 0x02,
    .iSerialNumber   = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

/* ------------------------------------------------------------------
 * USB Configuration Descriptor (USBTMC interface + bulk IN/OUT)
 * ------------------------------------------------------------------ */
enum {
    ITF_NUM_USBTMC = 0,
    ITF_NUM_TOTAL
};

#define USBTMC_EP_OUT 0x01
#define USBTMC_EP_IN  0x81

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_INTERFACE_DESC_LEN + TUD_ENDPOINT_DESC_LEN * 2)

uint8_t const desc_configuration[] = {
    /* Configuration descriptor */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    /* Interface descriptor: Class=0xFE, SubClass=0x03, Protocol=0x01 (USBTMC) */
    TUD_INTERFACE_DESCRIPTOR(ITF_NUM_USBTMC, 0, 2, 0xFE, 0x03, 0x01, 0),
    /* Bulk OUT endpoint */
    TUD_ENDPOINT_DESCRIPTOR(USBTMC_EP_OUT, TUSB_XFER_BULK, 64, 0),
    /* Bulk IN endpoint */
    TUD_ENDPOINT_DESCRIPTOR(USBTMC_EP_IN, TUSB_XFER_BULK, 64, 0),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

/* ------------------------------------------------------------------
 * USB String Descriptors
 * ------------------------------------------------------------------ */
static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t language_id) {
    (void)language_id;
    const char *str = NULL;
    uint8_t chr_count = 0;

    switch (index) {
    case 0:
        _desc_str[1] = 0x0409; /* English (US) */
        chr_count = 1;
        break;
    case 1: str = "IoTSploit"; break;
    case 2: str = "Pico2 USBTMC"; break;
    case 3: str = "0001"; break;
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
