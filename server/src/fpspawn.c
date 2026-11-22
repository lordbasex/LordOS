/* fpspawn.c — org.freedesktop.portal.Flatpak served by lord-server (see fpspawn.h; docs/FLATPAK.md, "Sub-sandboxes";
 * AGENTS.md E9).
 *
 * Who asks: the bus gives the caller's pid (GetConnectionUnixProcessID): a Flatpak app reaches the session bus only
 * through its xdg-dbus-proxy, which runs in the app's own root-owned scope (app-lordos-fp-<id>.scope). Only such a
 * caller is served, and only as that app: the new process is `flatpak run` of the same app id, placed in the same
 * scope, entering the namespaces of the same LordOS view (its user, mount, IPC and PID namespaces, taken from the
 * view's first process), with Landlock's scoping applied again and LordOS's never-granted options and current
 * restrictions after whatever the app asked for (fpak_spawn_argv). The app's command and arguments come after the app
 * id, so flatpak never reads them as its own options. Nothing reaches the host: no command outside Flatpak's sandbox,
 * no other app id, no path outside the app's own ~/.var/app/<id>/sandbox, and no custom /app or /usr (usr-fd, app-fd
 * and the expose-fd options are refused).
 *
 * The process tree: lord-server asks the app's own Spawn helper (fpspawner.h), which the launcher forked inside the
 * app's view right after the app's Landlock scoping: the helper forks `flatpak run` (whose pid the caller gets), so the
 * new process is in the app's scope, view namespaces and Landlock domain without lord-server entering any namespace.
 * Being in the app's own domain is what lets `flatpak run --parent-pid` open the app's /run/.userns and PID namespace
 * (Chromium's and zypak's EXPOSE_PIDS / SHARE_PIDS); a fresh domain can't. The helper reports how it ended, and
 * lord-server sends that as SpawnExited to the caller alone. */
#include "fpspawn.h"
#include "fpak.h"
#include "fpspawner.h"
#include "lordlog.h"
#include "sec/appid.h"
#include "sec/cgroup.h"
#include "sec/restrict.h"
#include "sec/sandbox.h"
#include "fw/rules.h"
#include "vault/dbus.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif

extern char **environ;

#define BUS_NAME "org.freedesktop.portal.Flatpak"
#define OBJ      "/org/freedesktop/portal/Flatpak"
#define VERSION  6u
#define SUPPORTS 1u                     /* FLATPAK_SPAWN_SUPPORT_FLAGS_EXPOSE_PIDS */
#define MAX_PENDING 16
#define PENDING_MS 3000
#define MAX_KIDS 128
#define MAX_PER_APP FPSW_MAX_KIDS
#define MAX_VIEWS 32

static DConn conn;
static int up;

static struct { DMsg call; uint32_t serial; double since; } pending[MAX_PENDING];
static int npending;

typedef struct {
    int live;
    pid_t pid;                          /* `flatpak run` in our PID namespace */
    int pidfd;
    int view;                           /* the views[] entry whose helper started it */
    uint32_t seq;
    char sender[64], id[FPAK_ID_MAX + 1];
    uint32_t flags;
    int inst_fd;                        /* NOTIFY_START: the read end of --instance-id-fd, or -1 */
    pid_t root;                         /* the view's first process (for the relative pid) */
    int parent_pid;                     /* the caller's instance, in the view's PID namespace (EXPOSE/SHARE_PIDS) */
    char inst[24];
    size_t inst_len;
    int want_started;                   /* the instance id came, SpawnStarted not sent yet */
    double since;
} Kid;
static Kid kids[MAX_KIDS];

/* the Spawn helpers of running Flatpak apps (fpspawner.h): one per app view, known by its pid once it said hello */
typedef struct { int fd; pid_t pid; char id[FPAK_ID_MAX + 1]; } View;
static View views[MAX_VIEWS];
static uint32_t next_seq = 1;

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* ---- reading the caller's processes ---- */

/* A small file through /proc (never following a last link), NUL-terminated. Bytes read, or -1. */
static ssize_t read_small(const char *path, char *out, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t got = 0;
    for (ssize_t r; got < cap - 1 && (r = read(fd, out + got, cap - 1 - got)) > 0;) got += (size_t)r;
    close(fd);
    out[got] = 0;
    return (ssize_t)got;
}

static int cgroup_of(pid_t pid, char *out, size_t cap) {
    char p[64], buf[1024];
    snprintf(p, sizeof p, "/proc/%d/cgroup", (int)pid);
    if (read_small(p, buf, sizeof buf) <= 0) return -1;
    char *l = strstr(buf, "0::");
    if (!l) return -1;
    l += 3;
    char *nl = strchr(l, '\n');
    if (nl) *nl = 0;
    return snprintf(out, cap, "%s", l) < (int)cap ? 0 : -1;
}

static pid_t ppid_of(pid_t pid) {
    char p[64], buf[2048];
    snprintf(p, sizeof p, "/proc/%d/status", (int)pid);
    if (read_small(p, buf, sizeof buf) <= 0) return -1;
    char *l = strstr(buf, "\nPPid:");
    return l ? (pid_t)atoi(l + 6) : -1;
}

/* The NSpid fields of pid (outermost first). How many, or -1. */
static int nspids(pid_t pid, int *out, int max) {
    char p[64], buf[2048];
    snprintf(p, sizeof p, "/proc/%d/status", (int)pid);
    if (read_small(p, buf, sizeof buf) <= 0) return -1;
    char *l = strstr(buf, "\nNSpid:");
    if (!l) return -1;
    l += 7;
    int n = 0;
    for (char *end; n < max; l = end) {
        long v = strtol(l, &end, 10);
        if (end == l) break;
        out[n++] = (int)v;
    }
    return n;
}

