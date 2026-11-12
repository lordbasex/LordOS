/* h_arc.c — archives in the helper (libarchive): list, extract and create .zip.
 *
 * Extracting never leaves the destination folder (fd "dir"), with three layers:
 *   1. arcpath.c cleans each name (no "..", no absolute paths) and only accepts relative symbolic
 *      links that stay inside; no devices, FIFOs or setuid are created;
 *   2. each folder is opened from the destination fd component by component with O_NOFOLLOW (a link
 *      of the archive itself can't be used to escape) and files with O_EXCL (no overwriting);
 *   3. Landlock: the process can only write below that folder.
 * Against bombs: caps on entries, on bytes written and on free disk space. */
#include "helper.h"
#include "arcpath.h"

#include <archive.h>
#include <archive_entry.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define ARC_MAX_ENTRIES 500000
#define ARC_LIST_MAX    100000
#define ARC_MAX_BYTES   (64ULL << 30)
#define ARC_MIN_FREE    (128ULL << 20)

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* Split archives (name.7z.001, name.zip.001…, name.z01…name.zip, name.part1.rar…): lord-server opens
 * every volume and sends them in order; libarchive reads them as one archive through its own
 * multi-volume support (one client data per volume, switched at each end). Only read and lseek on
 * fds it already has: nothing new for the sandbox. */
#define ARC_MAX_VOLS 255

typedef struct { int fd; char buf[1 << 16]; } Vol;
typedef struct { struct archive *a; Vol *v; int n; } Reader;

static la_ssize_t vol_read(struct archive *a, void *cd, const void **buf) {
    (void)a;
    Vol *v = cd;
    for (;;) {
        ssize_t r = read(v->fd, v->buf, sizeof v->buf);
        if (r < 0 && errno == EINTR) continue;
        *buf = v->buf;
        return r;
    }
}

static la_int64_t vol_seek(struct archive *a, void *cd, la_int64_t off, int whence) {
    (void)a;
    off_t r = lseek(((Vol *)cd)->fd, (off_t)off, whence);
    return r < 0 ? ARCHIVE_FATAL : (la_int64_t)r;
}

static la_int64_t vol_skip(struct archive *a, void *cd, la_int64_t req) {
    (void)a;
    Vol *v = cd;
    off_t cur = lseek(v->fd, 0, SEEK_CUR);
    if (cur < 0 || req <= 0) return 0;
    off_t end = lseek(v->fd, 0, SEEK_END);
    if (end < 0) return 0;
    off_t to = cur + (off_t)req > end ? end : cur + (off_t)req;
    return lseek(v->fd, to, SEEK_SET) < 0 ? 0 : (la_int64_t)(to - cur);
}

static int vol_switch(struct archive *a, void *from, void *to) {
    (void)a; (void)from;
    return to && lseek(((Vol *)to)->fd, 0, SEEK_SET) < 0 ? ARCHIVE_FATAL : ARCHIVE_OK;
}

/* The archive's fds, in order: "vols" (the slots of every volume, only "file" ones) or the single "slot". */
static int arc_fds(const JVal *args, int *fds) {
    const JVal *vols = json_get(args, "vols");
    if (!vols) { fds[0] = slot_fd(args, "slot"); return fds[0] >= 0 ? 1 : 0; }
    if (vols->type != J_ARR) return 0;
    int n = 0;
    for (const JVal *k = vols->kid; k; k = k->next) {
        if (n >= ARC_MAX_VOLS || k->type != J_NUM) return 0;
        double d = k->num;
        if (d < 0 || d >= hnslots || d != (double)(int)d) return 0;
        int fd = hslots[(int)d].fd;
        if (fd < 0 || strcmp(hslots[(int)d].role, "file")) return 0;
        fds[n++] = fd;
    }
    return n;
}

static off_t arc_size(const int *fds, int n) {
    off_t total = 0;
    struct stat st;
    for (int i = 0; i < n; i++) if (!fstat(fds[i], &st)) total += st.st_size;
    return total;
}

static void close_reader(Reader *r) {
    if (r->a) archive_read_free(r->a);
    free(r->v);
    r->a = NULL;
    r->v = NULL;
}

