/* vmunpack.c — lord-vmunpack: unpacks a machine group's verified archive (Whonix's KVM .libvirt.xz) into its members'
 * two disk files (docs/VIRTUALIZATION.md 19.6, "Unpacking"; AGENTS.md "lord-vmunpack"). The tar reader is vmunpack.h's.
 *
 * lord-vm (--group-create) starts it from the fixed path next to it, with argv { "lord-vmunpack" } and an empty
 * environment, and gives it:
 *   fd 0  the archive, opened read-only from the image store (already checked against the signed SHA-512 list)
 *   fd 3  an empty regular file for the gateway's disk, opened for writing in the group's staging folder
 *   fd 4  the same for the workstation's disk
 *   fd 5  the request: one line (vmunpack.h, vmu_req_parse), then its end
 *   fd 1  where it answers; fd 2 its log (one line from the sandbox)
 * It applies the helpers' sandbox (no files, no network, no exec, the syscall allowlist, a memory cap) before it reads
 * a byte, and opens nothing. On fd 1: "progress <archive bytes read> <archive size>\n" lines, at most four a second,
 * then exactly one of "ok <gateway real size> <workstation real size> <data bytes>\n" (exit 0) or
 * "error <code>\n" (exit 1; vmu_code_name's words). Links libc and LordOS's own pkg/xz.c (no liblzma, no libarchive). */
#include "vmunpack.h"
#include "../pkg/xz.h"

#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const CODES[VMU_COUNT] = {
    "ok", "badXz", "truncated", "trailing", "badTar", "badChecksum", "badType", "badName",
    "duplicate", "missing", "badSparse", "tooLarge", "tooMuchData", "io", "request",
};
const char *vmu_code_name(int code) { return code >= 0 && code < VMU_COUNT ? CODES[code] : "io"; }

int vmu_xz_code(int e) {
    return e == XZS_E_TRUNCATED ? VMU_TRUNCATED : e == XZS_E_TRAILING ? VMU_TRAILING : e == XZS_E_IO ? VMU_IO : VMU_BADXZ;
}

/* ---- the request ---- */

static int dec(const char *s, size_t n, uint64_t *v) {
    if (!n || n > 19 || (n > 1 && s[0] == '0')) return -1;
    uint64_t x = 0;
    for (size_t i = 0; i < n; i++) { if (s[i] < '0' || s[i] > '9') return -1; x = x * 10 + (uint64_t)(s[i] - '0'); }
    if (!x || x > (1ull << 62)) return -1;
    *v = x;
    return 0;
}

int vmu_req_parse(const char *line, size_t n, VmuReq *r) {
    if (!line || !r || n < 2 || n > VMU_REQ_MAX || line[n - 1] != '\n' || memchr(line, 0, n)) return -1;
    memset(r, 0, sizeof *r);
    const char *f[4];
    size_t fl[4];
    int k = 0;
    size_t i = 0, end = n - 1;
    while (i < end) {
        size_t j = i;
        while (j < end && line[j] != ' ') j++;
        if (j == i || k == 4) return -1;                     /* an empty field (two spaces) or a fifth one */
        f[k] = line + i;
        fl[k++] = j - i;
        if (j < end && j + 1 == end) return -1;              /* a space before the newline */
        i = j + 1;
    }
    if (k != 4 || dec(f[0], fl[0], &r->max_apparent) || dec(f[1], fl[1], &r->max_data)) return -1;
    for (int p = 0; p < 2; p++) {
        const char *s = f[2 + p];
        size_t l = fl[2 + p];
        if (l < 3 || l >= VMU_RE_MAX || s[0] != '^' || s[l - 1] != '$') return -1;
        for (size_t q = 0; q < l; q++) if ((unsigned char)s[q] <= 0x20 || (unsigned char)s[q] >= 0x7f) return -1;
        memcpy(r->re[p], s, l);
        r->re[p][l] = 0;
        regex_t re;
        if (regcomp(&re, r->re[p], REG_EXTENDED | REG_NOSUB)) return -1;
        regfree(&re);
    }
    return 0;
}

/* ---- reading ---- */

typedef struct {
    const VmuSrc *src;
    int eof;
} Rd;

/* exactly n bytes: VMU_OK, VMU_TRUNCATED at the source's end, or the source's error */
static int rd_full(Rd *d, uint8_t *b, size_t n) {
    size_t o = 0;
    while (o < n) {
        int r = d->eof ? 0 : d->src->read(d->src->ctx, b + o, n - o);
        if (r < 0) return d->src->err ? d->src->err(d->src->ctx) : VMU_IO;
        if (r == 0) { d->eof = 1; return VMU_TRUNCATED; }
        o += (size_t)r;
    }
    return VMU_OK;
}

