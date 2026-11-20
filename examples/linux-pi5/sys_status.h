#ifndef LINUX_PI5_SYS_STATUS_H
#define LINUX_PI5_SYS_STATUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Board serial from /proc/device-tree/serial-number, falling back to
 * /etc/machine-id or "0001". Always NUL-terminates `buf`. */
void pi5_sys_serial(char *buf, size_t len);

/* System uptime in whole seconds from /proc/uptime. Returns 0 on success. */
int pi5_sys_uptime(uint64_t *out_secs);

/* SoC temperature in millidegrees C from /sys/class/thermal/thermal_zone0/temp. */
int pi5_sys_temp_mc(int32_t *out_millic);

/* MemTotal and MemAvailable in KiB from /proc/meminfo. */
int pi5_sys_memory_kb(uint64_t *out_total_kb, uint64_t *out_avail_kb);

/* "<1m>,<5m>,<15m>" load averages from /proc/loadavg. */
int pi5_sys_load(char *buf, size_t len);

/* "<hostname>,<kernel>,<arch>" from uname(). */
int pi5_sys_host(char *buf, size_t len);

/* "<STATE>,<IPv4>" for `ifname` (e.g. "UP,10.8.0.10"). */
int pi5_sys_net(const char *ifname, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LINUX_PI5_SYS_STATUS_H */
