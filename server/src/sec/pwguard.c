/* pwguard.c — "no microphone", "no camera" and "no screen capture" for Linux apps inside PipeWire (see pwguard.h).
 *
 * A minimal client of PipeWire's native protocol (version 3): each message is a 16-byte header
 * (object id; opcode << 24 | payload size; sequence; number of fds) and a SPA POD Struct.
 * We only use: core.hello, client.update_properties, core.get_registry, core.pong,
 * registry.bind (clients only), registry.destroy (a restricted app's capture streams) and
 * client.update_permissions; and we read core.ping/error/remove_id, registry.global /
 * global_remove and client.info. Everything is non-blocking: lord-server never waits for it. */
#include "pwguard.h"
#include "appid.h"
#include "restrict.h"
#include "../fw/rules.h"

#include <dirent.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/unix_diag.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 256
#define MAX_NODES   1024
#define MAX_PORTS   4096
#define MAX_PWLINKS   1024
#define MAX_PROXY   (MAX_CLIENTS + 8)
#define IN_MAX      (1u << 20)
#define OUT_MAX     (1u << 20)

/* PipeWire's permission bits (pw_permission): read, write, execute, link, metadata */
#define PW_R 0400
#define PW_W 0200
#define PW_X 0100
#define PW_L 0020
#define PW_M 0010
#define PW_ALL (PW_R | PW_W | PW_X | PW_L | PW_M)

#define SPA_INT    4
#define SPA_LONG   5
#define SPA_STRING 8
#define SPA_STRUCT 14

typedef struct {
    uint32_t id, proxy;         /* global id; our bound proxy (0 = not bound yet) */
    int pid_sec, pid_app, pulse;/* pipewire.sec.pid (kernel), application.process.id (app-reported), a client of the real pipewire-pulse */
    int unverified;             /* who it is can't be established: treated as restricted while restricted apps run */
    int daemon;                 /* pipewire-pulse's own connection: the streams of modules a pulse client loaded */
    int main_daemon;            /* the session's pipewire itself (its modules: LordOS's Noise Reduction filter) */
    int sound;                  /* another of the sound system's own processes (PipeWire's filter-chain, WirePlumber) */
    int resolved;               /* app worked out (needs the client's info) */
    int pid_ok;                 /* the PID it was worked out from (resolve_as's pid_out): 0 when none could be */
    /* the capture indicator's name for a client with no verified app (pwguard_on_use), worked out again every 5 s */
    char seen[FW_APP_MAX], exe[32];
    double seen_until;
    char fpid[64];              /* pipewire.access.portal.app_id: the Flatpak app pipewire-pulse says it is (checked) */
    char app[FW_APP_MAX];
    uint32_t want, applied;     /* restrictions (FW_R_NO_MIC | FW_R_NO_CAMERA | FW_R_NO_SCREEN) to apply / applied */
    int live;
} PgClient;

typedef struct { uint32_t id, client, flag, stream; int live, voice; } PgNode;   /* flag: capture node; stream: capture stream; voice: Noise Reduction's own capture */
typedef struct { uint32_t id, node; int live; } PgPort;
/* a link from a capture node (a microphone, a camera, a shared screen) into an app's capture stream: what the capture
 * indicator says is in use (pwguard_on_use) */
typedef struct { uint32_t id, out, in; int live; } PgLink;

static struct {
    int fd;
    uint8_t in[IN_MAX];
    size_t inlen;
    uint8_t *out;
    size_t outlen;
    uint32_t seq;
    PgClient cl[MAX_CLIENTS];
    PgNode node[MAX_NODES];
    PgPort port[MAX_PORTS];
    PgLink link[MAX_PWLINKS];
    uint8_t proxy_used[MAX_PROXY];
    double next_try, next_tick;
    int up;
} P = { .fd = -1 };

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* ---- SPA POD: reading ---------------------------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* The pod at p (size + type + body), within n bytes. */
static int pod_at(const uint8_t *p, size_t n, PwPod *out, uint32_t *type) {
    if (n < 8) return -1;
    uint32_t size = rd32(p), t = rd32(p + 4);
    if (size > n - 8) return -1;
    out->p = p;
    out->n = 8 + size;
    if (type) *type = t;
    return 0;
}

int pwpod_struct_field(PwPod s, int index, PwPod *out) {
    uint32_t t;
    PwPod whole;
    if (pod_at(s.p, s.n, &whole, &t) || t != SPA_STRUCT) return -1;
    size_t off = 8, end = whole.n;
    for (int i = 0; off < end; i++) {
        PwPod f;
        if (pod_at(s.p + off, end - off, &f, NULL)) return -1;
        if (i == index) { *out = f; return 0; }
        size_t adv = (f.n + 7) & ~(size_t)7;
        if (adv > end - off) return -1;
        off += adv;
    }
    return -1;
}

int pwpod_int(PwPod p, int32_t *v) {
    if (p.n < 12 || rd32(p.p + 4) != SPA_INT || rd32(p.p) != 4) return -1;
    memcpy(v, p.p + 8, 4);
    return 0;
}

int pwpod_string(PwPod p, const char **s) {
    uint32_t size = p.n >= 8 ? rd32(p.p) : 0;
    if (p.n < 9 || rd32(p.p + 4) != SPA_STRING || !size || p.p[8 + size - 1]) return -1;
    *s = (const char *)p.p + 8;
    return 0;
}

/* A dict (Struct: Int n, then String key, String value...) → the value of key, or NULL. */
static const char *dict_get(PwPod d, const char *key) {
    PwPod f;
    int32_t n;
    if (pwpod_struct_field(d, 0, &f) || pwpod_int(f, &n) || n < 0 || n > 4096) return NULL;
    for (int i = 0; i < n; i++) {
        PwPod k, v;
        const char *ks, *vs;
        if (pwpod_struct_field(d, 1 + 2 * i, &k) || pwpod_string(k, &ks)) return NULL;
        if (strcmp(ks, key)) continue;
        if (pwpod_struct_field(d, 2 + 2 * i, &v) || pwpod_string(v, &vs)) return NULL;
        return vs;
    }
    return NULL;
}

/* ---- SPA POD: writing ---------------------------------------------------------------------- */

