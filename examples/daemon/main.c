/*
 * iotsploit-usb over TCP on a desktop host: Linux or Windows.
 *
 * This is both a runnable target in its own right — a Raspberry Pi 5 reached
 * over Ethernet is exactly this daemon — and the hardware-free test rig for the
 * Rust host: the whole SCPI device runs on localhost:5025, so descriptor
 * discovery, workflows and block transfers can be exercised without a board.
 *
 * Nothing here is OS-specific beyond the SIGPIPE guard below; the socket glue
 * owns the platform differences.
 *
 * Build:
 *   cmake -S . -B build -DUSBSCPI_BUILD_SOCKET_GLUE=ON
 *   cmake --build build
 *   ./build/examples/daemon/usbscpi_daemon [bind_addr] [port]
 */

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "usbscpi/usbscpi.h"
#include "usbscpi_socket.h"

/* ---------- static storage (no dynamic allocation after init) ---------- */

static uint8_t s_storage[2048];
static char    s_line[256];
static uint8_t s_io[4096];

/* ---------- demo device state ---------- */

#define DEMO_SCAN_MAX 8

static uint32_t s_gpio[32];
static int      s_scan_done;
static size_t   s_scan_count;
static time_t   s_scan_started;

/* ---------- command handlers ---------- */

static scpi_result_t cmd_gpio_set(scpi_t *scpi) {
    uint32_t pin = 0, level = 0;
    if (!SCPI_ParamUInt32(scpi, &pin, TRUE) || !SCPI_ParamUInt32(scpi, &level, TRUE)) {
        return SCPI_RES_ERR;
    }
    if (pin >= sizeof(s_gpio) / sizeof(s_gpio[0])) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    s_gpio[pin] = level ? 1u : 0u;
    return SCPI_RES_OK;
}

static scpi_result_t cmd_gpio_get(scpi_t *scpi) {
    uint32_t pin = 0;
    if (!SCPI_ParamUInt32(scpi, &pin, TRUE)) {
        return SCPI_RES_ERR;
    }
    if (pin >= sizeof(s_gpio) / sizeof(s_gpio[0])) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    SCPI_ResultUInt32(scpi, s_gpio[pin]);
    return SCPI_RES_OK;
}

/* DEMO:SCAN is a job (see .agents/standards/scpi-commands.md). It finishes
 * after two ticks of time(), i.e. between one and two seconds after it starts:
 * one tick could pass a millisecond after STARt. The workflow genuinely has to
 * poll, and RUNNING is always observable right after STARt. */
static int s_scan_running;

static void scan_update(void) {
    if (s_scan_running && time(NULL) - s_scan_started >= 2) {
        s_scan_running = 0;
        s_scan_done = 1;
        s_scan_count = DEMO_SCAN_MAX;
    }
}

