/* text.c — TrueType/OpenType fonts (stb_truetype + otf.c), glyph cache, measuring and
 * line wrapping. Fonts are mapped with mmap (not copied to RAM).
 *
 * Text that Inter covers (Latin, Greek, Cyrillic) goes through the fast path here; the rest
 * (Arabic, Hebrew, CJK, emoji, combining marks…) through textlayout.c. */
#include "ui.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "stb_truetype.h"
#include "stb_image.h"

typedef struct {
    stbtt_fontinfo info;
    unsigned char *data;
    size_t size;
    int ascent, descent, gap, upem;
    int color;                  /* CBDT bitmaps only (stb_truetype doesn't open it) */
    Otf otf;
    char path[512];
    int index;
} Font;

static const char *const font_files[] = {
    "Inter-Regular.ttf", "Inter-Medium.ttf", "Inter-SemiBold.ttf", "Inter-Bold.ttf",
};
#define NFONTS 4            /* Inter: the interface fonts */
#define MAX_FONTS 64
#define GID_BIT 0x80000000u /* in the cache: cp = GID_BIT | (max width << 16) | glyph */
static Font fonts[MAX_FONTS];
static int nfonts;
static char font_dir[1024];
static int mono_font[2] = { -2, -2 };   /* JetBrains Mono regular and bold: -2 not loaded, -1 absent */
static int brand_font[2] = { -2, -2 };  /* Plus Jakarta Sans Medium and ExtraBold (the wordmark), same */

static Glyph *cache;
static size_t cache_cap, cache_used;

static unsigned char *map_file(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    void *m = MAP_FAILED;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 12 && st.st_size < (64 << 20))
        m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return NULL;
    *size = (size_t)st.st_size;
    return m;
}

static unsigned be16(const unsigned char *d, size_t n, size_t off) { return off + 2 <= n ? (unsigned)(d[off] << 8 | d[off + 1]) : 0; }

/* Whether text drawn since the last call may land differently when moved by whole pixels: only
 * draw_fast with a font whose em is a power of two of at most 2048 units keeps every pen position an
 * exact binary fraction at a whole font size; textlayout.c (shaping, other scripts' fonts) is not
 * checked (paint.c's drag layer, layer_part_exact). */
static int drew_inexact;
int text_drew_inexact(void) { int r = drew_inexact; drew_inexact = 0; return r; }

/* a font already in memory (mapped: munmap it if it isn't a font) */
static int load_bytes(Font *f, unsigned char *data, size_t size, int index, int mapped) {
    memset(f, 0, sizeof *f);
    f->data = data;
    f->size = size;
    if (!f->data) return -1;
    int off = stbtt_GetFontOffsetForIndex(f->data, index);
    if (off < 0) off = 0;
    otf_init(&f->otf, f->data, f->size, off);
    if (stbtt_InitFont(&f->info, f->data, off)) {
        stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->gap);
        f->upem = be16(f->data, f->size, (size_t)f->info.head + 18);
    } else if (f->otf.cbdt) {
        /* color-bitmap-only font: cmap and metrics with otf.c */
        f->color = 1;
        f->upem = 2048;
        f->ascent = 1900;
        f->descent = -500;
    } else {
        otf_free(&f->otf);
        if (mapped) munmap(f->data, f->size);
        f->data = NULL;
        return -1;
    }
    f->index = index;
    return 0;
}

static int load(Font *f, const char *path, int index) {
    size_t size = 0;
    unsigned char *data = map_file(path, &size);
    if (load_bytes(f, data, size, index, 1)) return -1;
    snprintf(f->path, sizeof f->path, "%s", path);
    return 0;
}

int text_init(const char *dir) {
    snprintf(font_dir, sizeof font_dir, "%s", dir);
    for (int i = 0; i < NFONTS; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, font_files[i]);
        if (load(&fonts[i], path, 0)) {
            fprintf(stderr, "text: could not load %s\n", path);
            return -1;
        }
    }
    nfonts = NFONTS;
    cache_cap = 4096;
    cache = calloc(cache_cap, sizeof *cache);
    return cache ? 0 : -1;
}

