/* lm_stream.c — the remote desktop's live encoder (MK_STREAM, docs/REMOTE.md, "Video (WebRTC)").
 *
 * One per session and codec: lord-server copies each changed frame of the session (protected windows
 * already black) into a free slot of a sealed memfd (MM_INRING / MM_INFRAME), as for the screen
 * recording; here it becomes H.264 (VA-API when the card encodes it, else libx264) or VP8 (libvpx),
 * tuned for latency: no B-frames, one frame in, one frame out, a keyframe only when asked
 * (MM_KEYFRAME: a new viewer or a lost packet) and the bit rate the slowest viewer can take
 * (MM_RATE). The session's sound arrives in an audio ring that lord-server fills from PipeWire and
 * goes out as Opus (20 ms). Every encoded frame goes back as MM_PACKET chunks; the bridges of the
 * session's viewers only add the per-connection encryption. */
#include "lm.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

enum { CODEC_H264 = 1, CODEC_VP8 = 2 };

typedef struct {
    AVCodecContext *c;
    AVBufferRef *frames;            /* VA-API surfaces */
    struct SwsContext *sws;
    AVFrame *yuv;
    AVPacket *pkt;
    int w, h, codec, hw, force_key;
    const char *name;
    unsigned kbps;
} Vid;

typedef struct {
    AVCodecContext *c;
    AVFrame *f;
    AVPacket *pkt;
    int16_t buf[960 * 4];
    int have;
    int64_t pts;
} Aud;

static Vid V;
static Aud A;

/* Blocking send of one MM_PACKET message (the helper may wait; lord-server never does). */
static int send_wait(const int64_t v[6], uint32_t id, const uint8_t *p, size_t n) {
    for (int tries = 0; tries < 50; tries++) {
        if (!mp_send(LM_SOCK, MM_PACKET, id, v, p, n, NULL, 0)) return 0;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS) return -1;
        struct pollfd pf = { LM_SOCK, POLLOUT, 0 };
        poll(&pf, 1, 20);
    }
    return -1;
}

/* A packet as chunks of at most MP_PAYLOAD_MAX: v[0] total, v[1] offset, v[2] pts, v[3] flags. */
static void send_packet(uint32_t kind, const uint8_t *d, size_t n, int64_t pts, int key) {
    if (!n || n > (8u << 20)) return;
    for (size_t off = 0; off < n; off += MP_PAYLOAD_MAX) {
        size_t c = n - off > MP_PAYLOAD_MAX ? MP_PAYLOAD_MAX : n - off;
        int64_t v[6] = { (int64_t)n, (int64_t)off, pts, key, 0, 0 };
        if (send_wait(v, kind, d + off, c)) return;
    }
}

static AVCodecContext *open_h264_vaapi(Vid *e, AVBufferRef *hw, int fps) {
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!hw || !codec) return NULL;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    AVBufferRef *fr = av_hwframe_ctx_alloc(hw);
    AVHWFramesContext *fc = fr ? (AVHWFramesContext *)fr->data : NULL;
    if (!c || !fc) { av_buffer_unref(&fr); avcodec_free_context(&c); return NULL; }
    fc->format = AV_PIX_FMT_VAAPI;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = e->w;
    fc->height = e->h;
    fc->initial_pool_size = 4;
    if (av_hwframe_ctx_init(fr) < 0) { av_buffer_unref(&fr); avcodec_free_context(&c); return NULL; }
    c->hw_frames_ctx = av_buffer_ref(fr);
    c->pix_fmt = AV_PIX_FMT_VAAPI;
    c->width = e->w;
    c->height = e->h;
    c->time_base = (AVRational){ 1, 1000 };
    c->framerate = (AVRational){ fps, 1 };
    c->gop_size = fps * 60;
    c->max_b_frames = 0;
    c->profile = AV_PROFILE_H264_CONSTRAINED_BASELINE;
    c->bit_rate = (int64_t)e->kbps * 1000;
    c->rc_max_rate = c->bit_rate;
    c->rc_buffer_size = (int)(c->bit_rate / 2);
    av_opt_set(c->priv_data, "rc_mode", "VBR", 0);
    if (avcodec_open2(c, codec, NULL) < 0) { av_buffer_unref(&fr); avcodec_free_context(&c); LM_LOG("VA-API does not encode H.264 here: libx264"); return NULL; }
    e->frames = fr;
    e->hw = 1;
    e->name = "h264_vaapi";
    return c;
}

