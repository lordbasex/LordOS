/* lm_live.c — a live stream for a packaged app (MK_LIVE, docs/PACKAGED-APPS.md, "Live media").
 *
 * The companion reads the network and has no decoder, and the app has no way to run one. Encoded pictures and Opus
 * packets arrive here over the link (../live.h): pictures go to the frame slots of lm_out.c, which the app shows in a
 * <MediaView>, and sound to the ring lord-server plays. With the microphone (MM_RECORD, a ring lord-server fills after
 * the permission), it's encoded here and goes back the same way. What comes in is a stranger's data: this process has
 * no network, no files and no exec, and it decodes one frame at a time, always the newest it can. */
#include "lm.h"
#include "lm_link.h"
#include "../aring.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

#define WANT_KEY_MS 1000                /* at most one request a second */
#define MAX_BACKLOG (48000 * 4 / 5)     /* 200 ms of sound waiting: drop instead of falling behind */

typedef struct {
    LmLink link;
    AVCodecContext *video, *opus, *mic;
    AVPacket *pkt, *mic_pkt;
    AVFrame *frame, *mic_frame;
    ARingHdr *ring;
    size_t ring_len;
    void *mic_ring;
    size_t mic_ring_len;
    int16_t mic_buf[960];
    int mic_have, playing;
    int64_t mic_pts, decoded, shown, dropped;
    double dec_ms, last_key, last_stats;
    char codec[8];
} Live;

static Live L;

static void want_key(void) {
    double t = lm_now_ms();
    if (t - L.last_key < WANT_KEY_MS) return;
    L.last_key = t;
    lm_link_send(&L.link, LL_WANTKEY, 0, 0, NULL, 0);
}

static int open_video(const char *name, size_t n) {
    char want[8];
    if (n >= sizeof want) return -1;
    memcpy(want, name, n);
    want[n] = 0;
    if (L.video && !strcmp(want, L.codec)) return 0;
    enum AVCodecID id = !strcmp(want, "h264") ? AV_CODEC_ID_H264 : !strcmp(want, "vp8") ? AV_CODEC_ID_VP8 : AV_CODEC_ID_NONE;
    const AVCodec *c = id == AV_CODEC_ID_NONE ? NULL : avcodec_find_decoder(id);
    if (!c) return -1;
    avcodec_free_context(&L.video);
    L.video = avcodec_alloc_context3(c);
    if (!L.video) return -1;
    L.video->flags |= AV_CODEC_FLAG_LOW_DELAY;      /* a picture out for each picture in */
    L.video->thread_type = FF_THREAD_SLICE;
    L.video->thread_count = 2;
    L.video->max_pixels = (int64_t)MP_MAX_DIM * MP_MAX_DIM;
    if (avcodec_open2(L.video, c, NULL) < 0) { avcodec_free_context(&L.video); return -1; }
    snprintf(L.codec, sizeof L.codec, "%s", want);
    LM_LOG("live: %s", want);
    return 0;
}

static void info_of(const AVFrame *f) {
    static int w, h;
    if (f->width == w && f->height == h) return;
    w = f->width;
    h = f->height;
    char j[96];
    int n = snprintf(j, sizeof j, "{\"width\":%d,\"height\":%d,\"codec\":\"%s\"}", w, h, L.codec);
    mp_send(LM_SOCK, MM_INFO, 0, NULL, j, (size_t)n, NULL, 0);
}

static void on_video(const uint8_t *d, size_t n, int64_t pts) {
    if (!L.video || !n || n > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE) return;
    double t0 = lm_now_ms();
    if (av_new_packet(L.pkt, (int)n) < 0) return;
    memcpy(L.pkt->data, d, n);
    L.pkt->pts = pts;
    int r = avcodec_send_packet(L.video, L.pkt);
    av_packet_unref(L.pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) { want_key(); return; }
    while (avcodec_receive_frame(L.video, L.frame) >= 0) {
        L.decoded++;
        info_of(L.frame);
        int o = lm_out_present(L.frame, pts);
        if (o == 0) L.shown++; else L.dropped++;
        av_frame_unref(L.frame);
    }
    L.dec_ms += lm_now_ms() - t0;
}

static int open_opus(void) {
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    L.opus = c ? avcodec_alloc_context3(c) : NULL;
    if (!L.opus) return -1;
    L.opus->sample_rate = 48000;
    av_channel_layout_default(&L.opus->ch_layout, 2);
    if (avcodec_open2(L.opus, c, NULL) < 0) { avcodec_free_context(&L.opus); return -1; }
    return 0;
}