/* LordBench's helper (bench/lbui.c), which opens no files: one interface font built into the binary
 * fills the four Inter slots; there is no monospace or brand font (they would be opened by path). */
int text_init_mem(const unsigned char *data, size_t size) {
    for (int i = 0; i < NFONTS; i++)
        if (load_bytes(&fonts[i], (unsigned char *)data, size, 0, 0)) return -1;
    nfonts = NFONTS;
    font_dir[0] = 0;
    mono_font[0] = mono_font[1] = brand_font[0] = brand_font[1] = -1;
    cache_cap = 4096;
    cache = calloc(cache_cap, sizeof *cache);
    return cache ? 0 : -1;
}

/* Additional fonts (the terminal's, the emoji one...). If it was already loaded it returns
 * the same index. -1 if it couldn't. */
int text_font_load(const char *path, int index) {
    for (int i = 0; i < nfonts; i++) if (fonts[i].data && fonts[i].index == index && !strcmp(fonts[i].path, path)) return i;
    if (nfonts >= MAX_FONTS) return -1;
    if (load(&fonts[nfonts], path, index)) {
        fprintf(stderr, "text: could not load %s\n", path);
        return -1;
    }
    return nfonts++;
}

int text_font_add(const char *path) { return text_font_load(path, 0); }

uint32_t text_font_glyph(int font, uint32_t cp) {
    if (font < 0 || font >= nfonts || !fonts[font].data) return 0;
    Font *f = &fonts[font];
    return f->color ? otf_cmap(&f->otf, cp) : (uint32_t)stbtt_FindGlyphIndex(&f->info, (int)cp);
}

int text_font_has(int font, uint32_t cp) { return text_font_glyph(font, cp) != 0; }
int text_font_is_color(int font) { return font >= 0 && font < nfonts && fonts[font].otf.cbdt != 0; }
int text_font_can_shape(int font, int group) { return font >= 0 && font < nfonts && otf_has_gsub(&fonts[font].otf, group); }
void text_font_shape(int font, int group, GlyphRun *run) {
    if (font >= 0 && font < nfonts) otf_shape(&fonts[font].otf, group, run);
}

Otf *text_font_otf(int font) { return font >= 0 && font < nfonts && fonts[font].data ? &fonts[font].otf : NULL; }

float text_font_scale(int font, float size) {
    Font *f = &fonts[font];
    return f->color ? size / f->upem : stbtt_ScaleForMappingEmToPixels(&f->info, size);
}

int text_font_kern_glyphs(int font, uint32_t a, uint32_t b) {
    Font *f = &fonts[font];
    return f->color ? 0 : stbtt_GetGlyphKernAdvance(&f->info, (int)a, (int)b);
}

int text_ui_font_bold(int font) { return font >= 2 && font < NFONTS; }

void text_font_vmetrics(int font, float size, float *asc, float *desc) {
    Font *f = &fonts[font];
    float scale = f->color ? size / f->upem : stbtt_ScaleForMappingEmToPixels(&f->info, size);
    *asc = f->ascent * scale;
    *desc = f->descent * scale;
}

static int font_for(float weight) {
    if (weight >= TEXT_BRAND) {                 /* fontFamily: 'brand', loaded the first time it's used */
        int bold = weight - TEXT_BRAND >= 600;
        if (brand_font[bold] == -2) {
            char path[1200];
            snprintf(path, sizeof path, "%s/%s", font_dir, bold ? "PlusJakartaSans-ExtraBold.ttf" : "PlusJakartaSans-Medium.ttf");
            brand_font[bold] = text_font_load(path, 0);
        }
        if (brand_font[bold] >= 0) return brand_font[bold];
        weight -= TEXT_BRAND;                   /* missing: Inter */
    }
    if (weight >= TEXT_MONO) {                  /* fontFamily: 'monospace' */
        int bold = weight - TEXT_MONO >= 600;
        if (mono_font[bold] == -2) {
            char path[1200];
            snprintf(path, sizeof path, "%s/%s", font_dir, bold ? "JetBrainsMono-Bold.ttf" : "JetBrainsMono-Regular.ttf");
            mono_font[bold] = text_font_load(path, 0);
        }
        if (mono_font[bold] >= 0) return mono_font[bold];
        weight -= TEXT_MONO;
    }
    return weight < 450 ? 0 : weight < 550 ? 1 : weight < 650 ? 2 : 3;
}

