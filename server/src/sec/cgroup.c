/* cgroup.c — per-app cgroup v2 scope (see cgroup.h). All through files in /sys/fs/cgroup:
 * no libsystemd or D-Bus. Degrades with a warning if delegation or a controller is missing. */
#include "cgroup.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CG_ROOT "/sys/fs/cgroup"

static int write_file(const char *path, const char *val) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = strlen(val);
    ssize_t w = write(fd, val, n);
    int saved = errno;
    close(fd);
    errno = saved;
    return w == (ssize_t)n ? 0 : -1;
}

/* cgroup v2 path of the process (the "0::<path>" line of /proc/self/cgroup). */
const char *cgroup_self(void) {
    static char path[PATH_MAX];
    if (path[0]) return path;
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (!f) return "";
    char line[PATH_MAX];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "0::", 3)) continue;
        char *p = line + 3, *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        snprintf(path, sizeof path, "%s", p);
        break;
    }
    fclose(f);
    return path;
}

/* Directory where the app scope is created. In the delegated session, lord-server runs in the leaf
 * ".../lordos-session-<uid>/server": scopes go as siblings, in the parent directory (which
 * has the controllers enabled). If the parent isn't writable, its own cgroup is tried
 * (works if it has no controllers). If neither, there's no delegated subtree. */
static int base_dir(char *out, size_t n) {
    const char *rel = cgroup_self();
    if (!rel[0]) return -1;
    char self[PATH_MAX], parent[PATH_MAX];
    if ((size_t)snprintf(self, sizeof self, CG_ROOT "%s", rel) >= sizeof self) return -1;
    snprintf(parent, sizeof parent, "%s", self);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent + strlen(CG_ROOT)) {
        *slash = 0;
        if (!access(parent, W_OK)) { snprintf(out, n, "%s", parent); return 0; }
    }
    if (!access(self, W_OK)) { snprintf(out, n, "%s", self); return 0; }
    errno = EACCES;
    return -1;
}

/* Makes sure the controller is in the base directory's cgroup.subtree_control (so memory and
 * CPU can be limited in the child scopes). Best effort. */
static void enable_controllers(const char *base) {
    char path[PATH_MAX];
    if ((size_t)snprintf(path, sizeof path, "%s/cgroup.subtree_control", base) < sizeof path)
        write_file(path, "+memory +cpu +pids");
}

/* Writes val into file `leaf` inside directory dirfd (openat avoids building long paths). */
static int write_at(int dirfd, const char *leaf, const char *val) {
    int fd = openat(dirfd, leaf, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = strlen(val);
    ssize_t w = write(fd, val, n);
    int saved = errno;
    close(fd);
    errno = saved;
    return w == (ssize_t)n ? 0 : -1;
}

/* Is this a scope name lord-sessiond agrees to create? "app-lordos-<id>.scope" (Linux and .lpk
 * apps, lord-media) or "lordos-<helper>.scope" (lord-doc, lord-vault), never "lordos-session-…". */
int cgroup_scope_name_ok(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (n < 8 || n > 90 || strcmp(name + n - 6, ".scope")) return 0;
    if (strncmp(name, "app-lordos-", 11) && (strncmp(name, "lordos-", 7) || !strncmp(name, "lordos-session-", 15))) return 0;
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '_' || *p == '@')) return 0;
    return !strstr(name, "..");
}

/* Ask lord-sessiond (root) over CGROUP_SOCK to move THIS process into the scope. Returns 0 when
 * placed, -1 when refused or unanswered, -2 when there is no lord-sessiond (development). In a
 * lord-sessiond session (LORDOS_SESSIOND) it waits up to 3 s for a restarting lord-sessiond. */
