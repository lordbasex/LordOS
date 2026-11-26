/* mambient.c — moving wallpapers in lord-server (docs/MOVING-WALLPAPERS.md, docs/MEDIA.md "Video frames").
 *
 * What makes them cheap:
 *   - NV12 frames: lord-media sends the decoder's own 4:2:0 planes (a sealed memfd ring, MF_NV12) or, zero-copy, the
 *     VA-API surface itself as dmabufs (MM_DMAFRAME, MF_DMABUF). The GPU turns either into one RGBA texture a frame
 *     (gpu_yuv_convert), which every display's view draws below the UI (wl_paint_media): a new frame is a present of
 *     the displays that show it, never a UI repaint or a 4-byte-a-pixel upload. The CPU path (no GPU composition)
 *     asks BGRA as before.
 *   - Per view visibility: a view wholly hidden (opaque windows over its whole box, a fullscreen Linux app on its
 *     display, the screen off) is not presented; when no view of a player shows, the player pauses; after
 *     MLOOP_HIDDEN_STOP_MS hidden, its loop cache is freed and lord-media ended, and both come back when a view shows.
 *   - The loop cache: after one whole pass the frames stay on the GPU (packed NV12, 1.5 bytes a pixel, copied on the
 *     GPU with no read back), within a budget from Settings (motionCacheMb) and the card's VRAM; from then on the loop
 *     plays from it at the clip's own pace and lord-media is paused.
 *   - A frame-rate cap (Settings, or the battery rule): lord-media sends fewer frames, the cache shows fewer.
 * The rules themselves are mloop.c's (tested alone); this file holds the textures, the clocks and the helper.
 *
 * A dmabuf from lord-media is untrusted input (AGENTS.md, "lord-media → lord-server dmabuf frames"): only fds that are
 * dmabufs, their real sizes from lseek, mp_dma_check's closed rules and the driver's own modifier list; anything
 * refused sends that player back to the memfd ring for good, logged once. */
#include "media_int.h"
#include "mloop.h"
#include "nv12.h"
#include "../gpu/gpu.h"
#include "../ui/ui.h"
#include "../wl/wl.h"
#include "../monitor/gpustat.h"

#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define VIS_MS 250                      /* how often the views' visibility is looked at */
#define MEM_MS 5000                     /* and the GPU's memory, while a cache exists */
#define DMA_FLOOD 240                   /* zero-copy frames a second past which a helper is cut off */

typedef struct MAmb {
    char *path;                         /* the file (validated again at each respawn) */
    int js_paused;                      /* JS's pause/play (Reduce motion, idle, battery…) */
    int hold;                           /* MLH_* applied */
    double hidden_since;                /* when the last view went hidden (0: one shows) */
    int stopped;                        /* lord-media ended (MLH_STOP) */
    int helper_on;                      /* what lord-media was told: 1 play, 0 pause */
    int64_t resume_pos;
    int cache_mb, fps_cap;
    /* the loop cache */
    MLoopRec rec;
    GpuPack **frames;
    int nframes, state, fails;          /* state: AC_* */
    int64_t period, bytes, start_pts;
    int cw, ch, cmatrix, cfull, crate;  /* the frames' size and colours, the cap they were recorded under */
    double t0, paused_at;               /* the cache's clock: t = now - t0 */
    int shown;
    int64_t next_t;
    double last_vis;
} MAmb;

enum { AC_NONE = 0, AC_REC, AC_PLAY, AC_REFUSED };

static int64_t cache_total;             /* every player's cache together */
static double mem_checked;
static int64_t mem_budget = -1, vram_total = -1, vram_used = -1;
static int mem_integrated, mem_pressure;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int env_off(const char *name) { const char *e = getenv(name); return e && !strcmp(e, "0"); }

/* ---- the GPU's memory (monitor/gpustat.c: the card that renders) ---- */

