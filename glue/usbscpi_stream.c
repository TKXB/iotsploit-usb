/* usbscpi_sock_compat.h must come first: it raises _WIN32_WINNT, which is
 * only effective ahead of every other include. */
#include "usbscpi_sock_compat.h"

#include "usbscpi_stream.h"

#include "usbscpi/atomic.h"

#include <string.h>

/* How long poll() waits when there is nothing to do. This is the worst-case
 * added latency for a record, and the reason an idle stream costs one wakeup
 * per interval instead of a spin. Turn it down if a consumer needs tighter. */
#define USBSCPI_STREAM_POLL_MS 5

/* The consumer is not expected to send anything. Readability means either
 * stray bytes to discard or EOF, and this is only big enough to tell them
 * apart. */
#define USBSCPI_STREAM_SINK_BUF 64

/* One consumer at a time on one thread, so file-local state is all that is
 * needed — the same shape as usbscpi_socket.c. Both are read from other
 * threads, hence the ordered accessors. */
static size_t s_attached;
static size_t s_torn;

int usbscpi_stream_attached(void) {
    return usbscpi_load_acquire(&s_attached) != 0;
}

size_t usbscpi_stream_torn(void) {
    return usbscpi_load_acquire(&s_torn);
}

/* Discard the remainder of a record that was only partly sent before the
 * consumer vanished.
 *
 * The producer writes whole records, so the ring head is always a multiple of
 * `stride`. The tail is not, once a partial send has advanced it, and the next
 * consumer would then start mid-record and stay misaligned forever — the glue
 * cannot resynchronise it, because it cannot see record boundaries.
 *
 * count == head - tail, and head % stride == 0, so count % stride is exactly
 * how far the tail must advance to reach the next boundary. No access to the
 * tail itself is needed. */
static void realign(usbscpi_ring_t *ring, size_t stride) {
    if (stride <= 1) {
        return; /* a byte stream has no alignment to lose */
    }
    size_t rem = usbscpi_ring_count(ring) % stride;
    if (rem != 0) {
        usbscpi_ring_advance(ring, rem);
        usbscpi_store_release(&s_torn, usbscpi_load_acquire(&s_torn) + 1);
    }
}

/* Returns 0 if the consumer is still healthy, -1 if it went away.
 * `*want_write` tracks whether the socket owes us a POLLOUT. */
static int drain(usbscpi_ring_t *ring, usbscpi_sock_t fd, int *want_write) {
    for (;;) {
        const uint8_t *chunk = NULL;
        size_t n = usbscpi_ring_peek_linear(ring, &chunk);
        if (n == 0 || !chunk) {
            *want_write = 0; /* ring drained */
            return 0;
        }
#if defined(_WIN32)
        if (n > (size_t)INT_MAX) {
            n = (size_t)INT_MAX;
        }
#endif
        int sent = (int)send(fd, (const char *)chunk, (int)n, MSG_NOSIGNAL);
        if (sent > 0) {
            /* Advance only by what the socket actually took. A short write is
             * normal and leaves the rest for the next pass. */
            usbscpi_ring_advance(ring, (size_t)sent);
            continue;
        }
        if (sent < 0 && usbscpi_sock_would_block()) {
            /* Consumer is behind. Stop draining and let the ring absorb the
             * slack; the producer drops whole records once it fills. Never
             * block here — blocking would stall the producer and move the loss
             * somewhere it cannot be counted. */
            *want_write = 1;
            return 0;
        }
        if (sent < 0 && usbscpi_sock_interrupted()) {
            continue;
        }
        return -1;
    }
}

static void serve_consumer(usbscpi_ring_t *ring, usbscpi_sock_t fd, size_t stride) {
    int one = 1;
    /* Without TCP_NODELAY, Nagle holds small record batches for ~40 ms. */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    if (usbscpi_sock_set_nonblocking(fd) != 0) {
        usbscpi_closesocket(fd);
        return;
    }

    usbscpi_store_release(&s_attached, 1);

    int want_write = 0;
    for (;;) {
        usbscpi_pollfd_t pfd;
        pfd.fd = fd;
        /* POLLIN is always requested, even with nothing to send: it is how a
         * FIN is noticed while the stream is idle, rather than at whatever
         * later moment the next record happens to arrive. */
        pfd.events = (short)(POLLIN | (want_write ? POLLOUT : 0));
        pfd.revents = 0;

        int rc = usbscpi_poll(&pfd, 1, USBSCPI_STREAM_POLL_MS);
        if (rc < 0) {
            if (usbscpi_sock_interrupted()) {
                continue;
            }
            break;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }

        if (pfd.revents & POLLIN) {
            char sink[USBSCPI_STREAM_SINK_BUF];
            int n = (int)recv(fd, sink, (int)sizeof(sink), 0);
            if (n == 0) {
                break; /* orderly close */
            }
            if (n < 0 && !usbscpi_sock_would_block() && !usbscpi_sock_interrupted()) {
                break;
            }
            /* Anything the consumer sent is discarded: this channel carries
             * records one way. Control belongs on the SCPI socket. */
        }

        if (drain(ring, fd, &want_write) != 0) {
            break;
        }
    }

    /* Realign before publishing the detach, not after: anything watching
     * usbscpi_stream_attached() to know a session ended would otherwise be
     * able to read usbscpi_stream_torn() in the window before it is updated,
     * and see the previous session's count. */
    realign(ring, stride);
    usbscpi_store_release(&s_attached, 0);
    usbscpi_closesocket(fd);
}

int usbscpi_stream_serve(usbscpi_ring_t *ring, const char *bind_addr,
                         uint16_t port, size_t stride) {
    if (!ring || !bind_addr || stride == 0) {
        return -1;
    }
    if (usbscpi_sock_startup() != 0) {
        return -1;
    }

    usbscpi_sock_t lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lfd == USBSCPI_SOCK_INVALID) {
        usbscpi_sock_cleanup();
        return -1;
    }

    int one = 1;
#if defined(_WIN32)
    /* Windows SO_REUSEADDR is not the POSIX one: it lets an unrelated process
     * bind this same live port and steal connections. */
    (void)setsockopt(lfd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof(one));
#else
    (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        goto done;
    }
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        goto done;
    }
    /* Backlog 1: a second consumer waits rather than interleaving, because one
     * ring has one tail. */
    if (listen(lfd, 1) != 0) {
        goto done;
    }

    for (;;) {
        usbscpi_sock_t fd = accept(lfd, NULL, NULL);
        if (fd == USBSCPI_SOCK_INVALID) {
            if (usbscpi_sock_interrupted()) {
                continue;
            }
            break;
        }
        serve_consumer(ring, fd, stride);
    }

done:
    usbscpi_closesocket(lfd);
    usbscpi_sock_cleanup();
    return -1;
}
