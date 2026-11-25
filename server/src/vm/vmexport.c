/* vmexport.c — Export's manifest, OVF descriptor, .mf and tar writer (see vmexport.h; docs/VIRTUALIZATION.md 3.6
 * and 6.4). No process is started here: lord-vm makes the VMDKs with qemu-img and calls these around it. */
#include "vmexport.h"
#include "../pkg/crypto.h"
#include "../sec/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK (1u << 20)

static void say(char *why, size_t wn, const char *fmt, ...) {
    if (!why || !wn) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, wn, fmt, ap);
    va_end(ap);
}

/* ---- names ---- */

static int tag_ok(const char *s, size_t n) {
    if (n < 2 || n > 32 || s[0] != 's') return 0;
    for (size_t i = 0; i < n; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return 0;
    return 1;
}

int vmx_path_ok(const char *rel) {
    static const char *const fixed[] = {
        "machine.json", "snapshots.json", "cpu-usage.json", "firmware-code.fd",
        "disks/system.qcow2", "disks/system.raw", "disks/nvram.qcow2", "disks/tpm/tpm2-00.permall",
    };
    if (!rel) return 0;
    size_t n = strnlen(rel, VMX_PATH_MAX);
    if (n == 0 || n >= VMX_PATH_MAX) return 0;
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) if (!strcmp(rel, fixed[i])) return 1;
    /* disks/data-N.qcow2 | .raw, N 1..4: the four data disks machine.h allows */
    if (!strncmp(rel, "disks/data-", 11) && rel[11] >= '1' && rel[11] <= '4'
        && (!strcmp(rel + 12, ".qcow2") || !strcmp(rel + 12, ".raw"))) return 1;
    /* disks/tpm/snap-<tag>.permall, the tag as lordvm.c's tpm_snap names it */
    static const char pre[] = "disks/tpm/snap-", suf[] = ".permall";
    size_t pl = sizeof pre - 1, sl = sizeof suf - 1;
    if (n > pl + sl && !strncmp(rel, pre, pl) && !strcmp(rel + n - sl, suf)) return tag_ok(rel + pl, n - pl - sl);
    return 0;
}

int vmx_uuid_ok(const char *s) {
    if (!s || strnlen(s, 37) != 36) return 0;
    for (int i = 0; i < 36; i++) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return 0; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

/* strict UTF-8 (no overlong forms, surrogates or code points past U+10FFFF), no C0, DEL or C1 controls */
int vmx_name_ok(const char *s) {
    if (!s) return 0;
    size_t n = strnlen(s, VMX_NAME_MAX + 1);
    if (!n || n > VMX_NAME_MAX) return 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p;
        int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (k < 0 || (k == 1 && c < 0xc2) || (k == 3 && c > 0xf4)) return 0;
        uint32_t cp = k == 0 ? c : k == 1 ? c & 0x1f : k == 2 ? c & 0x0f : c & 0x07;
        for (int i = 1; i <= k; i++) {
            if ((p[i] & 0xc0) != 0x80) return 0;
            cp = cp << 6 | (p[i] & 0x3f);
        }
        if ((k == 2 && cp < 0x800) || (k == 3 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f)) return 0;
        p += k + 1;
    }
    return 1;
}

int vmx_created_ok(const char *s) {
    static const char pat[] = "dddd-dd-ddTdd:dd:ddZ";
    if (!s || strnlen(s, 21) != 20) return 0;
    for (int i = 0; i < 20; i++) {
        if (pat[i] == 'd') { if (s[i] < '0' || s[i] > '9') return 0; }
        else if (s[i] != pat[i]) return 0;
    }
#define D2(o) ((s[o] - '0') * 10 + (s[o + 1] - '0'))
    int mo = D2(5), d = D2(8), h = D2(11), mi = D2(14), se = D2(17);
#undef D2
    return mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h < 24 && mi < 60 && se < 60;
}