static int64_t meminfo(const char *key) {
    FILE *f = fopen("/proc/meminfo", "re");
    if (!f) return -1;
    char line[128];
    int64_t v = -1;
    size_t n = strlen(key);
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, n) && line[n] == ':') { v = atoll(line + n + 1) * 1024; break; }
    fclose(f);
    return v;
}

static void read_memory(int64_t setting_mb) {
    GpuInfo gi;
    gpu_info(&gi);
    char slot[64] = "";
    const char *rn = gi.render_node && gi.render_node[0] ? strrchr(gi.render_node, '/') : NULL;
    if (rn) {
        char p[96], real[PATH_MAX];
        snprintf(p, sizeof p, "/sys/class/drm/%s/device", rn + 1);
        if (realpath(p, real)) { const char *b = strrchr(real, '/'); const char *v = b ? b + 1 : real; size_t l = strlen(v); if (l < sizeof slot) memcpy(slot, v, l + 1); }
    }
    static GpuCard cards[GPU_CARDS];
    int n = gpu_cards_read("", NULL, cards, GPU_CARDS);
    const GpuCard *g = NULL;
    for (int i = 0; i < n && !g; i++) if (slot[0] && !strcmp(cards[i].slot, slot)) g = &cards[i];
    for (int i = 0; i < n && !g; i++) if (cards[i].render) g = &cards[i];
    vram_total = g ? g->vram_total : -1;
    vram_used = g ? g->vram_used : -1;
    /* no VRAM of its own, or no PCIe link (an APU, Intel, the Pi's v3d): shared memory */
    mem_integrated = !g || g->vram_total <= 0 || g->link_max_width < 0;
    int64_t mt = meminfo("MemTotal"), ma = meminfo("MemAvailable");
    /* the budget leaves out what the caches already hold (it's in vram_used) */
    int64_t used_others = vram_used >= 0 ? vram_used - cache_total : -1;
    mem_budget = mloop_budget(setting_mb, vram_total, used_others, mt, mem_integrated);
    mem_pressure = mloop_pressure(vram_total, vram_used, ma, mt, mem_integrated);
    mem_checked = now_ms();
}

/* ---- lord-media ---- */

static void helper_set(MediaSrc *s, int on) {
    MAmb *a = s->amb;
    if (!a || s->sock < 0 || a->helper_on == on) return;
    if (!mp_sendv(s->sock, on ? MM_PLAY : MM_PAUSE, 0, 0, 0, 0, 0)) a->helper_on = on;
}

/* the frame format this player should send now (MM_FORMAT) */
static int want_format(MediaSrc *s) {
    if (!gpu_active()) return MF_BGRA;
    if (env_off("LORD_MEDIA_NV12")) return MF_BGRA;
    if (s->amb && !s->dma_refused && !env_off("LORD_MEDIA_DMABUF") && s->box_w > 0) return MF_DMABUF;
    return MF_NV12;
}

static void format_check(MediaSrc *s) {
    if (s->sock < 0 || s->kind != SK_PLAYER) return;
    int f = want_format(s);
    if (f != s->fmt_sent && !mp_sendv(s->sock, MM_FORMAT, 0, f, 0, 0, 0)) s->fmt_sent = f;
}

void media_src_format_check(MediaSrc *s) { if (s && s->nv_capable) format_check(s); }

/* ---- the CPU's copy (map) of an NV12 frame ---- */

/* map as a BGRA buffer of w × h (lord-server's own, anonymous: pages that are never touched cost nothing) */
static int cpu_buf(MediaSrc *s, int w, int h) {
    size_t need = (size_t)w * h * 4;
    if (s->map && s->nv && s->map_len == need) return 0;
    if (s->map) munmap(s->map, s->map_len);
    void *m = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    s->map = m == MAP_FAILED ? NULL : m;
    s->map_len = s->map ? need : 0;
    s->ring_w = w;
    s->ring_h = h;
    s->ring_slots = 1;
    s->cur_slot = -1;
    s->nv_y0 = s->nv_y1 = 0;
    return s->map ? 0 : -1;
}

