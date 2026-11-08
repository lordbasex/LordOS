/* capture.c — screenshots, recording and what the remote desktop sees, with the protected
 * windows in black (see docs/MEDIA.md, «Protected windows»).
 *
 * A window is marked protected with wm.setProtected(id, true) (the SDK sets `protected` on the
 * node of its content). On the local screen it looks normal; in any capture it's painted
 * black:
 *   - screenshot: the whole tree is repainted onto a separate canvas in «capture
 *     mode» (paint_capture), where a protected node is a black rectangle. It respects the order
 *     of the windows: whatever is above the protected one is visible.
 *   - recording and remote desktop: if there is any protected window, a second
 *     canvas in capture mode is kept, repainted only in the region that changed in each frame; if there
 *     is none, the frame is used as is (no extra cost).
 * Recording (like the remote desktop) composites on the CPU, so the frame is in memory. */
#include "media_int.h"
#include "imgops.h"
#include "../ui/ui.h"
#include "../wl/wl.h"
#include "../hw/display.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static Canvas cap;                  /* scene in capture mode (persistent) */
static int cap_valid, last_remote;
static uint32_t *fs_px;             /* cap + full screen (temporary) */
static size_t fs_cap;
static MediaSrc *rec;               /* screen recording in progress (one at a time) */
static int rec_pending;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int count_protected(const Node *n) {
    if (!n || n->type == NODE_RAW || n->s.display_none) return 0;
    if (n->protect) return 1;
    int c = 0;
    for (const Node *k = n->first; k; k = k->next) c += count_protected(k);
    return c;
}

int media_protected_count(void) { return count_protected(ui.root); }

static int ensure_cap(int w, int h) {
    if (cap.px && cap.w == w && cap.h == h) return 0;
    free(cap.px);
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) { cap.px = NULL; return -1; }
    canvas_init(&cap, px, w, h, w);
    cap_valid = 0;
    return 0;
}

static void repaint_cap(Rect clip) {
    canvas_set_clip(&cap, clip);
    canvas_clear(&cap, 0x000000);
    paint_capture(&cap, ui.root);
    canvas_set_clip(&cap, (Rect){ 0, 0, cap.w, cap.h });
}

void media_scene_painted(const Canvas *scene) {
    if (!(rec || last_remote) || !media_protected_count()) { cap_valid = 0; return; }
    if (ensure_cap(scene->w, scene->h)) return;
    if (!cap_valid) { repaint_cap((Rect){ 0, 0, cap.w, cap.h }); cap_valid = 1; }
    else repaint_cap(scene->clip);
}

/* ---- cursor in the recording (the LordOS arrow or the app's image) ---- */
static const char *const arrow[] = {
    "X", "XX", "XoX", "XooX", "XoooX", "XooooX", "XoooooX", "XooooooX", "XoooooooX",
    "XooooooooX", "XoooooooooX", "XooooooooooX", "XooooooXXXXX", "XoooXooX",
    "XooX XooX", "XoX  XooX", "XX    XooX", "X     XooX", "       XX", NULL,
};

static void feed_recording(const Canvas *src, const MediaCursor *cur) {
    MediaSrc *s = rec;
    if (!s || !s->in_map || s->sock < 0) return;
    double t = now_ms();
    double gap = 1000.0 / (s->rec_fps > 0 ? s->rec_fps : 30);
    if (t - s->rec_last < gap - 2) { rec_pending = 1; return; }
    if (src->w != s->in_w || src->h != s->in_h) { s->rec_dropped++; return; }    /* the screen changed: skipped */
    int slot = -1;
    for (int i = 0; i < s->in_slots; i++) if (!s->in_busy[i]) { slot = i; break; }
    if (slot < 0) { s->rec_dropped++; rec_pending = 1; return; }              /* the encoder is behind */
    uint32_t *dst = (uint32_t *)(s->in_map + (size_t)slot * s->in_w * s->in_h * 4);
    for (int y = 0; y < src->h; y++) memcpy(dst + (size_t)y * s->in_w, src->px + (size_t)y * src->stride, (size_t)src->w * 4);
    if (cur) {
        Canvas c;
        canvas_init(&c, dst, s->in_w, s->in_h, s->in_w);
        if (cur->mode == 1 && cur->px) canvas_blit(&c, cur->px, cur->w, cur->h, cur->x - cur->hx, cur->y - cur->hy, 0, 1, 0);
        else if (cur->mode == 0) canvas_bitmap(&c, cur->x, cur->y, arrow, 0xff000000u, 0xffffffffu);
    }
    if (mp_sendv(s->sock, MM_INFRAME, (uint32_t)slot, (int64_t)(t - s->rec_start), 0, 0, 0)) { s->rec_dropped++; return; }
    s->in_busy[slot] = 1;
    s->rec_last = t;
    s->rec_frames++;
    rec_pending = 0;
}

