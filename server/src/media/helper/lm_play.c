/* lm_play.c — lord-media's player (MK_PLAY).
 *
 * FFmpeg only demuxes and decodes; the rest is our own:
 *   - a decoding thread with per-track queues (packets) and bounded output queues
 *     (video frames, PCM), like ffplay but smaller;
 *   - the main thread serves lord-server, gives the audio to PipeWire as it asks for it
 *     and presents each frame when it's due according to the audio clock (what PipeWire has already
 *     played, with its latency: GET_PLAYBACK_LATENCY). Without audio, the clock is the system's.
 *   - a frame that arrives late is dropped (and counted), so as not to delay the audio;
 *   - each frame is scaled only once to the size at which it's shown and written into one of
 *     three shared-memory slots with lord-server (sealed memfd). lord-server copies it
 *     once when compositing: there are no more copies.
 *   - VA-API (if lord-server passed the render node and there's a driver): the GPU decodes and the
 *     frame (NV12) is downloaded to scale it. */
#include "lm.h"
#include "../aring.h"

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "lm_hwfall.h"

#define VQ_MAX   6                   /* decoded frames waiting */
#define AQ_MS    1000                /* decoded PCM waiting */
#define PKTQ_MAX (24u << 20)         /* bytes of undecoded packets */
#define OUT_RATE 48000

/* ---- queues ---- */

typedef struct PktNode { AVPacket *pkt; struct PktNode *next; } PktNode;
typedef struct { PktNode *head, *tail; int n; size_t bytes; } PktQ;

static void pq_push(PktQ *q, AVPacket *p) {
    PktNode *n = malloc(sizeof *n);
    if (!n) { av_packet_free(&p); return; }
    n->pkt = p;
    n->next = NULL;
    if (q->tail) q->tail->next = n; else q->head = n;
    q->tail = n;
    q->n++;
    q->bytes += (size_t)p->size;
}
static AVPacket *pq_pop(PktQ *q) {
    PktNode *n = q->head;
    if (!n) return NULL;
    q->head = n->next;
    if (!q->head) q->tail = NULL;
    q->n--;
    q->bytes -= (size_t)n->pkt->size;
    AVPacket *p = n->pkt;
    free(n);
    return p;
}
static void pq_clear(PktQ *q) { AVPacket *p; while ((p = pq_pop(q))) av_packet_free(&p); }

typedef struct { AVFrame *f; int64_t pts; int serial; } VFrame;
typedef struct { int16_t *pcm; int bytes, off; int64_t pts; int serial; } AChunk;
typedef struct { int64_t start, end; char text[1024]; int serial; } SubEv;

typedef struct {
    AVFormatContext *fmt;
    int vi, ai, si;                 /* chosen tracks (-1 none) */
    int cover;                      /* the video track is cover art (attached_pic) */
    AVCodecContext *vdec, *adec, *sdec;
    AVBufferRef *hw;
    int hw_active;
    LmHwFall hwf;                   /* consecutive decoding failures (lm_hwfall.h) */
    int hw_resume;                  /* it went to software: seek to the clock */
    SwrContext *swr;
    int out_ch;
    int64_t start_ms, dur_ms;

    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int quit;
    int seek_req, serial;
    int64_t seek_ms;
    int track_req_a, track_req_s;   /* requested track change (-2 = none) */
    int eof;
    PktQ vq_pkt, aq_pkt, sq_pkt;
    VFrame vq[VQ_MAX];
    int vq_n;
    AChunk aq[256];
    int aq_n, aq_head;
    int64_t aq_ms;
    SubEv subs[32];
    int sub_n;
    int64_t skip_before;            /* after a seek: discard what's before (ms) */

    /* statistics (written by the decoding thread) */
    long decoded, shown, dropped;
    double dec_ms;
    double sync_sum, sync_max;      /* |frame pts - clock| when presenting it (ms) */
    long sync_n;
} Player;

static Player P;

static int64_t frame_ms(AVStream *st, const AVFrame *f) {
    int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    if (ts == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
    return av_rescale_q(ts, st->time_base, (AVRational){ 1, 1000 }) - P.start_ms;
}

/* ---- decoders ---- */

static enum AVPixelFormat get_hw_format(AVCodecContext *c, const enum AVPixelFormat *fmts) {
    (void)c;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) if (*p == AV_PIX_FMT_VAAPI) return *p;
    P.hw_active = 0;                /* the decoder can't use VA-API: software */
    return fmts[0];
}

