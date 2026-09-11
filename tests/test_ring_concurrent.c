/*
 * SPSC ring across a real thread boundary.
 *
 * The ring shipped with plain `volatile` indices and was exercised only by
 * single-threaded unit tests, so its memory ordering had never been run. This
 * test exists to fail if the acquire/release pairing in helpers/ring_buffer.c
 * is removed or weakened — under ThreadSanitizer it reports the race directly,
 * and on a weakly-ordered CPU it can catch a torn record even without TSan.
 *
 * Build with -fsanitize=thread for the strong version:
 *   gcc -fsanitize=thread -I include tests/test_ring_concurrent.c \
 *       helpers/ring_buffer.c -lpthread
 */

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "usbscpi/atomic.h"
#include "usbscpi/ring_buffer.h"

#define RECORDS   200000u
#define RING_SIZE 1024u          /* small on purpose: forces wrap and overflow */
#define MAGIC     0xA5C3F00Du

/* Self-checking record: `seq` is repeated so a torn write is detectable, and
 * `magic` catches a misaligned read that happens to look plausible. */
typedef struct {
    uint32_t seq;
    uint32_t magic;
    uint32_t seq_again;
    uint32_t pad;
} rec_t;

static usbscpi_ring_t s_ring;
static uint8_t        s_store[RING_SIZE];

static uint32_t s_produced;
static uint32_t s_dropped;   /* producer-side: ring was full */
static uint32_t s_consumed;
static uint32_t s_last_seq;
static int      s_torn;
static int      s_backwards;

/* The consumer needs to know when the producer has finished, and reading the
 * producer's counters to work it out would itself be a data race — the thing
 * this test is meant to detect. One ordered flag instead. */
static size_t s_done;

static void *producer(void *arg) {
    (void)arg;
    for (uint32_t i = 0; i < RECORDS; i++) {
        rec_t r = { i, MAGIC, i, 0 };
        /* Capacity check before the write: usbscpi_ring_write() truncates, and
         * a truncated record would desynchronise every record after it. */
        if (usbscpi_ring_free(&s_ring) < sizeof r) {
            s_dropped++;
        } else {
            size_t n = usbscpi_ring_write(&s_ring, (const uint8_t *)&r, sizeof r);
            assert(n == sizeof r);
            s_produced++;
        }
    }
    usbscpi_store_release(&s_done, 1);
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    uint32_t seen = 0;
    uint32_t batches = 0;
    for (;;) {
        size_t avail = usbscpi_ring_count(&s_ring);
        size_t whole = (avail / sizeof(rec_t)) * sizeof(rec_t);  /* stride-aligned only */
        if (whole == 0) {
            /* Drain fully before believing the producer is done. */
            if (usbscpi_load_acquire(&s_done)) break;
            continue;
        }
        /* Stall periodically so the ring actually fills. Without this the
         * consumer keeps up trivially and the full boundary — where an
         * ordering bug is most likely to show — never gets exercised. */
        if (++batches % 512u == 0u) {
            struct timespec ts = { 0, 20000 };   /* 20 us */
            nanosleep(&ts, NULL);
        }
        rec_t batch[16];
        if (whole > sizeof batch) whole = sizeof batch;
        size_t got = usbscpi_ring_read(&s_ring, (uint8_t *)batch, whole);
        assert(got % sizeof(rec_t) == 0);
        for (size_t i = 0; i < got / sizeof(rec_t); i++) {
            rec_t *r = &batch[i];
            if (r->magic != MAGIC || r->seq != r->seq_again) {
                s_torn++;               /* payload visible out of order, or torn */
            } else if (seen > 0 && r->seq <= s_last_seq) {
                s_backwards++;          /* ordering violation */
            }
            s_last_seq = r->seq;
            seen++;
        }
    }
    s_consumed = seen;
    return NULL;
}

int main(void) {
    assert(usbscpi_ring_init(&s_ring, s_store, sizeof s_store) == 0);

    pthread_t tp, tc;
    assert(pthread_create(&tc, NULL, consumer, NULL) == 0);
    assert(pthread_create(&tp, NULL, producer, NULL) == 0);
    assert(pthread_join(tp, NULL) == 0);
    assert(pthread_join(tc, NULL) == 0);

    printf("produced=%u dropped=%u consumed=%u torn=%d backwards=%d\n",
           s_produced, s_dropped, s_consumed, s_torn, s_backwards);

    assert(s_torn == 0);
    assert(s_backwards == 0);
    assert(s_produced + s_dropped == RECORDS);
    assert(s_consumed == s_produced);
    /* A run with no drops would mean the consumer kept up trivially and the
     * full/empty boundaries never got exercised. */
    assert(s_dropped > 0);

    printf("test_ring_concurrent: PASS\n");
    return 0;
}
