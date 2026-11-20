/*
 * Linux GPIO character device v2 backend for Raspberry Pi 5 (RP1) and
 * general Linux SBCs. Zero external dependencies — talks directly to
 * <linux/gpio.h> ioctls (Linux 5.10+).
 */

#include "gpio_chardev.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
    int             fd;
    pi5_gpio_dir_t  dir;
    pi5_gpio_pull_t pull;
    uint8_t         out_level;
} gpio_line_state_t;

static int               s_chip_fd = -1;
static char              s_chip_path[64];
static char              s_chip_label[64];
static uint32_t          s_num_lines;
static gpio_line_state_t s_lines[PI5_GPIO_MAX_PINS];

static void reset_line_table(void) {
    for (size_t i = 0; i < PI5_GPIO_MAX_PINS; i++) {
        s_lines[i].fd        = -1;
        s_lines[i].dir       = PI5_GPIO_DIR_FREE;
        s_lines[i].pull      = PI5_GPIO_PULL_NONE;
        s_lines[i].out_level = 0;
    }
}

static uint64_t pull_to_flags(pi5_gpio_pull_t pull) {
    switch (pull) {
    case PI5_GPIO_PULL_UP:   return GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    case PI5_GPIO_PULL_DOWN: return GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    case PI5_GPIO_PULL_NONE:
    default:                 return GPIO_V2_LINE_FLAG_BIAS_DISABLED;
    }
}

static int pin_valid(uint32_t pin) {
    if (s_chip_fd < 0) return 0;
    if (pin >= PI5_GPIO_MAX_PINS) return 0;
    if (s_num_lines > 0 && pin >= s_num_lines) return 0;
    return 1;
}

static int open_chip_at(const char *path, struct gpiochip_info *out_info) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    struct gpiochip_info info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0) {
        close(fd);
        return -1;
    }
    if (out_info) {
        *out_info = info;
    }
    return fd;
}

static int request_line(uint32_t pin, pi5_gpio_dir_t dir, uint8_t initial_out) {
    if (!pin_valid(pin)) {
        return -1;
    }

    gpio_line_state_t *st = &s_lines[pin];
    if (st->fd >= 0) {
        close(st->fd);
        st->fd = -1;
    }

    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof(req));
    req.offsets[0] = pin;
    req.num_lines  = 1;
    snprintf(req.consumer, sizeof(req.consumer), "iotsploit-scpi");

    uint64_t flags = pull_to_flags(st->pull);
    if (dir == PI5_GPIO_DIR_OUT) {
        flags |= GPIO_V2_LINE_FLAG_OUTPUT;
        req.config.num_attrs = 1;
        req.config.attrs[0].attr.id     = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        req.config.attrs[0].attr.values = initial_out ? 1ULL : 0ULL;
        req.config.attrs[0].mask        = 1ULL;
    } else {
        flags |= GPIO_V2_LINE_FLAG_INPUT;
    }
    req.config.flags = flags;

    if (ioctl(s_chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        st->dir = PI5_GPIO_DIR_FREE;
        return -1;
    }

    st->fd        = req.fd;
    st->dir       = dir;
    st->out_level = initial_out ? 1u : 0u;
    return 0;
}

int pi5_gpio_init(const char *chip_path) {
    pi5_gpio_cleanup();
    reset_line_table();

    struct gpiochip_info info;
    memset(&info, 0, sizeof(info));

    if (chip_path && chip_path[0] != '\0') {
        int fd = open_chip_at(chip_path, &info);
        if (fd < 0) {
            return -1;
        }
        s_chip_fd   = fd;
        s_num_lines = info.lines;
        snprintf(s_chip_path, sizeof(s_chip_path), "%s", chip_path);
        snprintf(s_chip_label, sizeof(s_chip_label), "%s", info.label);
        return 0;
    }

    /* Auto-discover Raspberry Pi 5 RP1 GPIO controller ("pinctrl-rp1"). */
    for (unsigned i = 0; i < 16; i++) {
        char candidate[32];
        snprintf(candidate, sizeof(candidate), "/dev/gpiochip%u", i);
        int fd = open_chip_at(candidate, &info);
        if (fd < 0) {
            continue;
        }
        if (strstr(info.label, "pinctrl-rp1") != NULL) {
            s_chip_fd   = fd;
            s_num_lines = info.lines;
            snprintf(s_chip_path, sizeof(s_chip_path), "%s", candidate);
            snprintf(s_chip_label, sizeof(s_chip_label), "%s", info.label);
            return 0;
        }
        close(fd);
    }

    return -1;
}

