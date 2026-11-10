/* vt.c — xterm-256color compatible terminal emulator.
 *
 * Sequence parser (VT500 states), grid with history in a ring, text reflow
 * on resize, alternate screen, SGR with 256 colors and direct color,
 * mouse modes, bracketed paste, synchronized output, hyperlinks
 * (OSC 8), shell integration (OSC 133 / 7) and the OSC 1337 inline-image protocol extensions (
 * marks, badge, inline images, current directory). */
#include "term.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

#include "stb_image.h"

enum { S_GROUND, S_ESC, S_ESC_INT, S_CSI, S_OSC, S_OSC_ESC, S_DCS, S_DCS_ESC, S_IGNORE, S_IGNORE_ESC, S_TMUX, S_TMUX_ESC };

#define STR_MAX (64 * 1024)
#define STR_MAX_FILE (48 * 1024 * 1024)    /* inline images (OSC 1337;File) */
#define MAX_LINKS 4096
#define MAX_IMAGES 64
#define MAX_MATCHES 20000

static uint32_t now_s;

/* ---- lines and buffers ---- */

static void line_reserve(Line *l, int n) {
    if (n <= l->cap) return;
    Cell *c = realloc(l->c, (size_t)n * sizeof *c);
    if (!c) return;
    memset(c + l->cap, 0, (size_t)(n - l->cap) * sizeof *c);
    l->c = c;
    l->cap = (uint16_t)n;
}

static void line_reset(Line *l) {
    if (l->c) memset(l->c, 0, (size_t)l->cap * sizeof *l->c);
    l->len = 0;
    l->flags = 0;
    l->time = 0;
}

static int cell_blank(Cell c) { return !c.cp && !c.bg && !c.attr && !c.link; }

/* Fills [from, to) with `b`. If `b` is the empty cell and reaches the end, shortens the line. */
static void line_fill(Line *l, int from, int to, Cell b) {
    if (from < 0) from = 0;
    if (to <= from) return;
    if (cell_blank(b) && to >= l->len) {
        if (from < l->len) { memset(l->c + from, 0, (size_t)(l->len - from) * sizeof *l->c); l->len = (uint16_t)from; }
        return;
    }
    line_reserve(l, to);
    if (to > l->cap) to = l->cap;
    for (int i = from; i < to; i++) l->c[i] = b;
    if (to > l->len) l->len = (uint16_t)to;
}

static Line *bl(Buffer *b, int i) { return &b->ring[(b->head + i) % b->cap]; }
static Line *L(Term *t, int row) { return bl(t->buf, t->buf->count - t->rows + row); }

static int buf_init(Buffer *b, int cap, int rows) {
    b->ring = calloc((size_t)cap, sizeof *b->ring);
    b->cap = cap;
    b->count = rows;
    b->head = 0;
    b->dropped = 0;
    return b->ring ? 0 : -1;
}

static void buf_free(Buffer *b) {
    if (!b->ring) return;
    for (int i = 0; i < b->cap; i++) free(b->ring[i].c);
    free(b->ring);
    b->ring = NULL;
}

/* Appends an empty line at the end; if the ring is full the oldest one is dropped. */
static void buf_push(Buffer *b) {
    if (b->count < b->cap) { line_reset(bl(b, b->count)); b->count++; return; }
    line_reset(&b->ring[b->head]);
    b->head = (b->head + 1) % b->cap;
    b->dropped++;
}

static Cell blank(const Term *t) { return (Cell){ 0, 0, t->pen.bg, 0, 0 }; }

void vt_mark_dirty(Term *t, int row) {
    if (row >= 0 && row < t->rows) t->dirty[row] = 1;
}

static void dirty_all(Term *t) { t->all_dirty = 1; }

static void reply(Term *t, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0 && t->respond) t->respond(t, buf, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
}

long long vt_first_abs(const Term *t) { return t->buf->dropped; }
long long vt_total_lines(const Term *t) { return t->buf->count; }
long long vt_cursor_abs(const Term *t) { return t->buf->dropped + t->buf->count - t->rows + t->cy; }

Line *vt_abs_line(Term *t, long long abs) {
    long long i = abs - t->buf->dropped;
    return i >= 0 && i < t->buf->count ? bl(t->buf, (int)i) : NULL;
}

Line *vt_view_line(Term *t, int row, long long *abs) {
    int i = t->buf->count - t->rows - t->view + row;
    if (abs) *abs = t->buf->dropped + i;
    return i >= 0 && i < t->buf->count ? bl(t->buf, i) : NULL;
}

/* ---- images ---- */

static void image_free(TermImage *im) { free(im->px); free(im->scaled); }

static void prune_images(Term *t) {
    int k = 0;
    for (int i = 0; i < t->nimages; i++) {
        TermImage *im = &t->images[i];
        if (im->line + im->hc <= t->main.dropped) image_free(im);
        else t->images[k++] = *im;
    }
    t->nimages = k;
}

/* ---- scroll ---- */

static void scroll_up(Term *t, int top, int bot, int n, int to_history) {
    int h = bot - top + 1;
    if (n <= 0 || h <= 0) return;
    if (n > h) n = h;
    Cell b = blank(t);
    if (to_history && top == 0 && bot == t->rows - 1 && t->buf == &t->main) {
        long long before = t->main.dropped;
        for (int i = 0; i < n; i++) {
            buf_push(&t->main);
            Line *l = L(t, t->rows - 1);
            line_fill(l, 0, t->cols, b);
        }
        if (t->view > 0) {
            int max = t->main.count - t->rows;
            t->view += n;
            if (t->view > max) t->view = max;
        }
        if (t->main.dropped != before) prune_images(t);
    } else {
        for (int k = 0; k < n; k++) {
            Line tmp = *L(t, top);
            for (int r = top; r < bot; r++) *L(t, r) = *L(t, r + 1);
            *L(t, bot) = tmp;
            line_reset(L(t, bot));
            line_fill(L(t, bot), 0, t->cols, b);
        }
    }
    dirty_all(t);
}

static void scroll_down(Term *t, int top, int bot, int n) {
    int h = bot - top + 1;
    if (n <= 0 || h <= 0) return;
    if (n > h) n = h;
    Cell b = blank(t);
    for (int k = 0; k < n; k++) {
        Line tmp = *L(t, bot);
        for (int r = bot; r > top; r--) *L(t, r) = *L(t, r - 1);
        *L(t, top) = tmp;
        line_reset(L(t, top));
        line_fill(L(t, top), 0, t->cols, b);
    }
    dirty_all(t);
}

static void index_down(Term *t) {
    if (t->cy == t->bot) scroll_up(t, t->top, t->bot, 1, 1);
    else if (t->cy < t->rows - 1) { vt_mark_dirty(t, t->cy); t->cy++; vt_mark_dirty(t, t->cy); }
}

static void reverse_index(Term *t) {
    if (t->cy == t->top) scroll_down(t, t->top, t->bot, 1);
    else if (t->cy > 0) { vt_mark_dirty(t, t->cy); t->cy--; vt_mark_dirty(t, t->cy); }
}

/* ---- cursor ---- */

static void move_to(Term *t, int x, int y) {
    vt_mark_dirty(t, t->cy);
    int miny = 0, maxy = t->rows - 1;
    if (t->modes & M_ORIGIN) { miny = t->top; maxy = t->bot; }
    t->cx = x < 0 ? 0 : x >= t->cols ? t->cols - 1 : x;
    t->cy = y < miny ? miny : y > maxy ? maxy : y;
    t->wrap_pending = 0;
    vt_mark_dirty(t, t->cy);
}

static void save_cursor(Term *t, SavedCursor *s) {
    *s = (SavedCursor){ t->cx, t->cy, t->wrap_pending, (t->modes & M_ORIGIN) != 0, t->gl, { t->g[0], t->g[1] }, t->pen };
}

static void restore_cursor(Term *t, const SavedCursor *s) {
    t->pen = s->pen;
    t->gl = s->gl;
    t->g[0] = s->g[0];
    t->g[1] = s->g[1];
    if (s->origin) t->modes |= M_ORIGIN; else t->modes &= ~(uint32_t)M_ORIGIN;
    vt_mark_dirty(t, t->cy);
    t->cx = s->cx < t->cols ? s->cx : t->cols - 1;
    t->cy = s->cy < t->rows ? s->cy : t->rows - 1;
    t->wrap_pending = s->wrap && t->cx == t->cols - 1;
    vt_mark_dirty(t, t->cy);
}

static void reset_tabs(Term *t, int from) {
    for (int i = from; i < t->cols; i++) t->tabs[i] = i % 8 == 0 && i;
}

/* ---- graphemes (internal global table) ---- */

#define CLUSTER_MAX 32768
#define CLUSTER_HASH 65536

typedef struct { uint32_t cp[CLUSTER_LEN]; uint8_t n; } Cluster;
static Cluster *clusters;
static int nclusters;
static int32_t *cluster_hash;

static uint32_t cluster_key(const uint32_t *cp, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ cp[i]) * 16777619u;
    return h & (CLUSTER_HASH - 1);
}

/* Returns the cp that represents the sequence (or 0 if the table filled up). */
static uint32_t cluster_intern(const uint32_t *cp, int n) {
    if (n == 1) return cp[0];
    if (!clusters) {
        clusters = calloc(CLUSTER_MAX, sizeof *clusters);
        cluster_hash = malloc(CLUSTER_HASH * sizeof *cluster_hash);
        if (!clusters || !cluster_hash) { free(clusters); free(cluster_hash); clusters = NULL; cluster_hash = NULL; return 0; }
        for (int i = 0; i < CLUSTER_HASH; i++) cluster_hash[i] = -1;
    }
    for (uint32_t h = cluster_key(cp, n);; h = (h + 1) & (CLUSTER_HASH - 1)) {
        int32_t i = cluster_hash[h];
        if (i < 0) {
            if (nclusters >= CLUSTER_MAX) return 0;
            Cluster *c = &clusters[nclusters];
            memcpy(c->cp, cp, (size_t)n * sizeof *cp);
            c->n = (uint8_t)n;
            cluster_hash[h] = nclusters;
            return CP_CLUSTER | (uint32_t)nclusters++;
        }
        if (clusters[i].n == n && !memcmp(clusters[i].cp, cp, (size_t)n * sizeof *cp)) return CP_CLUSTER | (uint32_t)i;
    }
}

int vt_cluster_cps(uint32_t cp, uint32_t *out, int max) {
    if (!(cp & CP_CLUSTER)) { if (max > 0) out[0] = cp; return 1; }
    uint32_t i = cp & ~CP_CLUSTER;
    if (!clusters || i >= (uint32_t)nclusters) { if (max > 0) out[0] = 0xfffd; return 1; }
    int n = clusters[i].n < max ? clusters[i].n : max;
    memcpy(out, clusters[i].cp, (size_t)n * sizeof *out);
    return n;
}

uint32_t vt_base_cp(uint32_t cp) {
    if (!(cp & CP_CLUSTER)) return cp;
    uint32_t i = cp & ~CP_CLUSTER;
    return clusters && i < (uint32_t)nclusters ? clusters[i].cp[0] : 0xfffd;
}

/* ---- writing characters ---- */

static const uint16_t dec_graphics[31] = {
    0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1, 0x2424, 0x240b, 0x2518,
    0x2510, 0x250c, 0x2514, 0x253c, 0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524,
    0x2534, 0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
};

static int char_width(uint32_t cp) {
    if (cp < 0x7f) return cp >= 0x20;
    int w = wcwidth((wchar_t)cp);
    return w < 0 ? 1 : w;
}

/* When overwriting half a cell of a wide character, the other half is erased. */
static void clear_wide(Line *l, int x) {
    if (x < 0 || x >= l->len) return;
    if (l->c[x].attr & A_WIDE_TAIL && x > 0) { l->c[x - 1].cp = 0; l->c[x - 1].attr &= (uint16_t)~A_WIDE; }
    if (l->c[x].attr & A_WIDE && x + 1 < l->len) { l->c[x + 1].cp = 0; l->c[x + 1].attr &= (uint16_t)~A_WIDE_TAIL; }
}

/* Cell where the last written character ended up (to attach combining marks to it). */
static Cell *prev_cell(Term *t, int *col) {
    int x = t->wrap_pending ? t->cx : t->cx - 1;
    if (x < 0) return NULL;
    Line *l = L(t, t->cy);
    if (x >= l->len) return NULL;
    if (l->c[x].attr & A_WIDE_TAIL && x > 0) x--;
    if (!l->c[x].cp) return NULL;
    *col = x;
    return &l->c[x];
}

static int is_ri(uint32_t cp) { return cp >= 0x1f1e6 && cp <= 0x1f1ff; }

/* Does the character join the previous grapheme? Always the zero-width ones (combining
 * accents, ZWJ, variation selectors). With mode 2027 (grapheme clusters, like contour,
 * wezterm or foot) also: what follows a ZWJ, skin tone modifiers and the
 * second regional indicator of a flag. */
static int joins(Term *t, uint32_t cp, int w, Cell *prev) {
    if (w == 0) return 1;
    if (!(t->modes & M_GRAPHEME) || !prev) return 0;
    uint32_t seq[CLUSTER_LEN];
    int n = vt_cluster_cps(prev->cp, seq, CLUSTER_LEN);
    if (seq[n - 1] == 0x200d) return 1;
    if (cp >= 0x1f3fb && cp <= 0x1f3ff && (prev->attr & A_WIDE)) return 1;
    if (is_ri(cp) && n == 1 && is_ri(seq[0])) return 1;
    return 0;
}

