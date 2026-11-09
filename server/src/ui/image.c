/* image.c — PNG/JPEG loading (stb_image), cached scaling and drawing.
 *
 * File images are cached by path and shared by every node that shows them. Each holder counts (image_get and
 * image_get_thumb add one, image_release drops it); an image nobody holds stays cached for a while and is dropped,
 * oldest first, once those idle images pass IMG_IDLE_BYTES or IMG_IDLE_MAX. Until 0.8.8.7 nothing was ever dropped
 * and every thumbnail was a full-size decode: a folder of phone photos in Files took 48 MB per picture on the
 * interface's thread and lord-server ran out of memory (issues #71 and #82).
 *
 * Thumbnails (src "thumb:<px>:<path>", image_get_thumb) are decoded off the interface's thread by IMG_THREADS workers,
 * straight down to fit <px> x <px>, so a folder on a slow file system (a Cloud mount) never blocks the desktop and a
 * picture costs at most px * px * 4 bytes. Until one is ready the node draws nothing; image_async_dispatch (main
 * loop, image_async_fd) puts the pixels in and repaints the nodes that show it. */
#include "ui.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "stb_image.h"

enum { IMG_READY, IMG_PENDING, IMG_FAILED };

struct Image {
    char *path;
    int max;                        /* a thumbnail: decoded to fit max x max (0: the file's own size) */
    int refs;                       /* holders (nodes, callers); at 0 it is idle and may be dropped */
    int state;                      /* IMG_READY, IMG_PENDING (a worker is decoding it), IMG_FAILED */
    unsigned job;                   /* IMG_PENDING: its request */
    int fresh;                      /* image_async_dispatch: just got its pixels */
    unsigned long used;             /* when it last went idle (LRU) */
    int w, h, opaque;
    uint32_t *px;                   /* premultiplied ARGB */
    uint32_t *scaled;               /* last scaled version */
    int sw, sh, sfit;
    uint32_t *scaled2;              /* the previous one: an icon at two sizes (Dock and bar) is not rescaled every frame */
    int sw2, sh2, sfit2;
    long ver;                       /* the "#<version>" it was loaded for (image_get) */
    struct Image *next;
};

static struct Image *images;
static unsigned long tick;

static size_t img_bytes(const Image *i) {
    return ((size_t)i->w * i->h + (size_t)i->sw * i->sh + (size_t)i->sw2 * i->sh2) * 4;
}

static void img_free(Image *i) { free(i->path); free(i->px); free(i->scaled); free(i->scaled2); free(i); }

/* Drops idle images, the longest idle first, until they fit the budget. A pending one never waits idle (dropped at
 * once in image_release), so this only frees decoded pixels. */
static void trim(void) {
    for (;;) {
        size_t bytes = 0;
        int n = 0;
        Image *old = NULL;
        for (Image *i = images; i; i = i->next) {
            if (i->refs > 0) continue;
            bytes += img_bytes(i);
            n++;
            if (!old || i->used < old->used) old = i;
        }
        if (!old || (bytes <= IMG_IDLE_BYTES && n <= IMG_IDLE_MAX)) return;
        for (Image **pp = &images; *pp; pp = &(*pp)->next) if (*pp == old) { *pp = old->next; break; }
        img_free(old);
    }
}

/* the file's pixels, premultiplied; NULL if it doesn't load or is larger than IMG_MAX_PIXELS (a small file can
 * claim a huge picture, and stb would allocate all of it) */
static uint32_t *load_file(const char *path, int *w, int *h, int *opaque) {
    int ch;
    if (stbi_info(path, w, h, &ch) && (long long)*w * *h > IMG_MAX_PIXELS) {
        fprintf(stderr, "image: %s is %dx%d, more than %d pixels; not loaded\n", path, *w, *h, IMG_MAX_PIXELS);
        return NULL;
    }
    unsigned char *rgba = stbi_load(path, w, h, &ch, 4);
    if (!rgba) { fprintf(stderr, "image: could not load %s\n", path); return NULL; }
    uint32_t *px = malloc((size_t)*w * *h * 4);
    *opaque = 1;
    for (size_t i = 0; px && i < (size_t)*w * *h; i++) {
        uint32_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
        if (a < 255) *opaque = 0;
        px[i] = a << 24 | ((r * a + 127) / 255) << 16 | ((g * a + 127) / 255) << 8 | ((b * a + 127) / 255);
    }
    stbi_image_free(rgba);
    return px;
}

