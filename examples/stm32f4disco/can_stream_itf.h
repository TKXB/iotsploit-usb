#ifndef STM32F4DISCO_CAN_STREAM_ITF_H
#define STM32F4DISCO_CAN_STREAM_ITF_H

/* USB descriptor of the CAN stream interface (can_stream.h), for
 * usb_descriptors.c. */

/* Stream interface descriptor: vendor class with subclass/protocol 'I','S'
 * ("IoTSploit stream"), so a host can tell it from another vendor interface on
 * the same device (gs_usb on the SocketCAN board). */
#define CAN_STREAM_ITF_SUBCLASS 0x49u
#define CAN_STREAM_ITF_PROTOCOL 0x53u
#define CAN_STREAM_DESC_LEN     TUD_VENDOR_DESC_LEN
#define CAN_STREAM_DESCRIPTOR(_itfnum, _stridx, _epout, _epin, _epsize)                 \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 2, TUSB_CLASS_VENDOR_SPECIFIC,                   \
    CAN_STREAM_ITF_SUBCLASS, CAN_STREAM_ITF_PROTOCOL, _stridx,                           \
    7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0,            \
    7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0

#endif
