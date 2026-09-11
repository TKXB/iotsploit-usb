/*
 * Synthetic producer for the data-plane glue.
 *
 * Deliberately knows nothing about CAN, or about any real source. If
 * glue/usbscpi_stream.c ever needs protocol knowledge to work, this harness
 * stops being able to exercise it — which is the point. The first
 * protocol-specific consumer does not appear until a later stage.
 *
 *   stream_testgen <bind_addr> <port> <records_per_sec>
 *
 * A rate of 0 produces nothing, for testing idle behaviour. Prints one line
 * per consumer disconnect so a test can assert on realignment and drops.
 */

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "usbscpi/atomic.h"
#include "usbscpi/ring_buffer.h"
#include "usbscpi_stream.h"

#define RING_SIZE 4096u              /* 256 records: small enough to overflow */
#define MAGIC     0x5EC0FFEEu

/* Self-checking: `seq` is repeated so a torn or misaligned read is detectable
 * by the consumer without trusting the transport.
 *
 * 20 bytes, deliberately not a power of two. Socket buffer space comes in
 * round binary sizes, so a partial send() tends to land on a 16-byte boundary
 * — with a 16-byte record the ring tail would stay accidentally aligned and
 * the realignment path would never be exercised. A stride that shares no
 * factor with those sizes forces the case the glue exists to handle. */
typedef struct {
    uint32_t seq;
    uint32_t magic;
    uint32_t seq_again;
    uint32_t pad;
    uint32_t pad2;
} rec_t;

static usbscpi_ring_t s_ring;
static uint8_t        s_store[RING_SIZE];
static size_t         s_dropped;     /* producer-side: ring was full */
static size_t         s_rate;

static void *producer(void *arg) {
    (void)arg;
    uint32_t seq = 0;
    if (s_rate == 0) {
        for (;;) {                       /* idle mode: never produce */
            struct timespec ts = { 1, 0 };
            nanosleep(&ts, NULL);
        }
    }
    long period_ns = (long)(1000000000L / (long)s_rate);
    for (;;) {
        rec_t r = { seq, MAGIC, seq, 0, 0 };
        /* Capacity check first: usbscpi_ring_write() truncates, and a
         * truncated record would desynchronise the consumer permanently. */
        if (usbscpi_ring_free(&s_ring) < sizeof r) {
            usbscpi_store_release(&s_dropped, usbscpi_load_acquire(&s_dropped) + 1);
        } else {
            usbscpi_ring_write(&s_ring, (const uint8_t *)&r, sizeof r);
        }
        seq++;
        if (period_ns > 200) {
            struct timespec ts = { 0, period_ns };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

/* Reports disconnects so a test can assert realignment happened. */
static void *reporter(void *arg) {
    (void)arg;
    int was = 0;
    for (;;) {
        int now = usbscpi_stream_attached();
        if (was && !now) {
            printf("disconnect torn=%zu dropped=%zu\n",
                   usbscpi_stream_torn(), usbscpi_load_acquire(&s_dropped));
            fflush(stdout);
        }
        was = now;
        struct timespec ts = { 0, 2000000 }; /* 2 ms */
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bind_addr> <port> <records_per_sec>\n", argv[0]);
        return 2;
    }
    const char *bind_addr = argv[1];
    uint16_t    port      = (uint16_t)atoi(argv[2]);
    s_rate                = (size_t)strtoul(argv[3], NULL, 10);

    if (usbscpi_ring_init(&s_ring, s_store, sizeof s_store) != 0) {
        fprintf(stderr, "ring init failed\n");
        return 1;
    }

    pthread_t tp, tr;
    if (pthread_create(&tp, NULL, producer, NULL) != 0 ||
        pthread_create(&tr, NULL, reporter, NULL) != 0) {
        fprintf(stderr, "thread create failed\n");
        return 1;
    }

    printf("stream_testgen listening on %s:%u stride=%zu rate=%zu\n",
           bind_addr, port, sizeof(rec_t), s_rate);
    fflush(stdout);

    if (usbscpi_stream_serve(&s_ring, bind_addr, port, sizeof(rec_t)) != 0) {
        fprintf(stderr, "stream listen on %s:%u failed\n", bind_addr, port);
        return 1;
    }
    return 0;
}