static AVCodecContext *open_sw(Vid *e, const char *name, int fps) {
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (!codec) return NULL;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (!c) return NULL;
    c->width = e->w;
    c->height = e->h;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->time_base = (AVRational){ 1, 1000 };
    c->framerate = (AVRational){ fps, 1 };
    c->gop_size = fps * 60;
    c->max_b_frames = 0;
    /* the desktop is sRGB: BT.709 primaries and matrix with the sRGB transfer, so the browser doesn't
     * re-shape the tones (dark greys came out lighter than the lossless tiles next to them) */
    c->color_range = AVCOL_RANGE_MPEG;
    c->colorspace = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_IEC61966_2_1;
    AVDictionary *o = NULL;
    if (e->codec == CODEC_H264) {
        c->thread_count = 2;
        c->rc_max_rate = (int64_t)e->kbps * 1000;
        c->rc_buffer_size = (int)(c->rc_max_rate / 3);       /* about 1/3 s: low latency */
        av_dict_set(&o, "preset", "ultrafast", 0);
        av_dict_set(&o, "tune", "zerolatency", 0);
        av_dict_set(&o, "profile", "baseline", 0);
        av_dict_set(&o, "crf", "22", 0);
        av_dict_set(&o, "forced-idr", "1", 0);
        /* ultrafast turns the deblocking filter off, which left video in visible blocks at a few
         * Mbit/s; it costs ~5 % of the encoder (1280x800 on a 1.8 GHz Xeon: 88 → 84 fps, 2 threads) */
        av_dict_set(&o, "x264-params", "repeat-headers=1:annexb=1:aud=0:scenecut=0:deblock=1", 0);
    } else {
        c->thread_count = 2;
        c->bit_rate = (int64_t)e->kbps * 1000;
        c->rc_min_rate = 0;
        c->rc_max_rate = c->bit_rate;
        c->rc_buffer_size = (int)(c->bit_rate / 3);
        c->qmin = 4;
        c->qmax = 56;
        av_dict_set(&o, "deadline", "realtime", 0);
        av_dict_set(&o, "cpu-used", "8", 0);
        av_dict_set(&o, "lag-in-frames", "0", 0);
        av_dict_set(&o, "error-resilient", "1", 0);
        av_dict_set(&o, "static-thresh", "100", 0);
    }
    int r = avcodec_open2(c, codec, &o);
    av_dict_free(&o);
    if (r < 0) { avcodec_free_context(&c); return NULL; }
    e->name = name;
    return c;
}

AVCodecContext *lm_stream_sw_encoder(int vp8, int w, int h, int fps, unsigned kbps) {
    Vid e;
    memset(&e, 0, sizeof e);
    e.codec = vp8 ? CODEC_VP8 : CODEC_H264;
    e.w = w & ~1;
    e.h = h & ~1;
    e.kbps = kbps;
    return open_sw(&e, vp8 ? "libvpx" : "libx264", fps);
}

static int vid_open(Vid *e, int codec, int w, int h, int fps, unsigned kbps, AVBufferRef *hw) {
    memset(e, 0, sizeof *e);
    e->codec = codec;
    e->w = w & ~1;
    e->h = h & ~1;
    e->kbps = kbps;
    if (e->w < 16 || e->h < 16) return -1;
    if (codec == CODEC_H264) {
        e->c = open_h264_vaapi(e, hw, fps);
        if (!e->c) e->c = open_sw(e, "libx264", fps);
    } else e->c = open_sw(e, "libvpx", fps);
    if (!e->c) return -1;
    e->pkt = av_packet_alloc();
    e->yuv = av_frame_alloc();
    e->yuv->format = e->frames ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    e->yuv->width = e->w;
    e->yuv->height = e->h;
    if (av_frame_get_buffer(e->yuv, 0) < 0) return -1;
    LM_LOG("remote desktop video: %dx%d with %s, up to %u kbit/s", e->w, e->h, e->name, kbps);
    return 0;
}

static void vid_close(Vid *e) {
    avcodec_free_context(&e->c);
    av_buffer_unref(&e->frames);
    sws_freeContext(e->sws);
    av_frame_free(&e->yuv);
    av_packet_free(&e->pkt);
}

static void vid_drain(Vid *e) {
    while (avcodec_receive_packet(e->c, e->pkt) >= 0) {
        send_packet(0, e->pkt->data, (size_t)e->pkt->size, e->pkt->pts, (e->pkt->flags & AV_PKT_FLAG_KEY) != 0);
        av_packet_unref(e->pkt);
    }
}

