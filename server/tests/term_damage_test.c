/* term-damage-test — the terminal's damage (term.c term_damage, 0.8.8.10): what a program writes is compared with
 * what was drawn, and only the cells that changed are repainted. A real session (term_create_fd on a pipe) fed like a
 * TUI: a spinner rewriting its row, the same screen written again, a cursor blinking and moving, colours, a scroll, a
 * translucent background. Every frame painted in part must equal a full repaint, and the parts must be small. */
#include "../src/term/term.h"
#include "../src/ui/ui.h"
#include "../src/canvas.h"
#include "../src/ui/fonts.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* the compositor's other content, not in this test */
void wl_paint(Canvas *c, int id, int node, float x, float y, float w, float h, float op) { (void)c; (void)id; (void)node; (void)x; (void)y; (void)w; (void)h; (void)op; }
void wl_paint_holes(int on) { (void)on; }
void wl_paint_kept(void) {}
void wl_paint_keep(void) {}
void wl_paint_begin(void) {}
void wl_paint_save(void) {}
void wl_paint_restore(void) {}
void wl_paint_popups(Canvas *c) { (void)c; }
void wl_tf_begin(int node, float sx, float sy, float tx, float ty, float op, int skip) { (void)node; (void)sx; (void)sy; (void)tx; (void)ty; (void)op; (void)skip; }
void wl_tf_end(void) {}
void wl_tf_nested(int d) { (void)d; }
void wl_tf_open_holes(Canvas *c) { (void)c; }
int wl_draws_meet(Rect r) { (void)r; return 0; }
void media_paint(Canvas *c, int id, int node, float x, float y, float w, float h, float r, float op) { (void)c; (void)id; (void)node; (void)x; (void)y; (void)w; (void)h; (void)r; (void)op; }
void media_paint_note(Canvas *c, int id, int node, float x, float y, float w, float h, float op) { (void)c; (void)id; (void)node; (void)x; (void)y; (void)w; (void)h; (void)op; }
int media_cover_node(float x0, float y0, float x1, float y1) { (void)x0; (void)y0; (void)x1; (void)y1; return 0; }
int media_id_protected(int id) { (void)id; return 0; }
int gpu_active(void) { return 0; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t *fb;
static int FW, FH;
static Canvas frame;

static void render(Node *root, int w, int h) {
    if (FW != w || FH != h) { free(fb); fb = calloc((size_t)w * h, 4); FW = w; FH = h; }
    canvas_init(&frame, fb, w, h, w);
    layout_root(root, (float)w, (float)h);
    paint_damage(root);
    ui.damage = (Box){ 0, 0, (float)w, (float)h };
    paint_root_clear(&frame, root, 0);
    ui.damage = (Box){ 0, 0, 0, 0 };
}

/* the damage of this frame painted rectangle by rectangle; the area painted */
static float paint_partial(Node *root, int w, int h) {
    layout_root(root, (float)w, (float)h);
    if (!paint_damage(root)) return 0;
    Box rs[DAMAGE_MAX];
    int n = paint_damage_rects(rs, DAMAGE_MAX);
    float area = 0;
    for (int i = 0; i < n; i++) {
        Rect r = { (int)fmaxf(0, floorf(rs[i].x0)), (int)fmaxf(0, floorf(rs[i].y0)), (int)fminf((float)w, ceilf(rs[i].x1)), (int)fminf((float)h, ceilf(rs[i].y1)) };
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        area += (float)(r.x1 - r.x0) * (r.y1 - r.y0);
        ui.damage = (Box){ (float)r.x0, (float)r.y0, (float)r.x1, (float)r.y1 };
        canvas_set_clip(&frame, r);
        paint_root_clear(&frame, root, 0);
    }
    canvas_set_clip(&frame, (Rect){ 0, 0, w, h });
    ui.damage = (Box){ 0, 0, 0, 0 };
    return area;
}

static int full_differs(Node *root, int w, int h) {
    static uint32_t *ref;
    if (!ref) ref = calloc((size_t)w * h, 4);
    Canvas rc;
    canvas_init(&rc, ref, w, h, w);
    ui.damage = (Box){ 0, 0, (float)w, (float)h };
    paint_root_clear(&rc, root, 0);
    ui.damage = (Box){ 0, 0, 0, 0 };
    int worst = 0;
    for (int i = 0; i < w * h; i++)
        for (int sh = 0; sh < 24; sh += 8) {
            int dv = abs((int)((ref[i] >> sh) & 255) - (int)((fb[i] >> sh) & 255));
            if (dv > worst) worst = dv;
        }
    return worst;
}

static void feed(Session *s, const char *txt) { term_feed(s, txt, strlen(txt)); }

int main(void) {
    enum { W = 900, H = 420 };
    const char *dir = getenv("LORD_FONTS") ? getenv("LORD_FONTS") : "../assets/fonts";
    fonts_set_builtin_dir(dir);
    if (text_init(dir)) { printf("term-damage-test: skipped: needs the fonts in %s\n", dir); return 0; }
    term_init(dir, "../assets/shell");
    int fd[2];
    if (pipe(fd)) return 1;
    Session *s = term_create_fd(fd[0], 1000);
    CHECK(s != NULL, "session");
    if (!s) return 1;
    ui_reset();
    Node *root = node_new(NODE_VIEW);
    ui.root = root;
    style_set_number(&root->s, "backgroundColor", (double)0xff203040);
    Node *tn = node_new(NODE_TERM);
    tn->term = s->id;
    s->node = tn->id;
    node_append(root, tn);
    style_set_number(&tn->s, "left", 20); style_set_number(&tn->s, "top", 20);
    style_set_number(&tn->s, "width", 860); style_set_number(&tn->s, "height", 380);
    style_set_string(&tn->s, "position", "absolute");
    render(root, W, H);
    term_damage(); paint_partial(root, W, H);      /* the grid's size settles */
    for (int i = 0; i < 12; i++) { char b[96]; snprintf(b, sizeof b, "line %d: some output of a program, ls -la and friends\r\n", i); feed(s, b); }
    feed(s, "\033[33m✻ Working… (12s · esc to interrupt)\033[0m\r\n> ");
    term_damage(); paint_partial(root, W, H);
    float term_area = 860.0f * 380.0f;
    const char *spin[] = { "✻", "✶", "✳", "✢" };
    int bad = 0, small = 0, frames = 0;
    for (int f = 0; f < 48; f++) {
        char b[256];
        int step = f % 8;
        if (step < 4) {      /* the TUI writes its live line again: the spinner and the seconds change, the rest is the same */
            snprintf(b, sizeof b, "\0337\033[13;1H\033[33m%s Working… (%ds · esc to interrupt)\033[0m\0338", spin[f % 4], 12 + f / 4);
            feed(s, b);
        } else if (step == 4) {   /* the same screen written again, unchanged */
            feed(s, "\0337\033[3;1Hline 2: some output of a program, ls -la and friends\0338");
        } else if (step == 5) {   /* typing */
            feed(s, "a");
        } else if (step == 6) {   /* the cursor blinks (term_tick's mark) */
            vt_mark_dirty(&s->t, s->t.cy);
            s->blink_on = !s->blink_on;
        } else {                  /* a colour change in the middle of a row */
            feed(s, "\0337\033[5;10H\033[1;35mSOME\033[0m\0338");
        }
        term_damage();
        float area = paint_partial(root, W, H);
        int worst = full_differs(root, W, H);
        if (worst > 2) { if (bad < 3) printf("  frame %d (step %d): differs from a full repaint by %d\n", f, step, worst); bad++; }
        frames++;
        small += area < term_area * 0.08f;
        if (f < 8) printf("  step %d: %.0f px repainted (%.1f %% of the terminal)\n", step, area, area * 100 / term_area);
    }
    CHECK(!bad, "%d frames differ from a full repaint", bad);
    CHECK(small >= frames - 2, "%d of %d frames repainted under 8 %% of the terminal", small, frames);
    /* a scroll repaints everything; then small again */
    feed(s, "\r\nnew line at the bottom\r\n");
    term_damage(); paint_partial(root, W, H);
    CHECK(full_differs(root, W, H) <= 2, "after a scroll");
    feed(s, "x");
    term_damage();
    float a2 = paint_partial(root, W, H);
    CHECK(a2 < term_area * 0.08f && full_differs(root, W, H) <= 2, "typing after a scroll: %.0f px", a2);
    /* translucent: still small, still right */
    s->prof.bg_opacity = 0.7f;
    s->t.all_dirty = 1;
    term_damage(); paint_partial(root, W, H);
    int tb = 0, ts = 0;
    for (int f = 0; f < 8; f++) {
        char b[160];
        snprintf(b, sizeof b, "\0337\033[13;1H\033[33m%s Working… (%ds · esc to interrupt)\033[0m\0338", spin[f % 4], 40 + f);
        feed(s, b);
        term_damage();
        float area = paint_partial(root, W, H);
        tb += full_differs(root, W, H) > 2;
        ts += area < term_area * 0.08f;
    }
    CHECK(!tb, "translucent: %d frames differ", tb);
    CHECK(ts >= 7, "translucent: %d of 8 frames small", ts);
    /* random writes: positions, colours, wide characters, erasing, inserting and deleting cells and lines, scroll
     * regions, the cursor's shape and visibility; every frame equal to a full repaint */
    s->prof.bg_opacity = 1;
    s->t.all_dirty = 1;                                 /* as a profile change does */
    term_damage(); paint_partial(root, W, H);
    uint32_t seed = getenv("TD_SEED") ? (uint32_t)atoi(getenv("TD_SEED")) : 7;
    int nrand = getenv("TD_FRAMES") ? atoi(getenv("TD_FRAMES")) : 400;
    int rb = 0;
    const char *bits[] = { "a", "Hola", "\033[31m", "\033[0m", "\033[1;44m", "日本", "😀", "\033[K", "\033[2K", "\033[1J", "\033[3P",
                           "\033[2@", "\033[L", "\033[M", "\033[3X", "\r", "\n", "\b", "\t", "\033[5 q", "\033[2 q", "\033[?25l", "\033[?25h",
                           "\033[4m", "\033[7m", "->", "=>", "\033[S", "\033[T", "\033[3;10r", "\033[r", "─┼─", "ñandú", "\033[5;5H" };
    for (int f = 0; f < nrand; f++) {
        char b[96];
        int n = 1 + (int)((seed = seed * 1103515245u + 12345u) >> 16) % 4;
        for (int i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            unsigned r = seed >> 8;
            if (r % 3 == 0) { snprintf(b, sizeof b, "\033[%u;%uH", 1 + r / 3 % 18, 1 + r / 57 % 70); feed(s, b); }
            feed(s, bits[r / 7 % (sizeof bits / sizeof *bits)]);
        }
        if (f % 5 == 0) { vt_mark_dirty(&s->t, s->t.cy); s->blink_on = !s->blink_on; }
        if (f == 200) term_focus(s, 1);
        term_damage();
        paint_partial(root, W, H);
        int worst = full_differs(root, W, H);
        if (worst > 2) { if (rb < 3) printf("  random frame %d differs by %d\n", f, worst); rb++; }
    }
    CHECK(!rb, "random writes: %d of %d frames differ from a full repaint", rb, nrand);
    printf("term-damage-test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