typedef struct { uint8_t b[16384]; size_t n; int err; } Pb;

static void pb_raw(Pb *b, const void *p, size_t n) {
    if (b->err || n > sizeof b->b - b->n) { b->err = 1; return; }
    memcpy(b->b + b->n, p, n);
    b->n += n;
}
static void pb_pad(Pb *b) { static const uint8_t z[8] = { 0 }; size_t r = b->n % 8; if (r) pb_raw(b, z, 8 - r); }
static void pb_hdr(Pb *b, uint32_t size, uint32_t type) { pb_raw(b, &size, 4); pb_raw(b, &type, 4); }
static void pb_int(Pb *b, int32_t v) { pb_hdr(b, 4, SPA_INT); pb_raw(b, &v, 4); pb_pad(b); }
static void pb_str(Pb *b, const char *s) { size_t l = strlen(s) + 1; pb_hdr(b, (uint32_t)l, SPA_STRING); pb_raw(b, s, l); pb_pad(b); }
static size_t pb_begin(Pb *b) { size_t at = b->n; pb_hdr(b, 0, SPA_STRUCT); return at; }
static void pb_end(Pb *b, size_t at) { if (b->err) return; uint32_t size = (uint32_t)(b->n - at - 8); memcpy(b->b + at, &size, 4); }

/* ---- connection ---------------------------------------------------------------------------- */

static void drop(void) {
    if (P.fd >= 0) close(P.fd);
    P.fd = -1;
    P.up = 0;
    P.inlen = P.outlen = 0;
    memset(P.cl, 0, sizeof P.cl);
    memset(P.node, 0, sizeof P.node);
    memset(P.port, 0, sizeof P.port);
    memset(P.link, 0, sizeof P.link);
    memset(P.proxy_used, 0, sizeof P.proxy_used);
    P.next_try = now_ms() + 2000;
}

static void flush_out(void) {
    while (P.fd >= 0 && P.outlen) {
        ssize_t w = send(P.fd, P.out, P.outlen, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) return;
        if (w <= 0) { drop(); return; }
        memmove(P.out, P.out + w, P.outlen - (size_t)w);
        P.outlen -= (size_t)w;
    }
}

static void send_msg(uint32_t id, uint32_t opcode, const Pb *b) {
    if (P.fd < 0 || b->err) return;
    if (!P.out && !(P.out = malloc(OUT_MAX))) return;
    if (P.outlen + 16 + b->n > OUT_MAX) { drop(); return; }   /* PipeWire isn't reading: start over */
    uint32_t h[4] = { id, opcode << 24 | (uint32_t)(b->n & 0xffffff), P.seq++, 0 };
    memcpy(P.out + P.outlen, h, 16);
    memcpy(P.out + P.outlen + 16, b->b, b->n);
    P.outlen += 16 + b->n;
}

static uint32_t proxy_new(void) {
    for (uint32_t i = 3; i < MAX_PROXY; i++) if (!P.proxy_used[i]) { P.proxy_used[i] = 1; return i; }
    return 0;
}

int pwguard_start(void) {
    if (P.fd >= 0) return 0;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || rt[0] != '/') return -1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if ((size_t)snprintf(sa.sun_path, sizeof sa.sun_path, "%s/pipewire-0-manager", rt) >= sizeof sa.sun_path) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) && errno != EINPROGRESS) { close(fd); P.next_try = now_ms() + 2000; return -1; }
    P.fd = fd;
    P.seq = 0;
    P.proxy_used[0] = P.proxy_used[1] = P.proxy_used[2] = 1;
    Pb b = { .n = 0 };
    size_t s = pb_begin(&b); pb_int(&b, 3); pb_end(&b, s);
    send_msg(0, 1, &b);                                         /* core.hello(3) */
    b.n = 0;
    s = pb_begin(&b);
    size_t d = pb_begin(&b);
    pb_int(&b, 2);
    pb_str(&b, "application.name"); pb_str(&b, "LordOS privacy guard");
    pb_str(&b, "media.category"); pb_str(&b, "Manager");
    pb_end(&b, d);
    pb_end(&b, s);
    send_msg(1, 2, &b);                                         /* client.update_properties */
    b.n = 0;
    s = pb_begin(&b); pb_int(&b, 3); pb_int(&b, 2); pb_end(&b, s);
    send_msg(0, 5, &b);                                         /* core.get_registry(3, new id 2) */
    flush_out();
    P.up = 1;
    return 0;
}

int pwguard_fd(void) { return P.fd; }
int pwguard_wants_write(void) { return P.fd >= 0 && P.outlen > 0; }

/* ---- the policy ---------------------------------------------------------------------------- */

uint32_t pwguard_capture_flag(const char *mc) {
    if (!mc) return 0;
    if (!strncmp(mc, "Audio/Source", 12) || !strcmp(mc, "Audio/Duplex")) return FW_R_NO_MIC;
    if (!strncmp(mc, "Video/Source", 12)) return FW_R_NO_CAMERA;
    return 0;
}

uint32_t pwguard_node_flag(const char *mc, const char *role, const char *name) {
    if (mc && !strncmp(mc, "Video/Source", 12) && ((role && !strcmp(role, "Screen")) || (name && !strncmp(name, "lordos-screencast-", 18))))
        return FW_R_NO_SCREEN;
    return pwguard_capture_flag(mc);
}

int pwguard_voice_capture(const char *mc, const char *name) {
    return mc && name && !strcmp(mc, "Stream/Input/Audio") && !strcmp(name, "lordos_voice_capture");
}

uint32_t pwguard_stream_flag(const char *mc) {
    if (!mc) return 0;
    if (!strcmp(mc, "Stream/Input/Audio")) return FW_R_NO_MIC;
    if (!strcmp(mc, "Stream/Input/Video")) return FW_R_NO_CAMERA;
    return 0;
}

static PgClient *client_by_id(uint32_t id) {
    for (int i = 0; i < MAX_CLIENTS; i++) if (P.cl[i].live && P.cl[i].id == id) return &P.cl[i];
    return NULL;
}

static PgNode *node_by_id(uint32_t id) {
    for (int i = 0; i < MAX_NODES; i++) if (P.node[i].live && P.node[i].id == id) return &P.node[i];
    return NULL;
}