/* The first process of the caller's view: its topmost ancestor still in the same scope. */
static pid_t view_root(pid_t pid) {
    char cg[512], pcg[512];
    if (cgroup_of(pid, cg, sizeof cg)) return -1;
    pid_t cur = pid;
    for (int i = 0; i < 64; i++) {
        pid_t pp = ppid_of(cur);
        if (pp <= 1 || cgroup_of(pp, pcg, sizeof pcg) || strcmp(cg, pcg)) return cur;
        cur = pp;
    }
    return -1;
}

/* key=value from a [group] of a small key file (the caller's /.flatpak-info) */
static int keyfile_get(const char *text, const char *group, const char *key, char *out, size_t cap) {
    char hdr[64];
    snprintf(hdr, sizeof hdr, "[%s]", group);
    const char *g = strstr(text, hdr);
    if (!g || (g != text && g[-1] != '\n')) return -1;
    size_t kl = strlen(key);
    for (const char *l = strchr(g, '\n'); l && l[1] && l[1] != '['; l = strchr(l + 1, '\n')) {
        if (strncmp(l + 1, key, kl) || l[1 + kl] != '=') continue;
        const char *v = l + 2 + kl, *e = strchr(v, '\n');
        size_t n = e ? (size_t)(e - v) : strlen(v);
        if (n >= cap) return -1;
        memcpy(out, v, n);
        out[n] = 0;
        return 0;
    }
    return -1;
}

static int digits_ok(const char *s) {
    if (!s || !s[0] || strlen(s) > 20) return 0;
    for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* "child-pid": N in an instance's bwrapinfo.json, read inside the view */
static int instance_child_pid(pid_t root, const char *instance) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char path[PATH_MAX], buf[4096];
    if (!rt || rt[0] != '/' || !digits_ok(instance)) return -1;
    snprintf(path, sizeof path, "/proc/%d/root%s/.flatpak/%s/bwrapinfo.json", (int)root, rt, instance);
    if (read_small(path, buf, sizeof buf) <= 0) return -1;
    char *k = strstr(buf, "\"child-pid\"");
    if (!k) return -1;
    k = strchr(k, ':');
    int v = k ? atoi(k + 1) : 0;
    return v > 0 ? v : -1;
}

/* ---- replies ---- */

static void reply_error(const DMsg *m, const char *name, const char *text) { dbus_error(&conn, m, name, text); }

static void send_to(const char *dest, const char *member, const char *sig, const DBuf *b) {
    DHdr h = { .type = DBUS_SIGNAL, .path = OBJ, .iface = BUS_NAME, .member = member, .dest = dest, .sig = sig };
    dbus_send(&conn, &h, b);
}

static void match(const char *sender, int add) {
    char rule[256];
    snprintf(rule, sizeof rule, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
             "member='NameOwnerChanged',arg0='%s'", sender);
    DBuf b;
    dw_init(&b);
    dw_str(&b, rule);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus",
               .member = add ? "AddMatch" : "RemoveMatch", .dest = "org.freedesktop.DBus", .sig = "s" };
    dbus_send(&conn, &h, &b);
    dw_free(&b);
}

/* ---- Spawn ---- */

typedef struct {
    char cwd[FPAK_SPAWN_STR_MAX];
    const char *args[FPAK_SPAWN_ARGS_MAX + 1], *env[FPAK_SPAWN_ENV_MAX + 1], *unset[FPAK_SPAWN_ENV_MAX + 1];
    const char *expose[FPAK_SPAWN_EXPOSE_MAX + 1], *expose_ro[FPAK_SPAWN_EXPOSE_MAX + 1];
    char *store;                        /* the argument strings, NUL-separated */
    size_t used, cap;
    uint32_t target[DBUS_MAX_FDS], handle[DBUS_MAX_FDS];
    int nfd;
    uint32_t flags, sandbox_flags;
    int has_cwd;
    char refused[48];                   /* the option not served, for the log */
    int exposed_ignored;                /* sandbox-expose-fd(-ro) handles left out */
} Req;

static const char *keep(Req *q, const uint8_t *p, size_t n) {
    if (memchr(p, 0, n) || q->used + n + 1 > q->cap) return NULL;
    char *s = q->store + q->used;
    memcpy(s, p, n);
    s[n] = 0;
    q->used += n + 1;
    return s;
}

/* a bytestring argument (ay, with its trailing NUL) */
static const char *keep_bytes(Req *q, DRd *r) {
    uint32_t n = 0;
    const uint8_t *p = dr_bytes(r, &n);
    if (r->err || !p || !n || p[n - 1]) return NULL;
    return keep(q, p, n - 1);
}

static int read_strv(Req *q, DRd *r, const char **out, int max) {
    size_t end = dr_arr(r, 4);
    int n = 0;
    while (!r->err && dr_more(r, end)) {
        uint32_t l = 0;
        const char *s = dr_str(r, &l);
        if (r->err || !s || n >= max || !(out[n++] = keep(q, (const uint8_t *)s, l))) return -1;
    }
    out[n] = NULL;
    return r->err ? -1 : 0;
}

