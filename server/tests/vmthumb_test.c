/* vmthumb_test.c — a machine's card picture (src/vm/vmthumb.c): the box filter's size, and the write off the UI thread
 * (vm_thumb_save_async): a PNG of the scaled size replaces thumb.png, a second one asked for while the first is written is
 * refused, and nothing is left behind but thumb.png. */
#include "vm/vmthumb.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); putchar('\n'); } } while (0)

static int png_size(const char *path, int *w, int *h) {
    unsigned char b[24];
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n != sizeof b || memcmp(b, "\x89PNG", 4)) return -1;
    *w = b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19];
    *h = b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23];
    return 0;
}

int main(void) {
    int w = 1280, h = 800, ow = 0, oh = 0;
    uint32_t *px = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) px[i] = (uint32_t)(i * 2654435761u);
    unsigned char *rgb = vm_thumb_scale(px, w, h, VM_THUMB_W, &ow, &oh);
    CHECK(rgb && ow == 480 && oh == 300, "scaled to %dx%d", ow, oh);
    free(rgb);
    CHECK(!vm_thumb_scale(px, 0, h, 480, &ow, &oh), "a 0-wide picture is refused");

    char dir[] = "/tmp/vmthumb-test.XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    char path[64];
    snprintf(path, sizeof path, "%s/thumb.png", dir);
    CHECK(vm_thumb_save_async(dir, px, w, h, VM_THUMB_W) == 0, "the first write is queued");
    CHECK(vm_thumb_save_async(dir, px, w, h, VM_THUMB_W) == -1, "a second one while the first is written is refused");
    int pw = 0, ph = 0, ok = 0;
    for (int i = 0; i < 500 && !ok; i++) {                  /* up to 5 s */
        struct timespec t = { 0, 10 * 1000 * 1000 };
        nanosleep(&t, NULL);
        ok = !png_size(path, &pw, &ph) && vm_thumb_save_async(dir, px, w, h, VM_THUMB_W) == 0;
    }
    CHECK(ok && pw == 480 && ph == 300, "thumb.png is a 480x300 PNG (%dx%d) and the next write is taken", pw, ph);
    for (int i = 0; i < 500 && vm_thumb_save_async(dir, px, 2, 2, VM_THUMB_W) != 0; i++) {
        struct timespec t = { 0, 10 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    for (int i = 0; i < 100; i++) {                          /* the last (2x2) one done */
        struct timespec t = { 0, 10 * 1000 * 1000 };
        nanosleep(&t, NULL);
        if (!png_size(path, &pw, &ph) && pw == 2) break;
    }
    CHECK(pw == 2 && ph == 2, "the last picture replaced it (%dx%d)", pw, ph);
    CHECK(vm_thumb_save_async(dir, px, 0, h, VM_THUMB_W) == -1, "a 0-wide picture is refused");
    CHECK(vm_thumb_save_async("/nonexistent-vmthumb", px, w, h, VM_THUMB_W) == 0, "a missing folder is queued and fails quietly");
    DIR *d = opendir(dir);
    int others = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        others += strcmp(e->d_name, "thumb.png") != 0;      /* a temporary file left behind */
    }
    if (d) closedir(d);
    CHECK(others == 0, "nothing but thumb.png is left (%d other)", others);
    unlink(path);
    rmdir(dir);
    free(px);
    printf("vmthumb-test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
