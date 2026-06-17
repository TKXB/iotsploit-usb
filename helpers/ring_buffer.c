#include "usbscpi/ring_buffer.h"

#include <string.h>

static int is_power_of_two(size_t value) {
    return value && ((value & (value - 1u)) == 0u);
}

int usbscpi_ring_init(usbscpi_ring_t *ring, uint8_t *storage, size_t size) {
    if (!ring || !storage || !is_power_of_two(size)) {
        return -1;
    }
    ring->buf = storage;
    ring->size = size;
    ring->head = 0;
    ring->tail = 0;
    return 0;
}

size_t usbscpi_ring_count(const usbscpi_ring_t *ring) {
    return ring ? ring->head - ring->tail : 0;
}

size_t usbscpi_ring_free(const usbscpi_ring_t *ring) {
    return ring ? ring->size - usbscpi_ring_count(ring) : 0;
}

size_t usbscpi_ring_write(usbscpi_ring_t *ring, const uint8_t *data, size_t len) {
    if (!ring || !data) {
        return 0;
    }
    size_t free_bytes = usbscpi_ring_free(ring);
    if (len > free_bytes) {
        len = free_bytes;
    }
    size_t mask = ring->size - 1u;
    for (size_t i = 0; i < len; i++) {
        ring->buf[(ring->head + i) & mask] = data[i];
    }
    ring->head += len;
    return len;
}

size_t usbscpi_ring_read(usbscpi_ring_t *ring, uint8_t *data, size_t len) {
    if (!ring || !data) {
        return 0;
    }
    size_t count = usbscpi_ring_count(ring);
    if (len > count) {
        len = count;
    }
    size_t mask = ring->size - 1u;
    for (size_t i = 0; i < len; i++) {
        data[i] = ring->buf[(ring->tail + i) & mask];
    }
    ring->tail += len;
    return len;
}

size_t usbscpi_ring_peek_linear(const usbscpi_ring_t *ring, const uint8_t **data) {
    if (!ring || !data || usbscpi_ring_count(ring) == 0) {
        if (data) {
            *data = NULL;
        }
        return 0;
    }
    size_t mask = ring->size - 1u;
    size_t index = ring->tail & mask;
    size_t count = usbscpi_ring_count(ring);
    size_t linear = ring->size - index;
    if (linear > count) {
        linear = count;
    }
    *data = &ring->buf[index];
    return linear;
}

void usbscpi_ring_advance(usbscpi_ring_t *ring, size_t len) {
    if (!ring) {
        return;
    }
    size_t count = usbscpi_ring_count(ring);
    ring->tail += len > count ? count : len;
}

void usbscpi_ring_clear(usbscpi_ring_t *ring) {
    if (ring) {
        ring->head = 0;
        ring->tail = 0;
    }
}
