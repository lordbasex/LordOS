/* h_pdf.c — PDF in the helper (Poppler, glib interface): open, render pages, select
 * and search text, outline, links, forms and annotations, and save a copy.
 *
 * Coordinates: everything that leaves here is in PDF points with the origin at the top left
 * of the page (as drawn), without the rotation the user chooses (the app applies that). */
#include "helper.h"

#include <cairo.h>
#include <poppler.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static PopplerDocument *doc;
static int npages;
static PopplerPage **pages;

/* annotations added in this session, for undo (stack per page) */
typedef struct Added { int page; PopplerAnnot *annot; struct Added *next; } Added;
static Added *added;

static PopplerPage *page_at(int i) {
    if (!doc || i < 0 || i >= npages) return NULL;
    if (!pages[i]) pages[i] = poppler_document_get_page(doc, i);
    return pages[i];
}

static int arg_page(const JVal *a, PopplerPage **p, double *w, double *h) {
    double d = json_num(a, "page", -1);
    if (d != (double)(int)d) return -1;
    *p = page_at((int)d);
    if (!*p) return -1;
    poppler_page_get_size(*p, w, h);
    return (int)d;
}

static void put_rect(JBuf *b, double x, double y, double w, double h) {
    jb_raw(b, "[");
    jb_num(b, x); jb_raw(b, ",");
    jb_num(b, y); jb_raw(b, ",");
    jb_num(b, w); jb_raw(b, ",");
    jb_num(b, h); jb_raw(b, "]");
}

/* PDF rectangle (origin at the bottom) → [x, y, w, h] with the origin at the top */
static void put_pdf_rect(JBuf *b, const PopplerRectangle *r, double ph) {
    double x1 = fmin(r->x1, r->x2), x2 = fmax(r->x1, r->x2), y1 = fmin(r->y1, r->y2), y2 = fmax(r->y1, r->y2);
    put_rect(b, x1, ph - y2, x2 - x1, y2 - y1);
}

static void put_gstr(JBuf *b, const char *key, gchar *s) {
    jb_printf(b, ",\"%s\":", key);
    jb_str(b, s ? s : "");
    g_free(s);
}

static void put_sizes(JBuf *b, int upto) {
    jb_raw(b, ",\"sizes\":[");
    int forms = 0;
    for (int i = 0; i < upto && i < npages; i++) {
        PopplerPage *p = page_at(i);
        double w = 612, h = 792;
        if (p) poppler_page_get_size(p, &w, &h);
        if (i) jb_raw(b, ",");
        jb_raw(b, "[");
        jb_num(b, w); jb_raw(b, ",");
        jb_num(b, h); jb_raw(b, "]");
        if (p && !forms && i < 200) {
            GList *f = poppler_page_get_form_field_mapping(p);
            if (f) forms = 1;
            poppler_page_free_form_field_mapping(f);
        }
        /* don't keep thousands of pages open: they're requested again when drawing */
        if (i > 64 && p) { g_object_unref(p); pages[i] = NULL; }
    }
    jb_printf(b, "],\"forms\":%s", forms ? "true" : "false");
}

static void op_sizes(uint32_t id) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"all\":true");
    put_sizes(&b, npages);
    jb_raw(&b, "}");
    reply_json(DR_DONE, id, &b);
}

