/* lm_vbench.c — LordBench's video measurement (MK_VBENCH; docs/BENCHMARK.md, "Workloads").
 *
 * No files and no devices: the test clips are made here, in memory. A synthetic picture (gradients, moving
 * blocks and a little noise, the same every time) is encoded with this system's FFmpeg into H.264, HEVC,
 * VP9 and AV1 at streaming bit rates, and each clip is then decoded in software the way the player does
 * (lm_play.c: every core, frame and slice threads) over and over for at least a second, three times; the
 * result is the median of frames decoded per second. Then the remote desktop's own encoders (lm_stream.c, its settings) encode
 * 1920×1080 frames as fast as they can. An encoder or decoder this FFmpeg lacks is reported as skipped.
 * Quick measures only H.264 at 1080p and the H.264 encoder. Answers MM_BENCH records only (proto.h). */
#include "lm.h"

#include <libavutil/opt.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int id;                             /* VB_* */
    enum AVCodecID codec;
    int w, h, frames;
    long kbps;
} Clip;

static void report(int id, int kind, double fps, double spread, int why) {
    int64_t v[6] = { kind, (int64_t)(fps * 1000), (int64_t)(spread * 1000), why, 0, 0 };
    mp_send(LM_SOCK, MM_BENCH, (uint32_t)id, v, NULL, 0, NULL, 0);
}

/* frame i of the synthetic picture */
static void synth(AVFrame *f, int i) {
    int w = f->width, h = f->height;
    for (int y = 0; y < h; y++) {
        uint8_t *row = f->data[0] + (size_t)y * f->linesize[0];
        for (int x = 0; x < w; x++) {
            unsigned n = (unsigned)(x * 73856093u ^ y * 19349663u ^ (unsigned)i * 83492791u);
            int v = ((x + 4 * i) * 255 / w + (y * 160) / h) / 2 + (int)((n >> 13) & 15);
            int bx = (x + 12 * i) / 96, by = (y + 5 * i) / 96;
            if (((bx ^ by) & 7) == 0) v = 235 - v / 3;
            row[x] = (uint8_t)(v < 16 ? 16 : v > 235 ? 235 : v);
        }
    }
    for (int p = 1; p < 3; p++)
        for (int y = 0; y < h / 2; y++) {
            uint8_t *row = f->data[p] + (size_t)y * f->linesize[p];
            for (int x = 0; x < w / 2; x++) row[x] = (uint8_t)(128 + ((p == 1 ? x + 2 * i : y - i) * 96 / (w / 2 + 1)) % 48 - 24);
        }
}

static AVFrame *new_frame(int w, int h) {
    AVFrame *f = av_frame_alloc();
    if (!f) return NULL;
    f->format = AV_PIX_FMT_YUV420P;
    f->width = w;
    f->height = h;
    if (av_frame_get_buffer(f, 0) < 0) av_frame_free(&f);
    return f;
}

/* an encoder for making the clip: the first of the names this FFmpeg has */
static AVCodecContext *clip_encoder(const Clip *c) {
    static const char *const h264[] = { "libx264", NULL }, *const hevc[] = { "libx265", NULL },
                             *const vp9[] = { "libvpx-vp9", NULL }, *const av1[] = { "libsvtav1", "libaom-av1", "librav1e", NULL };
    const char *const *names = c->codec == AV_CODEC_ID_H264 ? h264 : c->codec == AV_CODEC_ID_HEVC ? hevc : c->codec == AV_CODEC_ID_VP9 ? vp9 : av1;
    for (int k = 0; names[k]; k++) {
        const AVCodec *codec = avcodec_find_encoder_by_name(names[k]);
        if (!codec) continue;
        AVCodecContext *e = avcodec_alloc_context3(codec);
        if (!e) return NULL;
        e->width = c->w;
        e->height = c->h;
        e->pix_fmt = AV_PIX_FMT_YUV420P;
        e->time_base = (AVRational){ 1, 30 };
        e->framerate = (AVRational){ 30, 1 };
        e->bit_rate = c->kbps * 1000;
        e->gop_size = 60;
        e->thread_count = 0;
        AVDictionary *o = NULL;
        if (!strcmp(names[k], "libx264")) av_dict_set(&o, "preset", "veryfast", 0);
        else if (!strcmp(names[k], "libx265")) { av_dict_set(&o, "preset", "ultrafast", 0); av_dict_set(&o, "x265-params", "log-level=error", 0); }
        else if (!strcmp(names[k], "libvpx-vp9")) { av_dict_set(&o, "deadline", "realtime", 0); av_dict_set(&o, "cpu-used", "8", 0); av_dict_set(&o, "row-mt", "1", 0); }
        else if (!strcmp(names[k], "libsvtav1")) av_dict_set(&o, "preset", "12", 0);
        else if (!strcmp(names[k], "libaom-av1")) { av_dict_set(&o, "cpu-used", "9", 0); av_dict_set(&o, "usage", "realtime", 0); }
        else if (!strcmp(names[k], "librav1e")) av_dict_set(&o, "speed", "10", 0);
        int r = avcodec_open2(e, codec, &o);
        av_dict_free(&o);
        if (r >= 0) return e;
        avcodec_free_context(&e);
    }
    return NULL;
}

