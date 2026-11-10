/* lm_job.c — the MK_WORKER helper: handles short queued jobs, one per MM_JOB message
 * (MW_*), with the descriptors lord-server passes. It exits on its own if a while passes without work.
 * Each reply is an MM_RESULT with the job id. */
#include "lm.h"
#include "lm_svg.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#define IDLE_MS 20000

static void closefds(int *fds) { for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) { close(fds[i]); fds[i] = -1; } }

/* Maximum side for showing an image without exceeding memory (~64 MB of BGRA). */
static void fit_size(int w, int h, int max, int *ow, int *oh) {
    double s = 1;
    if (w > max || h > max) s = fmin((double)max / w, (double)max / h);
    if ((double)w * h * s * s > 16e6) s = sqrt(16e6 / ((double)w * h));
    *ow = (int)fmax(1, lround(w * s));
    *oh = (int)fmax(1, lround(h * s));
}

static void job_probe(uint32_t id, int in) {
    char err[128];
    AVFormatContext *fmt = lm_open_input(in, err, sizeof err);
    if (!fmt) { lm_result(id, -1, err, -1); return; }
    LmBuf b = { 0 };
    lm_probe_json(fmt, &b);
    lm_result(id, 0, b.d, -1);
    lm_buf_free(&b);
    lm_close_input(&fmt);
}

/* PNG thumbnail (thumb) or a poster's pixels in a memfd (poster). */
static void job_thumb(uint32_t id, int in, int out, int size, int as_pixels) {
    char err[128];
    AVFormatContext *fmt = lm_open_input(in, err, sizeof err);
    if (!fmt) { lm_result(id, -1, err, -1); return; }
    int64_t dur = fmt->duration > 0 ? fmt->duration / 1000 : 0;
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int is_video = vi >= 0 && !(fmt->streams[vi]->disposition & AV_DISPOSITION_ATTACHED_PIC) && dur > 1000;
    int orient = 0;
    AVFrame *f = lm_decode_still(fmt, is_video ? (dur > 30000 ? dur / 10 : dur / 3) : 0, &orient, !is_video);
    if (!f) { lm_close_input(&fmt); lm_result(id, -2, "media.notAnImage", -1); return; }
    if (size < 16) size = 16;
    if (size > 1024) size = 1024;
    int sw = f->width, sh = f->height;
    if (f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0) sw = (int)lround(sw * av_q2d(f->sample_aspect_ratio));
    int tw, th;
    fit_size(sw, sh, size, &tw, &th);
    if (sw <= size && sh <= size) { tw = sw; th = sh; }
    AVFrame *o = lm_to_bgra(f, tw, th, 1);
    av_frame_free(&f);
    lm_close_input(&fmt);
    if (!o) { lm_result(id, -1, "media.scaleFailed", -1); return; }
    char json[160];
    snprintf(json, sizeof json, "{\"width\":%d,\"height\":%d,\"orientation\":%d,\"video\":%s,\"duration\":%lld}", tw, th, orient,
             is_video ? "true" : "false", (long long)dur);
    if (as_pixels) {
        void *map;
        size_t n = (size_t)tw * th * 4;
        int mfd = lm_memfd(n, &map, "lord-media-poster");
        if (mfd < 0) { av_frame_free(&o); lm_result(id, -1, "sys.outOfMemory", -1); return; }
        for (int y = 0; y < th; y++) memcpy((uint8_t *)map + (size_t)y * tw * 4, o->data[0] + (size_t)y * o->linesize[0], (size_t)tw * 4);
        munmap(map, n);
        lm_result(id, 0, json, mfd);
        close(mfd);
    } else {
        int r = lm_encode_image(o, "png", 0, out);
        lm_result(id, r ? -1 : 0, r ? "media.encodeFailed" : json, -1);
    }
    av_frame_free(&o);
}

