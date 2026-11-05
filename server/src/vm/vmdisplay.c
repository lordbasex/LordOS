/* vmdisplay.c — lord-vmdisplay: one machine's picture and input (docs/VIRTUALIZATION.md 3.2 and "Phase 1
 * implementation"). It speaks QEMU's D-Bus display protocol peer to peer on fd 4 (a connection lord-vm had QEMU
 * accept), and passes lord-server only the fixed messages of vmproto.h on fd 3. The compositor never parses
 * anything QEMU sends.
 *
 *   lord-vmdisplay <machine id>     fd 3: SOCK_SEQPACKET to lord-server; fd 4: the D-Bus connection to QEMU
 *   lord-vmdisplay --selftest       applies the sandbox, then tries what it must not be able to do (JSON line)
 *
 * Decisions (Phase 0, section 13.6):
 *   - It requires org.qemu.Display1.Listener.Unix.Map: QEMU shares its picture once as a memfd and sends
 *     rectangles. Plain Scanout/Update arrays are still accepted and copied, because QEMU uses them for surfaces
 *     it can't share. DMABUF (3D) is refused in the alpha.
 *   - QEMU's memfd is sealed only against new seals, so QEMU can shrink it under our mapping: every read of it
 *     runs under a SIGBUS guard, and a fault drops that mapping (the picture waits for the next ScanoutMap).
 *     Refusing unsealed buffers would mean no picture at all with QEMU 10.0.
 *   - Copy before forward: the damaged area goes from QEMU's buffer into the picture lord-server shared (sealed
 *     by lord-server against shrinking), at most VD_RATE times a second, and not again until lord-server said
 *     it read the last one (VD_FRAME_DONE) or 250 ms passed. lord-server never sees QEMU's live memory.
 *   - It never makes a blocking call to QEMU after setup (QEMU calls the listener from its main loop): input goes
 *     out as method calls that expect no reply.
 *   - Sound (6.9, 6.20): an org.qemu.Display1.AudioOutListener on a third peer connection, registered before the
 *     sandbox. Only 16-bit little-endian PCM is taken (QEMU is told 48 kHz stereo s16, -audiodev in lordvm.c); while
 *     a recording runs (VD_AUDIO_REC) it goes, mixed to one channel, into the ring lord-media reads, with silence for
 *     the time the guest plays nothing and nothing beyond 200 ms ahead of the clock, so it keeps time with the
 *     picture. */
#include "vmproto.h"
#include "vmkeys.h"
#include "../sec/sandbox.h"
#include "../media/aring.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#define CH 3
#define DBUS_FD 4

#define PIXMAN_x8r8g8b8 0x20020888u
#define PIXMAN_a8r8g8b8 0x20028888u
#define LISTENER_PATH "/org/qemu/Display1/Listener"
#define CONSOLE_PATH  "/org/qemu/Display1/Console_0"
#define ACK_WAIT_MS   250
#define RESIZE_GAP_MS 100
#define STATS_MS      5000

static sd_bus *bus, *lbus, *abus;

/* where the picture comes from */
enum { SRC_NONE = 0, SRC_MAP, SRC_FB };
static struct {
    int src;
    uint32_t w, h;
    /* SRC_MAP: QEMU's buffer */
    uint8_t *map;
    size_t map_len;
    uint32_t off, stride;
    /* SRC_FB: our copy of an array Scanout */
    uint8_t *fb;
    /* lord-server's picture */
    uint8_t *shm;
    size_t shm_len;
    uint32_t shm_w, shm_h;
    int want_w, want_h;         /* last VD_SIZE sent */
    /* damage waiting to be copied */
    int dirty, dx0, dy0, dx1, dy1;
    int frame, acked;
    long long sent_ms, next_ms;
    int rate;
    /* cursor and pointer */
    int ptr_pending, ptr_x, ptr_y, ptr_on;
    /* input */
    int motion_pending, mx, my, last_mx, last_my;
    int abs_mouse;
    uint8_t keys[768];
    uint8_t buttons[16];
    int resize_pending, rw, rh;
    long long resize_ms;
    /* counters */
    long long stats_ms;
    int refused, updates, copies, stats_changed;
    int refused_dmabuf_told;
} D;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void say(VdMsg m, int fd) {
    if (vd_send(CH, &m, fd) && errno != EAGAIN) { /* lord-server gone: the loop sees the hangup */ }
}

static void refuse(const char *what, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void refuse(const char *what, const char *fmt, ...) {
    D.refused++;
    D.stats_changed = 1;
    if (D.refused > 50) return;            /* the count still goes up; the log doesn't fill */
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "lord-vmdisplay: refused %s: %s\n", what, line);
}

/* ---- the SIGBUS guard around reads of QEMU's buffer ---- */

static sigjmp_buf bus_jmp;
static volatile sig_atomic_t in_copy;

