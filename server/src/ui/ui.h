/* ui.h — LordOS interface engine: node tree, styles, layout, painting. */
#ifndef LORD_UI_H
#define LORD_UI_H

#include <stddef.h>
#include <stdint.h>
#include "../canvas.h"
#include "otf.h"

enum { UNIT_AUTO = 0, UNIT_PX, UNIT_PCT };
typedef struct { float v; uint8_t unit; } Dim;

enum { NODE_VIEW, NODE_TEXT, NODE_IMAGE, NODE_RAW, NODE_TERM, NODE_SURFACE, NODE_MEDIA };
enum { JUSTIFY_START, JUSTIFY_CENTER, JUSTIFY_END, JUSTIFY_BETWEEN, JUSTIFY_AROUND, JUSTIFY_EVENLY };
enum { ALIGN_AUTO, ALIGN_STRETCH, ALIGN_START, ALIGN_CENTER, ALIGN_END };
enum { TEXT_LEFT, TEXT_CENTER, TEXT_RIGHT, TEXT_START, TEXT_END };   /* start/end according to direction */
enum { EDGE_L, EDGE_T, EDGE_R, EDGE_B };
enum { DIR_INHERIT, DIR_LTR, DIR_RTL };
enum { POINTER_AUTO, POINTER_NONE, POINTER_BOX_NONE };   /* pointerEvents; box-none: only the children are hit */

/* Subset of CSS the engine understands (the JS names are in style.c). */
typedef struct {
    uint8_t display_none, absolute, row, justify, align_items, align_self;
    uint8_t overflow_hidden, pointer_none, text_align, object_fit, nowrap;   /* pointer_none: POINTER_* */
    uint8_t dir;                /* DIR_INHERIT / DIR_LTR / DIR_RTL (CSS direction) */
    uint8_t wrap;               /* flexWrap: 'wrap' (children that don't fit move to another line) */
    uint8_t mono;               /* fontFamily: 0 'sans-serif' (Inter), 1 'monospace' (JetBrains Mono), 2 'brand' (Plus Jakarta Sans) */
    uint8_t cursor;             /* cursor (CSS): CUR_* from cursor.h; 0 = the parent's */
    float grow, shrink;
    Dim basis, width, height, min_w, min_h, max_w, max_h;
    Dim pos[4];                 /* left, top, right, bottom */
    float margin[4], padding[4];
    float gap;
    uint32_t bg, border_color, color, shadow_color;
    uint8_t has_gradient;
    uint8_t text_overflow;      /* textOverflow: 'ellipsis' */
    uint8_t line_clamp;         /* numberOfLines / lineClamp: at most N lines, "…" on the last (0 = no limit) */
    uint32_t grad_from, grad_to;
    float grad_angle;
    float radius, border_width, opacity;
    float shadow_blur, shadow_x, shadow_y, backdrop_blur;
    float font_size, line_height, font_weight;
    float translate_x, translate_y;
    float z_index;
    /* Per-side and logical edges, scale/rotate: rarely used, so they live in an allocation of their
     * own (StyleExt, created by the first such property); every node walk stays as fast as before. */
    struct StyleExt *ext;
} Style;

typedef struct StyleExt {
    /* A value below only overrides the shorthand (borderWidth, borderColor, borderRadius, margin*,
     * padding*) when its bit is in edge_set (ES_*); start/end follow the node's direction and win
     * over the physical side, as in React Native. */
    uint32_t edge_set;
    /* transform: scale(...) rotate(...) for painting only (layout and translate are unaffected) */
    float scale_x, scale_y, rotate;
    float border_side[6];           /* widths: left, top, right, bottom, start, end */
    uint32_t border_side_color[6];
    float margin_inline[2], padding_inline[2];   /* start, end */
    Dim inset_inline[2];            /* insetInlineStart / insetInlineEnd (auto = unset) */
    float radius_corner[8];         /* top-left, top-right, bottom-right, bottom-left, top-start, top-end, bottom-start, bottom-end */
    Dim origin_x, origin_y;         /* transformOrigin, 50% 50% by default */
    /* not style: the edges resolved for the node's direction (layout.c node_resolve_edges), physical L, T, R, B */
    struct {
        float mg[4], pd[4], bw[4];
        uint32_t bc[4];
        float rad[4];               /* top-left, top-right, bottom-right, bottom-left */
        uint8_t ok, rtl, plain;     /* resolved for this style and direction; uniform border and radius */
    } r;
} StyleExt;
#define STYLE_EXT_VALUES offsetof(StyleExt, r)   /* what lord.setStyle compares */

