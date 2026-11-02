/* lbcond.c — see lbcond.h. */
#include "lbcond.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* a small sysfs/procfs file, one line, trailing newline cut; -1 if missing */
static int rd(const char *root, const char *path, char *out, size_t n) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s%s", root, path);
    FILE *f = fopen(p, "re");
    if (!f) return -1;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return 0;
}
static long rdl(const char *root, const char *path, long dflt) {
    char b[64];
    if (rd(root, path, b, sizeof b) || !b[0]) return dflt;
    char *e;
    long v = strtol(b, &e, 10);
    return e == b ? dflt : v;
}

static void thermal(const char *root, LbCond *c) {
    char d[PATH_MAX];
    snprintf(d, sizeof d, "%s/sys/class/thermal", root);
    DIR *dir = opendir(d);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (strncmp(e->d_name, "thermal_zone", 12) || strlen(e->d_name) > 20) continue;
        char p[96], type[24] = "";
        snprintf(p, sizeof p, "/sys/class/thermal/%s/type", e->d_name);
        rd(root, p, type, sizeof type);
        snprintf(p, sizeof p, "/sys/class/thermal/%s/temp", e->d_name);
        long t = rdl(root, p, LONG_MIN);
        if (t == LONG_MIN || t < -50000 || t > 150000) continue;
        double tc = t / 1000.0;
        if (tc > c->temp_c) { c->temp_c = tc; snprintf(c->zone, sizeof c->zone, "%s", type); }
        for (int i = 0; i < 16; i++) {
            char tt[24] = "";
            snprintf(p, sizeof p, "/sys/class/thermal/%s/trip_point_%d_type", e->d_name, i);
            if (rd(root, p, tt, sizeof tt)) break;
            snprintf(p, sizeof p, "/sys/class/thermal/%s/trip_point_%d_temp", e->d_name, i);
            long v = rdl(root, p, 0);
            if (v < 30000 || v > 130000) continue;       /* no trip, or a placeholder */
            double vc = v / 1000.0;
            if (!strcmp(tt, "passive") && (!c->passive_c || vc < c->passive_c)) c->passive_c = vc;
            if (!strcmp(tt, "critical") && (!c->critical_c || vc < c->critical_c)) c->critical_c = vc;
        }
    }
    closedir(dir);
}

/* without thermal zones (many desktops), the CPU's hwmon sensors: coretemp, k10temp, zenpower */
static void hwmon(const char *root, LbCond *c) {
    char d[PATH_MAX];
    snprintf(d, sizeof d, "%s/sys/class/hwmon", root);
    DIR *dir = opendir(d);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (strncmp(e->d_name, "hwmon", 5) || strlen(e->d_name) > 10) continue;
        char p[96], name[24] = "";
        snprintf(p, sizeof p, "/sys/class/hwmon/%s/name", e->d_name);
        if (rd(root, p, name, sizeof name)) continue;
        if (strcmp(name, "coretemp") && strcmp(name, "k10temp") && strcmp(name, "zenpower")) continue;
        for (int i = 1; i < 64; i++) {
            snprintf(p, sizeof p, "/sys/class/hwmon/%s/temp%d_input", e->d_name, i);
            long t = rdl(root, p, LONG_MIN);
            if (t == LONG_MIN) { if (i > 2) break; continue; }
            if (t < -50000 || t > 150000) continue;
            if (t / 1000.0 > c->temp_c) { c->temp_c = t / 1000.0; snprintf(c->zone, sizeof c->zone, "%s", name); }
            snprintf(p, sizeof p, "/sys/class/hwmon/%s/temp%d_crit", e->d_name, i);
            long cr = rdl(root, p, 0);
            if (cr >= 30000 && cr <= 130000 && (!c->critical_c || cr / 1000.0 < c->critical_c)) c->critical_c = cr / 1000.0;
            snprintf(p, sizeof p, "/sys/class/hwmon/%s/temp%d_max", e->d_name, i);
            long mx = rdl(root, p, 0);
            if (mx >= 30000 && mx <= 130000 && (!c->passive_c || mx / 1000.0 < c->passive_c)) c->passive_c = mx / 1000.0;
        }
    }
    closedir(dir);
}

static void cpus(const char *root, LbCond *c) {
    char d[PATH_MAX];
    snprintf(d, sizeof d, "%s/sys/devices/system/cpu", root);
    DIR *dir = opendir(d);
    if (!dir) return;
    struct dirent *e;
    double sum = 0;
    int n = 0;
    while ((e = readdir(dir))) {
        if (strncmp(e->d_name, "cpu", 3) || !e->d_name[3] || strlen(e->d_name) > 12 || strspn(e->d_name + 3, "0123456789") != strlen(e->d_name + 3)) continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/%s/cpufreq/scaling_cur_freq", e->d_name);
        long f = rdl(root, p, 0);
        if (f > 0) { sum += f / 1000.0; n++; }
        snprintf(p, sizeof p, "/sys/devices/system/cpu/%s/cpufreq/cpuinfo_max_freq", e->d_name);
        long m = rdl(root, p, 0);
        if (m / 1000.0 > c->freq_max_mhz) c->freq_max_mhz = m / 1000.0;
        static const char *const cnt[] = { "core_throttle_count", "package_throttle_count" };
        for (int i = 0; i < 2; i++) {
            snprintf(p, sizeof p, "/sys/devices/system/cpu/%s/thermal_throttle/%s", e->d_name, cnt[i]);
            long v = rdl(root, p, -1);
            if (v >= 0) { c->throttle_events += (unsigned long)v; c->throttle_known = 1; }
        }
    }
    closedir(dir);
    if (n) c->freq_mhz = sum / n;
    rd(root, "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", c->governor, sizeof c->governor);
}

