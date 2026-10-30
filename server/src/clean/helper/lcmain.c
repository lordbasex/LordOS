/* lcmain.c — lord-clean, Cleaner's scanner (docs/CLEANER-ARCHITECTURE.md).
 *
 * lord-server (clean.c) starts it from its own folder, with no arguments and a minimal environment,
 * and passes each root as an opened folder over SCM_RIGHTS. It lists them in ro_dir_fds and calls
 * sandbox_apply_helper before reading anything: no network, no exec, nothing written, nothing read
 * but those folders. Without Landlock it refuses to scan, as the design's "no write access to
 * anything" would not hold.
 *
 * The walk: getdents on an fd, statx with no following of links for each entry, folders entered with
 * openat2 (RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV). A link is never followed, a
 * mount never crossed, a name that can't be shown never reported. fsops_protected and the user's
 * exclusions are rules of the walk, and lord-server applies them again to every record. It changes
 * nothing: it only reports.
 *
 * Three modes: "rules" reports the files a rule covers; "tree" keeps totals per folder and reports the
 * folders and the files of at least minSize; "dups" reads the files that share a size and reports
 * groups with equal SHA-256 (a partial hash of the first and last 16 KB first). */
#include "../cleanfs.h"
#include "../cleanproto.h"
#include "../../doc/wire.h"
#include "../../fsops.h"
#include "../../pkg/crypto.h"
#include "../../sec/json.h"
#include "../../sec/sandbox.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define SOCK 3
#define PARTIAL 16384
#define MAX_LINKS 1000000                   /* inodes with several links remembered by a tree */

enum { M_RULES, M_TREE, M_DUPS };

typedef struct {
    int fd, index;
    const char *pattern, *base;
    unsigned dev_major, dev_minor;
} Root;

typedef struct { double s, a; long f; } Ag;

typedef struct {
    char *rel;
    int root;
    struct statx sx;
    uint8_t hash[32];
    int bad;
} File;

static int mode;
static long max_files, min_size;
static int max_depth;
static long seen, dirs, hashed, nsym, nspecial, nmount, nunread, nprot, nname, nnomatch, nexcl;
static const char *limit_hit = "";
static int stop;
static const char *exclude[CLEAN_MAX_EXCLUDE + 1];

static JBuf items, nodes;
static int nitems, nnodes;
static double last_flush;

static long next_node, large;
static Ag types[CLEAN_TYPES];
static uint64_t *link_table;
static size_t link_used;

static File *files;
static size_t nfiles, files_cap;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void limit(const char *what) { if (!limit_hit[0]) limit_hit = what; }

static void flush_batch(int group) {
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"seen\":%ld,\"dirs\":%ld,\"hashed\":%ld,\"group\":%s,\"items\":[", seen, dirs, hashed, group ? "true" : "false");
    if (nitems) jb_raw(&b, items.p);
    jb_raw(&b, "],\"nodes\":[");
    if (nnodes) jb_raw(&b, nodes.p);
    jb_raw(&b, "]}");
    if (b.oom || doc_send(SOCK, CR_BATCH, 0, b.p, b.n, NULL, 0)) _exit(1);      /* lord-server is gone */
    jb_free(&b);
    jb_free(&items);
    jb_free(&nodes);
    jb_init(&items);
    jb_init(&nodes);
    nitems = nnodes = 0;
    last_flush = now_ms();
}

static void emit(int root, const char *rel, const struct statx *sx) {
    jb_printf(&items, "%s{\"r\":%d,\"p\":", nitems ? "," : "", root);
    jb_str(&items, rel);
    jb_printf(&items, ",\"M\":%u,\"m\":%u,\"i\":\"%llu\",\"l\":%u,\"s\":%llu,\"a\":%llu,\"t\":%lld,\"n\":%u,\"u\":%u}",
              sx->stx_dev_major, sx->stx_dev_minor, (unsigned long long)sx->stx_ino, sx->stx_nlink,
              (unsigned long long)sx->stx_size, (unsigned long long)sx->stx_blocks * 512,
              (long long)sx->stx_mtime.tv_sec, sx->stx_mtime.tv_nsec, sx->stx_uid);
    nitems++;
}

