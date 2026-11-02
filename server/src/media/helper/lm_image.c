/* lm_image.c — images in axia-media: decode a frame (photo, cover art or a frame of a
 * video), convert it to BGRA, encode PNG/JPEG and describe a file in JSON (probe). */
#include "lm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

static int open_decoder(AVStream *st, AVCodecContext **out, int threads) {
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return -1;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (!c) return -1;
    avcodec_parameters_to_context(c, st->codecpar);
    c->thread_count = threads;
    c->pkt_timebase = st->time_base;
    if (avcodec_open2(c, codec, NULL) < 0) { avcodec_free_context(&c); return -1; }
    *out = c;
    return 0;
}

static int orientation_of(const AVFrame *f, const AVStream *st) {
    /* JPEG EXIF: FFmpeg leaves it in the frame's metadata; videos: rotation matrix */
    const AVDictionaryEntry *e = av_dict_get(f->metadata, "Orientation", NULL, 0);
    if (e) {
        int o = atoi(e->value);
        return o == 6 ? 90 : o == 3 ? 180 : o == 8 ? 270 : 0;
    }
    const AVFrameSideData *sd = av_frame_get_side_data(f, AV_FRAME_DATA_DISPLAYMATRIX);
    const int32_t *m = sd ? (const int32_t *)sd->data : NULL;
    if (!m && st) {
        const AVPacketSideData *p = av_packet_side_data_get(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
                                                            AV_PKT_DATA_DISPLAYMATRIX);
        if (p && p->size >= 36) m = (const int32_t *)p->data;
    }
    if (!m) return 0;
    double r = av_display_rotation_get(m);
    if (isnan(r)) return 0;
    int deg = ((int)lround(-r) % 360 + 360) % 360;
    return deg == 90 || deg == 180 || deg == 270 ? deg : 0;
}

AVFrame *lm_decode_still(AVFormatContext *fmt, int64_t pos_ms, int *orientation, int prefer_cover) {
    int vi = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) { if (prefer_cover || vi < 0) vi = (int)i; }
    int cover = vi >= 0 && (prefer_cover || av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0) == vi);
    if (!cover) vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vi < 0) return NULL;
    AVStream *st = fmt->streams[vi];
    AVCodecContext *dec = NULL;
    if (open_decoder(st, &dec, 2)) return NULL;
    AVFrame *f = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int got = 0;
    if (cover) {
        if (avcodec_send_packet(dec, &st->attached_pic) >= 0) { avcodec_send_packet(dec, NULL); got = avcodec_receive_frame(dec, f) >= 0; }
    } else {
        if (pos_ms > 0 && fmt->duration > 0 && !(fmt->iformat->flags & AVFMT_NOTIMESTAMPS)) {
            int64_t ts = av_rescale_q(pos_ms, (AVRational){ 1, 1000 }, st->time_base);
            if (st->start_time != AV_NOPTS_VALUE) ts += st->start_time;
            av_seek_frame(fmt, vi, ts, AVSEEK_FLAG_BACKWARD);
        }
        for (int n = 0; n < 600 && !got; n++) {
            int r = av_read_frame(fmt, pkt);
            if (r < 0) { avcodec_send_packet(dec, NULL); got = avcodec_receive_frame(dec, f) >= 0; break; }
            if (pkt->stream_index == vi && avcodec_send_packet(dec, pkt) >= 0) got = avcodec_receive_frame(dec, f) >= 0;
            av_packet_unref(pkt);
        }
    }
    if (got && orientation) *orientation = orientation_of(f, st);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    if (!got || f->width <= 0 || f->height <= 0) { av_frame_free(&f); return NULL; }
    return f;
}

