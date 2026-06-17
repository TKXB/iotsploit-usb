#include "usbscpi/usbscpi.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    MODE_TEXT = 0,
    MODE_BLOCK_DIGITS,
    MODE_BLOCK_LEN,
    MODE_BLOCK_PAYLOAD
} rx_mode_t;

struct usbscpi {
    usbscpi_config_t cfg;
    scpi_t scpi;
    rx_mode_t mode;
    size_t line_len;
    size_t block_len;
    size_t block_received;
    unsigned ndigits;
    unsigned got_digits;
    int block_started;
};

static int scpi_write_adapter(void *user, const char *data, size_t len) {
    usbscpi_t *ctx = (usbscpi_t *)user;
    if (!ctx || !ctx->cfg.usb_tx) {
        return SCPI_RES_ERR;
    }
    return ctx->cfg.usb_tx(ctx->cfg.user, (const uint8_t *)data, len, true);
}

static usbscpi_t *scpi_owner(scpi_t *scpi) {
    return scpi ? (usbscpi_t *)scpi->user_context : NULL;
}

static scpi_result_t cmd_idn(scpi_t *scpi) {
    usbscpi_t *ctx = scpi_owner(scpi);
    return SCPI_ResultText(scpi, (ctx && ctx->cfg.idn) ? ctx->cfg.idn : "usbscpi,component,0,0.1.0");
}

