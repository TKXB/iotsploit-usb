/*
 * iotsploit-usb over TCP on a plain Linux host.
 *
 * This is both a runnable target in its own right — a Raspberry Pi 5 reached
 * over Ethernet is exactly this daemon — and the hardware-free test rig for the
 * Rust host: the whole SCPI device runs on localhost:5025, so descriptor
 * discovery, workflows and block transfers can be exercised without a board.
 *
 * Build:
 *   cmake -S . -B build -DUSBSCPI_BUILD_SOCKET_GLUE=ON
 *   cmake --build build
 *   ./build/examples/linux/usbscpi_linux [bind_addr] [port]
 */

#include <signal.h>
#include <stdio.h>
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

/* A trigger_poll_fetch workflow needs a trigger that completes asynchronously;
 * this one "finishes" one second after it is started. */
static scpi_result_t cmd_scan(scpi_t *scpi) {
    (void)scpi;
    s_scan_done = 0;
    s_scan_count = DEMO_SCAN_MAX;
    s_scan_started = time(NULL);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_scan_done(scpi_t *scpi) {
    if (!s_scan_done && time(NULL) - s_scan_started >= 1) {
        s_scan_done = 1;
    }
    SCPI_ResultUInt32(scpi, (uint32_t)s_scan_done);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_scan_count(scpi_t *scpi) {
    SCPI_ResultUInt32(scpi, (uint32_t)s_scan_count);
    return SCPI_RES_OK;
}

static scpi_result_t cmd_scan_get(scpi_t *scpi) {
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

static const scpi_command_t demo_commands[] = {
    { "GPIO:SET",         cmd_gpio_set,   0 },
    { "GPIO:GET?",        cmd_gpio_get,   0 },
    { "DEMO:SCAN",        cmd_scan,       0 },
    { "DEMO:SCAN:DONE?",  cmd_scan_done,  0 },
    { "DEMO:SCAN:COUNt?", cmd_scan_count, 0 },
    { "DEMO:SCAN?",       cmd_scan_get,   0 },
    { "DEMO:DATA?",       cmd_data_read,  0 },
    SCPI_CMD_LIST_END
};

/* ---------- descriptor (SYSTem:HELP:DESCription?) ---------- */

static const usbscpi_param_desc_t desc_gpio_set_params[] = {
    { "pin", "u32", true }, { "level", "bool", true },
};
static const usbscpi_param_desc_t desc_gpio_get_params[] = {
    { "pin", "u32", true },
};
static const usbscpi_param_desc_t desc_scan_get_params[] = {
    { "index", "u32", true },
};
static const usbscpi_param_desc_t desc_data_read_params[] = {
    { "length", "u32", true },
};

static const usbscpi_command_desc_t desc_commands[] = {
    { "GPIO:SET",         "command", "Set GPIO output level",
      desc_gpio_set_params, 2, "none" },
    { "GPIO:GET?",        "query",   "Read GPIO level",
      desc_gpio_get_params, 1, "u32" },
    { "DEMO:SCAN",        "command", "Start the demo scan",
      NULL, 0, NULL },
    { "DEMO:SCAN:DONE?",  "query",   "1 = scan finished",
      NULL, 0, "bool" },
    { "DEMO:SCAN:COUNt?", "query",   "Number of demo results",
      NULL, 0, "u32" },
    { "DEMO:SCAN?",       "query",   "Get one demo result by index",
      desc_scan_get_params, 1, "string" },
    { "DEMO:DATA?",       "query",   "Read N bytes as a definite-length block",
      desc_data_read_params, 1, "block" },
};

static const usbscpi_workflow_desc_t desc_workflows[] = {
    {
        .name = "demo-scan",
        .type = "trigger_poll_fetch",
        .summary = "Run the demo scan and fetch every row",
        .trigger_cmd = "DEMO:SCAN",
        .done_query = "DEMO:SCAN:DONE?",
        .done_value = "1",
        .count_query = "DEMO:SCAN:COUNt?",
        .fetch_query = "DEMO:SCAN?",
        .fields = "ssid:string,rssi:i32:dbm,channel:u32,authmode:string,bssid:mac",
        .timeout_ms = 10000,
        .poll_ms = 250,
    },
};

static const usbscpi_descriptor_t s_descriptor = {
    .commands = desc_commands,
    .command_count = sizeof(desc_commands) / sizeof(desc_commands[0]),
    .workflows = desc_workflows,
    .workflow_count = sizeof(desc_workflows) / sizeof(desc_workflows[0]),
};

/* ---------- entry point ---------- */

int main(int argc, char **argv) {
    const char *bind_addr = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    port      = (argc > 2) ? (uint16_t)atoi(argv[2]) : 5025;

    /* send() already passes MSG_NOSIGNAL, but a stray SIGPIPE from any other
     * write would still kill the daemon. */
    signal(SIGPIPE, SIG_IGN);

    usbscpi_config_t cfg = {
        .usb_tx        = usbscpi_socket_tx,
        .line_buf      = s_line,
        .line_buf_len  = sizeof(s_line),
        .max_block_len = 4096,
        .idn           = "IoTSploit,linux-demo,0001,0.1.0",
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
    if (usbscpi_register(dev, demo_commands) != USBSCPI_OK) {
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