const uint32_t *media_src_rows(MediaSrc *s, int y0, int y1) {
    if (!s || !s->map) return NULL;
    if (!s->nv) return s->ring_slots && s->cur_slot >= 0 ? (const uint32_t *)(s->map + (size_t)s->cur_slot * s->ring_w * s->ring_h * 4) : s->px;
    int w = s->ring_w, h = s->ring_h;
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;
    if (y1 <= y0) return (const uint32_t *)s->map;
    if (s->nv_seq != s->frame_seq) { s->nv_seq = s->frame_seq; s->nv_y0 = s->nv_y1 = 0; }
    if (y0 >= s->nv_y0 && y1 <= s->nv_y1) return (const uint32_t *)s->map;
    int ok = -1;
    if (s->nv_slot >= 0 && s->nv_map && s->nv_ring.w == w && s->nv_ring.h == h) {
        const uint8_t *base = s->nv_map + (size_t)s->nv_slot * s->nv_ring.slot_bytes;
        nv12_to_argb(base + s->nv_ring.y_off, s->nv_ring.y_stride, base + s->nv_ring.uv_off, s->nv_ring.uv_stride, w, h,
                     s->nv_ring.matrix, s->nv_ring.full, (uint32_t *)s->map, w, y0, y1);
        ok = 0;
    } else if (s->vtex && gpu_tex_current(s->vtex) && gpu_tex_width(s->vtex) == w) {
        /* a zero-copy or cached frame: only the GPU has it; the rows read back (small strips) */
        ok = gpu_tex_read_rows(s->vtex, s->map, y0, y1 - y0, 0);
    }
    if (ok) return NULL;
    if (s->nv_y1 > s->nv_y0 && y0 <= s->nv_y1 && y1 >= s->nv_y0) {
        if (y0 < s->nv_y0) s->nv_y0 = y0;
        if (y1 > s->nv_y1) s->nv_y1 = y1;
    } else { s->nv_y0 = y0; s->nv_y1 = y1; }
    return (const uint32_t *)s->map;
}

static int nv_cpu_read(int id) {
    MediaSrc *s = media_src_get(id);
    if (!s || !s->nv) return -1;
    return media_src_rows(s, 0, s->ring_h) ? 0 : -1;
}

/* the frame now in vtex (or, without a GPU, in the ring) becomes the shown one */
static void nv_show(MediaSrc *s, int w, int h, int64_t pts) {
    if (cpu_buf(s, w, h)) return;
    s->cur_slot = 0;
    s->w = w;
    s->h = h;
    s->pos = pts;
    s->frame_seq++;
    s->cpu_stale = 1;
    s->cpu_read = nv_cpu_read;
    if (s->vtex && gpu_tex_current(s->vtex)) { s->gtex = s->vtex; s->gtex_flip = 0; }
    else s->gtex = NULL;
    if (s->gtex && s->gpu_drawn) {
        media_src_present(s);
        media_src_blur_damage(s);
    } else media_src_damage(s);
}

void media_nv_reset(MediaSrc *s) {
    if (s->nv_map) munmap(s->nv_map, s->nv_len);
    s->nv_map = NULL;
    s->nv_len = 0;
    s->nv_slot = -1;
    if (s->nv && s->map) { munmap(s->map, s->map_len); s->map = NULL; s->map_len = 0; s->ring_slots = 0; }
    if (s->gtex == s->vtex) { s->gtex = NULL; s->gpu_drawn = 0; }
    s->nv = 0;
    s->cpu_stale = 0;
}

/* ---- the loop cache ---- */