static AVCodecContext *open_dec(int idx, int video) {
    if (idx < 0) return NULL;
    AVStream *st = P.fmt->streams[idx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return NULL;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(c, st->codecpar);
    c->pkt_timebase = st->time_base;
    if (video) {
        c->thread_count = 0;
        c->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (P.hw && !P.cover) {
            for (int i = 0;; i++) {
                const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
                if (!cfg) break;
                if (cfg->device_type == AV_HWDEVICE_TYPE_VAAPI && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                    c->hw_device_ctx = av_buffer_ref(P.hw);
                    c->get_format = get_hw_format;
                    c->extra_hw_frames = VQ_MAX + 4;
                    P.hw_active = 1;
                    break;
                }
            }
        }
    }
    if (avcodec_open2(c, codec, NULL) < 0) { avcodec_free_context(&c); P.hw_active = 0; return NULL; }
    return c;
}

static int open_audio(int ai) {
    swr_free(&P.swr);
    avcodec_free_context(&P.adec);
    P.ai = ai;
    if (ai < 0) return 0;
    P.adec = open_dec(ai, 0);
    if (!P.adec) { P.ai = -1; return -1; }
    P.out_ch = 2;                    /* the ring is always stereo */
    AVChannelLayout out;
    av_channel_layout_default(&out, P.out_ch);
    if (swr_alloc_set_opts2(&P.swr, &out, AV_SAMPLE_FMT_S16, OUT_RATE, &P.adec->ch_layout, P.adec->sample_fmt,
                            P.adec->sample_rate, 0, NULL) < 0 || swr_init(P.swr) < 0) {
        swr_free(&P.swr);
        avcodec_free_context(&P.adec);
        P.ai = -1;
        return -1;
    }
    return 0;
}

/* ---- decoding thread ---- */

static void clear_outputs(void) {
    for (int i = 0; i < P.vq_n; i++) av_frame_free(&P.vq[i].f);
    P.vq_n = 0;
    for (int i = 0; i < P.aq_n; i++) free(P.aq[(P.aq_head + i) % 256].pcm);
    P.aq_n = P.aq_head = 0;
    P.aq_ms = 0;
    P.sub_n = 0;
}

static void ass_to_text(const char *ass, char *out, size_t n) {
    const char *p = ass;
    if (!strncmp(p, "Dialogue:", 9)) p += 9;
    for (int commas = 0; *p && commas < 8; p++) if (*p == ',') commas++;
    size_t o = 0;
    for (int tag = 0; *p && o + 2 < n; p++) {
        if (*p == '{') { tag = 1; continue; }
        if (*p == '}') { tag = 0; continue; }
        if (tag) continue;
        if (p[0] == '\\' && (p[1] == 'N' || p[1] == 'n')) { out[o++] = '\n'; p++; continue; }
        if (p[0] == '\\' && p[1] == 'h') { out[o++] = ' '; p++; continue; }
        out[o++] = *p;
    }
    out[o] = 0;
}

/* Video: first the frames the decoder already has are taken out, only while there's room
 * in the queue (the rest wait inside the decoder: when it's drained at the end several come out
 * together); then, one more packet. At the end of the file NULL is sent only once. */
static int vflushed;

static void pq_push_front(PktQ *q, AVPacket *p) {
    PktNode *n = malloc(sizeof *n);
    if (!n) { av_packet_free(&p); return; }
    n->pkt = p;
    n->next = q->head;
    q->head = n;
    if (!q->tail) q->tail = n;
    q->n++;
    q->bytes += (size_t)p->size;
}

/* The hardware decoder failed mid-file (a driver's error, a stream it took and can't follow): this file goes on in
 * software, from the keyframe before what is on screen. Under P.mu, on the decoding thread, which owns
 * P.vdec; frames still queued keep their own reference to the hardware's. */
#ifdef LM_HWFALL_TEST
static int hwf_tested;
#endif
static void hw_to_software(void) {
    AVCodecContext *old = P.vdec;
    AVBufferRef *hw = P.hw;
    int active = P.hw_active;
    P.hw = NULL;
    P.hw_active = 0;
    AVCodecContext *sw = open_dec(P.vi, 1);
    if (!sw) {                              /* no software decoder: as it was */
        P.hw = hw;
        P.hw_active = active;
        lm_hwfall_reset(&P.hwf);
        LM_LOG("hardware decoder failing and no software decoder to take over");
        return;
    }
    P.vdec = sw;
    pthread_mutex_unlock(&P.mu);
    avcodec_free_context(&old);
    av_buffer_unref(&hw);
    pthread_mutex_lock(&P.mu);
    lm_hwfall_reset(&P.hwf);
    P.hw_resume = 1;                        /* the main loop seeks to what is on screen (seek_to) */
    LM_LOG("hardware decoder failed: going on in software");
}

static int decode_video(void) {
    if (!P.vdec || P.cover) return 0;
    int serial = P.serial, progress = 0;
    AVFrame *f = av_frame_alloc();
    while (f && P.vq_n < VQ_MAX) {
        pthread_mutex_unlock(&P.mu);
        double t0 = lm_now_ms();
        int r = avcodec_receive_frame(P.vdec, f);
        double dt = lm_now_ms() - t0;
        pthread_mutex_lock(&P.mu);
#ifdef LM_HWFALL_TEST                       /* build/lord-media-hwfall: a decoder that fails after 30 frames */
        if (r >= 0 && P.decoded >= 30 && P.decoded < 1000 && !hwf_tested) {
            P.hw_active = 1;
            av_frame_unref(f);
            r = AVERROR_EXTERNAL;
            if (P.hwf.fails == LM_HWFALL_ERRS - 1) hwf_tested = 1;
        }
#endif
        if (r < 0) {
            if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) lm_hwfall_note(&P.hwf, 1);
            break;
        }
        lm_hwfall_note(&P.hwf, 0);
        progress = 1;
        P.dec_ms += dt;
        P.decoded++;
        int64_t pts = frame_ms(P.fmt->streams[P.vi], f);
        if (serial == P.serial && !(pts != AV_NOPTS_VALUE && pts < P.skip_before - 1)) {
            P.vq[P.vq_n++] = (VFrame){ f, pts == AV_NOPTS_VALUE ? 0 : pts, serial };
            f = av_frame_alloc();
        } else av_frame_unref(f);
        pthread_cond_broadcast(&P.cv);
    }
    av_frame_free(&f);
    if (P.hw_active && lm_hwfall_due(&P.hwf)) { hw_to_software(); return 1; }
    if (P.vq_n >= VQ_MAX || serial != P.serial) return progress;
    AVPacket *pkt = pq_pop(&P.vq_pkt);
    if (!pkt) {
        if (!P.eof || vflushed) return progress;
        vflushed = 1;                             /* end of file: drain the decoder */
        pthread_mutex_unlock(&P.mu);
        avcodec_send_packet(P.vdec, NULL);
        pthread_mutex_lock(&P.mu);
        return 1;
    }
    pthread_mutex_unlock(&P.mu);
    double t0 = lm_now_ms();
    int r = avcodec_send_packet(P.vdec, pkt);
    double dt = lm_now_ms() - t0;
    pthread_mutex_lock(&P.mu);
    P.dec_ms += dt;
    if (r == AVERROR(EAGAIN) && serial == P.serial) pq_push_front(&P.vq_pkt, pkt);   /* comes back later */
    else av_packet_free(&pkt);
    if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF) lm_hwfall_note(&P.hwf, 1);
    return 1;
}

