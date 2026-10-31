/* voltage.c — see voltage.h. */
#include "voltage.h"
#include "../hw/rpifw.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A small file (a sensor's driver may fail the read: that input does not exist for us). -1 if it can't be read. */
static int rd(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r < 0) return -1;
    out[r] = 0;
    return (int)r;
}

int volt_parse_int(const char *s, long *out) {
    const char *p = s;
    int neg = *p == '-';
    if (neg) p++;
    long v = 0;
    int k = 0;
    for (; *p >= '0' && *p <= '9'; p++, k++) {
        if (k >= 9) return -1;
        v = v * 10 + (*p - '0');
    }
    if (!k || (*p == '\n' && p[1]) || (*p && *p != '\n')) return -1;
    *out = neg ? -v : v;
    return 0;
}

/* A name from sysfs: its line, in the given characters, shorter than n. 0 if clean. */
static int clean(char *s, const char *extra, size_t n) {
    s[strcspn(s, "\n")] = 0;
    size_t l = strlen(s);
    if (!l || l >= n) return -1;
    for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && !strchr(extra, *p)) return -1;
    return 0;
}

static int by_index(const void *a, const void *b) { return ((const VoltInput *)a)->index - ((const VoltInput *)b)->index; }
static int by_dev(const void *a, const void *b) {
    const char *x = a, *y = b;
    size_t lx = strlen(x), ly = strlen(y);
    return lx != ly ? (lx < ly ? -1 : 1) : strcmp(x, y);        /* hwmon2 before hwmon10 */
}

/* "in<digits>_input": its number, or -1 */
static int input_index(const char *f) {
    if (strncmp(f, "in", 2) || f[2] < '0' || f[2] > '9') return -1;
    char *e;
    long i = strtol(f + 2, &e, 10);
    return !strcmp(e, "_input") && i >= 0 && i < 256 ? (int)i : -1;
}

/* A hwmon that only mirrors a power_supply device (the kernel makes one per battery, with its name sanitized). */
static int mirrors_supply(const char *root, const char *name) {
    char d[PATH_MAX];
    snprintf(d, sizeof d, "%s/sys/class/power_supply", root);
    DIR *dir = opendir(d);
    if (!dir) return 0;
    int same = 0;
    for (struct dirent *e; !same && (e = readdir(dir));) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= 64) continue;
        char s[64];
        memcpy(s, e->d_name, strlen(e->d_name) + 1);
        for (char *p = s; *p; p++) if (*p == '-' || *p == '*' || *p == ' ') *p = '_';
        same = !strcmp(s, name);
    }
    closedir(dir);
    return same;
}

static void read_chip(const char *root, const char *dev, VoltState *v) {
    char dir[1024], p[1536], name[64];
    VoltChip *c = &v->chips[v->nchips];
    snprintf(dir, sizeof dir, "%s/sys/class/hwmon/%s", root, dev);
    snprintf(p, sizeof p, "%s/name", dir);
    if (rd(p, name, sizeof name) <= 0 || clean(name, "_-.:", sizeof c->name) || mirrors_supply(root, name)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    memset(c, 0, sizeof *c);
    memcpy(c->dev, dev, strlen(dev) + 1);                /* read_chips() checked its length */
    memcpy(c->name, name, strlen(name) + 1);             /* clean() checked its length */
    for (struct dirent *e; (e = readdir(d)) && c->n < VOLT_INPUTS;) {
        int i = input_index(e->d_name);
        if (i < 0) continue;
        char b[32], label[64];
        long mv;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (rd(p, b, sizeof b) <= 0 || volt_parse_int(b, &mv) || labs(mv) > VOLT_LIMIT_MV) continue;
        VoltInput *in = &c->in[c->n];
        in->index = i;
        in->mv = mv;
        in->label[0] = 0;
        snprintf(p, sizeof p, "%s/in%d_label", dir, i);
        if (rd(p, label, sizeof label) > 0 && !clean(label, " +._/-", sizeof in->label)) memcpy(in->label, label, strlen(label) + 1);
        if (!in->label[0] && mv == 0) continue;          /* a pin with nothing on it, not a reading */
        c->n++;
    }
    closedir(d);
    if (!c->n) return;
    qsort(c->in, (size_t)c->n, sizeof c->in[0], by_index);
    v->nchips++;
}

static void read_chips(const char *root, VoltState *v) {
    char d[PATH_MAX], devs[32][16];
    snprintf(d, sizeof d, "%s/sys/class/hwmon", root);
    DIR *dir = opendir(d);
    if (!dir) return;
    size_t n = 0;
    for (struct dirent *e; n < 32 && (e = readdir(dir));) {
        const char *s = e->d_name;
        if (strncmp(s, "hwmon", 5) || !s[5] || strlen(s) > 10 || strspn(s + 5, "0123456789") != strlen(s + 5)) continue;
        snprintf(devs[n++], sizeof devs[0], "%s", s);
    }
    closedir(dir);
    qsort(devs, n, sizeof devs[0], by_dev);
    for (size_t i = 0; i < n && v->nchips < VOLT_CHIPS; i++) read_chip(root, devs[i], v);
}

static int supply_file(const char *dir, const char *f, char *out, size_t n) {
    char p[1800];
    snprintf(p, sizeof p, "%s/%s", dir, f);
    if (rd(p, out, n) < 0) return -1;
    out[strcspn(out, "\n")] = 0;
    return 0;
}

/* microvolts in a file → mV, -1 if it isn't a positive reading in range */
static long supply_mv(const char *dir, const char *f) {
    char b[32];
    long uv;
    if (supply_file(dir, f, b, sizeof b) || volt_parse_int(b, &uv) || uv <= 0 || uv / 1000 > VOLT_LIMIT_MV) return -1;
    return uv / 1000;
}

static int by_supply(const void *a, const void *b) { return strcmp(((const VoltSupply *)a)->name, ((const VoltSupply *)b)->name); }

static void read_supplies(const char *root, VoltState *v) {
    char d[1024];
    snprintf(d, sizeof d, "%s/sys/class/power_supply", root);
    DIR *dir = opendir(d);
    if (!dir) return;
    for (struct dirent *e; v->nsup < VOLT_SUPPLIES && (e = readdir(dir));) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof v->sup[0].name) continue;
        char name[sizeof v->sup[0].name], dev[1536], t[32] = "", scope[32] = "", b[16] = "";
        snprintf(name, sizeof name, "%s", e->d_name);
        if (clean(name, "_-.:", sizeof name)) continue;
        snprintf(dev, sizeof dev, "%s/%s", d, name);
        if (supply_file(dev, "type", t, sizeof t) || !t[0]) continue;
        supply_file(dev, "scope", scope, sizeof scope);
        if (!strcmp(scope, "Device")) continue;          /* a mouse's or a keyboard's */
        int battery = !strcmp(t, "Battery");
        if (battery && !supply_file(dev, "present", b, sizeof b) && !strcmp(b, "0")) continue;
        if (!battery && (supply_file(dev, "online", b, sizeof b) || strcmp(b, "1"))) continue;   /* unplugged: no reading */
        long mv = supply_mv(dev, "voltage_now");
        if (mv < 0) continue;
        VoltSupply *s = &v->sup[v->nsup++];
        snprintf(s->name, sizeof s->name, "%s", name);
        s->battery = battery;
        s->mv = mv;
        s->min_mv = battery ? supply_mv(dev, "voltage_min_design") : -1;
    }
    closedir(dir);
    qsort(v->sup, (size_t)v->nsup, sizeof v->sup[0], by_supply);
}