static void put_char(Term *t, uint32_t cp) {
    if (t->g[t->gl] == 1 && cp >= 0x60 && cp <= 0x7e) cp = dec_graphics[cp - 0x60];
    int w = char_width(cp);
    int pcol;
    Cell *prev = prev_cell(t, &pcol);
    if (joins(t, cp, w, prev)) {
        if (!prev) return;
        uint32_t seq[CLUSTER_LEN];
        int n = vt_cluster_cps(prev->cp, seq, CLUSTER_LEN);
        if (n >= CLUSTER_LEN) return;
        seq[n++] = cp;
        uint32_t id = cluster_intern(seq, n);
        if (id) prev->cp = id;
        /* mode 2027: VS16 or a complete flag take two cells */
        if ((t->modes & M_GRAPHEME) && !(prev->attr & A_WIDE) && (cp == 0xfe0f || is_ri(cp)) && !t->wrap_pending &&
            pcol + 1 < t->cols && t->cx == pcol + 1) {
            Line *l = L(t, t->cy);
            line_reserve(l, pcol + 2);
            clear_wide(l, pcol + 1);
            prev = &l->c[pcol];
            prev->attr |= A_WIDE;
            Cell tail = *prev;
            tail.cp = 0;
            tail.attr = (uint16_t)((prev->attr & ~A_WIDE) | A_WIDE_TAIL);
            l->c[pcol + 1] = tail;
            if (l->len < pcol + 2) l->len = (uint16_t)(pcol + 2);
            if (pcol + 2 >= t->cols) { t->cx = t->cols - 1; t->wrap_pending = (t->modes & M_AUTOWRAP) != 0; }
            else t->cx = pcol + 2;
        }
        vt_mark_dirty(t, t->cy);
        return;
    }
    t->last_cp = cp;
    if (t->wrap_pending && (t->modes & M_AUTOWRAP)) {
        L(t, t->cy)->flags |= LINE_WRAPPED;
        t->cx = 0;
        index_down(t);
    }
    t->wrap_pending = 0;
    if (w == 2 && t->cx == t->cols - 1) {
        if (t->modes & M_AUTOWRAP) {
            Line *l = L(t, t->cy);
            line_fill(l, t->cx, t->cx + 1, blank(t));
            l->flags |= LINE_WRAPPED;
            t->cx = 0;
            index_down(t);
        } else return;
    }
    Line *l = L(t, t->cy);
    line_reserve(l, t->cols);
    if (t->modes & M_INSERT && t->cx + w < t->cols) {
        memmove(l->c + t->cx + w, l->c + t->cx, (size_t)(t->cols - t->cx - w) * sizeof *l->c);
        if (l->len < t->cols) l->len = (uint16_t)(l->len + w > t->cols ? t->cols : l->len + w);
    }
    clear_wide(l, t->cx);
    if (w == 2) clear_wide(l, t->cx + 1);
    Cell c = t->pen;
    c.cp = cp;
    c.attr &= (uint16_t)~(A_WIDE | A_WIDE_TAIL);
    if (w == 2) c.attr |= A_WIDE;
    l->c[t->cx] = c;
    if (w == 2) { Cell tail = c; tail.cp = 0; tail.attr = (uint16_t)((c.attr & ~A_WIDE) | A_WIDE_TAIL); l->c[t->cx + 1] = tail; }
    if (t->cx + w > l->len) l->len = (uint16_t)(t->cx + w);
    if (!l->time) l->time = now_s;
    vt_mark_dirty(t, t->cy);
    if (t->cx + w >= t->cols) { t->cx = t->cols - 1; t->wrap_pending = (t->modes & M_AUTOWRAP) != 0; }
    else t->cx += w;
}

/* ---- erasing and editing ---- */

static void erase_line(Term *t, int row, int from, int to) {
    Line *l = L(t, row);
    clear_wide(l, from);
    clear_wide(l, to - 1);
    line_fill(l, from, to, blank(t));
    if (from == 0 && to >= t->cols) l->flags &= (uint8_t)~LINE_WRAPPED;
    vt_mark_dirty(t, row);
}

static void erase_display(Term *t, int mode) {
    if (mode == 0) {
        erase_line(t, t->cy, t->cx, t->cols);
        for (int r = t->cy + 1; r < t->rows; r++) { line_reset(L(t, r)); erase_line(t, r, 0, t->cols); }
    } else if (mode == 1) {
        for (int r = 0; r < t->cy; r++) { line_reset(L(t, r)); erase_line(t, r, 0, t->cols); }
        erase_line(t, t->cy, 0, t->cx + 1);
    } else if (mode == 2) {
        for (int r = 0; r < t->rows; r++) { line_reset(L(t, r)); erase_line(t, r, 0, t->cols); }
    } else if (mode == 3) vt_clear_scrollback(t);
    t->wrap_pending = 0;
}

static void insert_chars(Term *t, int n) {
    Line *l = L(t, t->cy);
    if (n > t->cols - t->cx) n = t->cols - t->cx;
    line_reserve(l, t->cols);
    clear_wide(l, t->cx);
    memmove(l->c + t->cx + n, l->c + t->cx, (size_t)(t->cols - t->cx - n) * sizeof *l->c);
    for (int i = 0; i < n; i++) l->c[t->cx + i] = blank(t);
    if (l->len > t->cx) l->len = (uint16_t)(l->len + n > t->cols ? t->cols : l->len + n);
    else if (!cell_blank(blank(t))) l->len = (uint16_t)(t->cx + n);
    vt_mark_dirty(t, t->cy);
}

static void delete_chars(Term *t, int n) {
    Line *l = L(t, t->cy);
    if (n > t->cols - t->cx) n = t->cols - t->cx;
    line_reserve(l, t->cols);
    clear_wide(l, t->cx);
    clear_wide(l, t->cx + n - 1);
    memmove(l->c + t->cx, l->c + t->cx + n, (size_t)(t->cols - t->cx - n) * sizeof *l->c);
    for (int i = t->cols - n; i < t->cols; i++) l->c[i] = blank(t);
    if (cell_blank(blank(t))) {
        int len = l->len > n ? l->len - n : 0;
        if (len < t->cx) len = l->len < t->cx ? l->len : t->cx;
        l->len = (uint16_t)len;
    } else l->len = (uint16_t)t->cols;
    vt_mark_dirty(t, t->cy);
}

/* ---- alternate screen ---- */

static void use_alt(Term *t, int on, int clear) {
    if (on == (t->buf == &t->alt)) return;
    if (on) {
        save_cursor(t, &t->saved_alt);
        t->buf = &t->alt;
        t->nkitty[1] = 0;                   /* the alternate screen starts with its own empty stack */
        if (clear) for (int r = 0; r < t->rows; r++) line_reset(L(t, r));
    } else {
        t->buf = &t->main;
        restore_cursor(t, &t->saved_alt);
    }
    t->view = 0;
    t->sel_active = 0;
    vt_search_clear(t);
    dirty_all(t);
}

/* ---- palette ---- */

void vt_apply_palette(Term *t) {
    static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
    for (int i = 0; i < 16; i++) t->palette[i] = t->prof->palette[i] | 0xff000000u;
    for (int i = 0; i < 216; i++)
        t->palette[16 + i] = 0xff000000u | (uint32_t)lv[i / 36] << 16 | (uint32_t)lv[i / 6 % 6] << 8 | (uint32_t)lv[i % 6];
    for (int i = 0; i < 24; i++) { uint32_t g = (uint32_t)(8 + 10 * i); t->palette[232 + i] = 0xff000000u | g << 16 | g << 8 | g; }
    dirty_all(t);
}

uint32_t vt_resolve(const Term *t, uint32_t color, int is_fg) {
    switch (color >> 24) {
    case 1: return t->palette[color & 0xff];
    case 2: return 0xff000000u | (color & 0xffffff);
    default:
        if (is_fg) return t->fg_set ? t->fg : (t->prof->fg | 0xff000000u);
        return t->bg_set ? t->bg : (t->prof->bg | 0xff000000u);
    }
}

/* ---- SGR ---- */

static int prm(const Term *t, int i, int def) { return i < t->nparams && t->params[i] > 0 ? t->params[i] : def; }
static int prm0(const Term *t, int i, int def) { return i < t->nparams && t->params[i] >= 0 ? t->params[i] : def; }

static void sgr(Term *t) {
    if (t->nparams == 0) { t->pen.attr = 0; t->pen.fg = t->pen.bg = 0; return; }
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i] < 0 ? 0 : t->params[i];
        int ns = 0;
        while (i + 1 + ns < t->nparams && t->colon[i + 1 + ns]) ns++;
        uint16_t *a = &t->pen.attr;
        switch (p) {
        case 0: *a = 0; t->pen.fg = t->pen.bg = 0; break;
        case 1: *a |= A_BOLD; break;
        case 2: *a |= A_DIM; break;
        case 3: *a |= A_ITALIC; break;
        case 4:
            *a &= (uint16_t)~(A_UNDER | A_UNDER2 | A_CURLY);
            if (ns > 0) {
                int s = t->params[i + 1];
                if (s == 2) *a |= A_UNDER2; else if (s == 3) *a |= A_CURLY; else if (s > 0) *a |= A_UNDER;
            } else *a |= A_UNDER;
            break;
        case 5: case 6: *a |= A_BLINK; break;
        case 7: *a |= A_INVERSE; break;
        case 8: *a |= A_HIDDEN; break;
        case 9: *a |= A_STRIKE; break;
        case 21: *a = (uint16_t)((*a & ~(A_UNDER | A_CURLY)) | A_UNDER2); break;
        case 22: *a &= (uint16_t)~(A_BOLD | A_DIM); break;
        case 23: *a &= (uint16_t)~A_ITALIC; break;
        case 24: *a &= (uint16_t)~(A_UNDER | A_UNDER2 | A_CURLY); break;
        case 25: *a &= (uint16_t)~A_BLINK; break;
        case 27: *a &= (uint16_t)~A_INVERSE; break;
        case 28: *a &= (uint16_t)~A_HIDDEN; break;
        case 29: *a &= (uint16_t)~A_STRIKE; break;
        case 53: *a |= A_OVERLINE; break;
        case 55: *a &= (uint16_t)~A_OVERLINE; break;
        case 39: t->pen.fg = 0; break;
        case 49: t->pen.bg = 0; break;
        case 38: case 48: case 58: {
            uint32_t col = 0;
            int ok = 0, used = 0;
            int q = i + 1, m = q < t->nparams ? t->params[q] : -1;
            if (ns > 0) {
                used = ns;
                if (m == 5 && ns >= 2) { col = COLOR_IDX(t->params[q + 1] & 0xff); ok = 1; }
                else if (m == 2 && ns >= 4) {
                    int o = ns >= 5 ? q + 2 : q + 1;
                    col = COLOR_RGB((uint32_t)(t->params[o] & 0xff) << 16 | (uint32_t)(t->params[o + 1] & 0xff) << 8 |
                                    (uint32_t)(t->params[o + 2] & 0xff));
                    ok = 1;
                }
            } else if (m == 5 && q + 1 < t->nparams) {
                col = COLOR_IDX(t->params[q + 1] & 0xff); ok = 1; used = 2;
            } else if (m == 2 && q + 3 < t->nparams) {
                col = COLOR_RGB((uint32_t)(t->params[q + 1] & 0xff) << 16 | (uint32_t)(t->params[q + 2] & 0xff) << 8 |
                                (uint32_t)(t->params[q + 3] & 0xff));
                ok = 1; used = 4;
            }
            if (ok && p == 38) t->pen.fg = col;
            if (ok && p == 48) t->pen.bg = col;
            i += used;
            continue;
        }
        default:
            if (p >= 30 && p <= 37) t->pen.fg = COLOR_IDX(p - 30);
            else if (p >= 40 && p <= 47) t->pen.bg = COLOR_IDX(p - 40);
            else if (p >= 90 && p <= 97) t->pen.fg = COLOR_IDX(p - 90 + 8);
            else if (p >= 100 && p <= 107) t->pen.bg = COLOR_IDX(p - 100 + 8);
            break;
        }
        i += ns;
    }
}

/* ---- modes ---- */

static void set_flag(Term *t, uint32_t f, int on) {
    if (on) t->modes |= f; else t->modes &= ~f;
}

static void set_mouse(Term *t, uint32_t f, int on) {
    if (on) t->modes &= ~(uint32_t)M_MOUSE;
    set_flag(t, f, on);
}

static double mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void dec_mode(Term *t, int mode, int on) {
    switch (mode) {
    case 1: set_flag(t, M_APPCURSOR, on); break;
    case 5: set_flag(t, M_REVERSE, on); dirty_all(t); break;
    case 6: set_flag(t, M_ORIGIN, on); move_to(t, 0, on ? t->top : 0); break;
    case 7: set_flag(t, M_AUTOWRAP, on); break;
    case 9: set_mouse(t, M_MOUSE_X10, on); break;
    case 12: set_flag(t, M_CURSOR_BLINK, on); break;
    case 25: set_flag(t, M_HIDECURSOR, !on); vt_mark_dirty(t, t->cy); break;
    case 47: case 1047: use_alt(t, on, mode == 1047); break;
    case 66: set_flag(t, M_APPKEYPAD, on); break;
    case 1000: set_mouse(t, M_MOUSE_BTN, on); break;
    case 1002: set_mouse(t, M_MOUSE_DRAG, on); break;
    case 1003: set_mouse(t, M_MOUSE_ANY, on); break;
    case 1004: set_flag(t, M_FOCUS, on); break;
    case 1006: set_flag(t, M_MOUSE_SGR, on); break;
    case 1007: set_flag(t, M_ALTSCROLL, on); break;
    case 1048: if (on) save_cursor(t, &t->saved); else restore_cursor(t, &t->saved); break;
    case 1049: use_alt(t, on, 1); break;
    case 2004: set_flag(t, M_BRACKETED, on); break;
    case 2027: set_flag(t, M_GRAPHEME, on); break;
    case 2026:
        set_flag(t, M_SYNC, on);
        t->sync_until = on ? mono_ms() + 150 : 0;
        if (!on) dirty_all(t);
        break;
    default: break;
    }
}