static void on_sigbus(int sig, siginfo_t *si, void *u) {
    (void)si; (void)u;
    if (in_copy) siglongjmp(bus_jmp, 1);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void drop_map(void) {
    if (D.map) munmap(D.map, D.map_len);
    D.map = NULL;
    D.map_len = 0;
    if (D.src == SRC_MAP) D.src = SRC_NONE;
}

static void drop_fb(void) {
    free(D.fb);
    D.fb = NULL;
    if (D.src == SRC_FB) D.src = SRC_NONE;
}

static void damage(int x, int y, int w, int h) {
    if (!D.dirty) { D.dx0 = x; D.dy0 = y; D.dx1 = x + w; D.dy1 = y + h; D.dirty = 1; return; }
    if (x < D.dx0) D.dx0 = x;
    if (y < D.dy0) D.dy0 = y;
    if (x + w > D.dx1) D.dx1 = x + w;
    if (y + h > D.dy1) D.dy1 = y + h;
}

/* ---- sound ---- */

#define AU_STREAMS 8
static struct {
    struct { uint64_t id; int used, ok, ch, on, mute; } st[AU_STREAMS];
    ARingHdr *ring;                 /* a recording's ring (VD_AUDIO_REC), or NULL */
    size_t ring_len;
    uint32_t size;                  /* our copy of its data size, never the shared header's */
    uint64_t wpos;
    long long t0;                   /* when the recording started (CLOCK_MONOTONIC ms) */
    uint64_t samples;               /* written since t0 */
    long long writes, pads, dropped;
} A;

static int au_find(uint64_t id) { for (int i = 0; i < AU_STREAMS; i++) if (A.st[i].used && A.st[i].id == id) return i; return -1; }

static void au_put(const int16_t *pcm, size_t k) {
    size_t n = k * 2;
    if (A.ring) ar_write_t(A.ring, A.size, &A.wpos, pcm, n);
    A.samples += k;
}
/* silence up to `upto` samples since t0 */
static void au_pad(uint64_t upto) {
    static const int16_t zero[960];
    while (A.ring && A.samples < upto) {
        size_t k = upto - A.samples < 960 ? (size_t)(upto - A.samples) : 960;
        au_put(zero, k);
        A.pads++;
    }
}
static uint64_t au_expected(long long now) { return now > A.t0 ? (uint64_t)(now - A.t0) * 48 : 0; }
static void au_tick(long long now) {
    uint64_t want = au_expected(now);
    if (A.ring && A.samples + 2400 < want) au_pad(want - 960);         /* 50 ms behind: the guest is quiet */
}

static int on_au_init(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id; uint8_t bits, nch; int sgn, flt, be; uint32_t freq, bpf, bps;
    if (sd_bus_message_read(m, "tybbuyuub", &id, &bits, &sgn, &flt, &freq, &nch, &bpf, &bps, &be) < 0) return 0;
    int i = au_find(id);
    for (int k = 0; i < 0 && k < AU_STREAMS; k++) if (!A.st[k].used) i = k;
    if (i < 0) return 0;
    A.st[i].used = 1; A.st[i].id = id; A.st[i].on = 0; A.st[i].mute = 0;
    A.st[i].ok = bits == 16 && sgn && !flt && !be && freq == 48000 && (nch == 1 || nch == 2) && bpf == (uint32_t)nch * 2;
    A.st[i].ch = nch;
    if (!A.st[i].ok) fprintf(stderr, "lord-vmdisplay: sound stream %llu isn't 48 kHz 16-bit (%u bits, %u Hz, %u channels): not used\n", (unsigned long long)id, bits, freq, nch);
    return sd_bus_reply_method_return(m, "");
}
static int on_au_fini(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id;
    if (sd_bus_message_read(m, "t", &id) >= 0) { int i = au_find(id); if (i >= 0) A.st[i].used = 0; }
    return sd_bus_reply_method_return(m, "");
}
static int on_au_enabled(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id; int on;
    if (sd_bus_message_read(m, "tb", &id, &on) >= 0) { int i = au_find(id); if (i >= 0) A.st[i].on = on; }
    return sd_bus_reply_method_return(m, "");
}
static int on_au_volume(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id; int mute;
    if (sd_bus_message_read(m, "tb", &id, &mute) >= 0) { int i = au_find(id); if (i >= 0) A.st[i].mute = mute; }
    return sd_bus_reply_method_return(m, "");
}
static int on_au_write(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id;
    const void *data = NULL;
    size_t n = 0;
    if (sd_bus_message_read(m, "t", &id) < 0 || sd_bus_message_read_array(m, 'y', &data, &n) < 0) return 0;
    int i = au_find(id);
    if (i < 0 || !A.st[i].ok || !A.ring) return sd_bus_reply_method_return(m, "");
    int ch = A.st[i].ch;
    size_t frames = n / ((size_t)ch * 2);
    long long now = now_ms();
    uint64_t want = au_expected(now);
    if (A.samples > want + 9600) { A.dropped += (long long)frames; return sd_bus_reply_method_return(m, ""); }   /* 200 ms ahead */
    au_tick(now);
    static int16_t mono[4096];
    const uint8_t *p = data;
    for (size_t f = 0; f < frames;) {
        size_t k = frames - f < 4096 ? frames - f : 4096;
        for (size_t j = 0; j < k; j++, f++) {
            int16_t l, r;
            memcpy(&l, p + f * (size_t)ch * 2, 2);
            if (ch == 2) memcpy(&r, p + f * 4 + 2, 2); else r = l;
            mono[j] = A.st[i].mute ? 0 : (int16_t)(((int)l + (int)r) / 2);
        }
        au_put(mono, k);
    }
    A.writes++;
    return sd_bus_reply_method_return(m, "");
}
static const sd_bus_vtable audio_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Init", "tybbuyuub", "", on_au_init, 0),
    SD_BUS_METHOD("Fini", "t", "", on_au_fini, 0),
    SD_BUS_METHOD("SetEnabled", "tb", "", on_au_enabled, 0),
    SD_BUS_METHOD("SetVolume", "tbay", "", on_au_volume, 0),
    SD_BUS_METHOD("Write", "tay", "", on_au_write, 0),
    SD_BUS_VTABLE_END
};