static inline int16_t to16(float v) {
    if (v > 1) v = 1;
    if (v < -1) v = -1;
    return (int16_t)(v * 32767);
}

/* One decoded frame as s16 stereo, in the ring. */
static void play(const AVFrame *f) {
    int ch = f->ch_layout.nb_channels;
    if (ch < 1 || f->nb_samples < 1 || f->nb_samples > 5760 || f->sample_rate != 48000) return;
    static int16_t pcm[5760 * 2];
    for (int i = 0; i < f->nb_samples; i++) for (int k = 0; k < 2; k++) {
        int c = k < ch ? k : 0;
        switch (f->format) {
        case AV_SAMPLE_FMT_FLTP: pcm[i * 2 + k] = to16(((const float *)f->data[c])[i]); break;
        case AV_SAMPLE_FMT_FLT:  pcm[i * 2 + k] = to16(((const float *)f->data[0])[i * ch + c]); break;
        case AV_SAMPLE_FMT_S16P: pcm[i * 2 + k] = ((const int16_t *)f->data[c])[i]; break;
        case AV_SAMPLE_FMT_S16:  pcm[i * 2 + k] = ((const int16_t *)f->data[0])[i * ch + c]; break;
        default: return;
        }
    }
    if (ar_avail(L.ring) > MAX_BACKLOG) return;
    ar_write(L.ring, pcm, (size_t)f->nb_samples * 4);
    if (!L.playing) { L.playing = 1; mp_sendv(LM_SOCK, MM_STATE, 0, MS_PLAYING, 0, 0, 0); }
    mp_sendv(LM_SOCK, MM_AUDIO, 0, 0, 0, 0, 0);
}

static void on_opus(const uint8_t *d, size_t n) {
    if (!L.ring || !n || n > 4000) return;              /* RFC 6716's longest frame is far below this */
    if (!L.opus && open_opus()) return;
    if (av_new_packet(L.pkt, (int)n) < 0) return;
    memcpy(L.pkt->data, d, n);
    int r = avcodec_send_packet(L.opus, L.pkt);
    av_packet_unref(L.pkt);
    if (r < 0) return;
    while (avcodec_receive_frame(L.opus, L.frame) >= 0) { play(L.frame); av_frame_unref(L.frame); }
}

static void on_msg(const LiveHdr *h, const uint8_t *d, void *arg) {
    (void)arg;
    switch (h->type) {
    case LL_CODEC:
        if (open_video((const char *)d, h->len)) { LM_LOG("live: no decoder for that codec"); lm_error("media.noDecoder"); }
        break;
    case LL_VIDEO: on_video(d, h->len, h->pts); break;
    case LL_OPUS:  on_opus(d, h->len); break;
    default: break;                                     /* unknown: ignored, a newer engine may send more */
    }
}

static int mic_open(void) {
    const AVCodec *c = avcodec_find_encoder_by_name("libopus");
    if (!c || !(L.mic = avcodec_alloc_context3(c))) return -1;
    L.mic->sample_fmt = AV_SAMPLE_FMT_S16;
    L.mic->sample_rate = 48000;
    av_channel_layout_default(&L.mic->ch_layout, 1);
    L.mic->bit_rate = 32000;
    L.mic->time_base = (AVRational){ 1, 48000 };
    AVDictionary *o = NULL;
    av_dict_set(&o, "application", "voip", 0);
    av_dict_set(&o, "frame_duration", "20", 0);
    int r = avcodec_open2(L.mic, c, &o);
    av_dict_free(&o);
    if (r < 0) { avcodec_free_context(&L.mic); return -1; }
    return 0;
}

static void on_mic(const int16_t *s, int k) {
    while (k > 0 && L.mic) {
        int take = 960 - L.mic_have;
        if (take > k) take = k;
        memcpy(L.mic_buf + L.mic_have, s, (size_t)take * 2);
        L.mic_have += take;
        s += take;
        k -= take;
        if (L.mic_have < 960) return;
        L.mic_have = 0;
        AVFrame *f = L.mic_frame;
        f->nb_samples = 960;
        f->format = AV_SAMPLE_FMT_S16;
        f->sample_rate = 48000;
        av_channel_layout_default(&f->ch_layout, 1);
        if (av_frame_get_buffer(f, 0) < 0) return;
        memcpy(f->data[0], L.mic_buf, 960 * 2);
        f->pts = L.mic_pts;
        L.mic_pts += 960;
        if (avcodec_send_frame(L.mic, f) >= 0)
            while (avcodec_receive_packet(L.mic, L.mic_pkt) >= 0) {
                lm_link_send(&L.link, LL_MIC, 0, L.mic_pkt->pts / 48, L.mic_pkt->data, (size_t)L.mic_pkt->size);
                av_packet_unref(L.mic_pkt);
            }
        av_frame_unref(f);
    }
}

