/* lm_enc.c — axia-media's encoder for recording the screen and the camera: H.264 (VA-API if available;
 * otherwise, libx264; last resort MPEG-4) and AAC from the microphone, in MP4. Times in ms since the
 * start (variable frame rate: it only encodes when something changed). */
#include "lm.h"

#include <stdlib.h>
#include <string.h>

#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

struct LmEnc {
    AVFormatContext *oc;
    AVCodecContext *v, *a;
    AVStream *vs, *as;
    AVBufferRef *frames;            /* VA-API: surfaces */
    struct SwsContext *sws;
    int sw, sh, sfmt;
    AVFrame *yuv, *af;
    SwrContext *swr;
    int64_t last_pts, a_pts;
    int w, h, header;
    const char *name;
    AVPacket *pkt;
};

const char *lm_enc_name(const LmEnc *e) { return e->name; }

static int write_packets(LmEnc *e, AVCodecContext *c, AVStream *st) {
    int r;
    while ((r = avcodec_receive_packet(c, e->pkt)) >= 0) {
        av_packet_rescale_ts(e->pkt, c->time_base, st->time_base);
        e->pkt->stream_index = st->index;
        av_interleaved_write_frame(e->oc, e->pkt);
    }
    return r == AVERROR(EAGAIN) || r == AVERROR_EOF ? 0 : r;
}

static AVCodecContext *video_encoder(LmEnc *e, int fps, AVBufferRef *hw) {
    const AVCodec *codec = NULL;
    AVCodecContext *c = NULL;
    if (hw && (codec = avcodec_find_encoder_by_name("h264_vaapi"))) {
        c = avcodec_alloc_context3(codec);
        AVBufferRef *fr = av_hwframe_ctx_alloc(hw);
        AVHWFramesContext *fc = fr ? (AVHWFramesContext *)fr->data : NULL;
        if (fc) {
            fc->format = AV_PIX_FMT_VAAPI;
            fc->sw_format = AV_PIX_FMT_NV12;
            fc->width = e->w;
            fc->height = e->h;
            fc->initial_pool_size = 8;
        }
        if (fc && av_hwframe_ctx_init(fr) >= 0) {
            c->hw_frames_ctx = av_buffer_ref(fr);
            c->pix_fmt = AV_PIX_FMT_VAAPI;
            c->width = e->w; c->height = e->h;
            c->time_base = (AVRational){ 1, 1000 };
            c->framerate = (AVRational){ fps, 1 };
            c->gop_size = fps * 2;
            c->max_b_frames = 0;
            c->global_quality = 24;
            if (e->oc->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(c, codec, NULL) >= 0) { e->frames = fr; e->name = "h264_vaapi"; return c; }
        }
        av_buffer_unref(&fr);
        avcodec_free_context(&c);
        LM_LOG("VA-API no codifica H.264 en esta placa: libx264");
    }
    static const char *const sw[] = { "libx264", "mpeg4", NULL };
    for (int i = 0; sw[i]; i++) {
        codec = avcodec_find_encoder_by_name(sw[i]);
        if (!codec) continue;
        c = avcodec_alloc_context3(codec);
        c->width = e->w; c->height = e->h;
        c->pix_fmt = AV_PIX_FMT_YUV420P;
        c->time_base = (AVRational){ 1, 1000 };
        c->framerate = (AVRational){ fps, 1 };
        c->gop_size = fps * 2;
        c->max_b_frames = 0;
        c->thread_count = 0;
        c->color_range = AVCOL_RANGE_MPEG;
        c->colorspace = AVCOL_SPC_BT709;
        c->color_primaries = AVCOL_PRI_BT709;
        c->color_trc = AVCOL_TRC_BT709;
        if (e->oc->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary *o = NULL;
        if (!strcmp(sw[i], "libx264")) {
            av_dict_set(&o, "preset", "veryfast", 0);
            av_dict_set(&o, "tune", "zerolatency", 0);
            av_dict_set(&o, "crf", "23", 0);
        } else c->bit_rate = 6000000;
        int r = avcodec_open2(c, codec, &o);
        av_dict_free(&o);
        if (r >= 0) { e->name = sw[i]; return c; }
        avcodec_free_context(&c);
    }
    return NULL;
}

LmEnc *lm_enc_open(int out_fd, int w, int h, int fps, AVBufferRef *hw, int audio_rate, int audio_ch, char *err, size_t errlen) {
    LmEnc *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->w = w & ~1;
    e->h = h & ~1;
    e->last_pts = -1;
    e->pkt = av_packet_alloc();
    e->oc = lm_open_output(out_fd, "mp4");
    if (!e->oc || e->w < 16 || e->h < 16) { lerr_set(err, errlen, "media.createFailed", NULL); lm_enc_close(e); return NULL; }
    e->v = video_encoder(e, fps > 0 ? fps : 30, hw);
    if (!e->v) { lerr_set(err, errlen, "media.noVideoEncoder", NULL); lm_enc_close(e); return NULL; }
    e->vs = avformat_new_stream(e->oc, NULL);
    avcodec_parameters_from_context(e->vs->codecpar, e->v);
    e->vs->time_base = (AVRational){ 1, 1000 };
    if (audio_rate > 0) {
        const AVCodec *ac = avcodec_find_encoder(AV_CODEC_ID_AAC);
        e->a = ac ? avcodec_alloc_context3(ac) : NULL;
        if (e->a) {
            e->a->sample_fmt = AV_SAMPLE_FMT_FLTP;
            e->a->sample_rate = audio_rate;
            av_channel_layout_default(&e->a->ch_layout, audio_ch);
            e->a->bit_rate = 128000;
            e->a->time_base = (AVRational){ 1, audio_rate };
            if (e->oc->oformat->flags & AVFMT_GLOBALHEADER) e->a->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(e->a, ac, NULL) < 0) avcodec_free_context(&e->a);
        }
        if (e->a) {
            e->as = avformat_new_stream(e->oc, NULL);
            avcodec_parameters_from_context(e->as->codecpar, e->a);
            e->as->time_base = e->a->time_base;
            swr_alloc_set_opts2(&e->swr, &e->a->ch_layout, AV_SAMPLE_FMT_FLTP, audio_rate, &e->a->ch_layout, AV_SAMPLE_FMT_S16, audio_rate, 0, NULL);
            swr_init(e->swr);
        } else LM_LOG("no AAC: the recording has no sound");
    }
    if (avformat_write_header(e->oc, NULL) < 0) { lerr_set(err, errlen, "media.writeFailed", NULL); lm_enc_close(e); return NULL; }
    e->header = 1;
    e->yuv = av_frame_alloc();
    e->yuv->format = e->frames ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    e->yuv->width = e->w;
    e->yuv->height = e->h;
    av_frame_get_buffer(e->yuv, 0);
    LM_LOG("recording %dx%d with %s%s", e->w, e->h, e->name, e->a ? " + AAC" : "");
    return e;
}

