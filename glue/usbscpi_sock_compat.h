#ifndef USBSCPI_SOCK_COMPAT_H
#define USBSCPI_SOCK_COMPAT_H

/*
 * Socket platform shims shared by the iotsploit-usb socket glues.
 *
 * The core is platform-free; this is where the socket API differences live,
 * and there are three of them: glibc, lwIP (ESP-IDF) and Winsock. Everything
 * that includes this header is written against the POSIX spelling.
 *
 * MUST BE THE FIRST INCLUDE in any translation unit that uses it — ahead of
 * the component's own headers, not just ahead of the socket headers. The
 * _WIN32_WINNT raise below only takes effect if nothing has pulled in a CRT
 * header first, and <stdint.h> (reached via usbscpi/usbscpi.h) is enough to
 * latch it on MinGW.
 */

/* inet_pton() and ws2tcpip.h are Vista+, and the MinGW CRT headers latch
 * _WIN32_WINNT to an older default the first time any of them is pulled in.
 * Raise it, but never lower a value the consumer's build system chose. */
#if defined(_WIN32)
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#endif

#if defined(_WIN32)

#include <limits.h>
#include <winsock2.h>
#include <ws2tcpip.h>

/* The Windows headers carry COM's legacy `#define interface struct`. libscpi
 * uses `interface` as a struct member and a parameter name
 * (third_party/libscpi/inc/scpi/types.h:428, scpi/parser.h:49), so any
 * translation unit that sees winsock before libscpi fails to compile. This
 * header is exactly that situation, by design — it must come first. */
#undef interface

typedef SOCKET usbscpi_sock_t;
#define USBSCPI_SOCK_INVALID INVALID_SOCKET
#define usbscpi_closesocket  closesocket

/* Winsock blocking calls do not return WSAEINTR outside of the long-removed
 * WSACancelBlockingCall, so the POSIX retry-on-EINTR branches are dead here. */
static inline int usbscpi_sock_interrupted(void) { return 0; }

/* WSAPoll is Vista+. It is unreliable for connect() progress, but this
 * component only ever asks it about readability, writability and EOF on an
 * established socket, which it handles correctly. */
typedef WSAPOLLFD usbscpi_pollfd_t;
static inline int usbscpi_poll(usbscpi_pollfd_t *fds, unsigned n, int ms) {
    return WSAPoll(fds, n, ms);
}
static inline int usbscpi_sock_set_nonblocking(usbscpi_sock_t fd) {
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on) == 0 ? 0 : -1;
}
static inline int usbscpi_sock_would_block(void) {
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

static inline int usbscpi_sock_startup(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
}
static inline void usbscpi_sock_cleanup(void) { WSACleanup(); }

#else /* POSIX / lwIP */

#include <errno.h>
#include <fcntl.h>
#ifdef ESP_PLATFORM
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#else
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

typedef int usbscpi_sock_t;
#define USBSCPI_SOCK_INVALID (-1)
#define usbscpi_closesocket  close

static inline int  usbscpi_sock_interrupted(void) { return errno == EINTR; }
static inline int  usbscpi_sock_startup(void)     { return 0; }
static inline void usbscpi_sock_cleanup(void)     { }

typedef struct pollfd usbscpi_pollfd_t;
static inline int usbscpi_poll(usbscpi_pollfd_t *fds, unsigned n, int ms) {
    return poll(fds, (nfds_t)n, ms);
}
static inline int usbscpi_sock_set_nonblocking(usbscpi_sock_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
}
static inline int usbscpi_sock_would_block(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

#endif

/* send() must not raise SIGPIPE when the peer vanished mid-write; on a daemon
 * that would be fatal. lwIP defines MSG_NOSIGNAL too, but guard anyway.
 * Winsock has no SIGPIPE and no such flag. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#endif /* USBSCPI_SOCK_COMPAT_H */
