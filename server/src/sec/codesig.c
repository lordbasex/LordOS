/* codesig.c — verified system code: the signed manifest of LordOS's own JavaScript and the
 * check of each file at load time (see codesig.h and docs/SIGNING.md, "Verified system code"). */
#include "codesig.h"
#include "json.h"
#include "../pkg/crypto.h"
#include "codesig_build.h"         /* build/gen: RELEASE=1 and CODE_PUBKEY (Makefile) */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#ifdef SYS_openat2
#include <linux/openat2.h>
#endif

/* tools/release/lordos-release.pub.pem: the last 32 bytes of its DER (SubjectPublicKeyInfo).
 * codesig_test checks that they match the PEM file. */
const uint8_t codesig_release_key[32] = {
    0xf1, 0xca, 0xb0, 0x0f, 0x31, 0xfe, 0x65, 0x53, 0x1d, 0x06, 0xa3, 0x63, 0x0b, 0x7d, 0x78, 0xd5,
    0xd9, 0xa8, 0xde, 0x07, 0x07, 0xf0, 0xf5, 0xad, 0xf3, 0xc2, 0x3e, 0x44, 0x4f, 0x5d, 0x39, 0xa0,
};

static void seterr(char *err, size_t n, const char *fmt, const char *a) {
    if (err && n) snprintf(err, n, fmt, a ? a : "");
}

/* ---- base64 ---- */