enum { SIDE_START = 4, SIDE_END = 5 };
#define ES_BW(i)      (1u << (i))           /* border width of side i (0..5) */
#define ES_BC(i)      (1u << (6 + (i)))     /* border color of side i */
#define ES_MARGIN(i)  (1u << (12 + (i)))    /* margin start (0) / end (1) */
#define ES_PADDING(i) (1u << (14 + (i)))    /* padding start (0) / end (1) */
#define ES_RADIUS(i)  (1u << (16 + (i)))    /* corner i (0..7) */

/* 2D affine transform: (x, y) → (a x + c y + e, b x + d y + f). */
typedef struct { float a, b, c, d, e, f; } Affine;

typedef struct Image Image;

/* Box in screen coordinates; empty if x1 <= x0. */
typedef struct { float x0, y0, x1, y1; } Box;

/* Transforms (layout.c place), only for nodes that have one or are inside one: own = this node's
 * scale/rotate around its origin, in layout coordinates; world = its ancestors' and its own
 * (layout → screen); inv = the inverse of world. */
typedef struct {
    Affine own, world, inv;
    uint64_t sig;               /* paint.c: geometry of its subtree in the last damage pass (own_tf) */
    uint8_t live, live_known;   /* paint.c: what decided its raster's kind in the last damage pass */
} NodeTf;

typedef struct Node {
    int id, type;
    Style s;
    char *text;                 /* NODE_RAW */
    Image *img;                 /* NODE_IMAGE */
    int term;                   /* NODE_TERM: session id; NODE_SURFACE: Wayland surface id; NODE_MEDIA: source (media/) */
    uint8_t protect;            /* protected window: black in screenshots, recordings and remote desktop (media/capture.c) */
    uint8_t rtl;                /* effective direction (computed in layout) */
    uint8_t own_tf, has_tf;     /* it has a scale/rotate; it or an ancestor has one (tf is set) */
    uint8_t layout_ok, layout_rtl;  /* its subtree is laid out for its current w × h and direction */
    struct Node *parent, *first, *last, *next, *prev;
    float x, y, w, h;           /* layout relative to the parent */
    float ax, ay;               /* absolute position on screen */
    int measure_valid;
    float measure_maxw, measure_w, measure_h;
    uint8_t paint_dirty;        /* its style or text changed */
    unsigned gen;               /* bumped with paint_dirty and never cleared: a transformed subtree's
                                 * cached raster compares it (paint.c tf_check) */
    Box prev_box;               /* what it occupied on screen in the last frame */
    NodeTf *tf;
} Node;

/* The node's edges for its direction (after layout's prepare): straight from the style unless it
 * has per-side or logical ones. Arrays are physical L, T, R, B. */
static inline const float *node_margin(const Node *n) { return n->s.ext ? n->s.ext->r.mg : n->s.margin; }
static inline const float *node_padding(const Node *n) { return n->s.ext ? n->s.ext->r.pd : n->s.padding; }
static inline float node_border(const Node *n, int e) { return n->s.ext ? n->s.ext->r.bw[e] : n->s.border_width; }
static inline int node_edges_plain(const Node *n) { return !n->s.ext || n->s.ext->r.plain; }

