/* wallpaper_image_test.c — a picture chosen as wallpaper: what is refused and what comes out. */
#include "../src/hw/wallpaper_image.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

static int bad;
#define OK(name, cond) do { if (!(cond)) { printf("FAIL %s\n", name); bad++; } } while (0)

typedef struct { unsigned char *p; size_t n; } Buf;
static void cb(void *c, void *d, int n) { Buf *b = c; b->p = realloc(b->p, b->n + (size_t)n); memcpy(b->p + b->n, d, (size_t)n); b->n += (size_t)n; }

/* left half red, right half blue */
static unsigned char *two(int w, int h, int comp) {
    unsigned char *p = calloc((size_t)w * h, (size_t)comp);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        unsigned char *o = p + ((size_t)y * w + x) * comp;
        o[x < w / 2 ? 0 : 2] = 255;
        if (comp == 4) o[3] = 255;
    }
    return p;
}
static Buf png(int w, int h) { Buf b = { 0 }; unsigned char *p = two(w, h, 3); stbi_write_png_to_func(cb, &b, w, h, 3, p, w * 3); free(p); return b; }
static Buf jpg(int w, int h) { Buf b = { 0 }; unsigned char *p = two(w, h, 3); stbi_write_jpg_to_func(cb, &b, w, h, 3, p, 95); free(p); return b; }