static void op_open(uint32_t id, const JVal *a) {
    int fd = slot_fd(a, "slot");
    if (fd < 0) { reply_err(id, "bad-request", "slot"); return; }
    if (doc) { reply_err(id, "busy", ""); return; }
    const char *pw = json_str(a, "password");
    GError *err = NULL;
    int dfd = dup(fd);
    doc = poppler_document_new_from_fd(dfd, pw && pw[0] ? pw : NULL, &err);
    if (!doc) {
        int locked = err && err->domain == POPPLER_ERROR && err->code == POPPLER_ERROR_ENCRYPTED;
        reply_err(id, locked ? "password" : "invalid", err ? err->message : "");
        if (err) g_error_free(err);
        if (dfd >= 0) close(dfd);
        return;
    }
    npages = poppler_document_get_n_pages(doc);
    if (npages < 0) npages = 0;
    if (npages > 100000) npages = 100000;
    pages = calloc((size_t)npages + 1, sizeof *pages);
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"pages\":%d", npages);
    put_gstr(&b, "title", poppler_document_get_title(doc));
    put_gstr(&b, "author", poppler_document_get_author(doc));
    put_gstr(&b, "subject", poppler_document_get_subject(doc));
    put_gstr(&b, "creator", poppler_document_get_creator(doc));
    put_gstr(&b, "producer", poppler_document_get_producer(doc));
    PopplerIndexIter *it = poppler_index_iter_new(doc);
    jb_printf(&b, ",\"outline\":%s", it ? "true" : "false");
    if (it) poppler_index_iter_free(it);
    /* sizes: with "quick" only the first one (the rest with DQ_PDF_SIZES, without holding up the first
     * render); otherwise all of them, to lay out continuous scrolling from the start */
    put_sizes(&b, json_bool(a, "quick", 0) ? 1 : npages);
    jb_raw(&b, "}");
    reply_json(DR_DONE, id, &b);
}

static void op_render(uint32_t id, const JVal *a) {
    PopplerPage *p;
    double pw, ph;
    int pg = arg_page(a, &p, &pw, &ph);
    if (pg < 0) { reply_err(id, "bad-page", ""); return; }
    int rot = (((int)json_num(a, "rot", 0) / 90) % 4 + 4) % 4;
    double dw = rot % 2 ? ph : pw, dh = rot % 2 ? pw : ph;
    if (dw < 1 || dh < 1) { reply_err(id, "bad-page", "size"); return; }
    double want = json_num(a, "width", 800);
    if (!(want >= 8)) want = 8;
    if (want > DOC_IMG_MAX) want = DOC_IMG_MAX;
    double s = want / dw;
    if (dh * s > DOC_IMG_MAX) s = DOC_IMG_MAX / dh;
    if (dw * s * dh * s > 36e6) s = sqrt(36e6 / (dw * dh));
    int W = (int)lround(dw * s), H = (int)lround(dh * s);
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    size_t len = (size_t)W * H * 4;
    int fd = doc_memfd_new("axia-doc-page", len);
    if (fd < 0) { reply_err(id, "memory", ""); return; }
    uint32_t *px = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { close(fd); reply_err(id, "memory", ""); return; }
    cairo_surface_t *surf = cairo_image_surface_create_for_data((unsigned char *)px, CAIRO_FORMAT_ARGB32, W, H, W * 4);
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    if (rot == 1) { cairo_translate(cr, W, 0); cairo_rotate(cr, M_PI / 2); }
    else if (rot == 2) { cairo_translate(cr, W, H); cairo_rotate(cr, M_PI); }
    else if (rot == 3) { cairo_translate(cr, 0, H); cairo_rotate(cr, -M_PI / 2); }
    cairo_scale(cr, (double)W / dw, (double)H / dh);
    poppler_page_render(p, cr);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);
    if (json_bool(a, "invert", 0)) invert_lightness(px, (size_t)W * H);
    munmap(px, len);
    char extra[64];
    snprintf(extra, sizeof extra, ",\"page\":%d", pg);
    reply_image_fd(id, fd, W, H, extra);
}