static size_t hash(uint32_t cp, int font, int size_q) {
    uint32_t h = cp * 2654435761u ^ (uint32_t)font * 40503u ^ (uint32_t)size_q * 97u;
    return h;
}

static Glyph *glyph_slot(uint32_t cp, int font, int size_q) {
    size_t i = hash(cp, font, size_q) & (cache_cap - 1);
    while (cache[i].used && !(cache[i].cp == cp && cache[i].font == font && cache[i].size_q == size_q))
        i = (i + 1) & (cache_cap - 1);
    return &cache[i];
}

static void cache_grow(void) {
    Glyph *old = cache;
    size_t old_cap = cache_cap;
    cache_cap *= 2;
    cache = calloc(cache_cap, sizeof *cache);
    cache_used = 0;
    for (size_t i = 0; i < old_cap; i++)
        if (old[i].used) { *glyph_slot(old[i].cp, old[i].font, old[i].size_q) = old[i]; cache_used++; }
    free(old);
}

static Glyph *slot_for(uint32_t key, int font, int size_q) {
    Glyph *g = glyph_slot(key, font, size_q);
    if (!g->used && (cache_used + 1) * 10 > cache_cap * 7) { cache_grow(); g = glyph_slot(key, font, size_q); }
    return g;
}

const Glyph *text_glyph(uint32_t cp, int font, float size) {
    int size_q = (int)(size * 2 + 0.5f);
    Glyph *g = slot_for(cp, font, size_q);
    if (g->used) return g;
    Font *f = &fonts[font];
    g->cp = cp; g->font = (uint16_t)font; g->size_q = (uint16_t)size_q; g->used = 1;
    cache_used++;
    if (f->color) return g;
    float scale = stbtt_ScaleForMappingEmToPixels(&f->info, size_q / 2.0f);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&f->info, (int)cp, &adv, &lsb);
    g->adv = adv * scale;
    g->bmp = stbtt_GetCodepointBitmap(&f->info, scale, scale, (int)cp, &g->w, &g->h, &g->xoff, &g->yoff);
    return g;
}

/* Glyph by index (result of text_font_shape). */
const Glyph *text_glyph_id(int font, uint32_t gid, float size) {
    int size_q = (int)(size * 2 + 0.5f);
    uint32_t key = GID_BIT | (gid & 0xffff);
    Glyph *g = slot_for(key, font, size_q);
    if (g->used) return g;
    Font *f = &fonts[font];
    g->cp = key; g->font = (uint16_t)font; g->size_q = (uint16_t)size_q; g->used = 1;
    cache_used++;
    if (f->color) return g;
    float scale = stbtt_ScaleForMappingEmToPixels(&f->info, size_q / 2.0f);
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&f->info, (int)gid, &adv, &lsb);
    g->adv = adv * scale;
    g->bmp = stbtt_GetGlyphBitmap(&f->info, scale, scale, (int)gid, &g->w, &g->h, &g->xoff, &g->yoff);
    return g;
}

/* Shrinks (or enlarges) a premultiplied ARGB bitmap by area averaging. */
static uint32_t *resample(const uint32_t *src, int sw, int sh, int dw, int dh) {
    uint32_t *out = malloc((size_t)dw * dh * 4);
    if (!out) return NULL;
    for (int y = 0; y < dh; y++) {
        int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t a = 0, r = 0, gg = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t p = src[(size_t)yy * sw + xx];
                    a += p >> 24; r += p >> 16 & 0xff; gg += p >> 8 & 0xff; b += p & 0xff; n++;
                }
            out[(size_t)y * dw + x] = (a / n) << 24 | (r / n) << 16 | (gg / n) << 8 | b / n;
        }
    }
    return out;
}

/* Color glyph (CBDT) scaled to fit in max_w × max_h pixels. argb = NULL if the
 * font has no bitmap for that glyph. */
