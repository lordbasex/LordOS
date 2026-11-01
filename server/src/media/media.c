/* media.c — the lord-media helpers from lord-server (see media.h and docs/MEDIA.md).
 *
 * Rules:
 *   - lord-media is launched only from here, with the fixed path <lord-server folder>/lord-media,
 *     no arguments and a minimal environment. The binary has to be owned by the user or by root and not
 *     writable by others.
 *   - It never receives a path: lord-server validates each path (home folder or the user's
 *     mounts, like lord.fs), opens the file and passes it the fd. Outputs are created as
 *     temporaries next to the destination and renamed when they finish well, without overwriting anything.
 *   - The socket is a non-blocking SOCK_SEQPACKET: lord-server never waits for the helper. If the
 *     helper crashes or hangs, it's killed and only that window finds out ("closed" event).
 *   - The shared memory the helper sends has to come sealed against shrinking
 *     (F_SEAL_SHRINK): otherwise, a compromised helper could bring down lord-server with
 *     SIGBUS by truncating it. */
#include "media_int.h"
#include "hwdec.h"
#include "../fsops.h"
#include "../pkg/crypto.h"
#include "../ui/ui.h"
#include "../ui/js_sys.h"
#include "../errcode.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

static MediaSrc srcs[MEDIA_MAX_SRC];
static int next_id = 1;
static char payload[MP_PAYLOAD_MAX + 1];

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

const char *media_home(void) {
    const char *h = getenv("HOME");
    return h ? h : "/";
}

/* ---- events for JS ---- */

typedef struct { int id; char type[16]; char *json; } Ev;
static Ev *evq;
static int nev, evcap;

void media_json_str(char *out, size_t n, const char *s) {
    size_t o = 0;
    if (n < 3) { if (n) out[0] = 0; return; }
    out[o++] = '"';
    for (; s && *s && o + 8 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o++] = '"';
    out[o] = 0;
}

void media_event(int id, const char *type, const char *fmt, ...) {
    if (nev >= 2048) return;                         /* JS isn't listening: they're lost (doesn't grow without bound) */
    if (nev == evcap) {
        int cap = evcap ? evcap * 2 : 64;
        Ev *q = realloc(evq, (size_t)cap * sizeof *q);
        if (!q) return;
        evq = q;
        evcap = cap;
    }
    va_list ap;
    va_start(ap, fmt);
    char *j = NULL;
    if (vasprintf(&j, fmt, ap) < 0) j = NULL;
    va_end(ap);
    if (!j) return;
    Ev *e = &evq[nev++];
    e->id = id;
    snprintf(e->type, sizeof e->type, "%s", type);
    e->json = j;
}

void media_events(MediaEventFn fn) {
    /* fn can generate more events (JS calls lord.media): they're processed in the next round */
    int n = nev;
    Ev *q = evq;
    evq = NULL;
    nev = evcap = 0;
    for (int i = 0; i < n; i++) { if (fn) fn(q[i].id, q[i].type, q[i].json); free(q[i].json); }
    free(q);
}

/* ---- launching the helper ---- */