static void op_select(uint32_t id, const JVal *a) {
    PopplerPage *p;
    double pw, ph;
    if (arg_page(a, &p, &pw, &ph) < 0) { reply_err(id, "bad-page", ""); return; }
    PopplerRectangle r = { json_num(a, "x1", 0), json_num(a, "y1", 0), json_num(a, "x2", pw), json_num(a, "y2", ph) };
    int st = (int)json_num(a, "style", 0);
    PopplerSelectionStyle style = st == 1 ? POPPLER_SELECTION_WORD : st == 2 ? POPPLER_SELECTION_LINE : POPPLER_SELECTION_GLYPH;
    char *text = poppler_page_get_selected_text(p, style, &r);
    cairo_region_t *reg = poppler_page_get_selected_region(p, 1.0, style, &r);
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"text\":");
    jb_str(&b, text ? text : "");
    jb_raw(&b, ",\"rects\":[");
    int n = reg ? cairo_region_num_rectangles(reg) : 0;
    for (int i = 0; i < n && i < 4000; i++) {
        cairo_rectangle_int_t c;
        cairo_region_get_rectangle(reg, i, &c);
        if (i) jb_raw(&b, ",");
        put_rect(&b, c.x, c.y, c.width, c.height);
    }
    jb_raw(&b, "]}");
    g_free(text);
    if (reg) cairo_region_destroy(reg);
    reply_json(DR_DONE, id, &b);
}

static void op_find(uint32_t id, const JVal *a) {
    const char *text = json_str(a, "text");
    if (!text || !text[0] || strlen(text) > 512) { reply_err(id, "bad-request", "text"); return; }
    PopplerFindFlags fl = POPPLER_FIND_DEFAULT;
    if (json_bool(a, "case", 0)) fl |= POPPLER_FIND_CASE_SENSITIVE;
    if (json_bool(a, "whole", 0)) fl |= POPPLER_FIND_WHOLE_WORDS_ONLY;
    int total = 0;
    for (int i = 0; i < npages && total < 20000; i++) {
        PopplerPage *p = page_at(i);
        if (!p) continue;
        double pw, ph;
        poppler_page_get_size(p, &pw, &ph);
        GList *hits = poppler_page_find_text_with_options(p, text, fl);
        if (hits) {
            JBuf b;
            jb_init(&b);
            jb_printf(&b, "{\"page\":%d,\"rects\":[", i);
            int k = 0;
            for (GList *l = hits; l; l = l->next, k++) {
                if (k) jb_raw(&b, ",");
                put_pdf_rect(&b, l->data, ph);
                poppler_rectangle_free(l->data);
            }
            g_list_free(hits);
            total += k;
            jb_raw(&b, "]}");
            reply_json(DR_PART, id, &b);
        }
        if (i > 64 && pages[i]) { g_object_unref(pages[i]); pages[i] = NULL; }
    }
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"total\":%d}", total);
    reply_json(DR_DONE, id, &b);
}

/* page number (0..n-1) of a destination, or -1 */
static int dest_page(PopplerDest *d) {
    if (!d) return -1;
    if (d->type == POPPLER_DEST_NAMED && d->named_dest) {
        PopplerDest *r = poppler_document_find_dest(doc, d->named_dest);
        int pg = r ? r->page_num - 1 : -1;
        if (r) poppler_dest_free(r);
        return pg;
    }
    return d->page_num - 1;
}

static void put_action(JBuf *b, PopplerAction *act) {
    if (!act) return;
    if (act->type == POPPLER_ACTION_GOTO_DEST) jb_printf(b, ",\"page\":%d", dest_page(act->goto_dest.dest));
    else if (act->type == POPPLER_ACTION_URI && act->uri.uri) { jb_raw(b, ",\"uri\":"); jb_str(b, act->uri.uri); }
    else if (act->type == POPPLER_ACTION_NAMED && act->named.named_dest) { jb_raw(b, ",\"named\":"); jb_str(b, act->named.named_dest); }
}

static int outline_count;

static void outline_walk(JBuf *b, PopplerIndexIter *it, int depth) {
    int first = 1;
    do {
        if (++outline_count > 5000) return;
        PopplerAction *act = poppler_index_iter_get_action(it);
        if (!first) jb_raw(b, ",");
        first = 0;
        jb_raw(b, "{\"title\":");
        jb_str(b, act && act->any.title ? act->any.title : "");
        put_action(b, act);
        if (act) poppler_action_free(act);
        PopplerIndexIter *kid = depth < 16 ? poppler_index_iter_get_child(it) : NULL;
        if (kid) {
            jb_printf(b, ",\"open\":%s,\"kids\":[", poppler_index_iter_is_open(it) ? "true" : "false");
            outline_walk(b, kid, depth + 1);
            jb_raw(b, "]");
            poppler_index_iter_free(kid);
        }
        jb_raw(b, "}");
    } while (poppler_index_iter_next(it));
}