static int hex64_ok(const char *s) {
    if (!s || strnlen(s, 65) != 64) return 0;
    for (int i = 0; i < 64; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

/* ---- opening without following links ---- */

/* rel (already vmx_path_ok) under dirfd, each folder with O_NOFOLLOW|O_DIRECTORY; the file with O_NOFOLLOW and
 * O_NONBLOCK, so a FIFO planted under the name can't hang the open. The descriptor of a regular file, or -1. */
static int open_rel(int dirfd, const char *rel, struct stat *st, char *why, size_t wn) {
    char buf[VMX_PATH_MAX];
    snprintf(buf, sizeof buf, "%s", rel);
    int cur = dirfd;
    char *s = buf;
    for (char *sl; (sl = strchr(s, '/')); s = sl + 1) {
        *sl = 0;
        int d = openat(cur, s, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (cur != dirfd) close(cur);
        if (d < 0) { say(why, wn, "%s: can't open its folder (%s)", rel, strerror(errno)); return -1; }
        cur = d;
    }
    int fd = openat(cur, s, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    int e = errno;
    if (cur != dirfd) close(cur);
    if (fd < 0) { say(why, wn, "%s: can't open it (%s)", rel, strerror(e)); return -1; }
    if (fstat(fd, st) || !S_ISREG(st->st_mode)) { close(fd); say(why, wn, "%s: not a regular file", rel); return -1; }
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    return fd;
}

/* SHA-256 of exactly size bytes from fd, which must then end; progress after each chunk */
static int hash_fd(int fd, uint64_t size, char hex[65], uint8_t *buf, uint64_t *done, uint64_t total,
                   VmxProgress progress, void *pctx) {
    Sha256 h;
    sha256_init(&h);
    uint64_t got = 0;
    for (;;) {
        ssize_t r = read(fd, buf, CHUNK);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        if ((uint64_t)r > size - got) return -1;                 /* longer than it said */
        sha256_update(&h, buf, (size_t)r);
        got += (uint64_t)r;
        *done += (uint64_t)r;
        if (progress) progress(pctx, *done, total);
    }
    if (got != size) return -1;
    uint8_t d[32];
    sha256_final(&h, d);
    hex_encode(d, 32, hex);
    return 0;
}

/* ---- the manifest ---- */

int vmx_manifest_build(int dirfd, const char *const *rel, int n, VmxManifest *m,
                       VmxProgress progress, void *pctx, char *why, size_t wn) {
    if (!m || !rel || n < 1 || n > VMX_MAX_FILES) { say(why, wn, "bad file list"); return -1; }
    int fds[VMX_MAX_FILES];
    uint64_t total = 0, done = 0;
    int k = 0, rc = -1;
    uint8_t *buf = NULL;
    for (; k < n; k++) {
        if (!vmx_path_ok(rel[k])) { say(why, wn, "%s: not a machine's file", rel[k] ? rel[k] : "(null)"); goto out; }
        for (int j = 0; j < k; j++) if (!strcmp(rel[j], rel[k])) { say(why, wn, "%s: listed twice", rel[k]); goto out; }
        struct stat st;
        if ((fds[k] = open_rel(dirfd, rel[k], &st, why, wn)) < 0) goto out;
        if ((uint64_t)st.st_size > VMX_SIZE_MAX) { close(fds[k]); say(why, wn, "%s: too large", rel[k]); goto out; }
        snprintf(m->e[k].path, sizeof m->e[k].path, "%s", rel[k]);
        m->e[k].size = (uint64_t)st.st_size;
        total += m->e[k].size;
    }
    if (!(buf = malloc(CHUNK))) { say(why, wn, "out of memory"); goto out; }
    for (int i = 0; i < n; i++)
        if (hash_fd(fds[i], m->e[i].size, m->e[i].sha256, buf, &done, total, progress, pctx)) {
            say(why, wn, "%s: can't read it, or it changed while read", rel[i]);
            goto out;
        }
    m->n = n;
    rc = 0;
out:
    for (int i = 0; i < k; i++) close(fds[i]);
    free(buf);
    return rc;
}

typedef struct { char *p; size_t n, cap; int bad; } Buf;

static void bput(Buf *b, const char *s, size_t n) {
    if (b->bad) return;
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n + 1) c *= 2;
        char *p = realloc(b->p, c);
        if (!p) { b->bad = 1; return; }
        b->p = p;
        b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

__attribute__((format(printf, 2, 3)))
static void bprintf(Buf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= sizeof tmp) { b->bad = 1; return; }
    bput(b, tmp, (size_t)r);
}

static char *bdone(Buf *b) {
    if (b->bad) { free(b->p); return NULL; }
    return b->p;
}

static void bjson(Buf *b, const char *s) {
    char q[6 * VMX_NAME_MAX + 16];                                /* names and paths are short: always room */
    bput(b, q, json_quote(q, sizeof q, s));
}

char *vmx_manifest_json(const VmxManifest *m) {
    if (!m || !vmx_uuid_ok(m->id) || !vmx_name_ok(m->name) || !vmx_created_ok(m->created)
        || m->n < 1 || m->n > VMX_MAX_FILES) return NULL;
    for (int i = 0; i < m->n; i++)
        if (!vmx_path_ok(m->e[i].path) || m->e[i].size > VMX_SIZE_MAX || !hex64_ok(m->e[i].sha256)) return NULL;
    Buf b = { 0 };
    bprintf(&b, "{\n  \"format\": \"%s\",\n  \"version\": %d,\n  \"id\": \"%s\",\n  \"name\": ", VMX_FORMAT, VMX_VERSION, m->id);
    bjson(&b, m->name);
    bprintf(&b, ",\n  \"created\": \"%s\",\n  \"encrypted\": %s,\n  \"snapshots\": %s,\n  \"files\": [\n",
            m->created, m->encrypted ? "true" : "false", m->snapshots ? "true" : "false");
    for (int i = 0; i < m->n; i++)
        bprintf(&b, "    { \"path\": \"%s\", \"size\": %llu, \"sha256\": \"%s\" }%s\n", m->e[i].path,
                (unsigned long long)m->e[i].size, m->e[i].sha256, i + 1 < m->n ? "," : "");
    bput(&b, "  ]\n}\n", 6);
    return bdone(&b);
}

/* a JSON number that is a whole number 0..max */
static int whole(const JVal *v, double max, uint64_t *out) {
    if (!v || v->type != J_NUM || !(v->num >= 0) || v->num > max) return 0;
    uint64_t u = (uint64_t)v->num;
    if ((double)u != v->num) return 0;
    *out = u;
    return 1;
}

static const char *str_of(const JVal *v) {
    return v && v->type == J_STR && strlen(v->str) == v->len ? v->str : NULL;
}

int vmx_manifest_parse(const char *text, size_t len, VmxManifest *m, char *why, size_t wn) {
    if (!text || !m) { say(why, wn, "no manifest"); return -1; }
    if (len > VMX_MANIFEST_MAX) { say(why, wn, "the manifest is too large"); return -1; }
    memset(m, 0, sizeof *m);
    JDoc doc;
    char err[128];
    if (json_parse(text, len, &doc, err, sizeof err)) { say(why, wn, "the manifest isn't JSON (%s)", err); return -1; }
    int rc = -1, seen = 0;
    const JVal *root = doc.root, *files = NULL;
    if (!root || root->type != J_OBJ) { say(why, wn, "the manifest isn't an object"); goto out; }
    static const char *const keys[] = { "format", "version", "id", "name", "created", "encrypted", "snapshots", "files" };
    for (const JVal *k = root->kid; k; k = k->next) {
        int i = 0;
        while (i < 8 && strcmp(k->key, keys[i])) i++;
        if (i == 8) { say(why, wn, "unknown key in the manifest"); goto out; }
        seen |= 1 << i;
        const char *s = str_of(k);
        uint64_t u;
        switch (i) {
        case 0: if (!s || strcmp(s, VMX_FORMAT)) { say(why, wn, "not a LordOS machine export"); goto out; } break;
        case 1: if (!whole(k, 1e6, &u) || u != VMX_VERSION) { say(why, wn, "an export version this LordOS can't read"); goto out; } break;
        case 2: if (!vmx_uuid_ok(s)) { say(why, wn, "bad machine id"); goto out; } memcpy(m->id, s, 37); break;
        case 3: if (!vmx_name_ok(s)) { say(why, wn, "bad machine name"); goto out; } snprintf(m->name, sizeof m->name, "%s", s); break;
        case 4: if (!vmx_created_ok(s)) { say(why, wn, "bad creation date"); goto out; } memcpy(m->created, s, 21); break;
        case 5: case 6:
            if (k->type != J_BOOL) { say(why, wn, "bad flag %s", keys[i]); goto out; }
            *(i == 5 ? &m->encrypted : &m->snapshots) = k->num != 0;
            break;
        case 7: files = k; break;
        }
    }
    if (seen != 0xff) { say(why, wn, "the manifest misses a key"); goto out; }
    if (files->type != J_ARR || files->len < 1 || files->len > VMX_MAX_FILES) { say(why, wn, "bad file list"); goto out; }
    for (const JVal *f = files->kid; f; f = f->next) {
        if (f->type != J_OBJ) { say(why, wn, "bad file entry"); goto out; }
        VmxEntry *e = &m->e[m->n];
        int fs = 0;
        for (const JVal *k = f->kid; k; k = k->next) {
            const char *s = str_of(k);
            if (!strcmp(k->key, "path")) {
                if (!vmx_path_ok(s)) { say(why, wn, "a file that isn't a machine's"); goto out; }
                for (int j = 0; j < m->n; j++) if (!strcmp(m->e[j].path, s)) { say(why, wn, "%s: listed twice", s); goto out; }
                snprintf(e->path, sizeof e->path, "%s", s);
                fs |= 1;
            } else if (!strcmp(k->key, "size")) {
                if (!whole(k, (double)VMX_SIZE_MAX, &e->size)) { say(why, wn, "bad file size"); goto out; }
                fs |= 2;
            } else if (!strcmp(k->key, "sha256")) {
                if (!hex64_ok(s)) { say(why, wn, "bad SHA-256"); goto out; }
                memcpy(e->sha256, s, 65);
                fs |= 4;
            } else { say(why, wn, "unknown key in a file entry"); goto out; }
        }
        if (fs != 7) { say(why, wn, "a file entry misses a key"); goto out; }
        m->n++;
    }
    rc = 0;
out:
    json_free(&doc);
    if (rc) memset(m, 0, sizeof *m);
    return rc;
}

/* ---- verifying a folder ---- */

static int listed(const VmxManifest *m, const char *rel) {
    for (int i = 0; i < m->n; i++) if (!strcmp(m->e[i].path, rel)) return 1;
    return 0;
}

/* every entry of the folder dfd (at prefix, depth 0 the machine's folder) is a listed regular file, manifest.json at
 * the top, or the folder disks/ (and disks/tpm/ in it); nothing is followed. Consumes dfd. */
static int walk(int dfd, const char *prefix, int depth, const VmxManifest *m, char *why, size_t wn) {
    DIR *d = fdopendir(dfd);
    if (!d) { close(dfd); say(why, wn, "can't list %s", *prefix ? prefix : "the folder"); return -1; }
    int rc = 0;
    struct dirent *de;
    while (!rc && (errno = 0, de = readdir(d))) {
        const char *nm = de->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        char rel[VMX_PATH_MAX];
        if (snprintf(rel, sizeof rel, "%s%s", prefix, nm) >= (int)sizeof rel) { say(why, wn, "an unexpected file in the export"); rc = -1; break; }
        struct stat st;
        if (fstatat(dirfd(d), nm, &st, AT_SYMLINK_NOFOLLOW)) { say(why, wn, "%s: can't read it", rel); rc = -1; break; }
        if (S_ISDIR(st.st_mode) && ((depth == 0 && !strcmp(nm, "disks")) || (depth == 1 && !strcmp(nm, "tpm")))) {
            int sub = openat(dirfd(d), nm, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (sub < 0) { say(why, wn, "%s: can't open it", rel); rc = -1; }
            else rc = walk(sub, depth == 0 ? "disks/" : "disks/tpm/", depth + 1, m, why, wn);
        } else if (S_ISREG(st.st_mode) && ((depth == 0 && !strcmp(nm, "manifest.json")) || listed(m, rel))) {
            continue;
        } else {
            say(why, wn, "%s: not part of the export", rel);
            rc = -1;
        }
    }
    if (!rc && errno) { say(why, wn, "can't list %s", *prefix ? prefix : "the folder"); rc = -1; }
    closedir(d);
    return rc;
}

int vmx_manifest_verify(int dirfd, const VmxManifest *m, int *fds, VmxProgress progress, void *pctx,
                        char *why, size_t wn) {
    if (!m || m->n < 1 || m->n > VMX_MAX_FILES) { say(why, wn, "bad manifest"); return -1; }
    for (int i = 0; i < m->n; i++) {
        if (!vmx_path_ok(m->e[i].path) || m->e[i].size > VMX_SIZE_MAX || !hex64_ok(m->e[i].sha256)) { say(why, wn, "bad manifest"); return -1; }
        for (int j = 0; j < i; j++) if (!strcmp(m->e[i].path, m->e[j].path)) { say(why, wn, "bad manifest"); return -1; }
    }
    /* the walk first: it costs nothing next to hashing, and an extra file means no import whatever the hashes say */
    int top = openat(dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (top < 0) { say(why, wn, "can't open the folder"); return -1; }
    if (walk(top, "", 0, m, why, wn)) return -1;
    int my[VMX_MAX_FILES], k = 0, rc = -1;
    uint64_t total = 0, done = 0;
    uint8_t *buf = NULL;
    for (int i = 0; i < m->n; i++) total += m->e[i].size;
    for (; k < m->n; k++) {
        struct stat st;
        int fd = open_rel(dirfd, m->e[k].path, &st, why, wn);
        if (fd < 0) goto out;
        /* a hard link could make a file outside the stick (or a user's own key) look like a part of the export */
        const char *bad = st.st_nlink != 1 ? "has other links" : (uint64_t)st.st_size != m->e[k].size ? "wrong size" : NULL;
        if (bad) { close(fd); say(why, wn, "%s: %s", m->e[k].path, bad); goto out; }
        my[k] = fd;
    }
    if (!(buf = malloc(CHUNK))) { say(why, wn, "out of memory"); goto out; }
    for (int i = 0; i < m->n; i++) {
        char hex[65];
        if (hash_fd(my[i], m->e[i].size, hex, buf, &done, total, progress, pctx)) { say(why, wn, "%s: can't read it, or it changed", m->e[i].path); goto out; }
        if (strcmp(hex, m->e[i].sha256)) { say(why, wn, "%s: its checksum doesn't match", m->e[i].path); goto out; }
        if (fds && lseek(my[i], 0, SEEK_SET) != 0) { say(why, wn, "%s: can't rewind it", m->e[i].path); goto out; }
    }
    rc = 0;
out:
    free(buf);
    for (int i = 0; i < k; i++) {
        if (!rc && fds) fds[i] = my[i];
        else close(my[i]);
    }
    return rc;
}

/* ---- OVF ---- */

/* a plain file name in the OVA: no folders, nothing that needs escaping in XML or in the .mf */
static int plain_name(const char *s) {
    size_t n = s ? strnlen(s, 100) : 0;
    if (!n || n >= 100 || s[0] == '.') return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9')
              || s[i] == '.' || s[i] == '_' || s[i] == '-')) return 0;
    return 1;
}

/* CIM_OperatingSystem OsType values (CIM 2.x ValueMap; VirtualBox's ovfreader.h CIMOSType_T has the same numbers):
 * 96 Debian 64-Bit, 101 Linux 64-Bit, 102 Other 64-Bit, 120 Windows 10 64-Bit (the newest Windows the list has; a
 * Windows 11 guest is imported as 10, its hardware is what counts) */
int vmx_ovf_os_id(const char *os) {
    if (!os) return 102;
    if (!strcmp(os, "linux")) return 101;
    if (!strcmp(os, "debian")) return 96;
    if (!strcmp(os, "windows")) return 120;
    return 102;
}

static const char *os_text(int id) {
    return id == 101 ? "Linux 64-Bit" : id == 96 ? "Debian 64-Bit" : id == 120 ? "Microsoft Windows 10 64-Bit" : "Other 64-Bit";
}

static void bxml(Buf *b, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '&': bput(b, "&amp;", 5); break;
        case '<': bput(b, "&lt;", 4); break;
        case '>': bput(b, "&gt;", 4); break;
        case '"': bput(b, "&quot;", 6); break;
        case '\'': bput(b, "&apos;", 6); break;
        default: bput(b, s, 1);
        }
    }
}

char *vmx_ovf(const VmxOvfInfo *in) {
    if (!in || !vmx_name_ok(in->name) || in->vcpus < 1 || in->vcpus > 256 || in->memory_mb < 1
        || in->memory_mb > (4ull << 20) || in->ndisks < 1 || in->ndisks > VMX_OVF_DISKS) return NULL;
    for (int i = 0; i < in->ndisks; i++) {
        const VmxOvfDisk *d = &in->disk[i];
        if (!plain_name(d->file) || !d->capacity || d->capacity > VMX_SIZE_MAX || d->populated > VMX_SIZE_MAX
            || !d->file_size || d->file_size > VMX_SIZE_MAX) return NULL;
        for (int j = 0; j < i; j++) if (!strcmp(d->file, in->disk[j].file)) return NULL;
    }
    int os = vmx_ovf_os_id(in->os);
    Buf b = { 0 };
    /* the namespaces as VirtualBox and VMware both write them (DSP0243 1.0.0; vmw: only for the firmware hint) */
    static const char head[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Envelope ovf:version=\"1.0\" xml:lang=\"en-US\" xmlns=\"http://schemas.dmtf.org/ovf/envelope/1\""
        " xmlns:ovf=\"http://schemas.dmtf.org/ovf/envelope/1\""
        " xmlns:rasd=\"http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_ResourceAllocationSettingData\""
        " xmlns:vssd=\"http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_VirtualSystemSettingData\""
        " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""
        " xmlns:vmw=\"http://www.vmware.com/schema/ovf\">\n  <References>\n";
    bput(&b, head, sizeof head - 1);
    for (int i = 0; i < in->ndisks; i++)
        bprintf(&b, "    <File ovf:href=\"%s\" ovf:id=\"file%d\" ovf:size=\"%llu\"/>\n", in->disk[i].file, i + 1,
                (unsigned long long)in->disk[i].file_size);
    bprintf(&b, "  </References>\n  <DiskSection>\n    <Info>Virtual disk information</Info>\n");
    for (int i = 0; i < in->ndisks; i++) {
        bprintf(&b, "    <Disk ovf:capacity=\"%llu\" ovf:capacityAllocationUnits=\"byte\" ovf:diskId=\"vmdisk%d\""
                    " ovf:fileRef=\"file%d\" ovf:format=\"http://www.vmware.com/interfaces/specifications/vmdk.html#streamOptimized\"",
                (unsigned long long)in->disk[i].capacity, i + 1, i + 1);
        if (in->disk[i].populated) bprintf(&b, " ovf:populatedSize=\"%llu\"", (unsigned long long)in->disk[i].populated);
        bprintf(&b, "/>\n");
    }
    bprintf(&b, "  </DiskSection>\n  <NetworkSection>\n    <Info>The list of logical networks</Info>\n"
                "    <Network ovf:name=\"NAT\">\n      <Description>The NAT network</Description>\n    </Network>\n"
                "  </NetworkSection>\n  <VirtualSystem ovf:id=\"");
    bxml(&b, in->name);
    bprintf(&b, "\">\n    <Info>A virtual machine</Info>\n    <Name>");
    bxml(&b, in->name);
    bprintf(&b, "</Name>\n    <OperatingSystemSection ovf:id=\"%d\">\n      <Info>The kind of installed guest operating system</Info>\n"
                "      <Description>%s</Description>\n    </OperatingSystemSection>\n", os, os_text(os));
    /* "vmx-10" (VMware hardware 10: Workstation 10, ESXi 5.5) is the first VMware family with a SATA controller,
     * which every guest has a driver for; VMware refuses a "virtualbox-*" family without --lax, and VirtualBox
     * imports "vmx-*" families. The rasd elements are in the schema's (alphabetical) order, which ovftool checks. */
    bprintf(&b, "    <VirtualHardwareSection>\n      <Info>Virtual hardware requirements</Info>\n      <System>\n"
                "        <vssd:ElementName>Virtual Hardware Family</vssd:ElementName>\n"
                "        <vssd:InstanceID>0</vssd:InstanceID>\n        <vssd:VirtualSystemIdentifier>");
    bxml(&b, in->name);
    bprintf(&b, "</vssd:VirtualSystemIdentifier>\n        <vssd:VirtualSystemType>vmx-10</vssd:VirtualSystemType>\n"
                "      </System>\n");
    bprintf(&b, "      <Item>\n        <rasd:AllocationUnits>hertz * 10^6</rasd:AllocationUnits>\n"
                "        <rasd:Description>Number of virtual CPUs</rasd:Description>\n"
                "        <rasd:ElementName>%d virtual CPU(s)</rasd:ElementName>\n        <rasd:InstanceID>1</rasd:InstanceID>\n"
                "        <rasd:ResourceType>3</rasd:ResourceType>\n        <rasd:VirtualQuantity>%d</rasd:VirtualQuantity>\n"
                "      </Item>\n", in->vcpus, in->vcpus);
    bprintf(&b, "      <Item>\n        <rasd:AllocationUnits>byte * 2^20</rasd:AllocationUnits>\n"
                "        <rasd:Description>Memory size</rasd:Description>\n"
                "        <rasd:ElementName>%llu MB of memory</rasd:ElementName>\n        <rasd:InstanceID>2</rasd:InstanceID>\n"
                "        <rasd:ResourceType>4</rasd:ResourceType>\n        <rasd:VirtualQuantity>%llu</rasd:VirtualQuantity>\n"
                "      </Item>\n", (unsigned long long)in->memory_mb, (unsigned long long)in->memory_mb);
    bprintf(&b, "      <Item>\n        <rasd:Address>0</rasd:Address>\n        <rasd:Description>SATA Controller</rasd:Description>\n"
                "        <rasd:ElementName>sataController0</rasd:ElementName>\n        <rasd:InstanceID>3</rasd:InstanceID>\n"
                "        <rasd:ResourceSubType>AHCI</rasd:ResourceSubType>\n        <rasd:ResourceType>20</rasd:ResourceType>\n"
                "      </Item>\n");
    for (int i = 0; i < in->ndisks; i++)
        bprintf(&b, "      <Item>\n        <rasd:AddressOnParent>%d</rasd:AddressOnParent>\n"
                    "        <rasd:ElementName>disk%d</rasd:ElementName>\n        <rasd:HostResource>ovf:/disk/vmdisk%d</rasd:HostResource>\n"
                    "        <rasd:InstanceID>%d</rasd:InstanceID>\n        <rasd:Parent>3</rasd:Parent>\n"
                    "        <rasd:ResourceType>17</rasd:ResourceType>\n      </Item>\n", i, i + 1, i + 1, 4 + i);
    bprintf(&b, "      <Item>\n        <rasd:AutomaticAllocation>true</rasd:AutomaticAllocation>\n"
                "        <rasd:Connection>NAT</rasd:Connection>\n        <rasd:Description>E1000 ethernet adapter on \"NAT\"</rasd:Description>\n"
                "        <rasd:ElementName>Ethernet adapter 1</rasd:ElementName>\n        <rasd:InstanceID>%d</rasd:InstanceID>\n"
                "        <rasd:ResourceSubType>E1000</rasd:ResourceSubType>\n        <rasd:ResourceType>10</rasd:ResourceType>\n"
                "      </Item>\n", 4 + in->ndisks);
    /* OVF 1.0 has no firmware setting: VMware's own extension, optional, so a reader that doesn't know it goes on */
    if (in->efi) bprintf(&b, "      <vmw:Config ovf:required=\"false\" vmw:key=\"firmware\" vmw:value=\"efi\"/>\n");
    bprintf(&b, "    </VirtualHardwareSection>\n  </VirtualSystem>\n</Envelope>\n");
    return bdone(&b);
}

char *vmx_mf(const char *const *names, const char *const *hex, int n) {
    if (!names || !hex || n < 1 || n > VMX_MAX_FILES) return NULL;
    Buf b = { 0 };
    for (int i = 0; i < n; i++) {
        if (!plain_name(names[i]) || !hex64_ok(hex[i])) { free(b.p); return NULL; }
        bprintf(&b, "SHA256(%s)= %s\n", names[i], hex[i]);
    }
    return bdone(&b);
}

/* ---- tar ---- */

#define TAR_NAME_MAX 1024
#define TAR_OCTAL11  077777777777LL           /* the most 11 octal digits hold: 8 GiB - 1 */

static int write_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) { errno = EIO; return -1; }
        c += w;
        n -= (size_t)w;
    }
    return 0;
}

