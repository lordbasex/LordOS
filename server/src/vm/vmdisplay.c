/* vmdisplay.c — lord-vmdisplay: one machine's picture and input (docs/VIRTUALIZATION.md 3.2 and "Phase 1
 * implementation"). It speaks QEMU's D-Bus display protocol peer to peer on fd 4 (a connection lord-vm had QEMU
 * accept), and passes lord-server only the fixed messages of vmproto.h on fd 3. The compositor never parses
 * anything QEMU sends.
 *
 *   lord-vmdisplay <machine id> [--heads N]
 *                                   fd 3: SOCK_SEQPACKET to lord-server; fd 4: the D-Bus connection to QEMU. --heads N
 *                                   (All Displays, docs/VIRTUALIZATION.md 5.4; 2..4): the virtio-gpu's outputs 0..N-1,
 *                                   QEMU's Console_0..Console_N-1, each checked to be the graphic console of that output,
 *                                   each with a listener of its own and its own channel to lord-server: output 0 on fd 3,
 *                                   output k on fd 4 + k. QEMU's peer-to-peer D-Bus display serves one connection (a new
 *                                   add_client takes its objects from the one before), so one helper serves every output.
 *                                   Only output 0's channel carries the guest's sound and microphone; each output is asked
 *                                   for its size at its corner of the desktop the outputs make (VD_LAYOUT). A closed
 *                                   channel of another output only stops that output; output 0's ends the helper
 *   lord-vmdisplay --selftest       applies the sandbox, then tries what it must not be able to do (JSON line)
 *
 * Decisions (Phase 0, section 13.6):
 *   - It requires org.qemu.Display1.Listener.Unix.Map: QEMU shares its picture once as a memfd and sends
 *     rectangles. Plain Scanout/Update arrays are still accepted and copied, because QEMU uses them for surfaces
 *     it can't share.
 *   - 3D (a machine with graphics "3d", QEMU's gl=on): the picture is a dmabuf QEMU's GPU context draws into
 *     (ScanoutDMABUF, single plane: Debian's 10.0 has no ScanoutDMABUF2). Its size, stride, fourcc and real length
 *     are checked, and the descriptor goes on to lord-server (VD_DMABUF), which reads it back with its own GPU
 *     context into the shared picture; this helper copies nothing. QEMU waits for the answer to each UpdateDMABUF
 *     before the guest draws again, so the answer waits until lord-server read that frame (VD_FRAME_DONE), or
 *     250 ms: the picture never tears, and a stalled lord-server never stalls the guest for long.
 *   - QEMU's memfd is sealed only against new seals, so QEMU can shrink it under our mapping: every read of it
 *     runs under a SIGBUS guard, and a fault drops that mapping (the picture waits for the next ScanoutMap).
 *     Refusing unsealed buffers would mean no picture at all with QEMU 10.0.
 *   - Copy before forward: the damaged area goes from QEMU's buffer into the picture lord-server shared (sealed
 *     by lord-server against shrinking), at most VD_RATE times a second, and not again until lord-server said
 *     it read the last one (VD_FRAME_DONE) or 250 ms passed. lord-server never sees QEMU's live memory.
 *   - Paced (VD_PACE, vmpace.h): lord-server says when a picture that keeps not changing (a vGPU's) or that no window
 *     shows may be taken less often; the copy, the VD_FRAME and a held UpdateDMABUF's answer then wait for the interval.
 *     Input to the guest ends an idle pace here at once.
 *   - It never makes a blocking call to QEMU after setup (QEMU calls the listener from its main loop): input goes
 *     out as method calls that expect no reply.
 *   - Sound (6.9, 6.20): an org.qemu.Display1.AudioOutListener on a third peer connection, registered before the
 *     sandbox. Only 16-bit little-endian PCM is taken (QEMU is told 48 kHz stereo s16, -audiodev in lordvm.c), with the
 *     guest's own mute and volume applied. It goes into lord-server's speakers' ring (VD_AUDIO_OUT), never more than
 *     VD_AUDIO_AHEAD_MS ahead of what lord-server read; and while a recording runs (VD_AUDIO_REC), mixed to one
 *     channel, into the ring lord-media reads, with silence for the time the guest plays nothing and nothing beyond
 *     200 ms ahead of the clock, so it keeps time with the picture.
 *   - Microphone (6.9): an org.qemu.Display1.AudioInListener on a fourth connection. QEMU's Read calls are answered at
 *     once, at the real rate: from lord-server's microphone ring (VD_MIC) when lord-server sent one, else silence. The
 *     helper never opens a device: the microphone is lord-server's stream, opened only after LordOS's permission. */
#include "vmproto.h"
#include "vmkeys.h"
#include "vmpace.h"
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

#define CH0 3
#define DBUS_FD 4

#define PIXMAN_x8r8g8b8 0x20020888u
#define PIXMAN_a8r8g8b8 0x20028888u
#define LISTENER_PATH "/org/qemu/Display1/Listener"
#define HEADS_MAX     4                 /* machine.h VM_HEADS_MAX */
#define ACK_WAIT_MS   250
#define RESIZE_GAP_MS 100
#define STATS_MS      5000

static sd_bus *bus, *abus, *ibus;

/* where the picture comes from */
enum { SRC_NONE = 0, SRC_MAP, SRC_FB, SRC_DMABUF };
/* one output of the display (one with a single display): everything below is per output. D is the one being served: set
 * before its listener's calls are processed, its channel is read, or its turn of the loop runs */