/* Damage as a short list of rectangles (0.8.2): small changes far apart (the Monitor's moving
 * dots) are repainted one by one instead of the box around all of them. dlist_add merges a
 * rectangle into the ones it overlaps and, past DAMAGE_MAX, the pair that wastes the least area. */
#define DAMAGE_MAX 16
typedef struct { Box r[DAMAGE_MAX]; int n; } DamageList;

typedef struct {
    Node **nodes;
    int cap;
    Node *root;
    int dirty;
    float width, height;
    char theme_dir[1024];
    double layout_ms, paint_ms;     /* last frame */
    Box damage;                     /* region to repaint (the box around dlist) */
    DamageList dlist;               /* the same damage as separate rectangles (damage_add) */
    Box dlast;                      /* ui.damage as damage_add left it: if it differs, it was set directly */
    double damage_px;               /* pixels repainted in the last frame */
    int quit_requested;             /* lord.quit() */
    uint8_t rtl;                    /* interface direction (Arabic, Hebrew… language) */
    char theme_request[1024];       /* lord.themes.use(): bundle to load */
    char theme_failed[1024];        /* last bundle that failed to load */
} Ui;

extern Ui ui;

/* node.c */
void  style_defaults(Style *s);
Node *node_new(int type);
Node *node_get(int id);
int   node_count(void);
void  node_destroy(Node *n);
void  node_append(Node *p, Node *c);
void  node_insert_before(Node *p, Node *c, Node *before);
void  node_detach(Node *c);
void  node_set_text(Node *n, const char *s);
void  node_invalidate(Node *n);
void  node_moved(Node *n);          /* only left/top changed (window drag): no repaint of its content */
/* after a style change (lord.setStyle): before = the style, ext_before = its StyleExt values (NULL
 * if it had none) before the change; places, remeasures or repaints only what the change needs */
void  node_restyled(Node *n, const Style *before, const StyleExt *ext_before);
int   node_gather_text(const Node *n, char *buf, int size);
int   node_children_ordered(Node *n, Node **out, int max);
void  damage_add(Box b);
void  dlist_add(DamageList *l, Box b);
float box_area(Box b);
void  damage_subtree(Node *n);
void  ui_reset(void);

/* style.c */
int style_set_number(Style *s, const char *key, double v);
int style_set_string(Style *s, const char *key, const char *v);
int style_reset(Style *s, const char *key);

/* layout.c */
void layout_root(Node *root, float w, float h);
void node_resolve_edges(Node *n);

/* transform.c: affine math shared by layout, paint and hit testing */
Affine affine_identity(void);
Affine affine_mul(Affine m, Affine n);              /* m after n: (m ∘ n)(p) = m(n(p)) */
int    affine_invert(Affine m, Affine *out);         /* 0 if singular */
void   affine_apply(Affine m, float x, float y, float *ox, float *oy);
Box    affine_box(Affine m, Box b);                  /* bounding box of the transformed rectangle */
int    style_rotates(float deg);
/* inline: layout asks it of every node */
static inline int style_has_transform(const Style *s) {
    const struct StyleExt *x = s->ext;
    return x && (x->scale_x != 1 || x->scale_y != 1 || (x->rotate != 0 && style_rotates(x->rotate)));
}
/* The node's own transform: translate(origin) · rotate · scale · translate(-origin), with the origin
 * resolved against its border box at (x, y, w, h). */
Affine style_transform(const Style *s, float x, float y, float w, float h);

