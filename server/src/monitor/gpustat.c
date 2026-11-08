/* gpustat.c — see gpustat.h. */
#include "gpustat.h"
#include "../sysreport/ids.h"
#include "../sec/json.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PROC_FDS_MAX  16384             /* fd entries looked at per process */
#define PROC_CLIENTS  16                /* DRM clients kept per process */
#define MAPS_MAX      (1u << 20)        /* bytes of /proc/<pid>/maps read */
#define MAPS_LIBS     16
#define MAPS_TTL_MS   10000             /* a process's libraries are read again after 10 s */
#define LIST_MAX      128               /* processes with a DRM client in one answer */
#define CAPS_TTL_MS   60000

/* ---- small helpers ---- */

static double now_ms(int wall) {
    struct timespec ts;
    clock_gettime(wall ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* A small file, NUL-terminated; -1 if it can't be read. Non-blocking: a sensor whose driver hangs is skipped. */
static long rd(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got + 1 < n) {
        ssize_t r = read(fd, out + got, n - 1 - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    out[got] = 0;
    return (long)got;
}

static void pathf(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void pathf(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= n) out[0] = 0;          /* a cut path is never used */
}

static int numbered(const char *name, const char *prefix) {
    size_t l = strlen(prefix);
    if (strncmp(name, prefix, l) || !name[l] || strlen(name) > l + 9) return 0;
    for (const char *p = name + l; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

int gpu_parse_u64(const char *s, unsigned long long *out) {
    unsigned long long v = 0;
    int k = 0;
    for (; *s >= '0' && *s <= '9'; s++, k++) {
        unsigned d = (unsigned)(*s - '0');
        if (k >= 20 || v > (ULLONG_MAX - d) / 10) return -1;
        v = v * 10 + d;
    }
    if (!k || (*s == '\n' && s[1]) || (*s && *s != '\n')) return -1;
    *out = v;
    return 0;
}

/* a sysfs number in [0, hi]; -1 otherwise */
static long long num_file(const char *path, unsigned long long hi) {
    char b[32];
    unsigned long long v;
    if (rd(path, b, sizeof b) <= 0 || gpu_parse_u64(b, &v) || v > hi) return -1;
    return (long long)v;
}

/* only [A-Za-z0-9] and the extra characters, shorter than n */
static int clean_name(const char *s, size_t len, const char *extra, size_t n) {
    if (!len || len >= n) return 0;
    for (size_t i = 0; i < len; i++) if (!isalnum((unsigned char)s[i]) && !strchr(extra, s[i])) return 0;
    return 1;
}

/* ---- JSON ---- */

typedef struct { char *p; size_t n, cap; int bad; } Js;

static void js_grow(Js *b, size_t extra) {
    if (b->bad || b->n + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + extra + 1) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) { b->bad = 1; return; }
    b->p = p;
    b->cap = cap;
}

static void jp(Js *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jp(Js *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= sizeof tmp) { b->bad = 1; return; }
    js_grow(b, (size_t)k);
    if (b->bad) return;
    memcpy(b->p + b->n, tmp, (size_t)k + 1);
    b->n += (size_t)k;
}

/* text already made here (another Js's JSON) */
static void jraw(Js *b, const char *s) {
    size_t k = strlen(s);
    js_grow(b, k);
    if (b->bad) return;
    memcpy(b->p + b->n, s, k + 1);
    b->n += k;
}

/* "text": escapes, control characters as \u00XX, invalid UTF-8 as '?' */
static void jstr(Js *b, const char *s) {
    js_grow(b, strlen(s) * 6 + 2);
    if (b->bad) return;
    char *o = b->p + b->n;
    *o++ = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (*p == '"' || *p == '\\') { *o++ = '\\'; *o++ = (char)*p++; continue; }
        if (*p < 0x20 || *p == 0x7f) { o += sprintf(o, "\\u%04x", *p++); continue; }
        int n = *p < 0x80 ? 1 : *p >= 0xc2 && *p < 0xe0 ? 2 : *p >= 0xe0 && *p < 0xf0 ? 3 : *p >= 0xf0 && *p <= 0xf4 ? 4 : 0;
        for (int i = 1; i < n; i++) if ((p[i] & 0xc0) != 0x80) n = 0;
        if (!n) { *o++ = '?'; p++; continue; }
        memcpy(o, p, (size_t)n);
        o += n;
        p += n;
    }
    *o++ = '"';
    b->n = (size_t)(o - b->p);
    b->p[b->n] = 0;
}

static void jnum(Js *b, const char *key, long long v) {
    if (v < 0) jp(b, ",\"%s\":null", key); else jp(b, ",\"%s\":%lld", key, v);
}

/* ---- fdinfo ---- */

static void kv_put(GpuKV *a, int *n, const char *name, size_t len, unsigned long long v) {
    for (int i = 0; i < *n; i++) if (strlen(a[i].name) == len && !memcmp(a[i].name, name, len)) { a[i].v = v; return; }
    if (*n >= GPU_KEYS) return;
    memcpy(a[*n].name, name, len);
    a[*n].name[len] = 0;
    a[*n].v = v;
    (*n)++;
}

static GpuMem *mem_slot(GpuClient *c, const char *name, size_t len) {
    for (int i = 0; i < c->nmem; i++) if (strlen(c->mem[i].name) == len && !memcmp(c->mem[i].name, name, len)) return &c->mem[i];
    if (c->nmem >= GPU_REGIONS) return NULL;
    GpuMem *m = &c->mem[c->nmem++];
    memcpy(m->name, name, len);
    m->name[len] = 0;
    m->memory = m->total = m->resident = m->shared = -1;
    return m;
}

/* "<digits>" followed by nothing or by one of the allowed units (bytes: "", KiB, MiB, GiB; time: "ns") */
static int value_num(const char *v, size_t len, int bytes, unsigned long long *out) {
    char b[48];
    if (!len || len >= sizeof b) return -1;
    memcpy(b, v, len);
    b[len] = 0;
    char *sp = strchr(b, ' ');
    unsigned long long mul = 1;
    if (sp) {
        *sp = 0;
        const char *u = sp + 1;
        if (bytes && !strcmp(u, "KiB")) mul = 1ull << 10;
        else if (bytes && !strcmp(u, "MiB")) mul = 1ull << 20;
        else if (bytes && !strcmp(u, "GiB")) mul = 1ull << 30;
        else if (!bytes && !strcmp(u, "ns")) mul = 1;
        else return -1;
    }
    unsigned long long x;
    if (gpu_parse_u64(b, &x) || x > ULLONG_MAX / mul) return -1;
    *out = x * mul;
    return 0;
}

int gpu_fdinfo_parse(const char *text, size_t len, GpuClient *c) {
    memset(c, 0, sizeof *c);
    const char *end = text + len;
    for (const char *l = text; l < end;) {
        const char *nl = memchr(l, '\n', (size_t)(end - l));
        const char *le = nl ? nl : end;
        const char *colon = memchr(l, ':', (size_t)(le - l));
        if (colon) {
            const char *k = l, *v = colon + 1;
            size_t kl = (size_t)(colon - l);
            while (v < le && (*v == ' ' || *v == '\t')) v++;
            size_t vl = (size_t)(le - v);
            while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t' || v[vl - 1] == '\r')) vl--;
            unsigned long long x;
#define KEY(s) (kl == sizeof(s) - 1 && !memcmp(k, s, kl))
#define PRE(s) (kl > sizeof(s) - 1 && !memcmp(k, s, sizeof(s) - 1))
#define SUB(s) k + sizeof(s) - 1, kl - (sizeof(s) - 1)
            if (KEY("drm-driver")) {
                if (clean_name(v, vl, "_-", sizeof c->driver)) { memcpy(c->driver, v, vl); c->driver[vl] = 0; }
            } else if (KEY("drm-pdev")) {
                if (clean_name(v, vl, ":._-", sizeof c->pdev)) { memcpy(c->pdev, v, vl); c->pdev[vl] = 0; }
            } else if (KEY("drm-client-id")) {
                if (!value_num(v, vl, 1, &x) && !memchr(v, ' ', vl)) { c->id = x; c->has_id = 1; }
            } else if (PRE("drm-engine-capacity-")) {
                if (clean_name(SUB("drm-engine-capacity-"), "_-", GPU_NAME) && !value_num(v, vl, 1, &x) && !memchr(v, ' ', vl) && x >= 1 && x <= 64)
                    kv_put(c->cap, &c->ncap, SUB("drm-engine-capacity-"), x);
            } else if (PRE("drm-engine-")) {
                if (clean_name(SUB("drm-engine-"), "_-", GPU_NAME) && !value_num(v, vl, 0, &x)) kv_put(c->eng, &c->neng, SUB("drm-engine-"), x);
            } else if (PRE("drm-total-cycles-")) {
                if (clean_name(SUB("drm-total-cycles-"), "_-", GPU_NAME) && !value_num(v, vl, 1, &x) && !memchr(v, ' ', vl))
                    kv_put(c->tcyc, &c->ntcyc, SUB("drm-total-cycles-"), x);
            } else if (PRE("drm-cycles-")) {
                if (clean_name(SUB("drm-cycles-"), "_-", GPU_NAME) && !value_num(v, vl, 1, &x) && !memchr(v, ' ', vl))
                    kv_put(c->cyc, &c->ncyc, SUB("drm-cycles-"), x);
            } else {
                static const char *const kinds[] = { "drm-memory-", "drm-total-", "drm-resident-", "drm-shared-" };
                for (int i = 0; i < 4; i++) {
                    size_t pl = strlen(kinds[i]);
                    if (kl <= pl || memcmp(k, kinds[i], pl)) continue;
                    if (!clean_name(k + pl, kl - pl, "_-", GPU_NAME) || value_num(v, vl, 1, &x) || x > (1ull << 50)) break;
                    GpuMem *m = mem_slot(c, k + pl, kl - pl);
                    if (!m) break;
                    long long *f = i == 0 ? &m->memory : i == 1 ? &m->total : i == 2 ? &m->resident : &m->shared;
                    *f = (long long)x;
                    break;
                }
            }
#undef KEY
#undef PRE
#undef SUB
        }
        if (!nl) break;
        l = nl + 1;
    }
    return c->driver[0] ? 0 : -1;
}

void gpu_dpm_parse(const char *text, int *cur, int *max) {
    *cur = *max = -1;
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        const char *p = l;
        int k = 0;
        while (*p >= '0' && *p <= '9' && k < 3) { p++; k++; }
        if (!k || *p != ':') continue;
        p++;
        while (*p == ' ') p++;
        long mhz = 0;
        k = 0;
        while (*p >= '0' && *p <= '9' && k < 6) { mhz = mhz * 10 + (*p - '0'); p++; k++; }
        if (!k || strncasecmp(p, "mhz", 3)) continue;
        p += 3;
        if (mhz > *max) *max = (int)mhz;
        while (*p == ' ') p++;
        if (*p == '*') *cur = (int)mhz;
    }
}

int gpu_say_parse(const char *text, GpuSay *s) {
    char be[16], q[16], lang[16], mode[16];
    int n = 0;
    memset(s, 0, sizeof *s);
    if (sscanf(text, "lord-say 1 %15s %15s %15s %15s%n", be, q, lang, mode, &n) != 4) return -1;
    if (strcmp(text + n, "\n") && text[n]) return -1;
    if (strcmp(be, "vulkan") && strcmp(be, "cpu")) return -1;
    if (strcmp(q, "hd") && strcmp(q, "standard")) return -1;
    if (strcmp(mode, "speak") && strcmp(mode, "save")) return -1;
    size_t ll = strlen(lang);
    if (ll < 2 || ll > 3) return -1;
    for (size_t i = 0; i < ll; i++) if (lang[i] < 'a' || lang[i] > 'z') return -1;
    memcpy(s->backend, be, strlen(be) + 1);
    memcpy(s->quality, q, strlen(q) + 1);
    memcpy(s->lang, lang, ll + 1);
    memcpy(s->mode, mode, strlen(mode) + 1);
    return 0;
}

/* ---- cards ---- */

static struct { char key[16]; char vname[96], dname[128]; } names[GPU_CARDS];
static int nnames;

static void card_names(GpuCard *g, const char *pci_ids, IdsDb *db, int *opened) {
    char key[16];
    snprintf(key, sizeof key, "%s:%s", g->vendor, g->device);
    for (int i = 0; i < nnames; i++)
        if (!strcmp(names[i].key, key)) { memcpy(g->vname, names[i].vname, sizeof g->vname); memcpy(g->dname, names[i].dname, sizeof g->dname); return; }
    if (!*opened) { *opened = 1; if (!pci_ids || ids_open(db, pci_ids)) memset(db, 0, sizeof *db); }
    ids_lookup(db, g->vendor, g->device, g->vname, sizeof g->vname, g->dname, sizeof g->dname);
    int slot = nnames < GPU_CARDS ? nnames++ : 0;
    memcpy(names[slot].key, key, sizeof key);
    memcpy(names[slot].vname, g->vname, sizeof g->vname);
    memcpy(names[slot].dname, g->dname, sizeof g->dname);
}

static void hex_id(const char *path, char out[8]) {
    char b[16];
    out[0] = 0;
    if (rd(path, b, sizeof b) <= 0) return;
    b[strcspn(b, "\n")] = 0;
    if (strncmp(b, "0x", 2) || strlen(b) != 6) return;
    for (int i = 2; i < 6; i++) if (!isxdigit((unsigned char)b[i])) return;
    for (int i = 0; i < 4; i++) out[i] = (char)tolower((unsigned char)b[i + 2]);
    out[4] = 0;
}

static void link_base(const char *path, char *out, size_t n, const char *extra) {
    char l[PATH_MAX];
    out[0] = 0;
    ssize_t k = readlink(path, l, sizeof l - 1);
    if (k <= 0) return;
    l[k] = 0;
    const char *b = strrchr(l, '/');
    b = b ? b + 1 : l;
    if (clean_name(b, strlen(b), extra, n)) memcpy(out, b, strlen(b) + 1);
}

static void card_hwmon(GpuCard *g, const char *dev) {
    char base[PATH_MAX], hw[PATH_MAX] = "", p[PATH_MAX + 64];
    pathf(base, sizeof base, "%s/hwmon", dev);
    DIR *d = opendir(base);
    for (struct dirent *e; d && (e = readdir(d));)
        if (numbered(e->d_name, "hwmon") && (!hw[0] || strcmp(e->d_name, strrchr(hw, '/') + 1) < 0)) pathf(hw, sizeof hw, "%s/%s", base, e->d_name);
    if (d) closedir(d);
    if (!hw[0]) return;
    for (int i = 1; i <= 8 && g->ntemp < GPU_TEMPS; i++) {
        pathf(p, sizeof p, "%s/temp%d_input", hw, i);
        char b[32];
        long long v;
        if (rd(p, b, sizeof b) <= 0) continue;
        unsigned long long u;
        int neg = b[0] == '-';
        if (gpu_parse_u64(b + neg, &u) || u > 200000) continue;
        v = neg ? -(long long)u : (long long)u;
        if (v < -40000 || v == 0) continue;               /* a sensor that isn't wired reads 0 */
        GpuTemp *t = &g->temp[g->ntemp];
        t->mc = (int)v;
        t->label[0] = 0;
        pathf(p, sizeof p, "%s/temp%d_label", hw, i);
        char lb[32];
        if (rd(p, lb, sizeof lb) > 0) { lb[strcspn(lb, "\n")] = 0; if (clean_name(lb, strlen(lb), " _-", sizeof t->label)) memcpy(t->label, lb, strlen(lb) + 1); }
        g->ntemp++;
    }
    pathf(p, sizeof p, "%s/fan1_input", hw);
    g->fan_rpm = (int)num_file(p, 50000);
    pathf(p, sizeof p, "%s/fan1_max", hw);
    g->fan_max = (int)num_file(p, 50000);
    pathf(p, sizeof p, "%s/pwm1", hw);
    long long pwm = num_file(p, 255);
    pathf(p, sizeof p, "%s/pwm1_max", hw);
    long long pmax = num_file(p, 255);
    if (pmax <= 0) pmax = 255;
    if (pwm >= 0) g->fan_pct = (int)((pwm * 100 + pmax / 2) / pmax);
    pathf(p, sizeof p, "%s/power1_average", hw);
    g->power_uw = num_file(p, 2000000000ull);
    if (g->power_uw < 0) { pathf(p, sizeof p, "%s/power1_input", hw); g->power_uw = num_file(p, 2000000000ull); }
    pathf(p, sizeof p, "%s/power1_cap", hw);
    g->cap_uw = num_file(p, 2000000000ull);
    pathf(p, sizeof p, "%s/energy1_input", hw);
    g->energy_uj = num_file(p, 1ull << 62);
    for (int i = 1; i <= 2; i++) {
        char lb[16];
        pathf(p, sizeof p, "%s/freq%d_label", hw, i);
        if (rd(p, lb, sizeof lb) <= 0) continue;
        lb[strcspn(lb, "\n")] = 0;
        pathf(p, sizeof p, "%s/freq%d_input", hw, i);
        long long hz = num_file(p, 100000000000ull);
        if (hz < 0) continue;
        if (!strcmp(lb, "sclk")) g->sclk_mhz = (int)(hz / 1000000);
        else if (!strcmp(lb, "mclk")) g->mclk_mhz = (int)(hz / 1000000);
    }
}

static int by_card(const void *a, const void *b) { return atoi((const char *)a + 4) - atoi((const char *)b + 4); }

int gpu_cards_read(const char *root, const char *pci_ids, GpuCard *out, int max) {
    char base[PATH_MAX], cards[32][16];
    int nc = 0;
    pathf(base, sizeof base, "%s/sys/class/drm", root);
    DIR *d = opendir(base);
    for (struct dirent *e; d && nc < 32 && (e = readdir(d));)
        if (numbered(e->d_name, "card") && strlen(e->d_name) < sizeof cards[0]) memcpy(cards[nc++], e->d_name, strlen(e->d_name) + 1);
    if (d) closedir(d);
    qsort(cards, (size_t)nc, sizeof cards[0], by_card);
    IdsDb db = { 0 };
    int opened = 0, n = 0;
    for (int i = 0; i < nc && n < max; i++) {
        GpuCard *g = &out[n];
        memset(g, 0, sizeof *g);
        g->busy = g->fan_rpm = g->fan_max = g->fan_pct = g->sclk_mhz = g->mclk_mhz = g->sclk_max = g->mclk_max = g->cur_mhz = g->max_mhz = -1;
        g->vram_used = g->vram_total = g->vis_used = g->vis_total = g->gtt_used = g->gtt_total = g->power_uw = g->cap_uw = g->energy_uj = -1;
        memcpy(g->card, cards[i], strlen(cards[i]) + 1);
        char dir[PATH_MAX], dev[PATH_MAX], p[PATH_MAX + 64], b[1024];
        pathf(dir, sizeof dir, "%s/%s", base, cards[i]);
        pathf(dev, sizeof dev, "%s/device", dir);
        if (!dir[0] || !dev[0]) continue;
        pathf(p, sizeof p, "%s/driver", dev);
        link_base(p, g->driver, sizeof g->driver, "_-");
        link_base(dev, g->slot, sizeof g->slot, ":._-");
        if (!g->driver[0]) continue;                       /* no driver bound: nothing to show */
        pathf(p, sizeof p, "%s/vendor", dev);
        hex_id(p, g->vendor);
        pathf(p, sizeof p, "%s/device", dev);
        hex_id(p, g->device);
        if (g->vendor[0] && g->device[0]) card_names(g, pci_ids, &db, &opened);
        pathf(p, sizeof p, "%s/drm", dev);
        DIR *r = opendir(p);
        for (struct dirent *e; r && (e = readdir(r));) if (numbered(e->d_name, "renderD")) g->render = 1;
        if (r) closedir(r);
        pathf(p, sizeof p, "%s/gpu_busy_percent", dev);
        g->busy = (int)num_file(p, 100);
        static const char *const mem[] = { "vram_used", "vram_total", "vis_vram_used", "vis_vram_total", "gtt_used", "gtt_total" };
        long long *mf[] = { &g->vram_used, &g->vram_total, &g->vis_used, &g->vis_total, &g->gtt_used, &g->gtt_total };
        for (int k = 0; k < 6; k++) { pathf(p, sizeof p, "%s/mem_info_%s", dev, mem[k]); *mf[k] = num_file(p, 1ull << 50); }
        pathf(p, sizeof p, "%s/pp_dpm_sclk", dev);
        if (rd(p, b, sizeof b) > 0) gpu_dpm_parse(b, &g->sclk_mhz, &g->sclk_max);
        pathf(p, sizeof p, "%s/pp_dpm_mclk", dev);
        if (rd(p, b, sizeof b) > 0) gpu_dpm_parse(b, &g->mclk_mhz, &g->mclk_max);
        /* Intel: i915 on the card, xe under its first tile */
        static const char *const act[] = { "%s/gt_act_freq_mhz", "%s/gt_cur_freq_mhz" };
        for (int k = 0; k < 2 && g->cur_mhz < 0; k++) { pathf(p, sizeof p, act[k], dir); g->cur_mhz = (int)num_file(p, 100000); }
        pathf(p, sizeof p, "%s/gt_max_freq_mhz", dir);
        g->max_mhz = (int)num_file(p, 100000);
        if (g->cur_mhz < 0) { pathf(p, sizeof p, "%s/tile0/gt0/freq0/act_freq", dev); g->cur_mhz = (int)num_file(p, 100000); }
        if (g->max_mhz < 0) { pathf(p, sizeof p, "%s/tile0/gt0/freq0/max_freq", dev); g->max_mhz = (int)num_file(p, 100000); }
        card_hwmon(g, dev);
        n++;
    }
    if (opened) ids_close(&db);
    return n;
}

/* ---- processes ---- */

typedef struct {
    int pid, ppid;
    char comm[32], state;
    unsigned long long ticks, start, rss_pages;
} ProcStat;

static int proc_stat(const char *proot, int pid, ProcStat *s) {
    char p[96], st[1024];
    pathf(p, sizeof p, "%s/%d/stat", proot, pid);
    if (rd(p, st, sizeof st) <= 0) return -1;
    char *lp = strchr(st, '('), *rp = strrchr(st, ')');
    if (!lp || !rp || rp < lp || rp[1] != ' ' || !rp[2]) return -1;
    size_t cl = (size_t)(rp - lp - 1);
    if (cl >= sizeof s->comm) cl = sizeof s->comm - 1;
    memcpy(s->comm, lp + 1, cl);
    s->comm[cl] = 0;
    s->pid = pid;
    s->state = isalpha((unsigned char)rp[2]) ? rp[2] : '?';
    unsigned long long v[24] = { 0 };
    char *q = rp + 3;
    for (int i = 0; i < 21; i++) v[i] = strtoull(q, &q, 10);
    s->ppid = (int)v[0];
    s->ticks = v[10] + v[11];
    s->start = v[18];
    s->rss_pages = v[20];
    return 0;
}

/* the AI helpers LordOS starts (docs/MONITOR.md, "GPU"): the model's table (apps/monitor/gpumodel.js) says what each is */
static int ai_name(const char *comm) {
    static const char *const ai[] = { "lord-tts", "lord-ocr", "lord-say" };
    for (size_t i = 0; i < sizeof ai / sizeof *ai; i++) if (!strcmp(comm, ai[i])) return 1;
    return 0;
}

/* the libraries a process maps that tell which GPU API it uses */
static int api_lib(const char *b) {
    static const char *const pre[] = { "libvulkan", "libGLX", "libEGL", "libGL.so", "libGLESv2", "libOpenGL", "libgallium", "libOpenCL", "libRusticlOpenCL",
        "libMesaOpenCL", "libcuda.so", "libnvcuvid", "libnvidia-encode", "libnvidia-glcore", "libnvidia-eglcore", "libva.so", "libva-drm", "libvdpau",
        "libamdhip64", "libggml-vulkan", "libggml-cuda", "libggml-hip" };
    for (size_t i = 0; i < sizeof pre / sizeof *pre; i++) if (!strncmp(b, pre[i], strlen(pre[i]))) return 1;
    const char *so = strstr(b, ".so");
    if (!so) return 0;
    size_t k = (size_t)(so - b);
    return (k > 4 && !strncmp(so - 4, "_dri", 4)) || (k > 10 && !strncmp(so - 10, "_drv_video", 10));
}

int gpu_maps_libs(const char *text, size_t len, char out[][64], int max) {
    int n = 0;
    const char *end = text + len;
    for (const char *l = text; l < end && n < max;) {
        const char *nl = memchr(l, '\n', (size_t)(end - l));
        const char *le = nl ? nl : end;
        const char *path = memchr(l, '/', (size_t)(le - l));
        if (path && le - path < 1024) {
            const char *b = path;
            for (const char *q = path; q < le; q++) if (*q == '/') b = q + 1;
            size_t bl = (size_t)(le - b);
            if (bl && bl < 64 && clean_name(b, bl, "._+-", 64)) {
                char nm[64];
                memcpy(nm, b, bl);
                nm[bl] = 0;
                int dup = 0;
                for (int i = 0; i < n && !dup; i++) dup = !strcmp(out[i], nm);
                if (!dup && api_lib(nm)) memcpy(out[n++], nm, bl + 1);
            }
        }
        if (!nl) break;
        l = nl + 1;
    }
    return n;
}

static struct { int pid; unsigned long long start; double at; int n; char libs[MAPS_LIBS][64]; } mcache[64];

static int proc_libs(const char *proot, const ProcStat *s, char (**libs)[64]) {
    int slot = s->pid & 63;
    double now = now_ms(0);
    if (mcache[slot].pid != s->pid || mcache[slot].start != s->start || now - mcache[slot].at > MAPS_TTL_MS) {
        mcache[slot].pid = s->pid;
        mcache[slot].start = s->start;
        mcache[slot].at = now;
        mcache[slot].n = 0;
        char p[96];
        pathf(p, sizeof p, "%s/%d/maps", proot, s->pid);
        char *t = malloc(MAPS_MAX + 1);
        long k = t ? rd(p, t, MAPS_MAX + 1) : -1;
        if (k > 0) mcache[slot].n = gpu_maps_libs(t, (size_t)k, mcache[slot].libs, MAPS_LIBS);
        free(t);
    }
    *libs = mcache[slot].libs;
    return mcache[slot].n;
}

static int same_client(const GpuClient *a, const GpuClient *b) {
    return a->has_id && b->has_id && a->id == b->id && !strcmp(a->pdev, b->pdev) && !strcmp(a->driver, b->driver);
}

/* the process's DRM clients (deduplicated: a client opened once and duplicated shows in several fds) */
static int proc_clients(const char *proot, int pid, GpuClient *out, int max, char *fdbuf) {
    char p[96];
    pathf(p, sizeof p, "%s/%d/fd", proot, pid);
    DIR *d = opendir(p);
    if (!d) return 0;
    int n = 0, seen = 0;
    for (struct dirent *e; (e = readdir(d)) && seen < PROC_FDS_MAX;) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        seen++;
        char l[64];
        ssize_t k = readlinkat(dirfd(d), e->d_name, l, sizeof l - 1);
        if (k <= 9) continue;
        l[k] = 0;
        if (strncmp(l, "/dev/dri/", 9)) continue;
        char fp[128];
        pathf(fp, sizeof fp, "%s/%d/fdinfo/%.16s", proot, pid, e->d_name);
        long got = rd(fp, fdbuf, GPU_FDINFO_MAX);
        if (got <= 0) continue;
        GpuClient c;
        if (gpu_fdinfo_parse(fdbuf, (size_t)got, &c)) continue;
        int dup = 0;
        for (int i = 0; i < n && !dup; i++) dup = same_client(&out[i], &c);
        if (!dup && n < max) out[n++] = c;
    }
    closedir(d);
    return n;
}

static void client_json(Js *b, const GpuClient *c) {
    jp(b, "{\"driver\":");
    jstr(b, c->driver);
    jp(b, ",\"pdev\":");
    jstr(b, c->pdev);
    if (c->has_id) jp(b, ",\"id\":%llu", c->id); else jp(b, ",\"id\":null");
    const GpuKV *sets[] = { c->eng, c->cap, c->cyc, c->tcyc };
    const int ns[] = { c->neng, c->ncap, c->ncyc, c->ntcyc };
    static const char *const keys[] = { "engines", "capacity", "cycles", "totalCycles" };
    for (int s = 0; s < 4; s++) {
        jp(b, ",\"%s\":{", keys[s]);
        for (int i = 0; i < ns[s]; i++) jp(b, "%s\"%s\":%llu", i ? "," : "", sets[s][i].name, sets[s][i].v);
        jp(b, "}");
    }
    jp(b, ",\"mem\":{");
    for (int i = 0; i < c->nmem; i++) {
        const GpuMem *m = &c->mem[i];
        jp(b, "%s\"%s\":{\"memory\":%lld,\"total\":%lld,\"resident\":%lld,\"shared\":%lld}", i ? "," : "", m->name, m->memory, m->total, m->resident, m->shared);
    }
    jp(b, "}}");
}

static void card_json(Js *b, const GpuCard *g, int first) {
    jp(b, "%s{\"card\":", first ? "" : ",");
    jstr(b, g->card);
    jp(b, ",\"driver\":");
    jstr(b, g->driver);
    jp(b, ",\"slot\":");
    jstr(b, g->slot);
    jp(b, ",\"vendor\":");
    jstr(b, g->vendor);
    jp(b, ",\"device\":");
    jstr(b, g->device);
    jp(b, ",\"vendorName\":");
    jstr(b, g->vname);
    jp(b, ",\"deviceName\":");
    jstr(b, g->dname);
    jp(b, ",\"render\":%s", g->render ? "true" : "false");
    jnum(b, "busy", g->busy);
    jnum(b, "vramUsed", g->vram_used);
    jnum(b, "vramTotal", g->vram_total);
    jnum(b, "visUsed", g->vis_used);
    jnum(b, "visTotal", g->vis_total);
    jnum(b, "gttUsed", g->gtt_used);
    jnum(b, "gttTotal", g->gtt_total);
    jp(b, ",\"temps\":[");
    for (int i = 0; i < g->ntemp; i++) {
        jp(b, "%s{\"label\":", i ? "," : "");
        jstr(b, g->temp[i].label);
        jp(b, ",\"c\":%.1f}", g->temp[i].mc / 1000.0);
    }
    jp(b, "]");
    jnum(b, "fanRpm", g->fan_rpm);
    jnum(b, "fanMax", g->fan_max);
    jnum(b, "fanPct", g->fan_pct);
    if (g->power_uw >= 0) jp(b, ",\"powerW\":%.2f", g->power_uw / 1e6); else jp(b, ",\"powerW\":null");
    if (g->cap_uw > 0) jp(b, ",\"powerCapW\":%.1f", g->cap_uw / 1e6); else jp(b, ",\"powerCapW\":null");
    jnum(b, "energyUj", g->energy_uj);
    jnum(b, "sclk", g->sclk_mhz);
    jnum(b, "sclkMax", g->sclk_max);
    jnum(b, "mclk", g->mclk_mhz);
    jnum(b, "mclkMax", g->mclk_max);
    jnum(b, "curMhz", g->cur_mhz);
    jnum(b, "maxMhz", g->max_mhz);
    jp(b, "}");
}

/* ---- the APIs installed (read every CAPS_TTL_MS) ---- */

static int list_names(const char *dir, const char *suffix, char out[][64], int max) {
    DIR *d = opendir(dir);
    int n = 0;
    for (struct dirent *e; d && n < max && (e = readdir(d));) {
        size_t l = strlen(e->d_name), sl = strlen(suffix);
        if (e->d_name[0] == '.' || l >= 64 || l <= sl || strcmp(e->d_name + l - sl, suffix) || !clean_name(e->d_name, l, "._+-", 64)) continue;
        memcpy(out[n++], e->d_name, l + 1);
    }
    if (d) closedir(d);
    return n;
}

static void names_json(Js *b, const char *key, char names_[][64], int n) {
    jp(b, ",\"%s\":[", key);
    for (int i = 0; i < n; i++) { if (i) jp(b, ","); jstr(b, names_[i]); }
    jp(b, "]");
}

/* Vulkan ICDs and OpenCL vendors: the library each names (its base name) and Vulkan's api_version */
static void icds_json(Js *b, const char *root) {
    static const char *const dirs[] = { "/usr/share/vulkan/icd.d", "/etc/vulkan/icd.d" };
    jp(b, ",\"vulkan\":[");
    int first = 1;
    for (int k = 0; k < 2; k++) {
        char dir[PATH_MAX], files[16][64];
        pathf(dir, sizeof dir, "%s%s", root, dirs[k]);
        int n = list_names(dir, ".json", files, 16);
        for (int i = 0; i < n; i++) {
            char p[PATH_MAX + 80], t[4096];
            pathf(p, sizeof p, "%s/%s", dir, files[i]);
            long len = rd(p, t, sizeof t);
            JDoc doc;
            if (len <= 0 || json_parse(t, (size_t)len, &doc, NULL, 0)) continue;
            const JVal *icd = json_get(doc.root, "ICD");
            const char *lib = json_str(icd, "library_path"), *api = json_str(icd, "api_version");
            if (lib) {
                const char *lb = strrchr(lib, '/');
                lb = lb ? lb + 1 : lib;
                jp(b, "%s{\"file\":", first ? "" : ",");
                jstr(b, files[i]);
                jp(b, ",\"lib\":");
                jstr(b, strlen(lb) < 64 ? lb : "");
                jp(b, ",\"api\":");
                jstr(b, api && strlen(api) < 16 ? api : "");
                jp(b, "}");
                first = 0;
            }
            json_free(&doc);
        }
    }
    jp(b, "],\"opencl\":[");
    char dir[PATH_MAX], files[16][64];
    pathf(dir, sizeof dir, "%s/etc/OpenCL/vendors", root);
    int n = list_names(dir, ".icd", files, 16);
    for (int i = 0; i < n; i++) {
        char p[PATH_MAX + 80], t[512];
        pathf(p, sizeof p, "%s/%s", dir, files[i]);
        if (rd(p, t, sizeof t) <= 0) continue;
        t[strcspn(t, "\n")] = 0;
        const char *lb = strrchr(t, '/');
        lb = lb ? lb + 1 : t;
        jp(b, "%s{\"file\":", i ? "," : "");
        jstr(b, files[i]);
        jp(b, ",\"lib\":");
        jstr(b, strlen(lb) < 64 ? lb : "");
        jp(b, "}");
    }
    jp(b, "]");
}

static char *caps_build(const char *root) {
    Js b = { 0 };
    jp(&b, "{\"v\":1");
    /* the multiarch library folders: /usr/lib/<triplet> */
    char base[PATH_MAX], libdirs[4][PATH_MAX];
    int nl = 0;
    pathf(base, sizeof base, "%s/usr/lib", root);
    DIR *d = opendir(base);
    for (struct dirent *e; d && nl < 4 && (e = readdir(d));)
        if (strstr(e->d_name, "-linux-gnu") && strlen(e->d_name) < 40 && clean_name(e->d_name, strlen(e->d_name), "_-", 40)) pathf(libdirs[nl++], PATH_MAX, "%s/%s", base, e->d_name);
    if (d) closedir(d);
    char dri[48][64], va[24][64], vdpau[16][64], libs[64][64];
    int ndri = 0, nva = 0, nvd = 0, nlibs = 0;
    for (int i = 0; i < nl; i++) {
        char p[PATH_MAX + 16];
        pathf(p, sizeof p, "%s/dri", libdirs[i]);
        ndri += list_names(p, "_dri.so", dri + ndri, 48 - ndri);
        nva += list_names(p, "_drv_video.so", va + nva, 24 - nva);
        pathf(p, sizeof p, "%s/vdpau", libdirs[i]);
        nvd += list_names(p, ".so.1", vdpau + nvd, 16 - nvd);
        /* the loaders and vendor runtimes, by name only (never loaded) */
        DIR *ld = opendir(libdirs[i]);
        for (struct dirent *e; ld && nlibs < 64 && (e = readdir(ld));) {
            static const char *const want[] = { "libvulkan.so.1", "libOpenCL.so.1", "libcuda.so.1", "libnvcuvid.so.1", "libnvidia-encode.so.1", "libamdhip64.so",
                "libva.so.2", "libvdpau.so.1", "libEGL.so.1", "libGLX.so.0", "libGLESv2.so.2", "libRusticlOpenCL.so.1", "libMesaOpenCL.so.1" };
            /* libgallium-<Mesa version>.so tells Mesa's version */
            int hit = !strncmp(e->d_name, "libgallium-", 11) && strlen(e->d_name) < 64 && clean_name(e->d_name, strlen(e->d_name), "._+-", 64);
            for (size_t w = 0; w < sizeof want / sizeof *want && !hit; w++) hit = !strcmp(e->d_name, want[w]);
            if (!hit) continue;
            int dup = 0;
            for (int k = 0; k < nlibs && !dup; k++) dup = !strcmp(libs[k], e->d_name);
            if (!dup) memcpy(libs[nlibs++], e->d_name, strlen(e->d_name) + 1);
        }
        if (ld) closedir(ld);
    }
    char p[PATH_MAX];
    pathf(p, sizeof p, "%s/opt/rocm/lib/libamdhip64.so", root);
    if (nlibs < 64 && !access(p, F_OK)) memcpy(libs[nlibs++], "libamdhip64.so", 15);
    names_json(&b, "dri", dri, ndri);
    names_json(&b, "va", va, nva);
    names_json(&b, "vdpau", vdpau, nvd);
    names_json(&b, "libs", libs, nlibs);
    icds_json(&b, root);
    pathf(p, sizeof p, "%s/dev/kfd", root);
    jp(&b, ",\"kfd\":%s", access(p, F_OK) ? "false" : "true");
    char nv[256] = "";
    pathf(p, sizeof p, "%s/proc/driver/nvidia/version", root);
    if (rd(p, nv, sizeof nv) > 0) nv[strcspn(nv, "\n")] = 0;
    jp(&b, ",\"nvidia\":");
    jstr(&b, nv);
    jp(&b, "}");
    if (b.bad) { free(b.p); return NULL; }
    return b.p;
}

/* ---- the whole answer ---- */

static void say_note(Js *b, const char *home, const ProcStat *s, unsigned uid, int *first) {
    GpuSay say;
    char p[PATH_MAX], t[128];
    if (!home || !*home) return;
    pathf(p, sizeof p, "%s/.cache/lordos/say/%d", home, s->pid);
    int fd = p[0] ? open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) : -1;
    if (fd < 0) return;
    struct stat st;
    ssize_t k = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == uid ? read(fd, t, sizeof t - 1) : -1;
    close(fd);
    if (k <= 0) return;
    t[k] = 0;
    if (gpu_say_parse(t, &say)) return;
    jp(b, "%s{\"pid\":%d,\"state\":\"%c\",\"backend\":\"%s\",\"quality\":\"%s\",\"lang\":\"%s\",\"mode\":\"%s\"}", *first ? "" : ",", s->pid, s->state,
       say.backend, say.quality, say.lang, say.mode);
    *first = 0;
}