/* VD_AUDIO_REC: the recording's ring (checked: sealed against shrinking, the size it says, 48 kHz, one channel) */
static void au_rec(int fd) {
    if (A.ring) { munmap(A.ring, A.ring_len); A.ring = NULL; }
    if (A.writes || A.pads) fprintf(stderr, "lord-vmdisplay: recording's sound: %lld writes, %lld silences, %lld dropped\n", A.writes, A.pads, A.dropped);
    A.writes = A.pads = A.dropped = 0;
    if (fd < 0) return;
    struct stat st;
    int seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0 || !(seals & F_SEAL_SHRINK) || fstat(fd, &st) || st.st_size < AR_HDR) { close(fd); return; }
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    ARingHdr *h = map;
    uint32_t size = h->size;
    if (h->magic != AR_MAGIC || h->rate != AR_RATE || h->channels != 1 || !size || (size & (size - 1)) || ar_bytes(size) > (size_t)st.st_size) {
        munmap(map, (size_t)st.st_size);
        return;
    }
    A.ring = h; A.ring_len = (size_t)st.st_size; A.size = size;
    A.wpos = atomic_load(&h->wpos);
    A.t0 = now_ms();
    A.samples = 0;
}

/* a new picture size: ask lord-server for a picture of it; the old one isn't written again */
static void new_size(uint32_t w, uint32_t h) {
    D.w = w;
    D.h = h;
    damage(0, 0, (int)w, (int)h);
    D.dx0 = D.dy0 = 0; D.dx1 = (int)w; D.dy1 = (int)h;
    if (D.shm && (D.shm_w != w || D.shm_h != h)) { munmap(D.shm, D.shm_len); D.shm = NULL; D.shm_len = 0; }
    if (!D.shm && (D.want_w != (int)w || D.want_h != (int)h)) {
        D.want_w = (int)w;
        D.want_h = (int)h;
        say(vd_msg(VD_SIZE, (int)w, (int)h, 0, 0, 0, 0), -1);
    }
}

/* copies the damage into lord-server's picture. 0 copied, -1 QEMU's buffer faulted (dropped) */
static int copy_damage(int x, int y, int w, int h) {
    /* volatile: they're read again after a SIGBUS longjmp */
    const uint8_t *volatile src;
    volatile uint32_t sstride;
    if (D.src == SRC_MAP) { src = D.map + D.off; sstride = D.stride; }
    else if (D.src == SRC_FB) { src = D.fb; sstride = D.w * 4; }
    else return 0;
    uint8_t *volatile dst = D.shm;
    size_t dstride = (size_t)D.w * 4, row = (size_t)w * 4;
    if (D.src == SRC_MAP) {
        if (sigsetjmp(bus_jmp, 1)) {
            in_copy = 0;
            refuse("ScanoutMap", "QEMU's buffer shrank under the copy (SIGBUS); waiting for a new one");
            drop_map();
            return -1;
        }
        in_copy = 1;
    }
    for (int r = 0; r < h; r++) memcpy(dst + (size_t)(y + r) * dstride + (size_t)x * 4, src + (size_t)(y + r) * sstride + (size_t)x * 4, row);
    in_copy = 0;
    D.copies++;
    D.stats_changed = 1;
    return 0;
}

static void flush(long long now) {
    if (!D.dirty || !D.shm || D.src == SRC_NONE || D.shm_w != D.w || D.shm_h != D.h) return;
    if (!D.acked && now - D.sent_ms < ACK_WAIT_MS) return;
    if (now < D.next_ms) return;
    int x0 = D.dx0 < 0 ? 0 : D.dx0, y0 = D.dy0 < 0 ? 0 : D.dy0;
    int x1 = D.dx1 > (int)D.w ? (int)D.w : D.dx1, y1 = D.dy1 > (int)D.h ? (int)D.h : D.dy1;
    D.dirty = 0;
    if (x1 <= x0 || y1 <= y0) return;
    if (copy_damage(x0, y0, x1 - x0, y1 - y0)) return;
    D.frame++;
    D.acked = 0;
    D.sent_ms = now;
    D.next_ms = now + 1000 / (D.rate ? D.rate : 60);
    say(vd_msg(VD_FRAME, x0, y0, x1 - x0, y1 - y0, D.frame, 0), -1);
}

/* ---- validation ---- */

static bool fmt_ok(uint32_t f) { return f == PIXMAN_x8r8g8b8 || f == PIXMAN_a8r8g8b8; }

static uint64_t geometry(const char *what, uint32_t w, uint32_t h, uint32_t stride, uint32_t fmt) {
    if (!w || !h || w > VD_MAX_DIM || h > VD_MAX_DIM) { refuse(what, "size %ux%u out of limits", w, h); return 0; }
    if (!fmt_ok(fmt)) { refuse(what, "pixel format 0x%08x", fmt); return 0; }
    if ((uint64_t)stride < (uint64_t)w * 4 || stride % 4) { refuse(what, "stride %u for width %u", stride, w); return 0; }
    uint64_t bytes = (uint64_t)stride * h;
    if (bytes > VD_MAX_BYTES || (uint64_t)w * h * 4 > VD_MAX_BYTES) { refuse(what, "%" PRIu64 " bytes over the limit", bytes); return 0; }
    return bytes;
}

