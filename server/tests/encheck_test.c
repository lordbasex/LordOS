/* encheck_test.c — the check of a hardware encoder's first keyframe (src/media/helper/lm_encheck.h, #94):
 * a good keyframe passes; one without SPS/PPS, a P-frame, a cut packet, a picture of another size or another
 * picture fail. The keyframes come from libx264 with the remote desktop's settings; with
 * ENCHECK_VAAPI=/dev/dri/renderDN the same check runs on that card's h264_vaapi (what lm_stream.c does). */
#include "../src/media/helper/lm_encheck.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#ifdef LM_VAAPI
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <va/va_drm.h>
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

enum { W = 640, H = 368 };

/* a desktop-like picture: flat panels, a gradient and some "text" */
static void paint(uint8_t *y, int ls, int w, int h, int seed) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int v = j < h / 8 ? 40 : i < w / 4 ? 200 : 16 + (i * 200 / w + seed * 37) % 220;
            if ((j / 6) % 4 == 1 && (i / 3 + seed) % 5 < 2 && i > w / 3) v = 20;
            y[(size_t)j * ls + i] = (uint8_t)v;
        }
}

static AVFrame *frame(int fmt, int w, int h, int seed) {
    AVFrame *f = av_frame_alloc();
    f->format = fmt;
    f->width = w;
    f->height = h;
    if (av_frame_get_buffer(f, 0) < 0) return NULL;
    paint(f->data[0], f->linesize[0], w, h, seed);
    int ch = h / 2;
    if (fmt == AV_PIX_FMT_NV12) for (int j = 0; j < ch; j++) memset(f->data[1] + (size_t)j * f->linesize[1], 128, (size_t)w);
    else for (int j = 0; j < ch; j++) { memset(f->data[1] + (size_t)j * f->linesize[1], 128, (size_t)w / 2); memset(f->data[2] + (size_t)j * f->linesize[2], 128, (size_t)w / 2); }
    return f;
}

typedef struct { uint8_t *d[4]; int n[4], key[4], count; } Pkts;

static void drain(AVCodecContext *c, Pkts *o) {
    AVPacket *p = av_packet_alloc();
    while (avcodec_receive_packet(c, p) >= 0) {
        if (o->count < 4) {
            o->d[o->count] = malloc((size_t)p->size);
            memcpy(o->d[o->count], p->data, (size_t)p->size);
            o->n[o->count] = p->size;
            o->key[o->count] = (p->flags & AV_PKT_FLAG_KEY) != 0;
            o->count++;
        }
        av_packet_unref(p);
    }
    av_packet_free(&p);
}

/* libx264 as lm_stream.c's open_sw sets it up */
static int x264(int w, int h, Pkts *o) {
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) return -1;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    c->width = w;
    c->height = h;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->time_base = (AVRational){ 1, 1000 };
    c->framerate = (AVRational){ 60, 1 };
    c->gop_size = 3600;
    c->max_b_frames = 0;
    c->thread_count = 2;
    c->rc_max_rate = 4000000;
    c->rc_buffer_size = 4000000 / 3;
    AVDictionary *d = NULL;
    av_dict_set(&d, "preset", "ultrafast", 0);
    av_dict_set(&d, "tune", "zerolatency", 0);
    av_dict_set(&d, "profile", "baseline", 0);
    av_dict_set(&d, "crf", "22", 0);
    av_dict_set(&d, "x264-params", "repeat-headers=1:annexb=1:aud=0:scenecut=0:deblock=1", 0);
    int r = avcodec_open2(c, codec, &d);
    av_dict_free(&d);
    if (r < 0) { avcodec_free_context(&c); return -1; }
    for (int i = 0; i < 2; i++) {
        AVFrame *f = frame(AV_PIX_FMT_YUV420P, w, h, i);
        f->pts = i * 16;
        avcodec_send_frame(c, f);
        av_frame_free(&f);
        drain(c, o);
    }
    avcodec_send_frame(c, NULL);
    drain(c, o);
    avcodec_free_context(&c);
    return 0;
}

static void ref_of(LmEncCheck *k, int w, int h, int seed) {
    AVFrame *f = frame(AV_PIX_FMT_YUV420P, w, h, seed);
    CHECK(!lm_encheck_ref(k, f->data[0], f->linesize[0], w, h));
    av_frame_free(&f);
}

/* the packet without its SPS and PPS NAL units */
static size_t strip_params(const uint8_t *in, size_t n, uint8_t *out) {
    size_t o = 0, i = 0;
    while (i < n) {
        size_t s = i;
        while (s + 3 <= n && !(in[s] == 0 && in[s + 1] == 0 && in[s + 2] == 1)) s++;
        if (s + 3 > n) break;
        size_t e = s + 3;
        while (e + 3 <= n && !(in[e] == 0 && in[e + 1] == 0 && in[e + 2] == 1)) e++;
        if (e + 3 > n) e = n;
        int t = in[s + 3] & 0x1f;
        if (t != 7 && t != 8) { memcpy(out + o, in + s, e - s); o += e - s; }
        i = e;
    }
    return o;
}

