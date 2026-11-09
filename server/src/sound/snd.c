/* snd.c — LordOS sound client.
 *
 * Speaks the PulseAudio native protocol with pipewire-pulse (the part of PipeWire that
 * serves it at $XDG_RUNTIME_DIR/pulse/native): no libpulse or libpipewire. Keeps a copy of
 * the outputs (sinks), inputs (sources), apps playing (sink inputs) and apps recording
 * (source outputs); subscribes to changes and notifies JS with "state" when something changed
 * and all replies have arrived. If PipeWire restarts, it reconnects every 2 s.
 *
 * Bluetooth cards (sndcard.c) are kept too, with their profiles: a headset plays well only in
 * its high-fidelity profile, and snd_set_profile switches it (issue #78, docs/SOUND.md).
 *
 * Extras: level meter for an input (a normal 48 kHz recording stream; the level is the largest
 * sample of each block, see snd_meter) and system sounds synthesized here (uploaded once to the
 * server's sample cache and then played with PLAY_SAMPLE). */
#include "snd.h"
#include "pulse.h"
#include "sndcard.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

#define PROTOCOL_VERSION 32             /* ports with "available", no availability groups (v34) */
#define MAX_DEV 32
#define MAX_STREAMS 64
#define MAX_PORTS 16
#define MAX_PENDING 256
#define MAX_CARDS 8                     /* Bluetooth cards (sndcard.h) */

typedef struct { char name[128], desc[128]; uint32_t priority, available; } Port;

typedef struct {
    uint32_t index, monitor, card, state, base;      /* monitor: for a sink, its source; for a source, its sink */
    char name[256], desc[256], icon[96], form[32], bus[32];
    uint8_t ch, map[PA_CHANNELS_MAX];
    uint32_t vol[PA_CHANNELS_MAX];
    int mute, nports;
    Port ports[MAX_PORTS];
    char active[128];
} Dev;

typedef struct {
    uint32_t index, client, dev, pid;
    char name[256], app[128], icon[96], binary[64], role[32], appid[128];
    uint8_t ch;
    uint32_t vol[PA_CHANNELS_MAX];
    int mute, corked, has_volume, writable;
} Stream;

typedef struct { Dev d[MAX_DEV]; int n; } DevList;
typedef struct { Stream s[MAX_STREAMS]; int n; } StreamList;
typedef struct { SndCard c[MAX_CARDS]; int n; } CardList;

enum {
    R_NONE, R_AUTH, R_NAME, R_SERVER, R_LIST, R_ONE, R_ACK, R_RECORD, R_UPLOAD, R_PLAY,
};

typedef struct { uint32_t tag; int kind, what; uint32_t arg; } Pending;

static struct {
    int fd, timer;                      /* socket; reconnection timer */
    int connected;                      /* handshake finished */
    uint32_t version, tag;
    Pending pend[MAX_PENDING];
    int info_pending, dirty;
    uint8_t *in;                        /* incoming packet */
    size_t in_len, in_cap;
    uint8_t *out;                       /* what's left to write */
    size_t out_len, out_cap;
    char server_name[64], server_version[32], default_sink[256], default_source[256];
    DevList sinks, sources;
    StreamList inputs, outputs;
    CardList cards;
    uint32_t meter_source, meter_channel, meter_stream_ok;
    int meter_pending;
    double peak;
    int peak_ready;
    int debug;                          /* LORD_SND_DEBUG=1: every packet to stderr */
    int samples_uploaded, uploading, play_after;   /* bits: pop, notify, error */
    char last_error[128];
} S = { .fd = -1, .timer = -1, .meter_source = PA_INVALID_INDEX, .meter_channel = PA_INVALID_INDEX };

static void disconnect(const char *why);
static void upload_data(int which, uint32_t channel);
static int play_sample(int which);

/* ---- E/S ---- */

static int flush_out(void) {
    while (S.out_len) {
        ssize_t w = send(S.fd, S.out, S.out_len, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) return 0;
        if (w <= 0) return -1;
        memmove(S.out, S.out + w, S.out_len - (size_t)w);
        S.out_len -= (size_t)w;
    }
    return 0;
}

static int queue_bytes(const uint8_t *p, size_t n) {
    if (S.fd < 0) return -1;
    if (S.out_len + n > S.out_cap) {
        size_t cap = S.out_cap ? S.out_cap * 2 : 4096;
        while (cap < S.out_len + n) cap *= 2;
        if (cap > PA_FRAME_MAX) return -1;
        uint8_t *o = realloc(S.out, cap);
        if (!o) return -1;
        S.out = o;
        S.out_cap = cap;
    }
    memcpy(S.out + S.out_len, p, n);
    S.out_len += n;
    if (flush_out()) { disconnect("write error"); return -1; }
    return 0;
}

static Pending *pend_add(uint32_t tag, int kind, int what, uint32_t arg) {
    for (int i = 0; i < MAX_PENDING; i++)
        if (S.pend[i].kind == R_NONE) {
            S.pend[i] = (Pending){ tag, kind, what, arg };
            if (kind == R_LIST || kind == R_ONE || kind == R_SERVER) S.info_pending++;
            return &S.pend[i];
        }
    return NULL;
}

/* Starts a command: tagstruct with command and tag. */
static uint32_t cmd_begin(TsOut *t, uint32_t cmd) {
    ts_init(t);
    uint32_t tag = S.tag++;
    ts_u32(t, cmd);
    ts_u32(t, tag);
    return tag;
}

static int cmd_send(TsOut *t, uint32_t tag, int kind, int what, uint32_t arg) {
    size_t n = ts_finish(t);
    int r = -1;
    if (n && pend_add(tag, kind, what, arg)) r = queue_bytes(t->d, n);
    ts_free(t);
    return r;
}

/* ---- model ---- */

static void copy(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }

static int is_list_cmd(int what) {
    return what == PA_CMD_GET_SINK_INFO_LIST || what == PA_CMD_GET_SOURCE_INFO_LIST ||
           what == PA_CMD_GET_SINK_INPUT_INFO_LIST || what == PA_CMD_GET_SOURCE_OUTPUT_INFO_LIST ||
           what == PA_CMD_GET_CARD_INFO_LIST;
}