static int dec_mode_state(const Term *t, int mode) {
    uint32_t f;
    switch (mode) {
    case 1: f = M_APPCURSOR; break;
    case 5: f = M_REVERSE; break;
    case 6: f = M_ORIGIN; break;
    case 7: f = M_AUTOWRAP; break;
    case 9: f = M_MOUSE_X10; break;
    case 12: f = M_CURSOR_BLINK; break;
    case 25: return t->modes & M_HIDECURSOR ? 2 : 1;
    case 47: case 1047: case 1049: return t->buf == &t->alt ? 1 : 2;
    case 66: f = M_APPKEYPAD; break;
    case 1000: f = M_MOUSE_BTN; break;
    case 1002: f = M_MOUSE_DRAG; break;
    case 1003: f = M_MOUSE_ANY; break;
    case 1004: f = M_FOCUS; break;
    case 1006: f = M_MOUSE_SGR; break;
    case 1007: f = M_ALTSCROLL; break;
    case 2004: f = M_BRACKETED; break;
    case 2026: f = M_SYNC; break;
    case 2027: f = M_GRAPHEME; break;
    default: return 0;
    }
    return t->modes & f ? 1 : 2;
}

/* ---- CSI ---- */

static void soft_reset(Term *t) {
    t->modes = (t->modes & (M_MOUSE | M_MOUSE_SGR | M_FOCUS | M_BRACKETED)) | M_AUTOWRAP;
    t->pen = (Cell){0};
    t->top = 0;
    t->bot = t->rows - 1;
    t->g[0] = t->g[1] = t->gl = 0;
    t->cursor_shape = 0;
    save_cursor(t, &t->saved);
}

static void csi(Term *t, int f) {
    int n = prm(t, 0, 1);
    if (t->priv == '?') {
        if (f == 'h' || f == 'l') {
            for (int i = 0; i < t->nparams; i++) dec_mode(t, t->params[i], f == 'h');
        } else if (f == 'p' && t->ninter == 1 && t->inter[0] == '$') {
            int m = prm0(t, 0, 0);
            reply(t, "\033[?%d;%d$y", m, dec_mode_state(t, m));
        } else if (f == 'u') {                              /* kitty keyboard protocol: the flags in use */
            int a = t->buf == &t->alt;
            reply(t, "\033[?%du", t->nkitty[a] ? t->kitty[a][t->nkitty[a] - 1] : 0);
        } else if (f == 'n' && prm0(t, 0, 0) == 6) {
            reply(t, "\033[?%d;%dR", t->cy + 1 - (t->modes & M_ORIGIN ? t->top : 0), t->cx + 1);
        } else if (f == 'J') erase_display(t, prm0(t, 0, 0));
        else if (f == 'K') {
            int m = prm0(t, 0, 0);
            erase_line(t, t->cy, m == 0 ? t->cx : 0, m == 1 ? t->cx + 1 : t->cols);
        } else if (f == 's') save_cursor(t, &t->saved);    /* XTSAVE: approximation */
        else if (f == 'S') {                                /* XTSMGRAPHICS: sixel capabilities */
            int item = prm0(t, 0, 0);
            if (item == 1) reply(t, "\033[?1;0;256S");
            else if (item == 2) reply(t, "\033[?2;0;%d;%dS", (int)(t->cols * t->cell_w), (int)(t->rows * t->cell_h));
            else reply(t, "\033[?%d;1;0S", item);
        }
        return;
    }
    if (t->priv == '>' || t->priv == '<' || t->priv == '=') {
        int a = t->buf == &t->alt;
        uint8_t *st = t->kitty[a];
        int *ns = &t->nkitty[a];
        if (f == 'u' && t->priv == '>') {               /* push (only flag 1, the one supported) */
            if (*ns == 8) { memmove(st, st + 1, 7); (*ns)--; }
            st[(*ns)++] = (uint8_t)(prm0(t, 0, 0) & 1);
        } else if (f == 'u' && t->priv == '<') {        /* pop n */
            int k = prm(t, 0, 1);
            *ns = k >= *ns ? 0 : *ns - k;
        } else if (f == 'u' && t->priv == '=') {        /* set (1), or (2), and not (3) the current flags */
            int v = prm0(t, 0, 0) & 1, mode = prm(t, 1, 1);
            if (!*ns) st[(*ns)++] = 0;
            uint8_t *cur = &st[*ns - 1];
            *cur = (uint8_t)(mode == 2 ? (*cur | v) : mode == 3 ? (*cur & ~v) : v);
        } else if (f == 'm' && t->priv == '>' && prm0(t, 0, 0) == 4) {
            t->other_keys = t->nparams > 1 ? prm0(t, 1, 0) : 0;     /* modifyOtherKeys: 0, 1 or 2 */
        }
        if (t->priv != '>') return;
        if (f == 'c') reply(t, "\033[>0;276;0c");
        else if (f == 'q') reply(t, "\033P>|LordOS Terminal 0.4\033\\");
        return;
    }
    if (t->priv) return;
    if (t->ninter == 1) {
        char in = t->inter[0];
        if (in == ' ' && f == 'q') { t->cursor_shape = prm0(t, 0, 0); vt_mark_dirty(t, t->cy); }
        else if (in == '!' && f == 'p') soft_reset(t);
        else if (in == '$' && f == 'p') {
            int m = prm0(t, 0, 0);
            int st = m == 4 ? (t->modes & M_INSERT ? 1 : 2) : m == 20 ? (t->modes & M_LNM ? 1 : 2) : 0;
            reply(t, "\033[%d;%d$y", m, st);
        }
        return;
    }
    switch (f) {
    case '@': insert_chars(t, n); break;
    case 'A': { int lim = t->cy >= t->top ? t->top : 0; move_to(t, t->cx, t->cy - n < lim ? lim : t->cy - n); break; }
    case 'B': case 'e': { int lim = t->cy <= t->bot ? t->bot : t->rows - 1; move_to(t, t->cx, t->cy + n > lim ? lim : t->cy + n); break; }
    case 'C': case 'a': move_to(t, t->cx + n, t->cy); break;
    case 'D': move_to(t, t->cx - n, t->cy); break;
    case 'E': move_to(t, 0, t->cy + n > t->bot ? t->bot : t->cy + n); break;
    case 'F': move_to(t, 0, t->cy - n < t->top ? t->top : t->cy - n); break;
    case 'G': case '`': move_to(t, n - 1, t->cy); break;
    case 'H': case 'f': {
        int y = prm(t, 0, 1) - 1, x = prm(t, 1, 1) - 1;
        move_to(t, x, (t->modes & M_ORIGIN) ? y + t->top : y);
        break;
    }
    case 'I': for (int i = 0; i < n; i++) { int x = t->cx + 1; while (x < t->cols - 1 && !t->tabs[x]) x++; move_to(t, x, t->cy); } break;
    case 'Z': for (int i = 0; i < n; i++) { int x = t->cx - 1; while (x > 0 && !t->tabs[x]) x--; move_to(t, x, t->cy); } break;
    case 'J': erase_display(t, prm0(t, 0, 0)); break;
    case 'K': {
        int m = prm0(t, 0, 0);
        erase_line(t, t->cy, m == 0 ? t->cx : 0, m == 1 ? t->cx + 1 : t->cols);
        t->wrap_pending = 0;
        break;
    }
    case 'L': if (t->cy >= t->top && t->cy <= t->bot) { scroll_down(t, t->cy, t->bot, n); t->cx = 0; } break;
    case 'M': if (t->cy >= t->top && t->cy <= t->bot) { scroll_up(t, t->cy, t->bot, n, 0); t->cx = 0; } break;
    case 'P': delete_chars(t, n); break;
    case 'S': scroll_up(t, t->top, t->bot, n, 0); break;
    case 'T': if (t->nparams <= 1) scroll_down(t, t->top, t->bot, n); break;
    case 'X': {
        Line *l = L(t, t->cy);
        int to = t->cx + n > t->cols ? t->cols : t->cx + n;
        clear_wide(l, t->cx);
        clear_wide(l, to - 1);
        line_fill(l, t->cx, to, blank(t));
        vt_mark_dirty(t, t->cy);
        break;
    }
    case 'b': {
        if (!t->last_cp) break;
        int max = t->cols * t->rows;
        for (int i = 0; i < n && i < max; i++) put_char(t, t->last_cp);
        break;
    }
    case 'c': if (prm0(t, 0, 0) == 0) reply(t, "\033[?62;4;22c"); break;      /* 4 = sixel */
    case 'd': move_to(t, t->cx, (t->modes & M_ORIGIN) ? n - 1 + t->top : n - 1); break;
    case 'g': {
        int m = prm0(t, 0, 0);
        if (m == 0 && t->cx < t->cols) t->tabs[t->cx] = 0;
        else if (m == 3) memset(t->tabs, 0, (size_t)t->cols);
        break;
    }
    case 'h': case 'l':
        for (int i = 0; i < t->nparams; i++) {
            if (t->params[i] == 4) set_flag(t, M_INSERT, f == 'h');
            if (t->params[i] == 20) set_flag(t, M_LNM, f == 'h');
        }
        break;
    case 'm': sgr(t); break;
    case 'n':
        if (prm0(t, 0, 0) == 5) reply(t, "\033[0n");
        else if (prm0(t, 0, 0) == 6) reply(t, "\033[%d;%dR", t->cy + 1 - (t->modes & M_ORIGIN ? t->top : 0), t->cx + 1);
        break;
    case 'r': {
        int top = prm(t, 0, 1) - 1, bot = prm(t, 1, t->rows) - 1;
        if (bot >= t->rows) bot = t->rows - 1;
        if (top < bot) { t->top = top; t->bot = bot; move_to(t, 0, (t->modes & M_ORIGIN) ? top : 0); }
        break;
    }
    case 's': save_cursor(t, &t->saved); break;
    case 'u': restore_cursor(t, &t->saved); break;
    case 't': {
        int op = prm0(t, 0, 0);
        if (op == 14) reply(t, "\033[4;%d;%dt", (int)(t->rows * t->cell_h), (int)(t->cols * t->cell_w));
        else if (op == 16) reply(t, "\033[6;%d;%dt", (int)t->cell_h, (int)t->cell_w);
        else if (op == 18) reply(t, "\033[8;%d;%dt", t->rows, t->cols);
        break;
    }
    default: break;
    }
}

/* ---- text (for copying, searching and extracting commands) ---- */

typedef struct { char *s; size_t n, cap; } Str;

static void str_put(Str *s, const char *d, size_t n) {
    if (s->n + n + 1 > s->cap) {
        size_t cap = s->cap ? s->cap : 256;
        while (cap < s->n + n + 1) cap *= 2;
        char *ns = realloc(s->s, cap);
        if (!ns) return;
        s->s = ns;
        s->cap = cap;
    }
    memcpy(s->s + s->n, d, n);
    s->n += n;
    s->s[s->n] = 0;
}

static void str_cp1(Str *s, uint32_t cp) {
    char b[4];
    int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xc0 | cp >> 6); b[1] = (char)(0x80 | (cp & 0x3f)); n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xe0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 0x3f)); b[2] = (char)(0x80 | (cp & 0x3f)); n = 3; }
    else { b[0] = (char)(0xf0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 0x3f)); b[2] = (char)(0x80 | (cp >> 6 & 0x3f)); b[3] = (char)(0x80 | (cp & 0x3f)); n = 4; }
    str_put(s, b, (size_t)n);
}

static void str_cp(Str *s, uint32_t cp) {
    uint32_t seq[CLUSTER_LEN];
    int n = vt_cluster_cps(cp, seq, CLUSTER_LEN);
    for (int i = 0; i < n; i++) str_cp1(s, seq[i]);
}

static void line_text(Str *s, Line *l, int from, int to, int keep_trailing) {
    if (!l) return;
    int end = to < l->len - 1 ? to : l->len - 1;
    if (!keep_trailing) while (end >= from && (!l->c[end].cp || l->c[end].cp == ' ') && !(l->c[end].attr & A_WIDE_TAIL)) end--;
    for (int x = from; x <= end; x++) {
        if (l->c[x].attr & A_WIDE_TAIL) continue;
        str_cp(s, l->c[x].cp ? l->c[x].cp : ' ');
    }
}

/* ---- OSC ---- */

static int hexval(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* "rgb:rr/gg/bb" (1 to 4 digits per channel) or "#rrggbb". */
static int parse_color(const char *s, uint32_t *out) {
    if (s[0] == '#' && strlen(s) == 7) {
        uint32_t v = 0;
        for (int i = 1; i < 7; i++) { int h = hexval(s[i]); if (h < 0) return -1; v = v << 4 | (uint32_t)h; }
        *out = 0xff000000u | v;
        return 0;
    }
    if (strncmp(s, "rgb:", 4)) return -1;
    s += 4;
    uint32_t ch[3];
    for (int k = 0; k < 3; k++) {
        uint32_t v = 0;
        int digits = 0;
        while (hexval(*s) >= 0) { v = v << 4 | (uint32_t)hexval(*s); s++; digits++; }
        if (!digits || digits > 4) return -1;
        ch[k] = digits == 1 ? v * 17 : digits == 2 ? v : digits == 3 ? v >> 4 : v >> 8;
        if (k < 2) { if (*s != '/') return -1; s++; }
    }
    *out = 0xff000000u | ch[0] << 16 | ch[1] << 8 | ch[2];
    return 0;
}

static void reply_color(Term *t, const char *prefix, uint32_t c) {
    unsigned r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
    reply(t, "\033]%s;rgb:%02x%02x/%02x%02x/%02x%02x\033\\", prefix, r, r, g, g, b, b);
}

static const int8_t b64[256] = {
    ['A'] = 1, ['B'] = 2, ['C'] = 3, ['D'] = 4, ['E'] = 5, ['F'] = 6, ['G'] = 7, ['H'] = 8, ['I'] = 9,
    ['J'] = 10, ['K'] = 11, ['L'] = 12, ['M'] = 13, ['N'] = 14, ['O'] = 15, ['P'] = 16, ['Q'] = 17,
    ['R'] = 18, ['S'] = 19, ['T'] = 20, ['U'] = 21, ['V'] = 22, ['W'] = 23, ['X'] = 24, ['Y'] = 25,
    ['Z'] = 26, ['a'] = 27, ['b'] = 28, ['c'] = 29, ['d'] = 30, ['e'] = 31, ['f'] = 32, ['g'] = 33,
    ['h'] = 34, ['i'] = 35, ['j'] = 36, ['k'] = 37, ['l'] = 38, ['m'] = 39, ['n'] = 40, ['o'] = 41,
    ['p'] = 42, ['q'] = 43, ['r'] = 44, ['s'] = 45, ['t'] = 46, ['u'] = 47, ['v'] = 48, ['w'] = 49,
    ['x'] = 50, ['y'] = 51, ['z'] = 52, ['0'] = 53, ['1'] = 54, ['2'] = 55, ['3'] = 56, ['4'] = 57,
    ['5'] = 58, ['6'] = 59, ['7'] = 60, ['8'] = 61, ['9'] = 62, ['+'] = 63, ['/'] = 64,
};

/* Decodes base64 (ignores spaces and '='); `out` needs n*3/4+3 bytes. */
size_t base64_decode(const char *in, size_t n, uint8_t *out) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64[(uint8_t)in[i]] - 1;
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (uint8_t)(acc >> bits); }
    }
    return o;
}