static void op_outline(uint32_t id) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"items\":[");
    PopplerIndexIter *it = doc ? poppler_index_iter_new(doc) : NULL;
    outline_count = 0;
    if (it) { outline_walk(&b, it, 0); poppler_index_iter_free(it); }
    jb_raw(&b, "]}");
    reply_json(DR_DONE, id, &b);
}

static void put_field(JBuf *b, PopplerFormField *f) {
    jb_printf(b, "{\"id\":%d,\"ro\":%s", poppler_form_field_get_id(f), poppler_form_field_is_read_only(f) ? "true" : "false");
    put_gstr(b, "name", poppler_form_field_get_partial_name(f));
    switch (poppler_form_field_get_field_type(f)) {
    case POPPLER_FORM_FIELD_TEXT:
        jb_printf(b, ",\"type\":\"text\",\"multiline\":%s,\"password\":%s,\"max\":%d",
                  poppler_form_field_text_get_text_type(f) == POPPLER_FORM_TEXT_MULTILINE ? "true" : "false",
                  poppler_form_field_text_is_password(f) ? "true" : "false", poppler_form_field_text_get_max_len(f));
        put_gstr(b, "value", poppler_form_field_text_get_text(f));
        break;
    case POPPLER_FORM_FIELD_BUTTON: {
        PopplerFormButtonType t = poppler_form_field_button_get_button_type(f);
        jb_printf(b, ",\"type\":\"%s\",\"value\":%s", t == POPPLER_FORM_BUTTON_CHECK ? "check" : t == POPPLER_FORM_BUTTON_RADIO ? "radio" : "push",
                  poppler_form_field_button_get_state(f) ? "true" : "false");
        break;
    }
    case POPPLER_FORM_FIELD_CHOICE: {
        int n = poppler_form_field_choice_get_n_items(f);
        jb_printf(b, ",\"type\":\"choice\",\"combo\":%s,\"items\":[", poppler_form_field_choice_get_choice_type(f) == POPPLER_FORM_CHOICE_COMBO ? "true" : "false");
        int sel = -1;
        for (int i = 0; i < n && i < 500; i++) {
            if (i) jb_raw(b, ",");
            gchar *s = poppler_form_field_choice_get_item(f, i);
            jb_str(b, s ? s : "");
            g_free(s);
            if (sel < 0 && poppler_form_field_choice_is_item_selected(f, i)) sel = i;
        }
        jb_printf(b, "],\"value\":%d", sel);
        break;
    }
    default:
        jb_raw(b, ",\"type\":\"other\"");
    }
    jb_raw(b, "}");
}