static void dev_prop(void *ud, const char *key, const char *val, uint32_t len) {
    (void)len;
    Dev *d = ud;
    if (!strcmp(key, "device.icon_name")) copy(d->icon, sizeof d->icon, val);
    else if (!strcmp(key, "device.form_factor")) copy(d->form, sizeof d->form, val);
    else if (!strcmp(key, "device.bus")) copy(d->bus, sizeof d->bus, val);
}

static int parse_dev(TsIn *t, Dev *d, int source) {
    memset(d, 0, sizeof *d);
    d->index = ts_get_u32(t);
    copy(d->name, sizeof d->name, ts_get_str(t));
    copy(d->desc, sizeof d->desc, ts_get_str(t));
    uint8_t f, c; uint32_t rate;
    ts_get_ss(t, &f, &c, &rate);
    ts_get_cmap(t, d->map);
    ts_get_u32(t);                                  /* module */
    d->ch = ts_get_cvol(t, d->vol);
    d->mute = ts_get_bool(t);
    d->monitor = ts_get_u32(t);
    ts_get_str(t);                                  /* monitor name */
    ts_get_u64(t);                                  /* latency */
    ts_get_str(t);                                  /* driver */
    ts_get_u32(t);                                  /* flags */
    ts_get_proplist(t, dev_prop, d);
    ts_get_u64(t);                                  /* requested latency */
    d->base = ts_get_vol(t);
    d->state = ts_get_u32(t);
    ts_get_u32(t);                                  /* volume steps */
    d->card = ts_get_u32(t);
    uint32_t np = ts_get_u32(t);
    for (uint32_t i = 0; i < np && !t->err; i++) {
        Port tmp, *p = d->nports < MAX_PORTS ? &d->ports[d->nports++] : &tmp;
        copy(p->name, sizeof p->name, ts_get_str(t));
        copy(p->desc, sizeof p->desc, ts_get_str(t));
        p->priority = ts_get_u32(t);
        p->available = ts_get_u32(t);               /* v24: 0 unknown, 1 no, 2 yes */
    }
    copy(d->active, sizeof d->active, ts_get_str(t));
    uint8_t nf = ts_get_u8(t);                      /* formats (v21 sinks, v22 sources) */
    for (uint8_t i = 0; i < nf && !t->err; i++) ts_skip_format(t);
    (void)source;
    return t->err ? -1 : 0;
}

static void stream_prop(void *ud, const char *key, const char *val, uint32_t len) {
    (void)len;
    Stream *s = ud;
    if (!strcmp(key, "application.name")) copy(s->app, sizeof s->app, val);
    else if (!strcmp(key, "application.icon_name")) copy(s->icon, sizeof s->icon, val);
    else if (!strcmp(key, "application.process.binary")) copy(s->binary, sizeof s->binary, val);
    else if (!strcmp(key, "application.process.id")) s->pid = (uint32_t)strtoul(val, NULL, 10);
    else if (!strcmp(key, "application.id")) copy(s->appid, sizeof s->appid, val);
    else if (!strcmp(key, "media.role")) copy(s->role, sizeof s->role, val);
    else if (!s->icon[0] && !strcmp(key, "media.icon_name")) copy(s->icon, sizeof s->icon, val);
}

static int parse_stream(TsIn *t, Stream *s, int output) {
    memset(s, 0, sizeof *s);
    s->index = ts_get_u32(t);
    copy(s->name, sizeof s->name, ts_get_str(t));
    ts_get_u32(t);                                  /* module */
    s->client = ts_get_u32(t);
    s->dev = ts_get_u32(t);
    uint8_t f, c, map[PA_CHANNELS_MAX]; uint32_t rate;
    ts_get_ss(t, &f, &c, &rate);
    ts_get_cmap(t, map);
    if (!output) s->ch = ts_get_cvol(t, s->vol);
    ts_get_u64(t);
    ts_get_u64(t);
    ts_get_str(t);                                  /* resample method */
    ts_get_str(t);                                  /* driver */
    if (!output) s->mute = ts_get_bool(t);
    ts_get_proplist(t, stream_prop, s);
    s->corked = ts_get_bool(t);
    if (output) { s->ch = ts_get_cvol(t, s->vol); s->mute = ts_get_bool(t); }
    s->has_volume = ts_get_bool(t);
    s->writable = ts_get_bool(t);
    ts_skip_format(t);
    return t->err ? -1 : 0;
}

static DevList *dev_list(int what) {
    return what == PA_CMD_GET_SINK_INFO || what == PA_CMD_GET_SINK_INFO_LIST ? &S.sinks : &S.sources;
}

static StreamList *stream_list(int what) {
    return what == PA_CMD_GET_SINK_INPUT_INFO || what == PA_CMD_GET_SINK_INPUT_INFO_LIST ? &S.inputs : &S.outputs;
}

static int is_dev_cmd(int what) {
    return what == PA_CMD_GET_SINK_INFO || what == PA_CMD_GET_SINK_INFO_LIST ||
           what == PA_CMD_GET_SOURCE_INFO || what == PA_CMD_GET_SOURCE_INFO_LIST;
}

static void upsert_dev(DevList *l, const Dev *d) {
    for (int i = 0; i < l->n; i++) if (l->d[i].index == d->index) { l->d[i] = *d; return; }
    if (l->n < MAX_DEV) l->d[l->n++] = *d;
}

static void upsert_stream(StreamList *l, const Stream *s) {
    for (int i = 0; i < l->n; i++) if (l->s[i].index == s->index) { l->s[i] = *s; return; }
    if (l->n < MAX_STREAMS) l->s[l->n++] = *s;
}

static void remove_card(uint32_t index) {
    CardList *l = &S.cards;
    for (int i = 0; i < l->n; i++) if (l->c[i].index == index) { l->c[i] = l->c[--l->n]; break; }
}

