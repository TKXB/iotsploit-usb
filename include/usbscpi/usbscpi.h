#ifndef USBSCPI_USBSCPI_H
#define USBSCPI_USBSCPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scpi/scpi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USBSCPI_VERSION_MAJOR 0
#define USBSCPI_VERSION_MINOR 1
#define USBSCPI_VERSION_PATCH 0

typedef struct usbscpi usbscpi_t;

typedef int (*usbscpi_usb_tx_t)(void *user, const uint8_t *data, size_t len, bool eom);
typedef int (*usbscpi_block_begin_t)(void *user, size_t total_len);
typedef int (*usbscpi_block_data_t)(void *user, const uint8_t *data, size_t len);
typedef int (*usbscpi_block_end_t)(void *user, size_t total_len);
typedef size_t (*usbscpi_data_free_t)(void *user);
typedef void (*usbscpi_event_t)(void *user);
typedef void (*usbscpi_lock_t)(void *user);

typedef struct {
    usbscpi_usb_tx_t usb_tx;
    usbscpi_block_begin_t on_block_begin;
    usbscpi_block_data_t on_block_data;
    usbscpi_block_end_t on_block_end;
    usbscpi_data_free_t data_free;
    usbscpi_event_t on_reset;
    usbscpi_lock_t lock;
    usbscpi_lock_t unlock;
    void *user;

    char *line_buf;
    size_t line_buf_len;
    size_t max_block_len;
    const char *idn;
} usbscpi_config_t;

typedef enum {
    USBSCPI_OK = 0,
    USBSCPI_ERR_ARG = -1,
    USBSCPI_ERR_STORAGE = -2,
    USBSCPI_ERR_OVERFLOW = -3,
    USBSCPI_ERR_PROTOCOL = -4,
    USBSCPI_ERR_CALLBACK = -5
} usbscpi_status_t;

size_t usbscpi_sizeof(void);
usbscpi_t *usbscpi_init(void *storage, size_t storage_len, const usbscpi_config_t *cfg);
int usbscpi_register(usbscpi_t *ctx, const scpi_command_t *commands);
int usbscpi_on_rx(usbscpi_t *ctx, const void *data, size_t len, bool eom);
void usbscpi_task(usbscpi_t *ctx);
void usbscpi_clear(usbscpi_t *ctx);
scpi_t *usbscpi_scpi(usbscpi_t *ctx);

#ifdef __cplusplus
}
#endif

#endif