static int vid_frame(Vid *e, const uint8_t *bgra, int w, int h, int64_t ms) {
    if (!e->sws) {
        e->sws = sws_getContext(w, h, AV_PIX_FMT_BGRA, e->w, e->h, e->yuv->format, SWS_POINT, NULL, NULL, NULL);
        if (!e->sws) return -1;
        sws_setColorspaceDetails(e->sws, sws_getCoefficients(SWS_CS_ITU709), 1, sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);
    }
    if (av_frame_make_writable(e->yuv) < 0) return -1;
    const uint8_t *src[1] = { bgra };
    int stride[1] = { w * 4 };
    sws_scale(e->sws, src, stride, 0, h, e->yuv->data, e->yuv->linesize);
    AVFrame *in = e->yuv, *hwf = NULL;
    if (e->frames) {
        hwf = av_frame_alloc();
        if (av_hwframe_get_buffer(e->frames, hwf, 0) < 0 || av_hwframe_transfer_data(hwf, e->yuv, 0) < 0) { av_frame_free(&hwf); return -1; }
        in = hwf;
    }
    in->pts = ms;
    in->pict_type = e->force_key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (e->force_key) in->flags |= AV_FRAME_FLAG_KEY; else in->flags &= ~AV_FRAME_FLAG_KEY;
    e->force_key = 0;
    int r = avcodec_send_frame(e->c, in);
    av_frame_free(&hwf);
    if (r < 0) return r;
    vid_drain(e);
    return 0;
}

static void vid_rate(Vid *e, unsigned kbps) {
    if (!e->c || kbps == e->kbps) return;
    e->kbps = kbps;
    /* libx264 reconfigures itself when these change between frames; the others keep their rate */
    if (e->codec == CODEC_H264 && !e->hw) {
        e->c->rc_max_rate = (int64_t)kbps * 1000;
        e->c->rc_buffer_size = (int)(e->c->rc_max_rate / 3);
    }
}

static int aud_open(Aud *a) {
    const AVCodec *codec = avcodec_find_encoder_by_name("libopus");
    if (!codec) { LM_LOG("no Opus encoder: the remote desktop has no sound"); return -1; }
    a->c = avcodec_alloc_context3(codec);
    a->c->sample_fmt = AV_SAMPLE_FMT_S16;
    a->c->sample_rate = 48000;
    av_channel_layout_default(&a->c->ch_layout, 1);
    a->c->bit_rate = 64000;
    a->c->time_base = (AVRational){ 1, 48000 };
    AVDictionary *o = NULL;
    av_dict_set(&o, "application", "lowdelay", 0);
    av_dict_set(&o, "frame_duration", "20", 0);
    int r = avcodec_open2(a->c, codec, &o);
    av_dict_free(&o);
    if (r < 0) { avcodec_free_context(&a->c); return -1; }
    a->f = av_frame_alloc();
    a->pkt = av_packet_alloc();
    return 0;
}

static void on_pcm(const int16_t *s, int k) {
    Aud *a = &A;
    if (!a->c) return;
    while (k > 0) {
        int take = 960 - a->have;
        if (take > k) take = k;
        memcpy(a->buf + a->have, s, (size_t)take * 2);
        a->have += take;
        s += take;
        k -= take;
        if (a->have < 960) break;
        a->f->nb_samples = 960;
        a->f->format = AV_SAMPLE_FMT_S16;
        a->f->sample_rate = 48000;
        av_channel_layout_default(&a->f->ch_layout, 1);
        if (av_frame_get_buffer(a->f, 0) < 0) return;
        memcpy(a->f->data[0], a->buf, 960 * 2);
        a->f->pts = a->pts;
        a->pts += 960;
        a->have = 0;
        if (avcodec_send_frame(a->c, a->f) >= 0)
            while (avcodec_receive_packet(a->c, a->pkt) >= 0) {
                send_packet(1, a->pkt->data, (size_t)a->pkt->size, a->pkt->pts, 0);
                av_packet_unref(a->pkt);
            }
        av_frame_unref(a->f);
    }
}

