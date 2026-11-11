/* lordlog.c — LordOS's log files: where they go, their line format, the cap and the stamping child
 * (lordlog.h, docs/SYSTEM.md "Log files"). libc only. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lordlog.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* ---- names ---- */

static int name_byte(unsigned char c) { return isalnum(c) || c == '.' || c == '_' || c == '+' || c == '-'; }

int lordlog_name_ok(const char *s) {
    if (!s || !*s || s[0] == '.' || s[0] == '-') return 0;
    size_t n = 0;
    for (; s[n]; n++) if (n >= LORDLOG_NAME_MAX || !name_byte((unsigned char)s[n])) return 0;
    return 1;
}

void lordlog_sanitize(const char *in, char *out, size_t n) {
    size_t max = n - 1 < LORDLOG_NAME_MAX ? n - 1 : LORDLOG_NAME_MAX, k = 0;
    for (; in && in[k] && k < max; k++) out[k] = name_byte((unsigned char)in[k]) ? in[k] : '_';
    out[k] = 0;
    if (!k) { out[0] = '_'; out[1] = 0; }
    if (out[0] == '.' || out[0] == '-') out[0] = '_';
}

/* ---- the directories ---- */

/* A directory, opened without following a symlink, owned by `trusted` and writable by nobody else;
 * made with `mode` when missing. */
static int open_trusted(int at, const char *name, mode_t mode, uid_t trusted, int create) {
    int made = 0;
    if (create) {
        if (!mkdirat(at, name, mode)) made = 1;
        else if (errno != EEXIST) return -1;
    }
    int fd = openat(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) { close(fd); return -1; }
    if (made && (st.st_mode & 07777) != mode && !fchmod(fd, mode)) st.st_mode = (st.st_mode & ~07777u) | mode;   /* umask */
    if (!S_ISDIR(st.st_mode) || st.st_uid != trusted || (st.st_mode & 022)) { close(fd); errno = EPERM; return -1; }
    return fd;
}

int lordlog_prepare_user_at(const char *root, const char *user, uid_t uid, gid_t gid, uid_t trusted) {
    if (!lordlog_name_ok(user) || !root || root[0] != '/') { errno = EINVAL; return -1; }
    int r = open_trusted(AT_FDCWD, root, 0755, trusted, 1);
    if (r < 0) return -1;
    int u = open_trusted(r, "users", 0711, trusted, 1);
    close(r);
    if (u < 0) return -1;
    int made = !mkdirat(u, user, 0700);
    if (!made && errno != EEXIST) { close(u); return -1; }
    int d = openat(u, user, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);   /* ELOOP for a symlink */
    int e = errno;
    close(u);
    if (d < 0) { errno = e; return -1; }
    struct stat st;
    int ok = !fstat(d, &st) && S_ISDIR(st.st_mode);
    if (ok && st.st_uid != uid) {
        /* only one this function made (still the trusted owner's) is handed over; another user's
         * directory (a removed account whose name came back) is never given to this one */
        if (!made && st.st_uid != trusted) { errno = EPERM; ok = 0; }
        else ok = !fchown(d, uid, gid);
    } else if (ok && st.st_gid != gid && fchown(d, (uid_t)-1, gid)) { /* the group doesn't matter: mode 0700 */ }
    if (ok && (st.st_mode & 07777) != 0700) ok = !fchmod(d, 0700);
    e = errno;
    close(d);
    errno = e;
    return ok ? 0 : -1;
}

int lordlog_prepare_user(const char *user, uid_t uid, gid_t gid) { return lordlog_prepare_user_at(LORDLOG_ROOT, user, uid, gid, 0); }

static int own_dir(const char *p, uid_t uid) {
    struct stat st;
    return !lstat(p, &st) && S_ISDIR(st.st_mode) && st.st_uid == uid;
}

int lordlog_user_dir_at(const char *root, const char *user, uid_t uid, const char *home, char *out, size_t n) {
    struct stat st;
    if (root && lordlog_name_ok(user) && (size_t)snprintf(out, n, "%s/users/%s", root, user) < n &&
        !lstat(out, &st) && S_ISDIR(st.st_mode) && st.st_uid == uid && !(st.st_mode & 077) && !access(out, W_OK | X_OK))
        return 1;
    if (!home || home[0] != '/' || (size_t)snprintf(out, n, "%s/.cache", home) >= n) { if (n) out[0] = 0; return -1; }
    mkdir(out, 0700);
    if ((size_t)snprintf(out, n, "%s/.cache/lordos", home) >= n) { out[0] = 0; return -1; }
    mkdir(out, 0700);
    if (!own_dir(out, uid)) { out[0] = 0; return -1; }
    return 0;
}

