/* lbui.c — see lbui.h. */
#include "lbui.h"
#include "lbtext.h"
#include "lbwork.h"
#include "bench.h"
#include "lb_fonts.h"
#include "../ui/ui.h"
#include "../ui/bidi.h"
#include "../ui/shape.h"
#include "../ui/unicode.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- what paint.c reaches outside the UI engine: lord-bench has no terminals, Wayland windows, media or
 * GPU path (the same stubs as tests/engine_test.c) ---- */
void term_paint(Canvas *c, int id, float x, float y, float w, float h, float op) { (void)c; (void)id; (void)x; (void)y; (void)w; (void)h; (void)op; }
void wl_paint(Canvas *c, int id, int node, float x, float y, float w, float h, float op) { (void)c; (void)id; (void)node; (void)x; (void)y; (void)w; (void)h; (void)op; }
void wl_paint_holes(int on) { (void)on; }
void wl_paint_kept(void) {}
void wl_paint_keep(void) {}
void wl_paint_save(void) {}
void wl_paint_restore(void) {}
int  wl_draws_meet(Rect r) { (void)r; return 0; }
void wl_paint_begin(void) {}
void wl_paint_popups(Canvas *c) { (void)c; }
void media_paint(Canvas *c, int id, int node, float x, float y, float w, float h, float r, float op) { (void)c; (void)id; (void)node; (void)x; (void)y; (void)w; (void)h; (void)r; (void)op; }
int media_cover_node(float x0, float y0, float x1, float y1) { (void)x0; (void)y0; (void)x1; (void)y1; return 0; }
int gpu_active(void) { return 0; }

/* ---- the scene ---- */

static Node *root;
static int unknown_styles, ready;

static Node *view(Node *parent) {
    Node *n = node_new(NODE_VIEW);
    if (n && parent) node_append(parent, n);
    return n;
}
static void sn(Node *n, const char *k, double v) { if (style_set_number(&n->s, k, v)) unknown_styles++; }
static void ss(Node *n, const char *k, const char *v) { if (style_set_string(&n->s, k, v)) unknown_styles++; }
static void pad(Node *n, double v) { sn(n, "paddingLeft", v); sn(n, "paddingRight", v); sn(n, "paddingTop", v); sn(n, "paddingBottom", v); }
static Node *text(Node *parent, const char *s, double size, uint32_t color) {
    Node *t = node_new(NODE_TEXT), *r = node_new(NODE_RAW);
    node_set_text(r, s);
    node_append(t, r);
    node_append(parent, t);
    sn(t, "fontSize", size);
    sn(t, "color", color);
    return t;
}
static Node *box(Node *parent, double w, double h, double radius, uint32_t color) {
    Node *b = view(parent);
    sn(b, "width", w); sn(b, "height", h); sn(b, "borderRadius", radius); sn(b, "backgroundColor", color);
    return b;
}
static Node *window(Node *parent, double x, double y, double w, double h, uint32_t bg) {
    Node *n = view(parent);
    ss(n, "position", "absolute");
    sn(n, "left", x); sn(n, "top", y); sn(n, "width", w); sn(n, "height", h);
    sn(n, "backgroundColor", bg); sn(n, "borderRadius", 12);
    sn(n, "shadowColor", 0x59000000); sn(n, "shadowBlur", 28); sn(n, "shadowOffsetY", 10);
    ss(n, "overflow", "hidden");
    return n;
}

/* A desktop as LordOS draws it: a translucent top bar, a file manager with a sidebar and a 40-row list, a
 * settings window with a wrapped paragraph and controls, a notification and the Dock, both blurring what is
 * behind them, a rotated badge and a half-transparent card. Fixed data only, and text only in characters Inter
 * covers: anything else would make the engine look for a fallback font, by path (textlayout.c). Every weight is
 * drawn with the one built-in Inter Regular (text_init_mem). */