/* the Pi firmware: rpi_volt's alarm and get_throttled (hw/rpifw.c, shared with System Information), or the
 * device tree */
static void rpi(const char *root, LbCond *c) {
    RpiFw fw;
    rpifw_read(root, &fw);
    c->rpi_uv_alarm = fw.uv_alarm;
    c->rpi_throttled = fw.throttled;
    if (fw.hwmon || fw.throttled >= 0) c->rpi = 1;
    if (c->rpi) return;
    /* the device tree's root compatible ("raspberrypi,5-model-b\0brcm,bcm2712\0"), so a Pi without the hwmon
     * module still gets LB_RPI_HOT */
    char p[PATH_MAX], b[256];
    snprintf(p, sizeof p, "%s/sys/firmware/devicetree/base/compatible", root);
    FILE *f = fopen(p, "re");
    if (!f) return;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    for (size_t i = 0; i < n; i += strlen(b + i) + 1)
        if (!strncmp(b + i, "raspberrypi,", 12)) { c->rpi = 1; break; }
}

static void power(const char *root, LbCond *c) {
    char d[PATH_MAX];
    snprintf(d, sizeof d, "%s/sys/class/power_supply", root);
    DIR *dir = opendir(d);
    if (!dir) return;
    struct dirent *e;
    int mains = -1;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) > 48) continue;
        char p[128], type[24] = "", st[24] = "";
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
        if (rd(root, p, type, sizeof type)) continue;
        if (!strcmp(type, "Battery")) {
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/scope", e->d_name);
            char scope[16] = "";
            if (!rd(root, p, scope, sizeof scope) && !strcmp(scope, "Device")) continue;   /* a mouse's battery */
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
            c->battery_pct = (int)rdl(root, p, -1);
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
            rd(root, p, st, sizeof st);
            if (!strcmp(st, "Discharging")) c->on_battery = 1;
            else if (c->on_battery < 0) c->on_battery = 0;
        } else if (!strcmp(type, "Mains") || !strcmp(type, "USB")) {
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/online", e->d_name);
            if (rdl(root, p, 0) == 1) mains = 1;
            else if (mains < 0) mains = 0;
        }
    }
    closedir(dir);
    if (mains == 1 && c->on_battery == 1 && c->battery_pct >= 0) c->on_battery = 0;   /* plugged in, reported late */
    if (c->on_battery < 0 && mains >= 0) c->on_battery = !mains;
}

void lb_cond_read(const char *root, LbCond *c) {
    memset(c, 0, sizeof *c);
    c->temp_c = -273;
    c->rpi_throttled = -1;
    c->rpi_uv_alarm = -1;
    c->on_battery = -1;
    c->battery_pct = -1;
    thermal(root, c);
    hwmon(root, c);
    cpus(root, c);
    rpi(root, c);
    power(root, c);
    rd(root, "/sys/firmware/acpi/platform_profile", c->platform_profile, sizeof c->platform_profile);
    for (char *s = c->governor; *s; s++) if (!((*s >= 'a' && *s <= 'z') || *s == '-' || *s == '_')) *s = '_';
    for (char *s = c->platform_profile; *s; s++) if (!((*s >= 'a' && *s <= 'z') || *s == '-' || *s == '_')) *s = '_';
    for (char *s = c->zone; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) *s = '_';
}

int lb_cpu_ticks(const char *root, unsigned long long *busy, unsigned long long *total) {
    char line[256];
    if (rd(root, "/proc/stat", line, sizeof line) || strncmp(line, "cpu ", 4)) return -1;
    unsigned long long v[10] = { 0 };
    int n = sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    if (n < 4) return -1;
    unsigned long long t = 0;
    for (int i = 0; i < n && i < 8; i++) t += v[i];       /* guest time is already in user */
    *total = t;
    *busy = t - v[3] - (n > 4 ? v[4] : 0);                  /* minus idle and iowait */
    return 0;
}

