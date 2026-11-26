/* lm_out.c — frame output from lord-media to lord-server (player and camera).
 *
 * Three slots in a sealed memfd (MM_RING). Each frame is scaled only once to the size at
 * which it's shown (the box lord-server announces with MM_SIZE, fitted without distortion) and
 * written into a free slot; MM_FRAME says which one. lord-server answers MM_ACK when it starts
 * showing it: from that moment the ones sent before are free. If there's no free slot
 * (lord-server behind), the frame is dropped instead of waiting.
 *
 * Two formats (proto.h MF_*): BGRA, converted here with the video's colour matrix, or (while lord-server's GPU composes,
 * MM_FORMAT) NV12, the decoder's own 4:2:0 planes: no RGB conversion here, 1.5 bytes a pixel instead of 4, and lord-server
 * converts it in a shader. An NV12 frame is never upscaled: at most the box's size, else the video's own (the GPU scales it
 * up); a decoder frame already NV12 at that size is copied plane by plane without swscale. */
#include "lm.h"

#include <math.h>
#include <string.h>
#include <sys/mman.h>

#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <unistd.h>
#ifdef LM_VAAPI
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <libavutil/hwcontext_vaapi.h>
#endif

typedef struct {
    int gen, w, h, fd, fmt, colors;
    int want_fmt;                   /* MM_FORMAT: MF_BGRA or MF_NV12 */
    uint8_t *map;
    size_t slot_bytes;
    int server_cur;                 /* slot lord-server is showing (ACK) */
    int64_t pending[MP_RING_SLOTS]; /* send order of the ones sent without ACK (0 = free) */
    int64_t seq;
    struct SwsContext *sws;
    int sw, sh, sfmt, sws_dw, sws_dh, sws_df;
    int box_w, box_h, native;
    AVFrame *last;                  /* last presented frame (to redraw when the size changes) */
    int64_t last_pts;
} Out;

static Out O = { .fd = -1, .server_cur = -1 };

void lm_out_init(int native) { O.native = native; }
void lm_out_format(int fmt) { O.want_fmt = fmt == MF_NV12 || fmt == MF_DMABUF ? fmt : MF_BGRA; }
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

static int ring_make(int w, int h, int fmt, int colors) {
    if (O.map) { munmap(O.map, O.slot_bytes * MP_RING_SLOTS); close(O.fd); }
    O.map = NULL;
    O.fd = -1;
    MpRing L;
    if (mp_ring_layout(fmt, w, h, MP_RING_SLOTS, fmt == MF_NV12 ? colors : 0, &L)) return -1;
    O.slot_bytes = L.slot_bytes;
    void *m;
    O.fd = lm_memfd(O.slot_bytes * MP_RING_SLOTS, &m, "lord-media-frames");
    if (O.fd < 0) return -1;
    O.map = m;
    O.w = w;
    O.h = h;
    O.fmt = fmt;
    O.colors = fmt == MF_NV12 ? colors : 0;
    O.gen = (O.gen + 1) & 0x7fffff;         /* the top bit of a frame id is a zero-copy frame's (MM_DMAFRAME) */
    O.server_cur = -1;
    memset(O.pending, 0, sizeof O.pending);
    int64_t v[6] = { O.gen, w, h, MP_RING_SLOTS, fmt, O.colors };
    mp_send(LM_SOCK, MM_RING, 0, v, NULL, 0, &O.fd, 1);
    return 0;
}

static int free_slot(void) {
    for (int i = 0; i < MP_RING_SLOTS; i++) if (i != O.server_cur && !O.pending[i]) return i;
    return -1;
}

static void dma_ack(uint32_t id);

void lm_out_ack(uint32_t id) {
    if (id & 0x80000000u) { dma_ack(id & 0x7fffffffu); return; }
    int gen = (int)(id >> 8), slot = (int)(id & 0xff);
    if (gen != O.gen || slot >= MP_RING_SLOTS) return;
    int64_t s = O.pending[slot];
    if (!s) return;
    for (int i = 0; i < MP_RING_SLOTS; i++) if (O.pending[i] && O.pending[i] <= s) O.pending[i] = 0;
    O.server_cur = slot;
}