typedef struct { AVPacket **p; int n, cap; } Pkts;

static int keep(Pkts *k, AVCodecContext *e) {
    for (;;) {
        AVPacket *p = av_packet_alloc();
        if (!p) return -1;
        int r = avcodec_receive_packet(e, p);
        if (r < 0) { av_packet_free(&p); return r == AVERROR(EAGAIN) || r == AVERROR_EOF ? 0 : -1; }
        if (k->n == k->cap) {
            int cap = k->cap ? k->cap * 2 : 64;
            AVPacket **q = realloc(k->p, (size_t)cap * sizeof *q);
            if (!q) { av_packet_free(&p); return -1; }
            k->p = q;
            k->cap = cap;
        }
        k->p[k->n++] = p;
    }
}

static void pkts_free(Pkts *k) {
    for (int i = 0; i < k->n; i++) av_packet_free(&k->p[i]);
    free(k->p);
    memset(k, 0, sizeof *k);
}

/* makes the clip; 0, or VBS_* */
static int make_clip(const Clip *c, Pkts *out, AVCodecParameters *par) {
    AVCodecContext *e = clip_encoder(c);
    if (!e) return VBS_NOCODEC;
    AVFrame *f = new_frame(c->w, c->h);
    int bad = !f;
    for (int i = 0; !bad && i < c->frames; i++) {
        if (av_frame_make_writable(f) < 0) { bad = 1; break; }
        synth(f, i);
        f->pts = i;
        bad = avcodec_send_frame(e, f) < 0 || keep(out, e) < 0;
    }
    if (!bad) bad = avcodec_send_frame(e, NULL) < 0 || keep(out, e) < 0;
    if (!bad) bad = avcodec_parameters_from_context(par, e) < 0;
    av_frame_free(&f);
    avcodec_free_context(&e);
    return bad || out->n == 0 ? VBS_FAILED : 0;
}

/* decodes the clip once as the player does: frames and ms, or -1 */
static int decode_once(const AVCodecParameters *par, const Pkts *k, int *nodec, double *ms_out) {
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) { *nodec = 1; return -1; }
    AVCodecContext *d = avcodec_alloc_context3(codec);
    AVFrame *f = av_frame_alloc();
    int frames = -1;
    if (d && f && avcodec_parameters_to_context(d, par) >= 0) {
        d->thread_count = 0;
        d->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (avcodec_open2(d, codec, NULL) >= 0) {
            int got = 0, bad = 0;
            double t0 = lm_now_ms();
            for (int i = 0; i <= k->n && !bad; i++) {
                if (avcodec_send_packet(d, i < k->n ? k->p[i] : NULL) < 0) { bad = 1; break; }
                int r;
                while ((r = avcodec_receive_frame(d, f)) >= 0) { got++; av_frame_unref(f); }
                if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) bad = 1;
            }
            *ms_out = lm_now_ms() - t0;
            if (!bad && got > 0) frames = got;
        }
    }
    av_frame_free(&f);
    avcodec_free_context(&d);
    return frames;
}

