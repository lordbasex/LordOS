/* sharewire.c — see sharewire.h. */
#include "sharewire.h"
#include "proto.h"

#include <string.h>

const char *sw_reason_name(int r) {
    static const char *const n[SW_REASONS] = {
        "ok", "no_identity", "denied", "no_person", "no_indicator", "busy", "locked", "away", "remote_desktop",
        "restricted", "revoked", "stopped", "bad_request", "ended_by_app",
    };
    return r >= 0 && r < SW_REASONS ? n[r] : "unknown";
}

int sw_next(const uint8_t *buf, size_t len, SwFrame *f) {
    if (len < 4) return 0;
    uint32_t l;
    memcpy(&l, buf, 4);
    if (l < 1 || l > SW_MAX_MSG + 1) return -1;
    if (len < 4 + (size_t)l) return 0;
    f->type = buf[4];
    f->data = buf + 5;
    f->n = l - 1;
    f->used = 4 + (size_t)l;
    return 1;
}

int sw_utf8_next(const char *s, size_t n, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)s;
    if (!n) return 0;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    int l = u[0] >= 0xf0 ? 4 : u[0] >= 0xe0 ? 3 : 2;
    if ((size_t)l > n) return 0;
    uint32_t c = u[0] & (0xff >> (l + 1));
    for (int i = 1; i < l; i++) c = (c << 6) | (u[i] & 0x3f);
    *cp = c;
    return l;
}

int sw_utf8_valid(const char *s, size_t n) {
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n;) {
        unsigned char c = u[i];
        if (!c) return 0;
        if (c < 0x80) { i++; continue; }
        int l = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!l || i + (size_t)l > n) return 0;
        for (int k = 1; k < l; k++) if ((u[i + (size_t)k] & 0xc0) != 0x80) return 0;
        if (c == 0xe0 && u[i + 1] < 0xa0) return 0;       /* overlong */
        if (c == 0xed && u[i + 1] > 0x9f) return 0;       /* surrogates */
        if (c == 0xf0 && u[i + 1] < 0x90) return 0;       /* overlong */
        if (c == 0xf4 && u[i + 1] > 0x8f) return 0;       /* above U+10FFFF */
        i += (size_t)l;
    }
    return 1;
}

static unsigned clamp_kbps(uint32_t k) { return k < 300 ? 300 : k > 20000 ? 20000 : k; }

int sw_parse(const SwFrame *f, SwMsg *m) {
    const uint8_t *d = f->data;
    uint32_t n = f->n;
    memset(m, 0, sizeof *m);
    m->type = f->type;
    switch (f->type) {
    case SHARE_START: {
        if (n != 8 || !d[0] || (d[0] & ~SW_F_ALL) || !(d[0] & SW_F_VIDEO) || d[1] < 1 || d[1] > 2) return -1;
        uint16_t hz;
        uint32_t k;
        memcpy(&hz, d + 2, 2);
        memcpy(&k, d + 4, 4);
        if (hz < 24 || hz > 240 || k < 300 || k > 20000) return -1;
        m->flags = d[0];
        m->codec = d[1];
        m->hz = hz;
        m->kbps = k;
        return 0;
    }
    case SHARE_QUERY: case SHARE_STOP: case SHARE_LOCK: case RP_KEYFRAME:
        return n ? -1 : 0;
    case SHARE_AWAKE: case SHARE_HEAR:
        if (n != 1 || d[0] > 1) return -1;
        m->on = d[0];
        return 0;
    case SHARE_TEXT:
        if (!n || n > SW_TEXT_MAX || !sw_utf8_valid((const char *)d, n)) return -1;
        m->text = (const char *)d;
        m->tlen = n;
        return 0;
    case RP_SETCLIP:
        if (n > SW_MAX_MSG || !sw_utf8_valid((const char *)d, n)) return -1;
        m->text = (const char *)d;
        m->tlen = n;
        return 0;
    case RP_RATE: {
        uint32_t k;
        if (n != 4) return -1;
        memcpy(&k, d, 4);
        m->kbps = clamp_kbps(k);
        return 0;
    }
    case RP_RESIZE: {
        uint16_t s[2];
        if (n != 4) return -1;
        memcpy(s, d, 4);
        if (s[0] < 640 || s[1] < 480 || s[0] > 3840 || s[1] > 2400) return -1;
        m->x = s[0];
        m->y = s[1];
        return 0;
    }
    case RP_PTR: {
        int16_t p[2];
        if (n != 4) return -1;
        memcpy(p, d, 4);
        if (p[0] < 0 || p[1] < 0) return -1;
        m->x = p[0];
        m->y = p[1];
        return 0;
    }
    case RP_BTN:
        if (n != 2 || d[0] > 2 || d[1] > 1) return -1;
        m->button = d[0];
        m->down = d[1];
        return 0;
    case RP_WHEEL:
        if (n != 1) return -1;
        m->steps = (signed char)d[0];
        return 0;
    case RP_SCROLL: {
        int32_t v[2];
        if (n != 9 || d[0] < 1 || d[0] > 3) return -1;
        memcpy(v, d + 1, 8);
        if (v[0] < -(1 << 24) || v[0] > (1 << 24) || v[1] < -(1 << 24) || v[1] > (1 << 24)) return -1;
        m->phase = d[0];
        m->dx = v[0];
        m->dy = v[1];
        return 0;
    }
    case RP_KEY: {
        uint16_t c;
        if (n != 3) return -1;
        memcpy(&c, d, 2);
        if (!c || c >= 0x2ff || d[2] > 2) return -1;
        m->code = c;
        m->value = d[2];
        return 0;
    }
    default:
        return -1;                      /* everything else, RP_DISPLAY and the other seat messages included */
    }
}

int sw_bucket_take(SwBucket *b, double now, double rate, double burst) {
    if (b->at <= 0) { b->at = now; b->tokens = burst; }
    double t = b->tokens + (now - b->at) * rate / 1000.0;
    b->tokens = t > burst ? burst : t;
    b->at = now;
    if (b->tokens < 1) return 0;
    b->tokens -= 1;
    return 1;
}

void sw_key_init(SwKeyLimit *k, double now) {
    memset(k, 0, sizeof *k);
    k->last = now - SW_KEY_MIN_MS;
    k->tokens = SW_KEY_BURST;
    k->at = now;
}
void sw_key_request(SwKeyLimit *k) { k->pending = 1; }
void sw_key_changed(SwKeyLimit *k) { k->changed = 1; }

static void refill(SwKeyLimit *k, double now) {
    if (now <= k->at) return;
    k->tokens += (now - k->at) / SW_KEY_STILL_MS;
    if (k->tokens > SW_KEY_BURST) k->tokens = SW_KEY_BURST;
    k->at = now;
}

int sw_key_due(SwKeyLimit *k, double now) {
    if (!k->pending || now - k->last < SW_KEY_MIN_MS) return 0;
    refill(k, now);
    if (!k->changed) {
        if (k->tokens < 1) return 0;
        k->tokens -= 1;
    }
    k->pending = 0;
    k->changed = 0;
    k->last = now;
    return 1;
}

double sw_key_wait(const SwKeyLimit *k, double now) {
    if (!k->pending) return -1;
    double w = k->last + SW_KEY_MIN_MS - now;
    if (!k->changed) {
        SwKeyLimit c = *k;
        refill(&c, now);
        if (c.tokens < 1) {
            double t = (1 - c.tokens) * SW_KEY_STILL_MS;
            if (t > w) w = t;
        }
    }
    return w < 0 ? 0 : w;
}
