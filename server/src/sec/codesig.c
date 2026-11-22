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

/* What identifies one file of the manifest pair: a package update replaces it (new inode, new
 * times), so a change is seen without reading it. present = 0: there is no such file. */
typedef struct { int present; dev_t dev; ino_t ino; off_t size; long mts, mtn, cts, ctn; } FileId;
typedef struct { FileId m, s; } PairId;

#define CODE_RELOADS_MAX 32         /* manifests replaced in one process; then a log in is needed */

static struct {
    CodeState state;
    char root[PATH_MAX];
    int rootfd;
    int release;
    CodeManifest *m;                /* the manifest in use (NULL without one); read with cur() */
    CodeManifest *retired[CODE_RELOADS_MAX];   /* replaced ones, kept until reset: a caller may still hold one */
    int nretired;
    uint8_t pub[32];                /* the key the login check used; a reload uses the same one */
    PairId id;                      /* the files the manifest in use was read from */
    PairId tried;                   /* the last pair a reload looked at (each pair is read at most once) */
    double last_try;                /* when (now_ms), for the rate limit */
    unsigned generation;            /* manifests accepted so far in this process (0: the login one) */
    char reload_why[96];            /* why the last reload was refused, for the refusal's reason */
    char reason[160];
    double ms;
    CodeRefusal refused[16];
    int nrefused;
} cs = { .state = CODE_UNCHECKED, .rootfd = -1 };
/* codesig_load also runs on the app host's bytecode thread (app/apphost_bc.c), after the state
 * was set up on the main thread: only the time spent and the refusals change, under this lock;
 * a reload (a new manifest after a package update) runs under rl_mu and publishes the new manifest
 * with one atomic pointer store, so a reader sees the old manifest or the new one, never a mix */
static pthread_mutex_t cs_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rl_mu = PTHREAD_MUTEX_INITIALIZER;
static void add_ms(double dt) { pthread_mutex_lock(&cs_mu); cs.ms += dt; pthread_mutex_unlock(&cs_mu); }
static CodeManifest *cur(void) { return __atomic_load_n(&cs.m, __ATOMIC_ACQUIRE); }

/* Who may own the manifest pair for a reload: root; and the minimum time between two reads. Only
 * codesig-test (built with LORD_CODESIG_TEST) may change them (codesig_test_policy); lord-server
 * has no way to. */
#ifndef LORD_CODESIG_TEST
static const uid_t trusted_uid = 0;
static const double reload_interval_ms = 1000;
#else
static uid_t trusted_uid = 0;
static double reload_interval_ms = 1000;
void codesig_test_policy(uid_t owner, int interval_ms) { trusted_uid = owner; reload_interval_ms = interval_ms; }
#endif

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

/* Reads a whole regular file from fd: at most max bytes; NULL if bigger, not regular or unreadable.
 * stp (may be NULL) gets the file's stat. */
static char *read_fd_st(int fd, size_t max, size_t *len, struct stat *stp) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > max) return NULL;
    if (stp) *stp = st;
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
static char *read_fd(int fd, size_t max, size_t *len) { return read_fd_st(fd, max, len, NULL); }

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

static void file_id(FileId *f, const struct stat *st) {
    memset(f, 0, sizeof *f);
    if (!st) return;
    f->present = 1;
    f->dev = st->st_dev;
    f->ino = st->st_ino;
    f->size = st->st_size;
    f->mts = (long)st->st_mtim.tv_sec;
    f->mtn = st->st_mtim.tv_nsec;
    f->cts = (long)st->st_ctim.tv_sec;
    f->ctn = st->st_ctim.tv_nsec;
}
static int file_id_eq(const FileId *a, const FileId *b) {
    if (a->present != b->present) return 0;
    return !a->present || (a->dev == b->dev && a->ino == b->ino && a->size == b->size && a->mts == b->mts &&
                           a->mtn == b->mtn && a->cts == b->cts && a->ctn == b->ctn);
}
static int pair_id_eq(const PairId *a, const PairId *b) { return file_id_eq(&a->m, &b->m) && file_id_eq(&a->s, &b->s); }