#define GUARDED (FW_R_NO_MIC | FW_R_NO_CAMERA | FW_R_NO_SCREEN)

/* Restricted apps are running (an app scope of this session whose app has no-mic, no-camera or no-screen):
 * only then are clients we can't identify treated as restricted, so normal use without
 * restrictions never pays for it. */
static int guard_armed;

/* What the client's app is restricted to, among what PipeWire hands out. */
static uint32_t client_restrictions(PgClient *c) {
    if (!c->resolved) return 0;
    /* pipewire-pulse's own connection carries the capture streams of modules any pulse client
     * loaded (module-loopback from the microphone into a pipe or a TCP server): while restricted
     * apps run they can't be told apart, so they are closed */
    if (c->daemon) return guard_armed ? GUARDED : 0;
    if (c->unverified) return guard_armed ? GUARDED : 0;
    if (!c->app[0]) return 0;
    uint32_t f = 0;
    if (restrict_blocks(c->app, FW_R_NO_MIC)) f |= FW_R_NO_MIC;
    if (restrict_blocks(c->app, FW_R_NO_CAMERA)) f |= FW_R_NO_CAMERA;
    if (restrict_blocks(c->app, FW_R_NO_SCREEN)) f |= FW_R_NO_SCREEN;   /* the screens Linux apps are shown (ScreenCast) */
    return f;
}

/* Sends the whole permission list of a restricted client (or the restore list when its
 * restriction is lifted): hidden = the capture nodes and ports of what it is restricted to, and
 * every other client. In chunks: PipeWire merges successive update_permissions, so a list longer
 * than one message is never dropped (that would leave the client with every permission). */
#define PERM_CHUNK 400
static void send_perm_chunk(PgClient *c, const int32_t *pairs, int32_t n) {
    Pb b = { .n = 0 };
    size_t s = pb_begin(&b);
    pb_int(&b, n);
    for (int32_t i = 0; i < n; i++) { pb_int(&b, pairs[2 * i]); pb_int(&b, pairs[2 * i + 1]); }
    pb_end(&b, s);
    if (b.err) { fprintf(stderr, "pwguard: permission chunk too long for %s\n", c->app); drop(); return; }
    send_msg(c->proxy, 4, &b);                                  /* client.update_permissions */
}

static void apply_permissions(PgClient *c, uint32_t flags) {
    if (!c->proxy) return;
    int32_t pairs[2 * PERM_CHUNK];
    int32_t n = 0;
#define PERM_ADD(id_, perm_) do { pairs[2 * n] = (int32_t)(id_); pairs[2 * n + 1] = (int32_t)(perm_); \
                                 if (++n == PERM_CHUNK) { send_perm_chunk(c, pairs, n); n = 0; if (P.fd < 0) return; } } while (0)
    uint32_t perm_clients = flags ? 0 : PW_ALL;
    for (int i = 0; i < MAX_NODES; i++) {
        PgNode *nd = &P.node[i];
        if (!nd->live || !nd->flag) continue;
        uint32_t want = (flags & nd->flag) ? 0 : PW_ALL;    /* restored when lifted */
        PERM_ADD(nd->id, want);
        for (int k = 0; k < MAX_PORTS; k++)
            if (P.port[k].live && P.port[k].node == nd->id) PERM_ADD(P.port[k].id, want);
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        PgClient *o = &P.cl[i];
        if (!o->live || o == c) continue;
        PERM_ADD(o->id, perm_clients);
    }
#undef PERM_ADD
    if (n) send_perm_chunk(c, pairs, n);
    if (P.fd >= 0) c->applied = flags;
}

/* registry.destroy of a global (a stream, a node or a client we must not leave running) */
static void destroy_global(uint32_t id) {
    Pb b = { .n = 0 };
    size_t s = pb_begin(&b); pb_int(&b, (int32_t)id); pb_end(&b, s);
    send_msg(2, 2, &b);
}

/* Destroys the capture streams of a restricted client (the app gets its stream closed). */
static void kill_streams(void) {
    for (int i = 0; i < MAX_NODES; i++) {
        PgNode *nd = &P.node[i];
        if (!nd->live || !nd->stream) continue;
        PgClient *c = client_by_id(nd->client);
        /* an owner we don't track (no client slot) counts as restricted while restricted apps run */
        if (c ? !(c->want & nd->stream) : !guard_armed) continue;
        destroy_global(nd->id);
        fprintf(stderr, "pwguard: %s is restricted (%s): its capture stream %u was closed\n", c ? (c->daemon ? "pipewire-pulse (a loaded module)" : c->app) : "an untracked client",
                nd->stream == FW_R_NO_MIC ? "no-mic" : "no-camera", nd->id);
        nd->stream = 0;                                         /* once; the global_remove follows */
    }
}

static void evaluate(PgClient *c) {
    c->want = client_restrictions(c);
    if (c->daemon) return;              /* only its module streams are closed: it serves every pulse client */
    if (c->want != c->applied) apply_permissions(c, c->want);
}

/* ---- who a client really is ---------------------------------------------------------------- */

/* Is pid running the root-owned executable at path (same inode, not a copy)? */
static int runs_exe(int pid, const char *path) {
    char p[64];
    struct stat st, want;
    snprintf(p, sizeof p, "/proc/%d/exe", pid);
    if (stat(p, &st) || stat(path, &want) || want.st_uid != 0 || (want.st_mode & 022)) return 0;
    if (st.st_dev != want.st_dev || st.st_ino != want.st_ino) return 0;
    snprintf(p, sizeof p, "/proc/%d", pid);
    return !stat(p, &st) && st.st_uid == getuid();
}

/* Is pid exactly in the user manager's session.slice/<unit>, alone? The whole path is compared:
 * the user owns the user@ subtree and can make a nested cgroup that merely ends in the same names. */