AVFrame *lm_to_bgra(const AVFrame *src, int w, int h, int premultiply) {
    if (w <= 0 || h <= 0 || w > MP_MAX_DIM || h > MP_MAX_DIM) return NULL;
    int downscale = w * 2 < src->width || h * 2 < src->height;
    struct SwsContext *sws = sws_getContext(src->width, src->height, src->format, w, h, AV_PIX_FMT_BGRA,
                                            (downscale ? SWS_AREA : SWS_BICUBIC) | SWS_ACCURATE_RND, NULL, NULL, NULL);
    if (!sws) return NULL;
    AVFrame *o = av_frame_alloc();
    o->format = AV_PIX_FMT_BGRA;
    o->width = w;
    o->height = h;
    if (!o || av_frame_get_buffer(o, 0) < 0) { av_frame_free(&o); sws_freeContext(sws); return NULL; }
    sws_scale(sws, (const uint8_t *const *)src->data, src->linesize, 0, src->height, o->data, o->linesize);
    sws_freeContext(sws);
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(src->format);
    int alpha = d && (d->flags & AV_PIX_FMT_FLAG_ALPHA);
    for (int y = 0; y < h; y++) {
        uint8_t *p = o->data[0] + (size_t)y * o->linesize[0];
        for (int x = 0; x < w; x++, p += 4) {
            if (!alpha) { p[3] = 255; continue; }
            if (premultiply && p[3] < 255) { unsigned a = p[3]; p[0] = (uint8_t)((p[0] * a + 127) / 255); p[1] = (uint8_t)((p[1] * a + 127) / 255); p[2] = (uint8_t)((p[2] * a + 127) / 255); }
        }
    }
    return o;
}

