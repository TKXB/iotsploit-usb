#ifndef USBSCPI_TEST_STUB_DESIG_H
#define USBSCPI_TEST_STUB_DESIG_H

#include <stdint.h>

extern uint32_t stub_uid[3];
#define DESIG_UNIQUE_ID0 stub_uid[0]
#define DESIG_UNIQUE_ID1 stub_uid[1]
#define DESIG_UNIQUE_ID2 stub_uid[2]

#endif