/* A path may end in "#<digits>": a version of a file that is rewritten in place (Remote Desk's card pictures). The
 * cache keeps one entry for the file and loads it again, into the same Image, when the version changes. */
Image *image_get(const char *spec) {
    char path[4096];
    const char *hash = strrchr(spec, '#');
    long ver = 0;
    size_t dl = hash ? strspn(hash + 1, "0123456789") : 0;
    if (hash && dl && dl <= 18 && !hash[1 + dl] && (size_t)(hash - spec) < sizeof path) {
        memcpy(path, spec, (size_t)(hash - spec));
        path[hash - spec] = 0;
        ver = strtol(hash + 1, NULL, 10);
    } else {
        snprintf(path, sizeof path, "%s", spec);
        hash = NULL;
    }
    for (Image *i = images; i; i = i->next) {
        if (i->max || strcmp(i->path, path)) continue;
        i->refs++;
        if (!hash || i->ver == ver) return i;
        int w, h, opaque;
        uint32_t *px = load_file(path, &w, &h, &opaque);
        if (!px) return i;                     /* the last one it had */
        free(i->px); free(i->scaled); free(i->scaled2);
        i->scaled = i->scaled2 = NULL;
        i->sw = i->sh = i->sw2 = i->sh2 = 0;
        i->px = px; i->w = w; i->h = h; i->opaque = opaque; i->ver = ver;
        return i;
    }
    int w, h, opaque;
    uint32_t *px = load_file(path, &w, &h, &opaque);
    if (!px) return NULL;
    Image *img = calloc(1, sizeof *img);
    if (!img || !(img->path = strdup(path))) { free(img); free(px); return NULL; }
    img->refs = 1;
    img->w = w; img->h = h; img->opaque = opaque; img->px = px; img->ver = ver;
    img->next = images;
    images = img;
    trim();
    return img;
}

/* ---- thumbnails, decoded by workers ---- */

typedef struct Job {
    unsigned id;
    int max;
    char *path;
    int ok, w, h, opaque;           /* the answer */
    uint32_t *px;
    struct Job *next;
} Job;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static Job *todo, *todo_tail, *done;
static int ntodo, nthreads, efd = -1;
static unsigned next_job = 1;

/* box-filters straight RGBA (sw x sh) down to tw x th, premultiplied ARGB */
static uint32_t *shrink(const unsigned char *rgba, int sw, int sh, int tw, int th, int *opaque) {
    uint32_t *px = malloc((size_t)tw * th * 4);
    if (!px) return NULL;
    *opaque = 1;
    for (int y = 0; y < th; y++) {
        int y0 = (int)((long long)y * sh / th), y1 = (int)((long long)(y + 1) * sh / th);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < tw; x++) {
            int x0 = (int)((long long)x * sw / tw), x1 = (int)((long long)(x + 1) * sw / tw);
            if (x1 <= x0) x1 = x0 + 1;
            uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const unsigned char *p = rgba + ((size_t)yy * sw + x0) * 4;
                for (int xx = x0; xx < x1; xx++, p += 4) {
                    uint32_t pa = p[3];
                    r += (uint64_t)p[0] * pa; g += (uint64_t)p[1] * pa; b += (uint64_t)p[2] * pa; a += pa;
                    n++;
                }
            }
            uint32_t A = (uint32_t)((a + n / 2) / n);
            if (A < 255) *opaque = 0;
            uint64_t d = n * 255;
            px[(size_t)y * tw + x] = A << 24 | (uint32_t)((r + d / 2) / d) << 16 | (uint32_t)((g + d / 2) / d) << 8 |
                                     (uint32_t)((b + d / 2) / d);
        }
    }
    return px;
}

/* the size a w x h picture gets to fit max x max (never larger than it is) */
void image_thumb_size(int w, int h, int max, int *tw, int *th) {
    if (w <= max && h <= max) { *tw = w; *th = h; return; }
    if (w >= h) { *tw = max; *th = (int)(((long long)h * max + w / 2) / w); }
    else { *th = max; *tw = (int)(((long long)w * max + h / 2) / h); }
    if (*tw < 1) *tw = 1;
    if (*th < 1) *th = 1;
}