/* Spawn(ay cwd, aay argv, a{uh} fds, a{ss} envs, u flags, a{sv} options). 0 read, -1 bad, -2 unsupported option. */
static int read_spawn(const DMsg *m, Req *q) {
    DRd r;
    dr_init(&r, m);
    const char *cwd = keep_bytes(q, &r);
    if (!cwd) return -1;
    if (cwd[0]) { snprintf(q->cwd, sizeof q->cwd, "%s", cwd); q->has_cwd = 1; }
    size_t end = dr_arr(&r, 4);
    int na = 0;
    while (!r.err && dr_more(&r, end)) {
        const char *a = keep_bytes(q, &r);
        if (!a || na >= FPAK_SPAWN_ARGS_MAX) return -1;
        q->args[na++] = a;
    }
    q->args[na] = NULL;
    end = dr_arr(&r, 8);
    while (!r.err && dr_more(&r, end)) {
        dr_struct(&r);
        uint32_t t = dr_u32(&r), h = dr_u32(&r);
        if (r.err || q->nfd >= DBUS_MAX_FDS || t > 1023 || m->n_fds <= 0 || h >= (uint32_t)m->n_fds) return -1;   /* unsigned: never a negative index */
        for (int i = 0; i < q->nfd; i++) if (q->target[i] == t) return -1;
        q->target[q->nfd] = t;
        q->handle[q->nfd++] = h;
    }
    end = dr_arr(&r, 8);
    int ne = 0;
    while (!r.err && dr_more(&r, end)) {
        dr_struct(&r);
        uint32_t kl = 0, vl = 0;
        const char *k = dr_str(&r, &kl), *v = dr_str(&r, &vl);
        if (r.err || !k || !v || ne >= FPAK_SPAWN_ENV_MAX || kl + vl + 2 > FPAK_SPAWN_STR_MAX) return -1;
        char kv[FPAK_SPAWN_STR_MAX];
        snprintf(kv, sizeof kv, "%s=%s", k, v);
        if (!(q->env[ne++] = keep(q, (const uint8_t *)kv, strlen(kv)))) return -1;
    }
    q->env[ne] = NULL;
    q->flags = dr_u32(&r);
    q->unset[0] = q->expose[0] = q->expose_ro[0] = NULL;
    end = dr_arr(&r, 8);
    while (!r.err && dr_more(&r, end)) {
        dr_struct(&r);
        const char *k = dr_str(&r, NULL), *sig = dr_variant(&r);
        if (r.err || !k || !sig) return -1;
        /* sandbox-expose(-ro) not served: the path is in a folder the app writes, and a link there would make
         * Flatpak export whatever it points at (the session bus socket, the home). glycin doesn't use it. Nor
         * usr-fd / app-fd (a custom /usr or /app). */
        if (!strcmp(k, "unset-env") && !strcmp(sig, "as")) { if (read_strv(q, &r, q->unset, FPAK_SPAWN_ENV_MAX)) return -1; }
        else if (!strcmp(k, "sandbox-flags") && !strcmp(sig, "u")) q->sandbox_flags = dr_u32(&r);
        else if ((!strcmp(k, "sandbox-expose-fd") || !strcmp(k, "sandbox-expose-fd-ro")) && !strcmp(sig, "ah")) {
            /* Read and checked, then not served (as flatpak-portal does with an fd it can't resolve): Flatpak would be
             * given the fd's path (--filesystem), which the app can swap for a link to anything its view reads between
             * the check and the bind. The Chromium browser asks this for its WidevineCdm folder (DRM video): it
             * starts and browses without it. */
            size_t e2 = dr_arr(&r, 4);
            while (!r.err && dr_more(&r, e2)) {
                uint32_t h = dr_u32(&r);
                if (r.err || h >= (uint32_t)m->n_fds) return -1;
                q->exposed_ignored++;
            }
        }
        else {                          /* sandbox-expose(-ro), usr-fd, app-fd, sandbox-expose-fd(-ro) and anything else: not served */
            snprintf(q->refused, sizeof q->refused, "%.40s", k);
            for (char *c = q->refused; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) *c = '?';
            return -2;
        }
    }
    return r.err ? -1 : 0;
}

static int count_app(const char *id) {
    int n = 0;
    for (int i = 0; i < MAX_KIDS; i++) n += kids[i].live && !strcmp(kids[i].id, id);
    return n;
}

/* ---- the apps' Spawn helpers (fpspawner.h) ---- */

/* the pid a pidfd refers to, in our PID namespace */
static pid_t pid_of_pidfd(int fd) {
    char p[64], buf[1024];
    snprintf(p, sizeof p, "/proc/self/fdinfo/%d", fd);
    int f = open(p, O_RDONLY | O_CLOEXEC);
    if (f < 0) return -1;
    ssize_t n = read(f, buf, sizeof buf - 1);
    close(f);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *l = strstr(buf, "\nPid:");
    long v = l ? strtol(l + 5, NULL, 10) : -1;
    return v > 0 && v < INT_MAX ? (pid_t)v : -1;
}

/* the process ended (or its helper went): SpawnExited to the caller alone */
static void kid_exited(int i, uint32_t status) {
    Kid *k = &kids[i];
    if (!k->live) return;
    if (k->inst_fd >= 0) { close(k->inst_fd); k->inst_fd = -1; }
    DBuf b;
    dw_init(&b);
    dw_u32(&b, (uint32_t)k->pid);
    dw_u32(&b, status);                         /* the wait status, as flatpak-portal sends it */
    if (up) send_to(k->sender, "SpawnExited", "uu", &b);
    dw_free(&b);
    if (up && (k->flags & FPAK_SPAWN_WATCH_BUS) && k->sender[0]) {
        int others = 0;
        for (int j = 0; j < MAX_KIDS; j++) others += j != i && kids[j].live && !strcmp(kids[j].sender, k->sender);
        if (!others) match(k->sender, 0);
    }
    if (k->pidfd >= 0) close(k->pidfd);
    k->pidfd = -1;
    k->live = 0;
}

static void view_gone(int v) {
    if (views[v].fd >= 0) close(views[v].fd);
    views[v].fd = -1;
    views[v].pid = 0;
    for (int i = 0; i < MAX_KIDS; i++) if (kids[i].live && kids[i].view == v) kid_exited(i, SIGKILL);
}

/* One message from a helper, without waiting. 1 read (*fd: the fd it carried, or -1), 0 nothing yet, -1 gone. */
static int view_recv(int v, FpswMsg *m, int *fd) {
    *fd = -1;
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int) * 4)]; } u;
    struct iovec iov = { m, sizeof *m };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.b, .msg_controllen = sizeof u.b };
    ssize_t r = recvmsg(views[v].fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    if (r == 0) return -1;
    int bad = r != (ssize_t)sizeof *m || m->magic != FPSW_MAGIC || (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        for (int j = 0; j < k; j++) {
            int f;
            memcpy(&f, CMSG_DATA(c) + j * sizeof(int), sizeof f);
            if (*fd < 0 && !bad) *fd = f; else close(f);
        }
    }
    if (bad) { if (*fd >= 0) close(*fd); *fd = -1; return -1; }    /* never from our own helper: drop it */
    return 1;
}

