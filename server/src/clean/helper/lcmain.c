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
 * mount never crossed, a name that can't be shown never reported. fsops_protected is a rule of the
 * walk, and lord-server applies it again to every record. It changes nothing: it only reports. */
#include "../cleanfs.h"
#include "../cleanproto.h"
#include "../../doc/wire.h"
#include "../../fsops.h"
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

typedef struct {
    int fd, index;
    const char *pattern, *base;
    unsigned dev_major, dev_minor;
} Root;

static long max_files;
static int max_depth;
static long seen, dirs, nsym, nspecial, nmount, nunread, nprot, nname, nnomatch;
static const char *limit_hit = "";
static int stop;

static JBuf batch;
static int batched;
static double last_flush;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void flush_batch(void) {
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"seen\":%ld,\"dirs\":%ld,\"items\":[", seen, dirs);
    if (batched) jb_raw(&b, batch.p);
    jb_raw(&b, "]}");
    if (b.oom || doc_send(SOCK, CR_BATCH, 0, b.p, b.n, NULL, 0)) _exit(1);      /* lord-server is gone */
    jb_free(&b);
    jb_free(&batch);
    jb_init(&batch);
    batched = 0;
    last_flush = now_ms();
}

static void emit(const Root *r, const char *rel, const struct statx *sx) {
    jb_printf(&batch, "%s{\"r\":%d,\"p\":", batched ? "," : "", r->index);
    jb_str(&batch, rel);
    jb_printf(&batch, ",\"M\":%u,\"m\":%u,\"i\":\"%llu\",\"l\":%u,\"s\":%llu,\"a\":%llu,\"t\":%lld,\"n\":%u,\"u\":%u}",
              sx->stx_dev_major, sx->stx_dev_minor, (unsigned long long)sx->stx_ino, sx->stx_nlink,
              (unsigned long long)sx->stx_size, (unsigned long long)sx->stx_blocks * 512,
              (long long)sx->stx_mtime.tv_sec, sx->stx_mtime.tv_nsec, sx->stx_uid);
    if (++batched >= CLEAN_BATCH) flush_batch();
}

/* takes fd; rel is the folder's path below the root ("" for the root), in a buffer of CLEAN_PATH_MAX + 1 */
static void walk(const Root *r, int fd, char *rel, size_t rl, int depth) {
    DIR *d = fdopendir(fd);
    if (!d) { close(fd); nunread++; return; }
    dirs++;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) { if (errno) nunread++; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (seen >= max_files) { limit_hit = "files"; stop = 1; break; }
        seen++;
        if (batched ? now_ms() - last_flush > 200 : (seen & 1023) == 0) flush_batch();
        size_t nl = strlen(e->d_name);
        if (!clean_name_ok(e->d_name) || rl + nl + 1 > CLEAN_PATH_MAX) { nname++; continue; }
        size_t l2 = rl;
        if (rl) rel[l2++] = '/';
        memcpy(rel + l2, e->d_name, nl + 1);
        char abs[PATH_MAX * 2 + CLEAN_PATH_MAX];
        if ((size_t)snprintf(abs, sizeof abs, "%s/%s", r->base, rel) >= sizeof abs) { nname++; goto next; }
        if (fsops_protected(abs)) { nprot++; goto next; }
        struct statx sx;
        if (statx(dirfd(d), e->d_name, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_UID |
                  STATX_INO | STATX_SIZE | STATX_BLOCKS | STATX_MTIME, &sx)) { nunread++; goto next; }
        int mountroot = (sx.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) && (sx.stx_attributes & STATX_ATTR_MOUNT_ROOT);
        int other_dev = sx.stx_dev_major != r->dev_major || sx.stx_dev_minor != r->dev_minor;
        if (S_ISLNK(sx.stx_mode)) nsym++;
        else if (S_ISDIR(sx.stx_mode)) {
            if (mountroot || other_dev) nmount++;
            else if (depth >= max_depth) limit_hit = limit_hit[0] ? limit_hit : "depth";
            else {
                int sub = clean_open(dirfd(d), e->d_name, O_RDONLY | O_DIRECTORY);
                if (sub >= 0) walk(r, sub, rel, l2 + nl, depth + 1);
                else if (errno == EXDEV) nmount++;
                else if (errno == ELOOP) nsym++;
                else nunread++;
            }
        } else if (!S_ISREG(sx.stx_mode)) nspecial++;
        else if (mountroot || other_dev) nmount++;
        else if (!clean_glob(r->pattern, e->d_name)) nnomatch++;
        else emit(r, rel, &sx);
    next:
        rel[rl] = 0;
        if (stop) break;
    }
    closedir(d);
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
    const char *home = json_str(d.root, "home");
    max_files = (long)json_num(d.root, "maxFiles", 0);
    max_depth = (int)json_num(d.root, "maxDepth", 0);
    int nroots = roots && roots->type == J_ARR ? (int)roots->len : -1;
    if (!home || nroots < 0 || nroots > CLEAN_MAX_ROOTS || nroots != go.nfds || max_files < 1 || max_files > CLEAN_MAX_FILES ||
        max_depth < 0 || max_depth > CLEAN_MAX_DEPTH) { reply_err("request"); return 1; }
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
    SandboxHelperCfg cfg = { .name = "lordos-cleaner", .ro_paths = NULL, .extra_syscalls = NULL, .mem_max_mb = 256,
                             .ro_dir_fds = rofd, .rw_dir_fds = NULL };
    int mask = sandbox_apply_helper(&cfg);
    if (!(mask & SB_LANDLOCK_FS)) { reply_err("sandbox"); return 1; }

    jb_init(&batch);
    last_flush = now_ms();
    static char rel[CLEAN_PATH_MAX + 1];
    for (i = 0; i < nroots && !stop; i++) {
        int fd = fcntl(list[i].fd, F_DUPFD_CLOEXEC, 0);    /* fdopendir takes it; the fd keeps its read position */
        if (fd < 0) { nunread++; continue; }
        rel[0] = 0;
        walk(&list[i], fd, rel, 0, 0);
    }
    flush_batch();
    char j[320];
    int n = snprintf(j, sizeof j, "{\"seen\":%ld,\"dirs\":%ld,\"limit\":\"%s\",\"symlinks\":%ld,\"special\":%ld,\"mounts\":%ld,"
                     "\"unreadable\":%ld,\"protected\":%ld,\"names\":%ld,\"nomatch\":%ld}",
                     seen, dirs, limit_hit, nsym, nspecial, nmount, nunread, nprot, nname, nnomatch);
    doc_send(SOCK, CR_DONE, 0, j, (size_t)n, NULL, 0);
    return 0;
}
