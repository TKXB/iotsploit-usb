/*
 * SocketCAN capture over the iotsploit-usb data plane.
 *
 * SCPI on :5025 is the control plane — open the interface, set filters, start
 * and stop the capture, read counters. Records leave on a second socket
 * (:5026 by default) that the host discovers with SYSTem:STReam:PORT?.
 *
 * This is the FIRST protocol-specific consumer of the data plane. Everything
 * CAN-shaped lives here; glue/usbscpi_stream.c stays payload-agnostic and is
 * driven in tests by a synthetic generator with none of this in it.
 *
 * Decoding is deliberately not done here. The device ships raw frames and a
 * schema describing them; ISO-TP reassembly, UDS and OBD-II belong on the host,
 * where they can be iterated without reflashing and where the raw capture stays
 * available as evidence.
 *
 *   usbscpi_can [bind_addr] [scpi_port] [stream_port]
 */

#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "usbscpi/atomic.h"
#include "usbscpi/ring_buffer.h"
#include "usbscpi/usbscpi.h"
#include "usbscpi_socket.h"
#include "usbscpi_stream.h"

#include "can_rec.h"

/* SO_RXQ_OVFL is how the kernel reports frames it dropped before the producer
 * could read them. Without it those frames vanish between the controller and
 * this process with nothing counted, which would make the drop total a lie. */
#ifndef SO_RXQ_OVFL
#define SO_RXQ_OVFL 40
#endif
#ifndef SCM_RXQ_OVFL
#define SCM_RXQ_OVFL SO_RXQ_OVFL
#endif

/* ---------- static storage (no dynamic allocation after init) ---------- */

static uint8_t s_storage[2048];
static char    s_line[256];
static uint8_t s_io[8192];

/* 64 KiB: ~744 records, about a second of a saturated 1 Mbit/s bus. Must be a
 * power of two; it need not be a multiple of the record stride, because
 * records are allowed to wrap. */
#define RING_BYTES 65536u
static usbscpi_ring_t s_ring;
static uint8_t        s_ring_store[RING_BYTES];

/* ---------- capture state ---------- */

static int      s_can_fd = -1;
static size_t   s_running;          /* ordered: set by SCPI thread, read by producer */
static uint64_t s_dropped;          /* ring overflow + kernel SO_RXQ_OVFL */
static uint32_t s_last_kernel_ovfl;
static int      s_have_kernel_ovfl;
static uint64_t s_captured;
static char     s_ifname[IFNAMSIZ] = "";
static uint16_t s_stream_port;
static pthread_mutex_t s_fd_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- CAN producer ---------- */

static uint64_t tv_to_us(const struct timeval *tv) {
    return (uint64_t)tv->tv_sec * 1000000ull + (uint64_t)tv->tv_usec;
}

/* One frame -> one record. Returns 0 if the frame was buffered, -1 if dropped. */
static int push_record(const struct canfd_frame *f, size_t nbytes, uint64_t ts_us) {
    can_rec_t r;
    memset(&r, 0, sizeof r);
    r.ts_us   = ts_us;
    r.can_id  = f->can_id & CAN_EFF_MASK;
    r.len     = f->len > 64 ? 64 : f->len;
    r.flags   = 0;
    if (f->can_id & CAN_EFF_FLAG) r.flags |= CAN_REC_F_EFF;
    if (f->can_id & CAN_RTR_FLAG) r.flags |= CAN_REC_F_RTR;
    if (f->can_id & CAN_ERR_FLAG) r.flags |= CAN_REC_F_ERR;
    if (nbytes == CANFD_MTU) {
        r.flags |= CAN_REC_F_FD;
        if (f->flags & CANFD_BRS) r.flags |= CAN_REC_F_BRS;
        if (f->flags & CANFD_ESI) r.flags |= CAN_REC_F_ESI;
    }
    memcpy(r.data, f->data, r.len);
    /* Stamped with the total as it stands now, so the consumer sees the gap at
     * the point it happened. */
    r.dropped = usbscpi_load_acquire(&s_dropped);

    /* Capacity check first: usbscpi_ring_write() truncates to fit, and a
     * truncated record would desynchronise the consumer for every record
     * after it. Drop whole records or none. */
    if (usbscpi_ring_free(&s_ring) < sizeof r) {
        return -1;
    }
    usbscpi_ring_write(&s_ring, (const uint8_t *)&r, sizeof r);
    return 0;
}