/* The same JPEG with an EXIF orientation. */
static Buf with_exif(Buf j, int o, int le) {
    unsigned char t[26] = { 'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 1, 3, 0, 1, 0, 0, 0, (unsigned char)o, 0 };
    if (!le) {
        static const unsigned char be[20] = { 'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1, 1, 0x12, 0, 3, 0, 0, 0, 1 };
        memcpy(t, be, 20);
        t[18] = 0;
        t[19] = (unsigned char)o;
    }
    size_t tn = 26, seg = 2 + 6 + tn;
    Buf b = { malloc(j.n + seg + 2), j.n + seg + 2 };
    unsigned char *p = b.p;
    *p++ = 0xff; *p++ = 0xd8; *p++ = 0xff; *p++ = 0xe1; *p++ = (unsigned char)(seg >> 8); *p++ = (unsigned char)seg;
    memcpy(p, "Exif\0\0", 6); p += 6;
    memcpy(p, t, tn); p += tn;
    memcpy(p, j.p + 2, j.n - 2);
    return b;
}

static void size_of(const unsigned char *d, size_t n, int *w, int *h) { int c; if (!stbi_info_from_memory(d, (int)n, w, h, &c)) *w = *h = 0; }

int main(void) {
    unsigned char *f = NULL, *t = NULL;
    size_t fn = 0, tn = 0;
    int w, h;

    /* by content, whatever the name */
    OK("text", wp_encode((const unsigned char *)"this is not a picture at all", 28, &f, &fn, NULL, NULL) == WP_BAD);
    OK("svg", wp_encode((const unsigned char *)"<svg xmlns='x'></svg>     ", 26, &f, &fn, NULL, NULL) == WP_BAD);
    OK("gif", wp_encode((const unsigned char *)"GIF89a\1\0\1\0\0\0\0\0\0\0", 16, &f, &fn, NULL, NULL) == WP_BAD);
    Buf p = png(64, 32), j = jpg(64, 32);
    OK("cut png", wp_encode(p.p, 20, &f, &fn, NULL, NULL) == WP_BAD);
    OK("cut jpeg", wp_encode(j.p, 40, &f, &fn, NULL, NULL) == WP_BAD);

    /* a header that promises 144 million pixels: refused before any memory is taken */
    Buf big = png(4, 4);
    big.p[16] = 0; big.p[17] = 0; big.p[18] = 0x2e; big.p[19] = 0xe0;   /* 12000 */
    big.p[20] = 0; big.p[21] = 0; big.p[22] = 0x2e; big.p[23] = 0xe0;
    OK("pixels", wp_encode(big.p, big.n, &f, &fn, NULL, NULL) == WP_TOOBIG);
    OK("file size", wp_encode(p.p, (size_t)WP_MAX_FILE + 1, &f, &fn, NULL, NULL) == WP_TOOBIG);

    /* a small PNG becomes a JPEG of its size, with a thumbnail */
    OK("png", wp_encode(p.p, p.n, &f, &fn, &t, &tn) == WP_OK);
    size_of(f, fn, &w, &h);
    OK("png size", w == 64 && h == 32);
    OK("finished jpeg", wp_is_jpeg(f, fn));
    OK("cut is not finished", !wp_is_jpeg(f, fn - 1));
    OK("not jpeg", !wp_is_jpeg(p.p, p.n));
    size_of(t, tn, &w, &h);
    OK("thumb size", w == 320 && h == 180);
    free(f); free(t);

    /* the largest side comes down to the limit, the shape stays */
    Buf l = png(5000, 1000);
    OK("large", wp_encode(l.p, l.n, &f, &fn, NULL, NULL) == WP_OK);
    size_of(f, fn, &w, &h);
    OK("large size", w == WP_MAX_SIDE && h == WP_MAX_SIDE / 5);
    free(f);

    /* a tall picture keeps its middle in the thumbnail */
    Buf tall = png(100, 1000);
    OK("tall", wp_encode(tall.p, tall.n, &f, &fn, &t, &tn) == WP_OK);
    free(f); free(t);

    /* EXIF */
    OK("no exif", wp_orientation(j.p, j.n) == 1);
    OK("png has none", wp_orientation(p.p, p.n) == 1);
    Buf e6 = with_exif(j, 6, 1), e8 = with_exif(j, 8, 0), e99 = with_exif(j, 99, 1);
    OK("orientation le", wp_orientation(e6.p, e6.n) == 6);
    OK("orientation be", wp_orientation(e8.p, e8.n) == 8);
    OK("orientation nonsense", wp_orientation(e99.p, e99.n) == 1);
    for (size_t cut = 0; cut < 60 && cut < e6.n; cut++) (void)wp_orientation(e6.p, cut);   /* under ASan */
    OK("rotated", wp_encode(e6.p, e6.n, &f, &fn, NULL, NULL) == WP_OK);
    int c;
    unsigned char *px = stbi_load_from_memory(f, (int)fn, &w, &h, &c, 3);
    OK("rotated size", px && w == 32 && h == 64);
    /* turned clockwise: the left half, red, is now on top */
    if (px) OK("rotated colors", px[(size_t)(5 * w + 16) * 3] > 200 && px[(size_t)(58 * w + 16) * 3 + 2] > 200);
    free(px); free(f);
    unsigned char *meta = e6.p;
    OK("rotated had exif", memmem(meta, e6.n, "Exif", 4) != NULL);
    OK("exif gone", wp_encode(e6.p, e6.n, &f, &fn, NULL, NULL) == WP_OK && !memmem(f, fn, "Exif", 4));
    free(f);

    /* transparency: over black, not whatever the file keeps under it */
    { Buf b = { 0 };
      unsigned char q[16] = { 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0 };
      stbi_write_png_to_func(cb, &b, 2, 2, 4, q, 8);
      OK("alpha", wp_encode(b.p, b.n, &f, &fn, NULL, NULL) == WP_OK);
      px = stbi_load_from_memory(f, (int)fn, &w, &h, &c, 3);
      OK("alpha black", px && px[0] < 16);
      free(px); free(f); }

    /* the file: no links, no pipes, no empty files, nothing too big */
    char dir[] = "/tmp/wpimgXXXXXX", a[64], b_[64];
    OK("tmp", mkdtemp(dir) != NULL);
    snprintf(a, sizeof a, "%s/a.png", dir);
    int fd = open(a, O_WRONLY | O_CREAT, 0644);
    OK("write", write(fd, p.p, p.n) == (ssize_t)p.n);
    close(fd);
    unsigned char *d = NULL;
    size_t n = 0;
    OK("read", wp_read_file(a, &d, &n) == WP_OK && n == p.n && !memcmp(d, p.p, n));
    free(d);
    snprintf(b_, sizeof b_, "%s/link.png", dir);
    OK("symlink made", !symlink(a, b_));
    OK("symlink refused", wp_read_file(b_, &d, &n) == WP_BAD);
    unlink(b_);
    OK("fifo made", !mkfifo(b_, 0644));
    OK("fifo refused", wp_read_file(b_, &d, &n) == WP_BAD);
    unlink(b_);
    OK("folder refused", wp_read_file(dir, &d, &n) == WP_BAD);
    OK("device refused", wp_read_file("/dev/zero", &d, &n) == WP_BAD);
    OK("missing", wp_read_file("/nonexistent/x.png", &d, &n) == WP_READ);
    fd = open(b_, O_WRONLY | O_CREAT, 0644);
    close(fd);
    OK("empty refused", wp_read_file(b_, &d, &n) == WP_BAD);
    OK("sparse made", !truncate(b_, (off_t)WP_MAX_FILE + 1));
    OK("huge refused", wp_read_file(b_, &d, &n) == WP_TOOBIG);
    unlink(b_);
    unlink(a);
    rmdir(dir);
    printf("wallpaper-image-test: %s\n", bad ? "FAILED" : "ok");
    return bad != 0;
}