static void decode(Job *j) {
    int w, h, ch;
    if (!stbi_info(j->path, &w, &h, &ch) || w <= 0 || h <= 0 || (long long)w * h > IMG_MAX_PIXELS) return;
    unsigned char *rgba = stbi_load(j->path, &w, &h, &ch, 4);
    if (!rgba) return;
    int tw, th;
    image_thumb_size(w, h, j->max, &tw, &th);
    j->px = shrink(rgba, w, h, tw, th, &j->opaque);
    stbi_image_free(rgba);
    if (j->px) { j->ok = 1; j->w = tw; j->h = th; }
}

static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&mu);
    for (;;) {
        while (!todo) pthread_cond_wait(&cv, &mu);
        Job *j = todo;
        todo = j->next;
        if (!todo) todo_tail = NULL;
        ntodo--;
        pthread_mutex_unlock(&mu);
        decode(j);
        pthread_mutex_lock(&mu);
        j->next = done;
        done = j;
        uint64_t one = 1;
        if (write(efd, &one, sizeof one) < 0) { /* the counter is already set: a dispatch is due anyway */ }
    }
    return NULL;
}

static void job_free(Job *j) { free(j->path); free(j->px); free(j); }

/* queues a decode; 0 when it can't (IMG_QUEUE_MAX waiting, no thread) */
static unsigned submit(const char *path, int max) {
    pthread_mutex_lock(&mu);
    if (efd < 0) efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    while (efd >= 0 && nthreads < IMG_THREADS) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        int bad = pthread_create(&t, &at, worker, NULL);
        pthread_attr_destroy(&at);
        if (bad) break;
        nthreads++;
    }
    unsigned id = 0;
    Job *j = efd >= 0 && nthreads && ntodo < IMG_QUEUE_MAX ? calloc(1, sizeof *j) : NULL;
    if (j && (j->path = strdup(path))) {
        if (!next_job) next_job = 1;
        id = j->id = next_job++;
        j->max = max;
        if (todo_tail) todo_tail->next = j; else todo = j;
        todo_tail = j;
        ntodo++;
        pthread_cond_signal(&cv);
    } else free(j);
    pthread_mutex_unlock(&mu);
    return id;
}

/* takes a request that hasn't started off the queue (its image was dropped); one already decoding is thrown away
 * when it ends */
static void cancel(unsigned id) {
    pthread_mutex_lock(&mu);
    Job *prev = NULL;
    for (Job *j = todo; j; prev = j, j = j->next) {
        if (j->id != id) continue;
        if (prev) prev->next = j->next; else todo = j->next;
        if (todo_tail == j) todo_tail = prev;
        ntodo--;
        job_free(j);
        break;
    }
    pthread_mutex_unlock(&mu);
}

Image *image_get_thumb(const char *path, int max) {
    if (!path || path[0] != '/') return NULL;
    if (max < 16) max = 16;
    if (max > IMG_THUMB_MAX) max = IMG_THUMB_MAX;
    for (Image *i = images; i; i = i->next)
        if (i->max == max && !strcmp(i->path, path)) { i->refs++; return i; }
    Image *img = calloc(1, sizeof *img);
    if (!img || !(img->path = strdup(path))) { free(img); return NULL; }
    img->max = max;
    img->refs = 1;
    img->opaque = 1;
    img->job = submit(path, max);
    img->state = img->job ? IMG_PENDING : IMG_FAILED;
    img->next = images;
    images = img;
    trim();
    return img;
}

int image_async_fd(void) { return efd; }

int image_async_dispatch(void) {
    uint64_t v;
    if (efd < 0 || (read(efd, &v, sizeof v) < 0 && errno != EAGAIN)) return 0;
    pthread_mutex_lock(&mu);
    Job *list = done;
    done = NULL;
    pthread_mutex_unlock(&mu);
    int n = 0;
    while (list) {
        Job *j = list;
        list = j->next;
        for (Image *i = images; i; i = i->next) {
            if (i->state != IMG_PENDING || i->job != j->id) continue;
            i->state = j->ok ? IMG_READY : IMG_FAILED;
            i->job = 0;
            if (j->ok) { i->px = j->px; j->px = NULL; i->w = j->w; i->h = j->h; i->opaque = j->opaque; i->fresh = 1; n++; }
            break;
        }
        job_free(j);
    }
    if (!n) return 0;
    /* the nodes that show them: measured and painted again */
    for (int k = 0; k < ui.cap; k++) {
        Node *nd = ui.nodes[k];
        if (nd && nd->img && nd->img->fresh) node_invalidate(nd);
    }
    for (Image *i = images; i; i = i->next) i->fresh = 0;
    ui.dirty = 1;
    return n;
}