/* Full image (for Photos): premultiplied BGRA in a sealed memfd. */
static void job_image(uint32_t id, int in) {
    char err[128];
    AVFormatContext *fmt = lm_open_input(in, err, sizeof err);
    if (!fmt) { lm_result(id, -1, err, -1); return; }
    int orient = 0;
    AVFrame *f = lm_decode_still(fmt, 0, &orient, 0);
    /* animated? (GIF, APNG): there's a second frame */
    int frames = 1;
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (f && vi >= 0) {
        AVPacket *pkt = av_packet_alloc();
        for (int n = 0; n < 8 && frames < 2; n++) {
            if (av_read_frame(fmt, pkt) < 0) break;
            if (pkt->stream_index == vi) frames++;
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
    lm_close_input(&fmt);
    if (!f) { lm_result(id, -2, "media.notAnImage", -1); return; }
    int w, h;
    fit_size(f->width, f->height, MP_MAX_DIM, &w, &h);
    AVFrame *o = lm_to_bgra(f, w, h, 1);
    int ow = f->width, oh = f->height;
    av_frame_free(&f);
    if (!o) { lm_result(id, -1, "media.convertFailed", -1); return; }
    void *map;
    size_t n = (size_t)w * h * 4;
    int mfd = lm_memfd(n, &map, "lord-media-image");
    if (mfd < 0) { av_frame_free(&o); lm_result(id, -1, "sys.outOfMemory", -1); return; }
    int alpha = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *s = o->data[0] + (size_t)y * o->linesize[0];
        memcpy((uint8_t *)map + (size_t)y * w * 4, s, (size_t)w * 4);
        if (!alpha) for (int x = 0; x < w; x++) if (s[x * 4 + 3] != 255) { alpha = 1; break; }
    }
    munmap(map, n);
    av_frame_free(&o);
    char json[200];
    snprintf(json, sizeof json, "{\"width\":%d,\"height\":%d,\"sourceWidth\":%d,\"sourceHeight\":%d,\"orientation\":%d,\"animated\":%s,\"alpha\":%s}",
             w, h, ow, oh, orient, frames > 1 ? "true" : "false", alpha ? "true" : "false");
    lm_result(id, 0, json, mfd);
    close(mfd);
}

/* An SVG icon (lm_svg.c) drawn to fit side x side, recolored when recolor >= 0: premultiplied BGRA in a sealed memfd.
 * lord-server opened the file (a regular file of at most LM_SVG_MAX_BYTES); its size is checked again here. */
static void job_svg(uint32_t id, int in, int side, long recolor) {
    struct stat st;
    if (in < 0 || fstat(in, &st) || !S_ISREG(st.st_mode)) { lm_result(id, -1, "media.notAFile", -1); return; }
    if (st.st_size > LM_SVG_MAX_BYTES) { lm_result(id, -1, "media.svgTooLarge", -1); return; }
    char *buf = malloc(LM_SVG_MAX_BYTES + 1);
    if (!buf) { lm_result(id, -1, "sys.outOfMemory", -1); return; }
    size_t len = 0;
    for (;;) {
        ssize_t r = pread(in, buf + len, LM_SVG_MAX_BYTES + 1 - len, (off_t)len);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        len += (size_t)r;
        if (len > LM_SVG_MAX_BYTES) break;      /* it grew after lord-server looked */
    }
    uint8_t *px = NULL;
    int w = 0, h = 0;
    const char *err = lm_svg_raster(buf, len, side, recolor, &px, &w, &h);
    free(buf);
    if (err) { LM_DBG("svg %u: %s", id, err); lm_result(id, -2, err, -1); return; }
    void *map;
    size_t n = (size_t)w * h * 4;
    int mfd = lm_memfd(n, &map, "lord-media-svg");
    if (mfd < 0) { free(px); lm_result(id, -1, "sys.outOfMemory", -1); return; }
    memcpy(map, px, n);
    munmap(map, n);
    free(px);
    char json[64];
    snprintf(json, sizeof json, "{\"width\":%d,\"height\":%d}", w, h);
    lm_result(id, 0, json, mfd);
    close(mfd);
}

static uint8_t clamp8(double v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)lround(v); }

/* Basic editing (saved as a copy): crop (in coordinates of the image already rotated per
 * EXIF), rotation in 90° steps, brightness and contrast (-100..100) and a new size (sw × sh). */
