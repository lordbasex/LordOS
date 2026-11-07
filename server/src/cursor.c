/* cursor.c — our own cursors: each shape is a union of signed-distance primitives
 * (polygons, capsules, rings) in a 32×32-unit space; rasterized with 4×4 samples
 * per pixel at 1x (32 px) or 2x (64 px, what fits in the KMS cursor plane). Black fill,
 * white outline and a soft shadow. No system theme is read. */
#include "cursor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAXP 24
enum { PR_POLY, PR_CAPSULE, PR_RING };
typedef struct {
    int kind, n;
    float p[MAXP][2];       /* polygon: vertices; capsule: p[0], p[1]; ring: center p[0] */
    float r, r2;            /* capsule: radius; ring: radius and half thickness */
} Prim;

typedef struct {
    Prim prim[12];
    int n;
    float hx, hy;           /* hotspot in units */
} Shape;

static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float l = vx * vx + vy * vy, t = l > 0 ? (wx * vx + wy * vy) / l : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float dx = wx - vx * t, dy = wy - vy * t;
    return sqrtf(dx * dx + dy * dy);
}

static float prim_sdf(const Prim *q, float x, float y) {
    if (q->kind == PR_CAPSULE) return seg_dist(x, y, q->p[0][0], q->p[0][1], q->p[1][0], q->p[1][1]) - q->r;
    if (q->kind == PR_RING) return fabsf(hypotf(x - q->p[0][0], y - q->p[0][1]) - q->r) - q->r2;
    float d = 1e9f;
    int inside = 0;
    for (int i = 0, j = q->n - 1; i < q->n; j = i++) {
        float ax = q->p[j][0], ay = q->p[j][1], bx = q->p[i][0], by = q->p[i][1];
        float s = seg_dist(x, y, ax, ay, bx, by);
        if (s < d) d = s;
        if ((by > y) != (ay > y) && x < (ax - bx) * (y - by) / (ay - by) + bx) inside = !inside;
    }
    return inside ? -d : d;
}

static float shape_sdf(const Shape *s, float x, float y) {
    float d = 1e9f;
    for (int i = 0; i < s->n; i++) {
        float v = prim_sdf(&s->prim[i], x, y);
        if (v < d) d = v;
    }
    return d;
}

/* ---- building the shapes ---- */

static Prim *add(Shape *s, int kind) {
    Prim *p = &s->prim[s->n++];
    memset(p, 0, sizeof *p);
    p->kind = kind;
    return p;
}
static void poly(Shape *s, int n, const float (*pts)[2]) {
    Prim *p = add(s, PR_POLY);
    p->n = n;
    memcpy(p->p, pts, sizeof(float) * 2 * (size_t)n);
}
static void capsule(Shape *s, float ax, float ay, float bx, float by, float r) {
    Prim *p = add(s, PR_CAPSULE);
    p->p[0][0] = ax; p->p[0][1] = ay; p->p[1][0] = bx; p->p[1][1] = by;
    p->r = r;
}
static void ring(Shape *s, float cx, float cy, float r, float half) {
    Prim *p = add(s, PR_RING);
    p->p[0][0] = cx; p->p[0][1] = cy;
    p->r = r;
    p->r2 = half;
}

/* Horizontal double arrow centered at (16,16), rotated `deg` degrees. */
static void double_arrow(Shape *s, float deg) {
    float a = deg * (float)M_PI / 180, c = cosf(a), sn = sinf(a);
    float pts[2][3][2] = { { { -10, 0 }, { -4.5f, -5 }, { -4.5f, 5 } }, { { 10, 0 }, { 4.5f, -5 }, { 4.5f, 5 } } };
    for (int k = 0; k < 2; k++) {
        float t[3][2];
        for (int i = 0; i < 3; i++) {
            t[i][0] = 16 + pts[k][i][0] * c - pts[k][i][1] * sn;
            t[i][1] = 16 + pts[k][i][0] * sn + pts[k][i][1] * c;
        }
        poly(s, 3, (const float (*)[2])t);
    }
    capsule(s, 16 - 6 * c, 16 - 6 * sn, 16 + 6 * c, 16 + 6 * sn, 1.4f);
    s->hx = s->hy = 16;
}

static void arrow(Shape *s, float ox, float oy) {
    static const float a[7][2] = { { 0, 0 }, { 0, 17 }, { 4, 13.4f }, { 7, 20 }, { 9.6f, 18.9f }, { 6.7f, 12.5f }, { 12, 12.5f } };
    float t[7][2];
    for (int i = 0; i < 7; i++) { t[i][0] = a[i][0] + ox; t[i][1] = a[i][1] + oy; }
    poly(s, 7, (const float (*)[2])t);
    s->hx = ox;
    s->hy = oy;
}

