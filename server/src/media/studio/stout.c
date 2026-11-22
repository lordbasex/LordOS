/* stout.c — LordOS Studio's loopback output, the pure parts (stout.h). */
#include "stout.h"

#include <math.h>
#include <string.h>

void st_out_size(int *w, int *h) {
    int W = *w, H = *h;
    if (W < ST_OUT_MIN_W || W > ST_OUT_MAX_W || H < ST_OUT_MIN_H || H > ST_OUT_MAX_H) { W = 1280; H = 720; }
    *w = W & ~1;
    *h = H & ~1;
}

size_t st_yuyv_size(int w, int h) { return (size_t)w * (size_t)h * 2; }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* signed distance to a rounded box centred at (cx, cy), half sizes hw, hh, corner r (negative inside) */
static float sd_rbox(float x, float y, float cx, float cy, float hw, float hh, float r) {
    float qx = fabsf(x - cx) - (hw - r), qy = fabsf(y - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

/* the lens hood: a trapezoid from x0 (half height h0) to x1 (half height h1), centred on cy; roughly signed */
static float sd_hood(float x, float y, float x0, float x1, float cy, float h0, float h1) {
    float t = clampf((x - x0) / (x1 - x0), 0, 1);
    float hh = h0 + (h1 - h0) * t;
    float dy = fabsf(y - cy) - hh;
    float dx = x < x0 ? x0 - x : x > x1 ? x - x1 : -fminf(x - x0, x1 - x);
    return fmaxf(dx, dy);
}

/* distance to the segment (ax, ay)–(bx, by) */
static float sd_seg(float x, float y, float ax, float ay, float bx, float by) {
    float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
    float t = clampf((px * dx + py * dy) / (dx * dx + dy * dy), 0, 1);
    float ex = px - dx * t, ey = py - dy * t;
    return sqrtf(ex * ex + ey * ey);
}

void st_card_bgra(int card, uint8_t *bgra, int w, int h) {
    const float u = (float)(w < h * 16 / 9 ? w * 9 / 16 : h) / 100.0f;     /* one hundredth of the 16:9 area's height */
    const float cx = w * 0.5f, cy = h * 0.5f;
    /* the glyph: a body and a hood, the same shapes as the capture indicator's camera (sdk/capturestatus.jsx) */
    const float bx = cx - 5 * u, bhw = 13 * u, bhh = 9 * u, br = 3 * u;
    const float hx0 = bx + bhw + 1.5f * u, hx1 = hx0 + 9 * u;
    const float ax = cx - 20 * u, ay = cy - 15 * u, sx = cx + 20 * u, sy = cy + 15 * u;   /* the slash (off) */
    const float fg[3] = { 0.80f, 0.82f, 0.86f };
    for (int y = 0; y < h; y++) {
        float g = (float)y / (float)(h > 1 ? h - 1 : 1);
        /* a dark vertical gradient, a little lighter in the middle */
        float base[3] = { 0.090f - 0.03f * g, 0.098f - 0.03f * g, 0.122f - 0.035f * g };
        uint8_t *row = bgra + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float d = fminf(sd_rbox(px, py, bx, cy, bhw, bhh, br), sd_hood(px, py, hx0, hx1, cy, 3.5f * u, 8 * u));
            float a = clampf(0.5f - d, 0, 1);                       /* one pixel of anti-aliasing */
            if (card == ST_CARD_OFF) {
                float s = sd_seg(px, py, ax, ay, sx, sy);
                float cut = clampf(0.5f - (s - 3.2f * u), 0, 1);     /* a gap around the slash */
                a *= 1 - cut;
                float line = clampf(0.5f - (s - 1.4f * u), 0, 1);
                a = a > line ? a : line;
            }
            a *= 0.9f;
            for (int c = 0; c < 3; c++) {
                float v = base[c] * (1 - a) + fg[c] * a;
                row[x * 4 + 2 - c] = (uint8_t)lrintf(clampf(v, 0, 1) * 255.0f);    /* B G R from R G B */
            }
            row[x * 4 + 3] = 255;
        }
    }
}

void st_bgra_to_yuyv(const uint8_t *bgra, int stride, int w, int h, uint8_t *yuyv) {
    for (int y = 0; y < h; y++) {
        const uint8_t *s = bgra + (size_t)y * (size_t)stride;
        uint8_t *d = yuyv + (size_t)y * (size_t)w * 2;
        for (int x = 0; x + 1 < w; x += 2) {
            int b0 = s[x * 4], g0 = s[x * 4 + 1], r0 = s[x * 4 + 2];
            int b1 = s[x * 4 + 4], g1 = s[x * 4 + 5], r1 = s[x * 4 + 6];
            int y0 = ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16;
            int y1 = ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16;
            int r = (r0 + r1 + 1) >> 1, g = (g0 + g1 + 1) >> 1, b = (b0 + b1 + 1) >> 1;
            int cb = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            int cr = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            d[x * 2] = (uint8_t)y0;
            d[x * 2 + 1] = (uint8_t)(cb < 16 ? 16 : cb > 240 ? 240 : cb);
            d[x * 2 + 2] = (uint8_t)y1;
            d[x * 2 + 3] = (uint8_t)(cr < 16 ? 16 : cr > 240 ? 240 : cr);
        }
    }
}

int st_client_usage(uint32_t type, const uint8_t *data, size_t len) {
    if (type != ST_EVENT_CLIENT_USAGE || !data || len < 4) return -1;
    uint32_t n;
    memcpy(&n, data, 4);
    return n > 1024 ? 1024 : (int)n;
}