static void op_pageinfo(uint32_t id, const JVal *a) {
    PopplerPage *p;
    double pw, ph;
    if (arg_page(a, &p, &pw, &ph) < 0) { reply_err(id, "bad-page", ""); return; }
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"links\":[");
    GList *links = poppler_page_get_link_mapping(p);
    int k = 0;
    for (GList *l = links; l && k < 2000; l = l->next) {
        PopplerLinkMapping *m = l->data;
        if (!m->action || (m->action->type != POPPLER_ACTION_GOTO_DEST && m->action->type != POPPLER_ACTION_URI)) continue;
        if (k++) jb_raw(&b, ",");
        jb_raw(&b, "{\"r\":");
        put_pdf_rect(&b, &m->area, ph);
        put_action(&b, m->action);
        jb_raw(&b, "}");
    }
    poppler_page_free_link_mapping(links);
    jb_raw(&b, "],\"fields\":[");
    GList *fields = poppler_page_get_form_field_mapping(p);
    k = 0;
    for (GList *l = fields; l && k < 2000; l = l->next) {
        PopplerFormFieldMapping *m = l->data;
        if (k++) jb_raw(&b, ",");
        jb_raw(&b, "{\"r\":");
        put_pdf_rect(&b, &m->area, ph);
        jb_raw(&b, ",\"f\":");
        put_field(&b, m->field);
        jb_raw(&b, "}");
    }
    poppler_page_free_form_field_mapping(fields);
    jb_raw(&b, "],\"notes\":[");
    GList *annots = poppler_page_get_annot_mapping(p);
    k = 0;
    for (GList *l = annots; l && k < 2000; l = l->next) {
        PopplerAnnotMapping *m = l->data;
        if (poppler_annot_get_annot_type(m->annot) != POPPLER_ANNOT_TEXT) continue;
        if (k++) jb_raw(&b, ",");
        jb_raw(&b, "{\"r\":");
        put_pdf_rect(&b, &m->area, ph);
        put_gstr(&b, "text", poppler_annot_get_contents(m->annot));
        jb_raw(&b, "}");
    }
    poppler_page_free_annot_mapping(annots);
    jb_raw(&b, "]}");
    reply_json(DR_DONE, id, &b);
}

static void op_setfield(uint32_t id, const JVal *a) {
    double fid = json_num(a, "id", -1);
    PopplerFormField *f = doc && fid >= 0 ? poppler_document_get_form_field(doc, (int)fid) : NULL;
    if (!f) { reply_err(id, "bad-field", ""); return; }
    if (poppler_form_field_is_read_only(f)) { g_object_unref(f); reply_err(id, "read-only", ""); return; }
    const JVal *v = json_get(a, "value");
    switch (poppler_form_field_get_field_type(f)) {
    case POPPLER_FORM_FIELD_TEXT:
        if (v && v->type == J_STR && v->len < 65536) poppler_form_field_text_set_text(f, v->str);
        break;
    case POPPLER_FORM_FIELD_BUTTON:
        if (v && v->type == J_BOOL) poppler_form_field_button_set_state(f, v->num != 0);
        break;
    case POPPLER_FORM_FIELD_CHOICE:
        if (v && v->type == J_NUM && v->num >= 0 && v->num < poppler_form_field_choice_get_n_items(f)) {
            poppler_form_field_choice_unselect_all(f);
            poppler_form_field_choice_select_item(f, (int)v->num);
        }
        break;
    default: break;
    }
    g_object_unref(f);
    reply_done_empty(id);
}

static void parse_color(const JVal *a, PopplerColor *c, double *alpha) {
    const char *s = json_str(a, "color");
    unsigned r = 255, g = 214, bl = 10;
    if (s && s[0] == '#' && strlen(s) == 7) sscanf(s + 1, "%2x%2x%2x", &r, &g, &bl);
    c->red = (guint16)(r * 257); c->green = (guint16)(g * 257); c->blue = (guint16)(bl * 257);
    if (alpha) { *alpha = json_num(a, "opacity", 1); if (!(*alpha > 0 && *alpha <= 1)) *alpha = 1; }
}

/* [x, y, w, h] top-left → PDF PopplerRectangle */
static int get_rect(const JVal *v, double ph, PopplerRectangle *r) {
    if (!v || v->type != J_ARR || v->len != 4) return -1;
    double q[4];
    int i = 0;
    for (const JVal *e = v->kid; e && i < 4; e = e->next, i++) { if (e->type != J_NUM) return -1; q[i] = e->num; }
    if (q[2] < 0 || q[3] < 0) return -1;
    r->x1 = q[0]; r->x2 = q[0] + q[2];
    r->y1 = ph - (q[1] + q[3]); r->y2 = ph - q[1];
    return 0;
}

static void remember(int page, PopplerAnnot *an) {
    Added *x = calloc(1, sizeof *x);
    if (!x) return;
    x->page = page;
    x->annot = g_object_ref(an);
    x->next = added;
    added = x;
}