static char *b64_text(const char *in, size_t max) {
    size_t n = strlen(in);
    char *out = malloc(n * 3 / 4 + 4);
    if (!out) return NULL;
    size_t m = base64_decode(in, n, (uint8_t *)out);
    if (m > max) m = max;
    out[m] = 0;
    return out;
}

static void copy_str(char *dst, size_t size, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < size; i++) dst[i] = (unsigned char)src[i] < 0x20 ? ' ' : src[i];   /* char is unsigned on ARM */
    dst[i] = 0;
}

static void url_decode(char *dst, size_t size, const char *src) {
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < size; p++) {
        if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) { dst[o++] = (char)(hexval(p[1]) << 4 | hexval(p[2])); p += 2; }
        else dst[o++] = *p;
    }
    dst[o] = 0;
}

static int add_link(Term *t, const char *uri) {
    for (int i = 0; i < t->nlinks; i++) if (!strcmp(t->links[i], uri)) return i + 1;
    if (!t->links) t->links = calloc(MAX_LINKS, sizeof *t->links);
    if (!t->links) return 0;
    if (t->nlinks >= MAX_LINKS) {   /* table full: it is recycled (old links lose their URI) */
        for (int i = 0; i < t->nlinks; i++) free(t->links[i]);
        t->nlinks = 0;
    }
    t->links[t->nlinks] = strdup(uri);
    return t->links[t->nlinks] ? ++t->nlinks : 0;
}

/* OSC 1337 image dimension: "N" cells, "Npx", "N%" or "auto". */
static int image_dim(const char *v, int cell, int avail_cells, int *px) {
    *px = 0;
    if (!v || !*v || !strcmp(v, "auto")) return -1;
    double d = atof(v);
    size_t n = strlen(v);
    if (n > 2 && !strcmp(v + n - 2, "px")) { *px = (int)d; return (int)((d + cell - 1) / cell); }
    if (v[n - 1] == '%') return (int)(avail_cells * d / 100);
    return (int)d;
}

static void add_image(Term *t, uint32_t *px, int w, int h, int wc, int hc);

static void place_image(Term *t, const char *args, const char *data) {
    if (t->buf != &t->main) return;
    char width[32] = "", height[32] = "";
    int inline_ = 0, keep = 1;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", args);
    for (char *tok = strtok(tmp, ";"); tok; tok = strtok(NULL, ";")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(tok, "width")) snprintf(width, sizeof width, "%s", eq + 1);
        else if (!strcmp(tok, "height")) snprintf(height, sizeof height, "%s", eq + 1);
        else if (!strcmp(tok, "inline")) inline_ = atoi(eq + 1);
        else if (!strcmp(tok, "preserveAspectRatio")) keep = atoi(eq + 1);
    }
    if (!inline_) return;
    size_t n = strlen(data);
    uint8_t *raw = malloc(n * 3 / 4 + 4);
    if (!raw) return;
    size_t len = base64_decode(data, n, raw);
    int w, h, comp;
    unsigned char *rgba = stbi_load_from_memory(raw, (int)len, &w, &h, &comp, 4);
    free(raw);
    if (!rgba) return;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { stbi_image_free(rgba); return; }
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) { stbi_image_free(rgba); return; }
    for (long i = 0; i < (long)w * h; i++) {
        uint32_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
        px[i] = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255);
    }
    stbi_image_free(rgba);

    float cw = t->cell_w > 0 ? t->cell_w : 8, ch = t->cell_h > 0 ? t->cell_h : 16;
    int pxw, pxh;
    int wc = image_dim(width, (int)cw, t->cols - t->cx, &pxw);
    int hc = image_dim(height, (int)ch, t->rows, &pxh);
    if (wc < 0 && hc < 0) { wc = (int)((w + cw - 1) / cw); hc = (int)((h + ch - 1) / ch); }
    else if (wc < 0) wc = keep ? (int)(hc * ch * w / h / cw + 0.999) : (int)((w + cw - 1) / cw);
    else if (hc < 0) hc = keep ? (int)(wc * cw * h / w / ch + 0.999) : (int)((h + ch - 1) / ch);
    if (wc > t->cols - t->cx) {     /* does not fit horizontally: shrunk keeping the aspect ratio */
        int nw = t->cols - t->cx;
        if (keep && wc > 0) hc = hc * nw / wc;
        wc = nw;
    }
    if (wc < 1) wc = 1;
    if (hc < 1) hc = 1;
    if (hc > 4 * t->rows) hc = 4 * t->rows;

    add_image(t, px, w, h, wc, hc);
}

/* Already decoded image → anchored at the cursor, occupying wc×hc cells. */
static void add_image(Term *t, uint32_t *px, int w, int h, int wc, int hc) {
    if (t->nimages >= MAX_IMAGES) { image_free(&t->images[0]); memmove(t->images, t->images + 1, (size_t)--t->nimages * sizeof *t->images); }
    if (!t->images) t->images = calloc(MAX_IMAGES, sizeof *t->images);
    if (!t->images) { free(px); return; }
    /* The space is reserved by moving the cursor down; the image stays anchored to its first line. */
    int col = t->cx;
    for (int i = 0; i < hc - 1; i++) index_down(t);
    long long line = vt_cursor_abs(t) - (hc - 1);
    t->images[t->nimages++] = (TermImage){ line, col, wc, hc, w, h, px, NULL, 0, 0 };
    t->cx = col + wc < t->cols ? col + wc : t->cols - 1;
    t->wrap_pending = 0;
    dirty_all(t);
}

/* ---- Sixel (DCS q): VT340 graphics used by img2sixel, chafa, gnuplot… ---- */

static uint32_t hls(int h, int l, int sat) {
    float H = (float)((h + 240) % 360) / 360, L = l / 100.0f, S = sat / 100.0f;
    float q = L < 0.5f ? L * (1 + S) : L + S - L * S, p = 2 * L - q;
    float c[3];
    for (int i = 0; i < 3; i++) {
        float tt = H + (1 - i) / 3.0f;
        if (tt < 0) tt += 1;
        if (tt > 1) tt -= 1;
        c[i] = tt < 1 / 6.0f ? p + (q - p) * 6 * tt : tt < 0.5f ? q : tt < 2 / 3.0f ? p + (q - p) * (2 / 3.0f - tt) * 6 : p;
    }
    return 0xff000000u | (uint32_t)(c[0] * 255) << 16 | (uint32_t)(c[1] * 255) << 8 | (uint32_t)(c[2] * 255);
}

static int sixel_num(const char **p) {
    int v = 0;
    while (**p >= '0' && **p <= '9') { if (v < 100000) v = v * 10 + (**p - '0'); (*p)++; }
    return v;
}

static void sixel(Term *t, const char *s) {
    if (t->buf != &t->main) return;
    const char *p = s;
    while (*p && *p != 'q') p++;
    if (!*p) return;
    p++;
    static const uint8_t vt340[16][3] = { {0,0,0},{20,20,80},{80,13,13},{20,80,20},{80,20,80},{20,80,80},{80,80,20},{53,53,53},
        {26,26,26},{33,33,60},{60,26,26},{33,60,33},{60,33,60},{33,60,60},{60,60,33},{80,80,80} };
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) {
        const uint8_t *c = vt340[i % 16];
        pal[i] = 0xff000000u | (uint32_t)(c[0] * 255 / 100) << 16 | (uint32_t)(c[1] * 255 / 100) << 8 | (uint32_t)(c[2] * 255 / 100);
    }
    int cw = 0, ch = 0, w = 0, h = 0, x = 0, y = 0, color = 0;
    uint32_t *px = NULL;
    while (*p) {
        char c = *p;
        if (c == '"') {
            p++;
            sixel_num(&p); if (*p == ';') p++;
            sixel_num(&p); if (*p == ';') p++;
            int rw = sixel_num(&p); if (*p == ';') p++;
            int rh = sixel_num(&p);
            if (rw > 0 && rh > 0 && rw <= 4096 && rh <= 4096 && !px) {
                cw = rw; ch = (rh + 5) / 6 * 6;
                px = calloc((size_t)cw * ch, 4);
                if (!px) return;
            }
        } else if (c == '#') {
            p++;
            int n = sixel_num(&p) & 255;
            if (*p == ';') {
                p++;
                int mode = sixel_num(&p); if (*p == ';') p++;
                int a = sixel_num(&p); if (*p == ';') p++;
                int b = sixel_num(&p); if (*p == ';') p++;
                int d = sixel_num(&p);
                pal[n] = mode == 1 ? hls(a, b, d)
                       : 0xff000000u | (uint32_t)(a * 255 / 100) << 16 | (uint32_t)(b * 255 / 100) << 8 | (uint32_t)(d * 255 / 100);
            }
            color = n;
        } else if (c == '$') { x = 0; p++; }
        else if (c == '-') { x = 0; y += 6; p++; }
        else if (c == '!' || (c >= '?' && c <= '~')) {
            int rep = 1;
            if (c == '!') { p++; rep = sixel_num(&p); if (rep < 1) rep = 1; c = *p; if (!(c >= '?' && c <= '~')) continue; }
            int bits = c - '?';
            p++;
            if (x + rep > cw || y + 6 > ch) {      /* the canvas grows */
                int nw = cw, nh = ch;
                while (x + rep > nw) nw = nw ? nw * 2 : 256;
                while (y + 6 > nh) nh = nh ? nh * 2 : 96;
                if (nw > 4096) nw = 4096;
                if (nh > 4096) nh = 4096;
                if (x + rep > nw || y + 6 > nh) { rep = nw - x; if (rep <= 0 || y + 6 > nh) continue; }
                uint32_t *n2 = calloc((size_t)nw * nh, 4);
                if (!n2) break;
                for (int r = 0; r < ch; r++) if (px) memcpy(n2 + (size_t)r * nw, px + (size_t)r * cw, (size_t)cw * 4);
                free(px);
                px = n2; cw = nw; ch = nh;
            }
            for (int k = 0; k < rep; k++, x++)
                for (int b = 0; b < 6; b++)
                    if (bits & (1 << b)) px[(size_t)(y + b) * cw + x] = pal[color];
            if (x > w) w = x;
            if (y + 6 > h) h = y + 6;
        } else p++;
    }
    if (!px || !w || !h) { free(px); return; }
    uint32_t *img = malloc((size_t)w * h * 4);
    if (!img) { free(px); return; }
    for (int r = 0; r < h; r++) memcpy(img + (size_t)r * w, px + (size_t)r * cw, (size_t)w * 4);
    free(px);
    float fw = t->cell_w > 0 ? t->cell_w : 8, fh = t->cell_h > 0 ? t->cell_h : 16;
    int wc = (int)((w + fw - 1) / fw), hc = (int)((h + fh - 1) / fh);
    if (wc > t->cols - t->cx) wc = t->cols - t->cx;
    add_image(t, img, w, h, wc < 1 ? 1 : wc, hc < 1 ? 1 : hc);
}

static void osc_1337(Term *t, char *s) {
    char *eq = strchr(s, '=');
    char *val = eq ? eq + 1 : "";
    if (eq) *eq = 0;
    if (!strcmp(s, "SetMark")) vt_set_mark(t);
    else if (!strcmp(s, "CurrentDir")) { copy_str(t->cwd, sizeof t->cwd, val); t->events |= EV_CWD; }
    else if (!strcmp(s, "RemoteHost")) { if (strcmp(t->host, val)) { copy_str(t->host, sizeof t->host, val); t->events |= EV_HOST; } }
    else if (!strcmp(s, "SetBadgeFormat")) {
        char *b = b64_text(val, sizeof t->badge - 1);
        if (b) { copy_str(t->badge, sizeof t->badge, b); free(b); t->events |= EV_BADGE; }
    } else if (!strcmp(s, "File")) {
        char *colon = strchr(val, ':');
        if (colon) { *colon = 0; place_image(t, val, colon + 1); }
    } else if (!strcmp(s, "ClearScrollback")) vt_clear_scrollback(t);
    else if (!strcmp(s, "CursorShape")) {
        int v = atoi(val);
        t->cursor_shape = v == 1 ? 6 : v == 2 ? 4 : 2;
        vt_mark_dirty(t, t->cy);
    } else if (!strcmp(s, "SetProfile")) { copy_str(t->profile_req, sizeof t->profile_req, val); t->events |= EV_PROFILE; }
    else if (!strcmp(s, "RequestAttention")) t->events |= EV_ATTENTION;
    else if (!strcmp(s, "ReportCellSize")) reply(t, "\033]1337;ReportCellSize=%.1f;%.1f\a", t->cell_h, t->cell_w);
    else if (!strcmp(s, "Copy")) {
        const char *d = strchr(val, ':');
        char *b = b64_text(d ? d + 1 : val, 1u << 20);
        if (b) { term_set_clipboard(b, strlen(b)); free(b); t->events |= EV_COPY; }
    } else if (!strcmp(s, "HighlightCursorLine")) { t->prof->cursor_guide = !strcmp(val, "yes") || !strcmp(val, "true"); dirty_all(t); }
}