static void set_colors(struct SwsContext *s, const AVFrame *f, int to_yuv) {
    int cs = f->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : f->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020
           : (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720) ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
    int full = f->color_range == AVCOL_RANGE_JPEG;
    /* YUV to YUV (NV12 scaled down): the same matrix and range on both sides, lord-server's shader applies them */
    if (to_yuv) sws_setColorspaceDetails(s, sws_getCoefficients(cs), full, sws_getCoefficients(cs), full, 0, 1 << 16, 1 << 16);
    else sws_setColorspaceDetails(s, sws_getCoefficients(cs), full, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
}

/* MM_RING's colours for a frame: the matrix as set_colors picks it, and the range */
static int frame_colors(const AVFrame *f) {
    int m = f->colorspace == AVCOL_SPC_BT709 ? MC_709 : f->colorspace == AVCOL_SPC_BT2020_NCL ? MC_2020
          : (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720) ? MC_709 : MC_601;
    return m | (f->color_range == AVCOL_RANGE_JPEG ? MC_FULL : 0);
}


/* ---- zero-copy (MF_DMABUF, docs/MOVING-WALLPAPERS.md) ----
 * The VA-API surface itself goes to lord-server: exported read-only as separate layers (Y as R8, U-V as GR88; R16 and
 * GR1616 for 10 bits) and sent as dmabuf fds with an MpDma description (MM_DMAFRAME). The decoder must not write the
 * surface again while lord-server may still draw it: each frame sent is held (its AVFrame keeps the surface out of the
 * pool) until lord-server has acknowledged a frame two numbers later. Only when the picture isn't larger than the box
 * (a quarter more): a larger one is scaled here into the NV12 ring, as is anything that can't be exported. */
#define DMA_HELD 6
static struct { AVFrame *f; uint32_t id; } held[DMA_HELD];
static uint32_t dma_seq;
static int dma_broken;

static void dma_ack(uint32_t id) {
    for (int i = 0; i < DMA_HELD; i++)
        if (held[i].f && held[i].id + 2 <= id) { av_frame_free(&held[i].f); held[i].id = 0; }
}

#ifdef LM_VAAPI
static int frame_colors(const AVFrame *f);
/* 0 sent, 1 dropped (lord-server behind), -1 can't (use the ring) */
static int dma_present(AVFrame *f, int64_t pts) {
    if (dma_broken || f->format != AV_PIX_FMT_VAAPI || !f->hw_frames_ctx || (f->width & 1) || (f->height & 1)) return -1;
    if (O.box_w <= 0 || f->width > O.box_w + O.box_w / 4 || f->height > O.box_h + O.box_h / 4) return -1;
    int slot = -1;
    for (int i = 0; i < DMA_HELD && slot < 0; i++) if (!held[i].f) slot = i;
    if (slot < 0) return 1;
    AVHWFramesContext *fc = (AVHWFramesContext *)f->hw_frames_ctx->data;
    AVVAAPIDeviceContext *hw = fc->device_ctx->hwctx;
    VASurfaceID surf = (VASurfaceID)(uintptr_t)f->data[3];
    if (vaSyncSurface(hw->display, surf) != VA_STATUS_SUCCESS) return -1;
    VADRMPRIMESurfaceDescriptor d;
    memset(&d, 0, sizeof d);
    if (vaExportSurfaceHandle(hw->display, surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                              VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &d) != VA_STATUS_SUCCESS) {
        LM_LOG("the VA-API surface can't be exported: frames through shared memory");
        dma_broken = 1;
        return -1;
    }
    int ok = d.num_layers == 2 && d.num_objects >= 1 && d.num_objects <= MP_DMA_MAX_OBJ && d.layers[0].num_planes == 1 && d.layers[1].num_planes == 1;
    MpDma m;
    memset(&m, 0, sizeof m);
    if (ok) {
        m.magic = MP_DMA_MAGIC;
        m.w = (uint32_t)f->width;
        m.h = (uint32_t)f->height;
        m.colors = (uint32_t)frame_colors(f);
        m.nobj = d.num_objects;
        for (uint32_t i = 0; i < d.num_objects; i++) m.modifier[i] = d.objects[i].drm_format_modifier;
        for (int l = 0; l < 2; l++) {
            m.plane[l].fourcc = d.layers[l].drm_format;
            m.plane[l].obj = d.layers[l].object_index[0];
            m.plane[l].offset = d.layers[l].offset[0];
            m.plane[l].pitch = d.layers[l].pitch[0];
        }
    }
    int fds[MP_DMA_MAX_OBJ] = { -1, -1 };
    for (uint32_t i = 0; i < d.num_objects && i < MP_DMA_MAX_OBJ; i++) fds[i] = d.objects[i].fd;
    int r = -1;
    if (ok) {
        uint32_t id = (++dma_seq) & 0x7fffffffu;
        int64_t v[6] = { pts, 0, 0, 0, 0, 0 };
        r = mp_send(LM_SOCK, MM_DMAFRAME, 0x80000000u | id, v, &m, sizeof m, fds, (int)d.num_objects);
        if (!r) { held[slot].f = av_frame_clone(f); held[slot].id = id; }
    } else {
        LM_LOG("the VA-API surface's layout is not two planes: frames through shared memory");
        dma_broken = 1;
    }
    for (uint32_t i = 0; i < d.num_objects; i++) close(d.objects[i].fd);
    return r == 0 ? 0 : ok ? 1 : -1;
}
#else
static int dma_present(AVFrame *f, int64_t pts) { (void)f; (void)pts; return -1; }
#endif

int lm_out_present(AVFrame *f, int64_t pts) {
    if (O.want_fmt == MF_DMABUF) {
        int d = dma_present(f, pts);
        if (d >= 0) {
            if (d == 0 && O.last != f) { av_frame_free(&O.last); O.last = av_frame_clone(f); O.last_pts = pts; }
            return d;
        }
    }
    AVFrame *sw = f, *tmp = NULL;
    if (f->format == AV_PIX_FMT_VAAPI || f->format == AV_PIX_FMT_DRM_PRIME) {
        tmp = av_frame_alloc();
        if (av_hwframe_transfer_data(tmp, f, 0) < 0) { av_frame_free(&tmp); return -1; }
        av_frame_copy_props(tmp, f);
        sw = tmp;
    }
    int w, h;
    out_size(sw, &w, &h);
    int fmt = O.want_fmt == MF_DMABUF ? MF_NV12 : O.want_fmt, colors = 0;
    if (fmt == MF_NV12) {
        /* never larger than the video itself: the GPU scales it up for nothing */
        int nw = sw->width & ~1, nh = sw->height & ~1;
        AVRational sar = sw->sample_aspect_ratio;
        if (!(sar.num > 0 && sar.den > 0 && sar.num != sar.den) && w >= nw && h >= nh && nw >= 2 && nh >= 2) { w = nw; h = nh; }
        colors = frame_colors(sw);
    }
    if ((w != O.w || h != O.h || fmt != O.fmt || colors != O.colors || !O.map) && ring_make(w, h, fmt, colors)) { av_frame_free(&tmp); return -1; }
    int slot = free_slot();
    if (slot < 0) { av_frame_free(&tmp); return 1; }
    uint8_t *base = O.map + O.slot_bytes * (size_t)slot;
    if (fmt == MF_NV12 && sw->format == AV_PIX_FMT_NV12 && sw->width >= w && sw->height >= h && (sw->width & ~1) == w && (sw->height & ~1) == h) {
        /* the decoder's planes as they are: a copy, no conversion */
        for (int y = 0; y < h; y++) memcpy(base + (size_t)y * w, sw->data[0] + (size_t)y * sw->linesize[0], (size_t)w);
        uint8_t *uv = base + (size_t)w * h;
        for (int y = 0; y < h / 2; y++) memcpy(uv + (size_t)y * w, sw->data[1] + (size_t)y * sw->linesize[1], (size_t)w);
    } else {
        int dfmt = fmt == MF_NV12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_BGRA;
        if (!O.sws || O.sw != sw->width || O.sh != sw->height || O.sfmt != sw->format || O.sws_dw != w || O.sws_dh != h || O.sws_df != dfmt) {
            sws_freeContext(O.sws);
            O.sws = sws_alloc_context();
            av_opt_set_int(O.sws, "srcw", sw->width, 0);
            av_opt_set_int(O.sws, "srch", sw->height, 0);
            av_opt_set_int(O.sws, "src_format", sw->format, 0);
            av_opt_set_int(O.sws, "dstw", w, 0);
            av_opt_set_int(O.sws, "dsth", h, 0);
            av_opt_set_int(O.sws, "dst_format", dfmt, 0);
            av_opt_set_int(O.sws, "sws_flags", SWS_BILINEAR, 0);
            av_opt_set_int(O.sws, "threads", 2, 0);
            if (sws_init_context(O.sws, NULL, NULL) < 0) { sws_freeContext(O.sws); O.sws = NULL; av_frame_free(&tmp); return -1; }
            set_colors(O.sws, sw, fmt == MF_NV12);
            O.sw = sw->width; O.sh = sw->height; O.sfmt = sw->format; O.sws_dw = w; O.sws_dh = h; O.sws_df = dfmt;
        }
        uint8_t *dst[4] = { base, NULL, NULL, NULL };
        int ls[4] = { w * 4, 0, 0, 0 };
        if (fmt == MF_NV12) { dst[1] = base + (size_t)w * h; ls[0] = w; ls[1] = w; }
        sws_scale(O.sws, (const uint8_t *const *)sw->data, sw->linesize, 0, sw->height, dst, ls);
    }
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