/* a tree counts an inode once: 1 the first time it is seen */
static int first_link(const struct statx *sx) {
    if (sx->stx_nlink < 2) return 1;
    if (!link_table) link_table = calloc(MAX_LINKS * 2, sizeof *link_table);
    if (!link_table || link_used >= MAX_LINKS) { limit("files"); return 1; }
    uint64_t key = sx->stx_ino * 0x9e3779b97f4a7c15ull ^ ((uint64_t)sx->stx_dev_major << 40) ^ ((uint64_t)sx->stx_dev_minor << 20);
    if (!key) key = 1;
    size_t i = (key ^ key >> 29) % (MAX_LINKS * 2);
    for (; link_table[i]; i = (i + 1) % (MAX_LINKS * 2)) if (link_table[i] == key) return 0;
    link_table[i] = key;
    link_used++;
    return 1;
}

static void on_file(const Root *r, const char *rel, const char *name, const struct statx *sx, Ag *here) {
    if (mode == M_RULES) {
        if (!clean_glob(r->pattern, name)) { nnomatch++; return; }
        emit(r->index, rel, sx);
        if (nitems >= CLEAN_BATCH) flush_batch(0);
    } else if (mode == M_TREE) {
        if (!first_link(sx)) return;
        double a = (double)sx->stx_blocks * 512;
        Ag *t = &types[clean_type(name)];
        t->s += (double)sx->stx_size; t->a += a; t->f++;
        here->s += (double)sx->stx_size; here->a += a; here->f++;
        if ((long)sx->stx_size >= min_size) {
            if (large >= CLEAN_MAX_LARGE) limit("results");
            else {
                large++;
                emit(r->index, rel, sx);
                if (nitems >= CLEAN_BATCH) flush_batch(0);
            }
        }
    } else if (sx->stx_size > 0) {
        if (nfiles == files_cap) {
            size_t cap = files_cap ? files_cap * 2 : 4096;
            File *f = realloc(files, cap * sizeof *f);
            if (!f) { limit("files"); stop = 1; return; }
            files = f;
            files_cap = cap;
        }
        char *copy = strdup(rel);
        if (!copy) { limit("files"); stop = 1; return; }
        files[nfiles++] = (File){ .rel = copy, .root = r->index, .sx = *sx };
    }
}

static void emit_node(long id, long up, const char *name, const Ag *ag) {
    jb_printf(&nodes, "%s{\"id\":%ld,\"up\":%ld,\"name\":", nnodes ? "," : "", id, up);
    jb_str(&nodes, name);
    jb_printf(&nodes, ",\"s\":%.0f,\"a\":%.0f,\"f\":%ld}", ag->s, ag->a, ag->f);
    if (++nnodes >= CLEAN_BATCH) flush_batch(0);
}

static int excluded(const char *abs) {
    for (int i = 0; exclude[i]; i++) if (clean_under(abs, exclude[i])) return 1;
    return 0;
}

/* takes fd; rel is the folder's path below the root ("" for the root), in a buffer of CLEAN_PATH_MAX + 1.
 * Tree: id is this folder's node (-1 past the limit), up its parent's, and its totals join *total. */