static void osc(Term *t) {
    char *s = t->str;
    int num = 0;
    while (*s >= '0' && *s <= '9') num = num * 10 + (*s++ - '0');
    if (*s == ';') s++;
    else if (*s) return;
    switch (num) {
    case 0: copy_str(t->title, sizeof t->title, s); copy_str(t->icon_title, sizeof t->icon_title, s); t->events |= EV_TITLE; break;
    case 1: copy_str(t->icon_title, sizeof t->icon_title, s); break;
    case 2: copy_str(t->title, sizeof t->title, s); t->events |= EV_TITLE; break;
    case 4:
        for (char *p = s; p && *p;) {
            char *semi = strchr(p, ';');
            if (!semi) break;
            int idx = atoi(p);
            char *spec = semi + 1, *next = strchr(spec, ';');
            if (next) *next = 0;
            if (idx >= 0 && idx < 256) {
                if (!strcmp(spec, "?")) { char pre[16]; snprintf(pre, sizeof pre, "4;%d", idx); reply_color(t, pre, t->palette[idx]); }
                else { uint32_t c; if (!parse_color(spec, &c)) { t->palette[idx] = c; dirty_all(t); } }
            }
            p = next ? next + 1 : NULL;
        }
        break;
    case 7:
        if (!strncmp(s, "file://", 7)) {
            const char *path = strchr(s + 7, '/');
            if (path) { url_decode(t->cwd, sizeof t->cwd, path); t->events |= EV_CWD; }
            /* file://host/path: if the shell did not report user@host (OSC 1337), at least the host */
            char host[256];
            size_t hl = path ? (size_t)(path - (s + 7)) : 0;
            if (hl && hl < sizeof host && !strchr(t->host, '@')) {
                memcpy(host, s + 7, hl);
                host[hl] = 0;
                if (strcmp(t->host, host)) { copy_str(t->host, sizeof t->host, host); t->events |= EV_HOST; }
            }
        }
        break;
    case 8: {
        char *semi = strchr(s, ';');
        if (!semi) break;
        t->pen.link = semi[1] ? (uint16_t)add_link(t, semi + 1) : 0;
        break;
    }
    case 9:
        if (!(isdigit((unsigned char)s[0]) && s[1] == ';')) { copy_str(t->notify, sizeof t->notify, s); t->events |= EV_NOTIFY; }
        break;
    case 10: case 11: case 12: {
        uint32_t c;
        uint32_t cur = num == 10 ? vt_resolve(t, 0, 1) : num == 11 ? vt_resolve(t, 0, 0)
                     : t->cursor_set ? t->cursor_color : (t->prof->cursor | 0xff000000u);
        char pre[4];
        snprintf(pre, sizeof pre, "%d", num);
        if (!strcmp(s, "?")) reply_color(t, pre, cur);
        else if (!parse_color(s, &c)) {
            if (num == 10) { t->fg = c; t->fg_set = 1; }
            else if (num == 11) { t->bg = c; t->bg_set = 1; }
            else { t->cursor_color = c; t->cursor_set = 1; }
            dirty_all(t);
        }
        break;
    }
    case 52: {
        char *semi = strchr(s, ';');
        if (!semi || !strcmp(semi + 1, "?")) break;    /* reading the clipboard is not allowed */
        char *b = b64_text(semi + 1, 1u << 20);
        if (b) { term_set_clipboard(b, strlen(b)); free(b); t->events |= EV_COPY; }
        break;
    }
    case 104:
        if (!*s) vt_apply_palette(t);
        else for (char *p = s; p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL) {
            int i = atoi(p);
            if (i >= 0 && i < 16) t->palette[i] = t->prof->palette[i] | 0xff000000u;
        }
        dirty_all(t);
        break;
    case 110: t->fg_set = 0; dirty_all(t); break;
    case 111: t->bg_set = 0; dirty_all(t); break;
    case 112: t->cursor_set = 0; break;
    case 133: {
        Line *l = L(t, t->cy);
        if (s[0] == 'A') { l->flags |= LINE_PROMPT; l->flags &= (uint8_t)~(LINE_OK | LINE_FAIL); vt_mark_dirty(t, t->cy); }
        else if (s[0] == 'B') { t->cmd_line = vt_cursor_abs(t); t->cmd_col = t->cx; }
        else if (s[0] == 'C') {
            /* The command is what was typed between the end of the prompt and the start of the output. */
            long long end = vt_cursor_abs(t) - (t->cx == 0 ? 1 : 0);
            if (t->cmd_line >= vt_first_abs(t) && t->cmd_line <= end && end - t->cmd_line < 50) {
                Str txt = {0};
                for (long long a = t->cmd_line; a <= end; a++) {
                    Line *cl = vt_abs_line(t, a);
                    line_text(&txt, cl, a == t->cmd_line ? t->cmd_col : 0, t->cols - 1, cl && (cl->flags & LINE_WRAPPED));
                }
                if (txt.s) {
                    char *p = txt.s;
                    while (*p == ' ') p++;
                    snprintf(t->run_cmd, sizeof t->run_cmd, "%s", p);
                    if (t->run_cmd[0]) t->events |= EV_RUN;
                    free(txt.s);
                }
            }
            t->cmd_line = -1;
            t->cmd_prompt = -1;
            for (long long a = vt_cursor_abs(t); a >= vt_first_abs(t); a--) {
                Line *p = vt_abs_line(t, a);
                if (p && p->flags & LINE_PROMPT) { t->cmd_prompt = a; break; }
            }
            l->flags |= LINE_OUTPUT;
        } else if (s[0] == 'D') {
            Line *p = t->cmd_prompt >= 0 ? vt_abs_line(t, t->cmd_prompt) : NULL;
            if (p) {
                int code = s[1] == ';' ? atoi(s + 2) : 0;
                p->flags |= code ? LINE_FAIL : LINE_OK;
                snprintf(t->command, sizeof t->command, "%d", code);
                t->events |= EV_COMMAND;
                dirty_all(t);
            }
            t->cmd_prompt = -1;
        }
        break;
    }
    case 1337: osc_1337(t, s); break;
    default: break;
    }
}

static void dcs(Term *t) {
    const char *p = t->str;
    while ((*p >= '0' && *p <= '9') || *p == ';') p++;
    if (*p == 'q') { sixel(t, t->str); return; }
    if (!strncmp(t->str, "$q", 2)) reply(t, "\033P0$r\033\\");
    else if (!strncmp(t->str, "+q", 2)) reply(t, "\033P0+r\033\\");
}

/* ---- parser ---- */

static void str_add(Term *t, uint8_t b) {
    size_t max = (t->str_len >= 10 && !strncmp(t->str, "1337;File=", 10)) || t->state == S_DCS || t->state == S_TMUX ? STR_MAX_FILE : STR_MAX;
    if (t->str_len + 1 >= max) return;
    if (t->str_len + 1 >= t->str_cap) {
        size_t cap = t->str_cap ? t->str_cap * 2 : 256;
        char *s = realloc(t->str, cap);
        if (!s) return;
        t->str = s;
        t->str_cap = cap;
    }
    t->str[t->str_len++] = (char)b;
    t->str[t->str_len] = 0;
}

static void str_begin(Term *t) {
    t->str_len = 0;
    if (t->str) t->str[0] = 0;
    else str_add(t, 0), t->str_len = 0;
}

static void str_end(Term *t) {
    /* Frees large buffers (images) so as not to retain memory. */
    if (t->str_cap > STR_MAX) { free(t->str); t->str = NULL; t->str_cap = 0; }
    t->str_len = 0;
}

static void control(Term *t, uint8_t b) {
    switch (b) {
    case 0x07: t->events |= EV_BELL; break;
    case 0x08: if (t->cx > 0) { t->cx--; vt_mark_dirty(t, t->cy); } t->wrap_pending = 0; break;
    case 0x09: {
        int x = t->cx + 1;
        while (x < t->cols - 1 && !t->tabs[x]) x++;
        t->cx = x < t->cols ? x : t->cols - 1;
        t->wrap_pending = 0;
        vt_mark_dirty(t, t->cy);
        break;
    }
    case 0x0a: case 0x0b: case 0x0c:
        index_down(t);
        if (t->modes & M_LNM) t->cx = 0;
        t->wrap_pending = 0;
        break;
    case 0x0d: t->cx = 0; t->wrap_pending = 0; vt_mark_dirty(t, t->cy); break;
    case 0x0e: t->gl = 1; break;
    case 0x0f: t->gl = 0; break;
    case 0x18: case 0x1a: t->state = S_GROUND; break;
    case 0x1b: t->state = S_ESC; t->ninter = 0; break;
    default: break;
    }
}

static void esc(Term *t, uint8_t b) {
    t->state = S_GROUND;
    switch (b) {
    case '[':
        t->state = S_CSI;
        t->nparams = 0;
        t->priv = 0;
        t->ninter = 0;
        memset(t->colon, 0, sizeof t->colon);
        break;
    case ']': t->state = S_OSC; str_begin(t); break;
    case 'P': t->state = S_DCS; str_begin(t); break;
    case 'X': case '^': case '_': t->state = S_IGNORE; break;
    case '7': save_cursor(t, &t->saved); break;
    case '8': restore_cursor(t, &t->saved); break;
    case 'D': index_down(t); break;
    case 'E': t->cx = 0; index_down(t); break;
    case 'H': if (t->cx < t->cols) t->tabs[t->cx] = 1; break;
    case 'M': reverse_index(t); break;
    case 'c': vt_reset(t); break;
    case '=': t->modes |= M_APPKEYPAD; break;
    case '>': t->modes &= ~(uint32_t)M_APPKEYPAD; break;
    default:
        if (b >= 0x20 && b <= 0x2f) { t->inter[0] = (char)b; t->ninter = 1; t->state = S_ESC_INT; }
        break;
    }
}

static void esc_int(Term *t, uint8_t b) {
    if (b >= 0x20 && b <= 0x2f) { if (t->ninter < 3) t->inter[t->ninter++] = (char)b; return; }
    t->state = S_GROUND;
    char i0 = t->inter[0];
    if (i0 == '(' || i0 == ')') t->g[i0 == ')'] = b == '0';
    else if (i0 == '#' && b == '8') {     /* DECALN: screen of E's for alignment */
        for (int r = 0; r < t->rows; r++) {
            Line *l = L(t, r);
            line_fill(l, 0, t->cols, (Cell){ 'E', 0, 0, 0, 0 });
            vt_mark_dirty(t, r);
        }
    }
}

static void csi_byte(Term *t, uint8_t b) {
    if (b >= '0' && b <= '9') {
        if (t->nparams == 0) { t->nparams = 1; t->params[0] = -1; }
        int *p = &t->params[t->nparams - 1];
        if (*p < 0) *p = 0;
        if (*p < 100000) *p = *p * 10 + (b - '0');
    } else if (b == ';' || b == ':') {
        if (t->nparams == 0) { t->nparams = 1; t->params[0] = -1; }
        if (t->nparams < 32) { t->colon[t->nparams] = b == ':'; t->params[t->nparams++] = -1; }
    } else if (b >= 0x3c && b <= 0x3f) {
        if (t->nparams == 0) t->priv = (char)b;
    } else if (b >= 0x20 && b <= 0x2f) {
        if (t->ninter < 3) t->inter[t->ninter++] = (char)b;
    } else if (b >= 0x40 && b <= 0x7e) {
        t->state = S_GROUND;
        csi(t, b);
    } else if (b < 0x20) {
        if (b == 0x1b) { t->state = S_ESC; t->ninter = 0; }
        else control(t, b);
    }
}

/* The byte after the ESC that ended a string (OSC, or SOS/PM/APC that are ignored): '\\' completes ST; anything
 * else begins what follows as it would after any ESC. Another ESC starts a new sequence and a control is run:
 * neither is swallowed (ESC ] … ESC ESC [ 14 G lost the second ESC and printed "[14G"; after an APC ended that
 * way everything was ignored up to the next ST). */
static void string_esc(Term *t, uint8_t b) {
    t->state = S_GROUND;
    if (b == '\\') return;
    t->state = S_ESC;
    t->ninter = 0;
    if (b == 0x1b) return;
    if (b < 0x20) { control(t, b); return; }
    esc(t, b);
}

void vt_write(Term *t, const uint8_t *d, size_t n) {
    now_s = (uint32_t)time(NULL);
    for (size_t i = 0; i < n; i++) {
        uint8_t b = d[i];
        switch (t->state) {
        case S_GROUND:
            if (t->utf8_left) {
                if ((b & 0xc0) == 0x80) {
                    t->utf8_cp = t->utf8_cp << 6 | (b & 0x3f);
                    if (--t->utf8_left == 0) put_char(t, t->utf8_cp > 0x10ffff ? 0xfffd : t->utf8_cp);
                    continue;
                }
                t->utf8_left = 0;
                put_char(t, 0xfffd);
            }
            if (b >= 0x20 && b < 0x7f) {
                put_char(t, b);
                /* Fast path: contiguous ASCII text. */
                while (i + 1 < n && d[i + 1] >= 0x20 && d[i + 1] < 0x7f) put_char(t, d[++i]);
            } else if (b < 0x20) control(t, b);
            else if (b == 0x7f) {}
            else if ((b & 0xe0) == 0xc0) { t->utf8_cp = b & 0x1f; t->utf8_left = 1; }
            else if ((b & 0xf0) == 0xe0) { t->utf8_cp = b & 0x0f; t->utf8_left = 2; }
            else if ((b & 0xf8) == 0xf0) { t->utf8_cp = b & 0x07; t->utf8_left = 3; }
            else put_char(t, 0xfffd);
            break;
        case S_ESC:
            if (b == 0x1b) break;
            if (b < 0x20) { control(t, b); break; }
            esc(t, b);
            break;
        case S_ESC_INT: if (b < 0x20) control(t, b); else esc_int(t, b); break;
        case S_CSI: csi_byte(t, b); break;
        case S_OSC:
            if (b == 0x07) { osc(t); str_end(t); t->state = S_GROUND; }
            else if (b == 0x1b) t->state = S_OSC_ESC;
            else if (b == 0x18 || b == 0x1a) { str_end(t); t->state = S_GROUND; }
            else str_add(t, b);
            break;
        case S_OSC_ESC:
            osc(t);
            str_end(t);
            string_esc(t, b);
            break;
        case S_DCS:
            if (b == 0x1b) t->state = S_DCS_ESC;
            else if (b == 0x18 || b == 0x1a) { str_end(t); t->state = S_GROUND; }
            else if (b == 'p' && t->tmux && t->str_len == 4 && !memcmp(t->str, "1000", 4)) {
                /* tmux -CC: control mode. Everything that follows is protocol lines until ST. */
                str_end(t);
                str_begin(t);
                t->state = S_TMUX;
                t->tmux(t, 1, NULL, 0);
            }
            else str_add(t, b);
            break;
        case S_TMUX:
            if (b == '\n') {
                size_t len = t->str_len;
                if (len && t->str[len - 1] == '\r') len--;
                if (t->str) t->str[len] = 0;
                t->tmux(t, 0, t->str ? t->str : "", len);
                t->str_len = 0;
            } else if (b == 0x1b) t->state = S_TMUX_ESC;
            else str_add(t, b);
            break;
        case S_TMUX_ESC:
            if (b == '\\') { str_end(t); t->state = S_GROUND; t->tmux(t, 2, NULL, 0); }
            else { t->state = S_TMUX; str_add(t, 0x1b); str_add(t, b); }
            break;
        case S_DCS_ESC:
            if (b == '\\') { dcs(t); str_end(t); t->state = S_GROUND; }
            else { str_add(t, 0x1b); str_add(t, b); t->state = S_DCS; }
            break;
        case S_IGNORE:
            if (b == 0x1b) t->state = S_IGNORE_ESC;
            else if (b == 0x18 || b == 0x1a) t->state = S_GROUND;
            break;
        case S_IGNORE_ESC: string_esc(t, b); break;
        }
    }
}