static bool rect_ok(const char *what, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (D.src == SRC_NONE) { refuse(what, "an update without a picture"); return false; }
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || (uint64_t)x + (uint64_t)w > D.w || (uint64_t)y + (uint64_t)h > D.h) {
        refuse(what, "rectangle %d,%d %dx%d outside %ux%u", x, y, w, h, D.w, D.h);
        return false;
    }
    return true;
}

/* ---- org.qemu.Display1.Listener ---- */

static int ok(sd_bus_message *m) { return sd_bus_reply_method_return(m, NULL); }

static int on_scanout(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint32_t w, h, stride, fmt;
    const void *data;
    size_t len;
    int r = sd_bus_message_read(m, "uuuu", &w, &h, &stride, &fmt);
    if (r >= 0) r = sd_bus_message_read_array(m, 'y', &data, &len);
    if (r < 0) { refuse("Scanout", "malformed"); return ok(m); }
    D.updates++;
    uint64_t bytes = geometry("Scanout", w, h, stride, fmt);
    if (!bytes) return ok(m);
    if (len != bytes) { refuse("Scanout", "%zu bytes for stride × height %" PRIu64, len, bytes); return ok(m); }
    drop_map();
    drop_fb();
    D.fb = malloc((size_t)w * h * 4);
    if (!D.fb) { refuse("Scanout", "out of memory"); return ok(m); }
    for (uint32_t row = 0; row < h; row++) memcpy(D.fb + (size_t)row * w * 4, (const uint8_t *)data + (size_t)row * stride, (size_t)w * 4);
    D.src = SRC_FB;
    new_size(w, h);
    return ok(m);
}

static int on_update(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int32_t x, y, w, h;
    uint32_t stride, fmt;
    const void *data;
    size_t len;
    int r = sd_bus_message_read(m, "iiiiuu", &x, &y, &w, &h, &stride, &fmt);
    if (r >= 0) r = sd_bus_message_read_array(m, 'y', &data, &len);
    if (r < 0) { refuse("Update", "malformed"); return ok(m); }
    D.updates++;
    if (D.src != SRC_FB) { refuse("Update", "no array picture to update"); return ok(m); }
    if (!rect_ok("Update", x, y, w, h)) return ok(m);
    if (!fmt_ok(fmt) || (uint64_t)stride < (uint64_t)w * 4) { refuse("Update", "format 0x%08x stride %u", fmt, stride); return ok(m); }
    if ((uint64_t)stride * (uint64_t)(h - 1) + (uint64_t)w * 4 > len) { refuse("Update", "%zu bytes too few", len); return ok(m); }
    for (int32_t row = 0; row < h; row++)
        memcpy(D.fb + ((size_t)(y + row) * D.w + (size_t)x) * 4, (const uint8_t *)data + (size_t)row * stride, (size_t)w * 4);
    damage(x, y, w, h);
    return ok(m);
}

static void told_dmabuf(void) {
    if (D.refused_dmabuf_told) return;
    D.refused_dmabuf_told = 1;
    say(vd_msg(VD_STATE, VDS_NO_PICTURE, VDR_REFUSED, 0, 0, 0, 0), -1);
}

static int on_scanout_dmabuf(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    refuse("ScanoutDMABUF", "3D pictures aren't used in this version");
    drop_map();
    drop_fb();
    told_dmabuf();
    return ok(m);       /* sd-bus closes the descriptor with the message */
}
static int on_update_dmabuf(sd_bus_message *m, void *u, sd_bus_error *e) { (void)u; (void)e; D.updates++; return ok(m); }

static int on_disable(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    drop_map();
    drop_fb();
    D.dirty = 0;
    say(vd_msg(VD_STATE, VDS_NO_PICTURE, VDR_DISABLED, 0, 0, 0, 0), -1);
    return ok(m);
}

static int on_mouse_set(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int32_t x, y, on;
    if (sd_bus_message_read(m, "iii", &x, &y, &on) < 0) { refuse("MouseSet", "malformed"); return ok(m); }
    if (x < -VD_MAX_CURSOR || y < -VD_MAX_CURSOR || x > (int32_t)VD_MAX_DIM || y > (int32_t)VD_MAX_DIM) { refuse("MouseSet", "position %d,%d", x, y); return ok(m); }
    D.ptr_x = x; D.ptr_y = y; D.ptr_on = on ? 1 : 0; D.ptr_pending = 1;
    return ok(m);
}

static int on_cursor_define(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int32_t w, h, hx, hy;
    const void *data;
    size_t len;
    int r = sd_bus_message_read(m, "iiii", &w, &h, &hx, &hy);
    if (r >= 0) r = sd_bus_message_read_array(m, 'y', &data, &len);
    if (r < 0) { refuse("CursorDefine", "malformed"); return ok(m); }
    if (w <= 0 || h <= 0 || w > VD_MAX_CURSOR || h > VD_MAX_CURSOR) { refuse("CursorDefine", "size %dx%d", w, h); return ok(m); }
    if (len != (size_t)w * (size_t)h * 4) { refuse("CursorDefine", "%zu bytes for %dx%d", len, w, h); return ok(m); }
    if (hx < 0 || hy < 0 || hx >= w || hy >= h) { refuse("CursorDefine", "hotspot %d,%d outside %dx%d", hx, hy, w, h); return ok(m); }
    int fd = memfd_create("lordos-vmcursor", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return ok(m);
    size_t off = 0;
    while (off < len) { ssize_t k = pwrite(fd, (const char *)data + off, len - off, (off_t)off); if (k <= 0) { if (k < 0 && errno == EINTR) continue; break; } off += (size_t)k; }
    if (off == len && !fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL))
        say(vd_msg(VD_CURSOR, w, h, hx, hy, 0, 0), fd);
    close(fd);
    return ok(m);
}

