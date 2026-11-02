/* h_make.c — building PDFs for printing (text from the Editor and Notes, images from Photos and the
 * scanner) and decoding or converting images that come from outside (the scanner), all inside the
 * helper. Text is laid out with cairo and FreeType, with per-character fallback among the fonts
 * fontconfig knows (no complex script shaping: see docs/PRINTING.md). */
#include "helper.h"

#include <cairo.h>
#include <cairo-ft.h>
#include <cairo-pdf.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "stb_image.h"
#include "stb_image_write.h"

#define IMG_FILE_MAX (128 << 20)

/* ---- output to an fd ---- */

static cairo_status_t write_fd(void *closure, const unsigned char *data, unsigned int len) {
    int fd = *(int *)closure;
    while (len) {
        ssize_t w = write(fd, data, len);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return CAIRO_STATUS_WRITE_ERROR; }
        data += w;
        len -= (unsigned)w;
    }
    return CAIRO_STATUS_SUCCESS;
}

static void paper_size(const JVal *a, double *w, double *h) {
    const char *p = json_str(a, "paper");
    if (p && !strcmp(p, "letter")) { *w = 612; *h = 792; }
    else if (p && !strcmp(p, "legal")) { *w = 612; *h = 1008; }
    else if (p && !strcmp(p, "a5")) { *w = 420; *h = 595; }
    else { *w = 595.28; *h = 841.89; }
}

static void finish_blob(uint32_t id, int fd) {
    off_t size = lseek(fd, 0, SEEK_END);
    if (size <= 0 || doc_seal(fd)) { close(fd); reply_err(id, "write", ""); return; }
    reply_blob(id, fd, (size_t)size);
}

/* ---- fonts with fallback ---- */

#define MAXF 24
static struct { FT_Face ft; cairo_font_face_t *cf; } fonts[2][MAXF];
static int nfonts[2];
static FT_Library ftlib;

static void fonts_load(int mono) {
    if (nfonts[mono]) return;
    if (!ftlib && FT_Init_FreeType(&ftlib)) return;
    FcInit();
    FcPattern *pat = FcNameParse((const FcChar8 *)(mono ? "monospace" : "sans-serif"));
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcFontSet *set = FcFontSort(NULL, pat, FcTrue, NULL, &res);
    for (int i = 0; set && i < set->nfont && nfonts[mono] < MAXF; i++) {
        FcChar8 *file = NULL;
        int index = 0;
        if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) != FcResultMatch) continue;
        FcPatternGetInteger(set->fonts[i], FC_INDEX, 0, &index);
        FT_Face f;
        if (FT_New_Face(ftlib, (const char *)file, index, &f)) continue;
        if (!FT_IS_SCALABLE(f) && !FT_HAS_COLOR(f)) { FT_Done_Face(f); continue; }
        FT_Select_Charmap(f, FT_ENCODING_UNICODE);
        fonts[mono][nfonts[mono]].ft = f;
        fonts[mono][nfonts[mono]].cf = cairo_ft_font_face_create_for_ft_face(f, 0);
        nfonts[mono]++;
    }
    if (set) FcFontSetDestroy(set);
    FcPatternDestroy(pat);
}

static int face_for(int mono, uint32_t c) {
    for (int i = 0; i < nfonts[mono]; i++) if (FT_Get_Char_Index(fonts[mono][i].ft, c)) return i;
    return 0;
}

static uint32_t utf8_next(const unsigned char **p) {
    const unsigned char *s = *p;
    uint32_t c = *s++;
    int k = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (c >= 0x80) c &= 0x3f >> k;
    for (int i = 0; i < k && (*s & 0xc0) == 0x80; i++) c = c << 6 | (*s++ & 0x3f);
    *p = s;
    return c;
}

typedef struct { uint32_t cp; int face; double adv; } Gl;

/* advance of a character in the given font, in points */
static double advance(cairo_t *cr, int mono, int face, uint32_t c, double size, unsigned long *glyph) {
    FT_Face f = fonts[mono][face].ft;
    FT_UInt g = FT_Get_Char_Index(f, c);
    *glyph = g;
    cairo_set_font_face(cr, fonts[mono][face].cf);
    cairo_set_font_size(cr, size);
    cairo_glyph_t gl = { g, 0, 0 };
    cairo_text_extents_t te;
    cairo_glyph_extents(cr, &gl, 1, &te);
    return te.x_advance;
}