/* how many images are cached, how many of them idle, and their bytes (engine-test) */
void image_cache_stats(int *count, int *idle, size_t *bytes) {
    *count = *idle = 0;
    *bytes = 0;
    for (Image *i = images; i; i = i->next) { (*count)++; *idle += i->refs <= 0; *bytes += img_bytes(i); }
}

void image_size(const Image *img, int *w, int *h) { *w = img->w; *h = img->h; }

/* A node's own image (`pixels` prop: an app's canvas, e.g. drawn from WASM).
 * It does not go into the path cache; it is reused if the size doesn't change. rgba = w*h*4 bytes. */
Image *image_pixels(Image *img, int w, int h, const uint8_t *rgba) {
    if (img && (img->path || img->w != w || img->h != h)) { image_release(img); img = NULL; }
    if (!img) {
        img = calloc(1, sizeof *img);
        if (!img) return NULL;
        img->w = w; img->h = h;
        img->px = malloc((size_t)w * h * 4);
        if (!img->px) { free(img); return NULL; }
    }
    img->opaque = 1;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
        if (a < 255) img->opaque = 0;
        img->px[i] = a << 24 | ((r * a + 127) / 255) << 16 | ((g * a + 127) / 255) << 8 | ((b * a + 127) / 255);
    }
    free(img->scaled);
    free(img->scaled2);
    img->scaled = img->scaled2 = NULL;
    img->sw = img->sh = img->sw2 = img->sh2 = 0;
    return img;
}

/* Same as image_pixels but with ARGB32 pixels already premultiplied (the ones returned by the
 * document helper, doc/doc.c: PDF pages, font samples). */
Image *image_argb(Image *img, int w, int h, const uint32_t *argb) {
    if (img && (img->path || img->w != w || img->h != h)) { image_release(img); img = NULL; }
    if (!img) {
        img = calloc(1, sizeof *img);
        if (!img) return NULL;
        img->w = w; img->h = h;
        img->px = malloc((size_t)w * h * 4);
        if (!img->px) { free(img); return NULL; }
    }
    memcpy(img->px, argb, (size_t)w * h * 4);
    img->opaque = 1;
    for (size_t i = 0; i < (size_t)w * h; i++) if ((argb[i] >> 24) != 255) { img->opaque = 0; break; }
    free(img->scaled);
    free(img->scaled2);
    img->scaled = img->scaled2 = NULL;
    img->sw = img->sh = img->sw2 = img->sh2 = 0;
    return img;
}

/* Frees an image from image_pixels; a file image loses a holder and, held by nobody, waits idle in the cache (a
 * thumbnail still decoding is dropped at once). */
void image_release(Image *img) {
    if (!img) return;
    if (!img->path) {
        free(img->px);
        free(img->scaled);
        free(img->scaled2);
        free(img);
        return;
    }
    if (--img->refs > 0) return;
    img->refs = 0;
    img->used = ++tick;
    if (img->state == IMG_PENDING) {
        cancel(img->job);
        for (Image **pp = &images; *pp; pp = &(*pp)->next) if (*pp == img) { *pp = img->next; break; }
        img_free(img);
        return;
    }
    trim();
}

static uint32_t sample_bilinear(const Image *img, float u, float v) {
    if (u < 0) u = 0;
    if (v < 0) v = 0;
    if (u > img->w - 1) u = (float)(img->w - 1);
    if (v > img->h - 1) v = (float)(img->h - 1);
    int x0 = (int)u, y0 = (int)v;
    int x1 = x0 + 1 < img->w ? x0 + 1 : x0, y1 = y0 + 1 < img->h ? y0 + 1 : y0;
    float fx = u - x0, fy = v - y0;
    uint32_t p[4] = { img->px[(size_t)y0 * img->w + x0], img->px[(size_t)y0 * img->w + x1],
                      img->px[(size_t)y1 * img->w + x0], img->px[(size_t)y1 * img->w + x1] };
    float wt[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) {
        float acc = 0;
        for (int k = 0; k < 4; k++) acc += ((p[k] >> s) & 255) * wt[k];
        out |= (uint32_t)(acc + 0.5f) << s;
    }
    return out;
}