static int open_reader(Reader *r, const int *fds, int n) {
    r->a = NULL;
    r->n = n;
    r->v = n > 0 ? calloc((size_t)n, sizeof *r->v) : NULL;
    if (!r->v) return -1;
    for (int i = 0; i < n; i++) {
        r->v[i].fd = fds[i];
        if (lseek(fds[i], 0, SEEK_SET) < 0) { close_reader(r); return -1; }
    }
    struct archive *a = r->a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (n == 1) archive_read_support_format_raw(a);   /* a bare .gz/.xz/.zst: a single file */
    archive_read_set_read_callback(a, vol_read);
    archive_read_set_seek_callback(a, vol_seek);
    archive_read_set_skip_callback(a, vol_skip);
    archive_read_set_switch_callback(a, vol_switch);
    int bad = archive_read_set_callback_data(a, &r->v[0]) != ARCHIVE_OK;
    for (int i = 1; i < n && !bad; i++) bad = archive_read_append_callback_data(a, &r->v[i]) != ARCHIVE_OK;
    if (bad || archive_read_open1(a) != ARCHIVE_OK) { close_reader(r); return -1; }
    return 0;
}

/* "." or "./": the archive's root folder (tar -C x .); not a problem, ignored */
static int is_root(const char *n) {
    if (!n) return 0;
    for (; *n; n++) if (*n != '.' && *n != '/') return 0;
    return 1;
}

/* "raw" without compression = any file: not an archive */
static int raw_plain(struct archive *a) {
    return archive_format(a) == ARCHIVE_FORMAT_RAW && archive_filter_code(a, 0) == ARCHIVE_FILTER_NONE;
}

static const char *entry_name(struct archive_entry *e) {
    const char *n = archive_entry_pathname_utf8(e);
    return n ? n : archive_entry_pathname(e);
}

static void op_list(uint32_t id, const JVal *args) {
    int fds[ARC_MAX_VOLS];
    Reader rd;
    int nf = arc_fds(args, fds);
    if (!nf || open_reader(&rd, fds, nf)) { reply_err(id, "invalid", ""); return; }
    struct archive *a = rd.a;
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"entries\":[");
    struct archive_entry *e;
    int n = 0, truncated = 0, r, encrypted = 0;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        if (raw_plain(a)) { r = ARCHIVE_FATAL; n = 0; break; }
        if (n >= ARC_LIST_MAX) { truncated = 1; break; }
        const char *name = entry_name(e);
        char clean[ARC_PATH_MAX + 1];
        if (is_root(name)) { archive_read_data_skip(a); continue; }
        int bad = !name || arc_clean_path(name, clean, sizeof clean);
        if (n++) jb_raw(&b, ",");
        jb_raw(&b, "{\"p\":");
        jb_str(&b, bad ? (name ? name : "") : clean);
        unsigned type = archive_entry_filetype(e);
        jb_printf(&b, ",\"s\":%lld,\"m\":%lld", (long long)archive_entry_size(e), (long long)archive_entry_mtime(e) * 1000);
        if (type == AE_IFDIR) jb_raw(&b, ",\"d\":true");
        if (type == AE_IFLNK) { jb_raw(&b, ",\"l\":"); jb_str(&b, archive_entry_symlink(e)); }
        if (archive_entry_is_encrypted(e)) { jb_raw(&b, ",\"enc\":true"); encrypted = 1; }
        if (bad) jb_raw(&b, ",\"bad\":true");
        jb_raw(&b, "}");
        archive_read_data_skip(a);
    }
    int fatal = r == ARCHIVE_FATAL && n == 0;
    const char *fmt = archive_format_name(a);
    jb_raw(&b, "],\"format\":");
    jb_str(&b, fmt ? fmt : "");
    jb_printf(&b, ",\"raw\":%s,\"truncated\":%s,\"encrypted\":%s}", archive_format(a) == ARCHIVE_FORMAT_RAW ? "true" : "false",
              truncated ? "true" : "false", encrypted ? "true" : "false");
    close_reader(&rd);
    if (fatal) { jb_free(&b); reply_err(id, "invalid", ""); return; }
    reply_json(DR_DONE, id, &b);
}

/* Opens the folder that contains `path` (clean) from root, component by component, without following links;
 * with create, creates the missing ones. Returns the fd and *leaf points to the last component. */
