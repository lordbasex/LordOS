/* card.c — the test card: a TV test pattern in the spirit of the Philips PM5544, drawn in code.
 * Ported from go-link-console's backend-device/pkg/testpattern (same author), from BT.601 YUV to
 * 32-bit XRGB: the same grid, castellations, side columns, colour bars, center cross, frequency
 * gratings, grey steps and the two black boxes (the clock on top, the input box below). */
#include "card.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RGB(r, g, b) (0xff000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static const uint32_t BLACK = RGB(0, 0, 0), WHITE = RGB(235, 235, 235), GRID_GRAY = RGB(110, 110, 110),
                      LIGHT_GRAY = RGB(180, 180, 180);
static const uint32_t BARS[6] = { RGB(190, 190, 40), RGB(0, 180, 180), RGB(0, 180, 50), RGB(190, 0, 190), RGB(190, 0, 20), RGB(20, 20, 170) };
static const uint32_t GREYS[6] = { RGB(0, 0, 0), RGB(51, 51, 51), RGB(102, 102, 102), RGB(153, 153, 153), RGB(204, 204, 204), RGB(255, 255, 255) };
static const uint32_t SIDE_TOP[2] = { RGB(19, 147, 112), RGB(65, 105, 225) }, SIDE_BOT[2] = { RGB(190, 82, 110), RGB(155, 105, 35) };
static const uint32_t RIGHT_TOP[2] = { RGB(107, 142, 35), RGB(65, 105, 225) }, RIGHT_BOT[2] = { RGB(123, 95, 230), RGB(155, 105, 35) };

static uint32_t outside(const Card *c, int x, int y, int cell, int line) {
    int col = x / cell, row = y / cell, cols = c->w / cell, rows = c->h / cell, edge = cell / 5;
    if (y < edge || y >= c->h - edge) return ((x / (cell / 2)) % 2 == 0) ? BLACK : WHITE;   /* castellation */
    if (x < edge || x >= c->w - edge) return (row % 2 == 0) ? BLACK : WHITE;
    int in_top = row >= 1 && row <= rows / 2 - 1, in_bot = row >= rows / 2 && row <= rows - 2;
    if (col == 1 && in_top) return SIDE_TOP[0];
    if (col == 1 && in_bot) return SIDE_BOT[0];
    if (col == 2 && row >= 1 && row <= 2) return SIDE_TOP[1];
    if (col == 2 && row >= rows - 3 && row <= rows - 2) return SIDE_BOT[1];
    if (col == cols - 2 && in_top) return RIGHT_TOP[0];
    if (col == cols - 2 && in_bot) return RIGHT_BOT[0];
    if (col == cols - 3 && row >= 1 && row <= 2) return RIGHT_TOP[1];
    if (col == cols - 3 && row >= rows - 3 && row <= rows - 2) return RIGHT_BOT[1];
    return (x % cell < line || y % cell < line) ? WHITE : GRID_GRAY;
}

static uint32_t inside(double fx, double fy, double cx, double cy, double r, double top, double d, int cell, int line) {
    double t = (fy - top) / d, left = cx - r;
    int band = (int)((fx - left) / (d / 6));
    if (band < 0) band = 0;
    if (band > 5) band = 5;
    if (t < 0.12) return WHITE;
    if (t < 0.19) return ((int)((fx - left) / (d / 16)) % 2 == 0) ? BLACK : LIGHT_GRAY;
    if (t < 0.33) return BARS[band];
    if (t < 0.40) {                              /* black band with the centre cross and grid lines */
        if (fabs(fy - cy) < line || fabs(fx - cx) < line || (int)fx % cell < line) return WHITE;
        return BLACK;
    }
    if (t < 0.52) {                              /* frequency gratings, coarse to fine */
        static const double periods[6] = { 16, 10, 8, 6, 4, 3 };
        if (fabs(fx - cx) < line) return WHITE;
        int l = (int)(127.5 + 127.5 * sin(2 * M_PI * fx / periods[band]));
        return RGB(l, l, l);
    }
    if (t < 0.60) return GREYS[band];
    if (t < 0.885) return WHITE;
    return fabs(fx - cx) < d * 0.05 ? BARS[4] : BARS[0];
}

int card_build(Card *c, int w, int h) {
    w &= ~1;
    h &= ~1;
    if (w < 64 || h < 48) return -1;
    uint32_t *px = realloc(c->px, (size_t)w * (size_t)h * 4);
    if (!px) return -1;
    c->px = px;
    c->w = w;
    c->h = h;
    int cell = w / 16, line = w / 320 > 2 ? w / 320 : 2;
    double cx = w / 2.0, cy = h / 2.0, r = h * 0.448, top = cy - r, d = 2 * r;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double fx = x + 0.5, fy = y + 0.5;
            px[(size_t)y * w + x] = ((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy) <= r * r)
                ? inside(fx, fy, cx, cy, r, top, d, cell, line) : outside(c, x, y, cell, line);
        }
    c->clock = (CardBox){ (int)(cx - r * 0.52), (int)(top + d * 0.03), (int)(r * 1.04), (int)(d * 0.085) };
    c->pad = (CardBox){ (int)(cx - r * 0.74), (int)(top + d * 0.615), (int)(r * 1.48), (int)(d * 0.27) };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int in_c = x >= c->clock.x && x < c->clock.x + c->clock.w && y >= c->clock.y && y < c->clock.y + c->clock.h;
            int in_p = x >= c->pad.x && x < c->pad.x + c->pad.w && y >= c->pad.y && y < c->pad.y + c->pad.h;
            if (in_c || in_p) px[(size_t)y * w + x] = BLACK;
        }
    return 0;
}

void card_free(Card *c) { free(c->px); memset(c, 0, sizeof *c); }