static void build_scene(void) {
    static const char *const words[] = { "Quarterly", "report", "holiday", "photos", "final", "version", "backup", "draft",
                                         "invoice", "scan", "notes", "presentation", "budget", "summary" };
    static const char *const places[] = { "Home", "Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos",
                                          "Trash", "This computer", "Network", "USB disk", "Recent", "Starred", "Shared" };
    static const char *const menu[] = { "LordOS", "Files", "Edit", "View", "Go", "Window", "Help" };
    static const char *const labels[] = { "Wallpaper", "Accent colour", "Appearance", "Text size", "Night light", "Animations",
                                          "Notifications", "Sounds", "Hot corners", "Dock size" };
    static const uint32_t icon_colors[] = { 0xff0a84ff, 0xff30d158, 0xffff9f0a, 0xffff453a, 0xffbf5af2, 0xff64d2ff,
                                            0xffffd60a, 0xffac8e68, 0xff5e5ce6, 0xffff375f, 0xff66d4cf, 0xff8e8e93 };
    ui_reset();
    unknown_styles = 0;
    root = view(NULL);
    ui.root = root;
    sn(root, "width", LB_UI_W); sn(root, "height", LB_UI_H);
    sn(root, "backgroundColor", 0xff2b3a4a);
    /* wallpaper shapes, so the blurs have something to blur */
    for (int i = 0; i < 6; i++) {
        Node *b = box(root, 520 - i * 40, 520 - i * 40, 260 - i * 20, icon_colors[i] & 0x80ffffff);
        ss(b, "position", "absolute");
        sn(b, "left", 80 + i * 300); sn(b, "top", 120 + (i % 3) * 260);
    }

    /* top bar */
    Node *bar = view(root);
    ss(bar, "position", "absolute");
    sn(bar, "left", 0); sn(bar, "top", 0); sn(bar, "width", LB_UI_W); sn(bar, "height", 30);
    sn(bar, "backgroundColor", 0x99202020); sn(bar, "backdropBlur", 20);
    ss(bar, "flexDirection", "row"); ss(bar, "alignItems", "center");
    sn(bar, "paddingLeft", 14); sn(bar, "paddingRight", 14); sn(bar, "gap", 18);
    for (int i = 0; i < 7; i++) { Node *t = text(bar, menu[i], 13, 0xffffffff); if (!i) sn(t, "fontWeight", 600); }
    Node *spring = view(bar);
    sn(spring, "flexGrow", 1);
    text(bar, "Wed 28 Oct  16:20", 13, 0xffffffff);

    /* the file manager */
    Node *files = window(root, 140, 80, 1180, 780, 0xfff5f5f7);
    Node *title = view(files);
    sn(title, "height", 44); ss(title, "flexDirection", "row"); ss(title, "alignItems", "center");
    sn(title, "paddingLeft", 16); sn(title, "gap", 8); sn(title, "backgroundColor", 0xffebebef);
    sn(title, "borderBottomWidth", 1); sn(title, "borderBottomColor", 0xffd4d4d8);
    box(title, 12, 12, 6, 0xffff5f57); box(title, 12, 12, 6, 0xfffebc2e); box(title, 12, 12, 6, 0xff28c840);
    Node *tt = text(title, "Documents", 14, 0xff202020);
    sn(tt, "marginLeft", 20); sn(tt, "fontWeight", 600);
    Node *body = view(files);
    sn(body, "flexGrow", 1); ss(body, "flexDirection", "row");
    Node *side = view(body);
    sn(side, "width", 220); sn(side, "backgroundColor", 0xffe6e6eb); sn(side, "paddingTop", 12); sn(side, "gap", 2);
    for (int i = 0; i < 14; i++) {
        Node *r = view(side);
        ss(r, "flexDirection", "row"); ss(r, "alignItems", "center");
        sn(r, "height", 28); sn(r, "paddingLeft", 14); sn(r, "gap", 10);
        if (i == 2) { sn(r, "backgroundColor", 0x330a84ff); sn(r, "borderRadius", 6); sn(r, "marginLeft", 6); sn(r, "marginRight", 6); }
        box(r, 16, 16, 4, icon_colors[i % 12]);
        text(r, places[i], 13, 0xff303030);
    }
    Node *main = view(body);
    sn(main, "flexGrow", 1);
    Node *tools = view(main);
    sn(tools, "height", 40); ss(tools, "flexDirection", "row"); ss(tools, "alignItems", "center");
    sn(tools, "paddingLeft", 12); sn(tools, "gap", 8); sn(tools, "borderBottomWidth", 1); sn(tools, "borderBottomColor", 0xffd8d8dc);
    for (int i = 0; i < 5; i++) { Node *b = box(tools, 28, 24, 6, 0xffffffff); sn(b, "borderWidth", 1); sn(b, "borderColor", 0xffc8c8cc); }
    Node *search = box(tools, 240, 26, 13, 0xffffffff);
    sn(search, "marginLeft", 300); sn(search, "borderWidth", 1); sn(search, "borderColor", 0xffc0c0c4);
    Node *list = view(main);
    sn(list, "flexGrow", 1); ss(list, "overflow", "hidden");
    char name[256];
    uint64_t seed = 7;
    for (int i = 0; i < 40; i++) {
        Node *row = view(list);
        ss(row, "flexDirection", "row"); ss(row, "alignItems", "center");
        sn(row, "height", 26); sn(row, "paddingLeft", 12); sn(row, "paddingRight", 12); sn(row, "gap", 8);
        if (i & 1) sn(row, "backgroundColor", 0xffededf0);
        box(row, 16, 16, 3, icon_colors[(i * 5) % 12]);
        int len = 0;
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        int nw = 2 + (int)(seed >> 60) % 9;
        for (int k = 0; k < nw; k++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            len += snprintf(name + len, sizeof name - (size_t)len, "%s%s", k ? " " : "", words[(seed >> 33) % 14]);
        }
        snprintf(name + len, sizeof name - (size_t)len, " %d.pdf", i);
        Node *t = text(row, name, 13, 0xff202020);
        sn(t, "flexGrow", 1); sn(t, "flexShrink", 1); sn(t, "flexBasis", 0);
        ss(t, "whiteSpace", "nowrap"); ss(t, "textOverflow", "ellipsis");
        Node *size = text(row, "1.2 MB", 12, 0xff707070);
        sn(size, "width", 70); ss(size, "textAlign", "end");
        Node *date = text(row, "18 Oct 2026, 04:20", 12, 0xff707070);
        sn(date, "width", 140);
    }
    Node *status = view(main);
    sn(status, "height", 26); sn(status, "paddingLeft", 12); ss(status, "justifyContent", "center");
    sn(status, "borderTopWidth", 1); sn(status, "borderTopColor", 0xffd8d8dc);
    text(status, "40 items, 1.4 GB free", 12, 0xff707070);

    /* a settings window over it: a wrapped paragraph and a column of controls */
    Node *set = window(root, 880, 260, 640, 600, 0xffffffff);
    pad(set, 24); sn(set, "gap", 10);
    Node *st = text(set, "Appearance", 22, 0xff1c1c1e);
    sn(st, "fontWeight", 700);
    Node *para = text(set, lb_paragraphs[LB_PARA_LATIN], 13, 0xff48484a);
    sn(para, "lineHeight", 1.45);
    for (int i = 0; i < 10; i++) {
        Node *r = view(set);
        ss(r, "flexDirection", "row"); ss(r, "alignItems", "center"); sn(r, "height", 30); sn(r, "gap", 12);
        Node *l = text(r, labels[i], 13, 0xff1c1c1e);
        sn(l, "width", 150);
        Node *field = box(r, 0, 28, 6, 0xfff2f2f7);
        sn(field, "flexGrow", 1); sn(field, "borderWidth", 1); sn(field, "borderColor", 0xffc7c7cc);
        Node *tog = box(r, 44, 24, 12, i % 3 ? 0xff34c759 : 0xffd1d1d6);
        ss(tog, "flexDirection", "row"); ss(tog, "justifyContent", i % 3 ? "flex-end" : "flex-start"); pad(tog, 2);
        Node *knob = box(tog, 20, 20, 10, 0xffffffff);
        sn(knob, "shadowColor", 0x40000000); sn(knob, "shadowBlur", 3); sn(knob, "shadowOffsetY", 1);
    }

    /* a notification with blur, a half-transparent card and a rotated badge */
    Node *note = window(root, 1500, 44, 390, 110, 0xd0f8f8fa);
    sn(note, "backdropBlur", 24); sn(note, "borderRadius", 16); pad(note, 14); sn(note, "gap", 4);
    Node *nt = text(note, "LordBench", 14, 0xff1c1c1e);
    sn(nt, "fontWeight", 600);
    text(note, "Measuring the interface: layout and paint of a fixed scene, then shaping five scripts.", 13, 0xff3a3a3c);
    Node *card = window(root, 1420, 620, 420, 240, 0xff1c1c1e);
    sn(card, "opacity", 0.9); pad(card, 18); sn(card, "gap", 8);
    for (int i = 0; i < 5; i++) {
        Node *r = view(card);
        ss(r, "flexDirection", "row"); sn(r, "gap", 10); ss(r, "alignItems", "center");
        box(r, 140 - i * 18, 10, 5, icon_colors[i]);
        text(r, menu[i], 12, 0xffd0d0d0);
    }
    Node *badge = box(root, 120, 36, 18, 0xffff375f);
    ss(badge, "position", "absolute"); sn(badge, "left", 1330); sn(badge, "top", 210); sn(badge, "rotate", 8);
    ss(badge, "alignItems", "center"); ss(badge, "justifyContent", "center");
    text(badge, "New", 14, 0xffffffff);

    /* the Dock */
    Node *dock = view(root);
    ss(dock, "position", "absolute");
    sn(dock, "left", 560); sn(dock, "top", 990); sn(dock, "width", 800); sn(dock, "height", 76);
    sn(dock, "backgroundColor", 0x80ffffff); sn(dock, "borderRadius", 22); sn(dock, "backdropBlur", 24);
    ss(dock, "flexDirection", "row"); ss(dock, "alignItems", "center"); ss(dock, "justifyContent", "center"); sn(dock, "gap", 12);
    for (int i = 0; i < 12; i++) {
        Node *ic = box(dock, 52, 52, 12, icon_colors[i]);
        sn(ic, "shadowColor", 0x40000000); sn(ic, "shadowBlur", 6); sn(ic, "shadowOffsetY", 2);
    }
}

