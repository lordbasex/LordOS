/* vmgpu.c — Machines' graphics cards: the pure parts (vmgpu.h). Reads sysfs under a root folder, parses the admin
 * actions' arguments and the fixed configuration files. No root needed, nothing written. */
#include "vmgpu.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char VMGPU_MODPROBE_TEXT[] =
    "# Written by LordOS (Settings, Graphics cards: Machines, shared). Choosing \"Unused\" there removes it.\n"
    "options i915 enable_gvt=1 enable_guc=0\n";
const char VMGPU_MODLOAD_TEXT[] =
    "# Written by LordOS (Settings, Graphics cards: Machines, shared). Choosing \"Unused\" there removes it.\n"
    "kvmgt\n";

static int read_text(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r < 0) return -1;
    out[r] = 0;
    return 0;
}

static int read_hex(const char *path, unsigned *v) {
    char b[32];
    if (read_text(path, b, sizeof b)) return -1;
    char *e;
    unsigned long x = strtoul(b, &e, 16);
    if (e == b) return -1;
    *v = (unsigned)x;
    return 0;
}

static int read_int(const char *path, int *v) {
    char b[32];
    if (read_text(path, b, sizeof b)) return -1;
    char *e;
    long x = strtol(b, &e, 10);
    if (e == b) return -1;
    *v = (int)x;
    return 0;
}

/* a symbolic link's last component */
static int link_base(const char *path, char *out, size_t n) {
    char t[512];
    ssize_t l = readlink(path, t, sizeof t - 1);
    if (l <= 0) return -1;
    t[l] = 0;
    const char *b = strrchr(t, '/');
    b = b ? b + 1 : t;
    if (strlen(b) >= n) return -1;
    snprintf(out, n, "%s", b);
    return 0;
}

int vmgpu_addr_valid(const char *a) {
    /* dddd:bb:ss.f */
    if (!a || strlen(a) != 12) return 0;
    for (int i = 0; i < 12; i++) {
        char c = a[i];
        if (i == 4 || i == 7) { if (c != ':') return 0; continue; }
        if (i == 10) { if (c != '.') return 0; continue; }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return a[11] >= '0' && a[11] <= '7';
}

int vmgpu_type_valid(const char *t) {
    size_t n = t ? strlen(t) : 0;
    if (!n || n > VMGPU_TYPE_MAX || !strcmp(t, ".") || !strcmp(t, "..")) return 0;
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)t[i]) && t[i] != '_' && t[i] != '-') return 0;
    return 1;
}

int vmgpu_gvt_device(unsigned vendor, unsigned device) {
    if (vendor != 0x8086) return 0;
    switch (device >> 8) {
    case 0x16:          /* Broadwell */
    case 0x19:          /* Skylake */
    case 0x59:          /* Kaby Lake, Amber Lake */
    case 0x3e:          /* Coffee Lake, Whiskey Lake */
    case 0x9b:          /* Comet Lake */
        return 1;
    case 0x87:          /* Amber Lake / Comet Lake Y (0x87c0, 0x87ca) */
        return device == 0x87c0 || device == 0x87ca;
    default:            /* Broxton */
        return device == 0x5a84 || device == 0x5a85 || device == 0x0a84 || device == 0x1a84 || device == 0x1a85;
    }
}

int vmgpu_type_desc(const char *text, VmGpuType *t) {
    int got_res = 0;
    for (const char *p = text; p && *p;) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char line[96];
        if (l < sizeof line) {
            memcpy(line, p, l);
            line[l] = 0;
            int a, b;
            if (sscanf(line, "low_gm_size: %dMB", &a) == 1 && a >= 0 && a < 1 << 20) t->low_mb = a;
            else if (sscanf(line, "high_gm_size: %dMB", &a) == 1 && a >= 0 && a < 1 << 20) t->high_mb = a;
            else if (sscanf(line, "fence: %d", &a) == 1 && a >= 0 && a < 1024) t->fence = a;
            else if (sscanf(line, "weight: %d", &a) == 1 && a >= 0 && a < 1024) t->weight = a;
            else if (sscanf(line, "resolution: %dx%d", &a, &b) == 2 && a > 0 && b > 0 && a <= 16384 && b <= 16384) { t->max_w = a; t->max_h = b; got_res = 1; }
        }
        p = e ? e + 1 : NULL;
    }
    return got_res ? 0 : -1;
}

/* largest first: high_gm, then weight, then name */
static int type_cmp(const void *x, const void *y) {
    const VmGpuType *a = x, *b = y;
    if (a->high_mb != b->high_mb) return b->high_mb - a->high_mb;
    if (a->weight != b->weight) return b->weight - a->weight;
    return strcmp(a->name, b->name);
}

