/* vmusb.c — a host USB device for a machine: sysfs, the rules, the checks (vmusb.h; docs/VIRTUALIZATION.md 6.6). */
#include "vmusb.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* one sysfs attribute, its last newline taken off: 0, or -1 */
static int attr(const char *sys, const char *name, const char *what, char *buf, size_t n) {
    char p[PATH_MAX];
    if (snprintf(p, sizeof p, "%s/%s/%s", sys, name, what) >= (int)sizeof p) return -1;
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' ')) buf[--r] = 0;
    return 0;
}

static int hex4(const char *s, unsigned *v) {
    if (strlen(s) != 4) return -1;
    unsigned x = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') x = x * 16 + (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') x = x * 16 + (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') x = x * 16 + (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *v = x;
    return 0;
}

static int hex2(const char *s) {
    if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1]) || s[2]) return -1;
    return (int)strtol(s, NULL, 16);
}

/* a decimal of 1 to `max` digits' worth, no sign or leading zero: 0, or -1 */
static int dec(const char *s, size_t len, int lo, int hi, int *out) {
    if (!len || len > 6 || (s[0] == '0' && len > 1)) return -1;
    long v = 0;
    for (size_t i = 0; i < len; i++) { if (s[i] < '0' || s[i] > '9') return -1; v = v * 10 + (s[i] - '0'); }
    if (v < lo || v > hi) return -1;
    *out = (int)v;
    return 0;
}

/* printable ASCII only (a device's strings are the device's: shown as text, never trusted) */
static void clean(char *dst, size_t n, const char *src) {
    size_t o = 0;
    for (const unsigned char *s = (const unsigned char *)src; *s && o + 1 < n; s++)
        if (*s >= 0x20 && *s < 0x7f) dst[o++] = (char)*s;
    while (o && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

static int serial_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || n > VMUSB_SERIAL_MAX) return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && !strchr("._-", *s)) return 0;
    return 1;
}

/* a child device's folder inside its hub's ("1-11.3" in "1-11"): never walked into */
static int is_device_name(const char *s) {
    if (!isdigit((unsigned char)s[0])) return 0;
    const char *d = strchr(s, '-');
    if (!d || !d[1]) return 0;
    for (const char *p = d + 1; *p; p++) if (!isdigit((unsigned char)*p) && *p != '.') return 0;
    return 1;
}

/* the nodes below dir (an open directory), depth first; seen counts what was looked at, to bound a strange tree */
static void walk(int dfd, int depth, VmUsbNode *out, int max, int *n, int *seen) {
    if (depth > 8) return;
    DIR *d = fdopendir(dfd);
    if (!d) { close(dfd); return; }
    /* this folder: a node of the input or block class? */
    char buf[32], sub[PATH_MAX];
    int fd = openat(dfd, "dev", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0) {
        ssize_t r = read(fd, buf, sizeof buf - 1);
        close(fd);
        unsigned ma, mi;
        ssize_t l = readlinkat(dfd, "subsystem", sub, sizeof sub - 1);
        if (r > 0 && l > 0 && depth > 0) {
            buf[r] = 0;
            sub[l] = 0;
            const char *base = strrchr(sub, '/');
            base = base ? base + 1 : sub;
            int blk = !strcmp(base, "block"), inp = !strcmp(base, "input");
            if ((blk || inp) && sscanf(buf, "%u:%u", &ma, &mi) == 2 && *n < max) {
                struct stat hs;
                int held = 0;
                if (blk && !fstatat(dfd, "holders", &hs, AT_SYMLINK_NOFOLLOW) && S_ISDIR(hs.st_mode)) {
                    int hfd = openat(dfd, "holders", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                    DIR *h = hfd >= 0 ? fdopendir(hfd) : NULL;
                    if (!h && hfd >= 0) close(hfd);
                    for (struct dirent *e; h && (e = readdir(h));) if (e->d_name[0] != '.') { held = 1; break; }
                    if (h) closedir(h);
                }
                /* an input class folder without a node of its own (inputN) has no "dev"; event, mouse and js do */
                out[(*n)++] = (VmUsbNode){ ma, mi, blk, held };
            }
        }
    }
    for (struct dirent *e; (e = readdir(d)) && *seen < 4000;) {
        (*seen)++;
        if (e->d_name[0] == '.') continue;
        if (depth > 0 && is_device_name(e->d_name)) continue;
        struct stat st;
        if (fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode)) continue;
        if (!strcmp(e->d_name, "power") || !strcmp(e->d_name, "holders") || !strcmp(e->d_name, "slaves")) continue;
        int c = openat(dfd, e->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (c >= 0) walk(c, depth + 1, out, max, n, seen);
    }
    closedir(d);
}

int vmusb_nodes(const char *sys, const VmUsbDev *dv, VmUsbNode *out, int max) {
    char p[PATH_MAX];
    if (snprintf(p, sizeof p, "%s/%s", sys, dv->name) >= (int)sizeof p) return 0;
    int fd = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC);     /* the bus's folder links to the device's */
    if (fd < 0) return 0;
    int n = 0, seen = 0;
    walk(fd, 0, out, max, &n, &seen);
    return n;
}