const Glyph *text_glyph_color(int font, uint32_t gid, int max_w, int max_h) {
    if (max_w < 1) max_w = 1;
    if (max_h < 1) max_h = 1;
    if (max_w > 0x3fff) max_w = 0x3fff;
    uint32_t key = GID_BIT | (uint32_t)max_w << 16 | (gid & 0xffff);
    Glyph *g = slot_for(key, font, max_h * 2);
    if (g->used) return g;
    g->cp = key; g->font = (uint16_t)font; g->size_q = (uint16_t)(max_h * 2); g->used = 1;
    cache_used++;
    size_t len;
    int ppem;
    const uint8_t *png = otf_color_png(&fonts[font].otf, (uint16_t)gid, max_h, &len, &ppem);
    int w, h, n;
    unsigned char *rgba = png ? stbi_load_from_memory(png, (int)len, &w, &h, &n, 4) : NULL;
    if (!rgba) return g;
    uint32_t *argb = malloc((size_t)w * h * 4);
    if (argb) {
        for (int i = 0; i < w * h; i++) {
            uint32_t r = rgba[i * 4], gg = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
            argb[i] = a << 24 | (r * a / 255) << 16 | (gg * a / 255) << 8 | (b * a / 255);
        }
        float s = fminf((float)max_w / w, (float)max_h / h);
        int dw = (int)(w * s + 0.5f), dh = (int)(h * s + 0.5f);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        g->argb = resample(argb, w, h, dw, dh);
        free(argb);
        if (g->argb) { g->w = dw; g->h = dh; g->adv = (float)dw; }
    }
    stbi_image_free(rgba);
    return g;
}

static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = s[0];
    int n = 0;
    if (c < 0x80) n = 0;
    else if ((c & 0xe0) == 0xc0) { c &= 0x1f; n = 1; }
    else if ((c & 0xf0) == 0xe0) { c &= 0x0f; n = 2; }
    else if ((c & 0xf8) == 0xf0) { c &= 0x07; n = 3; }
    else { *p += 1; return 0xfffd; }
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xc0) != 0x80) { *p += i; return 0xfffd; }
        c = (c << 6) | (s[i] & 0x3f);
    }
    *p += n + 1;
    return c;
}

static float kern(int font, float size, uint32_t a, uint32_t b) {
    if (!a) return 0;
    Font *f = &fonts[font];
    return stbtt_GetCodepointKernAdvance(&f->info, (int)a, (int)b) * stbtt_ScaleForMappingEmToPixels(&f->info, size);
}

float text_line_height(float size, float lh) {
    if (lh <= 0) return roundf(size * 1.25f);
    return lh <= 4 ? roundf(size * lh) : lh;   /* <= 4: multiplier, like unitless CSS */
}

typedef struct { const char *start, *end; float width; } Line;

/* Splits into lines on '\n' and on maximum width, breaking at spaces. */
static int wrap(const char *s, int font, float size, float max_w, int nowrap, Line *out, int max) {
    int n = 0;
    const char *ls = s;
    for (;;) {
        const char *p = ls, *brk = NULL, *end, *next;
        float w = 0, wbrk = 0, lw;
        uint32_t prev = 0;
        int done = 0;
        for (;;) {
            const char *q = p;
            uint32_t cp = utf8_next(&q);
            if (cp == 0) { end = p; lw = w; next = p; done = 1; break; }
            if (cp == '\n') { end = p; lw = w; next = q; break; }
            float adv = text_glyph(cp, font, size)->adv + kern(font, size, prev, cp);
            if (!nowrap && max_w > 0 && w + adv > max_w + 0.5f && p != ls) {
                if (brk) { end = brk; lw = wbrk; next = brk + 1; }
                else     { end = p; lw = w; next = p; }
                break;
            }
            if (cp == ' ') { brk = p; wbrk = w; }
            w += adv;
            prev = cp;
            p = q;
        }
        if (n < max) out[n] = (Line){ ls, end, lw };
        n++;
        if (done) break;
        ls = next;
    }
    return n < max ? n : max;
}

#define MAX_LINES 256
static Line lines[MAX_LINES];

/* Measurements are remembered: the layout is redone entirely on any change (a map that moves,
 * a list that scrolls) and re-measuring every text with kerning was the most expensive part. Key:
 * 64-bit hash of the text and all parameters; direct-access table. */
