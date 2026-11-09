/* pulse.c — tagstruct and packets of the PulseAudio native protocol (see pulse.h).
 *
 * A "tagstruct" is a sequence of values, each one preceded by a byte saying its type
 * ('L' u32, 't' string, 'v' per-channel volume, 'P' property list...). Integers are
 * big endian. Each packet carries a 20-byte descriptor: length, channel (-1 = control),
 * 64-bit offset and flags. */
#include "pulse.h"

#include <stdlib.h>
#include <string.h>

enum {
    T_STRING = 't', T_STRING_NULL = 'N', T_U32 = 'L', T_U8 = 'B', T_U64 = 'R', T_S64 = 'r',
    T_SAMPLE_SPEC = 'a', T_ARBITRARY = 'x', T_TRUE = '1', T_FALSE = '0', T_TIMEVAL = 'T',
    T_USEC = 'U', T_CHANNEL_MAP = 'm', T_CVOLUME = 'v', T_PROPLIST = 'P', T_VOLUME = 'V',
    T_FORMAT_INFO = 'f',
};

static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ---- writing ---- */

static uint8_t *grow(TsOut *t, size_t n) {
    if (t->err) return NULL;
    if (t->len + n > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 256;
        while (cap < t->len + n) cap *= 2;
        uint8_t *d = realloc(t->d, cap);
        if (!d) { t->err = 1; return NULL; }
        t->d = d;
        t->cap = cap;
    }
    uint8_t *p = t->d + t->len;
    t->len += n;
    return p;
}

void ts_init(TsOut *t) {
    memset(t, 0, sizeof *t);
    uint8_t *p = grow(t, PA_DESCRIPTOR_SIZE);
    if (p) memset(p, 0, PA_DESCRIPTOR_SIZE);
}

void ts_free(TsOut *t) { free(t->d); memset(t, 0, sizeof *t); }

void ts_u32(TsOut *t, uint32_t v) {
    uint8_t *p = grow(t, 5);
    if (p) { p[0] = T_U32; put_be32(p + 1, v); }
}

void ts_u8(TsOut *t, uint8_t v) {
    uint8_t *p = grow(t, 2);
    if (p) { p[0] = T_U8; p[1] = v; }
}

void ts_u64(TsOut *t, uint64_t v) {
    uint8_t *p = grow(t, 9);
    if (p) { p[0] = T_USEC; put_be32(p + 1, (uint32_t)(v >> 32)); put_be32(p + 5, (uint32_t)v); }
}

/* 'r': signed 64 bits (a card port's latency offset; tests build card replies with it). */
void ts_s64(TsOut *t, int64_t v) {
    uint8_t *p = grow(t, 9);
    if (p) { p[0] = T_S64; put_be32(p + 1, (uint32_t)((uint64_t)v >> 32)); put_be32(p + 5, (uint32_t)v); }
}

/* 'T': timeval (seconds and microseconds), requested by GET_PLAYBACK_LATENCY (lord-media). */
void ts_timeval(TsOut *t, uint32_t sec, uint32_t usec) {
    uint8_t *p = grow(t, 9);
    if (p) { p[0] = T_TIMEVAL; put_be32(p + 1, sec); put_be32(p + 5, usec); }
}

void ts_bool(TsOut *t, int v) {
    uint8_t *p = grow(t, 1);
    if (p) p[0] = v ? T_TRUE : T_FALSE;
}

void ts_str(TsOut *t, const char *s) {
    if (!s) { uint8_t *p = grow(t, 1); if (p) p[0] = T_STRING_NULL; return; }
    size_t n = strlen(s);
    uint8_t *p = grow(t, n + 2);
    if (p) { p[0] = T_STRING; memcpy(p + 1, s, n + 1); }
}

void ts_arb(TsOut *t, const void *data, uint32_t n) {
    uint8_t *p = grow(t, 5 + (size_t)n);
    if (p) { p[0] = T_ARBITRARY; put_be32(p + 1, n); if (n) memcpy(p + 5, data, n); }
}

void ts_ss(TsOut *t, uint8_t format, uint8_t channels, uint32_t rate) {
    uint8_t *p = grow(t, 7);
    if (p) { p[0] = T_SAMPLE_SPEC; p[1] = format; p[2] = channels; put_be32(p + 3, rate); }
}

void ts_cmap(TsOut *t, uint8_t n, const uint8_t *pos) {
    uint8_t *p = grow(t, 2 + (size_t)n);
    if (p) { p[0] = T_CHANNEL_MAP; p[1] = n; memcpy(p + 2, pos, n); }
}