int lm_encode_image(const AVFrame *bgra, const char *format, int quality, int out_fd) {
    int jpeg = format && (!strcmp(format, "jpeg") || !strcmp(format, "jpg"));
    int alpha = 0;
    if (!jpeg)
        for (int y = 0; y < bgra->height && !alpha; y++) {
            const uint8_t *p = bgra->data[0] + (size_t)y * bgra->linesize[0];
            for (int x = 0; x < bgra->width; x++) if (p[x * 4 + 3] != 255) { alpha = 1; break; }
        }
    const AVCodec *codec = avcodec_find_encoder(jpeg ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_PNG);
    if (!codec) return -1;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    enum AVPixelFormat pf = jpeg ? AV_PIX_FMT_YUVJ420P : alpha ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24;
    c->width = bgra->width;
    c->height = bgra->height;
    c->pix_fmt = pf;
    c->time_base = (AVRational){ 1, 25 };
    c->color_range = AVCOL_RANGE_JPEG;
    if (jpeg) {
        int q = quality <= 0 ? 90 : quality > 100 ? 100 : quality;
        int qs = 2 + (100 - q) * 29 / 100;                 /* quality 100 → q 2, 0 → q 31 */
        c->flags |= AV_CODEC_FLAG_QSCALE;
        c->global_quality = FF_QP2LAMBDA * qs;
        c->qmin = c->qmax = qs;
    }
    if (avcodec_open2(c, codec, NULL) < 0) { avcodec_free_context(&c); return -1; }
    /* premultiplied BGRA → unpremultiplied → the encoder's format */
    AVFrame *straight = av_frame_alloc();
    straight->format = AV_PIX_FMT_BGRA;
    straight->width = bgra->width;
    straight->height = bgra->height;
    av_frame_get_buffer(straight, 0);
    for (int y = 0; y < bgra->height; y++) {
        const uint8_t *s = bgra->data[0] + (size_t)y * bgra->linesize[0];
        uint8_t *d = straight->data[0] + (size_t)y * straight->linesize[0];
        for (int x = 0; x < bgra->width; x++, s += 4, d += 4) {
            unsigned a = s[3];
            if (a == 255 || a == 0) { memcpy(d, s, 4); continue; }
            d[0] = (uint8_t)(s[0] * 255u / a); d[1] = (uint8_t)(s[1] * 255u / a); d[2] = (uint8_t)(s[2] * 255u / a); d[3] = (uint8_t)a;
        }
    }
    AVFrame *f = av_frame_alloc();
    f->format = pf;
    f->width = bgra->width;
    f->height = bgra->height;
    f->color_range = AVCOL_RANGE_JPEG;
    av_frame_get_buffer(f, 0);
    struct SwsContext *sws = sws_getContext(bgra->width, bgra->height, AV_PIX_FMT_BGRA, f->width, f->height, pf,
                                            SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, NULL, NULL, NULL);
    int r = -1;
    if (sws) {
        sws_scale(sws, (const uint8_t *const *)straight->data, straight->linesize, 0, f->height, f->data, f->linesize);
        sws_freeContext(sws);
        AVPacket *pkt = av_packet_alloc();
        if (avcodec_send_frame(c, f) >= 0 && avcodec_send_frame(c, NULL) >= 0 && avcodec_receive_packet(c, pkt) >= 0) {
            r = lm_write_all(out_fd, pkt->data, (size_t)pkt->size);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
    av_frame_free(&straight);
    av_frame_free(&f);
    avcodec_free_context(&c);
    return r;
}

/* Tags of interest (in the container or, in Ogg/Opus/FLAC, in the audio track). */
static void tags_json(AVFormatContext *fmt, LmBuf *b) {
    static const char *const keys[] = { "title", "artist", "album", "album_artist", "date", "genre", "track", "disc", "comment", "composer", NULL };
    AVDictionary *dicts[2] = { fmt->metadata, NULL };
    int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ai >= 0) dicts[1] = fmt->streams[ai]->metadata;
    lm_buf_printf(b, "\"tags\":{");
    int first = 1;
    for (int k = 0; keys[k]; k++) {
        const AVDictionaryEntry *e = NULL;
        for (int d = 0; d < 2 && !e; d++) if (dicts[d]) e = av_dict_get(dicts[d], keys[k], NULL, 0);
        if (!e || !e->value[0]) continue;
        lm_buf_printf(b, "%s\"%s\":", first ? "" : ",", keys[k]);
        lm_buf_str(b, e->value);
        first = 0;
    }
    lm_buf_printf(b, "}");
}

void lm_probe_json(AVFormatContext *fmt, LmBuf *b) {
    int64_t dur = fmt->duration > 0 ? fmt->duration / 1000 : 0;
    lm_buf_printf(b, "{\"format\":");
    lm_buf_str(b, fmt->iformat ? fmt->iformat->name : "");
    lm_buf_printf(b, ",\"duration\":%lld,\"bitrate\":%lld,", (long long)dur, (long long)fmt->bit_rate);
    tags_json(fmt, b);
    lm_buf_printf(b, ",\"streams\":[");
    for (unsigned i = 0; i < fmt->nb_streams && i < 64; i++) {
        AVStream *st = fmt->streams[i];
        AVCodecParameters *p = st->codecpar;
        const char *type = p->codec_type == AVMEDIA_TYPE_VIDEO ? "video" : p->codec_type == AVMEDIA_TYPE_AUDIO ? "audio"
                         : p->codec_type == AVMEDIA_TYPE_SUBTITLE ? "subtitle" : "other";
        const AVCodecDescriptor *cd = avcodec_descriptor_get(p->codec_id);
        lm_buf_printf(b, "%s{\"index\":%u,\"type\":\"%s\",\"codec\":", i ? "," : "", i, type);
        lm_buf_str(b, cd ? cd->name : "?");
        const AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0), *title = av_dict_get(st->metadata, "title", NULL, 0);
        if (lang) { lm_buf_printf(b, ",\"lang\":"); lm_buf_str(b, lang->value); }
        if (title) { lm_buf_printf(b, ",\"title\":"); lm_buf_str(b, title->value); }
        if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
            double fps = st->avg_frame_rate.den ? av_q2d(st->avg_frame_rate) : 0;
            lm_buf_printf(b, ",\"width\":%d,\"height\":%d,\"fps\":%.3f,\"cover\":%s", p->width, p->height, fps,
                          (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ? "true" : "false");
        } else if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
            lm_buf_printf(b, ",\"channels\":%d,\"rate\":%d", p->ch_layout.nb_channels, p->sample_rate);
        } else if (p->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            int text = cd && (cd->props & AV_CODEC_PROP_TEXT_SUB);
            lm_buf_printf(b, ",\"text\":%s", text ? "true" : "false");
        }
        lm_buf_printf(b, ",\"default\":%s}", (st->disposition & AV_DISPOSITION_DEFAULT) ? "true" : "false");
    }
    lm_buf_printf(b, "]}");
}