static void read_types(const char *dev, VmGpu *g) {
    char p[1024];
    snprintf(p, sizeof p, "%s/mdev_supported_types", dev);
    DIR *d = opendir(p);
    if (!d) return;
    g->gvt_on = 1;
    for (struct dirent *e; (e = readdir(d)) && g->n_types < VMGPU_TYPES;) {
        if (!vmgpu_type_valid(e->d_name) || e->d_name[0] == '.') continue;
        VmGpuType t = { 0 };
        snprintf(t.name, sizeof t.name, "%.40s", e->d_name);
        char q[1400], text[512];
        snprintf(q, sizeof q, "%s/%s/description", p, e->d_name);
        if (read_text(q, text, sizeof text) || vmgpu_type_desc(text, &t)) continue;
        snprintf(q, sizeof q, "%s/%s/available_instances", p, e->d_name);
        if (read_int(q, &t.available) || t.available < 0) t.available = 0;
        g->types[g->n_types++] = t;
    }
    closedir(d);
    qsort(g->types, (size_t)g->n_types, sizeof g->types[0], type_cmp);
}

static int gpu_cmp(const void *x, const void *y) { return strcmp(((const VmGpu *)x)->addr, ((const VmGpu *)y)->addr); }

int vmgpu_list(const char *root, VmGpu *out, int max) {
    char base[512];
    snprintf(base, sizeof base, "%s/sys/bus/pci/devices", root);
    DIR *d = opendir(base);
    if (!d) return 0;
    int n = 0;
    for (struct dirent *e; (e = readdir(d)) && n < max;) {
        if (!vmgpu_addr_valid(e->d_name)) continue;
        char dev[800], p[1024];
        snprintf(dev, sizeof dev, "%s/%s", base, e->d_name);
        VmGpu g;
        memset(&g, 0, sizeof g);
        snprintf(p, sizeof p, "%s/class", dev);
        if (read_hex(p, &g.cls) || (g.cls >> 16) != 0x03) continue;
        snprintf(g.addr, sizeof g.addr, "%.12s", e->d_name);
        snprintf(p, sizeof p, "%s/vendor", dev);
        read_hex(p, &g.vendor);
        snprintf(p, sizeof p, "%s/device", dev);
        read_hex(p, &g.device);
        snprintf(p, sizeof p, "%s/driver", dev);
        if (link_base(p, g.driver, sizeof g.driver)) g.driver[0] = 0;
        char grp[32];
        snprintf(p, sizeof p, "%s/iommu_group", dev);
        g.iommu_group = !link_base(p, grp, sizeof grp) && isdigit((unsigned char)grp[0]) ? atoi(grp) : -1;
        snprintf(p, sizeof p, "%s/boot_vga", dev);
        int bv = 0;
        g.boot_vga = !read_int(p, &bv) && bv == 1;
        g.card = g.render = -1;
        snprintf(p, sizeof p, "%s/drm", dev);
        DIR *dd = opendir(p);
        for (struct dirent *x; dd && (x = readdir(dd));) {
            if (!strncmp(x->d_name, "card", 4) && isdigit((unsigned char)x->d_name[4])) g.card = atoi(x->d_name + 4);
            else if (!strncmp(x->d_name, "renderD", 7) && isdigit((unsigned char)x->d_name[7])) g.render = atoi(x->d_name + 7);
        }
        if (dd) closedir(dd);
        g.gvt_capable = vmgpu_gvt_device(g.vendor, g.device);
        if (g.gvt_capable) read_types(dev, &g);
        out[n++] = g;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof out[0], gpu_cmp);
    return n;
}

int vmgpu_mark_desktop(const char *root, const char *fd_dir, VmGpu *g, int n) {
    int marked = 0;
    for (int i = 0; i < n; i++) g[i].desktop = 0;
    DIR *d = fd_dir ? opendir(fd_dir) : NULL;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char lp[1400], t[64];
        snprintf(lp, sizeof lp, "%s/%s", fd_dir, e->d_name);
        ssize_t l = readlink(lp, t, sizeof t - 1);
        if (l <= 13) continue;
        t[l] = 0;
        if (strncmp(t, "/dev/dri/card", 13) || !isdigit((unsigned char)t[13])) continue;
        int card = atoi(t + 13);
        for (int i = 0; i < n; i++) if (g[i].card == card && !g[i].desktop) { g[i].desktop = 1; marked++; }
    }
    if (d) closedir(d);
    (void)root;
    if (!marked)
        for (int i = 0; i < n; i++) if (g[i].boot_vga) { g[i].desktop = 1; marked++; }
    /* nothing says which: a single GPU is the desktop's */
    if (!marked && n == 1) { g[0].desktop = 1; marked = 1; }
    return marked;
}

