/* cast.c — lord-server's side of sharing a screen or a window (see cast.h and castproto.h).
 *
 * Per session: one lord-screencast, started from <lord-server dir>/lord-screencast with no arguments, LANG only and
 * the channel on fd 3. lord-server connects the user's PipeWire socket itself ($XDG_RUNTIME_DIR/pipewire-0, a socket
 * owned by the user, and the daemon at the other end runs as the user) and hands the connected fd over: the helper
 * can make no socket. Each source gets a ring of CAST_SLOTS frames (a memfd sealed against shrinking and growing;
 * the helper gets a read-only reopen of it).
 *
 * The pixels: each source is painted again in capture mode, as Screenshots does (paint_capture: protected windows
 * black; a window alone with paint_capture_node: nothing over it, no other window, only its own sheets), into a
 * canvas of its own and only where it changed (cast_damage, and wl_content_take for Linux apps' new frames, which
 * on the GPU path are only textures). A slot keeps what it missed (stale) since it was last written, so filling it
 * copies only that. With the pointer embedded, the arrow is drawn into the slot and that place is stale next time.
 * Nothing is painted before the consumer asks (CM_WANT), before the indicator is on screen (cast_shown), or faster
 * than the rate it asked for. */
#include "cast.h"
#include "castproto.h"
#include "../ui/ui.h"
#include "../ui/portal.h"
#include "../media/media.h"
#include "../media/media_int.h"
#include "../media/imgops.h"
#include "../hw/display.h"
#include "../wl/wl.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_RUNS 4                      /* sessions sharing at once */
#define START_MS 8000.0                 /* the helper's nodes within this, or the Start fails */
#define REAP_MS 1000.0                  /* a stopped helper that lingers is killed */
#define ARROW 24                        /* the embedded arrow's box, px */

typedef struct {
    CastSource src;
    uint32_t w, h;                      /* the stream's size (physical px) */
    double k;                           /* physical = (logical - (ox, oy)) * k + (dx, dy) */
    float ox, oy;
    int dx, dy;
    int lx, ly, lw, lh;                 /* the source in logical px */
    int ring_fd;
    uint8_t *map;
    CastLayout lay;
    uint32_t *px;                       /* the source painted, kept between frames */
    Canvas cv;
    Rect dirty;                         /* of cv, still to paint */
    Rect stale[CAST_SLOTS];             /* of each slot, older than cv */
    int want;
    double period, last;
    uint64_t seq;
} Stream;

typedef struct {
    int id;                             /* the portal's session; 0 free */
    int sock;
    pid_t pid;
    CastPeer peer;
    Stream st[CAST_MAX_STREAMS];
    int n, cursor, shown, told;         /* told: portal_cast_streams was called */
    int gone;                           /* a shared window was destroyed (node_gone_hook): the share ends next tick */
    double since;
} Run;

static Run runs[MAX_RUNS];
static int nrunning;
static char helper[PATH_MAX];
static int ptr_x = -100000, ptr_y = -100000;
static pid_t zombies[8];
static double zombie_at[8];

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static Rect rect_join(Rect a, Rect b) {
    if (a.x1 <= a.x0 || a.y1 <= a.y0) return b;
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return a;
    return (Rect){ a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
}
static Rect rect_in(Rect r, int w, int h) {
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > w) r.x1 = w;
    if (r.y1 > h) r.y1 = h;
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return (Rect){ 0, 0, 0, 0 };
    return r;
}
static int rect_empty(Rect r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }

int cast_available(void) {
    if (!helper[0]) {
        char self[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n <= 0) return 0;
        self[n] = 0;
        char *slash = strrchr(self, '/');
        if (!slash) return 0;
        *slash = 0;
        if (strlen(self) + 17 > sizeof helper) return 0;
        snprintf(helper, sizeof helper, "%s/lord-screencast", self);
    }
    struct stat st, me;
    if (stat(helper, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) return 0;
    /* owned by root or by lord-server's owner, and not writable by anyone else */
    if (stat("/proc/self/exe", &me) || (st.st_uid != 0 && st.st_uid != me.st_uid) || (st.st_mode & 022)) return 0;
    return 1;
}

/* ---- the user's PipeWire ---- */

/* $XDG_RUNTIME_DIR/pipewire-0: a socket owned by us, and the process at the other end ours too. Non-blocking, so a
 * daemon whose backlog is full is a failed share, not a compositor waiting. */
static int pipewire_connect(void) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (!rt || rt[0] != '/' || strlen(rt) + 12 >= sizeof a.sun_path) return -1;
    snprintf(a.sun_path, sizeof a.sun_path, "%s/pipewire-0", rt);
    struct stat st;
    if (lstat(a.sun_path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != getuid()) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct ucred cr;
    socklen_t cl = sizeof cr;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) || cr.uid != getuid()) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- where a source is, now ---- */

