/*
 * File upload and download backend for examples/linux-pi5.
 *
 * Plugs directly into usbscpi's built-in binary block streaming callbacks
 * (DATA:WRITE #<N><len><bytes> and DATA:READ? <len> / FILE:READ? <len>) so
 * arbitrary binary files (containing NUL, CR, LF, '#') transfer cleanly in
 * chunks without dynamic allocation.
 */

#define _POSIX_C_SOURCE 200809L

#include "file_xfer.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int             s_fd   = -1;
static pi5_file_mode_t s_mode = PI5_FILE_IDLE;

int pi5_file_close(void) {
    int rc = 0;
    if (s_fd >= 0) {
        if (close(s_fd) != 0) {
            rc = -1;
        }
        s_fd = -1;
    }
    s_mode = PI5_FILE_IDLE;
    return rc;
}

void pi5_file_cleanup(void) {
    (void)pi5_file_close();
}

int pi5_file_open(const char *path, pi5_file_mode_t mode) {
    if (!path || path[0] == '\0' || mode == PI5_FILE_IDLE) {
        return -1;
    }
    (void)pi5_file_close();

    int flags = O_CLOEXEC;
    switch (mode) {
    case PI5_FILE_READ:
        flags |= O_RDONLY;
        break;
    case PI5_FILE_WRITE:
        flags |= O_WRONLY | O_CREAT | O_TRUNC;
        break;
    case PI5_FILE_APPEND:
        flags |= O_WRONLY | O_CREAT | O_APPEND;
        break;
    case PI5_FILE_IDLE:
    default:
        return -1;
    }

    int fd = open(path, flags, 0644);
    if (fd < 0) {
        return -1;
    }
    s_fd   = fd;
    s_mode = mode;
    return 0;
}

pi5_file_mode_t pi5_file_mode(void) {
    return s_mode;
}

int pi5_file_size(const char *opt_path, uint64_t *out_size) {
    if (!out_size) {
        return -1;
    }
    struct stat st;
    if (opt_path && opt_path[0] != '\0') {
        if (stat(opt_path, &st) != 0) {
            return -1;
        }
    } else if (s_fd >= 0) {
        if (fstat(s_fd, &st) != 0) {
            return -1;
        }
    } else {
        return -1;
    }
    if (st.st_size < 0) {
        return -1;
    }
    *out_size = (uint64_t)st.st_size;
    return 0;
}

int pi5_file_seek(uint64_t offset) {
    if (s_fd < 0) {
        return -1;
    }
    off_t pos = lseek(s_fd, (off_t)offset, SEEK_SET);
    return (pos == (off_t)-1) ? -1 : 0;
}

int pi5_file_delete(const char *path) {
    if (!path || path[0] == '\0') {
        return -1;
    }
    return (unlink(path) == 0) ? 0 : -1;
}

int pi5_file_crc32(const char *path, uint32_t *out_crc) {
    if (!path || path[0] == '\0' || !out_crc) {
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    uint32_t crc = 0xFFFFFFFFu;
    uint8_t  buf[512];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                crc ^= (uint32_t)buf[i];
                for (int b = 0; b < 8; b++) {
                    uint32_t mask = -(crc & 1u);
                    crc = (crc >> 1) ^ (0xEDB88320u & mask);
                }
            }
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        close(fd);
        return -1;
    }
    close(fd);
    *out_crc = ~crc;
    return 0;
}

/* ---------- usbscpi block & data callbacks ---------- */

int pi5_file_on_block_begin(void *user, size_t total_len) {
    (void)user;
    (void)total_len;
    if (s_fd < 0 || (s_mode != PI5_FILE_WRITE && s_mode != PI5_FILE_APPEND)) {
        return -1;
    }
    return 0;
}

int pi5_file_on_block_data(void *user, const uint8_t *data, size_t len) {
    (void)user;
    if (s_fd < 0 || (s_mode != PI5_FILE_WRITE && s_mode != PI5_FILE_APPEND)) {
        return -1;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(s_fd, data + off, len - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

int pi5_file_on_block_end(void *user, size_t total_len) {
    (void)user;
    (void)total_len;
    if (s_fd < 0 || (s_mode != PI5_FILE_WRITE && s_mode != PI5_FILE_APPEND)) {
        return -1;
    }
    return 0;
}

size_t pi5_file_data_avail(void *user) {
    (void)user;
    if (s_fd < 0 || s_mode != PI5_FILE_READ) {
        return 0;
    }
    struct stat st;
    if (fstat(s_fd, &st) != 0 || st.st_size < 0) {
        return 0;
    }
    off_t cur = lseek(s_fd, 0, SEEK_CUR);
    if (cur == (off_t)-1 || cur >= st.st_size) {
        return 0;
    }
    uint64_t rem = (uint64_t)(st.st_size - cur);
    if (rem > (uint64_t)SIZE_MAX) {
        return SIZE_MAX;
    }
    return (size_t)rem;
}

size_t pi5_file_data_read(void *user, uint8_t *buf, size_t len) {
    (void)user;
    if (s_fd < 0 || s_mode != PI5_FILE_READ || !buf || len == 0) {
        return 0;
    }
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(s_fd, buf + total, len - total);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    return total;
}

size_t pi5_file_data_free(void *user) {
    (void)user;
    if (s_fd < 0 || (s_mode != PI5_FILE_WRITE && s_mode != PI5_FILE_APPEND)) {
        return 0;
    }
    return PI5_FILE_MAX_BLOCK_LEN;
}
