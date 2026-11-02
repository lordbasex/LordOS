/* lcimage.c — decoding for similar images, in axia-clean only: axia-server never decodes a file it
 * didn't make. PNG, JPEG, GIF (first frame) and BMP with the stb_image the project already carries,
 * set up as axia-maild's mimg.c, inside the same sandbox as the walk: no network, no exec, nothing
 * written. A hostile file reaches no further than the scanner already could.
 *
 * The hash: the image as 9 × 8 averaged grey cells, one bit per neighbouring pair, 1 where the left is
 * brighter. It survives scaling, recompression and small changes of light. A cell map with almost no
 * contrast says nothing about the picture (blank pages, solid fills), so such an image is left out. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"

#include "lcimage.h"
#include "../cleanproto.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#define FLAT 6                              /* grey levels between the darkest and brightest cell */

uint64_t lc_dhash(const unsigned char *grey, int w, int h, int *flat) {
    unsigned cells[8][9];
    unsigned lo = 255, hi = 0;
    for (int y = 0; y < 8; y++) {
        int y0 = (int)((long long)y * h / 8), y1 = (int)((long long)(y + 1) * h / 8);
        for (int x = 0; x < 9; x++) {
            int x0 = (int)((long long)x * w / 9), x1 = (int)((long long)(x + 1) * w / 9);
            unsigned long long sum = 0, n = (unsigned long long)(y1 - y0) * (unsigned long long)(x1 - x0);
            for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) sum += grey[(size_t)j * (size_t)w + (size_t)i];
            cells[y][x] = n ? (unsigned)(sum / n) : 0;
            if (cells[y][x] < lo) lo = cells[y][x];
            if (cells[y][x] > hi) hi = cells[y][x];
        }
    }
    uint64_t r = 0;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) r = r << 1 | (cells[y][x] > cells[y][x + 1]);
    *flat = hi - lo < FLAT;
    return r;
}

int lc_image_hash(int fd, uint64_t size, uint64_t *hash, int *w, int *h) {
    if (!size || size > CLEAN_IMAGE_MAX) return -1;
    unsigned char *file = malloc((size_t)size);
    if (!file) return -1;
    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, file + got, (size_t)size - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    int comp, r = -1;
    if (got == size && stbi_info_from_memory(file, (int)size, w, h, &comp) && *w >= CLEAN_IMAGE_MIN_DIM && *h >= CLEAN_IMAGE_MIN_DIM &&
        (long long)*w * *h <= CLEAN_IMAGE_PIXELS) {
        int dw, dh;
        unsigned char *grey = stbi_load_from_memory(file, (int)size, &dw, &dh, &comp, 1);
        if (grey && dw == *w && dh == *h) {
            int flat;
            *hash = lc_dhash(grey, dw, dh, &flat);
            r = flat ? 1 : 0;
        }
        stbi_image_free(grey);
    }
    free(file);
    return r;
}
