/* wallpaper_image.c — a picture chosen by the user, made safe and quick to show (docs/WALLPAPERS.md). Runs only in a
 * child that has dropped to the session user (wallpaper_admin.c), never as root.
 *
 * Whatever the user picks becomes a plain baseline JPEG of at most 4K: the desktop and the login screen then load
 * something of a known size, whatever camera or file it came from. EXIF is applied, then gone, GPS position included. */
#include "wallpaper_image.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "stb_image_write.h"
#pragma GCC diagnostic pop
#pragma GCC diagnostic ignored "-Wunused-function"      /* the writer's other formats, reported at the end of the file */

#define THUMB_W 320
#define THUMB_H 180

int wp_read_file(const char *path, unsigned char **data, size_t *n) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return errno == ELOOP ? WP_BAD : WP_READ;
    struct stat st;
    int rc = WP_READ;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) rc = WP_BAD;
    else if (st.st_size > WP_MAX_FILE) rc = WP_TOOBIG;
    else if (st.st_size < 8) rc = WP_BAD;
    else {
        size_t size = (size_t)st.st_size, got = 0;
        unsigned char *b = malloc(size);
        while (b && got < size) {
            ssize_t r = read(fd, b + got, size - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (b && got == size) { *data = b; *n = size; rc = WP_OK; }
        else free(b);
    }
    close(fd);
    return rc;
}

static unsigned rd16(const unsigned char *p, int le) { return le ? p[0] | (unsigned)p[1] << 8 : (unsigned)p[0] << 8 | p[1]; }
static uint32_t rd32(const unsigned char *p, int le) {
    return le ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24
              : (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int exif_orientation(const unsigned char *t, size_t n) {         /* t: the TIFF header inside APP1 */
    if (n < 8) return 1;
    int le = t[0] == 'I' && t[1] == 'I';
    if (!le && !(t[0] == 'M' && t[1] == 'M')) return 1;
    uint32_t ifd = rd32(t + 4, le);
    if (ifd > n - 2 || n < 2) return 1;
    unsigned count = rd16(t + ifd, le);
    for (unsigned i = 0; i < count; i++) {
        size_t e = (size_t)ifd + 2 + (size_t)i * 12;
        if (e + 12 > n) return 1;
        if (rd16(t + e, le) == 0x0112) {
            unsigned v = rd16(t + e + 8, le);
            return v >= 1 && v <= 8 ? (int)v : 1;
        }
    }
    return 1;
}

int wp_orientation(const unsigned char *d, size_t n) {
    if (n < 4 || d[0] != 0xff || d[1] != 0xd8) return 1;
    size_t p = 2;
    while (p + 4 <= n && d[p] == 0xff) {
        unsigned m = d[p + 1];
        if (m == 0xda || m == 0xd9) break;              /* the picture itself: no more metadata */
        size_t l = (size_t)d[p + 2] << 8 | d[p + 3];
        if (l < 2 || p + 2 + l > n) break;
        if (m == 0xe1 && l >= 8 && !memcmp(d + p + 4, "Exif\0\0", 6)) return exif_orientation(d + p + 10, l - 8);
        p += 2 + l;
    }
    return 1;
}

/* The picture over black, as RGB in the same buffer. */
static void flatten(unsigned char *px, size_t count) {
    for (size_t i = 0; i < count; i++) {
        unsigned a = px[i * 4 + 3];
        for (int c = 0; c < 3; c++) px[i * 3 + (size_t)c] = (unsigned char)((px[i * 4 + (size_t)c] * a + 127) / 255);
    }
}

/* The rectangle (sx, sy, sw, sh) of an RGB picture, scaled to nw×nh by averaging: never upscales in practice. */
static unsigned char *scale(const unsigned char *in, int w, int sx, int sy, int sw, int sh, int nw, int nh) {
    unsigned char *out = malloc((size_t)nw * (size_t)nh * 3);
    if (!out) return NULL;
    for (int y = 0; y < nh; y++) {
        int y0 = (int)((long long)y * sh / nh), y1 = (int)((long long)(y + 1) * sh / nh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < nw; x++) {
            int x0 = (int)((long long)x * sw / nw), x1 = (int)((long long)(x + 1) * sw / nw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned long sum[3] = { 0, 0, 0 }, cnt = (unsigned long)(y1 - y0) * (unsigned long)(x1 - x0);
            for (int yy = y0; yy < y1; yy++) {
                const unsigned char *p = in + ((size_t)(sy + yy) * (size_t)w + (size_t)(sx + x0)) * 3;
                for (int xx = x0; xx < x1; xx++, p += 3) { sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; }
            }
            unsigned char *o = out + ((size_t)y * (size_t)nw + (size_t)x) * 3;
            for (int c = 0; c < 3; c++) o[c] = (unsigned char)(sum[c] / cnt);
        }
    }
    return out;
}

/* Upright, by the EXIF orientation. Returns the new picture and its size. */
static unsigned char *upright(const unsigned char *in, int w, int h, int o, int *ow, int *oh) {
    int swap = o >= 5;
    int W = swap ? h : w, H = swap ? w : h;
    unsigned char *out = malloc((size_t)W * (size_t)H * 3);
    if (!out) return NULL;
    for (int dy = 0; dy < H; dy++) for (int dx = 0; dx < W; dx++) {
        int sx = dx, sy = dy;
        switch (o) {
        case 2: sx = w - 1 - dx; break;
        case 3: sx = w - 1 - dx; sy = h - 1 - dy; break;
        case 4: sy = h - 1 - dy; break;
        case 5: sx = dy; sy = dx; break;
        case 6: sx = dy; sy = h - 1 - dx; break;
        case 7: sx = w - 1 - dy; sy = h - 1 - dx; break;
        case 8: sx = w - 1 - dy; sy = dx; break;
        default: break;
        }
        memcpy(out + ((size_t)dy * (size_t)W + (size_t)dx) * 3, in + ((size_t)sy * (size_t)w + (size_t)sx) * 3, 3);
    }
    *ow = W;
    *oh = H;
    return out;
}

typedef struct { unsigned char *p; size_t n, cap; int failed; } Out;
static void put(void *ctx, void *data, int size) {
    Out *o = ctx;
    if (size <= 0 || o->failed) return;
    if (o->n + (size_t)size > WP_MAX_JPEG) { o->failed = 1; return; }
    if (o->n + (size_t)size > o->cap) {
        size_t c = o->cap ? o->cap * 2 : 1u << 20;
        while (c < o->n + (size_t)size) c *= 2;
        unsigned char *b = realloc(o->p, c);
        if (!b) { o->failed = 1; return; }
        o->p = b;
        o->cap = c;
    }
    memcpy(o->p + o->n, data, (size_t)size);
    o->n += (size_t)size;
}

static int jpeg(const unsigned char *rgb, int w, int h, int quality, unsigned char **out, size_t *n) {
    Out o = { 0 };
    if (!stbi_write_jpg_to_func(put, &o, w, h, 3, rgb, quality) || o.failed || !o.n) { free(o.p); return WP_TOOBIG; }
    *out = o.p;
    *n = o.n;
    return WP_OK;
}

static int is_png(const unsigned char *d, size_t n) { return n > 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8); }
static int is_jpg(const unsigned char *d, size_t n) { return n > 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff; }

int wp_is_jpeg(const unsigned char *d, size_t n) {
    return n > 4 && n <= WP_MAX_JPEG && is_jpg(d, n) && d[n - 2] == 0xff && d[n - 1] == 0xd9;
}

int wp_encode(const unsigned char *in, size_t n, unsigned char **full, size_t *fn, unsigned char **thumb, size_t *tn) {
    if (n > WP_MAX_FILE) return WP_TOOBIG;
    if (!is_png(in, n) && !is_jpg(in, n)) return WP_BAD;
    int w, h, comp;
    if (!stbi_info_from_memory(in, (int)n, &w, &h, &comp) || w < 1 || h < 1) return WP_BAD;
    if ((long long)w * h > WP_MAX_PIXELS) return WP_TOOBIG;
    int orient = wp_orientation(in, n);
    unsigned char *px = stbi_load_from_memory(in, (int)n, &w, &h, &comp, 4);
    if (!px) return WP_BAD;
    flatten(px, (size_t)w * (size_t)h);
    unsigned char *rgb = px;                            /* from stb until replaced: both are plain malloc */
    int side = w > h ? w : h;
    if (side > WP_MAX_SIDE) {
        int nw = (int)((long long)w * WP_MAX_SIDE / side), nh = (int)((long long)h * WP_MAX_SIDE / side);
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        unsigned char *s = scale(rgb, w, 0, 0, w, h, nw, nh);
        stbi_image_free(rgb);
        if (!s) return WP_BAD;
        rgb = s;
        w = nw;
        h = nh;
    }
    if (orient != 1) {
        int nw, nh;
        unsigned char *u = upright(rgb, w, h, orient, &nw, &nh);
        free(rgb);
        if (!u) return WP_BAD;
        rgb = u;
        w = nw;
        h = nh;
    }
    int rc = jpeg(rgb, w, h, 90, full, fn);
    if (!rc && thumb) {
        /* the middle of the picture, like the desktop's cover */
        int cw = w, ch = (int)((long long)w * THUMB_H / THUMB_W);
        if (ch > h) { ch = h; cw = (int)((long long)h * THUMB_W / THUMB_H); }
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
        unsigned char *s = scale(rgb, w, (w - cw) / 2, (h - ch) / 2, cw, ch, THUMB_W, THUMB_H);
        if (!s) rc = WP_BAD;
        else { rc = jpeg(s, THUMB_W, THUMB_H, 80, thumb, tn); free(s); }
        if (rc) free(*full);
    }
    free(rgb);
    return rc;
}