const char *lordlog_root = LORDLOG_ROOT;

static char my_dir[PATH_MAX];
static int my_dir_done, my_dir_sys;

const char *lordlog_dir(void) {
    if (my_dir_done) return my_dir;
    my_dir_done = 1;
    char pwbuf[4096];
    struct passwd pw, *res = NULL;
    const char *name = !getpwuid_r(getuid(), &pw, pwbuf, sizeof pwbuf, &res) && res ? res->pw_name : NULL;
    const char *home = getenv("HOME");
    if (!home && res) home = res->pw_dir;
    /* /var/log/lordos only for a session lord-sessiond started, in the account's own home: a nested or
     * test lord-server (another $HOME) must never rotate the running session's files */
    int managed = getenv("LORDOS_SESSIOND") && res && home && !strcmp(home, res->pw_dir);
    my_dir_sys = lordlog_user_dir_at(managed ? lordlog_root : NULL, name, getuid(), home, my_dir, sizeof my_dir) == 1;
    return my_dir;
}

int lordlog_dir_is_system(void) { lordlog_dir(); return my_dir_sys; }

int lordlog_path(const char *sub, const char *name, char *out, size_t n) {
    const char *d = lordlog_dir();
    char comp[LORDLOG_NAME_MAX + 1];
    if (!*d) return -1;
    lordlog_sanitize(name, comp, sizeof comp);
    if (sub) {
        if (strcmp(sub, "apps") && strcmp(sub, "web")) return -1;
        if ((size_t)snprintf(out, n, "%s/%s", d, sub) >= n) return -1;
        mkdir(out, 0700);
        if (!own_dir(out, getuid())) return -1;
        return (size_t)snprintf(out, n, "%s/%s/%s.log", d, sub, comp) < n ? 0 : -1;
    }
    return (size_t)snprintf(out, n, "%s/%s.log", d, comp) < n ? 0 : -1;
}

/* ---- opening, the cap, rotation ---- */

/* "<path><ext>" without stdio (used after fork). Two names, never ".1" (logrotate's):
 *   .prev  the previous session's file, only from lordlog_rotate at a session's start;
 *   .old   the older part a cap moved aside during this session, so a long or noisy session
 *          never overwrites the previous one's (the crash evidence). */
static int with_ext(const char *path, const char *ext, char *out, size_t n) {
    size_t l = strlen(path), e = strlen(ext);
    if (l + e + 1 > n) return -1;
    memcpy(out, path, l);
    memcpy(out + l, ext, e + 1);
    return 0;
}
#define PREV(p, o, n) with_ext(p, ".prev", o, n)
#define OLD(p, o, n)  with_ext(p, ".old", o, n)

int lordlog_cap(const char *path, off_t cap) {
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    if (st.st_size <= cap) { close(fd); return 0; }
    char old[PATH_MAX];
    int r = -1;
    if (!OLD(path, old, sizeof old)) {
        unlink(old);
        int o = open(old, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC, 0600);
        if (o >= 0) {
            /* the newest part, at most cap bytes, from a line start */
            char buf[16384];
            off_t at = st.st_size - cap - 1, end = st.st_size;    /* from the byte before: it may end a line */
            int first = 1;
            while (at < end) {
                size_t want = (size_t)(end - at) < sizeof buf ? (size_t)(end - at) : sizeof buf;
                ssize_t k = pread(fd, buf, want, at);
                if (k <= 0) break;
                at += k;
                const char *p = buf;
                size_t len = (size_t)k;
                if (first) {
                    const char *nl = memchr(buf, '\n', len);
                    if (!nl) continue;
                    first = 0;
                    len -= (size_t)(nl + 1 - buf);
                    p = nl + 1;
                }
                if (len && write(o, p, len) != (ssize_t)len) break;
            }
            close(o);
        }
        r = ftruncate(fd, 0) ? -1 : 1;
    }
    close(fd);
    return r;
}

int lordlog_open(const char *path) {
    lordlog_cap(path, LORDLOG_CAP);
    /* O_NONBLOCK so a FIFO put there can't hold the caller; a regular file doesn't care, and it's cleared */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_NOCTTY | O_NONBLOCK | O_CLOEXEC, 0600);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode))) { close(fd); fd = -1; }
    if (fd >= 0) fcntl(fd, F_SETFL, O_APPEND);
    return fd;
}

