/* mview.c — how a `media` node is drawn (<MediaView media={id} />, see docs/MEDIA.md).
 *
 * The source can be a video frame (shared-memory slot with lord-media), a decoded
 * photo or a capture. Common case (video): lord-media already scaled the frame to the size of
 * the box, so it's copied as is (a single copy, the compositing one). With zoom, rotation or mirror
 * (Photos, Camera) an intermediate image of the visible size is built, with bilinear interpolation
 * and halvings (mipmaps) for large photos; it's reused as long as nothing
 * changes (moving the pointer doesn't recompute it). */
#include "media_int.h"
#include "../ui/ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void media_view_reset(MediaSrc *s) {
    unsigned gen = s->view.gen + 1;
    s->view = (MediaView){ .fit = 0, .zoom = 0, .cx = 0.5f, .cy = 0.5f, .rot = 0, .mirror = 0,
                           .bg = (s->kind == SK_PLAYER || s->kind == SK_CAMERA) ? 0xff000000u : 0, .gen = gen };
}

static const uint32_t *pixels_of(MediaSrc *s, int *w, int *h) {
    if (s->map && s->ring_slots) {
        if (s->cur_slot < 0) return NULL;
        *w = s->ring_w;
        *h = s->ring_h;
        return (const uint32_t *)(s->map + (size_t)s->cur_slot * (size_t)s->ring_w * s->ring_h * 4);
    }
    if (!s->px) return NULL;
    *w = s->w;
    *h = s->h;
    return s->px;
}

static uint32_t with_opacity(uint32_t argb, float op) {
    uint32_t a = (uint32_t)((argb >> 24) * op + 0.5f);
    return a << 24 | (argb & 0xffffff);
}

/* Halvings (2x2 average) for photos: mip[0] = 1/2, mip[1] = 1/4… */
static unsigned mip_seq_of[MEDIA_MAX_SRC];
static void build_mips(MediaSrc *s, const uint32_t *px, int w, int h) {
    for (int i = 0; i < s->nmip; i++) free(s->mip[i]);
    s->nmip = 0;
    const uint32_t *src = px;
    int sw = w, sh = h;
    while (s->nmip < 8 && sw >= 64 && sh >= 64) {
        int dw = sw / 2, dh = sh / 2;
        uint32_t *d = malloc((size_t)dw * dh * 4);
        if (!d) break;
        for (int y = 0; y < dh; y++) {
            const uint32_t *r0 = src + (size_t)(y * 2) * sw, *r1 = r0 + sw;
            for (int x = 0; x < dw; x++) {
                uint32_t a = r0[x * 2], b = r0[x * 2 + 1], c = r1[x * 2], e = r1[x * 2 + 1];
                uint32_t o = 0;
                for (int k = 0; k < 32; k += 8)
                    o |= ((((a >> k) & 255) + ((b >> k) & 255) + ((c >> k) & 255) + ((e >> k) & 255) + 2) / 4) << k;
                d[(size_t)y * dw + x] = o;
            }
        }
        s->mip[s->nmip] = d;
        s->mip_w[s->nmip] = dw;
        s->mip_h[s->nmip] = dh;
        s->nmip++;
        src = d;
        sw = dw;
        sh = dh;
    }
}

static inline uint32_t bilinear(const uint32_t *px, int w, int h, float u, float v) {
    if (u < 0) u = 0;
    if (v < 0) v = 0;
    if (u > w - 1) u = (float)(w - 1);
    if (v > h - 1) v = (float)(h - 1);
    int x0 = (int)u, y0 = (int)v;
    int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    uint32_t fx = (uint32_t)((u - x0) * 256), fy = (uint32_t)((v - y0) * 256);
    uint32_t a = px[(size_t)y0 * w + x0], b = px[(size_t)y0 * w + x1], c = px[(size_t)y1 * w + x0], d = px[(size_t)y1 * w + x1];
    uint32_t out = 0;
    for (int k = 0; k < 32; k += 8) {
        uint32_t top = ((a >> k) & 255) * (256 - fx) + ((b >> k) & 255) * fx;
        uint32_t bot = ((c >> k) & 255) * (256 - fx) + ((d >> k) & 255) * fx;
        out |= ((top * (256 - fy) + bot * fy + 32768) >> 16) << k;
    }
    return out;
}