static void walk(const Root *r, int fd, char *rel, size_t rl, int depth, long id, long up, Ag *total) {
    DIR *d = fdopendir(fd);
    if (!d) { close(fd); nunread++; return; }
    dirs++;
    Ag here = { 0, 0, 0 };
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) { if (errno) nunread++; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (seen >= max_files) { limit("files"); stop = 1; break; }
        seen++;
        if (nitems || nnodes ? now_ms() - last_flush > 200 : (seen & 1023) == 0) flush_batch(0);
        size_t nl = strlen(e->d_name);
        if (!clean_name_ok(e->d_name) || rl + nl + 1 > CLEAN_PATH_MAX) { nname++; continue; }
        size_t l2 = rl;
        if (rl) rel[l2++] = '/';
        memcpy(rel + l2, e->d_name, nl + 1);
        char abs[PATH_MAX * 2 + CLEAN_PATH_MAX];
        if ((size_t)snprintf(abs, sizeof abs, "%s/%s", strcmp(r->base, "/") ? r->base : "", rel) >= sizeof abs) { nname++; goto next; }
        if (fsops_protected(abs)) { nprot++; goto next; }
        if (excluded(abs)) { nexcl++; goto next; }
        struct statx sx;
        if (statx(dirfd(d), e->d_name, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_UID |
                  STATX_INO | STATX_SIZE | STATX_BLOCKS | STATX_MTIME, &sx)) { nunread++; goto next; }
        int mountroot = (sx.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) && (sx.stx_attributes & STATX_ATTR_MOUNT_ROOT);
        int other_dev = sx.stx_dev_major != r->dev_major || sx.stx_dev_minor != r->dev_minor;
        if (S_ISLNK(sx.stx_mode)) nsym++;
        else if (S_ISDIR(sx.stx_mode)) {
            if (mountroot || other_dev) nmount++;
            else if (depth >= max_depth) limit("depth");
            else {
                int sub = clean_open(dirfd(d), e->d_name, O_RDONLY | O_DIRECTORY);
                if (sub >= 0) {
                    long child = -1;
                    if (mode == M_TREE) {
                        if (id >= 0 && next_node < CLEAN_MAX_NODES) child = next_node++;
                        else limit("nodes");
                    }
                    walk(r, sub, rel, l2 + nl, depth + 1, child, id, &here);
                }
                else if (errno == EXDEV) nmount++;
                else if (errno == ELOOP) nsym++;
                else nunread++;
            }
        } else if (!S_ISREG(sx.stx_mode)) nspecial++;
        else if (mountroot || other_dev) nmount++;
        else on_file(r, rel, e->d_name, &sx, &here);
    next:
        rel[rl] = 0;
        if (stop) break;
    }
    if (mode == M_TREE) {
        const char *base = strrchr(rel, '/');
        base = base ? base + 1 : rel;
        if (id >= 0) emit_node(id, up, base, &here);
        if (total) { total->s += here.s; total->a += here.a; total->f += here.f; }
    }
    closedir(d);
}

/* ---- duplicates ---- */

static int by_size(const void *a, const void *b) {
    const File *x = a, *y = b;
    return x->sx.stx_size < y->sx.stx_size ? -1 : x->sx.stx_size > y->sx.stx_size;
}
static int by_hash(const void *a, const void *b) {
    const File *x = a, *y = b;
    int c = memcmp(x->hash, y->hash, 32);
    return c ? c : x->sx.stx_ino < y->sx.stx_ino ? -1 : x->sx.stx_ino > y->sx.stx_ino;
}

/* the same object the walk saw, read-only; -1 otherwise */
static int open_file(const File *f, const Root *list) {
    int fd = clean_open(list[f->root].fd, f->rel, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_ino != f->sx.stx_ino || (uint64_t)st.st_size != f->sx.stx_size) { close(fd); return -1; }
    return fd;
}

static int hash_part(File *f, const Root *list) {
    static uint8_t buf[PARTIAL];
    int fd = open_file(f, list);
    if (fd < 0) return -1;
    Sha256 h;
    sha256_init(&h);
    uint64_t size = f->sx.stx_size;
    ssize_t n = pread(fd, buf, sizeof buf, 0);
    if (n < 0) { close(fd); return -1; }
    sha256_update(&h, buf, (size_t)n);
    if (size > PARTIAL) {
        n = pread(fd, buf, sizeof buf, size > 2 * PARTIAL ? (off_t)(size - PARTIAL) : PARTIAL);
        if (n < 0) { close(fd); return -1; }
        sha256_update(&h, buf, (size_t)n);
    }
    close(fd);
    sha256_final(&h, f->hash);
    return 0;
}

