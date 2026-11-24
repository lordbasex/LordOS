/* vmdisplay_fuzz.c — lord-vmdisplay's D-Bus side against a hostile QEMU (docs/VIRTUALIZATION.md 3.2, 8).
 *
 *   build/vmdisplay-fuzz build/lord-vmdisplay-asan-test [seed] [calls per run]
 *
 * The guest controls what QEMU's display sends (its picture's size, stride and format, its cursor, its sound streams),
 * and a QEMU taken over by the guest controls every byte of it. This test plays such a QEMU: the console (with hostile
 * properties in some runs), the listener connection, the Unix.Map interface and both audio listeners, and sends
 * thousands of seeded random calls, each argument drawn from the edges (0, -1, INT_MIN, INT_MAX, the limits ± 1, sizes
 * that disagree with the data that comes with them, descriptors that are pipes, too small, unsealed or shrunk after
 * the map), plus calls with the wrong signature and unknown methods. It plays lord-server's side too (pictures for
 * VD_SIZE, frame answers, the sound rings, input), so the copies and the sound paths run.
 *
 * The helper is built with ASan and UBSan (any report aborts it). Checks, per run: it never dies, every call is
 * answered (an error for a bad signature, else a return; a held UpdateDMABUF within its hold), within QEMU's 1 s, it
 * exits 0 at VD_CLOSE, and its log has no sanitizer report. Deterministic for a seed; exits 1 on any failure. */
#include "../src/vm/vmproto.h"
#include "../src/vm/vmpace.h"
#include "../src/media/aring.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
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
#define AOUT "/org/qemu/Display1/AudioOutListener"
#define AIN "/org/qemu/Display1/AudioInListener"
#define QEMU_TIMEOUT_MS 1000

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int fails;
static void check(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  %s ", ok ? "ok    " : "FAILED");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok) fails++;
}

/* ---- seeded randomness ---- */
static uint64_t rs;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint32_t below(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }
static int chance(int pct) { return (int)below(100) < pct; }
/* an unsigned size from the edges or small */
static uint32_t pick_u(void) {
    static const uint32_t E[] = { 0, 1, 2, 3, 4, 7, 8, 16, 63, 64, 320, 640, 1023, 1024, 4096, VD_MAX_DIM - 1, VD_MAX_DIM, VD_MAX_DIM + 1,
                                  65535, 65536, 0x7fffffffu, 0x80000000u, 0xfffffffcu, 0xffffffffu };
    if (chance(50)) return E[below(sizeof E / sizeof *E)];
    return below(chance(70) ? 700 : 0xffffffffu);
}
static int32_t pick_i(void) {
    static const int32_t E[] = { 0, 1, -1, 2, 16, 255, 256, 257, -256, -257, 639, 640, 641, 479, 480, (int32_t)VD_MAX_DIM, INT_MAX, INT_MIN, INT_MAX - 1, INT_MIN + 1 };
    if (chance(50)) return E[below(sizeof E / sizeof *E)];
    return chance(80) ? (int32_t)below(700) - 30 : (int32_t)rnd();
}
static uint32_t pick_fmt(void) {
    static const uint32_t F[] = { 0x20020888u /* x8r8g8b8 */, 0x20028888u /* a8r8g8b8 */, VD_FOURCC_XRGB8888, VD_FOURCC_ARGB8888,
                                  VD_FOURCC_XBGR8888, VD_FOURCC_ABGR8888, 0, 0xffffffffu, 0x20020565u };
    return chance(85) ? F[below(sizeof F / sizeof *F)] : (uint32_t)rnd();
}

/* ---- mock QEMU ---- */
static sd_bus *q, *L, *AO, *AI;          /* console; listener; the two audio listeners */
static int srv = -1;                    /* lord-server's side of fd 3 */
static int gone;                        /* a connection broke: the helper died */
static int hostile_props;               /* this run's console says hostile things */
static int is_abs = 1;