#define TM_SETS 4096
#define TM_WAYS 4
static struct { uint64_t key; float w, h; } tm_cache[TM_SETS][TM_WAYS];
static unsigned char tm_next[TM_SETS];

static uint64_t tm_key(const char *s, float weight, float size, float line_h, float max_w, int nowrap) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) { h ^= *p; h *= 1099511628211ull; }
    float f[4] = { weight, size, line_h, max_w };
    const unsigned char *b = (const unsigned char *)f;
    for (size_t i = 0; i < sizeof f; i++) { h ^= b[i]; h *= 1099511628211ull; }
    h ^= (uint64_t)(nowrap + 1);
    h *= 1099511628211ull;
    return h | 1;                               /* 0 = empty slot */
}

/* max_lines > 0: only the first max_lines lines count; if there is more text, the last one is cut
 * with "…" and may take the whole width. */
static void text_measure_raw(const char *s, float weight, float size, float line_h, float max_w, int nowrap, int max_lines,
                             float *w, float *h) {
    int font = font_for(weight);
    if (text_needs_layout(s, font)) { text_layout_measure_ex(s, font, size, line_h, max_w, nowrap, max_lines, w, h); return; }
    int n = wrap(s, font, size, max_w, nowrap, lines, MAX_LINES);
    int vis = max_lines > 0 && n > max_lines ? max_lines : n;
    float mw = 0;
    for (int i = 0; i < vis; i++) if (lines[i].width > mw) mw = lines[i].width;
    if (vis < n && max_w > 0 && mw < max_w) mw = max_w;
    *w = ceilf(mw);
    *h = vis * text_line_height(size, line_h);
}

void text_measure(const char *s, float weight, float size, float line_h, float max_w, int nowrap, float *w, float *h) {
    text_measure_ex(s, weight, size, line_h, max_w, nowrap, 0, w, h);
}

void text_measure_ex(const char *s, float weight, float size, float line_h, float max_w, int nowrap, int max_lines,
                     float *w, float *h) {
    if (max_lines < 0) max_lines = 0;
    /* the line limit goes in the same key as nowrap (0 lines = the key of plain text_measure) */
    uint64_t k = tm_key(s, weight, size, line_h, max_w, nowrap | max_lines << 1);
    unsigned set = (unsigned)(k >> 20) & (TM_SETS - 1);
    for (int i = 0; i < TM_WAYS; i++)
        if (tm_cache[set][i].key == k) { *w = tm_cache[set][i].w; *h = tm_cache[set][i].h; return; }
    text_measure_raw(s, weight, size, line_h, max_w, nowrap, max_lines, w, h);
    int i = tm_next[set]++ % TM_WAYS;
    tm_cache[set][i].key = k;
    tm_cache[set][i].w = *w;
    tm_cache[set][i].h = *h;
}

float text_paint_scale = 1;

/* Fast path (Inter only): at most max_lines lines (0 = all). */
static void draw_fast(Canvas *c, const char *s, int font, float size, float line_h,
                      float x, float y, float box_w, int align, int nowrap, int max_lines, uint32_t argb) {
    /* scaled display: break lines with the logical metrics (what layout measured), draw at `size` */
    float k = text_paint_scale > 0 ? text_paint_scale : 1;
    int n = wrap(s, font, size / k, box_w / k, nowrap, lines, MAX_LINES);
    if (max_lines > 0 && n > max_lines) n = max_lines;
    if (k != 1) for (int i = 0; i < n; i++) lines[i].width *= k;
    float lh = text_line_height(size, line_h);
    Font *f = &fonts[font];
    if (!f->upem || (f->upem & (f->upem - 1)) || f->upem > 2048) drew_inexact = 1;
    float scale = stbtt_ScaleForMappingEmToPixels(&f->info, size);
    float asc = f->ascent * scale, desc = f->descent * scale;
    for (int i = 0; i < n; i++) {
        float pen = x;
        if (align == TEXT_CENTER) pen += (box_w - lines[i].width) / 2;
        else if (align == TEXT_RIGHT) pen += box_w - lines[i].width;
        float baseline = roundf(y + i * lh + (lh - (asc - desc)) / 2 + asc);
        if (baseline - asc > c->clip.y1 || baseline - desc < c->clip.y0) continue;
        uint32_t prev = 0;
        for (const char *p = lines[i].start; p < lines[i].end;) {
            uint32_t cp = utf8_next(&p);
            pen += kern(font, size, prev, cp);
            const Glyph *g = text_glyph(cp, font, size);
            if (g->bmp)
                canvas_alpha_mask(c, (int)roundf(pen) + g->xoff, (int)baseline + g->yoff, g->bmp, g->w, g->h, g->w, argb);
            pen += g->adv;
            prev = cp;
        }
    }
}