static int hash_full(File *f, const Root *list) {
    static uint8_t buf[1 << 18];
    int fd = open_file(f, list);
    if (fd < 0) return -1;
    Sha256 h;
    sha256_init(&h);
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return -1; }
        if (!n) break;
        sha256_update(&h, buf, (size_t)n);
    }
    close(fd);
    sha256_final(&h, f->hash);
    hashed++;
    return 0;
}

/* a..b sorted by hash: 1 if they hold at least two inodes */
static int distinct(const File *a, const File *b) {
    for (const File *f = a + 1; f < b; f++) if (f->sx.stx_ino != a->sx.stx_ino) return 1;
    return 0;
}

static void emit_group(const File *a, const File *b) {
    long n = 0;
    for (const File *f = a; f < b; f++) {
        if (n >= CLEAN_MAX_GROUP) { limit("results"); break; }
        emit(f->root, f->rel, &f->sx);
        if (++n % CLEAN_BATCH == 0 && f + 1 < b && n < CLEAN_MAX_GROUP) flush_batch(0);
    }
    flush_batch(1);
}

static void find_duplicates(const Root *list) {
    qsort(files, nfiles, sizeof *files, by_size);
    for (size_t i = 0; i < nfiles;) {
        size_t j = i + 1;
        while (j < nfiles && files[j].sx.stx_size == files[i].sx.stx_size) j++;
        if (j - i >= 2) {
            size_t ok = i;
            for (size_t k = i; k < j; k++) {
                if (hash_part(&files[k], list)) { nunread++; continue; }
                if (ok != k) { File t = files[ok]; files[ok] = files[k]; files[k] = t; }
                ok++;
            }
            qsort(files + i, ok - i, sizeof *files, by_hash);
            for (size_t p = i; p < ok;) {
                size_t q = p + 1;
                while (q < ok && !memcmp(files[q].hash, files[p].hash, 32)) q++;
                if (q - p >= 2 && distinct(&files[p], &files[q])) {
                    size_t good = p;
                    for (size_t k = p; k < q; k++) {
                        if (hash_full(&files[k], list)) { nunread++; continue; }
                        if (good != k) { File t = files[good]; files[good] = files[k]; files[k] = t; }
                        good++;
                        if (now_ms() - last_flush > 200) flush_batch(0);
                    }
                    qsort(files + p, good - p, sizeof *files, by_hash);
                    for (size_t x = p; x < good;) {
                        size_t y = x + 1;
                        while (y < good && !memcmp(files[y].hash, files[x].hash, 32)) y++;
                        if (y - x >= 2 && distinct(&files[x], &files[y])) emit_group(&files[x], &files[y]);
                        x = y;
                    }
                }
                p = q;
            }
        }
        i = j;
    }
}

static void reply_err(const char *code) {
    char j[48];
    int n = snprintf(j, sizeof j, "{\"error\":\"%s\"}", code);
    doc_send(SOCK, CR_ERR, 0, j, (size_t)n, NULL, 0);
}