/* a message that isn't the answer being waited for */
static void view_msg(int v, const FpswMsg *m, int fd) {
    if (m->type == FPSW_HELLO && !views[v].pid && fd >= 0) views[v].pid = pid_of_pidfd(fd);
    else if (m->type == FPSW_EXITED)
        for (int i = 0; i < MAX_KIDS; i++)
            if (kids[i].live && kids[i].view == v && kids[i].seq == m->seq) { kid_exited(i, (uint32_t)m->status); break; }
    if (fd >= 0) close(fd);
}

static void views_drain(void) {
    for (int v = 0; v < MAX_VIEWS; v++) {
        if (views[v].fd < 0) continue;
        FpswMsg m;
        int fd, r;
        while ((r = view_recv(v, &m, &fd)) == 1) view_msg(v, &m, fd);
        if (r < 0) view_gone(v);
    }
}

static int views_init;
static void views_setup(void) {
    if (views_init) return;
    views_init = 1;
    for (int v = 0; v < MAX_VIEWS; v++) views[v].fd = -1;
    for (int i = 0; i < MAX_KIDS; i++) { kids[i].pidfd = -1; kids[i].inst_fd = -1; }
}

void fpspawn_add_view(int fd, const char *id) {
    views_setup();
    views_drain();                              /* helpers that went free their entries */
    for (int v = 0; v < MAX_VIEWS; v++) {
        if (views[v].fd >= 0) continue;
        views[v].fd = fd;
        views[v].pid = 0;
        snprintf(views[v].id, sizeof views[v].id, "%s", id && fpak_id_ok(id) ? id : "");
        return;
    }
    close(fd);                                  /* no room: that app's Spawn calls are refused */
}

int fpspawn_pollfds(struct pollfd *p, int max) {
    int n = 0;
    for (int v = 0; views_init && v < MAX_VIEWS && n < max; v++) if (views[v].fd >= 0) p[n++] = (struct pollfd){ views[v].fd, POLLIN, 0 };
    return n;
}

void fpspawn_dispatch_views(void) { if (views_init) views_drain(); }

/* The helper of the caller's view: an app's own, in the same view (its first process). -1 if none. */
static int view_of(const char *id, pid_t root) {
    views_drain();
    for (int v = 0; v < MAX_VIEWS; v++)
        if (views[v].fd >= 0 && views[v].pid > 0 && !strcmp(views[v].id, id) && view_root(views[v].pid) == root) return v;
    return -1;
}

/* Asks the helper to start argv/envp with the app's fds placed. The new process's pidfd, or -1 (*step: why). */
static int view_spawn(int v, uint32_t seq, char *const argv[], char *const envp[], const Req *q, const DMsg *m, int inst_num,
                      int inst_w, int *step) {
    *step = 0;
    char *buf = malloc(FPSW_PAYLOAD_MAX);
    long n = buf ? fpsw_pack(argv, envp, buf, FPSW_PAYLOAD_MAX) : -1;
    int mfd = n > 0 ? memfd_create("lordos-spawn", MFD_CLOEXEC | MFD_ALLOW_SEALING) : -1;
    int ok = mfd >= 0 && write(mfd, buf, (size_t)n) == (ssize_t)n &&
             !fcntl(mfd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);
    free(buf);
    if (!ok) { if (mfd >= 0) close(mfd); *step = 90; return -1; }
    FpswMsg h;
    memset(&h, 0, sizeof h);
    h.magic = FPSW_MAGIC; h.type = FPSW_SPAWN; h.seq = seq; h.nfd = (uint32_t)q->nfd; h.inst_fd = inst_w >= 0 ? inst_num : -1;
    int fds[FPSW_MAX_FDS + 2], nf = 0;
    fds[nf++] = mfd;
    for (int i = 0; i < q->nfd; i++) { h.target[i] = q->target[i]; fds[nf++] = m->fds[q->handle[i]]; }
    if (inst_w >= 0) fds[nf++] = inst_w;
    union { struct cmsghdr c; char b[CMSG_SPACE(sizeof(int) * (FPSW_MAX_FDS + 2))]; } u;
    memset(&u, 0, sizeof u);
    struct iovec iov = { &h, sizeof h };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.b, .msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nf) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nf);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nf);
    ssize_t r;
    while ((r = sendmsg(views[v].fd, &mh, MSG_NOSIGNAL | MSG_DONTWAIT)) < 0 && errno == EINTR) {}
    close(mfd);
    if (r != (ssize_t)sizeof h) { *step = 91; return -1; }
    /* its answer, within 3 s; anything else it says meanwhile is handled as usual */
    double end = mono_ms() + 3000;
    for (;;) {
        FpswMsg a;
        int fd, got = view_recv(v, &a, &fd);
        if (got < 0) { view_gone(v); *step = 92; return -1; }
        if (got == 1 && a.seq == seq && (a.type == FPSW_STARTED || a.type == FPSW_FAILED)) {
            if (a.type == FPSW_STARTED && fd >= 0) return fd;
            if (fd >= 0) close(fd);
            *step = a.type == FPSW_FAILED ? a.status : 93;
            return -1;
        }
        if (got == 1) { view_msg(v, &a, fd); continue; }
        double left = end - mono_ms();
        if (left <= 0) { *step = 94; return -1; }
        struct pollfd pf = { views[v].fd, POLLIN, 0 };
        poll(&pf, 1, (int)left + 1);
    }
}