/* The pair as it is on disk now, without opening it (a link is seen as the link itself). */
static void pair_id_now(PairId *id) {
    struct stat st;
    file_id(&id->m, fstatat(cs.rootfd, CODE_MANIFEST, &st, AT_SYMLINK_NOFOLLOW) ? NULL : &st);
    file_id(&id->s, fstatat(cs.rootfd, CODE_SIGNATURE, &st, AT_SYMLINK_NOFOLLOW) ? NULL : &st);
}

/* Owned by the trusted owner (root) and not writable by its group or by others. */
static int owner_ok(const struct stat *st) { return st->st_uid == trusted_uid && !(st->st_mode & (S_IWGRP | S_IWOTH)); }

/* Opens one file of the pair, directly in the root: never a link (O_NOFOLLOW; the name has no '/'),
 * never a FIFO left waiting (O_NONBLOCK; read_fd_st takes only a regular file). */
static int open_pair_file(const char *name) {
    return openat(cs.rootfd, name, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW | O_NONBLOCK);
}

/* Reads code-manifest.json and code-manifest.sig from the root folder held open since the login
 * check: the one reader for the login check and for a reload. Each is read whole from one
 * descriptor, regular files only, never through a link, within the size limits. *mtext / *stext are
 * NULL when missing (or refused: a link, not a regular file, too big; mwhy says which for the
 * manifest). id gets what was read; *trusted is 1 when the root folder and both files are owned by
 * root and writable by nobody else (a reload needs that; the login check only records it). */
static void read_pair(char **mtext, size_t *mlen, char **stext, size_t *slen, PairId *id, int *trusted,
                      char *mwhy, size_t mwhylen) {
    struct stat st, rst;
    *mtext = *stext = NULL;
    *mlen = *slen = 0;
    memset(id, 0, sizeof *id);
    snprintf(mwhy, mwhylen, "no %s", CODE_MANIFEST);
    int ok = !fstat(cs.rootfd, &rst) && owner_ok(&rst);
    int fd = open_pair_file(CODE_MANIFEST);
    if (fd >= 0) {
        if ((*mtext = read_fd_st(fd, CODE_MANIFEST_MAX, mlen, &st))) { file_id(&id->m, &st); ok = ok && owner_ok(&st); }
        else snprintf(mwhy, mwhylen, "%s is not a regular file within the size limit", CODE_MANIFEST);
        close(fd);
    } else if (errno == ELOOP) snprintf(mwhy, mwhylen, "%s is a link", CODE_MANIFEST);
    fd = open_pair_file(CODE_SIGNATURE);
    if (fd >= 0) {
        if ((*stext = read_fd_st(fd, 4096, slen, &st))) { file_id(&id->s, &st); ok = ok && owner_ok(&st); }
        else ok = 0;                /* there is something, but not a signature that can be read */
        close(fd);
    } else if (errno != ENOENT) ok = 0;
    *trusted = ok && *mtext;
}

static void reset(void) {
    CodeManifest *m = cur();
    if (m) { codesig_free(m); free(m); }
    for (int i = 0; i < cs.nretired; i++) { codesig_free(cs.retired[i]); free(cs.retired[i]); }
    __atomic_store_n(&cs.m, NULL, __ATOMIC_RELEASE);
    cs.nretired = 0;
    if (cs.rootfd >= 0) close(cs.rootfd);
    cs.rootfd = -1;
    cs.root[0] = cs.reason[0] = cs.reload_why[0] = 0;
    memset(&cs.id, 0, sizeof cs.id);
    memset(&cs.tried, 0, sizeof cs.tried);
    cs.last_try = 0;
    cs.generation = 0;
    cs.nrefused = 0;
    cs.ms = 0;
    cs.state = CODE_UNCHECKED;
}