typedef struct {
    int ch;                             /* its channel to lord-server; -1 closed (another output's window went) */
    char path[40];                      /* QEMU's Console_<n> */
    sd_bus *lb;                         /* its listener's connection (lbus) */
    long long abs_asked_ms;
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
    int pace_ms, pace_kind;             /* VD_PACE: the least time between pictures, and why (vmpace.h) */
    /* cursor and pointer */
    int ptr_pending, ptr_x, ptr_y, ptr_on;
    /* input */
    int motion_pending, mx, my, last_mx, last_my;
    int abs_mouse;
    int layout[6];                      /* VD_LAYOUT: the shown display in the guest's desktop (0s: none) */
    /* "Capture mouse" (VD_CAPTURE, VD_REL): relative motion gathered until the next send, and until when IsAbsolute is
     * asked again sooner (lord-vm switches the guest's mouse at the same time) */
    int capture, rel_pending, rdx, rdy;
    long long abs_fast_until;
    uint8_t keys[768];
    uint8_t buttons[16];
    int resize_pending, rw, rh;
    long long resize_ms;
    /* counters */
    long long stats_ms;
    int refused, updates, copies, stats_changed;
    /* SRC_DMABUF: the UpdateDMABUF whose answer waits for lord-server (see the top), and since when */
    sd_bus_message *upd;
    long long upd_ms;
    int dmabufs;
    long upd_none;                      /* UpdateDMABUFs with no dmabuf picture to update */
} Head;
static Head H[HEADS_MAX];
static Head *Dp = &H[0];
static int nheads = 1;
#define D (*Dp)
#define CONSOLE_PATH (D.path)
#define lbus (D.lb)

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void say(VdMsg m, int fd) {
    if (D.ch < 0) return;
    if (vd_send(D.ch, &m, fd) && errno != EAGAIN) { /* lord-server gone: the loop sees the hangup */ }
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

/* how long QEMU's held UpdateDMABUFs waited for their answer, and the longest a turn of the loop kept every connection
 * waiting: QEMU gives each listener call 1 s (DBUS_DEFAULT_TIMEOUT) and then logs "Failed to call update: Timeout was
 * reached". Logged at the end (and a busy turn when it happens), so such a line can be told from one QEMU caused itself:
 * QEMU drops its own UpdateDMABUFs still queued when it sends a new ScanoutDMABUF or Disable (ui/dbus-listener.c,
 * ddl_discard_display_messages), and those calls never reach this helper */
static struct { long long held, slowest, busy_max, busy_told; } T;

/* QEMU's UpdateDMABUF waiting for its answer: given now */
static void answer_update(void) {
    if (!D.upd) return;
    long long waited = now_ms() - D.upd_ms;
    T.held++;
    if (waited > T.slowest) T.slowest = waited;
    sd_bus_reply_method_return(D.upd, NULL);
    D.upd = sd_bus_message_unref(D.upd);
}

/* the picture stops being a dmabuf: lord-server drops its copy of the descriptor (VD_DMABUF a = 0) */
static void drop_dmabuf(void) {
    answer_update();
    if (D.src != SRC_DMABUF) return;
    D.src = SRC_NONE;
    say(vd_msg(VD_DMABUF, 0, 0, 0, 0, 0, 0), -1);
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
#define AU_RATE 48000
/* one ring the helper maps: lord-server made it (sealed, its own size); the helper keeps its own cursor and size */
typedef struct {
    ARingHdr *h;
    size_t len;
    uint32_t size;
    uint64_t pos;                   /* write cursor (speakers, recording) or read cursor (microphone) */
} AuRing;
static struct {
    struct { uint64_t id; int used, ok, ch, on, mute, vol[2]; } st[AU_STREAMS];     /* the guest's output streams */
    struct { uint64_t id; int used, ok, ch, on, mute, vol; long long t0; uint64_t given; } in[AU_STREAMS];   /* its input streams */
    AuRing rec;                     /* a recording's ring (VD_AUDIO_REC, one channel), or none */
    AuRing out;                     /* the speakers' ring (VD_AUDIO_OUT, two channels), or none */
    AuRing mic;                     /* the microphone's ring (VD_MIC, one channel), or none */
    long long t0;                   /* when the recording started (CLOCK_MONOTONIC ms) */
    uint64_t samples;               /* written since t0 */
    long long writes, pads, dropped;
    long long out_bytes, out_dropped, mic_bytes, mic_silence;
    int out_on, in_on;              /* what lord-server was last told (VD_GUEST_AUDIO) */
} A;

static void ring_drop(AuRing *r) { if (r->h) munmap(r->h, r->len); memset(r, 0, sizeof *r); }
/* maps a ring lord-server sent: sealed against shrinking, its header as aring.h says, 48 kHz, `ch` channels */
static int ring_take(AuRing *r, int fd, uint32_t ch) {
    ring_drop(r);
    struct stat st;
    int seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0 || !(seals & F_SEAL_SHRINK) || fstat(fd, &st) || st.st_size < AR_HDR) { close(fd); return -1; }
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return -1;
    ARingHdr *h = map;
    uint32_t size = h->size;
    if (h->magic != AR_MAGIC || h->rate != AR_RATE || h->channels != ch || !size || (size & (size - 1)) || ar_bytes(size) > (size_t)st.st_size) {
        munmap(map, (size_t)st.st_size);
        return -1;
    }
    r->h = h; r->len = (size_t)st.st_size; r->size = size;
    r->pos = atomic_load(&h->wpos);
    if (ch == 1 && r == &A.mic) atomic_store(&h->rpos, r->pos);     /* the microphone: from now on, nothing older */
    return 0;
}

static int au_find(uint64_t id) { for (int i = 0; i < AU_STREAMS; i++) if (A.st[i].used && A.st[i].id == id) return i; return -1; }
static int in_find(uint64_t id) { for (int i = 0; i < AU_STREAMS; i++) if (A.in[i].used && A.in[i].id == id) return i; return -1; }

/* tells lord-server whether the guest plays or records anything now (the speakers' stream and the microphone follow) */
static void au_tell(void) {
    int out = 0, in = 0;
    for (int i = 0; i < AU_STREAMS; i++) { if (A.st[i].used && A.st[i].ok && A.st[i].on) out = 1; if (A.in[i].used && A.in[i].ok && A.in[i].on) in = 1; }
    if (out != A.out_on) { A.out_on = out; say(vd_msg(VD_GUEST_AUDIO, VDG_OUT, out, 0, 0, 0, 0), -1); }
    if (in != A.in_on) { A.in_on = in; say(vd_msg(VD_GUEST_AUDIO, VDG_IN, in, 0, 0, 0, 0), -1); }
}

static void au_put(const int16_t *pcm, size_t k) {
    if (A.rec.h) ar_write_t(A.rec.h, A.rec.size, &A.rec.pos, pcm, k * 2);
    A.samples += k;
}
/* silence up to `upto` samples since t0 */
static void au_pad(uint64_t upto) {
    static const int16_t zero[960];
    while (A.rec.h && A.samples < upto) {
        size_t k = upto - A.samples < 960 ? (size_t)(upto - A.samples) : 960;
        au_put(zero, k);
        A.pads++;
    }
}
static uint64_t au_expected(long long now) { return now > A.t0 ? (uint64_t)(now - A.t0) * 48 : 0; }
static void au_tick(long long now) {
    uint64_t want = au_expected(now);
    if (A.rec.h && A.samples + 2400 < want) au_pad(want - 960);         /* 50 ms behind: the guest is quiet */
}

/* the speakers: stereo frames into lord-server's ring, unless that would put it more than VD_AUDIO_AHEAD_MS ahead of
 * what lord-server read (then this piece is dropped: never a wait, never a growing delay) */
static void out_put(const int16_t *lr, size_t frames) {
    if (!A.out.h || !frames) return;
    size_t n = frames * 4;
    if (!vd_audio_fits(atomic_load_explicit(&A.out.h->rpos, memory_order_acquire), A.out.pos, A.out.size, n)) { A.out_dropped += (long long)n; return; }
    A.out_bytes += (long long)ar_write_t(A.out.h, A.out.size, &A.out.pos, lr, n);
}

static int on_au_init(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id; uint8_t bits, nch; int sgn, flt, be; uint32_t freq, bpf, bps;
    if (sd_bus_message_read(m, "tybbuyuub", &id, &bits, &sgn, &flt, &freq, &nch, &bpf, &bps, &be) < 0) return 0;
    int i = au_find(id);
    for (int k = 0; i < 0 && k < AU_STREAMS; k++) if (!A.st[k].used) i = k;
    if (i < 0) return 0;
    A.st[i].used = 1; A.st[i].id = id; A.st[i].on = 0; A.st[i].mute = 0; A.st[i].vol[0] = A.st[i].vol[1] = 255;
    A.st[i].ok = bits == 16 && sgn && !flt && !be && freq == AU_RATE && (nch == 1 || nch == 2) && bpf == (uint32_t)nch * 2;
    A.st[i].ch = nch;
    if (!A.st[i].ok) fprintf(stderr, "lord-vmdisplay: sound stream %llu isn't 48 kHz 16-bit (%u bits, %u Hz, %u channels): not used\n", (unsigned long long)id, bits, freq, nch);
    return sd_bus_reply_method_return(m, "");
}
static int on_au_fini(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id;
    if (sd_bus_message_read(m, "t", &id) >= 0) { int i = au_find(id); if (i >= 0) A.st[i].used = 0; au_tell(); }
    return sd_bus_reply_method_return(m, "");
}
static int on_au_enabled(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id; int on;
    if (sd_bus_message_read(m, "tb", &id, &on) >= 0) { int i = au_find(id); if (i >= 0) A.st[i].on = on; au_tell(); }
    return sd_bus_reply_method_return(m, "");
}
/* the guest's own mute and volume (its mixer): QEMU leaves them to the listener, 0..255 per channel, linear as QEMU's
 * own mixer applies them */
static int on_au_volume(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id; int mute;
    const void *vol = NULL;
    size_t nv = 0;
    if (sd_bus_message_read(m, "tb", &id, &mute) >= 0) {
        int i = au_find(id);
        if (i >= 0) {
            A.st[i].mute = mute;
            if (sd_bus_message_read_array(m, 'y', &vol, &nv) >= 0 && nv >= 1) {
                const uint8_t *v = vol;
                A.st[i].vol[0] = v[0];
                A.st[i].vol[1] = nv >= 2 ? v[1] : v[0];
            }
        }
    }
    return sd_bus_reply_method_return(m, "");
}
static int on_au_write(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id;
    const void *data = NULL;
    size_t n = 0;
    if (sd_bus_message_read(m, "t", &id) < 0 || sd_bus_message_read_array(m, 'y', &data, &n) < 0) return 0;
    int i = au_find(id);
    if (i < 0 || !A.st[i].ok || (!A.rec.h && !A.out.h)) return sd_bus_reply_method_return(m, "");
    int ch = A.st[i].ch, vl = A.st[i].mute ? 0 : A.st[i].vol[0], vr = A.st[i].mute ? 0 : A.st[i].vol[1];
    size_t frames = n / ((size_t)ch * 2);
    long long now = now_ms();
    int rec = !!A.rec.h;
    if (rec) {
        uint64_t want = au_expected(now);
        if (A.samples > want + 9600) { A.dropped += (long long)frames; rec = 0; }      /* 200 ms ahead */
        else au_tick(now);
    }
    static int16_t mono[2048], lr[4096];
    const uint8_t *p = data;
    for (size_t f = 0; f < frames;) {
        size_t k = frames - f < 2048 ? frames - f : 2048;
        for (size_t j = 0; j < k; j++, f++) {
            int16_t l, r;
            memcpy(&l, p + f * (size_t)ch * 2, 2);
            if (ch == 2) memcpy(&r, p + f * 4 + 2, 2); else r = l;
            int sl = l * vl / 255, sr = r * vr / 255;
            lr[j * 2] = (int16_t)sl;
            lr[j * 2 + 1] = (int16_t)sr;
            mono[j] = (int16_t)((sl + sr) / 2);
        }
        if (rec) au_put(mono, k);
        out_put(lr, k);
    }
    A.writes++;
    return sd_bus_reply_method_return(m, "");
}
/* QEMU reads the listener's Interfaces (no extra ones here: PCM goes as Write and Read calls) while it registers it */
static int au_interfaces(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *u, sd_bus_error *e) {
    (void)b; (void)path; (void)iface; (void)prop; (void)u; (void)e;
    return sd_bus_message_append_strv(reply, NULL);
}
static const sd_bus_vtable audio_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Interfaces", "as", au_interfaces, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Init", "tybbuyuub", "", on_au_init, 0),
    SD_BUS_METHOD("Fini", "t", "", on_au_fini, 0),
    SD_BUS_METHOD("SetEnabled", "tb", "", on_au_enabled, 0),
    SD_BUS_METHOD("SetVolume", "tbay", "", on_au_volume, 0),
    SD_BUS_METHOD("Write", "tay", "", on_au_write, 0),
    SD_BUS_VTABLE_END
};