/* Area averaging for strong downscaling (large icons → dock). */
static uint32_t sample_box(const Image *img, float u0, float v0, float u1, float v1) {
    int x0 = (int)floorf(u0), y0 = (int)floorf(v0), x1 = (int)ceilf(u1), y1 = (int)ceilf(v1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > img->w) x1 = img->w;
    if (y1 > img->h) y1 = img->h;
    uint32_t acc[4] = {0}, n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t p = img->px[(size_t)y * img->w + x];
            acc[0] += p & 255; acc[1] += (p >> 8) & 255; acc[2] += (p >> 16) & 255; acc[3] += p >> 24;
            n++;
        }
    if (!n) return 0;
    return (acc[3] / n) << 24 | (acc[2] / n) << 16 | (acc[1] / n) << 8 | (acc[0] / n);
}

static void rescale(Image *img, int bw, int bh, int fit) {
    if (img->scaled && img->sw == bw && img->sh == bh && img->sfit == fit) return;
    /* the other saved version: they are swapped */
    uint32_t *t = img->scaled; img->scaled = img->scaled2; img->scaled2 = t;
    int tw = img->sw, th = img->sh, tf = img->sfit;
    img->sw = img->sw2; img->sh = img->sh2; img->sfit = img->sfit2;
    img->sw2 = tw; img->sh2 = th; img->sfit2 = tf;
    if (img->scaled && img->sw == bw && img->sh == bh && img->sfit == fit) return;
    free(img->scaled);
    img->scaled = calloc((size_t)bw * bh, 4);
    if (!img->scaled) { img->sw = img->sh = 0; return; }
    img->sw = bw; img->sh = bh; img->sfit = fit;
    float sx = (float)bw / img->w, sy = (float)bh / img->h, ox = 0, oy = 0;
    if (fit != FIT_FILL) {
        float s = fit == FIT_COVER ? fmaxf(sx, sy) : fminf(sx, sy);
        sx = sy = s;
        ox = (bw - img->w * s) / 2;
        oy = (bh - img->h * s) / 2;
    }
    int box = sx < 0.75f || sy < 0.75f;
    for (int j = 0; j < bh; j++)
        for (int i = 0; i < bw; i++) {
            float u0 = (i - ox) / sx, v0 = (j - oy) / sy, u1 = (i + 1 - ox) / sx, v1 = (j + 1 - oy) / sy;
            if (u1 <= 0 || v1 <= 0 || u0 >= img->w || v0 >= img->h) continue;   /* contain margin */
            img->scaled[(size_t)j * bw + i] = box ? sample_box(img, u0, v0, u1, v1)
                                                  : sample_bilinear(img, (u0 + u1) / 2 - 0.5f, (v0 + v1) / 2 - 0.5f);
        }
}

void image_draw(Canvas *c, Image *img, float x, float y, float w, float h, float r, int fit, float opacity) {
    int bw = (int)roundf(w), bh = (int)roundf(h);
    if (bw <= 0 || bh <= 0 || !img->px) return;          /* a thumbnail not decoded yet: nothing */
    /* outside what is being repainted: nothing (not even rescaling) */
    int ix = (int)roundf(x), iy = (int)roundf(y);
    if (ix >= c->clip.x1 || iy >= c->clip.y1 || ix + bw <= c->clip.x0 || iy + bh <= c->clip.y0) return;
    if (bw == img->w && bh == img->h && img->px) {      /* 1:1 (a scrolled map, a new frame): no rescaling */
        canvas_blit(c, img->px, bw, bh, (int)roundf(x), (int)roundf(y), r, opacity, img->opaque);
        return;
    }
    rescale(img, bw, bh, fit);
    int opaque = img->opaque && (fit != FIT_CONTAIN);
    if (!img->scaled) return;
    canvas_blit(c, img->scaled, bw, bh, (int)roundf(x), (int)roundf(y), r, opacity, opaque);
}