static int board_here(const char *root) {
    char p[PATH_MAX], b[512];
    snprintf(p, sizeof p, "%s/proc/device-tree/compatible", root);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, b, sizeof b);
    close(fd);
    if (n <= 0 || !hw_is_bcm2712(b, (size_t)n)) return 0;
    snprintf(p, sizeof p, "%s/usr/bin/vcgencmd", root);
    return !access(p, X_OK);
}

void volt_read(const char *root, VoltState *v) {
    v->nchips = v->nsup = 0;
    v->board = board_here(root);
    RpiFw f;
    rpifw_read(root, &f);
    v->alarm = f.uv_alarm;
    v->since_boot = -1;
    if (f.throttled >= 0) {
        if (v->alarm < 0) v->alarm = f.throttled & 1;
        if (!f.hwmon) v->since_boot = !!(f.throttled & 0x10000);
    }
    read_chips(root, v);
    read_supplies(root, v);
}

int volt_any(const VoltState *v) { return v->board || v->alarm >= 0 || v->nchips > 0 || v->nsup > 0; }

char *volt_json(const VoltState *v) {
    size_t cap = 512 + (size_t)v->nchips * (64 + VOLT_INPUTS * 72) + (size_t)v->nsup * 96;
    char *o = malloc(cap);
    if (!o) return NULL;
    size_t n = (size_t)snprintf(o, cap, "{\"board\":%s,\"alarm\":%d,\"sinceBoot\":%d,\"chips\":[", v->board ? "true" : "false", v->alarm, v->since_boot);
    for (int i = 0; i < v->nchips; i++) {
        const VoltChip *c = &v->chips[i];
        n += (size_t)snprintf(o + n, cap - n, "%s{\"dev\":\"%s\",\"name\":\"%s\",\"in\":[", i ? "," : "", c->dev, c->name);
        for (int k = 0; k < c->n; k++)
            n += (size_t)snprintf(o + n, cap - n, "%s{\"i\":%d,\"v\":%.3f,\"label\":\"%s\"}", k ? "," : "", c->in[k].index, (double)c->in[k].mv / 1000.0, c->in[k].label);
        n += (size_t)snprintf(o + n, cap - n, "]}");
    }
    n += (size_t)snprintf(o + n, cap - n, "],\"supplies\":[");
    for (int i = 0; i < v->nsup; i++) {
        const VoltSupply *s = &v->sup[i];
        n += (size_t)snprintf(o + n, cap - n, "%s{\"name\":\"%s\",\"battery\":%s,\"v\":%.3f,\"min\":", i ? "," : "", s->name, s->battery ? "true" : "false", (double)s->mv / 1000.0);
        n += s->min_mv >= 0 ? (size_t)snprintf(o + n, cap - n, "%.3f}", (double)s->min_mv / 1000.0) : (size_t)snprintf(o + n, cap - n, "null}");
    }
    snprintf(o + n, cap - n, "]}");
    return o;
}