/* text.c */
typedef struct {
    uint32_t cp;
    uint16_t font, size_q;      /* size in half pixels */
    int used;
    int w, h, xoff, yoff;
    float adv;
    uint8_t *bmp;
    uint32_t *argb;             /* color glyphs (emoji): premultiplied ARGB */
} Glyph;
/* The pointer is valid until the next call (the cache may grow). */
const Glyph *text_glyph(uint32_t cp, int font, float size);
int   text_font_add(const char *path);
int   text_drew_inexact(void);                    /* text drawn since the last call may not move exactly; resets */
int   text_font_load(const char *path, int index);
int   text_font_has(int font, uint32_t cp);
uint32_t text_font_glyph(int font, uint32_t cp);          /* glyph index, 0 = doesn't have it */
int   text_font_is_color(int font);
int   text_font_can_shape(int font, int group);           /* OTF_LIGA / OTF_CCMP */
void  text_font_shape(int font, int group, GlyphRun *run);
const Glyph *text_glyph_id(int font, uint32_t gid, float size);
const Glyph *text_glyph_color(int font, uint32_t gid, int max_w, int max_h);
void  text_font_vmetrics(int font, float size, float *asc, float *desc);
int   text_init(const char *font_dir);
/* For LordBench's helper, which opens no files: one interface font already in memory (it must stay there) */
int   text_init_mem(const unsigned char *data, size_t size);
/* Weight + TEXT_MONO picks the monospaced font (JetBrains Mono) in text_measure/text_draw and in
 * lord.measureText: this way the font travels with the weight without changing the signatures. */
#define TEXT_MONO 1000.0f
/* Weight + TEXT_BRAND: the brand face of the LordOS wordmark (Plus Jakarta Sans, docs/brand/README.md). */
#define TEXT_BRAND 2000.0f
#define TEXT_WEIGHT(s) ((s)->font_weight + ((s)->mono == 2 ? TEXT_BRAND : (s)->mono ? TEXT_MONO : 0))
float text_line_height(float size, float line_h);
/* While painting a scaled display (paint.c): sizes passed to text_draw are physical, line breaks
 * and positions follow the logical metrics (size / scale), so text wraps exactly where layout said. */
extern float text_paint_scale;
void  text_measure(const char *s, float weight, float size, float line_h, float max_w, int nowrap, float *w, float *h);
void  text_draw(Canvas *c, const char *s, float weight, float size, float line_h,
                float x, float y, float box_w, int align, int nowrap, uint32_t argb);
/* With truncation: at most max_lines lines (0 = all) and, if ellipsis, "…" where text is cut (the
 * last visible line when more text follows, and any nowrap line wider than box_w). */
void  text_measure_ex(const char *s, float weight, float size, float line_h, float max_w, int nowrap,
                      int max_lines, float *w, float *h);
void  text_draw_ex(Canvas *c, const char *s, float weight, float size, float line_h,
                   float x, float y, float box_w, int align, int nowrap, int max_lines, int ellipsis, uint32_t argb);
/* The truncated form of one line: the longest prefix that ends on a grapheme cluster (never inside
 * a ligature, a mark sequence, an emoji ZWJ sequence or a flag) and fits in box_w with "…" after it.
 * Writes it (with the "…") to out and returns its width; returns -1 and copies s if s already fits
 * and force is 0. Shaped text is re-shaped, so Arabic letters take their final forms. */
float text_ellipsize(const char *s, float weight, float size, float box_w, int force, char *out, int out_size);
Otf  *text_font_otf(int font);
float text_font_scale(int font, float size);
int   text_font_kern_glyphs(int font, uint32_t a, uint32_t b);  /* font units */
int   text_ui_font_bold(int font);

/* textlayout.c: complex scripts (bidi, Arabic, CJK, emoji, font fallback) */
int   text_needs_layout(const char *s, int font);
void  text_layout_measure(const char *s, int font, float size, float line_h, float max_w, int nowrap, float *w, float *h);
void  text_layout_draw(Canvas *c, const char *s, int font, float size, float line_h,
                       float x, float y, float box_w, int align, int nowrap, uint32_t argb);
void  text_layout_flush(void);
unsigned text_generation(void);                            /* changes when text may measure differently (language) */
float text_layout_ellipsize(const char *s, int font, float size, float box_w, int force, char *out, int out_size);
void  text_layout_measure_ex(const char *s, int font, float size, float line_h, float max_w, int nowrap, int max_lines, float *w, float *h);
void  text_layout_draw_n(Canvas *c, const char *s, int font, float size, float line_h,
                         float x, float y, float box_w, int align, int nowrap, int max_lines, uint32_t argb);