/* Only Bluetooth cards are kept; one that stopped being one (never seen) is dropped. */
static void upsert_card(const SndCard *c) {
    CardList *l = &S.cards;
    if (!snd_card_is_bluetooth(c)) { remove_card(c->index); return; }
    for (int i = 0; i < l->n; i++) if (l->c[i].index == c->index) { l->c[i] = *c; return; }
    if (l->n < MAX_CARDS) l->c[l->n++] = *c;
}

static void remove_index(int facility, uint32_t index) {
    if (facility == PA_SUB_SINK || facility == PA_SUB_SOURCE) {
        DevList *l = facility == PA_SUB_SINK ? &S.sinks : &S.sources;
        for (int i = 0; i < l->n; i++) if (l->d[i].index == index) { l->d[i] = l->d[--l->n]; break; }
    } else {
        StreamList *l = facility == PA_SUB_SINK_INPUT ? &S.inputs : &S.outputs;
        for (int i = 0; i < l->n; i++) if (l->s[i].index == index) { l->s[i] = l->s[--l->n]; break; }
    }
}

static void on_info(int what, TsIn *t) {
    int list = is_list_cmd(what);
    if (what == PA_CMD_GET_CARD_INFO || what == PA_CMD_GET_CARD_INFO_LIST) {
        if (list) S.cards.n = 0;
        static SndCard c;                           /* ~6 KB: not on the stack */
        while (!ts_eof(t) && !t->err && snd_card_parse(t, &c) == 0) upsert_card(&c);
    } else if (is_dev_cmd(what)) {
        DevList *l = dev_list(what);
        if (list) l->n = 0;
        Dev d;
        while (!ts_eof(t) && !t->err && parse_dev(t, &d, l == &S.sources) == 0) upsert_dev(l, &d);
    } else {
        StreamList *l = stream_list(what);
        int output = l == &S.outputs;
        if (list) l->n = 0;
        Stream s;
        while (!ts_eof(t) && !t->err && parse_stream(t, &s, output) == 0) upsert_stream(l, &s);
    }
    if (t->err) fprintf(stderr, "sound: reply %d with unexpected format\n", what);
    S.dirty = 1;
}

static void request_info(uint32_t cmd, uint32_t index) {
    TsOut t;
    uint32_t tag = cmd_begin(&t, cmd);
    int list = is_list_cmd((int)cmd);
    if (!list) {
        ts_u32(&t, index);
        if (cmd == PA_CMD_GET_SINK_INFO || cmd == PA_CMD_GET_SOURCE_INFO || cmd == PA_CMD_GET_CARD_INFO) ts_str(&t, NULL);
    }
    cmd_send(&t, tag, list ? R_LIST : R_ONE, (int)cmd, index);
}

static void request_server(void) {
    TsOut t;
    uint32_t tag = cmd_begin(&t, PA_CMD_GET_SERVER_INFO);
    cmd_send(&t, tag, R_SERVER, 0, 0);
}

static void request_all(void) {
    request_server();
    request_info(PA_CMD_GET_SINK_INFO_LIST, 0);
    request_info(PA_CMD_GET_SOURCE_INFO_LIST, 0);
    request_info(PA_CMD_GET_SINK_INPUT_INFO_LIST, 0);
    request_info(PA_CMD_GET_SOURCE_OUTPUT_INFO_LIST, 0);
    request_info(PA_CMD_GET_CARD_INFO_LIST, 0);
}

static void on_subscribe_event(uint32_t ev, uint32_t index) {
    int facility = (int)(ev & PA_SUB_FACILITY_MASK), type = (int)(ev & PA_SUB_TYPE_MASK);
    static const uint32_t get[] = { PA_CMD_GET_SINK_INFO, PA_CMD_GET_SOURCE_INFO, PA_CMD_GET_SINK_INPUT_INFO, PA_CMD_GET_SOURCE_OUTPUT_INFO };
    if (facility == PA_SUB_SERVER) { request_server(); return; }
    if (facility == PA_SUB_CARD) {                  /* the profile changed: the ports of its sinks/sources */
        if (type == PA_SUB_REMOVE) { remove_card(index); S.dirty = 1; }
        else request_info(PA_CMD_GET_CARD_INFO, index);
        request_info(PA_CMD_GET_SINK_INFO_LIST, 0);
        request_info(PA_CMD_GET_SOURCE_INFO_LIST, 0);
        return;
    }
    if (facility > PA_SUB_SOURCE_OUTPUT) return;
    if (type == PA_SUB_REMOVE) { remove_index(facility, index); S.dirty = 1; return; }
    request_info(get[facility], index);
}

/* ---- handshake ---- */

static void send_client_name(void) {
    TsOut t;
    uint32_t tag = cmd_begin(&t, PA_CMD_SET_CLIENT_NAME);
    char pid[16];
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    const char *kv[] = {
        "application.name", "LordOS", "application.id", "org.lordos.Server",
        "application.icon_name", "audio-card", "application.process.id", pid,
        "application.process.binary", "lord-server",
    };
    ts_proplist(&t, kv, 5);
    cmd_send(&t, tag, R_NAME, 0, 0);
}

static void after_connected(void) {
    TsOut t;
    uint32_t tag = cmd_begin(&t, PA_CMD_SUBSCRIBE);
    ts_u32(&t, PA_SUB_MASK_SINK | PA_SUB_MASK_SOURCE | PA_SUB_MASK_SINK_INPUT | PA_SUB_MASK_SOURCE_OUTPUT |
               PA_SUB_MASK_SERVER | PA_SUB_MASK_CARD);
    cmd_send(&t, tag, R_ACK, PA_CMD_SUBSCRIBE, 0);
    request_all();
    if (S.meter_source != PA_INVALID_INDEX) { uint32_t m = S.meter_source; S.meter_source = PA_INVALID_INDEX; snd_meter(m); }
}

static const char *socket_path(char *buf, size_t cap) {
    const char *ps = getenv("PULSE_SERVER");
    if (ps && !strncmp(ps, "unix:", 5)) { snprintf(buf, cap, "%s", ps + 5); return buf; }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char def[64];
    if (!rt || !*rt) { snprintf(def, sizeof def, "/run/user/%u", (unsigned)getuid()); rt = def; }
    snprintf(buf, cap, "%s/pulse/native", rt);
    return buf;
}

