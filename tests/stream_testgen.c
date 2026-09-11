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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Threads and sleeping, the only two things this harness needs from the OS.
 * Kept here rather than in the component: usbscpi_stream_serve() blocks by
 * design so the *application* owns its threads, which is exactly why the
 * component needs no threading abstraction of its own. */
#if defined(_WIN32)
#include <windows.h>
#define THREAD_FN(name)  static DWORD WINAPI name(LPVOID arg)
#define THREAD_RETURN    return 0
static int thread_spawn(LPTHREAD_START_ROUTINE fn) {
    HANDLE h = CreateThread(NULL, 0, fn, NULL, 0, NULL);
    if (!h) return -1;
    CloseHandle(h);
    return 0;
}
/* Sleep() has ~1 ms granularity, so sub-millisecond pacing becomes 1 ms. The
 * tests that care about rate use the unthrottled path, which never sleeps. */
static void sleep_us(unsigned us) { Sleep(us < 1000 ? 1 : (DWORD)(us / 1000)); }
#else
#include <pthread.h>
#define THREAD_FN(name)  static void *name(void *arg)
#define THREAD_RETURN    return NULL
static int thread_spawn(void *(*fn)(void *)) {
    pthread_t t;
    return pthread_create(&t, NULL, fn, NULL) == 0 ? 0 : -1;
}
static void sleep_us(unsigned us) {
    struct timespec ts = { (time_t)(us / 1000000u), (long)(us % 1000000u) * 1000L };
    nanosleep(&ts, NULL);
}
#endif

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

THREAD_FN(producer) {
    (void)arg;
    uint32_t seq = 0;
    if (s_rate == 0) {
        for (;;) {                       /* idle mode: never produce */
            sleep_us(1000000u);
        }
    }
    unsigned period_us = (unsigned)(1000000u / (unsigned)s_rate);
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
        if (period_us > 0) {
            sleep_us(period_us);
        }
    }
    THREAD_RETURN;
}

/* Reports disconnects so a test can assert realignment happened. */
THREAD_FN(reporter) {
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
        sleep_us(2000u);
    }
    THREAD_RETURN;
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

    if (thread_spawn(producer) != 0 || thread_spawn(reporter) != 0) {
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
