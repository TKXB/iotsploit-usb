/* usbscpi_sock_compat.h must come first: it raises _WIN32_WINNT, which is
 * only effective ahead of every other include. */
#include "usbscpi_sock_compat.h"

#include "usbscpi_socket.h"

#include <string.h>

#define USBSCPI_SOCKET_RX_BUF 512

/* The current client. serve() owns one connection at a time and usb_tx is
 * called synchronously from inside usbscpi_on_rx() on that same thread, so a
 * single socket handle is all the state this glue needs. */
static volatile usbscpi_sock_t s_client_fd = USBSCPI_SOCK_INVALID;

int usbscpi_socket_client_connected(void) {
    return s_client_fd != USBSCPI_SOCK_INVALID;
}

int usbscpi_socket_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user;
    (void)eom; /* a stream has no message boundary to signal */

    usbscpi_sock_t fd = s_client_fd;
    if (fd == USBSCPI_SOCK_INVALID || !data) {
        return -1;
    }

    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
#if defined(_WIN32)
        /* Winsock takes an int length, unlike POSIX's size_t. */
        if (chunk > (size_t)INT_MAX) {
            chunk = (size_t)INT_MAX;
        }
#endif
        int n = (int)send(fd, (const char *)data + sent, (int)chunk, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && usbscpi_sock_interrupted()) {
            continue;
        }
        return -1;
    }
    return 0;
}

/* Serve one accepted connection until it closes or errors. */
static void serve_client(usbscpi_t *ctx, usbscpi_sock_t fd) {
    int one = 1;
    /* Without TCP_NODELAY, Nagle delays every small SCPI reply by ~40 ms. */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

    s_client_fd = fd;

    uint8_t buf[USBSCPI_SOCKET_RX_BUF];
    for (;;) {
        int n = (int)recv(fd, (char *)buf, (int)sizeof(buf), 0);
        if (n > 0) {
            /* eom is always false: a stream carries no message boundary, and
             * usbscpi_on_rx() only uses eom to flush an *unterminated* line —
             * which over TCP would mean executing a half-received command.
             * Commands still execute on '\n' or ';' as usual. */
            (void)usbscpi_on_rx(ctx, buf, (size_t)n, false);
            continue;
        }
        if (n < 0 && usbscpi_sock_interrupted()) {
            continue;
        }
        break; /* 0 = orderly close, <0 = error */
    }

    s_client_fd = USBSCPI_SOCK_INVALID;
    /* A client that died mid-block leaves MODE_BLOCK_PAYLOAD and a partial line
     * buffer behind; without this the next session inherits the corruption. */
    usbscpi_clear(ctx);
    usbscpi_closesocket(fd);
}

int usbscpi_socket_serve(usbscpi_t *ctx, const char *bind_addr, uint16_t port) {
    if (!ctx || !bind_addr) {
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
     * bind this same live port and steal connections. For a listener exposing
     * the whole SCPI surface that is a hijack, so ask for the opposite. */
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
    /* Backlog 1: a second client waits here rather than interleaving with the
     * first, because one usbscpi_t has one line buffer and one error queue. */
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
        serve_client(ctx, fd);
    }

done:
    usbscpi_closesocket(lfd);
    usbscpi_sock_cleanup();
    return -1;
}