static int helper_path(char *out, size_t n) {
    char self[PATH_MAX];
    ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
    if (l <= 0) return -1;
    self[l] = 0;
    char *slash = strrchr(self, '/');
    if (!slash) return -1;
    *slash = 0;
    if ((size_t)snprintf(out, n, "%.*s/lord-media", (int)(n > 16 ? n - 16 : 0), self) >= n) return -1;
    struct stat st;
    if (stat(out, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) return -1;
    /* owned by the user or by root, and nobody else can change it */
    if ((st.st_uid != 0 && st.st_uid != getuid()) || (st.st_mode & (S_IWGRP | S_IWOTH))) {
        fprintf(stderr, "media: %s is not trusted (owner or permissions)\n", out);
        return -1;
    }
    return 0;
}

int media_spawn(int kind, const char *params, const int *fds, int nfds, pid_t *pid) {
    static char path[PATH_MAX];
    static int checked;
    if (!checked) { checked = helper_path(path, sizeof path) ? -1 : 1; if (checked < 0) fprintf(stderr, "media: lord-media is missing (no video, music or camera)\n"); }
    if (checked < 0) return -1;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    /* roomy buffers: messages are small, but frame notifications must not get stuck */
    int sz = 1 << 18;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
    pid_t p = fork();
    if (p < 0) { close(sv[0]); close(sv[1]); return -1; }
    if (p == 0) {
        sigset_t all;
        sigemptyset(&all);
        sigprocmask(SIG_SETMASK, &all, NULL);        /* lord-server blocks SIGTERM/SIGINT (signalfd) */
        for (int sig = 1; sig < 32; sig++) if (sig != SIGKILL && sig != SIGSTOP) signal(sig, SIG_DFL);
        if (dup2(sv[1], 3) < 0) _exit(127);
#ifdef SYS_close_range
        syscall(SYS_close_range, 4U, ~0U, 0);
#endif
        char *const argv[] = { "lord-media", NULL };
        static char lang[64], dbg[32], va[96];
        char *env[5];
        int ne = 0;
        snprintf(lang, sizeof lang, "LANG=C.UTF-8");
        env[ne++] = lang;
        if (getenv("LORD_MEDIA_DEBUG")) { snprintf(dbg, sizeof dbg, "LORD_MEDIA_DEBUG=1"); env[ne++] = dbg; }
        if (getenv("LIBVA_DRIVER_NAME")) { snprintf(va, sizeof va, "LIBVA_DRIVER_NAME=%.60s", getenv("LIBVA_DRIVER_NAME")); env[ne++] = va; }
        env[ne] = NULL;
        execve(path, argv, env);
        _exit(127);
    }
    close(sv[1]);
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    int64_t v[6] = { kind, 0, 0, 0, 0, 0 };
    if (mp_send(sv[0], MM_JOB, 0, v, params, params ? strlen(params) : 0, fds, nfds)) {
        kill(p, SIGKILL);
        waitpid(p, NULL, 0);
        close(sv[0]);
        return -1;
    }
    *pid = p;
    return sv[0];
}

/* Processes that no longer have a socket: they're waited for (and killed if they take too long). */
typedef struct { pid_t pid; double deadline; } Zombie;
static Zombie zombies[64];
static int nzombies;

static void bury(pid_t pid, double grace_ms) {
    if (pid <= 0) return;
    if (waitpid(pid, NULL, WNOHANG) == pid) return;
    if (nzombies < 64) zombies[nzombies++] = (Zombie){ pid, now_ms() + grace_ms };
    else { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
}

static void reap(void) {
    double t = now_ms();
    int k = 0;
    for (int i = 0; i < nzombies; i++) {
        if (waitpid(zombies[i].pid, NULL, WNOHANG) == zombies[i].pid) continue;
        if (t > zombies[i].deadline) { kill(zombies[i].pid, SIGKILL); if (waitpid(zombies[i].pid, NULL, WNOHANG) == zombies[i].pid) continue; zombies[i].deadline = t + 1000; }
        zombies[k++] = zombies[i];
    }
    nzombies = k;
}

/* Called on every pass of the main loop: with no source left media_pollfds has nothing and media_dispatch
 * doesn't run, so a helper that was just stopped would stay a zombie until the next video or song. */
void media_reap(void) { if (nzombies) reap(); }
double media_reap_timeout(void) { return nzombies ? 100 : -1; }

/* ---- descriptors that are passed ---- */

int media_pulse_fd(void) {
    char path[sizeof ((struct sockaddr_un *)0)->sun_path];
    const char *ps = getenv("PULSE_SERVER"), *rt = getenv("XDG_RUNTIME_DIR");
    char def[64];
    if (ps && !strncmp(ps, "unix:", 5)) snprintf(path, sizeof path, "%s", ps + 5);
    else {
        if (!rt || !*rt) { snprintf(def, sizeof def, "/run/user/%u", (unsigned)getuid()); rt = def; }
        snprintf(path, sizeof path, "%s/pulse/native", rt);
    }
    struct stat st;
    if (stat(path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != getuid()) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    return fd;
}

const char *media_hwaccel(void) {
    const char *e = getenv("LORD_MEDIA_HWACCEL");
    if (e && (!strcmp(e, "vaapi") || !strcmp(e, "v4l2request") || !strcmp(e, "none"))) return e;
    if (getenv("LORD_MEDIA_HW") && !strcmp(getenv("LORD_MEDIA_HW"), "0")) return "none";
    /* the Pi 5: no VA-API, a stateless HEVC decoder */
    static int pi = -1;
    if (pi < 0) {
        pi = 0;
        for (int i = 0; i < 32 && !pi; i++) {
            char p[64], name[32] = "";
            snprintf(p, sizeof p, "/sys/class/video4linux/video%d/name", i);
            FILE *f = fopen(p, "r");
            if (!f) continue;
            if (fgets(name, sizeof name, f) && !strncmp(name, "rpi-hevc-dec", 12)) pi = 1;
            fclose(f);
        }
    }
    return pi ? "v4l2request" : "vaapi";
}

int media_render_fd(void) {
    if (!strcmp(media_hwaccel(), "none")) return -1;
    for (int i = 128; i < 136; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/dri/renderD%d", i);
        int fd = open(p, O_RDWR | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
    return -1;
}

/* ---- paths ---- */

int media_open_input(const char *path, char *real, size_t n) {
    char full[PATH_MAX * 2], r[PATH_MAX];
    if (!path || !*path || strlen(path) >= PATH_MAX) return -1;
    if (path[0] == '/') snprintf(full, sizeof full, "%s", path);
    else snprintf(full, sizeof full, "%s/%s", media_home(), path);
    if (!realpath(full, r)) return -1;
    /* the system's moving wallpapers are readable by anyone, the login screen too (docs/WALLPAPERS.md) */
    if (strncmp(r, "/var/lib/lordos/wallpapers/", 27) && strncmp(r, "/usr/share/backgrounds/lordos/motion/", 37) && !fsops_allowed(r)) return -1;
    int fd = open(r, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) { if (fd >= 0) close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    if (real) snprintf(real, n, "%s", r);
    return fd;
}

int media_open_output(const char *path, char *tmp, size_t tn, char *dst, size_t dn) {
    if (!path || js_sys_resolve_target(path, dst, dn)) return -1;
    struct stat st;
    if (!lstat(dst, &st)) { errno = EEXIST; return -2; }          /* a file is never overwritten */
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", dst);
    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    *slash = 0;
    static unsigned seq;
    for (int i = 0; i < 20; i++) {
        snprintf(tmp, tn, "%s/.lordos-media-%d-%u.part", dir, (int)getpid(), ++seq);
        int fd = open(tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    return -1;
}

int media_finish_output(const char *tmp, const char *dst, int ok) {
    if (!tmp || !tmp[0]) return -1;
    if (!ok) { unlink(tmp); return -1; }
    if (syscall(SYS_renameat2, AT_FDCWD, tmp, AT_FDCWD, dst, 1 /* RENAME_NOREPLACE */)) {
        if (errno == EINVAL && link(tmp, dst) == 0) { unlink(tmp); return 0; }     /* fs without renameat2 */
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---- sources ---- */

MediaSrc *media_src_new(int kind, const char *app) {
    for (int i = 0; i < MEDIA_MAX_SRC; i++) {
        MediaSrc *s = &srcs[i];
        if (s->used) continue;
        memset(s, 0, sizeof *s);
        s->used = 1;
        s->id = next_id++;
        s->kind = kind;
        s->sock = s->in_fd = -1;
        s->cur_slot = -1;
        snprintf(s->app, sizeof s->app, "%s", app ? app : "");
        media_view_reset(s);
        return s;
    }
    return NULL;
}

MediaSrc *media_src_at(int i) { return i >= 0 && i < MEDIA_MAX_SRC && srcs[i].used ? &srcs[i] : NULL; }

MediaSrc *media_src_get(int id) {
    for (int i = 0; i < MEDIA_MAX_SRC; i++) if (srcs[i].used && srcs[i].id == id) return &srcs[i];
    return NULL;
}

void media_src_damage(MediaSrc *s) {
    Node *n = s->node ? node_get(s->node) : NULL;
    /* only the node's box (not the shadow margin): that way a new frame falls inside it (media_cover_node) */
    if (n && n->type == NODE_MEDIA && n->term == s->id && n->prev_box.x1 > n->prev_box.x0)
        damage_add((Box){ floorf(n->ax), floorf(n->ay), ceilf(n->ax + n->w), ceilf(n->ay + n->h) });
    else s->node = 0;
    ui.dirty = 1;
}

static void unmap_pixels(MediaSrc *s) {
    if (s->map) munmap(s->map, s->map_len);
    s->map = NULL;
    s->map_len = 0;
    if (s->kind == SK_CAPTURE) free(s->px);
    s->px = NULL;
    free(s->cache);
    s->cache = NULL;
    for (int i = 0; i < s->nmip; i++) free(s->mip[i]);
    s->nmip = 0;
}

static void drop_helper(MediaSrc *s, int grace) {
    if (s->sock >= 0) { mp_sendv(s->sock, MM_STOP, 0, 0, 0, 0, 0); close(s->sock); }
    s->sock = -1;
    bury(s->pid, grace);
    s->pid = 0;
}

void media_src_close(MediaSrc *s) {
    if (!s || !s->used) return;
    /* a recording in progress is closed properly: it's asked to finish and the end is waited for */
    if ((s->kind == SK_SCREENREC || s->kind == SK_AUDIOREC) && s->sock >= 0 && s->state == MS_RECORDING) {
        mp_sendv(s->sock, MM_STOP, 0, 0, 0, 0, 0);
        s->closing = 1;
        return;
    }
    if (s->kind == SK_SCREENREC) media_capture_src_closed(s);
    if (s->kind == SK_PLAYER && s->stats[0])
        fprintf(stderr, "media: playback (%s): %lld frames decoded, %lld shown, %lld dropped, %s, average A/V offset %.2f ms (max. %lld)\n",
                s->app, (long long)s->stats[0], (long long)s->stats[1], (long long)s->stats[2], s->stats[3] ? "VA-API" : "software",
                s->stats[4] / 100.0, (long long)s->stats[5]);
    drop_helper(s, 2000);
    if (s->tmp[0]) media_finish_output(s->tmp, s->dst, 0);
    unmap_pixels(s);
    if (s->in_map) munmap(s->in_map, s->in_len);
    if (s->in_fd >= 0) close(s->in_fd);
    maudio_close(s->audio);
    maudio_close(s->mic);
    s->audio = s->mic = NULL;
    media_src_damage(s);
    s->used = 0;
}

static const char *state_name(int st) {
    static const char *const n[] = { "loading", "playing", "paused", "ended", "recording", "done", "failed" };
    return st >= 0 && st <= MS_FAILED ? n[st] : "?";
}

/* Helper's shared memory: it has to be sealed against shrinking and be large enough. */
static uint8_t *map_sealed(int fd, size_t need) {
    struct stat st;
    int seals = fcntl(fd, F_GET_SEALS);
    if (need == 0 || fstat(fd, &st) || (size_t)st.st_size < need || seals < 0 || !(seals & F_SEAL_SHRINK)) {
        fprintf(stderr, "media: invalid shared memory (unsealed or too small)\n");
        return NULL;
    }
    void *m = mmap(NULL, need, PROT_READ, MAP_SHARED, fd, 0);
    return m == MAP_FAILED ? NULL : m;
}

static void on_src_msg(MediaSrc *s, const MediaMsg *m, int *fds) {
    char esc[MP_PAYLOAD_MAX * 2 + 16];
    if (s->bk_msg && s->bk_msg(s, m, payload, fds)) return;    /* camera of an isolated app (brokercam.c) */
    switch (m->type) {
    case MM_INFO:
        if (m->len && payload[0] == '{') media_event(s->id, "info", "%s", payload);
        break;
    case MM_RING: {
        int64_t w = m->v[1], h = m->v[2], slots = m->v[3];
        if (fds[0] < 0 || w < 1 || h < 1 || w > MP_MAX_DIM || h > MP_MAX_DIM || slots < 1 || slots > MP_RING_SLOTS) break;
        size_t need = (size_t)w * (size_t)h * 4 * (size_t)slots;
        uint8_t *map = map_sealed(fds[0], need);
        if (!map) break;
        if (s->map) munmap(s->map, s->map_len);
        s->map = map;
        s->map_len = need;
        s->ring_gen = (int)m->v[0];
        s->ring_w = (int)w;
        s->ring_h = (int)h;
        s->ring_slots = (int)slots;
        s->cur_slot = -1;
        break;
    }
    case MM_FRAME: {
        int gen = (int)(m->id >> 8), slot = (int)(m->id & 0xff);
        if (!s->map || gen != s->ring_gen || slot >= s->ring_slots) break;
        s->cur_slot = slot;
        s->w = s->ring_w;
        s->h = s->ring_h;
        s->frame_seq++;
        s->pos = m->v[0];
        mp_sendv(s->sock, MM_ACK, m->id, 0, 0, 0, 0);
        media_src_damage(s);
        break;
    }
    case MM_POS:
        s->pos = m->v[0];
        s->dur = m->v[1];
        s->playing = (int)m->v[2];
        media_event(s->id, "pos", "{\"pos\":%lld,\"duration\":%lld,\"playing\":%s}", (long long)s->pos, (long long)s->dur, s->playing ? "true" : "false");
        break;
    case MM_AUDIO:
        maudio_service(s->audio);
        break;
    case MM_STATE:
        s->state = (int)m->v[0];
        if (s->audio) maudio_playing(s->audio, s->state == MS_PLAYING);
        if (s->state == MS_DONE || s->state == MS_FAILED) {
            if (s->kind == SK_SCREENREC || s->kind == SK_AUDIOREC) {
                int ok = s->state == MS_DONE && !media_finish_output(s->tmp, s->dst, 1);
                if (!ok && s->tmp[0]) media_finish_output(s->tmp, s->dst, 0);
                s->tmp[0] = 0;
                media_json_str(esc, sizeof esc, ok ? s->dst : "");
                media_event(s->id, "done", "{\"ok\":%s,\"path\":%s,\"duration\":%lld}", ok ? "true" : "false", esc, (long long)m->v[1]);
                if (s->kind == SK_SCREENREC) media_capture_src_closed(s);
                if (s->closing) { s->closing = 0; s->state = MS_DONE; media_src_close(s); return; }
            }
        }
        media_event(s->id, "state", "{\"state\":\"%s\"}", state_name(s->state));
        break;
    case MM_SUB:
        media_json_str(esc, sizeof esc, payload);
        media_event(s->id, "sub", "{\"start\":%lld,\"end\":%lld,\"text\":%s}", (long long)m->v[0], (long long)m->v[1], esc);
        break;
    case MM_LEVEL:
        media_event(s->id, "level", "{\"level\":%lld,\"ms\":%lld,\"a\":%lld,\"b\":%lld}", (long long)m->v[0], (long long)m->v[1],
                    (long long)m->v[2], (long long)m->v[3]);
        break;
    case MM_STATS:
        memcpy(s->stats, m->v, sizeof s->stats);
        media_event(s->id, "stats", "{\"decoded\":%lld,\"shown\":%lld,\"dropped\":%lld,\"hw\":%s,\"underruns\":%lld}", (long long)m->v[0],
                    (long long)m->v[1], (long long)m->v[2], m->v[3] ? "true" : "false", (long long)m->v[5]);
        break;
    case MM_SLOTFREE:
        if (m->id < 4) s->in_busy[m->id] = 0;
        break;
    case MM_RESULT: {
        /* camera: photo or video finished (id = 1 photo, 2 video) */
        if (s->kind == SK_CAMERA) {
            char *tmp = m->id == 1 ? s->tmp : s->tmp + 2048, *dst = m->id == 1 ? s->dst : s->dst + 2048;
            int ok = m->v[0] == 0 && !media_finish_output(tmp, dst, 1);
            if (!ok && tmp[0]) media_finish_output(tmp, dst, 0);
            media_json_str(esc, sizeof esc, ok ? dst : "");
            media_event(s->id, m->id == 1 ? "photo" : "video", "{\"ok\":%s,\"path\":%s}", ok ? "true" : "false", esc);
            tmp[0] = 0;
        }
        break;
    }
    case MM_ERROR:
        media_json_str(esc, sizeof esc, payload);
        media_event(s->id, "error", "{\"message\":%s}", esc);
        break;
    default: break;
    }
}

static void src_dispatch(MediaSrc *s) {
    for (int k = 0; k < 64 && s->sock >= 0; k++) {
        MediaMsg m;
        int fds[MP_MAX_FDS];
        int r = mp_recv(s->sock, &m, payload, sizeof payload, fds, 0);
        if (r < 0) break;
        if (r == 0) {
            int crashed = s->state != MS_DONE && s->state != MS_ENDED && !s->closing;
            int pid = s->pid;
            close(s->sock);
            s->sock = -1;
            int st = 0;
            if (pid > 0 && waitpid(pid, &st, WNOHANG) == pid) { s->pid = 0; if (WIFSIGNALED(st)) crashed = 1; }
            else bury(pid, 500), s->pid = 0;
            if (s->tmp[0]) { media_finish_output(s->tmp, s->dst, 0); s->tmp[0] = 0; }
            if (s->kind == SK_SCREENREC) media_capture_src_closed(s);
            if (crashed) fprintf(stderr, "media: lord-media (%s) terminated abruptly; the window stays\n", s->app);
            media_event(s->id, "closed", "{\"crashed\":%s}", crashed ? "true" : "false");
            if (s->closing) { s->closing = 0; s->state = MS_DONE; media_src_close(s); }
            return;
        }
        on_src_msg(s, &m, fds);
        for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]);
        if (!s->used) return;
    }
}

/* ---- workers: short queued jobs ---- */

typedef struct Job {
    int id, bg, kind, purpose, src, nfds;
    int fds[MP_MAX_FDS];
    int64_t a, b;
    char *params, *tmp, *dst;
    struct Job *next;
} Job;

typedef struct { int sock; pid_t pid; Job *cur; double idle_since; } Worker;
static Worker workers[2] = { { -1, 0, NULL, 0 }, { -1, 0, NULL, 0 } };
static Job *queue[2], *queue_tail[2];
static int queued[2];
static int next_job = 1;

/* pixels ready for lord.media.take (posters) */
typedef struct { int job, w, h; uint8_t *rgba; double at; } Pixels;
static Pixels pix[16];

static void job_free(Job *j) {
    for (int i = 0; i < j->nfds; i++) if (j->fds[i] >= 0) close(j->fds[i]);
    free(j->params);
    free(j->tmp);
    free(j->dst);
    free(j);
}

static void thumb_done(const char *key, int ok);

static void job_finish(Job *j, int status, int fd) {
    char esc[PATH_MAX * 2 + 8], res[MP_PAYLOAD_MAX * 2 + 8];
    int ok = status == 0;
    switch (j->purpose) {
    case JP_THUMB: {
        ok = ok && !media_finish_output(j->tmp, j->dst, 1);
        if (!ok && j->tmp) unlink(j->tmp);
        thumb_done(j->dst, ok);
        media_json_str(esc, sizeof esc, j->params ? j->params : "");
        media_json_str(res, sizeof res, ok ? j->dst : "");
        media_event(0, "thumb", "{\"src\":%s,\"thumb\":%s}", esc, res);
        break;
    }
    case JP_FILE:
        ok = ok && !media_finish_output(j->tmp, j->dst, 1);
        if (!ok && j->tmp) media_finish_output(j->tmp, j->dst, 0);
        media_json_str(esc, sizeof esc, ok ? j->dst : "");
        media_event(0, "job", "{\"id\":%d,\"ok\":%s,\"path\":%s,\"result\":%s}", j->id, ok ? "true" : "false", esc,
                    ok && payload[0] == '{' ? payload : "null");
        break;
    case JP_CLIP: {
        ok = ok && !media_finish_output(j->tmp, j->dst, 1);
        extern int wl_clip_use_image(const char *path);
        if (ok) ok = wl_clip_use_image(j->dst) == 0;
        if (j->dst) unlink(j->dst);
        media_event(0, "job", "{\"id\":%d,\"ok\":%s}", j->id, ok ? "true" : "false");
        break;
    }
    case JP_IMAGE: {
        MediaSrc *s = media_src_get(j->src);
        if (s && s->kind == SK_IMAGE && s->job == j->id) {
            int w = 0, h = 0;
            sscanf(payload, "{\"width\":%d,\"height\":%d", &w, &h);
            uint8_t *map = (ok && fd >= 0 && w > 0 && h > 0 && w <= MP_MAX_DIM && h <= MP_MAX_DIM) ? map_sealed(fd, (size_t)w * h * 4) : NULL;
            if (map) {
                s->map = map;
                s->map_len = (size_t)w * h * 4;
                s->px = (uint32_t *)map;
                s->w = w;
                s->h = h;
                s->opaque = !strstr(payload, "\"alpha\":true");
                s->frame_seq++;
                media_src_damage(s);
                media_event(s->id, "ready", "%s", payload);
            } else {
                media_json_str(esc, sizeof esc, ok || !lerr_is(payload) ? "media.imageUnreadable" : payload);
                media_event(s->id, "error", "{\"message\":%s}", esc);
            }
        }
        break;
    }
    case JP_PIXELS: {
        int w = 0, h = 0;
        sscanf(payload, "{\"width\":%d,\"height\":%d", &w, &h);
        uint8_t *map = (ok && fd >= 0 && w > 0 && h > 0 && w <= 1024 && h <= 1024) ? map_sealed(fd, (size_t)w * h * 4) : NULL;
        if (map) {
            int slot = 0;
            for (int i = 1; i < 16; i++) if (pix[i].at < pix[slot].at) slot = i;
            free(pix[slot].rgba);
            pix[slot] = (Pixels){ j->id, w, h, malloc((size_t)w * h * 4), now_ms() };
            if (pix[slot].rgba) {
                /* premultiplied BGRA → RGBA (what <Image pixels> expects) */
                const uint8_t *p = map;
                uint8_t *o = pix[slot].rgba;
                for (size_t i = 0; i < (size_t)w * h; i++, p += 4, o += 4) {
                    unsigned a = p[3];
                    o[0] = a && a < 255 ? (uint8_t)(p[2] * 255u / a) : p[2];
                    o[1] = a && a < 255 ? (uint8_t)(p[1] * 255u / a) : p[1];
                    o[2] = a && a < 255 ? (uint8_t)(p[0] * 255u / a) : p[0];
                    o[3] = (uint8_t)a;
                }
            }
            munmap(map, (size_t)w * h * 4);
        } else ok = 0;
        media_event(0, "job", "{\"id\":%d,\"ok\":%s,\"result\":%s}", j->id, ok ? "true" : "false", ok && payload[0] == '{' ? payload : "null");
        break;
    }
    default:
        if (ok && payload[0] == '{') media_event(0, "job", "{\"id\":%d,\"ok\":true,\"result\":%s}", j->id, payload);
        else {
            media_json_str(res, sizeof res, payload);
            media_event(0, "job", "{\"id\":%d,\"ok\":false,\"error\":%s}", j->id, res);
        }
    }
}

int media_take_pixels(int job, uint8_t **out, int *w, int *h) {
    for (int i = 0; i < 16; i++) {
        if (pix[i].job != job || !pix[i].rgba) continue;
        *out = pix[i].rgba;
        *w = pix[i].w;
        *h = pix[i].h;
        pix[i] = (Pixels){ 0 };
        return 0;
    }
    return -1;
}

static void worker_kick(int bg) {
    Worker *wk = &workers[bg];
    if (wk->cur || !queue[bg]) return;
    if (wk->sock < 0) {
        wk->sock = media_spawn(MK_WORKER, NULL, NULL, 0, &wk->pid);
        if (wk->sock < 0) {
            /* no lord-media: all jobs fail */
            while (queue[bg]) { Job *j = queue[bg]; queue[bg] = j->next; queued[bg]--; payload[0] = 0; snprintf(payload, sizeof payload, "media.noHelper"); job_finish(j, -1, -1); job_free(j); }
            queue_tail[bg] = NULL;
            return;
        }
    }
    Job *j = queue[bg];
    queue[bg] = j->next;
    if (!queue[bg]) queue_tail[bg] = NULL;
    queued[bg]--;
    int64_t v[6] = { j->kind, j->a, j->b, 0, 0, 0 };
    if (mp_send(wk->sock, MM_JOB, (uint32_t)j->id, v, j->params && j->purpose != JP_THUMB ? j->params : NULL,
                j->params && j->purpose != JP_THUMB ? strlen(j->params) : 0, j->fds, j->nfds)) {
        snprintf(payload, sizeof payload, "media.helperNotResponding");
        job_finish(j, -1, -1);
        job_free(j);
        return;
    }
    /* the helper already has the fds */
    for (int i = 0; i < j->nfds; i++) if (j->fds[i] >= 0) { close(j->fds[i]); j->fds[i] = -1; }
    wk->cur = j;
}

int media_job(int bg, int kind, int purpose, const int *fds, int nfds, int64_t a, int64_t b, const char *params,
              const char *tmp, const char *dst, int src) {
    bg = bg ? 1 : 0;
    if (queued[bg] > 600) { for (int i = 0; i < nfds; i++) if (fds[i] >= 0) close(fds[i]); return -1; }
    Job *j = calloc(1, sizeof *j);
    if (!j) { for (int i = 0; i < nfds; i++) if (fds[i] >= 0) close(fds[i]); return -1; }
    j->id = next_job++;
    j->bg = bg;
    j->kind = kind;
    j->purpose = purpose;
    j->src = src;
    j->a = a;
    j->b = b;
    j->nfds = nfds > MP_MAX_FDS ? MP_MAX_FDS : nfds;
    for (int i = 0; i < j->nfds; i++) j->fds[i] = fds[i];
    j->params = params ? strdup(params) : NULL;
    j->tmp = tmp ? strdup(tmp) : NULL;
    j->dst = dst ? strdup(dst) : NULL;
    if (queue_tail[bg]) queue_tail[bg]->next = j; else queue[bg] = j;
    queue_tail[bg] = j;
    queued[bg]++;
    worker_kick(bg);
    return j->id;
}

static void worker_dispatch(int bg) {
    Worker *wk = &workers[bg];
    for (int k = 0; k < 16 && wk->sock >= 0; k++) {
        MediaMsg m;
        int fds[MP_MAX_FDS];
        int r = mp_recv(wk->sock, &m, payload, sizeof payload, fds, 0);
        if (r < 0) break;
        if (r == 0) {
            close(wk->sock);
            wk->sock = -1;
            bury(wk->pid, 500);
            wk->pid = 0;
            if (wk->cur) {
                fprintf(stderr, "media: lord-media crashed during a job (%d); moving on to the next\n", wk->cur->kind);
                snprintf(payload, sizeof payload, "media.helperCrashed");
                job_finish(wk->cur, -1, -1);
                job_free(wk->cur);
                wk->cur = NULL;
            }
            break;
        }
        if (m.type == MM_RESULT && wk->cur && m.id == (uint32_t)wk->cur->id) {
            Job *j = wk->cur;
            wk->cur = NULL;
            job_finish(j, (int)m.v[0], fds[0]);
            job_free(j);
            wk->idle_since = now_ms();
        }
        for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]);
    }
    worker_kick(bg);
}

/* ---- thumbnails: ~/.cache/lordos/thumbs/<sha256>.png ---- */

#define THUMB_TRACK 4096
static uint64_t thumb_keys[THUMB_TRACK];        /* requested or failed (not retried) */
static int thumb_n;

static uint64_t key64(const char *s) { uint64_t h = 1469598103934665603ull; for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull; return h; }

static void thumb_done(const char *path, int ok) {
    if (!ok || !path) return;
    uint64_t k = key64(path);
    for (int i = 0; i < thumb_n; i++) if (thumb_keys[i] == k) { thumb_keys[i] = thumb_keys[--thumb_n]; break; }
}

static void mkdirs(const char *path) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0700); *s = '/'; }
    mkdir(p, 0700);
}

const char *media_thumb(const char *path, int size, char *out, size_t n) {
    char real[PATH_MAX];
    struct stat st;
    if (size < 16) size = 16;
    if (size > 1024) size = 1024;
    char full[PATH_MAX * 2];
    if (!path || !*path) return NULL;
    if (path[0] == '/') snprintf(full, sizeof full, "%s", path); else snprintf(full, sizeof full, "%s/%s", media_home(), path);
    if (!realpath(full, real) || !fsops_allowed(real) || stat(real, &st) || !S_ISREG(st.st_mode)) return NULL;
    char key[PATH_MAX + 64];
    snprintf(key, sizeof key, "%s\n%lld\n%lld\n%d", real, (long long)st.st_mtime, (long long)st.st_size, size);
    uint8_t h[32];
    sha256(key, strlen(key), h);
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.cache/lordos/thumbs", media_home());
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", h[i]);
    snprintf(out, n, "%s/%s.png", dir, hex);
    if (!access(out, R_OK)) return out;
    uint64_t k = key64(out);
    for (int i = 0; i < thumb_n; i++) if (thumb_keys[i] == k) return NULL;     /* requested or failed */
    if (thumb_n >= THUMB_TRACK) return NULL;
    mkdirs(dir);
    int in = media_open_input(real, NULL, 0);
    if (in < 0) return NULL;
    char tmp[PATH_MAX + 64];
    snprintf(tmp, sizeof tmp, "%.*s/.%s.%d.part", PATH_MAX - 64, dir, hex, (int)getpid());
    int o = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (o < 0) { close(in); return NULL; }
    thumb_keys[thumb_n++] = k;
    int fds[2] = { in, o };
    /* params carries the source path only for the event (it's not sent to the helper) */
    media_job(1, MW_THUMB, JP_THUMB, fds, 2, size, 0, real, tmp, out, 0);
    return NULL;
}

/* ---- cameras ---- */

int media_cameras(char *json, size_t n) {
    size_t o = 0;
    o += (size_t)snprintf(json + o, n - o, "[");
    int first = 1;
    for (int i = 0; i < 64 && o + 300 < n; i++) {
        char dev[32];
        snprintf(dev, sizeof dev, "/dev/video%d", i);
        int fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct v4l2_capability cap;
        memset(&cap, 0, sizeof cap);
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
            if ((caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING)) {
                char name[80], bus[80];
                media_json_str(name, sizeof name, (const char *)cap.card);
                media_json_str(bus, sizeof bus, (const char *)cap.bus_info);
                o += (size_t)snprintf(json + o, n - o, "%s{\"device\":\"%s\",\"name\":%s,\"bus\":%s}", first ? "" : ",", dev, name, bus);
                first = 0;
            }
        }
        close(fd);
    }
    snprintf(json + o, n - o, "]");
    return 0;
}

/* ---- main loop ---- */

int media_pollfds(struct pollfd *p, int max) {
    int n = 0;
    for (int i = 0; i < 2 && n < max; i++) if (workers[i].sock >= 0) p[n++] = (struct pollfd){ workers[i].sock, POLLIN, 0 };
    int hw = hwdec_pollfd();                   /* the hardware decoding probe (hwdec.c), once per boot */
    if (hw >= 0 && n < max) p[n++] = (struct pollfd){ hw, POLLIN, 0 };
    for (int i = 0; i < MEDIA_MAX_SRC && n < max; i++) if (srcs[i].used && srcs[i].sock >= 0) p[n++] = (struct pollfd){ srcs[i].sock, POLLIN, 0 };
    for (int i = 0; i < MEDIA_MAX_SRC && n < max; i++) {
        if (!srcs[i].used) continue;
        MAudio *as[2] = { srcs[i].audio, srcs[i].mic };
        for (int k = 0; k < 2 && n < max; k++) {
            short ev;
            int fd = maudio_pollfd(as[k], &ev);
            if (fd >= 0) p[n++] = (struct pollfd){ fd, ev, 0 };
        }
    }
    return n;
}

void media_dispatch(const struct pollfd *p, int n) {
    for (int k = 0; k < n; k++) {
        if (!p[k].revents) continue;
        int fd = p[k].fd;
        if (fd == hwdec_pollfd()) { hwdec_dispatch(); continue; }
        for (int i = 0; i < 2; i++) if (workers[i].sock == fd) worker_dispatch(i);
        for (int i = 0; i < MEDIA_MAX_SRC; i++) {
            if (!srcs[i].used) continue;
            if (srcs[i].sock == fd) { src_dispatch(&srcs[i]); continue; }
            short ev;
            if (srcs[i].audio && maudio_pollfd(srcs[i].audio, &ev) == fd) maudio_service(srcs[i].audio);
            if (srcs[i].used && srcs[i].mic && maudio_pollfd(srcs[i].mic, &ev) == fd) maudio_service(srcs[i].mic);
        }
    }
    reap();
}

void media_shutdown(void) {
    for (int i = 0; i < MEDIA_MAX_SRC; i++) if (srcs[i].used) { srcs[i].closing = 0; srcs[i].state = MS_DONE; media_src_close(&srcs[i]); }
    for (int i = 0; i < MEDIA_MAX_SRC; i++) if (srcs[i].used) { drop_helper(&srcs[i], 2000); srcs[i].used = 0; }
    for (int b = 0; b < 2; b++) {
        while (queue[b]) { Job *j = queue[b]; queue[b] = j->next; if (j->tmp) unlink(j->tmp); job_free(j); }
        queue_tail[b] = NULL;
        queued[b] = 0;
        if (workers[b].cur) { if (workers[b].cur->tmp) unlink(workers[b].cur->tmp); job_free(workers[b].cur); workers[b].cur = NULL; }
        if (workers[b].sock >= 0) { close(workers[b].sock); workers[b].sock = -1; bury(workers[b].pid, 1000); }
    }
    thumb_n = 0;
    hwdec_shutdown();
    media_events(NULL);
}
