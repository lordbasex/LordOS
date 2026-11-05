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

int vm_thumb_save(const char *dir, const uint32_t *px, int w, int h, int maxw) {
    int ow = 0, oh = 0;
    unsigned char *rgb = vm_thumb_scale(px, w, h, maxw, &ow, &oh);
    if (!rgb) return -1;
    int dfd = open(dir, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) { free(rgb); return -1; }
    char tmp[64];
    snprintf(tmp, sizeof tmp, ".thumb.%ld.tmp", (long)getpid());
    int rc = -1;
    int fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        Out o = { fd, 0 };
        stbi_write_png_compression_level = 3;          /* a quick write: it runs on the UI thread */
        int ok = stbi_write_png_to_func(put, &o, ow, oh, 3, rgb, ow * 3);
        if (close(fd)) o.failed = 1;
        if (ok && !o.failed && !renameat(dfd, tmp, dfd, "thumb.png")) rc = 0;
        else unlinkat(dfd, tmp, 0);
    }
    close(dfd);
    free(rgb);
    return rc;
}