static int tar_name_ok(const char *s) {
    size_t n = s ? strnlen(s, TAR_NAME_MAX + 1) : 0;
    if (!n || n > TAR_NAME_MAX || s[0] == '/') return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) return 0;
    for (const char *c = s;;) {                                   /* each component: not empty, not "." or ".." */
        const char *e = strchr(c, '/');
        size_t l = e ? (size_t)(e - c) : strlen(c);
        if (!l || (l == 1 && c[0] == '.') || (l == 2 && c[0] == '.' && c[1] == '.')) return 0;
        if (!e) return 1;
        c = e + 1;
    }
}

static void octal(char *f, size_t w, unsigned long long v) {     /* w-1 digits and a NUL */
    snprintf(f, w, "%0*llo", (int)(w - 1), v);
}

/* one ustar header block for a member of type t */
static void header(uint8_t h[512], const char *name, long long size, long long mtime, char t) {
    memset(h, 0, 512);
    size_t n = strlen(name);
    if (n > 99) {                                                 /* the pax 'path' has the whole name; cut at a UTF-8 start */
        n = 99;
        while (n && ((unsigned char)name[n] & 0xc0) == 0x80) n--;
    }
    memcpy(h, name, n);
    octal((char *)h + 100, 8, 0644);
    octal((char *)h + 108, 8, 0);
    octal((char *)h + 116, 8, 0);
    if (size <= TAR_OCTAL11) octal((char *)h + 124, 12, (unsigned long long)size);
    else {                                                        /* GNU base-256: the high bit, then big-endian */
        unsigned long long v = (unsigned long long)size;
        for (int i = 11; i >= 1; i--) { h[124 + i] = (uint8_t)v; v >>= 8; }
        h[124] = 0x80;
    }
    octal((char *)h + 136, 12, (unsigned long long)(mtime < 0 ? 0 : mtime > TAR_OCTAL11 ? TAR_OCTAL11 : mtime));
    memset(h + 148, ' ', 8);
    h[156] = (uint8_t)t;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);                   /* six digits, NUL, and the space already there */
}