static int read_dev(const char *sys, const char *name, VmUsbDev *d) {
    char b[256];
    memset(d, 0, sizeof *d);
    if (strlen(name) >= sizeof d->name || strchr(name, ':') || strchr(name, '/')) return -1;
    snprintf(d->name, sizeof d->name, "%s", name);
    if (attr(sys, name, "busnum", b, sizeof b) || dec(b, strlen(b), 1, VMUSB_BUS_MAX, &d->bus)) return -1;
    if (attr(sys, name, "devnum", b, sizeof b) || dec(b, strlen(b), 1, VMUSB_DEV_MAX, &d->dev)) return -1;
    if (attr(sys, name, "idVendor", b, sizeof b) || hex4(b, &d->vendor)) return -1;
    if (attr(sys, name, "idProduct", b, sizeof b) || hex4(b, &d->product)) return -1;
    if (attr(sys, name, "dev", b, sizeof b) || sscanf(b, "%u:%u", &d->major, &d->minor) != 2) return -1;
    if (!attr(sys, name, "speed", b, sizeof b)) d->speed = atoi(b) ? atoi(b) : 1;
    if (!strncmp(name, "usb", 3)) d->flags |= VMUSB_F_ROOTHUB;
    int cls = !attr(sys, name, "bDeviceClass", b, sizeof b) ? hex2(b) : -1;
    if (!attr(sys, name, "serial", b, sizeof b)) { clean(d->serial, sizeof d->serial, b); if (!serial_ok(d->serial)) d->serial[0] = 0; }
    if (!attr(sys, name, "manufacturer", b, sizeof b)) clean(d->maker, sizeof d->maker, b);
    if (!attr(sys, name, "product", b, sizeof b)) clean(d->model, sizeof d->model, b);
    /* the interfaces: "<name>:<config>.<n>" in the device's folder */
    int classes[64], nc = 0;
    if (cls >= 0) classes[nc++] = cls;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", sys, name);
    DIR *dir = opendir(p);
    size_t nl = strlen(name);
    for (struct dirent *e; dir && (e = readdir(dir)) && nc < 64;) {
        if (strncmp(e->d_name, name, nl) || e->d_name[nl] != ':') continue;
        char q[300];
        snprintf(q, sizeof q, "%s/%s", name, e->d_name);
        int ic;
        if (!attr(sys, q, "bInterfaceClass", b, sizeof b) && (ic = hex2(b)) >= 0) classes[nc++] = ic;
    }
    if (dir) closedir(dir);
    for (int i = 0; i < nc; i++) {
        switch (classes[i]) {
        case 0x09: d->flags |= VMUSB_F_HUB; break;
        case 0x08: d->flags |= VMUSB_F_STORAGE; break;
        case 0x0e: d->flags |= VMUSB_F_VIDEO; break;
        case 0x01: d->flags |= VMUSB_F_AUDIO; break;
        case 0x03: d->flags |= VMUSB_F_HID; break;
        case 0x07: d->flags |= VMUSB_F_PRINTER; break;
        }
    }
    VmUsbNode nodes[64];
    int nn = vmusb_nodes(sys, d, nodes, 64);
    for (int i = 0; i < nn; i++) d->flags |= nodes[i].block ? VMUSB_F_BLOCK : VMUSB_F_INPUT;
    return 0;
}