static void mic_close(void) {
    if (L.mic_ring) munmap(L.mic_ring, L.mic_ring_len);
    L.mic_ring = NULL;
    L.mic_have = 0;
    avcodec_free_context(&L.mic);
}

static ARingHdr *play_ring(int fd, size_t *len) {
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size < AR_HDR || !(fcntl(fd, F_GET_SEALS) & F_SEAL_SHRINK)) return NULL;
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return NULL;
    ARingHdr *h = m;
    if (h->magic != AR_MAGIC || h->rate != AR_RATE || h->channels != 2 || !h->size || (h->size & (h->size - 1)) ||
        ar_bytes(h->size) > (size_t)st.st_size) { munmap(m, (size_t)st.st_size); return NULL; }
    *len = (size_t)st.st_size;
    return h;
}

void lm_live(const char *params, int link, int ring) {
    (void)params;
    lm_link_init(&L.link, link);
    lm_out_init(0);
    L.ring = play_ring(ring, &L.ring_len);
    if (ring >= 0) close(ring);
    L.pkt = av_packet_alloc();
    L.mic_pkt = av_packet_alloc();
    L.frame = av_frame_alloc();
    L.mic_frame = av_frame_alloc();
    L.last_stats = lm_now_ms();
    static char payload[MP_PAYLOAD_MAX + 1];
    int stop = link < 0;
    while (!stop) {
        struct pollfd pf[2] = { { LM_SOCK, POLLIN, 0 }, { L.link.fd, lm_link_events(&L.link), 0 } };
        if (poll(pf, 2, L.mic_ring ? 10 : 500) < 0 && errno != EINTR) break;
        if (pf[0].revents & (POLLHUP | POLLERR)) break;
        if (pf[0].revents & POLLIN) {
            MediaMsg m;
            int fds[MP_MAX_FDS], r;
            while ((r = mp_recv(LM_SOCK, &m, payload, sizeof payload, fds, 0)) != -1) {
                if (r == 0 || m.type == MM_STOP) { stop = 1; for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]); break; }
                if (m.type == MM_ACK) lm_out_ack(m.id);
                else if (m.type == MM_SIZE) { if (lm_out_box((int)m.v[0], (int)m.v[1])) lm_out_redraw(); }
                else if (m.type == MM_RECORD) {
                    mic_close();
                    if (fds[0] >= 0) {
                        L.mic_ring = lm_mic_ring(fds[0], &L.mic_ring_len);
                        if (L.mic_ring && mic_open()) mic_close();
                    }
                }
                for (int i = 0; i < MP_MAX_FDS; i++) if (fds[i] >= 0) close(fds[i]);
            }
        }
        if (pf[1].revents & POLLOUT) lm_link_flush(&L.link);
        if (pf[1].revents & (POLLIN | POLLHUP)) { if (lm_link_read(&L.link, on_msg, NULL)) break; }
        if (L.mic_ring) lm_mic_drain(L.mic_ring, on_mic);
        double t = lm_now_ms();
        if (t - L.last_stats >= 2000) {
            L.last_stats = t;
            mp_sendv(LM_SOCK, MM_STATS, 0, L.decoded, L.shown, L.dropped, L.decoded ? (int64_t)(L.dec_ms * 100 / (double)L.decoded) : 0);
        }
    }
    LM_LOG("live: %lld pictures, %lld shown, %.2f ms each", (long long)L.decoded, (long long)L.shown, L.decoded ? L.dec_ms / (double)L.decoded : 0.0);
    mic_close();
    avcodec_free_context(&L.video);
    avcodec_free_context(&L.opus);
    av_packet_free(&L.pkt);
    av_packet_free(&L.mic_pkt);
    av_frame_free(&L.frame);
    av_frame_free(&L.mic_frame);
    lm_link_close(&L.link);
    mp_sendv(LM_SOCK, MM_STATE, 0, MS_DONE, 0, 0, 0);
}