/* "<len> key=value\n", len counting itself */
static int pax_record(char *out, size_t cap, const char *key, const char *val) {
    size_t body = strlen(key) + strlen(val) + 3, len = body + 1;
    for (;;) {
        char d[24];
        size_t dl = (size_t)snprintf(d, sizeof d, "%zu", len);
        if (body + dl == len) break;
        len = body + dl;
    }
    if (len >= cap) return -1;
    snprintf(out, cap, "%zu %s=%s\n", len, key, val);
    return (int)len;
}

static int pad(int out, unsigned long long n) {
    static const uint8_t z[512];
    size_t r = (size_t)(n % 512);
    return r ? write_all(out, z, 512 - r) : 0;
}

static int member_header(int out, const char *name, long long size, long long mtime) {
    if (!tar_name_ok(name) || size < 0 || size > (1LL << 62)) { errno = EINVAL; return -1; }
    size_t nl = strlen(name);
    if (nl > 99 || size > TAR_OCTAL11) {
        char x[TAR_NAME_MAX + 64], v[24];
        int xl = 0, r;
        if (nl > 99) { if ((r = pax_record(x, sizeof x, "path", name)) < 0) { errno = EINVAL; return -1; } xl += r; }
        if (size > TAR_OCTAL11) {
            snprintf(v, sizeof v, "%lld", size);
            if ((r = pax_record(x + xl, sizeof x - (size_t)xl, "size", v)) < 0) { errno = EINVAL; return -1; }
            xl += r;
        }
        /* the pax header's own name, as GNU tar makes it: PaxHeaders/ and the member's last component */
        const char *base = strrchr(name, '/');
        base = base ? base + 1 : name;
        char pn[128];
        snprintf(pn, sizeof pn, "PaxHeaders/%s", base);
        uint8_t h[512];
        header(h, pn, xl, mtime, 'x');
        if (write_all(out, h, 512) || write_all(out, x, (size_t)xl) || pad(out, (unsigned long long)xl)) return -1;
    }
    uint8_t h[512];
    header(h, name, size, mtime, '0');
    return write_all(out, h, 512);
}

