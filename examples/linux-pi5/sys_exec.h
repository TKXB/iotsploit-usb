#ifndef LINUX_PI5_SYS_EXEC_H
#define LINUX_PI5_SYS_EXEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Keep payload well inside the 4096-byte TCP SCPI MTU so the IEEE 488.2
 * definite-length block header ("#44000" + payload + "\n") always fits. */
#define PI5_EXEC_BUF_CAP 4000u

/* Default synchronous timeout for SYSTem:EXEC? "<cmd>" (milliseconds). */
#define PI5_EXEC_DEFAULT_TIMEOUT_MS 5000u

typedef enum {
    PI5_EXEC_IDLE = 0,
    PI5_EXEC_RUNNING,
    PI5_EXEC_DONE,
    PI5_EXEC_FAILED,
} pi5_exec_state_t;

/* Spawn `/bin/sh -c <cmd>` asynchronously in its own process group.
 * Stops any previously running command and clears the output buffer.
 * Returns 0 on success, -1 on fork/pipe error. */
int pi5_exec_start(const char *cmd);

/* Run `/bin/sh -c <cmd>` synchronously up to `timeout_ms` milliseconds.
 * Captures stdout+stderr into the internal buffer and records exit code.
 * If the command times out, kills the process group and sets exit code -124.
 * Returns 0 if the command ran (regardless of its exit status), -1 if fork/pipe failed. */
int pi5_exec_run_sync(const char *cmd, uint32_t timeout_ms);

/* Non-blocking poll: drains pipe and reaps child if exited. */
pi5_exec_state_t pi5_exec_poll(void);

/* Stop any running command (SIGTERM -> SIGKILL) and reap it. */
void pi5_exec_stop(void);

/* Stop any running command, clear output buffer, reset state to IDLE. */
void pi5_exec_clear(void);

/* Clean up on server exit. */
void pi5_exec_cleanup(void);

/* Last exit code: 0 on success, 1..255 on non-zero exit, -124 on timeout,
 * or -signum if terminated by signal. */
int32_t pi5_exec_exit_code(void);

/* Pointer and byte length of captured stdout+stderr buffer. */
const uint8_t *pi5_exec_output(size_t *out_len);

/* Returns 1 once per FAILED job transition so STATe? can queue an SCPI error
 * without repeating it on subsequent polls. */
int pi5_exec_take_error_latch(void);

#ifdef __cplusplus
}
#endif

#endif /* LINUX_PI5_SYS_EXEC_H */
