#ifndef LINUX_PI5_GPIO_CHARDEV_H
#define LINUX_PI5_GPIO_CHARDEV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PI5_GPIO_MAX_PINS 64u

typedef enum {
    PI5_GPIO_DIR_FREE = 0,
    PI5_GPIO_DIR_IN,
    PI5_GPIO_DIR_OUT,
} pi5_gpio_dir_t;

typedef enum {
    PI5_GPIO_PULL_NONE = 0,
    PI5_GPIO_PULL_UP,
    PI5_GPIO_PULL_DOWN,
} pi5_gpio_pull_t;

/* Open a GPIO character device. If `chip_path` is NULL or empty, scans
 * /dev/gpiochip0..15 for a chip whose label contains "pinctrl-rp1" (Pi 5 RP1).
 * Returns 0 on success, -1 if no matching chip could be opened. */
int pi5_gpio_init(const char *chip_path);

/* Release all requested lines and close the chip fd. */
void pi5_gpio_cleanup(void);

/* Path of the opened chip, or NULL if no chip is open. */
const char *pi5_gpio_chip_path(void);

/* Label reported by GPIO_GET_CHIPINFO_IOCTL, or "" if none. */
const char *pi5_gpio_chip_label(void);

/* Number of lines reported by the opened chip (0 if not open). */
uint32_t pi5_gpio_num_lines(void);

/* Configure pin as output (if not already) and drive `level` (0/1). */
int pi5_gpio_set(uint32_t pin, uint32_t level);

/* Read pin level (requests as input if currently FREE). */
int pi5_gpio_get(uint32_t pin, uint32_t *out_level);

/* Explicitly switch pin direction to IN or OUT. */
int pi5_gpio_set_dir(uint32_t pin, pi5_gpio_dir_t dir);

/* Current direction of `pin` (PI5_GPIO_DIR_FREE if unclaimed or out of range). */
pi5_gpio_dir_t pi5_gpio_get_dir(uint32_t pin);

/* Configure pull bias (NONE / UP / DOWN); re-requests line if active. */
int pi5_gpio_set_pull(uint32_t pin, pi5_gpio_pull_t pull);

/* Close the line request fd for `pin` so other Linux tools can claim it. */
int pi5_gpio_release(uint32_t pin);

#ifdef __cplusplus
}
#endif

#endif /* LINUX_PI5_GPIO_CHARDEV_H */