const VmGpu *vmgpu_gvt(const VmGpu *g, int n) {
    for (int i = 0; i < n; i++) if (g[i].gvt_capable && g[i].gvt_on && !g[i].desktop && g[i].n_types) return &g[i];
    return NULL;
}

int vmgpu_pick_type(const VmGpu *g, int whole, int w, int h, int any_state) {
    if (!g || !g->n_types) return -1;
    if (whole) {
        for (int i = 0; i < g->n_types; i++) if (any_state || g->types[i].available > 0) return i;
        return -1;
    }
    /* the types are largest first: the last one that fits */
    int pick = -1;
    for (int i = 0; i < g->n_types; i++) {
        const VmGpuType *t = &g->types[i];
        if (!any_state && t->available <= 0) continue;
        if (w > 0 && h > 0 && (t->max_w < w || t->max_h < h)) continue;
        pick = i;
    }
    return pick;
}

/* "<a> <b>" with exactly one space, no other whitespace or control characters; b may be NULL (one word) */
static int words(const char *arg, size_t len, char *a, size_t an, char *b, size_t bn) {
    if (!arg || !len || len > 128 || memchr(arg, 0, len)) return -1;
    for (size_t i = 0; i < len; i++) if ((unsigned char)arg[i] <= ' ' && arg[i] != ' ') return -1;
    const char *sp = memchr(arg, ' ', len);
    if (!b) {
        if (sp || len >= an) return -1;
        memcpy(a, arg, len);
        a[len] = 0;
        return 0;
    }
    if (!sp) return -1;
    size_t la = (size_t)(sp - arg), lb = len - la - 1;
    if (!la || !lb || la >= an || lb >= bn || memchr(sp + 1, ' ', lb)) return -1;
    memcpy(a, arg, la); a[la] = 0;
    memcpy(b, sp + 1, lb); b[lb] = 0;
    return 0;
}

static int uuid_valid(const char *s) {
    if (strlen(s) != 36) return 0;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return 0; }
        else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    }
    return 1;
}

int vmgpu_role_arg(const char *arg, size_t len, char addr[16], int *role) {
    char r[24];
    if (words(arg, len, addr, 16, r, sizeof r) || !vmgpu_addr_valid(addr)) return -1;
    static const char *const R[] = { "unused", "machines-shared", "machines-dedicated", "ai" };
    for (int i = 0; i < 4; i++) if (!strcmp(r, R[i])) { *role = i; return 0; }
    return -1;
}

int vmgpu_create_arg(const char *arg, size_t len, char id[37], char type[VMGPU_TYPE_MAX + 1]) {
    if (words(arg, len, id, 37, type, VMGPU_TYPE_MAX + 1) || !uuid_valid(id) || !vmgpu_type_valid(type)) return -1;
    return 0;
}

int vmgpu_remove_arg(const char *arg, size_t len, char id[37]) {
    if (words(arg, len, id, 37, NULL, 0) || !uuid_valid(id)) return -1;
    return 0;
}

int vmgpu_freq_arg(const char *arg, size_t len, char addr[16], int *mhz) {
    char v[16];
    if (words(arg, len, addr, 16, v, sizeof v) || !vmgpu_addr_valid(addr)) return -1;
    if (!strcmp(v, "default")) { *mhz = 0; return 0; }
    if (strlen(v) > 5 || v[0] == '0') return -1;
    for (const char *p = v; *p; p++) if (!isdigit((unsigned char)*p)) return -1;
    int x = atoi(v);
    if (x < 1 || x > 10000) return -1;
    *mhz = x;
    return 0;
}

/* the meaningful lines of a configuration file: comments and blanks dropped, spaces collapsed; for "options" lines,
 * the options sorted. Joined with '\n' into out */
