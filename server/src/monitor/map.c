/* map.c — the Monitor's world map, filled from vectors on its own thread (see map.h).
 *
 * land.bin (tools/gen-monitor-data.py): per country, its box and its rings in hundredths of a degree.
 * Equirectangular projection: x = (lon - lon0) * ppd, y = (lat0 - lat) * ppd.
 *
 * Fill: active edge table with 4 sub-scanlines per row and exact horizontal coverage
 * (antialiasing), even-odd over all countries at once (they don't overlap; holes, like
 * Lesotho inside South Africa, resolve themselves). Then: borders with antialiased lines, arcs
 * from "this machine" (quadratic curve) and the dots. Everything is recomputed only when the
 * screen thread asks for another frame (zoom, panning or new data). */
#include "map.h"
#include "monpriv.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SS 4                            /* sub-scanlines per row */
#define LAND_MAX (8u << 20)

typedef struct { int16_t bbox[4]; uint32_t ring0, nrings; } Country;

static struct {
    uint8_t *raw;                       /* land.bin */
    Country *c;
    uint32_t nc;
    uint32_t *ring_off, *ring_n;        /* offset (in points) and length of each ring */
    uint32_t nrings;
} L;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int started, pending;
    MapReq req;
    uint64_t req_seq, done_seq;
    uint8_t *frame;
    size_t frame_cap;
    MapReq frame_view;
    double frame_ms;
    char path[PATH_MAX];
} M = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

/* ---- data ------------------------------------------------------------------------------- */

static void land_free(void) {
    free(L.raw); free(L.c); free(L.ring_off); free(L.ring_n);
    memset(&L, 0, sizeof L);
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int16_t rd16(const uint8_t *p) { return (int16_t)(uint16_t)(p[0] | p[1] << 8); }

/* Reads and verifies land.bin: everything used afterwards is inside the file. */
static int land_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long len = -1;
    if (!fseek(f, 0, SEEK_END)) len = ftell(f);
    uint8_t *b = len > 12 && (unsigned long)len <= LAND_MAX ? malloc((size_t)len) : NULL;
    int ok = b && !fseek(f, 0, SEEK_SET) && fread(b, 1, (size_t)len, f) == (size_t)len && !memcmp(b, "LORDMAP1", 8);
    fclose(f);
    if (!ok) { free(b); return -1; }
    size_t n = (size_t)len, o = 12;
    uint32_t nc = rd32(b + 8), nrings = 0;
    /* first pass: count and verify lengths */
    for (uint32_t i = 0; i < nc && ok; i++) {
        if (o + 12 > n) { ok = 0; break; }
        uint32_t nr = (uint32_t)(b[o + 10] | b[o + 11] << 8);
        o += 12;
        for (uint32_t r = 0; r < nr; r++) {
            if (o + 4 > n) { ok = 0; break; }
            uint32_t np = rd32(b + o);
            o += 4;
            if (np > (n - o) / 4) { ok = 0; break; }
            o += (size_t)np * 4;
            nrings++;
        }
    }
    if (!ok || nc > 4096) { free(b); return -1; }
    L.raw = b;
    L.nc = nc;
    L.c = calloc(nc ? nc : 1, sizeof *L.c);
    L.ring_off = calloc(nrings ? nrings : 1, sizeof *L.ring_off);
    L.ring_n = calloc(nrings ? nrings : 1, sizeof *L.ring_n);
    if (!L.c || !L.ring_off || !L.ring_n) { land_free(); return -1; }
    o = 12;
    uint32_t k = 0;
    for (uint32_t i = 0; i < nc; i++) {
        for (int j = 0; j < 4; j++) L.c[i].bbox[j] = rd16(b + o + 2 + 2 * j);
        uint32_t nr = (uint32_t)(b[o + 10] | b[o + 11] << 8);
        o += 12;
        L.c[i].ring0 = k;
        L.c[i].nrings = nr;
        for (uint32_t r = 0; r < nr; r++, k++) {
            uint32_t np = rd32(b + o);
            o += 4;
            L.ring_off[k] = (uint32_t)o;            /* in bytes */
            L.ring_n[k] = np;
            o += (size_t)np * 4;
        }
    }
    L.nrings = nrings;
    return 0;
}

