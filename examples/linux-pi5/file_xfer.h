#ifndef LINUX_PI5_FILE_XFER_H
#define LINUX_PI5_FILE_XFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum single DATA:WRITE block size accepted by the server (64 KiB).
 * Larger files are uploaded by sending multiple DATA:WRITE blocks while the
 * file is open in WRITE or APPEND mode. */
#define PI5_FILE_MAX_BLOCK_LEN 65536u

typedef enum {
    PI5_FILE_IDLE = 0,
    PI5_FILE_READ,
    PI5_FILE_WRITE,
    PI5_FILE_APPEND,
} pi5_file_mode_t;

/* Open `path` in READ, WRITE (create/truncate), or APPEND (create/append) mode.
 * Closes any previously open file first. Returns 0 on success, -1 on error. */
int pi5_file_open(const char *path, pi5_file_mode_t mode);

/* Close the currently open file (if any). Returns 0 on success, -1 on close error. */
int pi5_file_close(void);

/* Close any open file on server shutdown. */
void pi5_file_cleanup(void);

/* Current file transfer mode (IDLE, READ, WRITE, APPEND). */
pi5_file_mode_t pi5_file_mode(void);

/* File size in bytes. If `opt_path` is non-NULL and non-empty, stats that path;
 * otherwise stats the currently open file. Returns 0 on success, -1 on error. */
int pi5_file_size(const char *opt_path, uint64_t *out_size);

/* Seek to byte `offset` in the currently open file. Returns 0 on success. */
int pi5_file_seek(uint64_t offset);

/* Remove a file via unlink(). Returns 0 on success, -1 on error. */
int pi5_file_delete(const char *path);

/* Compute standard IEEE 802.3 CRC-32 of `path` without external libraries.
 * Returns 0 on success, -1 if the file could not be read. */
int pi5_file_crc32(const char *path, uint32_t *out_crc);

/* usbscpi_config_t block / data callbacks for DATA:WRITE, DATA:READ?,
 * DATA:COUNt?, DATA:FREE?, and FILE:READ?. */
int    pi5_file_on_block_begin(void *user, size_t total_len);
int    pi5_file_on_block_data(void *user, const uint8_t *data, size_t len);
int    pi5_file_on_block_end(void *user, size_t total_len);
size_t pi5_file_data_avail(void *user);
size_t pi5_file_data_read(void *user, uint8_t *buf, size_t len);
size_t pi5_file_data_free(void *user);

#ifdef __cplusplus
}
#endif

#endif /* LINUX_PI5_FILE_XFER_H */