static scpi_result_t cmd_rst(scpi_t *scpi) {
    usbscpi_t *ctx = scpi_owner(scpi);
    if (ctx && ctx->cfg.on_reset) {
        ctx->cfg.on_reset(ctx->cfg.user);
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_cls(scpi_t *scpi) {
    SCPI_ErrorClear(scpi);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_opc(scpi_t *scpi) {
    return SCPI_ResultUInt32(scpi, 1);
}

static scpi_result_t cmd_syst_err(scpi_t *scpi) {
    char buf[80];
    scpi_error_t err;
    if (!SCPI_ErrorPop(scpi, &err)) {
        return SCPI_ResultText(scpi, "0,\"No error\"");
    }
    snprintf(buf, sizeof(buf), "%d,\"%s\"", err.code, err.message);
    return SCPI_ResultText(scpi, buf);
}

static scpi_result_t cmd_data_free(scpi_t *scpi) {
    usbscpi_t *ctx = scpi_owner(scpi);
    size_t free_bytes = 0;
    if (ctx && ctx->cfg.data_free) {
        free_bytes = ctx->cfg.data_free(ctx->cfg.user);
    }
    return SCPI_ResultUInt32(scpi, (uint32_t)free_bytes);
}

static const scpi_command_t default_commands[] = {
    { "*IDN?", cmd_idn, 0 },
    { "*RST", cmd_rst, 0 },
    { "*CLS", cmd_cls, 0 },
    { "*OPC?", cmd_opc, 0 },
    { "SYSTem:ERRor?", cmd_syst_err, 0 },
    { "DATA:FREE?", cmd_data_free, 0 },
    SCPI_CMD_LIST_END
};

static void lock_ctx(usbscpi_t *ctx) {
    if (ctx && ctx->cfg.lock) {
        ctx->cfg.lock(ctx->cfg.user);
    }
}

static void unlock_ctx(usbscpi_t *ctx) {
    if (ctx && ctx->cfg.unlock) {
        ctx->cfg.unlock(ctx->cfg.user);
    }
}

static int line_is_data_write_prefix(const char *line, size_t len) {
    static const char prefix1[] = ":DATA:WRITE ";
    static const char prefix2[] = "DATA:WRITE ";
    while (len && isspace((unsigned char)line[0])) {
        line++;
        len--;
    }
    if (len != sizeof(prefix1) - 1 && len != sizeof(prefix2) - 1) {
        return 0;
    }
    const char *prefix = (len == sizeof(prefix1) - 1) ? prefix1 : prefix2;
    for (size_t i = 0; i < len; i++) {
        if (toupper((unsigned char)line[i]) != prefix[i]) {
            return 0;
        }
    }
    return 1;
}

static int execute_pending_line(usbscpi_t *ctx) {
    if (ctx->line_len == 0) {
        return USBSCPI_OK;
    }
    if (ctx->line_len + 1 > ctx->cfg.line_buf_len) {
        ctx->line_len = 0;
        SCPI_ErrorPush(&ctx->scpi, -200, "Line overflow");
        return USBSCPI_ERR_OVERFLOW;
    }
    ctx->cfg.line_buf[ctx->line_len++] = '\n';
    int rc = SCPI_Input(&ctx->scpi, ctx->cfg.line_buf, ctx->line_len);
    ctx->line_len = 0;
    return rc == SCPI_RES_OK ? USBSCPI_OK : USBSCPI_ERR_PROTOCOL;
}

static int block_begin(usbscpi_t *ctx) {
    if (ctx->block_started) {
        return USBSCPI_OK;
    }
    if (ctx->cfg.max_block_len && ctx->block_len > ctx->cfg.max_block_len) {
        SCPI_ErrorPush(&ctx->scpi, -223, "Block too large");
        return USBSCPI_ERR_OVERFLOW;
    }
    ctx->block_started = 1;
    if (ctx->cfg.on_block_begin && ctx->cfg.on_block_begin(ctx->cfg.user, ctx->block_len) != 0) {
        SCPI_ErrorPush(&ctx->scpi, -200, "Block begin rejected");
        return USBSCPI_ERR_CALLBACK;
    }
    return USBSCPI_OK;
}

static int block_finish(usbscpi_t *ctx) {
    if (ctx->cfg.on_block_end && ctx->cfg.on_block_end(ctx->cfg.user, ctx->block_len) != 0) {
        SCPI_ErrorPush(&ctx->scpi, -200, "Block end rejected");
        return USBSCPI_ERR_CALLBACK;
    }
    ctx->mode = MODE_TEXT;
    ctx->line_len = 0;
    ctx->block_len = 0;
    ctx->block_received = 0;
    ctx->ndigits = 0;
    ctx->got_digits = 0;
    ctx->block_started = 0;
    return USBSCPI_OK;
}

size_t usbscpi_sizeof(void) {
    return sizeof(usbscpi_t);
}

usbscpi_t *usbscpi_init(void *storage, size_t storage_len, const usbscpi_config_t *cfg) {
    if (!storage || storage_len < sizeof(usbscpi_t) || !cfg || !cfg->usb_tx ||
        !cfg->line_buf || cfg->line_buf_len < 16) {
        return NULL;
    }

    usbscpi_t *ctx = (usbscpi_t *)storage;
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = *cfg;
    SCPI_Init(&ctx->scpi, scpi_write_adapter, ctx);
    ctx->scpi.user_context = ctx;
    SCPI_RegisterCommands(&ctx->scpi, default_commands);
    return ctx;
}

int usbscpi_register(usbscpi_t *ctx, const scpi_command_t *commands) {
    if (!ctx || !commands) {
        return USBSCPI_ERR_ARG;
    }
    return SCPI_RegisterCommands(&ctx->scpi, commands);
}

int usbscpi_on_rx(usbscpi_t *ctx, const void *data, size_t len, bool eom) {
    if (!ctx || (!data && len)) {
        return USBSCPI_ERR_ARG;
    }
    const uint8_t *p = (const uint8_t *)data;
    int status = USBSCPI_OK;

    lock_ctx(ctx);
    for (size_t i = 0; i < len && status == USBSCPI_OK;) {
        uint8_t c = p[i];
        switch (ctx->mode) {
        case MODE_TEXT:
            i++;
            if (c == '#') {
                if (line_is_data_write_prefix(ctx->cfg.line_buf, ctx->line_len)) {
                    ctx->mode = MODE_BLOCK_DIGITS;
                    ctx->line_len = 0;
                    break;
                }
            }
            if (c == '\r') {
                break;
            }
            if (c == '\n' || c == ';') {
                ctx->cfg.line_buf[ctx->line_len++] = '\n';
                if (SCPI_Input(&ctx->scpi, ctx->cfg.line_buf, ctx->line_len) != SCPI_RES_OK) {
                    status = USBSCPI_ERR_PROTOCOL;
                }
                ctx->line_len = 0;
                break;
            }
            if (ctx->line_len + 1 >= ctx->cfg.line_buf_len) {
                ctx->line_len = 0;
                SCPI_ErrorPush(&ctx->scpi, -200, "Line overflow");
                status = USBSCPI_ERR_OVERFLOW;
                break;
            }
            ctx->cfg.line_buf[ctx->line_len++] = (char)c;
            break;

        case MODE_BLOCK_DIGITS:
            i++;
            if (c < '1' || c > '9') {
                SCPI_ErrorPush(&ctx->scpi, -161, "Invalid block header");
                status = USBSCPI_ERR_PROTOCOL;
                break;
            }
            ctx->ndigits = (unsigned)(c - '0');
            ctx->got_digits = 0;
            ctx->block_len = 0;
            ctx->block_received = 0;
            ctx->mode = MODE_BLOCK_LEN;
            break;

        case MODE_BLOCK_LEN:
            i++;
            if (!isdigit((unsigned char)c)) {
                SCPI_ErrorPush(&ctx->scpi, -161, "Invalid block length");
                status = USBSCPI_ERR_PROTOCOL;
                break;
            }
            ctx->block_len = ctx->block_len * 10u + (size_t)(c - '0');
            if (++ctx->got_digits == ctx->ndigits) {
                status = block_begin(ctx);
                ctx->mode = MODE_BLOCK_PAYLOAD;
                if (ctx->block_len == 0 && status == USBSCPI_OK) {
                    status = block_finish(ctx);
                }
            }
            break;

        case MODE_BLOCK_PAYLOAD: {
            if (ctx->block_received == ctx->block_len) {
                status = block_finish(ctx);
                break;
            }
            size_t remaining = ctx->block_len - ctx->block_received;
            size_t available = len - i;
            size_t chunk = remaining < available ? remaining : available;
            if (chunk && ctx->cfg.on_block_data &&
                ctx->cfg.on_block_data(ctx->cfg.user, p + i, chunk) != 0) {
                SCPI_ErrorPush(&ctx->scpi, -200, "Block data rejected");
                status = USBSCPI_ERR_CALLBACK;
                break;
            }
            i += chunk;
            ctx->block_received += chunk;
            if (ctx->block_received == ctx->block_len) {
                status = block_finish(ctx);
            }
            break;
        }
        }
    }

    if (status == USBSCPI_OK && eom && ctx->mode == MODE_TEXT) {
        status = execute_pending_line(ctx);
    }
    if (status != USBSCPI_OK) {
        ctx->mode = MODE_TEXT;
        ctx->line_len = 0;
        ctx->block_started = 0;
    }
    unlock_ctx(ctx);
    return status;
}

void usbscpi_task(usbscpi_t *ctx) {
    (void)ctx;
}

void usbscpi_clear(usbscpi_t *ctx) {
    if (!ctx) {
        return;
    }
    lock_ctx(ctx);
    ctx->mode = MODE_TEXT;
    ctx->line_len = 0;
    ctx->block_len = 0;
    ctx->block_received = 0;
    ctx->ndigits = 0;
    ctx->got_digits = 0;
    ctx->block_started = 0;
    SCPI_ErrorClear(&ctx->scpi);
    unlock_ctx(ctx);
}

scpi_t *usbscpi_scpi(usbscpi_t *ctx) {
    return ctx ? &ctx->scpi : NULL;
}