static void cache_drop(MediaSrc *s, const char *why) {
    MAmb *a = s->amb;
    if (!a) return;
    int was = a->state;
    for (int i = 0; i < a->nframes; i++) gpu_pack_free(a->frames[i]);
    free(a->frames);
    a->frames = NULL;
    cache_total -= a->bytes;
    if (cache_total < 0) cache_total = 0;
    a->bytes = 0;
    a->nframes = 0;
    mloop_rec_reset(&a->rec);
    a->rec.on = 0;
    if (was == AC_PLAY && why) fprintf(stderr, "media: moving wallpaper's loop cache dropped (%s)\n", why);
    a->state = was == AC_REFUSED ? AC_REFUSED : AC_NONE;
    if (was == AC_PLAY && s->sock >= 0) {
        /* lord-media goes on from where the cache was */
        int64_t t = a->hold == MLH_RUN ? (int64_t)(now_ms() - a->t0) : (int64_t)(a->paused_at - a->t0);
        if (a->period > 0) mp_sendv(s->sock, MM_SEEK, 0, (t % a->period + a->period) % a->period, 0, 0, 0);
        helper_set(s, a->hold == MLH_RUN);
    }
}

/* may a pass be recorded now? */
static int cache_may_start(MediaSrc *s, int w, int h) {
    MAmb *a = s->amb;
    if (a->state != AC_NONE || a->cache_mb <= 0 || !gpu_active() || a->hold != MLH_RUN || env_off("LORD_MEDIA_LOOPCACHE")) return 0;
    if (s->dur <= 0 || s->dur > MLOOP_MAX_MS || a->rec.interval <= 0 || !gpu_pack_ready()) return 0;
    if (now_ms() - mem_checked > MEM_MS || mem_budget < 0) read_memory(a->cache_mb);
    int64_t need = mloop_estimate(w, h, s->dur, a->rec.interval);
    if (need <= 0 || mem_pressure || cache_total + need > mem_budget) {
        a->state = AC_REFUSED;          /* until the size or the settings change */
        fprintf(stderr, "media: moving wallpaper's loop cache: %lld MB needed, %lld MB allowed: decoding as it plays\n",
                (long long)(need >> 20), (long long)((mem_budget - cache_total) >> 20));
        return 0;
    }
    return 1;
}

/* one frame shown from lord-media: kept in the cache while a pass is recorded (src: its planes on the GPU) */
static void cache_frame(MediaSrc *s, const GpuYuv *src, int64_t pts, int w, int h, int matrix, int full) {
    MAmb *a = s->amb;
    if (!a || a->state == AC_PLAY) return;
    int start = cache_may_start(s, w, h);
    int r = mloop_rec_frame(&a->rec, pts, w, h, start || a->state == AC_REC);
    if (r == MLR_KEEP) {
        if (a->state != AC_REC) {
            a->state = AC_REC;
            a->cw = w; a->ch = h; a->cmatrix = matrix; a->cfull = full; a->crate = a->fps_cap;
            a->frames = calloc(MLOOP_MAX_FRAMES, sizeof *a->frames);
            if (!a->frames) { cache_drop(s, NULL); a->state = AC_REFUSED; return; }
        }
        GpuPack *k = matrix == a->cmatrix && full == a->cfull ? gpu_yuv_pack(src) : NULL;
        int64_t b = gpu_pack_bytes(k);
        if (!k || a->rec.n - 1 != a->nframes || cache_total + b > mem_budget) {
            gpu_pack_free(k);
            cache_drop(s, NULL);
            if (++a->fails >= 3) a->state = AC_REFUSED;
            return;
        }
        a->frames[a->nframes++] = k;
        a->bytes += b;
        cache_total += b;
    } else if (r == MLR_DONE) {
        int64_t period;
        if (!mloop_rec_whole(&a->rec, s->dur, &period)) { cache_drop(s, NULL); if (++a->fails >= 3) a->state = AC_REFUSED; return; }
        a->state = AC_PLAY;
        a->period = period;
        a->t0 = now_ms() - (double)pts;     /* this frame is the next turn's: the clock goes on from it */
        a->shown = -1;
        a->next_t = 0;
        helper_set(s, 0);
        fprintf(stderr, "media: moving wallpaper's loop cache: %d frames %dx%d, %.1f s, %lld MB on the GPU; lord-media paused\n",
                a->nframes, a->cw, a->ch, period / 1000.0, (long long)(a->bytes >> 20));
    } else if (r == MLR_ABORT && a->state == AC_REC) {
        cache_drop(s, NULL);
        if (++a->fails >= 3) a->state = AC_REFUSED;
    }
}