int lm_enc_video(LmEnc *e, const AVFrame *f, int64_t t_ms) {
    if (!e || !e->v) return -1;
    if (t_ms <= e->last_pts) t_ms = e->last_pts + 1;
    if (!e->sws || e->sw != f->width || e->sh != f->height || e->sfmt != f->format) {
        sws_freeContext(e->sws);
        e->sws = sws_getContext(f->width, f->height, f->format, e->w, e->h, e->yuv->format, SWS_BILINEAR, NULL, NULL, NULL);
        if (!e->sws) return -1;
        sws_setColorspaceDetails(e->sws, sws_getCoefficients(SWS_CS_DEFAULT), f->color_range == AVCOL_RANGE_JPEG,
                                 sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);
        e->sw = f->width; e->sh = f->height; e->sfmt = f->format;
    }
    if (av_frame_make_writable(e->yuv) < 0) return -1;
    sws_scale(e->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, e->yuv->data, e->yuv->linesize);
    AVFrame *in = e->yuv, *hwf = NULL;
    if (e->frames) {
        hwf = av_frame_alloc();
        if (av_hwframe_get_buffer(e->frames, hwf, 0) < 0 || av_hwframe_transfer_data(hwf, e->yuv, 0) < 0) { av_frame_free(&hwf); return -1; }
        in = hwf;
    }
    in->pts = t_ms;
    e->last_pts = t_ms;
    int r = avcodec_send_frame(e->v, in);
    av_frame_free(&hwf);
    if (r < 0) return r;
    return write_packets(e, e->v, e->vs);
}

int lm_enc_audio(LmEnc *e, const int16_t *pcm, int samples) {
    if (!e || !e->a) return 0;
    const uint8_t *src[1] = { (const uint8_t *)pcm };
    swr_convert(e->swr, NULL, 0, src, samples);
    int fs = e->a->frame_size > 0 ? e->a->frame_size : 1024;
    while (swr_get_out_samples(e->swr, 0) >= fs) {
        AVFrame *f = av_frame_alloc();
        f->nb_samples = fs;
        f->format = AV_SAMPLE_FMT_FLTP;
        f->sample_rate = e->a->sample_rate;
        av_channel_layout_copy(&f->ch_layout, &e->a->ch_layout);
        if (av_frame_get_buffer(f, 0) < 0) { av_frame_free(&f); return -1; }
        int got = swr_convert(e->swr, f->extended_data, fs, NULL, 0);
        if (got <= 0) { av_frame_free(&f); break; }
        f->nb_samples = got;
        f->pts = e->a_pts;
        e->a_pts += got;
        avcodec_send_frame(e->a, f);
        av_frame_free(&f);
        write_packets(e, e->a, e->as);
    }
    return 0;
}

int64_t lm_enc_close(LmEnc *e) {
    if (!e) return -1;
    int64_t dur = e->last_pts;
    if (e->header) {
        if (e->v) { avcodec_send_frame(e->v, NULL); write_packets(e, e->v, e->vs); }
        if (e->a) { avcodec_send_frame(e->a, NULL); write_packets(e, e->a, e->as); }
    }
    if (e->oc) lm_close_output(e->oc, e->header);
    avcodec_free_context(&e->v);
    avcodec_free_context(&e->a);
    av_buffer_unref(&e->frames);
    sws_freeContext(e->sws);
    swr_free(&e->swr);
    av_frame_free(&e->yuv);
    av_packet_free(&e->pkt);
    int ok = e->header;
    free(e);
    return ok ? dur : -1;
}