/* ---- paint threads: the same row bands as lord-server's pool (par.c), without its priority ---- */

typedef void (*BandFn)(void *ctx, int a, int b);
static struct {
    pthread_mutex_t mu;
    pthread_cond_t go, done;
    unsigned long gen;
    BandFn fn;
    void *ctx;
    int j0, n, pieces, threads, stop;
    atomic_int next, left;
    pthread_t th[LB_UI_POOL_MAX];
} pool = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, NULL, NULL, 0, 0, 0, 1, 0, 0, 0, { 0 } };
static _Thread_local int in_pool;

static void pool_pieces(void) {
    in_pool = 1;
    for (;;) {
        int p = atomic_fetch_add(&pool.next, 1);
        if (p >= pool.pieces) break;
        int a = pool.j0 + (int)((long)pool.n * p / pool.pieces), b = pool.j0 + (int)((long)pool.n * (p + 1) / pool.pieces);
        if (b > a) pool.fn(pool.ctx, a, b);
        if (atomic_fetch_sub(&pool.left, 1) == 1) {
            pthread_mutex_lock(&pool.mu);
            pthread_cond_signal(&pool.done);
            pthread_mutex_unlock(&pool.mu);
        }
    }
    in_pool = 0;
}

/* a worker starts from the generation current when it was created, so it only takes jobs posted after it */
static void *pool_worker(void *arg) {
    unsigned long seen = (unsigned long)(uintptr_t)arg;
    for (;;) {
        pthread_mutex_lock(&pool.mu);
        while (pool.gen == seen && !pool.stop) pthread_cond_wait(&pool.go, &pool.mu);
        int stop = pool.stop;
        seen = pool.gen;
        pthread_mutex_unlock(&pool.mu);
        if (stop) return NULL;
        pool_pieces();
    }
}