static void op_annot(uint32_t id, const JVal *a) {
    PopplerPage *p;
    double pw, ph;
    int pg = arg_page(a, &p, &pw, &ph);
    if (pg < 0) { reply_err(id, "bad-page", ""); return; }
    const char *type = json_str(a, "type");
    PopplerColor color;
    double alpha;
    parse_color(a, &color, &alpha);
    PopplerAnnot *an = NULL;
    if (type && !strcmp(type, "highlight")) {
        const JVal *rects = json_get(a, "rects");
        if (!rects || rects->type != J_ARR || !rects->len || rects->len > 2000) { reply_err(id, "bad-request", "rects"); return; }
        GArray *quads = g_array_new(FALSE, FALSE, sizeof(PopplerQuadrilateral));
        PopplerRectangle box = { 1e9, 1e9, -1e9, -1e9 };
        for (const JVal *e = rects->kid; e; e = e->next) {
            PopplerRectangle r;
            if (get_rect(e, ph, &r)) continue;
            PopplerQuadrilateral q = { { r.x1, r.y2 }, { r.x2, r.y2 }, { r.x1, r.y1 }, { r.x2, r.y1 } };
            g_array_append_val(quads, q);
            box.x1 = fmin(box.x1, r.x1); box.y1 = fmin(box.y1, r.y1);
            box.x2 = fmax(box.x2, r.x2); box.y2 = fmax(box.y2, r.y2);
        }
        if (!quads->len) { g_array_free(quads, TRUE); reply_err(id, "bad-request", "rects"); return; }
        an = poppler_annot_text_markup_new_highlight(doc, &box, quads);
        g_array_free(quads, TRUE);
    } else if (type && !strcmp(type, "note")) {
        PopplerRectangle r;
        if (get_rect(json_get(a, "r"), ph, &r)) { reply_err(id, "bad-request", "r"); return; }
        an = poppler_annot_text_new(doc, &r);
        const char *text = json_str(a, "text");
        if (an && text && strlen(text) < 65536) poppler_annot_set_contents(an, text);
        if (an) poppler_annot_text_set_icon(POPPLER_ANNOT_TEXT(an), POPPLER_ANNOT_TEXT_ICON_NOTE);
    } else if (type && !strcmp(type, "ink")) {
        /* poppler-glib 25 doesn't create ink annotations: the stroke goes as a stamp with its own
         * (transparent) image, which any PDF reader shows the same way */
        const JVal *strokes = json_get(a, "strokes");
        if (!strokes || strokes->type != J_ARR || !strokes->len || strokes->len > 500) { reply_err(id, "bad-request", "strokes"); return; }
        double width = json_num(a, "width", 2);
        if (!(width > 0.2 && width < 40)) width = 2;
        double x1 = 1e9, y1 = 1e9, x2 = -1e9, y2 = -1e9;
        size_t pts = 0;
        for (const JVal *s = strokes->kid; s; s = s->next) {
            if (s->type != J_ARR) continue;
            int i = 0;
            for (const JVal *e = s->kid; e; e = e->next, i++) {
                if (e->type != J_NUM) continue;
                if (i % 2 == 0) { x1 = fmin(x1, e->num); x2 = fmax(x2, e->num); }
                else { y1 = fmin(y1, e->num); y2 = fmax(y2, e->num); pts++; }
            }
        }
        if (!pts || pts > 200000 || x2 < x1) { reply_err(id, "bad-request", "strokes"); return; }
        x1 -= width; y1 -= width; x2 += width; y2 += width;
        double sc = 3;                                  /* image at 216 dpi */
        int W = (int)ceil((x2 - x1) * sc), H = (int)ceil((y2 - y1) * sc);
        if (W < 1 || H < 1 || W > 4096 || H > 4096) { reply_err(id, "bad-request", "size"); return; }
        cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        cairo_t *cr = cairo_create(img);
        cairo_scale(cr, sc, sc);
        cairo_translate(cr, -x1, -y1);
        cairo_set_source_rgba(cr, color.red / 65535.0, color.green / 65535.0, color.blue / 65535.0, alpha);
        cairo_set_line_width(cr, width);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        for (const JVal *s = strokes->kid; s; s = s->next) {
            if (s->type != J_ARR) continue;
            int i = 0;
            double x = 0;
            for (const JVal *e = s->kid; e; e = e->next, i++) {
                if (e->type != J_NUM) continue;
                if (i % 2 == 0) x = e->num;
                else if (i == 1) cairo_move_to(cr, x, e->num);
                else cairo_line_to(cr, x, e->num);
            }
            if (i == 2) cairo_line_to(cr, x + 0.01, s->kid->next->num);
            cairo_stroke(cr);
        }
        cairo_destroy(cr);
        cairo_surface_flush(img);
        PopplerRectangle r = { x1, ph - y2, x2, ph - y1 };
        an = poppler_annot_stamp_new(doc, &r);
        GError *err = NULL;
        if (an && !poppler_annot_stamp_set_custom_image(POPPLER_ANNOT_STAMP(an), img, &err)) {
            g_object_unref(an);
            an = NULL;
            if (err) g_error_free(err);
        }
        cairo_surface_destroy(img);
    }
    if (!an) { reply_err(id, "bad-request", "type"); return; }
    if (type && strcmp(type, "ink")) poppler_annot_set_color(an, &color);
    poppler_page_add_annot(p, an);
    remember(pg, an);
    g_object_unref(an);
    reply_done_empty(id);
}

