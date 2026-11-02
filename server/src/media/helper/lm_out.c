/* lm_out.c — frame output from axia-media to axia-server (player and camera).
 *
 * Three slots in a sealed memfd (MM_RING). Each frame is scaled only once to the size at
 * which it's shown (the box axia-server announces with MM_SIZE, fitted without distortion) and
 * written into a free slot; MM_FRAME says which one. axia-server answers MM_ACK when it starts
 * showing it: from that moment the ones sent before are free. If there's no free slot
 * (axia-server behind), the frame is dropped instead of waiting. */
#include "lm.h"

#include <math.h>
#include <string.h>
#include <sys/mman.h>

#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

typedef struct {
    int gen, w, h, fd;
    uint8_t *map;
    size_t slot_bytes;
    int server_cur;                 /* slot axia-server is showing (ACK) */
    int64_t pending[MP_RING_SLOTS]; /* send order of the ones sent without ACK (0 = free) */
    int64_t seq;
    struct SwsContext *sws;
    int sw, sh, sfmt, sws_dw, sws_dh;
    int box_w, box_h, native;
    AVFrame *last;                  /* last presented frame (to redraw when the size changes) */
    int64_t last_pts;
} Out;

static Out O = { .fd = -1, .server_cur = -1 };

void lm_out_init(int native) { O.native = native; }
int  lm_out_has_box(void) { return O.box_w > 0; }
int  lm_out_has_last(void) { return O.last != NULL; }
const AVFrame *lm_out_last(void) { return O.last; }

static void out_size(const AVFrame *f, int *w, int *h) {
    int sw = f->width, sh = f->height;
    AVRational sar = f->sample_aspect_ratio;
    double dw = sar.num > 0 && sar.den > 0 ? sw * av_q2d(sar) : sw;
    if (O.native || O.box_w <= 0 || O.box_h <= 0) {
        *w = (int)lround(dw);
        *h = sh;
    } else {
        double s = fmin(O.box_w / dw, (double)O.box_h / sh);
        *w = (int)lround(dw * s);
        *h = (int)lround(sh * s);
    }
    if (*w > MP_MAX_DIM) *w = MP_MAX_DIM;
    if (*h > MP_MAX_DIM) *h = MP_MAX_DIM;
    if (*w < 2) *w = 2;
    if (*h < 2) *h = 2;
    *w &= ~1;
    *h &= ~1;
}

static int ring_make(int w, int h) {
    if (O.map) { munmap(O.map, O.slot_bytes * MP_RING_SLOTS); close(O.fd); }
    O.map = NULL;
    O.fd = -1;
    O.slot_bytes = (size_t)w * h * 4;
    void *m;
    O.fd = lm_memfd(O.slot_bytes * MP_RING_SLOTS, &m, "axia-media-frames");
    if (O.fd < 0) return -1;
    O.map = m;
    O.w = w;
    O.h = h;
    O.gen = (O.gen + 1) & 0xffffff;
    O.server_cur = -1;
    memset(O.pending, 0, sizeof O.pending);
    int64_t v[6] = { O.gen, w, h, MP_RING_SLOTS, 0, 0 };
    mp_send(LM_SOCK, MM_RING, 0, v, NULL, 0, &O.fd, 1);
    return 0;
}

static int free_slot(void) {
    for (int i = 0; i < MP_RING_SLOTS; i++) if (i != O.server_cur && !O.pending[i]) return i;
    return -1;
}

void lm_out_ack(uint32_t id) {
    int gen = (int)(id >> 8), slot = (int)(id & 0xff);
    if (gen != O.gen || slot >= MP_RING_SLOTS) return;
    int64_t s = O.pending[slot];
    if (!s) return;
    for (int i = 0; i < MP_RING_SLOTS; i++) if (O.pending[i] && O.pending[i] <= s) O.pending[i] = 0;
    O.server_cur = slot;
}

static void set_colors(struct SwsContext *s, const AVFrame *f) {
    int cs = f->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : f->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020
           : (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720) ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
    int full = f->color_range == AVCOL_RANGE_JPEG;
    sws_setColorspaceDetails(s, sws_getCoefficients(cs), full, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
}

int lm_out_present(AVFrame *f, int64_t pts) {
    AVFrame *sw = f, *tmp = NULL;
    if (f->format == AV_PIX_FMT_VAAPI || f->format == AV_PIX_FMT_DRM_PRIME) {
        tmp = av_frame_alloc();
        if (av_hwframe_transfer_data(tmp, f, 0) < 0) { av_frame_free(&tmp); return -1; }
        av_frame_copy_props(tmp, f);
        sw = tmp;
    }
    int w, h;
    out_size(sw, &w, &h);
    if ((w != O.w || h != O.h || !O.map) && ring_make(w, h)) { av_frame_free(&tmp); return -1; }
    int slot = free_slot();
    if (slot < 0) { av_frame_free(&tmp); return 1; }
    if (!O.sws || O.sw != sw->width || O.sh != sw->height || O.sfmt != sw->format || O.sws_dw != w || O.sws_dh != h) {
        sws_freeContext(O.sws);
        O.sws = sws_alloc_context();
        av_opt_set_int(O.sws, "srcw", sw->width, 0);
        av_opt_set_int(O.sws, "srch", sw->height, 0);
        av_opt_set_int(O.sws, "src_format", sw->format, 0);
        av_opt_set_int(O.sws, "dstw", w, 0);
        av_opt_set_int(O.sws, "dsth", h, 0);
        av_opt_set_int(O.sws, "dst_format", AV_PIX_FMT_BGRA, 0);
        av_opt_set_int(O.sws, "sws_flags", SWS_BILINEAR, 0);
        av_opt_set_int(O.sws, "threads", 2, 0);
        if (sws_init_context(O.sws, NULL, NULL) < 0) { sws_freeContext(O.sws); O.sws = NULL; av_frame_free(&tmp); return -1; }
        set_colors(O.sws, sw);
        O.sw = sw->width; O.sh = sw->height; O.sfmt = sw->format; O.sws_dw = w; O.sws_dh = h;
    }
    uint8_t *dst[4] = { O.map + O.slot_bytes * (size_t)slot, NULL, NULL, NULL };
    int ls[4] = { w * 4, 0, 0, 0 };
    sws_scale(O.sws, (const uint8_t *const *)sw->data, sw->linesize, 0, sw->height, dst, ls);
    O.pending[slot] = ++O.seq;
    int64_t v[6] = { pts, w, h, 0, 0, 0 };
    mp_send(LM_SOCK, MM_FRAME, (uint32_t)(O.gen << 8 | slot), v, NULL, 0, NULL, 0);
    if (O.last != f && O.last != sw) {
        av_frame_free(&O.last);
        O.last = av_frame_clone(sw);
    }
    O.last_pts = pts;
    av_frame_free(&tmp);
    return 0;
}

/* The box changed: 1 if it really changed. */
int lm_out_box(int w, int h) {
    if (w <= 0 || h <= 0 || w > MP_MAX_DIM || h > MP_MAX_DIM || (w == O.box_w && h == O.box_h)) return 0;
    O.box_w = w;
    O.box_h = h;
    return 1;
}

/* Presents the last frame again (with the new size). 0 if there was one. */
int lm_out_redraw(void) {
    if (!O.last) return -1;
    AVFrame *f = av_frame_clone(O.last);
    int r = lm_out_present(f, O.last_pts);
    av_frame_free(&f);
    return r < 0 ? -1 : 0;
}
