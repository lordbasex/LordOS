/* vmthumb.c — a machine's picture for its card in the library: the picture lord-vmdisplay hands lord-server, scaled
 * down (box filter) and kept as <machine>/thumb.png (0600, replaced atomically). Runs in lord-server, as the person;
 * QEMU can't write there (its Landlock allows disks/ only). The last one stays when the machine stops. */
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
#pragma GCC diagnostic ignored "-Wunused-function"

#include "vmthumb.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

unsigned char *vm_thumb_scale(const uint32_t *px, int w, int h, int maxw, int *ow, int *oh) {
    if (!px || w < 1 || h < 1 || maxw < 1) return NULL;
    int tw = w > maxw ? maxw : w, th = (int)((long long)h * tw / w);
    if (th < 1) th = 1;
    unsigned char *rgb = malloc((size_t)tw * th * 3);
    if (!rgb) return NULL;
    for (int y = 0; y < th; y++) {
        int y0 = (int)((long long)y * h / th), y1 = (int)((long long)(y + 1) * h / th);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < tw; x++) {
            int x0 = (int)((long long)x * w / tw), x1 = (int)((long long)(x + 1) * w / tw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned long r = 0, g = 0, b = 0, k = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t v = px[(size_t)yy * w + xx];
                    r += (v >> 16) & 255; g += (v >> 8) & 255; b += v & 255; k++;
                }
            unsigned char *o = rgb + ((size_t)y * tw + x) * 3;
            o[0] = (unsigned char)(r / k); o[1] = (unsigned char)(g / k); o[2] = (unsigned char)(b / k);
        }
    }
    *ow = tw; *oh = th;
    return rgb;
}

typedef struct { int fd, failed; } Out;
static void put(void *ud, void *data, int n) {
    Out *o = ud;
    const char *p = data;
    while (!o->failed && n > 0) {
        ssize_t r = write(o->fd, p, (size_t)n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { o->failed = 1; break; }
        p += r; n -= (int)r;
    }
}

/* an RGB picture as <dir>/<name>, replaced atomically; tmp: the temporary file's name in dir */
static int write_rgb(const char *dir, const char *name, const char *tmp, const unsigned char *rgb, int ow, int oh) {
    int dfd = open(dir, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) return -1;
    int rc = -1;
    int fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        Out o = { fd, 0 };
        stbi_write_png_compression_level = 3;          /* a quick write */
        int ok = stbi_write_png_to_func(put, &o, ow, oh, 3, (void *)rgb, ow * 3);
        if (close(fd)) o.failed = 1;
        if (ok && !o.failed && !renameat(dfd, tmp, dfd, name)) rc = 0;
        else unlinkat(dfd, tmp, 0);
    }
    close(dfd);
    return rc;
}

int vm_png_save(const char *dir, const char *name, const uint32_t *px, int w, int h, int maxw) {
    int ow = 0, oh = 0;
    unsigned char *rgb = vm_thumb_scale(px, w, h, maxw, &ow, &oh);
    if (!rgb) return -1;
    char tmp[64];
    snprintf(tmp, sizeof tmp, ".thumb.%ld.tmp", (long)getpid());
    int rc = write_rgb(dir, name, tmp, rgb, ow, oh);
    free(rgb);
    return rc;
}

/* the card's picture off the UI thread: scaling and encoding took 20 to 57 ms there, every 5 s while a machine's window
 * was open (docs/MAIN-THREAD.md). The picture is copied here (it changes under us) and a thread of its own scales, encodes
 * and writes it. One write at a time; a picture asked for while one is being written is skipped. */
typedef struct { char dir[PATH_MAX]; uint32_t *px; int w, h, maxw; } ThumbJob;
static atomic_int thumb_busy;

static void *thumb_main(void *arg) {
    ThumbJob *j = arg;
    int ow = 0, oh = 0;
    unsigned char *rgb = vm_thumb_scale(j->px, j->w, j->h, j->maxw, &ow, &oh);
    if (rgb) {
        char tmp[64];
        snprintf(tmp, sizeof tmp, ".thumb.%ld.bg.tmp", (long)getpid());
        write_rgb(j->dir, "thumb.png", tmp, rgb, ow, oh);
        free(rgb);
    }
    free(j->px);
    free(j);
    atomic_store(&thumb_busy, 0);
    return NULL;
}

int vm_thumb_save_async(const char *dir, const uint32_t *px, int w, int h, int maxw) {
    if (!dir || !px || w < 1 || h < 1 || maxw < 1 || strlen(dir) >= PATH_MAX || atomic_exchange(&thumb_busy, 1)) return -1;
    ThumbJob *j = calloc(1, sizeof *j);
    if (j) j->px = malloc((size_t)w * h * 4);
    if (!j || !j->px) { free(j); atomic_store(&thumb_busy, 0); return -1; }
    memcpy(j->px, px, (size_t)w * h * 4);
    j->w = w; j->h = h; j->maxw = maxw;
    memcpy(j->dir, dir, strlen(dir) + 1);
    pthread_attr_t at;
    pthread_t t;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int e = pthread_create(&t, &at, thumb_main, j);
    pthread_attr_destroy(&at);
    if (e) { free(j->px); free(j); atomic_store(&thumb_busy, 0); return -1; }
    return 0;
}