static int by_place(const void *a, const void *b) {
    const VmUsbDev *x = a, *y = b;
    return x->bus != y->bus ? x->bus - y->bus : x->dev - y->dev;
}

int vmusb_list(const char *sys, VmUsbDev *out, int max) {
    DIR *d = opendir(sys);
    if (!d) return 0;
    int n = 0;
    for (struct dirent *e; (e = readdir(d)) && n < max;) {
        if (e->d_name[0] == '.' || strchr(e->d_name, ':') || !strncmp(e->d_name, "usb", 3)) continue;
        if (!read_dev(sys, e->d_name, &out[n])) n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, by_place);
    return n;
}

int vmusb_find(const char *sys, int bus, int dev, VmUsbDev *out) {
    DIR *d = opendir(sys);
    if (!d) return -1;
    int found = -1;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;
        VmUsbDev x;
        if (!read_dev(sys, e->d_name, &x) && x.bus == bus && x.dev == dev) { *out = x; found = 0; break; }
    }
    closedir(d);
    return found;
}

static int uuid_ok(const char *s, size_t n) {
    if (n != 36) return 0;
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return 0; }
        else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    }
    return 1;
}

/* "vvvv:pppp" exactly, lowercase */
static int ids(const char *s, size_t n, unsigned *v, unsigned *p) {
    if (n != 9 || s[4] != ':') return -1;
    for (size_t i = 0; i < 9; i++) if (i != 4 && !((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return -1;
    char a[5], b[5];
    memcpy(a, s, 4); a[4] = 0;
    memcpy(b, s + 5, 4); b[4] = 0;
    return hex4(a, v) || hex4(b, p) ? -1 : 0;
}

int vmusb_req_parse(const char *arg, size_t len, char id[37], int *bus, int *dev, unsigned *vendor, unsigned *product) {
    if (!arg || len > 80 || memchr(arg, 0, len)) return -1;
    const char *f[4];
    size_t fl[4];
    int nf = 0;
    size_t st = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i < len && arg[i] != ' ') continue;
        if (i == st || nf == 4) return -1;          /* an empty field (leading, doubled or trailing space) or a fifth */
        f[nf] = arg + st;
        fl[nf++] = i - st;
        st = i + 1;
    }
    if (nf != 4) return -1;
    if (!uuid_ok(f[0], fl[0])) return -1;
    if (dec(f[1], fl[1], 1, VMUSB_BUS_MAX, bus) || dec(f[2], fl[2], 1, VMUSB_DEV_MAX, dev)) return -1;
    if (ids(f[3], fl[3], vendor, product)) return -1;
    memcpy(id, f[0], 36);
    id[36] = 0;
    return 0;
}

int vmusb_rule_parse(const char *s, VmUsbRule *r) {
    if (!s) return -1;
    size_t n = strlen(s);
    memset(r, 0, sizeof *r);
    if (n < 9 || ids(s, 9, &r->vendor, &r->product)) return -1;
    if (n == 9) return 0;
    if (s[9] != ':' || !serial_ok(s + 10)) return -1;
    snprintf(r->serial, sizeof r->serial, "%s", s + 10);
    return 0;
}

void vmusb_rule_str(const VmUsbRule *r, char *out, size_t n) {
    if (r->serial[0]) snprintf(out, n, "%04x:%04x:%s", r->vendor, r->product, r->serial);
    else snprintf(out, n, "%04x:%04x", r->vendor, r->product);
}

int vmusb_rule_match(const VmUsbRule *r, const VmUsbDev *d) {
    if (r->vendor != d->vendor || r->product != d->product) return 0;
    return !r->serial[0] || !strcmp(r->serial, d->serial);
}

int vmusb_input_held(const char *proc, const VmUsbNode *n, int nn, pid_t skip_pid) {
    int any = 0;
    for (int i = 0; i < nn; i++) if (!n[i].block) any = 1;
    if (!any) return 0;
    DIR *d = opendir(proc);
    if (!d) return 1;                       /* can't tell: as if held */
    int held = 0;
    for (struct dirent *e; !held && (e = readdir(d));) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        if (skip_pid > 0 && atol(e->d_name) == (long)skip_pid) continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s/fd", proc, e->d_name);
        int fdd = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        DIR *fds = fdd >= 0 ? fdopendir(fdd) : NULL;
        if (!fds) { if (fdd >= 0) close(fdd); continue; }
        for (struct dirent *f; !held && (f = readdir(fds));) {
            struct stat st;
            if (f->d_name[0] == '.' || fstatat(fdd, f->d_name, &st, 0) || !S_ISCHR(st.st_mode)) continue;
            for (int i = 0; i < nn; i++)
                if (!n[i].block && major(st.st_rdev) == n[i].major && minor(st.st_rdev) == n[i].minor) { held = 1; break; }
        }
        closedir(fds);
    }
    closedir(d);
    return held;
}