static int decode_audio(void) {
    if (!P.adec || P.aq_ms >= AQ_MS || P.aq_n >= 250) return 0;
    AVPacket *pkt = pq_pop(&P.aq_pkt);
    if (!pkt) return 0;
    int serial = P.serial;
    int ai = P.ai;
    pthread_mutex_unlock(&P.mu);
    avcodec_send_packet(P.adec, pkt);
    av_packet_free(&pkt);
    AVFrame *f = av_frame_alloc();
    while (avcodec_receive_frame(P.adec, f) >= 0) {
        int64_t pts = frame_ms(P.fmt->streams[ai], f);
        int n = swr_get_out_samples(P.swr, f->nb_samples);
        int16_t *pcm = malloc((size_t)n * P.out_ch * 2);
        uint8_t *o[1] = { (uint8_t *)pcm };
        int got = pcm ? swr_convert(P.swr, o, n, (const uint8_t **)f->extended_data, f->nb_samples) : 0;
        av_frame_unref(f);
        if (got <= 0) { free(pcm); continue; }
        int bytes = got * P.out_ch * 2, off = 0;
        /* after a seek: trim what's before the target */
        if (pts != AV_NOPTS_VALUE && pts < P.skip_before) {
            int64_t skip = (P.skip_before - pts) * OUT_RATE / 1000;
            if (skip >= got) { free(pcm); continue; }
            off = (int)skip * P.out_ch * 2;
            pts = P.skip_before;
        }
        pthread_mutex_lock(&P.mu);
        if (serial == P.serial && P.aq_n < 256) {
            P.aq[(P.aq_head + P.aq_n) % 256] = (AChunk){ pcm, bytes, off, pts, serial };
            P.aq_n++;
            P.aq_ms += (int64_t)(bytes - off) * 1000 / (OUT_RATE * P.out_ch * 2);
        } else free(pcm);
        pthread_cond_broadcast(&P.cv);
        pthread_mutex_unlock(&P.mu);
    }
    av_frame_free(&f);
    pthread_mutex_lock(&P.mu);
    return 1;
}