static void job_edit(uint32_t id, int in, int out, const char *p) {
    char err[128], format[16];
    AVFormatContext *fmt = lm_open_input(in, err, sizeof err);
    if (!fmt) { lm_result(id, -1, err, -1); return; }
    int orient = 0;
    AVFrame *f = lm_decode_still(fmt, 0, &orient, 0);
    lm_close_input(&fmt);
    if (!f) { lm_result(id, -2, "media.notAnImage", -1); return; }
    int w, h;
    fit_size(f->width, f->height, MP_MAX_DIM, &w, &h);
    AVFrame *o = lm_to_bgra(f, w, h, 0);
    av_frame_free(&f);
    if (!o) { lm_result(id, -1, "sys.outOfMemory", -1); return; }
    int rot = (((int)mp_param_int(p, "rotate", 0) + orient) % 360 + 360) % 360;
    int bright = (int)mp_param_int(p, "brightness", 0), contrast = (int)mp_param_int(p, "contrast", 0);
    if (mp_param(p, "format", format, sizeof format)) snprintf(format, sizeof format, "png");
    /* rotation: new image */
    int rw = (rot == 90 || rot == 270) ? h : w, rh = (rot == 90 || rot == 270) ? w : h;
    AVFrame *r = av_frame_alloc();
    r->format = AV_PIX_FMT_BGRA; r->width = rw; r->height = rh;
    if (av_frame_get_buffer(r, 0) < 0) { av_frame_free(&o); av_frame_free(&r); lm_result(id, -1, "sys.outOfMemory", -1); return; }
    for (int y = 0; y < rh; y++) {
        uint32_t *d = (uint32_t *)(r->data[0] + (size_t)y * r->linesize[0]);
        for (int x = 0; x < rw; x++) {
            int sx = x, sy = y;
            if (rot == 90) { sx = y; sy = h - 1 - x; }
            else if (rot == 180) { sx = w - 1 - x; sy = h - 1 - y; }
            else if (rot == 270) { sx = w - 1 - y; sy = x; }
            d[x] = ((const uint32_t *)(o->data[0] + (size_t)sy * o->linesize[0]))[sx];
        }
    }
    av_frame_free(&o);
    /* crop (if given; otherwise, the whole image) */
    int cx = (int)mp_param_int(p, "cx", 0), cy = (int)mp_param_int(p, "cy", 0);
    int cw = (int)mp_param_int(p, "cw", rw), ch = (int)mp_param_int(p, "ch", rh);
    /* the crop comes in preview coordinates (scale = reference width) */
    int ref = (int)mp_param_int(p, "refw", rw);
    if (ref > 0 && ref != rw) { double s = (double)rw / ref; cx = (int)(cx * s); cy = (int)(cy * s); cw = (int)(cw * s); ch = (int)(ch * s); }
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx + cw > rw) cw = rw - cx;
    if (cy + ch > rh) ch = rh - cy;
    if (cw < 1 || ch < 1) { av_frame_free(&r); lm_result(id, -1, "media.invalidCrop", -1); return; }
    AVFrame *c = av_frame_alloc();
    c->format = AV_PIX_FMT_BGRA; c->width = cw; c->height = ch;
    av_frame_get_buffer(c, 0);
    double k = (100.0 + contrast) / 100.0;           /* contrast around mid gray */
    k = k * k;
    double bo = bright * 1.28;
    for (int y = 0; y < ch; y++) {
        const uint8_t *s = r->data[0] + (size_t)(y + cy) * r->linesize[0] + (size_t)cx * 4;
        uint8_t *d = c->data[0] + (size_t)y * c->linesize[0];
        for (int x = 0; x < cw; x++, s += 4, d += 4) {
            for (int i = 0; i < 3; i++) d[i] = (bright || contrast) ? clamp8((s[i] - 128.0) * k + 128.0 + bo) : s[i];
            d[3] = s[3];
        }
    }
    av_frame_free(&r);
    /* resize after the crop (Photos → Edit → Size), when asked: sides 1..MP_MAX_DIM, straight alpha */
    int sw = (int)mp_param_int(p, "sw", 0), sh = (int)mp_param_int(p, "sh", 0);
    if (sw > 0 && sh > 0 && sw <= MP_MAX_DIM && sh <= MP_MAX_DIM && (sw != cw || sh != ch)) {
        AVFrame *z = lm_to_bgra(c, sw, sh, 0);
        if (!z) { av_frame_free(&c); lm_result(id, -1, "sys.outOfMemory", -1); return; }
        av_frame_free(&c);
        c = z;
        cw = sw;
        ch = sh;
    }
    /* lm_encode_image expects premultiplied */
    for (int y = 0; y < ch; y++) {
        uint8_t *d = c->data[0] + (size_t)y * c->linesize[0];
        for (int x = 0; x < cw; x++, d += 4) if (d[3] < 255) for (int i = 0; i < 3; i++) d[i] = (uint8_t)(d[i] * d[3] / 255);
    }
    int e = lm_encode_image(c, format, (int)mp_param_int(p, "quality", 92), out);
    char json[96];
    snprintf(json, sizeof json, "{\"width\":%d,\"height\":%d}", cw, ch);
    lm_result(id, e ? -1 : 0, e ? "media.encodeFailed" : json, -1);
    av_frame_free(&c);
}