static const DisplayOut *display_of(uint32_t conn) {
    const DisplayPlan *p = display_current();
    for (int i = 0; i < p->n; i++) if (p->o[i].conn == conn) return &p->o[i];
    return NULL;
}

/* The window's frame node, if it is on screen: still the node that was picked (its serial: an id another node took
 * after it was destroyed is not it), in the tree, shown, of a size worth a stream. */
static Node *window_of(int id, uint64_t serial) {
    Node *n = node_ref(id, serial);
    if (!n || n->w < 1 || n->h < 1) return NULL;
    int depth = 0;
    for (const Node *a = n; a; a = a->parent, depth++) {
        if (a->s.display_none || depth > 4096) return NULL;
        if (a == ui.root) return n;
    }
    return NULL;
}

/* Fills the stream's geometry from its source as it is now: 0, -1 gone. */
static int geometry(Stream *s, uint32_t *pw, uint32_t *ph) {
    if (s->src.kind == CAST_SRC_MONITOR) {
        const DisplayOut *d = display_of(s->src.display);
        if (!d && s->src.display == 0 && display_current()->n == 0) {   /* no display (headless): the whole space */
            s->k = 1; s->ox = s->oy = 0; s->dx = s->dy = 0;
            s->lx = s->ly = 0; s->lw = (int)ui.width; s->lh = (int)ui.height;
            *pw = (uint32_t)s->lw & ~1u; *ph = (uint32_t)s->lh & ~1u;
            return 0;
        }
        if (!d) return -1;
        s->k = d->v.k;
        s->ox = (float)d->v.x; s->oy = (float)d->v.y;
        s->dx = d->v.dx; s->dy = d->v.dy;
        s->lx = d->v.x; s->ly = d->v.y; s->lw = d->v.w; s->lh = d->v.h;
        *pw = (uint32_t)d->v.pw & ~1u;
        *ph = (uint32_t)d->v.ph & ~1u;
        return 0;
    }
    Node *n = window_of(s->src.node, s->src.serial);
    if (!n) return -1;
    /* sharp on the display it is mostly on */
    double k = 1;
    const DisplayPlan *p = display_current();
    float cx = n->ax + n->w / 2, cy = n->ay + n->h / 2;
    for (int i = 0; i < p->n; i++) {
        const OutView *v = &p->o[i].v;
        if (cx >= v->x && cy >= v->y && cx < v->x + v->w && cy < v->y + v->h) { k = v->k; break; }
    }
    s->k = k;
    s->ox = n->ax; s->oy = n->ay;
    s->dx = s->dy = 0;
    s->lx = (int)lroundf(n->ax); s->ly = (int)lroundf(n->ay); s->lw = (int)lroundf(n->w); s->lh = (int)lroundf(n->h);
    *pw = (uint32_t)lround(n->w * k) & ~1u;
    *ph = (uint32_t)lround(n->h * k) & ~1u;
    return 0;
}

/* Drops the sheet at index i of a window source: it is never painted again. */
static void also_drop(CastSource *c, int i) {
    for (int j = i + 1; j < c->nalso; j++) { c->also[j - 1] = c->also[j]; c->also_serial[j - 1] = c->also_serial[j]; }
    c->nalso--;
}