static void *can_producer(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_fd_lock);
        int fd = s_can_fd;
        pthread_mutex_unlock(&s_fd_lock);

        if (fd < 0 || !usbscpi_load_acquire(&s_running)) {
            struct timespec ts = { 0, 10000000 }; /* 10 ms */
            nanosleep(&ts, NULL);
            continue;
        }

        struct pollfd pfd = { fd, POLLIN, 0 };
        /* Bounded so STOP and CLOSe stay responsive without a second wakeup
         * mechanism. */
        if (poll(&pfd, 1, 100) <= 0) {
            continue;
        }

        struct canfd_frame frame;
        char           ctrl[CMSG_SPACE(sizeof(struct timeval)) + CMSG_SPACE(sizeof(uint32_t))];
        struct iovec   iov = { &frame, sizeof frame };
        struct msghdr  msg;
        memset(&msg, 0, sizeof msg);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof ctrl;

        /* recvmsg, not read: the control messages are the only way to get the
         * kernel's arrival timestamp and its overflow count. */
        ssize_t n = recvmsg(fd, &msg, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        if (n != (ssize_t)CAN_MTU && n != (ssize_t)CANFD_MTU) {
            continue;
        }

        uint64_t ts_us = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level != SOL_SOCKET) continue;
            if (c->cmsg_type == SO_TIMESTAMP) {
                struct timeval tv;
                memcpy(&tv, CMSG_DATA(c), sizeof tv);
                ts_us = tv_to_us(&tv);
            } else if (c->cmsg_type == SCM_RXQ_OVFL) {
                uint32_t ovfl;
                memcpy(&ovfl, CMSG_DATA(c), sizeof ovfl);
                if (s_have_kernel_ovfl && ovfl != s_last_kernel_ovfl) {
                    /* Kernel-side loss: frames this process never saw. */
                    usbscpi_store_release(&s_dropped,
                        usbscpi_load_acquire(&s_dropped) + (ovfl - s_last_kernel_ovfl));
                }
                s_last_kernel_ovfl = ovfl;
                s_have_kernel_ovfl = 1;
            }
        }
        if (ts_us == 0) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            ts_us = (uint64_t)now.tv_sec * 1000000ull + (uint64_t)now.tv_nsec / 1000ull;
        }

        if (push_record(&frame, (size_t)n, ts_us) != 0) {
            usbscpi_store_release(&s_dropped, usbscpi_load_acquire(&s_dropped) + 1);
        } else {
            s_captured++;
        }
    }
    return NULL;
}

/* ---------- SCPI command handlers ---------- */

static scpi_result_t cmd_can_open(scpi_t *scpi) {
    const char *name = NULL;
    size_t len = 0;
    if (!SCPI_ParamCharacters(scpi, &name, &len, TRUE) || len == 0 || len >= IFNAMSIZ) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_INVALID_STRING_DATA);
        return SCPI_RES_ERR;
    }
    char ifname[IFNAMSIZ];
    memcpy(ifname, name, len);
    ifname[len] = '\0';
    /* Strip the quotes libscpi leaves on a string literal. */
    char *p = ifname;
    size_t l = strlen(p);
    if (l >= 2 && (p[0] == '"' || p[0] == '\'')) { p[l - 1] = '\0'; p++; }

    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }

    int on = 1;
    /* Best-effort: a kernel without one of these is still usable, it just
     * reports less. CAN_RAW_FD_FRAMES fails on a non-FD interface. */
    (void)setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &on, sizeof on);
    (void)setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof on);
    (void)setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof on);
    /* Error frames as records rather than as silence: bus-off and
     * error-passive are exactly what a capture is for. */
    can_err_mask_t err_mask = CAN_ERR_MASK;
    (void)setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &err_mask, sizeof err_mask);

    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, p, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        close(fd);
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof addr);
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }

    pthread_mutex_lock(&s_fd_lock);
    if (s_can_fd >= 0) close(s_can_fd);
    s_can_fd = fd;
    pthread_mutex_unlock(&s_fd_lock);
    snprintf(s_ifname, sizeof s_ifname, "%s", p);
    s_have_kernel_ovfl = 0;
    return SCPI_RES_OK;
}

static scpi_result_t cmd_can_close(scpi_t *scpi) {
    (void)scpi;
    usbscpi_store_release(&s_running, 0);
    pthread_mutex_lock(&s_fd_lock);
    if (s_can_fd >= 0) { close(s_can_fd); s_can_fd = -1; }
    pthread_mutex_unlock(&s_fd_lock);
    s_ifname[0] = '\0';
    return SCPI_RES_OK;
}