/* ---- the guest's microphone (AudioInListener) ----
 * QEMU asks with Read from its main loop and waits for the answer, so it's answered at once: the frames the time since
 * the stream started is owed (never more than asked), from lord-server's microphone ring when there is one (VD_MIC),
 * else silence. A guest that records never stalls, and hears nothing of this computer without the ring. */
static int on_in_init(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id; uint8_t bits, nch; int sgn, flt, be; uint32_t freq, bpf, bps;
    if (sd_bus_message_read(m, "tybbuyuub", &id, &bits, &sgn, &flt, &freq, &nch, &bpf, &bps, &be) < 0) return 0;
    int i = in_find(id);
    for (int k = 0; i < 0 && k < AU_STREAMS; k++) if (!A.in[k].used) i = k;
    if (i < 0) return 0;
    A.in[i].used = 1; A.in[i].id = id; A.in[i].on = 0; A.in[i].mute = 0; A.in[i].vol = 255;
    A.in[i].ok = bits == 16 && sgn && !flt && !be && freq == AU_RATE && (nch == 1 || nch == 2) && bpf == (uint32_t)nch * 2;
    A.in[i].ch = nch;
    if (!A.in[i].ok) fprintf(stderr, "lord-vmdisplay: input stream %llu isn't 48 kHz 16-bit (%u bits, %u Hz, %u channels): silence\n", (unsigned long long)id, bits, freq, nch);
    return sd_bus_reply_method_return(m, "");
}
static int on_in_fini(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id;
    if (sd_bus_message_read(m, "t", &id) >= 0) { int i = in_find(id); if (i >= 0) A.in[i].used = 0; au_tell(); }
    return sd_bus_reply_method_return(m, "");
}
static int on_in_enabled(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id; int on;
    if (sd_bus_message_read(m, "tb", &id, &on) >= 0) {
        int i = in_find(id);
        if (i >= 0) { A.in[i].on = on; A.in[i].t0 = now_ms(); A.in[i].given = 0; }
        au_tell();
    }
    return sd_bus_reply_method_return(m, "");
}
/* the guest's capture mute and level (its mixer), applied here as for the speakers: a guest that muted its microphone
 * hears silence */
