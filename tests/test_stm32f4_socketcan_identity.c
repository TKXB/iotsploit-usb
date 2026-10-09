/* Compile the real board descriptor/identity owner with a fake UID. Runtime
 * verification requires USB, *IDN? and the programmer to agree byte for byte. */
#include "tusb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t stub_uid[3] = { 0x0030002D, 0x31314706, 0x36343037 };

const char *board_serial(void);
const char *board_idn(void);
const uint8_t *tud_descriptor_device_cb(void);
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t language_id);

int main(void) {
    const char expected_serial[] = "2D0030000647313137303436";
    const char expected_idn[] =
        "IoTSploit,STM32F4-Disco-SocketCAN,2D0030000647313137303436,0.1.0";
    assert(strcmp(board_serial(), expected_serial) == 0);
    assert(strcmp(board_idn(), expected_idn) == 0);
    assert(strlen(board_idn()) == sizeof(expected_idn) - 1);

    const tusb_desc_device_t *device =
        (const tusb_desc_device_t *)tud_descriptor_device_cb();
    const uint16_t *serial = tud_descriptor_string_cb(device->iSerialNumber, 0x0409);
    assert(serial != NULL);
    assert((serial[0] >> 8) == TUSB_DESC_STRING);
    assert((serial[0] & 0xFF) == 2 + 2 * strlen(expected_serial));
    for (unsigned i = 0; i < strlen(expected_serial); i++) {
        assert(serial[i + 1] == (uint8_t)expected_serial[i]);
    }

    puts("STM32F4 SocketCAN USB/SCPI identity matches the programmer UID and full version");
    return 0;
}