static void show_run(cairo_t *cr, int mono, int face, cairo_glyph_t *g, int n, double size) {
    if (!n) return;
    cairo_set_font_face(cr, fonts[mono][face].cf);
    cairo_set_font_size(cr, size);
    cairo_show_glyphs(cr, g, n);
}

/* header (the title, gray) and page number */
static void header(cairo_t *cr, const char *title, int page, double pw, double ph, double margin) {
    cairo_glyph_t run[256];
    cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
    if (title && title[0]) {
        const unsigned char *q = (const unsigned char *)title;
        double x = margin;
        int n = 0, cf = -1;
        while (*q && x < pw - margin && n < 256) {
            uint32_t c = utf8_next(&q);
            int f = face_for(0, c);
            unsigned long g;
            double ad = advance(cr, 0, f, c, 8.5, &g);
            if (f != cf && n) { show_run(cr, 0, cf, run, n, 8.5); n = 0; }
            cf = f;
            run[n++] = (cairo_glyph_t){ g, x, margin - 8 };
            x += ad;
        }
        if (n) show_run(cr, 0, cf, run, n, 8.5);
    }
    char num[16];
    snprintf(num, sizeof num, "%d", page);
    cairo_set_font_face(cr, fonts[0][0].cf);
    cairo_set_font_size(cr, 8.5);
    cairo_text_extents_t te;
    cairo_text_extents(cr, num, &te);
    cairo_move_to(cr, pw - margin - te.x_advance, ph - margin / 2);
    cairo_show_text(cr, num);
    cairo_set_source_rgb(cr, 0, 0, 0);
}

static void op_text(uint32_t id, const JVal *a) {
    const char *text = json_str(a, "text");
    const char *title = json_str(a, "title");
    if (!text) { reply_err(id, "bad-request", "text"); return; }
    int mono = json_bool(a, "mono", 0);
    double size = json_num(a, "size", mono ? 9.5 : 11);
    if (!(size >= 5 && size <= 40)) size = mono ? 9.5 : 11;
    double pw, ph;
    paper_size(a, &pw, &ph);
    fonts_load(mono);
    if (!nfonts[mono]) { reply_err(id, "no-fonts", ""); return; }
    int fd = doc_memfd_new("axia-doc-pdf", 0);
    if (fd < 0) { reply_err(id, "memory", ""); return; }
    cairo_surface_t *s = cairo_pdf_surface_create_for_stream(write_fd, &fd, pw, ph);
    if (title && title[0]) cairo_pdf_surface_set_metadata(s, CAIRO_PDF_METADATA_TITLE, title);
    cairo_pdf_surface_set_metadata(s, CAIRO_PDF_METADATA_CREATOR, "Axia DE");
    cairo_t *cr = cairo_create(s);
    const double margin = 56, line = size * 1.38, top = margin + (title && title[0] ? 18 : 0);
    double maxw = pw - 2 * margin;
    int page = 1;
    double y = top;
    cairo_glyph_t run[512];
    const unsigned char *p = (const unsigned char *)text;
    fonts_load(0);
    header(cr, title, page, pw, ph, margin);
    size_t total = 0;
    while (*p) {
        /* a paragraph: up to the end of the line */
        Gl gl[2048];
        int n = 0;
        while (*p && *p != '\n' && n < 2040) {          /* a very long line continues in the next round */
            uint32_t c = utf8_next(&p);
            if (c == '\r') continue;
            if (c == '\t') { for (int k = 0; k < 4 && n < 2048; k++) gl[n++] = (Gl){ ' ', face_for(mono, ' '), 0 }; continue; }
            if (c < 0x20) continue;
            if (n < 2048) gl[n++] = (Gl){ c, face_for(mono, c), 0 };
        }
        if (*p == '\n') p++;
        unsigned long gid[2048];
        for (int i = 0; i < n; i++) gl[i].adv = advance(cr, mono, gl[i].face, gl[i].cp, size, &gid[i]);
        int i = 0;
        do {
            /* how many fit on the line; break at the last space */
            double w = 0;
            int j = i, brk = -1;
            while (j < n && w + gl[j].adv <= maxw) { if (gl[j].cp == ' ') brk = j; w += gl[j].adv; j++; }
            if (j < n && brk > i) j = brk + 1;
            if (j == i && j < n) j = i + 1;
            if (y + line > ph - margin) {
                cairo_show_page(cr);
                page++;
                y = top;
                header(cr, title, page, pw, ph, margin);
            }
            double x = margin;
            int rn = 0, cf = -1;
            for (int k = i; k < j; k++) {
                if (gl[k].face != cf && rn) { show_run(cr, mono, cf, run, rn, size); rn = 0; }
                cf = gl[k].face;
                if (rn < 512) run[rn++] = (cairo_glyph_t){ gid[k], x, y + size };
                x += gl[k].adv;
            }
            show_run(cr, mono, cf, run, rn, size);
            y += line;
            i = j;
            while (i < n && gl[i].cp == ' ' && i > 0 && j < n) i++;
        } while (i < n);
        total += (size_t)n;
        if (total > (size_t)40 << 20) break;
    }
    cairo_destroy(cr);
    cairo_surface_finish(s);
    cairo_status_t st = cairo_surface_status(s);
    cairo_surface_destroy(s);
    if (st != CAIRO_STATUS_SUCCESS) { close(fd); reply_err(id, "write", cairo_status_to_string(st)); return; }
    finish_blob(id, fd);
}