static int on_in_volume(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id; int mute;
    const void *vol = NULL;
    size_t nv = 0;
    if (sd_bus_message_read(m, "tb", &id, &mute) >= 0) {
        int i = in_find(id);
        if (i >= 0) {
            A.in[i].mute = mute;
            if (sd_bus_message_read_array(m, 'y', &vol, &nv) >= 0 && nv >= 1) A.in[i].vol = ((const uint8_t *)vol)[0];
        }
    }
    return sd_bus_reply_method_return(m, "");
}
static int on_in_read(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    uint64_t id, size;
    if (sd_bus_message_read(m, "tt", &id, &size) < 0) return 0;
    int i = in_find(id);
    static int16_t buf[2 * 4800];               /* 100 ms of stereo at most per answer */
    size_t frames = 0;
    if (i >= 0 && A.in[i].ok) {
        long long now = now_ms();
        if (!A.in[i].t0) A.in[i].t0 = now;
        size_t fb = (size_t)A.in[i].ch * 2, room = size / fb > 4800 ? 4800 : (size_t)(size / fb);
        frames = vd_mic_owed(A.in[i].t0, now, &A.in[i].given, room);
        static int16_t mono[4800];
        size_t got = 0;
        if (A.mic.h && frames) {
            /* more than 100 ms waiting in the ring: the oldest is skipped, so the guest hears the microphone without delay */
            uint64_t avail = ar_avail_t(A.mic.h, A.mic.size, A.mic.pos);
            if (avail > (uint64_t)(frames + 4800) * 2) { A.mic.pos += (avail - (uint64_t)frames * 2) & ~(uint64_t)1; atomic_store(&A.mic.h->rpos, A.mic.pos); }
            got = ar_read_t(A.mic.h, A.mic.size, &A.mic.pos, mono, frames * 2) / 2;
            A.mic_bytes += (long long)got * 2;
        }
        if (got < frames) { memset(mono + got, 0, (frames - got) * 2); A.mic_silence += (long long)(frames - got); }
        int gain = A.in[i].mute ? 0 : A.in[i].vol;
        if (gain != 255) for (size_t k = 0; k < got; k++) mono[k] = (int16_t)(mono[k] * gain / 255);
        if (A.in[i].ch == 2) for (size_t k = 0; k < frames; k++) buf[k * 2] = buf[k * 2 + 1] = mono[k];
        else memcpy(buf, mono, frames * 2);
        A.in[i].given += frames;
        frames *= fb / 2;                       /* in samples of int16 */
    }
    sd_bus_message *r = NULL;
    int rc = sd_bus_message_new_method_return(m, &r);
    if (rc >= 0) rc = sd_bus_message_append_array(r, 'y', buf, frames * 2);
    if (rc >= 0) rc = sd_bus_send(NULL, r, NULL);
    sd_bus_message_unref(r);
    return rc;
}
static const sd_bus_vtable audio_in_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Interfaces", "as", au_interfaces, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Init", "tybbuyuub", "", on_in_init, 0),
    SD_BUS_METHOD("Fini", "t", "", on_in_fini, 0),
    SD_BUS_METHOD("SetEnabled", "tb", "", on_in_enabled, 0),
    SD_BUS_METHOD("SetVolume", "tbay", "", on_in_volume, 0),
    SD_BUS_METHOD("Read", "tt", "ay", on_in_read, 0),
    SD_BUS_VTABLE_END
};

/* RegisterOutListener's and RegisterInListener's answer: 1 taken, 2 refused */
static int au_registered(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)e;
    *(int *)u = sd_bus_message_is_method_error(m, NULL) ? 2 : 1;
    if (*(int *)u == 2) fprintf(stderr, "lord-vmdisplay: Register*Listener: %s\n", sd_bus_message_get_error(m) ? sd_bus_message_get_error(m)->message : "?");
    return 0;
}

static void au_counts(const char *why) {
    if (A.writes || A.pads || A.out_bytes || A.out_dropped || A.mic_bytes)
        fprintf(stderr, "lord-vmdisplay: sound (%s): %lld writes from QEMU; recording: %lld silences, %lld dropped; %lld bytes to the speakers, "
                "%lld dropped to keep the delay; %lld bytes from the microphone, %lld frames of silence for it\n",
                why, A.writes, A.pads, A.dropped, A.out_bytes, A.out_dropped, A.mic_bytes, A.mic_silence);
}

/* VD_AUDIO_REC: the recording's ring (checked: sealed against shrinking, the size it says, 48 kHz, one channel) */
static void au_rec(int fd) {
    au_counts("recording");
    A.writes = A.pads = A.dropped = 0;
    ring_drop(&A.rec);
    if (fd < 0 || ring_take(&A.rec, fd, 1)) return;
    A.t0 = now_ms();
    A.samples = 0;
}