void lordlog_rotate(const char *path) {
    struct stat st;
    char old[PATH_MAX];
    if (!lstat(path, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && !PREV(path, old, sizeof old)) rename(path, old);
}

/* ---- the line format ---- */

/* word w (lowercase) at a word start in s, case-insensitively; a prefix is enough ("fail" → "failed") */
static int has_word(const char *s, const char *w) {
    size_t n = strlen(w);
    for (const char *p = s; *p; p++) {
        if (p > s && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
        if (!strncasecmp(p, w, n)) return 1;
    }
    return 0;
}

const char *lordlog_level(const char *msg) {
    static const char *const err[] = { "error", "fail", "fatal", "abort", "crash", "segfault", "panic", "assertion", NULL };
    static const char *const warn[] = { "warn", "could not", "cannot", "can't", "unable", "not started", "refused", "denied",
                                        "ignored", "timed out", "timeout", "missing", NULL };
    for (int i = 0; err[i]; i++) if (has_word(msg, err[i])) return "ERROR";
    if (strstr(msg, "Error")) return "ERROR";        /* TypeError, ReferenceError, … */
    for (int i = 0; warn[i]; i++) if (has_word(msg, warn[i])) return "WARN";
    return "INFO";
}

int lordlog_subsystem(const char *line, const char *dflt, char *sub, size_t n, const char **msg) {
    static const char *const levels[] = { "error", "warning", "warn", "note", "info", "debug", "fatal", NULL };
    const char *p = line;
    size_t k = 0;
    if (isalpha((unsigned char)*p)) {
        while (k < 40 && (isalnum((unsigned char)p[k]) || p[k] == '_' || p[k] == '.' || p[k] == '-')) k++;
        size_t name = k;
        if (p[k] == '[') {
            size_t j = k + 1;
            while (j - k <= 10 && isdigit((unsigned char)p[j])) j++;
            if (p[j] == ']' && j > k + 1) k = j + 1;
        }
        int is_level = 0;
        for (int i = 0; levels[i]; i++) if (strlen(levels[i]) == name && !strncasecmp(p, levels[i], name)) is_level = 1;
        if (p[k] == ':' && (p[k + 1] == ' ' || !p[k + 1]) && !is_level && k < n) {
            memcpy(sub, p, k);
            sub[k] = 0;
            *msg = p[k + 1] ? p + k + 2 : p + k + 1;
            return 1;
        }
    }
    snprintf(sub, n, "%s", dflt);
    *msg = line;
    return 0;
}

size_t lordlog_format(char *out, size_t n, double unix_s, const char *level, const char *sub, const char *msg) {
    if (n < 2) { if (n) out[0] = 0; return 0; }
    time_t t = (time_t)unix_s;
    int ms = (int)((unix_s - (double)t) * 1000);
    if (ms < 0) ms = 0;
    if (ms > 999) ms = 999;
    struct tm tm;
    localtime_r(&t, &tm);
    long off = tm.tm_gmtoff / 60;
    char ts[48];
    size_t l = strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(ts + l, sizeof ts - l, ".%03d%c%02ld:%02ld", ms, off < 0 ? '-' : '+', labs(off) / 60, labs(off) % 60);
    int k = snprintf(out, n, "%s %-5s %s: %s", ts, level, sub, msg);
    size_t len = k < 0 ? 0 : (size_t)k >= n - 1 ? n - 2 : (size_t)k;
    out[len++] = '\n';
    out[len] = 0;
    return len;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

void lordlog_write(int fd, const char *level, const char *sub, const char *fmt, ...) {
    if (fd < 0) return;
    char msg[1024], line[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    size_t n = lordlog_format(line, sizeof line, now_s(), level, sub, msg);
    if (write(fd, line, n) < 0) { /* no log */ }
}

/* ---- the stamping child ---- */

typedef struct { int fd; char path[PATH_MAX]; const char *dflt; off_t since_check; } Out;

#define LINE_MAX_BYTES 8192
#define SCAN_EVERY_S   60

static void out_reopen_if_big(Out *o) {
    struct stat st;
    if (fstat(o->fd, &st) || st.st_size <= LORDLOG_CAP) return;
    char old[PATH_MAX];
    if (OLD(o->path, old, sizeof old) || rename(o->path, old)) { if (ftruncate(o->fd, 0)) {} return; }
    int fd = open(o->path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC, 0600);
    if (fd < 0) return;
    if (dup2(fd, o->fd) >= 0) close(fd); else { close(o->fd); o->fd = fd; }
}

static void out_line(Out *o, const char *level, const char *sub, const char *msg) {
    char buf[LINE_MAX_BYTES + 256];
    size_t n = lordlog_format(buf, sizeof buf, now_s(), level, sub, msg);
    if (write(o->fd, buf, n) < 0) return;
    o->since_check += (off_t)n;
    if (o->since_check > (1 << 20)) { o->since_check = 0; out_reopen_if_big(o); }
}

/* every *.log under dir, dir/apps and dir/web that is not one of ours: capped */
static void scan(const char *dir, const Out *outs, int nout) {
    static const char *const subs[] = { "", "/apps", "/web" };
    for (int s = 0; s < 3; s++) {
        char d[PATH_MAX];
        if ((size_t)snprintf(d, sizeof d, "%s%s", dir, subs[s]) >= sizeof d) continue;
        int fd = open(d, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        DIR *dp = fd >= 0 ? fdopendir(fd) : NULL;
        if (!dp) { if (fd >= 0) close(fd); continue; }
        struct dirent *e;
        while ((e = readdir(dp))) {
            size_t l = strlen(e->d_name);
            if (l < 5 || strcmp(e->d_name + l - 4, ".log") || !lordlog_name_ok(e->d_name)) continue;
            char p[PATH_MAX];
            if ((size_t)snprintf(p, sizeof p, "%s/%s", d, e->d_name) >= sizeof p) continue;
            int ours = 0;
            for (int i = 0; i < nout; i++) if (!strcmp(outs[i].path, p)) ours = 1;
            if (!ours) lordlog_cap(p, LORDLOG_CAP);
        }
        closedir(dp);
    }
}

static void stamper(int in, int raw, Out *outs, int nout, const char *scan_dir) {
    static char buf[LINE_MAX_BYTES + 1];
    size_t len = 0;
    int last = 0;                                   /* the previous line's output, level, subsystem */
    char last_sub[64] = "", last_level[8] = "INFO";
    time_t scanned = 0;
    for (;;) {
        struct pollfd p = { in, POLLIN, 0 };
        int r = poll(&p, 1, scan_dir ? SCAN_EVERY_S * 1000 : -1);
        if (r < 0 && errno != EINTR) break;
        int eof = 0;
        if (r > 0) {
            ssize_t k = read(in, buf + len, LINE_MAX_BYTES - len);
            if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (k <= 0) eof = 1;
            else len += (size_t)k;
            size_t start = 0;
            for (size_t i = 0; i <= len; i++) {
                int full = i == len && start == 0 && len == LINE_MAX_BYTES;   /* a line too long: cut */
                int tail = i == len && eof && i > start;                       /* the last bytes before EOF */
                if (i < len ? buf[i] != '\n' : !(full || tail)) continue;
                buf[i] = 0;
                const char *line = buf + start;
                size_t ll = i - start;
                start = i < len ? i + 1 : i;
                if (ll && line[ll - 1] == '\r') ((char *)line)[--ll] = 0;
                if (raw >= 0) {
                    ((char *)line)[ll] = '\n';
                    if (write(raw, line, ll + 1) < 0) { /* the journal is gone: the file still gets it */ }
                    ((char *)line)[ll] = 0;
                }
                if (!ll) continue;
                const char *msg;
                char sub[64];
                if ((line[0] == ' ' || line[0] == '\t') && last_sub[0]) {
                    /* a continuation (a JS stack's "    at …"): with the line before it */
                    out_line(&outs[last], last_level, last_sub, line);
                    continue;
                }
                lordlog_subsystem(line, outs[0].dflt, sub, sizeof sub, &msg);
                int o = nout > 1 && !strcmp(sub, "js") ? 1 : 0;
                const char *lv = lordlog_level(msg);
                out_line(&outs[o], lv, sub, msg);
                last = o;
                snprintf(last_sub, sizeof last_sub, "%s", sub);
                snprintf(last_level, sizeof last_level, "%s", lv);
            }
            memmove(buf, buf + start, len - start);
            len -= start;
        }
        if (scan_dir && time(NULL) - scanned >= SCAN_EVERY_S) { scan(scan_dir, outs, nout); scanned = time(NULL); }
        if (eof) break;
    }
}

/* An isolated app's output: copied as is, never parsed (it is untrusted), only counted for the cap */
static void copier(int in, Out *o) {
    static char buf[65536];
    for (;;) {
        ssize_t k = read(in, buf, sizeof buf);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        if (write(o->fd, buf, (size_t)k) < 0) { /* full disk: dropped */ }
        o->since_check += (off_t)k;
        if (o->since_check > (1 << 20)) { o->since_check = 0; out_reopen_if_big(o); }
    }
}

/* Forks the child; in the parent, stdout (if also_stdout) and stderr become the pipe. copy: the
 * child copies bytes into outs[0] instead of stamping lines. */
static int start(Out *outs, int nout, int raw, const char *scan_dir, int also_stdout, int copy) {
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    tzset();
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        /* in: 3, outputs: 4.., raw last; nothing else of the parent stays open */
        int keep[4], nk = 0;
        keep[nk++] = fcntl(p[0], F_DUPFD_CLOEXEC, 64);
        for (int i = 0; i < nout; i++) keep[nk++] = fcntl(outs[i].fd, F_DUPFD_CLOEXEC, 64);
        if (raw >= 0) keep[nk++] = fcntl(raw, F_DUPFD_CLOEXEC, 64);
        for (int i = 0; i < nk; i++) if (keep[i] < 0 || dup2(keep[i], 3 + i) < 0) _exit(1);
        syscall(SYS_close_range, 3 + nk, ~0u, 0);
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); if (null > 2) close(null); }
        for (int i = 0; i < nout; i++) outs[i].fd = 4 + i;
        signal(SIGTERM, SIG_IGN);                  /* it drains the pipe until every writer is gone */
        signal(SIGINT, SIG_IGN);
        signal(SIGHUP, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        prctl(PR_SET_NAME, "lord-log");
        if (copy) copier(3, &outs[0]);
        else stamper(3, raw >= 0 ? 3 + nout + 1 : -1, outs, nout, scan_dir);
        _exit(0);
    }
    close(p[0]);
    if (pid < 0) { close(p[1]); return -1; }
    if (also_stdout) dup2(p[1], 1);
    dup2(p[1], 2);
    if (p[1] > 2) close(p[1]);
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (also_stdout) setvbuf(stdout, NULL, _IOLBF, 0);
    return 0;
}

int lordlog_session_start(void) {
    const char *d = lordlog_dir();
    if (!*d) return -1;
    static Out outs[2];
    outs[0] = (Out){ .fd = -1, .dflt = "compositor" };
    outs[1] = (Out){ .fd = -1, .dflt = "shell" };
    if ((size_t)snprintf(outs[0].path, sizeof outs[0].path, "%s/compositor.log", d) >= sizeof outs[0].path ||
        (size_t)snprintf(outs[1].path, sizeof outs[1].path, "%s/shell.log", d) >= sizeof outs[1].path) return -1;
    for (int i = 0; i < 2; i++) {
        lordlog_rotate(outs[i].path);
        outs[i].fd = lordlog_open(outs[i].path);
    }
    int r = outs[0].fd >= 0 && outs[1].fd >= 0 ? start(outs, 2, -1, d, 1, 0) : -1;
    for (int i = 0; i < 2; i++) if (outs[i].fd >= 0) close(outs[i].fd);
    return r;
}

static int journal = -1;

int lordlog_journal_fd(void) { return journal; }

int lordlog_system_start(const char *name) {
    if (!lordlog_name_ok(name) || journal >= 0) return -1;
    static Out out;
    out = (Out){ .fd = -1, .dflt = name };
    if ((size_t)snprintf(out.path, sizeof out.path, LORDLOG_ROOT "/%s.log", name) >= sizeof out.path) return -1;
    int r = open_trusted(AT_FDCWD, LORDLOG_ROOT, 0755, 0, 1);
    if (r < 0) return -1;
    char file[LORDLOG_NAME_MAX + 8];
    snprintf(file, sizeof file, "%s.log", name);
    out.fd = openat(r, file, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC, 0640);
    close(r);
    struct stat st;
    if (out.fd < 0) return -1;
    if (fstat(out.fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1) { close(out.fd); return -1; }
    int j = fcntl(2, F_DUPFD_CLOEXEC, 3);
    int rc = j >= 0 ? start(&out, 1, j, NULL, 0, 0) : -1;
    close(out.fd);
    if (rc) { if (j >= 0) close(j); return -1; }
    journal = j;
    return 0;
}

int lordlog_app_start(const char *sub, const char *name) {
    static Out out;
    out = (Out){ .fd = -1, .dflt = "app" };
    if (lordlog_path(sub, name, out.path, sizeof out.path)) return -1;
    struct stat a, b;
    /* only when stderr is that very file (app_spawn opened it): then it is the only writer's way in */
    if (fstat(2, &a) || lstat(out.path, &b) || !S_ISREG(a.st_mode) || !S_ISREG(b.st_mode) ||
        a.st_dev != b.st_dev || a.st_ino != b.st_ino) return -1;
    out.fd = 2;
    return start(&out, 1, -1, NULL, 1, 1);
}
