/* vmunpack_test.c — lord-vmunpack (src/vm/vmunpack.c) and the streaming xz decoder under it (src/pkg/xz.c, xzs_*),
 * with archives the test makes itself (no binary fixtures): plain members; GNU tar's own old-GNU sparse members (with
 * extension headers); hand-made hostile headers (a sparse map out of order, overlapping, beyond the real size, not
 * adding up; real size or data over the caps; long names, links, folders, pax headers; names with a path; a member
 * twice; a role missing; a bad checksum; no end; data after the end); xz made by xz(1): several blocks and streams,
 * stream padding, every check type, a cut stream, data after it, a changed byte; then a fuzz pass over a good archive.
 * Outputs must keep their holes and hold exactly the members' bytes. Last, given its path, lord-vmunpack itself on
 * file descriptors, in its sandbox. Under ASan and UBSan (mk/vmunpack.mk). */
#include "../src/vm/vmunpack.h"
#include "../src/pkg/xz.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* the allocation unit a hole is measured in: the page size (tmpfs; 16 KB on a Raspberry Pi 5) or 4 KB, the larger */
static long long page_unit(void) { long p = sysconf(_SC_PAGESIZE); return p > 4096 ? p : 4096; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char tmp[64];

/* ---- a growing buffer ---- */
typedef struct { uint8_t *p; size_t n, cap; } Buf;
static void put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n) c *= 2;
        b->p = realloc(b->p, c);
        if (!b->p) exit(1);
        b->cap = c;
    }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void zeros(Buf *b, size_t n) { static const uint8_t z[512]; while (n) { size_t k = n < 512 ? n : 512; put(b, z, k); n -= k; } }

/* ---- tar headers ---- */
enum { GNU = 0, POSIX = 1 };
static void octal(uint8_t *f, size_t n, unsigned long long v) { snprintf((char *)f, n, "%0*llo", (int)n - 1, v); }
static void base256(uint8_t *f, size_t n, unsigned long long v) {
    memset(f, 0, n);
    for (size_t i = n - 1; i > 0; i--) { f[i] = (uint8_t)v; v >>= 8; }
    f[0] = 0x80;
}
static void num12(uint8_t *f, unsigned long long v) { if (v < 077777777777ull) octal(f, 12, v); else base256(f, 12, v); }
static void seal(uint8_t *h) {
    memset(h + 148, ' ', 8);
    unsigned s = 0;
    for (int i = 0; i < 512; i++) s += h[i];
    snprintf((char *)h + 148, 8, "%06o", s);
    h[155] = ' ';
}
static void header(uint8_t *h, const char *name, char type, unsigned long long size, int fmt) {
    memset(h, 0, 512);
    memcpy(h, name, strlen(name) < 100 ? strlen(name) : 100);
    octal(h + 100, 8, 0644); octal(h + 108, 8, 1000); octal(h + 116, 8, 1000);
    num12(h + 124, size);
    octal(h + 136, 12, 1700000000);
    h[156] = (uint8_t)type;
    if (fmt == GNU) memcpy(h + 257, "ustar  ", 8); else memcpy(h + 257, "ustar\0" "00", 8);
    seal(h);
}
/* a regular member with its data (len bytes of pattern seed) and padding */
static void member(Buf *t, const char *name, char type, const uint8_t *data, size_t len, int fmt) {
    uint8_t h[512];
    header(h, name, type, len, fmt);
    put(t, h, 512);
    put(t, data, len);
    zeros(t, (512 - (len & 511)) & 511);
}
static void end(Buf *t) { zeros(t, 1024); }

/* an old-GNU sparse member: map of n extents (offsets and lengths as given, even if wrong), real size, data from src
 * (the sparse file's contents at each extent) or a pattern; stored size = sum or forced */
typedef struct { unsigned long long off, len; } Ex;
static void sparse(Buf *t, const char *name, const Ex *m, int n, unsigned long long real, const uint8_t *src, long long force_size) {
    unsigned long long sum = 0;
    for (int i = 0; i < n; i++) sum += m[i].len;
    uint8_t h[512];
    header(h, name, 'S', force_size >= 0 ? (unsigned long long)force_size : sum, GNU);
    int k = 0;
    for (; k < n && k < 4; k++) { num12(h + 386 + 24 * k, m[k].off); num12(h + 386 + 24 * k + 12, m[k].len); }
    h[482] = n > 4;
    num12(h + 483, real);
    seal(h);
    put(t, h, 512);
    while (k < n) {
        uint8_t x[512] = { 0 };
        int j = 0;
        for (; j < 21 && k < n; j++, k++) { num12(x + 24 * j, m[k].off); num12(x + 24 * j + 12, m[k].len); }
        x[504] = k < n;
        put(t, x, 512);
    }
    size_t at = t->n;
    for (int i = 0; i < n; i++) {
        if (src) put(t, src + m[i].off, (size_t)m[i].len);
        else for (unsigned long long b = 0; b < m[i].len; b++) { uint8_t c = (uint8_t)(m[i].off + b + 1); put(t, &c, 1); }
    }
    zeros(t, (512 - ((t->n - at) & 511)) & 511);
}