/* Hand: index finger up (pointer), fingers open (grab) or closed (grabbing). */
static void hand(Shape *s, int kind) {
    static const float palm[6][2] = { { 8, 15 }, { 21, 15 }, { 22, 22 }, { 19, 28 }, { 11, 28 }, { 7.5f, 21 } };
    poly(s, 6, palm);
    if (kind == 0) {
        capsule(s, 10, 5, 10, 17, 1.9f);         /* index finger */
        capsule(s, 13.6f, 12, 13.6f, 17, 1.8f);
        capsule(s, 17, 12.6f, 17, 17, 1.8f);
        capsule(s, 20.2f, 13.6f, 20.2f, 18, 1.7f);
        capsule(s, 4.6f, 16.5f, 8.5f, 21, 1.8f);  /* thumb */
        s->hx = 10; s->hy = 3;
    } else if (kind == 1) {
        capsule(s, 10, 8, 10, 17, 1.8f);
        capsule(s, 13.6f, 6, 13.6f, 17, 1.8f);
        capsule(s, 17, 6.6f, 17, 17, 1.8f);
        capsule(s, 20.2f, 9, 20.2f, 18, 1.7f);
        capsule(s, 4, 14.5f, 8.5f, 21, 1.8f);
        s->hx = 14; s->hy = 14;
    } else {
        capsule(s, 10, 13.2f, 10, 17, 1.8f);
        capsule(s, 13.6f, 12.4f, 13.6f, 17, 1.8f);
        capsule(s, 17, 12.8f, 17, 17, 1.8f);
        capsule(s, 20.2f, 13.8f, 20.2f, 18, 1.7f);
        capsule(s, 6, 17, 8.5f, 21, 1.8f);
        s->hx = 14; s->hy = 16;
    }
}

static void build(int shape, Shape *s) {
    memset(s, 0, sizeof *s);
    switch (shape) {
    case CUR_POINTER: hand(s, 0); break;
    case CUR_GRAB: hand(s, 1); break;
    case CUR_GRABBING: hand(s, 2); break;
    case CUR_TEXT:
        capsule(s, 16, 7.5f, 16, 24.5f, 0.9f);
        capsule(s, 12.5f, 6.5f, 15, 7.6f, 0.9f); capsule(s, 17, 7.6f, 19.5f, 6.5f, 0.9f);
        capsule(s, 12.5f, 25.5f, 15, 24.4f, 0.9f); capsule(s, 17, 24.4f, 19.5f, 25.5f, 0.9f);
        s->hx = 16; s->hy = 16;
        break;
    case CUR_CROSSHAIR:
        capsule(s, 16, 5, 16, 13, 0.9f); capsule(s, 16, 19, 16, 27, 0.9f);
        capsule(s, 5, 16, 13, 16, 0.9f); capsule(s, 19, 16, 27, 16, 0.9f);
        s->hx = s->hy = 16;
        break;
    case CUR_MOVE: {
        capsule(s, 16, 8, 16, 24, 1.3f); capsule(s, 8, 16, 24, 16, 1.3f);
        static const float t[4][3][2] = { { { 16, 3 }, { 11.5f, 8.5f }, { 20.5f, 8.5f } }, { { 16, 29 }, { 11.5f, 23.5f }, { 20.5f, 23.5f } },
                                          { { 3, 16 }, { 8.5f, 11.5f }, { 8.5f, 20.5f } }, { { 29, 16 }, { 23.5f, 11.5f }, { 23.5f, 20.5f } } };
        for (int i = 0; i < 4; i++) poly(s, 3, t[i]);
        s->hx = s->hy = 16;
        break;
    }
    case CUR_NOT_ALLOWED:
        ring(s, 16, 16, 8.5f, 1.7f);
        capsule(s, 10.2f, 10.2f, 21.8f, 21.8f, 1.7f);
        s->hx = s->hy = 16;
        break;
    case CUR_WAIT:
        ring(s, 16, 16, 7.5f, 2.2f);
        capsule(s, 16, 16, 16, 11, 1.1f);
        capsule(s, 16, 16, 19.5f, 16, 1.1f);
        s->hx = s->hy = 16;
        break;
    case CUR_PROGRESS:
        arrow(s, 3, 3);
        ring(s, 22, 23, 4.5f, 1.4f);
        break;
    case CUR_EW: case CUR_E: case CUR_W: double_arrow(s, 0); break;
    case CUR_NS: case CUR_N: case CUR_S: double_arrow(s, 90); break;
    case CUR_NWSE: case CUR_NW: case CUR_SE: double_arrow(s, 45); break;
    case CUR_NESW: case CUR_NE: case CUR_SW: double_arrow(s, -45); break;
    default: arrow(s, 3, 3); break;
    }
}

/* ---- rasterization ---- */

static CursorImage cache[6][CUR_COUNT];       /* 75, 100, 125, 150, 175 and 200 % */
static int size_pct = 100;
static unsigned size_serial;
static int boost_8ths;                          /* shake to find it (shake.c): 0..8, toward 200 % */
static int shake_on = 1;

int cursor_set_size(int pct) {
    if (pct < 75) pct = 75;
    if (pct > 200) pct = 200;
    if (pct == size_pct) return 0;
    size_pct = pct;
    size_serial++;
    return 1;
}
int cursor_size(void) { return size_pct; }