static int in_user_unit(int pid, const char *unit) {
    char p[64], line[1024], want[256];
    unsigned u = (unsigned)getuid();
    snprintf(want, sizeof want, "0::/user.slice/user-%u.slice/user@%u.service/session.slice/%s\n", u, u, unit);
    snprintf(p, sizeof p, "/proc/%d/cgroup", pid);
    FILE *f = fopen(p, "re");
    int ok = 0;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "0::", 3)) { ok = !strcmp(line, want); break; }
    if (f) fclose(f);
    if (!ok) return 0;
    /* and the only process there: the user can also write that unit's own cgroup.procs (the
     * subtree is theirs), so a second process moved in makes neither count (fail closed) */
    snprintf(want, sizeof want, "/sys/fs/cgroup/user.slice/user-%u.slice/user@%u.service/session.slice/%s/cgroup.procs", u, u, unit);
    f = fopen(want, "re");
    int n = 0, first = -1;
    while (f && fgets(line, sizeof line, f)) { if (!n) first = atoi(line); n++; }
    if (f) fclose(f);
    return n == 1 && first == pid;
}

/* Is pid the session's real pipewire-pulse? Never from a property: the executable is the root-owned
 * /usr/bin/pipewire (pipewire-pulse is a link to it), the process is ours, and it runs in the user
 * manager's pipewire-pulse.service. */
static int is_pulse_daemon(int pid) {
    return runs_exe(pid, "/usr/bin/pipewire") && in_user_unit(pid, "pipewire-pulse.service");
}

/* The unix sockets of the system and their peers (sock_diag), to see who is really connected to
 * pipewire-pulse. Returns the number of pairs written into ino/peer. */
static int unix_peers(uint32_t *ino, uint32_t *peer, int max) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (fd < 0) return -1;
    struct { struct nlmsghdr nh; struct unix_diag_req r; } req;
    memset(&req, 0, sizeof req);
    req.nh.nlmsg_len = sizeof req;
    req.nh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.r.sdiag_family = AF_UNIX;
    req.r.udiag_states = ~0u;
    req.r.udiag_show = UDIAG_SHOW_PEER;
    if (send(fd, &req, sizeof req, 0) != (ssize_t)sizeof req) { close(fd); return -1; }
    static uint8_t buf[32768];
    int n = 0, done = 0;
    while (!done) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r <= 0) break;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (size_t)r); h = NLMSG_NEXT(h, r)) {
            if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR) { done = 1; break; }
            struct unix_diag_msg *m = NLMSG_DATA(h);
            int len = (int)h->nlmsg_len - (int)NLMSG_LENGTH(sizeof *m);
            for (struct rtattr *a = (struct rtattr *)(m + 1); len > 0 && RTA_OK(a, len); a = RTA_NEXT(a, len))
                if (a->rta_type == UNIX_DIAG_PEER && RTA_PAYLOAD(a) >= 4 && n < max) { ino[n] = m->udiag_ino; memcpy(&peer[n], RTA_DATA(a), 4); n++; }
        }
    }
    close(fd);
    return n;
}

/* How many distinct connections to pipewire-pulse (process pp) process pid holds; -1 if unknown.
 * The server ends are pipewire-pulse's (always in our network namespace); their peers are the client
 * ends, which pid holds even when it lives in a network namespace of its own (a Flatpak app with no
 * network), where sock_diag doesn't list them. */
static int pulse_connections(int pp, int pid) {
    static uint32_t ino[65536], peer[65536];
    int np = unix_peers(ino, peer, 65536);
    if (np < 0) return -1;
    char d[64], l[320], t[64];
    static uint32_t clients[4096];
    int nc = 0;
    for (int pass = 0; pass < 2; pass++) {
        snprintf(d, sizeof d, "/proc/%d/fd", pass ? pid : pp);
        DIR *dir = opendir(d);
        if (!dir) return -1;
        struct dirent *e;
        int count = 0;
        while ((e = readdir(dir))) {
            if (e->d_name[0] == '.') continue;
            snprintf(l, sizeof l, "%s/%s", d, e->d_name);
            ssize_t k = readlink(l, t, sizeof t - 1);
            unsigned long v;
            if (k <= 0) continue;
            t[k] = 0;
            if (sscanf(t, "socket:[%lu]", &v) != 1) continue;
            if (!pass) {
                /* a socket of pipewire-pulse: its peer is a client's end */
                for (int i = 0; i < np; i++) if (ino[i] == (uint32_t)v) { if (peer[i] && nc < 4096) clients[nc++] = peer[i]; break; }
                continue;
            }
            for (int i = 0; i < nc; i++) if (clients[i] == (uint32_t)v) { count++; break; }
        }
        closedir(dir);
        if (pass) return count;
    }
    return -1;
}

/* The pid as pid's innermost PID namespace sees it (the last NSpid field), or -1. */
static int inner_pid(int pid) {
    char p[64], line[256];
    snprintf(p, sizeof p, "/proc/%d/status", pid);
    FILE *f = fopen(p, "re");
    int in = -1;
    while (f && fgets(line, sizeof line, f)) {
        if (strncmp(line, "NSpid:", 6)) continue;
        char *s = line + 6, *end;
        for (;;) {
            long v = strtol(s, &end, 10);
            if (end == s) break;
            in = (int)v;
            s = end;
        }
        break;
    }
    if (f) fclose(f);
    return in;
}

/* A Flatpak app's pulse client reports its pid inside the app's sandbox, and pipewire-pulse says which app
 * it is (pipewire.access.portal.app_id, from the connecting process's .flatpak-info). Neither is taken
 * on its word: the pid outside is a process of that app's own root-owned scope whose innermost pid is
 * the one reported and which holds at least as many connections to pipewire-pulse as there are clients
 * claiming that app and pid. Returns it, or 0. */
static int flatpak_host_pid(const PgClient *c, int claims) {
    size_t n = strnlen(c->fpid, sizeof c->fpid);
    if (!n || n > 59 || c->pid_app <= 0) return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = c->fpid[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-')) return 0;
    }
    if (c->fpid[0] == '.' || strstr(c->fpid, "..")) return 0;
    char path[192];
    snprintf(path, sizeof path, "/sys/fs/cgroup/system.slice/lordos.service/lordos-session-%u/app-lordos-fp-%s.scope/cgroup.procs",
             (unsigned)getuid(), c->fpid);
    FILE *f = fopen(path, "re");
    if (!f) return 0;
    int found = 0, pid;
    while (!found && fscanf(f, "%d", &pid) == 1)
        if (pid > 0 && inner_pid(pid) == c->pid_app && pulse_connections(c->pid_sec, pid) >= claims) found = pid;
    fclose(f);
    return found;
}