static int decode_sub(void) {
    if (!P.sdec) return 0;
    AVPacket *pkt = pq_pop(&P.sq_pkt);
    if (!pkt) return 0;
    int serial = P.serial;
    AVStream *st = P.fmt->streams[P.si];
    pthread_mutex_unlock(&P.mu);
    AVSubtitle sub;
    int got = 0;
    if (avcodec_decode_subtitle2(P.sdec, &sub, &got, pkt) >= 0 && got) {
        int64_t base = pkt->pts != AV_NOPTS_VALUE ? av_rescale_q(pkt->pts, st->time_base, (AVRational){ 1, 1000 }) - P.start_ms : 0;
        int64_t dur = pkt->duration > 0 ? av_rescale_q(pkt->duration, st->time_base, (AVRational){ 1, 1000 }) : 0;
        SubEv ev = { base + sub.start_display_time, sub.end_display_time ? base + sub.end_display_time : base + (dur ? dur : 4000), "", serial };
        for (unsigned i = 0; i < sub.num_rects; i++) {
            char line[1024];
            if (sub.rects[i]->type == SUBTITLE_ASS && sub.rects[i]->ass) ass_to_text(sub.rects[i]->ass, line, sizeof line);
            else if (sub.rects[i]->text) snprintf(line, sizeof line, "%s", sub.rects[i]->text);
            else continue;
            size_t l = strlen(ev.text);
            snprintf(ev.text + l, sizeof ev.text - l, "%s%s", l ? "\n" : "", line);
        }
        avsubtitle_free(&sub);
        pthread_mutex_lock(&P.mu);
        if (ev.text[0] && serial == P.serial && P.sub_n < 32) P.subs[P.sub_n++] = ev;
        pthread_mutex_unlock(&P.mu);
    }
    av_packet_free(&pkt);
    pthread_mutex_lock(&P.mu);
    return 1;
}

static void do_seek(void) {
    int64_t ms = P.seek_ms;
    P.seek_req = 0;
    pthread_mutex_unlock(&P.mu);
    int64_t ts = av_rescale(ms + P.start_ms, AV_TIME_BASE, 1000);
    if (av_seek_frame(P.fmt, -1, ts, AVSEEK_FLAG_BACKWARD) < 0) av_seek_frame(P.fmt, -1, ts, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
    if (P.vdec) avcodec_flush_buffers(P.vdec);
    if (P.adec) avcodec_flush_buffers(P.adec);
    if (P.sdec) avcodec_flush_buffers(P.sdec);
    pthread_mutex_lock(&P.mu);
    pq_clear(&P.vq_pkt);
    pq_clear(&P.aq_pkt);
    pq_clear(&P.sq_pkt);
    clear_outputs();
    P.skip_before = ms;
    P.eof = 0;
    vflushed = 0;
}

static void *decode_thread(void *arg) {
    (void)arg;
    AVPacket *pkt = av_packet_alloc();
    pthread_mutex_lock(&P.mu);
    while (!P.quit) {
        if (P.track_req_a != -2) {
            int a = P.track_req_a;
            P.track_req_a = -2;
            pthread_mutex_unlock(&P.mu);
            open_audio(a);
            pthread_mutex_lock(&P.mu);
        }
        if (P.track_req_s != -2) {
            int s = P.track_req_s;
            P.track_req_s = -2;
            avcodec_free_context(&P.sdec);
            pq_clear(&P.sq_pkt);
            P.si = s;
            if (s >= 0) P.sdec = open_dec(s, 0);
            if (!P.sdec) P.si = -1;
        }
        if (P.seek_req) do_seek();
        int progress = 0;
        progress |= decode_audio();
        progress |= decode_video();
        progress |= decode_sub();
        int need_v = P.vdec && P.vq_n < VQ_MAX && !P.vq_pkt.n;
        int need_a = P.adec && P.aq_ms < AQ_MS && !P.aq_pkt.n;
        size_t qbytes = P.vq_pkt.bytes + P.aq_pkt.bytes + P.sq_pkt.bytes;
        if (!P.eof && (need_v || need_a || qbytes < (1u << 20)) && qbytes < PKTQ_MAX) {
            pthread_mutex_unlock(&P.mu);
            int r = av_read_frame(P.fmt, pkt);
            pthread_mutex_lock(&P.mu);
            if (P.seek_req) { av_packet_unref(pkt); continue; }
            if (r < 0) {
                P.eof = 1;
                progress = 1;
            } else {
                AVPacket *c = av_packet_alloc();
                av_packet_move_ref(c, pkt);
                if (c->stream_index == P.vi && P.vdec && !P.cover) pq_push(&P.vq_pkt, c);
                else if (c->stream_index == P.ai && P.adec) pq_push(&P.aq_pkt, c);
                else if (c->stream_index == P.si && P.sdec) pq_push(&P.sq_pkt, c);
                else av_packet_free(&c);
                progress = 1;
            }
        }
        if (!progress) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 15 * 1000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&P.cv, &P.mu, &ts);
        }
    }
    pthread_mutex_unlock(&P.mu);
    av_packet_free(&pkt);
    return NULL;
}