/* GNU's numbers: base-256 when the first byte has its top bit (positive only), else octal with leading spaces and a
 * space or NUL after the digits (and nothing else to the field's end) */
static int num(const uint8_t *f, size_t n, uint64_t *v) {
    uint64_t x = 0;
    if (f[0] & 0x80) {
        if (f[0] != 0x80) return -1;                         /* 0xff: negative; other top bits: not GNU's */
        for (size_t i = 1; i < n; i++) { if (x >> 55) return -1; x = x << 8 | f[i]; }
        if (x >> 62) return -1;
        *v = x;
        return 0;
    }
    size_t i = 0;
    while (i < n && f[i] == ' ') i++;
    size_t d0 = i;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; i++) { if (x >> 59) return -1; x = x << 3 | (uint64_t)(f[i] - '0'); }
    if (i == d0) return -1;
    for (; i < n; i++) if (f[i] != ' ' && f[i] != 0) return -1;
    *v = x;
    return 0;
}

static int zero_block(const uint8_t *b) {
    for (int i = 0; i < 512; i++) if (b[i]) return 0;
    return 1;
}

static int checksum_ok(const uint8_t *h) {
    uint64_t want;
    if (num(h + 148, 8, &want)) return 0;
    uint64_t u = 0;
    int64_t s = 0;
    for (int i = 0; i < 512; i++) {
        uint8_t c = i >= 148 && i < 156 ? ' ' : h[i];
        u += c;
        s += (signed char)c;
    }
    return want == u || (s >= 0 && want == (uint64_t)s);
}

/* the member's name: a plain name within the 100-byte field */
static int plain_name(const uint8_t *h, char out[100]) {
    const uint8_t *z = memchr(h, 0, 100);
    if (!z || z == h) return -1;
    size_t n = (size_t)(z - h);
    if (h[0] == '.') return -1;
    for (size_t i = 0; i < n; i++) if (h[i] <= 0x20 || h[i] >= 0x7f || h[i] == '/' || h[i] == '\\') return -1;
    memcpy(out, h, n);
    out[n] = 0;
    return 0;
}

typedef struct { uint64_t off, len; } Ext;

/* one map entry of 24 bytes: 1 added, 0 an empty entry, -1 a bad one */
static int map_entry(const uint8_t *e, Ext **map, size_t *n, size_t *cap) {
    int empty = 1;
    for (int i = 0; i < 24; i++) if (e[i]) { empty = 0; break; }
    if (empty) return 0;
    uint64_t off, len;
    if (num(e, 12, &off) || num(e + 12, 12, &len)) return -1;
    if (*n == VMU_SPARSE_MAX) return -1;
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 64;
        Ext *m = realloc(*map, nc * sizeof *m);
        if (!m) return -1;
        *map = m;
        *cap = nc;
    }
    (*map)[(*n)++] = (Ext){ off, len };
    return 1;
}

/* writes len bytes at off from the source, leaving 4 KiB-aligned runs of zeros unwritten */
static int copy_out(Rd *d, int fd, uint64_t off, uint64_t len, uint8_t *buf, size_t bn) {
    while (len) {
        size_t n = len < bn ? (size_t)len : bn;
        int e = rd_full(d, buf, n);
        if (e) return e;
        size_t i = 0;
        while (i < n) {
            /* a run of blocks that aren't all zero, each block ending on a 4 KiB boundary of the output */
            size_t s = i;
            int any = 0;
            while (i < n) {
                size_t bl = 4096 - (size_t)((off + i) & 4095);
                if (bl > n - i) bl = n - i;
                int z = buf[i] == 0 && !memcmp(buf + i, buf + i + 1, bl - 1);
                if (z && any) break;
                if (z) { i += bl; s = i; continue; }
                any = 1;
                i += bl;
            }
            for (size_t w = s; w < i;) {
                ssize_t r = pwrite(fd, buf + w, i - w, (off_t)(off + w));
                if (r < 0 && errno == EINTR) continue;
                if (r <= 0) return VMU_IO;
                w += (size_t)r;
            }
        }
        off += n;
        len -= n;
    }
    return VMU_OK;
}

static int skip(Rd *d, uint64_t len, uint8_t *buf, size_t bn) {
    while (len) {
        size_t n = len < bn ? (size_t)len : bn;
        int e = rd_full(d, buf, n);
        if (e) return e;
        len -= n;
    }
    return VMU_OK;
}

#define BUF_BYTES (1u << 20)