/* Paints r of the source into its canvas: 1, 0 the source isn't there now. */
static int paint_source(Stream *s, Rect r) {
    if (rect_empty(r)) return 1;
    if (s->src.kind != CAST_SRC_MONITOR) {
        if (!node_ref(s->src.node, s->src.serial)) return 0;
        for (int i = s->src.nalso; i-- > 0;) if (!node_ref(s->src.also[i], s->src.also_serial[i])) also_drop(&s->src, i);
    }
    PaintView keep = paint_view;
    paint_view = (PaintView){ s->ox, s->oy, (float)s->k, (float)s->dx, (float)s->dy, -1 };
    canvas_set_clip(&s->cv, r);
    canvas_clear(&s->cv, 0x000000);
    int ok = 1;
    if (s->src.kind == CAST_SRC_MONITOR) {
        paint_capture(&s->cv, ui.root);
        paint_capture_mode = 1;
        wl_paint_fullscreen(&s->cv);
        paint_capture_mode = 0;
    } else ok = paint_capture_node(&s->cv, ui.root, s->src.node, s->src.also, s->src.nalso);
    canvas_set_clip(&s->cv, (Rect){ 0, 0, s->cv.w, s->cv.h });
    paint_view = keep;
    return ok;
}

/* ---- the ring ---- */

static void ring_free(Stream *s) {
    if (s->map) munmap(s->map, (size_t)s->lay.total);
    if (s->ring_fd >= 0) close(s->ring_fd);
    free(s->px);
    s->map = NULL;
    s->px = NULL;
    s->ring_fd = -1;
}