/* ---- the clock ---- */

typedef struct {
    int audio;                      /* there's audio output */
    int playing;
    int64_t anchor_pts;             /* pts of the first byte sent after the last seek */
    int64_t anchor_byte;            /* PipeWire's written at that moment (-1: not yet) */
    int64_t base_pts;               /* system clock: pts when starting/pausing */
    double base_ms;
    int64_t audio_end;              /* pts of the end of what was sent */
} Clock;

static Clock C;
static ARingHdr *AR;                /* lord-server's audio ring (or NULL: no audio) */
#define FRAME_BYTES 4                /* s16 stereo */

static int64_t clock_ms(void) {
    if (!C.playing) return C.base_pts;
    if (!C.audio) return C.base_pts + (int64_t)(lm_now_ms() - C.base_ms);
    int playing;
    int64_t heard, mono_us;
    if (C.anchor_byte < 0 || !ar_clock_read(AR, &playing, &heard, &mono_us)) return C.base_pts;
    double bps = (double)OUT_RATE * FRAME_BYTES / 1000.0;
    double pos = C.anchor_pts + (heard - C.anchor_byte) / bps;
    if (playing) pos += lm_now_ms() - mono_us / 1000.0;
    if (pos < C.anchor_pts) pos = (double)C.anchor_pts;
    if (pos > C.audio_end) pos = (double)C.audio_end;
    return (int64_t)pos;
}

static void feed_audio(void) {
    if (!C.audio || !C.playing) return;
    pthread_mutex_lock(&P.mu);
    int wrote = 0;
    while (P.aq_n && ar_space(AR) >= FRAME_BYTES) {
        AChunk *c = &P.aq[P.aq_head];
        if (c->serial != P.serial) { free(c->pcm); P.aq_head = (P.aq_head + 1) % 256; P.aq_n--; continue; }
        int n = c->bytes - c->off;
        uint64_t space = ar_space(AR) & ~(uint64_t)(FRAME_BYTES - 1);
        if ((uint64_t)n > space) n = (int)space;
        if (n <= 0) break;
        uint64_t w = atomic_load(&AR->wpos);
        if (C.anchor_byte < 0) { C.anchor_byte = (int64_t)w; C.anchor_pts = c->pts + (int64_t)c->off * 1000 / (OUT_RATE * FRAME_BYTES); }
        n = (int)ar_write(AR, (uint8_t *)c->pcm + c->off, (size_t)n);
        wrote += n;
        c->off += n;
        int64_t ms = (int64_t)n * 1000 / (OUT_RATE * FRAME_BYTES);
        P.aq_ms -= ms;
        C.audio_end = C.anchor_pts + ((int64_t)w + n - C.anchor_byte) * 1000 / (OUT_RATE * FRAME_BYTES);
        if (c->off >= c->bytes) { free(c->pcm); P.aq_head = (P.aq_head + 1) % 256; P.aq_n--; }
    }
    pthread_cond_broadcast(&P.cv);
    pthread_mutex_unlock(&P.mu);
    if (wrote) mp_sendv(LM_SOCK, MM_AUDIO, 0, 0, 0, 0, 0);       /* lord-server passes it to PipeWire */
}

static void set_playing(int on) {
    if (on == C.playing) return;
    int64_t now = clock_ms();
    C.playing = on;
    C.base_pts = now;
    C.base_ms = lm_now_ms();
    mp_sendv(LM_SOCK, MM_STATE, 0, on ? MS_PLAYING : MS_PAUSED, 0, 0, 0);
}