void pi5_gpio_cleanup(void) {
    for (size_t i = 0; i < PI5_GPIO_MAX_PINS; i++) {
        if (s_lines[i].fd >= 0) {
            close(s_lines[i].fd);
            s_lines[i].fd = -1;
        }
        s_lines[i].dir = PI5_GPIO_DIR_FREE;
    }
    if (s_chip_fd >= 0) {
        close(s_chip_fd);
        s_chip_fd = -1;
    }
    s_chip_path[0]  = '\0';
    s_chip_label[0] = '\0';
    s_num_lines     = 0;
}

const char *pi5_gpio_chip_path(void) {
    return (s_chip_fd >= 0) ? s_chip_path : NULL;
}

const char *pi5_gpio_chip_label(void) {
    return (s_chip_fd >= 0) ? s_chip_label : "";
}

uint32_t pi5_gpio_num_lines(void) {
    return s_num_lines;
}

int pi5_gpio_set(uint32_t pin, uint32_t level) {
    if (!pin_valid(pin)) {
        return -1;
    }
    uint8_t val = level ? 1u : 0u;
    gpio_line_state_t *st = &s_lines[pin];
    if (st->fd < 0 || st->dir != PI5_GPIO_DIR_OUT) {
        return request_line(pin, PI5_GPIO_DIR_OUT, val);
    }

    struct gpio_v2_line_values vals;
    memset(&vals, 0, sizeof(vals));
    vals.bits = val ? 1ULL : 0ULL;
    vals.mask = 1ULL;
    if (ioctl(st->fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0) {
        return -1;
    }
    st->out_level = val;
    return 0;
}

int pi5_gpio_get(uint32_t pin, uint32_t *out_level) {
    if (!pin_valid(pin) || !out_level) {
        return -1;
    }
    gpio_line_state_t *st = &s_lines[pin];
    if (st->fd < 0) {
        if (request_line(pin, PI5_GPIO_DIR_IN, 0) != 0) {
            return -1;
        }
    }

    struct gpio_v2_line_values vals;
    memset(&vals, 0, sizeof(vals));
    vals.mask = 1ULL;
    if (ioctl(st->fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
        return -1;
    }
    *out_level = (vals.bits & 1ULL) ? 1u : 0u;
    return 0;
}

int pi5_gpio_set_dir(uint32_t pin, pi5_gpio_dir_t dir) {
    if (!pin_valid(pin)) {
        return -1;
    }
    if (dir == PI5_GPIO_DIR_FREE) {
        return pi5_gpio_release(pin);
    }
    return request_line(pin, dir, s_lines[pin].out_level);
}

pi5_gpio_dir_t pi5_gpio_get_dir(uint32_t pin) {
    if (pin >= PI5_GPIO_MAX_PINS) {
        return PI5_GPIO_DIR_FREE;
    }
    return s_lines[pin].dir;
}

int pi5_gpio_set_pull(uint32_t pin, pi5_gpio_pull_t pull) {
    if (!pin_valid(pin)) {
        return -1;
    }
    s_lines[pin].pull = pull;
    if (s_lines[pin].fd >= 0 && s_lines[pin].dir != PI5_GPIO_DIR_FREE) {
        return request_line(pin, s_lines[pin].dir, s_lines[pin].out_level);
    }
    return 0;
}

int pi5_gpio_release(uint32_t pin) {
    if (pin >= PI5_GPIO_MAX_PINS) {
        return -1;
    }
    if (s_lines[pin].fd >= 0) {
        close(s_lines[pin].fd);
        s_lines[pin].fd = -1;
    }
    s_lines[pin].dir = PI5_GPIO_DIR_FREE;
    return 0;
}