static void arm_timer(int on) {
    if (S.timer < 0) S.timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (S.timer < 0) return;
    struct itimerspec its = { 0 };
    if (on) { its.it_value.tv_sec = 2; its.it_interval.tv_sec = 2; }
    timerfd_settime(S.timer, 0, &its, NULL);
}

static int try_connect(void) {
    char path[sizeof ((struct sockaddr_un *)0)->sun_path];
    socket_path(path, sizeof path);
    struct stat st;
    /* only the user's own server (the socket lives in their runtime folder) */
    if (stat(path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != getuid()) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    S.fd = fd;
    S.tag = 0;
    S.in_len = S.out_len = 0;
    S.info_pending = 0;
    memset(S.pend, 0, sizeof S.pend);
    S.samples_uploaded = S.uploading = S.play_after = 0;
    S.meter_channel = PA_INVALID_INDEX;
    S.meter_pending = 0;

    /* cookie: PulseAudio's if it exists (pipewire-pulse doesn't require it) */
    uint8_t cookie[256] = { 0 };
    const char *home = getenv("HOME");
    if (home) {
        char cp[512];
        snprintf(cp, sizeof cp, "%s/.config/pulse/cookie", home);
        int cf = open(cp, O_RDONLY | O_CLOEXEC);
        if (cf >= 0) { if (read(cf, cookie, sizeof cookie) < 0) memset(cookie, 0, sizeof cookie); close(cf); }
    }
    TsOut t;
    uint32_t tag = cmd_begin(&t, PA_CMD_AUTH);
    ts_u32(&t, PROTOCOL_VERSION);                   /* no shared memory: data goes over the socket */
    ts_arb(&t, cookie, sizeof cookie);
    if (cmd_send(&t, tag, R_AUTH, 0, 0)) return -1;
    arm_timer(0);
    return 0;
}

static void disconnect(const char *why) {
    if (S.fd >= 0) {
        close(S.fd);
        S.fd = -1;
        if (S.connected) fprintf(stderr, "sound: disconnected from PipeWire (%s); retrying\n", why);
    }
    S.connected = 0;
    S.sinks.n = S.sources.n = S.inputs.n = S.outputs.n = S.cards.n = 0;
    S.server_name[0] = S.default_sink[0] = S.default_source[0] = 0;
    S.dirty = 1;
    arm_timer(1);
}

int snd_start(void) {
    S.debug = getenv("LORD_SND_DEBUG") != NULL;
    if (try_connect()) { disconnect("no server"); return -1; }
    return 0;
}

void snd_stop(void) {
    if (S.fd >= 0) close(S.fd);
    if (S.timer >= 0) close(S.timer);
    S.fd = S.timer = -1;
    free(S.in); free(S.out);
    S.in = S.out = NULL;
    S.in_cap = S.out_cap = S.in_len = S.out_len = 0;
}

int snd_fd(void) { return S.fd >= 0 ? S.fd : S.timer; }

/* ---- incoming packets ---- */

static void on_reply(Pending p, TsIn *t) {
    switch (p.kind) {
    case R_AUTH: {
        S.version = ts_get_u32(t) & 0xffff;
        if (S.version > PROTOCOL_VERSION) S.version = PROTOCOL_VERSION;
        if (S.version < PROTOCOL_VERSION) { disconnect("old protocol version"); return; }
        send_client_name();
        break;
    }
    case R_NAME:
        S.connected = 1;
        S.dirty = 1;
        fprintf(stderr, "sound: connected to PipeWire (protocol %u)\n", S.version);
        after_connected();
        break;
    case R_SERVER: {
        copy(S.server_name, sizeof S.server_name, ts_get_str(t));
        copy(S.server_version, sizeof S.server_version, ts_get_str(t));
        ts_get_str(t); ts_get_str(t);                 /* user, host */
        uint8_t f, c; uint32_t r;
        ts_get_ss(t, &f, &c, &r);
        copy(S.default_sink, sizeof S.default_sink, ts_get_str(t));
        copy(S.default_source, sizeof S.default_source, ts_get_str(t));
        S.dirty = 1;
        break;
    }
    case R_LIST: case R_ONE: on_info(p.what, t); break;
    case R_RECORD: {
        uint32_t ch = ts_get_u32(t);
        S.meter_pending = 0;
        if (t->err) break;
        if (S.meter_source == PA_INVALID_INDEX || S.meter_source != p.arg) {   /* it was turned off in the meantime */
            TsOut o;
            uint32_t tag = cmd_begin(&o, PA_CMD_DELETE_RECORD_STREAM);
            ts_u32(&o, ch);
            cmd_send(&o, tag, R_ACK, PA_CMD_DELETE_RECORD_STREAM, 0);
            break;
        }
        S.meter_channel = ch;
        break;
    }
    case R_UPLOAD: {
        uint32_t ch = ts_get_u32(t), len = ts_get_u32(t);
        (void)len;
        if (t->err || p.arg > 2) break;
        upload_data((int)p.arg, ch);
        break;
    }
    default: break;
    }
}

static void on_error(Pending p, uint32_t code) {
    if (p.kind == R_ONE) return;                    /* it disappeared between the notice and the request */
    if (p.kind == R_RECORD) S.meter_pending = 0;
    snprintf(S.last_error, sizeof S.last_error, "comando %d: error %u", p.what, code);
    fprintf(stderr, "sonido: %s\n", S.last_error);
    if (p.kind == R_AUTH || p.kind == R_NAME) disconnect("connection refused");
}

static void on_packet(uint32_t channel, const uint8_t *d, size_t n) {
    if (channel != 0xffffffffu) {                   /* data: the meter (float32 samples; the level is the largest) */
        if (channel == S.meter_channel && n >= 4) {
            float v = 0, m = 0;
            for (size_t i = 0; i + 4 <= n; i += 4) { memcpy(&v, d + i, 4); v = fabsf(v); if (v > m) m = v; }
            S.peak = m > 1 ? 1 : m;
            S.peak_ready = 1;
        }
        return;
    }
    TsIn t;
    ts_in(&t, d, n);
    uint32_t cmd = ts_get_u32(&t), tag = ts_get_u32(&t);
    if (t.err) return;
    if (S.debug) fprintf(stderr, "sonido: <- comando %u tag %d (%zu bytes)\n", cmd, (int)tag, n);
    if (cmd == PA_CMD_REPLY || cmd == PA_CMD_ERROR) {
        for (int i = 0; i < MAX_PENDING; i++) {
            if (S.pend[i].kind == R_NONE || S.pend[i].tag != tag) continue;
            Pending p = S.pend[i];
            S.pend[i].kind = R_NONE;
            if (p.kind == R_LIST || p.kind == R_ONE || p.kind == R_SERVER) S.info_pending--;
            if (cmd == PA_CMD_REPLY) on_reply(p, &t); else on_error(p, ts_get_u32(&t));
            return;
        }
        return;
    }
    if (cmd == PA_CMD_SUBSCRIBE_EVENT) {
        uint32_t ev = ts_get_u32(&t), idx = ts_get_u32(&t);
        if (!t.err) on_subscribe_event(ev, idx);
        return;
    }
    if (cmd == PA_CMD_RECORD_STREAM_KILLED) {       /* the meter's source disappeared */
        uint32_t ch = ts_get_u32(&t);
        if (ch == S.meter_channel) { S.meter_channel = PA_INVALID_INDEX; S.meter_source = PA_INVALID_INDEX; }
    }
    /* REQUEST, STARTED, OVERFLOW...: we don't play our own streams (samples go to the cache) */
}

void snd_dispatch(void) {
    if (S.fd < 0) {
        uint64_t x;
        if (S.timer >= 0 && read(S.timer, &x, sizeof x) > 0) try_connect();
        return;
    }
    for (;;) {
        if (S.in_cap - S.in_len < 65536) {
            size_t cap = S.in_cap ? S.in_cap * 2 : 65536;
            while (cap - S.in_len < 65536) cap *= 2;
            if (cap > PA_FRAME_MAX + 65536) { disconnect("packet too large"); return; }
            uint8_t *b = realloc(S.in, cap);
            if (!b) { disconnect("out of memory"); return; }
            S.in = b;
            S.in_cap = cap;
        }
        ssize_t r = recv(S.fd, S.in + S.in_len, S.in_cap - S.in_len, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno == EAGAIN) break;
        if (r <= 0) { disconnect(r == 0 ? "connection closed" : "read error"); return; }
        S.in_len += (size_t)r;
        size_t off = 0;
        while (S.in_len - off >= PA_DESCRIPTOR_SIZE) {
            uint32_t len, ch, flags;
            uint64_t offset;
            pa_descriptor_read(S.in + off, &len, &ch, &offset, &flags);
            if (len > PA_FRAME_MAX) { disconnect("invalid packet"); return; }
            if (S.in_len - off < PA_DESCRIPTOR_SIZE + (size_t)len) break;
            on_packet(ch, S.in + off + PA_DESCRIPTOR_SIZE, len);
            if (S.fd < 0) return;
            off += PA_DESCRIPTOR_SIZE + (size_t)len;
        }
        memmove(S.in, S.in + off, S.in_len - off);
        S.in_len -= off;
    }
    if (flush_out()) disconnect("write error");
}

void snd_events(SndEventFn fn) {
    if (S.fd >= 0 && S.out_len && flush_out()) disconnect("write error");
    if (S.dirty && S.info_pending <= 0) {
        S.dirty = 0;
        char *j = snd_state_json();
        if (j) fn("state", j);
        free(j);
    }
    if (S.peak_ready) {
        S.peak_ready = 0;
        char j[96];
        snprintf(j, sizeof j, "{\"source\":%u,\"level\":%.4f}", S.meter_source, S.peak);
        fn("peak", j);
    }
}

/* ---- state as JSON ---- */

typedef struct { char *d; size_t len, cap; } Buf;

static void bput(Buf *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(b->d ? b->d + b->len : NULL, b->d ? b->cap - b->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (b->d && b->len + (size_t)n < b->cap) { b->len += (size_t)n; return; }
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + (size_t)n + 1) cap *= 2;
        char *d = realloc(b->d, cap);
        if (!d) return;
        b->d = d;
        b->cap = cap;
    }
}

