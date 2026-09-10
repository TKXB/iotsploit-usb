#include "usbscpi_socket.h"

#include <errno.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* send() must not raise SIGPIPE when the peer vanished mid-response; on a
 * daemon that would be fatal. lwIP defines MSG_NOSIGNAL too, but guard anyway. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define USBSCPI_SOCKET_RX_BUF 512

/* The current client. serve() owns one connection at a time and usb_tx is
 * called synchronously from inside usbscpi_on_rx() on that same thread, so a
 * single file descriptor is all the state this glue needs. -1 = no client. */
static volatile int s_client_fd = -1;

int usbscpi_socket_client_connected(void) {
    return s_client_fd >= 0;
}

int usbscpi_socket_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user;
    (void)eom; /* a stream has no message boundary to signal */

    int fd = s_client_fd;
    if (fd < 0 || !data) {
        return -1;
    }

    size_t sent = 0;
    while (sent < len) {
        int n = (int)send(fd, (const char *)data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

/* Serve one accepted connection until it closes or errors. */
static void serve_client(usbscpi_t *ctx, int fd) {
    int one = 1;
    /* Without TCP_NODELAY, Nagle delays every small SCPI reply by ~40 ms. */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

    s_client_fd = fd;

    uint8_t buf[USBSCPI_SOCKET_RX_BUF];
    for (;;) {
        int n = (int)recv(fd, (char *)buf, sizeof(buf), 0);
        if (n > 0) {
            /* eom is always false: a stream carries no message boundary, and
             * usbscpi_on_rx() only uses eom to flush an *unterminated* line —
             * which over TCP would mean executing a half-received command.
             * Commands still execute on '\n' or ';' as usual. */
            (void)usbscpi_on_rx(ctx, buf, (size_t)n, false);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break; /* 0 = orderly close, <0 = error */
    }

    s_client_fd = -1;
    /* A client that died mid-block leaves MODE_BLOCK_PAYLOAD and a partial line
     * buffer behind; without this the next session inherits the corruption. */
    usbscpi_clear(ctx);
    close(fd);
}

int usbscpi_socket_serve(usbscpi_t *ctx, const char *bind_addr, uint16_t port) {
    if (!ctx || !bind_addr) {
        return -1;
    }

    int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lfd < 0) {
        return -1;
    }

    int one = 1;
    (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        close(lfd);
        return -1;
    }

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(lfd);
        return -1;
    }
    /* Backlog 1: a second client waits here rather than interleaving with the
     * first, because one usbscpi_t has one line buffer and one error queue. */
    if (listen(lfd, 1) != 0) {
        close(lfd);
        return -1;
    }

    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        serve_client(ctx, fd);
    }

    close(lfd);
    return -1;
}