static int open_parent(int root, char *path, int create, const char **leaf) {
    int cur = dup(root);
    char *p = path;
    for (;;) {
        char *slash = strchr(p, '/');
        if (!slash) { *leaf = p; return cur; }
        *slash = 0;
        int next = openat(cur, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            if (mkdirat(cur, p, 0755) && errno != EEXIST) { *slash = '/'; close(cur); return -1; }
            next = openat(cur, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        *slash = '/';
        close(cur);
        if (next < 0) return -1;
        cur = next;
        p = slash + 1;
    }
}

typedef struct {
    uint32_t id;
    int root;
    off_t arcsize;
    long files, skipped;
    unsigned long long bytes;
    double last;
    JBuf skips;
    const char *err;
} Ex;

static void skip(Ex *x, const char *name, const char *why) {
    x->skipped++;
    if (x->skipped > 200) return;
    if (x->skips.n) jb_raw(&x->skips, ",");
    jb_raw(&x->skips, "{\"p\":");
    jb_str(&x->skips, name);
    jb_raw(&x->skips, ",\"why\":");
    jb_str(&x->skips, why);
    jb_raw(&x->skips, "}");
}

static void progress(Ex *x, struct archive *a, const char *name, int force) {
    double t = now_ms();
    if (!force && t - x->last < 120) return;
    x->last = t;
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"files\":%ld,\"bytes\":%llu,\"pos\":%lld,\"size\":%lld,\"name\":", x->files, x->bytes,
              (long long)archive_filter_bytes(a, -1), (long long)x->arcsize);
    jb_str(&b, name);
    jb_raw(&b, "}");
    reply_json(DR_PROG, x->id, &b);
}

static int room(Ex *x) {
    if (x->bytes > ARC_MAX_BYTES) { x->err = "too-big"; return 0; }
    struct statvfs sv;
    if (!fstatvfs(x->root, &sv) && (unsigned long long)sv.f_bavail * sv.f_frsize < ARC_MIN_FREE) { x->err = "disk-full"; return 0; }
    return 1;
}

static int write_file(Ex *x, struct archive *a, struct archive_entry *e, int dir, const char *leaf, const char *name) {
    int fd = openat(dir, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { skip(x, name, errno == EEXIST ? "exists" : "open"); return 0; }
    const void *buf;
    size_t size;
    la_int64_t off;
    int r, ok = 1;
    unsigned long long since = 0;
    while ((r = archive_read_data_block(a, &buf, &size, &off)) == ARCHIVE_OK) {
        if (off < 0) { ok = 0; break; }
        const char *p = buf;
        size_t done = 0;
        while (done < size) {
            ssize_t w = pwrite(fd, p + done, size - done, (off_t)off + (off_t)done);
            if (w <= 0) { if (w < 0 && errno == EINTR) continue; ok = 0; x->err = errno == ENOSPC ? "disk-full" : "write"; break; }
            done += (size_t)w;
        }
        if (!ok) break;
        x->bytes += size;
        since += size;
        if (since > (32u << 20)) { since = 0; if (!room(x)) { ok = 0; break; } }
        progress(x, a, name, 0);
    }
    if (r == ARCHIVE_FATAL || r == ARCHIVE_FAILED) { ok = 0; if (!x->err) x->err = "corrupt"; }
    /* final size (the holes at the end of a sparse file) */
    la_int64_t want = archive_entry_size_is_set(e) ? archive_entry_size(e) : -1;
    if (ok && want > 0) { struct stat st; if (!fstat(fd, &st) && st.st_size < want && ftruncate(fd, want)) ok = 0; }
    fchmod(fd, arc_file_mode(archive_entry_perm(e)));
    struct timespec ts[2] = { { archive_entry_mtime(e), 0 }, { archive_entry_mtime(e), 0 } };
    futimens(fd, ts);
    close(fd);
    if (!ok) { unlinkat(dir, leaf, 0); return x->err ? -1 : 0; }
    x->files++;
    return 0;
}

static void op_extract(uint32_t id, const JVal *args) {
    int fds[ARC_MAX_VOLS], root = slot_fd(args, "dir");
    Reader rd;
    int nf = arc_fds(args, fds);
    if (!nf || root < 0 || open_reader(&rd, fds, nf)) { reply_err(id, "invalid", ""); return; }
    struct archive *a = rd.a;
    Ex x = { .id = id, .root = root, .arcsize = arc_size(fds, nf) };
    jb_init(&x.skips);
    const JVal *only = json_get(args, "only");
    const char *rawname = json_str(args, "rawname");
    char rawclean[256] = "data";
    if (rawname && strlen(rawname) < sizeof rawclean && !strchr(rawname, '/') && strcmp(rawname, "..") && strcmp(rawname, ".") && rawname[0])
        snprintf(rawclean, sizeof rawclean, "%s", rawname);
    struct archive_entry *e;
    int r, n = 0;
    while (!x.err && ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN)) {
        if (raw_plain(a)) { x.err = "invalid"; break; }
        if (++n > ARC_MAX_ENTRIES) { x.err = "too-many"; break; }
        const char *name = entry_name(e);
        char clean[ARC_PATH_MAX + 1];
        if (archive_format(a) == ARCHIVE_FORMAT_RAW) name = rawclean;
        if (is_root(name)) continue;
        if (!name || arc_clean_path(name, clean, sizeof clean)) { skip(&x, name ? name : "", "unsafe-path"); continue; }
        if (only && only->type == J_ARR) {
            int hit = 0;
            for (const JVal *o = only->kid; o && !hit; o = o->next) if (o->type == J_STR && arc_under(clean, o->str)) hit = 1;
            if (!hit) continue;
        }
        if (archive_entry_is_encrypted(e)) { skip(&x, clean, "encrypted"); continue; }
        unsigned type = archive_entry_filetype(e);
        const char *hard = archive_entry_hardlink(e);
        const char *leaf;
        char work[ARC_PATH_MAX + 1];
        memcpy(work, clean, strlen(clean) + 1);
        if (type == AE_IFDIR) {
            /* the folder itself: open_parent creates the ones above; this one is created here */
            int d = open_parent(root, work, 1, &leaf);
            if (d < 0) { skip(&x, clean, "unsafe-path"); continue; }
            if (mkdirat(d, leaf, arc_dir_mode(archive_entry_perm(e))) && errno != EEXIST) skip(&x, clean, "mkdir");
            close(d);
            continue;
        }
        int d = open_parent(root, work, 1, &leaf);
        if (d < 0) { skip(&x, clean, "unsafe-path"); continue; }
        if (hard) {
            char tclean[ARC_PATH_MAX + 1], twork[ARC_PATH_MAX + 1];
            const char *tleaf;
            if (arc_clean_path(hard, tclean, sizeof tclean)) { skip(&x, clean, "unsafe-link"); close(d); continue; }
            memcpy(twork, tclean, strlen(tclean) + 1);
            int td = open_parent(root, twork, 0, &tleaf);
            if (td < 0 || linkat(td, tleaf, d, leaf, 0)) skip(&x, clean, "link");
            else x.files++;
            if (td >= 0) close(td);
        } else if (type == AE_IFREG) {
            write_file(&x, a, e, d, leaf, clean);
        } else if (type == AE_IFLNK) {
            const char *target = archive_entry_symlink(e);
            if (!target || !arc_link_ok(clean, target)) skip(&x, clean, "unsafe-link");
            else if (symlinkat(target, d, leaf)) skip(&x, clean, errno == EEXIST ? "exists" : "link");
            else x.files++;
        } else skip(&x, clean, "special");
        close(d);
        progress(&x, a, clean, 0);
    }
    if (!x.err && r == ARCHIVE_FATAL) x.err = n ? "corrupt" : "invalid";
    progress(&x, a, "", 1);
    close_reader(&rd);
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"files\":%ld,\"bytes\":%llu,\"skipped\":%ld,\"skips\":[", x.files, x.bytes, x.skipped);
    if (x.skips.p) jb_raw(&b, x.skips.p);
    jb_raw(&b, "]");
    if (x.err) { jb_raw(&b, ",\"error\":"); jb_str(&b, x.err); }
    jb_raw(&b, "}");
    jb_free(&x.skips);
    reply_json(x.err ? DR_ERR : DR_DONE, id, &b);
}