/* ---- images ---- */

static unsigned char *read_all(int fd, size_t *len) {
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > IMG_FILE_MAX) return NULL;
    unsigned char *buf = malloc((size_t)st.st_size);
    if (!buf) return NULL;
    off_t off = 0;
    while (off < st.st_size) {
        ssize_t r = pread(fd, buf + off, (size_t)(st.st_size - off), off);
        if (r <= 0) { free(buf); return NULL; }
        off += r;
    }
    *len = (size_t)st.st_size;
    return buf;
}

/* decodes to a cairo surface (premultiplied ARGB) */
static cairo_surface_t *decode(int fd) {
    size_t len;
    unsigned char *data = read_all(fd, &len);
    if (!data) return NULL;
    int w, h, ch;
    if (!stbi_info_from_memory(data, (int)len, &w, &h, &ch) || w <= 0 || h <= 0 || (double)w * h > 120e6) { free(data); return NULL; }
    unsigned char *rgba = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4);
    free(data);
    if (!rgba) return NULL;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(s)) { stbi_image_free(rgba); cairo_surface_destroy(s); return NULL; }
    uint32_t *px = (uint32_t *)cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s) / 4;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const unsigned char *q = rgba + ((size_t)y * w + x) * 4;
            uint32_t a = q[3];
            px[(size_t)y * stride + x] = a << 24 | ((q[0] * a + 127) / 255) << 16 | ((q[1] * a + 127) / 255) << 8 | ((q[2] * a + 127) / 255);
        }
    stbi_image_free(rgba);
    cairo_surface_mark_dirty(s);
    return s;
}

static void op_image_pdf(const DocMsg *m, const JVal *a) {
    cairo_surface_t *img = decode(m->nfds ? m->fds[0] : -1);
    if (!img) { reply_err(m->h.id, "invalid", ""); return; }
    double pw, ph;
    paper_size(a, &pw, &ph);
    int iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
    if (iw > ih && pw < ph) { double t = pw; pw = ph; ph = t; }      /* landscape */
    int fd = doc_memfd_new("axia-doc-pdf", 0);
    cairo_surface_t *s = cairo_pdf_surface_create_for_stream(write_fd, &fd, pw, ph);
    cairo_t *cr = cairo_create(s);
    double margin = json_bool(a, "fill", 0) ? 0 : 18;
    double sc = fmin((pw - 2 * margin) / iw, (ph - 2 * margin) / ih);
    cairo_translate(cr, (pw - iw * sc) / 2, (ph - ih * sc) / 2);
    cairo_scale(cr, sc, sc);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_finish(s);
    cairo_surface_destroy(s);
    cairo_surface_destroy(img);
    finish_blob(m->h.id, fd);
}