int cursor_set_boost(double level) {
    int b = (int)(level * 8 + 0.5);
    if (b < 0) b = 0;
    if (b > 8) b = 8;
    if (b == boost_8ths) return 0;
    boost_8ths = b;
    size_serial++;
    return 1;
}
void cursor_set_shake(int on) { shake_on = on != 0; if (!shake_on) cursor_set_boost(0); }
int cursor_shake(void) { return shake_on; }
unsigned cursor_size_serial(void) { return size_serial; }

int cursor_effective_pct(int pct) {
    if (pct < 100) pct = 100;
    if (pct > 200) pct = 200;
    pct = pct * size_pct / 100;
    if (pct < 75) pct = 75;
    if (pct > 200) pct = 200;
    pct += (200 - pct) * boost_8ths / 8;        /* while shaken: up to the largest the plane takes */
    return (pct + 12) / 25 * 25;
}

static uint32_t pack(float a, float lum) {
    if (a <= 0) return 0;
    if (a > 1) a = 1;
    uint32_t A = (uint32_t)(a * 255 + 0.5f), c = (uint32_t)(lum * a * 255 + 0.5f);
    if (c > A) c = A;
    return A << 24 | c << 16 | c << 8 | c;
}

const CursorImage *cursor_image(int shape, int scale) { return cursor_image_pct(shape, scale >= 2 ? 200 : 100); }

/* The cursor at a display's scale (docs/DISPLAYS.md): the same vector shapes rasterized at
 * 32 px × scale, so a 150 % display gets a sharp 48 px arrow. */
const CursorImage *cursor_image_pct(int shape, int pct) {
    if (shape <= CUR_AUTO || shape >= CUR_COUNT) shape = CUR_DEFAULT;
    pct = cursor_effective_pct(pct);
    CursorImage *ci = &cache[(pct - 75) / 25][shape];
    if (ci->px) return ci;
    float scale = pct / 100.0f;
    Shape s;
    build(shape, &s);
    int size = (int)(32 * scale + 0.5f);
    uint32_t *px = calloc((size_t)size * size, 4);
    if (!px) return NULL;
    const float outline = 1.25f, inv = 1.0f / scale;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float a = 0, lum = 0;          /* alpha and light sums (premultiplied) */
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float ux = (x + (sx + 0.5f) / 4) * inv, uy = (y + (sy + 0.5f) / 4) * inv;
                    float d = shape_sdf(&s, ux, uy);
                    if (d < 0) { a += 1; }                          /* black fill */
                    else if (d < outline) { a += 1; lum += 1; }     /* white outline */
                    else {                                          /* shadow: the shape shifted 1 unit down */
                        float ds = shape_sdf(&s, ux - 0.4f, uy - 1.0f) - outline;
                        if (ds < 2.5f) a += 0.28f * (1 - (ds < 0 ? 0 : ds) / 2.5f);
                    }
                }
            a /= 16;
            lum /= 16;
            px[(size_t)y * size + x] = pack(a, a > 0 ? lum / a : 0);
        }
    ci->px = px;
    ci->w = ci->h = size;
    ci->hx = (int)(s.hx * scale + 0.5f);
    ci->hy = (int)(s.hy * scale + 0.5f);
    return ci;
}

int cursor_scale_for(int W, int H) { return W >= 2560 && H >= 1440 ? 2 : 1; }

int cursor_for_edges(int edges) {
    switch (edges & 15) {
    case 1: return CUR_N;
    case 2: return CUR_S;
    case 4: return CUR_W;
    case 8: return CUR_E;
    case 5: return CUR_NW;
    case 9: return CUR_NE;
    case 6: return CUR_SW;
    case 10: return CUR_SE;
    default: return CUR_MOVE;
    }
}

/* wp_cursor_shape_device_v1.shape (cursor-shape-v1.xml): 1 default … 34 zoom_out. */
int cursor_from_wayland_shape(unsigned shape) {
    static const unsigned char map[] = {
        0, CUR_DEFAULT, CUR_DEFAULT, CUR_DEFAULT, CUR_POINTER, CUR_PROGRESS, CUR_WAIT, CUR_CROSSHAIR, CUR_CROSSHAIR,
        CUR_TEXT, CUR_TEXT, CUR_DEFAULT, CUR_DEFAULT, CUR_MOVE, CUR_NOT_ALLOWED, CUR_NOT_ALLOWED, CUR_GRAB, CUR_GRABBING,
        CUR_E, CUR_N, CUR_NE, CUR_NW, CUR_S, CUR_SE, CUR_SW, CUR_W, CUR_EW, CUR_NS, CUR_NESW, CUR_NWSE, CUR_EW, CUR_NS,
        CUR_MOVE, CUR_DEFAULT, CUR_DEFAULT,
    };
    return shape < sizeof map ? map[shape] : CUR_DEFAULT;
}