static inline void ring_pt(uint32_t ring, uint32_t i, double *lon, double *lat) {
    const uint8_t *p = L.raw + L.ring_off[ring] + (size_t)i * 4;
    *lon = rd16(p) / 100.0;
    *lat = rd16(p + 2) / 100.0;
}

/* ---- drawing ---------------------------------------------------------------------------- */

typedef struct { float y0, y1, x, dx; } Edge;

static size_t edge_key(const Edge *d, size_t nk) {
    float k = ceilf(d->y0 * SS - 0.5f);
    return k < 0 ? 0 : k >= (float)nk ? nk - 1 : (size_t)k;
}

static void blend(uint8_t *px, uint32_t rgba, float cov) {
    float a = (rgba & 0xff) / 255.0f * cov;
    if (a <= 0.003f) return;
    if (a > 1) a = 1;
    float da = px[3] / 255.0f, oa = a + da * (1 - a);
    float r = (float)(rgba >> 24), g = (float)((rgba >> 16) & 0xff), b = (float)((rgba >> 8) & 0xff);
    px[0] = (uint8_t)((r * a + px[0] * da * (1 - a)) / oa + 0.5f);
    px[1] = (uint8_t)((g * a + px[1] * da * (1 - a)) / oa + 0.5f);
    px[2] = (uint8_t)((b * a + px[2] * da * (1 - a)) / oa + 0.5f);
    px[3] = (uint8_t)(oa * 255 + 0.5f);
}

static int visible(const MapReq *q, const Country *c, double margin) {
    double lon1 = q->lon0 + q->w / q->ppd, lat1 = q->lat0 - q->h / q->ppd;
    return c->bbox[2] / 100.0 >= q->lon0 - margin && c->bbox[0] / 100.0 <= lon1 + margin &&
           c->bbox[3] / 100.0 >= lat1 - margin && c->bbox[1] / 100.0 <= q->lat0 + margin;
}