static int enter_via_sessiond(const char *name, const CgroupLimits *lim) {
    int managed = getenv("LORDOS_SESSIOND") != NULL;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", CGROUP_SOCK);
    int fd = -1;
    for (int i = 0; i < 30; i++) {
        fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd < 0) return managed ? -1 : -2;
        if (!connect(fd, (struct sockaddr *)&a, sizeof a)) break;
        int e = errno;
        close(fd);
        fd = -1;
        if (!managed && e == ENOENT) return -2;
        if (e != ENOENT && e != ECONNREFUSED && e != EAGAIN) break;
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (fd < 0) { fprintf(stderr, "cgroup: lord-sessiond does not answer (%s); %s runs without a scope\n", CGROUP_SOCK, name); return -1; }
    struct timeval tv = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req[160], rep[128];
    int n = snprintf(req, sizeof req, "enter %s %ld %ld %d", name, lim ? lim->mem_high_mb : 0, lim ? lim->mem_max_mb : 0, lim ? lim->cpu_percent : 0);
    ssize_t r = n > 0 && n < (int)sizeof req ? send(fd, req, (size_t)n, MSG_NOSIGNAL) : -1;
    if (r == n) r = recv(fd, rep, sizeof rep - 1, 0);
    close(fd);
    if (r <= 0) { fprintf(stderr, "cgroup: no reply from lord-sessiond; %s runs without a scope\n", name); return -1; }
    rep[r] = 0;
    if (strncmp(rep, "ok", 2)) { fprintf(stderr, "cgroup: lord-sessiond refused %s: %s\n", name, rep); return -1; }
    return 0;
}

int cgroup_enter(const char *name, const CgroupLimits *lim) {
    char base[PATH_MAX], scope[PATH_MAX], val[64];
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '_' || *p == '@'))
            { fprintf(stderr, "cgroup: invalid name %s\n", name); return -1; }
    /* With lord-sessiond the scopes belong to root (root-owned cgroup.procs): it creates them and
     * moves us, so an app cannot migrate to another cgroup of the session. Without it
     * (development) we fall back to the old self-migration. */
    int via = enter_via_sessiond(name, lim);
    if (via != -2) return via;
    if (base_dir(base, sizeof base)) { fprintf(stderr, "cgroup: no delegated subtree (%s); the app runs without a scope\n", errno == EACCES ? "not writable" : strerror(errno)); return -1; }
    enable_controllers(base);
    if ((size_t)snprintf(scope, sizeof scope, "%s/%s", base, name) >= sizeof scope) return -1;
    if (mkdir(scope, 0755) && errno != EEXIST) { fprintf(stderr, "cgroup: could not create %s: %s\n", name, strerror(errno)); return -1; }
    int dir = open(scope, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) { fprintf(stderr, "cgroup: could not open %s: %s\n", name, strerror(errno)); return -1; }
    /* limits before moving the process (best effort: the controller may be missing) */
    if (lim) {
        if (lim->mem_high_mb) { snprintf(val, sizeof val, "%ld", lim->mem_high_mb * 1024L * 1024L); write_at(dir, "memory.high", val); }
        if (lim->mem_max_mb) { snprintf(val, sizeof val, "%ld", lim->mem_max_mb * 1024L * 1024L); write_at(dir, "memory.max", val); }
        if (lim->cpu_percent) { snprintf(val, sizeof val, "%ld 100000", (long)lim->cpu_percent * 1000L); write_at(dir, "cpu.max", val); }
    }
    int r = write_at(dir, "cgroup.procs", "0");
    close(dir);
    if (r) { fprintf(stderr, "cgroup: could not enter %s: %s\n", name, strerror(errno)); return -1; }
    return 0;
}

/* chown root:root a cgroup directory and every control file in it (dirfd = that directory). */
static void chown_root_dir(int dirfd) {
    fchown(dirfd, 0, 0);
    int d = dup(dirfd);
    DIR *dir = d >= 0 ? fdopendir(d) : NULL;
    if (!dir) { if (d >= 0) close(d); return; }
    struct dirent *e;
    while ((e = readdir(dir))) if (e->d_name[0] != '.') fchownat(dirfd, e->d_name, 0, 0, AT_SYMLINK_NOFOLLOW);
    closedir(dir);
}