CodeState codesig_init_root(const char *root, const uint8_t pub[32], int release) {
    reset();
    double t0 = now_ms();
    cs.release = release;
    memcpy(cs.pub, pub, 32);
    int trusted = 0;
    if (!root || !realpath(root, cs.root) || (cs.rootfd = open(cs.root, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
        snprintf(cs.reason, sizeof cs.reason, "no LordOS folder");
        cs.root[0] = 0;
        cs.state = release ? CODE_INVALID : CODE_DEVELOPMENT;
        goto done;
    }
    size_t mlen = 0, slen = 0;
    char *mtext, *stext, mwhy[96];
    read_pair(&mtext, &mlen, &stext, &slen, &cs.id, &trusted, mwhy, sizeof mwhy);
    cs.tried = cs.id;
    if (!mtext) {
        snprintf(cs.reason, sizeof cs.reason, "%s", mwhy);
        cs.state = release ? CODE_INVALID : CODE_DEVELOPMENT;
    } else if (!stext && release) {
        snprintf(cs.reason, sizeof cs.reason, "unsigned manifest in a release build");
        cs.state = CODE_INVALID;
    } else {
        char err[128];
        CodeManifest *m = calloc(1, sizeof *m);
        if (!m) {
            snprintf(cs.reason, sizeof cs.reason, "out of memory");
            cs.state = CODE_INVALID;
        } else if (codesig_parse(mtext, mlen, stext, slen, pub, m, err, sizeof err)) {
            free(m);
            snprintf(cs.reason, sizeof cs.reason, "%s", err);
            cs.state = CODE_INVALID;
        } else {
            __atomic_store_n(&cs.m, m, __ATOMIC_RELEASE);
            cs.state = m->signed_ok ? CODE_VERIFIED : CODE_DEVELOPMENT;
            if (!m->signed_ok) snprintf(cs.reason, sizeof cs.reason, "unsigned manifest");
        }
    }
    free(mtext);
    free(stext);
done:
    cs.ms += now_ms() - t0;
    fprintf(stderr, "code: %s (%s%s%s) in %.2f ms, %d files listed\n", codesig_state_name(cs.state), cs.root[0] ? cs.root : "-",
            cs.reason[0] ? ": " : "", cs.reason, cs.ms, cur() ? cur()->nfiles : 0);
    if (cs.state == CODE_VERIFIED && !trusted)
        fprintf(stderr, "code: the manifest is not owned by root or others can write it: an update under this session needs a new log in\n");
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
const CodeManifest *codesig_manifest(void) { return cur(); }
unsigned codesig_generation(void) { return __atomic_load_n(&cs.generation, __ATOMIC_ACQUIRE); }
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
    fprintf(stderr, "code: REFUSED %s: %s — this LordOS component was modified and was not loaded\n", rel, why);
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

/* ---- a new manifest under a running session (a package update) ---- */

/* dpkg's order of versions (verrevcmp): digits compare as numbers; between them '~' sorts before
 * everything, even the end, letters before other characters. The manifest's version is the
 * package's (package.json: "0.8.9.4", "0.8.6+v087.21"). <0, 0, >0. */
static int vorder(int c) {
    if (c >= '0' && c <= '9') return 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return c;
    if (c == '~') return -1;
    return c ? c + 256 : 0;
}
int codesig_version_cmp(const char *a, const char *b) {
    while (*a || *b) {
        int first = 0;
        while ((*a && !(*a >= '0' && *a <= '9')) || (*b && !(*b >= '0' && *b <= '9'))) {
            int ac = vorder((unsigned char)*a), bc = vorder((unsigned char)*b);
            if (ac != bc) return ac - bc;
            a++;
            b++;
        }
        while (*a == '0') a++;
        while (*b == '0') b++;
        while (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            if (!first) first = *a - *b;
            a++;
            b++;
        }
        if (*a >= '0' && *a <= '9') return 1;
        if (*b >= '0' && *b <= '9') return -1;
        if (first) return first;
    }
    return 0;
}

/* Reads the manifest pair again when it is not the one in use: only in a verified session, only
 * from the root folder held open since login, only root-owned files nobody else can write, only
 * with a good signature by the same compiled-in key (codesig_parse, as at login), and never an
 * older version. Each pair on disk is read at most once and reads are at least reload_interval_ms
 * apart. 1: a new manifest is in use; 0: nothing to do (unchanged, or not now); -1: refused, the
 * one in use stays (cs.reload_why says why). */
int codesig_reload(void) {
    if (cs.state != CODE_VERIFIED || cs.rootfd < 0 || !cur()) return 0;
    pthread_mutex_lock(&rl_mu);
    int r = 0;
    PairId now;
    pair_id_now(&now);
    double t = now_ms();
    if (pair_id_eq(&now, &cs.id)) { cs.reload_why[0] = 0; goto out; }
    if (pair_id_eq(&now, &cs.tried)) goto out;
    if (cs.last_try > 0 && t - cs.last_try < reload_interval_ms) goto out;
    cs.tried = now;
    cs.last_try = t;
    r = -1;
    CodeManifest *old = cur(), *m = NULL;
    size_t mlen, slen;
    char *mtext, *stext, mwhy[96], err[128];
    PairId id;
    int trusted;
    /* cs.tried stays what the stat saw (a link or a missing file has no read identity): if the pair
     * changed again between the stat and the read, the next call reads it once more */
    read_pair(&mtext, &mlen, &stext, &slen, &id, &trusted, mwhy, sizeof mwhy);
    if (!mtext) snprintf(cs.reload_why, sizeof cs.reload_why, "%s", mwhy);
    else if (!stext) snprintf(cs.reload_why, sizeof cs.reload_why, "unsigned manifest");
    else if (!trusted) snprintf(cs.reload_why, sizeof cs.reload_why, "the manifest is not owned by root or others can write it");
    else if (cs.nretired >= CODE_RELOADS_MAX) snprintf(cs.reload_why, sizeof cs.reload_why, "too many updates in this session");
    else if (!(m = calloc(1, sizeof *m))) snprintf(cs.reload_why, sizeof cs.reload_why, "out of memory");
    else if (codesig_parse(mtext, mlen, stext, slen, cs.pub, m, err, sizeof err)) snprintf(cs.reload_why, sizeof cs.reload_why, "%.90s", err);
    else if (!m->signed_ok) snprintf(cs.reload_why, sizeof cs.reload_why, "unsigned manifest");
    else if (codesig_version_cmp(m->version, old->version) < 0)
        snprintf(cs.reload_why, sizeof cs.reload_why, "version %.20s is older than %.20s", m->version, old->version);
    else {
        cs.retired[cs.nretired++] = old;            /* a caller may still hold it: freed at reset */
        __atomic_store_n(&cs.m, m, __ATOMIC_RELEASE);
        __atomic_add_fetch(&cs.generation, 1, __ATOMIC_ACQ_REL);
        cs.id = id;
        cs.reload_why[0] = 0;
        m = NULL;
        r = 1;
        fprintf(stderr, "code: the signed manifest changed under this session (%s -> %s, %d files): verified and in use\n",
                old->version, cur()->version, cur()->nfiles);
    }
    if (m) { codesig_free(m); free(m); }
    if (r < 0) fprintf(stderr, "code: the manifest changed under this session but was not accepted: %s; the one read before stays\n", cs.reload_why);
    free(mtext);
    free(stext);
out:
    pthread_mutex_unlock(&rl_mu);
    return r;
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
        if (cs.state != CODE_DEVELOPMENT) { snprintf(why, whylen, "not part of LordOS's system code"); refuse(path, why); return NULL; }
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
        if (fd >= 0) { buf = read_fd(fd, max, &n); close(fd); }
        if (!buf) { snprintf(why, whylen, "cannot read it"); return NULL; }
        fprintf(stderr, "code: %s loaded unchecked (outside %s, development build)\n", path, cs.root[0] ? cs.root : "the LordOS folder");
        *len = n;
        return buf;
    }
    buf = read_beneath(rel, max, &n);
    if (!buf) { snprintf(why, whylen, "cannot read it"); return NULL; }   /* missing: not a modification */
    /* a package update replaced the manifest pair: the new one, if it verifies (codesig_reload) */
    int reloaded = codesig_reload() > 0;
    const CodeManifest *m = cur();
    if (!m) {
        /* development build without a manifest: runs, flagged in About */
        add_ms(now_ms() - t0);
        *len = n;
        return buf;
    }
    int bad = codesig_check_buf(m, rel, buf, n, why, whylen);
    if (bad && !reloaded && codesig_reload() > 0) {
        /* the file differs from the manifest in use and a newer signed one just verified: the same
         * bytes are checked once more against it (never read again: what is checked is what runs) */
        m = cur();
        bad = codesig_check_buf(m, rel, buf, n, why, whylen);
    }
    if (bad) {
        pthread_mutex_lock(&rl_mu);
        if (cs.reload_why[0]) {
            size_t k = strlen(why);
            snprintf(why + k, whylen - k, " (the updated manifest was not accepted: %s)", cs.reload_why);
        }
        pthread_mutex_unlock(&rl_mu);
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