/* the environment of `flatpak run`: lord-server's, as the launcher gives it to a Flatpak app */
static int make_env(char **out, int max, int bus, int x11) {
    int n = 0;
    for (char **e = environ; *e && n < max - 1; e++) {
        if (!strncmp(*e, "XDG_ACTIVATION_TOKEN=", 21) || !strncmp(*e, "LORDOS_", 7)) continue;
        if (!bus && !strncmp(*e, "DBUS_SESSION_BUS_ADDRESS=", 25)) continue;
        if (!x11 && (!strncmp(*e, "DISPLAY=", 8) || !strncmp(*e, "XAUTHORITY=", 11))) continue;
        out[n++] = *e;
    }
    out[n] = NULL;
    return n;
}

static void do_spawn(DMsg *m, uint32_t caller) {
    char app[FW_APP_MAX], id[FPAK_ID_MAX + 1];
    views_setup();
    if (!caller || appid_of_pid(caller, app, sizeof app) != 0 || strncmp(app, "flatpak:", 8) || !fpak_id_ok(app + 8)) {
        reply_error(m, "org.freedesktop.DBus.Error.AccessDenied", "only a Flatpak app LordOS started may spawn");
        return;
    }
    memcpy(id, app + 8, strlen(app + 8) + 1);             /* fpak_id_ok: at most FPAK_ID_MAX bytes */
    if (count_app(id) >= MAX_PER_APP) { reply_error(m, "org.freedesktop.DBus.Error.LimitsExceeded", "too many processes"); return; }
    /* who it says it is, from its own /.flatpak-info, must be the app its scope says */
    char path[96], info[8192], name[FPAK_ID_MAX + 2], iid[24] = "";
    snprintf(path, sizeof path, "/proc/%u/root/.flatpak-info", caller);
    if (read_small(path, info, sizeof info) <= 0 || keyfile_get(info, "Application", "name", name, sizeof name) || strcmp(name, id) ||
        keyfile_get(info, "Instance", "instance-id", iid, sizeof iid) || !digits_ok(iid)) {
        reply_error(m, "org.freedesktop.DBus.Error.AccessDenied", "not a Flatpak instance");
        return;
    }
    pid_t root = view_root((pid_t)caller);
    if (root <= 0) { reply_error(m, "org.freedesktop.DBus.Error.Failed", "the app's view is gone"); return; }
    Req *q = calloc(1, sizeof *q);
    if (!q || !(q->store = malloc(q->cap = 256 * 1024))) { free(q); reply_error(m, "org.freedesktop.DBus.Error.NoMemory", "no memory"); return; }
    int rr = read_spawn(m, q);
    FpakSpawn sp = { .flags = q->flags, .sandbox_flags = q->sandbox_flags, .cwd = q->has_cwd ? q->cwd : NULL, .args = q->args,
                     .env = q->env, .unset_env = q->unset, .expose = q->expose, .expose_ro = q->expose_ro, .instance_fd = -1 };
    if (!rr && (q->flags & (FPAK_SPAWN_EXPOSE_PIDS | FPAK_SPAWN_SHARE_PIDS))) sp.parent_pid = instance_child_pid(root, iid);
    if (!rr && (q->flags & FPAK_SPAWN_NOTIFY_START)) {
        int n = 3;
        for (int again = 1; again;) { again = 0; for (int i = 0; i < q->nfd; i++) if ((int)q->target[i] == n) { n++; again = 1; } }
        sp.instance_fd = n;
    }
    /* the app's installation and LordOS's restrictions now */
    char arch[32], branch[FPAK_BRANCH_MAX], root_dir[PATH_MAX];
    const char *home = getenv("HOME");
    int found = -1;
    if (home && home[0] == '/' && snprintf(root_dir, sizeof root_dir, "%s/.local/share/flatpak", home) < (int)sizeof root_dir)
        found = fpak_installed(root_dir, id, arch, sizeof arch, branch, sizeof branch, NULL, 0);
    if (found) found = fpak_installed("/var/lib/flatpak", id, arch, sizeof arch, branch, sizeof branch, NULL, 0);
    static const uint32_t flags[] = { FW_R_NO_NETWORK, FW_R_NO_USB, FW_R_NO_BLUETOOTH, FW_R_NO_FILES, FW_R_NO_MIC, FW_R_NO_CAMERA };
    uint32_t restricted = 0;
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) if (restrict_blocks(app, flags[i])) restricted |= flags[i];
    int dev = (restricted & (FW_R_NO_FILES | FW_R_NO_MIC | FW_R_NO_CAMERA | FW_R_NO_USB | FW_R_NO_BLUETOOTH)) != 0;
    char xs[PATH_MAX];
    const char *d = getenv("DISPLAY");
    struct stat st;
    int x11 = !dev && d && d[0] == ':' && snprintf(xs, sizeof xs, "/proc/%d/root/tmp/.X11-unix/X%s", (int)root, d + 1) < (int)sizeof xs &&
              !lstat(xs, &st) && S_ISSOCK(st.st_mode);
    FpakRun run = { .id = id, .arch = arch, .branch = branch, .restricted = restricted, .home = home, .x11 = x11 };
    static char *argv[FPAK_SPAWN_ARGV_MAX], *envp[512];
    char *abuf = malloc(256 * 1024);
    int argc = rr || found || !abuf ? -1 : fpak_spawn_argv(&run, &sp, argv, abuf, 256 * 1024);
    if (argc < 0 || ((q->flags & (FPAK_SPAWN_EXPOSE_PIDS | FPAK_SPAWN_SHARE_PIDS)) && sp.parent_pid <= 0)) {
        reply_error(m, rr == -2 ? "org.freedesktop.DBus.Error.NotSupported" : "org.freedesktop.DBus.Error.InvalidArgs",
                    rr == -2 ? "option not supported" : "request refused");
        fprintf(stderr, "fpspawn: %s: a Spawn was refused (%s%s)\n", id, rr == -2 ? "option not served: " : found ? "not installed" : "invalid request",
                rr == -2 ? q->refused : "");
        free(abuf); free(q->store); free(q);
        return;
    }
    make_env(envp, 512, !dev, x11);
    int slot = -1;
    for (int i = 0; i < MAX_KIDS && slot < 0; i++) if (!kids[i].live) slot = i;
    int inst[2] = { -1, -1 };
    char logp[PATH_MAX];
    int logfd = lordlog_path("apps", id, logp, sizeof logp) ? -1 : lordlog_open(logp);
    int v = view_of(id, root);
    if (v < 0) {
        reply_error(m, "org.freedesktop.DBus.Error.Failed", "this app has no Spawn helper (open it again)");
        fprintf(stderr, "fpspawn: %s: no Spawn helper in the caller's view\n", id);
        goto out;
    }
    if (slot < 0 || (sp.instance_fd >= 0 && pipe2(inst, O_CLOEXEC))) {
        reply_error(m, "org.freedesktop.DBus.Error.LimitsExceeded", "too many processes");
        goto out;
    }
    uint32_t seq = next_seq++;
    if (!next_seq) next_seq = 1;
    int step = 0, pidfd = view_spawn(v, seq, argv, envp, q, m, sp.instance_fd, inst[1], &step);
    if (inst[1] >= 0) { close(inst[1]); inst[1] = -1; }
    pid_t kid = pidfd >= 0 ? pid_of_pidfd(pidfd) : -1;
    /* only a process in the caller's own scope is ever signalled for it (SpawnSignal, --watch-bus) */
    char kcg[512], rcg[512];
    if (kid > 0 && (cgroup_of(kid, kcg, sizeof kcg) || cgroup_of(root, rcg, sizeof rcg) || strcmp(kcg, rcg))) { kid = -1; step = 95; }
    if (kid > 0) {
        Kid *k = &kids[slot];
        memset(k, 0, sizeof *k);
        k->live = 1;
        k->pid = kid;
        k->pidfd = pidfd;
        k->view = v;
        k->seq = seq;
        k->since = mono_ms();
        k->flags = q->flags;
        k->root = root;
        k->parent_pid = sp.parent_pid;
        k->inst_fd = inst[0];
        inst[0] = -1;
        if (k->inst_fd >= 0) fcntl(k->inst_fd, F_SETFL, O_NONBLOCK);
        snprintf(k->sender, sizeof k->sender, "%s", m->sender ? m->sender : "");
        snprintf(k->id, sizeof k->id, "%s", id);
        if ((q->flags & FPAK_SPAWN_WATCH_BUS) && k->sender[0]) match(k->sender, 1);
        DBuf b;
        dw_init(&b);
        dw_u32(&b, (uint32_t)kid);
        dbus_reply(&conn, m, "u", &b);
        dw_free(&b);
        if (logfd >= 0) lordlog_write(logfd, "INFO", "apps", "spawned a process of %s%s%s", id, q->flags & FPAK_SPAWN_SANDBOX ? " (sub-sandbox)" : "",
                                      q->exposed_ignored ? " without the files it asked to expose (sandbox-expose-fd isn't served)" : "");
    } else {
        if (pidfd >= 0) { syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, NULL, 0); close(pidfd); }
        reply_error(m, "org.freedesktop.DBus.Error.Failed", "could not start it in the app's view");
        fprintf(stderr, "fpspawn: %s: could not start a spawned process (step %d)\n", id, step);
    }