void cgroup_harden_session(int sessfd) {
    if (geteuid() != 0 || sessfd < 0) return;
    chown_root_dir(sessfd);
    int d = dup(sessfd);
    DIR *dir = d >= 0 ? fdopendir(d) : NULL;
    if (!dir) { if (d >= 0) close(d); return; }
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.' || e->d_type != DT_DIR) continue;
        int c = openat(sessfd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (c >= 0) { chown_root_dir(c); close(c); }
    }
    closedir(dir);
}

/* Service root: its own cgroup, or its parent if we're in the "supervisor" leaf created by
 * cgroup_service_init(). */
static int service_root(char *out, size_t n) {
    const char *rel = cgroup_self();
    if (!rel[0]) return -1;
    if ((size_t)snprintf(out, n, CG_ROOT "%s", rel) >= n) return -1;
    size_t l = strlen(out);
    static const char leaf[] = "/supervisor";
    if (l > sizeof leaf - 1 && !strcmp(out + l - (sizeof leaf - 1), leaf)) out[l - (sizeof leaf - 1)] = 0;
    return 0;
}

int cgroup_service_init(void) {
    char svc[PATH_MAX];
    if (geteuid() != 0 || service_root(svc, sizeof svc) || access(svc, W_OK)) return -1;
    int fd = open(svc, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (mkdirat(fd, "supervisor", 0755) && errno != EEXIST) { close(fd); return -1; }
    int lfd = openat(fd, "supervisor", O_DIRECTORY | O_CLOEXEC);
    int r = lfd >= 0 ? write_at(lfd, "cgroup.procs", "0") : -1;
    if (lfd >= 0) close(lfd);
    close(fd);
    if (r) { fprintf(stderr, "cgroup: lordos.service without Delegate=yes (could not use the supervisor leaf)\n"); return -1; }
    enable_controllers(svc);        /* now the root has no processes: it can enable controllers */
    /* sessions that survived a restart (or were delegated by an older lord-sessiond): back to root */
    DIR *dir = opendir(svc);
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        unsigned u;
        char tail;
        if (sscanf(e->d_name, "lordos-session-%u%c", &u, &tail) != 1) continue;
        int sfd = openat(dirfd(dir), e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (sfd >= 0) { cgroup_harden_session(sfd); close(sfd); }
    }
    if (dir) closedir(dir);
    return 0;
}

int cgroup_enter_root(const char *name) {
    if (geteuid() != 0 || !cgroup_scope_name_ok(name)) return -1;
    const char *self = cgroup_self();                   /* ".../lordos-session-<uid>/server" */
    char dir[PATH_MAX];
    if (!self[0] || (size_t)snprintf(dir, sizeof dir, CG_ROOT "%s", self) >= sizeof dir) return -1;
    char *sl = strrchr(dir, '/');
    unsigned uid;
    char tail;
    if (!sl || strcmp(sl, "/server")) return -1;
    *sl = 0;
    const char *sess = strrchr(dir, '/');
    if (!sess || sscanf(sess + 1, "lordos-session-%u%c", &uid, &tail) != 1) return -1;
    int fd = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int fresh = !mkdirat(fd, name, 0755);
    if (!fresh && errno != EEXIST) { close(fd); return -1; }
    int lfd = openat(fd, name, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int r = lfd >= 0 ? write_at(lfd, "cgroup.procs", "0") : -1;
    if (lfd >= 0) close(lfd);
    close(fd);
    if (!r && fresh) cgroup_settle();
    return r;
}

void cgroup_settle(void) {
    struct timespec ts = { 0, 250 * 1000 * 1000 };
    nanosleep(&ts, NULL);
}

int cgroup_service_leaf(const char *name) {
    if (!name || strcmp(name, "netd")) return -1;     /* the only one (docs/MONITOR.md, "Essential system traffic") */
    char svc[PATH_MAX];
    if (geteuid() != 0 || service_root(svc, sizeof svc)) return -1;
    int fd = open(svc, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (mkdirat(fd, name, 0755) && errno != EEXIST) { close(fd); return -1; }
    int lfd = openat(fd, name, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int r = lfd >= 0 ? write_at(lfd, "cgroup.procs", "0") : -1;
    if (lfd >= 0) close(lfd);
    close(fd);
    return r;
}

int cgroup_delegate_session(unsigned uid, unsigned gid) {
    char svc[PATH_MAX], sess[PATH_MAX];
    (void)gid;
    if (service_root(svc, sizeof svc)) return -1;
    if (access(svc, W_OK)) { fprintf(stderr, "cgroup: lordos.service without Delegate=yes; session %u runs without a subtree\n", uid); return -1; }
    enable_controllers(svc);
    if ((size_t)snprintf(sess, sizeof sess, "%s/lordos-session-%u", svc, uid) >= sizeof sess) return -1;
    if (mkdir(sess, 0755) && errno != EEXIST) { fprintf(stderr, "cgroup: could not create the subtree of session %u: %s\n", uid, strerror(errno)); return -1; }
    enable_controllers(sess);
    int sfd = open(sess, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (sfd < 0) return -1;
    if (mkdirat(sfd, "server", 0755) && errno != EEXIST) { close(sfd); return -1; }
    int lfd = openat(sfd, "server", O_DIRECTORY | O_CLOEXEC);
    /* The compositor weighs ten times an app scope or helper: with the default weight, an app keeping
     * every core busy took half of the compositor's CPU whatever its nice value (nice only counts inside
     * one cgroup), and a Raspberry Pi 5's desktop fell from 59 to 24 frames a second
     * (docs/BENCHMARK.md, "The desktop under load"). Best effort: without the cpu controller it stays. */
    if (lfd >= 0) write_at(lfd, "cpu.weight", CG_SERVER_CPU_WEIGHT);
    if (lfd < 0 || write_at(lfd, "cgroup.procs", "0")) { fprintf(stderr, "cgroup: could not move session %u to its leaf: %s\n", uid, strerror(errno)); if (lfd >= 0) close(lfd); close(sfd); return -1; }
    /* The subtree stays root-owned (it used to be chowned to the user). lord-sessiond creates the
     * app scopes on request (cgroup_place_serve) with a root-owned cgroup.procs, so no user
     * process can migrate to another cgroup of the session (see docs/PRIVACY.md). */
    close(lfd);
    cgroup_harden_session(sfd);
    close(sfd);
    fprintf(stderr, "cgroup: session %u in lordos-session-%u (root-owned scopes)\n", uid, uid);
    return 0;
}

int app_scope_enter(const char *id) {
    char name[96];
    if (snprintf(name, sizeof name, "app-lordos-%s.scope", id) >= (int)sizeof name || !cgroup_scope_name_ok(name)) {
        fprintf(stderr, "cgroup: %s can't be an app scope\n", id);
        return -1;
    }
    CgroupLimits lim = { .mem_high_mb = 0, .mem_max_mb = 0, .cpu_percent = 0 };   /* Linux apps: no limit, attribution only */
    return cgroup_enter(name, &lim);
}

/* ---- lord-sessiond side: placing a session process into its app scope ---- */

/* "0::<path>" of /proc/<pid>/cgroup, or -1. */
static int proc_cgroup(pid_t pid, char *out, size_t n) {
    char p[64], line[PATH_MAX];
    snprintf(p, sizeof p, "/proc/%d/cgroup", (int)pid);
    FILE *f = fopen(p, "re");
    if (!f) return -1;
    int r = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "0::", 3)) continue;
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if ((size_t)snprintf(out, n, "%s", line + 3) < n) r = 0;
        break;
    }
    fclose(f);
    return r;
}

/* The executable of the peer must be a LordOS binary from bindir (lord-server, lord-media,
 * lord-doc…). A binary replaced while it runs shows up as "<path> (deleted)": still accepted. */
static int exe_trusted(pid_t pid, const char *bindir, int insecure) {
    char p[64], exe[PATH_MAX];
    snprintf(p, sizeof p, "/proc/%d/exe", (int)pid);
    ssize_t l = readlink(p, exe, sizeof exe - 1);
    if (l <= 0) return 0;
    exe[l] = 0;
    static const char del[] = " (deleted)";
    size_t el = strlen(exe);
    if (el > sizeof del - 1 && !strcmp(exe + el - (sizeof del - 1), del)) exe[el - (sizeof del - 1)] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash) return 0;
    *slash = 0;
    if (strcmp(exe, bindir)) return 0;
    if (insecure) return 1;
    struct stat st;
    return stat(bindir, &st) == 0 && st.st_uid == 0 && !(st.st_mode & (S_IWGRP | S_IWOTH));
}

static void reply(int fd, const char *msg) { send(fd, msg, strlen(msg), MSG_NOSIGNAL | MSG_DONTWAIT); }

int cgroup_place_serve(int cfd, const char *bindir, int insecure) {
    struct ucred cr;
    socklen_t cl = sizeof cr;
    if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) || cr.pid <= 0) { reply(cfd, "err peer"); return -1; }
    struct timeval tv = { 1, 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req[256], name[96];
    ssize_t r = recv(cfd, req, sizeof req - 1, 0);
    if (r <= 0) return -1;
    req[r] = 0;
    long mh = 0, mm = 0;
    int cpu = 0;
    if (sscanf(req, "enter %95s %ld %ld %d", name, &mh, &mm, &cpu) != 4 || !cgroup_scope_name_ok(name)) { reply(cfd, "err request"); return -1; }
    if (cr.uid < 1000 || cr.uid >= 60000) { reply(cfd, "err uid"); return -1; }
    /* The peer must still be in its session's «server» leaf: lord-server itself, or a child it
     * forked right before exec. A process already in an app scope (an app) can never ask. */
    char svc[PATH_MAX], want[PATH_MAX], have[PATH_MAX];
    if (service_root(svc, sizeof svc)) { reply(cfd, "err service"); return -1; }
    const char *rel = svc + strlen(CG_ROOT);
    if ((size_t)snprintf(want, sizeof want, "%s/lordos-session-%u/server", rel, (unsigned)cr.uid) >= sizeof want) return -1;
    if (proc_cgroup(cr.pid, have, sizeof have) || strcmp(have, want)) { reply(cfd, "err not in the session leaf"); return -1; }
    if (!exe_trusted(cr.pid, bindir, insecure)) { reply(cfd, "err untrusted executable"); return -1; }
    char sess[PATH_MAX];
    if ((size_t)snprintf(sess, sizeof sess, "%s/lordos-session-%u", svc, (unsigned)cr.uid) >= sizeof sess) return -1;
    int sfd = open(sess, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (sfd < 0) { reply(cfd, "err no session subtree"); return -1; }
    if (mkdirat(sfd, name, 0755) && errno != EEXIST) { close(sfd); reply(cfd, "err mkdir"); return -1; }
    int dfd = openat(sfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(sfd);
    if (dfd < 0) { reply(cfd, "err open"); return -1; }
    chown_root_dir(dfd);            /* a scope left by an older lord-sessiond may be the user's */
    char val[64];
    if (mh > 0 && mh <= 65536) { snprintf(val, sizeof val, "%ld", mh * 1024L * 1024L); write_at(dfd, "memory.high", val); }
    if (mm > 0 && mm <= 65536) { snprintf(val, sizeof val, "%ld", mm * 1024L * 1024L); write_at(dfd, "memory.max", val); }
    if (cpu > 0 && cpu <= 6400) { snprintf(val, sizeof val, "%ld 100000", (long)cpu * 1000L); write_at(dfd, "cpu.max", val); }
    snprintf(val, sizeof val, "%d", (int)cr.pid);
    int w = write_at(dfd, "cgroup.procs", val);
    close(dfd);
    if (w) { reply(cfd, "err move"); return -1; }
    /* the peer is blocked waiting for this reply, so the pid cannot have been reused meanwhile */
    reply(cfd, "ok");
    return 0;
}