/* the cache's next frame, when due */
static void cache_step(MediaSrc *s, double now) {
    MAmb *a = s->amb;
    if (!a || a->state != AC_PLAY || a->hold != MLH_RUN) return;
    int64_t t = (int64_t)(now - a->t0), next = -1;
    int k = mloop_pick(a->rec.pts, a->nframes, a->period, t, a->fps_cap, a->shown, &next);
    int64_t turn = a->period > 0 ? t / a->period * a->period : 0;
    a->next_t = next >= 0 ? turn + (next > t % a->period ? next : a->period) : 0;
    if (k < 0 || k == a->shown) return;
    if (!gpu_pack_current(a->frames[k]) || gpu_pack_convert(a->frames[k], &s->vtex, a->cmatrix, a->cfull)) { cache_drop(s, "GPU context lost"); return; }
    a->shown = k;
    s->nv_slot = -1;                    /* the CPU's rows now come from the GPU (media_src_rows) */
    nv_show(s, a->cw, a->ch, a->rec.pts[k]);
}

/* ---- frames from lord-media ---- */

int media_nv_ring(MediaSrc *s, const MediaMsg *m, int fd) {
    MpRing L;
    if (fd < 0 || mp_ring_layout(m->v[4], m->v[1], m->v[2], m->v[3], m->v[5], &L) || L.fmt != MF_NV12) return -1;
    size_t need = L.slot_bytes * (size_t)L.slots;
    uint8_t *map = media_map_sealed(fd, need);
    if (!map) return -1;
    if (s->nv_map) munmap(s->nv_map, s->nv_len);
    if (!s->nv && s->map) { munmap(s->map, s->map_len); s->map = NULL; s->map_len = 0; }
    s->nv = 1;
    s->nv_map = map;
    s->nv_len = need;
    s->nv_ring = L;
    s->nv_slot = -1;
    s->ring_gen = (int)m->v[0];
    return 0;
}

void media_nv_frame(MediaSrc *s, int slot, int64_t pts) {
    const MpRing *L = &s->nv_ring;
    if (!s->nv_map || slot < 0 || slot >= L->slots) return;
    format_check(s);
    if (s->amb && s->amb->state == AC_PLAY) return;          /* the cache plays: a frame sent before the pause */
    s->nv_slot = slot;
    const uint8_t *base = s->nv_map + (size_t)slot * L->slot_bytes;
    int ok = -1;
    if (gpu_active()) {
        s->yuv = gpu_yuv_upload(s->yuv, base + L->y_off, L->y_stride, base + L->uv_off, L->uv_stride, L->w, L->h);
        if (s->yuv && !gpu_yuv_convert(s->yuv, &s->vtex, L->matrix, L->full)) {
            ok = 0;
            if (s->amb) cache_frame(s, s->yuv, pts, L->w, L->h, L->matrix, L->full);
        }
    }
    if (ok && s->vtex) { gpu_tex_free(s->vtex); s->vtex = NULL; }
    nv_show(s, L->w, L->h, pts);
}

/* a dmabuf fd? (its link reads anon_inode:dmabuf, or /dmabuf: on newer kernels) */
static int is_dmabuf(int fd) {
    char p[64], l[128];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, l, sizeof l - 1);
    if (n <= 0) return 0;
    l[n] = 0;
    return !strncmp(l, "anon_inode:dmabuf", 17) || !strncmp(l, "/dmabuf:", 8);
}

static void dma_refuse(MediaSrc *s, const char *why) {
    if (!s->dma_refused) fprintf(stderr, "media: zero-copy frame refused (%s): this player goes back to the shared-memory ring\n", why);
    s->dma_refused = 1;
    format_check(s);
}