const Canvas *media_composed(const Canvas *out, int remote, const MediaCursor *cur) {
    if (remote != last_remote) { last_remote = remote; cap_valid = 0; }
    const Canvas *src = out;
    if ((remote || rec) && media_protected_count() && !ensure_cap(out->w, out->h)) {
        if (!cap_valid) { repaint_cap((Rect){ 0, 0, cap.w, cap.h }); cap_valid = 1; }
        src = &cap;
        if (wl_fullscreen_active()) {
            size_t need = (size_t)cap.w * cap.h;
            if (need > fs_cap) { free(fs_px); fs_px = malloc(need * 4); fs_cap = fs_px ? need : 0; }
            if (fs_px) {
                static Canvas tmp;
                canvas_init(&tmp, fs_px, cap.w, cap.h, cap.w);
                canvas_copy(&tmp, &cap);
                paint_capture_mode = 1;
                wl_paint_fullscreen(&tmp);
                paint_capture_mode = 0;
                src = &tmp;
            }
        }
    }
    if (rec) feed_recording(src, cur);
    return src;
}

int media_wants_frame(void) {
    return rec && rec_pending && now_ms() - rec->rec_last >= 1000.0 / (rec->rec_fps > 0 ? rec->rec_fps : 30);
}

int media_needs_cpu(void) { return rec != NULL; }

int media_memfd_from_pixels(const uint32_t *px, int w, int h) {
    size_t n = (size_t)w * h * 4;
    int fd = memfd_create("lordos-capture", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)n)) { close(fd); return -1; }
    void *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return -1; }
    memcpy(m, px, n);
    munmap(m, n);
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    return fd;
}

/* ---- screenshot ---- */

/* The whole screen is the main display at its own resolution (sharp at 150 %), or every display
 * (Settings → Displays, "capture all": the global space at 100 %). A region (x, y, w, h) is logical. */
static const DisplayOut *capture_main(void) {
    const DisplayPlan *p = display_current();
    if (p->n < 1 || (display_capture_all() && p->n > 1 && !p->mirror)) return NULL;
    return &p->o[p->main];
}

/* Size of the frames main.c feeds the recording and the remote desktop (main display or all). */
void media_capture_size(int *w, int *h) {
    const DisplayOut *m = capture_main();
    *w = (m ? m->v.pw : (int)ui.width) & ~1;
    *h = (m ? m->v.ph : (int)ui.height) & ~1;
}

