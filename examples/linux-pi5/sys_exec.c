/*
 * Linux shell command execution helper for examples/linux-pi5.
 *
 * Uses fork() + pipe() + poll() + process-group kill (-pid) with static
 * buffers (no heap allocation). Supports both synchronous bounded-timeout
 * execution (SYSTem:EXEC?) and asynchronous job polling (SYSTem:EXEC:STARt).
 */

#define _POSIX_C_SOURCE 200809L

#include "sys_exec.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t            s_pid     = -1;
static int              s_pipe_fd = -1;
static pi5_exec_state_t s_state   = PI5_EXEC_IDLE;
static int32_t          s_exit_code;
static int              s_err_latched;
static uint8_t          s_out_buf[PI5_EXEC_BUF_CAP];
static size_t           s_out_len;

static uint64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000UL);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

static void drain_pipe(void) {
    if (s_pipe_fd < 0) {
        return;
    }
    for (;;) {
        uint8_t discard[256];
        uint8_t *dst;
        size_t   cap;

        if (s_out_len < sizeof(s_out_buf)) {
            dst = s_out_buf + s_out_len;
            cap = sizeof(s_out_buf) - s_out_len;
        } else {
            /* Buffer full: keep draining the pipe so the child never blocks. */
            dst = discard;
            cap = sizeof(discard);
        }

        ssize_t n = read(s_pipe_fd, dst, cap);
        if (n > 0) {
            if (dst != discard) {
                s_out_len += (size_t)n;
            }
            continue;
        }
        if (n == 0) {
            close(s_pipe_fd);
            s_pipe_fd = -1;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        /* EAGAIN / EWOULDBLOCK or error */
        break;
    }
}

static void close_pipe_if_open(void) {
    if (s_pipe_fd >= 0) {
        drain_pipe();
        if (s_pipe_fd >= 0) {
            close(s_pipe_fd);
            s_pipe_fd = -1;
        }
    }
}

pi5_exec_state_t pi5_exec_poll(void) {
    drain_pipe();

    if (s_pid > 0) {
        int status = 0;
        pid_t rc = waitpid(s_pid, &status, WNOHANG);
        if (rc == s_pid) {
            close_pipe_if_open();
            s_pid = -1;
            if (WIFEXITED(status)) {
                s_exit_code = (int32_t)WEXITSTATUS(status);
                if (s_exit_code == 0) {
                    s_state = PI5_EXEC_DONE;
                } else {
                    s_state       = PI5_EXEC_FAILED;
                    s_err_latched = 1;
                }
            } else if (WIFSIGNALED(status)) {
                s_exit_code   = -(int32_t)WTERMSIG(status);
                s_state       = PI5_EXEC_FAILED;
                s_err_latched = 1;
            } else {
                s_exit_code   = -1;
                s_state       = PI5_EXEC_FAILED;
                s_err_latched = 1;
            }
        } else if (rc < 0 && errno == ECHILD) {
            close_pipe_if_open();
            s_pid   = -1;
            s_state = PI5_EXEC_DONE;
        }
    }
    return s_state;
}

void pi5_exec_stop(void) {
    if (s_pid > 0) {
        (void)kill(-s_pid, SIGTERM);
        for (int i = 0; i < 10; i++) {
            int status = 0;
            pid_t rc = waitpid(s_pid, &status, WNOHANG);
            if (rc == s_pid || (rc < 0 && errno == ECHILD)) {
                s_pid = -1;
                break;
            }
            sleep_ms(10);
        }
        if (s_pid > 0) {
            (void)kill(-s_pid, SIGKILL);
            int status = 0;
            (void)waitpid(s_pid, &status, 0);
            s_pid = -1;
        }
    }
    close_pipe_if_open();
    if (s_state == PI5_EXEC_RUNNING) {
        s_exit_code = -(int32_t)SIGTERM;
        s_state     = PI5_EXEC_DONE;
    }
}

void pi5_exec_clear(void) {
    pi5_exec_stop();
    s_out_len     = 0;
    s_exit_code   = 0;
    s_err_latched = 0;
    s_state       = PI5_EXEC_IDLE;
}

void pi5_exec_cleanup(void) {
    pi5_exec_clear();
}

int pi5_exec_start(const char *cmd) {
    if (!cmd || cmd[0] == '\0') {
        return -1;
    }

    pi5_exec_clear();

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: own process group so timeout/stop kills the whole subshell tree. */
        (void)setpgid(0, 0);
        close(pipefd[0]);

        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        (void)dup2(pipefd[1], STDOUT_FILENO);
        (void)dup2(pipefd[1], STDERR_FILENO);
        if (pipefd[1] > STDERR_FILENO) {
            close(pipefd[1]);
        }

        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent */
    (void)setpgid(pid, pid);
    close(pipefd[1]);

    int fl = fcntl(pipefd[0], F_GETFL, 0);
    if (fl >= 0) {
        (void)fcntl(pipefd[0], F_SETFL, fl | O_NONBLOCK);
    }
    int fdfl = fcntl(pipefd[0], F_GETFD, 0);
    if (fdfl >= 0) {
        (void)fcntl(pipefd[0], F_SETFD, fdfl | FD_CLOEXEC);
    }

    s_pid         = pid;
    s_pipe_fd     = pipefd[0];
    s_state       = PI5_EXEC_RUNNING;
    s_exit_code   = 0;
    s_out_len     = 0;
    s_err_latched = 0;
    return 0;
}

int pi5_exec_run_sync(const char *cmd, uint32_t timeout_ms) {
    if (pi5_exec_start(cmd) != 0) {
        return -1;
    }
    if (timeout_ms == 0) {
        timeout_ms = PI5_EXEC_DEFAULT_TIMEOUT_MS;
    }

    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    while (pi5_exec_poll() == PI5_EXEC_RUNNING) {
        uint64_t now = now_ms();
        if (now >= deadline) {
            /* Timed out: kill process group and mark -124. */
            if (s_pid > 0) {
                (void)kill(-s_pid, SIGKILL);
                int status = 0;
                (void)waitpid(s_pid, &status, 0);
                s_pid = -1;
            }
            close_pipe_if_open();
            s_exit_code = -124;
            s_state     = PI5_EXEC_FAILED;
            break;
        }

        uint64_t rem = deadline - now;
        int wait_ms  = (rem > 25ULL) ? 25 : (int)rem;
        if (s_pipe_fd >= 0) {
            struct pollfd pfd;
            pfd.fd      = s_pipe_fd;
            pfd.events  = POLLIN | POLLHUP;
            pfd.revents = 0;
            (void)poll(&pfd, 1, wait_ms);
        } else {
            sleep_ms((uint32_t)wait_ms);
        }
    }

    /* Synchronous query returns output directly; clear the async error latch. */
    s_err_latched = 0;
    return 0;
}

int32_t pi5_exec_exit_code(void) {
    (void)pi5_exec_poll();
    return s_exit_code;
}

const uint8_t *pi5_exec_output(size_t *out_len) {
    (void)pi5_exec_poll();
    if (out_len) {
        *out_len = s_out_len;
    }
    return s_out_buf;
}

int pi5_exec_take_error_latch(void) {
    int latched   = s_err_latched;
    s_err_latched = 0;
    return latched;
}