void text_draw(Canvas *c, const char *s, float weight, float size, float line_h,
               float x, float y, float box_w, int align, int nowrap, uint32_t argb) {
    int font = font_for(weight);
    if (text_needs_layout(s, font)) { drew_inexact = 1; text_layout_draw(c, s, font, size, line_h, x, y, box_w, align, nowrap, argb); return; }
    draw_fast(c, s, font, size, line_h, x, y, box_w, align, nowrap, 0, argb);
}

/* ---- truncation with an ellipsis ---- */

#define ELLIPSIS "\xe2\x80\xa6"           /* U+2026 */

static int copy_str(char *out, int out_size, const char *s, int len) {
    if (out_size <= 0) return 0;
    if (len > out_size - 1) len = out_size - 1;
    memcpy(out, s, (size_t)len);
    out[len] = 0;
    return len;
}

/* prefix (len bytes, trailing spaces dropped) + "…" */
int text_ellipsis_join(char *out, int out_size, const char *s, int len) {
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (len > out_size - 4) {               /* keep whole UTF-8 sequences */
        len = out_size - 4;
        while (len > 0 && ((unsigned char)s[len] & 0xc0) == 0x80) len--;
    }
    if (len < 0) len = 0;
    int k = copy_str(out, out_size, s, len);
    if (k + 3 < out_size) { memcpy(out + k, ELLIPSIS, 4); k += 3; }
    return k;
}

static float ellipsize_raw(const char *s, float weight, float size, float box_w, int force, char *out, int out_size) {
    int font = font_for(weight);
    if (text_needs_layout(s, font)) return text_layout_ellipsize(s, font, size, box_w, force, out, out_size);
    /* Inter covers the whole text, and it has no marks or joiners (those go through textlayout.c):
     * every code point is a cluster */
    float ell = text_glyph(0x2026, font, size)->adv;
    float w = 0, best_w = 0;
    uint32_t prev = 0;
    int best = 0;
    const char *p = s;
    for (;;) {
        const char *q = p;
        uint32_t cp = utf8_next(&q);
        if (!cp || cp == '\n') break;
        float adv = text_glyph(cp, font, size)->adv + kern(font, size, prev, cp);
        if (w + adv + ell <= box_w + 0.5f) { best = (int)(q - s); best_w = w + adv; }
        w += adv;
        prev = cp;
        p = q;
    }
    if (!force && w <= box_w + 0.5f) { copy_str(out, out_size, s, (int)(p - s)); return -1; }
    text_ellipsis_join(out, out_size, s, best);
    return best_w + ell;
}

/* Byte ranges of the lines (logical metrics). */
/* Cut positions are remembered like measurements (a list repaints the same labels every frame):
 * key = text and parameters, value = the prefix length in bytes (-1: it fits) and the width. */
#define EC_SETS 2048
static struct { uint64_t key; int cut; float w; } ec_cache[EC_SETS];

float text_ellipsize(const char *s, float weight, float size, float box_w, int force, char *out, int out_size) {
    uint64_t k = tm_key(s, weight, size, 0, box_w, 2 + force);
    unsigned slot = (unsigned)(k >> 17) & (EC_SETS - 1);
    if (ec_cache[slot].key == k) {
        int cut = ec_cache[slot].cut;
        if (cut < 0) copy_str(out, out_size, s, (int)strlen(s));
        else text_ellipsis_join(out, out_size, s, cut);
        return ec_cache[slot].w;
    }
    float w = ellipsize_raw(s, weight, size, box_w, force, out, out_size);
    int len = (int)strlen(out);
    if (w >= 0 && out_size > 4 && len < out_size - 1) {          /* only if out wasn't truncated by its size */
        ec_cache[slot].key = k;
        ec_cache[slot].cut = len >= 3 ? len - 3 : 0;
        ec_cache[slot].w = w;
    } else if (w < 0) {
        ec_cache[slot].key = k;
        ec_cache[slot].cut = -1;
        ec_cache[slot].w = w;
    }
    return w;
}