MediaSrc *media_screenshot(const char *app, int x, int y, int w, int h, uint32_t display, int until) {
    int W = (int)ui.width, H = (int)ui.height;
    if (W <= 0 || H <= 0 || !ui.root) return NULL;
    /* one display asked for (Screenshots: a file per display, an area on any of them) */
    const DisplayPlan *plan = display_current();
    const DisplayOut *m = NULL;
    for (int i = 0; display && i < plan->n; i++) if (plan->o[i].conn == display) m = &plan->o[i];
    if (display && !m) return NULL;
    if (!display && (w <= 0 || h <= 0)) m = capture_main();
    if (m) { W = m->v.pw; H = m->v.ph; }
    uint32_t *full = malloc((size_t)W * H * 4);
    if (!full) return NULL;
    Canvas c;
    canvas_init(&c, full, W, H, W);
    canvas_clear(&c, 0x000000);
    PaintView keep = paint_view;
    if (m) paint_view = (PaintView){ (float)m->v.x, (float)m->v.y, (float)m->v.k, (float)m->v.dx, (float)m->v.dy, -1 };
    int ok = 1;
    if (until) ok = paint_capture_until(&c, ui.root, until);     /* a window alone: nothing above it */
    else {
        paint_capture(&c, ui.root);
        paint_capture_mode = 1;
        wl_paint_fullscreen(&c);
        paint_capture_mode = 0;
    }
    paint_view = keep;
    if (!ok) { free(full); return NULL; }       /* not a node on the screen: the caller crops a capture instead */
    if (w <= 0 || h <= 0) { x = 0; y = 0; w = W; h = H; }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w < 1 || h < 1) { free(full); return NULL; }
    MediaSrc *s = media_src_new(SK_CAPTURE, app);
    if (!s) { free(full); return NULL; }
    if (x == 0 && y == 0 && w == W && h == H) s->px = full;
    else {
        s->px = malloc((size_t)w * h * 4);
        if (s->px) for (int j = 0; j < h; j++) memcpy(s->px + (size_t)j * w, full + (size_t)(y + j) * W + x, (size_t)w * 4);
        free(full);
        if (!s->px) { s->used = 0; return NULL; }
    }
    s->w = w;
    s->h = h;
    s->opaque = 1;
    for (size_t i = 0; i < (size_t)w * h; i++) s->px[i] |= 0xff000000u;      /* no holes (GPU path) */
    s->frame_seq = 1;
    return s;
}

MediaSrc *media_crop(MediaSrc *src, int x, int y, int w, int h) {
    if (!src || !src->px || (src->kind != SK_CAPTURE && src->kind != SK_IMAGE)) return NULL;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > src->w) w = src->w - x;
    if (y + h > src->h) h = src->h - y;
    if (w < 1 || h < 1) return NULL;
    MediaSrc *s = media_src_new(SK_CAPTURE, src->app);
    if (!s) return NULL;
    s->px = malloc((size_t)w * h * 4);
    if (!s->px) { s->used = 0; return NULL; }
    for (int j = 0; j < h; j++) memcpy(s->px + (size_t)j * w, src->px + (size_t)(y + j) * src->w + x, (size_t)w * 4);
    s->w = w;
    s->h = h;
    s->opaque = src->opaque;           /* a picture with transparency keeps it */
    s->frame_seq = 1;
    return s;
}

/* The image editor's resize and quarter turns (imgops.c): always a new capture source, so the
 * editor keeps the previous one for undo. */
static MediaSrc *new_pixels(const MediaSrc *src, int w, int h) {
    if (w < 1 || h < 1 || w > MP_MAX_DIM || h > MP_MAX_DIM) return NULL;
    MediaSrc *s = media_src_new(SK_CAPTURE, src->app);
    if (!s) return NULL;
    s->px = malloc((size_t)w * h * 4);
    if (!s->px) { s->used = 0; return NULL; }
    s->w = w;
    s->h = h;
    s->opaque = src->opaque;
    s->frame_seq = 1;
    return s;
}

MediaSrc *media_resize(MediaSrc *src, int w, int h) {
    if (!src || !src->px || (src->kind != SK_CAPTURE && src->kind != SK_IMAGE)) return NULL;
    MediaSrc *s = new_pixels(src, w, h);
    if (!s) return NULL;
    if (img_resize(src->px, src->w, src->h, s->px, w, h)) { free(s->px); s->px = NULL; s->used = 0; return NULL; }
    return s;
}

MediaSrc *media_rotate(MediaSrc *src, int turns) {
    if (!src || !src->px || (src->kind != SK_CAPTURE && src->kind != SK_IMAGE)) return NULL;
    int w, h;
    img_rotated_size(src->w, src->h, turns, &w, &h);
    MediaSrc *s = new_pixels(src, w, h);
    if (!s) return NULL;
    img_rotate(src->px, src->w, src->h, turns, s->px);
    return s;
}

/* ---- screen recording ---- */