static int p_label(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e) {
    (void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
    if (!hostile_props) return sd_bus_message_append(r, "s", "test");
    static char big[4097];
    for (int k = 0; k < 4096; k++) big[k] = (char)(1 + below(126));     /* controls and printable, no NUL */
    big[4096] = 0;
    return sd_bus_message_append(r, "s", big);
}
static int p_w(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e) {
    (void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
    return sd_bus_message_append(r, "u", hostile_props ? 0xffffffffu : 640u);
}
static int p_h(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e) {
    (void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
    return sd_bus_message_append(r, "u", hostile_props ? 0u : 480u);
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
    (void)e;
    int fd;
    if (sd_bus_message_read(m, "h", &fd) < 0) return -EINVAL;
    *(sd_bus **)u = server_bus(fcntl(fd, F_DUPFD_CLOEXEC, 3));
    return sd_bus_reply_method_return(m, NULL);
}
static int m_any(sd_bus_message *m, void *u, sd_bus_error *e) { (void)u; (void)e; return sd_bus_reply_method_return(m, NULL); }
/* QEMU's refusals, in a hostile run, carry words meant for the session's log: new lines, an escape sequence */
static const char EVIL[] = "refused\n2026-11-24 lord-server: forged entry\x1b[2J\r\x07";
static int m_uiinfo(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    if (hostile_props) return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "%s", EVIL);
    return sd_bus_reply_method_return(m, NULL);
}
static int m_register_in(sd_bus_message *m, void *u, sd_bus_error *e) {
    if (hostile_props) return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_NOT_SUPPORTED, "%s", EVIL);
    return m_register(m, u, e);
}
static const sd_bus_vtable console_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Label", "s", p_label, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Width", "u", p_w, 0, 0),
    SD_BUS_PROPERTY("Height", "u", p_h, 0, 0),
    SD_BUS_METHOD_WITH_OFFSET("RegisterListener", "h", "", m_register, 0, 0),
    SD_BUS_METHOD("SetUIInfo", "qqiiuu", "", m_uiinfo, 0),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable mouse_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("IsAbsolute", "b", p_abs, 0, 0),
    SD_BUS_METHOD("Press", "u", "", m_any, 0),
    SD_BUS_METHOD("Release", "u", "", m_any, 0),
    SD_BUS_METHOD("SetAbsPosition", "uu", "", m_any, 0),
    SD_BUS_METHOD("RelMotion", "ii", "", m_any, 0),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable kbd_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Press", "u", "", m_any, 0),
    SD_BUS_METHOD("Release", "u", "", m_any, 0),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable audio_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD_WITH_OFFSET("RegisterOutListener", "h", "", m_register, 0, 0),
    SD_BUS_METHOD_WITH_OFFSET("RegisterInListener", "h", "", m_register_in, sizeof(sd_bus *), 0),
    SD_BUS_VTABLE_END
};
static struct { sd_bus *out, *in; } audio_buses;