/* Pixels from lord-server (screenshots) → PNG/JPEG. */
static void job_encode(uint32_t id, int mfd, int out, int w, int h, const char *p) {
    struct stat st;
    if (w <= 0 || h <= 0 || w > MP_MAX_DIM || h > MP_MAX_DIM || fstat(mfd, &st) || st.st_size < (off_t)w * h * 4) {
        lm_result(id, -1, "media.invalidPixels", -1);
        return;
    }
    void *m = mmap(NULL, (size_t)w * h * 4, PROT_READ, MAP_SHARED, mfd, 0);
    if (m == MAP_FAILED) { lm_result(id, -1, "media.mapFailed", -1); return; }
    AVFrame *f = av_frame_alloc();
    f->format = AV_PIX_FMT_BGRA; f->width = w; f->height = h;
    av_frame_get_buffer(f, 0);
    for (int y = 0; y < h; y++) memcpy(f->data[0] + (size_t)y * f->linesize[0], (uint8_t *)m + (size_t)y * w * 4, (size_t)w * 4);
    munmap(m, (size_t)w * h * 4);
    char format[16];
    if (mp_param(p, "format", format, sizeof format)) snprintf(format, sizeof format, "png");
    int r = lm_encode_image(f, format, (int)mp_param_int(p, "quality", 92), out);
    av_frame_free(&f);
    lm_result(id, r ? -1 : 0, r ? "media.encodeFailed" : "{}", -1);
}

/* ---- audio: decode everything (or a segment) to float mono/stereo ---- */

typedef struct {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    SwrContext *swr;
    int ai, rate, channels;
    AVPacket *pkt;
    AVFrame *frame;
} AudioIn;

static int audio_open(AudioIn *a, int in, int rate, int channels, char *err, size_t errlen) {
    memset(a, 0, sizeof *a);
    a->fmt = lm_open_input(in, err, errlen);
    if (!a->fmt) return -1;
    a->ai = av_find_best_stream(a->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (a->ai < 0) { lerr_set(err, errlen, "media.noAudio", NULL); return -1; }
    AVStream *st = a->fmt->streams[a->ai];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    a->dec = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!a->dec) { lerr_set(err, errlen, "media.noDecoder", NULL); return -1; }
    avcodec_parameters_to_context(a->dec, st->codecpar);
    a->dec->pkt_timebase = st->time_base;
    if (avcodec_open2(a->dec, codec, NULL) < 0) { lerr_set(err, errlen, "media.audioOpenFailed", NULL); return -1; }
    a->rate = rate > 0 ? rate : a->dec->sample_rate;
    a->channels = channels;
    AVChannelLayout out;
    av_channel_layout_default(&out, channels);
    if (swr_alloc_set_opts2(&a->swr, &out, AV_SAMPLE_FMT_FLT, a->rate, &a->dec->ch_layout, a->dec->sample_fmt, a->dec->sample_rate, 0, NULL) < 0 ||
        swr_init(a->swr) < 0) { lerr_set(err, errlen, "media.audioConvertFailed", NULL); return -1; }
    a->pkt = av_packet_alloc();
    a->frame = av_frame_alloc();
    return 0;
}

static void audio_close(AudioIn *a) {
    swr_free(&a->swr);
    avcodec_free_context(&a->dec);
    av_packet_free(&a->pkt);
    av_frame_free(&a->frame);
    lm_close_input(&a->fmt);
}