static void pool_rows(int j0, int j1, BandFn fn, void *ctx) {
    if (j1 <= j0) return;
    if (pool.threads <= 1 || in_pool || j1 - j0 < 2) { fn(ctx, j0, j1); return; }
    int n = j1 - j0, pieces = pool.threads * 3;
    if (pieces > n) pieces = n;
    pthread_mutex_lock(&pool.mu);
    pool.fn = fn;
    pool.ctx = ctx;
    pool.j0 = j0;
    pool.n = n;
    pool.pieces = pieces;
    atomic_store(&pool.next, 0);
    atomic_store(&pool.left, pieces);
    pool.gen++;
    pthread_cond_broadcast(&pool.go);
    pthread_mutex_unlock(&pool.mu);
    pool_pieces();
    pthread_mutex_lock(&pool.mu);
    while (atomic_load(&pool.left) > 0) pthread_cond_wait(&pool.done, &pool.mu);
    pthread_mutex_unlock(&pool.mu);
}

static int pool_start(int threads) {
    pthread_mutex_lock(&pool.mu);
    pool.stop = 0;
    unsigned long gen = pool.gen;
    pthread_mutex_unlock(&pool.mu);
    pool.threads = 1;
    for (int i = 1; i < threads; i++) {
        if (pthread_create(&pool.th[i], NULL, pool_worker, (void *)(uintptr_t)gen)) break;
        pool.threads++;
    }
    return pool.threads == threads ? 0 : -1;
}