static int prop_interfaces(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *u, sd_bus_error *e) {
    (void)b; (void)path; (void)iface; (void)prop; (void)u; (void)e;
    int r = sd_bus_message_open_container(reply, 'a', "s");
    if (r >= 0) r = sd_bus_message_append(reply, "s", "org.qemu.Display1.Listener.Unix.Map");
    return r < 0 ? r : sd_bus_message_close_container(reply);
}

/* ---- org.qemu.Display1.Listener.Unix.Map ---- */

static int on_scanout_map(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int fd;
    uint32_t off, w, h, stride, fmt;
    if (sd_bus_message_read(m, "huuuuu", &fd, &off, &w, &h, &stride, &fmt) < 0) { refuse("ScanoutMap", "malformed"); return ok(m); }
    D.updates++;
    uint64_t bytes = geometry("ScanoutMap", w, h, stride, fmt);
    if (!bytes) return ok(m);
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { refuse("ScanoutMap", "not a memory file"); return ok(m); }
    off_t real = lseek(fd, 0, SEEK_END);
    if (real < 0 || (uint64_t)off + bytes > (uint64_t)real || (uint64_t)real > (uint64_t)VD_MAX_BYTES * 2) {
        refuse("ScanoutMap", "offset %u + %" PRIu64 " bytes against a file of %lld", off, bytes, (long long)real);
        return ok(m);
    }
    void *p = mmap(NULL, (size_t)real, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { refuse("ScanoutMap", "mmap: %s", strerror(errno)); return ok(m); }
    drop_map();
    drop_fb();
    D.map = p; D.map_len = (size_t)real; D.off = off; D.stride = stride;
    D.src = SRC_MAP;
    static int seals_told;
    if (!seals_told) {
        int seals = fcntl(fd, F_GET_SEALS);
        fprintf(stderr, "lord-vmdisplay: QEMU's picture: %ux%u, memfd of %lld bytes, sealed against shrinking: %s (reads are SIGBUS-guarded)\n",
                w, h, (long long)real, seals >= 0 && (seals & F_SEAL_SHRINK) ? "yes" : "no");
        seals_told = 1;
    }
    new_size(w, h);
    return ok(m);
}

static int on_update_map(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int32_t x, y, w, h;
    if (sd_bus_message_read(m, "iiii", &x, &y, &w, &h) < 0) { refuse("UpdateMap", "malformed"); return ok(m); }
    D.updates++;
    if (D.src != SRC_MAP) { refuse("UpdateMap", "no shared picture"); return ok(m); }
    if (rect_ok("UpdateMap", x, y, w, h)) damage(x, y, w, h);
    return ok(m);
}

static const sd_bus_vtable listener_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Scanout", "uuuuay", "", on_scanout, 0),
    SD_BUS_METHOD("Update", "iiiiuuay", "", on_update, 0),
    SD_BUS_METHOD("ScanoutDMABUF", "huuuutb", "", on_scanout_dmabuf, 0),
    SD_BUS_METHOD("UpdateDMABUF", "iiii", "", on_update_dmabuf, 0),
    SD_BUS_METHOD("Disable", "", "", on_disable, 0),
    SD_BUS_METHOD("MouseSet", "iii", "", on_mouse_set, 0),
    SD_BUS_METHOD("CursorDefine", "iiiiay", "", on_cursor_define, 0),
    SD_BUS_PROPERTY("Interfaces", "as", prop_interfaces, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable map_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("ScanoutMap", "huuuuu", "", on_scanout_map, 0),
    SD_BUS_METHOD("UpdateMap", "iiii", "", on_update_map, 0),
    SD_BUS_VTABLE_END
};

/* ---- input to QEMU: method calls that expect no reply ---- */

static void call_noreply(const char *iface, const char *method, const char *sig, ...) {
    sd_bus_message *m = NULL;
    if (sd_bus_message_new_method_call(bus, &m, NULL, CONSOLE_PATH, iface, method) < 0) return;
    va_list ap;
    va_start(ap, sig);
    int r = sd_bus_message_appendv(m, sig, ap);
    va_end(ap);
    if (r >= 0) r = sd_bus_message_set_expect_reply(m, 0);
    if (r >= 0) sd_bus_send(bus, m, NULL);
    sd_bus_message_unref(m);
}

static void key(int code, int down) {
    uint32_t q = vm_evdev_to_qnum(code);
    if (!q || code < 0 || code >= 768) return;
    if (!down && !D.keys[code]) return;
    D.keys[code] = (uint8_t)down;
    call_noreply("org.qemu.Display1.Keyboard", down ? "Press" : "Release", "u", q);
}

static void button(int b, int down) {
    if (b < 0 || b >= 16) return;
    if (!down && !D.buttons[b]) return;
    D.buttons[b] = (uint8_t)down;
    call_noreply("org.qemu.Display1.Mouse", down ? "Press" : "Release", "u", (uint32_t)b);
}

static void wheel(int steps, int up_button, int down_button) {
    int b = steps > 0 ? up_button : down_button;
    for (int i = 0; i < abs(steps); i++) {
        call_noreply("org.qemu.Display1.Mouse", "Press", "u", (uint32_t)b);
        call_noreply("org.qemu.Display1.Mouse", "Release", "u", (uint32_t)b);
    }
}

static void release_all(void) {
    for (int k = 0; k < 768; k++) if (D.keys[k]) key(k, 0);
    for (int b = 0; b < 16; b++) if (D.buttons[b]) button(b, 0);
}

/* Mouse.IsAbsolute changes when the guest's tablet driver starts: asked again, without waiting, at most once a second
 * while the pointer moves */
static long long abs_asked_ms;
static int on_abs(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int v = 0;
    if (!sd_bus_message_is_method_error(m, NULL) && sd_bus_message_read(m, "v", "b", &v) >= 0) D.abs_mouse = v;
    return 0;
}
static void ask_abs(long long now) {
    if (now - abs_asked_ms < 1000) return;
    abs_asked_ms = now;
    sd_bus_call_method_async(bus, NULL, NULL, CONSOLE_PATH, "org.freedesktop.DBus.Properties", "Get", on_abs, NULL, "ss", "org.qemu.Display1.Mouse", "IsAbsolute");
}

static void send_input(long long now) {
    if (D.motion_pending && D.w && D.h) {
        D.motion_pending = 0;
        ask_abs(now);
        int x = D.mx < (int)D.w ? D.mx : (int)D.w - 1, y = D.my < (int)D.h ? D.my : (int)D.h - 1;
        if (D.abs_mouse) call_noreply("org.qemu.Display1.Mouse", "SetAbsPosition", "uu", (uint32_t)x, (uint32_t)y);
        else call_noreply("org.qemu.Display1.Mouse", "RelMotion", "ii", x - D.last_mx, y - D.last_my);
        D.last_mx = x; D.last_my = y;
    }
    if (D.resize_pending && now - D.resize_ms >= RESIZE_GAP_MS) {
        D.resize_pending = 0;
        D.resize_ms = now;
        call_noreply("org.qemu.Display1.Console", "SetUIInfo", "qqiiuu", (uint16_t)0, (uint16_t)0, 0, 0, (uint32_t)D.rw, (uint32_t)D.rh);
    }
    if (D.ptr_pending) { D.ptr_pending = 0; say(vd_msg(VD_POINTER, D.ptr_x, D.ptr_y, D.ptr_on, 0, 0, 0), -1); }
}

/* ---- lord-server's messages ---- */

static void take_picture(int fd) {
    struct stat st;
    int seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0 || !(seals & F_SEAL_SHRINK) || fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return; }
    if (D.want_w <= 0 || D.want_h <= 0 || st.st_size != (off_t)D.want_w * D.want_h * 4) { close(fd); return; }   /* an answer to an older size */
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return;
    if (D.shm) munmap(D.shm, D.shm_len);
    D.shm = p;
    D.shm_len = (size_t)st.st_size;
    D.shm_w = (uint32_t)D.want_w;
    D.shm_h = (uint32_t)D.want_h;
    D.want_w = D.want_h = 0;
    D.acked = 1;
    if (D.w == D.shm_w && D.h == D.shm_h) damage(0, 0, (int)D.w, (int)D.h);
    else new_size(D.w, D.h);
}