static scpi_result_t cmd_scan_start(scpi_t *scpi) {
    (void)scpi;
    s_scan_done = 0;
    s_scan_count = 0;
    s_scan_running = 1;
    s_scan_started = time(NULL);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_scan_stop(scpi_t *scpi) {
    (void)scpi;
    scan_update();
    s_scan_running = 0;
    return SCPI_RES_OK;
}

static scpi_result_t cmd_scan_state(scpi_t *scpi) {
    scan_update();
    const char *state = s_scan_running ? "RUNNING" : s_scan_done ? "DONE" : "IDLE";
    SCPI_ResultMnemonic(scpi, state);
    return SCPI_RES_OK;
}

/* Old DEMO:SCAN:DONE?: 1 once the scan has finished. */
static scpi_result_t cmd_scan_done(scpi_t *scpi) {
    scan_update();
    SCPI_ResultUInt32(scpi, (uint32_t)s_scan_done);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_scan_count(scpi_t *scpi) {
    scan_update();
    SCPI_ResultUInt32(scpi, (uint32_t)s_scan_count);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_scan_clear(scpi_t *scpi) {
    (void)scpi;
    s_scan_done = 0;
    s_scan_count = 0;
    return SCPI_RES_OK;
}

static scpi_result_t cmd_scan_fetch(scpi_t *scpi) {
    uint32_t index = 0;
    if (!SCPI_ParamUInt32(scpi, &index, TRUE)) {
        return SCPI_RES_ERR;
    }
    if (index >= s_scan_count) {
        SCPI_ErrorPush(scpi, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    char row[96];
    snprintf(row, sizeof(row), "\"demo-ap-%u\",%d,%u,OPEN,02:00:00:00:00:%02X",
             (unsigned)index, -40 - (int)index, (unsigned)(index % 11u) + 1u,
             (unsigned)index);
    SCPI_ResultCharacters(scpi, row, strlen(row));
    return SCPI_RES_OK;
}

/* Exercises the definite-length block response path over TCP: the payload is
 * deliberately full of bytes that would break a newline-delimited reader. */
static scpi_result_t cmd_data_read(scpi_t *scpi) {
    uint32_t want = 0;
    if (!SCPI_ParamUInt32(scpi, &want, TRUE)) {
        return SCPI_RES_ERR;
    }
    if (want > sizeof(s_io)) {
        want = (uint32_t)sizeof(s_io);
    }
    for (uint32_t i = 0; i < want; i++) {
        s_io[i] = (uint8_t)(i & 0xFFu);
    }
    SCPI_ResultArbitraryBlock(scpi, s_io, want);
    return SCPI_RES_OK;
}

/* ---------- commands and descriptor (SYSTem:HELP:DESCription?) ---------- */

static const usbscpi_param_desc_t gpio_set_params[] = {
    USBSCPI_PARAM("pin", "u32", true),
    USBSCPI_PARAM("value", "bool", true),
};
static const usbscpi_param_desc_t gpio_get_params[] = {
    USBSCPI_PARAM("pin", "u32", true),
};
static const usbscpi_param_desc_t scan_fetch_params[] = {
    USBSCPI_PARAM_PICK("index", "DEMO:SCAN:COUNt?", "DEMO:SCAN:FETCh?"),
};
static const usbscpi_param_desc_t data_read_params[] = {
    USBSCPI_PARAM("length", "u32", true),
};

/* The ALIAS entries are the names used before the command standard; they keep
 * older hosts working and are not described. */
#define DEMO_COMMANDS(CMD, ALIAS)                                                        \
    CMD("GPIO",              cmd_gpio_set,   "command", "Set a GPIO output level",      \
        USBSCPI_PARAMS(gpio_set_params), "none")                                        \
    CMD("GPIO?",             cmd_gpio_get,   "query",   "Read a GPIO level",            \
        USBSCPI_PARAMS(gpio_get_params), "u32")                                         \
    CMD("DEMO:SCAN:STARt",   cmd_scan_start, "command", "Start the demo scan (1 s)",    \
        USBSCPI_NO_PARAMS, "none")                                                      \
    CMD("DEMO:SCAN:STOP",    cmd_scan_stop,  "command", "Stop the demo scan",           \
        USBSCPI_NO_PARAMS, "none")                                                      \
    CMD("DEMO:SCAN:STATe?",  cmd_scan_state, "query",   "IDLE, RUNNING or DONE",        \
        USBSCPI_NO_PARAMS, "string")                                                    \
    CMD("DEMO:SCAN:COUNt?",  cmd_scan_count, "query",   "Number of demo results",       \
        USBSCPI_NO_PARAMS, "u32")                                                       \
    CMD("DEMO:SCAN:FETCh?",  cmd_scan_fetch, "query",   "Demo result by index",         \
        USBSCPI_PARAMS(scan_fetch_params), "string")                                    \
    CMD("DEMO:SCAN:CLEar",   cmd_scan_clear, "command", "Forget the demo results",      \
        USBSCPI_NO_PARAMS, "none")                                                      \
    CMD("DEMO:DATA?",        cmd_data_read,  "block",   "Read N bytes as a binary block", \
        USBSCPI_PARAMS(data_read_params), "block")                                      \
    ALIAS("GPIO:SET",        cmd_gpio_set)                                              \
    ALIAS("GPIO:GET?",       cmd_gpio_get)                                              \
    ALIAS("DEMO:SCAN",       cmd_scan_start)                                            \
    ALIAS("DEMO:SCAN:DONE?", cmd_scan_done)                                             \
    ALIAS("DEMO:SCAN?",      cmd_scan_fetch)
USBSCPI_DEFINE_COMMANDS(demo, DEMO_COMMANDS);

static const usbscpi_workflow_desc_t demo_workflows[] = {
    { USBSCPI_WF_ACQUIRE("demo-scan", "DEMO:SCAN", "Run the demo scan and fetch every row",
                         "ssid:string,rssi:i32:dbm,channel:u32,authmode:string,bssid:mac",
                         10000) },
};

static const usbscpi_descriptor_t s_descriptor = {
    .commands = demo_desc_commands,
    .command_count = USBSCPI_COUNT(demo_desc_commands),
    .workflows = demo_workflows,
    .workflow_count = USBSCPI_COUNT(demo_workflows),
};

/* ---------- entry point ---------- */

int main(int argc, char **argv) {
    const char *bind_addr = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    port      = (argc > 2) ? (uint16_t)atoi(argv[2]) : 5025;

#ifdef SIGPIPE
    /* send() already passes MSG_NOSIGNAL, but a stray SIGPIPE from any other
     * write would still kill the daemon. Windows has no SIGPIPE at all. */
    signal(SIGPIPE, SIG_IGN);
#endif

    usbscpi_config_t cfg = {
        .usb_tx        = usbscpi_socket_tx,
        .line_buf      = s_line,
        .line_buf_len  = sizeof(s_line),
        .max_block_len = 4096,
        .idn           = "IoTSploit,tcp-demo,0001,0.1.0",
        .io_buf        = s_io,
        .io_buf_len    = sizeof(s_io),
        .proto         = 1,
        /* A socket is not limited to a USB endpoint's 64/512 bytes; this caps
         * SYSTem:HELP:HEADers? and block reads. */
        .mtu           = 4096,
        .descriptor    = &s_descriptor,
    };

    usbscpi_t *dev = usbscpi_init(s_storage, sizeof(s_storage), &cfg);
    if (!dev) {
        fprintf(stderr, "usbscpi_init failed\n");
        return 1;
    }
    if (usbscpi_register(dev, demo_scpi_commands) != USBSCPI_OK) {
        fprintf(stderr, "usbscpi_register failed\n");
        return 1;
    }

    printf("iotsploit-usb SCPI/TCP daemon listening on %s:%u\n", bind_addr, port);
    fflush(stdout);

    if (usbscpi_socket_serve(dev, bind_addr, port) != 0) {
        fprintf(stderr, "listen on %s:%u failed\n", bind_addr, port);
        return 1;
    }
    return 0;
}
