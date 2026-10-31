/* rpifw.c — see rpifw.h. */
#include "rpifw.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* a small sysfs file, one line, trailing newline kept for the strict parser; -1 if missing */
static int rd(const char *root, const char *path, char *out, size_t n) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s%s", root, path);
    FILE *f = fopen(p, "re");
    if (!f) return -1;
    size_t len = fread(out, 1, n - 1, f);
    fclose(f);
    out[len] = 0;
    return 0;
}

int hw_is_bcm2712(const char *compat, size_t n) {
    for (size_t i = 0; compat && i < n;) {
        size_t l = strnlen(compat + i, n - i);
        if (l == 12 && !memcmp(compat + i, "brcm,bcm2712", 12)) return 1;
        i += l + 1;
    }
    return 0;
}

int rpifw_parse_throttled(const char *s) {
    if (!s) return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    unsigned long v = 0;
    int n = 0;
    for (; *s && *s != '\n'; s++, n++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (d < 0 || n >= 8) return -1;
        v = v << 4 | (unsigned long)d;
    }
    if (!n || (*s == '\n' && s[1])) return -1;
    return (int)(v & (RPIFW_NOW_MASK | RPIFW_STICKY_MASK));
}

/* the rpi_volt hwmon's under-voltage alarm: exactly "0" or "1" (an optional newline), anything else unknown */
static void volt(const char *root, RpiFw *f) {
    char d[PATH_MAX];
    snprintf(d, sizeof d, "%s/sys/class/hwmon", root);
    DIR *dir = opendir(d);
    if (!dir) return;
    struct dirent *e;
    while (!f->hwmon && (e = readdir(dir))) {
        if (strncmp(e->d_name, "hwmon", 5) || strlen(e->d_name) > 10) continue;
        char p[96], name[32];
        snprintf(p, sizeof p, "/sys/class/hwmon/%s/name", e->d_name);
        if (rd(root, p, name, sizeof name) || (strcmp(name, "rpi_volt\n") && strcmp(name, "rpi_volt"))) continue;
        f->hwmon = 1;
        char v[8];
        snprintf(p, sizeof p, "/sys/class/hwmon/%s/in0_lcrit_alarm", e->d_name);
        if (!rd(root, p, v, sizeof v) && (v[0] == '0' || v[0] == '1') && (!v[1] || (v[1] == '\n' && !v[2])))
            f->uv_alarm = v[0] - '0';
    }
    closedir(dir);
}

/* get_throttled (drivers/firmware/raspberrypi.c of older Raspberry Pi kernels): directly under a platform node
 * or in its "<node>:firmware" child (soc/soc:firmware, axi/axi:firmware, soc@107c000000/soc@107c000000:firmware) */
static void throttled(const char *root, RpiFw *f) {
    char d[PATH_MAX];
    snprintf(d, sizeof d, "%s/sys/devices/platform", root);
    DIR *dir = opendir(d);
    if (!dir) return;
    struct dirent *e;
    while (f->throttled < 0 && (e = readdir(dir))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) > 64) continue;
        char p[256], b[32];
        for (int i = 0; i < 2 && f->throttled < 0; i++) {
            if (i) snprintf(p, sizeof p, "/sys/devices/platform/%s/%s:firmware/get_throttled", e->d_name, e->d_name);
            else snprintf(p, sizeof p, "/sys/devices/platform/%s/get_throttled", e->d_name);
            if (!rd(root, p, b, sizeof b)) f->throttled = rpifw_parse_throttled(b);
        }
    }
    closedir(dir);
}

void rpifw_read(const char *root, RpiFw *f) {
    f->hwmon = 0;
    f->uv_alarm = -1;
    f->throttled = -1;
    volt(root, f);
    throttled(root, f);
}
