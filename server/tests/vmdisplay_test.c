/* vmdisplay_test.c — lord-vmdisplay against a mock QEMU and a mock lord-server (docs/VIRTUALIZATION.md 3.2).
 *
 *   build/vmdisplay-test build/lord-vmdisplay
 *
 * The test plays QEMU's side of the peer-to-peer D-Bus display (fd 4: org.qemu.Display1.Console, Mouse, Keyboard;
 * the listener connection lord-vmdisplay hands over with RegisterListener) and lord-server's side of fd 3 (vmproto.h),
 * then checks:
 *   - QEMU's listener calls (ScanoutDMABUF, UpdateDMABUF) are always answered well inside QEMU's 1 s timeout
 *     (DBUS_DEFAULT_TIMEOUT in QEMU's include/qemu/dbus.h), also while lord-server never reads a frame, while it is
 *     slow to send a new picture after a resize, and while input keeps arriving;
 *   - buttons, in both mouse modes: the right button goes out as Mouse.Press/Release 2 like the left as 0, and only
 *     after the pointer's position that came with it (free: SetAbsPosition; captured: RelMotion), so the guest gets
 *     the click where the pointer is.
 * No QEMU, no KVM, no GPU: a memfd stands in for the dmabuf. Exits 1 on the first failure. */
#include "../src/vm/vmproto.h"
#include "../src/vm/vmpace.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#define CONSOLE "/org/qemu/Display1/Console_0"
#define LISTENER "/org/qemu/Display1/Listener"
#define QEMU_TIMEOUT_MS 1000            /* what QEMU waits for each listener answer */

static sd_bus *q, *L;                   /* QEMU's side: the console connection, the listener connection */
static int srv = -1;                    /* lord-server's side of fd 3 */
static int fails, closing;              /* closing: the helper was told to go; its connections end */

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void check(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  %s ", ok ? "ok    " : "FAILED");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok) fails++;
}

/* ---- what "QEMU" saw ---- */

typedef struct { char what[16]; int a, b; } Ev;
static Ev ev[8192];
static int nev;
static int is_abs = 1;                  /* Mouse.IsAbsolute: the tablet (1) or the relative mouse (0) is QEMU's active one */
static void note(const char *what, int a, int b) {
    if (nev >= (int)(sizeof ev / sizeof *ev)) return;
    snprintf(ev[nev].what, sizeof ev[nev].what, "%s", what);
    ev[nev].a = a; ev[nev].b = b;
    nev++;
}