static int all_zero(const uint8_t *p, size_t n) {          /* n <= 65536; memcmp is many times a byte loop */
    static const uint8_t z[65536];
    return !memcmp(p, z, n);
}

int vmx_tar_file(int out, const char *name, int in, long long size, long long mtime) {
    if (member_header(out, name, size, mtime)) return -1;
    /* holes only into a regular file written at its end, not appending: a skipped run must read back as zeros */
    struct stat st;
    off_t pos = lseek(out, 0, SEEK_CUR);
    int fl = fcntl(out, F_GETFL);
    int holes = pos >= 0 && fl >= 0 && !(fl & O_APPEND) && !fstat(out, &st) && S_ISREG(st.st_mode) && st.st_size <= pos;
    uint8_t *buf = malloc(CHUNK);
    if (!buf) { errno = ENOMEM; return -1; }
    long long left = size;
    int skipped = 0, rc = -1;
    while (left > 0) {
        size_t want = left < (long long)CHUNK ? (size_t)left : CHUNK, got = 0;
        while (got < want) {
            ssize_t r = read(in, buf + got, want - got);
            if (r < 0) { if (errno == EINTR) continue; goto out; }
            if (r == 0) { errno = ENODATA; goto out; }           /* shorter than it said */
            got += (size_t)r;
        }
        for (size_t o = 0; o < got; o += 65536) {
            size_t l = got - o < 65536 ? got - o : 65536;
            if (holes && all_zero(buf + o, l)) {
                if (lseek(out, (off_t)l, SEEK_CUR) < 0) goto out;
                skipped = 1;
            } else {
                if (write_all(out, buf + o, l)) goto out;
                skipped = 0;
            }
        }
        left -= (long long)got;
    }
    if (skipped) {                                                /* a hole at the very end: give the file its length */
        off_t end = lseek(out, 0, SEEK_CUR);
        if (end < 0 || fstat(out, &st) || (st.st_size < end && ftruncate(out, end))) goto out;
    }
    rc = pad(out, (unsigned long long)size);
out:
    free(buf);
    return rc;
}

int vmx_tar_bytes(int out, const char *name, const void *p, size_t n, long long mtime) {
    if (!p && n) { errno = EINVAL; return -1; }
    if ((unsigned long long)n > (1ULL << 62)) { errno = EINVAL; return -1; }
    if (member_header(out, name, (long long)n, mtime)) return -1;
    if (n && write_all(out, p, n)) return -1;
    return pad(out, n);
}

int vmx_tar_end(int out) {
    static const uint8_t z[1024];
    return write_all(out, z, sizeof z);
}