static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int codesig_sig_decode(const char *text, size_t len, uint8_t out[64]) {
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) len--;
    /* 64 bytes = 86 characters + "==" */
    if (len != 88 || text[86] != '=' || text[87] != '=') return -1;
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < 86; i++) {
        int v = b64v((unsigned char)text[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= 64) return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    /* the 4 leftover bits must be zero (one encoding per signature) */
    if (o != 64 || bits != 4 || (acc & 0xf)) return -1;
    return 0;
}

/* ---- the manifest ---- */

static int valid_rel(const char *p) {
    size_t n = strlen(p);
    if (!n || n > CODE_PATH_MAX || p[0] == '/' || p[n - 1] == '/') return 0;
    const char *seg = p;
    for (size_t i = 0; i <= n; i++) {
        char c = p[i];
        if (c == '/' || c == 0) {
            size_t sl = (size_t)(p + i - seg);
            if (sl == 0 || (sl == 1 && seg[0] == '.') || (sl == 2 && seg[0] == '.' && seg[1] == '.')) return 0;
            seg = p + i + 1;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-' || c == '+')) return 0;
    }
    return 1;
}

static int lower_hex64(const char *s) {
    if (!s || strlen(s) != 64) return 0;
    for (int i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

void codesig_free(CodeManifest *m) {
    if (!m) return;
    free(m->files);
    memset(m, 0, sizeof *m);
}

int codesig_parse(const char *json, size_t len, const char *sig, size_t siglen, const uint8_t pub[32],
                  CodeManifest *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    if (!json || len == 0 || len > CODE_MANIFEST_MAX) { seterr(err, errlen, "manifest size%s", NULL); return -1; }
    /* the signature first: nothing of an unsigned text is parsed when a signature is expected */
    if (sig) {
        uint8_t s[64];
        if (codesig_sig_decode(sig, siglen, s)) { seterr(err, errlen, "signature is not 64 bytes of base64%s", NULL); return -1; }
        if (!ed25519_verify(pub, s, (const uint8_t *)json, len)) { seterr(err, errlen, "bad signature%s", NULL); return -1; }
        out->signed_ok = 1;
    }
    JDoc d;
    char jerr[96];
    if (json_parse(json, len, &d, jerr, sizeof jerr)) { seterr(err, errlen, "manifest: %s", jerr); return -1; }
    int rc = -1;
    const JVal *r = d.root;
    const JVal *files = json_get(r, "files");
    const char *kind = json_str(r, "kind"), *ver = json_str(r, "version");
    if (!r || r->type != J_OBJ || json_num(r, "format", 0) != 1) { seterr(err, errlen, "manifest: format is not 1%s", NULL); goto out; }
    if (!kind || strcmp(kind, CODE_KIND)) { seterr(err, errlen, "manifest: kind is not " CODE_KIND "%s", NULL); goto out; }
    if (!ver || strlen(ver) >= sizeof out->version) { seterr(err, errlen, "manifest: bad version%s", NULL); goto out; }
    if (!files || files->type != J_ARR || files->len > CODE_FILES_MAX) { seterr(err, errlen, "manifest: bad file list%s", NULL); goto out; }
    snprintf(out->version, sizeof out->version, "%s", ver);
    out->files = calloc(files->len ? files->len : 1, sizeof *out->files);
    if (!out->files) { seterr(err, errlen, "out of memory%s", NULL); goto out; }
    for (const JVal *f = files->kid; f; f = f->next) {
        const char *p = json_str(f, "path"), *h = json_str(f, "sha256");
        const JVal *sz = json_get(f, "size");
        if (f->type != J_OBJ || !p || !valid_rel(p)) { seterr(err, errlen, "manifest: bad path %s", p); goto out; }
        if (!sz || sz->type != J_NUM || sz->num < 0 || sz->num > CODE_FILE_MAX || sz->num != (double)(uint64_t)sz->num) {
            seterr(err, errlen, "manifest: bad size for %s", p); goto out;
        }
        if (!lower_hex64(h)) { seterr(err, errlen, "manifest: bad sha256 for %s", p); goto out; }
        if (out->nfiles && strcmp(out->files[out->nfiles - 1].path, p) >= 0) { seterr(err, errlen, "manifest: paths not sorted or repeated at %s", p); goto out; }
        CodeFile *cf = &out->files[out->nfiles++];
        snprintf(cf->path, sizeof cf->path, "%s", p);
        cf->size = (uint64_t)sz->num;
        hex_decode(h, cf->sha256, 32);
    }
    rc = 0;
out:
    json_free(&d);
    if (rc) codesig_free(out);
    return rc;
}

const CodeFile *codesig_find(const CodeManifest *m, const char *rel) {
    int lo = 0, hi = m ? m->nfiles - 1 : -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(m->files[mid].path, rel);
        if (!c) return &m->files[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

int codesig_check_buf(const CodeManifest *m, const char *rel, const void *buf, size_t len, char *why, size_t n) {
    const CodeFile *f = codesig_find(m, rel);
    if (!f) { seterr(why, n, "not listed in the manifest%s", NULL); return -1; }
    if (f->size != (uint64_t)len) { seterr(why, n, "size differs from the manifest%s", NULL); return -1; }
    uint8_t h[32];
    sha256(buf, len, h);
    if (memcmp(h, f->sha256, 32)) { seterr(why, n, "SHA-256 differs from the manifest%s", NULL); return -1; }
    return 0;
}

/* ---- the process's state ---- */

static struct {
    CodeState state;
    char root[PATH_MAX];
    int rootfd;
    int release;
    int have_manifest;
    CodeManifest m;
    char reason[160];
    double ms;
    CodeRefusal refused[16];
    int nrefused;
} cs = { .state = CODE_UNCHECKED, .rootfd = -1 };
/* codesig_load also runs on the app host's bytecode thread (app/apphost_bc.c), after the state
 * was set up on the main thread: the manifest is read-only then, only the time spent and the
 * refusals change, under this lock */
static pthread_mutex_t cs_mu = PTHREAD_MUTEX_INITIALIZER;
static void add_ms(double dt) { pthread_mutex_lock(&cs_mu); cs.ms += dt; pthread_mutex_unlock(&cs_mu); }

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int codesig_release_build(void) {
#ifdef LORD_CODE_RELEASE
    return 1;
#else
    return 0;
#endif
}

/* Reads a whole regular file from fd: at most max bytes; NULL if bigger, not regular or unreadable. */
static char *read_fd(int fd, size_t max, size_t *len) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > max) return NULL;
    size_t cap = (size_t)st.st_size + 1, n = 0;
    char *buf = malloc(cap + 1);
    if (!buf) return NULL;
    for (;;) {
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0) { if (errno == EINTR) continue; free(buf); return NULL; }
        if (r == 0) break;
        n += (size_t)r;
        if (n == cap) { free(buf); return NULL; }       /* it grew while being read */
    }
    buf[n] = 0;
    *len = n;
    return buf;
}

/* Opens rel under the root without leaving it (no "..", no absolute links, no /proc magic). */
static int open_beneath(const char *rel) {
#ifdef SYS_openat2
    struct open_how how = { .flags = O_RDONLY | O_CLOEXEC | O_NOCTTY, .resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS };
    int fd = (int)syscall(SYS_openat2, cs.rootfd, rel, &how, sizeof how);
    if (fd >= 0 || errno != ENOSYS) return fd;
#endif
    /* kernels before 5.6: rel is already validated (no ".."); no links at the last step */
    return openat(cs.rootfd, rel, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW);
}

static char *read_beneath(const char *rel, size_t max, size_t *len) {
    int fd = open_beneath(rel);
    if (fd < 0) return NULL;
    char *b = read_fd(fd, max, len);
    close(fd);
    return b;
}

static void reset(void) {
    codesig_free(&cs.m);
    if (cs.rootfd >= 0) close(cs.rootfd);
    cs.rootfd = -1;
    cs.have_manifest = 0;
    cs.root[0] = cs.reason[0] = 0;
    cs.nrefused = 0;
    cs.ms = 0;
    cs.state = CODE_UNCHECKED;
}

CodeState codesig_init_root(const char *root, const uint8_t pub[32], int release) {
    reset();
    double t0 = now_ms();
    cs.release = release;
    if (!root || !realpath(root, cs.root) || (cs.rootfd = open(cs.root, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
        snprintf(cs.reason, sizeof cs.reason, "no Axia DE folder");
        cs.root[0] = 0;
        cs.state = release ? CODE_INVALID : CODE_DEVELOPMENT;
        goto done;
    }
    size_t mlen = 0, slen = 0;
    char *mtext = read_beneath(CODE_MANIFEST, CODE_MANIFEST_MAX, &mlen);
    char *stext = read_beneath(CODE_SIGNATURE, 4096, &slen);
    if (!mtext) {
        snprintf(cs.reason, sizeof cs.reason, "no %s", CODE_MANIFEST);
        cs.state = release ? CODE_INVALID : CODE_DEVELOPMENT;
    } else if (!stext && release) {
        snprintf(cs.reason, sizeof cs.reason, "unsigned manifest in a release build");
        cs.state = CODE_INVALID;
    } else {
        char err[128];
        if (codesig_parse(mtext, mlen, stext, slen, pub, &cs.m, err, sizeof err)) {
            snprintf(cs.reason, sizeof cs.reason, "%s", err);
            cs.state = CODE_INVALID;
        } else {
            cs.have_manifest = 1;
            cs.state = cs.m.signed_ok ? CODE_VERIFIED : CODE_DEVELOPMENT;
            if (!cs.m.signed_ok) snprintf(cs.reason, sizeof cs.reason, "unsigned manifest");
        }
    }
    free(mtext);
    free(stext);
done:
    cs.ms += now_ms() - t0;
    fprintf(stderr, "code: %s (%s%s%s) in %.2f ms, %d files listed\n", codesig_state_name(cs.state), cs.root[0] ? cs.root : "-",
            cs.reason[0] ? ": " : "", cs.reason, cs.ms, cs.have_manifest ? cs.m.nfiles : 0);
    return cs.state;
}

const uint8_t *codesig_key(void) {
#ifdef LORD_CODE_PUBKEY_HEX
    /* a test build with a throwaway key (make CODE_PUBKEY=<hex>, docs/SIGNING.md); never a release */
    static uint8_t k[32];
    static int done;
    if (!done) { done = 1; if (hex_decode(LORD_CODE_PUBKEY_HEX, k, 32)) memset(k, 0, 32); }
    return k;
#else
    return codesig_release_key;
#endif
}

CodeState codesig_init(const char *assets_dir) {
    if (cs.state != CODE_UNCHECKED) return cs.state;
    char root[PATH_MAX];
    if (!assets_dir || (size_t)snprintf(root, sizeof root, "%s/..", assets_dir) >= sizeof root) root[0] = 0;
    return codesig_init_root(root[0] ? root : NULL, codesig_key(), codesig_release_build());
}

CodeState codesig_state(void) { return cs.state; }
const char *codesig_root(void) { return cs.root; }
const char *codesig_reason(void) { return cs.reason; }
const CodeManifest *codesig_manifest(void) { return cs.have_manifest ? &cs.m : NULL; }
double codesig_ms(void) { pthread_mutex_lock(&cs_mu); double v = cs.ms; pthread_mutex_unlock(&cs_mu); return v; }
int codesig_refused_count(void) { pthread_mutex_lock(&cs_mu); int v = cs.nrefused; pthread_mutex_unlock(&cs_mu); return v; }
CodeRefusal *codesig_refused(int i) { return i >= 0 && i < codesig_refused_count() ? &cs.refused[i] : NULL; }

const char *codesig_state_name(CodeState s) {
    switch (s) {
    case CODE_VERIFIED: return "verified";
    case CODE_DEVELOPMENT: return "development";
    case CODE_INVALID: return "invalid";
    default: return "unchecked";
    }
}

static void refuse(const char *rel, const char *why) {
    fprintf(stderr, "code: REFUSED %s: %s — this Axia DE component was modified and was not loaded\n", rel, why);
    pthread_mutex_lock(&cs_mu);
    int known = 0;
    for (int i = 0; i < cs.nrefused; i++) if (!strcmp(cs.refused[i].path, rel)) known = 1;
    if (!known && cs.nrefused < (int)(sizeof cs.refused / sizeof cs.refused[0])) {
        CodeRefusal *r = &cs.refused[cs.nrefused];
        snprintf(r->path, sizeof r->path, "%s", rel);
        snprintf(r->reason, sizeof r->reason, "%s", why);
        r->reported = 0;
        cs.nrefused++;                  /* after the entry is complete */
    }
    pthread_mutex_unlock(&cs_mu);
}

char *codesig_load(const char *path, size_t max, size_t *len, char *why, size_t whylen) {
    char wbuf[128];
    if (!why) { why = wbuf; whylen = sizeof wbuf; }
    why[0] = 0;
    if (cs.state == CODE_UNCHECKED) codesig_init(NULL);
    double t0 = now_ms();
    char real[PATH_MAX];
    const char *rel = NULL;
    size_t rl = strlen(cs.root);
    if (cs.root[0] && realpath(path, real) && !strncmp(real, cs.root, rl) && real[rl] == '/') rel = real + rl + 1;
    char *buf = NULL;
    size_t n = 0;
    if (cs.state == CODE_INVALID) {
        snprintf(why, whylen, "the system code manifest is not valid (%s)", cs.reason);
        refuse(rel ? rel : path, why);
        return NULL;
    }
    if (!rel || !valid_rel(rel)) {
        /* outside LordOS's folder: only a development build without a signed manifest runs it */
        if (cs.state != CODE_DEVELOPMENT) { snprintf(why, whylen, "not part of Axia DE's system code"); refuse(path, why); return NULL; }
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
        if (fd >= 0) { buf = read_fd(fd, max, &n); close(fd); }
        if (!buf) { snprintf(why, whylen, "cannot read it"); return NULL; }
        fprintf(stderr, "code: %s loaded unchecked (outside %s, development build)\n", path, cs.root[0] ? cs.root : "the LordOS folder");
        *len = n;
        return buf;
    }
    buf = read_beneath(rel, max, &n);
    if (!buf) { snprintf(why, whylen, "cannot read it"); return NULL; }   /* missing: not a modification */
    if (!cs.have_manifest) {
        /* development build without a manifest: runs, flagged in About */
        add_ms(now_ms() - t0);
        *len = n;
        return buf;
    }
    if (codesig_check_buf(&cs.m, rel, buf, n, why, whylen)) {
        free(buf);
        refuse(rel, why);
        return NULL;
    }
    double dt = now_ms() - t0;
    add_ms(dt);
    fprintf(stderr, "code: %s %s (%zu KB) checked in %.2f ms\n", cs.state == CODE_VERIFIED ? "verified" : "hash ok", rel, n / 1024, dt);
    *len = n;
    return buf;
}
