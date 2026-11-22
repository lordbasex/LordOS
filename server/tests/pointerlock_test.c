/* pointerlock_test.c — the pointer lock of LordOS's own windows (src/pointerlock.c; Machines' "Capture mouse", docs/VIRTUALIZATION.md
 * 6.25): where the pointer stays, where the motion goes, the keys that end it, a node that goes away or is replaced. */
#include "pointerlock.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <time.h>

static int fails, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }

/* two nodes: 7 at (100, 50) 400 × 300, 9 at (0, 0) 200 × 100; gone[] takes one away */
static int gone[16];
static int rect(int node, float r[4]) {
    if (node <= 0 || node >= 16 || gone[node]) return -1;
    if (node == 7) { r[0] = 100; r[1] = 50; r[2] = 400; r[3] = 300; return 0; }
    if (node == 9) { r[0] = 0; r[1] = 0; r[2] = 200; r[3] = 100; return 0; }
    if (node == 3) { r[0] = 10; r[1] = 10; r[2] = 0; r[3] = 0; return 0; }     /* no size */
    return -1;
}

static int mx, my, moves, ends, why = -1;
static void motion(void *ud, int dx, int dy) { (void)ud; mx += dx; my += dy; moves++; }
static void ended(void *ud, int w) { (void)ud; ends++; why = w; }
static int tag_a, tag_b, tag_c;
static int btn[8], btn_down[8], nbtn, wy, wx;
static void button(void *ud, int b, int down) { (void)ud; if (nbtn < 8) { btn[nbtn] = b; btn_down[nbtn] = down; nbtn++; } }
static void wheel(void *ud, int dy, int dx) { (void)ud; wy += dy; wx += dx; }

static void sleep_ms(int ms) { struct timespec t = { 0, ms * 1000000L }; nanosleep(&t, NULL); }