int vmu_unpack(const VmuReq *r, const VmuSrc *src, const int out[2], VmuResult *res) {
    memset(res, 0, sizeof *res);
    regex_t re[2];
    if (regcomp(&re[0], r->re[0], REG_EXTENDED | REG_NOSUB)) return VMU_REQUEST;
    if (regcomp(&re[1], r->re[1], REG_EXTENDED | REG_NOSUB)) { regfree(&re[0]); return VMU_REQUEST; }
    uint8_t *buf = malloc(BUF_BYTES);
    Ext *map = NULL;
    size_t nmap = 0, capmap = 0;
    Rd d = { src, 0 };
    int done[2] = { 0, 0 }, members = 0, e = buf ? VMU_OK : VMU_IO;
    uint64_t total = 0;
    uint8_t h[512], x[512];
    while (!e) {
        if ((e = rd_full(&d, h, 512))) break;
        if (zero_block(h)) {
            /* the end: a second zero block, then only zeros (a tar's record padding), then the source's end */
            if ((e = rd_full(&d, h, 512))) break;
            if (!zero_block(h)) { e = VMU_BADTAR; break; }
            uint64_t tail = 0;
            for (;;) {
                int k = src->read(src->ctx, buf, BUF_BYTES);
                if (k < 0) { e = src->err ? src->err(src->ctx) : VMU_IO; break; }
                if (k == 0) break;
                for (int i = 0; i < k; i++) if (buf[i]) { e = VMU_BADTAR; break; }
                if (e) break;
                tail += (uint64_t)k;
                if (tail > VMU_TAIL_MAX) { e = VMU_TRAILING; break; }
            }
            break;
        }
        if (!checksum_ok(h)) { e = VMU_BADCHECKSUM; break; }
        if (++members > VMU_MEMBERS_MAX) { e = VMU_BADTAR; break; }
        int gnu = !memcmp(h + 257, "ustar  ", 8), posix = !memcmp(h + 257, "ustar\0" "00", 8);
        if (!gnu && !posix) { e = VMU_BADTAR; break; }
        char type = (char)h[156];
        if (type != '0' && type != 0 && type != 'S') { e = VMU_BADTYPE; break; }
        if (type == 'S' && !gnu) { e = VMU_BADTYPE; break; }
        if (posix) for (int i = 345; i < 500; i++) if (h[i]) { e = VMU_BADNAME; break; }   /* a prefix: a path */
        if (e) break;
        char name[100];
        if (plain_name(h, name)) { e = VMU_BADNAME; break; }
        uint64_t size, real;
        if (num(h + 124, 12, &size)) { e = VMU_BADTAR; break; }
        nmap = 0;
        if (type == 'S') {
            /* old GNU sparse: four entries in the header, more in extension headers while "isextended" says so */
            int ended = 0;
            for (int i = 0; i < 4 && !e; i++) {
                int k = map_entry(h + 386 + 24 * i, &map, &nmap, &capmap);
                if (k < 0 || (k > 0 && ended)) e = VMU_BADSPARSE;
                if (k == 0) ended = 1;
            }
            if (e) break;
            if (num(h + 483, 12, &real)) { e = VMU_BADSPARSE; break; }
            uint8_t ext = h[482];
            while (!e && ext) {
                if (ext != 1 || ended) { e = VMU_BADSPARSE; break; }
                if ((e = rd_full(&d, x, 512))) break;
                for (int i = 0; i < 21 && !e; i++) {
                    int k = map_entry(x + 24 * i, &map, &nmap, &capmap);
                    if (k < 0 || (k > 0 && ended)) e = VMU_BADSPARSE;
                    if (k == 0) ended = 1;
                }
                for (int i = 505; i < 512 && !e; i++) if (x[i]) e = VMU_BADSPARSE;
                ext = x[504];
            }
            if (e) break;
            if (real > r->max_apparent) { e = VMU_TOOLARGE; break; }
            uint64_t end = 0, sum = 0;
            for (size_t i = 0; i < nmap; i++) {
                if (map[i].off < end || map[i].len > real || map[i].off > real - map[i].len) { e = VMU_BADSPARSE; break; }
                end = map[i].off + map[i].len;
                sum += map[i].len;
            }
            if (e) break;
            if (sum != size) { e = VMU_BADSPARSE; break; }
        } else {
            real = size;
            if (real > r->max_apparent) { e = VMU_TOOLARGE; break; }
            if (size) {
                if (!capmap) {
                    map = malloc(64 * sizeof *map);
                    if (!map) { e = VMU_IO; break; }
                    capmap = 64;
                }
                map[0] = (Ext){ 0, size };
                nmap = 1;
            }
        }
        if (size > r->max_data || total > r->max_data - size) { e = VMU_TOOMUCHDATA; break; }
        total += size;
        int m0 = !regexec(&re[0], name, 0, NULL, 0), m1 = !regexec(&re[1], name, 0, NULL, 0);
        if (m0 && m1) { e = VMU_BADNAME; break; }
        int role = m0 ? 0 : m1 ? 1 : -1;
        if (role < 0) e = skip(&d, size, buf, BUF_BYTES);
        else {
            if (done[role]) { e = VMU_DUPLICATE; break; }
            done[role] = 1;
            for (size_t i = 0; i < nmap && !e; i++) e = copy_out(&d, out[role], map[i].off, map[i].len, buf, BUF_BYTES);
            if (e) break;
            if (ftruncate(out[role], (off_t)real)) { e = VMU_IO; break; }
            res->real[role] = real;
            res->data += size;
            memcpy(res->name[role], name, sizeof res->name[role]);
        }
        if (e) break;
        if (size & 511) e = skip(&d, 512 - (size & 511), buf, BUF_BYTES);   /* the last block's padding */
    }
    if (!e && (!done[0] || !done[1])) e = VMU_MISSING;
    free(map);
    free(buf);
    regfree(&re[0]);
    regfree(&re[1]);
    return e;
}