/* a new picture size: ask lord-server for a picture of it; the old one isn't written again */
static const char *me = "";             /* the machine's id, for the notes in the session's log */
static void new_size(uint32_t w, uint32_t h) {
    /* the guest's own size (what follows a VD_RESIZE when its display driver does): a note when it changes, so a
     * request that isn't followed can be told from one that never went out */
    if (w != D.w || h != D.h) fprintf(stderr, "lord-vmdisplay %s: the guest's picture is %ux%u\n", me, w, h);
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
    else return 0;          /* a dmabuf: lord-server reads it itself */
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
    D.next_ms = now + vmpace_interval(D.rate, D.pace_ms);
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
    drop_dmabuf();
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

/* the formats a dmabuf picture may have: 32-bit RGB, one plane (QEMU's GL scanouts) */
static bool dmabuf_fmt_ok(uint32_t f) { return f == VD_FOURCC_XRGB8888 || f == VD_FOURCC_ARGB8888 || f == VD_FOURCC_XBGR8888 || f == VD_FOURCC_ABGR8888; }

static int on_scanout_dmabuf(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int fd;
    uint32_t w, h, stride, fourcc;
    uint64_t mod;
    int y0_top;
    if (sd_bus_message_read(m, "huuuutb", &fd, &w, &h, &stride, &fourcc, &mod, &y0_top) < 0) { refuse("ScanoutDMABUF", "malformed"); return ok(m); }
    D.updates++;
    if (!w || !h || w > VD_MAX_DIM || h > VD_MAX_DIM || (uint64_t)w * h * 4 > VD_MAX_BYTES) { refuse("ScanoutDMABUF", "size %ux%u out of limits", w, h); return ok(m); }
    if (!dmabuf_fmt_ok(fourcc)) { refuse("ScanoutDMABUF", "fourcc 0x%08x", fourcc); return ok(m); }
    if ((uint64_t)stride < (uint64_t)w * 4 || stride % 4 || stride > VD_DMABUF_STRIDE_MAX) { refuse("ScanoutDMABUF", "stride %u for width %u", stride, w); return ok(m); }
    /* its real length: never shorter than what it says it holds (a tiled buffer may be longer) */
    off_t real = lseek(fd, 0, SEEK_END);
    uint64_t bytes = (uint64_t)stride * h;
    if (real < 0 || (uint64_t)real < bytes || (uint64_t)real > (uint64_t)VD_MAX_BYTES * 2) {
        refuse("ScanoutDMABUF", "%lld bytes against stride × height %" PRIu64, (long long)real, bytes);
        return ok(m);
    }
    drop_map();
    drop_fb();
    answer_update();
    D.src = SRC_DMABUF;
    if (!D.dmabufs++) fprintf(stderr, "lord-vmdisplay: QEMU's 3D picture: %ux%u dmabuf, fourcc 0x%08x, modifier 0x%" PRIx64 ", %lld bytes, %s\n",
                              w, h, fourcc, mod, (long long)real, y0_top ? "bottom row first" : "top row first");
    /* sd-bus keeps its descriptor and closes it with the message; lord-server gets its own. QEMU's y0_top is in GL's
     * terms (its own windows blit with a flip when it's false): set, the first row in memory is the picture's bottom
     * (measured 2026-11-20, virgl on amd64: false, rows top first) */
    say(vd_msg(VD_DMABUF, (int)w, (int)h, (int)stride | (y0_top ? VD_DMABUF_FLIP : 0), (int32_t)fourcc, (int32_t)(uint32_t)mod, (int32_t)(uint32_t)(mod >> 32)), fd);
    new_size(w, h);
    return ok(m);
}

/* the damage, and QEMU's answer held until lord-server read the frame (the top of this file) */
static int on_update_dmabuf(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int32_t x, y, w, h;
    if (sd_bus_message_read(m, "iiii", &x, &y, &w, &h) < 0) { refuse("UpdateDMABUF", "malformed"); return ok(m); }
    D.updates++;
    /* QEMU's vfio display (a GVT-g vGPU) sends one every refresh, also before its first ScanoutDMABUF and after the
     * guest turned its plane off: nothing to show, the last picture stays; answered, counted, never logged */
    if (D.src != SRC_DMABUF) { D.upd_none++; return ok(m); }
    if (!rect_ok("UpdateDMABUF", x, y, w, h)) return ok(m);
    damage(x, y, w, h);
    answer_update();
    D.upd = sd_bus_message_ref(m);
    D.upd_ms = now_ms();
    return 1;
}

static int on_disable(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    drop_map();
    drop_fb();
    drop_dmabuf();
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
    /* a hotspot QEMU doesn't know: a vGPU's cursor plane (vfio display) has none, and VFIO says so with 0xffffffff
     * (-1 here). Its image is drawn from its top left corner, where Windows' arrow has its tip: 0, 0. Refused, such a
     * machine would have no pointer of its own (2026-11-22) */
    if (hx < 0 || hy < 0 || hx >= w || hy >= h) { hx = 0; hy = 0; }
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
    drop_dmabuf();
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

static int on_ui_info(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    const sd_bus_error *err = sd_bus_message_get_error(m);
    if (err) fprintf(stderr, "lord-vmdisplay %s: QEMU refused the new size: %s\n", me, err->message ? err->message : err->name ? err->name : "?");
    return 0;
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
static int on_abs(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)e;
    Head *h = u;                        /* the output that asked */
    int v = 0;
    if (h && !sd_bus_message_is_method_error(m, NULL) && sd_bus_message_read(m, "v", "b", &v) >= 0) h->abs_mouse = v;
    return 0;
}
static void ask_abs(long long now) {
    if (now - D.abs_asked_ms < (now < D.abs_fast_until ? 100 : 1000)) return;
    D.abs_asked_ms = now;
    sd_bus_call_method_async(bus, NULL, NULL, CONSOLE_PATH, "org.freedesktop.DBus.Properties", "Get", on_abs, Dp, "ss", "org.qemu.Display1.Mouse", "IsAbsolute");
}

/* a picture point for the guest's tablet: onto the shown display of its whole desktop (VD_LAYOUT), or as it is */
static int abs_x(int x) { return vd_abs_map(x, (int)D.w, D.layout[0], D.layout[2], D.layout[4]); }
static int abs_y(int y) { return vd_abs_map(y, (int)D.h, D.layout[1], D.layout[3], D.layout[5]); }

/* the pointer's motion gathered since the last send: sent before any button or wheel step that follows it, so a click
 * lands where the pointer is. The machine's window sends a click as VD_MOTION then VD_BUTTON in one burst; with the
 * button sent at once and the motion at the end of the turn, the guest's tablet pressed at the previous position and
 * moved with the button down, a short drag, and Windows' shell opens no menu for a right-button drag (2026-11-22,
 * tests/vmdisplay_test.c) */
static void send_motion(long long now) {
    if (D.rel_pending && D.w && D.h) {
        D.rel_pending = 0;
        ask_abs(now);
        if (!D.abs_mouse) call_noreply("org.qemu.Display1.Mouse", "RelMotion", "ii", D.rdx, D.rdy);
        else {
            /* the guest's mouse is still the tablet (lord-vm's switch on its way, or no relative mouse there): the
             * pointer moves by the motion inside the picture, as it would on a desktop */
            int x = D.last_mx + D.rdx, y = D.last_my + D.rdy;
            x = x < 0 ? 0 : x >= (int)D.w ? (int)D.w - 1 : x;
            y = y < 0 ? 0 : y >= (int)D.h ? (int)D.h - 1 : y;
            call_noreply("org.qemu.Display1.Mouse", "SetAbsPosition", "uu", (uint32_t)abs_x(x), (uint32_t)abs_y(y));
            D.last_mx = x; D.last_my = y;
        }
        D.rdx = D.rdy = 0;
    }
    if (D.motion_pending && D.capture) D.motion_pending = 0;     /* captured: only relative motion moves it */
    if (D.motion_pending && D.w && D.h) {
        D.motion_pending = 0;
        ask_abs(now);
        int x = D.mx < (int)D.w ? D.mx : (int)D.w - 1, y = D.my < (int)D.h ? D.my : (int)D.h - 1;
        if (D.abs_mouse) call_noreply("org.qemu.Display1.Mouse", "SetAbsPosition", "uu", (uint32_t)abs_x(x), (uint32_t)abs_y(y));
        else call_noreply("org.qemu.Display1.Mouse", "RelMotion", "ii", x - D.last_mx, y - D.last_my);
        D.last_mx = x; D.last_my = y;
    }
}

static void send_input(long long now) {
    /* just captured or released: the mouse QEMU uses is asked again without waiting for a move (lord-vm switches it at the
     * same time), so the first move after it goes the right way */
    if (now < D.abs_fast_until) ask_abs(now);
    send_motion(now);
    if (D.resize_pending && now - D.resize_ms >= RESIZE_GAP_MS) {
        D.resize_pending = 0;
        D.resize_ms = now;
        /* with an answer: QEMU says so when the display device can't be asked (a refusal used to go unseen) */
        fprintf(stderr, "lord-vmdisplay %s: the guest is asked for %dx%d\n", me, D.rw, D.rh);
        /* one of several heads: at its corner in the desktop the heads make side by side (VD_LAYOUT, vm_layout_ok), which
         * QEMU hands the guest as the output's suggested position */
        int xo = nheads > 1 && D.layout[4] ? D.layout[0] : 0, yo = nheads > 1 && D.layout[4] ? D.layout[1] : 0;
        sd_bus_call_method_async(bus, NULL, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "SetUIInfo", on_ui_info, NULL, "qqiiuu",
                                 (uint16_t)0, (uint16_t)0, xo, yo, (uint32_t)D.rw, (uint32_t)D.rh);
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
/* VD_PACE (vmpace.h): a shorter interval applies to the picture waiting now */
static void pace_set(int ms, int kind) {
    D.pace_ms = ms;
    D.pace_kind = kind;
    long long due = D.sent_ms + vmpace_interval(D.rate, ms);
    if (due < D.next_ms) D.next_ms = due;
}

/* input to the guest ends an idle pace here and now: the picture it makes goes out at the display's rate */
static void pace_input(void) {
    if (D.pace_kind == VDP_IDLE) pace_set(0, VDP_NONE);
}

static int from_server(void) {
    for (int budget = 0; budget < VD_POLL_MAX; budget++) {
        VdMsg m;
        int fd = -1;
        int r = D.ch < 0 ? -1 : vd_recv(D.ch, &m, &fd);
        if (r < 0) return 1;
        if (!r) return 0;
        if (!vd_msg_valid(&m, 1)) { if (fd >= 0) close(fd); refuse("message", "type %u from lord-server", m.type); continue; }
        if (m.type != VD_SHM && m.type != VD_AUDIO_REC && m.type != VD_AUDIO_OUT && m.type != VD_MIC && fd >= 0) { close(fd); fd = -1; }
        /* the guest's sound and microphone are output 0's channel's only */
        if (Dp != &H[0] && (m.type == VD_AUDIO_REC || m.type == VD_AUDIO_OUT || m.type == VD_MIC)) { if (fd >= 0) close(fd); refuse("message", "sound on another output's channel"); continue; }
        switch (m.type) {
        case VD_SHM: if (fd >= 0) take_picture(fd); break;
        case VD_AUDIO_REC: au_rec(m.a ? fd : -1); if (!m.a && fd >= 0) close(fd); break;
        case VD_AUDIO_OUT:
            /* the speakers' ring (two channels), or none: the guest's sound isn't heard */
            if (m.a && fd >= 0) { if (ring_take(&A.out, fd, 2)) refuse("VD_AUDIO_OUT", "not a sealed 48 kHz stereo ring"); }
            else { if (fd >= 0) close(fd); au_counts("speakers off"); ring_drop(&A.out); }
            break;
        case VD_MIC:
            /* the microphone's ring (one channel), or none: the guest hears silence */
            if (m.a && fd >= 0) { if (ring_take(&A.mic, fd, 1)) refuse("VD_MIC", "not a sealed 48 kHz mono ring"); }
            else { if (fd >= 0) close(fd); ring_drop(&A.mic); }
            break;
        case VD_FRAME_DONE: if (m.e == D.frame) D.acked = 1; break;
        case VD_KEY: pace_input(); key(m.a, m.b); break;
        case VD_MOTION: pace_input(); D.mx = m.a; D.my = m.b; D.motion_pending = 1; break;
        case VD_BUTTON: pace_input(); send_motion(now_ms()); button(m.a, m.b); break;
        case VD_WHEEL: pace_input(); send_motion(now_ms()); if (m.a) wheel(m.a, 3, 4); if (m.b) wheel(m.b, 8, 7); break;
        case VD_REL:
            if (!D.capture) break;
            pace_input();
            D.rdx = D.rdx + m.a > 65535 ? 65535 : D.rdx + m.a < -65535 ? -65535 : D.rdx + m.a;
            D.rdy = D.rdy + m.b > 65535 ? 65535 : D.rdy + m.b < -65535 ? -65535 : D.rdy + m.b;
            D.rel_pending = 1;
            break;
        case VD_CAPTURE:
            D.capture = m.a;
            D.rdx = D.rdy = 0; D.rel_pending = 0;
            D.abs_fast_until = now_ms() + 3000;
            D.abs_asked_ms = 0;
            fprintf(stderr, "lord-vmdisplay %s: the mouse is %s\n", me, m.a ? "captured (relative)" : "free (absolute)");
            break;
        case VD_LAYOUT:
            memcpy(D.layout, (int[6]){ m.a, m.b, m.c, m.d, m.e, m.f }, sizeof D.layout);
            if (m.e) fprintf(stderr, "lord-vmdisplay %s: the tablet points at %dx%d at %d,%d of the guest's %dx%d desktop\n", me, m.c, m.d, m.a, m.b, m.e, m.f);
            else fprintf(stderr, "lord-vmdisplay %s: the tablet covers the picture\n", me);
            break;
        case VD_RELEASE_ALL: release_all(); break;
        case VD_RESIZE: D.rw = m.a; D.rh = m.b; D.resize_pending = 1; break;
        case VD_RATE: D.rate = m.a; break;
        case VD_PACE: pace_set(m.a, m.b); break;
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

/* A sound listener (out: the guest's sound; in: its microphone) on a new peer connection. QEMU asks the new listener
 * for its properties before it answers Register*Listener: the call is made without waiting, and every connection is
 * served until the answer comes (5 s at most). NULL: no such listener (no audio backend, or QEMU refused it). */
static sd_bus *register_audio(const char *id, const char *method, const char *path, const char *iface, const sd_bus_vtable *vt) {
    int av[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, av)) return NULL;
    sd_bus *b = p2p(av[0]);
    static int done;
    done = 0;
    sd_bus_slot *slot = NULL;
    if (b && sd_bus_add_object_vtable(b, NULL, path, iface, vt, NULL) >= 0 &&
        sd_bus_call_method_async(bus, &slot, NULL, "/org/qemu/Display1/Audio", "org.qemu.Display1.Audio", method, au_registered, &done, "h", av[1]) >= 0) {
        close(av[1]);
        av[1] = -1;
        long long until = now_ms() + 5000;
        /* the console's listener and the other sound one too: QEMU asks them for their properties and answers nothing
         * else until it has them */
        while (!done && now_ms() < until) {
            sd_bus *all[3 + HEADS_MAX] = { b, bus, abus };
            int na = 3;
            for (int k = 0; k < nheads; k++) all[na++] = H[k].lb;
            for (int k = 0; k < na; k++) if (all[k]) { if (k >= 3) Dp = &H[k - 3]; while (sd_bus_process(all[k], NULL) > 0) ; }
            Dp = &H[0];
            if (done) break;
            struct pollfd pf[3 + HEADS_MAX];
            int np = 0;
            for (int k = 0; k < na; k++) if (all[k]) pf[np++] = (struct pollfd){ sd_bus_get_fd(all[k]), (short)sd_bus_get_events(all[k]), 0 };
            poll(pf, (nfds_t)np, 100);
        }
    }
    sd_bus_slot_unref(slot);
    if (av[1] >= 0) close(av[1]);
    if (done == 1) { fprintf(stderr, "lord-vmdisplay %s: %s: listener registered\n", id, method); return b; }
    fprintf(stderr, "lord-vmdisplay %s: %s: none (%s)\n", id, method, done == 2 ? "QEMU refused the listener" : b ? "no answer" : "no connection");
    if (b) sd_bus_flush_close_unref(b);
    return NULL;
}

int main(int argc, char **argv) {
    prctl(PR_SET_DUMPABLE, 0);
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && !strcmp(argv[1], "--selftest")) return selftest();
    if (argc == 4 && !strcmp(argv[2], "--heads")) nheads = vd_head_arg(argv[3], HEADS_MAX + 1);
    if ((argc != 2 && argc != 4) || !uuid_ok(argv[1]) || (argc == 4 && nheads < 2)) {
        fprintf(stderr, "usage: lord-vmdisplay <machine id> [--heads 2-%d]   (fd 3: lord-server, fd 4: QEMU's D-Bus display, fd 4+k: output k's channel)\n", HEADS_MAX);
        return 2;
    }
    me = argv[1];
    for (int k = 0; k < nheads; k++) {
        H[k].ch = k ? DBUS_FD + k : CH0;
        snprintf(H[k].path, sizeof H[k].path, "/org/qemu/Display1/Console_%d", k);
        H[k].rate = 60;
        H[k].acked = 1;
        if (!is_socket(H[k].ch, SOCK_SEQPACKET)) { fprintf(stderr, "lord-vmdisplay: fd %d must be a SOCK_SEQPACKET\n", H[k].ch); return 2; }
        fcntl(H[k].ch, F_SETFD, FD_CLOEXEC);
    }
    if (!is_socket(DBUS_FD, SOCK_STREAM)) { fprintf(stderr, "lord-vmdisplay: fd 4 must be a stream socket\n"); return 2; }
    fcntl(DBUS_FD, F_SETFD, FD_CLOEXEC);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigbus;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGBUS, &sa, NULL);

    bus = p2p(DBUS_FD);
    if (!bus) { for (int k = 0; k < nheads; k++) { Dp = &H[k]; say(vd_msg(VD_STATE, VDS_LOST, VDR_SETUP, 0, 0, 0, 0), -1); } return 1; }
    sd_bus_error e = SD_BUS_ERROR_NULL;
    uint32_t cws[HEADS_MAX] = { 0 }, chs[HEADS_MAX] = { 0 };
    for (int k = 0; k < nheads; k++) {
        Dp = &H[k];
        uint32_t cw = 0, ch = 0;
        int absm = 0;
        char *label = NULL;
        int good = sd_bus_get_property_string(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "Label", &e, &label) >= 0;
        if (!good) fprintf(stderr, "lord-vmdisplay: QEMU's console %d isn't there: %s\n", k, e.message ? e.message : "?");
        sd_bus_error_free(&e);
        if (good && nheads > 1) {
            /* QEMU numbers its consoles itself: Console_k must be the graphic console of output k (the virtio-gpu's outputs
             * come first: one display device, -nodefaults) */
            uint32_t hh = 0xffffffffu;
            char *type = NULL;
            sd_bus_get_property_trivial(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "Head", &e, 'u', &hh);
            sd_bus_error_free(&e);
            sd_bus_get_property_string(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "Type", &e, &type);
            sd_bus_error_free(&e);
            good = hh == (uint32_t)k && type && !strcmp(type, "Graphic");
            if (!good) fprintf(stderr, "lord-vmdisplay %s: console %d isn't the display's output %d\n", argv[1], k, k);
            free(type);
        }
        if (!good) {
            free(label);
            for (int j = 0; j < nheads; j++) { Dp = &H[j]; say(vd_msg(VD_STATE, VDS_LOST, VDR_SETUP, 0, 0, 0, 0), -1); }
            return 1;
        }
        sd_bus_get_property_trivial(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "Width", &e, 'u', &cw);
        sd_bus_error_free(&e);
        sd_bus_get_property_trivial(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "Height", &e, 'u', &ch);
        sd_bus_error_free(&e);
        if (sd_bus_get_property_trivial(bus, NULL, CONSOLE_PATH, "org.qemu.Display1.Mouse", "IsAbsolute", &e, 'b', &absm) < 0) absm = 0;
        sd_bus_error_free(&e);
        D.abs_mouse = absm;
        cws[k] = cw; chs[k] = ch;
        fprintf(stderr, "lord-vmdisplay %s: console %d \"%s\" %ux%u, mouse %s\n", argv[1], k, label ? label : "", cw, ch, absm ? "absolute" : "relative");
        free(label);
    }
    /* then a listener on each: QEMU asks a new listener for its properties before it answers anything else (one blocking
     * call to it meanwhile would wait out D-Bus's timeout), so each is registered without waiting and every listener made
     * so far is served until the answer comes (5 s at most) */
    for (int k = 0; k < nheads; k++) {
        Dp = &H[k];
        int lv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, lv)) return 1;
        lbus = p2p(lv[0]);
        if (!lbus || sd_bus_add_object_vtable(lbus, NULL, LISTENER_PATH, "org.qemu.Display1.Listener", listener_vt, NULL) < 0 ||
            sd_bus_add_object_vtable(lbus, NULL, LISTENER_PATH, "org.qemu.Display1.Listener.Unix.Map", map_vt, NULL) < 0) return 1;
        static int done;
        done = 0;
        sd_bus_slot *slot = NULL;
        if (sd_bus_call_method_async(bus, &slot, NULL, CONSOLE_PATH, "org.qemu.Display1.Console", "RegisterListener", au_registered, &done, "h", lv[1]) >= 0) {
            long long until = now_ms() + 5000;
            while (!done && now_ms() < until) {
                struct pollfd pf[1 + HEADS_MAX];
                int np = 0;
                while (sd_bus_process(bus, NULL) > 0) ;
                for (int j = 0; j <= k; j++) { Dp = &H[j]; while (sd_bus_process(lbus, NULL) > 0) ; }
                Dp = &H[k];
                if (done) break;
                pf[np++] = (struct pollfd){ sd_bus_get_fd(bus), (short)sd_bus_get_events(bus), 0 };
                for (int j = 0; j <= k; j++) pf[np++] = (struct pollfd){ sd_bus_get_fd(H[j].lb), (short)sd_bus_get_events(H[j].lb), 0 };
                poll(pf, (nfds_t)np, 100);
            }
        }
        sd_bus_slot_unref(slot);
        close(lv[1]);
        if (done != 1) {
            fprintf(stderr, "lord-vmdisplay: RegisterListener (console %d): %s\n", k, done ? "refused" : "no answer");
            for (int j = 0; j < nheads; j++) { Dp = &H[j]; say(vd_msg(VD_STATE, VDS_LOST, VDR_SETUP, 0, 0, 0, 0), -1); }
            return 1;
        }
    }
    Dp = &H[0];
    /* the guest's sound and microphone: a listener of each, on a connection of its own, if QEMU has an audio backend on
     * this display (output 0's channel) */
    abus = register_audio(argv[1], "RegisterOutListener", "/org/qemu/Display1/AudioOutListener", "org.qemu.Display1.AudioOutListener", audio_vt);
    ibus = register_audio(argv[1], "RegisterInListener", "/org/qemu/Display1/AudioInListener", "org.qemu.Display1.AudioInListener", audio_in_vt);

    /* setup done: from here no files, no new sockets, no exec, threads only, in its own scope */
    char scope[80];
    snprintf(scope, sizeof scope, "lordos-vmdisplay-%s", argv[1]);
    /* each further output may hold a picture of its own (QEMU's map, a copy of an array scanout) */
    SandboxHelperCfg cfg = { .name = scope, .ro_paths = NULL, .extra_syscalls = (int[]){ -1 }, .allow_ioctl_classes = 0, .mem_max_mb = 512 + 256 * (nheads - 1) };
    sandbox_apply_helper(&cfg);
    for (int k = 0; k < nheads; k++) {
        Dp = &H[k];
        say(vd_msg(VD_READY, (int)cws[k], (int)chs[k], D.abs_mouse, 0, 0, 0), -1);
        say(vd_msg(VD_STATE, VDS_CONNECTED, VDR_NONE, 0, 0, 0, 0), -1);
        D.stats_ms = now_ms();
    }
    Dp = &H[0];
    int rc = 0;
    long long woke = now_ms();
    for (;;) {
        int r, lost = 0;
        for (int k = 0; k < nheads && !lost; k++) {
            Dp = &H[k];
            while ((r = sd_bus_process(lbus, NULL)) > 0) ;
            if (r < 0) lost = 1;
        }
        Dp = &H[0];
        if (!lost) { while ((r = sd_bus_process(bus, NULL)) > 0) ; if (r < 0) lost = 1; }
        if (lost) { for (int k = 0; k < nheads; k++) { Dp = &H[k]; say(vd_msg(VD_STATE, VDS_LOST, VDR_QEMU_GONE, 0, 0, 0, 0), -1); } Dp = &H[0]; break; }
        if (abus) {
            while ((r = sd_bus_process(abus, NULL)) > 0) ;
            if (r < 0) { fprintf(stderr, "lord-vmdisplay: the sound connection went away\n"); sd_bus_flush_close_unref(abus); abus = NULL; }
        }
        if (ibus) {
            while ((r = sd_bus_process(ibus, NULL)) > 0) ;
            if (r < 0) { fprintf(stderr, "lord-vmdisplay: the microphone connection went away\n"); sd_bus_flush_close_unref(ibus); ibus = NULL; }
        }
        long long now = now_ms();
        if (A.rec.h) au_tick(now);
        int to = -1;
        for (int k = 0; k < nheads; k++) {
            Dp = &H[k];
            if (D.ch < 0) continue;
            send_input(now);
            flush(now);
            /* QEMU's held UpdateDMABUF: once lord-server read the frame, when there's nowhere to send it, or after 250 ms */
            if (D.upd && ((D.acked && !D.dirty) || !D.shm || now - D.upd_ms >= vmpace_hold_ms(D.pace_ms, ACK_WAIT_MS))) answer_update();
            if (D.stats_changed && now - D.stats_ms >= STATS_MS) {
                say(vd_msg(VD_STATS, D.refused, D.updates, D.copies, (int)D.w, (int)D.h, D.frame), -1);
                D.refused = D.updates = D.copies = 0;
                D.stats_changed = 0;
                D.stats_ms = now;
            }
            if (D.updates) D.stats_changed = 1;
            int tl = bus_timeout_ms(lbus);
            if (tl >= 0 && (to < 0 || tl < to)) to = tl;
            if (D.dirty && D.shm) {
                long long wake = D.acked ? D.next_ms : D.sent_ms + ACK_WAIT_MS;
                if (wake < D.next_ms) wake = D.next_ms;         /* paced longer than the wait for an answer (vmpace.h) */
                int tf = wake <= now ? 0 : (int)(wake - now);
                if (to < 0 || tf < to) to = tf;
            }
            if (D.upd) { int tu = (int)(D.upd_ms + vmpace_hold_ms(D.pace_ms, ACK_WAIT_MS) - now); if (tu < 0) tu = 0; if (to < 0 || tu < to) to = tu; }
            if (D.resize_pending) { int tr = (int)(D.resize_ms + RESIZE_GAP_MS - now); if (tr < 0) tr = 0; if (to < 0 || tr < to) to = tr; }
            if (D.stats_changed) { int ts = (int)(D.stats_ms + STATS_MS - now); if (ts < 0) ts = 0; if (to < 0 || ts < to) to = ts; }
            if (now < D.abs_fast_until && (to < 0 || to > 100)) to = 100;     /* send_input asks IsAbsolute again meanwhile */
        }
        Dp = &H[0];
        /* every output's listener, the display's connection, every output's channel, the sound's two */
        struct pollfd p[3 + 2 * HEADS_MAX];
        int np = 0;
        for (int k = 0; k < nheads; k++) p[np++] = (struct pollfd){ sd_bus_get_fd(H[k].lb), (short)sd_bus_get_events(H[k].lb), 0 };
        int ch_at = np;
        for (int k = 0; k < nheads; k++) p[np++] = (struct pollfd){ H[k].ch, H[k].ch >= 0 ? POLLIN : 0, 0 };
        p[np++] = (struct pollfd){ sd_bus_get_fd(bus), (short)sd_bus_get_events(bus), 0 };
        p[np++] = (struct pollfd){ abus ? sd_bus_get_fd(abus) : -1, abus ? (short)sd_bus_get_events(abus) : 0, 0 };
        p[np++] = (struct pollfd){ ibus ? sd_bus_get_fd(ibus) : -1, ibus ? (short)sd_bus_get_events(ibus) : 0, 0 };
        int t2 = bus_timeout_ms(bus);
        if (t2 >= 0 && (to < 0 || t2 < to)) to = t2;
        if (abus) { int t3 = bus_timeout_ms(abus); if (t3 >= 0 && (to < 0 || t3 < to)) to = t3; }
        if (ibus) { int t4 = bus_timeout_ms(ibus); if (t4 >= 0 && (to < 0 || t4 < to)) to = t4; }
        if (A.rec.h && (to < 0 || to > 20)) to = 20;         /* silence keeps time while a recording runs */
        long long busy = now_ms() - woke;
        if (busy > T.busy_max) T.busy_max = busy;
        if (busy >= 200 && now - T.busy_told >= 10000) {
            T.busy_told = now;
            fprintf(stderr, "lord-vmdisplay %s: a turn of its loop took %lld ms (QEMU's calls waited meanwhile)\n", me, busy);
        }
        if (poll(p, (nfds_t)np, to) < 0 && errno != EINTR) break;
        woke = now_ms();
        int end = 0;
        for (int k = 0; k < nheads && !end; k++) {
            Dp = &H[k];
            if (D.ch < 0) continue;
            short re = p[ch_at + k].revents;
            int gone = (re & (POLLHUP | POLLERR) && !(re & POLLIN)) || (re & POLLIN && from_server());
            if (!gone) continue;
            if (!k) { end = 1; break; }
            /* another output's window went: that output alone stops (its keys and buttons let go, QEMU's held call answered) */
            fprintf(stderr, "lord-vmdisplay %s: output %d's window went\n", me, k);
            release_all();
            answer_update();
            close(D.ch);
            D.ch = -1;
            if (D.shm) { munmap(D.shm, D.shm_len); D.shm = NULL; D.shm_len = 0; }
        }
        Dp = &H[0];
        if (end) break;
    }
    for (int k = 1; k < nheads; k++) { Dp = &H[k]; release_all(); answer_update(); }
    Dp = &H[0];
    release_all();
    answer_update();
    au_counts("end");
    fprintf(stderr, "lord-vmdisplay %s: the display listener's calls: %lld UpdateDMABUF answers held for lord-server, the "
                    "longest %lld ms; the longest turn of the loop %lld ms\n", me, T.held, T.slowest, T.busy_max);
    sd_bus_process(bus, NULL);      /* pushes what the socket takes; never waits on QEMU */
    return rc;
}
