/* mview_test.c — how a media node draws its picture (src/media/mview.c), with the rest of the
 * media code replaced by stubs. The case that matters: a Machines or Remote Desk picture is
 * replaced (a new size from the guest or the remote computer) while it's shown scaled, as in full
 * screen. Whoever replaces it frees the scaled copy; the next paint must make a new one instead of
 * drawing nothing (the black full screen of the Machines alpha, 2026-11-05). */
#include "../src/media/mview.c"

#include <stdio.h>

static MediaSrc src = { .used = 1, .id = 7, .kind = SK_IMAGE, .sock = -1 };
MediaSrc *media_src_get(int id) { return id == src.id ? &src : NULL; }
MediaSrc *media_src_at(int i) { return i == 0 ? &src : NULL; }
Node *node_get(int id) { (void)id; return NULL; }
int mp_sendv(int sock, uint32_t type, uint32_t id, int64_t a, int64_t b, int64_t c, int64_t d) {
    (void)sock; (void)type; (void)id; (void)a; (void)b; (void)c; (void)d;
    return -1;
}

void media_cursor_touch(void) {}

static int fails, checks;
#define CHECK(cond, what) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL: %s\n", what); } } while (0)

/* a picture of w × h, one color, as js_vm.c's set_picture leaves it: the scaled copy dropped */
static uint32_t *picture(int w, int h, uint32_t argb) {
    uint32_t *px = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) px[i] = argb;
    free(src.px);
    src.px = px; src.w = w; src.h = h; src.opaque = 1; src.frame_seq++;
    free(src.cache); src.cache = NULL;      /* what set_picture did before the fix (cache_w/h kept) */
    return px;
}

static long lit(const Canvas *c, uint32_t rgb) {
    long n = 0;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) n += (c->px[(size_t)y * c->stride + x] & 0xffffff) == rgb;
    return n;
}

int main(void) {
    enum { W = 320, H = 200 };
    uint32_t *buf = calloc((size_t)W * H, 4);
    Canvas c;
    canvas_init(&c, buf, W, H, W);

    /* 1. a big scaled picture fills a big scaled copy */
    picture(300, 190, 0x00ff0000u);         /* QEMU's x8r8g8b8: the x byte is 0 */
    canvas_clear(&c, 0);
    media_paint(&c, 7, 1, 0, 0, W, H, 0, 1);
    CHECK(src.cache != NULL, "scaled picture: a scaled copy is made");
    CHECK(lit(&c, 0xff0000) > W * H / 2, "scaled picture: drawn");
    CHECK(src.painted_seq == src.frame_seq, "scaled picture: painted_seq follows the frame");

    /* 2. the picture is replaced by a smaller one (still scaled): the copy is gone, its old size stays */
    picture(160, 120, 0x0000ff00u);
    canvas_clear(&c, 0);
    media_paint(&c, 7, 1, 0, 0, W, H, 0, 1);
    CHECK(src.cache != NULL, "replaced picture: a new scaled copy is made");
    CHECK(lit(&c, 0x00ff00) > W * H / 2, "replaced picture: drawn (not black)");

    /* 3. the same size as the box: copied as is, no scaled copy needed */
    picture(W, H, 0x000000ffu);
    canvas_clear(&c, 0);
    media_paint(&c, 7, 1, 0, 0, W, H, 0, 1);
    CHECK(lit(&c, 0x0000ff) == W * H, "picture of the box's size: every pixel");

    /* 4. a frame that changes pixels in place rebuilds the copy (frame_seq) */
    uint32_t *px = picture(100, 50, 0x00ffffffu);
    canvas_clear(&c, 0);
    media_paint(&c, 7, 1, 0, 0, W, H, 0, 1);
    for (int i = 0; i < 100 * 50; i++) px[i] = 0x00123456u;
    src.frame_seq++;
    media_paint(&c, 7, 1, 0, 0, W, H, 0, 1);
    CHECK(lit(&c, 0x123456) > 0, "a new frame in the same picture: drawn again");

    /* 5. an x8r8g8b8 picture (alpha byte 0) is drawn opaque, copied as is and scaled */
    picture(W, H, 0x00336699u);
    src.xrgb = 1;
    canvas_clear(&c, 0);
    media_paint(&c, 7, 1, 0, 0, W, H, 0, 1);
    CHECK((buf[(size_t)H / 2 * W + W / 2] >> 24) == 0xff, "xrgb copied as is: alpha 255");
    picture(100, 50, 0x00336699u);
    canvas_clear(&c, 0);
    media_paint(&c, 7, 1, 0, 0, W, H, 0, 1);
    CHECK(buf[(size_t)H / 2 * W + W / 2] == 0xff336699u, "xrgb scaled: alpha 255");
    src.xrgb = 0;

    printf("mview_test: %d checks, %d failed\n", checks, fails);
    free(src.px);
    free(src.cache);
    free(buf);
    return fails ? 1 : 0;
}