/* 0 go on, 1 close */
static int from_server(void) {
    for (int budget = 0; budget < VD_POLL_MAX; budget++) {
        VdMsg m;
        int fd = -1;
        int r = vd_recv(CH, &m, &fd);
        if (r < 0) return 1;
        if (!r) return 0;
        if (!vd_msg_valid(&m, 1)) { if (fd >= 0) close(fd); refuse("message", "type %u from lord-server", m.type); continue; }
        if (m.type != VD_SHM && m.type != VD_AUDIO_REC && fd >= 0) { close(fd); fd = -1; }
        switch (m.type) {
        case VD_SHM: if (fd >= 0) take_picture(fd); break;
        case VD_AUDIO_REC: au_rec(m.a ? fd : -1); if (!m.a && fd >= 0) close(fd); break;
        case VD_FRAME_DONE: if (m.e == D.frame) D.acked = 1; break;
        case VD_KEY: key(m.a, m.b); break;
        case VD_MOTION: D.mx = m.a; D.my = m.b; D.motion_pending = 1; break;
        case VD_BUTTON: button(m.a, m.b); break;
        case VD_WHEEL: if (m.a) wheel(m.a, 3, 4); if (m.b) wheel(m.b, 8, 7); break;
        case VD_RELEASE_ALL: release_all(); break;
        case VD_RESIZE: D.rw = m.a; D.rh = m.b; D.resize_pending = 1; break;
        case VD_RATE: D.rate = m.a; break;
        case VD_CLOSE: return 1;
        }
    }
    return 0;
}

/* ---- setup ---- */

static sd_bus *p2p(int fd) {
    sd_bus *b = NULL;
    int r = sd_bus_new(&b);
    if (r >= 0) r = sd_bus_set_fd(b, fd, fd);
    if (r >= 0) r = sd_bus_negotiate_fds(b, 1);
    if (r >= 0) r = sd_bus_start(b);
    if (r < 0) { fprintf(stderr, "lord-vmdisplay: D-Bus setup: %s\n", strerror(-r)); sd_bus_unref(b); return NULL; }
    sd_bus_set_method_call_timeout(b, 5 * 1000 * 1000);
    return b;
}