out:
    for (int i = 0; i < 2; i++) if (inst[i] >= 0) close(inst[i]);
    if (logfd >= 0) close(logfd);
    free(abuf); free(q->store); free(q);
}

static void spawn_signal(const DMsg *m) {
    DRd r;
    dr_init(&r, m);
    uint32_t pid = dr_u32(&r), sig = dr_u32(&r);
    int pg = dr_bool(&r);
    if (r.err || sig < 1 || sig > 64) { reply_error(m, "org.freedesktop.DBus.Error.InvalidArgs", "bad signal"); return; }
    for (int i = 0; i < MAX_KIDS; i++) {
        Kid *k = &kids[i];
        if (!k->live || (uint32_t)k->pid != pid || !m->sender || strcmp(k->sender, m->sender)) continue;
        if (pg) killpg(k->pid, (int)sig); else syscall(SYS_pidfd_send_signal, k->pidfd, (int)sig, NULL, 0);
        break;
    }
    dbus_reply(&conn, m, NULL, NULL);           /* as flatpak-portal: also for a pid that isn't the caller's */
}

static void reply_props(const DMsg *m, int all, const char *prop) {
    DBuf b;
    dw_init(&b);
    if (all) {
        size_t a = dw_arr_begin(&b, 8);
        dw_struct(&b); dw_str(&b, "version"); dw_variant(&b, "u"); dw_u32(&b, VERSION);
        dw_struct(&b); dw_str(&b, "supports"); dw_variant(&b, "u"); dw_u32(&b, SUPPORTS);
        dw_arr_end(&b, a);
        dbus_reply(&conn, m, "a{sv}", &b);
    } else if (!strcmp(prop, "version") || !strcmp(prop, "supports")) {
        dw_variant(&b, "u");
        dw_u32(&b, !strcmp(prop, "version") ? VERSION : SUPPORTS);
        dbus_reply(&conn, m, "v", &b);
    } else reply_error(m, "org.freedesktop.DBus.Error.UnknownProperty", "no such property");
    dw_free(&b);
}

static int ask_sender(DMsg *m) {
    if (npending >= MAX_PENDING || !m->sender) { reply_error(m, "org.freedesktop.DBus.Error.LimitsExceeded", "busy"); return 0; }
    DBuf b;
    dw_init(&b);
    dw_str(&b, m->sender);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus",
               .member = "GetConnectionUnixProcessID", .dest = "org.freedesktop.DBus", .sig = "s" };
    uint32_t s = dbus_send(&conn, &h, &b);
    dw_free(&b);
    if (!s) { reply_error(m, "org.freedesktop.DBus.Error.Failed", "bus"); return 0; }
    pending[npending].call = *m;
    pending[npending].serial = s;
    pending[npending].since = mono_ms();
    npending++;
    return 1;
}