void media_dma_frame(MediaSrc *s, const MediaMsg *m, const char *payload, int *fds) {
    int nfds = 0;
    for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) nfds = i + 1;
    mp_sendv(s->sock, MM_ACK, m->id, 0, 0, 0, 0);            /* whatever happens, the helper may let older ones go */
    if (s->dma_refused || !s->amb) return;
    if (!gpu_active()) { format_check(s); return; }
    /* a helper flooding fds is cut off */
    double t = now_ms();
    if (t - s->dma_win > 1000) { s->dma_win = t; s->dma_n = 0; }
    if (++s->dma_n > DMA_FLOOD) { dma_refuse(s, "too many"); return; }
    if (nfds < 1 || nfds > MP_DMA_MAX_OBJ) { dma_refuse(s, "fds"); return; }
    int64_t size[MP_DMA_MAX_OBJ] = { 0, 0 };
    for (int i = 0; i < nfds; i++) {
        if (fds[i] < 0 || !is_dmabuf(fds[i])) { dma_refuse(s, "not a dmabuf"); return; }
        off_t e = lseek(fds[i], 0, SEEK_END);
        size[i] = e > 0 ? (int64_t)e : 0;
    }
    /* never larger than the box asked (MM_SIZE), a quarter more for the rounding of a video's own size */
    int bw = s->box_w > 0 ? s->box_w + s->box_w / 4 + 16 : 0, bh = s->box_h > 0 ? s->box_h + s->box_h / 4 + 16 : 0;
    if (mp_dma_check(payload, m->len, nfds, size, bw, bh, gpu_dmabuf_plane_ok)) { dma_refuse(s, "description"); return; }
    MpDma d;
    memcpy(&d, payload, sizeof d);
    GpuDmabuf py = { .width = (int)d.w, .height = (int)d.h, .nplanes = 1, .fourcc = d.plane[0].fourcc, .modifier = d.modifier[d.plane[0].obj],
                     .fd = { fds[d.plane[0].obj], -1, -1, -1 }, .offset = { d.plane[0].offset }, .stride = { d.plane[0].pitch } };
    GpuDmabuf puv = { .width = (int)d.w / 2, .height = (int)d.h / 2, .nplanes = 1, .fourcc = d.plane[1].fourcc, .modifier = d.modifier[d.plane[1].obj],
                      .fd = { fds[d.plane[1].obj], -1, -1, -1 }, .offset = { d.plane[1].offset }, .stride = { d.plane[1].pitch } };
    GpuYuv *y = gpu_yuv_import(&py, &puv);
    if (!y) { dma_refuse(s, "import"); return; }
    int matrix = (int)(d.colors & 0xff), full = (d.colors & MC_FULL) != 0;
    if (s->amb->state == AC_PLAY) { gpu_yuv_free(y); return; }
    int r = gpu_yuv_convert(y, &s->vtex, matrix, full);
    if (!r) cache_frame(s, y, m->v[0], (int)d.w, (int)d.h, matrix, full);
    gpu_yuv_free(y);                    /* GL keeps what its queued draws need */
    if (r) { dma_refuse(s, "draw"); return; }
    if (!s->nv) { s->nv = 1; if (s->map) { munmap(s->map, s->map_len); s->map = NULL; s->map_len = 0; } }
    s->nv_slot = -1;
    nv_show(s, (int)d.w, (int)d.h, m->v[0]);
}

/* ---- a moving wallpaper's player ---- */

void media_ambient_new(MediaSrc *s, const char *path, int cache_mb, int fps_cap) {
    MAmb *a = calloc(1, sizeof *a);
    if (!a) return;
    a->path = strdup(path);
    a->cache_mb = cache_mb;
    a->fps_cap = fps_cap;
    a->helper_on = 1;
    a->hold = MLH_RUN;
    s->amb = a;
}

void media_ambient_free(MediaSrc *s) {
    MAmb *a = s->amb;
    if (!a) return;
    cache_drop(s, NULL);
    free(a->path);
    free(a);
    s->amb = NULL;
    gpu_yuv_free(s->yuv);
    s->yuv = NULL;
}

