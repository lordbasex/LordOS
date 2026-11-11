/* webicon.c — see webicon.h. */
#include "webicon.h"
#include "../web/webreg.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "stb_image_write.h"
#pragma GCC diagnostic pop
#pragma GCC diagnostic ignored "-Wunused-function"   /* stb_image_write's static declarations we do not use */
#include "stb_image.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int webicon_name(const char *app, int kind, char *out, size_t n) {
    if (!webreg_id_ok(app) || (kind != 1 && kind != 2)) return -1;
    int w = snprintf(out, n, "%s.%s", app, kind == 1 ? "png" : "svg");
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

static int home_ok(const char *home) { return home && home[0] == '/' && strlen(home) < PATH_MAX - 128; }

static int path_of(const char *home, const char *app, int kind, char *out, size_t n) {
    char name[WEBREG_ID_MAX + 8];
    if (!home_ok(home) || webicon_name(app, kind, name, sizeof name)) return -1;
    int w = snprintf(out, n, "%s/" WEBICON_DIR "/%s", home, name);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

/* ~/.cache, ~/.cache/lordos, …/web, …/icons: each a folder of ours, no link on the way */
static int make_dir(const char *home) {
    static const char *const parts[] = { ".cache", ".cache/lordos", ".cache/lordos/web", WEBICON_DIR };
    char p[PATH_MAX];
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", home, parts[i]);
        if (mkdir(p, 0700) && errno != EEXIST) return -1;
        struct stat st;
        if (lstat(p, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) return -1;
    }
    return 0;
}

int webicon_cached(const char *home, const char *app, char *out, size_t n) {
    for (int kind = 1; kind <= 2; kind++) {
        char p[PATH_MAX];
        struct stat st;
        if (path_of(home, app, kind, p, sizeof p)) return -1;
        if (lstat(p, &st) || !S_ISREG(st.st_mode) || st.st_size < 1 || st.st_size > WEBREG_ICON_BYTES) continue;
        if (snprintf(out, n, "%s", p) >= (int)n) return -1;
        return 0;
    }
    return -1;
}

int webicon_store(const char *home, const char *app, const unsigned char *b, size_t len, char *out, size_t n) {
    int kind = b && len ? webreg_icon_kind(b, len) : 0;
    char p[PATH_MAX], other[PATH_MAX];
    if (!kind || path_of(home, app, kind, p, sizeof p) || path_of(home, app, 3 - kind, other, sizeof other) || make_dir(home)) return -1;
    if (webreg_write_file(p, b, len)) return -1;
    unlink(other);
    if (out && n) snprintf(out, n, "%s", p);
    return 0;
}

void webicon_remove(const char *home, const char *app) {
    char p[PATH_MAX];
    for (int kind = 1; kind <= 2; kind++) if (!path_of(home, app, kind, p, sizeof p)) unlink(p);
}

/* ---- a profile's icon ---- */

long webicon_color_rgb(const char *c) {
    static const struct { const char *name; long rgb; } tags[] = {   /* sdk/fileglyphs.jsx TAG_COLORS */
        { "red", 0xff453a }, { "orange", 0xff9f0a }, { "yellow", 0xffd60a }, { "green", 0x30d158 },
        { "blue", 0x0a84ff }, { "purple", 0xbf5af2 }, { "gray", 0x8e8e93 },
    };
    for (size_t i = 0; c && i < sizeof tags / sizeof tags[0]; i++) if (!strcmp(c, tags[i].name)) return tags[i].rgb;
    return -1;
}

int webicon_profile_name(const char *app, const char *profile, char *out, size_t n) {
    if (!webreg_id_ok(app) || !webreg_id_ok(profile)) return -1;
    int w = snprintf(out, n, "%s.%s.png", app, profile);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

/* 5×7 capitals and digits, a row per byte (bit 4 the left column) */
static const unsigned char *glyph(char ch) {
    static const unsigned char az[26][7] = {
        { 14, 17, 17, 31, 17, 17, 17 }, { 30, 17, 17, 30, 17, 17, 30 }, { 14, 17, 16, 16, 16, 17, 14 }, { 30, 17, 17, 17, 17, 17, 30 },
        { 31, 16, 16, 30, 16, 16, 31 }, { 31, 16, 16, 30, 16, 16, 16 }, { 14, 17, 16, 23, 17, 17, 15 }, { 17, 17, 17, 31, 17, 17, 17 },
        { 14, 4, 4, 4, 4, 4, 14 },      { 7, 2, 2, 2, 2, 18, 12 },      { 17, 18, 20, 24, 20, 18, 17 }, { 16, 16, 16, 16, 16, 16, 31 },
        { 17, 27, 21, 21, 17, 17, 17 }, { 17, 17, 25, 21, 19, 17, 17 }, { 14, 17, 17, 17, 17, 17, 14 }, { 30, 17, 17, 30, 16, 16, 16 },
        { 14, 17, 17, 17, 21, 18, 13 }, { 30, 17, 17, 30, 20, 18, 17 }, { 15, 16, 16, 14, 1, 1, 30 },   { 31, 4, 4, 4, 4, 4, 4 },
        { 17, 17, 17, 17, 17, 17, 14 }, { 17, 17, 17, 17, 17, 10, 4 },  { 17, 17, 17, 21, 21, 21, 10 }, { 17, 17, 10, 4, 10, 17, 17 },
        { 17, 17, 10, 4, 4, 4, 4 },     { 31, 1, 2, 4, 8, 16, 31 },
    };
    static const unsigned char d09[10][7] = {
        { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 },      { 14, 17, 1, 2, 4, 8, 31 },     { 31, 2, 4, 2, 1, 17, 14 },
        { 2, 6, 10, 18, 31, 2, 2 },     { 31, 16, 30, 1, 1, 17, 14 },   { 6, 8, 16, 30, 17, 17, 14 },   { 31, 1, 2, 4, 8, 8, 8 },
        { 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 2, 12 },
    };
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') return az[ch - 'A'];
    if (ch >= '0' && ch <= '9') return d09[ch - '0'];
    return NULL;
}

/* paints rgb over px with coverage a (0..1), "over" in straight alpha */
static void blend(unsigned char *px, long rgb, double a) {
    if (a <= 0) return;
    if (a > 1) a = 1;
    unsigned char c[3] = { (unsigned char)(rgb >> 16), (unsigned char)(rgb >> 8), (unsigned char)rgb };
    double da = px[3] / 255.0, oa = a + da * (1 - a);
    for (int k = 0; k < 3; k++) px[k] = (unsigned char)(oa > 0 ? (c[k] * a + px[k] * da * (1 - a)) / oa + 0.5 : 0);
    px[3] = (unsigned char)(oa * 255 + 0.5);
}

unsigned char *webicon_compose(const unsigned char *base, size_t len, long rgb, char letter) {
    const int S = WEBICON_SIDE;
    unsigned char *out = calloc((size_t)S * S, 4);
    if (!out) return NULL;
    int w = 0, h = 0, ch = 0;
    /* the same check as every icon first (signature, IHDR, 1..4096 px), then stb as for the Dock's icons */
    unsigned char *src = base && len && len <= WEBREG_ICON_BYTES && webreg_icon_kind(base, len) == 1
                             ? stbi_load_from_memory(base, (int)len, &w, &h, &ch, 4) : NULL;
    if (src && w > 0 && h > 0) {
        /* a box filter into S×S (nearest when smaller), the aspect kept, centered */
        int side = w > h ? w : h;
        int ox = (side - w) / 2, oy = (side - h) / 2;
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                int x0 = x * side / S - ox, x1 = (x + 1) * side / S - ox, y0 = y * side / S - oy, y1 = (y + 1) * side / S - oy;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                double acc[4] = { 0, 0, 0, 0 };
                int cnt = 0;
                for (int sy = y0; sy < y1; sy++)
                    for (int sx = x0; sx < x1; sx++) {
                        cnt++;
                        if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                        const unsigned char *p = src + ((size_t)sy * w + sx) * 4;
                        double a = p[3];
                        acc[0] += p[0] * a; acc[1] += p[1] * a; acc[2] += p[2] * a; acc[3] += a;
                    }
                unsigned char *o = out + ((size_t)y * S + x) * 4;
                if (acc[3] > 0) for (int k = 0; k < 3; k++) o[k] = (unsigned char)(acc[k] / acc[3] + 0.5);
                o[3] = (unsigned char)(cnt ? acc[3] / cnt + 0.5 : 0);
            }
    } else {
        for (int i = 0; i < S * S; i++) { out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = 0x8e; out[i * 4 + 3] = 255; }
    }
    if (src) stbi_image_free(src);
    if (rgb < 0) return out;
    /* the dot: radius 13 in the bottom end corner, a 2 px white ring, its edge by coverage */
    const double cx = S - 15.0, cy = S - 15.0, r = 13.0, ring = 2.0;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy, dist = sqrt(dx * dx + dy * dy);
            unsigned char *o = out + ((size_t)y * S + x) * 4;
            blend(o, 0xffffff, r + ring + 0.5 - dist);
            blend(o, rgb, r + 0.5 - dist);
        }
    const unsigned char *g = glyph(letter);
    if (g) {
        /* 5×7 at 2× (10×14), centered in the dot */
        int gx = (int)cx - 5, gy = (int)cy - 7;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++) {
                if (!(g[row] & (16 >> col))) continue;
                for (int sy = 0; sy < 2; sy++)
                    for (int sx = 0; sx < 2; sx++) {
                        int x = gx + col * 2 + sx, y = gy + row * 2 + sy;
                        if (x >= 0 && y >= 0 && x < S && y < S) blend(out + ((size_t)y * S + x) * 4, 0xffffff, 1);
                    }
            }
    }
    return out;
}