static void bstr(Buf *b, const char *s) {
    bput(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') bput(b, "\\%c", *p);
        else if (*p < 0x20) bput(b, "\\u%04x", *p);
        else bput(b, "%c", *p);
    }
    bput(b, "\"");
}

static void bvol(Buf *b, uint8_t ch, const uint32_t *v) {
    bput(b, "\"volume\":[");
    double top = 0;
    for (uint8_t i = 0; i < ch; i++) {
        double x = (double)v[i] / PA_VOLUME_NORM;
        if (x > top) top = x;
        bput(b, "%s%.4f", i ? "," : "", x);
    }
    bput(b, "],\"level\":%.4f", top);
}

static void bidx(Buf *b, const char *key, uint32_t v) {
    if (v == PA_INVALID_INDEX) bput(b, ",\"%s\":-1", key); else bput(b, ",\"%s\":%u", key, v);
}

static void bdevs(Buf *b, const DevList *l, int source) {
    for (int i = 0; i < l->n; i++) {
        const Dev *d = &l->d[i];
        bput(b, "%s{\"index\":%u,\"name\":", i ? "," : "", d->index);
        bstr(b, d->name);
        bput(b, ",\"description\":");
        bstr(b, d->desc[0] ? d->desc : d->name);
        bput(b, ",\"icon\":");
        bstr(b, d->icon);
        bput(b, ",\"formFactor\":");
        bstr(b, d->form);
        bput(b, ",\"bus\":");
        bstr(b, d->bus);
        bput(b, ",");
        bvol(b, d->ch, d->vol);
        bput(b, ",\"mute\":%s,\"state\":%u", d->mute ? "true" : "false", d->state);
        bidx(b, source ? "monitorOf" : "monitor", d->monitor);
        bidx(b, "card", d->card);
        bput(b, ",\"default\":%s", !strcmp(d->name, source ? S.default_source : S.default_sink) ? "true" : "false");
        bput(b, ",\"ports\":[");
        for (int k = 0; k < d->nports; k++) {
            const Port *p = &d->ports[k];
            bput(b, "%s{\"name\":", k ? "," : "");
            bstr(b, p->name);
            bput(b, ",\"description\":");
            bstr(b, p->desc);
            bput(b, ",\"priority\":%u,\"available\":%s}", p->priority,
                 p->available == 1 ? "false" : p->available == 2 ? "true" : "null");
        }
        bput(b, "],\"activePort\":");
        bstr(b, d->active);
        bput(b, "}");
    }
}