static int line_ranges(const char *s, int font, float size, float box_w, int nowrap, int *bs, int *be, int max) {
    if (text_needs_layout(s, font)) return text_layout_line_bytes(s, font, size, box_w, nowrap, bs, be, max);
    int n = wrap(s, font, size, box_w, nowrap, lines, MAX_LINES);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) { bs[i] = (int)(lines[i].start - s); be[i] = (int)(lines[i].end - s); }
    return n;
}

static void draw_lines(Canvas *c, const char *s, int font, float size, float line_h,
                       float x, float y, float box_w, int align, int nowrap, int max_lines, uint32_t argb) {
    if (text_needs_layout(s, font)) { drew_inexact = 1; text_layout_draw_n(c, s, font, size, line_h, x, y, box_w, align, nowrap, max_lines, argb); }
    else draw_fast(c, s, font, size, line_h, x, y, box_w, align, nowrap, max_lines, argb);
}

void text_draw_ex(Canvas *c, const char *s, float weight, float size, float line_h,
                  float x, float y, float box_w, int align, int nowrap, int max_lines, int ellipsis, uint32_t argb) {
    if (max_lines < 0) max_lines = 0;
    if (!ellipsis && !max_lines) { text_draw(c, s, weight, size, line_h, x, y, box_w, align, nowrap, argb); return; }
    int font = font_for(weight);
    float k = text_paint_scale > 0 ? text_paint_scale : 1;
    float ls = size / k, lw = box_w / k, lh = text_line_height(size, line_h);
    static char buf[8192], cut[8200];
    if (nowrap && !strchr(s, '\n')) {          /* the common case: one line (a file name, a title) */
        const char *t = text_ellipsize(s, weight, ls, lw, 0, cut, sizeof cut) < 0 ? s : cut;
        draw_lines(c, t, font, size, line_h, x, y, box_w, align, 1, 1, argb);
        return;
    }
    static int bs[MAX_LINES], be[MAX_LINES];
    int n = line_ranges(s, font, ls, lw, nowrap, bs, be, MAX_LINES);
    int vis = max_lines && n > max_lines ? max_lines : n;
    int more = vis < n;
    /* nothing is cut: the usual path */
    if (!more && !nowrap) { draw_lines(c, s, font, size, line_h, x, y, box_w, align, nowrap, 0, argb); return; }
    if (!more && nowrap) {
        float w, h;
        text_measure_ex(s, weight, ls, 0, 0, 1, 0, &w, &h);
        if (w <= lw + 0.5f) { draw_lines(c, s, font, size, line_h, x, y, box_w, align, nowrap, 0, argb); return; }
    }
    /* lines before the last visible one, as usual (wrapped text); nowrap lines each on their own */
    int first_cut = nowrap ? 0 : vis - 1;
    if (first_cut > 0) draw_lines(c, s, font, size, line_h, x, y, box_w, align, nowrap, first_cut, argb);
    for (int i = first_cut; i < vis; i++) {
        float ly = y + i * lh;
        if (ly > c->clip.y1 || ly + lh < c->clip.y0) continue;
        /* the last visible line of wrapped text carries the rest of its paragraph */
        int a = bs[i], b = be[i];
        if (!nowrap && i == vis - 1) { b = a; while (s[b] && s[b] != '\n') b++; }
        copy_str(buf, sizeof buf, s + a, b - a);
        int force = i == vis - 1 && more;
        text_ellipsize(buf, weight, ls, lw, force, cut, sizeof cut);
        draw_lines(c, cut, font_for(weight), size, line_h, x, ly, box_w, align, 1, 1, argb);
    }
}
