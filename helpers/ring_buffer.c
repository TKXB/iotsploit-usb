#include "usbscpi/ring_buffer.h"

#include <string.h>

#include "usbscpi/atomic.h"

/*
 * SPSC discipline, and why every load here is acquire and every store release.
 *
 * The producer publishes payload bytes and then `head`; the consumer must not
 * observe the new `head` before the bytes it covers. The mirror holds for the
 * consumer publishing `tail`. Both directions therefore need ordering, and
 * because `usbscpi_ring_count()` is called from both sides it cannot know
 * which index is "its own" — so it orders both. That is marginally stronger
 * than a hand-split producer/consumer API would need, costs nothing on x86 and
 * one cheap load-acquire on aarch64, and removes a whole class of "which side
 * is this called from" reasoning errors.
 */

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
    if (!ring) {
        return 0;
    }
    /* head - tail is correct across unsigned wraparound; the indices are
     * free-running and masked only when addressing the buffer. */
    size_t head = usbscpi_load_acquire(&ring->head);
    size_t tail = usbscpi_load_acquire(&ring->tail);
    return head - tail;
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
        /* Partial write. Correct for a byte stream; a caller shipping
         * fixed-stride records must check usbscpi_ring_free() first and drop
         * the whole record, because a truncated one desynchronises the
         * consumer for every record after it. */
        len = free_bytes;
    }
    size_t head = ring->head; /* producer-owned; no other writer */
    size_t mask = ring->size - 1u;
    for (size_t i = 0; i < len; i++) {
        ring->buf[(head + i) & mask] = data[i];
    }
    /* Release: the payload above must be visible before the index that
     * publishes it. */
    usbscpi_store_release(&ring->head, head + len);
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
    size_t tail = ring->tail; /* consumer-owned; no other writer */
    size_t mask = ring->size - 1u;
    for (size_t i = 0; i < len; i++) {
        data[i] = ring->buf[(tail + i) & mask];
    }
    usbscpi_store_release(&ring->tail, tail + len);
    return len;
}

size_t usbscpi_ring_peek_linear(const usbscpi_ring_t *ring, const uint8_t **data) {
    if (!ring || !data || usbscpi_ring_count(ring) == 0) {
        if (data) {
            *data = NULL;
        }
        return 0;
    }
    size_t count = usbscpi_ring_count(ring);
    size_t tail = usbscpi_load_acquire(&ring->tail);
    size_t mask = ring->size - 1u;
    size_t index = tail & mask;
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
    size_t tail = ring->tail; /* consumer-owned */
    usbscpi_store_release(&ring->tail, tail + (len > count ? count : len));
}

void usbscpi_ring_clear(usbscpi_ring_t *ring) {
    if (ring) {
        /* Writes both indices: quiesced use only, see the header. */
        usbscpi_store_release(&ring->head, 0);
        usbscpi_store_release(&ring->tail, 0);
    }
}