static int profile_path(const char *config, const char *app, const char *profile, char *out, size_t n) {
    char name[2 * WEBREG_ID_MAX + 8];
    if (!config || config[0] != '/' || webicon_profile_name(app, profile, name, sizeof name)) return -1;
    int w = snprintf(out, n, "%s/icons/%s", config, name);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

void webicon_profile_remove(const char *config, const char *app, const char *profile) {
    char p[PATH_MAX];
    if (!profile_path(config, app, profile, p, sizeof p)) unlink(p);
}

int webicon_profile_write(const char *config, const char *app, const char *profile, const unsigned char *base, size_t len, long rgb, char letter) {
    char p[PATH_MAX], dir[PATH_MAX];
    if (profile_path(config, app, profile, p, sizeof p)) return -1;
    if (rgb < 0) { unlink(p); return 0; }
    snprintf(dir, sizeof dir, "%s/icons", config);
    struct stat st;
    if (mkdir(dir, 0700) && errno != EEXIST) return -1;
    if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) return -1;
    unsigned char *rgba = webicon_compose(base, len, rgb, letter);
    if (!rgba) return -1;
    int n = 0;
    unsigned char *png = stbi_write_png_to_mem(rgba, WEBICON_SIDE * 4, WEBICON_SIDE, WEBICON_SIDE, 4, &n);
    free(rgba);
    int rc = png && n > 0 ? webreg_write_file(p, png, (size_t)n) : -1;
    free(png);
    return rc;
}