/* A ring and a canvas for w × h; *ro: the helper's read-only copy of the ring. 0, or -1 (nothing changed). */
static int ring_new(Stream *s, uint32_t w, uint32_t h, int *ro) {
    CastLayout l;
    if (cast_layout(w, h, &l)) return -1;
    uint32_t *px = malloc((size_t)w * h * 4);
    int fd = memfd_create("lordos-screencast", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    uint8_t *map = MAP_FAILED;
    if (px && fd >= 0 && !ftruncate(fd, (off_t)l.total) && !fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL))
        map = mmap(NULL, (size_t)l.total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    char p[64];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    *ro = map != MAP_FAILED ? open(p, O_RDONLY | O_CLOEXEC) : -1;
    if (*ro < 0) {
        if (map != MAP_FAILED) munmap(map, (size_t)l.total);
        if (fd >= 0) close(fd);
        free(px);
        return -1;
    }
    CastRingHdr hd = { CAST_RING_MAGIC, w, h, l.stride, CAST_SLOTS, 0, l.slot_bytes };
    memcpy(map, &hd, sizeof hd);
    ring_free(s);
    s->ring_fd = fd;
    s->map = map;
    s->lay = l;
    s->px = px;
    s->w = w;
    s->h = h;
    canvas_init(&s->cv, px, (int)w, (int)h, (int)w);
    s->dirty = (Rect){ 0, 0, (int)w, (int)h };
    for (int i = 0; i < CAST_SLOTS; i++) s->stale[i] = s->dirty;
    return 0;
}

static int send_stream(Run *r, int i) {
    Stream *s = &r->st[i];
    int ro = -1;
    uint32_t w, h;
    if (geometry(s, &w, &h) || ring_new(s, w, h, &ro)) return -1;
    CastMsg m = { .type = CM_STREAM, .stream = (uint16_t)i, .a = CAST_MAX_FPS, .b = (uint32_t)s->src.kind, .w = w, .h = h };
    int e = cast_send(r->sock, &m, ro);
    close(ro);
    return e;
}

/* ---- sessions ---- */

static Run *run_of(int id) {
    for (int i = 0; id > 0 && i < MAX_RUNS; i++) if (runs[i].id == id) return &runs[i];
    return NULL;
}

int cast_running(void) { return nrunning; }

static void reap(int wait_ms) {
    for (int i = 0; i < 8; i++) {
        if (zombies[i] <= 0) continue;
        int st;
        if (waitpid(zombies[i], &st, WNOHANG) == zombies[i]) { zombies[i] = 0; continue; }
        if (wait_ms || now_ms() > zombie_at[i]) { kill(zombies[i], SIGKILL); waitpid(zombies[i], &st, 0); zombies[i] = 0; }
    }
}

static void run_free(Run *r) {
    if (r->sock >= 0) {
        CastMsg m = { .type = CM_STOP };
        cast_send(r->sock, &m, -1);
        close(r->sock);
    }
    if (r->pid > 0) {
        int slot = -1;
        for (int i = 0; i < 8 && slot < 0; i++) if (zombies[i] <= 0) slot = i;
        if (slot < 0) { reap(1); slot = 0; }
        zombies[slot] = r->pid;
        zombie_at[slot] = now_ms() + REAP_MS;
    }
    for (int i = 0; i < CAST_MAX_STREAMS; i++) ring_free(&r->st[i]);
    memset(r, 0, sizeof *r);
    r->sock = -1;
    if (nrunning > 0) nrunning--;
    if (!nrunning) wl_content_track(0);         /* nothing shared: nothing tracked */
}

/* node_gone_hook: a node is being destroyed. A shared window's ends its share (at the next cast_tick, out of the JS
 * call that destroyed it); a sheet's is dropped from its window's share. */
static void node_gone(int id, uint64_t serial) {
    if (!nrunning) return;
    for (int i = 0; i < MAX_RUNS; i++) {
        Run *r = &runs[i];
        for (int j = 0; r->id && j < r->n; j++) {
            CastSource *c = &r->st[j].src;
            if (c->kind == CAST_SRC_MONITOR) continue;
            if (c->node == id && c->serial == serial) r->gone = 1;
            for (int k = c->nalso; k-- > 0;) if (c->also[k] == id && c->also_serial[k] == serial) also_drop(c, k);
        }
    }
}

void cast_end(int id) {
    Run *r = run_of(id);
    if (r) run_free(r);
}

/* The helper failed or went: the portal hears it as a failed Start, or as the session's end. */
static void run_lost(Run *r) {
    int id = r->id, told = r->told;
    run_free(r);
    if (told) portal_cast_lost(id);
    else portal_cast_failed(id);
}

int cast_begin(int id, const CastSource *src, int n, int cursor_embedded) {
    if (n < 1 || n > CAST_MAX_STREAMS || run_of(id) || !cast_available()) return -1;
    Run *r = NULL;
    for (int i = 0; i < MAX_RUNS && !r; i++) if (!runs[i].id) r = &runs[i];
    if (!r) return -1;
    node_gone_hook = node_gone;
    memset(r, 0, sizeof *r);
    r->sock = -1;
    for (int i = 0; i < CAST_MAX_STREAMS; i++) r->st[i].ring_fd = -1;
    for (int i = 0; i < n; i++) {
        uint32_t w, h;
        r->st[i].src = src[i];
        if (src[i].nalso < 0 || src[i].nalso > CAST_ALSO_MAX || (src[i].kind != CAST_SRC_MONITOR && !src[i].serial) || geometry(&r->st[i], &w, &h) || cast_layout(w, h, &r->st[i].lay)) return -1;
    }
    int pw = pipewire_connect();
    if (pw < 0) { fprintf(stderr, "screencast: no PipeWire of this user's to share through\n"); return -1; }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { close(pw); return -1; }
    int a = fcntl(sv[1], F_DUPFD_CLOEXEC, 10);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, a, 3);
    posix_spawn_file_actions_addclosefrom_np(&fa, 4);     /* nothing else of lord-server's reaches it */
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    char *envp[] = { "LANG=C.UTF-8", NULL };
    char *argv[] = { "lord-screencast", NULL };
    pid_t pid = -1;
    int e = a >= 0 ? posix_spawn(&pid, helper, &fa, &at, argv, envp) : -1;
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (a >= 0) close(a);
    close(sv[1]);
    if (e) { close(sv[0]); close(pw); return -1; }
    r->id = id;
    r->sock = sv[0];
    r->pid = pid;
    r->n = n;
    r->cursor = cursor_embedded;
    r->since = now_ms();
    r->peer.nstreams = n;
    nrunning++;
    wl_content_track(1);
    CastMsg hello = { .type = CM_HELLO, .a = (uint32_t)n };
    int bad = cast_send(r->sock, &hello, pw);
    close(pw);
    for (int i = 0; i < n && !bad; i++) bad = send_stream(r, i);
    if (bad) { run_free(r); return -1; }
    fprintf(stderr, "screencast: session %d: %d source%s, helper %d\n", id, n, n == 1 ? "" : "s", (int)pid);
    return 0;
}