static void pool_stop(void) {
    pthread_mutex_lock(&pool.mu);
    pool.stop = 1;
    pthread_cond_broadcast(&pool.go);
    pthread_mutex_unlock(&pool.mu);
    for (int i = 1; i < pool.threads; i++) pthread_join(pool.th[i], NULL);
    pool.threads = 1;
    pool.gen = 0;                                   /* no worker left: the next run starts clean */
    pool.pieces = 0;
    atomic_store(&pool.next, 0);
    atomic_store(&pool.left, 0);
}

/* ---- layout and paint ---- */

int lb_ui_init(void) {
    if (ready) return 0;
    if (text_init_mem(lb_font_inter, sizeof lb_font_inter)) return -1;
    build_scene();
    ready = 1;
    return 0;
}

static void invalidate_all(void) {
    for (int i = 0; i < ui.cap; i++) if (ui.nodes[i]) node_invalidate(ui.nodes[i]);
}

static void frame(Canvas *c) {
    invalidate_all();
    layout_root(root, LB_UI_W, LB_UI_H);
    paint_damage(root);
    ui.damage = (Box){ 0, 0, LB_UI_W, LB_UI_H };
    canvas_set_clip(c, (Rect){ 0, 0, LB_UI_W, LB_UI_H });
    canvas_clear(c, 0);
    paint_root(c, root);
    ui.damage = (Box){ 0, 0, 0, 0 };
}

double lb_ui(int threads, double secs) {
    if (lb_ui_init()) return -1;
    uint32_t *px = malloc((size_t)LB_UI_W * LB_UI_H * 4);
    if (!px) return -1;
    Canvas c;
    canvas_init(&c, px, LB_UI_W, LB_UI_H, LB_UI_W);
    int k = threads < 1 ? 1 : threads > LB_UI_POOL_MAX ? LB_UI_POOL_MAX : threads;
    double r = -1;
    if (k > 1) {
        if (pool_start(k)) goto out;
        canvas_parallel(pool_rows);
    }
    frame(&c);                                      /* glyphs rasterized, shadow masks made: a running desktop's caches */
    double t0 = bench_now_ms(), end = t0 + secs * 1000, now;
    long frames = 0;
    do { frame(&c); frames++; now = bench_now_ms(); } while (now < end);
    r = frames * 1000.0 / (now - t0);
out:
    canvas_parallel(NULL);
    pool_stop();
    free(px);
    return r;
}

