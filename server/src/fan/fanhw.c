/* fanhw.c — temperature and fan discovery, duty writes and giving control back (see fanhw.h). */
#include "fan/fanhw.h"
#include "fan/fancurve.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEMP_NONE (-1000000)

/* ---- small sysfs helpers ---- */

static int read_str(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r < 0) return -1;
    out[r] = 0;
    while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == ' ' || out[r - 1] == 0)) out[--r] = 0;
    return 0;
}

static int read_int(const char *path, int *v) {
    char b[32], *e;
    if (read_str(path, b, sizeof b) || !b[0]) return -1;
    errno = 0;
    long x = strtol(b, &e, 10);
    if (errno || *e || x < INT_MIN || x > INT_MAX) return -1;
    *v = (int)x;
    return 0;
}

static int write_int(const char *path, int v) {
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);   /* O_TRUNC: no-op on sysfs, needed for the test tree */
    if (fd < 0) return -1;
    char b[16];
    int n = snprintf(b, sizeof b, "%d", v);
    ssize_t w = write(fd, b, (size_t)n);
    close(fd);
    return w == n ? 0 : -1;
}

/* sysfs names end up in JSON: keep only [A-Za-z0-9_.-] */
static void clean(char *s) {
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.')) *s = '_';
}

static int exists(const char *p) { return access(p, F_OK) == 0; }

static void pathf(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void pathf(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= n) out[0] = 0;   /* a truncated path is never used */
}

