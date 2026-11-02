/* outputs.c — layout math for several displays (see outputs.h). */
#include "outputs.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>

/* ---- scale ---- */

int out_scale_valid(int pct) {
    return pct >= OUT_SCALE_MIN && pct <= OUT_SCALE_MAX && (pct - OUT_SCALE_MIN) % OUT_SCALE_STEP == 0;
}

int out_scale_round(double f) {
    if (!(f > 0)) return OUT_SCALE_MIN;
    int pct = (int)lround(f * 100.0 / OUT_SCALE_STEP) * OUT_SCALE_STEP;
    if (pct < OUT_SCALE_MIN) pct = OUT_SCALE_MIN;
    if (pct > OUT_SCALE_MAX) pct = OUT_SCALE_MAX;
    return pct;
}

/* About 110 logical px per inch is what desktop UIs (and LordOS's theme) are drawn for: a 24"
 * 1080p (92 dpi) stays at 100 %, a 27" 4K (163 dpi) gets 150 %, a 13" 3K laptop 200 %. The logical
 * desktop never gets smaller than 1024 × 600. */
int out_scale_recommended(int pw, int ph, int mm_w, int mm_h) {
    if (pw <= 0 || ph <= 0 || mm_w <= 0 || mm_h <= 0) return OUT_SCALE_MIN;
    double diag_px = sqrt((double)pw * pw + (double)ph * ph);
    double diag_in = sqrt((double)mm_w * mm_w + (double)mm_h * mm_h) / 25.4;
    if (diag_in < 3) return OUT_SCALE_MIN;          /* bogus EDID sizes (projectors report 0 or 1 cm) */
    int pct = out_scale_round(diag_px / diag_in / 110.0);
    while (pct > OUT_SCALE_MIN && (pw * 100 / pct < 1024 || ph * 100 / pct < 600)) pct -= OUT_SCALE_STEP;
    return pct;
}

void out_logical_size(int pw, int ph, int pct, int *lw, int *lh) {
    if (!out_scale_valid(pct)) pct = OUT_SCALE_MIN;
    *lw = (pw * 100 + pct / 2) / pct;
    *lh = (ph * 100 + pct / 2) / pct;
}

/* ---- views ---- */

OutView out_view(int x, int y, int pw, int ph, int pct) {
    OutView v = { x, y, 0, 0, pw, ph, 1, 0, 0 };
    if (!out_scale_valid(pct)) pct = OUT_SCALE_MIN;
    out_logical_size(pw, ph, pct, &v.w, &v.h);
    v.k = pct / 100.0;
    return v;
}

OutView out_view_mirror(int lw, int lh, int pw, int ph) {
    OutView v = { 0, 0, lw, lh, pw, ph, 1, 0, 0 };
    if (lw <= 0 || lh <= 0) return v;
    double kx = (double)pw / lw, ky = (double)ph / lh;
    v.k = kx < ky ? kx : ky;
    v.dx = (int)lround((pw - lw * v.k) / 2);
    v.dy = (int)lround((ph - lh * v.k) / 2);
    return v;
}

void out_to_phys(const OutView *v, double lx, double ly, double *px, double *py) {
    *px = (lx - v->x) * v->k + v->dx;
    *py = (ly - v->y) * v->k + v->dy;
}

void out_to_logical(const OutView *v, double px, double py, double *lx, double *ly) {
    *lx = (px - v->dx) / v->k + v->x;
    *ly = (py - v->dy) / v->k + v->y;
}

Rect out_phys_rect(const OutView *v, float x0, float y0, float x1, float y1) {
    if (x0 < v->x) x0 = (float)v->x;              /* only the part of the box this display shows */
    if (y0 < v->y) y0 = (float)v->y;
    if (x1 > v->x + v->w) x1 = (float)(v->x + v->w);
    if (y1 > v->y + v->h) y1 = (float)(v->y + v->h);
    if (x1 <= x0 || y1 <= y0) return (Rect){ 0, 0, 0, 0 };
    double a, b, c, d;
    out_to_phys(v, x0, y0, &a, &b);
    out_to_phys(v, x1, y1, &c, &d);
    Rect r = { (int)floor(a), (int)floor(b), (int)ceil(c), (int)ceil(d) };
    /* scaled: antialiased edges of a logical box spill into the next physical pixel */
    if (v->k != 1) { r.x0--; r.y0--; r.x1++; r.y1++; }
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > v->pw) r.x1 = v->pw;
    if (r.y1 > v->ph) r.y1 = v->ph;
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return (Rect){ 0, 0, 0, 0 };
    return r;
}

void out_logical_box(const OutView *v, Rect r, float *x0, float *y0, float *x1, float *y1) {
    double a, b, c, d;
    out_to_logical(v, r.x0, r.y0, &a, &b);
    out_to_logical(v, r.x1, r.y1, &c, &d);
    *x0 = (float)floor(a);
    *y0 = (float)floor(b);
    *x1 = (float)ceil(c);
    *y1 = (float)ceil(d);
}

/* ---- arrangement ---- */