char *gpu_json(const char *root, const char *pci_ids, unsigned uid, int self, const char *home) {
    static GpuCard cards[GPU_CARDS];
    static GpuClient clients[PROC_CLIENTS];
    static char *caps;
    static double caps_at;
    static char caps_root[64];
    char proot[PATH_MAX];
    pathf(proot, sizeof proot, "%s/proc", root);
    char *fdbuf = malloc(GPU_FDINFO_MAX);
    if (!fdbuf) return NULL;
    Js b = { 0 }, ai = { 0 }, say = { 0 }, vms = { 0 };
    long hz = sysconf(_SC_CLK_TCK), page = sysconf(_SC_PAGESIZE);
    jp(&b, "{\"t\":%.0f,\"mono\":%.0f,\"hz\":%ld,\"cards\":[", now_ms(1), now_ms(0), hz);
    int nc = gpu_cards_read(root, pci_ids, cards, GPU_CARDS);
    for (int i = 0; i < nc; i++) card_json(&b, &cards[i], i == 0);
    jp(&b, "],\"list\":[");
    int nlist = 0, fai = 1, fsay = 1, fvm = 1;
    DIR *d = opendir(proot);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9' || strlen(e->d_name) > 9) continue;
        int pid = atoi(e->d_name);
        ProcStat s;
        if (proc_stat(proot, pid, &s)) continue;
        char p[PATH_MAX];
        struct stat sb;
        pathf(p, sizeof p, "%s/%d", proot, pid);
        int own = !stat(p, &sb) && sb.st_uid == uid;
        if (ai_name(s.comm)) {
            jp(&ai, "%s{\"pid\":%d,\"ppid\":%d,\"name\":", fai ? "" : ",", pid, s.ppid);
            jstr(&ai, s.comm);
            jp(&ai, ",\"state\":\"%c\",\"own\":%s}", s.state, own ? "true" : "false");
            fai = 0;
            if (!strcmp(s.comm, "lord-say")) say_note(&say, home, &s, uid, &fsay);
        }
        if (!own) continue;                                 /* another user's (or a sandboxed helper's) fds aren't ours to read */
        if (!strncmp(s.comm, "qemu-system", 11)) {
            jp(&vms, "%s{\"pid\":%d,\"ticks\":%llu,\"rss\":%llu}", fvm ? "" : ",", pid, s.ticks, s.rss_pages * (unsigned long long)page);
            fvm = 0;
        }
        if (nlist >= LIST_MAX) continue;
        int n = proc_clients(proot, pid, clients, PROC_CLIENTS, fdbuf);
        if (!n) continue;
        char cmd[256] = "", exe[PATH_MAX] = "";
        pathf(p, sizeof p, "%s/%d/cmdline", proot, pid);
        long cl = rd(p, cmd, sizeof cmd);
        if (cl > 0) {
            char *bs = strrchr(cmd, '/');
            if (bs) memmove(cmd, bs + 1, strlen(bs + 1) + 1);
        }
        pathf(p, sizeof p, "%s/%d/exe", proot, pid);
        ssize_t k = readlink(p, exe, sizeof exe - 1);
        exe[k > 0 ? k : 0] = 0;
        jp(&b, "%s{\"pid\":%d,\"ppid\":%d,\"name\":", nlist ? "," : "", pid, s.ppid);
        jstr(&b, s.comm);
        jp(&b, ",\"cmd\":");
        jstr(&b, cmd);
        jp(&b, ",\"exe\":");
        jstr(&b, exe);
        jp(&b, ",\"self\":%s,\"libs\":[", pid == self ? "true" : "false");
        char (*libs)[64];
        int nlibs = proc_libs(proot, &s, &libs);
        for (int i = 0; i < nlibs; i++) { if (i) jp(&b, ","); jstr(&b, libs[i]); }
        jp(&b, "],\"clients\":[");
        for (int i = 0; i < n; i++) { if (i) jp(&b, ","); client_json(&b, &clients[i]); }
        jp(&b, "]}");
        nlist++;
    }
    if (d) closedir(d);
    double now = now_ms(0);
    if (!caps || now - caps_at > CAPS_TTL_MS || strncmp(caps_root, root, sizeof caps_root)) {
        free(caps);
        caps = caps_build(root);
        caps_at = now;
        snprintf(caps_root, sizeof caps_root, "%s", root);
    }
    jraw(&b, "],\"ai\":[");
    jraw(&b, ai.p ? ai.p : "");
    jraw(&b, "],\"say\":[");
    jraw(&b, say.p ? say.p : "");
    jraw(&b, "],\"vms\":[");
    jraw(&b, vms.p ? vms.p : "");
    jraw(&b, "],\"caps\":");
    jraw(&b, caps ? caps : "null");
    jraw(&b, "}");
    int bad = b.bad || ai.bad || say.bad || vms.bad;
    free(ai.p);
    free(say.p);
    free(vms.p);
    free(fdbuf);
    if (bad) { free(b.p); return NULL; }
    return b.p;
}