static void op_undo(uint32_t id, const JVal *a) {
    int pg = (int)json_num(a, "page", -1);
    Added **pp = &added;
    while (*pp && (*pp)->page != pg) pp = &(*pp)->next;
    PopplerPage *p = page_at(pg);
    if (!*pp || !p) { reply_err(id, "nothing", ""); return; }
    Added *x = *pp;
    *pp = x->next;
    poppler_page_remove_annot(p, x->annot);
    g_object_unref(x->annot);
    free(x);
    reply_done_empty(id);
}

static void op_save(uint32_t id, const DocMsg *m, const JVal *a) {
    int fd = m->nfds ? m->fds[0] : slot_fd(a, "slot");      /* new file opened by axia-server */
    if (!doc || fd < 0) { reply_err(id, "bad-request", "slot"); return; }
    GError *err = NULL;
    int d = dup(fd);
    gboolean ok = d >= 0 && poppler_document_save_to_fd(doc, d, TRUE, &err);
    /* poppler-glib does fdopen(d) and doesn't close that FILE: the last part stays in the stdio buffer
     * until the process exits. It's flushed here and the fd is closed (the FILE is left empty). */
    fflush(NULL);
    if (d >= 0) close(d);
    if (!ok) { reply_err(id, "save", err ? err->message : strerror(errno)); if (err) g_error_free(err); return; }
    reply_done_empty(id);
}

static void op_blob(uint32_t id) {
    if (!doc) { reply_err(id, "bad-request", ""); return; }
    int fd = doc_memfd_new("axia-doc-pdf", 0);
    GError *err = NULL;
    int d = fd >= 0 ? dup(fd) : -1;
    gboolean ok = d >= 0 && poppler_document_save_to_fd(doc, d, TRUE, &err);
    fflush(NULL);                            /* see op_save */
    if (d >= 0) close(d);
    if (!ok) {
        reply_err(id, "save", err ? err->message : "");
        if (err) g_error_free(err);
        if (fd >= 0) close(fd);
        return;
    }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size <= 0 || doc_seal(fd)) { close(fd); reply_err(id, "save", ""); return; }
    reply_blob(id, fd, (size_t)size);
}