/* ---- life cycle ---- */

int vt_init(Term *t, int cols, int rows, int scrollback, TermProfile *p) {
    memset(t, 0, sizeof *t);
    if (cols < 2) cols = 2;
    if (rows < 1) rows = 1;
    t->cols = cols;
    t->rows = rows;
    t->prof = p;
    if (buf_init(&t->main, scrollback + rows, rows) || buf_init(&t->alt, rows, rows)) return -1;
    t->buf = &t->main;
    t->tabs = calloc((size_t)cols, 1);
    t->dirty = calloc((size_t)rows, 1);
    if (!t->tabs || !t->dirty) return -1;
    t->cmd_prompt = -1;
    t->cmd_line = -1;
    t->focused = 1;
    vt_reset(t);
    return 0;
}

void vt_reset(Term *t) {
    if (t->buf == &t->alt) t->buf = &t->main;
    t->modes = M_AUTOWRAP | M_ALTSCROLL;
    t->pen = (Cell){0};
    t->top = 0;
    t->bot = t->rows - 1;
    t->cx = t->cy = t->wrap_pending = 0;
    t->g[0] = t->g[1] = t->gl = 0;
    t->cursor_shape = 0;
    t->nkitty[0] = t->nkitty[1] = t->other_keys = 0;
    t->fg_set = t->bg_set = t->cursor_set = 0;
    t->state = S_GROUND;
    t->utf8_left = 0;
    reset_tabs(t, 0);
    for (int r = 0; r < t->rows; r++) line_reset(L(t, r));
    save_cursor(t, &t->saved);
    save_cursor(t, &t->saved_alt);
    vt_apply_palette(t);
    dirty_all(t);
}

void vt_free(Term *t) {
    buf_free(&t->main);
    buf_free(&t->alt);
    free(t->tabs);
    free(t->dirty);
    free(t->str);
    for (int i = 0; i < t->nlinks; i++) free(t->links[i]);
    free(t->links);
    for (int i = 0; i < t->nimages; i++) image_free(&t->images[i]);
    free(t->images);
    free(t->matches);
    memset(t, 0, sizeof *t);
}

/* ---- resize with reflow (main screen) ---- */

typedef struct { Line *v; int n, cap; } LineVec;

static Line *vec_push(LineVec *v) {
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 256;
        Line *nv = realloc(v->v, (size_t)cap * sizeof *nv);
        if (!nv) return NULL;
        v->v = nv;
        v->cap = cap;
    }
    Line *l = &v->v[v->n++];
    memset(l, 0, sizeof *l);
    return l;
}

static void reflow_main(Term *t, int ncols, int nrows, int *ccx, int *ccy) {
    Buffer *b = &t->main;
    int ocols = t->cols;
    int count = b->count;
    int cur = count - t->rows + *ccy;
    while (count - 1 > cur && bl(b, count - 1)->len == 0 && !bl(b, count - 1)->flags) count--;

    LineVec out = {0};
    int *map = malloc((size_t)count * sizeof *map);
    size_t cells_cap = (size_t)ocols * 64;
    Cell *cells = malloc(cells_cap * sizeof *cells);
    int new_cur = 0, new_col = 0;
    if (!map || !cells) { free(map); free(cells); return; }

    for (int i = 0; i < count;) {
        int j = i;
        while (j < count - 1 && (bl(b, j)->flags & LINE_WRAPPED)) j++;
        size_t total = 0;
        for (int k = i; k <= j; k++) total += k < j ? (size_t)ocols : bl(b, k)->len;
        if (total + 1 > cells_cap) {
            Cell *nc = realloc(cells, (total + 1) * sizeof *nc);
            if (!nc) break;
            cells = nc;
            cells_cap = total + 1;
        }
        size_t pos = 0;
        for (int k = i; k <= j; k++) {
            Line *l = bl(b, k);
            int w = k < j ? ocols : l->len;
            /* Padding cell left by a wide character that did not fit at the end: skipped. */
            if (k < j && (l->len < ocols || (!l->c[ocols - 1].cp && !l->c[ocols - 1].attr))) {
                Line *nx = bl(b, k + 1);
                if (nx->len && (nx->c[0].attr & A_WIDE)) w = ocols - 1;
            }
            for (int x = 0; x < w; x++) cells[pos++] = x < l->len ? l->c[x] : (Cell){0};
        }
        while (total > 0 && cell_blank(cells[total - 1])) total--;
        long cur_off = cur >= i && cur <= j ? (long)(cur - i) * ocols + *ccx : -1;

        uint8_t first_flags = bl(b, i)->flags & (uint8_t)~LINE_WRAPPED;
        uint32_t time0 = bl(b, i)->time;
        int first_new = out.n;
        size_t start = 0;
        do {
            size_t len = total - start > (size_t)ncols ? (size_t)ncols : total - start;
            if (len == (size_t)ncols && len > 1 && (cells[start + len - 1].attr & A_WIDE)) len--;
            Line *nl = vec_push(&out);
            if (!nl) break;
            nl->flags = start == 0 ? first_flags : 0;
            nl->time = time0;
            if (len) {
                line_reserve(nl, (int)len);
                memcpy(nl->c, cells + start, len * sizeof *cells);
                nl->len = (uint16_t)len;
            }
            if (cur_off >= 0 && (size_t)cur_off >= start && ((size_t)cur_off < start + len || start + len >= total)) {
                long col = cur_off - (long)start;
                while (col >= ncols) {
                    nl->flags |= LINE_WRAPPED;
                    nl = vec_push(&out);
                    if (!nl) break;
                    col -= ncols;
                }
                new_cur = out.n - 1;
                new_col = (int)col;
                cur_off = -1;
            }
            start += len;
            if (start < total) nl->flags |= LINE_WRAPPED;
        } while (start < total);
        for (int k = i; k <= j; k++) {
            long off = (long)(k - i) * ocols / ncols;
            map[k] = first_new + (int)(off < out.n - first_new ? off : out.n - first_new - 1);
        }
        i = j + 1;
    }
    free(cells);

    /* Lines left below the cursor that do not fit on screen are dropped. */
    while (out.n - new_cur > nrows) { out.n--; free(out.v[out.n].c); }
    while (out.n < nrows) vec_push(&out);

    int cap = b->cap - t->rows + nrows;
    int drop = out.n > cap ? out.n - cap : 0;
    for (int i = 0; i < drop; i++) free(out.v[i].c);
    for (int i = 0; i < b->cap; i++) free(b->ring[i].c);
    free(b->ring);
    b->ring = calloc((size_t)cap, sizeof *b->ring);
    b->cap = cap;
    b->head = 0;
    b->count = out.n - drop;
    long long old_dropped = b->dropped;
    b->dropped += drop;
    for (int i = 0; i < b->count; i++) b->ring[i] = out.v[drop + i];
    free(out.v);

    for (int k = 0; k < t->nimages; k++) {
        long long oi = t->images[k].line - old_dropped;
        t->images[k].line = oi >= 0 && oi < count ? b->dropped + map[oi] - drop : -1000000;
        free(t->images[k].scaled);
        t->images[k].scaled = NULL;
    }
    free(map);
    *ccy = new_cur - drop - (b->count - nrows);
    if (*ccy < 0) *ccy = 0;
    *ccx = new_col < ncols ? new_col : ncols - 1;
}

static void resize_alt(Term *t, int ncols, int nrows) {
    Buffer *b = &t->alt;
    Line *ring = calloc((size_t)nrows, sizeof *ring);
    if (!ring) return;
    int keep = t->rows < nrows ? t->rows : nrows;
    for (int r = 0; r < t->rows; r++) {
        Line *l = bl(b, r);
        if (r < keep) {
            ring[r] = *l;
            if (ring[r].len > ncols) ring[r].len = (uint16_t)ncols;
            l->c = NULL;
        }
    }
    (void)ncols;
    buf_free(b);
    b->ring = ring;
    b->cap = b->count = nrows;
    b->head = 0;
}

void vt_resize(Term *t, int ncols, int nrows) {
    if (ncols < 2) ncols = 2;
    if (nrows < 1) nrows = 1;
    if (ncols > TERM_MAX_COLS) ncols = TERM_MAX_COLS;
    if (nrows > TERM_MAX_ROWS) nrows = TERM_MAX_ROWS;
    if (ncols == t->cols && nrows == t->rows) return;
    uint8_t *tabs = realloc(t->tabs, (size_t)ncols);
    uint8_t *dirty = realloc(t->dirty, (size_t)nrows);
    if (!tabs || !dirty) return;
    t->tabs = tabs;
    t->dirty = dirty;
    int in_alt = t->buf == &t->alt;
    int mcx = in_alt ? t->saved_alt.cx : t->cx, mcy = in_alt ? t->saved_alt.cy : t->cy;
    reflow_main(t, ncols, nrows, &mcx, &mcy);
    resize_alt(t, ncols, nrows);
    int ocols = t->cols;
    t->cols = ncols;
    t->rows = nrows;
    if (in_alt) {
        t->saved_alt.cx = mcx;
        t->saved_alt.cy = mcy;
        t->cx = t->cx < ncols ? t->cx : ncols - 1;
        t->cy = t->cy < nrows ? t->cy : nrows - 1;
    } else { t->cx = mcx; t->cy = mcy; }
    t->wrap_pending = 0;
    if (t->saved.cx >= ncols) t->saved.cx = ncols - 1;
    if (t->saved.cy >= nrows) t->saved.cy = nrows - 1;
    t->top = 0;
    t->bot = nrows - 1;
    reset_tabs(t, ocols < ncols ? ocols : ncols);
    if (ocols > ncols) reset_tabs(t, 0);
    memset(t->dirty, 1, (size_t)nrows);
    t->view = 0;
    t->sel_active = 0;
    vt_search_clear(t);
    dirty_all(t);
}

/* ---- history and view ---- */

void vt_scroll_view(Term *t, int delta) {
    int max = t->buf->count - t->rows;
    int v = t->view + delta;
    if (v < 0) v = 0;
    if (v > max) v = max;
    if (v != t->view) { t->view = v; dirty_all(t); }
}

void vt_clear_scrollback(Term *t) {
    if (t->buf != &t->main) return;
    Buffer *b = &t->main;
    int extra = b->count - t->rows;
    for (int i = 0; i < extra; i++) line_reset(bl(b, i));
    b->head = (b->head + extra) % b->cap;
    b->count = t->rows;
    b->dropped += extra;
    t->view = 0;
    t->sel_active = 0;
    vt_search_clear(t);
    prune_images(t);
    dirty_all(t);
}

/* ---- selection ---- */

static int url_char(uint32_t cp);

static int is_word(const Term *t, uint32_t cp) {
    if (!cp) return 0;
    if (cp < 128 && strchr(t->prof->word_chars, (int)cp)) return 1;
    return iswalnum((wint_t)cp);
}

static uint32_t cell_cp(Term *t, long long line, int col) {
    Line *l = vt_abs_line(t, line);
    if (!l || col < 0 || col >= l->len) return 0;
    if (l->c[col].attr & A_WIDE_TAIL && col > 0) return vt_base_cp(l->c[col - 1].cp);
    return vt_base_cp(l->c[col].cp);
}

static void sel_update(Term *t) {
    long long sl = t->sel_al, el = t->sel_bl;
    int sc = t->sel_ac, ec = t->sel_bc;
    if (!t->sel_rect && (sl > el || (sl == el && sc > ec))) { long long tl = sl; sl = el; el = tl; int tc = sc; sc = ec; ec = tc; }
    if (t->sel_rect) {
        if (sl > el) { long long tl = sl; sl = el; el = tl; }
        if (sc > ec) { int tc = sc; sc = ec; ec = tc; }
    }
    if (t->sel_mode == 1) {
        if (is_word(t, cell_cp(t, sl, sc))) while (sc > 0 && is_word(t, cell_cp(t, sl, sc - 1))) sc--;
        if (is_word(t, cell_cp(t, el, ec))) while (ec < t->cols - 1 && is_word(t, cell_cp(t, el, ec + 1))) ec++;
    } else if (t->sel_mode == 3) {     /* smart selection: URL, path, email… */
        if (url_char(cell_cp(t, sl, sc))) while (sc > 0 && url_char(cell_cp(t, sl, sc - 1))) sc--;
        if (url_char(cell_cp(t, el, ec))) while (ec < t->cols - 1 && url_char(cell_cp(t, el, ec + 1))) ec++;
    } else if (t->sel_mode == 2) {
        while (sl > vt_first_abs(t)) { Line *p = vt_abs_line(t, sl - 1); if (!p || !(p->flags & LINE_WRAPPED)) break; sl--; }
        for (;;) { Line *l = vt_abs_line(t, el); if (!l || !(l->flags & LINE_WRAPPED)) break; el++; }
        sc = 0;
        ec = t->cols - 1;
    }
    t->sel_sl = sl; t->sel_sc = sc; t->sel_el = el; t->sel_ec = ec;
    dirty_all(t);
}