int cast_remote(int id, uint32_t req) {
    Run *r = run_of(id);
    if (!r || !r->told || cast_remote_add(&r->peer, req)) return -1;
    int fd = pipewire_connect();
    if (fd < 0) { CastMsg x = { .type = CM_REMOTE, .a = req }; cast_check_helper(&r->peer, &x, 0); return -1; }
    CastMsg m = { .type = CM_CLIENT, .a = req };
    int e = cast_send(r->sock, &m, fd);
    close(fd);
    if (e) { run_lost(r); return -1; }
    return 0;
}

void cast_shown(int id) {
    Run *r = run_of(id);
    if (!r || r->shown) return;
    r->shown = 1;
    for (int i = 0; i < r->n; i++) r->st[i].dirty = (Rect){ 0, 0, (int)r->st[i].w, (int)r->st[i].h };
}

void cast_end_all(void) {
    for (int i = 0; i < MAX_RUNS; i++) if (runs[i].id) run_lost(&runs[i]);
}

/* ---- what changed ---- */

static void damage_run(Run *r, float x0, float y0, float x1, float y1) {
    for (int i = 0; i < r->n; i++) {
        Stream *s = &r->st[i];
        if (!s->px) continue;
        Rect d = { (int)floor((x0 - s->ox) * s->k) + s->dx - 1, (int)floor((y0 - s->oy) * s->k) + s->dy - 1,
                   (int)ceil((x1 - s->ox) * s->k) + s->dx + 1, (int)ceil((y1 - s->oy) * s->k) + s->dy + 1 };
        s->dirty = rect_join(s->dirty, rect_in(d, (int)s->w, (int)s->h));
    }
}

void cast_damage(float x0, float y0, float x1, float y1) {
    if (!nrunning || x1 <= x0 || y1 <= y0) return;
    for (int i = 0; i < MAX_RUNS; i++) if (runs[i].id) damage_run(&runs[i], x0, y0, x1, y1);
}

void cast_pointer(int x, int y) {
    if (!nrunning || (x == ptr_x && y == ptr_y)) return;
    for (int i = 0; i < MAX_RUNS; i++) {
        if (!runs[i].id || !runs[i].cursor) continue;
        damage_run(&runs[i], (float)ptr_x, (float)ptr_y, (float)ptr_x + ARROW, (float)ptr_y + ARROW);
        damage_run(&runs[i], (float)x, (float)y, (float)x + ARROW, (float)y + ARROW);
    }
    ptr_x = x;
    ptr_y = y;
}

/* ---- the helper's messages ---- */

static void tell_streams(Run *r) {
    CastStreamInfo info[CAST_MAX_STREAMS];
    for (int i = 0; i < r->n; i++)
        info[i] = (CastStreamInfo){ r->peer.node[i], r->st[i].lx, r->st[i].ly, r->st[i].lw, r->st[i].lh, r->st[i].src.kind };
    r->told = 1;
    portal_cast_streams(r->id, info, r->n);
}

static void read_run(Run *r) {
    for (int k = 0; k < 64; k++) {
        CastMsg m;
        int fd = -1;
        int got = cast_recv(r->sock, &m, &fd);
        if (got == 0) return;
        if (got < 0 || cast_check_helper(&r->peer, &m, fd >= 0)) {
            if (fd >= 0) close(fd);
            if (got > 0) fprintf(stderr, "screencast: session %d: the helper sent what it may not (%u); it ends\n", r->id, m.type);
            run_lost(r);
            return;
        }
        Stream *s = m.stream < CAST_MAX_STREAMS ? &r->st[m.stream] : NULL;
        switch (m.type) {
        case CM_NODE: {
            int all = 1;
            for (int i = 0; i < r->n; i++) all &= r->peer.node[i] != 0;
            if (all && !r->told) tell_streams(r);
            break;
        }
        case CM_WANT:
            if (!s) break;
            if (m.a && !s->want) s->dirty = (Rect){ 0, 0, (int)s->w, (int)s->h };   /* a consumer arrived: a whole frame */
            s->want = (int)m.a;
            s->period = cast_period_ms(m.b, m.c);
            break;
        case CM_REMOTE: {
            int id = r->id;
            portal_cast_remote(id, m.a, fd);        /* the portal keeps fd or closes it */
            if (!run_of(id)) return;
            break;
        }
        case CM_FAIL:
            fprintf(stderr, "screencast: session %d: the helper failed (%u)\n", r->id, m.a);
            run_lost(r);
            return;
        default: break;                             /* CM_DONE: the peer freed the slot */
        }
    }
}