int pdf_handle(const DocMsg *m, const JVal *a) {
    uint32_t id = m->h.id;
    if (m->h.op != DQ_PDF_OPEN && !doc) { reply_err(id, "not-open", ""); return 1; }
    switch (m->h.op) {
    case DQ_PDF_OPEN: op_open(id, a); return 1;
    case DQ_PDF_RENDER: op_render(id, a); return 1;
    case DQ_PDF_SELECT: op_select(id, a); return 1;
    case DQ_PDF_FIND: op_find(id, a); return 1;
    case DQ_PDF_OUTLINE: op_outline(id); return 1;
    case DQ_PDF_PAGEINFO: op_pageinfo(id, a); return 1;
    case DQ_PDF_SETFIELD: op_setfield(id, a); return 1;
    case DQ_PDF_ANNOT: op_annot(id, a); return 1;
    case DQ_PDF_UNDO: op_undo(id, a); return 1;
    case DQ_PDF_SAVE: op_save(id, m, a); return 1;
    case DQ_PDF_BLOB: op_blob(id); return 1;
    case DQ_PDF_SIZES: op_sizes(id); return 1;
    default: return 0;
    }
}

/* Thumbnail of page 1 without its own session (Quick Preview, Files thumbnails): the utilities
 * session is already warm, so the process startup isn't paid. */
/* A minimal PDF rendered once: loads Poppler, cairo and fontconfig before the first Quick
 * Look (Files asks the utilities session for it when it opens). */
void pdf_warm(uint32_t id) {
    static const char mini[] =
        "%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
        "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 72 72]/Contents 4 0 R/Resources<</Font<</F1 5 0 R>>>>>>endobj\n"
        "4 0 obj<</Length 33>>stream\nBT /F1 12 Tf 10 30 Td (Aa) Tj ET\nendstream endobj\n"
        "5 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\ntrailer<</Root 1 0 R>>\n%%EOF\n";
    GBytes *bytes = g_bytes_new_static(mini, sizeof mini - 1);
    PopplerDocument *dd = poppler_document_new_from_bytes(bytes, NULL, NULL);
    if (dd) {
        PopplerPage *p = poppler_document_get_page(dd, 0);
        if (p) {
            cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
            cairo_t *cr = cairo_create(s);
            poppler_page_render(p, cr);
            cairo_destroy(cr);
            cairo_surface_destroy(s);
            g_object_unref(p);
        }
        g_object_unref(dd);
    }
    g_bytes_unref(bytes);
    reply_done_empty(id);
}

void pdf_thumb(const DocMsg *m, const JVal *a) {
    if (m->nfds < 1) { reply_err(m->h.id, "bad-request", ""); return; }
    GError *err = NULL;
    int d = dup(m->fds[0]);
    PopplerDocument *dd = d >= 0 ? poppler_document_new_from_fd(d, NULL, &err) : NULL;
    if (!dd) {
        int locked = err && err->domain == POPPLER_ERROR && err->code == POPPLER_ERROR_ENCRYPTED;
        reply_err(m->h.id, locked ? "password" : "invalid", err ? err->message : "");
        if (err) g_error_free(err);
        if (d >= 0) close(d);
        return;
    }
    int n = poppler_document_get_n_pages(dd);
    PopplerPage *p = n > 0 ? poppler_document_get_page(dd, 0) : NULL;
    if (!p) { g_object_unref(dd); reply_err(m->h.id, "invalid", "pages"); return; }
    double pw, ph;
    poppler_page_get_size(p, &pw, &ph);
    double mw = json_num(a, "width", 400), mh = json_num(a, "height", 600);
    if (!(mw >= 16 && mw <= 4096)) mw = 400;
    if (!(mh >= 16 && mh <= 4096)) mh = 600;
    double s = fmin(mw / pw, mh / ph);
    int W = (int)fmax(1, lround(pw * s)), H = (int)fmax(1, lround(ph * s));
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, s, s);
    poppler_page_render(p, cr);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    uint32_t *px = (uint32_t *)cairo_image_surface_get_data(surf);
    if (json_bool(a, "invert", 0)) invert_lightness(px, (size_t)W * H);
    char extra[48];
    snprintf(extra, sizeof extra, ",\"pages\":%d", n);
    reply_image(m->h.id, px, W, H, extra);
    cairo_surface_destroy(surf);
    g_object_unref(p);
    g_object_unref(dd);
}