int lb_ui_check(int *nodes, int *unknown) {
    if (lb_ui_init()) return -1;
    *nodes = node_count();
    *unknown = unknown_styles;
    return 0;
}

/* ---- shaping ---- */

enum { F_LATIN, F_ARABIC, F_DEVANAGARI, F_THAI, F_CJK, F_COUNT };
static const unsigned char *const font_data[F_COUNT] = { lb_font_inter, lb_font_arabic, lb_font_devanagari, lb_font_thai, lb_font_cjk };
static const size_t font_size[F_COUNT] = { sizeof lb_font_inter, sizeof lb_font_arabic, sizeof lb_font_devanagari, sizeof lb_font_thai, sizeof lb_font_cjk };

#define PARA_MAX 1024

typedef struct {
    Otf otf[F_COUNT];
    Shaper sh;
    uint32_t cp[PARA_MAX];
    uint8_t lv[PARA_MAX], lv2[PARA_MAX];
    int scr[PARA_MAX], fnt[PARA_MAX], order[PARA_MAX];
} ShapeState;

static int script_deva = -1;

/* the font of a script that Inter (the interface font) doesn't cover, as textlayout.c's script_font() */
static int script_font(int sc) {
    if (sc == SCRIPT_ARAB) return F_ARABIC;
    if (sc == script_deva) return F_DEVANAGARI;
    if (sc == SCRIPT_THAI) return F_THAI;
    if (sc == SCRIPT_HANI || sc == SCRIPT_HIRA || sc == SCRIPT_KANA) return F_CJK;
    return -1;
}

/* marks, joiners and selectors stay with the character before them (textlayout.c) */
static int sticks_to_prev(uint32_t cp) {
    return uc_ccc(cp) || uc_ignorable(cp) || uc_bidi(cp) == BIDI_NSM || uc_join(cp) == JOIN_T || (cp >= 0x1F3FB && cp <= 0x1F3FF);
}

/* set once, before any shaping thread starts: the threads only read it */
static void shape_setup(void) {
    if (script_deva < 0) script_deva = uc_script(0x0915);
}

static ShapeState *shape_new(void) {
    ShapeState *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    for (int f = 0; f < F_COUNT; f++) otf_init(&s->otf[f], font_data[f], font_size[f], 0);
    return s;
}

static void shape_del(ShapeState *s) {
    if (!s) return;
    for (int f = 0; f < F_COUNT; f++) otf_free(&s->otf[f]);
    shape_free(&s->sh);
    free(s);
}

static uint32_t utf8_next(const unsigned char **p) {
    const unsigned char *s = *p;
    uint32_t c = *s++;
    int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (n) c &= 0x3fu >> n;
    while (n-- && *s) c = c << 6 | (*s++ & 0x3f);
    *p = s;
    return c;
}

/* one paragraph, as textlayout.c's build() does its first step (one line, no wrapping): glyphs, the
 * .notdef ones and the advance in font units; -1 if it doesn't fit */