static int overlaps(ORect a, ORect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Aligns a coordinate to the other display's edges when close (start with start, end with end). */
static int align(int v, int len, int o, int olen, int snap) {
    if (abs(v - o) <= snap) return o;
    if (abs(v + len - (o + olen)) <= snap) return o + olen - len;
    if (abs(v + len / 2 - (o + olen / 2)) <= snap) return o + olen / 2 - len / 2;
    return v;
}

void out_snap(const ORect *r, int n, int moving, int want_x, int want_y, int snap, int *x, int *y) {
    int w = r[moving].w, h = r[moving].h;
    long best = LONG_MAX;
    int bx = want_x, by = want_y;
    for (int i = 0; i < n; i++) {
        if (i == moving) continue;
        ORect o = r[i];
        int cx[4], cy[4];
        int yy = align(clampi(want_y, o.y - h + 1, o.y + o.h - 1), h, o.y, o.h, snap);
        int xx = align(clampi(want_x, o.x - w + 1, o.x + o.w - 1), w, o.x, o.w, snap);
        cx[0] = o.x + o.w; cy[0] = yy;              /* right of it */
        cx[1] = o.x - w;   cy[1] = yy;              /* left */
        cx[2] = xx;        cy[2] = o.y + o.h;       /* below */
        cx[3] = xx;        cy[3] = o.y - h;         /* above */
        for (int c = 0; c < 4; c++) {
            ORect m = { cx[c], cy[c], w, h };
            int bad = 0;
            for (int j = 0; j < n && !bad; j++) if (j != moving && overlaps(m, r[j])) bad = 1;
            if (bad) continue;
            long dx = cx[c] - want_x, dy = cy[c] - want_y, d = dx * dx + dy * dy;
            if (d < best) { best = d; bx = cx[c]; by = cy[c]; }
        }
    }
    if (best == LONG_MAX && n > 1) {                /* nothing free next to anyone: right of everything */
        int right = INT_MIN;
        for (int i = 0; i < n; i++) if (i != moving && r[i].x + r[i].w > right) right = r[i].x + r[i].w;
        bx = right;
        by = r[moving == 0 ? 1 : 0].y;
    }
    *x = bx;
    *y = by;
}

/* An edge of one on an edge of the other, along at least one pixel. */
static int touching(ORect a, ORect b) {
    if ((a.x + a.w == b.x || b.x + b.w == a.x) && (a.y < b.y + b.h && b.y < a.y + a.h)) return 1;
    if ((a.y + a.h == b.y || b.y + b.h == a.y) && (a.x < b.x + b.w && b.x < a.x + a.w)) return 1;
    return 0;
}

int out_fits(const ORect *r, int n, int moving, int x, int y) {
    if (n < 2) return 1;
    ORect m = { x, y, r[moving].w, r[moving].h };
    int touch = 0;
    for (int j = 0; j < n; j++) {
        if (j == moving) continue;
        if (overlaps(m, r[j])) return 0;
        if (touching(m, r[j])) touch = 1;
    }
    return touch;
}

int out_valid(const ORect *r, int n) {
    if (n <= 1) return 1;
    if (n > 64) return 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) if (overlaps(r[i], r[j])) return 0;
    /* joined: grow the set reached from r[0] through shared edges */
    unsigned long long seen = 1;
    for (int grew = 1; grew;) {
        grew = 0;
        for (int i = 0; i < n; i++) {
            if (seen >> i & 1) continue;
            for (int j = 0; j < n; j++)
                if ((seen >> j & 1) && touching(r[i], r[j])) { seen |= 1ULL << i; grew = 1; break; }
        }
    }
    return seen == (n == 64 ? ~0ULL : (1ULL << n) - 1);
}

void out_fix(ORect *r, int n) {
    if (out_valid(r, n)) return;
    for (int i = 1; i < n; i++) out_snap(r, i + 1, i, r[i].x, r[i].y, 0, &r[i].x, &r[i].y);
}

void out_normalize(ORect *r, int n, int *bw, int *bh) {
    if (n <= 0) { *bw = *bh = 0; return; }
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (int i = 0; i < n; i++) {
        if (r[i].x < x0) x0 = r[i].x;
        if (r[i].y < y0) y0 = r[i].y;
        if (r[i].x + r[i].w > x1) x1 = r[i].x + r[i].w;
        if (r[i].y + r[i].h > y1) y1 = r[i].y + r[i].h;
    }
    for (int i = 0; i < n; i++) { r[i].x -= x0; r[i].y -= y0; }
    *bw = x1 - x0;
    *bh = y1 - y0;
}

void out_auto(ORect *r, int n) {
    int x = 0;
    for (int i = 0; i < n; i++) { r[i].x = x; r[i].y = 0; x += r[i].w; }
}

/* ---- pointer and windows ---- */

int out_at(const ORect *r, int n, int x, int y) {
    for (int i = 0; i < n; i++)
        if (x >= r[i].x && y >= r[i].y && x < r[i].x + r[i].w && y < r[i].y + r[i].h) return i;
    return -1;
}

void out_clamp(const ORect *r, int n, int *x, int *y) {
    if (n <= 0 || out_at(r, n, *x, *y) >= 0) return;
    long best = LONG_MAX;
    int bx = *x, by = *y;
    for (int i = 0; i < n; i++) {
        if (r[i].w <= 0 || r[i].h <= 0) continue;
        int cx = clampi(*x, r[i].x, r[i].x + r[i].w - 1), cy = clampi(*y, r[i].y, r[i].y + r[i].h - 1);
        long dx = cx - *x, dy = cy - *y, d = dx * dx + dy * dy;
        if (d < best) { best = d; bx = cx; by = cy; }
    }
    *x = bx;
    *y = by;
}

int out_best(const ORect *r, int n, float x0, float y0, float x1, float y1) {
    int best = -1;
    double area = 0;
    for (int i = 0; i < n; i++) {
        double w = fmin(x1, r[i].x + r[i].w) - fmax(x0, r[i].x), h = fmin(y1, r[i].y + r[i].h) - fmax(y0, r[i].y);
        if (w <= 0 || h <= 0) continue;
        if (w * h > area) { area = w * h; best = i; }
    }
    return best;
}