static void on_reply(const DMsg *m) {
    if (!m->sender || strcmp(m->sender, "org.freedesktop.DBus")) return;
    for (int i = 0; i < npending; i++) {
        if (pending[i].serial != m->reply_serial) continue;
        DMsg call = pending[i].call;
        pending[i] = pending[--npending];
        uint32_t pid = 0;
        if (m->type == DBUS_RETURN && m->sig && !strcmp(m->sig, "u")) { DRd r; dr_init(&r, m); pid = dr_u32(&r); if (r.err) pid = 0; }
        do_spawn(&call, pid);
        dbus_msg_free(&call);
        return;
    }
}

static void on_owner_changed(const DMsg *m) {
    DRd r;
    dr_init(&r, m);
    const char *name = dr_str(&r, NULL), *old = dr_str(&r, NULL), *now = dr_str(&r, NULL);
    (void)old;
    if (r.err || !name || !now || now[0]) return;
    for (int i = 0; i < MAX_KIDS; i++) {
        Kid *k = &kids[i];
        if (!k->live || !(k->flags & FPAK_SPAWN_WATCH_BUS) || strcmp(k->sender, name)) continue;
        killpg(k->pid, SIGKILL);        /* the caller left the bus: what it spawned goes too (--watch-bus) */
        syscall(SYS_pidfd_send_signal, k->pidfd, SIGKILL, NULL, 0);
    }
}

/* 1 if the message was kept, 0 if the caller frees it */
static int handle(DMsg *m) {
    if ((m->type == DBUS_RETURN || m->type == DBUS_ERROR) && m->reply_serial) { on_reply(m); return 0; }
    if (m->type == DBUS_SIGNAL) {
        if (m->sender && !strcmp(m->sender, "org.freedesktop.DBus") && m->member && !strcmp(m->member, "NameOwnerChanged") &&
            m->sig && !strcmp(m->sig, "sss"))
            on_owner_changed(m);
        return 0;
    }
    if (m->type != DBUS_CALL || !m->member) return 0;
    const char *sig = m->sig ? m->sig : "";
    if (m->iface && !strcmp(m->iface, "org.freedesktop.DBus.Introspectable")) {
        DBuf b;
        dw_init(&b);
        dw_str(&b, "<node><interface name=\"org.freedesktop.portal.Flatpak\">"
                   "<property name=\"version\" type=\"u\" access=\"read\"/><property name=\"supports\" type=\"u\" access=\"read\"/>"
                   "<method name=\"Spawn\"><arg type=\"ay\" direction=\"in\"/><arg type=\"aay\" direction=\"in\"/>"
                   "<arg type=\"a{uh}\" direction=\"in\"/><arg type=\"a{ss}\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/>"
                   "<arg type=\"a{sv}\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/></method>"
                   "<method name=\"SpawnSignal\"><arg type=\"u\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/>"
                   "<arg type=\"b\" direction=\"in\"/></method>"
                   "<signal name=\"SpawnStarted\"><arg type=\"u\"/><arg type=\"u\"/></signal>"
                   "<signal name=\"SpawnExited\"><arg type=\"u\"/><arg type=\"u\"/></signal>"
                   "</interface></node>");
        dbus_reply(&conn, m, "s", &b);
        dw_free(&b);
        return 0;
    }
    /* Peer on any path, as every D-Bus connection answers it (Steam's Flatpak pings "/" to check the portal) */
    int peer = fpak_bus_peer(m->iface, m->member);
    if (peer == 1 && !sig[0]) { dbus_reply(&conn, m, NULL, NULL); return 0; }
    if (peer == 2 && !sig[0]) {
        char id[64] = "";
        FILE *f = fopen("/etc/machine-id", "re");
        if (f) { if (!fgets(id, sizeof id, f)) id[0] = 0; fclose(f); }
        id[strcspn(id, "\n")] = 0;
        DBuf b;
        dw_init(&b);
        dw_str(&b, id);
        dbus_reply(&conn, m, "s", &b);
        dw_free(&b);
        return 0;
    }
    if (!m->path || strcmp(m->path, OBJ)) { reply_error(m, "org.freedesktop.DBus.Error.UnknownObject", "no such object"); return 0; }
    if (m->iface && !strcmp(m->iface, "org.freedesktop.DBus.Properties")) {
        DRd r;
        dr_init(&r, m);
        const char *ifc = dr_str(&r, NULL);
        if (!strcmp(m->member, "GetAll") && !strcmp(sig, "s") && ifc && !strcmp(ifc, BUS_NAME)) reply_props(m, 1, NULL);
        else if (!strcmp(m->member, "Get") && !strcmp(sig, "ss") && ifc && !strcmp(ifc, BUS_NAME)) {
            const char *prop = dr_str(&r, NULL);
            if (r.err || !prop) reply_error(m, "org.freedesktop.DBus.Error.InvalidArgs", "bad property");
            else reply_props(m, 0, prop);
        } else reply_error(m, "org.freedesktop.DBus.Error.PropertyReadOnly", "read-only");
        return 0;
    }
    if (m->iface && strcmp(m->iface, BUS_NAME)) { reply_error(m, "org.freedesktop.DBus.Error.UnknownInterface", "no such interface"); return 0; }
    if (!strcmp(m->member, "Spawn") && !strcmp(sig, "ayaaya{uh}a{ss}ua{sv}")) return ask_sender(m);
    if (!strcmp(m->member, "SpawnSignal") && !strcmp(sig, "uub")) { spawn_signal(m); return 0; }
    if (!strcmp(m->member, "CreateUpdateMonitor")) { reply_error(m, "org.freedesktop.DBus.Error.NotSupported", "updates come from the Store"); return 0; }
    reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
    return 0;
}