/* The pid as the process itself sees it is pid: it lives in our PID namespace. A process in another
 * one (a no-files app, a container) reports its inner pid, so a claim of its outer pid can only be
 * someone else's. */
static int same_pidns(int pid) {
    char p[64], line[256];
    snprintf(p, sizeof p, "/proc/%d/status", pid);
    FILE *f = fopen(p, "re");
    int ok = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (strncmp(line, "NSpid:", 6)) continue;
        int a, b;
        ok = sscanf(line + 6, "%d %d", &a, &b) == 1 && a == pid;
        break;
    }
    if (f) fclose(f);
    return ok;
}

/* A process the user's service manager runs (systemd-run --user, D-Bus activation): outside every
 * app scope, so an app could escape its restriction there. The sound services themselves don't count. */
static int in_user_manager(int pid) {
    char p[64], line[1024], unit[64];
    snprintf(p, sizeof p, "/proc/%d/cgroup", pid);
    FILE *f = fopen(p, "re");
    int r = 0;
    snprintf(unit, sizeof unit, "/user@%u.service/", (unsigned)getuid());
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "0::", 3)) { r = strstr(line, unit) != NULL; break; }
    if (f) fclose(f);
    if (!r) return 0;
    /* the sound services: their exact unit AND their root-owned executable */
    if (runs_exe(pid, "/usr/bin/pipewire") && (in_user_unit(pid, "pipewire.service") || in_user_unit(pid, "pipewire-pulse.service"))) return 0;
    if (runs_exe(pid, "/usr/bin/wireplumber") && in_user_unit(pid, "wireplumber.service")) return 0;
    return 1;
}

/* Who a client is: its app into app ("" when it is in no app's scope), *unverified when that can't be established, and the
 * PID it was worked out from into *pid_out. verify: a pulse client's own word (application.process.id) is checked; the
 * restrictions do it only while restricted apps run (guard_armed), the capture indicator always. */
static void resolve_as(PgClient *c, int verify, char *app, size_t n, int *unverified, int *pid_out) {
    app[0] = 0;
    *unverified = 0;
    *pid_out = 0;
    int pid = c->pid_sec;                           /* the socket's credentials: always the start */
    if (c->pulse && !verify) pid = c->pid_app;      /* no restricted app runs: nothing to verify */
    else if (c->pulse) {
        /* a pulse client of the real pipewire-pulse: the app's own pid is what it reported, so it
         * counts only if that process really holds as many connections to pipewire-pulse as
         * there are clients claiming it */
        int claims = 0;
        int fp = 0;
        if (c->fpid[0] && c->pid_app > 0) {
            /* a Flatpak app's: its pid inside the sandbox, found again in that app's scope. Tried first: that small
             * pid is usually also some other process's out here, so "same PID namespace" can't tell them apart */
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (P.cl[i].live && P.cl[i].pulse && P.cl[i].pid_app == c->pid_app && !strcmp(P.cl[i].fpid, c->fpid)) claims++;
            fp = flatpak_host_pid(c, claims);
            if (fp) pid = fp;
            else if (!same_pidns(c->pid_app)) { *unverified = 1; return; }
            claims = 0;
        }
        if (!fp) {
            for (int i = 0; i < MAX_CLIENTS; i++) if (P.cl[i].live && P.cl[i].pulse && P.cl[i].pid_app == c->pid_app) claims++;
            int real = c->pid_app > 0 ? pulse_connections(c->pid_sec, c->pid_app) : -1;
            if (real < claims || !same_pidns(c->pid_app)) { *unverified = 1; return; }
            pid = c->pid_app;
        }
    }
    if (pid <= 0) { *unverified = 1; return; }
    *pid_out = pid;
    int r = appid_of_pid((unsigned)pid, app, n);
    if (r < 0) { *unverified = 1; app[0] = 0; }      /* gone or unreadable */

    else if (r > 0) {                                /* not in any app's scope */
        app[0] = 0;
        /* escaped to the user's service manager while restricted apps run: treated as restricted
         * (like lord-netperm's per-user union) */
        if (guard_armed && in_user_manager(pid)) *unverified = 1;
    }
}

static void resolve(PgClient *c) {
    c->resolved = 1;
    resolve_as(c, guard_armed, c->app, sizeof c->app, &c->unverified, &c->pid_ok);
}

static int guarded(const char *app, void *u) {
    (void)u;
    return restrict_blocks(app, FW_R_NO_MIC) || restrict_blocks(app, FW_R_NO_CAMERA) || restrict_blocks(app, FW_R_NO_SCREEN);
}

/* Re-arms the guard: is any restricted app of this session running? (its scope is there) */
static void arm(void) { guard_armed = appid_running(guarded, NULL); }

/* ---- what is captured now (the capture indicator, sdk/capturestatus.jsx) ------------------- */

static void (*use_emit)(const char *kind, const char *json);
static char use_last[4096] = "[]";
static int use_dirty;

void pwguard_on_use(void (*emit)(const char *kind, const char *json)) { use_emit = emit; }

static int use_same(const PwUse *a, const PwUse *b) {
    return !strcmp(a->app, b->app) && (a->app[0] || (!strcmp(a->sys, b->sys) && !strcmp(a->exe, b->exe)));
}

static int plain(const char *s) {
    for (; *s; s++) if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20) return 0;
    return 1;
}

/* "[{"app":"desktop:obs","mic":1,"camera":0,"screen":1},...]": one entry per app, its flags merged; with no app, one per
 * sound-system part ("sys":"sound") or program ("exe":"<name>"), "" and neither an unknown program. -1 when it doesn't
 * fit or a field isn't plain (the caller then says an unknown program captures). */