int cast_pollfds(struct pollfd *p, int max) {
    int n = 0;
    for (int i = 0; i < MAX_RUNS && n < max; i++) if (runs[i].id) p[n++] = (struct pollfd){ runs[i].sock, POLLIN, 0 };
    return n;
}

void cast_dispatch(const struct pollfd *p, int n) {
    for (int k = 0; k < n; k++) {
        if (!p[k].revents) continue;
        for (int i = 0; i < MAX_RUNS; i++) {
            if (!runs[i].id || runs[i].sock != p[k].fd) continue;
            if (p[k].revents & POLLIN) read_run(&runs[i]);
            if (runs[i].id && runs[i].sock == p[k].fd && (p[k].revents & (POLLERR | POLLHUP | POLLNVAL))) run_lost(&runs[i]);
            break;
        }
    }
}

/* ---- frames ---- */

/* The arrow at the pointer into slot memory (stride w): the LordOS one, as the recordings draw it. */
static Rect arrow_into(Stream *s, uint32_t *slot) {
    int x = (int)floor((ptr_x - s->ox) * s->k) + s->dx, y = (int)floor((ptr_y - s->oy) * s->k) + s->dy;
    Rect r = rect_in((Rect){ x, y, x + ARROW, y + ARROW }, (int)s->w, (int)s->h);
    if (rect_empty(r)) return r;
    Canvas c;
    canvas_init(&c, slot, (int)s->w, (int)s->h, (int)s->w);
    media_draw_arrow(&c, x, y);
    return r;
}

static void copy_rect(uint32_t *dst, const uint32_t *src, int stride, Rect r) {
    if (rect_empty(r)) return;
    for (int y = r.y0; y < r.y1; y++) memcpy(dst + (size_t)y * stride + r.x0, src + (size_t)y * stride + r.x0, (size_t)(r.x1 - r.x0) * 4);
}

/* A window that moved or changed size, a display that changed mode: 0 go on, 1 wait (a new ring is on its way), -1 the
 * source is gone. */
static int follow(Run *r, int i) {
    Stream *s = &r->st[i];
    float ox = s->ox, oy = s->oy;
    double k = s->k;
    uint32_t w, h;
    if (geometry(s, &w, &h)) return -1;
    if (w == s->w && h == s->h) {
        if (ox != s->ox || oy != s->oy || k != s->k) s->dirty = (Rect){ 0, 0, (int)w, (int)h };
        return 0;
    }
    if (w < CAST_MIN_DIM || h < CAST_MIN_DIM || w > CAST_MAX_DIM || h > CAST_MAX_DIM) return 1;
    for (int j = 0; j < CAST_SLOTS; j++) if (r->peer.busy[i][j]) return 1;     /* its frames on the old ring first */
    return send_stream(r, i) ? -1 : 1;
}

static void frame_of(Run *r, int i, double t) {
    Stream *s = &r->st[i];
    int f = follow(r, i);
    if (f < 0) { int id = r->id; run_free(r); portal_cast_lost(id); return; }
    if (f || !s->want || t - s->last < s->period - 1) return;
    /* a fullscreen app on this display is painted outside the tree: its frames can't be told apart, so it is whole */
    if (s->src.kind == CAST_SRC_MONITOR && wl_fullscreen_active()) s->dirty = (Rect){ 0, 0, (int)s->w, (int)s->h };
    if (rect_empty(s->dirty)) return;
    int slot = cast_slot_take(&r->peer, i);
    if (slot < 0) return;                           /* the helper is behind: this frame waits */
    if (!paint_source(s, s->dirty)) { r->peer.busy[i][slot] = 0; int id = r->id; run_free(r); portal_cast_lost(id); return; }
    for (int j = 0; j < CAST_SLOTS; j++) s->stale[j] = rect_join(s->stale[j], s->dirty);
    s->dirty = (Rect){ 0, 0, 0, 0 };
    uint32_t *dst = (uint32_t *)(s->map + cast_slot_off(&s->lay, (uint32_t)slot));
    copy_rect(dst, s->px, (int)s->w, s->stale[slot]);
    s->stale[slot] = (Rect){ 0, 0, 0, 0 };
    if (r->cursor) s->stale[slot] = arrow_into(s, dst);     /* drawn over: that place is stale next time */
    CastMsg m = { .type = CM_FRAME, .stream = (uint16_t)i, .a = (uint32_t)slot, .w = s->w, .h = s->h, .seq = ++s->seq };
    if (cast_send(r->sock, &m, -1)) {               /* the slot is up to date but didn't go: all of it next time */
        r->peer.busy[i][slot] = 0;
        s->dirty = (Rect){ 0, 0, (int)s->w, (int)s->h };
        if (errno != EAGAIN) run_lost(r);
        return;
    }
    s->last = t;
}

