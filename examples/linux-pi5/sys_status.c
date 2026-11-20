/*
 * Read-only Linux system status helpers (/proc, /sys, uname, getifaddrs).
 * Zero external dependencies.
 */

#include "sys_status.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/utsname.h>

static int read_first_line(const char *path, char *buf, size_t len) {
    if (!buf || len == 0) return -1;
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    size_t n = fread(buf, 1, len - 1, fp);
    fclose(fp);
    if (n == 0) return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == '\0' || buf[n - 1] == ' ')) {
        buf[--n] = '\0';
    }
    return (n > 0) ? 0 : -1;
}

void pi5_sys_serial(char *buf, size_t len) {
    if (!buf || len == 0) return;
    char tmp[64];
    if (read_first_line("/proc/device-tree/serial-number", tmp, sizeof(tmp)) == 0) {
        snprintf(buf, len, "%s", tmp);
        return;
    }
    if (read_first_line("/etc/machine-id", tmp, sizeof(tmp)) == 0) {
        if (strlen(tmp) > 16) tmp[16] = '\0';
        snprintf(buf, len, "%s", tmp);
        return;
    }
    snprintf(buf, len, "0001");
}

int pi5_sys_uptime(uint64_t *out_secs) {
    if (!out_secs) return -1;
    char line[64];
    if (read_first_line("/proc/uptime", line, sizeof(line)) != 0) {
        return -1;
    }
    double up = 0.0;
    if (sscanf(line, "%lf", &up) != 1 || up < 0.0) {
        return -1;
    }
    *out_secs = (uint64_t)up;
    return 0;
}

int pi5_sys_temp_mc(int32_t *out_millic) {
    if (!out_millic) return -1;
    char line[32];
    if (read_first_line("/sys/class/thermal/thermal_zone0/temp", line, sizeof(line)) != 0) {
        return -1;
    }
    char *end = NULL;
    long v = strtol(line, &end, 10);
    if (end == line) {
        return -1;
    }
    *out_millic = (int32_t)v;
    return 0;
}

int pi5_sys_memory_kb(uint64_t *out_total_kb, uint64_t *out_avail_kb) {
    if (!out_total_kb || !out_avail_kb) return -1;
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;

    uint64_t total = 0, avail = 0;
    int got_total = 0, got_avail = 0;
    char line[128];
    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned long long val = 0;
        if (!got_total && sscanf(line, "MemTotal: %llu kB", &val) == 1) {
            total = (uint64_t)val;
            got_total = 1;
        } else if (!got_avail && sscanf(line, "MemAvailable: %llu kB", &val) == 1) {
            avail = (uint64_t)val;
            got_avail = 1;
        }
        if (got_total && got_avail) break;
    }
    fclose(fp);
    if (!got_total || !got_avail) return -1;
    *out_total_kb = total;
    *out_avail_kb = avail;
    return 0;
}

int pi5_sys_load(char *buf, size_t len) {
    if (!buf || len == 0) return -1;
    char line[96];
    if (read_first_line("/proc/loadavg", line, sizeof(line)) != 0) {
        return -1;
    }
    char l1[16], l5[16], l15[16];
    if (sscanf(line, "%15s %15s %15s", l1, l5, l15) != 3) {
        return -1;
    }
    int n = snprintf(buf, len, "%s,%s,%s", l1, l5, l15);
    return (n > 0 && (size_t)n < len) ? 0 : -1;
}

int pi5_sys_host(char *buf, size_t len) {
    if (!buf || len == 0) return -1;
    struct utsname u;
    if (uname(&u) != 0) {
        return -1;
    }
    int n = snprintf(buf, len, "%s,%s,%s", u.nodename, u.release, u.machine);
    return (n > 0 && (size_t)n < len) ? 0 : -1;
}

static int valid_ifname(const char *s) {
    if (!s || s[0] == '\0') return 0;
    size_t n = strlen(s);
    if (n > 31) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) {
            return 0;
        }
    }
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return 0;
    return 1;
}

int pi5_sys_net(const char *ifname, char *buf, size_t len) {
    if (!buf || len == 0 || !valid_ifname(ifname)) {
        return -1;
    }

    char path[96];
    char state[32];
    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ifname);
    if (read_first_line(path, state, sizeof(state)) != 0) {
        return -1;
    }
    for (size_t i = 0; state[i] != '\0'; i++) {
        state[i] = (char)toupper((unsigned char)state[i]);
    }

    char ip[INET_ADDRSTRLEN] = "NONE";
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == 0) {
        for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || !ifa->ifa_addr) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;
            if (strcmp(ifa->ifa_name, ifname) != 0) continue;
            const struct sockaddr_in *sin = (const struct sockaddr_in *)ifa->ifa_addr;
            if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)) != NULL) {
                break;
            }
        }
        freeifaddrs(ifaddr);
    }

    int n = snprintf(buf, len, "%s,%s", state, ip);
    return (n > 0 && (size_t)n < len) ? 0 : -1;
}
