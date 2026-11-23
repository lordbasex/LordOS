/* shellx_units_test.c — the pure parts of the shell extras (src/wl/wlshell.h): the bell's rate limit (bellgate.h), a
 * blur region cut into rectangles (bgregion.h) and the session store and its file (xsstore.c). */
#include "wl/bellgate.h"
#include "wl/bgregion.h"
#include "wl/xsstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void bell(void) {
    BellGate g;
    memset(&g, 0, sizeof g);
    int a = 1, b = 2, n = 0;
    for (int i = 0; i < 10; i++) n += bell_pass(&g, &a, 1000);      /* a burst: one */
    CHECK(n == 1);
    CHECK(!bell_pass(&g, &b, 1050));                                /* any client: one every BELL_ANY_MS */
    CHECK(bell_pass(&g, &b, 1100));
    CHECK(!bell_pass(&g, &a, 1200));                                /* a: one every BELL_GAP_MS */
    CHECK(bell_pass(&g, &a, 1250));
    n = 0;                                                          /* a: at most BELL_BURST in BELL_WINDOW_MS */
    for (uint32_t t = 1500; t < 9000; t += 300) n += bell_pass(&g, &a, t);
    CHECK(n == BELL_BURST - 2);
    CHECK(bell_pass(&g, &a, 1000 + BELL_WINDOW_MS + 10));           /* a new window */
    for (int i = 0; i < 40; i++) bell_pass(&g, (void *)(intptr_t)(100 + i), 20000u + 200u * (uint32_t)i);   /* slots reused */
    CHECK(bell_pass(&g, &a, 40000));
}

static void region(void) {
    WlRgn r = { 0 };
    int out[16][4];
    CHECK(bgr_rects(&r, 0, 0, 100, 100, out, 16) == 0);
    wlrgn_push(&r, 0, 10, 10, 100, 50);
    CHECK(bgr_rects(&r, 0, 0, 300, 200, out, 16) == 1);
    CHECK(out[0][0] == 10 && out[0][1] == 10 && out[0][2] == 100 && out[0][3] == 50);
    /* clipped to the geometry and relative to it */
    CHECK(bgr_rects(&r, 20, 20, 50, 50, out, 16) == 1);
    CHECK(out[0][0] == 0 && out[0][1] == 0 && out[0][2] == 50 && out[0][3] == 40);
    /* a hole cut out of the middle: four rectangles around it */
    wlrgn_free(&r);
    wlrgn_push(&r, 0, 0, 0, 90, 90);
    wlrgn_push(&r, 1, 30, 30, 30, 30);
    int n = bgr_rects(&r, 0, 0, 90, 90, out, 16), area = 0;
    for (int i = 0; i < n; i++) area += out[i][2] * out[i][3];
    CHECK(n == 4 && area == 90 * 90 - 30 * 30);
    /* more than max: the bounding box */
    CHECK(bgr_rects(&r, 0, 0, 90, 90, out, 2) == 1 && out[0][2] == 90 && out[0][3] == 90);
    /* a rounded panel (steps at the corners) stays a few rectangles */
    wlrgn_free(&r);
    wlrgn_push(&r, 0, 4, 0, 192, 4);
    wlrgn_push(&r, 0, 0, 4, 200, 92);
    wlrgn_push(&r, 0, 4, 96, 192, 4);
    CHECK(bgr_rects(&r, 0, 0, 200, 100, out, 16) == 3);
    /* past BGR_OPS rectangles: the bounding box of what was added */
    wlrgn_free(&r);
    for (int i = 0; i < BGR_OPS + 1; i++) wlrgn_push(&r, 0, i * 2, 0, 1, 1);
    CHECK(bgr_rects(&r, 0, 0, 1000, 1000, out, 16) == 1 && out[0][2] == BGR_OPS * 2 + 1);
    /* nothing inside the geometry */
    wlrgn_free(&r);
    wlrgn_push(&r, 0, 500, 500, 10, 10);
    CHECK(bgr_rects(&r, 0, 0, 100, 100, out, 16) == 0);
    wlrgn_free(&r);
}

