/* vmexec.c — VM_EXEC's request and answer, and what the guest's scripts print (vmexec.h). Pure, apart from the memfd
 * helpers; tests/vmexec_test.c checks every limit and fuzzes the three parsers under ASan and UBSan. */
#include "vm/vmexec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define PERSON_FIND \
    "u=$(loginctl list-sessions --no-legend 2>/dev/null | awk '$2>=1000 && $2<60000 && $4 ~ /^seat/ {print $3; exit}'); " \
    "[ -n \"$u\" ] || u=$(loginctl list-sessions --no-legend 2>/dev/null | awk '$2>=1000 && $2<60000 {print $3; exit}'); " \
    "[ -n \"$u\" ] || u=$(getent passwd | awk -F: '$3>=1000 && $3<60000 && $7 !~ /(nologin|false)$/ {print $1; exit}'); "

/* as root: only finds the person and becomes them; the program, its arguments and its input never touch root's shell */
const char VMX_ROOT_SH[] =
    "p=\"$1\"; t=\"$2\"; shift 2; " PERSON_FIND
    "[ -n \"$u\" ] || { echo 'no account of a person in the guest' >&2; exit 3; }; "
    "command -v runuser >/dev/null 2>&1 || { echo 'the guest has no runuser' >&2; exit 4; }; "
    "exec runuser -u \"$u\" -- /bin/sh -c \"$p\" lordos-exec \"$t\" \"$@\"";

/* as the person, in their home folder: the program under timeout(1), its output kept in a private temporary folder,
 * then the header and at most VMX_OUT_MAX + VMX_ERR_MAX bytes of it */
const char VMX_PERSON_SH[] =
    "t=\"$1\"; shift; cd -- \"$HOME\" 2>/dev/null || cd /; "
    "d=$(umask 077; mktemp -d 2>/dev/null) || { echo 'no temporary folder in the guest' >&2; exit 70; }; "
    "timeout -k 5 \"$t\" \"$@\" >\"$d/o\" 2>\"$d/e\"; r=$?; "
    "o=$(wc -c <\"$d/o\"); e=$(wc -c <\"$d/e\"); "
    "printf 'LVXG %d %d %d\\n' \"$r\" \"$((o))\" \"$((e))\"; "
    "head -c 131072 \"$d/o\"; head -c 32768 \"$d/e\"; rm -rf -- \"$d\"";

_Static_assert(VMX_OUT_MAX == 131072 && VMX_ERR_MAX == 32768, "VMX_PERSON_SH's head -c counts");

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

uint8_t *vmx_req_encode(unsigned timeout, int argc, const char *const *argv, const uint8_t *in, size_t inlen, size_t *len) {
    if (timeout < 1 || timeout > VMX_TIMEOUT_MAX || argc < 1 || argc > VMX_ARGC_MAX || inlen > VMX_STDIN_MAX || (inlen && !in)) return NULL;
    size_t total = 0;
    for (int i = 0; i < argc; i++) {
        size_t l = argv[i] ? strlen(argv[i]) : 0;
        if (l < 1 || l > VMX_ARG_MAX) return NULL;
        total += l;
    }
    if (total > VMX_ARGS_MAX) return NULL;
    size_t n = 12 + 4 * (size_t)argc + total + 4 + inlen;
    uint8_t *b = malloc(n), *p = b;
    if (!b) return NULL;
    memcpy(p, "LVX1", 4); put32(p + 4, timeout); put32(p + 8, (uint32_t)argc); p += 12;
    for (int i = 0; i < argc; i++) {
        size_t l = strlen(argv[i]);
        put32(p, (uint32_t)l); memcpy(p + 4, argv[i], l); p += 4 + l;
    }
    put32(p, (uint32_t)inlen); p += 4;
    if (inlen) memcpy(p, in, inlen);
    *len = n;
    return b;
}

int vmx_req_parse(const uint8_t *b, size_t n, VmxReq *r) {
    memset(r, 0, sizeof *r);
    if (!b || n < 16 || n > VMX_REQ_MAX || memcmp(b, "LVX1", 4)) return -1;
    uint32_t t = get32(b + 4), c = get32(b + 8);
    if (t < 1 || t > VMX_TIMEOUT_MAX || c < 1 || c > VMX_ARGC_MAX) return -1;
    size_t at = 12, used = 0;
    for (uint32_t i = 0; i < c; i++) {
        if (n - at < 4) return -1;
        uint32_t l = get32(b + at);
        at += 4;
        if (l < 1 || l > VMX_ARG_MAX || l > n - at || used + l > VMX_ARGS_MAX) return -1;
        if (memchr(b + at, 0, l)) return -1;
        memcpy(r->store + used + i, b + at, l);          /* each copy followed by its NUL: i bytes of NULs before it */
        r->store[used + i + l] = 0;
        r->argv[i] = r->store + used + i;
        used += l;
        at += l;
    }
    if (n - at < 4) return -1;
    uint32_t il = get32(b + at);
    at += 4;
    if (il > VMX_STDIN_MAX || il != n - at) return -1;
    r->timeout = t;
    r->argc = (int)c;
    r->argv[c] = NULL;
    r->in = il ? b + at : NULL;
    r->inlen = il;
    return 0;
}