static void op_decode(const DocMsg *m, const JVal *a) {
    cairo_surface_t *img = decode(m->nfds ? m->fds[0] : -1);
    if (!img) { reply_err(m->h.id, "invalid", ""); return; }
    int iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
    double mw = json_num(a, "maxw", 1600), mh = json_num(a, "maxh", 1600);
    if (!(mw >= 8 && mw <= DOC_IMG_MAX)) mw = 1600;
    if (!(mh >= 8 && mh <= DOC_IMG_MAX)) mh = 1600;
    double sc = fmin(1.0, fmin(mw / iw, mh / ih));
    int W = (int)fmax(1, lround(iw * sc)), H = (int)fmax(1, lround(ih * sc));
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_t *cr = cairo_create(out);
    cairo_scale(cr, (double)W / iw, (double)H / ih);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(out);
    char extra[64];
    snprintf(extra, sizeof extra, ",\"iw\":%d,\"ih\":%d", iw, ih);
    reply_image(m->h.id, (const uint32_t *)cairo_image_surface_get_data(out), W, H, extra);
    cairo_surface_destroy(out);
    cairo_surface_destroy(img);
}

static void stb_out(void *ctx, void *data, int size) {
    int fd = *(int *)ctx;
    const char *p = data;
    while (size > 0) {
        ssize_t w = write(fd, p, (size_t)size);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; }
        p += w;
        size -= (int)w;
    }
}

static void op_convert(const DocMsg *m, const JVal *a) {
    if (m->nfds < 2) { reply_err(m->h.id, "bad-request", ""); return; }
    const char *fmt = json_str(a, "format");
    int out = m->fds[1];
    if (fmt && !strcmp(fmt, "pdf")) {
        cairo_surface_t *img = decode(m->fds[0]);
        if (!img) { reply_err(m->h.id, "invalid", ""); return; }
        double pw, ph;
        paper_size(a, &pw, &ph);
        int iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
        /* page the size of the scan if "fit" is requested, otherwise the paper's */
        if (json_bool(a, "native", 0)) { double dpi = json_num(a, "dpi", 150); if (!(dpi >= 50 && dpi <= 2400)) dpi = 150; pw = iw * 72 / dpi; ph = ih * 72 / dpi; }
        cairo_surface_t *s = cairo_pdf_surface_create_for_stream(write_fd, &out, pw, ph);
        cairo_t *cr = cairo_create(s);
        double sc = fmin(pw / iw, ph / ih);
        cairo_scale(cr, sc, sc);
        cairo_set_source_surface(cr, img, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_finish(s);
        cairo_status_t st = cairo_surface_status(s);
        cairo_surface_destroy(s);
        cairo_surface_destroy(img);
        if (st) { reply_err(m->h.id, "write", ""); return; }
        reply_done_empty(m->h.id);
        return;
    }
    size_t len;
    unsigned char *data = read_all(m->fds[0], &len);
    if (!data) { reply_err(m->h.id, "invalid", ""); return; }
    int w, h, ch;
    unsigned char *rgb = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 3);
    free(data);
    if (!rgb) { reply_err(m->h.id, "invalid", ""); return; }
    int ok;
    if (fmt && !strcmp(fmt, "png")) ok = stbi_write_png_to_func(stb_out, &out, w, h, 3, rgb, w * 3);
    else {
        int q = (int)json_num(a, "quality", 90);
        ok = stbi_write_jpg_to_func(stb_out, &out, w, h, 3, rgb, q < 10 || q > 100 ? 90 : q);
    }
    stbi_image_free(rgb);
    if (!ok) { reply_err(m->h.id, "write", ""); return; }
    reply_done_empty(m->h.id);
}

int make_handle(const DocMsg *m, const JVal *a) {
    switch (m->h.op) {
    case DQ_MAKE_TEXT: op_text(m->h.id, a); return 1;
    case DQ_MAKE_IMAGE: op_image_pdf(m, a); return 1;
    case DQ_IMG_DECODE: op_decode(m, a); return 1;
    case DQ_IMG_CONVERT: op_convert(m, a); return 1;
    default: return 0;
    }
}