static void fill_land(const MapReq *q, uint8_t *out) {
    size_t cap = 4096, n = 0;
    Edge *e = malloc(cap * sizeof *e);
    if (!e) return;
    for (uint32_t ci = 0; ci < L.nc; ci++) {
        const Country *c = &L.c[ci];
        if (!visible(q, c, 0.5)) continue;
        for (uint32_t r = c->ring0; r < c->ring0 + c->nrings; r++) {
            uint32_t np = L.ring_n[r];
            double lon, lat;
            ring_pt(r, np - 1, &lon, &lat);
            float px = (float)((lon - q->lon0) * q->ppd), py = (float)((q->lat0 - lat) * q->ppd);
            for (uint32_t i = 0; i < np; i++) {
                ring_pt(r, i, &lon, &lat);
                float x = (float)((lon - q->lon0) * q->ppd), y = (float)((q->lat0 - lat) * q->ppd);
                if (i + 1 < np && fabsf(x - px) + fabsf(y - py) < 1.0f) continue;   /* less than 1 px: skipped */
                if (y != py && !((y < 0 && py < 0) || (y > q->h && py > q->h))) {
                    if (n == cap) {
                        Edge *ne = realloc(e, cap * 2 * sizeof *e);
                        if (!ne) { free(e); return; }
                        e = ne;
                        cap *= 2;
                    }
                    Edge *d = &e[n++];
                    if (py < y) { d->y0 = py; d->y1 = y; d->x = px; }
                    else { d->y0 = y; d->y1 = py; d->x = x; }
                    d->dx = (x - px) / (y - py);
                }
                px = x;
                py = y;
            }
        }
    }
    /* sort by starting sub-scanline (counting sort, O(n)): the edge enters at sub-scanline k, the
     * first with k + 0.5 >= y0 * SS */
    size_t nk = (size_t)q->h * SS + 1;
    uint32_t *cnt = calloc(nk + 1, sizeof *cnt);
    Edge *sorted = malloc((n ? n : 1) * sizeof *sorted);
    if (!cnt || !sorted) { free(cnt); free(sorted); free(e); return; }
    for (size_t i = 0; i < n; i++) cnt[edge_key(&e[i], nk) + 1]++;
    for (size_t k = 0; k < nk; k++) cnt[k + 1] += cnt[k];
    for (size_t i = 0; i < n; i++) sorted[cnt[edge_key(&e[i], nk)]++] = e[i];
    free(cnt);
    free(e);
    e = sorted;
    float *cov = calloc((size_t)q->w + 2, sizeof *cov), *run = calloc((size_t)q->w + 2, sizeof *run);
    size_t *act = malloc((n ? n : 1) * sizeof *act);
    float *xs = malloc((n ? n : 1) * sizeof *xs);
    if (!cov || !run || !act || !xs) { free(cov); free(run); free(act); free(xs); free(e); return; }
    const uint8_t lr = (uint8_t)(q->land >> 24), lg = (uint8_t)(q->land >> 16), lb = (uint8_t)(q->land >> 8);
    const float la = (q->land & 0xff) / 255.0f;
    size_t next = 0, na = 0;
    const float w = (float)q->w;
    for (int y = 0; y < q->h; y++) {
        memset(cov, 0, ((size_t)q->w + 2) * sizeof *cov);
        memset(run, 0, ((size_t)q->w + 2) * sizeof *run);
        int any = 0;
        for (int s = 0; s < SS; s++) {
            float sy = y + (s + 0.5f) / SS;
            size_t sub = (size_t)y * SS + (size_t)s;
            while (next < n && edge_key(&e[next], nk) <= sub) act[na++] = next++;
            /* the active edges stay sorted by x between sub-scanlines: they barely move,
             * so insertion is almost linear */
            size_t k = 0;
            for (size_t i = 0; i < na; i++) {
                Edge *d = &e[act[i]];
                if (d->y1 <= sy) continue;
                act[k] = act[i];
                xs[k] = d->y0 > sy ? -1e9f : d->x + (sy - d->y0) * d->dx;
                size_t j = k++;
                while (j > 0 && xs[j - 1] > xs[j]) {
                    float tx = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = tx;
                    size_t ta = act[j]; act[j] = act[j - 1]; act[j - 1] = ta;
                    j--;
                }
            }
            na = k;
            /* the ones that haven't started yet (y0 > sy) were left at the start with x = -1e9: skipped */
            size_t first = 0;
            while (first < na && xs[first] < -1e8f) first++;
            float *xv = xs + first;
            size_t nx = na - first;
            for (size_t i = 0; i + 1 < nx; i += 2) {
                float a = xv[i] < 0 ? 0 : xv[i], b = xv[i + 1] > w ? w : xv[i + 1];
                if (b <= a) continue;
                int ia = (int)a, ib = (int)b;
                const float u = 1.0f / SS;
                any = 1;
                if (ia == ib) { cov[ia] += u * (b - a); continue; }
                cov[ia] += u * (ia + 1 - a);
                run[ia + 1] += u;                   /* whole span: accumulated sum at the end */
                run[ib] -= u;
                if (ib < q->w) cov[ib] += u * (b - ib);
            }
        }
        if (!any) continue;
        /* land is written directly, without blending (below there are only grid lines) */
        uint8_t *row = out + (size_t)y * q->w * 4;
        float acc = 0;
        for (int x = 0; x < q->w; x++) {
            acc += run[x];
            float c = cov[x] + acc;
            if (c <= 0.004f) continue;
            if (c > 1) c = 1;
            uint8_t *p = row + x * 4;
            p[0] = lr; p[1] = lg; p[2] = lb;
            p[3] = (uint8_t)(c * la * 255 + 0.5f);
        }
    }
    free(cov); free(run); free(act); free(xs); free(e);
}

/* ~1 px antialiased line (Xiaolin Wu). */
static void plot(const MapReq *q, uint8_t *out, int x, int y, uint32_t c, float a) {
    if (x >= 0 && y >= 0 && x < q->w && y < q->h) blend(out + ((size_t)y * q->w + x) * 4, c, a);
}