uint8_t *vmx_res_encode(const VmxRes *r, size_t *len) {
    if (r->outlen > VMX_OUT_MAX || r->errlen > VMX_ERR_MAX || r->exit < 0 || r->exit > 255 || (r->flags & ~7u)) return NULL;
    size_t n = 20 + r->outlen + r->errlen;
    uint8_t *b = malloc(n);
    if (!b) return NULL;
    memcpy(b, "LVXR", 4); put32(b + 4, (uint32_t)r->exit); put32(b + 8, r->flags);
    put32(b + 12, (uint32_t)r->outlen);
    if (r->outlen) memcpy(b + 16, r->out, r->outlen);
    put32(b + 16 + r->outlen, (uint32_t)r->errlen);
    if (r->errlen) memcpy(b + 20 + r->outlen, r->err, r->errlen);
    *len = n;
    return b;
}

int vmx_res_parse(const uint8_t *b, size_t n, VmxRes *r) {
    memset(r, 0, sizeof *r);
    if (!b || n < 20 || n > VMX_RES_MAX || memcmp(b, "LVXR", 4)) return -1;
    uint32_t e = get32(b + 4), f = get32(b + 8), ol = get32(b + 12);
    if (e > 255 || (f & ~7u) || ol > VMX_OUT_MAX || ol > n - 20) return -1;
    uint32_t el = get32(b + 16 + ol);
    if (el > VMX_ERR_MAX || el != n - 20 - ol) return -1;
    r->exit = (int)e; r->flags = f;
    r->out = b + 16; r->outlen = ol;
    r->err = b + 20 + ol; r->errlen = el;
    return 0;
}

/* a decimal number of at most 10 digits, no sign, no leading zero (but "0"), ending at `end` (' ' or '\n') */
static int dec(const uint8_t *b, size_t n, size_t *at, uint8_t end, unsigned long long *v) {
    size_t s = *at, k = 0;
    unsigned long long x = 0;
    while (s + k < n && b[s + k] >= '0' && b[s + k] <= '9' && k < 11) { x = x * 10 + (unsigned)(b[s + k] - '0'); k++; }
    if (!k || k > 10 || (k > 1 && b[s] == '0') || s + k >= n || b[s + k] != end) return -1;
    *at = s + k + 1;
    *v = x;
    return 0;
}

int vmx_guest_parse(const uint8_t *b, size_t n, VmxRes *r) {
    memset(r, 0, sizeof *r);
    if (!b || n < 11 || n > 64 + VMX_OUT_MAX + VMX_ERR_MAX || memcmp(b, "LVXG ", 5)) return -1;
    size_t at = 5;
    unsigned long long e, o, x;
    if (dec(b, n, &at, ' ', &e) || dec(b, n, &at, ' ', &o) || dec(b, n, &at, '\n', &x) || e > 255) return -1;
    size_t ok = o > VMX_OUT_MAX ? VMX_OUT_MAX : (size_t)o, xk = x > VMX_ERR_MAX ? VMX_ERR_MAX : (size_t)x;
    if (n - at != ok + xk) return -1;
    r->exit = (int)e;
    r->flags = (o > VMX_OUT_MAX ? VMX_F_OUT_CUT : 0) | (x > VMX_ERR_MAX ? VMX_F_ERR_CUT : 0) | (e == VMX_EXIT_TIMEOUT || e == 137 ? VMX_F_TIMEOUT : 0);
    r->out = b + at; r->outlen = ok;
    r->err = b + at + ok; r->errlen = xk;
    return 0;
}

int vmx_memfd(const char *name, const uint8_t *b, size_t n) {
    int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    size_t w = 0;
    while (w < n) {
        ssize_t k = write(fd, b + w, n - w);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { close(fd); return -1; }
        w += (size_t)k;
    }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)) { close(fd); return -1; }
    return fd;
}

uint8_t *vmx_memfd_read(int fd, size_t max, size_t *len) {
    struct stat st;
    int seals = fcntl(fd, F_GET_SEALS);
    int need = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    if (seals < 0 || (seals & need) != need || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 1 || (unsigned long long)st.st_size > max) return NULL;
    size_t n = (size_t)st.st_size, got = 0;
    uint8_t *b = malloc(n);
    if (!b) return NULL;
    while (got < n) {
        ssize_t k = pread(fd, b + got, n - got, (off_t)got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { free(b); return NULL; }
        got += (size_t)k;
    }
    *len = n;
    return b;
}

int vmx_guest_path_ok(const char *p) {
    size_t n = p ? strlen(p) : 0;
    if (n < 1 || n > VMX_ARG_MAX) return 0;
    const unsigned char *s = (const unsigned char *)p;
    for (size_t i = 0; i < n;) {
        unsigned c = s[i];
        if (c < 0x20 || c == 0x7f) return 0;
        if (c < 0x80) { i++; continue; }
        int k = c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xc2 && c <= 0xdf ? 1 : -1;
        if (k < 0 || n - i <= (size_t)k) return 0;
        for (int j = 1; j <= k; j++) if ((s[i + j] & 0xc0) != 0x80) return 0;
        /* overlong three- and four-byte forms, surrogates, past U+10FFFF */
        if ((c == 0xe0 && s[i + 1] < 0xa0) || (c == 0xed && s[i + 1] > 0x9f) || (c == 0xf0 && s[i + 1] < 0x90) || (c == 0xf4 && s[i + 1] > 0x8f)) return 0;
        /* C1 controls (U+0080..U+009F) */
        if (c == 0xc2 && s[i + 1] < 0xa0) return 0;
        i += (size_t)k + 1;
    }
    return 1;
}