static void bstreams(Buf *b, const StreamList *l, int output) {
    int first = 1;
    for (int i = 0; i < l->n; i++) {
        const Stream *s = &l->s[i];
        /* our own streams (meter, system sounds) aren't shown: otherwise the microphone
         * meter would make it look like someone is recording */
        if (!strcmp(s->appid, "org.lordos.Server")) continue;
        bput(b, "%s{\"index\":%u,\"name\":", first ? "" : ",", s->index);
        first = 0;
        bstr(b, s->name);
        bput(b, ",\"app\":");
        bstr(b, s->app[0] ? s->app : s->binary);
        bput(b, ",\"appId\":");
        bstr(b, s->appid);
        bput(b, ",\"icon\":");
        bstr(b, s->icon);
        bput(b, ",\"binary\":");
        bstr(b, s->binary);
        bput(b, ",\"role\":");
        bstr(b, s->role);
        bput(b, ",\"pid\":%u", s->pid);
        bidx(b, output ? "source" : "sink", s->dev);
        bput(b, ",");
        bvol(b, s->ch, s->vol);
        bput(b, ",\"mute\":%s,\"corked\":%s,\"hasVolume\":%s,\"volumeWritable\":%s}",
             s->mute ? "true" : "false", s->corked ? "true" : "false",
             s->has_volume ? "true" : "false", s->writable ? "true" : "false");
    }
}

static void bcards(Buf *b) {
    for (int i = 0; i < S.cards.n; i++) {
        const SndCard *c = &S.cards.c[i];
        bput(b, "%s{\"index\":%u,\"name\":", i ? "," : "", c->index);
        bstr(b, c->name);
        bput(b, ",\"description\":");
        bstr(b, c->desc[0] ? c->desc : c->name);
        bput(b, ",\"api\":");
        bstr(b, c->api);
        bput(b, ",\"bus\":");
        bstr(b, c->bus);
        bput(b, ",\"activeProfile\":");
        bstr(b, c->active);
        bput(b, ",\"profiles\":[");
        for (int k = 0; k < c->nprofiles; k++) {
            const SndProfile *p = &c->profiles[k];
            bput(b, "%s{\"name\":", k ? "," : "");
            bstr(b, p->name);
            bput(b, ",\"description\":");
            bstr(b, p->desc);
            bput(b, ",\"sinks\":%u,\"sources\":%u,\"priority\":%u,\"available\":%s}", p->sinks, p->sources, p->priority,
                 p->available ? "true" : "false");
        }
        bput(b, "]}");
    }
}

char *snd_state_json(void) {
    Buf b = { 0 };
    bput(&b, "{\"connected\":%s,\"server\":", S.connected ? "true" : "false");
    bstr(&b, S.server_name);
    bput(&b, ",\"version\":");
    bstr(&b, S.server_version);
    bput(&b, ",\"defaultSink\":");
    bstr(&b, S.default_sink);
    bput(&b, ",\"defaultSource\":");
    bstr(&b, S.default_source);
    bidx(&b, "meter", S.meter_source);
    bput(&b, ",\"sinks\":[");
    bdevs(&b, &S.sinks, 0);
    bput(&b, "],\"sources\":[");
    bdevs(&b, &S.sources, 1);
    bput(&b, "],\"inputs\":[");
    bstreams(&b, &S.inputs, 0);
    bput(&b, "],\"outputs\":[");
    bstreams(&b, &S.outputs, 1);
    bput(&b, "],\"cards\":[");
    bcards(&b);
    bput(&b, "]}");
    return b.d;
}

/* ---- commands ---- */

static const uint8_t *find_vol(int kind, uint32_t index, uint8_t *ch) {
    if (kind == SND_SINK || kind == SND_SOURCE) {
        const DevList *l = kind == SND_SINK ? &S.sinks : &S.sources;
        for (int i = 0; i < l->n; i++) if (l->d[i].index == index) { *ch = l->d[i].ch; return (const uint8_t *)l->d[i].vol; }
    } else {
        const StreamList *l = kind == SND_INPUT ? &S.inputs : &S.outputs;
        for (int i = 0; i < l->n; i++) if (l->s[i].index == index) { *ch = l->s[i].ch; return (const uint8_t *)l->s[i].vol; }
    }
    return NULL;
}

static uint32_t to_vol(double x) {
    if (!(x >= 0)) x = 0;                           /* NaN too */
    double v = x * PA_VOLUME_NORM + 0.5;
    if (v > PA_VOLUME_MAX) v = PA_VOLUME_MAX;
    return (uint32_t)v;
}

int snd_set_volume(int kind, uint32_t index, const double *v, int n) {
    if (!S.connected || n < 1 || n > PA_CHANNELS_MAX) return -1;
    uint8_t ch = 0;
    if (!find_vol(kind, index, &ch) || !ch) return -1;
    uint32_t vol[PA_CHANNELS_MAX];
    for (int i = 0; i < ch; i++) vol[i] = to_vol(v[i < n ? i : n - 1]);   /* fewer values: the last one is repeated */
    static const uint32_t cmd[] = { PA_CMD_SET_SINK_VOLUME, PA_CMD_SET_SOURCE_VOLUME, PA_CMD_SET_SINK_INPUT_VOLUME, PA_CMD_SET_SOURCE_OUTPUT_VOLUME };
    TsOut t;
    uint32_t tag = cmd_begin(&t, cmd[kind]);
    ts_u32(&t, index);
    if (kind == SND_SINK || kind == SND_SOURCE) ts_str(&t, NULL);
    ts_cvol(&t, ch, vol);
    return cmd_send(&t, tag, R_ACK, (int)cmd[kind], index);
}