static scpi_result_t cmd_can_filter_add(scpi_t *scpi) {
    uint32_t id = 0, mask = 0;
    if (!SCPI_ParamUInt32(scpi, &id, TRUE) || !SCPI_ParamUInt32(scpi, &mask, TRUE)) {
        return SCPI_RES_ERR;
    }
    pthread_mutex_lock(&s_fd_lock);
    int fd = s_can_fd;
    int rc = -1;
    if (fd >= 0) {
        struct can_filter flt = { id, mask };
        rc = setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, &flt, sizeof flt);
    }
    pthread_mutex_unlock(&s_fd_lock);
    if (rc != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_can_send(scpi_t *scpi) {
    uint32_t id = 0;
    const char *hex = NULL;
    size_t hlen = 0;
    if (!SCPI_ParamUInt32(scpi, &id, TRUE) ||
        !SCPI_ParamCharacters(scpi, &hex, &hlen, TRUE)) {
        return SCPI_RES_ERR;
    }
    struct can_frame f;
    memset(&f, 0, sizeof f);
    f.can_id = id;
    size_t n = 0;
    for (size_t i = 0; i + 1 < hlen && n < 8; i += 2) {
        unsigned v = 0;
        if (sscanf(hex + i, "%2x", &v) != 1) break;
        f.data[n++] = (uint8_t)v;
    }
    f.can_dlc = (uint8_t)n;

    pthread_mutex_lock(&s_fd_lock);
    int fd = s_can_fd;
    ssize_t w = (fd >= 0) ? write(fd, &f, sizeof f) : -1;
    pthread_mutex_unlock(&s_fd_lock);
    if (w != (ssize_t)sizeof f) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_port(scpi_t *scpi) {
    SCPI_ResultUInt32(scpi, s_stream_port);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_start(scpi_t *scpi) {
    pthread_mutex_lock(&s_fd_lock);
    int fd = s_can_fd;
    pthread_mutex_unlock(&s_fd_lock);
    if (fd < 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    usbscpi_store_release(&s_running, 1);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_stop(scpi_t *scpi) {
    (void)scpi;
    usbscpi_store_release(&s_running, 0);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_format(scpi_t *scpi) {
    char buf[256];
    /* Self-describing, so the host parses generically instead of hardcoding
     * this layout — the same contract the workflow `fields=` spec uses. */
    snprintf(buf, sizeof buf, "ver=%u,stride=%u,fields=%s",
             CAN_REC_VERSION, CAN_REC_STRIDE, CAN_REC_FIELDS);
    SCPI_ResultCharacters(scpi, buf, strlen(buf));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_dropped(scpi_t *scpi) {
    SCPI_ResultUInt64(scpi, usbscpi_load_acquire(&s_dropped));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_torn(scpi_t *scpi) {
    SCPI_ResultUInt32(scpi, (uint32_t)usbscpi_stream_torn());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_stream_count(scpi_t *scpi) {
    SCPI_ResultUInt64(scpi, s_captured);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_can_state(scpi_t *scpi) {
    char buf[128];
    pthread_mutex_lock(&s_fd_lock);
    int open = s_can_fd >= 0;
    pthread_mutex_unlock(&s_fd_lock);
    snprintf(buf, sizeof buf, "\"%s\",%d,%d,%d",
             s_ifname[0] ? s_ifname : "", open,
             (int)(usbscpi_load_acquire(&s_running) != 0),
             usbscpi_stream_attached());
    SCPI_ResultCharacters(scpi, buf, strlen(buf));
    return SCPI_RES_OK;
}

static const scpi_command_t can_commands[] = {
    { "CAN:OPEN",                 cmd_can_open,       0 },
    { "CAN:CLOSe",                cmd_can_close,      0 },
    { "CAN:FILTer:ADD",           cmd_can_filter_add, 0 },
    { "CAN:SEND",                 cmd_can_send,       0 },
    { "CAN:STATe?",               cmd_can_state,      0 },
    { "SYSTem:STReam:PORT?",      cmd_stream_port,    0 },
    { "SYSTem:STReam:STARt",      cmd_stream_start,   0 },
    { "SYSTem:STReam:STOP",       cmd_stream_stop,    0 },
    { "SYSTem:STReam:FORMat?",    cmd_stream_format,  0 },
    { "SYSTem:STReam:DROPped?",   cmd_stream_dropped, 0 },
    { "SYSTem:STReam:TORN?",      cmd_stream_torn,    0 },
    { "SYSTem:STReam:COUNt?",     cmd_stream_count,   0 },
    SCPI_CMD_LIST_END
};

/* ---------- descriptor ---------- */

static const usbscpi_param_desc_t p_open[] = {
    { .name = "interface", .type = "string", .required = true },
};
static const usbscpi_param_desc_t p_filter[] = {
    { .name = "id",   .type = "u32", .required = true },
    { .name = "mask", .type = "u32", .required = true },
};
static const usbscpi_param_desc_t p_send[] = {
    { .name = "id",   .type = "u32",    .required = true },
    { .name = "data", .type = "string", .required = true },
};

static const usbscpi_command_desc_t desc_commands[] = {
    { "CAN:OPEN",               "command", "Bind a SocketCAN interface", p_open, 1, "none" },
    { "CAN:CLOSe",              "command", "Release the interface", NULL, 0, "none" },
    { "CAN:FILTer:ADD",         "command", "Add a raw CAN filter", p_filter, 2, "none" },
    { "CAN:SEND",               "command", "Send one classic frame", p_send, 2, "none" },
    { "CAN:STATe?",             "query",   "iface,open,running,attached", NULL, 0, "string" },
    { "SYSTem:STReam:PORT?",    "query",   "Data-plane TCP port", NULL, 0, "u32" },
    { "SYSTem:STReam:STARt",    "command", "Begin filling the ring", NULL, 0, "none" },
    { "SYSTem:STReam:STOP",     "command", "Stop filling the ring", NULL, 0, "none" },
    { "SYSTem:STReam:FORMat?",  "query",   "Record version, stride and schema", NULL, 0, "string" },
    { "SYSTem:STReam:DROPped?", "query",   "Frames lost (ring + kernel)", NULL, 0, "u32" },
    { "SYSTem:STReam:TORN?",    "query",   "Records discarded to realignment", NULL, 0, "u32" },
    { "SYSTem:STReam:COUNt?",   "query",   "Frames captured", NULL, 0, "u32" },
};

static const usbscpi_descriptor_t s_descriptor = {
    .commands = desc_commands,
    .command_count = sizeof(desc_commands) / sizeof(desc_commands[0]),
    .workflows = NULL,
    .workflow_count = 0,
};

/* ---------- threads ---------- */

static const char *s_bind_addr = "127.0.0.1";

static void *stream_thread(void *arg) {
    (void)arg;
    /* Blocks. The stride is the only thing the transport learns about CAN. */
    (void)usbscpi_stream_serve(&s_ring, s_bind_addr, s_stream_port, CAN_REC_STRIDE);
    return NULL;
}

int main(int argc, char **argv) {
    s_bind_addr          = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t scpi_port   = (argc > 2) ? (uint16_t)atoi(argv[2]) : 5025;
    s_stream_port        = (argc > 3) ? (uint16_t)atoi(argv[3]) : 5026;

    signal(SIGPIPE, SIG_IGN);

    if (usbscpi_ring_init(&s_ring, s_ring_store, sizeof s_ring_store) != 0) {
        fprintf(stderr, "ring init failed\n");
        return 1;
    }

    usbscpi_config_t cfg = {
        .usb_tx        = usbscpi_socket_tx,
        .line_buf      = s_line,
        .line_buf_len  = sizeof(s_line),
        .max_block_len = 4096,
        .idn           = "IoTSploit,can-capture,0001,0.1.0",
        .io_buf        = s_io,
        .io_buf_len    = sizeof(s_io),
        .proto         = 1,
        .mtu           = 8192,
        .descriptor    = &s_descriptor,
    };
    usbscpi_t *dev = usbscpi_init(s_storage, sizeof(s_storage), &cfg);
    if (!dev || usbscpi_register(dev, can_commands) != USBSCPI_OK) {
        fprintf(stderr, "usbscpi init/register failed\n");
        return 1;
    }

    pthread_t tp, ts;
    if (pthread_create(&ts, NULL, stream_thread, NULL) != 0 ||
        pthread_create(&tp, NULL, can_producer, NULL) != 0) {
        fprintf(stderr, "thread create failed\n");
        return 1;
    }

    printf("iotsploit-usb CAN capture: SCPI %s:%u, data plane %s:%u, stride %u\n",
           s_bind_addr, scpi_port, s_bind_addr, s_stream_port, CAN_REC_STRIDE);
    fflush(stdout);

    if (usbscpi_socket_serve(dev, s_bind_addr, scpi_port) != 0) {
        fprintf(stderr, "SCPI listen on %s:%u failed\n", s_bind_addr, scpi_port);
        return 1;
    }
    return 0;
}