void ts_cvol(TsOut *t, uint8_t n, const uint32_t *v) {
    uint8_t *p = grow(t, 2 + 4 * (size_t)n);
    if (!p) return;
    p[0] = T_CVOLUME;
    p[1] = n;
    for (uint8_t i = 0; i < n; i++) put_be32(p + 2 + 4 * i, v[i]);
}

void ts_proplist(TsOut *t, const char *const *kv, int pairs) {
    uint8_t *p = grow(t, 1);
    if (p) p[0] = T_PROPLIST;
    for (int i = 0; i < pairs; i++) {
        uint32_t n = (uint32_t)strlen(kv[2 * i + 1]) + 1;
        ts_str(t, kv[2 * i]);
        ts_u32(t, n);
        ts_arb(t, kv[2 * i + 1], n);
    }
    ts_str(t, NULL);
}

void ts_format_pcm(TsOut *t) {
    uint8_t *p = grow(t, 1);
    if (p) p[0] = T_FORMAT_INFO;
    ts_u8(t, 1);                        /* PA_ENCODING_PCM */
    ts_proplist(t, NULL, 0);
}

size_t ts_finish(TsOut *t) {
    if (t->err || t->len < PA_DESCRIPTOR_SIZE) return 0;
    put_be32(t->d, (uint32_t)(t->len - PA_DESCRIPTOR_SIZE));
    put_be32(t->d + 4, 0xffffffffu);
    memset(t->d + 8, 0, 12);
    return t->len;
}

uint8_t *ts_data_packet(uint32_t channel, const void *data, uint32_t n, size_t *total) {
    uint8_t *p = malloc(PA_DESCRIPTOR_SIZE + (size_t)n);
    if (!p) return NULL;
    put_be32(p, n);
    put_be32(p + 4, channel);
    memset(p + 8, 0, 12);
    memcpy(p + PA_DESCRIPTOR_SIZE, data, n);
    *total = PA_DESCRIPTOR_SIZE + (size_t)n;
    return p;
}

void pa_descriptor_read(const uint8_t *d, uint32_t *len, uint32_t *channel, uint64_t *offset, uint32_t *flags) {
    *len = get_be32(d);
    *channel = get_be32(d + 4);
    *offset = (uint64_t)get_be32(d + 8) << 32 | get_be32(d + 12);
    *flags = get_be32(d + 16);
}

/* ---- reading ---- */

void ts_in(TsIn *t, const uint8_t *d, size_t len) { t->d = d; t->len = len; t->pos = 0; t->err = 0; }
int ts_eof(const TsIn *t) { return t->pos >= t->len; }

static const uint8_t *take(TsIn *t, size_t n) {
    if (t->err || t->len - t->pos < n) { t->err = 1; return NULL; }
    const uint8_t *p = t->d + t->pos;
    t->pos += n;
    return p;
}

static int expect(TsIn *t, uint8_t tag) {
    const uint8_t *p = take(t, 1);
    if (!p) return 0;
    if (*p != tag) { t->err = 1; return 0; }
    return 1;
}

uint32_t ts_get_u32(TsIn *t) {
    if (!expect(t, T_U32)) return 0;
    const uint8_t *p = take(t, 4);
    return p ? get_be32(p) : 0;
}

uint8_t ts_get_u8(TsIn *t) {
    if (!expect(t, T_U8)) return 0;
    const uint8_t *p = take(t, 1);
    return p ? *p : 0;
}

uint64_t ts_get_u64(TsIn *t) {
    const uint8_t *tag = take(t, 1);
    if (!tag || (*tag != T_USEC && *tag != T_U64 && *tag != T_S64)) { t->err = 1; return 0; }
    const uint8_t *p = take(t, 8);
    return p ? (uint64_t)get_be32(p) << 32 | get_be32(p + 4) : 0;
}

int ts_get_bool(TsIn *t) {
    const uint8_t *p = take(t, 1);
    if (!p) return 0;
    if (*p == T_TRUE) return 1;
    if (*p != T_FALSE) t->err = 1;
    return 0;
}

const char *ts_get_str(TsIn *t) {
    const uint8_t *p = take(t, 1);
    if (!p) return NULL;
    if (*p == T_STRING_NULL) return NULL;
    if (*p != T_STRING) { t->err = 1; return NULL; }
    const uint8_t *s = t->d + t->pos, *z = memchr(s, 0, t->len - t->pos);
    if (!z) { t->err = 1; return NULL; }
    t->pos += (size_t)(z - s) + 1;
    return (const char *)s;
}