int snd_set_level(int kind, uint32_t index, double level) {
    uint8_t ch = 0;
    const uint32_t *cur = (const uint32_t *)find_vol(kind, index, &ch);
    if (!cur || !ch) return -1;
    uint32_t top = 0;
    for (int i = 0; i < ch; i++) if (cur[i] > top) top = cur[i];
    double v[PA_CHANNELS_MAX];
    for (int i = 0; i < ch; i++) v[i] = top ? level * cur[i] / top : level;
    return snd_set_volume(kind, index, v, ch);
}

int snd_set_mute(int kind, uint32_t index, int mute) {
    if (!S.connected || kind < SND_SINK || kind > SND_OUTPUT) return -1;
    static const uint32_t cmd[] = { PA_CMD_SET_SINK_MUTE, PA_CMD_SET_SOURCE_MUTE, PA_CMD_SET_SINK_INPUT_MUTE, PA_CMD_SET_SOURCE_OUTPUT_MUTE };
    TsOut t;
    uint32_t tag = cmd_begin(&t, cmd[kind]);
    ts_u32(&t, index);
    if (kind == SND_SINK || kind == SND_SOURCE) ts_str(&t, NULL);
    ts_bool(&t, mute);
    return cmd_send(&t, tag, R_ACK, (int)cmd[kind], index);
}

static int dev_exists(int kind, const char *name, uint32_t index) {
    const DevList *l = kind == SND_SINK ? &S.sinks : &S.sources;
    for (int i = 0; i < l->n; i++)
        if (name ? !strcmp(l->d[i].name, name) : l->d[i].index == index) return 1;
    return 0;
}

int snd_set_default(int kind, const char *name) {
    if (!S.connected || (kind != SND_SINK && kind != SND_SOURCE) || !dev_exists(kind, name, 0)) return -1;
    uint32_t c = kind == SND_SINK ? PA_CMD_SET_DEFAULT_SINK : PA_CMD_SET_DEFAULT_SOURCE;
    TsOut t;
    uint32_t tag = cmd_begin(&t, c);
    ts_str(&t, name);
    return cmd_send(&t, tag, R_ACK, (int)c, 0);
}

int snd_set_port(int kind, uint32_t index, const char *port) {
    if (!S.connected || (kind != SND_SINK && kind != SND_SOURCE)) return -1;
    const DevList *l = kind == SND_SINK ? &S.sinks : &S.sources;
    int ok = 0;
    for (int i = 0; i < l->n; i++)
        if (l->d[i].index == index)
            for (int k = 0; k < l->d[i].nports; k++) if (!strcmp(l->d[i].ports[k].name, port)) ok = 1;
    if (!ok) return -1;
    uint32_t c = kind == SND_SINK ? PA_CMD_SET_SINK_PORT : PA_CMD_SET_SOURCE_PORT;
    TsOut t;
    uint32_t tag = cmd_begin(&t, c);
    ts_u32(&t, index);
    ts_str(&t, NULL);
    ts_str(&t, port);
    return cmd_send(&t, tag, R_ACK, (int)c, index);
}

/* A Bluetooth card's profile: only a card in the state and one of its profiles. */
int snd_set_profile(uint32_t card, const char *profile) {
    if (!S.connected || !profile) return -1;
    const SndCard *c = NULL;
    for (int i = 0; i < S.cards.n; i++) if (S.cards.c[i].index == card) c = &S.cards.c[i];
    if (!c || !snd_card_profile(c, profile)) return -1;
    TsOut t;
    uint32_t tag = cmd_begin(&t, PA_CMD_SET_CARD_PROFILE);
    ts_u32(&t, card);
    ts_str(&t, NULL);
    ts_str(&t, profile);
    return cmd_send(&t, tag, R_ACK, PA_CMD_SET_CARD_PROFILE, card);
}

int snd_move(int kind, uint32_t index, uint32_t device) {
    if (!S.connected || (kind != SND_INPUT && kind != SND_OUTPUT)) return -1;
    if (!dev_exists(kind == SND_INPUT ? SND_SINK : SND_SOURCE, NULL, device)) return -1;
    uint32_t c = kind == SND_INPUT ? PA_CMD_MOVE_SINK_INPUT : PA_CMD_MOVE_SOURCE_OUTPUT;
    TsOut t;
    uint32_t tag = cmd_begin(&t, c);
    ts_u32(&t, index);
    ts_u32(&t, device);
    ts_str(&t, NULL);
    return cmd_send(&t, tag, R_ACK, (int)c, index);
}

/* Meter: a plain 1-channel float recording stream at 48 kHz, read in blocks of 2048 samples (~23
 * levels a second); the level is the largest sample of each block (on_packet). Until 0.8.1 it was
 * pavucontrol's trick, a 25 Hz stream with PEAK_DETECT and resample.peaks: in PipeWire that is a
 * node at rate 25 with a quantum of 1, and on a VM it was seen next to PipeWire using ~36 % CPU
 * while Settings → Sound was open. A normal-rate stream is what every recorder asks for. */