static int p_label(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e) {
    (void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
    return sd_bus_message_append(r, "s", "test");
}
static int p_w(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e) {
    (void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
    return sd_bus_message_append(r, "u", 640u);
}
static int p_h(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e) {
    (void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
    return sd_bus_message_append(r, "u", 480u);
}
static int p_abs(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e) {
    (void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
    return sd_bus_message_append(r, "b", is_abs);
}

static sd_bus *server_bus(int fd) {
    sd_bus *b = NULL;
    sd_id128_t id;
    sd_id128_randomize(&id);
    int r = sd_bus_new(&b);
    if (r >= 0) r = sd_bus_set_fd(b, fd, fd);
    if (r >= 0) r = sd_bus_set_server(b, 1, id);
    if (r >= 0) r = sd_bus_negotiate_fds(b, 1);
    if (r >= 0) r = sd_bus_start(b);
    if (r < 0) { fprintf(stderr, "mock QEMU: D-Bus server: %s\n", strerror(-r)); exit(1); }
    sd_bus_set_method_call_timeout(b, 5ull * 1000 * 1000);
    return b;
}

static int m_register(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int fd;
    if (sd_bus_message_read(m, "h", &fd) < 0) return -EINVAL;
    L = server_bus(fcntl(fd, F_DUPFD_CLOEXEC, 3));
    return sd_bus_reply_method_return(m, NULL);
}
static int m_uiinfo(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint16_t wmm, hmm; int32_t x, y; uint32_t w, h;
    if (sd_bus_message_read(m, "qqiiuu", &wmm, &hmm, &x, &y, &w, &h) >= 0) note("SetUIInfo", (int)w, (int)h);
    return sd_bus_reply_method_return(m, NULL);
}
static int m_press(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)e;
    uint32_t b;
    if (sd_bus_message_read(m, "u", &b) >= 0) note(u ? "Release" : "Press", (int)b, 0);
    return sd_bus_reply_method_return(m, NULL);
}
/* QEMU's own checks (ui/dbus-console.c): an absolute position only to an absolute mouse, motion only to a relative one */
static int m_abs(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    uint32_t x, y;
    if (sd_bus_message_read(m, "uu", &x, &y) < 0) return -EINVAL;
    if (!is_abs) return sd_bus_error_set(e, SD_BUS_ERROR_INVALID_ARGS, "Mouse is not absolute");
    if (x >= 640 || y >= 480) return sd_bus_error_set(e, SD_BUS_ERROR_INVALID_ARGS, "Invalid mouse position");
    note("SetAbs", (int)x, (int)y);
    return sd_bus_reply_method_return(m, NULL);
}
static int m_rel(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    int32_t dx, dy;
    if (sd_bus_message_read(m, "ii", &dx, &dy) < 0) return -EINVAL;
    if (is_abs) return sd_bus_error_set(e, SD_BUS_ERROR_INVALID_ARGS, "Mouse is not relative");
    note("RelMotion", dx, dy);
    return sd_bus_reply_method_return(m, NULL);
}
static int m_key(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)e;
    uint32_t k;
    if (sd_bus_message_read(m, "u", &k) >= 0) note(u ? "KeyRelease" : "KeyPress", (int)k, 0);
    return sd_bus_reply_method_return(m, NULL);
}
static int m_noaudio(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_NOT_SUPPORTED, "no audio backend on this display");
}

static const sd_bus_vtable console_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Label", "s", p_label, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Width", "u", p_w, 0, 0),
    SD_BUS_PROPERTY("Height", "u", p_h, 0, 0),
    SD_BUS_METHOD("RegisterListener", "h", "", m_register, 0),
    SD_BUS_METHOD("SetUIInfo", "qqiiuu", "", m_uiinfo, 0),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable mouse_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("IsAbsolute", "b", p_abs, 0, 0),
    SD_BUS_METHOD_WITH_OFFSET("Press", "u", "", m_press, 0, 0),
    SD_BUS_METHOD_WITH_OFFSET("Release", "u", "", m_press, 1, 0),
    SD_BUS_METHOD("SetAbsPosition", "uu", "", m_abs, 0),
    SD_BUS_METHOD("RelMotion", "ii", "", m_rel, 0),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable kbd_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD_WITH_OFFSET("Press", "u", "", m_key, 0, 0),
    SD_BUS_METHOD_WITH_OFFSET("Release", "u", "", m_key, 1, 0),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable audio_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("RegisterOutListener", "h", "", m_noaudio, 0),
    SD_BUS_METHOD("RegisterInListener", "h", "", m_noaudio, 0),
    SD_BUS_VTABLE_END
};

/* ---- "lord-server" ---- */

static int ready, ack_frames = 1, frames_seen, dmabufs_seen;
/* lord-server's picture as the helper writes it (the last VD_SHM), and the VD_FRAME rectangles seen */
static uint8_t *shm_map;
static size_t shm_len;
static int shm_w;
static struct { int x, y, w, h, e, f; } rects[256];
static int nrects;
static long long shm_hold_until;        /* a slow lord-server: the picture for a new size comes only after this */
static int shm_want_w, shm_want_h;