int vmusb_block_busy(const char *sys, const VmUsbDev *dv, const char *mountinfo, const char *swaps) {
    VmUsbNode n[64];
    int nn = vmusb_nodes(sys, dv, n, 64), any = 0;
    for (int i = 0; i < nn; i++) if (n[i].block) { any = 1; if (n[i].held) return 1; }
    if (!any) return 0;
    char line[4096];
    FILE *f = fopen(mountinfo, "re");
    if (!f) return 1;
    int busy = 0;
    while (!busy && fgets(line, sizeof line, f)) {
        unsigned ma, mi;
        /* "36 35 98:0 /mnt1 /mnt2 …": the third field */
        if (sscanf(line, "%*s %*s %u:%u", &ma, &mi) != 2) continue;
        for (int i = 0; i < nn; i++) if (n[i].block && n[i].major == ma && n[i].minor == mi) { busy = 1; break; }
    }
    fclose(f);
    if (busy) return 1;
    f = fopen(swaps, "re");
    while (f && !busy && fgets(line, sizeof line, f)) {
        char path[PATH_MAX];
        struct stat st;
        if (line[0] != '/' || sscanf(line, "%4095s", path) != 1 || stat(path, &st) || !S_ISBLK(st.st_mode)) continue;
        for (int i = 0; i < nn; i++) if (n[i].block && major(st.st_rdev) == n[i].major && minor(st.st_rdev) == n[i].minor) { busy = 1; break; }
    }
    if (f) fclose(f);
    return busy;
}

const char *vmusb_refusal(const char *sys, const char *proc, const char *mountinfo, const char *swaps, const VmUsbDev *d, int admin) {
    if (d->flags & (VMUSB_F_ROOTHUB | VMUSB_F_HUB)) return "vm.usbHub";
    if (d->major != VMUSB_MAJOR) return "vm.usbGone";
    VmUsbNode n[64];
    int nn = vmusb_nodes(sys, d, n, 64);
    if (vmusb_input_held(proc, n, nn, 0)) return "vm.usbInputInUse";
    if ((d->flags & (VMUSB_F_STORAGE | VMUSB_F_BLOCK)) && !admin) return "vm.usbStorageAdmin";
    if (vmusb_block_busy(sys, d, mountinfo, swaps)) return "vm.usbMounted";
    return NULL;
}

int vmusb_fd_ok(int fd, int bus, int dev) {
    struct stat st;
    if (fd < 0 || bus < 1 || bus > VMUSB_BUS_MAX || dev < 1 || dev > VMUSB_DEV_MAX) return -1;
    if (fstat(fd, &st) || !S_ISCHR(st.st_mode) || major(st.st_rdev) != VMUSB_MAJOR) return -1;
    if (minor(st.st_rdev) != (unsigned)((bus - 1) * 128 + dev - 1)) return -1;
    int fl = fcntl(fd, F_GETFL);
    return fl >= 0 && (fl & O_ACCMODE) == O_RDWR ? 0 : -1;
}

int vmusb_line_parse(const char *s, int *bus, int *dev, unsigned *vendor, unsigned *product) {
    if (!s) return -1;
    size_t n = strlen(s);
    const char *a = memchr(s, ' ', n);
    if (!a) return -1;
    const char *b = memchr(a + 1, ' ', n - (size_t)(a + 1 - s));
    if (!b) return -1;
    if (dec(s, (size_t)(a - s), 1, VMUSB_BUS_MAX, bus) || dec(a + 1, (size_t)(b - a - 1), 1, VMUSB_DEV_MAX, dev)) return -1;
    return ids(b + 1, strlen(b + 1), vendor, product);
}