void vt_select_start(Term *t, long long line, int col, int mode, int rect) {
    t->sel_active = 1;
    t->sel_mode = mode;
    t->sel_rect = rect;
    t->sel_al = t->sel_bl = line;
    t->sel_ac = t->sel_bc = col;
    sel_update(t);
}

void vt_select_extend(Term *t, long long line, int col) {
    if (!t->sel_active) return;
    t->sel_bl = line;
    t->sel_bc = col;
    sel_update(t);
}

void vt_select_all(Term *t) {
    t->sel_active = 1;
    t->sel_mode = 0;
    t->sel_rect = 0;
    t->sel_al = vt_first_abs(t);
    t->sel_ac = 0;
    t->sel_bl = vt_first_abs(t) + t->buf->count - 1;
    t->sel_bc = t->cols - 1;
    sel_update(t);
}

void vt_select_clear(Term *t) {
    if (t->sel_active) dirty_all(t);
    t->sel_active = 0;
}

int vt_selected(const Term *t, long long line, int col) {
    if (!t->sel_active || line < t->sel_sl || line > t->sel_el) return 0;
    if (t->sel_rect) return col >= t->sel_sc && col <= t->sel_ec;
    if (line == t->sel_sl && col < t->sel_sc) return 0;
    if (line == t->sel_el && col > t->sel_ec) return 0;
    return 1;
}

char *vt_selection_text(Term *t) {
    if (!t->sel_active) return NULL;
    Str s = {0};
    for (long long a = t->sel_sl; a <= t->sel_el; a++) {
        Line *l = vt_abs_line(t, a);
        int from = t->sel_rect || a == t->sel_sl ? t->sel_sc : 0;
        int to = t->sel_rect || a == t->sel_el ? t->sel_ec : t->cols - 1;
        int wrapped = l && (l->flags & LINE_WRAPPED) && !t->sel_rect;
        line_text(&s, l, from, to, wrapped && a != t->sel_el);
        if (a != t->sel_el && !wrapped) str_put(&s, "\n", 1);
    }
    if (!s.s) str_put(&s, "", 0);
    return s.s;
}

char *vt_text_range(Term *t, long long from, long long to) {
    Str s = {0};
    for (long long a = from; a <= to; a++) {
        Line *l = vt_abs_line(t, a);
        int wrapped = l && (l->flags & LINE_WRAPPED);
        line_text(&s, l, 0, t->cols - 1, wrapped);
        if (a != to && !wrapped) str_put(&s, "\n", 1);
    }
    if (!s.s) str_put(&s, "", 0);
    return s.s;
}

/* Selects the output of the last finished command (between OSC 133;C and the next prompt). */
int vt_select_last_output(Term *t) {
    long long first = vt_first_abs(t), a = vt_cursor_abs(t);
    while (a >= first) { Line *l = vt_abs_line(t, a); if (l && l->flags & LINE_PROMPT) break; a--; }
    long long prompt = a;
    for (a = prompt - 1; a >= first; a--) { Line *l = vt_abs_line(t, a); if (l && l->flags & LINE_OUTPUT) break; }
    if (a < first || prompt <= a) return 0;
    t->sel_active = 1; t->sel_mode = 0; t->sel_rect = 0;
    t->sel_al = a; t->sel_ac = 0; t->sel_bl = prompt - 1; t->sel_bc = t->cols - 1;
    sel_update(t);
    return 1;
}

/* ---- search ---- */

void vt_search_clear(Term *t) {
    if (t->nmatches) dirty_all(t);
    free(t->matches);
    t->matches = NULL;
    t->nmatches = 0;
    t->match_cur = -1;
}

static void show_line(Term *t, long long abs) {
    long long top = vt_first_abs(t) + t->buf->count - t->rows - t->view;
    if (abs >= top && abs < top + t->rows) return;
    int idx = (int)(abs - vt_first_abs(t));
    int v = t->buf->count - t->rows - (idx - t->rows / 2);
    int max = t->buf->count - t->rows;
    t->view = v < 0 ? 0 : v > max ? max : v;
    dirty_all(t);
}

static uint32_t fold(uint32_t cp, int cs) { return cs ? cp : (uint32_t)towlower((wint_t)cp); }

/* A logical line (the physical ones joined by line wrapping) as character codes,
 * with the grid position of each one. */
typedef struct { uint32_t *text; long long *pl; int *pc; size_t n, cap; } Gather;

static int gather_push(Gather *g, uint32_t cp, long long line, int col) {
    if (g->n + 1 > g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 4096;
        uint32_t *nt = realloc(g->text, cap * sizeof *nt);
        if (nt) g->text = nt;
        long long *npl = realloc(g->pl, cap * sizeof *npl);
        if (npl) g->pl = npl;
        int *npc = realloc(g->pc, cap * sizeof *npc);
        if (npc) g->pc = npc;
        if (!nt || !npl || !npc) return -1;
        g->cap = cap;
    }
    g->text[g->n] = cp;
    g->pl[g->n] = line;
    g->pc[g->n] = col;
    g->n++;
    return 0;
}

/* Gathers the logical line that starts at `a`; returns the first line of the next one. */
static long long gather_line(Term *t, long long a, Gather *g) {
    long long last = vt_first_abs(t) + t->buf->count - 1, b = a;
    g->n = 0;
    for (;; b++) {
        Line *l = vt_abs_line(t, b);
        int w = l ? ((l->flags & LINE_WRAPPED) ? t->cols : l->len) : 0;
        for (int x = 0; x < w; x++) {
            if (x < l->len && l->c[x].attr & A_WIDE_TAIL) continue;
            uint32_t cp = x < l->len ? l->c[x].cp : 0, seq[CLUSTER_LEN];
            int n = vt_cluster_cps(cp ? cp : ' ', seq, CLUSTER_LEN);
            for (int k = 0; k < n; k++) if (gather_push(g, seq[k], b, x)) return last + 1;
        }
        if (!l || !(l->flags & LINE_WRAPPED) || b >= last) break;
    }
    return b + 1;
}

static void gather_free(Gather *g) { free(g->text); free(g->pl); free(g->pc); }

static void add_match(Term *t, Gather *g, size_t i, size_t e) {
    Line *el = vt_abs_line(t, g->pl[e]);
    int ec = g->pc[e] + (el && g->pc[e] < el->len && (el->c[g->pc[e]].attr & A_WIDE) ? 1 : 0);
    t->matches[t->nmatches++] = (Match){ g->pl[i], g->pl[e], g->pc[i], ec };
}

static void search_done(Term *t) {
    if (t->nmatches) {
        t->match_cur = t->nmatches - 1;
        show_line(t, t->matches[t->match_cur].l0);
    }
    dirty_all(t);
}

int vt_search(Term *t, const char *query, int cs) {
    vt_search_clear(t);
    uint32_t q[256];
    int qn = 0;
    for (const unsigned char *p = (const unsigned char *)query; *p && qn < 256;) {
        uint32_t c = *p;
        int extra = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : 3;
        c &= extra == 0 ? 0x7f : extra == 1 ? 0x1f : extra == 2 ? 0x0f : 0x07;
        p++;
        for (int k = 0; k < extra && (*p & 0xc0) == 0x80; k++) c = c << 6 | (*p++ & 0x3f);
        q[qn++] = fold(c, cs);
    }
    if (!qn) return 0;
    t->matches = malloc(MAX_MATCHES * sizeof *t->matches);
    if (!t->matches) return 0;
    Gather g = {0};
    long long last = vt_first_abs(t) + t->buf->count - 1;
    for (long long a = vt_first_abs(t); a <= last && t->nmatches < MAX_MATCHES;) {
        a = gather_line(t, a, &g);
        for (size_t i = 0; i + (size_t)qn <= g.n && t->nmatches < MAX_MATCHES; i++) {
            int k = 0;
            while (k < qn && fold(g.text[i + k], cs) == q[k]) k++;
            if (k < qn) continue;
            add_match(t, &g, i, i + (size_t)qn - 1);
            i += (size_t)qn - 1;
        }
    }
    gather_free(&g);
    search_done(t);
    return t->nmatches;
}

/* ---- saving and restoring the screen (sessions that survive a restart) ---- */

static void sgr_color(Str *s, uint32_t c, int fg) {
    char b[32];
    if ((c >> 24) == 1) snprintf(b, sizeof b, ";%d;5;%u", fg ? 38 : 48, c & 0xff);
    else if ((c >> 24) == 2) snprintf(b, sizeof b, ";%d;2;%u;%u;%u", fg ? 38 : 48, c >> 16 & 0xff, c >> 8 & 0xff, c & 0xff);
    else return;
    str_put(s, b, strlen(b));
}

static void sgr_cell(Str *s, Cell c) {
    str_put(s, "\033[0", 3);
    static const struct { uint16_t a; const char *code; } map[] = {
        { A_BOLD, ";1" }, { A_DIM, ";2" }, { A_ITALIC, ";3" }, { A_UNDER, ";4" }, { A_UNDER2, ";21" }, { A_CURLY, ";4:3" },
        { A_BLINK, ";5" }, { A_INVERSE, ";7" }, { A_HIDDEN, ";8" }, { A_STRIKE, ";9" }, { A_OVERLINE, ";53" },
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (c.attr & map[i].a) str_put(s, map[i].code, strlen(map[i].code));
    sgr_color(s, c.fg, 1);
    sgr_color(s, c.bg, 0);
    str_put(s, "m", 1);
}

static void serialize_line(Str *s, Line *l, Cell *pen) {
    for (int x = 0; x < l->len; x++) {
        Cell c = l->c[x];
        if (c.attr & A_WIDE_TAIL) continue;
        uint16_t look = c.attr & (uint16_t)~(A_WIDE | A_WIDE_TAIL);
        if (look != pen->attr || c.fg != pen->fg || c.bg != pen->bg) {
            Cell p = c;
            p.attr = look;
            sgr_cell(s, p);
            *pen = p;
        }
        if (c.cp) str_cp(s, c.cp); else str_put(s, " ", 1);
    }
}

/* Content as escape sequences (text, colors, line wrapping, prompt marks,
 * cursor and alternate screen). Feeding it back through vt_write() gives the same result. */
void vt_enter_tmux(Term *t) { str_end(t); str_begin(t); t->state = S_TMUX; }
int  vt_in_tmux(const Term *t) { return t->state == S_TMUX || t->state == S_TMUX_ESC; }

char *vt_serialize(Term *t, int max_lines, size_t *len) {
    Str s = {0};
    Buffer *b = &t->main;
    int from = b->count > max_lines ? b->count - max_lines : 0;
    Cell pen = {0};
    for (int i = from; i < b->count; i++) {
        Line *l = bl(b, i);
        if (l->flags & LINE_PROMPT) str_put(&s, "\033]133;A\007", 8);
        serialize_line(&s, l, &pen);
        if (i + 1 < b->count && !((l->flags & LINE_WRAPPED) && l->len >= t->cols)) str_put(&s, "\033[0m\r\n", 6), pen = (Cell){0};
    }
    str_put(&s, "\033[0m", 4);
    if (t->buf == &t->alt) {
        str_put(&s, "\033[?1049h", 8);
        for (int r = 0; r < t->rows; r++) {
            char cup[24];
            snprintf(cup, sizeof cup, "\033[%dH", r + 1);
            str_put(&s, cup, strlen(cup));
            pen = (Cell){0};
            serialize_line(&s, bl(&t->alt, t->alt.count - t->rows + r), &pen);
        }
        str_put(&s, "\033[0m", 4);
    }
    char cur[32];
    snprintf(cur, sizeof cur, "\033[%d;%dH", t->cy + 1, t->cx + 1);
    str_put(&s, cur, strlen(cur));
    if (t->modes & M_HIDECURSOR) str_put(&s, "\033[?25l", 6);
    *len = s.n;
    return s.s;
}

/* "Highlight" trigger (as in popular terminal emulators): paints the last occurrence of `text` on the
 * screen with the given colors (-1 = unchanged). Returns 1 if it found it. */
int vt_highlight(Term *t, const char *text, int64_t fg, int64_t bg) {
    uint32_t q[512];
    int qn = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && qn < 512;) {
        uint32_t c = *p;
        int extra = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : 3;
        c &= extra == 0 ? 0x7f : extra == 1 ? 0x1f : extra == 2 ? 0x0f : 0x07;
        p++;
        for (int k = 0; k < extra && (*p & 0xc0) == 0x80; k++) c = c << 6 | (*p++ & 0x3f);
        q[qn++] = c;
    }
    if (!qn) return 0;
    Gather g = {0};
    long long last = vt_first_abs(t) + t->buf->count - 1, first = last - t->rows + 1;
    long long hit_line = -1;
    size_t hit = 0;
    Gather best = {0};
    for (long long a = first; a <= last;) {
        a = gather_line(t, a, &g);
        for (size_t i = 0; i + (size_t)qn <= g.n; i++) {
            int k = 0;
            while (k < qn && g.text[i + k] == q[k]) k++;
            if (k < qn) continue;
            hit_line = a;
            hit = i;
        }
        if (hit_line == a) {     /* the logical line with the last match is saved */
            gather_free(&best);
            best = g;
            g = (Gather){0};
        }
    }
    gather_free(&g);
    if (hit_line < 0) { gather_free(&best); return 0; }
    for (size_t i = hit; i < hit + (size_t)qn && i < best.n; i++) {
        Line *l = vt_abs_line(t, best.pl[i]);
        int col = best.pc[i];
        if (!l || col >= l->len) continue;
        if (fg >= 0) l->c[col].fg = COLOR_RGB((uint32_t)fg);
        if (bg >= 0) l->c[col].bg = COLOR_RGB((uint32_t)bg);
        if (l->c[col].attr & A_WIDE && col + 1 < l->len) { if (fg >= 0) l->c[col + 1].fg = l->c[col].fg; if (bg >= 0) l->c[col + 1].bg = l->c[col].bg; }
    }
    gather_free(&best);
    dirty_all(t);
    return 1;
}

