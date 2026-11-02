/* mpulse.c — PipeWire streams for media, in axia-server (PulseAudio protocol, the
 * same tagstruct as server/src/sound/pulse.c). One connection per stream, so that each one
 * shows up with its app in Settings → Sound: playback (Player, Photos) with its latency for the
 * video clock, or recording (microphone, after the permission). All asynchronous: axia-server never
 * waits for PipeWire (see aring.h and docs/MEDIA.md). */
#include "mpulse.h"
#include "../sound/pulse.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static double lm_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

#define PROTO 32

enum { W_NONE, W_AUTH, W_NAME, W_PLAY, W_REC, W_LAT, W_ACK };

static int out_flush(LmPulse *p) {
    while (p->out_len) {
        ssize_t w = send(p->fd, p->out, p->out_len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) return 0;
        if (w <= 0) return -1;
        memmove(p->out, p->out + w, p->out_len - (size_t)w);
        p->out_len -= (size_t)w;
    }
    return 0;
}

static int out_queue(LmPulse *p, const uint8_t *d, size_t n) {
    if (p->fd < 0) return -1;
    if (p->out_len + n > p->out_cap) {
        size_t cap = p->out_cap ? p->out_cap * 2 : 65536;
        while (cap < p->out_len + n) cap *= 2;
        if (cap > (8u << 20)) return -1;
        uint8_t *o = realloc(p->out, cap);
        if (!o) return -1;
        p->out = o;
        p->out_cap = cap;
    }
    memcpy(p->out + p->out_len, d, n);
    p->out_len += n;
    return out_flush(p);
}

static uint32_t begin(LmPulse *p, TsOut *t, uint32_t cmd) {
    ts_init(t);
    uint32_t tag = p->tag++;
    ts_u32(t, cmd);
    ts_u32(t, tag);
    return tag;
}

static int sendcmd(LmPulse *p, TsOut *t, uint32_t tag, int what) {
    size_t n = ts_finish(t);
    int r = n ? out_queue(p, t->d, n) : -1;
    ts_free(t);
    if (!r && what != W_ACK) { p->wait_tag = tag; p->wait_what = what; p->wait_done = 0; }
    return r;
}

static void advance(LmPulse *p);

static void on_packet(LmPulse *p, uint32_t ch, const uint8_t *d, size_t n) {
    if (ch != 0xffffffffu) {
        if (ch == p->rec_ch && p->on_record) p->on_record(p->ud, d, n);
        return;
    }
    TsIn t;
    ts_in(&t, d, n);
    uint32_t cmd = ts_get_u32(&t), tag = ts_get_u32(&t);
    if (t.err) return;
    if (cmd == PA_CMD_REQUEST) {
        uint32_t c = ts_get_u32(&t), bytes = ts_get_u32(&t);
        if (!t.err && c == p->play_ch) p->requested += bytes;
        return;
    }
    if (cmd == PA_CMD_PLAYBACK_STREAM_KILLED || cmd == PA_CMD_RECORD_STREAM_KILLED) { p->killed = 1; return; }
    if (cmd == PA_CMD_UNDERFLOW) { p->underflows++; return; }
    if (cmd != PA_CMD_REPLY && cmd != PA_CMD_ERROR) return;
    if (tag == p->lat_tag && p->lat_pending) {
        p->lat_pending = 0;
        if (cmd == PA_CMD_REPLY) {
            uint64_t sink = ts_get_u64(&t);
            ts_get_u64(&t);                              /* source_usec */
            p->lat_playing = ts_get_bool(&t);
            ts_skip(&t);                                 /* local timeval */
            ts_skip(&t);                                 /* remote timeval */
            int64_t wi = (int64_t)ts_get_u64(&t), ri = (int64_t)ts_get_u64(&t);
            if (!t.err) {
                p->sink_usec = sink;
                p->write_index = wi;
                p->read_index = ri;
                p->lat_at_ms = (p->lat_sent_ms + lm_now_ms()) / 2;   /* the server answered halfway */
                p->lat_valid = 1;
            }
        }
        return;
    }
    if (tag != p->wait_tag || p->wait_done) return;
    p->wait_done = cmd == PA_CMD_REPLY ? 1 : -1;
    if (cmd != PA_CMD_REPLY) { p->failed = 1; return; }
    if (p->wait_what == W_AUTH) { uint32_t v = ts_get_u32(&t); p->version = (v & 0xffff) < PROTO ? (v & 0xffff) : PROTO; }
    else if (p->wait_what == W_PLAY) {
        p->play_ch = ts_get_u32(&t);
        p->play_index = ts_get_u32(&t);
        p->requested += ts_get_u32(&t);
    } else if (p->wait_what == W_REC) {
        p->rec_ch = ts_get_u32(&t);
        p->rec_index = ts_get_u32(&t);
    }
    advance(p);
}

