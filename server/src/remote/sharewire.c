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
    case SHARE_DISPLAY:
        if (n != 4) return -1;
        memcpy(&m->display, d, 4);
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

int sw_held_down(const SwHeld *h, int code) {
    return code > 0 && code < SW_KEY_CODES && (h->bit[code / 8] >> (code % 8)) & 1;
}

int sw_held_key(SwHeld *h, int code, int value) {
    if (code <= 0 || code >= SW_KEY_CODES || value < 0 || value > 2) return -1;
    int on = sw_held_down(h, code);
    if (value == 1 && on) return 2;
    if (!on && value != 1) return -1;
    if (value == 1) h->bit[code / 8] |= (uint8_t)(1u << (code % 8));
    else if (value == 0) h->bit[code / 8] &= (uint8_t)~(1u << (code % 8));
    return value;
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

/* A display's name as it goes on the wire: valid UTF-8 without control characters, at most SW_DISPLAY_NAME bytes. */
static size_t clean_name(const char *s, char *out) {
    size_t o = 0, n = strnlen(s, 256);
    for (size_t i = 0; i < n;) {
        const unsigned char c = (const unsigned char)s[i];
        int l = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!l || i + (size_t)l > n || !sw_utf8_valid(s + i, (size_t)l)) { i++; continue; }
        if (l == 1 && (c < 0x20 || c == 0x7f)) { i++; continue; }
        if (o + (size_t)l > SW_DISPLAY_NAME) break;
        memcpy(out + o, s + i, (size_t)l);
        o += (size_t)l;
        i += (size_t)l;
    }
    return o;
}

static uint16_t u16_of(int v) { return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); }

size_t sw_displays_encode(uint8_t *out, size_t cap, uint32_t current, const SwDisplay *d, int n) {
    if (n < 0 || n > SW_DISPLAYS_MAX + 1 || cap < 5) return 0;
    size_t k = 0;
    memcpy(out, &current, 4);
    out[4] = (uint8_t)n;
    k = 5;
    for (int i = 0; i < n; i++) {
        char name[SW_DISPLAY_NAME];
        size_t nl = clean_name(d[i].name, name);
        if (k + 22 + nl > cap) return 0;
        int32_t xy[2] = { d[i].x, d[i].y };
        uint16_t sz[4] = { u16_of(d[i].w), u16_of(d[i].h), u16_of(d[i].pw), u16_of(d[i].ph) };
        memcpy(out + k, &d[i].id, 4);
        memcpy(out + k + 4, xy, 8);
        memcpy(out + k + 12, sz, 8);
        out[k + 20] = (uint8_t)(d[i].flags & (SW_DF_MAIN | SW_DF_ALL));
        out[k + 21] = (uint8_t)nl;
        memcpy(out + k + 22, name, nl);
        k += 22 + nl;
    }
    return k;
}

int sw_display_known(uint32_t id, const SwDisplay *d, int n) {
    if (!id) return 1;
    for (int i = 0; i < n; i++) if (d[i].id == id) return 1;
    return 0;
}

int sw_dead_split(uint32_t cp, char base[8], const char **mark) {
    /* the dead keys' marks as kb_label names them: acute, grave, circumflex, tilde, diaeresis, cedilla, ring */
    static const char *const marks[] = { "\xc2\xb4", "`", "^", "~", "\xc2\xa8", "\xc2\xb8", "\xc2\xb0" };
    /* each letter, its base and its mark */
    static const struct { uint32_t cp; char base; uint8_t mark; } t[] = {
        { 0xe1, 'a', 0 }, { 0xe9, 'e', 0 }, { 0xed, 'i', 0 }, { 0xf3, 'o', 0 }, { 0xfa, 'u', 0 }, { 0xfd, 'y', 0 },
        { 0xc1, 'A', 0 }, { 0xc9, 'E', 0 }, { 0xcd, 'I', 0 }, { 0xd3, 'O', 0 }, { 0xda, 'U', 0 }, { 0xdd, 'Y', 0 },
        { 0x107, 'c', 0 }, { 0x106, 'C', 0 }, { 0x144, 'n', 0 }, { 0x143, 'N', 0 }, { 0x15b, 's', 0 },
        { 0x15a, 'S', 0 }, { 0x17a, 'z', 0 }, { 0x179, 'Z', 0 }, { 0x13a, 'l', 0 }, { 0x139, 'L', 0 },
        { 0x155, 'r', 0 }, { 0x154, 'R', 0 }, { 0xe0, 'a', 1 }, { 0xe8, 'e', 1 }, { 0xec, 'i', 1 }, { 0xf2, 'o', 1 },
        { 0xf9, 'u', 1 }, { 0xc0, 'A', 1 }, { 0xc8, 'E', 1 }, { 0xcc, 'I', 1 }, { 0xd2, 'O', 1 }, { 0xd9, 'U', 1 },
        { 0xe2, 'a', 2 }, { 0xea, 'e', 2 }, { 0xee, 'i', 2 }, { 0xf4, 'o', 2 }, { 0xfb, 'u', 2 }, { 0xc2, 'A', 2 },
        { 0xca, 'E', 2 }, { 0xce, 'I', 2 }, { 0xd4, 'O', 2 }, { 0xdb, 'U', 2 }, { 0xe3, 'a', 3 }, { 0xf5, 'o', 3 },
        { 0xf1, 'n', 3 }, { 0xc3, 'A', 3 }, { 0xd5, 'O', 3 }, { 0xd1, 'N', 3 }, { 0xe4, 'a', 4 }, { 0xeb, 'e', 4 },
        { 0xef, 'i', 4 }, { 0xf6, 'o', 4 }, { 0xfc, 'u', 4 }, { 0xff, 'y', 4 }, { 0xc4, 'A', 4 }, { 0xcb, 'E', 4 },
        { 0xcf, 'I', 4 }, { 0xd6, 'O', 4 }, { 0xdc, 'U', 4 }, { 0x178, 'Y', 4 }, { 0xe7, 'c', 5 }, { 0xc7, 'C', 5 },
        { 0xe5, 'a', 6 }, { 0xc5, 'A', 6 },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].cp == cp) {
            base[0] = t[i].base;
            base[1] = 0;
            *mark = marks[t[i].mark];
            return 1;
        }
    return 0;
}