const uint8_t *ts_get_arb(TsIn *t, uint32_t *n) {
    *n = 0;
    if (!expect(t, T_ARBITRARY)) return NULL;
    const uint8_t *p = take(t, 4);
    if (!p) return NULL;
    uint32_t len = get_be32(p);
    const uint8_t *d = take(t, len);
    if (d) *n = len;
    return d;
}

void ts_get_ss(TsIn *t, uint8_t *format, uint8_t *channels, uint32_t *rate) {
    *format = *channels = 0;
    *rate = 0;
    if (!expect(t, T_SAMPLE_SPEC)) return;
    const uint8_t *p = take(t, 6);
    if (!p) return;
    *format = p[0];
    *channels = p[1];
    *rate = get_be32(p + 2);
}

uint8_t ts_get_cmap(TsIn *t, uint8_t *pos) {
    if (!expect(t, T_CHANNEL_MAP)) return 0;
    const uint8_t *p = take(t, 1);
    if (!p || *p > PA_CHANNELS_MAX) { t->err = 1; return 0; }
    uint8_t n = *p;
    const uint8_t *d = take(t, n);
    if (!d) return 0;
    memcpy(pos, d, n);
    return n;
}

uint8_t ts_get_cvol(TsIn *t, uint32_t *v) {
    if (!expect(t, T_CVOLUME)) return 0;
    const uint8_t *p = take(t, 1);
    if (!p || *p > PA_CHANNELS_MAX) { t->err = 1; return 0; }
    uint8_t n = *p;
    const uint8_t *d = take(t, 4 * (size_t)n);
    if (!d) return 0;
    for (uint8_t i = 0; i < n; i++) v[i] = get_be32(d + 4 * i);
    return n;
}

uint32_t ts_get_vol(TsIn *t) {
    if (!expect(t, T_VOLUME)) return 0;
    const uint8_t *p = take(t, 4);
    return p ? get_be32(p) : 0;
}

void ts_get_proplist(TsIn *t, TsPropFn fn, void *ud) {
    if (!expect(t, T_PROPLIST)) return;
    for (int guard = 0; guard < 4096 && !t->err; guard++) {
        const char *key = ts_get_str(t);
        if (!key || t->err) return;                 /* 'N' ends the list */
        uint32_t n = ts_get_u32(t), got = 0;
        const uint8_t *val = ts_get_arb(t, &got);
        if (t->err || got != n) { t->err = 1; return; }
        if (fn) {
            /* strings come with their 0; if not, it's copied to terminate it */
            char tmp[512];
            if (got && val[got - 1] == 0) fn(ud, key, (const char *)val, got - 1);
            else if (got < sizeof tmp) { memcpy(tmp, val, got); tmp[got] = 0; fn(ud, key, tmp, got); }
        }
    }
    t->err = 1;
}

void ts_skip_format(TsIn *t) {
    if (!expect(t, T_FORMAT_INFO)) return;
    ts_get_u8(t);
    ts_get_proplist(t, NULL, NULL);
}

void ts_skip(TsIn *t) {
    if (t->err || ts_eof(t)) { t->err = 1; return; }
    uint8_t tag = t->d[t->pos];
    uint32_t n;
    uint8_t buf[PA_CHANNELS_MAX];
    uint32_t vol[PA_CHANNELS_MAX];
    switch (tag) {
    case T_STRING: case T_STRING_NULL: ts_get_str(t); break;
    case T_U32: ts_get_u32(t); break;
    case T_U8: ts_get_u8(t); break;
    case T_U64: case T_S64: case T_USEC: ts_get_u64(t); break;
    case T_TRUE: case T_FALSE: ts_get_bool(t); break;
    case T_SAMPLE_SPEC: { uint8_t f, c; uint32_t r; ts_get_ss(t, &f, &c, &r); break; }
    case T_ARBITRARY: ts_get_arb(t, &n); break;
    case T_TIMEVAL: take(t, 9); break;
    case T_CHANNEL_MAP: ts_get_cmap(t, buf); break;
    case T_CVOLUME: ts_get_cvol(t, vol); break;
    case T_PROPLIST: ts_get_proplist(t, NULL, NULL); break;
    case T_VOLUME: ts_get_vol(t); break;
    case T_FORMAT_INFO: ts_skip_format(t); break;
    default: t->err = 1;
    }
}