int lmp_dispatch(LmPulse *p) {
    if (p->fd < 0) return -1;
    for (;;) {
        if (p->in_cap - p->in_len < 65536) {
            size_t cap = p->in_cap ? p->in_cap * 2 : 131072;
            if (cap > PA_FRAME_MAX + 131072) return -1;
            uint8_t *b = realloc(p->in, cap);
            if (!b) return -1;
            p->in = b;
            p->in_cap = cap;
        }
        ssize_t r = recv(p->fd, p->in + p->in_len, p->in_cap - p->in_len, MSG_DONTWAIT);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno == EAGAIN) break;
        if (r <= 0) { p->fd = -1; return -1; }
        p->in_len += (size_t)r;
        size_t off = 0;
        while (p->in_len - off >= PA_DESCRIPTOR_SIZE) {
            uint32_t len, ch, flags;
            uint64_t o;
            pa_descriptor_read(p->in + off, &len, &ch, &o, &flags);
            if (len > PA_FRAME_MAX) { p->fd = -1; return -1; }
            if (p->in_len - off < PA_DESCRIPTOR_SIZE + (size_t)len) break;
            on_packet(p, ch, p->in + off + PA_DESCRIPTOR_SIZE, len);
            off += PA_DESCRIPTOR_SIZE + (size_t)len;
        }
        memmove(p->in, p->in + off, p->in_len - off);
        p->in_len -= off;
    }
    return out_flush(p);
}

static int send_name(LmPulse *p) {
    TsOut t;
    uint32_t tag = begin(p, &t, PA_CMD_SET_CLIENT_NAME);
    const char *app_name = p->app_name, *app_id = p->app_id;
    char pid[16];
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    const char *kv[] = { "application.name", app_name, "application.id", app_id, "application.process.id", pid,
                         "application.process.binary", "axia-media", "application.icon_name", "multimedia-player" };
    ts_proplist(&t, kv, 5);
    return sendcmd(p, &t, tag, W_NAME);
}

static int send_playback(LmPulse *p);
static int send_record(LmPulse *p);

/* Next step of the handshake: AUTH → name → create the stream → ready. */
static void advance(LmPulse *p) {
    if (p->wait_done != 1) return;
    if (p->wait_what == W_AUTH) { if (send_name(p)) p->failed = 1; }
    else if (p->wait_what == W_NAME) { if ((p->want == 1 ? send_playback(p) : send_record(p))) p->failed = 1; }
    else if (p->wait_what == W_PLAY || p->wait_what == W_REC) p->ready = 1;
}

int lmp_connect(LmPulse *p, int fd, const char *app_name, const char *app_id) {
    memset(p, 0, sizeof *p);
    p->fd = fd;
    p->play_ch = p->rec_ch = 0xffffffffu;
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    snprintf(p->app_name, sizeof p->app_name, "%s", app_name);
    snprintf(p->app_id, sizeof p->app_id, "%s", app_id);
    TsOut t;
    uint8_t cookie[256] = { 0 };
    uint32_t tag = begin(p, &t, PA_CMD_AUTH);
    ts_u32(&t, PROTO);
    ts_arb(&t, cookie, sizeof cookie);
    return sendcmd(p, &t, tag, W_AUTH);
}