int fpspawn_start(void) {
    views_setup();
    int fd = dbus_connect_session();
    if (fd < 0 || dbus_open_fds(&conn, fd, 2000)) { if (fd >= 0) dbus_close(&conn); return -1; }
    DBuf b;
    dw_init(&b);
    dw_str(&b, BUS_NAME);
    dw_u32(&b, 4);                          /* DBUS_NAME_FLAG_DO_NOT_QUEUE */
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus", .member = "RequestName",
               .dest = "org.freedesktop.DBus", .sig = "su" };
    uint32_t s = dbus_send(&conn, &h, &b);
    dw_free(&b);
    DMsg m;
    int ok = s && dbus_wait_reply(&conn, s, &m, 2000) == 1;
    uint32_t res = 0;
    if (ok) { DRd r; dr_init(&r, &m); res = dr_u32(&r); }
    if (s) dbus_msg_free(&m);
    if (!ok || (res != 1 && res != 4)) {
        fprintf(stderr, "fpspawn: another program already serves %s\n", BUS_NAME);
        dbus_close(&conn);
        return -1;
    }
    up = 1;
    return 0;
}

int fpspawn_fd(void) { return up ? conn.fd : -1; }

void fpspawn_stop(void) {
    if (!up) return;
    for (int i = 0; i < npending; i++) { reply_error(&pending[i].call, "org.freedesktop.DBus.Error.ServiceUnknown", "Flatpak was removed"); dbus_msg_free(&pending[i].call); }
    npending = 0;
    dbus_flush(&conn);
    dbus_close(&conn);                      /* the bus releases the name with the connection */
    up = 0;
}
int fpspawn_wants_write(void) { return up && dbus_pending_out(&conn); }

/* NOTIFY_START: SpawnStarted with the new process as the caller's PID namespace sees it. Flatpak writes the instance id
 * before bubblewrap has written its bwrapinfo.json and started the sandbox, so the pid is looked for again (on each tick
 * and a few times right away) until it is there, for up to 3 s; after that the caller gets 0, as from flatpak-portal. */
static uint32_t relative_pid(Kid *k) {
    int child = digits_ok(k->inst) ? instance_child_pid(k->root, k->inst) : -1;
    if (child <= 0 || k->parent_pid <= 0) return 0;
    /* the caller's depth from its process in the view, then the new process's pid at that depth */
    char cg[512], path[640];
    uint32_t rel = 0;
    if (cgroup_of(k->pid, cg, sizeof cg) || snprintf(path, sizeof path, "/sys/fs/cgroup%s/cgroup.procs", cg) >= (int)sizeof path) return 0;
    FILE *f = fopen(path, "re");
    int pid, depth = -1, v[16], n;
    int rootns[16], rn = nspids(k->root, rootns, 16);
    int base = rn > 0 ? rn - 1 : -1;            /* the view's level */
    for (int pass = 0; f && base >= 0 && pass < 2; pass++) {
        rewind(f);
        while (fscanf(f, "%d", &pid) == 1) {
            if ((n = nspids(pid, v, 16)) <= base) continue;
            if (!pass && v[base] == k->parent_pid) { depth = n - 1; break; }
            if (pass && v[base] == child && depth >= 0 && n > depth) { rel = (uint32_t)v[depth]; break; }
        }
        if (depth < 0) break;
    }
    if (f) fclose(f);
    return rel;
}

static void send_started(Kid *k, uint32_t rel) {
    k->want_started = 0;
    DBuf b;
    dw_init(&b);
    dw_u32(&b, (uint32_t)k->pid);
    dw_u32(&b, rel);
    send_to(k->sender, "SpawnStarted", "uu", &b);
    dw_free(&b);
}

static void try_started(Kid *k) {
    uint32_t rel = 0;
    for (int i = 0; i < 10 && !(rel = relative_pid(k)); i++) {
        struct timespec ts = { 0, 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (rel || mono_ms() - k->since > 3000) send_started(k, rel);
}

static void started(Kid *k) {
    char buf[24];
    ssize_t r = read(k->inst_fd, buf, sizeof buf - 1 - k->inst_len);
    if (r > 0) { memcpy(k->inst + k->inst_len, buf, (size_t)r); k->inst_len += (size_t)r; k->inst[k->inst_len] = 0; }
    if (r < 0 && errno == EAGAIN) return;
    close(k->inst_fd);
    k->inst_fd = -1;
    k->want_started = 1;
    try_started(k);
}

void fpspawn_tick(void) {
    views_setup();
    views_drain();                                  /* how spawned processes ended, helpers that went */
    if (!up) return;
    double now = mono_ms();
    for (int i = 0; i < npending;) {                /* the bus never said who asked: refused */
        if (now - pending[i].since < PENDING_MS) { i++; continue; }
        DMsg call = pending[i].call;
        pending[i] = pending[--npending];
        reply_error(&call, "org.freedesktop.DBus.Error.Timeout", "no answer from the bus");
        dbus_msg_free(&call);
    }
    for (int i = 0; i < MAX_KIDS; i++) {
        if (!kids[i].live) continue;
        if (kids[i].inst_fd >= 0) started(&kids[i]);
        else if (kids[i].want_started) try_started(&kids[i]);
    }
    dbus_flush(&conn);
}

void fpspawn_dispatch(void) {
    if (!up) return;
    DMsg m;
    int r;
    while ((r = dbus_next(&conn, &m)) == 1) { if (!handle(&m)) dbus_msg_free(&m); }
    if (r < 0 || dbus_flush(&conn) < 0) {
        fprintf(stderr, "fpspawn: lost the session bus\n");
        for (int i = 0; i < npending; i++) dbus_msg_free(&pending[i].call);
        npending = 0;
        dbus_close(&conn);
        up = 0;
    }
}