static void line(const MapReq *q, uint8_t *out, float x0, float y0, float x1, float y1, uint32_t c) {
    int steep = fabsf(y1 - y0) > fabsf(x1 - x0);
    float t;
    if (steep) { t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    float dx = x1 - x0, g = dx > 0 ? (y1 - y0) / dx : 1, y = y0 + g * (roundf(x0) - x0);
    for (int x = (int)roundf(x0); x <= (int)roundf(x1); x++, y += g) {
        int iy = (int)floorf(y);
        float f = y - iy;
        if (steep) { plot(q, out, iy, x, c, 1 - f); plot(q, out, iy + 1, x, c, f); }
        else { plot(q, out, x, iy, c, 1 - f); plot(q, out, x, iy + 1, c, f); }
    }
}

static void borders(const MapReq *q, uint8_t *out) {
    for (uint32_t ci = 0; ci < L.nc; ci++) {
        const Country *c = &L.c[ci];
        if (!visible(q, c, 0.5)) continue;
        for (uint32_t r = c->ring0; r < c->ring0 + c->nrings; r++) {
            uint32_t np = L.ring_n[r];
            double lon, lat;
            ring_pt(r, np - 1, &lon, &lat);
            float px = (float)((lon - q->lon0) * q->ppd), py = (float)((q->lat0 - lat) * q->ppd);
            for (uint32_t i = 0; i < np; i++) {
                ring_pt(r, i, &lon, &lat);
                float x = (float)((lon - q->lon0) * q->ppd), y = (float)((q->lat0 - lat) * q->ppd);
                if (i + 1 < np && fabsf(x - px) + fabsf(y - py) < 1.5f) continue;
                int out_x = (x < -2 && px < -2) || (x > q->w + 2 && px > q->w + 2);
                int out_y = (y < -2 && py < -2) || (y > q->h + 2 && py > q->h + 2);
                if (!out_x && !out_y && fabsf(x - px) + fabsf(y - py) < 4 * (float)(q->w + q->h)) line(q, out, px, py, x, y, q->border);
                px = x;
                py = y;
            }
        }
    }
}

/* Disc with a soft edge. */
static void disc(const MapReq *q, uint8_t *out, float cx, float cy, float rad, uint32_t c, float alpha) {
    for (int y = (int)floorf(cy - rad - 1); y <= (int)ceilf(cy + rad + 1); y++)
        for (int x = (int)floorf(cx - rad - 1); x <= (int)ceilf(cx + rad + 1); x++) {
            float d = hypotf(x + 0.5f - cx, y + 0.5f - cy), a = rad + 0.5f - d;
            if (a > 0) plot(q, out, x, y, c, (a > 1 ? 1 : a) * alpha);
        }
}

/* Arc: quadratic curve that rises towards the pole, stronger towards the destination. */
/* A quadratic arc from (x0,y0) to (x1,y1), lifted by 30 % of its length, fading in toward the place.
 * Each arc first collects its own coverage (the strongest stamp per pixel, so the overlapping stamps
 * don't darken it) in a mask over its bounding box, then blends once over whatever is below: land,
 * borders and other arcs. (Before 0.8.5 it was blended only where the map was still fainter than the
 * arc, which hid most of an arc over land.) */
static void arc(const MapReq *q, uint8_t *out, float x0, float y0, float x1, float y1, float liftk, uint32_t c, float width, float alpha) {
    static uint8_t *mask;
    static size_t mcap;
    float len = hypotf(x1 - x0, y1 - y0);
    if (len < 2) return;
    float lift = len * liftk, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2 - lift;
    /* the control point bounds the curve */
    int bx0 = (int)floorf(fminf(fminf(x0, x1), cx) - width - 1), bx1 = (int)ceilf(fmaxf(fmaxf(x0, x1), cx) + width + 1);
    int by0 = (int)floorf(fminf(fminf(y0, y1), cy) - width - 1), by1 = (int)ceilf(fmaxf(fmaxf(y0, y1), cy) + width + 1);
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > q->w) bx1 = q->w;
    if (by1 > q->h) by1 = q->h;
    if (bx0 >= bx1 || by0 >= by1) return;
    int bw = bx1 - bx0, bh = by1 - by0;
    size_t need = (size_t)bw * bh;
    if (need > mcap) {
        uint8_t *m = realloc(mask, need);
        if (!m) return;
        mask = m;
        mcap = need;
    }
    memset(mask, 0, need);
    int steps = (int)(len / 0.8f) + 1;
    if (steps > 20000) steps = 20000;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps, u = 1 - t;
        float x = u * u * x0 + 2 * u * t * cx + t * t * x1, y = u * u * y0 + 2 * u * t * cy + t * t * y1;
        float a = alpha * (0.45f + 0.55f * t);
        for (int yy = (int)floorf(y - width); yy <= (int)ceilf(y + width); yy++)
            for (int xx = (int)floorf(x - width); xx <= (int)ceilf(x + width); xx++) {
                if (xx < bx0 || yy < by0 || xx >= bx1 || yy >= by1) continue;
                float d = hypotf(xx + 0.5f - x, yy + 0.5f - y), k = width / 2 + 0.5f - d;
                if (k <= 0) continue;
                int v = (int)((k > 1 ? 1 : k) * a * 255 + 0.5f);
                uint8_t *m = mask + (size_t)(yy - by0) * bw + (xx - bx0);
                if (v > *m) *m = (uint8_t)(v > 255 ? 255 : v);
            }
    }
    for (int yy = 0; yy < bh; yy++)
        for (int xx = 0; xx < bw; xx++) {
            uint8_t m = mask[(size_t)yy * bw + xx];
            if (m) blend(out + ((size_t)(yy + by0) * q->w + (xx + bx0)) * 4, c, m / 255.0f);
        }
}