int   text_layout_line_bytes(const char *s, int font, float size, float max_w, int nowrap, int *bs, int *be, int max);
int   text_ellipsis_join(char *out, int out_size, const char *s, int len);   /* prefix + "…" */
void  text_set_language(const char *lang);                 /* picks the CJK font (ja/ko/zh) */
const char *text_language(void);                         /* the UI language the SDK set ("" before) */
int   text_script_fallback(uint32_t cp, int bold);         /* terminal: font per script */
int   text_direction(const char *s);                       /* 1 if the first strong character is RTL */

/* image.c */
Image *image_get(const char *path);
void   image_size(const Image *img, int *w, int *h);
Image *image_pixels(Image *img, int w, int h, const uint8_t *rgba);
Image *image_argb(Image *img, int w, int h, const uint32_t *argb);   /* premultiplied ARGB (doc/doc.c) */
void   image_release(Image *img);
void   image_draw(Canvas *c, Image *img, float x, float y, float w, float h, float r, int fit, float opacity);

/* paint.c */
/* The display being painted (docs/DISPLAYS.md): canvas pixel = (logical - (x, y)) * k + (dx, dy).
 * index tells displays apart (the window-drag layer belongs to one of them). Default: 1:1. */
typedef struct { float x, y, k, dx, dy; int index; } PaintView;
extern PaintView paint_view;
void  paint_layer_view(int index);  /* the display with most of the window being moved */
int   paint_layer_node(void);       /* that window's node (0: no drag layer) */
int   paint_damage(Node *root);     /* computes ui.damage; 0 if there is nothing to repaint */
/* after paint_damage: the rectangles to repaint (1 = just ui.damage), each inside ui.damage;
 * see "Several damaged rectangles" in paint.c */
int   paint_damage_rects(Box *out, int max);
/* damage gathered over several frames (a display whose flip was pending): the blur rule again,
 * on the tree as it is now (paint.c, main.c paint_out) */
void  paint_damage_settle(DamageList *l);
extern int paint_multi_damage;      /* 1: several rectangles (LORD_DAMAGE=multi, tests); 0 by default */
extern int paint_tf_nocache;        /* tests: a reference paint that neither reads nor fills the
                                     * transformed subtrees' raster cache (engine-test) */
void  paint_root(Canvas *c, Node *root);
void  paint_root_clear(Canvas *c, Node *root, uint32_t rgb);   /* canvas_clear + paint_root */
void  paint_root_full(Canvas *c, Node *root);  /* everything, no drag layer or skipped nodes (checks only) */
/* The copy of what is below a dragged window (paint.c, docs/WINDOWS.md). paint_below_switch (1 by
 * default): a frame may use it where 001633b's rule painted everything, when that gives the same
 * pixels. paint_below_verify (tests): each such frame is painted again the old way and the pixels that
 * differ are added to paint_below_bad (paint_below_checked counts the frames). */
extern int  paint_below_switch, paint_below_verify;
extern long paint_below_bad, paint_below_checked;
/* why a frame did not switch: an ancestor of the window paints over it (border, rounded corners), a
 * blurred or a transformed node before it meets the frame's box, the last build didn't reach the
 * window, or more to build than to paint */