int pwguard_use_format(const PwUse *u, int n, char *out, size_t cap) {
    int w = snprintf(out, cap, "[");
    if (w < 0 || (size_t)w >= cap) return -1;
    size_t at = (size_t)w;
    int first = 1;
    for (int i = 0; i < n; i++) {
        int seen = 0;
        for (int k = 0; k < i && !seen; k++) if (use_same(&u[k], &u[i])) seen = 1;
        if (seen) continue;
        uint32_t f = 0;
        for (int k = i; k < n; k++) if (use_same(&u[k], &u[i])) f |= u[k].flags;
        if (!(f & (FW_R_NO_MIC | FW_R_NO_CAMERA | FW_R_NO_SCREEN))) continue;
        const char *sys = u[i].app[0] ? "" : u[i].sys, *exe = u[i].app[0] || sys[0] ? "" : u[i].exe;
        if (!plain(u[i].app) || !plain(sys) || !plain(exe)) return -1;   /* ids and names are plain; anything else is not named */
        w = snprintf(out + at, cap - at, "%s{\"app\":\"%s\",%s%s%s%s%s%s\"mic\":%d,\"camera\":%d,\"screen\":%d}", first ? "" : ",", u[i].app,
                     sys[0] ? "\"sys\":\"" : "", sys, sys[0] ? "\"," : "", exe[0] ? "\"exe\":\"" : "", exe, exe[0] ? "\"," : "",
                     !!(f & FW_R_NO_MIC), !!(f & FW_R_NO_CAMERA), !!(f & FW_R_NO_SCREEN));
        if (w < 0 || (size_t)w >= cap - at) return -1;
        at += (size_t)w;
        first = 0;
    }
    w = snprintf(out + at, cap - at, "]");
    if (w < 0 || (size_t)w >= cap - at) return -1;
    return 0;
}

int pwguard_sandbox_pick(const PwCand *c, int n, int claims, char *out, size_t cap) {
    if (!out || !cap) return -1;
    out[0] = 0;
    if (claims < 1) return -1;
    const char *pick = NULL;
    for (int i = 0; i < n; i++) {
        if (c[i].conns < claims || !c[i].app[0]) continue;
        if (!pick) pick = c[i].app;
        else if (strcmp(pick, c[i].app)) return -1;           /* two apps could be it: neither is named */
    }
    if (!pick || strlen(pick) + 1 > cap) return -1;
    memcpy(out, pick, strlen(pick) + 1);
    return 0;
}

typedef struct { const PgClient *c; PwCand cand[16]; int n; } SandboxScan;

static void sandbox_each(void *ctx, const char *cg, unsigned pid) {
    SandboxScan *s = ctx;
    char app[FW_APP_MAX];
    if (s->n >= (int)(sizeof s->cand / sizeof s->cand[0]) || appid_session_owner(cg, app, sizeof app) != 0) return;
    if (strncmp(app, "desktop:", 8) && strncmp(app, "flatpak:", 8) && strncmp(app, "lpk:", 4)) return;
    if (same_pidns((int)pid) || inner_pid((int)pid) != s->c->pid_app) return;
    int conns = pulse_connections(s->c->pid_sec, (int)pid);
    if (conns <= 0) return;
    snprintf(s->cand[s->n].app, sizeof s->cand[s->n].app, "%s", app);
    s->cand[s->n].conns = conns;
    s->n++;
}

/* A pulse client in a sandbox of its own (WebKit's bubblewrap in LordOS's web apps, which says it is Flatpak app
 * "org.webkit.app-<hash>"; an app in its own PID namespace) reports its pid inside it, so resolve_as can't verify it.
 * Only to name it: the app whose root-owned scope holds a process with that inner pid, in another PID namespace, holding
 * at least as many connections to pipewire-pulse as there are clients claiming that pid and Flatpak id; when two apps
 * could be it, neither. */
static void sandbox_owner(const PgClient *c, char *app, size_t n) {
    app[0] = 0;
    if (c->pid_app <= 0 || c->pid_sec <= 0) return;
    int claims = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (P.cl[i].live && P.cl[i].pulse && P.cl[i].pid_app == c->pid_app && !strcmp(P.cl[i].fpid, c->fpid)) claims++;
    SandboxScan s = { .c = c };
    appid_session_procs(sandbox_each, &s);
    if (pwguard_sandbox_pick(s.cand, s.n, claims, app, n)) app[0] = 0;
}

/* Who owns a capture stream, for the indicator: always checked (resolve_as with verify), never a pulse client's word
 * alone. 1 when it is lord-server itself (its own recordings: the shell already shows them under the app they are for).
 * A stream with no verified app is still named when the kernel can tell (appid.h, "only to show it"): the app a process
 * that left its scope comes from, the app of a sandboxed pulse client (sandbox_owner), the sound system's own processes;
 * else the program's executable. None of this changes what the restrictions decide. */
static int capture_owner(PgClient *c, PwUse *w) {
    w->app[0] = w->exe[0] = w->sys[0] = 0;
    if (!c) return 0;                                       /* untracked: unknown */
    if (c->daemon || c->main_daemon || c->sound) { snprintf(w->sys, sizeof w->sys, "sound"); return 0; }
    if (!c->resolved) return 0;
    int unverified = 0, pid = 0;
    if (c->pulse && !guard_armed) resolve_as(c, 1, w->app, sizeof w->app, &unverified, &pid);
    else {
        unverified = c->unverified;
        snprintf(w->app, sizeof w->app, "%s", c->app);
        pid = c->pid_ok;
    }
    /* pid: the process it really is, also when it is "unverified" only because it runs in the user's manager while
     * restricted apps run (resolve_as); 0 when even that can't be established */
    if (unverified) w->app[0] = 0;
    else if (pid > 0 && pid == (int)getpid()) return 1;
    if (w->app[0]) return 0;
    double t = now_ms();
    if (t >= c->seen_until) {
        c->seen[0] = c->exe[0] = 0;
        if (pid > 0) {
            if (appid_of_lineage((unsigned)pid, c->seen, sizeof c->seen)) c->seen[0] = 0;
            if (!c->seen[0] && appid_exe_name((unsigned)pid, c->exe, sizeof c->exe)) c->exe[0] = 0;
        } else if (c->pulse) sandbox_owner(c, c->seen, sizeof c->seen);
        c->seen_until = t + 5000;
    }
    snprintf(w->app, sizeof w->app, "%s", c->seen);
    snprintf(w->exe, sizeof w->exe, "%s", c->exe);
    return 0;
}

