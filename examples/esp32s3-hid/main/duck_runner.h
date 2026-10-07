#ifndef DUCK_RUNNER_H
#define DUCK_RUNNER_H

/*
 * Asynchronous, bounded, cancellable script runner.
 *
 * SCPI handlers validate and enqueue; they never type. The runner advances in
 * bounded steps from a dedicated task, using deadlines instead of blocking
 * delays, and checks cancellation between every report and during every wait.
 * A press report and its release report are distinct submissions; the backend
 * owns the actual transfer. HID, clock and RTOS are injected, so the whole
 * state machine runs on the host under tests/test_duckscript.c.
 *
 * Externally visible states match the integration plan's DUCK:STATe? contract.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "duckscript.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DUCK_IDLE = 0,
    DUCK_RUNNING = 1,
    DUCK_DONE = 2,
    DUCK_ERROR = 3,
    DUCK_CANCELLED = 4
} duck_state_t;

/* Submit one boot-protocol keyboard report (modifier byte + up to 6 keycodes).
 * Returns: 1 = accepted/submitted, 0 = busy (retry later, not an error),
 * negative = fatal delivery failure (e.g. detached). */
typedef int (*duck_hid_submit_t)(void *user, uint8_t modifier,
                                 const uint8_t keys[6]);
/* Optional: true when the backend can accept a new report now. NULL = always. */
typedef bool (*duck_hid_ready_t)(void *user);
/* Monotonic milliseconds. */
typedef uint32_t (*duck_clock_t)(void *user);

typedef struct {
    duck_hid_submit_t submit;
    duck_hid_ready_t  ready;     /* may be NULL */
    duck_clock_t      now_ms;
    void             *user;
    uint32_t          max_runtime_ms;  /* total run budget; 0 = use default */
} duck_runner_cfg_t;

#define DUCK_ERR_MAX 64u

typedef struct {
    duck_runner_cfg_t cfg;

    volatile int state;           /* duck_state_t */
    volatile bool stop_requested;
    volatile bool detached;

    const char *script;
    size_t      script_len;

    int      phase;               /* internal RS_* */
    int      term_reason;         /* duck_state_t to settle into after release */
    size_t   cursor;              /* byte offset of next unread line */
    uint32_t line_no;             /* 1-based current/last line; 0 before a run */
    size_t   emit_idx;            /* STRING: next character index */
    duck_line_t cur;              /* line currently being emitted */

    uint8_t  pend_mod;            /* report currently being delivered */
    uint8_t  pend_key;

    uint32_t start_ms;
    uint32_t delay_deadline;

    char     err[DUCK_ERR_MAX];
} duck_runner_t;

/* Initialise with a backend/clock config. Leaves the runner idle. */
void duck_runner_init(duck_runner_t *r, const duck_runner_cfg_t *cfg);

/* Validate and begin running `script` (caller keeps it alive for the run).
 * Resets terminal state atomically. Returns 0 if the run was accepted, or
 * negative if rejected (already running, or the script failed validation — the
 * runner then reports DUCK_ERROR with the failing line and reason). */
int duck_runner_start(duck_runner_t *r, const char *script, size_t len);

/* Request cancellation. Idempotent; a no-op when not running. */
void duck_runner_stop(duck_runner_t *r);

/* USB detached mid-run: discard queued work and end with a delivery error.
 * No further reports can be delivered. */
void duck_runner_detach(duck_runner_t *r);

/* Advance bounded work. Call repeatedly from the runner task. */
void duck_runner_poll(duck_runner_t *r);

duck_state_t duck_runner_state(const duck_runner_t *r);
uint32_t     duck_runner_line(const duck_runner_t *r);
const char  *duck_runner_error(const duck_runner_t *r);

#ifdef __cplusplus
}
#endif

#endif /* DUCK_RUNNER_H */
