/* rdesk_thumb_test.c — Remote Desk's card pictures (src/rdesk/thumb.c): the scale, the PNG on disk with its modes, a
 * replaced picture, a removed one, and ids that could name another file. */
#include "../src/rdesk/thumb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    /* a 1920×1080 picture: left half red, right half blue */
    int w = 1920, h = 1080;
    uint32_t *px = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) px[(size_t)y * w + x] = x < w / 2 ? 0xffff0000u : 0xff0000ffu;
    int ow = 0, oh = 0;
    unsigned char *rgb = rd_thumb_scale(px, w, h, RD_THUMB_W, &ow, &oh);
    CHECK(rgb && ow == 640 && oh == 360);
    if (rgb) {
        CHECK(rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 0);
        unsigned char *last = rgb + ((size_t)359 * 640 + 639) * 3;
        CHECK(last[0] == 0 && last[1] == 0 && last[2] == 255);
    }
    free(rgb);
    /* smaller than the width: kept as it is */
    rgb = rd_thumb_scale(px, 320, 200, RD_THUMB_W, &ow, &oh);
    CHECK(rgb && ow == 320 && oh == 200);
    free(rgb);
    CHECK(!rd_thumb_scale(px, 0, 10, 640, &ow, &oh));

    CHECK(rd_thumb_id_ok("rdp-172_17_0_6-0"));
    CHECK(rd_thumb_id_ok("d1a2b3"));
    CHECK(!rd_thumb_id_ok(""));
    CHECK(!rd_thumb_id_ok("../x"));
    CHECK(!rd_thumb_id_ok("a/b"));
    CHECK(!rd_thumb_id_ok(".hidden"));
    char longid[80];
    memset(longid, 'a', 65); longid[65] = 0;
    CHECK(!rd_thumb_id_ok(longid));

    char home[] = "/tmp/rdesk-thumb-XXXXXX";
    if (!mkdtemp(home)) { printf("FAIL mkdtemp\n"); return 1; }
    CHECK(rd_thumb_save(home, "../evil", px, w, h, 640) == -1);
    CHECK(rd_thumb_save("relative", "dev1", px, w, h, 640) == -1);
    CHECK(rd_thumb_save(home, "dev1", px, w, h, 640) == 0);
    char path[512], dir[512];
    snprintf(dir, sizeof dir, "%s/.local/share/lordos/remote-desk/thumbs", home);
    snprintf(path, sizeof path, "%s/dev1.png", dir);
    struct stat st;
    CHECK(!stat(dir, &st) && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0700);
    CHECK(!stat(path, &st) && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600 && st.st_size > 100);
    FILE *f = fopen(path, "rb");
    unsigned char sig[24] = { 0 };
    CHECK(f && fread(sig, 1, sizeof sig, f) == sizeof sig);
    if (f) fclose(f);
    CHECK(!memcmp(sig, "\x89PNG\r\n\x1a\n", 8));
    /* IHDR: width and height, big-endian */
    CHECK(sig[16] == 0 && sig[17] == 0 && sig[18] == 2 && sig[19] == 0x80 && sig[22] == 1 && sig[23] == 0x68);
    /* a folder another user could read is closed again */
    chmod(dir, 0755);
    CHECK(rd_thumb_save(home, "dev1", px, 800, 600, 640) == 0);
    CHECK(!stat(dir, &st) && (st.st_mode & 0777) == 0700);
    f = fopen(path, "rb");
    CHECK(f && fread(sig, 1, sizeof sig, f) == sizeof sig);
    if (f) fclose(f);
    CHECK(sig[22] == 1 && sig[23] == 0xe0);          /* 480 high now: the file was replaced */
    CHECK(rd_thumb_remove(home, "dev1") == 0);
    CHECK(stat(path, &st) && rd_thumb_remove(home, "dev1") == 0);
    CHECK(rd_thumb_remove(home, "../x") == -1);
    /* a link where the folder goes is not followed */
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s/.local/share/lordos/remote-desk/thumbs' && mkdir -p '%s/elsewhere' && ln -s '%s/elsewhere' '%s/.local/share/lordos/remote-desk/thumbs'", home, home, home, home);
    CHECK(!system(cmd));
    CHECK(rd_thumb_save(home, "dev2", px, w, h, 640) == -1);
    snprintf(path, sizeof path, "%s/elsewhere/dev2.png", home);
    CHECK(stat(path, &st));
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    if (system(cmd)) printf("note: %s left behind\n", home);
    free(px);
    printf("rdesk-thumb-test: %d failed\n", fails);
    return fails != 0;
}