int main(void) {
    ptrlock_init(rect);
    PtrLockSink a = { .motion = motion, .ended = ended, .ud = &tag_a }, js = { .motion = NULL, .ended = ended, .ud = &tag_b };
    PtrLockSink vm = { .motion = motion, .ended = ended, .ud = &tag_c, .button = button, .wheel = wheel };
    int x, y;
    unsigned s0 = ptrlock_serial();

    check(ptrlock_begin(5, &a) == -1 && ptrlock_begin(3, &a) == -1 && ptrlock_begin(0, &a) == -1, "no node, no size: refused");
    check(!ptrlock_active() && !ptrlock_hidden() && !ptrlock_where(&x, &y), "nothing held");
    check(ptrlock_begin(7, &a) == 0 && ptrlock_active() && ptrlock_node() == 7 && ptrlock_owned_by(&tag_a), "held on node 7");
    check(ptrlock_serial() != s0 && ptrlock_hidden(), "a new serial, the pointer hidden");
    check(ptrlock_where(&x, &y) && x == 300 && y == 200, "the pointer stays at the node's centre");

    /* motion: whole pixels to the sink, fractions carried */
    ptrlock_motion(3.5, -2.25); ptrlock_motion(0.5, -0.75); ptrlock_motion(0, 0);
    check(mx == 4 && my == -3 && moves == 2, "fractions carried to the next call");
    ptrlock_motion(-5000, 9000);
    check(mx == -4996 && my == 8997, "no clamping to any edge");

    /* the owner shows a pointer's image: drawn inside the node; the real pointer (hit tests, clicks) never moves */
    unsigned s1 = ptrlock_serial();
    int cx, cy;
    check(!ptrlock_cursor(&cx, &cy), "nothing shown: the image is at the pointer");
    ptrlock_show(1, 150.4f, 60.6f);
    check(!ptrlock_hidden() && ptrlock_serial() != s1 && ptrlock_cursor(&cx, &cy) && cx == 150 && cy == 61, "the image where the owner says");
    check(ptrlock_where(&x, &y) && x == 300 && y == 200, "the pointer itself stays at the centre");
    /* a guest's position can't take the pointer, or even its image, outside the machine's picture */
    const float far[][2] = { { 9000, -40 }, { -1e9f, 1e9f }, { 99, 49 }, { 500, 350 }, { 1e30f, -1e30f } };
    for (int i = 0; i < 5; i++) {
        ptrlock_show(1, far[i][0], far[i][1]);
        check(ptrlock_cursor(&cx, &cy) && cx >= 100 && cx <= 499 && cy >= 50 && cy <= 349, "the image is kept inside the node");
        check(ptrlock_where(&x, &y) && x == 300 && y == 200, "a guest's position never moves the pointer");
    }
    ptrlock_show(1, 9000, -40);
    check(ptrlock_cursor(&cx, &cy) && cx == 499 && cy == 50, "kept a pixel from the far edges");
    s1 = ptrlock_serial();
    ptrlock_show(1, 9000, -40);
    check(ptrlock_serial() == s1, "the same place again changes nothing");
    ptrlock_show(0, 0, 0);
    check(ptrlock_hidden() && !ptrlock_cursor(&cx, &cy) && ptrlock_where(&x, &y) && x == 300 && y == 200, "hidden again");
    /* no button sink: buttons and wheel go to the centre as usual */
    check(!ptrlock_button(0, 1) && !ptrlock_wants_wheel() && !ptrlock_wheel(1, 0), "no button sink: not taken");

    /* keys: ❖Esc ends it and is taken; Esc alone, a repeat or a release don't; Ctrl+Alt ends it and goes on */
    check(!ptrlock_key(KEY_ESC, 1, 0, 0, 0) && ptrlock_active(), "Esc alone goes to the owner");
    check(!ptrlock_key(KEY_ESC, 2, 1, 0, 0) && !ptrlock_key(KEY_ESC, 0, 1, 0, 0) && ptrlock_active(), "a repeat or release of ❖Esc does nothing");
    check(!ptrlock_key(KEY_LEFTCTRL, 1, 0, 1, 0) && ptrlock_active(), "Ctrl alone does nothing");
    check(!ptrlock_key(KEY_A, 1, 0, 1, 1) && ptrlock_active(), "Ctrl+Alt+A: only the modifiers count");
    check(ptrlock_key(KEY_ESC, 1, 1, 0, 0) == 1 && !ptrlock_active() && ends == 1 && why == PTRLOCK_END_KEYS, "❖Esc ends it, taken");
    int en = 0, ew = -1;
    check(ptrlock_take_end(&en, &ew) && en == 7 && ew == PTRLOCK_END_KEYS && !ptrlock_take_end(&en, &ew), "the end is told once (the shell's ❖ tap)");
    check(!ptrlock_key(KEY_ESC, 1, 1, 0, 0), "❖Esc with no lock goes on as before");
    check(ptrlock_begin(7, &a) == 0, "again");
    check(!ptrlock_key(KEY_RIGHTALT, 1, 0, 1, 1) && !ptrlock_active() && ends == 2 && why == PTRLOCK_END_KEYS, "Ctrl+Alt ends it, not taken");
    check(ptrlock_begin(7, &a) == 0 && !ptrlock_key(KEY_LEFTCTRL, 1, 0, 1, 1) && !ptrlock_active() && ends == 3, "Alt+Ctrl too");

    /* the owner ends it; another owner can't */
    check(ptrlock_begin(7, &a) == 0, "again");
    ptrlock_end_owner(&tag_b);
    check(ptrlock_active(), "another sink's end does nothing");
    ptrlock_end_owner(&tag_a);
    check(!ptrlock_active() && ends == 4 && why == PTRLOCK_END_OWNER, "its owner ends it");

    /* a new lock replaces the old one, which hears it */
    check(ptrlock_begin(7, &a) == 0 && ptrlock_begin(9, &js) == 0 && ends == 5 && why == PTRLOCK_END_REPLACED && ptrlock_node() == 9, "replaced");
    /* without a motion sink: gathered for JavaScript */
    int dx, dy;
    check(!ptrlock_take(&dx, &dy), "nothing gathered yet");
    ptrlock_motion(2, 3); ptrlock_motion(-7, 1);
    check(ptrlock_take(&dx, &dy) && dx == -5 && dy == 4 && !ptrlock_take(&dx, &dy), "gathered once a frame");
    check(ptrlock_retarget(&tag_a, 7) == -1 && ptrlock_node() == 9, "only its owner moves it");
    check(ptrlock_retarget(&tag_b, 7) == 0 && ptrlock_node() == 7 && ptrlock_where(&x, &y) && x == 300, "moved to a rebuilt window's node");
    check(ptrlock_retarget(&tag_b, 5) == -1 && ptrlock_node() == 7, "never to a node that isn't there");

    /* the node goes away: held for PTRLOCK_GONE_MS (the window may name its new node), then ended */
    gone[7] = 1;
    check(!ptrlock_where(&x, &y) && ptrlock_active() && ptrlock_waiting(), "gone: waiting");
    gone[7] = 0;
    check(ptrlock_where(&x, &y) && !ptrlock_waiting(), "back in time: held as before");
    gone[7] = 1;
    ptrlock_where(&x, &y);
    sleep_ms(PTRLOCK_GONE_MS + 50);
    check(!ptrlock_where(&x, &y) && !ptrlock_active() && why == PTRLOCK_END_GONE && ends == 6, "gone for good: ended");
    gone[7] = 0;

    /* a button sink (Machines): buttons and wheel straight to the owner, no hit test */
    check(ptrlock_begin(7, &vm) == 0 && ptrlock_wants_wheel(), "a lock with a button sink");
    check(ptrlock_button(0, 1) && ptrlock_button(0, 0) && ptrlock_button(2, 1) && nbtn == 3 && btn[0] == 0 && btn_down[0] == 1 && btn_down[1] == 0 && btn[2] == 2,
          "buttons taken by the owner, in order");
    check(ptrlock_wheel(-3, 0) && ptrlock_wheel(0, 2) && ptrlock_wheel(0, 0) && wy == -3 && wx == 2, "wheel steps taken");
    ptrlock_end_owner(&tag_c);
    check(ends == 7 && why == PTRLOCK_END_OWNER, "its owner ends it");
    check(!ptrlock_button(0, 1) && !ptrlock_wheel(1, 0) && nbtn == 3, "after the end, nothing is taken");

    /* the input went away (main.c's display_detach) */
    check(ptrlock_begin(9, &a) == 0, "again");
    ptrlock_end(PTRLOCK_END_INPUT);
    check(!ptrlock_active() && why == PTRLOCK_END_INPUT && ends == 8, "the input left: ended");
    ptrlock_end(PTRLOCK_END_INPUT);
    check(ends == 8, "ending twice tells once");
    ptrlock_motion(5, 5);
    ptrlock_show(1, 1, 1);
    check(!ptrlock_hidden() && !ptrlock_take(&dx, &dy), "no lock: motion and show do nothing");

    printf("pointerlock-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