/* ---- create .zip ---- */

typedef struct { uint32_t id; struct archive *w; long files; unsigned long long bytes; double last; int fail; } Cr;

static void cr_progress(Cr *c, const char *name, int force) {
    double t = now_ms();
    if (!force && t - c->last < 120) return;
    c->last = t;
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"files\":%ld,\"bytes\":%llu,\"name\":", c->files, c->bytes);
    jb_str(&b, name);
    jb_raw(&b, "}");
    reply_json(DR_PROG, c->id, &b);
}

static void add_fd(Cr *c, int fd, const char *name, int depth);

static void add_entry(Cr *c, int fd, const char *name, const struct stat *st, int depth) {
    struct archive_entry *e = archive_entry_new();
    archive_entry_set_pathname_utf8(e, name);
    archive_entry_set_mtime(e, st->st_mtime, 0);
    if (S_ISDIR(st->st_mode)) {
        archive_entry_set_filetype(e, AE_IFDIR);
        archive_entry_set_perm(e, st->st_mode & 0755);
        if (archive_write_header(c->w, e) != ARCHIVE_OK) c->fail = 1;
        archive_entry_free(e);
        if (depth > 64) return;
        int dd = dup(fd);
        DIR *d = dd >= 0 ? fdopendir(dd) : NULL;
        if (!d) { if (dd >= 0) close(dd); return; }
        struct dirent *de;
        while (!c->fail && (de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            int k = openat(fd, de->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            if (k < 0 && errno == ELOOP) {
                /* symbolic link: stored as a link */
                char target[4096];
                ssize_t tl = readlinkat(fd, de->d_name, target, sizeof target - 1);
                if (tl > 0) {
                    target[tl] = 0;
                    char full[ARC_PATH_MAX + 1];
                    if (snprintf(full, sizeof full, "%s/%s", name, de->d_name) < (int)sizeof full) {
                        struct archive_entry *l = archive_entry_new();
                        archive_entry_set_pathname_utf8(l, full);
                        archive_entry_set_filetype(l, AE_IFLNK);
                        archive_entry_set_perm(l, 0777);
                        archive_entry_set_symlink_utf8(l, target);
                        if (archive_write_header(c->w, l) != ARCHIVE_OK) c->fail = 1;
                        archive_entry_free(l);
                    }
                }
                continue;
            }
            if (k < 0) continue;
            char full[ARC_PATH_MAX + 1];
            if (snprintf(full, sizeof full, "%s/%s", name, de->d_name) < (int)sizeof full) add_fd(c, k, full, depth + 1);
            close(k);
        }
        closedir(d);
        return;
    }
    if (!S_ISREG(st->st_mode)) { archive_entry_free(e); return; }       /* FIFOs, devices: no */
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, st->st_mode & 0755);
    archive_entry_set_size(e, st->st_size);
    if (archive_write_header(c->w, e) != ARCHIVE_OK) { c->fail = 1; archive_entry_free(e); return; }
    archive_entry_free(e);
    static char buf[1 << 16];
    off_t off = 0;
    for (;;) {
        ssize_t r = pread(fd, buf, sizeof buf, off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        if (archive_write_data(c->w, buf, (size_t)r) < 0) { c->fail = 1; break; }
        off += r;
        c->bytes += (unsigned long long)r;
        cr_progress(c, name, 0);
    }
    c->files++;
}

static void add_fd(Cr *c, int fd, const char *name, int depth) {
    struct stat st;
    if (fstat(fd, &st)) return;
    add_entry(c, fd, name, &st, depth);
}

static void op_create(uint32_t id, const JVal *args) {
    int out = slot_fd(args, "out");
    const JVal *items = json_get(args, "items");
    if (out < 0 || !items || items->type != J_ARR) { reply_err(id, "bad-request", ""); return; }
    Cr c = { .id = id };
    c.w = archive_write_new();
    archive_write_set_format_zip(c.w);
    archive_write_set_options(c.w, "zip:hdrcharset=UTF-8");
    if (archive_write_open_fd(c.w, out) != ARCHIVE_OK) { archive_write_free(c.w); reply_err(id, "write", ""); return; }
    for (const JVal *it = items->kid; it && !c.fail; it = it->next) {
        int fd = slot_fd(it, "slot");
        const char *name = json_str(it, "name");
        char clean[ARC_PATH_MAX + 1];
        if (fd < 0 || !name || arc_clean_path(name, clean, sizeof clean) || strchr(clean, '/')) continue;
        add_fd(&c, fd, clean, 0);
    }
    if (archive_write_close(c.w) != ARCHIVE_OK) c.fail = 1;
    archive_write_free(c.w);
    cr_progress(&c, "", 1);
    if (c.fail) { reply_err(id, "write", ""); return; }
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"files\":%ld,\"bytes\":%llu}", c.files, c.bytes);
    reply_json(DR_DONE, id, &b);
}

int arc_handle(const DocMsg *m, const JVal *a) {
    switch (m->h.op) {
    case DQ_ARC_LIST: op_list(m->h.id, a); return 1;
    case DQ_ARC_EXTRACT: op_extract(m->h.id, a); return 1;
    case DQ_ARC_CREATE: op_create(m->h.id, a); return 1;
    default: return 0;
    }
}