static void store(void) {
    XsStore st = { 0 };
    char id[XS_ID_LEN + 1], id2[XS_ID_LEN + 1];
    unsigned char rnd[16];
    for (int i = 0; i < 16; i++) rnd[i] = (unsigned char)(i * 17);
    xs_new_id(rnd, id);
    CHECK(xs_id_ok(id) && strlen(id) == 32);
    CHECK(!xs_id_ok("ABCDEF0123456789abcdef0123456789"));   /* upper case is not ours */
    CHECK(!xs_id_ok("../../etc"));
    CHECK(xs_owner_ok("program:/usr/bin/firefox") && xs_owner_ok("flatpak:org.mozilla.firefox") && xs_owner_ok("app:notes"));
    CHECK(!xs_owner_ok("") && !xs_owner_ok("firefox") && !xs_owner_ok("program:relative") && !xs_owner_ok("program:/a\nb"));
    XsSess *s = xs_new(&st, id, "program:/usr/bin/firefox", 100);
    CHECK(s && s->persist);
    /* found only with its owner */
    CHECK(xs_find(&st, id, "program:/usr/bin/firefox") == s);
    CHECK(!xs_find(&st, id, "program:/usr/bin/other"));
    CHECK(!xs_find(&st, id, ""));
    XsTop *t = xs_top_put(s, "main window");
    CHECK(t && !xs_top_has(t));
    xs_top_set(t, (int[]){ 10, 20, 800, 600 }, 0);
    CHECK(xs_top_has(t));
    CHECK(!xs_top_put(s, "bad\nname"));
    char longname[XS_NAME_MAX + 2];
    memset(longname, 'x', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    CHECK(!xs_top_put(s, longname));
    xs_top_set(xs_top_put(s, "prefs"), (int[]){ -50, 5, 300, 200 }, 1);
    /* no owner: kept in memory, never saved nor found */
    rnd[0] = 1;
    xs_new_id(rnd, id2);
    XsSess *anon = xs_new(&st, id2, "", 100);
    CHECK(anon && !anon->persist && !xs_find(&st, id2, ""));
    /* the file round trip */
    size_t len = 0;
    char *text = xs_dump(&st, &len);
    CHECK(text && strstr(text, "T 10 20 800 600 0 main window\n") && !strstr(text, id2));
    XsStore back = { 0 };
    CHECK(xs_load(&back, text, len) == 1);
    XsSess *b = xs_find(&back, id, "program:/usr/bin/firefox");
    CHECK(b && b->ntop == 2 && xs_top(b, "prefs") && xs_top(b, "prefs")->max == 1 && xs_top(b, "prefs")->r[0] == -50);
    free(text);
    /* a file edited by hand: every bad line skipped, a bad session with its windows */
    const char *edited = "lordos-wl-sessions 1\n"
                         "S 0123456789abcdef0123456789abcdef 5 program:/usr/bin/a\n"
                         "T 1 2 3 4 0 ok\n"
                         "T 1 2 -3 4 0 negative size\n"
                         "T 1 2 3 4 7 bad max\n"
                         "T 999999999 2 3 4 0 too far\n"
                         "T 1 2 3 4 0 ok\n"                       /* the same name twice: the first one stays */
                         "S NOTANID 5 program:/usr/bin/a\n"
                         "T 1 2 3 4 0 orphan\n"
                         "S 0123456789abcdef0123456789abcdee 5 not-an-owner\n"
                         "T 1 2 3 4 0 orphan2\n";
    CHECK(xs_load(&back, edited, strlen(edited)) == 1);
    b = xs_find(&back, "0123456789abcdef0123456789abcdef", "program:/usr/bin/a");
    CHECK(b && b->ntop == 1 && !strcmp(b->top[0].name, "ok"));
    CHECK(xs_load(&back, "something else\n", 15) == 0);
    CHECK(xs_load(&back, "lordos-wl-sessions 1\nS 0123456789abcdef0123456789abcdef 5 program:/a\0x", 76) == 0);   /* a NUL */
    /* per owner at most XS_PER_OWNER: the least recently used goes, not one in use */
    xs_clear(&st);
    XsSess *first = NULL;
    for (int i = 0; i < XS_PER_OWNER + 3; i++) {
        rnd[1] = (unsigned char)i;
        xs_new_id(rnd, id);
        XsSess *n = xs_new(&st, id, "app:notes", 1000 + i);
        CHECK(n);
        if (!i) { first = n; n->inuse = 1; }
    }
    int mine = 0;
    for (int i = 0; i < st.n; i++) if (!strcmp(st.s[i]->owner, "app:notes")) mine++;
    CHECK(mine == XS_PER_OWNER);
    int kept = 0;
    for (int i = 0; i < st.n; i++) if (st.s[i] == first) kept = 1;
    CHECK(kept);
    /* apps with no owner share one owner's limit: they can't push the other apps' sessions out */
    for (int i = 0; i < 3 * XS_SESSIONS; i++) {
        rnd[2] = (unsigned char)i;
        rnd[3] = (unsigned char)(i >> 8);
        xs_new_id(rnd, id);
        CHECK(xs_new(&st, id, "", 5000 + i));
    }
    mine = 0;
    for (int i = 0; i < st.n; i++) if (!strcmp(st.s[i]->owner, "app:notes")) mine++;
    CHECK(mine == XS_PER_OWNER && st.n == 2 * XS_PER_OWNER);
    xs_clear(&st);
    xs_clear(&back);
}

/* xs_top_note, what xsession.c's snapshots do: the regression of a window maximized from its first frame to its close
 * (its app remembered maximized), which never got a place, so it was neither restored nor saved. */
static void note(void) {
    XsStore st = { 0 }, back = { 0 };
    char id[XS_ID_LEN + 1];
    unsigned char rnd[16];
    for (int i = 0; i < 16; i++) rnd[i] = (unsigned char)(200 - i);
    xs_new_id(rnd, id);
    XsSess *s = xs_new(&st, id, "program:/usr/bin/editor", 1);
    CHECK(s);
    /* maximized at every snapshot (each tick and the close): it still has a place, and it is maximized */
    XsTop *t = xs_top_put(s, "primary");
    CHECK(xs_top_note(t, (int[]){ 0, 28, 1280, 690 }, 1, 0) == 1);
    CHECK(xs_top_has(t) && t->max == 1 && t->r[0] == 0 && t->r[1] == 28 && t->r[2] == 1280 && t->r[3] == 690);
    CHECK(xs_top_note(t, (int[]){ 0, 28, 1280, 690 }, 1, 0) == 0);           /* nothing new: not dirty */
    size_t len = 0;
    char *text = xs_dump(&st, &len);
    CHECK(text && strstr(text, "T 0 28 1280 690 1 primary\n"));             /* saved, so it outlives lord-server too */
    CHECK(xs_load(&back, text, len) == 1 && xs_top_has(xs_top(xs_find(&back, id, "program:/usr/bin/editor"), "primary")));
    free(text);
    /* back to normal: its own place, not maximized; maximized again: that normal place is kept */
    CHECK(xs_top_note(t, (int[]){ 100, 120, 800, 600 }, 0, 0) == 1 && t->max == 0 && t->r[0] == 100 && t->r[2] == 800);
    CHECK(xs_top_note(t, (int[]){ 0, 28, 1280, 690 }, 1, 0) == 1 && t->max == 1 && t->r[0] == 100 && t->r[2] == 800);
    /* fullscreen: the entry as it was */
    CHECK(xs_top_note(t, (int[]){ 0, 0, 1280, 800 }, 1, 1) == 0 && t->max == 1 && t->r[2] == 800);
    CHECK(xs_top_note(t, (int[]){ 0, 0, 1280, 800 }, 0, 1) == 0 && t->r[2] == 800);
    /* fullscreen since it opened: its rect, so it can come back, not marked maximized */
    XsTop *f = xs_top_put(s, "game");
    CHECK(xs_top_note(f, (int[]){ 0, 0, 1280, 800 }, 0, 1) == 1 && xs_top_has(f) && f->max == 0);
    /* a normal window: as before */
    XsTop *n = xs_top_put(s, "prefs");
    CHECK(xs_top_note(n, (int[]){ 5, 6, 300, 200 }, 0, 0) == 1 && xs_top_has(n) && n->max == 0);
    CHECK(xs_top_note(NULL, (int[]){ 5, 6, 300, 200 }, 0, 0) == 0);          /* xs_top_put full or name not storable */
    xs_clear(&st);
    xs_clear(&back);
}

int main(void) {
    bell();
    region();
    store();
    note();
    if (fails) { printf("shellx-units: %d failed\n", fails); return 1; }
    printf("shellx-units: ok\n");
    return 0;
}
