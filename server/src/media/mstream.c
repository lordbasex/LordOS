/* mstream.c — the remote desktop's video on the session side (docs/REMOTE.md, "Video (WebRTC)").
 *
 * The session renders once; the frame the remote desktop sees (protected windows in black, from
 * media_composed or the GPU's read-back) feeds one live encoder per codec in use: lord-media in
 * MK_STREAM mode, through a sealed memfd of 3 slots, as for the screen recording. Every viewer of this
 * session gets the same encoded frames (remote/mirror.c sends them to each bridge); only the
 * encryption is per connection. Two users means two sessions, two lord-servers and two encoders.
 * If the encoder is behind (no free slot) the frame waits: the newest one goes when a slot frees up,
 * so a slow machine sends fewer frames, never old ones. Sound: the session's default output,
 * recorded by lord-server itself (maudio.c, PipeWire) into a ring the encoder reads. */
#include "../par.h"
#include "../canvas.h"
#include "mstream.h"
#include "media_int.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define FPS_MAX 240          /* what the encoder is asked for at most (the viewers' screens decide) */
#define MAX_FRAME (8u << 20)

typedef struct {
    int id;                     /* the MediaSrc of the helper (0: not running) */
    int viewers, audio;
    unsigned kbps, sent_kbps;
    int w, h, pending, want_key, flushing;
    double t0, last_feed, retry_at, flush_at;
    int fps;                    /* frames a second (the fastest viewer's screen) */
    uint8_t *buf;
    size_t len, cap, total;
    long frames, enc100;
    int hw;
    int dirty[4][4];            /* per input slot: what changed since it was last written (x0, y0, x1, y1) */
} Enc;

static Enc E[MS_CODECS];
static MStreamOut out;
static const uint32_t *fpx;
static int fstride, fw, fh;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

void mstream_init(const MStreamOut *o) { if (o) out = *o; }

static MediaSrc *src_of(Enc *e) {
    MediaSrc *s = e->id ? media_src_get(e->id) : NULL;
    return s && s->kind == SK_STREAM ? s : NULL;
}

static Enc *enc_of(const MediaSrc *s) {
    for (int c = 1; c < MS_CODECS; c++) if (E[c].id && E[c].id == s->id) return &E[c];
    return NULL;
}

/* Messages of our helpers, before media.c's own handling (which keeps MM_SLOTFREE and MM_STATE). */
static int on_msg(void *src, const MediaMsg *m, const char *payload, int *fds) {
    (void)fds;
    MediaSrc *s = src;
    Enc *e = enc_of(s);
    if (!e) return 0;
    int codec = (int)(e - E);
    if (m->type == MM_PACKET) {
        int64_t total = m->v[0], off = m->v[1];
        if (m->id == 1) {                                       /* Opus: one chunk */
            if (off == 0 && total == (int64_t)m->len && m->len > 0 && m->len <= 1500 && out.audio)
                out.audio(codec, (uint32_t)m->v[2], (const uint8_t *)payload, m->len);
            return 1;
        }
        if (m->id != 0 || total <= 0 || total > (int64_t)MAX_FRAME || off < 0 || !m->len) return 1;
        if (off == 0) { e->len = 0; e->total = (size_t)total; }
        if ((size_t)off != e->len || (size_t)total != e->total || e->len + m->len > e->total) { e->len = 0; e->total = 0; e->want_key = 1; return 1; }
        if (e->len + m->len > e->cap) {
            size_t cap = e->cap ? e->cap : 65536;
            while (cap < e->len + m->len) cap *= 2;
            uint8_t *nb = realloc(e->buf, cap);
            if (!nb) { e->len = 0; return 1; }
            e->buf = nb;
            e->cap = cap;
        }
        memcpy(e->buf + e->len, payload, m->len);
        e->len += m->len;
        if (e->len == e->total) {
            if (out.video) out.video(codec, m->v[3] != 0, (uint32_t)m->v[2], e->buf, e->len);
            e->len = e->total = 0;
        }
        return 1;
    }
    if (m->type == MM_STATS) { e->frames = (long)m->v[0]; e->enc100 = (long)m->v[1]; e->hw = m->v[2] != 0; return 1; }
    if (m->type == MM_STATE && m->v[0] == MS_RECORDING) e->hw = m->v[1] != 0;
    return 0;
}

static void stop(Enc *e) {
    MediaSrc *s = src_of(e);
    if (s) { s->closing = 0; s->state = MS_DONE; media_src_close(s); }
    e->id = 0;
    e->len = e->total = 0;
}