static void use_report(void) {
    if (!use_emit) return;
    static PwUse u[MAX_PWLINKS];
    int n = 0;
    for (int i = 0; i < MAX_PWLINKS; i++) {
        PgLink *l = &P.link[i];
        if (!l->live) continue;
        PgNode *src = node_by_id(l->out), *dst = node_by_id(l->in);
        if (!src || !dst || !src->flag || !dst->stream) continue;
        PgClient *owner = client_by_id(dst->client);
        /* Noise Reduction's own capture (lordos-voice.conf, a module of the session's pipewire itself): the microphone
         * goes into the filter, not to an app; the app that reads the "LordOS Noise Reduction" source is the use, and its
         * own link (that source is an Audio/Source) is counted. Only when the node really is the daemon's. */
        if (dst->voice && (!dst->client || (owner && owner->main_daemon))) continue;
        if (capture_owner(owner, &u[n])) continue;
        u[n].flags = src->flag;
        n++;
    }
    char json[sizeof use_last];
    if (pwguard_use_format(u, n, json, sizeof json)) snprintf(json, sizeof json, "[{\"app\":\"\",\"mic\":1,\"camera\":1,\"screen\":0}]");
    if (!strcmp(json, use_last)) return;
    memcpy(use_last, json, sizeof json);
    use_emit("capture-use", json);
}

/* ---- events -------------------------------------------------------------------------------- */

static void on_global(PwPod body) {
    PwPod f;
    int32_t id;
    const char *type;
    if (pwpod_struct_field(body, 0, &f) || pwpod_int(f, &id) || pwpod_struct_field(body, 2, &f) || pwpod_string(f, &type)) return;
    PwPod props;
    int has_props = !pwpod_struct_field(body, 4, &props);
    if (!strcmp(type, "PipeWire:Interface:Client")) {
        PgClient *c = NULL;
        for (int i = 0; i < MAX_CLIENTS && !c; i++) if (!P.cl[i].live) c = &P.cl[i];
        uint32_t px = c ? proxy_new() : 0;
        if (!c || !px) {
            /* fail closed: a client we can't follow could record without its restriction */
            fprintf(stderr, "pwguard: too many PipeWire clients: client %d was disconnected\n", (int)id);
            destroy_global((uint32_t)id);
            return;
        }
        memset(c, 0, sizeof *c);
        c->live = 1;
        c->id = (uint32_t)id;
        {
            Pb b = { .n = 0 };
            size_t s = pb_begin(&b); pb_int(&b, id); pb_str(&b, "PipeWire:Interface:Client"); pb_int(&b, 3); pb_int(&b, (int32_t)px); pb_end(&b, s);
            send_msg(2, 1, &b);                                 /* registry.bind → client.info follows */
            c->proxy = px;
        }
        /* the others already restricted must not see this one either */
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (P.cl[i].live && &P.cl[i] != c && P.cl[i].applied) apply_permissions(&P.cl[i], P.cl[i].applied);
    } else if (!strcmp(type, "PipeWire:Interface:Node") && has_props) {
        const char *mc = dict_get(props, "media.class"), *cid = dict_get(props, "client.id");
        uint32_t flag = pwguard_node_flag(mc, dict_get(props, "media.role"), dict_get(props, "node.name")), stream = pwguard_stream_flag(mc);
        if (!flag && !stream) return;
        PgNode *nd = NULL;
        for (int i = 0; i < MAX_NODES && !nd; i++) if (!P.node[i].live) nd = &P.node[i];
        if (!nd) {
            /* fail closed: a capture stream, or a capture node of a client that is restricted or
             * not tracked, that we can't follow is destroyed */
            PgClient *oc = cid ? client_by_id((uint32_t)strtoul(cid, NULL, 10)) : NULL;
            if (guard_armed && (stream || !oc || oc->want)) {
                fprintf(stderr, "pwguard: too many capture nodes: node %d was destroyed\n", (int)id);
                destroy_global((uint32_t)id);
            }
            return;
        }
        nd->live = 1;
        nd->id = (uint32_t)id;
        nd->flag = flag;
        nd->stream = stream;
        nd->client = cid ? (uint32_t)strtoul(cid, NULL, 10) : 0;
        nd->voice = pwguard_voice_capture(mc, dict_get(props, "node.name"));
        if (flag)                                               /* a new microphone or camera: hidden from the restricted at once */
            for (int i = 0; i < MAX_CLIENTS; i++) if (P.cl[i].live && (P.cl[i].applied & flag)) apply_permissions(&P.cl[i], P.cl[i].applied);
        if (stream) kill_streams();
    } else if (!strcmp(type, "PipeWire:Interface:Link") && has_props) {
        const char *o = dict_get(props, "link.output.node"), *in = dict_get(props, "link.input.node");
        if (!o || !in) return;
        PgNode *src = node_by_id((uint32_t)strtoul(o, NULL, 10)), *dst = node_by_id((uint32_t)strtoul(in, NULL, 10));
        if (!src || !dst || !src->flag || !dst->stream) return;     /* only a capture node into a capture stream */
        PgLink *l = NULL;
        for (int i = 0; i < MAX_PWLINKS && !l; i++) if (!P.link[i].live) l = &P.link[i];
        if (!l) return;
        *l = (PgLink){ .id = (uint32_t)id, .out = src->id, .in = dst->id, .live = 1 };
        use_dirty = 1;
    } else if (!strcmp(type, "PipeWire:Interface:Port") && has_props) {
        const char *nid = dict_get(props, "node.id");
        if (!nid) return;
        uint32_t node = (uint32_t)strtoul(nid, NULL, 10);
        PgNode *nd = node_by_id(node);
        if (!nd || !nd->flag) return;
        PgPort *pt = NULL;
        for (int i = 0; i < MAX_PORTS && !pt; i++) if (!P.port[i].live) pt = &P.port[i];
        if (!pt) return;
        pt->live = 1;
        pt->id = (uint32_t)id;
        pt->node = node;
        for (int i = 0; i < MAX_CLIENTS; i++) if (P.cl[i].live && (P.cl[i].applied & nd->flag)) apply_permissions(&P.cl[i], P.cl[i].applied);
    }
}

