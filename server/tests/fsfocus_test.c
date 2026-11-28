/* fsfocus_test.c — who has the keyboard while a Linux window is fullscreen (src/wl/fsfocus.h; issue #175). */
#include "wl/fsfocus.h"

#include <stdio.h>

static int fails, total;

static void is(int got, int want, const char *what) {
    total++;
    if (got != want) { fails++; printf("FAIL %s: got %d, want %d\n", what, got, want); }
}

int main(void) {
    /* two displays side by side: a video fullscreen on the left one (1920x1080), work on the right (2560x1440) */
    FsfRect left = { 0, 0, 1920, 1080 };
    FsfRect editor = { 2100, 200, 1200, 800 }, behind = { 300, 200, 800, 600 }, straddle = { 1500, 100, 1000, 600 };
    is(fsf_hidden(editor, left, 0), 0, "a window on the other display is not hidden");
    is(fsf_hidden(behind, left, 0), 1, "a window under the video is hidden");
    is(fsf_hidden(straddle, left, 0), 0, "a window across both, its centre on the other display, is not");
    straddle.x = 1200;
    is(fsf_hidden(straddle, left, 0), 1, "a window across both, its centre under the video, is");
    is(fsf_hidden(editor, left, 1), 1, "one display: every window is hidden");

    /* the keys (the issue): the editor chosen on the other display keeps them wherever the pointer goes */
    is(fsf_take(0, 0, 0, 1), 0, "the editor keeps the keys with the pointer back over the video");
    is(fsf_take(0, 0, 0, 0), 0, "and with the pointer on its own display");
    is(fsf_take(1, 0, 0, 1), 0, "the fullscreen window asked for itself: itself");
    is(fsf_take(0, 0, 1, 0), 1, "a window under the video never takes them, wherever the pointer is");
    is(fsf_take(0, 1, 0, 1), 1, "no window, the pointer on the video's display: the video");
    is(fsf_take(0, 1, 0, 0), 0, "no window, the pointer on the other display: nobody (the desktop there)");
    /* (BUG-30 of the ws170 QA) a LordOS window (Notes) chosen on the other display: "no window" with the pointer there.
     * The pointer resting on the video's display afterwards (the person's, or taken back after an agent's click) no
     * longer gives the next key to the video; chosen on the video's display, the video keeps them as before */
    is(fsf_take(0, 1, 0, fsf_none_ptr(0, 1)), 0, "Notes chosen on the other display, the pointer back over the video: Notes keeps the keys");
    is(fsf_take(0, 1, 0, fsf_none_ptr(1, 0)), 1, "chosen on the video's display (one display, or under it): the video");
    is(fsf_take(0, 1, 0, fsf_none_ptr(-1, 1)), 1, "nothing chosen since the fullscreen began: the pointer now, as before");
    is(fsf_take(0, 1, 0, fsf_none_ptr(-1, 0)), 0, "nothing chosen since, the pointer on the other display: nobody");

    /* one display: as before 0.8.8.10, the fullscreen window always has the keys */
    is(fsf_take(0, 0, fsf_hidden(editor, left, 1), 0), 1, "one display: any other window gives way");

    printf("fsfocus: %d/%d ok\n", total - fails, total);
    return fails != 0;
}