/* Requests the next block of interleaved float samples. Returns the count (per channel),
 * 0 at the end. buf: malloc that grows. */
static int audio_read(AudioIn *a, float **buf, int *cap) {
    for (;;) {
        int r = avcodec_receive_frame(a->dec, a->frame);
        if (r >= 0) {
            int n = swr_get_out_samples(a->swr, a->frame->nb_samples);
            if (n > *cap) { float *nb = realloc(*buf, (size_t)n * a->channels * sizeof(float)); if (!nb) return 0; *buf = nb; *cap = n; }
            uint8_t *o[1] = { (uint8_t *)*buf };
            int got = swr_convert(a->swr, o, n, (const uint8_t **)a->frame->extended_data, a->frame->nb_samples);
            av_frame_unref(a->frame);
            if (got > 0) return got;
            continue;
        }
        if (r == AVERROR_EOF) return 0;
        r = av_read_frame(a->fmt, a->pkt);
        if (r < 0) { avcodec_send_packet(a->dec, NULL); continue; }
        if (a->pkt->stream_index == a->ai) avcodec_send_packet(a->dec, a->pkt);
        av_packet_unref(a->pkt);
    }
}

static void job_waveform(uint32_t id, int in, int bars) {
    char err[128];
    AudioIn a;
    if (bars < 8) bars = 8;
    if (bars > 2000) bars = 2000;
    if (audio_open(&a, in, 8000, 1, err, sizeof err)) { audio_close(&a); lm_result(id, -1, err, -1); return; }
    int64_t dur = a.fmt->duration > 0 ? a.fmt->duration / 1000 : 0;
    int64_t total = dur > 0 ? dur * 8 : 8000 * 600;            /* expected samples at 8 kHz */
    float *peaks = calloc((size_t)bars, sizeof(float)), *buf = NULL;
    int cap = 0, n;
    int64_t pos = 0;
    while ((n = audio_read(&a, &buf, &cap)) > 0) {
        for (int i = 0; i < n; i++, pos++) {
            int64_t bi = pos * bars / total;
            if (bi >= bars) bi = bars - 1;
            float v = fabsf(buf[i]);
            if (v > peaks[bi]) peaks[bi] = v;
        }
    }
    LmBuf b = { 0 };
    lm_buf_printf(&b, "{\"duration\":%lld,\"peaks\":[", (long long)(dur ? dur : pos / 8));
    int used = dur ? bars : (int)(pos * bars / total) + 1;
    if (used > bars) used = bars;
    for (int i = 0; i < used; i++) lm_buf_printf(&b, "%s%d", i ? "," : "", (int)fminf(1000, peaks[i] * 1000));
    lm_buf_printf(&b, "]}");
    lm_result(id, 0, b.d, -1);
    lm_buf_free(&b);
    free(peaks);
    free(buf);
    audio_close(&a);
}