/* Meridians and parallels every 30° (every 10° when zoomed), very faint: the sea is map too. */
static void graticule(const MapReq *q, uint8_t *out) {
    uint32_t c = (q->border & 0xffffff00u) | ((q->border & 0xff) / 4);
    int step = q->ppd > 12 ? 10 : 30;
    for (int lon = -180; lon <= 180; lon += step) {
        float x = (float)((lon - q->lon0) * q->ppd);
        line(q, out, x, (float)((q->lat0 - 84) * q->ppd), x, (float)((q->lat0 + 56) * q->ppd), c);
    }
    for (int lat = -50; lat <= 80; lat += step == 10 ? 10 : 30) {
        float y = (float)((q->lat0 - lat) * q->ppd);
        line(q, out, (float)((-180 - q->lon0) * q->ppd), y, (float)((180 - q->lon0) * q->ppd), y, c);
    }
}

static void render(const MapReq *q, uint8_t *out) {
    memset(out, 0, (size_t)q->w * q->h * 4);
    graticule(q, out);
    if (L.raw) {
        fill_land(q, out);
        borders(q, out);
    }
    float hx = (float)((q->hlon - q->lon0) * q->ppd), hy = (float)((q->lat0 - q->hlat) * q->ppd);
    for (int pass = 0; pass < 2 && q->has_home && q->narcs; pass++)        /* each app's arcs */
        for (int i = 0; i < q->narcs; i++) {
            const MapArc *a = &q->arcs[i];
            if (!!a->sel != pass) continue;
            float x = (float)((a->lon - q->lon0) * q->ppd), y = (float)((q->lat0 - a->lat) * q->ppd);
            arc(q, out, hx, hy, x, y, a->lift, a->color, a->sel ? 2.4f : a->idle ? 1.4f : 2.0f, a->sel ? 1.0f : a->idle ? 0.55f : 0.9f);
        }
    for (int pass = 0; pass < 2 && q->has_home && !q->narcs; pass++)        /* selected ones last, on top */
        for (int i = 0; i < q->ndots; i++) {
            const MapDot *d = &q->dots[i];
            if (!!d->sel != pass) continue;
            float x = (float)((d->lon - q->lon0) * q->ppd), y = (float)((q->lat0 - d->lat) * q->ppd);
            arc(q, out, hx, hy, x, y, 0.3f, d->sel ? q->sel_color : q->accent, d->sel ? 2.6f : d->idle ? 1.4f : 1.8f, d->sel ? 1.0f : d->idle ? 0.5f : 0.8f);
        }
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < q->ndots; i++) {
            const MapDot *d = &q->dots[i];
            if (!!d->sel != pass) continue;
            float x = (float)((d->lon - q->lon0) * q->ppd), y = (float)((q->lat0 - d->lat) * q->ppd);
            uint32_t c = d->sel ? q->sel_color : q->accent;
            disc(q, out, x, y, d->r + 3, c, 0.25f);
            disc(q, out, x, y, d->r, c, 0.95f);
            if (d->sel) disc(q, out, x, y, d->r * 0.4f, 0xffffffffu, 1);
        }
    if (q->has_home) {
        disc(q, out, hx, hy, 8, q->home_color, 0.28f);
        disc(q, out, hx, hy, 4.5f, q->home_color, 1);
    }
}