static int start(Enc *e, int codec) {
    if (!fpx || fw < 16 || fh < 16 || now_ms() < e->retry_at) return -1;
    MediaSrc *s = media_src_new(SK_STREAM, "remote");
    if (!s) return -1;
    int ring = -1, render = codec == 1 ? media_render_fd() : -1;
    if (e->audio) s->mic = maudio_open(1, "LordOS", "Remote desktop", "@DEFAULT_MONITOR@", &ring);
    char params[200];
    snprintf(params, sizeof params, "codec=%s\nwidth=%d\nheight=%d\nfps=%d\nkbps=%u\nhw=%d\nhwaccel=%s\naudio=%d\nrender=%d\n",
             codec == 1 ? "h264" : "vp8", fw, fh, e->fps, e->kbps, render >= 0, media_hwaccel(), ring >= 0, render >= 0);
    int fds[2] = { ring, render };
    s->sock = media_spawn(MK_STREAM, params, fds, 2, &s->pid);
    if (render >= 0) close(render);
    if (s->sock < 0) { media_src_close(s); e->retry_at = now_ms() + 10000; return -1; }
    s->bk_msg = on_msg;
    /* input slots: lord-server writes, lord-media only reads */
    int slots = 3;
    size_t n = (size_t)fw * fh * 4 * slots;
    int fd = memfd_create("lordos-remote", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    void *m = MAP_FAILED;
    if (fd >= 0 && !ftruncate(fd, (off_t)n) && !fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL))
        m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int64_t v[6] = { slots, fw, fh, 0, 0, 0 };
    if (m == MAP_FAILED || mp_send(s->sock, MM_INRING, 0, v, NULL, 0, &fd, 1)) {
        if (m != MAP_FAILED) munmap(m, n);
        if (fd >= 0) close(fd);
        media_src_close(s);
        e->retry_at = now_ms() + 10000;
        return -1;
    }
    s->in_map = m;
    s->in_len = n;
    s->in_fd = fd;
    s->in_w = fw;
    s->in_h = fh;
    s->in_slots = slots;
    memset(s->in_busy, 0, sizeof s->in_busy);
    e->id = s->id;
    e->w = fw;
    e->h = fh;
    e->t0 = now_ms();
    for (int i = 0; i < 4; i++) { e->dirty[i][0] = e->dirty[i][1] = 0; e->dirty[i][2] = fw; e->dirty[i][3] = fh; }
    e->last_feed = 0;
    e->pending = 1;
    e->sent_kbps = e->kbps;
    e->want_key = 0;
    fprintf(stderr, "remote: video encoder for %s started (%dx%d, %d fps, %u kbit/s%s)\n", codec == 1 ? "H.264" : "VP8", fw, fh, e->fps, e->kbps,
            ring >= 0 ? ", with sound" : "");
    return 0;
}

int mstream_info(int codec, int *hw, int *fps, long *enc100) {
    if (codec < 1 || codec >= MS_CODECS || !E[codec].id) return 0;
    *hw = E[codec].hw;
    *fps = E[codec].fps;
    *enc100 = E[codec].enc100;
    return 1;
}

int mstream_active(void) {
    for (int c = 1; c < MS_CODECS; c++) if (E[c].id) return 1;
    return 0;
}

void mstream_want(int codec, int viewers, int audio, unsigned kbps, int fps) {
    if (codec < 1 || codec >= MS_CODECS) return;
    Enc *e = &E[codec];
    fps = fps < 24 ? 30 : fps > FPS_MAX ? FPS_MAX : fps;
    if (e->id && fps != e->fps) stop(e);                      /* another frame rate: the encoder starts again */
    e->fps = fps;
    int was = e->viewers;
    e->viewers = viewers;
    e->kbps = kbps < 300 ? 300 : kbps > 20000 ? 20000 : kbps;
    if (!viewers) {
        if (e->id) fprintf(stderr, "remote: video encoder for %s stopped\n", codec == 1 ? "H.264" : "VP8");
        stop(e);
        e->audio = 0;
        return;
    }
    if (audio && !e->audio && e->id) stop(e);                /* sound joins: restart with the ring */
    e->audio = e->audio || audio;
    if (viewers > was) e->want_key = 1;                       /* a new viewer needs a keyframe */
    if (!e->id) start(e, codec);
}

/* A keyframe now, even if the screen doesn't change: frames are only encoded when something changes,
 * and a viewer that lost one (or just joined) would otherwise wait for the next change. */
void mstream_keyframe(int codec) {
    if (codec < 1 || codec >= MS_CODECS) return;
    E[codec].want_key = 1;
    E[codec].pending = 1;
}

/* the copy into an input slot, by rows over the pool (par.h) */
typedef struct { uint32_t *dst; int w, x0, x1; } SlotCopy;
static void slot_rows(void *ctx, int y0, int y1) {
    SlotCopy *k = ctx;
    for (int y = y0; y < y1; y++) memcpy(k->dst + (size_t)y * k->w + k->x0, fpx + (size_t)y * fstride + k->x0, (size_t)(k->x1 - k->x0) * 4);
}

