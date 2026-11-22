/* edid.c — EDID reader (see edid.h). */
#include "edid.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Text of a display descriptor: up to 13 bytes, ends with 0x0A and is padded with spaces.
 * Printable ASCII only; anything else is dropped. */
static void desc_text(const uint8_t *d, char *out) {
    int n = 0;
    for (int i = 5; i < 18; i++) {
        uint8_t c = d[i];
        if (c == 0x0a || c == 0) break;
        if (c >= 0x20 && c < 0x7f) out[n++] = (char)c;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
}

int edid_parse(const uint8_t *d, size_t len, Edid *e) {
    static const uint8_t hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    memset(e, 0, sizeof *e);
    if (!d || len < 128 || memcmp(d, hdr, 8)) return -1;
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++) sum = (uint8_t)(sum + d[i]);
    if (sum) return -1;
    unsigned m = (unsigned)d[8] << 8 | d[9];
    for (int i = 0; i < 3; i++) {
        unsigned c = (m >> (10 - 5 * i)) & 31;
        e->vendor[i] = c >= 1 && c <= 26 ? (char)('A' + c - 1) : '?';
    }
    e->product = (uint16_t)(d[10] | d[11] << 8);
    e->serial = (uint32_t)d[12] | (uint32_t)d[13] << 8 | (uint32_t)d[14] << 16 | (uint32_t)d[15] << 24;
    e->week = d[16];
    e->year = d[17] ? 1990 + d[17] : 0;
    e->version = d[18];
    e->revision = d[19];
    e->digital = d[20] >> 7;
    e->width_cm = d[21];
    e->height_cm = d[22];
    {   /* 10-bit chromaticities: the two low bits of each in bytes 25 and 26, the high eight in 27..34 */
        const int lo[8] = { d[25] >> 6, d[25] >> 4, d[25] >> 2, d[25], d[26] >> 6, d[26] >> 4, d[26] >> 2, d[26] };
        int any0 = 0;
        for (int i = 0; i < 8; i++) {
            int v = d[27 + i] << 2 | (lo[i] & 3);
            e->chroma[i] = (int)((v * 1000000LL + 512) / 1024);
            if (!v) any0 = 1;
        }
        if (any0) memset(e->chroma, 0, sizeof e->chroma);
    }
    for (int k = 0; k < 4; k++) {
        const uint8_t *x = d + 54 + 18 * k;
        if (x[0] || x[1]) {                          /* detailed timing */
            if (!e->pref_w) {
                e->pref_w = x[2] | (x[4] & 0xf0) << 4;
                e->pref_h = x[5] | (x[7] & 0xf0) << 4;
            }
            continue;
        }
        if (x[3] == 0xfc) desc_text(x, e->name);
        else if (x[3] == 0xff) desc_text(x, e->serial_text);
    }
    return 0;
}

void edid_identity(const Edid *e, char *out, size_t n) {
    if (e->serial_text[0]) snprintf(out, n, "%s-%04x-%s", e->vendor, e->product, e->serial_text);
    else if (e->serial) snprintf(out, n, "%s-%04x-%08x", e->vendor, e->product, e->serial);
    else snprintf(out, n, "%s-%04x-%s", e->vendor, e->product, e->name[0] ? e->name : "0");
    /* only [A-Za-z0-9.-]: used as a displays.json key */
    for (char *p = out; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.')) *p = '_';
}

void edid_display_name(const Edid *e, char *out, size_t n) {
    if (e->name[0]) snprintf(out, n, "%s", e->name);
    else snprintf(out, n, "%s %04X", e->vendor, e->product);
}

int edid_diagonal_tenths(const Edid *e) {
    if (e->width_cm <= 0 || e->height_cm <= 0) return 0;
    double in = sqrt((double)e->width_cm * e->width_cm + (double)e->height_cm * e->height_cm) / 2.54;
    return (int)(in * 10 + 0.5);
}