void cast_tick(void) {
    if (zombies[0] || zombies[1] || zombies[2] || zombies[3] || zombies[4] || zombies[5] || zombies[6] || zombies[7]) reap(0);
    if (!nrunning) return;
    float x0, y0, x1, y1;
    if (wl_content_take(&x0, &y0, &x1, &y1)) cast_damage(x0, y0, x1, y1);
    double t = now_ms();
    for (int i = 0; i < MAX_RUNS; i++) {
        Run *r = &runs[i];
        if (!r->id) continue;
        if (r->gone) { fprintf(stderr, "screencast: session %d: the shared window closed; it ends\n", r->id); run_lost(r); continue; }
        if (!r->told) {
            if (t - r->since > START_MS) { fprintf(stderr, "screencast: session %d: no streams from the helper\n", r->id); run_lost(r); }
            continue;
        }
        if (!r->shown) continue;
        for (int j = 0; j < r->n && r->id; j++) frame_of(r, j, t);
    }
}

double cast_timeout(void) {
    double best = -1, t = now_ms();
    for (int i = 0; i < 8; i++) if (zombies[i] > 0) { double d = zombie_at[i] - t; best = d < 0 ? 0 : d; break; }
    for (int i = 0; i < MAX_RUNS; i++) {
        const Run *r = &runs[i];
        if (!r->id) continue;
        if (r->gone) return 0;
        if (!r->told) { double d = r->since + START_MS - t; if (d < 0) d = 0; if (best < 0 || d < best) best = d; continue; }
        for (int j = 0; r->shown && j < r->n; j++) {
            const Stream *s = &r->st[j];
            if (!s->want || (rect_empty(s->dirty) && !(s->src.kind == CAST_SRC_MONITOR && wl_fullscreen_active()))) continue;
            double d = s->last + s->period - t;
            if (d < 0) d = 0;
            if (best < 0 || d < best) best = d;
        }
    }
    return best;
}

/* ---- the chooser's pictures ---- */

int cast_thumb(const CastSource *src, int max) {
    if (!src || max < 16 || max > 1024 || src->nalso < 0 || src->nalso > CAST_ALSO_MAX) return 0;
    Stream s;
    memset(&s, 0, sizeof s);
    s.src = *src;
    uint32_t w, h;
    if (geometry(&s, &w, &h) || w < 2 || h < 2 || w > CAST_MAX_DIM || h > CAST_MAX_DIM) return 0;
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) return 0;
    canvas_init(&s.cv, px, (int)w, (int)h, (int)w);
    s.px = px;
    s.w = w;
    s.h = h;
    int ok = paint_source(&s, (Rect){ 0, 0, (int)w, (int)h });
    int tw = (int)w, th = (int)h;
    if (tw > max) { th = (int)((double)th * max / tw); tw = max; }
    if (th < 1) th = 1;
    MediaSrc *m = ok ? media_src_new(SK_CAPTURE, "system") : NULL;
    if (m && (m->px = malloc((size_t)tw * th * 4)) && !img_resize(px, (int)w, (int)h, m->px, tw, th)) {
        m->w = tw;
        m->h = th;
        m->opaque = 1;
        for (size_t i = 0; i < (size_t)tw * th; i++) m->px[i] |= 0xff000000u;
        m->frame_seq = 1;
        free(px);
        return m->id;
    }
    if (m) { free(m->px); m->px = NULL; m->used = 0; }
    free(px);
    return 0;
}