/* decodes the clip again and again for at least DECODE_MS: frames per second, or -1 */
#define DECODE_MS 1000.0
static double decode_rate(const AVCodecParameters *par, const Pkts *k, int *nodec) {
    double total_ms = 0;
    long frames = 0;
    while (total_ms < DECODE_MS) {
        double ms = 0;
        int n = decode_once(par, k, nodec, &ms);
        if (n <= 0) return -1;
        frames += n;
        total_ms += ms;
    }
    return total_ms > 0 ? frames * 1000.0 / total_ms : -1;
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

static void measure_decode(const Clip *c) {
    report(c->id, VBR_STEP, 0, 0, 0);
    Pkts k = { 0 };
    AVCodecParameters *par = avcodec_parameters_alloc();
    int why = par ? make_clip(c, &k, par) : VBS_FAILED;
    double v[3];
    for (int r = 0; !why && r < 3; r++) {
        int nodec = 0;
        v[r] = decode_rate(par, &k, &nodec);
        if (v[r] <= 0) why = nodec ? VBS_NOCODEC : VBS_FAILED;
    }
    if (why) {
        LM_LOG("video measurement %d skipped (%s)", c->id, why == VBS_NOCODEC ? "no encoder or decoder" : "failed");
        report(c->id, VBR_SKIP, 0, 0, why);
    } else {
        qsort(v, 3, sizeof *v, cmp_d);
        report(c->id, VBR_RESULT, v[1], v[2] - v[0], 0);
    }
    pkts_free(&k);
    avcodec_parameters_free(&par);
}

/* the remote desktop's encoder (lm_stream.c) on 1920×1080 frames as fast as it goes, three times */
static void measure_encode(int id, int vp8) {
    report(id, VBR_STEP, 0, 0, 0);
    enum { NF = 8, PER = 60 };
    AVFrame *src[NF] = { 0 };
    int bad = 0;
    for (int i = 0; i < NF && !bad; i++) { src[i] = new_frame(1920, 1080); if (src[i]) synth(src[i], i * 3); else bad = 1; }
    double v[3];
    int why = bad ? VBS_FAILED : 0;
    for (int r = 0; !why && r < 3; r++) {
        AVCodecContext *e = lm_stream_sw_encoder(vp8, 1920, 1080, 30, 8000);
        AVPacket *p = av_packet_alloc();
        if (!e || !p) { why = e ? VBS_FAILED : VBS_NOCODEC; avcodec_free_context(&e); av_packet_free(&p); break; }
        double t0 = lm_now_ms();
        for (int i = 0; i < PER && !why; i++) {
            AVFrame *f = src[i % NF];
            f->pts = (int64_t)i * 33;
            if (avcodec_send_frame(e, f) < 0) why = VBS_FAILED;
            while (!why && avcodec_receive_packet(e, p) >= 0) av_packet_unref(p);
        }
        double ms = lm_now_ms() - t0;
        v[r] = ms > 0 ? PER * 1000.0 / ms : -1;
        if (v[r] <= 0) why = VBS_FAILED;
        av_packet_free(&p);
        avcodec_free_context(&e);
    }
    for (int i = 0; i < NF; i++) av_frame_free(&src[i]);
    if (why) report(id, VBR_SKIP, 0, 0, why);
    else {
        qsort(v, 3, sizeof *v, cmp_d);
        report(id, VBR_RESULT, v[1], v[2] - v[0], 0);
    }
}

void lm_vbench(const char *params) {
    int full = mp_param_int(params, "full", 0) == 1;
    static const Clip clips[] = {
        { VB_H264_1080, AV_CODEC_ID_H264, 1920, 1080, 60, 8000 },
        { VB_HEVC_1080, AV_CODEC_ID_HEVC, 1920, 1080, 60, 6000 },
        { VB_VP9_1080, AV_CODEC_ID_VP9, 1920, 1080, 60, 6000 },
        { VB_AV1_1080, AV_CODEC_ID_AV1, 1920, 1080, 60, 4000 },
        { VB_H264_2160, AV_CODEC_ID_H264, 3840, 2160, 30, 25000 },
        { VB_HEVC_2160, AV_CODEC_ID_HEVC, 3840, 2160, 30, 16000 },
    };
    for (size_t i = 0; i < sizeof clips / sizeof clips[0]; i++)
        if (full || clips[i].id == VB_H264_1080) measure_decode(&clips[i]);
        else report(clips[i].id, VBR_SKIP, 0, 0, VBS_MODE);
    measure_encode(VB_ENC_H264, 0);
    if (full) measure_encode(VB_ENC_VP8, 1);
    else report(VB_ENC_VP8, VBR_SKIP, 0, 0, VBS_MODE);
    mp_sendv(LM_SOCK, MM_STATE, 0, MS_DONE, 0, 0, 0);
}
