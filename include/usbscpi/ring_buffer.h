#ifndef USBSCPI_RING_BUFFER_H
#define USBSCPI_RING_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;
    size_t size;
    volatile size_t head;
    volatile size_t tail;
} usbscpi_ring_t;

int usbscpi_ring_init(usbscpi_ring_t *ring, uint8_t *storage, size_t size);
size_t usbscpi_ring_count(const usbscpi_ring_t *ring);
size_t usbscpi_ring_free(const usbscpi_ring_t *ring);
size_t usbscpi_ring_write(usbscpi_ring_t *ring, const uint8_t *data, size_t len);
size_t usbscpi_ring_read(usbscpi_ring_t *ring, uint8_t *data, size_t len);
size_t usbscpi_ring_peek_linear(const usbscpi_ring_t *ring, const uint8_t **data);
void usbscpi_ring_advance(usbscpi_ring_t *ring, size_t len);
void usbscpi_ring_clear(usbscpi_ring_t *ring);

#ifdef __cplusplus
}
#endif

#endif