void media_paint(Canvas *c, int handle, int node, float x, float y, float w, float h, float radius, float op) {
    MediaSrc *s = media_src_get(handle);
    if (!s || w < 1 || h < 1) return;
    s->node = node;
    int bw = (int)lroundf(w), bh = (int)lroundf(h);
    s->box_w = bw;
    s->box_h = bh;
    /* the helper scales to the box size: it's notified when it changes */
    if ((s->kind == SK_PLAYER || s->kind == SK_CAMERA) && s->sock >= 0 && (bw != s->sent_w || bh != s->sent_h)) {
        int rw = (s->view.rot % 180) ? bh : bw, rh = (s->view.rot % 180) ? bw : bh;
        if (s->view.zoom > 0 || s->view.fit) { rw = 0; rh = 0; }      /* with zoom: original size */
        if (rw > 0 && mp_sendv(s->sock, MM_SIZE, 0, rw, rh, 0, 0) == 0) { s->sent_w = bw; s->sent_h = bh; }
    }
    Rect old = canvas_push_clip(c, x, y, w, h);
    int sw, sh;
    const uint32_t *px = pixels_of(s, &sw, &sh);
    if (!px) {
        if (s->view.bg >> 24) canvas_rrect(c, x, y, w, h, radius, with_opacity(s->view.bg, op));
        canvas_set_clip(c, old);
        return;
    }
    int rot = s->view.rot;
    int rw = rot % 180 ? sh : sw, rh = rot % 180 ? sw : sh;
    float sc;
    if (s->view.zoom > 0) sc = s->view.zoom;
    else if (s->view.fit == 1) sc = fmaxf(w / rw, h / rh);
    else sc = fminf(w / rw, h / rh);
    /* video: the frame already comes at the right size; if it differs by one pixel from rounding, it's copied anyway */
    int ring = s->map && s->ring_slots;
    if (ring && !rot && s->view.zoom <= 0 && !s->view.fit && (abs(sw - bw) <= 2 || abs(sh - bh) <= 2) && sw <= bw + 2 && sh <= bh + 2) sc = 1;
    float dw = rw * sc, dh = rh * sc, dx, dy;
    if (s->view.zoom > 0) { dx = x + w / 2 - s->view.cx * dw; dy = y + h / 2 - s->view.cy * dh; }
    else { dx = x + (w - dw) / 2; dy = y + (h - dh) / 2; }
    int opaque = ring || s->opaque;      /* captures set it; a picture with transparency opened in the markup editor doesn't */
    /* background: only the bands the image doesn't cover (a video frame doesn't repaint beneath itself) */
    if (s->view.bg >> 24) {
        uint32_t bg = with_opacity(s->view.bg, op);
        int covers = opaque && op >= 0.999f;
        if (!covers || radius > 0) canvas_rrect(c, x, y, w, h, radius, bg);
        else {
            float ix0 = roundf(fmaxf(dx, x)), iy0 = roundf(fmaxf(dy, y)), ix1 = roundf(fminf(dx + dw, x + w)), iy1 = roundf(fminf(dy + dh, y + h));
            if (ix1 <= ix0 || iy1 <= iy0) canvas_rrect(c, x, y, w, h, 0, bg);
            else {
                if (iy0 > y) canvas_rrect(c, x, y, w, iy0 - y, 0, bg);
                if (iy1 < y + h) canvas_rrect(c, x, iy1, w, y + h - iy1, 0, bg);
                if (ix0 > x) canvas_rrect(c, x, iy0, ix0 - x, iy1 - iy0, 0, bg);
                if (ix1 < x + w) canvas_rrect(c, ix1, iy0, x + w - ix1, iy1 - iy0, 0, bg);
            }
        }
    }
    if (!rot && !s->view.mirror && fabsf(sc - 1) < 0.001f && !s->view.bright && !s->view.contrast) {
        canvas_blit(c, px, sw, sh, (int)lroundf(dx), (int)lroundf(dy), 0, op, opaque);
        canvas_set_clip(c, old);
        return;
    }
    /* visible part of the image inside the box */
    int vx0 = (int)floorf(fmaxf(dx, x)), vy0 = (int)floorf(fmaxf(dy, y));
    int vx1 = (int)ceilf(fminf(dx + dw, x + w)), vy1 = (int)ceilf(fminf(dy + dh, y + h));
    int vw = vx1 - vx0, vh = vy1 - vy0;
    if (vw <= 0 || vh <= 0 || vw > 8192 || vh > 8192) { canvas_set_clip(c, old); return; }
    float key[4] = { dx - vx0, dy - vy0, sc, (float)(vw * 65536 + vh) };
    if (!s->cache || s->cache_w != vw || s->cache_h != vh || s->cache_seq != s->frame_seq || s->cache_gen != s->view.gen ||
        memcmp(key, s->cache_box, sizeof key)) {
        if (s->cache_w * s->cache_h < vw * vh) { free(s->cache); s->cache = malloc((size_t)vw * vh * 4); }
        if (!s->cache) { canvas_set_clip(c, old); return; }
        s->cache_w = vw;
        s->cache_h = vh;
        s->cache_seq = s->frame_seq;
        s->cache_gen = s->view.gen;
        memcpy(s->cache_box, key, sizeof key);
        /* reduced photos: the matching mipmap level */
        const uint32_t *lp = px;
        int lw = sw, lh = sh;
        float lf = 1;
        if (!ring && sc < 0.5f) {
            if (s->nmip == 0 || mip_seq_of[s->id % MEDIA_MAX_SRC] != s->frame_seq) { build_mips(s, px, sw, sh); mip_seq_of[s->id % MEDIA_MAX_SRC] = s->frame_seq; }
            for (int l = 0; l < s->nmip && sc * lf < 0.5f; l++) { lp = s->mip[l]; lw = s->mip_w[l]; lh = s->mip_h[l]; lf *= 2; }
        }
        float inv = 1.0f / sc;
        for (int j = 0; j < vh; j++) {
            uint32_t *o = s->cache + (size_t)j * vw;
            float vp = (vy0 + j + 0.5f - dy) * inv;               /* coordinate in the rotated image */
            for (int i = 0; i < vw; i++) {
                float up = (vx0 + i + 0.5f - dx) * inv;
                if (s->view.mirror) up = rw - up;
                float u, v;
                switch (rot) {
                case 90: u = vp; v = sh - up; break;
                case 180: u = sw - up; v = sh - vp; break;
                case 270: u = sw - vp; v = up; break;
                default: u = up; v = vp;
                }
                o[i] = bilinear(lp, lw, lh, u / lf - 0.5f, v / lf - 0.5f);
            }
        }
        if (s->view.bright || s->view.contrast) {
            /* same calculation as lord-media's editing (lm_job.c): contrast around mid gray */
            uint8_t lut[256];
            double k = (100.0 + s->view.contrast) / 100.0, bo = s->view.bright * 1.28;
            k *= k;
            for (int i = 0; i < 256; i++) { double v = (i - 128.0) * k + 128.0 + bo; lut[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5); }
            for (size_t i = 0; i < (size_t)vw * vh; i++) {
                uint32_t q = s->cache[i];
                s->cache[i] = (q & 0xff000000u) | (uint32_t)lut[(q >> 16) & 255] << 16 | (uint32_t)lut[(q >> 8) & 255] << 8 | lut[q & 255];
            }
        }
    }
    canvas_blit(c, s->cache, vw, vh, vx0, vy0, 0, op, opaque);
    canvas_set_clip(c, old);
}