int main(void) {
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    DocMsg go;
    if (doc_recv(SOCK, &go) || go.h.op != CQ_GO || !go.json) return 1;
    JDoc d = { 0 };
    if (json_parse(go.json, go.len, &d, NULL, 0)) { reply_err("request"); return 1; }
    const JVal *roots = json_get(d.root, "roots");
    const char *home = json_str(d.root, "home"), *m = json_str(d.root, "mode");
    max_files = (long)json_num(d.root, "maxFiles", 0);
    max_depth = (int)json_num(d.root, "maxDepth", 0);
    min_size = (long)json_num(d.root, "minSize", 0);
    int nroots = roots && roots->type == J_ARR ? (int)roots->len : -1;
    mode = m && !strcmp(m, "tree") ? M_TREE : m && !strcmp(m, "dups") ? M_DUPS : M_RULES;
    if (!home || nroots < 0 || nroots > CLEAN_MAX_ROOTS || nroots != go.nfds || max_files < 1 || max_files > CLEAN_TREE_MAX_FILES ||
        max_depth < 0 || max_depth > CLEAN_MAX_DEPTH) { reply_err("request"); return 1; }
    const JVal *ex = json_get(d.root, "exclude");
    int nex = 0;
    if (ex && ex->type == J_ARR) for (const JVal *v = ex->kid; v && nex < CLEAN_MAX_EXCLUDE; v = v->next) if (v->type == J_STR) exclude[nex++] = v->str;
    fsops_init(home, "");

    /* lowest priority: a scan must not slow the desktop */
    setpriority(PRIO_PROCESS, 0, 19);
    syscall(SYS_ioprio_set, 1, 0, (3 << 13));          /* IOPRIO_CLASS_IDLE */

    static Root list[CLEAN_MAX_ROOTS];
    static int rofd[CLEAN_MAX_ROOTS + 1];
    int i = 0;
    for (const JVal *v = roots->kid; v; v = v->next, i++) {
        struct statx sx;
        double rule = json_num(v, "rule", -1);
        list[i] = (Root){ .fd = go.fds[i], .index = (int)rule, .pattern = json_str(v, "pattern"), .base = json_str(v, "base") };
        if (rule < 0 || rule >= CLEAN_MAX_ROOTS || !list[i].pattern || !list[i].base || statx(go.fds[i], "", AT_EMPTY_PATH, STATX_TYPE, &sx) || !S_ISDIR(sx.stx_mode)) {
            reply_err("request");
            return 1;
        }
        list[i].dev_major = sx.stx_dev_major;
        list[i].dev_minor = sx.stx_dev_minor;
        rofd[i] = go.fds[i];
    }
    rofd[nroots] = -1;
    SandboxHelperCfg cfg = { .name = "lordos-cleaner", .ro_paths = NULL, .extra_syscalls = NULL, .mem_max_mb = 512,
                             .ro_dir_fds = rofd, .rw_dir_fds = NULL };
    int mask = sandbox_apply_helper(&cfg);
    if (!(mask & SB_LANDLOCK_FS)) { reply_err("sandbox"); return 1; }

    jb_init(&items);
    jb_init(&nodes);
    last_flush = now_ms();
    static char rel[CLEAN_PATH_MAX + 1];
    for (i = 0; i < nroots && !stop; i++) {
        int fd = fcntl(list[i].fd, F_DUPFD_CLOEXEC, 0);    /* fdopendir takes it; the fd keeps its read position */
        if (fd < 0) { nunread++; continue; }
        rel[0] = 0;
        Ag total = { 0, 0, 0 };
        if (mode == M_TREE) next_node = 1;
        walk(&list[i], fd, rel, 0, 0, mode == M_TREE ? 0 : -1, -1, &total);
    }
    flush_batch(0);
    if (mode == M_DUPS) {
        static Root by_rule[CLEAN_MAX_ROOTS];
        for (i = 0; i < nroots; i++) by_rule[list[i].index] = list[i];
        find_duplicates(by_rule);
    }
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"seen\":%ld,\"dirs\":%ld,\"limit\":\"%s\",\"symlinks\":%ld,\"special\":%ld,\"mounts\":%ld,"
                  "\"unreadable\":%ld,\"protected\":%ld,\"names\":%ld,\"nomatch\":%ld,\"excluded\":%ld,\"types\":[",
              seen, dirs, limit_hit, nsym, nspecial, nmount, nunread, nprot, nname, nnomatch, nexcl);
    for (i = 0; i < CLEAN_TYPES; i++) jb_printf(&b, "%s{\"s\":%.0f,\"a\":%.0f,\"f\":%ld}", i ? "," : "", types[i].s, types[i].a, types[i].f);
    jb_raw(&b, "]}");
    doc_send(SOCK, CR_DONE, 0, b.p, b.n, NULL, 0);
    return 0;
}