const char *lb_storage_kind(const char *root, const char *path) {
    struct statfs fs;
    struct stat st;
    if (statfs(path, &fs) || stat(path, &st)) return "unknown";
    if (fs.f_type == 0x01021994 /* tmpfs */ || fs.f_type == 0x794c7630 /* overlayfs */) return "ram";
    if (!major(st.st_dev)) return "unknown";
    char p[PATH_MAX], real[PATH_MAX];
    snprintf(p, sizeof p, "%s/sys/dev/block/%u:%u", root, major(st.st_dev), minor(st.st_dev));
    if (!realpath(p, real)) return "unknown";
    /* device-mapper (LUKS, LVM): the first device below it */
    char slaves[PATH_MAX + 8];
    snprintf(slaves, sizeof slaves, "%s/slaves", real);
    DIR *sd = opendir(slaves);
    if (sd) {
        struct dirent *e;
        while ((e = readdir(sd))) if (e->d_name[0] != '.') {
            char q[PATH_MAX + 300];
            snprintf(q, sizeof q, "%s/%s", slaves, e->d_name);
            char r2[PATH_MAX];
            if (realpath(q, r2)) snprintf(real, sizeof real, "%s", r2);
            break;
        }
        closedir(sd);
    }
    /* a partition: its disk is the parent folder */
    char part[PATH_MAX + 12];
    snprintf(part, sizeof part, "%s/partition", real);
    if (!access(part, F_OK)) { char *sl = strrchr(real, '/'); if (sl) *sl = 0; }
    const char *name = strrchr(real, '/');
    name = name ? name + 1 : real;
    if (strstr(real, "/usb")) return "usb";
    if (!strncmp(name, "nvme", 4)) return "nvme";
    if (!strncmp(name, "mmcblk", 6)) return "sd";
    if (!strncmp(name, "vd", 2) || !strncmp(name, "xvd", 3)) return "virtual";
    if (!strncmp(name, "sd", 2)) {
        char vendor[64] = "", q[PATH_MAX + 32];
        snprintf(q, sizeof q, "%s/device/vendor", real);
        FILE *f = fopen(q, "re");
        if (f) { if (!fgets(vendor, sizeof vendor, f)) vendor[0] = 0; fclose(f); }
        if (strstr(vendor, "QEMU") || strstr(vendor, "VMware") || strstr(vendor, "VBOX") || strstr(vendor, "Msft")) return "virtual";
        snprintf(q, sizeof q, "%s/queue/rotational", real);
        f = fopen(q, "re");
        int rot = -1;
        if (f) { if (fscanf(f, "%d", &rot) != 1) rot = -1; fclose(f); }
        return rot == 1 ? "hdd" : rot == 0 ? "ssd" : "unknown";
    }
    return "unknown";
}

/* this sample's events: the "now" bits, sticky bits set again after being seen clear, the alarm likewise, and
 * LB_RPI_HOT; a sticky bit or the alarm seen clear leaves the base, so its next setting is new */
static unsigned rpi_events(LbRpiWatch *w, const LbCond *c) {
    unsigned ev = 0;
    if (c->rpi) w->rpi = 1;
    if (c->rpi_throttled >= 0) {
        unsigned v = (unsigned)c->rpi_throttled, st = (v & LB_RPI_STICKY_MASK) >> 16;
        w->bits = 1;
        ev |= v & LB_RPI_NOW_MASK;
        ev |= st & ~w->base & 0xfu;
        w->base &= ~(0xfu & ~st);
    }
    if (c->rpi_uv_alarm >= 0) {
        w->alarm = 1;
        if (c->rpi_uv_alarm == 1) { if (!(w->base & (LB_RPI_UV << 4))) ev |= LB_RPI_UV; }
        else w->base &= ~(unsigned)(LB_RPI_UV << 4);
    }
    if (c->rpi && c->temp_c >= LB_RPI_FW_TEMP_C) ev |= LB_RPI_HOT;
    return ev;
}

void lb_rpi_watch_start(LbRpiWatch *w, const LbCond *c0) {
    memset(w, 0, sizeof *w);
    w->rpi = c0->rpi;
    w->bits = c0->rpi_throttled >= 0;
    w->alarm = c0->rpi_uv_alarm >= 0;
    if (c0->rpi_throttled >= 0) w->base = ((unsigned)c0->rpi_throttled & LB_RPI_STICKY_MASK) >> 16;
    if (c0->rpi_uv_alarm == 1) w->base |= LB_RPI_UV << 4;
    /* what is happening at the start itself counts: the "now" bits and the temperature */
    if (c0->rpi_throttled >= 0) w->seen |= (unsigned)c0->rpi_throttled & LB_RPI_NOW_MASK;
    if (c0->rpi && c0->temp_c >= LB_RPI_FW_TEMP_C) w->seen |= LB_RPI_HOT;
}

unsigned lb_rpi_watch(LbRpiWatch *w, const LbCond *cn) {
    unsigned ev = rpi_events(w, cn);
    w->seen |= ev;
    return ev;
}

const char *lb_cond_stop(const LbCond *start, const LbCond *now, unsigned rpi_events, int peak) {
    if (now->temp_c > -270) {
        if (now->passive_c > 0 && now->temp_c >= now->passive_c - 2) return "thermal";
        if (now->passive_c <= 0 && now->critical_c > 0 && now->temp_c >= now->critical_c - 10) return "thermal";
    }
    if (peak) {
        if (start->throttle_known && now->throttle_events > start->throttle_events) return "throttled";
        if (rpi_events & LB_RPI_SLOWED) return "throttled";
    }
    return NULL;
}

int lb_cond_battery_low(const LbCond *c) { return c->on_battery == 1 && c->battery_pct >= 0 && c->battery_pct < 30; }
