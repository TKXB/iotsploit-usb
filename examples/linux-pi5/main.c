/*
 * Linux / Raspberry Pi 5 SCPI-over-Ethernet server example.
 *
 * Reuses the iotsploit-usb core (src/usbscpi.c) and BSD-socket transport glue
 * (glue/usbscpi_socket.c). Exposes Pi 5 RP1 GPIO via Linux GPIO cdev v2 ioctls
 * and read-only Linux system status (/proc, /sys, uname, getifaddrs).
 *
 * Build:
 *   cmake -S . -B build -DUSBSCPI_BUILD_SOCKET_GLUE=ON
 *   cmake --build build --target usbscpi_linux_pi5
 *
 * Usage:
 *   ./build/examples/linux-pi5/usbscpi_linux_pi5 [--bind ADDR] [--port PORT] [--gpiochip PATH]
 */

#include <ctype.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbscpi/usbscpi.h"
#include "usbscpi_socket.h"

#include "file_xfer.h"
#include "gpio_chardev.h"
#include "sys_exec.h"
#include "sys_status.h"

/* ---------- static storage (no dynamic allocation after init) ---------- */

static uint8_t s_storage[4096];
static char    s_line[256];
static uint8_t s_io[8192];
static char    s_idn[96];

static void on_exit_signal(int sig) {
    (void)sig;
    pi5_file_cleanup();
    pi5_exec_cleanup();
    pi5_gpio_cleanup();
    _Exit(0);
}

static int token_ieq(const char *s, size_t len, const char *lit) {
    size_t n = strlen(lit);
    if (len != n) return 0;
    for (size_t i = 0; i < n; i++) {
        if (toupper((unsigned char)s[i]) != toupper((unsigned char)lit[i])) {
            return 0;
        }
    }
    return 1;
}

/* ---------- GPIO handlers ---------- */