/* hwmonN, thermal_zoneN, cooling_deviceN: the prefix followed only by digits */
static int numbered(const char *name, const char *prefix) {
    size_t l = strlen(prefix);
    if (strncmp(name, prefix, l) || !name[l]) return 0;
    for (const char *p = name + l; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* Super I/O and fan controller drivers whose pwmN_enable=1 means "manual" and whose fans are case
 * or CPU fans. GPU drivers and laptop embedded controllers are not here: their fans stay with
 * the firmware. */
static int chip_allowed(const char *chip) {
    static const char *const pre[] = { "nct6", "nct7", "it8", "f71", "f8000", "w83", "sch5", "emc2", "adt7", "amc6821", "max6650", "max31790", NULL };
    for (int i = 0; pre[i]; i++) if (!strncmp(chip, pre[i], strlen(pre[i]))) return 1;
    return 0;
}

const char *fanhw_kind_name(FanKind k) { return k == FK_BOARD ? "board" : "hwmon"; }

/* ---- scan ---- */

static void find_temp(FanHw *h) {
    char base[FANHW_PATH], p[FANHW_PATH], type[32];
    h->temp_path[0] = 0;
    /* 1. thermal zones of the CPU package */
    pathf(base, sizeof base, "%s/sys/class/thermal", h->root);
    DIR *d = opendir(base);
    struct dirent *e;
    char zone0[FANHW_PATH] = "";
    while (d && (e = readdir(d))) {
        if (!numbered(e->d_name, "thermal_zone")) continue;
        pathf(p, sizeof p, "%s/%s/type", base, e->d_name);
        if (read_str(p, type, sizeof type)) continue;
        clean(type);
        if (!strcmp(e->d_name, "thermal_zone0")) pathf(zone0, sizeof zone0, "%s/%s/temp", base, e->d_name);
        if (!strcmp(type, "cpu-thermal") || !strcmp(type, "x86_pkg_temp") || !strcmp(type, "cpu_thermal")) {
            pathf(h->temp_path, sizeof h->temp_path, "%s/%s/temp", base, e->d_name);
            snprintf(h->temp_src, sizeof h->temp_src, "%s", type);
            break;
        }
    }
    if (d) closedir(d);
    if (h->temp_path[0]) return;
    /* 2. CPU sensors in hwmon */
    pathf(base, sizeof base, "%s/sys/class/hwmon", h->root);
    d = opendir(base);
    while (d && (e = readdir(d))) {
        if (!numbered(e->d_name, "hwmon")) continue;
        pathf(p, sizeof p, "%s/%s/name", base, e->d_name);
        if (read_str(p, type, sizeof type)) continue;
        if (!strcmp(type, "k10temp") || !strcmp(type, "coretemp") || !strcmp(type, "zenpower") || !strcmp(type, "cpu_thermal")) {
            pathf(h->temp_path, sizeof h->temp_path, "%s/%s/temp1_input", base, e->d_name);
            snprintf(h->temp_src, sizeof h->temp_src, "%s", type);
            break;
        }
    }
    if (d) closedir(d);
    if (h->temp_path[0]) return;
    /* 3. whatever thermal_zone0 is */
    if (zone0[0]) { snprintf(h->temp_path, sizeof h->temp_path, "%s", zone0); snprintf(h->temp_src, sizeof h->temp_src, "thermal_zone0"); }
}

/* The cooling device of type "pwm-fan" and the active trips of the zone bound to it. */
static void find_board_trips(FanHw *h) {
    char base[FANHW_PATH], p[FANHW_PATH], s[64], link[FANHW_PATH];
    pathf(base, sizeof base, "%s/sys/class/thermal", h->root);
    DIR *d = opendir(base);
    struct dirent *e;
    char cdev_name[256] = "";
    while (d && (e = readdir(d))) {
        if (!numbered(e->d_name, "cooling_device")) continue;
        pathf(p, sizeof p, "%s/%s/type", base, e->d_name);
        if (!read_str(p, s, sizeof s) && !strcmp(s, "pwm-fan")) {
            snprintf(cdev_name, sizeof cdev_name, "%s", e->d_name);
            pathf(h->cdev, sizeof h->cdev, "%s/%s", base, e->d_name);
            break;
        }
    }
    if (d) closedir(d);
    if (!cdev_name[0]) return;
    d = opendir(base);
    while (d && (e = readdir(d)) && !h->ntrips) {
        if (!numbered(e->d_name, "thermal_zone")) continue;
        char zdir[FANHW_PATH];
        pathf(zdir, sizeof zdir, "%s/%s", base, e->d_name);
        DIR *z = opendir(zdir);
        struct dirent *f;
        while (z && (f = readdir(z))) {
            if (!numbered(f->d_name, "cdev")) continue;
            pathf(p, sizeof p, "%s/%s", zdir, f->d_name);
            ssize_t l = readlink(p, link, sizeof link - 1);
            if (l <= 0) continue;
            link[l] = 0;
            const char *b = strrchr(link, '/');
            b = b ? b + 1 : link;
            if (strcmp(b, cdev_name)) continue;
            int trip, v;
            pathf(p, sizeof p, "%s/%s_trip_point", zdir, f->d_name);
            if (read_int(p, &trip) || trip < 0 || trip > 63) continue;
            pathf(p, sizeof p, "%s/trip_point_%d_type", zdir, trip);
            if (read_str(p, s, sizeof s) || strcmp(s, "active")) continue;
            pathf(p, sizeof p, "%s/trip_point_%d_temp", zdir, trip);
            if (read_int(p, &v)) continue;
            int dup = 0;
            for (int i = 0; i < h->ntrips; i++) dup |= h->trip[i] == trip;
            if (dup || h->ntrips >= FANHW_MAX_TRIPS) continue;
            h->trip[h->ntrips] = trip;
            h->trip_orig[h->ntrips] = v;
            h->ntrips++;
        }
        if (z) closedir(z);
        if (h->ntrips) snprintf(h->zone, sizeof h->zone, "%s", zdir);
    }
    if (d) closedir(d);
    /* sort by original temperature (the backstop keeps their order) */
    for (int i = 1; i < h->ntrips; i++)
        for (int j = i; j > 0 && h->trip_orig[j] < h->trip_orig[j - 1]; j--) {
            int a = h->trip[j], b = h->trip_orig[j];
            h->trip[j] = h->trip[j - 1]; h->trip_orig[j] = h->trip_orig[j - 1];
            h->trip[j - 1] = a; h->trip_orig[j - 1] = b;
        }
}

int fanhw_scan(FanHw *h, const char *root) {
    memset(h, 0, sizeof *h);
    snprintf(h->root, sizeof h->root, "%s", root ? root : "");
    char base[FANHW_PATH], p[FANHW_PATH], chip[32], model[128];
    pathf(p, sizeof p, "%s/proc/device-tree/model", h->root);
    if (!read_str(p, model, sizeof model) && strstr(model, "Raspberry Pi 5")) h->board = 1;
    find_temp(h);
    pathf(base, sizeof base, "%s/sys/class/hwmon", h->root);
    DIR *d = opendir(base);
    struct dirent *e;
    /* readdir order is not stable: collect the names and sort them */
    char names[32][24];
    int nn = 0;
    while (d && (e = readdir(d)) && nn < 32)
        if (numbered(e->d_name, "hwmon") && strlen(e->d_name) < sizeof names[0]) snprintf(names[nn++], sizeof names[0], "%s", e->d_name);
    if (d) closedir(d);
    for (int i = 1; i < nn; i++)
        for (int j = i; j > 0 && atoi(names[j] + 5) < atoi(names[j - 1] + 5); j--) {
            char t[24];
            memcpy(t, names[j], sizeof t); memcpy(names[j], names[j - 1], sizeof t); memcpy(names[j - 1], t, sizeof t);
        }
    for (int k = 0; k < nn; k++) {
        char dir[FANHW_PATH];
        pathf(dir, sizeof dir, "%s/%s", base, names[k]);
        pathf(p, sizeof p, "%s/name", dir);
        if (read_str(p, chip, sizeof chip)) continue;
        clean(chip);
        int board = !strcmp(chip, "pwmfan");
        int allowed = board || chip_allowed(chip);
        for (int ch = 1; ch <= 8; ch++) {
            char pwm[FANHW_PATH], en[FANHW_PATH], rpm[FANHW_PATH];
            pathf(pwm, sizeof pwm, "%s/pwm%d", dir, ch);
            pathf(en, sizeof en, "%s/pwm%d_enable", dir, ch);
            pathf(rpm, sizeof rpm, "%s/fan%d_input", dir, ch);
            int has_pwm = exists(pwm) && exists(en);
            if (!strcmp(chip, "amdgpu") && ch == 1 && has_pwm && exists(rpm) && h->ng < FANHW_MAX_GPU) {
                GpuFan *g = &h->g[h->ng++];
                snprintf(g->dir, sizeof g->dir, "%s", dir);
                pathf(p, sizeof p, "%s/device", dir);
                if (!realpath(p, g->dev)) snprintf(g->dev, sizeof g->dev, "%s", dir);
                const char *b = strrchr(g->dev, '/');
                snprintf(g->slot, sizeof g->slot, "%s", b ? b + 1 : g->dev);
                for (char *q = g->slot; *q; q++) if (*q != ':') { char c1[2] = { *q, 0 }; clean(c1); *q = c1[0]; }
                int v;
                pathf(p, sizeof p, "%s/pwm1_min", dir);
                g->pwm_min = !read_int(p, &v) && v >= 0 && v < 255 ? v : 0;
                pathf(p, sizeof p, "%s/pwm1_max", dir);
                g->pwm_max = !read_int(p, &v) && v > g->pwm_min && v <= 255 ? v : 255;
                g->has_rpm = 1;
                g->pct = g->orig_enable = g->orig_pwm = -1;
            }
            if (allowed && has_pwm && h->n < FANHW_MAX) {
                FanTarget *t = &h->t[h->n++];
                t->kind = board ? FK_BOARD : FK_HWMON;
                snprintf(t->chip, sizeof t->chip, "%s", chip);
                snprintf(t->dir, sizeof t->dir, "%s", dir);
                pathf(p, sizeof p, "%s/device", dir);
                if (!realpath(p, t->dev)) snprintf(t->dev, sizeof t->dev, "%s", dir);
                t->ch = ch;
                t->has_rpm = exists(rpm);
                t->orig_enable = -1;
                t->last_pct = -1;
                if (board) find_board_trips(h);
            } else if (exists(rpm) && h->nro < FANHW_MAX_RO) {
                FanRo *r = &h->ro[h->nro++];
                snprintf(r->chip, sizeof r->chip, "%s", chip);
                snprintf(r->path, sizeof r->path, "%s", rpm);
            }
        }
    }
    return h->n;
}

int fanhw_temp(const FanHw *h) {
    int v;
    if (!h->temp_path[0] || read_int(h->temp_path, &v) || v < -50000 || v > 200000) return TEMP_NONE;
    return v;
}

/* ---- control ---- */

static int to_pwm(int pct) { return pct <= 0 ? 0 : pct >= 100 ? 255 : (pct * 255 + 50) / 100; }
static int to_pct(int pwm) { return pwm <= 0 ? 0 : pwm >= 255 ? 100 : (pwm * 100 + 127) / 255; }

static void tpath(const FanTarget *t, const char *what, char *out, size_t n) {
    pathf(out, n, "%s/pwm%d%s", t->dir, t->ch, what);
}

int fanhw_take(FanHw *h, const char *state_path) {
    /* 1. the restore notes first, so a crash right after leaves enough to undo it */
    for (int i = 0; i < h->n; i++) {
        FanTarget *t = &h->t[i];
        char p[FANHW_PATH];
        tpath(t, "_enable", p, sizeof p);
        int v;
        if (t->kind == FK_HWMON && !read_int(p, &v)) t->orig_enable = v;
    }
    h->cpu_held = 1;
    if (state_path) fanhw_notes(h, state_path, 1);
    int ok = 0;
    for (int i = 0; i < h->n; i++) {
        FanTarget *t = &h->t[i];
        char p[FANHW_PATH];
        if (t->kind == FK_HWMON) {
            tpath(t, "_enable", p, sizeof p);
            int v;
            if (t->orig_enable < 0 && !read_int(p, &v)) t->orig_enable = v;
            if (write_int(p, 1)) { fprintf(stderr, "lord-fand: %s: manual mode refused\n", p); continue; }
        }
        t->taken = 1;
        ok++;
    }
    /* the board fan: move the kernel's trips up to the backstop, so it does not fight our writes
     * but still spins the fan up by itself if we ever stop answering above it */
    for (int i = 0; i < h->ntrips; i++) {
        char p[FANHW_PATH];
        pathf(p, sizeof p, "%s/trip_point_%d_temp", h->zone, h->trip[i]);
        int want = (FANHW_BACKSTOP_C + 2 * i) * 1000;
        if (h->trip_orig[i] < want && write_int(p, want) == 0) h->trips_raised = 1;
    }
    return ok;
}

int fanhw_pct(const FanHw *h, int i) {
    char p[FANHW_PATH];
    int v;
    if (i < 0 || i >= h->n) return -1;
    tpath(&h->t[i], "", p, sizeof p);
    return read_int(p, &v) ? -1 : to_pct(v);
}

int fanhw_set(FanHw *h, int i, int pct) {
    if (i < 0 || i >= h->n || !h->t[i].taken) return -1;
    FanTarget *t = &h->t[i];
    char p[FANHW_PATH];
    tpath(t, "", p, sizeof p);
    int cur;
    if (read_int(p, &cur)) return -1;
    int want = to_pwm(pct);
    /* a stopped fan given a low duty may not start: one tick at full speed first */
    if (cur == 0 && pct > 0 && pct < FAN_KICK_BELOW) { want = 255; t->kicking = 1; }
    else t->kicking = 0;
    t->last_pct = pct;
    if (cur == want) return 0;
    return write_int(p, want);
}

int fanhw_rpm(const FanHw *h, int i) {
    char p[FANHW_PATH];
    int v;
    if (i < 0 || i >= h->n || !h->t[i].has_rpm) return -1;
    pathf(p, sizeof p, "%s/fan%d_input", h->t[i].dir, h->t[i].ch);
    return read_int(p, &v) || v < 0 ? -1 : v;
}

int fanhw_ro_rpm(const FanHw *h, int i) {
    int v;
    if (i < 0 || i >= h->nro || read_int(h->ro[i].path, &v) || v < 0) return -1;
    return v;
}

/* The board fan's cooling state for temp under the original trips, written to the cooling device
 * (which sets the duty from the device tree's cooling levels). */
static void board_state_now(FanHw *h) {
    if (!h->cdev[0]) return;
    int temp = fanhw_temp(h), max = 0, st = 0;
    char p[FANHW_PATH];
    pathf(p, sizeof p, "%s/max_state", h->cdev);
    if (read_int(p, &max) || max < 0) return;
    for (int i = 0; i < h->ntrips; i++) if (temp != TEMP_NONE && temp >= h->trip_orig[i]) st++;
    if (st > max) st = max;
    pathf(p, sizeof p, "%s/cur_state", h->cdev);
    write_int(p, st);
}

void fanhw_release(FanHw *h) {
    h->cpu_held = 0;
    for (int i = 0; i < h->ntrips; i++) {
        char p[FANHW_PATH];
        pathf(p, sizeof p, "%s/trip_point_%d_temp", h->zone, h->trip[i]);
        write_int(p, h->trip_orig[i]);
    }
    h->trips_raised = 0;
    board_state_now(h);
    for (int i = 0; i < h->n; i++) {
        FanTarget *t = &h->t[i];
        if (t->kind == FK_HWMON && t->taken && t->orig_enable >= 0) {
            char p[FANHW_PATH];
            tpath(t, "_enable", p, sizeof p);
            write_int(p, t->orig_enable);
        }
        t->taken = 0;
    }
}

int fanhw_restore_file(const char *root, const char *state_path) {
    FanHw h;
    fanhw_scan(&h, root);
    FILE *f = fopen(state_path, "re");
    int rc = 0;
    if (f) {
        char line[FANHW_PATH + 64];
        if (!fgets(line, sizeof line, f) || strcmp(line, "lord-fand 1\n")) rc = -1;
        while (rc == 0 && fgets(line, sizeof line, f)) {
            int a, b, n = 0;
            char dev[FANHW_PATH];
            if (sscanf(line, "trip %d %d%n", &a, &b, &n) == 2 && line[n] == '\n') {
                if (b < 0 || b > 200000) continue;
                for (int i = 0; i < h.ntrips; i++) if (h.trip[i] == a) h.trip_orig[i] = b;
            } else if (sscanf(line, "gpufan %d %d %319s%n", &a, &b, dev, &n) == 3 && line[n] == '\n') {
                if (a < 0 || a > 5 || b < 0 || b > 255) continue;
                for (int i = 0; i < h.ng; i++) if (!strcmp(h.g[i].dev, dev)) { h.g[i].orig_enable = a; h.g[i].orig_pwm = b; h.g[i].manual = 1; }
            } else if (sscanf(line, "enable %d %d %319s%n", &a, &b, dev, &n) == 3 && line[n] == '\n') {
                if (b < 0 || b > 5) continue;
                for (int i = 0; i < h.n; i++)
                    if (h.t[i].kind == FK_HWMON && h.t[i].ch == a && !strcmp(h.t[i].dev, dev)) { h.t[i].orig_enable = b; h.t[i].taken = 1; }
            }
        }
        fclose(f);
    }
    /* the trips go back even without notes: never leave them at the backstop */
    if (h.ntrips) {
        int raised = 1;
        for (int i = 0; i < h.ntrips; i++) raised &= h.trip_orig[i] >= FANHW_BACKSTOP_C * 1000;
        if (raised && !f) rc = -1;   /* no notes and trips look raised: the original values are unknown */
    }
    if (f || !h.ntrips) fanhw_release(&h);
    for (int i = 0; i < h.ng; i++) if (h.g[i].manual) fanhw_gpu_auto(&h, i, GR_STOP, NULL);   /* GPU fans: automatic */
    unlink(state_path);
    return rc;
}

/* ---- restore notes ---- */

static int note_safe(const char *dev) { return dev[0] && !strchr(dev, ' ') && !strchr(dev, '\n'); }

void fanhw_notes(FanHw *h, const char *state_path, int cpu) {
    if (!state_path) return;
    int any = 0;
    for (int i = 0; i < h->ng; i++) any |= h->g[i].manual;
    cpu = cpu || h->cpu_held;
    if (cpu) any |= h->n > 0 || h->ntrips > 0;
    if (!any) { unlink(state_path); return; }
    char tmp[FANHW_PATH];
    pathf(tmp, sizeof tmp, "%s.tmp", state_path);
    FILE *f = tmp[0] ? fopen(tmp, "we") : NULL;
    if (!f) return;
    fprintf(f, "lord-fand 1\n");
    if (cpu) {
        for (int i = 0; i < h->n; i++) {
            FanTarget *t = &h->t[i];
            if (t->kind == FK_HWMON && t->orig_enable >= 0 && note_safe(t->dev)) fprintf(f, "enable %d %d %s\n", t->ch, t->orig_enable, t->dev);
        }
        for (int i = 0; i < h->ntrips; i++) fprintf(f, "trip %d %d\n", h->trip[i], h->trip_orig[i]);
    }
    for (int i = 0; i < h->ng; i++) {
        GpuFan *g = &h->g[i];
        if (g->manual && note_safe(g->dev)) fprintf(f, "gpufan %d %d %s\n", g->orig_enable < 0 ? 2 : g->orig_enable, g->orig_pwm < 0 ? 0 : g->orig_pwm, g->dev);
    }
    if (fclose(f) == 0) rename(tmp, state_path);
    else unlink(tmp);
}

/* ---- GPU fans (amdgpu) ---- */

const char *fanhw_revert_name(GpuRevert r) {
    return r == GR_HOT ? "hot" : r == GR_ERROR ? "error" : r == GR_SESSION ? "session" : r == GR_STOP ? "stop" : "";
}

static void gpath(const GpuFan *g, const char *f, char *out, size_t n) { pathf(out, n, "%s/%s", g->dir, f); }

int fanhw_gpu_temp(const FanHw *h, int i) {
    if (i < 0 || i >= h->ng) return TEMP_NONE;
    int best = TEMP_NONE;
    for (int k = 1; k <= 3; k++) {
        char p[FANHW_PATH], f[16];
        snprintf(f, sizeof f, "temp%d_input", k);
        gpath(&h->g[i], f, p, sizeof p);
        int v;
        if (!read_int(p, &v) && v > 0 && v < 200000 && v > best) best = v;
    }
    return best;
}

static int gpu_to_pwm(const GpuFan *g, int pct) { return g->pwm_min + (pct * (g->pwm_max - g->pwm_min) + 50) / 100; }

int fanhw_gpu_pct(const FanHw *h, int i) {
    if (i < 0 || i >= h->ng) return -1;
    const GpuFan *g = &h->g[i];
    char p[FANHW_PATH];
    int v;
    gpath(g, "pwm1", p, sizeof p);
    if (read_int(p, &v) || v < 0 || v > 255) return -1;
    if (v <= g->pwm_min) return 0;
    return ((v - g->pwm_min) * 100 + (g->pwm_max - g->pwm_min) / 2) / (g->pwm_max - g->pwm_min);
}

int fanhw_gpu_rpm(const FanHw *h, int i) {
    if (i < 0 || i >= h->ng) return -1;
    char p[FANHW_PATH];
    int v;
    gpath(&h->g[i], "fan1_input", p, sizeof p);
    return read_int(p, &v) || v < 0 ? -1 : v;
}

void fanhw_gpu_auto(FanHw *h, int i, GpuRevert why, const char *state_path) {
    if (i < 0 || i >= h->ng) return;
    GpuFan *g = &h->g[i];
    char p[FANHW_PATH];
    gpath(g, "pwm1_enable", p, sizeof p);
    if (g->manual || why == GR_NONE) write_int(p, 2);   /* amdgpu's automatic: the firmware's own fan table */
    if (g->manual && why != GR_NONE) {
        g->reverted = why;
        fprintf(stderr, "lord-fand: GPU fan %s back to automatic (%s)\n", g->slot, fanhw_revert_name(why));
    }
    g->manual = 0;
    g->pct = -1;
    g->errors = 0;
    fanhw_notes(h, state_path, 0);
}

int fanhw_gpu_manual(FanHw *h, int i, int pct, unsigned owner, const char *state_path) {
    if (i < 0 || i >= h->ng) return -1;
    GpuFan *g = &h->g[i];
    char en[FANHW_PATH], pw[FANHW_PATH];
    gpath(g, "pwm1_enable", en, sizeof en);
    gpath(g, "pwm1", pw, sizeof pw);
    int t = fanhw_gpu_temp(h, i);
    if (t != TEMP_NONE && t >= FANHW_GPU_BACKSTOP_C * 1000) return -1;   /* too hot to take it from the firmware now */
    if (!g->manual) {
        int v;
        g->orig_enable = !read_int(en, &v) && v >= 0 && v <= 5 ? v : 2;
        g->orig_pwm = !read_int(pw, &v) && v >= 0 && v <= 255 ? v : 0;
        g->manual = 1;                                    /* the notes first, so a crash right after can undo it */
        fanhw_notes(h, state_path, 0);
        if (write_int(en, 1)) { fanhw_gpu_auto(h, i, GR_ERROR, state_path); return -1; }
    }
    g->pct = fan_gpu_pct(pct);
    g->owner = owner;
    g->errors = 0;
    g->reverted = GR_NONE;
    if (write_int(pw, gpu_to_pwm(g, g->pct))) { fanhw_gpu_auto(h, i, GR_ERROR, state_path); return -1; }
    return 0;
}

void fanhw_gpu_tick(FanHw *h, int i, int session, const char *state_path) {
    if (i < 0 || i >= h->ng || !h->g[i].manual) return;
    GpuFan *g = &h->g[i];
    int t = fanhw_gpu_temp(h, i);
    if (t != TEMP_NONE && t >= FANHW_GPU_BACKSTOP_C * 1000) { fanhw_gpu_auto(h, i, GR_HOT, state_path); return; }
    if (!session) { fanhw_gpu_auto(h, i, GR_SESSION, state_path); return; }
    char en[FANHW_PATH], pw[FANHW_PATH];
    gpath(g, "pwm1_enable", en, sizeof en);
    gpath(g, "pwm1", pw, sizeof pw);
    int e, v, want = gpu_to_pwm(g, g->pct), bad = t == TEMP_NONE;
    /* after a suspend the driver may have reset it: take it again */
    if (read_int(en, &e)) bad = 1;
    else if (e != 1 && write_int(en, 1)) bad = 1;
    if (read_int(pw, &v)) bad = 1;
    else if (v != want && write_int(pw, want)) bad = 1;
    g->errors = bad ? g->errors + 1 : 0;
    if (g->errors >= FANHW_GPU_ERRORS) fanhw_gpu_auto(h, i, GR_ERROR, state_path);
}

int fanhw_uid_has_session(const char *root, unsigned uid) {
    if (uid == 0) return 1;
    char base[FANHW_PATH];
    pathf(base, sizeof base, "%s/proc", root);
    DIR *d = opendir(base);
    if (!d) return 1;                                     /* can't tell: keep it (the backstop still holds) */
    int found = 0;
    for (struct dirent *e; !found && (e = readdir(d));) {
        if (!numbered(e->d_name, "") || strlen(e->d_name) > 10) continue;
        char p[FANHW_PATH], comm[32], st[1024];
        pathf(p, sizeof p, "%s/%s/comm", base, e->d_name);
        if (read_str(p, comm, sizeof comm) || strcmp(comm, "lord-server")) continue;
        pathf(p, sizeof p, "%s/%s/status", base, e->d_name);
        if (read_str(p, st, sizeof st)) continue;
        const char *u = strstr(st, "\nUid:");
        unsigned real;
        if (u && sscanf(u + 5, "%u", &real) == 1 && real == uid) found = 1;
    }
    closedir(d);
    return found;
}