static void norm_conf(const char *text, char *out, size_t n) {
    out[0] = 0;
    size_t o = 0;
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char line[256];
        if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, p, l);
        line[l] = 0;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *tok[16];
        int nt = 0;
        char *sv = NULL;
        for (char *s = strtok_r(line, " \t\r", &sv); s && nt < 16; s = strtok_r(NULL, " \t\r", &sv)) tok[nt++] = s;
        if (nt >= 3 && !strcmp(tok[0], "options"))      /* "options <module>" stay first */
            for (int i = 3; i < nt; i++)
                for (int k = i; k > 2 && strcmp(tok[k - 1], tok[k]) > 0; k--) { char *t = tok[k]; tok[k] = tok[k - 1]; tok[k - 1] = t; }
        for (int i = 0; i < nt && o + 2 < n; i++) o += (size_t)snprintf(out + o, n - o, "%s%s", i ? " " : (o ? "\n" : ""), tok[i]);
        if (o >= n) o = n - 1;
        p = e ? e + 1 : p + l;
        if (!e) break;
    }
}

int vmgpu_conf_ours(const char *text, int which) {
    char a[512], b[512];
    norm_conf(text, a, sizeof a);
    norm_conf(which ? VMGPU_MODLOAD_TEXT : VMGPU_MODPROBE_TEXT, b, sizeof b);
    return !strcmp(a, b);
}

static int file_ours(const char *root, const char *path, int which) {
    char p[1024], text[2048];
    snprintf(p, sizeof p, "%s%s", root, path);
    return !read_text(p, text, sizeof text) && vmgpu_conf_ours(text, which);
}

int vmgpu_configured(const char *root) {
    int probe = file_ours(root, VMGPU_MODPROBE, 0) || file_ours(root, VMGPU_LEGACY_MODPROBE, 0);
    int load = file_ours(root, VMGPU_MODLOAD, 1) || file_ours(root, VMGPU_LEGACY_MODLOAD, 1);
    return probe && load ? VMGPU_ROLE_SHARED : VMGPU_ROLE_UNUSED;
}

/* ---- JSON for lord.vm.gpus ---- */

typedef struct { char *p; size_t len, cap; int bad; } Buf;
static void put(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Buf *b, const char *fmt, ...) {
    if (b->bad) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int k = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
        if (k < 0) { b->bad = 1; return; }
        if ((size_t)k < b->cap - b->len) { b->len += (size_t)k; return; }
        size_t nc = b->cap * 2 + (size_t)k;
        char *q = realloc(b->p, nc);
        if (!q) { b->bad = 1; return; }
        b->p = q; b->cap = nc;
    }
}

static const char *vendor_name(unsigned v) { return v == 0x8086 ? "Intel" : v == 0x1002 ? "AMD" : v == 0x10de ? "NVIDIA" : ""; }

char *vmgpu_json(const VmGpu *g, int n, int configured) {
    Buf b = { malloc(1024), 0, 1024, 0 };
    if (!b.p) return NULL;
    put(&b, "{\"gpus\":[");
    int any_on = 0;
    for (int i = 0; i < n; i++) {
        const VmGpu *x = &g[i];
        if (x->gvt_on && !x->desktop) any_on = 1;
        put(&b, "%s{\"address\":\"%s\",\"vendor\":%u,\"device\":%u,\"vendorName\":\"%s\",\"driver\":\"%s\",\"iommuGroup\":%d,"
                "\"desktop\":%s,\"bootVga\":%s,\"render\":\"", i ? "," : "", x->addr, x->vendor, x->device, vendor_name(x->vendor), x->driver,
            x->iommu_group, x->desktop ? "true" : "false", x->boot_vga ? "true" : "false");
        if (x->render >= 0) put(&b, "/dev/dri/renderD%d", x->render);
        put(&b, "\",\"gvtCapable\":%s,\"gvtOn\":%s,\"types\":[", x->gvt_capable ? "true" : "false", x->gvt_on ? "true" : "false");
        for (int k = 0; k < x->n_types; k++) {
            const VmGpuType *t = &x->types[k];
            put(&b, "%s{\"name\":\"%s\",\"lowMb\":%d,\"highMb\":%d,\"maxW\":%d,\"maxH\":%d,\"weight\":%d,\"available\":%d}", k ? "," : "", t->name,
                t->low_mb, t->high_mb, t->max_w, t->max_h, t->weight, t->available);
        }
        put(&b, "]}");
    }
    /* restart: the files say one thing and the running kernel another */
    int restart = (configured == VMGPU_ROLE_SHARED) != any_on;
    int capable = 0;
    for (int i = 0; i < n; i++) if (g[i].gvt_capable && !g[i].desktop) capable = 1;
    if (!capable) restart = 0;
    put(&b, "],\"configured\":\"%s\",\"restart\":%s}", configured == VMGPU_ROLE_SHARED ? "machines-shared" : "unused", restart ? "true" : "false");
    if (b.bad) { free(b.p); return NULL; }
    return b.p;
}