/* ---- the helper ---- */
#ifndef VMU_NO_MAIN
#include "../sec/sandbox.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_DICT (256u << 20)

static int say(const char *line) {
    size_t n = strlen(line);
    for (size_t o = 0; o < n;) {
        ssize_t w = write(1, line + o, n - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        o += (size_t)w;
    }
    return 0;
}

typedef struct { uint64_t total; long long next; XzStream *xz; } Prog;
static Prog prog;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_archive(void *ctx, uint8_t *buf, size_t n) {
    (void)ctx;
    for (;;) {
        ssize_t r = read(0, buf, n > (1u << 30) ? (1u << 30) : n);
        if (r < 0 && errno == EINTR) continue;
        return r < 0 ? -1 : (int)r;
    }
}

static int read_xz(void *ctx, uint8_t *buf, size_t n) {
    ssize_t r = xzs_read((XzStream *)ctx, buf, n > (1u << 30) ? (1u << 30) : n);
    long long t = now_ms();
    if (r > 0 && t >= prog.next) {
        char l[80];
        snprintf(l, sizeof l, "progress %llu %llu\n", (unsigned long long)xzs_in_pos((XzStream *)ctx), (unsigned long long)prog.total);
        say(l);
        prog.next = t + 250;
    }
    return (int)r;
}
static int err_xz(void *ctx) { return vmu_xz_code(xzs_error((XzStream *)ctx)); }

static int answer_err(int code) {
    char l[40];
    snprintf(l, sizeof l, "error %s\n", vmu_code_name(code));
    say(l);
    return 1;
}

int main(int argc, char **argv) {
    (void)argv;
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    prctl(PR_SET_DUMPABLE, 0);
    struct rlimit nocore = { 0, 0 };
    setrlimit(RLIMIT_CORE, &nocore);
    if (argc != 1) return 2;
    SandboxHelperCfg cfg = {
        .name = "lordos-vmunpack",
        .ro_paths = NULL,
        .extra_syscalls = (int[]){ -1 },
        .allow_ioctl_classes = 0,
        .mem_max_mb = 320,          /* the window (a dictionary of at most 256 MiB, plus 2 MiB), 1 MiB of copy, the map */
        .ro_dir_fds = NULL,
        .rw_dir_fds = NULL,
    };
    sandbox_apply_helper(&cfg);

    char line[VMU_REQ_MAX + 1];
    size_t ln = 0;
    while (ln < VMU_REQ_MAX) {
        ssize_t r = read(5, line + ln, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        if (line[ln++] == '\n') break;
    }
    VmuReq req;
    if (vmu_req_parse(line, ln, &req)) return answer_err(VMU_REQUEST);
    struct stat a, g, w;
    if (fstat(0, &a) || !S_ISREG(a.st_mode) || fstat(3, &g) || !S_ISREG(g.st_mode) || g.st_size ||
        fstat(4, &w) || !S_ISREG(w.st_mode) || w.st_size || (g.st_dev == w.st_dev && g.st_ino == w.st_ino)) return answer_err(VMU_REQUEST);
    /* the outputs written at offsets: opened for writing, not for appending (pwrite would append) */
    for (int fd = 3; fd <= 4; fd++) {
        int fl = fcntl(fd, F_GETFL);
        if (fl < 0 || (fl & O_APPEND) || (fl & O_ACCMODE) == O_RDONLY) return answer_err(VMU_REQUEST);
    }
    prog.total = (uint64_t)a.st_size;
    XzStream *xz = xzs_new(read_archive, NULL, MAX_DICT);
    if (!xz) return answer_err(VMU_IO);
    VmuSrc src = { read_xz, err_xz, xz };
    VmuResult res;
    int out[2] = { 3, 4 };
    int e = vmu_unpack(&req, &src, out, &res);
    xzs_free(xz);
    if (e) return answer_err(e);
    char l[120];
    snprintf(l, sizeof l, "ok %llu %llu %llu\n", (unsigned long long)res.real[0], (unsigned long long)res.real[1], (unsigned long long)res.data);
    return say(l) ? 1 : 0;
}
#endif
