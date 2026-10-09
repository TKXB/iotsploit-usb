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
typedef size_t (*usbscpi_data_avail_t)(void *user);
typedef size_t (*usbscpi_data_read_t)(void *user, uint8_t *buf, size_t len);

/* ---- Descriptor metadata (SYSTem:HELP:DESCription?) ---- */

typedef struct {
    const char *name;
    const char *type;     /* "u32", "bool", "string", "float" */
    bool required;
    /* Optional "options source". When both queries are non-NULL, this
     * parameter's value is a row index into a result set, so hosts should
     * offer a picker instead of a free-text field. `options_count_query`
     * returns the number of rows (e.g. "BLE:SCAN:COUNt?") and
     * `options_fetch_query` is queried per index for each row's label
     * (e.g. "BLE:SCAN?"). Both NULL for a plain free-text parameter.
     * Emitted after the base fields as `|<count_query>|<fetch_query>`. */
    const char *options_count_query;
    const char *options_fetch_query;
} usbscpi_param_desc_t;

typedef struct {
    const char *pattern;      /* SCPI pattern, e.g. "GPIO:SET" */
    const char *kind;         /* "command", "query", "block" */
    const char *summary;      /* one-line description */
    const usbscpi_param_desc_t *params;
    size_t param_count;
    const char *return_type;  /* "none", "text", "u32", "bool", "block" */
} usbscpi_command_desc_t;

/* An interactive prompt fired when an interactive workflow reaches `state`.
 * Emitted as `prompt=<state>|<kind>|<send_cmd>|<value_query>` (the trailing
 * `|<value_query>` is omitted when NULL). `|` is used instead of `:` because
 * SCPI headers contain `:`. */
typedef struct {
    const char *state;        /* state value at which this prompt fires */
    const char *kind;         /* "passkey", "number", "text", "confirm", "display" */
    const char *send_cmd;     /* SCPI header for the response; NULL/empty for display */
    const char *value_query;  /* optional query whose value is shown to the user; NULL if none */
} usbscpi_prompt_desc_t;

typedef struct {
    const char *name;
    const char *type;         /* "trigger_poll_fetch", "trigger_poll_interactive" */
    const char *summary;
    const char *trigger_cmd;
    const char *done_query;
    const char *done_value;
    const char *count_query;
    const char *fetch_query;
    const char *state_query;       /* interactive only */
    const char *success_value;     /* interactive only */
    const char *const *failed_values;  /* failure state values, NULL if none */
    size_t failed_value_count;
    const usbscpi_prompt_desc_t *prompts;  /* interactive prompts, NULL if none */
    size_t prompt_count;
    /* Interactive only: query issued once the workflow reaches success_value;
     * its response is surfaced to the user (e.g. "BLE:SEC?"). NULL if none.
     * Emitted as `result=<query>`. */
    const char *result_query;
    /* Column schema for each fetch row, emitted verbatim as `fields=<spec>`.
     * Grammar: `<name>:<type>[:<unit>],<name>:<type>[:<unit>],…`
     * NULL when the workflow does not advertise columns; the host then falls
     * back to a single `value:string` column (raw-line behaviour). */
    const char *fields;
    /* Column schema for the optional `result=` query, emitted verbatim as
     * `result_fields=<spec>`. NULL when not advertised. */
    const char *result_fields;
    uint32_t timeout_ms;
    uint32_t poll_ms;
    /* Names this workflow had before. Each is emitted as a full copy of this
     * WF line under the old name plus `renamed=<name>`, so older hosts keep
     * running it and newer ones tell the user the new name. */
    const char *const *renamed_from;
    size_t renamed_from_count;
} usbscpi_workflow_desc_t;

typedef struct {
    const usbscpi_command_desc_t *commands;
    size_t command_count;
    const usbscpi_workflow_desc_t *workflows;
    size_t workflow_count;
} usbscpi_descriptor_t;

/* ---- Declaring commands once ----
 *
 * List a board's commands once; USBSCPI_DEFINE_COMMANDS generates the libscpi
 * table and the descriptor table from it, so the two cannot drift.
 *
 *   static const usbscpi_param_desc_t gpio_params[] = {
 *       USBSCPI_PARAM("pin", "u32", true),
 *       USBSCPI_PARAM("value", "bool", true),
 *   };
 *   #define APP_COMMANDS(CMD, ALIAS)                                         \
 *       CMD("GPIO", cmd_gpio_set, "command", "Set GPIO output level",        \
 *           USBSCPI_PARAMS(gpio_params), "none")                             \
 *       ALIAS("GPIO:SET", cmd_gpio_set)
 *   USBSCPI_DEFINE_COMMANDS(app, APP_COMMANDS);
 *
 * This defines `app_scpi_commands` (for usbscpi_register) and
 * `app_desc_commands` (for usbscpi_descriptor_t.commands; count it with
 * USBSCPI_COUNT). An ALIAS works but is not described: use it for old names
 * kept for compatibility. CMD arguments: header, handler, kind ("command",
 * "query" or "block"), summary, parameters (USBSCPI_PARAMS(array) or
 * USBSCPI_NO_PARAMS) and return type. */

