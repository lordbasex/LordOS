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
int wl_paint_media(Canvas *c, int sid, int node, float bx, float by, float bw, float bh, float px, float py, float pw, float ph,
                   float sx, float sy, float sw, float sh, float op) {
    (void)c; (void)sid; (void)node; (void)bx; (void)by; (void)bw; (void)bh; (void)px; (void)py; (void)pw; (void)ph; (void)sx; (void)sy; (void)sw; (void)sh; (void)op;
    return 0;
}
void media_src_need_cpu(MediaSrc *s) { (void)s; }
/* NV12 sources (mambient.c, mviews.c, paint.c): not part of these checks */
int paint_blurs_over(const Node *n, Box *out, int max) { (void)n; (void)out; (void)max; return 0; }
void media_src_blur_note(MediaSrc *s, int node, const void *boxes, int n) { (void)s; (void)node; (void)boxes; (void)n; }
const uint32_t *media_src_rows(MediaSrc *s, int y0, int y1) { (void)s; (void)y0; (void)y1; return NULL; }
void media_ambient_box(MediaSrc *s) { (void)s; }
void media_src_format_check(MediaSrc *s) { (void)s; }
void media_src_tex_fresh(MediaSrc *s) { (void)s; }
void media_src_view_note(MediaSrc *s, int node, int bw, int bh) { (void)s; (void)node; (void)bw; (void)bh; }
void media_src_view_drop(MediaSrc *s, int node) { if (s->node == node) s->node = 0; }
int media_src_view_box(MediaSrc *s, int *bw, int *bh) { (void)s; *bw = *bh = 0; return 0; }

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

    /* 4b. where the paint put the picture, as fractions of the box (media_src_damage_rect damages only the part of
     * the node a frame changed): 100 × 50 in 320 × 200 is scaled 3.2 times, 20 px down */
    CHECK(src.map_ok && src.map_w == 100 && src.map_h == 50, "the picture's place: known after a paint");
    CHECK(fabsf(src.map_fx) < 1e-4f && fabsf(src.map_fy - 0.1f) < 1e-4f, "the picture's place: its corner");
    CHECK(fabsf(src.map_kx - 3.2f / W) < 1e-6f && fabsf(src.map_ky - 3.2f / H) < 1e-6f, "the picture's place: its scale");
    src.view.rot = 90;
    media_paint(&c, 7, 1, 0, 0, W, H, 0, 1);
    CHECK(!src.map_ok, "the picture's place: unknown when rotated (the whole node is damaged)");
    src.view.rot = 0;

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

    /* 6. a scaled picture whose frames change a part (Part 1 step 5, the CPU path): only that part of the scaled copy is
     * made again, and what is drawn is exactly what a whole copy draws (random pictures, random changes, scales up
     * and down, with and without brightness) */
    {
        unsigned seed = 12345;
        #define RND() (seed = seed * 1103515245u + 12345u, (seed >> 8))
        const int sizes[3][2] = { { 300, 190 }, { 211, 97 }, { 640, 400 } };
        uint32_t *ref = calloc((size_t)W * H, 4);
        Canvas rc;
        canvas_init(&rc, ref, W, H, W);
        int bands = 0, mism = 0;
        for (int k = 0; k < 3; k++) {
            int pw = sizes[k][0], ph = sizes[k][1];
            uint32_t *p6 = picture(pw, ph, 0);
            for (int i = 0; i < pw * ph; i++) p6[i] = RND() & 0xffffff;
            src.xrgb = 1;
            src.view.bright = k == 1 ? 20 : 0;
            src.view.contrast = k == 1 ? -10 : 0;
            canvas_clear(&c, 0);
            media_paint(&c, 7, 1, 0, 0, W, H, 0, 1);
            for (int it = 0; it < 40; it++) {
                /* one or two frames, each a change of one rectangle, noted as media_src_damage_rect does */
                int nf = 1 + (int)(RND() % 2);
                for (int f = 0; f < nf; f++) {
                    int rw = 1 + (int)(RND() % 40), rh = 1 + (int)(RND() % 30);
                    int rx = (int)(RND() % (unsigned)(pw - rw + 1)), ry = (int)(RND() % (unsigned)(ph - rh + 1));
                    for (int y = ry; y < ry + rh; y++)
                        for (int x = rx; x < rx + rw; x++) p6[(size_t)y * pw + x] = RND() & 0xffffff;
                    src.frame_seq++;
                    media_pd_note(&src, rx, ry, rw, rh);
                }
                unsigned b0 = src.pd_band;
                media_paint(&c, 7, 1, 0, 0, W, H, 0, 1);
                bands += src.pd_band != b0;
                /* the reference: the same picture painted from a whole new copy */
                uint32_t *keep = src.cache;
                unsigned kseq = src.cache_seq;
                src.cache = NULL;
                canvas_clear(&rc, 0);
                media_paint(&rc, 7, 1, 0, 0, W, H, 0, 1);
                free(src.cache);
                src.cache = keep;
                src.cache_seq = kseq;
                if (memcmp(buf, ref, (size_t)W * H * 4)) mism++;
            }
        }
        CHECK(bands == 120, "a changed part: only that part of the scaled copy made again (120 of 120 paints)");
        CHECK(mism == 0, "a changed part: drawn exactly as a whole new copy draws it");
        /* a frame whose change isn't known (no note): the whole copy */
        src.frame_seq++;
        unsigned f0 = src.pd_full, b0 = src.pd_band;
        media_paint(&c, 7, 1, 0, 0, W, H, 0, 1);
        CHECK(src.pd_full == f0 + 1 && src.pd_band == b0, "a frame without a known change: the whole copy");
        src.view.bright = src.view.contrast = 0;
        src.xrgb = 0;
        free(ref);
    }

    printf("mview_test: %d checks, %d failed\n", checks, fails);
    free(src.px);
    free(src.cache);
    free(buf);
    return fails ? 1 : 0;
}