static void feed(Enc *e, int codec) {
    MediaSrc *s = src_of(e);
    if (!s || s->sock < 0) {
        if (e->id) { fprintf(stderr, "remote: the video encoder stopped; starting it again\n"); e->id = 0; e->retry_at = now_ms() + 2000; }
        start(e, codec);
        return;
    }
    double t = now_ms();
    /* one more (tiny, unchanged) frame shortly after the last change: browsers paint a lone frame late,
     * often only when the next one arrives */
    if (!e->pending && e->flush_at && t >= e->flush_at) { e->pending = 1; e->flush_at = 0; e->flushing = 1; }
    if (!e->pending || !fpx) return;
    if (t - e->last_feed < 1000.0 / e->fps - 1) return;
    int slot = -1;
    for (int i = 0; i < s->in_slots; i++) if (!s->in_busy[i]) { slot = i; break; }
    if (slot < 0) return;                                     /* the encoder is behind: the newest frame waits */
    /* only what changed since this slot was last written: the rest of it is already the screen */
    uint32_t *dst = (uint32_t *)(s->in_map + (size_t)slot * e->w * e->h * 4);
    int *d = slot < 4 ? e->dirty[slot] : NULL, full[4] = { 0, 0, e->w, e->h };
    if (!d) d = full;
    int x0 = d[0] < 0 ? 0 : d[0], y0 = d[1] < 0 ? 0 : d[1], x1 = d[2] > e->w ? e->w : d[2], y1 = d[3] > e->h ? e->h : d[3];
    if (x1 > x0 && y1 > y0) {
        SlotCopy k = { dst, e->w, x0, x1 };
        if ((long)(x1 - x0) * (y1 - y0) >= 262144 && canvas_parallel_on()) par_rows(y0, y1, slot_rows, &k);
        else slot_rows(&k, y0, y1);
    }
    if (slot < 4) d[0] = d[1] = d[2] = d[3] = 0;
    if (e->kbps != e->sent_kbps) { mp_sendv(s->sock, MM_RATE, 0, e->kbps, 0, 0, 0); e->sent_kbps = e->kbps; }
    if (e->want_key) { mp_sendv(s->sock, MM_KEYFRAME, 0, 0, 0, 0, 0); e->want_key = 0; }
    if (mp_sendv(s->sock, MM_INFRAME, (uint32_t)slot, (int64_t)(t - e->t0), 0, 0, 0)) return;
    s->in_busy[slot] = 1;
    e->last_feed = t;
    e->pending = 0;
    e->flush_at = e->flushing ? 0 : t + 70;
    e->flushing = 0;
}

void mstream_frame(const uint32_t *px, int stride, int w, int h, const int *box) {
    fpx = px;
    fstride = stride;
    fw = w & ~1;
    fh = h & ~1;
    int all[4] = { 0, 0, fw, fh };
    if (!box) box = all;
    for (int c = 1; c < MS_CODECS; c++) {
        Enc *e = &E[c];
        if (!e->viewers) continue;
        if (e->id && (e->w != fw || e->h != fh)) stop(e);    /* new screen size: a new encoder */
        for (int i = 0; i < 4; i++) {                        /* every slot misses this change */
            int *d = e->dirty[i];
            if (d[2] <= d[0] || d[3] <= d[1]) memcpy(d, box, sizeof all);
            else {
                if (box[0] < d[0]) d[0] = box[0];
                if (box[1] < d[1]) d[1] = box[1];
                if (box[2] > d[2]) d[2] = box[2];
                if (box[3] > d[3]) d[3] = box[3];
            }
        }
        e->pending = 1;
        feed(e, c);
    }
}

void mstream_tick(void) {
    for (int c = 1; c < MS_CODECS; c++) if (E[c].viewers) feed(&E[c], c);
}

double mstream_timeout(void) {
    double best = -1, t = now_ms();
    for (int c = 1; c < MS_CODECS; c++) {
        Enc *e = &E[c];
        if (!e->viewers) continue;
        double d = !e->id ? (e->retry_at > t ? e->retry_at - t : 200) : e->pending ? 1000.0 / e->fps - (t - e->last_feed)
                 : e->flush_at ? e->flush_at - t : -1;
        if (d >= 0 && d < 4) d = 4;
        if (d >= 0 && (best < 0 || d < best)) best = d;
    }
    return best;
}

void mstream_describe(int codec, char *o, size_t n) {
    if (codec < 1 || codec >= MS_CODECS || !E[codec].id) { snprintf(o, n, "-"); return; }
    const Enc *e = &E[codec];
    snprintf(o, n, "%s (%s) %dx%d %u kbit/s, %ld frames, %.2f ms/frame", codec == 1 ? "H.264" : "VP8", e->hw ? "VA-API" : "software",
             e->w, e->h, e->kbps, e->frames, e->enc100 / 100.0);
}