#define USBSCPI_COUNT(array) (sizeof(array) / sizeof((array)[0]))

#define USBSCPI_PARAM(name, type, required) { (name), (type), (required), NULL, NULL }
/* A row index chosen from a job's results, which hosts offer as a picker. */
#define USBSCPI_PARAM_PICK(name, count_query, fetch_query) \
    { (name), "u32", true, (count_query), (fetch_query) }
#define USBSCPI_PARAMS(array) (array), USBSCPI_COUNT(array)
#define USBSCPI_NO_PARAMS NULL, 0

#define USBSCPI_SCPI_CMD_(header, handler, kind, summary, params, returns) { (header), (handler), 0 },
#define USBSCPI_SCPI_ALIAS_(header, handler) { (header), (handler), 0 },
#define USBSCPI_DESC_CMD_(header, handler, kind, summary, params, returns) \
    { (header), (kind), (summary), params, (returns) },
#define USBSCPI_DESC_ALIAS_(header, handler)

#define USBSCPI_DEFINE_COMMANDS(prefix, LIST)                                     \
    static const scpi_command_t prefix##_scpi_commands[] = {                     \
        LIST(USBSCPI_SCPI_CMD_, USBSCPI_SCPI_ALIAS_) SCPI_CMD_LIST_END            \
    };                                                                            \
    static const usbscpi_command_desc_t prefix##_desc_commands[] = {             \
        LIST(USBSCPI_DESC_CMD_, USBSCPI_DESC_ALIAS_)                              \
    }

/* ---- Declaring workflows ----
 *
 * A workflow drives one job (see .agents/standards/scpi-commands.md), so its
 * commands follow from the job's subsystem name, which must be a string
 * literal. The macros expand to designated initializers; wrap them in braces
 * and add the optional parts:
 *
 *   static const usbscpi_workflow_desc_t workflows[] = {
 *       { USBSCPI_WF_ACQUIRE("wifi-scan", "WLAN:SCAN", "Scan for Wi-Fi access points",
 *                            "ssid:string,rssi:i32:dbm,bssid:mac", 15000) },
 *       { USBSCPI_WF_INTERACTIVE("ble-pair", "BLE:PAIR", "Pair with the connected device", 30000),
 *         USBSCPI_WF_PROMPTS(pair_prompts),
 *         USBSCPI_WF_RESULT("BLE:SEC?", "mac:mac,level:u32"),
 *         USBSCPI_WF_RENAMED_FROM(pair_old_names) },
 *   };
 *
 * Acquire: <sub>:STARt, poll <sub>:STATe? until DONE (stop on FAILED), then
 * <sub>:COUNt? and <sub>:FETCh? <index>. Interactive: <sub>:STARt, poll
 * <sub>:STATe? until DONE (stop on FAILED), answering prompts on the way. */

/* The job state that ends a workflow with an error. */
extern const char *const usbscpi_failed_state[1];

#define USBSCPI_WF_ACQUIRE(wf_name, sub, wf_summary, wf_fields, wf_timeout_ms)     \
    .name = (wf_name), .type = "trigger_poll_fetch", .summary = (wf_summary),     \
    .trigger_cmd = sub ":STARt", .done_query = sub ":STATe?", .done_value = "DONE", \
    .count_query = sub ":COUNt?", .fetch_query = sub ":FETCh?",                   \
    .fields = (wf_fields), .failed_values = usbscpi_failed_state,                 \
    .failed_value_count = 1, .timeout_ms = (wf_timeout_ms), .poll_ms = 250

#define USBSCPI_WF_INTERACTIVE(wf_name, sub, wf_summary, wf_timeout_ms)            \
    .name = (wf_name), .type = "trigger_poll_interactive", .summary = (wf_summary), \
    .trigger_cmd = sub ":STARt", .state_query = sub ":STATe?",                    \
    .success_value = "DONE", .failed_values = usbscpi_failed_state,               \
    .failed_value_count = 1, .timeout_ms = (wf_timeout_ms), .poll_ms = 200

#define USBSCPI_WF_PROMPTS(array) .prompts = (array), .prompt_count = USBSCPI_COUNT(array)
#define USBSCPI_WF_RESULT(query, result_spec) .result_query = (query), .result_fields = (result_spec)
#define USBSCPI_WF_RENAMED_FROM(array) \
    .renamed_from = (array), .renamed_from_count = USBSCPI_COUNT(array)

/* Prompts fire on the job states named in the standard. */
#define USBSCPI_PROMPT_PASSKEY(send_cmd) { "PASSKEY", "passkey", (send_cmd), NULL }
#define USBSCPI_PROMPT_CONFIRM(send_cmd, value_query) \
    { "CONFIRM", "confirm", (send_cmd), (value_query) }
#define USBSCPI_PROMPT_DISPLAY(value_query) { "DISPLAY", "display", NULL, (value_query) }

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

    usbscpi_data_avail_t data_avail;
    usbscpi_data_read_t  data_read;
    uint8_t *io_buf;
    size_t io_buf_len;
    unsigned proto;
    size_t mtu;
    const usbscpi_descriptor_t *descriptor;  /* optional, NULL = unsupported */
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
