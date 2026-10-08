/* Real socket lifecycle rig for the HID application's portable state.
 * No USB or threading: scripts advance when state is queried. */
#include <stdio.h>
#include <stdlib.h>
#include "duck_runner.h"
#include "duck_store.h"
#include "usbscpi_socket.h"
static duck_store_t store;
static duck_runner_t runner;
static int owner, next_owner;
static uint32_t clock_ms(void *u) { (void)u; return 0; }
static int submit(void *u, uint8_t mod, const uint8_t keys[6]) {
    (void)u; (void)mod; (void)keys; return 1;
}
static void session(void *u, bool connected) {
    (void)u;
    if (connected) owner = ++next_owner;
    else { duck_store_abort(&store, owner); duck_runner_stop(&runner); owner = 0; }
}
static int begin(void *u, size_t n) { (void)u; return duck_store_block_begin(&store, owner, n); }
static int data(void *u, const uint8_t *p, size_t n) { (void)u; return duck_store_block_data(&store, p, n); }
static int end(void *u, size_t n) { (void)u; return duck_store_block_end(&store, n); }
static scpi_result_t reserve(scpi_t *ctx) {
    uint32_t n;
    if (!SCPI_ParamUInt32(ctx, &n, TRUE)) return SCPI_RES_ERR;
    if (duck_store_reserve(&store, owner, n)) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_SETTINGS_CONFLICT); return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}
static scpi_result_t run(scpi_t *ctx) {
    if (duck_runner_start(&runner, "DELAY 5000\nENTER", 16)) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR); return SCPI_RES_ERR;
    }
    return SCPI_RES_OK;
}
static scpi_result_t stop(scpi_t *ctx) { (void)ctx; duck_runner_stop(&runner); return SCPI_RES_OK; }
static scpi_result_t state(scpi_t *ctx) {
    duck_runner_poll(&runner);
    SCPI_ResultUInt32(ctx, (uint32_t)duck_runner_state(&runner)); return SCPI_RES_OK;
}
static scpi_result_t upload_state(scpi_t *ctx) {
    SCPI_ResultUInt32(ctx, (uint32_t)duck_store_upload_state(&store)); return SCPI_RES_OK;
}
static const scpi_command_t commands[] = {
    { "DUCK:UPLoad:STARt", reserve, 0 }, { "DUCK:RUN", run, 0 },
    { "DUCK:STOP", stop, 0 }, { "DUCK:STATe?", state, 0 },
    { "DUCK:UPLoad:STATe?", upload_state, 0 }, SCPI_CMD_LIST_END
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    duck_store_init(&store);
    duck_runner_cfg_t rcfg = { .submit = submit, .now_ms = clock_ms };
    duck_runner_init(&runner, &rcfg);
    uint8_t storage[2048], io[8192]; char line[256];
    usbscpi_config_t cfg = {
        .usb_tx = usbscpi_socket_tx, .on_block_begin = begin,
        .on_block_data = data, .on_block_end = end,
        .line_buf = line, .line_buf_len = sizeof(line),
        .io_buf = io, .io_buf_len = sizeof(io), .max_block_len = DUCK_SCRIPT_MAX
    };
    usbscpi_t *dev = usbscpi_init(storage, sizeof(storage), &cfg);
    if (!dev || usbscpi_register(dev, commands) != USBSCPI_OK) return 2;
    return usbscpi_socket_serve_sessions(dev, "127.0.0.1", (uint16_t)atoi(argv[1]), session, NULL);
}
