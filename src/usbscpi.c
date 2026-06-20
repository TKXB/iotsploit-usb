#include "usbscpi/usbscpi.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "scpi/scpi.h"

#ifndef USBSCPI_MAX_CMDS
#define USBSCPI_MAX_CMDS 48u          /* core + user commands, incl. END sentinel */
#endif
#ifndef USBSCPI_INPUT_BUF_LEN
#define USBSCPI_INPUT_BUF_LEN 128u    /* libscpi working/parse buffer (one line)  */
#endif
#ifndef USBSCPI_ERR_QUEUE_LEN
#define USBSCPI_ERR_QUEUE_LEN 8       /* libscpi error queue depth                */
#endif

typedef enum {
    MODE_TEXT = 0,
    MODE_BLOCK_DIGITS,
    MODE_BLOCK_LEN,
    MODE_BLOCK_PAYLOAD
} rx_mode_t;

struct usbscpi {
    usbscpi_config_t cfg;
    scpi_t scpi;
    scpi_interface_t itf;
    char scpi_input[USBSCPI_INPUT_BUF_LEN];
    scpi_error_t err_queue[USBSCPI_ERR_QUEUE_LEN];
    scpi_command_t cmd_merged[USBSCPI_MAX_CMDS];
    size_t cmd_count;                 /* real commands (excludes END sentinel)    */

    /* RX byte state machine + DATA:WRITE block streaming (unchanged design) */
    rx_mode_t mode;
    size_t line_len;
    size_t block_len;
    size_t block_received;
    unsigned ndigits;
    unsigned got_digits;
    int block_started;
};

static usbscpi_t *scpi_owner(scpi_t *scpi) {
    return scpi ? (usbscpi_t *)scpi->user_context : NULL;
}

/* ---- raw output helper: write straight to the transport, no SCPI delimiter ---- */
static void raw_write(scpi_t *scpi, const char *data, size_t len) {
    if (scpi && scpi->interface && scpi->interface->write && len) {
        scpi->interface->write(scpi, data, len);
    }
}

/* ------------------------------------------------------------------ */
/* libscpi interface callbacks                                         */
/* ------------------------------------------------------------------ */

static size_t scpi_write_cb(scpi_t *scpi, const char *data, size_t len) {
    usbscpi_t *ctx = scpi_owner(scpi);
    if (!ctx || !ctx->cfg.usb_tx) {
        return 0;
    }
    return ctx->cfg.usb_tx(ctx->cfg.user, (const uint8_t *)data, len, true) == 0 ? len : 0;
}

static scpi_result_t scpi_flush_cb(scpi_t *scpi) {
    (void)scpi;
    return SCPI_RES_OK;
}

static int scpi_error_cb(scpi_t *scpi, int_fast16_t err) {
    (void)scpi;
    (void)err;
    return 0;
}

static scpi_result_t scpi_control_cb(scpi_t *scpi, scpi_ctrl_name_t ctrl, scpi_reg_val_t val) {
    (void)scpi;
    (void)ctrl;
    (void)val;
    return SCPI_RES_OK;
}

static scpi_result_t scpi_reset_cb(scpi_t *scpi) {
    usbscpi_t *ctx = scpi_owner(scpi);
    if (ctx && ctx->cfg.on_reset) {
        ctx->cfg.on_reset(ctx->cfg.user);
    }
    return SCPI_RES_OK;
}

/* ------------------------------------------------------------------ */
/* core commands (custom; IEEE 488.2 / SYST:ERR use libscpi built-ins) */
/* ------------------------------------------------------------------ */

static scpi_result_t cmd_idn(scpi_t *scpi) {
    usbscpi_t *ctx = scpi_owner(scpi);
    const char *idn = (ctx && ctx->cfg.idn) ? ctx->cfg.idn : "usbscpi,component,0,0.1.0";
    SCPI_ResultCharacters(scpi, idn, strlen(idn));   /* raw, no quotes */
    return SCPI_RES_OK;
}