/* ---- mock lord-server ---- */
static int ready, frames, ack_frames = 1;
static void send_shm(int w, int h) {
    int fd = memfd_create("vmdisplay-fuzz-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, (off_t)w * h * 4) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW)) { perror("memfd"); exit(1); }
    VdMsg m = vd_msg(VD_SHM, 0, 0, 0, 0, 0, 0);
    vd_send(srv, &m, fd);
    close(fd);
}
static void tell(int type, int a, int b, int c, int d, int e, int f) { VdMsg m = vd_msg((uint16_t)type, a, b, c, d, e, f); vd_send(srv, &m, -1); }
static int ring(uint32_t ch, uint32_t size) {
    int fd = memfd_create("vmdisplay-fuzz-ring", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, (off_t)ar_bytes(size))) { perror("ring"); exit(1); }
    ARingHdr *h = mmap(NULL, ar_bytes(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (h == MAP_FAILED) { perror("ring map"); exit(1); }
    h->magic = AR_MAGIC; h->rate = AR_RATE; h->channels = ch; h->size = size;
    if (ch == 1) atomic_store(&h->wpos, (uint64_t)below(size) & ~(uint64_t)1);    /* a microphone with something in it */
    munmap(h, ar_bytes(size));
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
    return fd;
}
static void from_helper(void) {
    for (;;) {
        VdMsg m;
        int fd = -1;
        int r = vd_recv(srv, &m, &fd);
        if (r <= 0) return;
        if (fd >= 0) close(fd);
        switch (m.type) {
        case VD_READY: ready = 1; break;
        case VD_SIZE: if (m.a > 0 && m.b > 0 && (int64_t)m.a * m.b <= 4096 * 4096) send_shm(m.a, m.b); break;
        case VD_FRAME: frames++; if (ack_frames) tell(VD_FRAME_DONE, 0, 0, 0, 0, m.e, 0); break;
        }
    }
}
static int process_all(sd_bus *b) {
    int r;
    if (!b) return 0;
    while ((r = sd_bus_process(b, NULL)) > 0) ;
    return r;
}
static void pump(int ms) {
    long long until = now_ms() + ms;
    for (;;) {
        if (process_all(q) < 0 || process_all(L) < 0) { gone = 1; return; }
        if (process_all(AO) < 0) AO = NULL;     /* the helper may drop a sound connection; never the display */
        if (process_all(AI) < 0) AI = NULL;
        from_helper();
        long long left = until - now_ms();
        if (left <= 0) return;
        struct pollfd p[5] = {
            { sd_bus_get_fd(q), (short)sd_bus_get_events(q), 0 },
            { L ? sd_bus_get_fd(L) : -1, L ? (short)sd_bus_get_events(L) : 0, 0 },
            { AO ? sd_bus_get_fd(AO) : -1, AO ? (short)sd_bus_get_events(AO) : 0, 0 },
            { AI ? sd_bus_get_fd(AI) : -1, AI ? (short)sd_bus_get_events(AI) : 0, 0 },
            { srv, POLLIN, 0 },
        };
        poll(p, 5, left > 5 ? 5 : (int)left);
    }
}

/* ---- the calls, each with its answer ---- */
typedef struct { long long sent, answered; int error, held_ok; char what[24]; } Call;
static Call calls[1 << 15];
static int ncalls;
static int on_answer(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)e;
    Call *c = u;
    c->answered = now_ms();
    c->error = sd_bus_message_is_method_error(m, NULL);
    return 0;
}
/* a call on bus b; sig/ap appended. NULL when the message couldn't be built (a value D-Bus itself refuses) */
static Call *callv(sd_bus *b, const char *path, const char *iface, const char *method, const char *sig, va_list ap) {
    if (!b || ncalls >= (int)(sizeof calls / sizeof *calls)) return NULL;
    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_method_call(b, &m, NULL, path, iface, method);
    if (r >= 0 && sig) r = sd_bus_message_appendv(m, sig, ap);
    if (r < 0) { sd_bus_message_unref(m); return NULL; }
    Call *c = &calls[ncalls++];
    memset(c, 0, sizeof *c);
    snprintf(c->what, sizeof c->what, "%s", method);
    c->sent = now_ms();
    r = sd_bus_call_async(b, NULL, m, on_answer, c, (uint64_t)QEMU_TIMEOUT_MS * 4 * 1000);
    sd_bus_message_unref(m);
    if (r < 0) { ncalls--; return NULL; }
    return c;
}
static Call *lcall(const char *method, const char *sig, ...) {
    va_list ap; va_start(ap, sig);
    Call *c = callv(L, LISTENER, "org.qemu.Display1.Listener", method, sig, ap);
    va_end(ap);
    return c;
}
static Call *mcall(const char *method, const char *sig, ...) {
    va_list ap; va_start(ap, sig);
    Call *c = callv(L, LISTENER, "org.qemu.Display1.Listener.Unix.Map", method, sig, ap);
    va_end(ap);
    return c;
}
static Call *acall(int in, const char *method, const char *sig, ...) {
    va_list ap; va_start(ap, sig);
    Call *c = callv(in ? AI : AO, in ? AIN : AOUT, in ? "org.qemu.Display1.AudioInListener" : "org.qemu.Display1.AudioOutListener", method, sig, ap);
    va_end(ap);
    return c;
}
/* a call with an array argument built by hand (appendv takes arrays only as their elements) */
static Call *call_arr(sd_bus *b, const char *path, const char *iface, const char *method, const char *head, const uint32_t *hv, int nh,
                      const void *data, size_t len) {
    if (!b || ncalls >= (int)(sizeof calls / sizeof *calls)) return NULL;
    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_method_call(b, &m, NULL, path, iface, method);
    for (int i = 0; r >= 0 && i < nh; i++) {
        char t = head[i];
        if (t == 't') r = sd_bus_message_append_basic(m, 't', &(uint64_t){ hv[i] });
        else if (t == 'b') r = sd_bus_message_append_basic(m, 'b', &(int){ hv[i] != 0 });
        else r = sd_bus_message_append_basic(m, t, &hv[i]);
    }
    if (r >= 0) r = sd_bus_message_append_array(m, 'y', data, len);
    if (r < 0) { sd_bus_message_unref(m); return NULL; }
    Call *c = &calls[ncalls++];
    memset(c, 0, sizeof *c);
    snprintf(c->what, sizeof c->what, "%s", method);
    c->sent = now_ms();
    r = sd_bus_call_async(b, NULL, m, on_answer, c, (uint64_t)QEMU_TIMEOUT_MS * 4 * 1000);
    sd_bus_message_unref(m);
    if (r < 0) { ncalls--; return NULL; }
    return c;
}

static uint8_t *blob;                   /* random bytes the arrays take from */
#define BLOB (16u << 20)
static size_t pick_len(uint64_t want) {
    if (want <= BLOB && chance(70)) return (size_t)want;
    if (want && want - 1 <= BLOB && chance(40)) return (size_t)want - 1;
    if (want + 1 <= BLOB && chance(30)) return (size_t)want + 1;
    return below(chance(80) ? 4096 : BLOB);
}

static int map_fd = -1;                 /* the last ScanoutMap's file, kept so it can shrink under the helper */
static uint32_t cur_w = 640, cur_h = 480;

/* a picture the helper takes (so the updates that follow reach the copies): 0 an array, 1 a map, 2 a dmabuf */
static void good_scanout(int kind) {
    uint32_t w = 1 + below(640), h = 1 + below(480), stride = w * 4 + 4 * below(4);
    if (kind == 0) {
        uint32_t hv[4] = { w, h, stride, 0x20020888u };
        if (call_arr(L, LISTENER, "org.qemu.Display1.Listener", "Scanout", "uuuu", hv, 4, blob, (size_t)stride * h)) { cur_w = w; cur_h = h; }
        return;
    }
    int fd = memfd_create(kind == 1 ? "fuzz-map" : "fuzz-dmabuf", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    uint32_t off = kind == 1 ? 4 * below(64) : 0;
    if (fd < 0 || ftruncate(fd, (off_t)off + (off_t)stride * h)) { perror("memfd"); exit(1); }
    Call *c = kind == 1 ? mcall("ScanoutMap", "huuuuu", fd, off, w, h, stride, 0x20020888u)
                        : lcall("ScanoutDMABUF", "huuuutb", fd, w, h, stride, VD_FOURCC_XRGB8888, (uint64_t)0, 0);
    if (c) { cur_w = w; cur_h = h; }
    if (kind == 1) { if (map_fd >= 0) close(map_fd); map_fd = fd; } else close(fd);
}

static void one_call(void) {
    uint32_t w = pick_u(), h = pick_u(), stride, fmt = pick_fmt();
    if (chance(60)) { w = 1 + below(800); h = 1 + below(600); }
    stride = chance(70) ? w * 4 : chance(50) ? w * 4 + 4 * below(16) : pick_u();
    switch (below(19)) {
    case 0: {               /* Scanout: an array picture */
        uint32_t hv[4] = { w, h, stride, fmt };
        size_t len = pick_len((uint64_t)stride * h);
        if (call_arr(L, LISTENER, "org.qemu.Display1.Listener", "Scanout", "uuuu", hv, 4, blob, len) && (uint64_t)stride * h == len) { cur_w = w; cur_h = h; }
        break;
    }
    case 1: case 2: {       /* Update: a rectangle of it */
        if (chance(50)) good_scanout(0);
        int32_t x = pick_i(), y = pick_i(), rw = pick_i(), rh = pick_i();
        if (chance(60)) { x = (int32_t)below(cur_w); y = (int32_t)below(cur_h); rw = 1 + (int32_t)below(cur_w - (uint32_t)x + (chance(20) ? 2 : 0)); rh = 1 + (int32_t)below(cur_h - (uint32_t)y + (chance(20) ? 2 : 0)); }
        uint32_t st = chance(70) ? (uint32_t)rw * 4 : pick_u();
        uint64_t need = rh > 0 ? (uint64_t)st * (uint64_t)(rh - 1) + (uint64_t)(uint32_t)rw * 4 : 0;
        uint32_t hv[6] = { (uint32_t)x, (uint32_t)y, (uint32_t)rw, (uint32_t)rh, st, fmt };
        call_arr(L, LISTENER, "org.qemu.Display1.Listener", "Update", "iiiiuu", hv, 6, blob, pick_len(need));
        break;
    }
    case 3: {               /* ScanoutDMABUF: a memfd of some size, or a pipe */
        int fd, p[2] = { -1, -1 };
        if (chance(10) && !pipe2(p, O_CLOEXEC)) fd = p[0];
        else {
            fd = memfd_create("fuzz-dmabuf", MFD_CLOEXEC);
            uint64_t want = (uint64_t)stride * h;
            off_t sz = chance(60) && want <= (64u << 20) ? (off_t)want : (off_t)below(1u << 22);
            if (fd < 0 || ftruncate(fd, sz)) { perror("memfd"); exit(1); }
        }
        if (lcall("ScanoutDMABUF", "huuuutb", fd, w, h, stride, fmt, (uint64_t)rnd(), (int)below(2)) && w <= 800 && h <= 600) { cur_w = w; cur_h = h; }
        close(fd);
        if (p[1] >= 0) close(p[1]);
        break;
    }
    case 4: case 5: {       /* UpdateDMABUF */
        if (chance(40)) good_scanout(2);
        int32_t x = pick_i(), y = pick_i(), rw = pick_i(), rh = pick_i();
        if (chance(60)) { x = 0; y = 0; rw = (int32_t)cur_w; rh = (int32_t)cur_h; }
        Call *c = lcall("UpdateDMABUF", "iiii", x, y, rw, rh);
        if (c) c->held_ok = 1;
        break;
    }
    case 6: lcall("Disable", NULL); break;
    case 7: lcall("MouseSet", "iii", pick_i(), pick_i(), pick_i()); break;
    case 8: {               /* CursorDefine */
        int32_t cw = chance(60) ? 1 + (int32_t)below(256) : pick_i(), ch = chance(60) ? 1 + (int32_t)below(256) : pick_i();
        uint64_t want = cw > 0 && ch > 0 ? (uint64_t)cw * (uint64_t)ch * 4 : 0;
        uint32_t hv[4] = { (uint32_t)cw, (uint32_t)ch, (uint32_t)pick_i(), (uint32_t)pick_i() };
        call_arr(L, LISTENER, "org.qemu.Display1.Listener", "CursorDefine", "iiii", hv, 4, blob, pick_len(want));
        break;
    }
    case 9: {               /* ScanoutMap: a memfd (sealed or not) or a pipe, an offset */
        int fd, p[2] = { -1, -1 };
        uint32_t off = chance(70) ? 4 * below(64) : pick_u();
        if (chance(8) && !pipe2(p, O_CLOEXEC)) fd = p[0];
        else {
            fd = memfd_create("fuzz-map", MFD_CLOEXEC | MFD_ALLOW_SEALING);
            uint64_t want = (uint64_t)off + (uint64_t)stride * h;
            off_t sz = chance(70) && want <= (64u << 20) ? (off_t)want + (chance(30) ? (off_t)below(8192) : 0) : (off_t)below(1u << 22);
            if (fd < 0 || ftruncate(fd, sz)) { perror("memfd"); exit(1); }
            if (chance(50)) fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK);
        }
        if (mcall("ScanoutMap", "huuuuu", fd, off, w, h, stride, fmt) && w <= 800 && h <= 600) { cur_w = w; cur_h = h; }
        if (p[0] >= 0) { close(p[0]); close(p[1]); }
        else { if (map_fd >= 0) close(map_fd); map_fd = fd; }
        break;
    }
    case 10: case 11: {     /* UpdateMap; now and then the map's file shrinks first (QEMU's memory taken away: SIGBUS) */
        if (chance(50)) good_scanout(1);
        if (map_fd >= 0 && chance(8)) { if (ftruncate(map_fd, (off_t)below(4096))) { } }
        int32_t x = pick_i(), y = pick_i(), rw = pick_i(), rh = pick_i();
        if (chance(60)) { x = 0; y = 0; rw = (int32_t)cur_w; rh = (int32_t)cur_h; }
        mcall("UpdateMap", "iiii", x, y, rw, rh);
        break;
    }
    case 12: {              /* the wrong signature, or no such method: D-Bus answers it with an error */
        static const char *const M[] = { "Scanout", "Update", "ScanoutDMABUF", "UpdateDMABUF", "MouseSet", "CursorDefine", "Disable", "NoSuchMethod" };
        const char *mm = M[below(8)];
        if (chance(50)) lcall(mm, "s", "x");
        else lcall(mm, "uu", (uint32_t)rnd(), (uint32_t)rnd());
        if (chance(30)) mcall("ScanoutMap", "s", "y");
        break;
    }
    case 13: {              /* the listener's properties */
        sd_bus_message *m = NULL;
        if (sd_bus_message_new_method_call(L, &m, NULL, LISTENER, "org.freedesktop.DBus.Properties", "GetAll") >= 0 &&
            sd_bus_message_append(m, "s", chance(50) ? "org.qemu.Display1.Listener" : "nope") >= 0 && ncalls < (int)(sizeof calls / sizeof *calls)) {
            Call *c = &calls[ncalls++];
            memset(c, 0, sizeof *c);
            snprintf(c->what, sizeof c->what, "GetAll");
            c->sent = now_ms();
            if (sd_bus_call_async(L, NULL, m, on_answer, c, (uint64_t)QEMU_TIMEOUT_MS * 4 * 1000) < 0) ncalls--;
        }
        sd_bus_message_unref(m);
        break;
    }
    case 14: case 15: {     /* the speakers: Init, Fini, SetEnabled, SetVolume, Write */
        if (!AO) break;
        uint64_t id = below(chance(80) ? 4 : 64);
        switch (below(5)) {
        case 0: {
            uint8_t nch = chance(70) ? (uint8_t)(1 + below(2)) : (uint8_t)rnd();
            int good = chance(60);
            acall(0, "Init", "tybbuyuub", id, (uint8_t)(good ? 16 : rnd()), (int)(good || chance(50)), (int)(!good && chance(50)),
                  good ? 48000u : pick_u(), nch, good ? (uint32_t)nch * 2 : pick_u(), pick_u(), (int)(!good && chance(30)));
            break;
        }
        case 1: acall(0, "Fini", "t", id); break;
        case 2: acall(0, "SetEnabled", "tb", id, (int)below(2)); break;
        case 3: { uint32_t hv[2] = { (uint32_t)id, below(2) }; call_arr(AO, AOUT, "org.qemu.Display1.AudioOutListener", "SetVolume", "tb", hv, 2, blob, below(4)); break; }
        case 4: { uint32_t hv[1] = { (uint32_t)id }; call_arr(AO, AOUT, "org.qemu.Display1.AudioOutListener", "Write", "t", hv, 1, blob, chance(80) ? below(8192) : below(1u << 20)); break; }
        }
        break;
    }
    case 16: case 17: {     /* the microphone: Init, Fini, SetEnabled, SetVolume, Read */
        if (!AI) break;
        uint64_t id = below(chance(80) ? 4 : 64);
        switch (below(5)) {
        case 0: {
            uint8_t nch = chance(70) ? (uint8_t)(1 + below(2)) : (uint8_t)rnd();
            int good = chance(60);
            acall(1, "Init", "tybbuyuub", id, (uint8_t)(good ? 16 : rnd()), (int)(good || chance(50)), (int)(!good && chance(50)),
                  good ? 48000u : pick_u(), nch, good ? (uint32_t)nch * 2 : pick_u(), pick_u(), (int)(!good && chance(30)));
            break;
        }
        case 1: acall(1, "Fini", "t", id); break;
        case 2: acall(1, "SetEnabled", "tb", id, (int)below(2)); break;
        case 3: { uint32_t hv[2] = { (uint32_t)id, below(2) }; call_arr(AI, AIN, "org.qemu.Display1.AudioInListener", "SetVolume", "tb", hv, 2, blob, below(4)); break; }
        case 4: acall(1, "Read", "tt", id, chance(50) ? (uint64_t)below(20000) : (uint64_t)rnd()); break;
        }
        break;
    }
    case 18: {              /* lord-server's side meanwhile: input, sizes, rates, pacing, rings */
        switch (below(8)) {
        case 0: tell(VD_MOTION, (int)below(cur_w), (int)below(cur_h), 0, 0, 0, 0); break;
        case 1: tell(VD_BUTTON, (int)below(3), (int)below(2), 0, 0, 0, 0); break;
        case 2: tell(VD_KEY, (int)below(256), (int)below(2), 0, 0, 0, 0); break;
        case 3: tell(VD_RESIZE, 320 + (int)below(1600), 200 + (int)below(1000), 0, 0, 0, 0); break;
        case 4: tell(VD_RATE, 1 + (int)below(240), 0, 0, 0, 0, 0); break;
        case 5: tell(VD_PACE, (int)below(200), (int)below(3), 0, 0, 0, 0); break;
        case 6: { int fd = ring(2, 1u << (12 + below(6))); VdMsg m = vd_msg(VD_AUDIO_OUT, 1, 0, 0, 0, 0, 0); vd_send(srv, &m, fd); close(fd); break; }
        case 7: { int fd = ring(1, 1u << (12 + below(6))); VdMsg m = vd_msg(chance(50) ? VD_MIC : VD_AUDIO_REC, 1, 0, 0, 0, 0, 0); vd_send(srv, &m, fd); close(fd); break; }
        }
        ack_frames = !chance(5) ? 1 : ack_frames ^ 1;
        break;
    }
    }
}

static int run(const char *helper, int idx, int ncall, int props) {
    int f0 = fails;
    printf("run %d: %s console, %d calls\n", idx, props ? "hostile" : "plain", ncall);
    hostile_props = props;
    q = L = AO = AI = NULL;
    audio_buses.out = audio_buses.in = NULL;
    gone = ready = frames = 0;
    ncalls = 0;
    map_fd = -1;
    cur_w = 640; cur_h = 480;
    int sv[2], dv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, dv)) { perror("socketpair"); exit(1); }
    char errpath[64];
    snprintf(errpath, sizeof errpath, "build/vmdisplay-fuzz-%d.err", idx);
    pid_t pid = fork();
    if (pid == 0) {
        int a = fcntl(sv[1], F_DUPFD, 20), b = fcntl(dv[1], F_DUPFD, 21);
        int e = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (e >= 0) dup2(e, 2);
        dup2(a, 3);
        dup2(b, 4);
        setenv("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=1:allocator_may_return_null=1", 1);
        setenv("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1", 1);
        char *args[] = { "lord-vmdisplay", "0b2b4eb6-77dd-42a8-933c-3cf7eac71aa2", NULL };
        execv(helper, args);
        _exit(127);
    }
    close(sv[1]);
    close(dv[1]);
    srv = sv[0];
    fcntl(srv, F_SETFL, O_NONBLOCK);
    q = server_bus(dv[0]);
    if (sd_bus_add_object_vtable(q, NULL, CONSOLE, "org.qemu.Display1.Console", console_vt, &L) < 0 ||
        sd_bus_add_object_vtable(q, NULL, CONSOLE, "org.qemu.Display1.Mouse", mouse_vt, NULL) < 0 ||
        sd_bus_add_object_vtable(q, NULL, CONSOLE, "org.qemu.Display1.Keyboard", kbd_vt, NULL) < 0 ||
        sd_bus_add_object_vtable(q, NULL, "/org/qemu/Display1/Audio", "org.qemu.Display1.Audio", audio_vt, &audio_buses) < 0) {
        fprintf(stderr, "mock QEMU: vtables\n");
        exit(1);
    }
    for (long long t = now_ms() + 10000; !ready && !gone && now_ms() < t;) { pump(10); AO = audio_buses.out; AI = audio_buses.in; }
    AO = audio_buses.out; AI = audio_buses.in;
    check(ready && L && AO && (AI || props), "it registered its listener and the sound listeners QEMU took, and said it's ready");
    if (!ready || !L) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); return 1; }

    for (int i = 0; i < ncall && !gone; i++) {
        one_call();
        if (chance(25)) pump((int)below(4));
        /* never more than a few hundred waiting at once: QEMU itself waits on each */
        if (i % 64 == 63) pump(20);
    }
    /* every answer in, the held ones too: QEMU's flood is served in order, so the last ones wait behind the big arrays */
    for (long long t = now_ms() + 30000; now_ms() < t && !gone;) {
        int left = 0;
        for (int i = 0; i < ncalls; i++) if (!calls[i].answered) left++;
        if (!left) break;
        pump(50);
    }
    int flood = ncalls;
    /* then one call at a time, as QEMU's console makes them: each answered inside QEMU's timeout */
    for (int i = 0; i < 300 && !gone; i++) {
        int k = ncalls;
        one_call();
        for (long long t = now_ms() + 3000; k < ncalls && !calls[ncalls - 1].answered && now_ms() < t && !gone;) pump(1);
    }
    pump(400);
    int st = 0, alive = waitpid(pid, &st, WNOHANG) == 0;
    check(alive && !gone, "it lived through %d hostile calls (%d frames handed on)", ncalls, frames);
    int un = 0, slow = 0;
    long long worst = 0;
    const char *slow_what = "";
    for (int i = 0; i < ncalls; i++) {
        Call *c = &calls[i];
        if (!c->answered && i >= flood && c->held_ok) continue;     /* a held UpdateDMABUF: answered with the next frame */
        if (!c->answered) { un++; if (un <= 3) fprintf(stderr, "run %d: no answer to #%d %s\n", idx, i, c->what); continue; }
        if (i < flood) continue;
        long long d = c->answered - c->sent;
        if (d > worst) { worst = d; slow_what = c->what; }
        if (d >= QEMU_TIMEOUT_MS) slow++;
    }
    check(!un, "every call answered (%d unanswered)", un);
    check(!slow, "one at a time, each answered within QEMU's %d ms (%d calls, worst %lld ms, %s)", QEMU_TIMEOUT_MS, ncalls - flood, worst, slow_what);
    tell(VD_CLOSE, 0, 0, 0, 0, 0, 0);
    long long t = now_ms() + 5000;
    pid_t w = 0;
    while (now_ms() < t && (w = waitpid(pid, &st, WNOHANG)) == 0) { pump(10); }
    if (w != pid) { kill(pid, SIGKILL); waitpid(pid, &st, 0); }
    check(w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0, "VD_CLOSE: it exits 0 (%s %d)", WIFEXITED(st) ? "status" : "signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
    /* no sanitizer report in its log */
    FILE *f = fopen(errpath, "r");
    int report = 0;
    char line[512];
    int ctl = 0, forged = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (strstr(line, "Sanitizer") || strstr(line, "runtime error")) { if (!report) fprintf(stderr, "%s", line); report = 1; }
        if (!strncmp(line, "2026-11-24 lord-server", 22)) forged = 1;
        for (char *c = line; *c; c++) if ((unsigned char)*c < 0x20 && *c != '\n') ctl = 1;
    }
    if (f) fclose(f);
    check(!report, "no ASan or UBSan report (%s)", errpath);
    check(!ctl && !forged, "QEMU's words (a label, its refusals) reached the log as one line each, with no control character");
    if (map_fd >= 0) close(map_fd);
    sd_bus_flush_close_unref(L); sd_bus_flush_close_unref(q);
    if (audio_buses.out) sd_bus_flush_close_unref(audio_buses.out);
    if (audio_buses.in) sd_bus_flush_close_unref(audio_buses.in);
    close(srv);
    return fails > f0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: vmdisplay-fuzz build/lord-vmdisplay-asan-test [seed] [calls per run]\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x1d15ea5e;
    int ncall = argc > 3 ? atoi(argv[3]) : 2500;
    blob = malloc(BLOB);
    if (!blob) return 1;
    rs = seed ? seed : 1;
    for (size_t i = 0; i < BLOB; i += 8) { uint64_t v = rnd(); memcpy(blob + i, &v, 8); }
    printf("lord-vmdisplay against a hostile QEMU (seed 0x%" PRIx64 "):\n", seed);
    for (int r = 0; r < 4; r++) run(argv[1], r, ncall, r == 1);
    printf("%s\n", fails ? "vmdisplay-fuzz: FAILED" : "vmdisplay-fuzz: all passed");
    return fails ? 1 : 0;
}