static void channel_map(TsOut *t, int ch) {
    uint8_t pos[2] = { ch == 1 ? 0 : 1, 2 };            /* MONO or FRONT_LEFT, FRONT_RIGHT */
    ts_cmap(t, (uint8_t)ch, pos);
}

int lmp_playback(LmPulse *p, int rate, int ch, const char *media_name, const char *role) {
    p->want = 1;
    p->rate = rate;
    p->channels = ch;
    p->frame_bytes = (uint32_t)ch * 2;
    snprintf(p->media_name, sizeof p->media_name, "%s", media_name);
    snprintf(p->role, sizeof p->role, "%s", role);
    return 0;
}

static int send_playback(LmPulse *p) {
    int rate = p->rate, ch = p->channels;
    const char *media_name = p->media_name, *role = p->role;
    if (p->fd < 0) return -1;
    TsOut t;
    uint32_t tag = begin(p, &t, PA_CMD_CREATE_PLAYBACK_STREAM);
    ts_ss(&t, PA_SAMPLE_S16LE, (uint8_t)ch, (uint32_t)rate);
    channel_map(&t, ch);
    ts_u32(&t, PA_INVALID_INDEX);                    /* the default output */
    ts_str(&t, NULL);
    ts_u32(&t, 0xffffffffu);                         /* maxlength */
    ts_bool(&t, 1);                                  /* corked: starts when there's data */
    uint32_t bps = (uint32_t)rate * p->frame_bytes;
    ts_u32(&t, bps / 5);                             /* tlength: 200 ms */
    ts_u32(&t, 0xffffffffu);                         /* prebuf: the server's */
    ts_u32(&t, bps / 50);                            /* minreq: 20 ms */
    ts_u32(&t, 0);                                   /* syncid */
    uint32_t vol[2] = { PA_VOLUME_NORM, PA_VOLUME_NORM };
    ts_cvol(&t, (uint8_t)ch, vol);
    ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0);   /* no_remap … fix_channels */
    ts_bool(&t, 0);                                  /* no_move */
    ts_bool(&t, 0);                                  /* variable_rate */
    ts_bool(&t, 0);                                  /* muted */
    ts_bool(&t, 1);                                  /* adjust_latency */
    const char *kv[] = { "media.name", media_name, "media.role", role };
    ts_proplist(&t, kv, 2);
    ts_bool(&t, 0);                                  /* volume_set */
    ts_bool(&t, 0);                                  /* early_requests */
    ts_bool(&t, 0);                                  /* muted_set */
    ts_bool(&t, 0);                                  /* dont_inhibit_auto_suspend */
    ts_bool(&t, 0);                                  /* fail_on_suspend */
    ts_bool(&t, 0);                                  /* relative_volume */
    ts_bool(&t, 0);                                  /* passthrough */
    ts_u8(&t, 0);                                    /* formats: uses the sample spec */
    p->corked = 1;
    return sendcmd(p, &t, tag, W_PLAY);
}

int lmp_record(LmPulse *p, const char *source, int rate, int ch, const char *media_name) {
    p->want = 2;
    p->rate = rate;
    p->channels = ch;
    p->frame_bytes = (uint32_t)ch * 2;
    snprintf(p->media_name, sizeof p->media_name, "%s", media_name);
    snprintf(p->source, sizeof p->source, "%s", source ? source : "");
    return 0;
}