int snd_meter(uint32_t source) {
    if (S.meter_channel != PA_INVALID_INDEX && S.fd >= 0) {
        TsOut t;
        uint32_t tag = cmd_begin(&t, PA_CMD_DELETE_RECORD_STREAM);
        ts_u32(&t, S.meter_channel);
        cmd_send(&t, tag, R_ACK, PA_CMD_DELETE_RECORD_STREAM, 0);
        S.meter_channel = PA_INVALID_INDEX;
    }
    S.meter_source = source;
    S.peak = 0;
    if (source == PA_INVALID_INDEX) return 0;
    if (!S.connected) return 0;                     /* created on connect */
    if (!dev_exists(SND_SOURCE, NULL, source)) { S.meter_source = PA_INVALID_INDEX; return -1; }
    TsOut t;
    uint32_t tag = cmd_begin(&t, PA_CMD_CREATE_RECORD_STREAM);
    uint8_t mono = 0;                               /* PA_CHANNEL_POSITION_MONO */
    ts_ss(&t, PA_SAMPLE_FLOAT32LE, 1, 48000);
    ts_cmap(&t, 1, &mono);
    ts_u32(&t, source);
    ts_str(&t, NULL);
    ts_u32(&t, 0xffffffffu);                        /* maxlength: the server's */
    ts_bool(&t, 0);                                 /* corked */
    ts_u32(&t, 2048 * 4);                           /* fragsize: 2048 samples, ~43 ms */
    ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0);   /* no_remap … fix_channels */
    ts_bool(&t, 1);                                 /* no_move */
    ts_bool(&t, 0);                                 /* variable_rate */
    ts_bool(&t, 0);                                 /* peak_detect: no, computed here */
    ts_bool(&t, 1);                                 /* adjust_latency */
    const char *kv2[] = { "media.name", "LordOS: level meter", "application.id", "org.lordos.Server" };
    ts_proplist(&t, kv2, 2);
    ts_u32(&t, PA_INVALID_INDEX);                   /* direct_on_input */
    ts_bool(&t, 0);                                 /* early_requests */
    ts_bool(&t, 1);                                 /* dont_inhibit_auto_suspend */
    ts_bool(&t, 0);                                 /* fail_on_suspend */
    ts_u8(&t, 1);                                   /* one format: PCM */
    ts_format_pcm(&t);
    uint32_t vol = PA_VOLUME_NORM;
    ts_cvol(&t, 1, &vol);
    ts_bool(&t, 0);                                 /* muted */
    ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0); ts_bool(&t, 0);   /* volume_set, muted_set, relative, passthrough */
    S.meter_pending = 1;
    return cmd_send(&t, tag, R_RECORD, PA_CMD_CREATE_RECORD_STREAM, source);
}

/* ---- system sounds ---- */

static const char *const sample_names[3] = { "lordos-pop", "lordos-notify", "lordos-error" };

#define SR 44100

/* Synthesizes the sound: sines with an envelope (short attack, exponential decay). s16le mono. */
static int16_t *synth(int which, uint32_t *bytes) {
    static const struct { double f1, f2, dur, split; } tone[3] = {
        { 880, 0, 0.07, 0 },                        /* pop: a short tap */
        { 1046.5, 1568, 0.32, 0.12 },               /* notify: C6 → G6 */
        { 311, 233, 0.30, 0.13 },                   /* error: two low notes */
    };
    int n = (int)(tone[which].dur * SR);
    int16_t *s = malloc((size_t)n * 2);
    if (!s) return NULL;
    for (int i = 0; i < n; i++) {
        double tt = (double)i / SR, f = tone[which].f1, t0 = 0;
        if (tone[which].f2 && tt >= tone[which].split) { f = tone[which].f2; t0 = tone[which].split; }
        double local = tt - t0, env = (1 - exp(-local * 900)) * exp(-local * (which ? 14 : 45));
        double v = sin(2 * M_PI * f * local) * 0.8 + sin(4 * M_PI * f * local) * 0.12;
        s[i] = (int16_t)(v * env * 0.5 * 32767);
    }
    *bytes = (uint32_t)n * 2;
    return s;
}

static void upload_data(int which, uint32_t channel) {
    uint32_t bytes = 0;
    int16_t *s = synth(which, &bytes);
    if (!s) return;
    for (uint32_t off = 0; off < bytes; off += 65536) {
        uint32_t n = bytes - off < 65536 ? bytes - off : 65536;
        size_t total;
        uint8_t *pkt = ts_data_packet(channel, (const uint8_t *)s + off, n, &total);
        if (pkt) { queue_bytes(pkt, total); free(pkt); }
    }
    free(s);
    TsOut t;
    uint32_t tag = cmd_begin(&t, PA_CMD_FINISH_UPLOAD_STREAM);
    ts_u32(&t, channel);
    cmd_send(&t, tag, R_ACK, PA_CMD_FINISH_UPLOAD_STREAM, 0);
    S.samples_uploaded |= 1 << which;
    S.uploading &= ~(1 << which);
    if (S.play_after & 1 << which) { S.play_after &= ~(1 << which); play_sample(which); }
}

/* goes after the upload on the same socket: the server already has it when this arrives */
static int play_sample(int which) {
    TsOut t;
    uint32_t tag = cmd_begin(&t, PA_CMD_PLAY_SAMPLE);
    ts_u32(&t, PA_INVALID_INDEX);
    ts_str(&t, NULL);                               /* the default output */
    ts_u32(&t, PA_VOLUME_NORM);
    ts_str(&t, sample_names[which]);
    const char *kv[] = { "media.role", "event", "application.name", "LordOS", "application.id", "org.lordos.Server", "event.id", sample_names[which] };
    ts_proplist(&t, kv, 4);
    return cmd_send(&t, tag, R_ACK, PA_CMD_PLAY_SAMPLE, 0);
}

int snd_play(const char *name) {
    int which = -1;
    if (!strcmp(name, "pop")) which = 0;
    else if (!strcmp(name, "notify")) which = 1;
    else if (!strcmp(name, "error")) which = 2;
    if (which < 0 || !S.connected) return -1;
    if (S.samples_uploaded & 1 << which) return play_sample(which);
    S.play_after |= 1 << which;
    if (S.uploading & 1 << which) return 0;
    uint32_t bytes = 0;
    free(synth(which, &bytes));
    uint8_t mono = 0;
    TsOut t;
    uint32_t tag = cmd_begin(&t, PA_CMD_CREATE_UPLOAD_STREAM);
    ts_str(&t, sample_names[which]);
    ts_ss(&t, PA_SAMPLE_S16LE, 1, SR);
    ts_cmap(&t, 1, &mono);
    ts_u32(&t, bytes);
    const char *kv[] = { "media.role", "event", "event.id", sample_names[which] };
    ts_proplist(&t, kv, 2);
    if (cmd_send(&t, tag, R_UPLOAD, PA_CMD_CREATE_UPLOAD_STREAM, (uint32_t)which)) return -1;
    S.uploading |= 1 << which;
    return 0;
}