/* ---- running the unpacker in-process ---- */
typedef struct { const uint8_t *p; size_t n, at; } Mem;
static int mem_read(void *c, uint8_t *b, size_t n) {
    Mem *m = c;
    size_t k = m->n - m->at;
    if (k > n) k = n;
    if (k > 7777) k = 7777;                                  /* odd pieces, to cross every boundary */
    memcpy(b, m->p + m->at, k);
    m->at += k;
    return (int)k;
}
static int xz_rd(void *c, uint8_t *b, size_t n) { return (int)xzs_read(c, b, n > 100000 ? 100000 : n); }
static int xz_err(void *c) { return vmu_xz_code(xzs_error(c)); }

static VmuReq REQ;
static const char *GW = "Whonix-Gateway-LXQt-18.2.4.3.Intel_AMD64.qcow2", *WS = "Whonix-Workstation-LXQt-18.2.4.3.Intel_AMD64.qcow2";

static int outfd(int i) {
    char p[96];
    snprintf(p, sizeof p, "%s/out%d", tmp, i);
    unlink(p);
    return open(p, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
}

/* raw: the tar itself; else xz bytes */
static int run(const uint8_t *p, size_t n, int xz, VmuResult *res) {
    int o[2] = { outfd(0), outfd(1) };
    Mem m = { p, n, 0 };
    XzStream *s = NULL;
    VmuSrc src = { mem_read, NULL, &m };
    if (xz) { s = xzs_new(mem_read, &m, 64u << 20); src = (VmuSrc){ xz_rd, xz_err, s }; }
    VmuResult r;
    int e = vmu_unpack(&REQ, &src, o, res ? res : &r);
    xzs_free(s);
    close(o[0]); close(o[1]);
    return e;
}

static uint8_t *slurp(const char *path, size_t *n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    fstat(fd, &st);
    uint8_t *b = malloc((size_t)st.st_size + 1);
    size_t o = 0;
    while (b && o < (size_t)st.st_size) { ssize_t r = read(fd, b + o, (size_t)st.st_size - o); if (r <= 0) break; o += (size_t)r; }
    close(fd);
    *n = o;
    return b;
}
static void spit(const char *path, const uint8_t *p, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0 || write(fd, p, n) != (ssize_t)n) { perror(path); exit(1); }
    close(fd);
}

/* xz(1) on the bytes, with its options: the compressed bytes (malloc), or NULL when there's no xz */
static uint8_t *xz_of(const uint8_t *p, size_t n, const char *opts, size_t *on) {
    char in[96], out[96], cmd[400];
    snprintf(in, sizeof in, "%s/x.in", tmp);
    snprintf(out, sizeof out, "%s/x.in.xz", tmp);
    spit(in, p, n);
    unlink(out);
    snprintf(cmd, sizeof cmd, "xz -q -k -f %s %s 2>/dev/null", opts, in);
    if (system(cmd)) return NULL;
    return slurp(out, on);
}

static int same_file(int role, const uint8_t *want, size_t n, size_t real) {
    char p[96];
    snprintf(p, sizeof p, "%s/out%d", tmp, role);
    size_t got_n;
    uint8_t *got = slurp(p, &got_n);
    int ok = got && got_n == real && (!n || !memcmp(got, want, n));
    for (size_t i = n; ok && i < real; i++) if (got[i]) ok = 0;
    free(got);
    return ok;
}

/* the bytes of out<role> that are allocated (SEEK_DATA runs) */
static long long allocated(int role) {
    char p[96];
    snprintf(p, sizeof p, "%s/out%d", tmp, role);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    long long tot = 0;
    off_t at = 0;
    for (;;) {
        off_t d = lseek(fd, at, SEEK_DATA);
        if (d < 0) break;
        off_t h = lseek(fd, d, SEEK_HOLE);
        if (h <= d) break;
        tot += h - d;
        at = h;
    }
    close(fd);
    return tot;
}

static uint8_t pat[300000];