void media_nv_free(MediaSrc *s) {
    media_ambient_free(s);
    gpu_yuv_free(s->yuv);
    s->yuv = NULL;
    if (s->gtex == s->vtex) s->gtex = NULL;
    gpu_tex_free(s->vtex);
    s->vtex = NULL;
    media_nv_reset(s);
}

void media_ambient_box(MediaSrc *s) {
    MAmb *a = s->amb;
    if (!a) return;
    if (a->state == AC_PLAY || a->state == AC_REC) cache_drop(s, "display size changed");
    if (a->state == AC_REFUSED) a->state = AC_NONE;            /* a new size: try again */
    a->fails = 0;
}

static int respawn(MediaSrc *s) {
    MAmb *a = s->amb;
    char real[PATH_MAX];
    int in = media_open_input(a->path, real, sizeof real);
    if (in < 0) return -1;
    int render = media_render_fd();
    s->fmt_sent = want_format(s);
    char params[320];
    snprintf(params, sizeof params, "native=0\nloop=1\nautoplay=1\naudio=0\nhw=%d\nhwonly=1\nrate=%d\nhwaccel=%s\nnv12=%d\nfpscap=%d\n",
             render >= 0, s->rate > 0 ? s->rate : 100, media_hwaccel(), s->fmt_sent == MF_BGRA ? 0 : s->fmt_sent, a->fps_cap);
    int fds[3] = { in, -1, render };
    s->sock = media_spawn(MK_PLAY, params, fds, 3, &s->pid);
    close(in);
    if (render >= 0) close(render);
    if (s->sock < 0) return -1;
    a->helper_on = 1;
    s->sent_w = s->sent_h = 0;
    if (s->box_w > 0 && !mp_sendv(s->sock, MM_SIZE, 0, s->box_w, s->box_h, 0, 0)) { s->sent_w = s->box_w; s->sent_h = s->box_h; }
    if (a->resume_pos > 0) mp_sendv(s->sock, MM_SEEK, 0, a->resume_pos, 0, 0, 0);
    return 0;
}

static void apply_hold(MediaSrc *s, int hold, double now) {
    MAmb *a = s->amb;
    if (hold == a->hold && !(hold == MLH_RUN && a->stopped)) return;
    int was = a->hold;
    a->hold = hold;
    if (hold == MLH_STOP) {
        a->resume_pos = a->state == AC_PLAY && a->period > 0 ? ((int64_t)(a->paused_at - a->t0) % a->period) : s->pos;
        cache_drop(s, NULL);
        if (a->state == AC_REFUSED) a->state = AC_NONE;
        if (s->sock >= 0) { media_helper_stop(s); a->stopped = 1; fprintf(stderr, "media: moving wallpaper hidden for %d s: lord-media stopped, its cache freed\n", MLOOP_HIDDEN_STOP_MS / 1000); }
        return;
    }
    if (hold == MLH_PAUSE) {
        if (was == MLH_RUN) a->paused_at = now;
        helper_set(s, 0);
        return;
    }
    /* run */
    if (was != MLH_RUN && a->state == AC_PLAY) a->t0 += now - a->paused_at;
    if (a->stopped) {
        a->stopped = 0;
        if (respawn(s)) { fprintf(stderr, "media: moving wallpaper could not start again\n"); media_event(s->id, "closed", "{\"crashed\":false}"); }
        return;
    }
    if (a->state != AC_PLAY) helper_set(s, 1);
    if (s->gpu_drawn && s->gtex) media_src_present(s);
}