/* Export audio: Opus (Ogg), MP3 or WAV, with optional trimming (from/to in ms). */
static void job_transcode(uint32_t id, int in, int out, const char *p) {
    char err[128], format[16];
    if (mp_param(p, "format", format, sizeof format)) snprintf(format, sizeof format, "opus");
    int opus = !strcmp(format, "opus"), mp3 = !strcmp(format, "mp3");
    if (!opus && !mp3 && strcmp(format, "wav")) { lm_result(id, -1, "media.unsupportedFormat", -1); return; }
    const AVCodec *codec = opus ? avcodec_find_encoder_by_name("libopus") : mp3 ? avcodec_find_encoder_by_name("libmp3lame")
                                : avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
    if (!codec && opus) codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    if (!codec) { lm_result(id, -1, "media.noEncoder", -1); return; }
    AudioIn a;
    int rate = mp3 ? 44100 : 48000;           /* MP3 at 44.1 kHz; Opus and WAV like the recorder */
    if (audio_open(&a, in, rate, 1, err, sizeof err)) { audio_close(&a); lm_result(id, -1, err, -1); return; }
    /* mono if the source is mono (the recorder records in mono); otherwise, stereo */
    int ch = a.dec->ch_layout.nb_channels >= 2 ? 2 : 1;
    if (ch == 2) { audio_close(&a); if (audio_open(&a, in, rate, 2, err, sizeof err)) { audio_close(&a); lm_result(id, -1, err, -1); return; } }
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    av_channel_layout_default(&enc->ch_layout, ch);
    enc->sample_rate = rate;
    const void *fmts = NULL;
    int nfmts = 0;
    if (avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &fmts, &nfmts) < 0 || !fmts || nfmts < 1) fmts = NULL;
    enc->sample_fmt = fmts ? ((const enum AVSampleFormat *)fmts)[0] : AV_SAMPLE_FMT_S16;
    enc->bit_rate = opus ? 64000 * ch : mp3 ? 96000 * ch : 0;
    enc->time_base = (AVRational){ 1, rate };
    AVFormatContext *oc = lm_open_output(out, opus ? "ogg" : mp3 ? "mp3" : "wav");
    if (!oc) { avcodec_free_context(&enc); audio_close(&a); lm_result(id, -1, "media.noOutput", -1); return; }
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(enc, codec, NULL) < 0) { lm_close_output(oc, 0); avcodec_free_context(&enc); audio_close(&a); lm_result(id, -1, "media.encoderOpenFailed", -1); return; }
    AVStream *os = avformat_new_stream(oc, NULL);
    avcodec_parameters_from_context(os->codecpar, enc);
    os->time_base = enc->time_base;
    if (avformat_write_header(oc, NULL) < 0) { lm_close_output(oc, 0); avcodec_free_context(&enc); audio_close(&a); lm_result(id, -1, "media.writeFailed", -1); return; }
    int64_t from = mp_param_int(p, "from", 0) * rate / 1000, to = mp_param_int(p, "to", -1);
    to = to < 0 ? INT64_MAX : to * rate / 1000;
    /* interleaved float → the encoder's format, in frames of frame_size */
    SwrContext *cv = NULL;
    swr_alloc_set_opts2(&cv, &enc->ch_layout, enc->sample_fmt, rate, &enc->ch_layout, AV_SAMPLE_FMT_FLT, rate, 0, NULL);
    swr_init(cv);
    int fsz = enc->frame_size > 0 ? enc->frame_size : 1152;
    AVFrame *ef = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    float *buf = NULL;
    int cap = 0, n;
    int64_t pos = 0, pts = 0;
    int okw = 0;
    for (int flushing = 0; ; ) {
        n = flushing ? 0 : audio_read(&a, &buf, &cap);
        if (n > 0) {
            int64_t s0 = pos, s1 = pos + n;
            pos = s1;
            if (s1 <= from || s0 >= to) { if (s0 >= to) flushing = 1; continue; }
            int off = s0 < from ? (int)(from - s0) : 0, end = s1 > to ? (int)(to - s0) : n;
            const uint8_t *src[1] = { (const uint8_t *)(buf + (size_t)off * ch) };
            swr_convert(cv, NULL, 0, src, end - off);
        } else flushing = 1;
        while (swr_get_out_samples(cv, 0) >= fsz || (flushing && swr_get_out_samples(cv, 0) > 0)) {
            ef->nb_samples = fsz;
            ef->format = enc->sample_fmt;
            ef->sample_rate = rate;
            av_channel_layout_copy(&ef->ch_layout, &enc->ch_layout);
            if (av_frame_get_buffer(ef, 0) < 0) break;
            int got = swr_convert(cv, ef->extended_data, fsz, NULL, 0);
            if (got <= 0) { av_frame_unref(ef); break; }
            ef->nb_samples = got;
            ef->pts = pts;
            pts += got;
            if (avcodec_send_frame(enc, ef) >= 0)
                while (avcodec_receive_packet(enc, pkt) >= 0) {
                    av_packet_rescale_ts(pkt, enc->time_base, os->time_base);
                    pkt->stream_index = os->index;
                    av_interleaved_write_frame(oc, pkt);
                    okw = 1;
                }
            av_frame_unref(ef);
        }
        if (flushing) break;
    }
    avcodec_send_frame(enc, NULL);
    while (avcodec_receive_packet(enc, pkt) >= 0) {
        av_packet_rescale_ts(pkt, enc->time_base, os->time_base);
        pkt->stream_index = os->index;
        av_interleaved_write_frame(oc, pkt);
        okw = 1;
    }
    int r = lm_close_output(oc, 1);
    char json[96];
    snprintf(json, sizeof json, "{\"duration\":%lld}", (long long)(pts * 1000 / rate));
    lm_result(id, (r < 0 || !okw) ? -1 : 0, (r < 0 || !okw) ? "media.exportFailed" : json, -1);
    swr_free(&cv);
    av_frame_free(&ef);
    av_packet_free(&pkt);
    free(buf);
    avcodec_free_context(&enc);
    audio_close(&a);
}