static void send_shm(int w, int h) {
    int fd = memfd_create("vmdisplay-test-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, (off_t)w * h * 4) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW)) { perror("memfd"); exit(1); }
    if (shm_map) munmap(shm_map, shm_len);
    shm_len = (size_t)w * h * 4;
    shm_map = mmap(NULL, shm_len, PROT_READ, MAP_SHARED, fd, 0);
    if (shm_map == MAP_FAILED) { perror("mmap"); exit(1); }
    shm_w = w;
    VdMsg m = vd_msg(VD_SHM, 0, 0, 0, 0, 0, 0);
    vd_send(srv, &m, fd);
    close(fd);
}
static void tell(int type, int a, int b) { VdMsg m = vd_msg((uint16_t)type, a, b, 0, 0, 0, 0); vd_send(srv, &m, -1); }

static void from_helper(void) {
    for (;;) {
        VdMsg m;
        int fd = -1;
        int r = vd_recv(srv, &m, &fd);
        if (r <= 0) return;
        if (fd >= 0) close(fd);
        switch (m.type) {
        case VD_READY: ready = 1; break;
        case VD_DMABUF: if (m.a) dmabufs_seen++; break;
        case VD_SIZE:
            shm_want_w = m.a; shm_want_h = m.b;
            if (now_ms() >= shm_hold_until) { send_shm(m.a, m.b); shm_want_w = shm_want_h = 0; }
            break;
        case VD_FRAME:
            if (!m.f) frames_seen++;
            if (nrects < 256) { rects[nrects].x = m.a; rects[nrects].y = m.b; rects[nrects].w = m.c; rects[nrects].h = m.d; rects[nrects].e = m.e; rects[nrects].f = m.f; nrects++; }
            if (ack_frames) { VdMsg a = vd_msg(VD_FRAME_DONE, 0, 0, 0, 0, m.e, 0); vd_send(srv, &a, -1); }
            break;
        }
    }
}

/* ---- the loop: QEMU's two connections and lord-server's channel, for `ms` ---- */

static void pump(int ms) {
    long long until = now_ms() + ms;
    for (;;) {
        int r;
        while ((r = sd_bus_process(q, NULL)) > 0) ;
        if (r < 0) { if (closing) return; fprintf(stderr, "mock QEMU: console connection: %s\n", strerror(-r)); exit(1); }
        if (L) {
            while ((r = sd_bus_process(L, NULL)) > 0) ;
            if (r < 0) { if (closing) return; fprintf(stderr, "mock QEMU: listener connection: %s\n", strerror(-r)); exit(1); }
        }
        from_helper();
        if (shm_want_w && now_ms() >= shm_hold_until) { send_shm(shm_want_w, shm_want_h); shm_want_w = shm_want_h = 0; }
        long long left = until - now_ms();
        if (left <= 0) return;
        struct pollfd p[3] = {
            { sd_bus_get_fd(q), (short)sd_bus_get_events(q), 0 },
            { L ? sd_bus_get_fd(L) : -1, L ? (short)sd_bus_get_events(L) : 0, 0 },
            { srv, POLLIN, 0 },
        };
        int to = left > 5 ? 5 : (int)left;
        poll(p, 3, to);
    }
}

/* ---- QEMU's listener calls, each with its answer's latency ---- */

typedef struct { long long sent, answered; int error; } Call;
static Call calls[4096];
static int ncalls;
static int on_answer(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)e;
    Call *c = u;
    c->answered = now_ms();
    c->error = sd_bus_message_is_method_error(m, NULL);
    return 0;
}
static Call *listener_call(const char *method, const char *sig, ...) {
    if (ncalls >= (int)(sizeof calls / sizeof *calls)) return NULL;
    Call *c = &calls[ncalls++];
    memset(c, 0, sizeof *c);
    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_method_call(L, &m, NULL, LISTENER, "org.qemu.Display1.Listener", method);
    va_list ap;
    va_start(ap, sig);
    if (r >= 0) r = sd_bus_message_appendv(m, sig, ap);
    va_end(ap);
    c->sent = now_ms();
    if (r >= 0) r = sd_bus_call_async(L, NULL, m, on_answer, c, (uint64_t)QEMU_TIMEOUT_MS * 1000);
    sd_bus_message_unref(m);
    if (r < 0) { fprintf(stderr, "mock QEMU: %s: %s\n", method, strerror(-r)); exit(1); }
    return c;
}
static Call *map_call(const char *method, const char *sig, ...) {
    if (ncalls >= (int)(sizeof calls / sizeof *calls)) return NULL;
    Call *c = &calls[ncalls++];
    memset(c, 0, sizeof *c);
    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_method_call(L, &m, NULL, LISTENER, "org.qemu.Display1.Listener.Unix.Map", method);
    va_list ap;
    va_start(ap, sig);
    if (r >= 0) r = sd_bus_message_appendv(m, sig, ap);
    va_end(ap);
    c->sent = now_ms();
    if (r >= 0) r = sd_bus_call_async(L, NULL, m, on_answer, c, (uint64_t)QEMU_TIMEOUT_MS * 1000);
    sd_bus_message_unref(m);
    if (r < 0) { fprintf(stderr, "mock QEMU: %s: %s\n", method, strerror(-r)); exit(1); }
    return c;
}
static void scanout(uint32_t w, uint32_t h) {
    int fd = memfd_create("vmdisplay-test-dmabuf", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)w * h * 4)) { perror("memfd"); exit(1); }
    listener_call("ScanoutDMABUF", "huuuutb", fd, w, h, w * 4, VD_FOURCC_XRGB8888, (uint64_t)0, 0);
    close(fd);
}
/* the worst latency among calls [from, ncalls); -1 when one went unanswered or failed */
static long long worst(int from, int *unanswered) {
    long long w = 0;
    *unanswered = 0;
    for (int i = from; i < ncalls; i++) {
        if (!calls[i].answered || calls[i].error) { (*unanswered)++; continue; }
        if (calls[i].answered - calls[i].sent > w) w = calls[i].answered - calls[i].sent;
    }
    return w;
}