static void seek_to(int64_t ms) {
    if (ms < 0) ms = 0;
    if (P.dur_ms > 0 && ms > P.dur_ms) ms = P.dur_ms;
    pthread_mutex_lock(&P.mu);
    P.serial++;
    P.seek_req = 1;
    P.seek_ms = ms;
    clear_outputs();
    pthread_cond_broadcast(&P.cv);
    pthread_mutex_unlock(&P.mu);
    if (C.audio) {
        /* lord-server discards what was left in the ring and in PipeWire (FLUSH) */
        atomic_store(&AR->discard_to, atomic_load(&AR->wpos));
        atomic_fetch_add(&AR->serial, 1);
        C.anchor_byte = -1;
        C.audio_end = ms;
        mp_sendv(LM_SOCK, MM_AUDIO, 0, 0, 0, 0, 0);
    }
    C.base_pts = ms;
    C.base_ms = lm_now_ms();
    mp_sendv(LM_SOCK, MM_POS, 0, ms, P.dur_ms, C.playing, 0);
}

static void send_info(int render) {
    LmBuf b = { 0 };
    lm_probe_json(P.fmt, &b);
    if (b.len && b.d[b.len - 1] == '}') b.len--;
    lm_buf_printf(&b, ",\"video\":%d,\"audio\":%d,\"cover\":%s,\"hw\":%s,\"hwAvailable\":%s,\"audioOut\":%s}", P.cover ? -1 : P.vi, P.ai,
                  P.cover ? "true" : "false", P.hw_active ? "true" : "false", P.hw ? "true" : "false", C.audio ? "true" : "false");
    (void)render;
    mp_send(LM_SOCK, MM_INFO, 0, NULL, b.d, b.len, NULL, 0);
    lm_buf_free(&b);
}