static int bus_timeout_ms(sd_bus *b) {
    uint64_t usec;
    if (sd_bus_get_timeout(b, &usec) <= 0 || usec == UINT64_MAX) return -1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t n = (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
    return usec <= n ? 0 : (int)((usec - n + 999) / 1000);
}

static int is_socket(int fd, int type) {
    int t = 0;
    socklen_t l = sizeof t;
    return !getsockopt(fd, SOL_SOCKET, SO_TYPE, &t, &l) && t == type;
}

static int selftest(void) {
    SandboxHelperCfg cfg = { .name = NULL, .ro_paths = NULL, .extra_syscalls = (int[]){ -1 }, .allow_ioctl_classes = 0, .mem_max_mb = 0 };
    sandbox_apply_helper(&cfg);
    const char *home = getenv("HOME");
    char hp[512];
    snprintf(hp, sizeof hp, "%s/.profile", home ? home : "/root");
    int f_etc = open("/etc/hostname", O_RDONLY | O_CLOEXEC);
    int f_home = open(hp, O_RDONLY | O_CLOEXEC);
    int f_null = open("/dev/null", O_RDWR | O_CLOEXEC);
    int f_kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    int f_tmp = open("/tmp/lord-vmdisplay-selftest", O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    int s_inet = socket(AF_INET, SOCK_STREAM, 0), s_unix = socket(AF_UNIX, SOCK_STREAM, 0);
    int sp[2], s_pair = socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    pid_t child = fork();
    if (child == 0) _exit(0);
    int mfd = memfd_create("t", MFD_CLOEXEC);
    printf("{\"open_etc\":%s,\"open_home\":%s,\"open_dev_null\":%s,\"open_dev_kvm\":%s,\"create_tmp\":%s,\"socket_inet\":%s,\"socket_unix\":%s,\"socketpair\":%s,\"fork\":%s,\"memfd\":%s}\n",
           f_etc >= 0 ? "true" : "false", f_home >= 0 ? "true" : "false", f_null >= 0 ? "true" : "false", f_kvm >= 0 ? "true" : "false",
           f_tmp >= 0 ? "true" : "false", s_inet >= 0 ? "true" : "false", s_unix >= 0 ? "true" : "false", s_pair == 0 ? "true" : "false",
           child >= 0 ? "true" : "false", mfd >= 0 ? "true" : "false");
    fflush(stdout);
    return 0;
}

static int uuid_ok(const char *s) {
    if (!s || strlen(s) != 36) return 0;
    for (int i = 0; i < 36; i++) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return 0; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    prctl(PR_SET_DUMPABLE, 0);
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && !strcmp(argv[1], "--selftest")) return selftest();
    if (argc != 2 || !uuid_ok(argv[1])) { fprintf(stderr, "usage: lord-vmdisplay <machine id>   (fd 3: lord-server, fd 4: QEMU's D-Bus display)\n"); return 2; }
    if (!is_socket(CH, SOCK_SEQPACKET) || !is_socket(DBUS_FD, SOCK_STREAM)) { fprintf(stderr, "lord-vmdisplay: fd 3 must be a SOCK_SEQPACKET and fd 4 a stream socket\n"); return 2; }
    fcntl(CH, F_SETFD, FD_CLOEXEC);
    fcntl(DBUS_FD, F_SETFD, FD_CLOEXEC);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigbus;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGBUS, &sa, NULL);
    D.rate = 60;
    D.acked = 1;

    bus = p2p(DBUS_FD);
    if (!bus) { say(vd_msg(VD_STATE, VDS_LOST, VDR_SETUP, 0, 0, 0, 0), -1); return 1; }
    sd_bus_error e = SD_BUS_ERROR_NULL;
    uint32_t cw = 0, ch = 0;
    int absm = 0;
    char *label = NULL;
    if (sd_bus_get_property_string(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "Label", &e, &label) < 0) {
        fprintf(stderr, "lord-vmdisplay: QEMU's console isn't there: %s\n", e.message ? e.message : "?");
        say(vd_msg(VD_STATE, VDS_LOST, VDR_SETUP, 0, 0, 0, 0), -1);
        return 1;
    }
    sd_bus_error_free(&e);
    sd_bus_get_property_trivial(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "Width", &e, 'u', &cw);
    sd_bus_error_free(&e);
    sd_bus_get_property_trivial(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "Height", &e, 'u', &ch);
    sd_bus_error_free(&e);
    if (sd_bus_get_property_trivial(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Mouse", "IsAbsolute", &e, 'b', &absm) < 0) absm = 0;
    sd_bus_error_free(&e);
    D.abs_mouse = absm;
    int lv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, lv)) return 1;
    lbus = p2p(lv[0]);
    if (!lbus || sd_bus_add_object_vtable(lbus, NULL, LISTENER_PATH, "org.qemu.Display1.Listener", listener_vt, NULL) < 0 ||
        sd_bus_add_object_vtable(lbus, NULL, LISTENER_PATH, "org.qemu.Display1.Listener.Unix.Map", map_vt, NULL) < 0) return 1;
    sd_bus_message *rep = NULL;
    if (sd_bus_call_method(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "RegisterListener", &e, &rep, "h", lv[1]) < 0) {
        fprintf(stderr, "lord-vmdisplay: RegisterListener: %s\n", e.message ? e.message : "?");
        say(vd_msg(VD_STATE, VDS_LOST, VDR_SETUP, 0, 0, 0, 0), -1);
        return 1;
    }
    sd_bus_message_unref(rep);
    close(lv[1]);
    /* the guest's sound: a listener of its own, if QEMU has an audio backend on this display */
    int av[2];
    if (!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, av)) {
        abus = p2p(av[0]);
        rep = NULL;
        if (abus && sd_bus_add_object_vtable(abus, NULL, "/org/qemu/Display1/AudioOutListener", "org.qemu.Display1.AudioOutListener", audio_vt, NULL) >= 0 &&
            sd_bus_call_method(bus, NULL, "/org/qemu/Display1/Audio", "org.qemu.Display1.Audio", "RegisterOutListener", &e, &rep, "h", av[1]) >= 0)
            fprintf(stderr, "lord-vmdisplay %s: sound listener registered\n", argv[1]);
        else {
            fprintf(stderr, "lord-vmdisplay %s: no sound (%s)\n", argv[1], e.message ? e.message : "no listener");
            if (abus) { sd_bus_flush_close_unref(abus); abus = NULL; }
        }
        sd_bus_error_free(&e);
        sd_bus_message_unref(rep);
        close(av[1]);
    }
    fprintf(stderr, "lord-vmdisplay %s: console \"%s\" %ux%u, mouse %s\n", argv[1], label ? label : "", cw, ch, absm ? "absolute" : "relative");
    free(label);

    /* setup done: from here no files, no new sockets, no exec, threads only, in its own scope */
    char scope[80];
    snprintf(scope, sizeof scope, "lordos-vmdisplay-%s", argv[1]);
    SandboxHelperCfg cfg = { .name = scope, .ro_paths = NULL, .extra_syscalls = (int[]){ -1 }, .allow_ioctl_classes = 0, .mem_max_mb = 512 };
    sandbox_apply_helper(&cfg);
    say(vd_msg(VD_READY, (int)cw, (int)ch, absm, 0, 0, 0), -1);
    say(vd_msg(VD_STATE, VDS_CONNECTED, VDR_NONE, 0, 0, 0, 0), -1);

    D.stats_ms = now_ms();
    int rc = 0;
    for (;;) {
        int r;
        while ((r = sd_bus_process(lbus, NULL)) > 0) ;
        if (r < 0) { say(vd_msg(VD_STATE, VDS_LOST, VDR_QEMU_GONE, 0, 0, 0, 0), -1); break; }
        while ((r = sd_bus_process(bus, NULL)) > 0) ;
        if (r < 0) { say(vd_msg(VD_STATE, VDS_LOST, VDR_QEMU_GONE, 0, 0, 0, 0), -1); break; }
        if (abus) {
            while ((r = sd_bus_process(abus, NULL)) > 0) ;
            if (r < 0) { fprintf(stderr, "lord-vmdisplay: the sound connection went away\n"); sd_bus_flush_close_unref(abus); abus = NULL; }
        }
        long long now = now_ms();
        if (A.ring) au_tick(now);
        send_input(now);
        flush(now);
        if (D.stats_changed && now - D.stats_ms >= STATS_MS) {
            say(vd_msg(VD_STATS, D.refused, D.updates, D.copies, (int)D.w, (int)D.h, D.frame), -1);
            D.refused = D.updates = D.copies = 0;
            D.stats_changed = 0;
            D.stats_ms = now;
        }
        if (D.updates) D.stats_changed = 1;
        struct pollfd p[4] = {
            { sd_bus_get_fd(lbus), (short)sd_bus_get_events(lbus), 0 },
            { sd_bus_get_fd(bus), (short)sd_bus_get_events(bus), 0 },
            { CH, POLLIN, 0 },
            { abus ? sd_bus_get_fd(abus) : -1, abus ? (short)sd_bus_get_events(abus) : 0, 0 },
        };
        int to = bus_timeout_ms(lbus), t2 = bus_timeout_ms(bus);
        if (t2 >= 0 && (to < 0 || t2 < to)) to = t2;
        if (abus) { int t3 = bus_timeout_ms(abus); if (t3 >= 0 && (to < 0 || t3 < to)) to = t3; }
        if (A.ring && (to < 0 || to > 20)) to = 20;         /* silence keeps time while a recording runs */
        if (D.dirty && D.shm) {
            long long wake = D.acked ? D.next_ms : D.sent_ms + ACK_WAIT_MS;
            int tf = wake <= now ? 0 : (int)(wake - now);
            if (to < 0 || tf < to) to = tf;
        }
        if (D.resize_pending) { int tr = (int)(D.resize_ms + RESIZE_GAP_MS - now); if (tr < 0) tr = 0; if (to < 0 || tr < to) to = tr; }
        if (D.stats_changed) { int ts = (int)(D.stats_ms + STATS_MS - now); if (ts < 0) ts = 0; if (to < 0 || ts < to) to = ts; }
        if (poll(p, 4, to) < 0 && errno != EINTR) break;
        if (p[2].revents & (POLLHUP | POLLERR) && !(p[2].revents & POLLIN)) break;
        if (p[2].revents & POLLIN && from_server()) break;
    }
    release_all();
    sd_bus_process(bus, NULL);      /* pushes what the socket takes; never waits on QEMU */
    return rc;
}