void lm_stream(const char *params, int ring_fd, int render) {
    char cname[16] = "";
    mp_param(params, "codec", cname, sizeof cname);
    int codec = !strcmp(cname, "vp8") ? CODEC_VP8 : !strcmp(cname, "h264") ? CODEC_H264 : 0;
    int w = (int)mp_param_int(params, "width", 0), h = (int)mp_param_int(params, "height", 0);
    int fps = (int)mp_param_int(params, "fps", 30);
    long kbps = mp_param_int(params, "kbps", 4000);
    if (fps < 5 || fps > 240) fps = 30;
    if (kbps < 200 || kbps > 50000) kbps = 4000;
    char accel[24];
    AVBufferRef *hw = codec == CODEC_H264 ? lm_hw_device(lm_hw_accel(params, accel, sizeof accel), render) : NULL;
    if (!codec || w < 16 || h < 16 || w > MP_MAX_DIM || h > MP_MAX_DIM || vid_open(&V, codec, w, h, fps, (unsigned)kbps, hw)) {
        lm_error("media.noVideoEncoder");
        mp_sendv(LM_SOCK, MM_STATE, 0, MS_FAILED, 0, 0, 0);
        return;
    }
    size_t ring_len = 0;
    void *ring = lm_mic_ring(ring_fd, &ring_len);
    if (ring_fd >= 0) close(ring_fd);
    if (ring && aud_open(&A)) { munmap(ring, ring_len); ring = NULL; }
    mp_sendv(LM_SOCK, MM_STATE, 0, MS_RECORDING, V.hw, 0, 0);
    static char payload[MP_PAYLOAD_MAX + 1];
    uint8_t *rmap = NULL;
    size_t ring_bytes = 0;
    int rw = 0, rh = 0, slots = 0, stop = 0;
    long frames = 0;
    double enc_ms = 0, last_stats = lm_now_ms();
    while (!stop) {
        struct pollfd pf[1] = { { LM_SOCK, POLLIN, 0 } };
        if (poll(pf, 1, ring ? 10 : 200) < 0 && errno != EINTR) break;
        if (pf[0].revents & (POLLHUP | POLLERR)) break;
        if (pf[0].revents & POLLIN) {
            MediaMsg m;
            int fds[MP_MAX_FDS], r;
            while ((r = mp_recv(LM_SOCK, &m, payload, sizeof payload, fds, 0)) != -1) {
                if (r == 0 || m.type == MM_STOP) { stop = 1; break; }
                if (m.type == MM_INRING && fds[0] >= 0 && !rmap) {
                    struct stat st;
                    rw = (int)m.v[1]; rh = (int)m.v[2]; slots = (int)m.v[0];
                    ring_bytes = (size_t)rw * rh * 4 * (size_t)slots;
                    if (rw == w && rh == h && slots > 0 && slots <= 4 && !fstat(fds[0], &st) && (size_t)st.st_size >= ring_bytes &&
                        (fcntl(fds[0], F_GET_SEALS) & F_SEAL_SHRINK)) {
                        void *mm = mmap(NULL, ring_bytes, PROT_READ, MAP_SHARED, fds[0], 0);
                        if (mm != MAP_FAILED) rmap = mm;
                    }
                } else if (m.type == MM_INFRAME && rmap && m.id < (uint32_t)slots) {
                    double t0 = lm_now_ms();
                    if (vid_frame(&V, rmap + (size_t)m.id * rw * rh * 4, rw, rh, m.v[0]) >= 0) frames++;
                    enc_ms += lm_now_ms() - t0;
                    mp_sendv(LM_SOCK, MM_SLOTFREE, m.id, 0, 0, 0, 0);
                } else if (m.type == MM_KEYFRAME) V.force_key = 1;
                else if (m.type == MM_RATE && m.v[0] >= 200 && m.v[0] <= 50000) vid_rate(&V, (unsigned)m.v[0]);
                for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]);
            }
        }
        if (ring) lm_mic_drain(ring, on_pcm);
        if (lm_now_ms() - last_stats >= 2000) {
            mp_sendv(LM_SOCK, MM_STATS, 0, frames, frames ? (int64_t)(enc_ms * 100 / frames) : 0, V.hw, V.kbps);
            last_stats = lm_now_ms();
        }
    }
    LM_LOG("remote desktop video: %ld frames, %.2f ms each with %s", frames, frames ? enc_ms / frames : 0.0, V.name);
    vid_close(&V);
    if (A.c) { avcodec_free_context(&A.c); av_frame_free(&A.f); av_packet_free(&A.pkt); }
    if (ring) munmap(ring, ring_len);
    if (rmap) munmap(rmap, ring_bytes);
    mp_sendv(LM_SOCK, MM_STATE, 0, MS_DONE, 0, 0, 0);
}