/* The frame of a video or of the camera covers its whole box (the bands are painted opaque): if everything
 * that changed is inside, what's beneath that node isn't visible. */
int media_cover_node(float x0, float y0, float x1, float y1) {
    if (x1 <= x0 || y1 <= y0) return 0;
    for (int i = 0; i < MEDIA_MAX_SRC; i++) {
        MediaSrc *s = media_src_at(i);
        if (!s) continue;
        if (!s->node || (s->kind != SK_PLAYER && s->kind != SK_CAMERA) || (s->view.bg >> 24) != 255 || !s->map) continue;
        Node *n = node_get(s->node);
        if (!n || n->type != NODE_MEDIA || n->term != s->id || n->s.opacity < 1 || n->s.radius > 0) continue;
        /* the box must be visible and without transparency in the ancestors */
        int ok = 1;
        for (Node *p = n; p && ok; p = p->parent) if (p->s.display_none || p->s.opacity < 1) ok = 0;
        if (!ok) continue;
        float nx0 = floorf(n->ax), ny0 = floorf(n->ay), nx1 = ceilf(n->ax + n->w), ny1 = ceilf(n->ay + n->h);
        if (x0 >= nx0 && y0 >= ny0 && x1 <= nx1 && y1 <= ny1) return n->id;
    }
    return 0;
}