void lm_player(const char *params, int fd, int pulse, int render) {
    char err[160];
    lm_out_init((int)mp_param_int(params, "native", 0));
    int loop = (int)mp_param_int(params, "loop", 0);
    int autoplay = (int)mp_param_int(params, "autoplay", 1);
    int want_audio = (int)mp_param_int(params, "audio", 1);

    /* VA-API before seccomp (it loads the driver from /usr/lib) */
    char accel[24];
    P.hw = lm_hw_device(lm_hw_accel(params, accel, sizeof accel), render);
    /* lord-server creates the audio ring (sealed); it's validated before using it */
    if (pulse >= 0 && want_audio) {
        struct stat st;
        void *m = MAP_FAILED;
        int seals = fcntl(pulse, F_GET_SEALS);
        if (!fstat(pulse, &st) && st.st_size >= AR_HDR && (seals & F_SEAL_SHRINK)) m = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, pulse, 0);
        if (m != MAP_FAILED) {
            ARingHdr *h = m;
            if (h->magic == AR_MAGIC && h->rate == OUT_RATE && h->channels == 2 && h->size && !(h->size & (h->size - 1)) && ar_bytes(h->size) <= (size_t)st.st_size) AR = h;
            else munmap(m, (size_t)st.st_size);
        }
        if (!AR) LM_LOG("invalid audio ring: no sound");
    }
    close(pulse);

    P.fmt = lm_open_input(fd, err, sizeof err);
    if (!P.fmt) { lm_error("%s", err); return; }
    P.start_ms = P.fmt->start_time != AV_NOPTS_VALUE ? P.fmt->start_time / 1000 : 0;
    P.dur_ms = P.fmt->duration > 0 ? P.fmt->duration / 1000 : 0;
    P.vi = av_find_best_stream(P.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    P.cover = P.vi >= 0 && (P.fmt->streams[P.vi]->disposition & AV_DISPOSITION_ATTACHED_PIC);
    int ai = want_audio ? av_find_best_stream(P.fmt, AVMEDIA_TYPE_AUDIO, -1, P.vi >= 0 && !P.cover ? P.vi : -1, NULL, 0) : -1;
    P.si = -1;
    P.track_req_a = P.track_req_s = -2;
    for (unsigned i = 0; i < P.fmt->nb_streams; i++) P.fmt->streams[i]->discard = AVDISCARD_ALL;
    if (P.vi >= 0) {
        P.fmt->streams[P.vi]->discard = AVDISCARD_DEFAULT;
        P.vdec = open_dec(P.vi, 1);
        if (!P.vdec && P.hw) { av_buffer_unref(&P.hw); P.vdec = open_dec(P.vi, 1); }   /* without VA-API */
        if (!P.vdec) P.vi = -1;
    }
    if (ai >= 0) { P.fmt->streams[ai]->discard = AVDISCARD_DEFAULT; if (open_audio(ai)) LM_LOG("no audio decoder"); }
    else P.ai = -1;
    /* all audio and subtitle tracks are read (to be able to switch without reopening) */
    for (unsigned i = 0; i < P.fmt->nb_streams; i++) {
        enum AVMediaType t = P.fmt->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_AUDIO || t == AVMEDIA_TYPE_SUBTITLE) P.fmt->streams[i]->discard = AVDISCARD_DEFAULT;
    }
    if (P.vi < 0 && P.ai < 0) { lm_error("media.nothingToPlay"); lm_close_input(&P.fmt); return; }
    C.audio = P.ai >= 0 && AR != NULL;
    C.anchor_byte = -1;
    send_info(render);

    pthread_mutex_init(&P.mu, NULL);
    pthread_cond_init(&P.cv, NULL);
    /* cover art: a single frame, presented right away */
    AVFrame *cover = P.cover ? lm_decode_still(P.fmt, 0, NULL, 1) : NULL;
    if (P.cover) { avcodec_free_context(&P.vdec); av_seek_frame(P.fmt, -1, 0, AVSEEK_FLAG_BACKWARD); }
    if (pthread_create(&P.th, NULL, decode_thread, NULL)) { lm_error("media.threadFailed"); return; }

    static char payload[MP_PAYLOAD_MAX + 1];
    double last_pos = 0, last_stats = 0, last_shown_ms = lm_now_ms();
    int64_t seek_wait = 0;
    int ended = 0;
    if (autoplay) set_playing(1); else mp_sendv(LM_SOCK, MM_STATE, 0, MS_PAUSED, 0, 0, 0);
    for (;;) {
        /* when to look again: the next frame or 20 ms */
        int timeout = 20;
        pthread_mutex_lock(&P.mu);
        if (P.vq_n && C.playing) {
            int64_t d = P.vq[0].pts - clock_ms();
            if (d < timeout) timeout = d < 1 ? 1 : (int)d;
        }
        pthread_mutex_unlock(&P.mu);
        if (C.audio && P.aq_n && timeout > 10) timeout = 10;          /* keep filling the ring */
        struct pollfd pf[1] = { { LM_SOCK, POLLIN, 0 } };
        if (poll(pf, 1, timeout) < 0 && errno != EINTR) break;
        if (pf[0].revents & (POLLHUP | POLLERR)) break;
        if (pf[0].revents & POLLIN) {
            MediaMsg m;
            int fds[MP_MAX_FDS];
            int r;
            int quit = 0;
            while ((r = mp_recv(LM_SOCK, &m, payload, sizeof payload, fds, 0)) != -1) {
                for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]);
                if (r == 0) { quit = 1; break; }
                switch (m.type) {
                case MM_ACK: lm_out_ack(m.id); break;
                case MM_SIZE:
                    if (lm_out_box((int)m.v[0], (int)m.v[1]) && !lm_out_redraw() && cover) lm_out_present(cover, 0);
                    break;
                case MM_PLAY:
                    if (ended) { ended = 0; seek_to(0); }
                    set_playing(1);
                    break;
                case MM_PAUSE: set_playing(0); break;
                case MM_SEEK: ended = 0; seek_to(m.v[0]); seek_wait = 1; break;
                case MM_LOOP: loop = m.v[0] != 0; break;
                case MM_TRACK:
                    pthread_mutex_lock(&P.mu);
                    if (m.v[0] == 's' && (m.v[1] < 0 || ((unsigned)m.v[1] < P.fmt->nb_streams && P.fmt->streams[m.v[1]]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)))
                        P.track_req_s = (int)m.v[1];
                    if (m.v[0] == 'a' && (unsigned)m.v[1] < P.fmt->nb_streams && P.fmt->streams[m.v[1]]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && C.audio)
                        P.track_req_a = (int)m.v[1];
                    pthread_cond_broadcast(&P.cv);
                    pthread_mutex_unlock(&P.mu);
                    if (m.v[0] == 'a') seek_to(clock_ms());
                    break;
                case MM_STOP: quit = 1; break;
                default: break;
                }
                if (quit) break;
            }
            if (quit) break;
        }
        if (C.audio) {
            feed_audio();
            /* lord-server stopped consuming (PipeWire went away): continue with the system clock */
            /* normally the ring is almost full (decoding runs ~1 s ahead): what
             * indicates the cut is that lord-server's read position doesn't move */
            static double full_since;
            static uint64_t last_r;
            uint64_t r = atomic_load(&AR->rpos);
            if (C.playing && ar_space(AR) < 4096 && r == last_r) {
                if (!full_since) full_since = lm_now_ms();
                else if (lm_now_ms() - full_since > 2000) {
                    int64_t now = clock_ms();
                    LM_LOG("the audio isn't advancing: continuing without sound");
                    C.audio = 0;
                    C.base_pts = now;
                    C.base_ms = lm_now_ms();
                }
            } else full_since = 0;
            last_r = r;
        }
        if (cover && lm_out_has_box() && !lm_out_has_last()) lm_out_present(cover, 0);
        /* video: the frame that matches the clock */
        int64_t now = clock_ms();
        for (;;) {
            pthread_mutex_lock(&P.mu);
            if (!P.vq_n) { pthread_mutex_unlock(&P.mu); break; }
            VFrame vf = P.vq[0];
            if (vf.serial != P.serial) {
                av_frame_free(&P.vq[0].f);
                memmove(P.vq, P.vq + 1, sizeof P.vq[0] * (size_t)--P.vq_n);
                pthread_mutex_unlock(&P.mu);
                continue;
            }
            /* after a seek, the first frame is shown even while paused */
            int show_paused = seek_wait && !C.playing;
            if (!show_paused && (!C.playing || vf.pts > now + 4)) { pthread_mutex_unlock(&P.mu); break; }
            int late = P.vq_n > 1 && P.vq[1].pts <= now && lm_now_ms() - last_shown_ms < 250;
            P.vq_n--;
            memmove(P.vq, P.vq + 1, sizeof P.vq[0] * (size_t)P.vq_n);
            pthread_cond_broadcast(&P.cv);
            pthread_mutex_unlock(&P.mu);
            if (late && !seek_wait) { P.dropped++; av_frame_free(&vf.f); continue; }
            int64_t clk = clock_ms();
            int r = lm_out_present(vf.f, vf.pts);
            if (r < 0 && vf.f->format == AV_PIX_FMT_VAAPI) {   /* the frame could not be read back */
                pthread_mutex_lock(&P.mu);
                lm_hwfall_note(&P.hwf, 1);
                pthread_mutex_unlock(&P.mu);
            }
            if (r == 1) P.dropped++;
            else if (r == 0) {
                P.shown++;
                last_shown_ms = lm_now_ms();
                if (C.playing && !seek_wait) { double e = fabs((double)(vf.pts - clk)); P.sync_sum += e; P.sync_n++; if (e > P.sync_max) P.sync_max = e; }
            }
            seek_wait = 0;
            av_frame_free(&vf.f);
            if (!C.playing) break;
        }
        /* subtitles: sent as soon as they're decoded (the JS clock shows them) */
        pthread_mutex_lock(&P.mu);
        int resume = P.hw_resume;
        P.hw_resume = 0;
        pthread_mutex_unlock(&P.mu);
        if (resume) { seek_to(clock_ms()); seek_wait = 1; }
        pthread_mutex_lock(&P.mu);
        for (int i = 0; i < P.sub_n; i++) {
            if (P.subs[i].serial != P.serial) continue;
            int64_t v[6] = { P.subs[i].start, P.subs[i].end, 0, 0, 0, 0 };
            mp_send(LM_SOCK, MM_SUB, 0, v, P.subs[i].text, strlen(P.subs[i].text), NULL, 0);
        }
        P.sub_n = 0;
        int drained = P.eof && !P.vq_n && !P.aq_n && !P.vq_pkt.n && !P.aq_pkt.n;
        pthread_mutex_unlock(&P.mu);
        /* end: nothing left and the audio has already played */
        if (drained && C.playing && !ended && (!C.audio || clock_ms() >= C.audio_end - 30 || !ar_avail(AR))) {
            if (loop) seek_to(0);
            else {
                ended = 1;
                C.base_pts = P.dur_ms ? P.dur_ms : clock_ms();
                C.playing = 0;
                mp_sendv(LM_SOCK, MM_POS, 0, C.base_pts, P.dur_ms, 0, 0);
                mp_sendv(LM_SOCK, MM_STATE, 0, MS_ENDED, 0, 0, 0);
            }
        }
        double t = lm_now_ms();
        if (t - last_pos > 250) {
            mp_sendv(LM_SOCK, MM_POS, 0, clock_ms(), P.dur_ms, C.playing, 0);
            last_pos = t;
        }
        if (t - last_stats > 1000) {
            pthread_mutex_lock(&P.mu);
            int64_t v[6] = { P.decoded, P.shown, P.dropped, P.hw_active, P.sync_n ? (int64_t)(P.sync_sum * 100 / P.sync_n) : 0, (int64_t)P.sync_max };
            pthread_mutex_unlock(&P.mu);
            mp_send(LM_SOCK, MM_STATS, 0, v, NULL, 0, NULL, 0);
            last_stats = t;
        }
    }
    pthread_mutex_lock(&P.mu);
    P.quit = 1;
    pthread_cond_broadcast(&P.cv);
    pthread_mutex_unlock(&P.mu);
    pthread_join(P.th, NULL);
}