/* a small good archive: license, gateway (regular), an xml, workstation (sparse, hand-made), end */
static void good(Buf *t) {
    member(t, "WHONIX_BINARY_LICENSE_AGREEMENT", '0', pat, 1000, GNU);
    member(t, GW, '0', pat + 7, 70000, GNU);
    member(t, "Whonix-Gateway.xml", '0', pat, 513, GNU);
    Ex m[6] = { { 0, 4096 }, { 65536, 100 }, { 70000, 5000 }, { 200000, 8192 }, { 300000, 1 }, { 1 << 20, 0 } };
    sparse(t, WS, m, 6, 1 << 20, NULL, -1);
    end(t);
    zeros(t, 10240 - t->n % 10240);
}

static void test_req(void) {
    VmuReq r;
    const char *ok = "137438953472 25769803776 ^Whonix-Gateway-LXQt-[0-9]+(\\.[0-9]+){3}\\.Intel_AMD64\\.qcow2$ ^Whonix-Workstation-LXQt-[0-9]+(\\.[0-9]+){3}\\.Intel_AMD64\\.qcow2$\n";
    CHECK(!vmu_req_parse(ok, strlen(ok), &r) && r.max_apparent == 137438953472ull && r.max_data == 25769803776ull, "the Whonix request");
    const char *bad[] = { "1 2 ^a$ ^b$", "1 2 ^a$ ^b$ \n", "1  2 ^a$ ^b$\n", "0 2 ^a$ ^b$\n", "1 2 a$ ^b$\n", "1 2 ^a ^b$\n",
                          "1 2 ^a$\n", "1 2 ^a$ ^b$ ^c$\n", "01 2 ^a$ ^b$\n", "1 x ^a$ ^b$\n", "1 2 ^(a$ ^b$\n",
                          "99999999999999999999 2 ^a$ ^b$\n", "4611686018427387905 2 ^a$ ^b$\n", "1 2 ^a\x01$ ^b$\n", "" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(vmu_req_parse(bad[i], strlen(bad[i]), &r), "request %zu refused", i);
    CHECK(vmu_req_parse("1 2 ^a$ ^b$\n\0", 13, &r), "a NUL refused");
}

static void test_tar(void) {
    VmuResult res;
    Buf t = { 0 };
    good(&t);
    int e = run(t.p, t.n, 0, &res);
    CHECK(e == VMU_OK, "good raw archive: %s", vmu_code_name(e));
    CHECK(res.real[0] == 70000 && res.real[1] == (1 << 20) && res.data == 70000 + 4096 + 100 + 5000 + 8192 + 1, "sizes %llu %llu %llu",
          (unsigned long long)res.real[0], (unsigned long long)res.real[1], (unsigned long long)res.data);
    CHECK(same_file(0, pat + 7, 70000, 70000), "gateway's bytes");
    {
        uint8_t *w = calloc(1, 1 << 20);
        Ex m[5] = { { 0, 4096 }, { 65536, 100 }, { 70000, 5000 }, { 200000, 8192 }, { 300000, 1 } };
        for (int i = 0; i < 5; i++) for (unsigned long long b = 0; b < m[i].len; b++) w[m[i].off + b] = (uint8_t)(m[i].off + b + 1);
        CHECK(same_file(1, w, 1 << 20, 1 << 20), "workstation's bytes");
        free(w);
        long long a = allocated(1);
        /* each extent may take whole pages (blocks) at both ends: 16 KB on a Raspberry Pi 5's kernel, 4 KB elsewhere */
        long long u = page_unit();
        CHECK(a > 0 && a <= 17389 + 10 * u, "the workstation keeps its holes (%lld allocated, unit %lld)", a, u);
    }
    /* a POSIX ustar regular member is fine too; base-256 sizes read */
    {
        Buf u = { 0 };
        member(&u, GW, 0, pat, 10, POSIX);
        Ex m[1] = { { 0, 512 } };
        sparse(&u, WS, m, 1, 100000000000ull, NULL, -1);     /* real size in base-256 */
        end(&u);
        e = run(u.p, u.n, 0, &res);
        CHECK(e == VMU_OK && res.real[1] == 100000000000ull, "POSIX member, base-256 real size: %s", vmu_code_name(e));
        free(u.p);
    }
    struct { const char *what; int want; } cases[] = {
        { "out of order", VMU_BADSPARSE }, { "overlap", VMU_BADSPARSE }, { "beyond", VMU_BADSPARSE }, { "sum", VMU_BADSPARSE },
        { "apparent cap", VMU_TOOLARGE }, { "data cap", VMU_TOOMUCHDATA }, { "regular over apparent", VMU_TOOLARGE },
        { "L", VMU_BADTYPE }, { "K", VMU_BADTYPE }, { "1", VMU_BADTYPE }, { "2", VMU_BADTYPE }, { "5", VMU_BADTYPE }, { "x", VMU_BADTYPE },
        { "g", VMU_BADTYPE }, { "D", VMU_BADTYPE }, { "3", VMU_BADTYPE }, { "S posix", VMU_BADTYPE },
        { "../x", VMU_BADNAME }, { "/abs", VMU_BADNAME }, { "a/b", VMU_BADNAME }, { ".hid", VMU_BADNAME }, { "prefix", VMU_BADNAME },
        { "space name", VMU_BADNAME }, { "both", VMU_BADNAME },
        { "twice", VMU_DUPLICATE }, { "missing", VMU_MISSING }, { "checksum", VMU_BADCHECKSUM }, { "no end", VMU_TRUNCATED },
        { "cut data", VMU_TRUNCATED }, { "after end", VMU_BADTAR }, { "one zero block", VMU_BADTAR }, { "zeros tail", VMU_TRAILING },
        { "no magic", VMU_BADTAR }, { "bad size", VMU_BADTAR }, { "negative", VMU_BADTAR }, { "ext flag 2", VMU_BADSPARSE },
        { "entry after empty", VMU_BADSPARSE }, { "too many members", VMU_BADTAR },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const char *w = cases[i].what;
        Buf b = { 0 };
        uint8_t h[512];
        int closed = 0;
        if (!strcmp(w, "out of order")) { Ex m[2] = { { 8192, 10 }, { 0, 10 } }; member(&b, GW, '0', pat, 5, GNU); sparse(&b, WS, m, 2, 9000, NULL, -1); }
        else if (!strcmp(w, "overlap")) { Ex m[2] = { { 0, 100 }, { 50, 10 } }; member(&b, GW, '0', pat, 5, GNU); sparse(&b, WS, m, 2, 9000, NULL, -1); }
        else if (!strcmp(w, "beyond")) { Ex m[1] = { { 8995, 10 } }; member(&b, GW, '0', pat, 5, GNU); sparse(&b, WS, m, 1, 9000, NULL, -1); }
        else if (!strcmp(w, "sum")) { Ex m[1] = { { 0, 10 } }; member(&b, GW, '0', pat, 5, GNU); sparse(&b, WS, m, 1, 9000, NULL, 512); }
        else if (!strcmp(w, "apparent cap")) { Ex m[1] = { { 0, 10 } }; member(&b, GW, '0', pat, 5, GNU); sparse(&b, WS, m, 1, (1ull << 40) + 1, NULL, -1); }
        else if (!strcmp(w, "data cap")) { member(&b, "LICENSE", '0', pat, 200000, GNU); member(&b, GW, '0', pat, 200000, GNU); }
        else if (!strcmp(w, "regular over apparent")) { REQ.max_apparent = 1000; member(&b, GW, '0', pat, 1001, GNU); }
        else if (strlen(w) == 1 || !strcmp(w, "S posix")) {
            char ty = !strcmp(w, "S posix") ? 'S' : w[0];
            header(h, ty == '5' ? "dir" : "x", ty, 0, !strcmp(w, "S posix") ? POSIX : GNU);
            put(&b, h, 512);
        } else if (w[0] == '.' || w[0] == '/' || !strcmp(w, "a/b") || !strcmp(w, "space name")) member(&b, w, '0', pat, 5, GNU);
        else if (!strcmp(w, "prefix")) { header(h, GW, '0', 0, POSIX); memcpy(h + 345, "dir", 3); seal(h); put(&b, h, 512); }
        else if (!strcmp(w, "both")) { strcpy(REQ.re[1], "^Whonix-.*$"); member(&b, GW, '0', pat, 5, GNU); }
        else if (!strcmp(w, "twice")) { member(&b, GW, '0', pat, 5, GNU); member(&b, GW, '0', pat, 5, GNU); }
        else if (!strcmp(w, "missing")) { member(&b, GW, '0', pat, 5, GNU); member(&b, "Whonix-Workstation.xml", '0', pat, 5, GNU); }
        else if (!strcmp(w, "checksum")) { header(h, GW, '0', 0, GNU); h[0] ^= 1; put(&b, h, 512); }
        else if (!strcmp(w, "no end")) { member(&b, GW, '0', pat, 5, GNU); member(&b, WS, '0', pat, 5, GNU); closed = 1; }
        else if (!strcmp(w, "cut data")) { header(h, GW, '0', 5000, GNU); put(&b, h, 512); put(&b, pat, 1000); closed = 1; }
        else if (!strcmp(w, "after end")) { member(&b, GW, '0', pat, 5, GNU); member(&b, WS, '0', pat, 5, GNU); end(&b); put(&b, "x", 1); closed = 1; }
        else if (!strcmp(w, "one zero block")) { member(&b, GW, '0', pat, 5, GNU); member(&b, WS, '0', pat, 5, GNU); zeros(&b, 512); member(&b, "x", '0', pat, 1, GNU); closed = 1; }
        else if (!strcmp(w, "zeros tail")) { member(&b, GW, '0', pat, 5, GNU); member(&b, WS, '0', pat, 5, GNU); end(&b); zeros(&b, (2u << 20)); closed = 1; }
        else if (!strcmp(w, "no magic")) { header(h, GW, '0', 0, GNU); memset(h + 257, 0, 8); seal(h); put(&b, h, 512); }
        else if (!strcmp(w, "bad size")) { header(h, GW, '0', 0, GNU); memcpy(h + 124, "0000000009a\0", 12); seal(h); put(&b, h, 512); }
        else if (!strcmp(w, "negative")) { header(h, GW, '0', 0, GNU); memset(h + 124, 0xff, 12); seal(h); put(&b, h, 512); }
        else if (!strcmp(w, "ext flag 2")) {
            Ex m[1] = { { 0, 10 } };
            member(&b, GW, '0', pat, 5, GNU);
            size_t at = b.n;
            sparse(&b, WS, m, 1, 9000, NULL, -1);
            b.p[at + 482] = 2; seal(b.p + at);
        } else if (!strcmp(w, "entry after empty")) {
            Ex m[1] = { { 0, 10 } };
            member(&b, GW, '0', pat, 5, GNU);
            size_t at = b.n;
            sparse(&b, WS, m, 1, 9000, NULL, -1);
            num12(b.p + at + 386 + 48, 100); num12(b.p + at + 386 + 60, 0); seal(b.p + at);
        } else if (!strcmp(w, "too many members")) { for (int k = 0; k < VMU_MEMBERS_MAX + 1; k++) member(&b, "LICENSE", '0', pat, 1, GNU); }
        if (!closed) end(&b);
        e = run(b.p, b.n, 0, NULL);
        CHECK(e == cases[i].want, "%s: got %s, want %s", w, vmu_code_name(e), vmu_code_name(cases[i].want));
        if (!strcmp(w, "regular over apparent")) REQ.max_apparent = 1ull << 40;
        if (!strcmp(w, "both")) strcpy(REQ.re[1], "^Whonix-Workstation-LXQt-[0-9]+(\\.[0-9]+){3}\\.Intel_AMD64\\.qcow2$");
        free(b.p);
    }
    free(t.p);
}

/* GNU tar's own sparse members (old-GNU 'S' with extension headers), and its whole archive through the unpacker */
static void test_gnu_tar(void) {
    char d[96], f[160], g[160], cmd[600];
    snprintf(d, sizeof d, "%s/src", tmp);
    mkdir(d, 0700);
    snprintf(f, sizeof f, "%s/%s", d, WS);
    snprintf(g, sizeof g, "%s/%s", d, GW);
    int fd = open(f, O_RDWR | O_CREAT | O_TRUNC, 0600);
    const size_t real = 64u << 20;
    uint8_t *want = calloc(1, real);
    for (int i = 0; i < 40; i++) {                           /* 40 extents: four in the header, 36 in two extensions */
        off_t at = (off_t)i * (1 << 20) + (i * 4096) % 65536;
        for (int k = 0; k < 3000; k++) want[at + k] = (uint8_t)(i * 31 + k + 1);
        if (pwrite(fd, want + at, 3000, at) != 3000) exit(1);
    }
    if (ftruncate(fd, (off_t)real)) exit(1);
    close(fd);
    spit(g, pat, 123456);
    char ta[96];
    snprintf(ta, sizeof ta, "%s/gnu.tar", tmp);
    snprintf(cmd, sizeof cmd, "tar --format=gnu -cSf %s -C %s %s %s 2>/dev/null", ta, d, GW, WS);
    if (system(cmd)) { printf("skipped: GNU tar's sparse archive (needs tar)\n"); free(want); return; }
    size_t n;
    uint8_t *t = slurp(ta, &n);
    int S = 0;
    for (size_t o = 0; o + 512 <= n; o += 512) if (t[o + 156] == 'S' && !memcmp(t + o + 257, "ustar  ", 8)) S = 1;
    CHECK(S, "GNU tar wrote an old-GNU sparse member");
    VmuResult res;
    int e = run(t, n, 0, &res);
    CHECK(e == VMU_OK, "GNU tar's archive: %s", vmu_code_name(e));
    CHECK(same_file(1, want, real, real) && same_file(0, pat, 123456, 123456), "GNU tar's members, byte for byte");
    long long a = allocated(1);
    CHECK(a > 0 && a < 40 * (8192 + 2 * page_unit()) + 65536, "GNU tar's sparse member keeps its holes (%lld allocated)", a);
    size_t xn;
    uint8_t *x = xz_of(t, n, "-6 --block-size=200000 --check=crc64", &xn);
    if (x) {
        e = run(x, xn, 1, &res);
        CHECK(e == VMU_OK && same_file(1, want, real, real), "GNU tar's archive in xz, several blocks: %s", vmu_code_name(e));
    }
    free(x); free(t); free(want);
}

/* the streaming decoder alone: equal to xz_decode, every check type, streams, padding, damage */
static size_t max_dict = 64u << 20;
static int xzs_all(const uint8_t *p, size_t n, uint8_t **out, size_t *on, int *err) {
    Mem m = { p, n, 0 };
    XzStream *s = xzs_new(mem_read, &m, max_dict);
    Buf o = { 0 };
    uint8_t b[33333];
    ssize_t r;
    while ((r = xzs_read(s, b, sizeof b)) > 0) put(&o, b, (size_t)r);
    *err = r < 0 ? xzs_error(s) : 0;
    xzs_free(s);
    *out = o.p;
    *on = o.n;
    return (int)r;
}

static void test_xz(void) {
    size_t n = 3u << 20;
    uint8_t *data = malloc(n);
    uint32_t x = 12345;
    for (size_t i = 0; i < n; i++) { x = x * 1103515245u + 12345u; data[i] = i % 100000 < 50000 ? (uint8_t)(x >> 24) : (uint8_t)(i / 7); }
    size_t xn;
    uint8_t *c = xz_of(data, n, "-1", &xn);
    if (!c) { printf("skipped: xz streams (needs xz)\n"); free(data); return; }
    free(c);
    const char *opts[] = { "-1 --check=crc32", "-6 --check=crc64", "-0 --check=none", "-3 --block-size=100000", "-e -1", "-9 --check=crc64" };
    for (size_t i = 0; i < sizeof opts / sizeof *opts; i++) {
        c = xz_of(data, n, opts[i], &xn);
        uint8_t *o, *o2 = NULL;
        size_t on, on2 = 0;
        int err, r = xzs_all(c, xn, &o, &on, &err);
        CHECK(r == 0 && on == n && !memcmp(o, data, n), "xz %s: streaming (err %d)", opts[i], err);
        CHECK(!xz_decode(c, xn, &o2, &on2, 16u << 20) && on2 == n && !memcmp(o2, data, n), "xz %s: in memory", opts[i]);
        free(o); free(o2);
        if (i == 1) {
            /* two streams with padding between and after */
            Buf b = { 0 };
            put(&b, c, xn); zeros(&b, 8); put(&b, c, xn); zeros(&b, 4);
            r = xzs_all(b.p, b.n, &o, &on, &err);
            CHECK(r == 0 && on == 2 * n && !memcmp(o, data, n) && !memcmp(o + n, data, n), "two streams with padding");
            free(o);
            b.n -= 4; zeros(&b, 3);
            r = xzs_all(b.p, b.n, &o, &on, &err);
            CHECK(r < 0 && err == XZS_E_TRAILING, "padding not a multiple of four: %d", err);
            free(o);
            b.n -= 3; put(&b, "garbage!", 8);
            r = xzs_all(b.p, b.n, &o, &on, &err);
            CHECK(r < 0 && err == XZS_E_TRAILING, "data after the stream: %d", err);
            free(o);
            for (size_t cut = 1; cut < xn; cut += xn / 37 + 1) {
                r = xzs_all(c, xn - cut, &o, &on, &err);
                CHECK(r < 0 && err == XZS_E_TRUNCATED, "cut %zu bytes short: %d", cut, err);
                free(o);
            }
            for (size_t at = 12; at < xn; at += xn / 53 + 1) {
                c[at] ^= 0x10;
                r = xzs_all(c, xn, &o, &on, &err);
                CHECK(r < 0, "a changed byte at %zu is noticed", at);
                free(o);
                c[at] ^= 0x10;
            }
            free(b.p);
        }
        free(c);
    }
    c = xz_of(data, n, "-1 --check=sha256", &xn);
    {
        uint8_t *o;
        size_t on;
        int err, r = xzs_all(c, xn, &o, &on, &err);
        CHECK(r < 0 && err == XZS_E_CHECK, "SHA-256 check refused: %d", err);
        free(o);
    }
    free(c);
    c = xz_of(data, n, "-6", &xn);                     /* a dictionary of 4 MiB (xz fits it to the 3 MiB input) */
    {
        uint8_t *o;
        size_t on;
        max_dict = 1u << 20;
        int err, r = xzs_all(c, xn, &o, &on, &err);
        max_dict = 64u << 20;
        CHECK(r < 0 && err == XZS_E_DICT, "a dictionary over the cap: %d", err);
        free(o);
    }
    free(c);
    {
        uint8_t *o;
        size_t on;
        int err, r = xzs_all((const uint8_t *)"", 0, &o, &on, &err);
        CHECK(r < 0 && err == XZS_E_TRUNCATED, "empty input");
        free(o);
    }
    free(data);
}

/* the unpacker through xz, with damage, then a fuzz pass */
static void test_xz_tar(void) {
    Buf t = { 0 };
    good(&t);
    size_t xn;
    uint8_t *c = xz_of(t.p, t.n, "-6 --block-size=65536", &xn);
    if (!c) { free(t.p); return; }
    VmuResult res;
    int e = run(c, xn, 1, &res);
    CHECK(e == VMU_OK && same_file(0, pat + 7, 70000, 70000), "good archive in xz: %s", vmu_code_name(e));
    e = run(c, xn - 9, 1, NULL);
    CHECK(e == VMU_TRUNCATED, "xz cut short: %s", vmu_code_name(e));
    Buf b = { 0 };
    put(&b, c, xn); put(&b, "PK\3\4", 4);
    e = run(b.p, b.n, 1, NULL);
    CHECK(e == VMU_TRAILING, "data after the xz stream: %s", vmu_code_name(e));
    free(b.p);
    c[xn / 2] ^= 0x40;
    e = run(c, xn, 1, NULL);
    CHECK(e == VMU_BADXZ, "a changed byte in a block: %s", vmu_code_name(e));
    c[xn / 2] ^= 0x40;
    /* fuzz: random changes to the xz and to the raw tar; nothing may crash, hang or leak (ASan) */
    uint32_t r = 2463534242u;
    int okc = 0;
    for (int it = 0; it < 3000; it++) {
        int raw = it & 1;
        Buf m = { 0 };
        put(&m, raw ? t.p : c, raw ? t.n : xn);
        int k = 1 + (int)((r = r * 1664525u + 1013904223u) >> 29);
        for (int j = 0; j < k; j++) {
            r = r * 1664525u + 1013904223u;
            size_t at = (r >> 8) % m.n;
            r = r * 1664525u + 1013904223u;
            switch (r >> 30) {
            case 0: m.p[at] ^= (uint8_t)(1u << ((r >> 5) & 7)); break;
            case 1: m.p[at] = (uint8_t)r; break;
            case 2: m.n = at ? at : 1; break;
            default: if (raw) { m.p[(at & ~511u) + 156 < m.n ? (at & ~511u) + 156 : at] = (uint8_t)"0S125xLK"[r & 7]; } else m.p[at] = 0; break;
            }
        }
        if (run(m.p, m.n, !raw, NULL) == VMU_OK) okc++;
        free(m.p);
    }
    printf("fuzz: 3000 mutated archives, %d still unpacked\n", okc);
    free(c);
    free(t.p);
}

/* lord-vmunpack itself, on descriptors, in its sandbox */
static int helper(const char *bin, const uint8_t *arc, size_t an, const char *req, int fill3, char *ans, size_t ansn) {
    char ap[96];
    snprintf(ap, sizeof ap, "%s/arc", tmp);
    spit(ap, arc, an);
    int a = open(ap, O_RDONLY | O_CLOEXEC), o0 = outfd(0), o1 = outfd(1), rq[2], out[2];
    if (fill3 == 1 && write(o0, "x", 1) != 1) return -1;
    if (fill3 == 2) fcntl(o0, F_SETFL, O_APPEND);          /* pwrite would append: refused */
    if (pipe2(rq, O_CLOEXEC) || pipe2(out, O_CLOEXEC)) return -1;
    if (write(rq[1], req, strlen(req)) != (ssize_t)strlen(req)) return -1;
    close(rq[1]);
    int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
    pid_t pid = fork();
    if (pid == 0) {
        int fds[6] = { a, out[1], null, o0, o1, rq[0] }, hi[6];
        for (int i = 0; i < 6; i++) hi[i] = fcntl(fds[i], F_DUPFD_CLOEXEC, 20);
        for (int i = 0; i < 6; i++) if (dup2(hi[i], i) != i) _exit(127);
        char *args[] = { "lord-vmunpack", NULL }, *env[] = { NULL };
        execve(bin, args, env);
        _exit(127);
    }
    close(out[1]); close(rq[0]); close(a); close(o0); close(o1); close(null);
    char b[8192];
    size_t n = 0;
    ssize_t r;
    while (n < sizeof b - 1 && (r = read(out[0], b + n, sizeof b - 1 - n)) > 0) n += (size_t)r;
    close(out[0]);
    b[n] = 0;
    int st = 0;
    waitpid(pid, &st, 0);
    /* the last line */
    char *l = b + n;
    if (l > b && l[-1] == '\n') l--;
    while (l > b && l[-1] != '\n') l--;
    size_t ll = strlen(l) < ansn - 1 ? strlen(l) : ansn - 1;
    memcpy(ans, l, ll);
    ans[ll] = 0;
    for (char *p = b; p < l;) {                              /* every line before the answer is progress */
        char *e = strchr(p, '\n');
        if (!e) break;
        if (strncmp(p, "progress ", 9)) { fails++; fprintf(stderr, "FAIL: a line that isn't progress: %.*s\n", (int)(e - p), p); }
        p = e + 1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + WTERMSIG(st);
}

static void test_helper(const char *bin) {
    if (access(bin, X_OK)) { printf("skipped: lord-vmunpack itself (no %s)\n", bin); return; }
    Buf t = { 0 };
    good(&t);
    size_t xn;
    uint8_t *c = xz_of(t.p, t.n, "-6", &xn);
    if (!c) { free(t.p); return; }
    char req[600], ans[200];
    snprintf(req, sizeof req, "%llu %llu %s %s\n", (unsigned long long)REQ.max_apparent, (unsigned long long)REQ.max_data, REQ.re[0], REQ.re[1]);
    int st = helper(bin, c, xn, req, 0, ans, sizeof ans);
    char want[120];
    snprintf(want, sizeof want, "ok 70000 1048576 %d\n", 70000 + 4096 + 100 + 5000 + 8192 + 1);
    CHECK(st == 0 && !strcmp(ans, want), "the helper: status %d, answer %s", st, ans);
    CHECK(same_file(0, pat + 7, 70000, 70000), "the helper wrote the gateway's disk");
    st = helper(bin, c, xn - 3, req, 0, ans, sizeof ans);
    CHECK(st == 1 && !strcmp(ans, "error truncated\n"), "the helper on a cut archive: %d %s", st, ans);
    st = helper(bin, c, xn, "1 2 nope\n", 0, ans, sizeof ans);
    CHECK(st == 1 && !strcmp(ans, "error request\n"), "the helper with a bad request: %d %s", st, ans);
    st = helper(bin, c, xn, req, 1, ans, sizeof ans);
    CHECK(st == 1 && !strcmp(ans, "error request\n"), "the helper with an output that isn't empty: %d %s", st, ans);
    st = helper(bin, c, xn, req, 2, ans, sizeof ans);
    CHECK(st == 1 && !strcmp(ans, "error request\n"), "the helper with an output opened for appending: %d %s", st, ans);
    Buf b = { 0 };
    member(&b, "../../etc/passwd", '0', pat, 5, GNU);
    end(&b);
    free(c);
    c = xz_of(b.p, b.n, "-1", &xn);
    st = helper(bin, c, xn, req, 0, ans, sizeof ans);
    CHECK(st == 1 && !strcmp(ans, "error badName\n"), "the helper on a path in a name: %d %s", st, ans);
    free(b.p); free(c); free(t.p);
}

int main(int argc, char **argv) {
    snprintf(tmp, sizeof tmp, "/tmp/vmunpack-test.XXXXXX");
    if (!mkdtemp(tmp)) { perror("mkdtemp"); return 1; }
    for (size_t i = 0; i < sizeof pat; i++) pat[i] = (uint8_t)(i * 7 + (i >> 8) + 1);
    const char *line = "1099511627776 300000 ^Whonix-Gateway-LXQt-[0-9]+(\\.[0-9]+){3}\\.Intel_AMD64\\.qcow2$ ^Whonix-Workstation-LXQt-[0-9]+(\\.[0-9]+){3}\\.Intel_AMD64\\.qcow2$\n";
    if (vmu_req_parse(line, strlen(line), &REQ)) { fprintf(stderr, "the test's request doesn't parse\n"); return 1; }
    test_req();
    test_tar();
    REQ.max_data = 1ull << 30;
    test_gnu_tar();
    test_xz();
    REQ.max_data = 300000;
    test_xz_tar();
    if (argc > 1) test_helper(argv[1]);
    char cmd[120];
    snprintf(cmd, sizeof cmd, "rm -rf %s", tmp);
    if (system(cmd)) { /* a temporary folder left behind */ }
    if (fails) { fprintf(stderr, "vmunpack-test: %d failures\n", fails); return 1; }
    printf("vmunpack-test: ok\n");
    return 0;
}