static int send_record(LmPulse *p) {
    int rate = p->rate, ch = p->channels;
    const char *source = p->source, *media_name = p->media_name;
    if (p->fd < 0) return -1;
    TsOut t;
    uint32_t tag = begin(p, &t, PA_CMD_CREATE_RECORD_STREAM);
    ts_ss(&t, PA_SAMPLE_S16LE, (uint8_t)ch, (uint32_t)rate);
    channel_map(&t, ch);
    ts_u32(&t, PA_INVALID_INDEX);
    ts_str(&t, source && *source ? source : NULL);
    ts_u32(&t, 0xffffffffu);
    ts_bool(&t, 0);                                  /* corked */
    ts_u32(&t, (uint32_t)rate * p->frame_bytes / 50); /* fragsize: 20 ms */
    ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0);
    ts_bool(&t, 0);                                  /* no_move */
    ts_bool(&t, 0);                                  /* variable_rate */
    ts_bool(&t, 0);                                  /* peak_detect */
    ts_bool(&t, 1);                                  /* adjust_latency */
    const char *kv[] = { "media.name", media_name, "media.role", "production" };
    ts_proplist(&t, kv, 2);
    ts_u32(&t, PA_INVALID_INDEX);                    /* direct_on_input */
    ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0);  /* early_requests, dont_inhibit, fail_on_suspend */
    ts_u8(&t, 1);
    ts_format_pcm(&t);
    uint32_t vol[2] = { PA_VOLUME_NORM, PA_VOLUME_NORM };
    ts_cvol(&t, (uint8_t)ch, vol);
    ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0);   /* muted, volume_set, muted_set, relative, passthrough */
    return sendcmd(p, &t, tag, W_REC);
}

int lmp_write(LmPulse *p, const void *d, size_t n) {
    if (p->fd < 0 || !p->ready || p->play_ch == 0xffffffffu) return -1;
    size_t total;
    uint8_t *pkt = ts_data_packet(p->play_ch, d, (uint32_t)n, &total);
    if (!pkt) return -1;
    int r = out_queue(p, pkt, total);
    free(pkt);
    if (!r) {
        p->written += (int64_t)n;
        p->requested -= (int64_t)n;
    }
    return r;
}

void lmp_cork(LmPulse *p, int on) {
    if (p->fd < 0 || !p->ready || p->play_ch == 0xffffffffu) return;
    TsOut t;
    uint32_t tag = begin(p, &t, PA_CMD_CORK_PLAYBACK_STREAM);
    ts_u32(&t, p->play_ch);
    ts_bool(&t, on);
    sendcmd(p, &t, tag, W_ACK);
    p->corked = on;
    p->lat_valid = 0;
}

void lmp_flush(LmPulse *p) {
    if (p->fd < 0 || !p->ready || p->play_ch == 0xffffffffu) return;
    TsOut t;
    uint32_t tag = begin(p, &t, PA_CMD_FLUSH_PLAYBACK_STREAM);
    ts_u32(&t, p->play_ch);
    sendcmd(p, &t, tag, W_ACK);
    p->lat_valid = 0;
}

void lmp_latency(LmPulse *p) {
    if (p->fd < 0 || !p->ready || p->play_ch == 0xffffffffu || p->lat_pending) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    TsOut t;
    uint32_t tag = begin(p, &t, PA_CMD_GET_PLAYBACK_LATENCY);
    ts_u32(&t, p->play_ch);
    ts_timeval(&t, (uint32_t)tv.tv_sec, (uint32_t)tv.tv_usec);
    size_t n = ts_finish(&t);
    if (n && !out_queue(p, t.d, n)) { p->lat_tag = tag; p->lat_pending = 1; p->lat_sent_ms = lm_now_ms(); }
    ts_free(&t);
}

void lmp_volume(LmPulse *p, double v) {
    if (p->fd < 0 || !p->ready || p->play_ch == 0xffffffffu) return;
    if (v < 0) v = 0;
    if (v > 1.5) v = 1.5;
    uint32_t vol = (uint32_t)(v * PA_VOLUME_NORM + 0.5);      /* linear, same as lord.sound */
    uint32_t vv[2] = { vol, vol };
    TsOut t;
    uint32_t tag = begin(p, &t, PA_CMD_SET_SINK_INPUT_VOLUME);
    ts_u32(&t, p->play_index);
    ts_cvol(&t, (uint8_t)p->channels, vv);
    sendcmd(p, &t, tag, W_ACK);
}

void lmp_close(LmPulse *p) {
    if (p->fd >= 0) close(p->fd);
    p->fd = -1;
    free(p->in);
    free(p->out);
    p->in = p->out = NULL;
}