enum { BR_OK, BR_ANCESTOR, BR_BLUR, BR_TRANSFORM, BR_UNREACHED, BR_COST, BR_N };
typedef struct { long full, part, switched, refused[BR_N]; } BelowStats;
void  paint_below_stats(BelowStats *s);    /* builds (whole, part) and switched frames since the last call */
/* G0 (docs/GPU.md, "The dragged window on the GPU"): paint_root_clear's frame, except that where
 * the dragged window lands (g->w) nothing is painted: the GPU composites the layer over the copy of what
 * is below there, with the CPU's arithmetic. Only on frames that use that copy and only when nothing
 * else would paint there (see paint.c); 1 if it was taken (the scene's g->w is then stale and only
 * g->strip were painted), 0 if the whole clip was painted as paint_root_clear(c, root, 0) does.
 * paint_g0_done: the GPU got g's textures (the dirty rectangles start again). LORD_GPU_VERIFY=N: every
 * N-th taken frame also paints g->w on the CPU into g->ref, for the GPU to compare. */
struct GpuG0;
int   paint_root_g0(Canvas *c, Node *root, struct GpuG0 *g);
void  paint_g0_done(void);
/* changes whenever a GpuG0 that paint_root_g0 filled may no longer be submitted as it is: the drag layer began,
 * ended or moved to another display, its buffers were reallocated or the textures need everything again. The
 * frame pipeline (main.c) checks it before submitting a prepared G0 frame. */
unsigned paint_g0_epoch(void);
/* why a frame that used the copy was not composited on the GPU: the layer (invalid, re-rendered, app
 * windows or invalid pixels in it, other opacity, a transform), its rectangle not inside the damage,
 * an ancestor painting over it, an app window under it, a node painted after the window in the
 * damage, or something changed while painting (post) */
enum { G0R_LAYER, G0R_CLIP, G0R_ANCESTOR, G0R_APP, G0R_AFTER, G0R_POST,
       G0R_FEED,                            /* the textures still lack more than G0_FEED_PX (paint_g0_feed) */
       G0R_N };
/* G0R_LAYER split by its cause, in g0_gate's order: no window or not wanted, not valid (painted whole
 * next), being resized, being rendered, capture mode, another display's, transformed, the window or an
 * ancestor raw or hidden, app holes in it, app surfaces in it, a colour above its alpha, no buffer, not
 * under the root, another opacity, a part that would make it paint whole */
enum { G0L_GONE, G0L_INVALID, G0L_RESIZE, G0L_RENDERING, G0L_CAPTURE, G0L_VIEW, G0L_TRANSFORM, G0L_RAW,
       G0L_HOLES, G0L_SURFACES, G0L_PREMUL, G0L_NOBUF, G0L_TREE, G0L_OPACITY, G0L_PART, G0L_N };
typedef struct { long frames, strip_px, tried, refused[G0R_N], layer[G0L_N]; } G0Stats;
void  paint_g0_stats(G0Stats *s);          /* since the last call */
/* The last paint_root_* call: -1 G0 not tried (no copy used, or not a G0 call), 1 taken, 0 refused
 * (*why a G0R_*, *sub a G0L_* when it is G0R_LAYER, else -1) */
int   paint_g0_last(int *why, int *sub);
/* G0's textures catching up (docs/GPU.md, "Uploads without a stall"): what of the layer and of the copy
 * of what is below the GPU still lacks, in pixels (both; 0 nothing, or no layer on display view) */
long  paint_g0_backlog(int view);
/* Sends at most budget pixels of it, rectangles cut by rows, through up(ctx, which, px, w, h, r) (which:
 * 0 the copy, 1 the layer; px w × h, stride w), and forgets what was sent. Each texture is asked first
 * with an empty r. up returns 0 sent, 1 its texture held nothing of this (now it is ready for it: all of
 * it is needed again), -1 failed (stop). Only the display that has the layer (view), and only while G0
 * could use it. Returns the pixels sent. */
long  paint_g0_feed(int view, long budget, int (*up)(void *ctx, int which, const uint32_t *px, int w, int h, Rect r),
                    void *ctx);
Node *hit_test(Node *root, float x, float y);
/* Capture mode (media/capture.c): protected nodes are painted black and app windows
 * are copied even when compositing on the GPU. paint_capture paints the whole tree this way. */
extern int paint_capture_mode;
void  paint_capture(Canvas *c, Node *root);

#endif
