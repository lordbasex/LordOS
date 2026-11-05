/* vmagent.c — org.lordos.agent frames, UTF-8 and base64 (see vmagent.h). */
#include "vmagent.h"

#include <string.h>

int ag_frame_next(const uint8_t *b, size_t n, AgFrame *f) {
    if (n < AG_HEAD) return 0;
    uint32_t len = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    if (len > AG_FRAME_MAX) return -1;
    if (n < AG_HEAD + (size_t)len) return 0;
    f->kind = (uint16_t)(b[4] | b[5] << 8);
    f->chan = (uint16_t)(b[6] | b[7] << 8);
    f->data = b + AG_HEAD;
    f->len = len;
    return AG_HEAD + (int)len;
}

void ag_frame_head(uint8_t o[AG_HEAD], uint16_t kind, uint16_t chan, uint32_t len) {
    o[0] = (uint8_t)len; o[1] = (uint8_t)(len >> 8); o[2] = (uint8_t)(len >> 16); o[3] = (uint8_t)(len >> 24);
    o[4] = (uint8_t)kind; o[5] = (uint8_t)(kind >> 8); o[6] = (uint8_t)chan; o[7] = (uint8_t)(chan >> 8);
}

int ag_utf8_ok(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t c = p[i];
        if (!c) return 0;
        int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (k < 0 || (k == 1 && c < 0xc2) || (k == 3 && c > 0xf4) || i + (size_t)k >= n) return 0;
        uint32_t cp = k == 0 ? c : k == 1 ? c & 0x1f : k == 2 ? c & 0x0f : c & 0x07;
        for (int j = 1; j <= k; j++) {
            if (i + (size_t)j >= n || (p[i + j] & 0xc0) != 0x80) return 0;
            cp = cp << 6 | (p[i + j] & 0x3f);
        }
        if ((k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return 0;
        i += (size_t)k + 1;
    }
    return 1;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

long ag_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    size_t need = (n + 2) / 3 * 4;
    if (need + 1 > cap) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return (long)o;
}

long ag_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap) {
    uint32_t v = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '=' ) break;
        if (c == '\n' || c == '\r') continue;
        const char *p = c ? strchr(B64, c) : NULL;
        if (!p) return -1;
        v = v << 6 | (uint32_t)(p - B64);
        bits += 6;
        if (bits >= 8) { bits -= 8; if (o >= cap) return -1; out[o++] = (uint8_t)(v >> bits); }
    }
    return (long)o;
}
