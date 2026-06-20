#include "scpi/scpi.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void scpi_write_cstr(scpi_t *ctx, const char *s) {
    if (ctx && ctx->write && s) {
        ctx->write(ctx->user, s, strlen(s));
    }
}

static int scpi_write_data(scpi_t *ctx, const char *data, size_t len) {
    if (ctx && ctx->write && len) {
        return ctx->write(ctx->user, data, len);
    }
    return SCPI_RES_ERR;
}

static int segment_match(const char *pattern, size_t plen, const char *input, size_t ilen) {
    char full[48];
    char short_name[48];
    size_t f = 0;
    size_t s = 0;

    for (size_t i = 0; i < plen && f + 1 < sizeof(full); i++) {
        unsigned char c = (unsigned char)pattern[i];
        if (isalpha(c)) {
            full[f++] = (char)toupper(c);
            if (isupper(c) && s + 1 < sizeof(short_name)) {
                short_name[s++] = (char)c;
            }
        } else {
            full[f++] = (char)c;
            if (s + 1 < sizeof(short_name)) {
                short_name[s++] = (char)c;
            }
        }
    }
    full[f] = '\0';
    short_name[s] = '\0';

    char normalized[48];
    if (ilen >= sizeof(normalized)) {
        return 0;
    }
    for (size_t i = 0; i < ilen; i++) {
        normalized[i] = (char)toupper((unsigned char)input[i]);
    }
    normalized[ilen] = '\0';

    return strcmp(normalized, full) == 0 || strcmp(normalized, short_name) == 0;
}

static int ascii_case_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int pattern_match(const char *pattern, const char *input) {
    if (!pattern || !input) {
        return 0;
    }
    if (pattern[0] == '*') {
        return ascii_case_equal(pattern, input);
    }

    if (input[0] == ':') {
        input++;
    }
    if (pattern[0] == ':') {
        pattern++;
    }

    while (*pattern && *input) {
        const char *p_end = strchr(pattern, ':');
        const char *i_end = strchr(input, ':');
        size_t p_len = p_end ? (size_t)(p_end - pattern) : strlen(pattern);
        size_t i_len = i_end ? (size_t)(i_end - input) : strlen(input);

        if (!segment_match(pattern, p_len, input, i_len)) {
            return 0;
        }
        pattern += p_len;
        input += i_len;
        if (*pattern == ':') {
            pattern++;
        }
        if (*input == ':') {
            input++;
        }
    }

    return *pattern == '\0' && *input == '\0';
}

static void trim_line(char *line) {
    size_t len = strlen(line);
    while (len && isspace((unsigned char)line[len - 1])) {
        line[--len] = '\0';
    }
    char *start = line;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != line) {
        memmove(line, start, strlen(start) + 1);
    }
}

static int execute_line(scpi_t *ctx, char *line) {
    trim_line(line);
    if (line[0] == '\0') {
        return SCPI_RES_OK;
    }

    char *command = line;
    while (*command && isspace((unsigned char)*command)) {
        command++;
    }
    char *end = command;
    while (*end && !isspace((unsigned char)*end)) {
        end++;
    }

    char *args_start = end;
    if (*end != '\0') {
        args_start = end + 1;
        *end = '\0';
    }
    ctx->args = args_start;

    for (size_t t = 0; t < ctx->table_count; t++) {
        const scpi_command_t *cmd = ctx->tables[t];
        for (; cmd && cmd->pattern; cmd++) {
            if (pattern_match(cmd->pattern, command)) {
                return cmd->callback ? cmd->callback(ctx) : SCPI_RES_ERR;
            }
        }
    }

    SCPI_ErrorPush(ctx, -113, "Undefined header");
    return SCPI_RES_ERR;
}

void SCPI_Init(scpi_t *ctx, scpi_write_t write, void *user) {
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->write = write;
    ctx->user = user;
}

int SCPI_RegisterCommands(scpi_t *ctx, const scpi_command_t *commands) {
    if (!ctx || !commands || ctx->table_count >= sizeof(ctx->tables) / sizeof(ctx->tables[0])) {
        return SCPI_RES_ERR;
    }
    ctx->tables[ctx->table_count++] = commands;
    return SCPI_RES_OK;
}

int SCPI_Input(scpi_t *ctx, const char *data, size_t len) {
    if (!ctx || (!data && len)) {
        return SCPI_RES_ERR;
    }

    int result = SCPI_RES_OK;
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n' || c == ';') {
            ctx->input[ctx->input_len] = '\0';
            if (execute_line(ctx, ctx->input) != SCPI_RES_OK) {
                result = SCPI_RES_ERR;
            }
            ctx->input_len = 0;
            continue;
        }
        if (ctx->input_len + 1 >= sizeof(ctx->input)) {
            ctx->input_len = 0;
            SCPI_ErrorPush(ctx, -200, "Input buffer overflow");
            return SCPI_RES_ERR;
        }
        ctx->input[ctx->input_len++] = c;
    }
    return result;
}