/* a process's CPU time so far, in ms (utime + stime) */
static long long cpu_of(pid_t pid) {
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strrchr(buf, ')');
    unsigned long ut = 0, st = 0;
    if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) return -1;
    return (long long)(ut + st) * 1000 / sysconf(_SC_CLK_TCK);
}

static int find(const char *what, int a, int from) {
    for (int i = from; i < nev; i++) if (!strcmp(ev[i].what, what) && ev[i].a == a) return i;
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: vmdisplay-test build/lord-vmdisplay\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    int sv[2], dv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, dv)) { perror("socketpair"); return 1; }
    pid_t pid = fork();
    if (pid == 0) {
        int a = fcntl(sv[1], F_DUPFD, 20), b = fcntl(dv[1], F_DUPFD, 21);
        dup2(a, 3);
        dup2(b, 4);
        char *args[] = { "lord-vmdisplay", "0b2b4eb6-77dd-42a8-933c-3cf7eac71aa2", NULL };
        execv(argv[1], args);
        _exit(127);
    }
    close(sv[1]);
    close(dv[1]);
    srv = sv[0];
    fcntl(srv, F_SETFL, O_NONBLOCK);
    q = server_bus(dv[0]);
    if (sd_bus_add_object_vtable(q, NULL, CONSOLE, "org.qemu.Display1.Console", console_vt, NULL) < 0 ||
        sd_bus_add_object_vtable(q, NULL, CONSOLE, "org.qemu.Display1.Mouse", mouse_vt, NULL) < 0 ||
        sd_bus_add_object_vtable(q, NULL, CONSOLE, "org.qemu.Display1.Keyboard", kbd_vt, NULL) < 0 ||
        sd_bus_add_object_vtable(q, NULL, "/org/qemu/Display1/Audio", "org.qemu.Display1.Audio", audio_vt, NULL) < 0) {
        fprintf(stderr, "mock QEMU: vtables\n");
        return 1;
    }

    printf("lord-vmdisplay against a mock QEMU:\n");
    for (long long t = now_ms() + 5000; !ready && now_ms() < t;) pump(10);
    check(ready && L, "it registered its listener and said it's ready");
    if (!ready || !L) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); return 1; }

    /* the 3D picture: a dmabuf, then lord-server's picture for its size */
    int c0 = ncalls, un;
    scanout(640, 480);
    pump(200);
    check(dmabufs_seen == 1 && worst(c0, &un) < 100 && !un, "ScanoutDMABUF answered at once and handed on (%d dmabuf)", dmabufs_seen);

    /* QEMU's vfio display (a GVT-g vGPU) sends an UpdateDMABUF every refresh (30 ms), whatever came back */
    c0 = ncalls;
    for (int i = 0; i < 30; i++) { listener_call("UpdateDMABUF", "iiii", 0, 0, 640, 480); pump(30); }
    pump(400);
    long long w = worst(c0, &un);
    check(!un && w < 250, "UpdateDMABUF every 30 ms, lord-server reading each frame: worst answer %lld ms", w);

    /* lord-server reads no frame (a hidden window, a stalled compositor): an answer waits for it 250 ms at most (the
     * pacing virgl needs, vmdisplay.c's top), far inside QEMU's 1 s */
    ack_frames = 0;
    c0 = ncalls;
    for (int i = 0; i < 10; i++) { listener_call("UpdateDMABUF", "iiii", 0, 0, 640, 480); pump(100); }
    pump(600);
    w = worst(c0, &un);
    check(!un && w <= 300, "lord-server reads no frame: worst answer %lld ms (QEMU waits %d ms at most)", w, QEMU_TIMEOUT_MS);
    ack_frames = 1;

    /* a resize (apps mode asks the guest for 1920x1080): lord-server is slow with the new picture */
    c0 = ncalls;
    shm_hold_until = now_ms() + 1500;
    scanout(1920, 1080);
    for (int i = 0; i < 50; i++) { listener_call("UpdateDMABUF", "iiii", 0, 0, 1920, 1080); pump(30); }
    pump(400);
    w = worst(c0, &un);
    check(!un && w < 100, "a resize while lord-server is slow with the new picture: worst answer %lld ms", w);
    check(dmabufs_seen == 2, "the new dmabuf went on to lord-server");

    /* updates in a burst, with input arriving all the time */
    c0 = ncalls;
    for (int i = 0; i < 200; i++) {
        listener_call("UpdateDMABUF", "iiii", 0, 0, 1920, 1080);
        tell(VD_MOTION, i % 600, i % 400);
        if (i % 20 == 0) pump(1);
    }
    pump(800);
    w = worst(c0, &un);
    check(!un && w < 250, "200 updates at once with input arriving: worst answer %lld ms", w);
    pump(200);

    /* ---- pacing (VD_PACE, vmpace.h): QEMU as its vfio display does it, the console blocked until each answer, then the
     * next update at its next 30 ms refresh. Frames handed to lord-server in 2 s, unpaced, idle and hidden; the
     * answers always inside QEMU's 1 s; input ends idle at once ---- */
    {
        int rates[3];
        long long cpu[3];
        const int kinds[3][2] = { { 0, VDP_NONE }, { VMPACE_IDLE_MS, VDP_IDLE }, { VMPACE_HIDDEN_MS, VDP_HIDDEN } };
        for (int k = 0; k < 3; k++) {
            tell(VD_PACE, kinds[k][0], kinds[k][1]);
            /* hidden, lord-server doesn't paint, so it never acknowledges a 2D frame: the helper goes on after 250 ms */
            ack_frames = kinds[k][1] != VDP_HIDDEN;
            pump(600);                           /* what the last pace held goes out first */
            int f0 = frames_seen;
            long long u0 = cpu_of(pid);
            c0 = ncalls;
            Call *open = NULL;
            long long end = now_ms() + 2000, next = 0;
            while (now_ms() < end) {
                /* hidden: damage keeps coming while a frame waits (as a 2D picture's UpdateMap does) */
                if ((!open || open->answered || kinds[k][1] == VDP_HIDDEN) && now_ms() >= next) { open = listener_call("UpdateDMABUF", "iiii", 0, 0, 1920, 1080); next = now_ms() + 30; }
                pump(5);
            }
            pump(1000);
            rates[k] = frames_seen - f0;
            cpu[k] = cpu_of(pid) - u0;
            ack_frames = 1;
            w = worst(c0, &un);
            check(!un && w < QEMU_TIMEOUT_MS - 50, "paced %d ms: worst answer %lld ms", kinds[k][0], w);
        }
        check(rates[0] >= 50 && rates[1] <= 22 && rates[1] >= 12 && rates[2] <= 5 && rates[2] >= 2,
              "frames in 2 s: %d unpaced, %d idle (%d ms), %d hidden (%d ms)", rates[0], rates[1], VMPACE_IDLE_MS, rates[2], VMPACE_HIDDEN_MS);
        /* paced longer than the 250 ms it waits for an answer, it sleeps until the interval ends (it once spun there) */
        check(cpu[1] < 200 && cpu[2] < 200, "the helper's CPU in 3 s: %lld ms unpaced, %lld ms idle, %lld ms hidden", cpu[0], cpu[1], cpu[2]);
        /* idle: a key ends it in the helper, without lord-server: the held update goes out at once */
        tell(VD_PACE, VMPACE_IDLE_MS, VDP_IDLE);
        pump(300);
        Call *c = listener_call("UpdateDMABUF", "iiii", 0, 0, 1920, 1080);     /* goes out: the interval starts */
        while (!c->answered) pump(2);
        c = listener_call("UpdateDMABUF", "iiii", 0, 0, 1920, 1080);           /* held for the interval */
        pump(20);
        check(!c->answered, "idle: the next update is held for the interval");
        long long t0 = now_ms();
        tell(VD_KEY, 30, 1);
        tell(VD_KEY, 30, 0);
        while (!c->answered && now_ms() - t0 < 1000) pump(2);
        check(c->answered && c->answered - t0 >= 0 && c->answered - t0 < 40, "idle: input brings the next picture at once (%lld ms)", c->answered ? c->answered - t0 : -1);
        tell(VD_PACE, 0, VDP_NONE);
        pump(200);
    }

    /* ---- a 2D picture (ScanoutMap, UpdateMap): a clock in one corner and a square in the other go to lord-server as
     * two rectangles of one frame, each copied, nothing between them (MACHINES-GPU.md Part 1 step 4) ---- */
    {
        const int W2 = 800, H2 = 600;
        int qfd = memfd_create("vmdisplay-test-map", MFD_CLOEXEC);
        if (qfd < 0 || ftruncate(qfd, (off_t)W2 * H2 * 4)) { perror("memfd"); return 1; }
        uint32_t *q32 = mmap(NULL, (size_t)W2 * H2 * 4, PROT_READ | PROT_WRITE, MAP_SHARED, qfd, 0);
        if (q32 == MAP_FAILED) { perror("mmap"); return 1; }
        for (int i = 0; i < W2 * H2; i++) q32[i] = 0x00101010u;
        map_call("ScanoutMap", "huuuuu", qfd, 0u, (uint32_t)W2, (uint32_t)H2, (uint32_t)W2 * 4, 0x20020888u);
        close(qfd);
        for (long long t = now_ms() + 2000; (!shm_map || shm_w != W2) && now_ms() < t;) pump(10);
        pump(400);                              /* the first, whole frame */
        /* lord-server is still reading the frame before (no answer): the changes that come meanwhile go out together */
        ack_frames = 0;
        map_call("UpdateMap", "iiii", 0, 0, 1, 1);
        for (long long t = now_ms() + 500; !rects[nrects ? nrects - 1 : 0].w && now_ms() < t;) pump(5);
        pump(20);
        int r0 = nrects;
        for (int y = 560; y < 580; y++) for (int x = 700; x < 760; x++) q32[y * W2 + x] = 0x00ff0000u;     /* the clock */
        for (int y = 100; y < 164; y++) for (int x = 50; x < 114; x++) q32[y * W2 + x] = 0x0000ff00u;      /* the square */
        map_call("UpdateMap", "iiii", 700, 560, 60, 20);
        map_call("UpdateMap", "iiii", 50, 100, 64, 64);
        for (long long t = now_ms() + 1000; nrects - r0 < 2 && now_ms() < t;) pump(5);
        pump(100);
        ack_frames = 1;
        int n = nrects - r0, same = n == 2 && rects[r0].e == rects[r0 + 1].e, fs = n == 2 && rects[r0].f == 1 && rects[r0 + 1].f == 0;
        long area = 0;
        for (int i = r0; i < nrects; i++) area += (long)rects[i].w * rects[i].h;
        check(n == 2 && same && fs, "two far changes: %d rectangles in one frame (f counts down to 0)", n);
        check(area == 60 * 20 + 64 * 64, "only their pixels: %ld, not the box over both (%d)", area, (760 - 50) * (580 - 100));
        const uint32_t *s32 = (const uint32_t *)shm_map;
        check(shm_map && s32[570 * W2 + 730] == 0x00ff0000u && s32[130 * W2 + 80] == 0x0000ff00u && s32[300 * W2 + 400] == 0x00101010u,
              "both copied into lord-server's picture");
        /* nine far changes: never more than VD_FRAME_RECTS rectangles, and every change still there */
        ack_frames = 0;
        map_call("UpdateMap", "iiii", 0, 0, 1, 1);
        pump(300);
        r0 = nrects;
        for (int k = 0; k < 9; k++) {
            int x = 10 + k * 85, y = 10 + (k % 3) * 200;
            for (int yy = y; yy < y + 8; yy++) for (int xx = x; xx < x + 8; xx++) q32[yy * W2 + xx] = 0x000000ffu;
            map_call("UpdateMap", "iiii", x, y, 8, 8);
        }
        for (long long t = now_ms() + 1000; (nrects == r0 || rects[nrects - 1].f) && now_ms() < t;) pump(5);
        pump(100);
        ack_frames = 1;
        int ok9 = 1;
        for (int k = 0; k < 9; k++) ok9 &= s32[(14 + (k % 3) * 200) * W2 + 14 + k * 85] == 0x000000ffu;
        check(nrects - r0 >= 1 && nrects - r0 <= VD_FRAME_RECTS && ok9, "nine changes: %d rectangles (at most %d), all nine copied", nrects - r0, VD_FRAME_RECTS);
        munmap(q32, (size_t)W2 * H2 * 4);
    }

    /* ---- buttons, the mouse free: the tablet, absolute ---- */
    is_abs = 1;
    int e0 = nev;
    /* as the machine's window sends a click: the press's position, then the button (vmwindow.jsx down) */
    tell(VD_MOTION, 101, 51);
    tell(VD_BUTTON, VDB_RIGHT, 1);
    pump(50);
    tell(VD_BUTTON, VDB_RIGHT, 0);
    pump(100);
    int pos = -1, pr = find("Press", VDB_RIGHT, e0), rl = find("Release", VDB_RIGHT, e0);
    for (int i = e0; i < nev; i++) if (!strcmp(ev[i].what, "SetAbs") && ev[i].a == 101 && ev[i].b == 51) { pos = i; break; }
    check(pr >= 0 && rl > pr, "free: a right click goes out as Press 2, Release 2");
    check(pos >= 0 && pos < pr, "free: the pointer is at the click's position before the right button goes down (position #%d, press #%d)", pos, pr);
    e0 = nev;
    tell(VD_MOTION, 300, 200);
    tell(VD_BUTTON, VDB_LEFT, 1);
    tell(VD_BUTTON, VDB_LEFT, 0);
    pump(150);
    pos = -1; pr = find("Press", VDB_LEFT, e0); rl = find("Release", VDB_LEFT, e0);
    for (int i = e0; i < nev; i++) if (!strcmp(ev[i].what, "SetAbs") && ev[i].a == 300 && ev[i].b == 200) { pos = i; break; }
    check(pr >= 0 && rl > pr && pos >= 0 && pos < pr, "free: a left click, position first, then Press 0, Release 0");
    e0 = nev;
    tell(VD_MOTION, 400, 300);
    tell(VD_WHEEL, 1, 0);
    pump(150);
    pos = -1; pr = find("Press", 3, e0);
    for (int i = e0; i < nev; i++) if (!strcmp(ev[i].what, "SetAbs") && ev[i].a == 400 && ev[i].b == 300) { pos = i; break; }
    check(pr >= 0 && pos >= 0 && pos < pr, "free: a wheel step goes where the pointer is (wheel-up as button 3)");

    /* ---- captured: the relative mouse ---- */
    tell(VD_CAPTURE, 1, 0);
    is_abs = 0;
    pump(400);                          /* IsAbsolute is asked again every 100 ms after a switch */
    e0 = nev;
    tell(VD_REL, 5, 3);
    tell(VD_BUTTON, VDB_RIGHT, 1);
    pump(50);
    tell(VD_BUTTON, VDB_RIGHT, 0);
    pump(100);
    int mv = find("RelMotion", 5, e0);
    pr = find("Press", VDB_RIGHT, e0); rl = find("Release", VDB_RIGHT, e0);
    check(pr >= 0 && rl > pr, "captured: a right click goes out as Press 2, Release 2");
    check(mv >= 0 && mv < pr, "captured: the motion that came before the press goes out before it");
    tell(VD_CAPTURE, 0, 0);
    is_abs = 1;
    pump(400);

    /* release-all lets go of a button held across the switch */
    e0 = nev;
    tell(VD_BUTTON, VDB_RIGHT, 1);
    tell(VD_RELEASE_ALL, 0, 0);
    pump(100);
    check(find("Press", VDB_RIGHT, e0) >= 0 && find("Release", VDB_RIGHT, e0) > find("Press", VDB_RIGHT, e0), "release-all lets go of the right button");

    /* the end: VD_CLOSE, and nothing left waiting in QEMU */
    tell(VD_CLOSE, 0, 0);
    closing = 1;
    pump(300);
    int st = 0;
    for (long long t = now_ms() + 2000; now_ms() < t;) { if (waitpid(pid, &st, WNOHANG) == pid) { pid = 0; break; } pump(10); }
    check(!pid && WIFEXITED(st) && WEXITSTATUS(st) == 0, "VD_CLOSE: it exits");
    if (pid) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
    w = worst(0, &un);
    check(!un, "every one of QEMU's %d listener calls was answered (worst %lld ms)", ncalls, w);

    printf("%s\n", fails ? "vmdisplay-test: FAILED" : "vmdisplay-test: all passed");
    return fails ? 1 : 0;
}