static scpi_result_t cmd_syst_cap(scpi_t *scpi) {
    usbscpi_t *ctx = scpi_owner(scpi);
    if (!ctx) return SCPI_RES_ERR;
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "proto=%u;mtu=%zu;maxblock=%zu;feat=",
                     ctx->cfg.proto, ctx->cfg.mtu, ctx->cfg.max_block_len);
    if (n < 0) return SCPI_RES_ERR;
    SCPI_ResultCharacters(scpi, buf, (size_t)n);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_syst_help_head(scpi_t *scpi) {
    usbscpi_t *ctx = scpi_owner(scpi);
    if (!ctx) return SCPI_RES_ERR;

    uint32_t offset = 0, count = (uint32_t)-1;
    (void)SCPI_ParamUInt32(scpi, &offset, FALSE);
    (void)SCPI_ParamUInt32(scpi, &count, FALSE);

    size_t total_size = 0;
    size_t cmd_idx = 0;
    for (const scpi_command_t *cmd = ctx->cmd_merged; cmd->pattern; cmd++) {
        if (cmd_idx >= offset && cmd_idx < offset + count) {
            total_size += strlen(cmd->pattern) + 1; /* +1 for '\n' */
        }
        cmd_idx++;
    }
    if (ctx->cfg.mtu && total_size > ctx->cfg.mtu) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_TOO_MUCH_DATA);
        return SCPI_RES_ERR;
    }

    /* Raw write each pattern on its own line; do NOT use SCPI_Result* here
     * (libscpi would insert comma delimiters between results). */
    cmd_idx = 0;
    for (const scpi_command_t *cmd = ctx->cmd_merged; cmd->pattern; cmd++) {
        if (cmd_idx >= offset && cmd_idx < offset + count) {
            raw_write(scpi, cmd->pattern, strlen(cmd->pattern));
            raw_write(scpi, "\n", 1);
        }
        cmd_idx++;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_data_free(scpi_t *scpi) {
    usbscpi_t *ctx = scpi_owner(scpi);
    size_t free_bytes = 0;
    if (ctx && ctx->cfg.data_free) {
        free_bytes = ctx->cfg.data_free(ctx->cfg.user);
    }
    SCPI_ResultUInt32(scpi, (uint32_t)free_bytes);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_data_count(scpi_t *scpi) {
    usbscpi_t *ctx = scpi_owner(scpi);
    size_t count = 0;
    if (ctx && ctx->cfg.data_avail) {
        count = ctx->cfg.data_avail(ctx->cfg.user);
    }
    SCPI_ResultUInt32(scpi, (uint32_t)count);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_data_read(scpi_t *scpi) {
    usbscpi_t *ctx = scpi_owner(scpi);
    if (!ctx) return SCPI_RES_ERR;

    uint32_t count = 0;
    if (SCPI_ParamUInt32(scpi, &count, TRUE) != TRUE) {
        return SCPI_RES_ERR;
    }

    size_t avail = 0;
    if (ctx->cfg.data_avail) {
        avail = ctx->cfg.data_avail(ctx->cfg.user);
    }

    size_t to_read = count;
    if (to_read > ctx->cfg.io_buf_len) to_read = ctx->cfg.io_buf_len;
    if (to_read > avail) to_read = avail;

    size_t actual = 0;
    if (to_read > 0 && ctx->cfg.data_read && ctx->cfg.io_buf) {
        actual = ctx->cfg.data_read(ctx->cfg.user, ctx->cfg.io_buf, to_read);
    }

    if (ctx->cfg.mtu) {
        size_t temp = actual;
        int ndigits = 0;
        do { ndigits++; temp /= 10; } while (temp > 0);
        size_t total = 1 + (size_t)ndigits + actual + 1; /* # + digits + payload + \n */
        if (total > ctx->cfg.mtu) {
            SCPI_ErrorPush(scpi, SCPI_ERROR_TOO_MUCH_DATA);
            return SCPI_RES_ERR;
        }
    }

    SCPI_ResultArbitraryBlock(scpi, ctx->cfg.io_buf, actual);
    return SCPI_RES_OK;
}

static const scpi_command_t core_commands[] = {
    { "*IDN?", cmd_idn, 0 },
    { "*RST", SCPI_CoreRst, 0 },
    { "*CLS", SCPI_CoreCls, 0 },
    { "*OPC?", SCPI_CoreOpcQ, 0 },
    { "SYSTem:ERRor?", SCPI_SystemErrorNextQ, 0 },
    { "SYSTem:ERRor:COUNt?", SCPI_SystemErrorCountQ, 0 },
    { "SYSTem:CAPabilities?", cmd_syst_cap, 0 },
    { "SYSTem:HELP:HEADers?", cmd_syst_help_head, 0 },
    { "DATA:FREE?", cmd_data_free, 0 },
    { "DATA:COUNt?", cmd_data_count, 0 },
    { "DATA:READ?", cmd_data_read, 0 },
    SCPI_CMD_LIST_END
};

/* ------------------------------------------------------------------ */
/* lock / unlock                                                       */
/* ------------------------------------------------------------------ */

static void lock_ctx(usbscpi_t *ctx) {
    if (ctx && ctx->cfg.lock) ctx->cfg.lock(ctx->cfg.user);
}
static void unlock_ctx(usbscpi_t *ctx) {
    if (ctx && ctx->cfg.unlock) ctx->cfg.unlock(ctx->cfg.user);
}

/* ------------------------------------------------------------------ */
/* RX text/block state machine (feeds full text lines to libscpi)      */
/* ------------------------------------------------------------------ */

static int feed_line(usbscpi_t *ctx) {
    /* line_buf already holds 'line_len' bytes including a trailing '\n' */
    return SCPI_Input(&ctx->scpi, ctx->cfg.line_buf, (int)ctx->line_len) == TRUE
               ? USBSCPI_OK : USBSCPI_ERR_PROTOCOL;
}

static int line_is_data_write_prefix(const char *line, size_t len) {
    static const char prefix1[] = ":DATA:WRITE ";
    static const char prefix2[] = "DATA:WRITE ";
    while (len && isspace((unsigned char)line[0])) { line++; len--; }
    if (len != sizeof(prefix1) - 1 && len != sizeof(prefix2) - 1) return 0;
    const char *prefix = (len == sizeof(prefix1) - 1) ? prefix1 : prefix2;
    for (size_t i = 0; i < len; i++) {
        if (toupper((unsigned char)line[i]) != prefix[i]) return 0;
    }
    return 1;
}

static int execute_pending_line(usbscpi_t *ctx) {
    if (ctx->line_len == 0) return USBSCPI_OK;
    if (ctx->line_len + 1 > ctx->cfg.line_buf_len) {
        ctx->line_len = 0;
        SCPI_ErrorPush(&ctx->scpi, SCPI_ERROR_EXECUTION_ERROR);
        return USBSCPI_ERR_OVERFLOW;
    }
    ctx->cfg.line_buf[ctx->line_len++] = '\n';
    int rc = feed_line(ctx);
    ctx->line_len = 0;
    return rc;
}

static int block_begin(usbscpi_t *ctx) {
    if (ctx->block_started) return USBSCPI_OK;
    if (ctx->cfg.max_block_len && ctx->block_len > ctx->cfg.max_block_len) {
        SCPI_ErrorPush(&ctx->scpi, SCPI_ERROR_TOO_MUCH_DATA);
        return USBSCPI_ERR_OVERFLOW;
    }
    ctx->block_started = 1;
    if (ctx->cfg.on_block_begin && ctx->cfg.on_block_begin(ctx->cfg.user, ctx->block_len) != 0) {
        SCPI_ErrorPush(&ctx->scpi, SCPI_ERROR_EXECUTION_ERROR);
        return USBSCPI_ERR_CALLBACK;
    }
    return USBSCPI_OK;
}

static int block_finish(usbscpi_t *ctx) {
    if (ctx->cfg.on_block_end && ctx->cfg.on_block_end(ctx->cfg.user, ctx->block_len) != 0) {
        SCPI_ErrorPush(&ctx->scpi, SCPI_ERROR_EXECUTION_ERROR);
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

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

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
    ctx->mode = MODE_TEXT;

    /* seed cmd_merged with core commands + END sentinel */
    size_t i = 0;
    for (; core_commands[i].pattern && i + 1 < USBSCPI_MAX_CMDS; i++) {
        ctx->cmd_merged[i] = core_commands[i];
    }
    ctx->cmd_count = i;
    ctx->cmd_merged[i].pattern = NULL;
    ctx->cmd_merged[i].callback = NULL;
    ctx->cmd_merged[i].tag = 0;

    ctx->itf.error = scpi_error_cb;
    ctx->itf.write = scpi_write_cb;
    ctx->itf.control = scpi_control_cb;
    ctx->itf.flush = scpi_flush_cb;
    ctx->itf.reset = scpi_reset_cb;

    SCPI_Init(&ctx->scpi, ctx->cmd_merged, &ctx->itf, scpi_units_def,
              NULL, NULL, NULL, NULL,
              ctx->scpi_input, sizeof(ctx->scpi_input),
              ctx->err_queue, USBSCPI_ERR_QUEUE_LEN);
    ctx->scpi.user_context = ctx;
    return ctx;
}

int usbscpi_register(usbscpi_t *ctx, const scpi_command_t *commands) {
    if (!ctx || !commands) {
        return USBSCPI_ERR_ARG;
    }
    for (size_t i = 0; commands[i].pattern; i++) {
        if (ctx->cmd_count + 1 >= USBSCPI_MAX_CMDS) {   /* keep room for END */
            return USBSCPI_ERR_OVERFLOW;
        }
        ctx->cmd_merged[ctx->cmd_count++] = commands[i];
    }
    ctx->cmd_merged[ctx->cmd_count].pattern = NULL;
    ctx->cmd_merged[ctx->cmd_count].callback = NULL;
    ctx->cmd_merged[ctx->cmd_count].tag = 0;
    /* scpi.cmdlist already points at cmd_merged; nothing else to do */
    return USBSCPI_OK;
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
            if (c == '\r' || c == '\0') {   /* ignore CR and stray NULs */
                break;
            }
            if (c == '\n' || c == ';') {
                ctx->cfg.line_buf[ctx->line_len++] = '\n';
                status = feed_line(ctx);
                ctx->line_len = 0;
                break;
            }
            if (ctx->line_len + 1 >= ctx->cfg.line_buf_len) {
                ctx->line_len = 0;
                SCPI_ErrorPush(&ctx->scpi, SCPI_ERROR_EXECUTION_ERROR);
                status = USBSCPI_ERR_OVERFLOW;
                break;
            }
            ctx->cfg.line_buf[ctx->line_len++] = (char)c;
            break;

        case MODE_BLOCK_DIGITS:
            i++;
            if (c < '1' || c > '9') {
                SCPI_ErrorPush(&ctx->scpi, SCPI_ERROR_INVALID_BLOCK_DATA);
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
                SCPI_ErrorPush(&ctx->scpi, SCPI_ERROR_INVALID_BLOCK_DATA);
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
                SCPI_ErrorPush(&ctx->scpi, SCPI_ERROR_EXECUTION_ERROR);
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