int SCPI_ResultText(scpi_t *ctx, const char *value) {
    scpi_write_cstr(ctx, value ? value : "");
    scpi_write_cstr(ctx, "\n");
    return SCPI_RES_OK;
}

int SCPI_ResultUInt32(scpi_t *ctx, uint32_t value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu\n", (unsigned long)value);
    scpi_write_cstr(ctx, buf);
    return SCPI_RES_OK;
}

int SCPI_ResultInt32(scpi_t *ctx, int32_t value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld\n", (long)value);
    scpi_write_cstr(ctx, buf);
    return SCPI_RES_OK;
}

int SCPI_ResultBool(scpi_t *ctx, int value) {
    return SCPI_ResultUInt32(ctx, value ? 1u : 0u);
}

int SCPI_ParamUInt32(scpi_t *ctx, uint32_t *val, int mandatory) {
    if (!ctx || !val) {
        return SCPI_RES_ERR;
    }
    if (!ctx->args) {
        if (mandatory) {
            SCPI_ErrorPush(ctx, -109, "Missing parameter");
        }
        return mandatory ? SCPI_RES_ERR : SCPI_RES_OK;
    }
    const char *p = ctx->args;
    while (*p && (isspace((unsigned char)*p) || *p == ',')) {
        p++;
    }
    if (*p == '\0') {
        if (mandatory) {
            SCPI_ErrorPush(ctx, -109, "Missing parameter");
        }
        return mandatory ? SCPI_RES_ERR : SCPI_RES_OK;
    }

    char *endptr = NULL;
    unsigned long v = strtoul(p, &endptr, 0);
    if (endptr == p) {
        if (mandatory) {
            SCPI_ErrorPush(ctx, -224, "Illegal parameter value");
        }
        return mandatory ? SCPI_RES_ERR : SCPI_RES_OK;
    }

    *val = (uint32_t)v;
    ctx->args = endptr;
    return SCPI_RES_OK;
}

int SCPI_ResultArbitraryBlock(scpi_t *ctx, const uint8_t *data, size_t len) {
    if (!ctx) {
        return SCPI_RES_ERR;
    }
    size_t temp = len;
    int ndigits = 0;
    do {
        ndigits++;
        temp /= 10;
    } while (temp > 0);

    char header[16];
    int n = snprintf(header, sizeof(header), "#%d%zu", ndigits, (size_t)len);
    if (n < 0 || (size_t)n >= sizeof(header)) {
        return SCPI_RES_ERR;
    }

    if (scpi_write_data(ctx, header, (size_t)n) != SCPI_RES_OK) {
        return SCPI_RES_ERR;
    }
    if (len > 0 && scpi_write_data(ctx, (const char *)data, len) != SCPI_RES_OK) {
        return SCPI_RES_ERR;
    }
    if (scpi_write_data(ctx, "\n", 1) != SCPI_RES_OK) {
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

void SCPI_ErrorPush(scpi_t *ctx, int code, const char *message) {
    if (!ctx) {
        return;
    }
    size_t pos = (ctx->error_head + ctx->error_count) % (sizeof(ctx->errors) / sizeof(ctx->errors[0]));
    if (ctx->error_count == sizeof(ctx->errors) / sizeof(ctx->errors[0])) {
        ctx->error_head = (ctx->error_head + 1) % (sizeof(ctx->errors) / sizeof(ctx->errors[0]));
        pos = (ctx->error_head + ctx->error_count - 1) % (sizeof(ctx->errors) / sizeof(ctx->errors[0]));
    } else {
        ctx->error_count++;
    }
    ctx->errors[pos].code = code;
    snprintf(ctx->errors[pos].message, sizeof(ctx->errors[pos].message), "%s", message ? message : "Error");
}

int SCPI_ErrorPop(scpi_t *ctx, scpi_error_t *out) {
    if (!ctx || !out || ctx->error_count == 0) {
        return 0;
    }
    *out = ctx->errors[ctx->error_head];
    ctx->error_head = (ctx->error_head + 1) % (sizeof(ctx->errors) / sizeof(ctx->errors[0]));
    ctx->error_count--;
    return 1;
}

void SCPI_ErrorClear(scpi_t *ctx) {
    if (!ctx) {
        return;
    }
    ctx->error_head = 0;
    ctx->error_count = 0;
}