static int shape_para(ShapeState *s, int p, int *glyphs, int *notdef, long *advance) {
    int n = 0;
    for (const unsigned char *q = (const unsigned char *)lb_paragraphs[p]; *q && n < PARA_MAX;) s->cp[n++] = utf8_next(&q);
    if (n == 0 || n >= PARA_MAX) return -1;
    int para = bidi_levels(s->cp, n, -1, s->lv, NULL);
    int last = SCRIPT_COMMON;
    for (int i = 0; i < n; i++) {                   /* common characters take the script before them… */
        int sc = uc_script(s->cp[i]);
        if (sc <= SCRIPT_INHERITED) sc = last; else last = sc;
        s->scr[i] = sc;
    }
    for (int i = n - 1, next = SCRIPT_COMMON; i >= 0; i--) {     /* …or after them, at the start */
        if (s->scr[i] <= SCRIPT_INHERITED) s->scr[i] = next; else next = s->scr[i];
    }
    /* each character's font as textlayout.c's font_for_cp(): marks with their base, common characters with
     * the font before them when it has them, then Inter, then the script's font */
    for (int i = 0; i < n; i++) {
        uint32_t c = s->cp[i];
        int f;
        if (i > 0 && sticks_to_prev(c)) f = s->fnt[i - 1];
        else if (i > 0 && uc_script(c) <= SCRIPT_INHERITED && otf_cmap(&s->otf[s->fnt[i - 1]], c)) f = s->fnt[i - 1];
        else if (otf_cmap(&s->otf[F_LATIN], c)) f = F_LATIN;
        else { f = script_font(s->scr[i]); if (f < 0 || !otf_cmap(&s->otf[f], c)) f = F_LATIN; }
        s->fnt[i] = f;
    }
    int g = 0, bad = 0;
    long adv = 0;
    for (int rs = 0; rs < n;) {
        int re = rs + 1;
        while (re < n && s->fnt[re] == s->fnt[rs] && s->scr[re] == s->scr[rs] && s->lv[re] == s->lv[rs]) re++;
        int f = s->fnt[rs];
        int k = shape_run(&s->sh, &s->otf[f], s->cp + rs, re - rs, uc_script_tag(s->scr[rs]), s->lv[rs] & 1);
        for (int j = 0; j < k; j++) {
            if (!s->sh.run.g[j]) bad++;
            if (s->sh.pos[j].att < 0) adv += s->sh.pos[j].adv;
        }
        g += k;
        rs = re;
    }
    memcpy(s->lv2, s->lv, (size_t)n);
    bidi_line(s->cp, s->lv2, 0, n, para, s->order);     /* the visual order of the line */
    if (glyphs) *glyphs = g;
    if (notdef) *notdef = bad;
    if (advance) *advance = adv;
    return 0;
}

int lb_shape_check(int p, int *glyphs, int *notdef, long *advance) {
    if (p < 0 || p >= LB_PARA_COUNT) return -1;
    shape_setup();
    ShapeState *s = shape_new();
    if (!s) return -1;
    int r = shape_para(s, p, glyphs, notdef, advance);
    shape_del(s);
    return r;
}

typedef struct {
    double secs;
    double runs[256], ms[256];
    atomic_int failed;
} ShapeJob;

static void shape_thread(void *arg, int t) {
    ShapeJob *j = arg;
    ShapeState *s = shape_new();
    if (!s) { atomic_store(&j->failed, 1); return; }
    long sink = 0;
    double t0 = bench_now_ms(), end = t0 + j->secs * 1000, now;
    double runs = 0;
    do {
        for (int p = 0; p < LB_PARA_COUNT; p++) {
            long a = 0;
            if (shape_para(s, p, NULL, NULL, &a)) { atomic_store(&j->failed, 1); shape_del(s); return; }
            sink += a + s->order[0];                 /* the advances and the visual order are used */
        }
        runs++;
        now = bench_now_ms();
    } while (now < end);
    j->runs[t] = runs + (sink == 0x7fffffffffffL);  /* never true: keeps the work from being optimized away */
    j->ms[t] = now - t0;
    shape_del(s);
}

double lb_shape(int threads, double secs) {
    static ShapeJob j;
    memset(&j, 0, sizeof j);
    if (threads < 1) threads = 1;
    if (threads > 256) threads = 256;
    j.secs = secs;
    shape_setup();
    if (lb_parallel(threads, shape_thread, &j) < 0 || atomic_load(&j.failed)) return -1;
    double rate = 0;
    for (int i = 0; i < threads; i++) if (j.ms[i] > 0) rate += j.runs[i] * 1000.0 / j.ms[i];
    return rate;
}