/* Text of all logical lines separated by '\n', for searching with regular
 * expressions in JS; the matches come back with vt_set_matches. */
char *vt_logical_text(Term *t) {
    Gather g = {0};
    Str s = {0};
    long long last = vt_first_abs(t) + t->buf->count - 1;
    for (long long a = vt_first_abs(t); a <= last;) {
        a = gather_line(t, a, &g);
        for (size_t i = 0; i < g.n; i++) str_cp(&s, g.text[i]);
        str_put(&s, "\n", 1);
    }
    gather_free(&g);
    if (!s.s) str_put(&s, "", 0);
    return s.s;
}

/* trip: triples (logical line, first character, count), sorted. */
int vt_set_matches(Term *t, const int *trip, int n) {
    vt_search_clear(t);
    if (n <= 0) { dirty_all(t); return 0; }
    t->matches = malloc(MAX_MATCHES * sizeof *t->matches);
    if (!t->matches) return 0;
    Gather g = {0};
    long long last = vt_first_abs(t) + t->buf->count - 1;
    int idx = 0, k = 0;
    for (long long a = vt_first_abs(t); a <= last && k < n && t->nmatches < MAX_MATCHES; idx++) {
        a = gather_line(t, a, &g);
        while (k < n && trip[k * 3] == idx) {
            int st = trip[k * 3 + 1], len = trip[k * 3 + 2];
            if (st >= 0 && len > 0 && (size_t)(st + len) <= g.n && t->nmatches < MAX_MATCHES) add_match(t, &g, (size_t)st, (size_t)(st + len - 1));
            k++;
        }
        while (k < n && trip[k * 3] < idx) k++;
    }
    gather_free(&g);
    search_done(t);
    return t->nmatches;
}

int vt_search_move(Term *t, int dir) {
    if (!t->nmatches) return -1;
    t->match_cur = (t->match_cur + dir + t->nmatches) % t->nmatches;
    show_line(t, t->matches[t->match_cur].l0);
    dirty_all(t);
    return t->match_cur;
}

int vt_match_at(const Term *t, long long line, int col) {
    int lo = 0, hi = t->nmatches - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const Match *m = &t->matches[mid];
        if (m->l0 < line || (m->l0 == line && m->c0 <= col)) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return 0;
    const Match *m = &t->matches[best];
    if (m->l1 > line || (m->l1 == line && m->c1 >= col)) return best == t->match_cur ? 2 : 1;
    return 0;
}

/* ---- marks ---- */

void vt_set_mark(Term *t) {
    L(t, t->cy)->flags |= LINE_MARK;
    vt_mark_dirty(t, t->cy);
}

int vt_jump_mark(Term *t, int dir) {
    Buffer *b = t->buf;
    int top = b->count - t->rows - t->view;
    for (int i = top + dir; i >= 0 && i < b->count; i += dir) {
        if (bl(b, i)->flags & (LINE_PROMPT | LINE_MARK)) {
            int v = b->count - t->rows - i;
            int max = b->count - t->rows;
            t->view = v < 0 ? 0 : v > max ? max : v;
            dirty_all(t);
            return 1;
        }
    }
    if (dir > 0 && t->view) { t->view = 0; dirty_all(t); return 1; }
    return 0;
}

/* ---- links, words ---- */

static int url_char(uint32_t cp) {
    return cp > ' ' && !strchr("<>\"'`|{}", (int)(cp < 128 ? cp : 'a'));
}

char *vt_url_at(Term *t, long long line, int col) {
    Line *l = vt_abs_line(t, line);
    if (!l || col < 0 || col >= l->len) return NULL;
    if (l->c[col].link && l->c[col].link <= t->nlinks) return strdup(t->links[l->c[col].link - 1]);
    if (!url_char(vt_base_cp(l->c[col].cp))) return NULL;
    int a = col, b = col;
    while (a > 0 && url_char(vt_base_cp(l->c[a - 1].cp))) a--;
    while (b + 1 < l->len && url_char(vt_base_cp(l->c[b + 1].cp))) b++;
    Str s = {0};
    for (int x = a; x <= b; x++) if (!(l->c[x].attr & A_WIDE_TAIL)) str_cp(&s, l->c[x].cp);
    if (!s.s) return NULL;
    while (s.n && strchr(".,;:!?)]", s.s[s.n - 1]) && !(s.s[s.n - 1] == ')' && strchr(s.s, '('))) s.s[--s.n] = 0;
    const char *p = s.s;
    int scheme = 0;
    while (isalpha((unsigned char)p[scheme])) scheme++;
    int ok = (scheme >= 2 && !strncmp(p + scheme, "://", 3)) || !strncmp(p, "mailto:", 7) ||
             p[0] == '/' || !strncmp(p, "~/", 2) || !strncmp(p, "./", 2) || !strncmp(p, "www.", 4);
    if (!ok) { free(s.s); return NULL; }
    return s.s;
}

/* The line under a cell, split there: the text before the cell and from it on (the Terminal finds the file name under
 * the pointer in `ls` output; apps/terminal/lsnames.js). Both are malloc'd. */
int vt_line_at(Term *t, long long line, int col, char **before, char **after) {
    *before = *after = NULL;
    Line *l = vt_abs_line(t, line);
    if (!l || col < 0 || col >= l->len) return -1;
    Str a = {0}, b = {0};
    if (col > 0) line_text(&a, l, 0, col - 1, 1);
    line_text(&b, l, col, l->len - 1, 0);
    if (!a.s) str_put(&a, "", 0);
    if (!b.s) str_put(&b, "", 0);
    *before = a.s;
    *after = b.s;
    return 0;
}

static int token_char(uint32_t cp) { return cp && (iswalnum((wint_t)cp) || (cp < 128 && strchr("-_./~:@", (int)cp))); }

char *vt_cursor_word(Term *t) {
    Line *l = L(t, t->cy);
    int b = t->cx - 1, a = b;
    while (a >= 0 && a < l->len && token_char(vt_base_cp(l->c[a].cp))) a--;
    Str s = {0};
    for (int x = a + 1; x <= b && x < l->len; x++) str_cp(&s, l->c[x].cp);
    if (!s.s) str_put(&s, "", 0);
    return s.s;
}

/* Words on the screen and recent history that start with `prefix` (autocomplete). */
char *vt_words(Term *t, const char *prefix, int max) {
    size_t pn = strlen(prefix);
    Str out = {0};
    int found = 0;
    long long first = vt_first_abs(t), last = first + t->buf->count - 1;
    long long stop = last - 2000 > first ? last - 2000 : first;
    for (long long a = last; a >= stop && found < max; a--) {
        Line *l = vt_abs_line(t, a);
        if (!l) continue;
        for (int x = l->len - 1; x >= 0 && found < max;) {
            if (!token_char(vt_base_cp(l->c[x].cp))) { x--; continue; }
            int e = x;
            while (x >= 0 && token_char(vt_base_cp(l->c[x].cp))) x--;
            Str w = {0};
            for (int k = x + 1; k <= e; k++) str_cp(&w, l->c[k].cp);
            if (w.s && w.n > pn && !strncmp(w.s, prefix, pn)) {
                int dup = 0;
                if (out.s) {
                    for (char *p = out.s; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
                        size_t len = strchr(p, '\n') ? (size_t)(strchr(p, '\n') - p) : strlen(p);
                        if (len == w.n && !strncmp(p, w.s, len)) { dup = 1; break; }
                    }
                }
                if (!dup) { if (out.n) str_put(&out, "\n", 1); str_put(&out, w.s, w.n); found++; }
            }
            free(w.s);
        }
    }
    if (!out.s) str_put(&out, "", 0);
    return out.s;
}

/* ---- copy mode: move an own cursor with the keyboard and copy ---- */

int vt_copy_mode(Term *t, int on) {
    t->copy_active = on;
    t->copy_sel = 0;
    t->sel_active = 0;
    if (on) {
        long long top = vt_first_abs(t) + t->buf->count - t->rows - t->view;
        t->copy_line = t->view ? top : vt_cursor_abs(t);
        t->copy_col = t->view ? 0 : t->cx;
    } else t->view = 0;
    dirty_all(t);
    return on;
}

static void copy_move(Term *t, long long line, int col) {
    long long first = vt_first_abs(t), last = first + t->buf->count - 1;
    t->copy_line = line < first ? first : line > last ? last : line;
    t->copy_col = col < 0 ? 0 : col >= t->cols ? t->cols - 1 : col;
    if (t->copy_sel) vt_select_extend(t, t->copy_line, t->copy_col);
    show_line(t, t->copy_line);
    dirty_all(t);
}

/* Returns 0 if still in copy mode, 1 if it exited and 2 if it copied (text in *yank). */
int vt_copy_key(Term *t, const char *k, int ctrl, char **yank) {
    long long l = t->copy_line;
    int c = t->copy_col, half = t->rows / 2;
    if (!strcmp(k, "Escape") || !strcmp(k, "q") || (ctrl && !strcmp(k, "c"))) { vt_copy_mode(t, 0); return 1; }
    if (!strcmp(k, "h") || !strcmp(k, "ArrowLeft")) copy_move(t, l, c - 1);
    else if (!strcmp(k, "l") || !strcmp(k, "ArrowRight")) copy_move(t, l, c + 1);
    else if (!strcmp(k, "k") || !strcmp(k, "ArrowUp")) copy_move(t, l - 1, c);
    else if (!strcmp(k, "j") || !strcmp(k, "ArrowDown")) copy_move(t, l + 1, c);
    else if (!strcmp(k, "0") || !strcmp(k, "Home")) copy_move(t, l, 0);
    else if (!strcmp(k, "$") || !strcmp(k, "End")) { Line *ln = vt_abs_line(t, l); copy_move(t, l, ln && ln->len ? ln->len - 1 : 0); }
    else if (!strcmp(k, "^")) { Line *ln = vt_abs_line(t, l); int x = 0; while (ln && x < ln->len && (!ln->c[x].cp || ln->c[x].cp == ' ')) x++; copy_move(t, l, x); }
    else if (!strcmp(k, "g")) copy_move(t, vt_first_abs(t), 0);
    else if (!strcmp(k, "G")) copy_move(t, vt_cursor_abs(t), t->cx);
    else if ((ctrl && !strcmp(k, "u")) || !strcmp(k, "PageUp")) copy_move(t, l - (ctrl ? half : t->rows), c);
    else if ((ctrl && !strcmp(k, "d")) || !strcmp(k, "PageDown")) copy_move(t, l + (ctrl ? half : t->rows), c);
    else if (!strcmp(k, "w") || !strcmp(k, "b")) {
        /* start of the next/previous word, crossing lines if needed (like vi) */
        int dir = k[0] == 'w' ? 1 : -1;
        long long first = vt_first_abs(t), last = first + t->buf->count - 1;
        long long ln = l;
        int x = c;
        if (dir > 0) {
            while (x < t->cols && is_word(t, cell_cp(t, ln, x))) x++;
            for (;;) {
                while (x < t->cols && !is_word(t, cell_cp(t, ln, x))) x++;
                if (x < t->cols || ln >= last) break;
                ln++;
                x = 0;
            }
            if (x >= t->cols) x = t->cols - 1;
        } else {
            x--;
            for (;;) {
                while (x >= 0 && !is_word(t, cell_cp(t, ln, x))) x--;
                if (x >= 0 || ln <= first) break;
                ln--;
                x = t->cols - 1;
            }
            if (x < 0) x = 0;
            while (x > 0 && is_word(t, cell_cp(t, ln, x - 1))) x--;
        }
        copy_move(t, ln, x);
    } else if (!strcmp(k, "v") || !strcmp(k, "V") || (ctrl && !strcmp(k, "v"))) {
        int mode = ctrl ? 3 : k[0] == 'V' ? 2 : 1;
        if (t->copy_sel == mode) { t->copy_sel = 0; vt_select_clear(t); }
        else { t->copy_sel = mode; vt_select_start(t, l, c, mode == 2 ? 2 : 0, mode == 3); }
    } else if (!strcmp(k, "y") || !strcmp(k, "Enter")) {
        if (!t->sel_active) vt_select_start(t, l, 0, 2, 0);    /* no selection: copies the line */
        *yank = vt_selection_text(t);
        vt_copy_mode(t, 0);
        return 2;
    }
    return 0;
}

/* ---- keys with modifiers (kitty keyboard protocol, flag 1; xterm's modifyOtherKeys) ---- */

int vt_key_encode(const Term *t, const char *key, uint32_t cp, int m, char *out, size_t size) {
    int a = t->buf == &t->alt;
    int kitty = t->nkitty[a] && (t->kitty[a][t->nkitty[a] - 1] & 1);
    int code = 0;
    if (key) {
        if (!strcmp(key, "Enter")) code = 13;
        else if (!strcmp(key, "Tab")) code = 9;
        else if (!strcmp(key, "Backspace")) code = 127;
        else if (!strcmp(key, "Escape")) code = 27;
        else return 0;
    } else {
        /* only Ctrl or Alt change a character; the key's own (lowercase) character is reported */
        if (!((m - 1) & 6) || cp < 0x20 || cp > 0x7e) return 0;
        code = cp >= 'A' && cp <= 'Z' ? (int)cp + 32 : (int)cp;
    }
    int n = 0;
    if (kitty) {
        if (code == 27 && m == 1) n = snprintf(out, size, "\033[27u");     /* Escape alone, told apart from a sequence */
        else if (m > 1) n = snprintf(out, size, "\033[%d;%du", code, m);
    } else if (t->other_keys == 2 && m > 1 && !(code == 9 && m == 2)) {      /* Shift+Tab keeps CSI Z */
        n = snprintf(out, size, "\033[27;%d;%d~", m, code);
    }
    return n > 0 && (size_t)n < size ? n : 0;
}