/* Sandbox test (build/media-test sandbox): every attempt has to fail. */
static void job_selftest(uint32_t id) {
    LmBuf b = { 0 };
    int fd = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
    lm_buf_printf(&b, "{\"open_etc_passwd\":\"%s\"", fd < 0 ? strerror(errno) : "ABIERTO");
    if (fd >= 0) close(fd);
    fd = open("/home", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    lm_buf_printf(&b, ",\"open_home\":\"%s\"", fd < 0 ? strerror(errno) : "ABIERTO");
    if (fd >= 0) close(fd);
    fd = open("/tmp/lord-media-test", O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    lm_buf_printf(&b, ",\"create_tmp\":\"%s\"", fd < 0 ? strerror(errno) : "CREADO");
    if (fd >= 0) close(fd);
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    lm_buf_printf(&b, ",\"socket_inet\":\"%s\"", fd < 0 ? strerror(errno) : "ABIERTO");
    if (fd >= 0) close(fd);
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    lm_buf_printf(&b, ",\"socket_unix\":\"%s\"", fd < 0 ? strerror(errno) : "ABIERTO");
    if (fd >= 0) close(fd);
    pid_t pid = fork();
    if (pid == 0) _exit(0);
    lm_buf_printf(&b, ",\"fork\":\"%s\"", pid < 0 ? strerror(errno) : "CREADO");
    char *argv[] = { "/bin/true", NULL };
    lm_buf_printf(&b, ",\"exec\":\"%s\"}", execve(argv[0], argv, NULL) < 0 ? strerror(errno) : "?");
    lm_result(id, 0, b.d, -1);
    lm_buf_free(&b);
}

void lm_worker(void) {
    static char payload[MP_PAYLOAD_MAX + 1];
    for (;;) {
        struct pollfd p = { LM_SOCK, POLLIN, 0 };
        int r = poll(&p, 1, IDLE_MS);
        if (r == 0) { LM_DBG("no work: exiting"); return; }
        if (r < 0 && errno == EINTR) continue;
        MediaMsg m;
        int fds[MP_MAX_FDS];
        int got = mp_recv(LM_SOCK, &m, payload, sizeof payload, fds, 1);
        if (got == 0) return;
        if (got < 0) continue;
        if (m.type == MM_STOP) { closefds(fds); return; }
        if (m.type != MM_JOB) { closefds(fds); continue; }
        double t0 = lm_now_ms();
        switch (m.v[0]) {
        case MW_PROBE: job_probe(m.id, fds[0]); break;
        case MW_THUMB: job_thumb(m.id, fds[0], fds[1], (int)m.v[1], fds[1] < 0); break;
        case MW_IMAGE: job_image(m.id, fds[0]); break;
        case MW_EDIT: job_edit(m.id, fds[0], fds[1], payload); break;
        case MW_ENCODE: job_encode(m.id, fds[0], fds[1], (int)m.v[1], (int)m.v[2], payload); break;
        case MW_TRANSCODE: job_transcode(m.id, fds[0], fds[1], payload); break;
        case MW_WAVEFORM: job_waveform(m.id, fds[0], (int)m.v[1]); break;
        case MW_SELFTEST: job_selftest(m.id); break;
        case MW_SVG: job_svg(m.id, fds[0], (int)m.v[1], (long)m.v[2]); break;
        default: lm_result(m.id, -1, "media.unknownJob", -1);
        }
        LM_DBG("trabajo %u (%lld) en %.1f ms", m.id, (long long)m.v[0], lm_now_ms() - t0);
        closefds(fds);
    }
}