static scpi_result_t cmd_gpio_set(scpi_t *scpi) {
    uint32_t pin = 0, level = 0;
    if (!SCPI_ParamUInt32(scpi, &pin, TRUE) || !SCPI_ParamUInt32(scpi, &level, TRUE)) {
        return SCPI_RES_ERR;
    }
    if (pin >= PI5_GPIO_MAX_PINS) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    if (pi5_gpio_set(pin, level ? 1u : 0u) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_gpio_get(scpi_t *scpi) {
    uint32_t pin = 0;
    if (!SCPI_ParamUInt32(scpi, &pin, TRUE)) {
        return SCPI_RES_ERR;
    }
    if (pin >= PI5_GPIO_MAX_PINS) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    uint32_t level = 0;
    if (pi5_gpio_get(pin, &level) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultUInt32(scpi, level);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_gpio_dir_set(scpi_t *scpi) {
    uint32_t pin = 0;
    const char *word = NULL;
    size_t wlen = 0;
    if (!SCPI_ParamUInt32(scpi, &pin, TRUE) ||
        !SCPI_ParamCharacters(scpi, &word, &wlen, TRUE)) {
        return SCPI_RES_ERR;
    }
    if (pin >= PI5_GPIO_MAX_PINS) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    pi5_gpio_dir_t dir;
    if (token_ieq(word, wlen, "IN")) {
        dir = PI5_GPIO_DIR_IN;
    } else if (token_ieq(word, wlen, "OUT")) {
        dir = PI5_GPIO_DIR_OUT;
    } else {
        SCPI_ErrorPush(scpi, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
        return SCPI_RES_ERR;
    }
    if (pi5_gpio_set_dir(pin, dir) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_gpio_dir_get(scpi_t *scpi) {
    uint32_t pin = 0;
    if (!SCPI_ParamUInt32(scpi, &pin, TRUE)) {
        return SCPI_RES_ERR;
    }
    if (pin >= PI5_GPIO_MAX_PINS) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    const char *name = "FREE";
    switch (pi5_gpio_get_dir(pin)) {
    case PI5_GPIO_DIR_IN:  name = "IN";  break;
    case PI5_GPIO_DIR_OUT: name = "OUT"; break;
    case PI5_GPIO_DIR_FREE:
    default:               name = "FREE"; break;
    }
    SCPI_ResultMnemonic(scpi, name);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_gpio_pull_set(scpi_t *scpi) {
    uint32_t pin = 0;
    const char *word = NULL;
    size_t wlen = 0;
    if (!SCPI_ParamUInt32(scpi, &pin, TRUE) ||
        !SCPI_ParamCharacters(scpi, &word, &wlen, TRUE)) {
        return SCPI_RES_ERR;
    }
    if (pin >= PI5_GPIO_MAX_PINS) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    pi5_gpio_pull_t pull;
    if (token_ieq(word, wlen, "NONE")) {
        pull = PI5_GPIO_PULL_NONE;
    } else if (token_ieq(word, wlen, "UP")) {
        pull = PI5_GPIO_PULL_UP;
    } else if (token_ieq(word, wlen, "DOWN")) {
        pull = PI5_GPIO_PULL_DOWN;
    } else {
        SCPI_ErrorPush(scpi, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
        return SCPI_RES_ERR;
    }
    if (pi5_gpio_set_pull(pin, pull) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_gpio_release(scpi_t *scpi) {
    uint32_t pin = 0;
    if (!SCPI_ParamUInt32(scpi, &pin, TRUE)) {
        return SCPI_RES_ERR;
    }
    if (pin >= PI5_GPIO_MAX_PINS) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    (void)pi5_gpio_release(pin);
    return SCPI_RES_OK;
}

/* ---------- System status handlers ---------- */

static scpi_result_t cmd_sys_uptime(scpi_t *scpi) {
    uint64_t secs = 0;
    if (pi5_sys_uptime(&secs) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultUInt64(scpi, secs);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_sys_temp(scpi_t *scpi) {
    int32_t mc = 0;
    if (pi5_sys_temp_mc(&mc) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultInt32(scpi, mc);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_sys_memory(scpi_t *scpi) {
    uint64_t total = 0, avail = 0;
    if (pi5_sys_memory_kb(&total, &avail) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    char out[64];
    int n = snprintf(out, sizeof(out), "%" PRIu64 ",%" PRIu64, total, avail);
    if (n <= 0 || (size_t)n >= sizeof(out)) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultCharacters(scpi, out, (size_t)n);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_sys_load(scpi_t *scpi) {
    char out[64];
    if (pi5_sys_load(out, sizeof(out)) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultCharacters(scpi, out, strlen(out));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_sys_host(scpi_t *scpi) {
    char out[192];
    if (pi5_sys_host(out, sizeof(out)) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultCharacters(scpi, out, strlen(out));
    return SCPI_RES_OK;
}

static scpi_result_t cmd_sys_net(scpi_t *scpi) {
    const char *raw = NULL;
    size_t rlen = 0;
    if (!SCPI_ParamCharacters(scpi, &raw, &rlen, TRUE)) {
        return SCPI_RES_ERR;
    }
    if (rlen == 0 || rlen >= 32) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
        return SCPI_RES_ERR;
    }
    char ifname[32];
    memcpy(ifname, raw, rlen);
    ifname[rlen] = '\0';

    char out[64];
    if (pi5_sys_net(ifname, out, sizeof(out)) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultCharacters(scpi, out, strlen(out));
    return SCPI_RES_OK;
}

/* ---------- Linux command execution handlers (SYSTem:EXEC*) ---------- */

static int extract_cmd_param(scpi_t *scpi, char *dst, size_t cap) {
    const char *raw = NULL;
    size_t rlen = 0;
    if (!SCPI_ParamCharacters(scpi, &raw, &rlen, TRUE)) {
        return -1;
    }
    if (rlen == 0 || rlen >= cap) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
        return -1;
    }
    /* Copy and unescape doubled quotes ("" -> " or '' -> ') inside SCPI strings. */
    size_t out_i = 0;
    for (size_t i = 0; i < rlen && out_i + 1 < cap; i++) {
        dst[out_i++] = raw[i];
        if ((raw[i] == '"' || raw[i] == '\'') && i + 1 < rlen && raw[i + 1] == raw[i]) {
            i++;
        }
    }
    dst[out_i] = '\0';
    return (out_i > 0) ? 0 : -1;
}

static scpi_result_t cmd_sys_exec(scpi_t *scpi) {
    char cmd[224];
    if (extract_cmd_param(scpi, cmd, sizeof(cmd)) != 0) {
        return SCPI_RES_ERR;
    }
    if (pi5_exec_run_sync(cmd, PI5_EXEC_DEFAULT_TIMEOUT_MS) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    size_t out_len = 0;
    const uint8_t *out = pi5_exec_output(&out_len);
    SCPI_ResultArbitraryBlock(scpi, out, out_len);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_sys_exec_start(scpi_t *scpi) {
    char cmd[224];
    if (extract_cmd_param(scpi, cmd, sizeof(cmd)) != 0) {
        return SCPI_RES_ERR;
    }
    if (pi5_exec_start(cmd) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_sys_exec_stop(scpi_t *scpi) {
    (void)scpi;
    pi5_exec_stop();
    return SCPI_RES_OK;
}

static scpi_result_t cmd_sys_exec_state(scpi_t *scpi) {
    pi5_exec_state_t st = pi5_exec_poll();
    if (st == PI5_EXEC_FAILED && pi5_exec_take_error_latch()) {
        usbscpi_queue_error(scpi, SCPI_ERROR_EXECUTION_ERROR);
    }
    const char *word = "IDLE";
    switch (st) {
    case PI5_EXEC_RUNNING: word = "RUNNING"; break;
    case PI5_EXEC_DONE:    word = "DONE";    break;
    case PI5_EXEC_FAILED:  word = "FAILED";  break;
    case PI5_EXEC_IDLE:
    default:               word = "IDLE";    break;
    }
    SCPI_ResultMnemonic(scpi, word);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_sys_exec_code(scpi_t *scpi) {
    SCPI_ResultInt32(scpi, pi5_exec_exit_code());
    return SCPI_RES_OK;
}

static scpi_result_t cmd_sys_exec_output(scpi_t *scpi) {
    size_t out_len = 0;
    const uint8_t *out = pi5_exec_output(&out_len);
    SCPI_ResultArbitraryBlock(scpi, out, out_len);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_sys_exec_clear(scpi_t *scpi) {
    (void)scpi;
    pi5_exec_clear();
    return SCPI_RES_OK;
}

/* ---------- File transfer handlers (FILE:* + DATA:WRITE / DATA:READ?) ---------- */

static scpi_result_t cmd_file_open(scpi_t *scpi) {
    char path[224];
    if (extract_cmd_param(scpi, path, sizeof(path)) != 0) {
        return SCPI_RES_ERR;
    }
    const char *mword = NULL;
    size_t mlen = 0;
    if (!SCPI_ParamCharacters(scpi, &mword, &mlen, TRUE)) {
        return SCPI_RES_ERR;
    }
    pi5_file_mode_t mode;
    if (token_ieq(mword, mlen, "READ") || token_ieq(mword, mlen, "R")) {
        mode = PI5_FILE_READ;
    } else if (token_ieq(mword, mlen, "WRITE") || token_ieq(mword, mlen, "W")) {
        mode = PI5_FILE_WRITE;
    } else if (token_ieq(mword, mlen, "APPEND") || token_ieq(mword, mlen, "A")) {
        mode = PI5_FILE_APPEND;
    } else {
        SCPI_ErrorPush(scpi, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
        return SCPI_RES_ERR;
    }
    if (pi5_file_open(path, mode) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_file_close(scpi_t *scpi) {
    if (pi5_file_close() != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_file_state(scpi_t *scpi) {
    const char *word = "IDLE";
    switch (pi5_file_mode()) {
    case PI5_FILE_READ:   word = "READ";   break;
    case PI5_FILE_WRITE:  word = "WRITE";  break;
    case PI5_FILE_APPEND: word = "APPEND"; break;
    case PI5_FILE_IDLE:
    default:              word = "IDLE";   break;
    }
    SCPI_ResultMnemonic(scpi, word);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_file_size(scpi_t *scpi) {
    const char *raw = NULL;
    size_t rlen = 0;
    char path[224];
    const char *opt_path = NULL;
    if (SCPI_ParamCharacters(scpi, &raw, &rlen, FALSE) && rlen > 0) {
        if (rlen >= sizeof(path)) {
            SCPI_ErrorPush(scpi, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
            return SCPI_RES_ERR;
        }
        memcpy(path, raw, rlen);
        path[rlen] = '\0';
        opt_path = path;
    }
    uint64_t sz = 0;
    if (pi5_file_size(opt_path, &sz) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultUInt64(scpi, sz);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_file_seek(scpi_t *scpi) {
    uint64_t offset = 0;
    if (!SCPI_ParamUInt64(scpi, &offset, TRUE)) {
        return SCPI_RES_ERR;
    }
    if (pi5_file_seek(offset) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_file_read(scpi_t *scpi) {
    uint32_t want = 0;
    if (!SCPI_ParamUInt32(scpi, &want, TRUE)) {
        return SCPI_RES_ERR;
    }
    if (pi5_file_mode() != PI5_FILE_READ) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    /* Cap payload so `#44000<payload>\n` fits inside the 4096-byte TCP MTU. */
    size_t cap = 4000u;
    if (cap > sizeof(s_io)) {
        cap = sizeof(s_io);
    }
    size_t to_read = (want < cap) ? (size_t)want : cap;
    size_t actual  = pi5_file_data_read(NULL, s_io, to_read);
    SCPI_ResultArbitraryBlock(scpi, s_io, actual);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_file_delete(scpi_t *scpi) {
    char path[224];
    if (extract_cmd_param(scpi, path, sizeof(path)) != 0) {
        return SCPI_RES_ERR;
    }
    if (pi5_file_delete(path) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}

static scpi_result_t cmd_file_crc32(scpi_t *scpi) {
    char path[224];
    if (extract_cmd_param(scpi, path, sizeof(path)) != 0) {
        return SCPI_RES_ERR;
    }
    uint32_t crc = 0;
    if (pi5_file_crc32(path, &crc) != 0) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_EXECUTION_ERROR);
        return SCPI_RES_ERR;
    }
    SCPI_ResultUInt32(scpi, crc);
    return SCPI_RES_OK;
}

/* ---------- commands and descriptor (SYSTem:HELP:DESCription?) ---------- */

static const usbscpi_param_desc_t gpio_set_params[] = {
    USBSCPI_PARAM("pin", "u32", true),
    USBSCPI_PARAM("value", "bool", true),
};
static const usbscpi_param_desc_t gpio_pin_params[] = {
    USBSCPI_PARAM("pin", "u32", true),
};
static const usbscpi_param_desc_t gpio_dir_params[] = {
    USBSCPI_PARAM("pin", "u32", true),
    USBSCPI_PARAM("dir", "string", true),
};
static const usbscpi_param_desc_t gpio_pull_params[] = {
    USBSCPI_PARAM("pin", "u32", true),
    USBSCPI_PARAM("bias", "string", true),
};
static const usbscpi_param_desc_t sys_net_params[] = {
    USBSCPI_PARAM("interface", "string", true),
};
static const usbscpi_param_desc_t sys_exec_params[] = {
    USBSCPI_PARAM("command", "string", true),
};
static const usbscpi_param_desc_t file_open_params[] = {
    USBSCPI_PARAM("path", "string", true),
    USBSCPI_PARAM("mode", "string", true),
};
static const usbscpi_param_desc_t file_path_params[] = {
    USBSCPI_PARAM("path", "string", true),
};
static const usbscpi_param_desc_t file_opt_path_params[] = {
    USBSCPI_PARAM("path", "string", false),
};
static const usbscpi_param_desc_t file_seek_params[] = {
    USBSCPI_PARAM("offset", "u64", true),
};
static const usbscpi_param_desc_t file_read_params[] = {
    USBSCPI_PARAM("length", "u32", true),
};

#define PI5_COMMANDS(CMD, ALIAS)                                                        \
    CMD("GPIO",                 cmd_gpio_set,        "command", "Set a GPIO output level", \
        USBSCPI_PARAMS(gpio_set_params), "none")                                        \
    CMD("GPIO?",                cmd_gpio_get,        "query",   "Read a GPIO level",       \
        USBSCPI_PARAMS(gpio_pin_params), "u32")                                         \
    CMD("GPIO:DIRection",       cmd_gpio_dir_set,    "command", "Set GPIO direction (IN/OUT)", \
        USBSCPI_PARAMS(gpio_dir_params), "none")                                        \
    CMD("GPIO:DIRection?",      cmd_gpio_dir_get,    "query",   "Read GPIO direction",     \
        USBSCPI_PARAMS(gpio_pin_params), "string")                                      \
    CMD("GPIO:PULL",            cmd_gpio_pull_set,   "command", "Set GPIO bias (NONE/UP/DOWN)", \
        USBSCPI_PARAMS(gpio_pull_params), "none")                                       \
    CMD("GPIO:RELease",         cmd_gpio_release,    "command", "Release a GPIO line",     \
        USBSCPI_PARAMS(gpio_pin_params), "none")                                        \
    CMD("SYSTem:UPTime?",       cmd_sys_uptime,      "query",   "System uptime in seconds", \
        USBSCPI_NO_PARAMS, "u64")                                                       \
    CMD("SYSTem:TEMPerature?",  cmd_sys_temp,        "query",   "SoC temperature in millidegrees C", \
        USBSCPI_NO_PARAMS, "i32")                                                       \
    CMD("SYSTem:MEMory?",       cmd_sys_memory,      "query",   "Total and available memory in KiB", \
        USBSCPI_NO_PARAMS, "string")                                                    \
    CMD("SYSTem:LOAD?",         cmd_sys_load,        "query",   "1m,5m,15m load averages", \
        USBSCPI_NO_PARAMS, "string")                                                    \
    CMD("SYSTem:HOST?",         cmd_sys_host,        "query",   "Hostname, kernel release, and arch", \
        USBSCPI_NO_PARAMS, "string")                                                    \
    CMD("SYSTem:NET?",          cmd_sys_net,         "query",   "Interface state and IPv4 address", \
        USBSCPI_PARAMS(sys_net_params), "string")                                       \
    CMD("SYSTem:EXEC?",         cmd_sys_exec,        "block",   "Execute a shell command and return output block", \
        USBSCPI_PARAMS(sys_exec_params), "block")                                       \
    CMD("SYSTem:EXEC:STARt",    cmd_sys_exec_start,  "command", "Start a shell command asynchronously", \
        USBSCPI_PARAMS(sys_exec_params), "none")                                        \
    CMD("SYSTem:EXEC:STOP",     cmd_sys_exec_stop,   "command", "Stop the running shell command", \
        USBSCPI_NO_PARAMS, "none")                                                      \
    CMD("SYSTem:EXEC:STATe?",   cmd_sys_exec_state,  "query",   "IDLE, RUNNING, DONE or FAILED", \
        USBSCPI_NO_PARAMS, "string")                                                    \
    CMD("SYSTem:EXEC:CODE?",    cmd_sys_exec_code,   "query",   "Exit code of the shell command", \
        USBSCPI_NO_PARAMS, "i32")                                                       \
    CMD("SYSTem:EXEC:OUTPut?",  cmd_sys_exec_output, "block",   "Read captured stdout/stderr as a binary block", \
        USBSCPI_NO_PARAMS, "block")                                                      \
    CMD("SYSTem:EXEC:CLEar",    cmd_sys_exec_clear,  "command", "Clear command output and reset state", \
        USBSCPI_NO_PARAMS, "none")                                                      \
    CMD("FILE:OPEN",            cmd_file_open,       "command", "Open file for READ, WRITE or APPEND", \
        USBSCPI_PARAMS(file_open_params), "none")                                       \
    CMD("FILE:CLOSe",           cmd_file_close,      "command", "Close currently open file", \
        USBSCPI_NO_PARAMS, "none")                                                      \
    CMD("FILE:STATe?",          cmd_file_state,      "query",   "IDLE, READ, WRITE or APPEND", \
        USBSCPI_NO_PARAMS, "string")                                                    \
    CMD("FILE:SIZE?",           cmd_file_size,       "query",   "File size in bytes",   \
        USBSCPI_PARAMS(file_opt_path_params), "u64")                                    \
    CMD("FILE:SEEK",            cmd_file_seek,       "command", "Seek to byte offset in open file", \
        USBSCPI_PARAMS(file_seek_params), "none")                                       \
    CMD("FILE:READ?",           cmd_file_read,       "block",   "Read up to N bytes from open file as binary block", \
        USBSCPI_PARAMS(file_read_params), "block")                                      \
    CMD("FILE:DELete",          cmd_file_delete,     "command", "Delete a file on the target", \
        USBSCPI_PARAMS(file_path_params), "none")                                       \
    CMD("FILE:CRC32?",          cmd_file_crc32,      "query",   "CRC-32 checksum of a file", \
        USBSCPI_PARAMS(file_path_params), "u32")                                        \
    ALIAS("GPIO:SET",           cmd_gpio_set)                                           \
    ALIAS("GPIO:GET?",          cmd_gpio_get)
USBSCPI_DEFINE_COMMANDS(pi5, PI5_COMMANDS);

static const usbscpi_descriptor_t s_descriptor = {
    .commands       = pi5_desc_commands,
    .command_count  = USBSCPI_COUNT(pi5_desc_commands),
    .workflows      = NULL,
    .workflow_count = 0,
};

/* ---------- entry point ---------- */

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--bind ADDR] [--port PORT] [--gpiochip PATH]\n"
            "       %s [bind_addr] [port]\n",
            prog, prog);
}

int main(int argc, char **argv) {
    const char *bind_addr = "127.0.0.1";
    uint16_t    port      = 5025;
    const char *gpiochip  = NULL;
    int         pos       = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--gpiochip") == 0 && i + 1 < argc) {
            gpiochip = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else {
            if (pos == 0) {
                bind_addr = argv[i];
            } else if (pos == 1) {
                port = (uint16_t)atoi(argv[i]);
            }
            pos++;
        }
    }

#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    signal(SIGINT, on_exit_signal);
    signal(SIGTERM, on_exit_signal);

    if (pi5_gpio_init(gpiochip) == 0) {
        printf("GPIO ready: %s (%s, %u lines)\n",
               pi5_gpio_chip_path(), pi5_gpio_chip_label(), pi5_gpio_num_lines());
    } else {
        fprintf(stderr,
                "warning: no GPIO chip opened (%s); GPIO commands will return execution error\n",
                gpiochip ? gpiochip : "auto pinctrl-rp1");
    }

    char serial[48];
    pi5_sys_serial(serial, sizeof(serial));
    snprintf(s_idn, sizeof(s_idn), "IoTSploit,Linux-Pi5,%s,0.1.0", serial);

    usbscpi_config_t cfg = {
        .usb_tx         = usbscpi_socket_tx,
        .on_block_begin = pi5_file_on_block_begin,
        .on_block_data  = pi5_file_on_block_data,
        .on_block_end   = pi5_file_on_block_end,
        .data_free      = pi5_file_data_free,
        .data_avail     = pi5_file_data_avail,
        .data_read      = pi5_file_data_read,
        .line_buf       = s_line,
        .line_buf_len   = sizeof(s_line),
        .max_block_len  = PI5_FILE_MAX_BLOCK_LEN,
        .idn            = s_idn,
        .io_buf         = s_io,
        .io_buf_len     = sizeof(s_io),
        .proto          = 1,
        .mtu            = 4096,
        .descriptor     = &s_descriptor,
    };

    usbscpi_t *dev = usbscpi_init(s_storage, sizeof(s_storage), &cfg);
    if (!dev) {
        fprintf(stderr, "usbscpi_init failed\n");
        pi5_file_cleanup();
        pi5_exec_cleanup();
        pi5_gpio_cleanup();
        return 1;
    }
    if (usbscpi_register(dev, pi5_scpi_commands) != USBSCPI_OK) {
        fprintf(stderr, "usbscpi_register failed\n");
        pi5_file_cleanup();
        pi5_exec_cleanup();
        pi5_gpio_cleanup();
        return 1;
    }

    printf("iotsploit-usb Linux-Pi5 SCPI/TCP server listening on %s:%u\n", bind_addr, port);
    fflush(stdout);

    int rc = usbscpi_socket_serve(dev, bind_addr, port);
    pi5_file_cleanup();
    pi5_exec_cleanup();
    pi5_gpio_cleanup();
    if (rc != 0) {
        fprintf(stderr, "listen on %s:%u failed\n", bind_addr, port);
        return 1;
    }
    return 0;
}