int media_ambient_control(MediaSrc *s, const char *cmd, double v) {
    MAmb *a = s->amb;
    if (!a) return 0;
    if (!strcmp(cmd, "play") || !strcmp(cmd, "pause")) { a->js_paused = !strcmp(cmd, "pause"); a->last_vis = 0; return 1; }
    if (!strcmp(cmd, "cacheMb") && v >= 0 && v <= 65536) {
        int mb = (int)v;
        if (mb != a->cache_mb) { a->cache_mb = mb; mem_budget = -1; if (!mb) cache_drop(s, "turned off"); if (a->state == AC_REFUSED) a->state = AC_NONE; a->fails = 0; }
        return 1;
    }
    if (!strcmp(cmd, "fpsCap") && (v == 0 || v == 15 || v == 30)) {
        int cap = (int)v;
        if (cap == a->fps_cap) return 1;
        /* a cache recorded under a lower cap lacks frames for a higher one */
        if (a->state == AC_PLAY && a->crate > 0 && (cap == 0 || cap > a->crate)) cache_drop(s, "frame rate raised");
        a->fps_cap = cap;
        if (s->sock >= 0) mp_sendv(s->sock, MM_FPSCAP, 0, cap, 0, 0, 0);
        return 1;
    }
    return 0;
}

/* ---- every pass of the main loop ---- */

static int view_hidden(const Node *n, int unseen) {
    if (unseen) return 1;
    float fx, fy, fw, fh;
    if (wl_fullscreen_box(&fx, &fy, &fw, &fh) && n->ax >= fx - 1 && n->ay >= fy - 1 && n->ax + n->w <= fx + fw + 1 && n->ay + n->h <= fy + fh + 1) return 1;
    return paint_node_hidden(n);
}

static double due_ms = -1;

void media_ambient_tick(int unseen) {
    double now = now_ms();
    due_ms = -1;
    int any_cache = 0;
    for (int i = 0; i < MEDIA_MAX_SRC; i++) {
        MediaSrc *s = media_src_at(i);
        if (!s || !s->amb) continue;
        MAmb *a = s->amb;
        if (a->state == AC_PLAY || a->state == AC_REC) any_cache = 1;
        if (now - a->last_vis >= VIS_MS || unseen) {
            a->last_vis = now;
            int any = 0, back = 0;
            for (int v = 0; v < MEDIA_VIEW_NODES; v++) {
                Node *n = media_src_view_at(s, v);
                int h = n ? view_hidden(n, unseen) : 1;
                if (n && !h) any = 1;
                if (s->view_hidden[v] && !h) back = 1;
                s->view_hidden[v] = (unsigned char)h;
            }
            if (!media_src_view_count(s)) any = !unseen;            /* not painted yet: it's about to be */
            if (any) a->hidden_since = 0;
            else if (!a->hidden_since) a->hidden_since = now;
            int hold = mloop_hold(any, a->js_paused, a->hidden_since ? now - a->hidden_since : 0);
            apply_hold(s, hold, now);
            if (back && s->gtex && s->gpu_drawn && !a->stopped) media_src_present(s);   /* the newest frame where it shows again */
        }
        if (a->state == AC_PLAY && a->hold == MLH_RUN) {
            cache_step(s, now);
            if (a->state == AC_PLAY) {
                double d = a->t0 + (double)a->next_t - now;
                if (d < 1) d = 1;
                if (due_ms < 0 || d < due_ms) due_ms = d;
            }
        }
        double vd = a->last_vis + VIS_MS - now;
        if (vd < 1) vd = 1;
        if (due_ms < 0 || vd < due_ms) due_ms = vd;
    }
    /* memory pressure and a lost context: the caches go */
    if (any_cache && now - mem_checked > MEM_MS) {
        int64_t mb = 0;
        for (int i = 0; i < MEDIA_MAX_SRC; i++) { MediaSrc *s = media_src_at(i); if (s && s->amb && s->amb->cache_mb > mb) mb = s->amb->cache_mb; }
        read_memory(mb);
        for (int i = 0; i < MEDIA_MAX_SRC; i++) {
            MediaSrc *s = media_src_at(i);
            if (!s || !s->amb || (s->amb->state != AC_PLAY && s->amb->state != AC_REC)) continue;
            int lost = s->amb->nframes > 0 && !gpu_pack_current(s->amb->frames[0]);
            if (mem_pressure || lost) cache_drop(s, lost ? "GPU context lost" : "GPU memory pressure");
        }
    }
}

double media_ambient_due(void) { return due_ms; }