static void on_global_remove(uint32_t id) {
    for (int i = 0; i < MAX_CLIENTS; i++) if (P.cl[i].live && P.cl[i].id == id) P.cl[i].live = 0;   /* its proxy: remove_id */
    for (int i = 0; i < MAX_NODES; i++) if (P.node[i].live && P.node[i].id == id) P.node[i].live = 0;
    for (int i = 0; i < MAX_PORTS; i++) if (P.port[i].live && P.port[i].id == id) P.port[i].live = 0;
    for (int i = 0; i < MAX_PWLINKS; i++)
        if (P.link[i].live && (P.link[i].id == id || P.link[i].in == id || P.link[i].out == id)) { P.link[i].live = 0; use_dirty = 1; }
}

/* client.info(id, change_mask, props): who it is */
static void on_client_info(uint32_t proxy, PwPod body) {
    PgClient *c = NULL;
    for (int i = 0; i < MAX_CLIENTS && !c; i++) if (P.cl[i].live && P.cl[i].proxy == proxy) c = &P.cl[i];
    PwPod props;
    if (!c || pwpod_struct_field(body, 2, &props)) return;
    const char *sp = dict_get(props, "pipewire.sec.pid"), *ap = dict_get(props, "application.process.id");
    c->pid_sec = sp ? atoi(sp) : 0;
    c->pid_app = ap ? atoi(ap) : 0;
    const char *fp = dict_get(props, "pipewire.access.portal.app_id");
    snprintf(c->fpid, sizeof c->fpid, "%s", fp && strlen(fp) < sizeof c->fpid ? fp : "");
    /* a pulse client only if the connection really comes from the session's pipewire-pulse; the
     * pulse.server.type property alone is the client's word */
    int pulse_conn = c->pid_sec > 0 && is_pulse_daemon(c->pid_sec);
    c->pulse = dict_get(props, "pulse.server.type") != NULL && pulse_conn;
    c->daemon = pulse_conn && !c->pulse;
    c->main_daemon = c->pid_sec > 0 && runs_exe(c->pid_sec, "/usr/bin/pipewire") && in_user_unit(c->pid_sec, "pipewire.service");
    /* only to name it in the capture indicator: the exact unit and the root-owned executable, as in_user_manager */
    c->sound = c->pid_sec > 0 && !c->main_daemon && !pulse_conn &&
               ((runs_exe(c->pid_sec, "/usr/bin/pipewire") && in_user_unit(c->pid_sec, "filter-chain.service")) ||
                (runs_exe(c->pid_sec, "/usr/bin/wireplumber") && in_user_unit(c->pid_sec, "wireplumber.service")));
    arm();
    resolve(c);
    evaluate(c);
    kill_streams();
}

static void on_message(uint32_t id, uint32_t opcode, PwPod body) {
    PwPod f;
    int32_t a, b;
    if (id == 0 && opcode == 2) {                               /* core.ping(id, seq) → pong */
        if (pwpod_struct_field(body, 0, &f) || pwpod_int(f, &a) || pwpod_struct_field(body, 1, &f) || pwpod_int(f, &b)) return;
        Pb p = { .n = 0 };
        size_t s = pb_begin(&p); pb_int(&p, a); pb_int(&p, b); pb_end(&p, s);
        send_msg(0, 3, &p);
    } else if (id == 0 && opcode == 3) {                        /* core.error */
        const char *msg = "";
        if (!pwpod_struct_field(body, 3, &f)) pwpod_string(f, &msg);
        fprintf(stderr, "pwguard: PipeWire error: %s\n", msg);
    } else if (id == 0 && opcode == 4) {                        /* core.remove_id */
        if (!pwpod_struct_field(body, 0, &f) && !pwpod_int(f, &a) && a > 2 && a < MAX_PROXY) P.proxy_used[a] = 0;
    } else if (id == 2 && opcode == 0) on_global(body);
    else if (id == 2 && opcode == 1) {
        if (!pwpod_struct_field(body, 0, &f) && !pwpod_int(f, &a)) on_global_remove((uint32_t)a);
    } else if (id > 2 && opcode == 0) on_client_info(id, body); /* our bound clients: info */
}

void pwguard_dispatch(void) {
    if (P.fd < 0) return;
    flush_out();
    for (;;) {
        if (P.fd < 0) return;
        char cbuf[CMSG_SPACE(28 * sizeof(int))];
        struct iovec io = { P.in + P.inlen, sizeof P.in - P.inlen };
        struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
        ssize_t n = recvmsg(P.fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) break;
        if (n <= 0) { drop(); return; }
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm))   /* we never use fds: close them */
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                int *fds = (int *)CMSG_DATA(cm);
                size_t k = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (size_t i = 0; i < k; i++) close(fds[i]);
            }
        P.inlen += (size_t)n;
        size_t off = 0;
        while (P.inlen - off >= 16) {
            uint32_t id = rd32(P.in + off), op = rd32(P.in + off + 4);
            uint32_t size = op & 0xffffff, opcode = op >> 24;
            if (size > IN_MAX - 16) { drop(); return; }
            if (P.inlen - off < 16 + (size_t)size) break;
            PwPod body = { P.in + off + 16, size };
            on_message(id, opcode, body);
            if (P.fd < 0) return;
            off += 16 + size;
        }
        memmove(P.in, P.in + off, P.inlen - off);
        P.inlen -= off;
        if (P.inlen == sizeof P.in) { drop(); return; }
    }
    flush_out();
}

void pwguard_tick(void) {
    double t = now_ms();
    if (P.fd < 0) {
        if (use_emit && strcmp(use_last, "[]")) { strcpy(use_last, "[]"); use_emit("capture-use", use_last); }   /* PipeWire went */
        if (t >= P.next_try) pwguard_start();
        return;
    }
    if (use_dirty) { use_dirty = 0; use_report(); }
    if (t < P.next_tick) return;
    P.next_tick = t + 1000;
    /* the rules or the running apps may have changed: apply or lift; who the pulse clients are is
     * checked again (their claims depend on who else is connected) */
    arm();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        PgClient *c = &P.cl[i];
        if (!c->live || !c->resolved) continue;
        if (c->pulse || c->unverified || !c->app[0]) resolve(c);   /* also: did it escape to user@ meanwhile? */
        evaluate(c);
    }
    kill_streams();
    use_report();                       /* who an app is may have been checked again */
    flush_out();
}