int media_screenrec_start(MediaSrc *s, int fps) {
    if (rec) return -1;
    int W, H;
    media_capture_size(&W, &H);
    int slots = 3;
    size_t n = (size_t)W * H * 4 * slots;
    int fd = memfd_create("lordos-screen", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, (off_t)n) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) { if (fd >= 0) close(fd); return -1; }
    void *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return -1; }
    s->in_map = m;
    s->in_len = n;
    s->in_fd = fd;
    s->in_w = W;
    s->in_h = H;
    s->in_slots = slots;
    memset(s->in_busy, 0, sizeof s->in_busy);
    s->rec_fps = fps > 0 && fps <= 60 ? fps : 30;
    s->rec_start = now_ms();
    s->rec_last = 0;
    int64_t v[6] = { slots, W, H, 0, 0, 0 };
    if (mp_send(s->sock, MM_INRING, 0, v, NULL, 0, &fd, 1)) { munmap(m, n); close(fd); s->in_map = NULL; s->in_fd = -1; return -1; }
    rec = s;
    rec_pending = 1;
    cap_valid = 0;
    /* the next full frame (and on the CPU: main.c turns off the GPU while recording) */
    damage_add((Box){ 0, 0, ui.width, ui.height });
    ui.dirty = 1;
    return 0;
}

int media_framerec_start(MediaSrc *s, int W, int H, int fps) {
    W &= ~1; H &= ~1;
    if (W < 16 || H < 16 || W > 8192 || H > 8192) return -1;
    int slots = 3;
    size_t n = (size_t)W * H * 4 * slots;
    int fd = memfd_create("lordos-frames", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, (off_t)n) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) { if (fd >= 0) close(fd); return -1; }
    void *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return -1; }
    s->in_map = m; s->in_len = n; s->in_fd = fd; s->in_w = W; s->in_h = H; s->in_slots = slots;
    memset(s->in_busy, 0, sizeof s->in_busy);
    s->rec_fps = fps > 0 && fps <= 60 ? fps : 30;
    s->rec_start = now_ms();
    s->rec_last = 0;
    int64_t v[6] = { slots, W, H, 0, 0, 0 };
    if (mp_send(s->sock, MM_INRING, 0, v, NULL, 0, &fd, 1)) { munmap(m, n); close(fd); s->in_map = NULL; s->in_fd = -1; return -1; }
    return 0;
}

int media_framerec_feed(MediaSrc *s, const uint32_t *px, int w, int h) {
    if (!s || !s->in_map || s->sock < 0 || !px || w < 1 || h < 1) return 0;
    double t = now_ms();
    if (t - s->rec_last < 1000.0 / (s->rec_fps > 0 ? s->rec_fps : 30) - 2) return 0;
    int slot = -1;
    for (int i = 0; i < s->in_slots; i++) if (!s->in_busy[i]) { slot = i; break; }
    if (slot < 0) { s->rec_dropped++; return 0; }
    uint32_t *dst = (uint32_t *)(s->in_map + (size_t)slot * s->in_w * s->in_h * 4);
    int cw = w < s->in_w ? w : s->in_w, ch = h < s->in_h ? h : s->in_h;
    if (cw != s->in_w || ch != s->in_h) memset(dst, 0, (size_t)s->in_w * s->in_h * 4);
    for (int y = 0; y < ch; y++) {
        uint32_t *d = dst + (size_t)y * s->in_w;
        const uint32_t *q = px + (size_t)y * w;
        for (int x = 0; x < cw; x++) d[x] = q[x] | 0xff000000u;      /* x8r8g8b8: the fourth byte isn't alpha */
    }
    if (mp_sendv(s->sock, MM_INFRAME, (uint32_t)slot, (int64_t)(t - s->rec_start), 0, 0, 0)) { s->rec_dropped++; return 0; }
    s->in_busy[slot] = 1;
    s->rec_last = t;
    s->rec_frames++;
    return 1;
}

void media_capture_src_closed(MediaSrc *s) {
    if (rec != s) return;
    rec = NULL;
    rec_pending = 0;
    fprintf(stderr, "media: screen recording: %ld frames, %ld dropped\n", s->rec_frames, s->rec_dropped);
}