/* ---- thread ------------------------------------------------------------------------------ */

static void *worker(void *arg) {
    (void)arg;
    uint8_t *buf = NULL;
    size_t cap = 0;
    for (;;) {
        pthread_mutex_lock(&M.mu);
        while (!M.pending) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 20;
            /* 20 s without requests (Monitor closed or on another tab): the data is released */
            if (pthread_cond_timedwait(&M.cv, &M.mu, &ts) == ETIMEDOUT && !M.pending) {
                land_free();
                free(M.frame);
                M.frame = NULL;
                M.frame_cap = 0;
                free(buf);
                buf = NULL;
                cap = 0;
            }
        }
        MapReq q = M.req;
        uint64_t seq = M.req_seq;
        M.pending = 0;
        pthread_mutex_unlock(&M.mu);
        if (!L.raw && M.path[0]) land_load(M.path);
        size_t need = (size_t)q.w * q.h * 4;
        if (need > cap) { free(buf); buf = malloc(need); cap = buf ? need : 0; }
        if (!buf) continue;
        double t0 = mon_now_ms();
        render(&q, buf);
        double ms = mon_now_ms() - t0;
        pthread_mutex_lock(&M.mu);
        uint8_t *old = M.frame;             /* double buffer: the previous frame is reused */
        size_t old_cap = M.frame_cap;
        M.frame = buf;
        M.frame_cap = cap;
        M.frame_view = q;
        M.frame_ms = ms;
        M.done_seq = seq;
        pthread_mutex_unlock(&M.mu);
        buf = old;
        cap = old ? old_cap : 0;
    }
    return NULL;
}

void map_init(const char *assets_dir) {
    pthread_mutex_lock(&M.mu);
    if (assets_dir && *assets_dir) snprintf(M.path, sizeof M.path, "%s/monitor/land.bin", assets_dir);
    pthread_mutex_unlock(&M.mu);
}

uint64_t map_request(const MapReq *r) {
    pthread_mutex_lock(&M.mu);
    M.req = *r;
    M.req_seq++;
    M.pending = 1;
    if (!M.started) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 512 * 1024);
        if (!pthread_create(&th, &at, worker, NULL)) M.started = 1;
        pthread_attr_destroy(&at);
    }
    uint64_t s = M.req_seq;
    pthread_cond_signal(&M.cv);
    pthread_mutex_unlock(&M.mu);
    return s;
}

int map_take(uint64_t *seq, uint8_t **rgba, MapReq *view, double *ms) {
    int got = 0;
    pthread_mutex_lock(&M.mu);
    if (M.frame && M.done_seq != *seq) {
        size_t n = (size_t)M.frame_view.w * M.frame_view.h * 4;
        *rgba = malloc(n);
        if (*rgba) {
            memcpy(*rgba, M.frame, n);
            *view = M.frame_view;
            *ms = M.frame_ms;
            *seq = M.done_seq;
            got = 1;
        }
    }
    pthread_mutex_unlock(&M.mu);
    return got;
}