#ifdef LM_VAAPI
static void vaapi(const char *node) {
    int fd = open(node, O_RDWR | O_CLOEXEC);
    VADisplay d = fd >= 0 ? vaGetDisplayDRM(fd) : NULL;
    int ma, mi;
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!d || vaInitialize(d, &ma, &mi) != VA_STATUS_SUCCESS || !codec) { printf("encheck: skipped VA-API: needs a VA-API driver on %s\n", node); return; }
    AVBufferRef *hw = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
    ((AVVAAPIDeviceContext *)((AVHWDeviceContext *)hw->data)->hwctx)->display = d;
    if (av_hwdevice_ctx_init(hw) < 0) { printf("encheck: skipped VA-API: no device\n"); return; }
    AVBufferRef *fr = av_hwframe_ctx_alloc(hw);
    AVHWFramesContext *fc = (AVHWFramesContext *)fr->data;
    fc->format = AV_PIX_FMT_VAAPI;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = W;
    fc->height = H;
    fc->initial_pool_size = 4;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (av_hwframe_ctx_init(fr) < 0) { printf("encheck: skipped VA-API: no surfaces\n"); return; }
    c->hw_frames_ctx = av_buffer_ref(fr);
    c->pix_fmt = AV_PIX_FMT_VAAPI;
    c->width = W;
    c->height = H;
    c->time_base = (AVRational){ 1, 1000 };
    c->framerate = (AVRational){ 60, 1 };
    c->gop_size = 3600;
    c->max_b_frames = 0;
    c->profile = AV_PROFILE_H264_CONSTRAINED_BASELINE;
    c->bit_rate = 4000000;
    c->rc_max_rate = c->bit_rate;
    c->rc_buffer_size = (int)(c->bit_rate / 2);
    av_opt_set(c->priv_data, "rc_mode", "VBR", 0);
    if (avcodec_open2(c, codec, NULL) < 0) { printf("encheck: skipped VA-API: this card does not encode H.264\n"); return; }
    Pkts o = { 0 };
    for (int i = 0; i < 3; i++) {
        AVFrame *f = frame(AV_PIX_FMT_NV12, W, H, i), *g = av_frame_alloc();
        CHECK(av_hwframe_get_buffer(fr, g, 0) >= 0 && av_hwframe_transfer_data(g, f, 0) >= 0);
        g->pts = i * 16;
        CHECK(avcodec_send_frame(c, g) >= 0);
        av_frame_free(&f);
        av_frame_free(&g);
        drain(c, &o);
    }
    LmEncCheck k;
    ref_of(&k, W, H, 0);
    const char *why = "";
    CHECK(o.count >= 1 && o.key[0]);
    int r = o.count >= 1 ? lm_encheck_packet(&k, o.d[0], (size_t)o.n[0], &why) : -1;
    printf("encheck: VA-API (%s) first keyframe: %s%s\n", vaQueryVendorString(d), r ? "fails: " : "passes", why);
    CHECK(r == 0);
    lm_encheck_free(&k);
    for (int i = 0; i < o.count; i++) free(o.d[i]);
    avcodec_free_context(&c);
    av_buffer_unref(&fr);
    av_buffer_unref(&hw);
}
#endif

int main(void) {
    av_log_set_level(AV_LOG_QUIET);
    Pkts o = { 0 };
    if (x264(W, H, &o) || o.count < 2) { printf("encheck: skipped: needs FFmpeg with libx264\n"); return 0; }
    CHECK(o.key[0] && !o.key[1]);
    LmEncCheck k;
    const char *why = "";

    ref_of(&k, W, H, 0);
    CHECK(lm_encheck_packet(&k, o.d[0], (size_t)o.n[0], &why) == 0);       /* the keyframe of that picture */
    CHECK(!why[0]);

    /* a driver without packed headers: slices but no SPS/PPS, which no browser decodes */
    uint8_t *bare = malloc((size_t)o.n[0]);
    size_t bn = strip_params(o.d[0], (size_t)o.n[0], bare);
    CHECK(bn > 0 && bn < (size_t)o.n[0]);
    CHECK(lm_encheck_packet(&k, bare, bn, &why) == -1 && strstr(why, "SPS"));
    free(bare);

    CHECK(lm_encheck_packet(&k, o.d[1], (size_t)o.n[1], &why) == -1);      /* a P-frame alone */
    CHECK(lm_encheck_packet(&k, o.d[0], 40, &why) == -1);                  /* cut short */
    CHECK(lm_encheck_packet(&k, NULL, 0, &why) == -1);
    lm_encheck_free(&k);

    /* the decoded picture is not the frame that went in (a driver returning garbage, or a stale surface) */
    LmEncCheck other;
    AVFrame *f = frame(AV_PIX_FMT_YUV420P, W, H, 0);
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) f->data[0][(size_t)j * f->linesize[0] + i] ^= 0xff;
    CHECK(!lm_encheck_ref(&other, f->data[0], f->linesize[0], W, H));
    av_frame_free(&f);
    CHECK(lm_encheck_packet(&other, o.d[0], (size_t)o.n[0], &why) == -1 && strstr(why, "not the frame"));
    lm_encheck_free(&other);

    /* another size */
    ref_of(&k, W - 64, H, 0);
    CHECK(lm_encheck_packet(&k, o.d[0], (size_t)o.n[0], &why) == -1 && strstr(why, "size"));
    lm_encheck_free(&k);

    /* no reference */
    memset(&k, 0, sizeof k);
    CHECK(lm_encheck_packet(&k, o.d[0], (size_t)o.n[0], &why) == -1);
    for (int i = 0; i < o.count; i++) free(o.d[i]);

#ifdef LM_VAAPI
    const char *node = getenv("ENCHECK_VAAPI");
    if (node && *node) vaapi(node);
#endif
    if (fails) { printf("encheck: %d failed\n", fails); return 1; }
    printf("encheck: ok\n");
    return 0;
}
