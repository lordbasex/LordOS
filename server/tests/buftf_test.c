/* buftf_test.c — src/wl/buftf.h: a buffer turned upright for each wl_output.transform, the texture corners of a turned
 * dmabuf, and a surface's damage in buffer pixels through the buffer scale and the viewport (docs/WAYLAND.md, "Frames,
 * titles and damage"). */
#include <stdio.h>
#include <string.h>
#include "wl/buftf.h"

static int fails;
static void check(const char *what, long got, long want) {
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

/* a 3 × 2 buffer, pixel value = 10 * row + column:
 *   0  1  2
 *  10 11 12 */
static const uint32_t B[6] = { 0, 1, 2, 10, 11, 12 };

static void upright(int tf, const uint32_t *want, int uw, int uh, const char *name) {
    uint32_t got[6];
    memset(got, 0xff, sizeof got);
    buftf_copy(tf, B, 3, 2, 3, got, 0);
    char what[96];
    snprintf(what, sizeof what, "%s: upright %dx%d as the protocol turns it", name, uw, uh);
    check(what, !memcmp(got, want, sizeof(uint32_t) * (size_t)(uw * uh)), 1);
    if (memcmp(got, want, sizeof(uint32_t) * (size_t)(uw * uh))) {
        for (int i = 0; i < uw * uh; i++) printf("%u%s", got[i], (i + 1) % uw ? " " : " | ");
        printf("\n");
    }
}

int main(void) {
    /* What the person sees for each transform. The client rendered its picture P turned by the transform; turning the
     * buffer back gives P. P here is what B shows when the client meant it: for 90 (the client turned P 90° counter-
     * clockwise into B) the picture is B turned 90° clockwise, a 2 × 3 image:
     *   10  0
     *   11  1
     *   12  2      (weston_transformed_coord, wlroots' wlr_fbox_transform: the same) */
    static const uint32_t n0[6] = { 0, 1, 2, 10, 11, 12 };
    static const uint32_t r90[6] = { 10, 0, 11, 1, 12, 2 };
    static const uint32_t r180[6] = { 12, 11, 10, 2, 1, 0 };
    static const uint32_t r270[6] = { 2, 12, 1, 11, 0, 10 };
    static const uint32_t f0[6] = { 2, 1, 0, 12, 11, 10 };
    static const uint32_t f90[6] = { 0, 10, 1, 11, 2, 12 };
    static const uint32_t f180[6] = { 10, 11, 12, 0, 1, 2 };
    static const uint32_t f270[6] = { 12, 2, 11, 1, 10, 0 };
    upright(0, n0, 3, 2, "normal");
    upright(1, r90, 2, 3, "90");
    upright(2, r180, 3, 2, "180");
    upright(3, r270, 2, 3, "270");
    upright(4, f0, 3, 2, "flipped");
    upright(5, f90, 2, 3, "flipped 90");
    upright(6, f180, 3, 2, "flipped 180");
    upright(7, f270, 2, 3, "flipped 270");
    check("swaps: 90", buftf_swaps(1), 1);
    check("swaps: 180", buftf_swaps(2), 0);
    check("swaps: flipped 270", buftf_swaps(7), 1);

    /* the texture corners of the whole upright picture (2 × 3 for a 3 × 2 buffer at 90): its top-left is the buffer's
     * bottom-left, its top-right the buffer's top-left */
    float c[8];
    buftf_corners(1, 2, 3, 0, 0, 2, 3, c);
    check("corners 90: top-left is the buffer's (0, 2)", (long)(c[0] * 10 + c[1]), 2);
    check("corners 90: top-right is the buffer's (0, 0)", (long)(c[2] * 10 + c[3]), 0);
    check("corners 90: bottom-right is the buffer's (3, 0)", (long)(c[6] * 10 + c[7]), 30);
    buftf_corners(0, 3, 2, 1, 0, 3, 2, c);
    check("corners normal: a crop's corners as they are", (long)(c[0] * 1000 + c[1] * 100 + c[6] * 10 + c[7]), 1032);

    /* damage: surface coordinates → buffer pixels */
    BufBox b = buftf_surface_damage(10, 20, 20, 30, 1, 640, 480, 0, 0, -1, -1, 0, 0);
    check("damage at scale 1", b.x0 * 1000000L + b.y0 * 10000 + b.x1 * 100 + b.y1, 10 * 1000000L + 20 * 10000 + 20 * 100 + 30);
    b = buftf_surface_damage(10, 20, 20, 30, 2, 1280, 960, 0, 0, -1, -1, 0, 0);
    check("damage at scale 2: doubled", b.x0 * 10000000L + b.y0 * 10000 + b.x1 + 0L * b.y1, 20 * 10000000L + 40 * 10000 + 40);
    check("damage at scale 2: bottom", b.y1, 60);
    /* a viewport: the buffer 1000 px wide shown 500 wide (dw), crop the whole buffer: ×2, one pixel more each side */
    b = buftf_surface_damage(100, 100, 110, 110, 1, 1000, 1000, 0, 0, 1000, 1000, 500, 500);
    check("damage through a viewport: left", b.x0, 199);
    check("damage through a viewport: right", b.x1, 221);
    /* a crop at (200, 100) of 400 × 300 shown at its own size */
    b = buftf_surface_damage(0, 0, 10, 10, 1, 1000, 1000, 200, 100, 400, 300, 0, 0);
    check("damage through a crop", b.x0 * 1000L + b.y0, 200 * 1000L + 100);
    /* clamped to the buffer, empty off it */
    b = buftf_surface_damage(-50, -50, 5000, 5000, 1, 640, 480, 0, 0, -1, -1, 0, 0);
    check("damage clamped", b.x0 + b.y0 + (b.x1 == 640) + (b.y1 == 480), 2);
    b = buftf_surface_damage(700, 0, 800, 10, 1, 640, 480, 0, 0, -1, -1, 0, 0);
    check("damage off the buffer is empty", b.x1 - b.x0, 0);
    BufBox u = buftf_union((BufBox){ 0, 0, 0, 0 }, (BufBox){ 5, 5, 10, 10 });
    check("union with an empty box", u.x0 + u.x1, 15);
    u = buftf_union((BufBox){ 0, 0, 4, 4 }, (BufBox){ 5, 5, 10, 10 });
    check("union", u.x0 * 1000L + u.x1 * 10 + u.y1, 110);
    printf("%s\n", fails ? "buftf-test: FAILED" : "buftf-test: ok");
    return fails ? 1 : 0;
}
