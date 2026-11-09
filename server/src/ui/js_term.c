/* js_term.c — lord.term.* and lord.clipboard for the Terminal app.
 *
 * lord.term.create() is the only API that starts a process: always the login shell of the
 * user already running the session (see term/pty.c). It accepts no commands or arguments. */
#include "js_term.h"
#include "../wl/wl.h"
#include "ui.h"
#include "../term/term.h"
#include "../term/termfill.h"
#include "../term/tcapture.h"
#include "../term/sftpc.h"
#include "js_sys.h"
#include "js_err.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Session *arg_session(JSContext *c, JSValueConst v) {
    int32_t id;
    if (JS_ToInt32(c, &id, v)) return NULL;
    return term_get(id);
}

static double num(JSContext *c, JSValueConst o, const char *k, double def) {
    JSValue v = JS_GetPropertyStr(c, o, k);
    double d = def;
    if (JS_IsNumber(v) || JS_IsBool(v)) JS_ToFloat64(c, &d, v);
    JS_FreeValue(c, v);
    return d;
}

static int has(JSContext *c, JSValueConst o, const char *k) {
    JSValue v = JS_GetPropertyStr(c, o, k);
    int r = !JS_IsUndefined(v) && !JS_IsNull(v);
    JS_FreeValue(c, v);
    return r;
}

static void str(JSContext *c, JSValueConst o, const char *k, char *out, size_t size) {
    JSValue v = JS_GetPropertyStr(c, o, k);
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        if (s) { snprintf(out, size, "%s", s); JS_FreeCString(c, s); }
    }
    JS_FreeValue(c, v);
}

static JSValue str_or_null(JSContext *c, char *s) {
    JSValue r = s ? JS_NewString(c, s) : JS_NULL;
    free(s);
    return r;
}

/* ---- lifecycle ---- */

static JSValue f_create(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    char cwd[PATH_MAX] = "";
    int scrollback = 0;
    if (argc > 0 && JS_IsObject(argv[0])) {
        str(c, argv[0], "cwd", cwd, sizeof cwd);
        scrollback = (int)num(c, argv[0], "scrollback", 0);
    }
    Session *s = term_create(cwd[0] ? cwd : NULL, scrollback);
    if (!s) return js_throw_code(c, "term.createFailed", NULL);
    return JS_NewInt32(c, s->id);
}

static JSValue f_list(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    int ids[TERM_MAX];
    int n = term_list(ids, TERM_MAX);
    JSValue a = JS_NewArray(c);
    for (int i = 0; i < n; i++) JS_SetPropertyUint32(c, a, (uint32_t)i, JS_NewInt32(c, ids[i]));
    return a;
}

static JSValue f_close(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    term_close(arg_session(c, argv[0]));
    return JS_UNDEFINED;
}

/* Colors: 0xRRGGBB integers (the app converts them from '#rrggbb'). */
static JSValue f_configure(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    JSValueConst o = argv[1];
    if (!s || !JS_IsObject(o)) return JS_UNDEFINED;
    TermProfile *p = &s->prof;
    float fs = (float)num(c, o, "fontSize", p->font_size);
    p->font_size = fs < 6 ? 6 : fs > 72 ? 72 : fs;
    float ls = (float)num(c, o, "lineSpacing", p->line_spacing);
    p->line_spacing = ls < 0.8f ? 0.8f : ls > 2 ? 2 : ls;
    p->padding = (float)num(c, o, "padding", p->padding);
    JSValue pal = JS_GetPropertyStr(c, o, "palette");
    if (JS_IsArray(c, pal))
        for (uint32_t i = 0; i < 16; i++) {
            JSValue e = JS_GetPropertyUint32(c, pal, i);
            double d;
            if (JS_IsNumber(e) && !JS_ToFloat64(c, &d, e)) p->palette[i] = (uint32_t)d & 0xffffff;
            JS_FreeValue(c, e);
        }
    JS_FreeValue(c, pal);
#define COLOR(key, field) if (has(c, o, key)) p->field = (uint32_t)num(c, o, key, 0) & 0xffffff
    COLOR("foreground", fg);
    COLOR("background", bg);
    COLOR("cursor", cursor);
    COLOR("selection", sel_bg);
    COLOR("link", link);
    COLOR("cursorGuideColor", guide);
#undef COLOR
    p->bold = has(c, o, "bold") ? (uint32_t)num(c, o, "bold", 0) & 0xffffff : 0;
    p->has_cursor_text = has(c, o, "cursorText");
    if (p->has_cursor_text) p->cursor_text = (uint32_t)num(c, o, "cursorText", 0) & 0xffffff;
    p->has_sel_fg = has(c, o, "selectionText");
    if (p->has_sel_fg) p->sel_fg = (uint32_t)num(c, o, "selectionText", 0) & 0xffffff;
    float bo = (float)num(c, o, "backgroundOpacity", p->bg_opacity);
    p->bg_opacity = bo < 0 ? 0 : bo > 1 ? 1 : bo;
    p->min_contrast = (float)num(c, o, "minimumContrast", p->min_contrast);
    p->bold_bright = (int)num(c, o, "boldBright", p->bold_bright);
    p->use_bold = (int)num(c, o, "useBoldFont", p->use_bold);
    p->use_italic = (int)num(c, o, "useItalic", p->use_italic);
    p->cursor_blink = (int)num(c, o, "cursorBlink", p->cursor_blink);
    p->cursor_guide = (int)num(c, o, "cursorGuide", p->cursor_guide);
    p->blink_text = (int)num(c, o, "blinkText", p->blink_text);
    p->alt_esc = (int)num(c, o, "altSendsEscape", p->alt_esc);
    p->mouse_reporting = (int)num(c, o, "mouseReporting", p->mouse_reporting);
    p->timestamps = (int)num(c, o, "timestamps", p->timestamps);
    p->copy_on_select = (int)num(c, o, "copyOnSelect", p->copy_on_select);
    p->alt_scroll = (int)num(c, o, "alternateScroll", p->alt_scroll);
    char style[16] = "";
    str(c, o, "cursorStyle", style, sizeof style);
    if (!strcmp(style, "block")) p->cursor_style = CURSOR_BLOCK;
    else if (!strcmp(style, "underline")) p->cursor_style = CURSOR_UNDERLINE;
    else if (!strcmp(style, "beam")) p->cursor_style = CURSOR_BEAM;
    str(c, o, "wordChars", p->word_chars, sizeof p->word_chars);
    p->ligatures = (int)num(c, o, "ligatures", p->ligatures);
    p->bidi = (int)num(c, o, "bidi", p->bidi);
    char fam[64], na[64];
    snprintf(fam, sizeof fam, "%s", p->font_family);
    snprintf(na, sizeof na, "%s", p->na_family);
    str(c, o, "font", fam, sizeof fam);
    str(c, o, "nonAsciiFont", na, sizeof na);
    term_set_fonts(p, fam, na);
    vt_apply_palette(&s->t);
    Node *n = node_get(s->node);
    if (n) node_invalidate(n);
    return JS_UNDEFINED;
}

/* ---- input ---- */

static JSValue f_write(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    size_t n;
    uint8_t *raw = JS_GetArrayBuffer(c, &n, argv[1]);      /* raw bytes as-is ("hex" mappings) */
    if (raw) { if (s) term_input(s, (const char *)raw, n); return JS_UNDEFINED; }
    JS_FreeValue(c, JS_GetException(c));
    const char *d = JS_ToCStringLen(c, &n, argv[1]);
    if (s && d) term_input(s, d, n);
    if (d) JS_FreeCString(c, d);
    return JS_UNDEFINED;
}

static JSValue f_key(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    const char *key = JS_ToCString(c, argv[1]), *chr = JS_ToCString(c, argv[2]), *base = JS_ToCString(c, argv[3]);
    int32_t mods = 0;
    JS_ToInt32(c, &mods, argv[4]);
    if (s && key && chr && base) term_key(s, key, chr, base, mods);
    if (key) JS_FreeCString(c, key);
    if (chr) JS_FreeCString(c, chr);
    if (base) JS_FreeCString(c, base);
    return JS_UNDEFINED;
}

static JSValue f_paste(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    const char *d = JS_ToCString(c, argv[1]);
    if (s && d) term_paste(s, d);
    if (d) JS_FreeCString(c, d);
    return JS_UNDEFINED;
}

static JSValue f_mouse(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    const char *type = JS_ToCString(c, argv[1]);
    double x = 0, y = 0;
    int32_t button = 0, mods = 0;
    JS_ToFloat64(c, &x, argv[2]);
    JS_ToFloat64(c, &y, argv[3]);
    JS_ToInt32(c, &button, argv[4]);
    JS_ToInt32(c, &mods, argv[5]);
    int r = s && type ? term_mouse(s, type, (float)x, (float)y, button, mods) : 0;
    if (type) JS_FreeCString(c, type);
    return JS_NewInt32(c, r);
}

static JSValue f_wheel(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    int32_t lines = 0, mods = 0;
    double x = 0, y = 0;
    JS_ToInt32(c, &lines, argv[1]);
    JS_ToFloat64(c, &x, argv[2]);
    JS_ToFloat64(c, &y, argv[3]);
    JS_ToInt32(c, &mods, argv[4]);
    if (s) term_wheel(s, lines, (float)x, (float)y, mods);
    return JS_UNDEFINED;
}

static JSValue f_focus(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    if (s) term_focus(s, JS_ToBool(c, argv[1]));
    return JS_UNDEFINED;
}

/* ---- view, history, selection ---- */

static JSValue f_scroll(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    if (!s) return JS_UNDEFINED;
    Term *t = &s->t;
    if (JS_IsString(argv[1])) {
        const char *w = JS_ToCString(c, argv[1]);
        if (w) {
            if (!strcmp(w, "top")) vt_scroll_view(t, t->buf->count);
            else if (!strcmp(w, "bottom")) vt_scroll_view(t, -t->view);
            else if (!strcmp(w, "pageUp")) vt_scroll_view(t, t->rows - 1);
            else if (!strcmp(w, "pageDown")) vt_scroll_view(t, -(t->rows - 1));
            JS_FreeCString(c, w);
        }
    } else {
        int32_t d = 0;
        JS_ToInt32(c, &d, argv[1]);
        vt_scroll_view(t, d);
    }
    return JS_UNDEFINED;
}

static JSValue f_select(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    const char *w = JS_ToCString(c, argv[1]);
    int r = 0;
    if (s && w) {
        if (!strcmp(w, "all")) { vt_select_all(&s->t); r = 1; }
        else if (!strcmp(w, "none")) vt_select_clear(&s->t);
        else if (!strcmp(w, "lastOutput")) r = vt_select_last_output(&s->t);
    }
    if (w) JS_FreeCString(c, w);
    return JS_NewBool(c, r);
}

/* Terminal text: "selection", "screen", "all" or "lastOutput". */
static JSValue f_text(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    const char *w = JS_ToCString(c, argv[1]);
    char *out = NULL;
    if (s && w) {
        Term *t = &s->t;
        long long first = vt_first_abs(t), last = first + vt_total_lines(t) - 1;
        if (!strcmp(w, "selection")) out = vt_selection_text(t);
        else if (!strcmp(w, "screen")) out = vt_text_range(t, last - t->rows + 1, last);
        else if (!strcmp(w, "all")) out = vt_text_range(t, first, last);
        else if (!strcmp(w, "lastOutput")) {
            if (vt_select_last_output(t)) { out = vt_selection_text(t); vt_select_clear(t); }
        }
    }
    if (w) JS_FreeCString(c, w);
    return str_or_null(c, out);
}

/* lord.term.highlight(id, text, fg, bg): 0xRRGGBB colors or -1 = unchanged. */
static JSValue f_highlight(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    const char *txt = JS_ToCString(c, argv[1]);
    int64_t fg = -1, bg = -1;
    JS_ToInt64(c, &fg, argv[2]);
    JS_ToInt64(c, &bg, argv[3]);
    int ok = 0;
    if (s && txt && strlen(txt) < 2048) {
        ok = vt_highlight(&s->t, txt, fg < 0 ? -1 : fg & 0xffffff, bg < 0 ? -1 : bg & 0xffffff);
        Node *n = node_get(s->node);
        if (ok && n) node_invalidate(n);
    }
    if (txt) JS_FreeCString(c, txt);
    return JS_NewBool(c, ok);
}

/* lord.term.ackRestored(): the app already announced that sessions were recovered after a restart;
 * a later theme change must not repeat the notice. */
static JSValue f_ack_restored(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)c; (void)this_val; (void)argc; (void)argv;
    int ids[TERM_MAX], n = term_list(ids, TERM_MAX);
    for (int i = 0; i < n; i++) { Session *s = term_get(ids[i]); if (s) s->restored = 0; }
    return JS_UNDEFINED;
}

/* ---- tmux -CC ---- */

/* lord.term.tmuxPane(gateway, n) → id of the virtual session of pane %n */
static JSValue f_tmux_pane(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *g = arg_session(c, argv[0]);
    int32_t pane = -1;
    JS_ToInt32(c, &pane, argv[1]);
    Session *v = term_tmux_pane(g, pane);
    return v ? JS_NewInt32(c, v->id) : JS_NULL;
}

/* lord.term.tmuxCommand(gateway, "tmux command"): one line, without newlines. Only while the
 * session is in control mode: it's the same as what the user would type in that tmux. */
static JSValue f_tmux_command(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *g = arg_session(c, argv[0]);
    size_t n;
    const char *cmd = JS_ToCStringLen(c, &n, argv[1]);
    int ok = g && g->tmux && cmd && n < 4096 && !memchr(cmd, '\n', n) && !memchr(cmd, '\r', n);
    if (ok) {
        term_tmux_send(g, cmd, n);
        term_tmux_send(g, "\n", 1);
    }
    if (cmd) JS_FreeCString(c, cmd);
    return JS_NewBool(c, ok);
}

/* lord.term.feed(id, text): writes to the screen (not to the shell). For tmux panes. */
static JSValue f_feed(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    size_t n;
    const char *d = JS_ToCStringLen(c, &n, argv[1]);
    if (s && d && s->gateway) term_feed(s, d, n);
    if (d) JS_FreeCString(c, d);
    return JS_UNDEFINED;
}

static JSValue f_tmux_ready(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    if (s && s->gateway) term_tmux_ready(s);
    return JS_UNDEFINED;
}

static JSValue f_copy(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    char *t = s ? vt_selection_text(&s->t) : NULL;
    if (t && t[0]) term_set_clipboard(t, strlen(t));
    return str_or_null(c, t);
}

static JSValue f_clear(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    const char *w = JS_ToCString(c, argv[1]);
    if (s && w) {
        if (!strcmp(w, "scrollback")) vt_clear_scrollback(&s->t);
        else if (!strcmp(w, "buffer")) {
            /* Clear buffer (❖K): everything is cleared and the shell redraws the prompt. */
            vt_clear_scrollback(&s->t);
            vt_write(&s->t, (const uint8_t *)"\033[H\033[2J", 7);
            term_input(s, "\f", 1);
        } else if (!strcmp(w, "reset")) vt_reset(&s->t);
    }
    if (w) JS_FreeCString(c, w);
    return JS_UNDEFINED;
}

/* ---- search and marks ---- */

static JSValue search_state(JSContext *c, Session *s) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "count", JS_NewInt32(c, s ? s->t.nmatches : 0));
    JS_SetPropertyStr(c, o, "index", JS_NewInt32(c, s ? s->t.match_cur : -1));
    return o;
}

static JSValue f_search(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    Session *s = arg_session(c, argv[0]);
    const char *q = JS_ToCString(c, argv[1]);
    if (s && q) {
        if (q[0]) vt_search(&s->t, q, argc > 2 && JS_ToBool(c, argv[2]));
        else vt_search_clear(&s->t);
    }
    if (q) JS_FreeCString(c, q);
    return search_state(c, s);
}

static JSValue f_search_move(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    int32_t dir = 1;
    JS_ToInt32(c, &dir, argv[1]);
    if (s) vt_search_move(&s->t, dir < 0 ? -1 : 1);
    return search_state(c, s);
}

static JSValue f_mark(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    const char *w = JS_ToCString(c, argv[1]);
    int r = 0;
    if (s && w) {
        if (!strcmp(w, "set")) { vt_set_mark(&s->t); r = 1; }
        else if (!strcmp(w, "prev")) r = vt_jump_mark(&s->t, -1);
        else if (!strcmp(w, "next")) r = vt_jump_mark(&s->t, 1);
    }
    if (w) JS_FreeCString(c, w);
    return JS_NewBool(c, r);
}

static JSValue f_url_at(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    double x = 0, y = 0;
    JS_ToFloat64(c, &x, argv[1]);
    JS_ToFloat64(c, &y, argv[2]);
    if (!s) return JS_NULL;
    long long line;
    int col;
    term_cell_at(s, (float)x, (float)y, &line, &col);
    return str_or_null(c, vt_url_at(&s->t, line, col));
}

static JSValue f_words(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    const char *p = JS_ToCString(c, argv[1]);
    char *out = s && p ? vt_words(&s->t, p, 40) : NULL;
    if (p) JS_FreeCString(c, p);
    return str_or_null(c, out);
}

static JSValue f_cursor_word(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    return str_or_null(c, s ? vt_cursor_word(&s->t) : NULL);
}

/* ---- regular-expression search (the regex runs in JS) ---- */

static JSValue f_logical_text(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    return str_or_null(c, s ? vt_logical_text(&s->t) : NULL);
}

static JSValue f_set_matches(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    if (!s || !JS_IsArray(c, argv[1])) return search_state(c, s);
    JSValue lv = JS_GetPropertyStr(c, argv[1], "length");
    uint32_t n = 0;
    JS_ToUint32(c, &n, lv);
    JS_FreeValue(c, lv);
    n -= n % 3;
    if (n > 60000) n = 60000;
    int *trip = malloc((n ? n : 1) * sizeof *trip);
    if (!trip) return search_state(c, s);
    for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(c, argv[1], i);
        int32_t v = 0;
        JS_ToInt32(c, &v, e);
        JS_FreeValue(c, e);
        trip[i] = v;
    }
    vt_set_matches(&s->t, trip, (int)(n / 3));
    free(trip);
    return search_state(c, s);
}

/* ---- copy mode ---- */

static JSValue f_copy_mode(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    return JS_NewBool(c, s ? vt_copy_mode(&s->t, JS_ToBool(c, argv[1])) : 0);
}

static JSValue f_copy_key(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    const char *k = JS_ToCString(c, argv[1]);
    char *yank = NULL;
    int r = s && k ? vt_copy_key(&s->t, k, JS_ToBool(c, argv[2]), &yank) : 1;
    if (k) JS_FreeCString(c, k);
    if (yank && yank[0]) term_set_clipboard(yank, strlen(yank));
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "status", JS_NewInt32(c, r));
    JS_SetPropertyStr(c, o, "text", str_or_null(c, yank));
    return o;
}

/* ---- Instant Replay, logging and export ---- */

static JSValue f_replay(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    if (!s) return JS_NULL;
    if (JS_IsString(argv[1])) {
        const char *w = JS_ToCString(c, argv[1]);
        if (w) {
            if (!strcmp(w, "enter")) term_replay(s, s->nframes - 1);
            else if (!strcmp(w, "exit")) term_replay(s, -1);
            JS_FreeCString(c, w);
        }
    } else if (JS_IsNumber(argv[1])) {
        int32_t i = 0;
        JS_ToInt32(c, &i, argv[1]);
        term_replay(s, i < 0 ? 0 : i);
    }
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "count", JS_NewInt32(c, s->nframes));
    JS_SetPropertyStr(c, o, "index", JS_NewInt32(c, s->replay));
    const Frame *f = s->replay >= 0 ? &s->frames[(s->frame_head + s->replay) % REPLAY_FRAMES] : NULL;
    const Frame *f0 = s->nframes ? &s->frames[s->frame_head] : NULL;
    JS_SetPropertyStr(c, o, "time", JS_NewFloat64(c, f ? f->time : 0));
    JS_SetPropertyStr(c, o, "start", JS_NewFloat64(c, f0 ? f0->time : 0));
    Node *n = node_get(s->node);
    if (n) node_invalidate(n);
    return o;
}

static JSValue f_log(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    const char *path = s ? term_log(s, JS_ToBool(c, argv[1])) : NULL;
    return path ? JS_NewString(c, path) : JS_NULL;
}

static JSValue f_export(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    char path[600];
    return s && !term_export(s, path, sizeof path) ? JS_NewString(c, path) : JS_NULL;
}

/* ---- estado ---- */

static void proc_comm(pid_t pid, char *out, size_t size) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (!f) return;
    if (fgets(out, (int)size, f)) out[strcspn(out, "\n")] = 0;
    fclose(f);
}

static JSValue f_info(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    if (!s) return JS_NULL;
    Term *t = &s->t;
    JSValue o = JS_NewObject(c);
    char proc[64] = "", cwd[PATH_MAX] = "";
    pid_t pg = 0;
    if (s->alive) {
        pg = tcgetpgrp(s->fd);
        proc_comm(pg > 0 ? pg : s->pid, proc, sizeof proc);
        char link[64];
        snprintf(link, sizeof link, "/proc/%d/cwd", (int)(pg > 0 ? pg : s->pid));
        ssize_t n = readlink(link, cwd, sizeof cwd - 1);
        if (n < 0) { snprintf(link, sizeof link, "/proc/%d/cwd", (int)s->pid); n = readlink(link, cwd, sizeof cwd - 1); }
        cwd[n > 0 ? n : 0] = 0;
    }
    char shell[64] = "";
    if (s->alive) proc_comm(s->pid, shell, sizeof shell);
    JS_SetPropertyStr(c, o, "title", JS_NewString(c, t->title));
    JS_SetPropertyStr(c, o, "cwd", JS_NewString(c, t->cwd[0] ? t->cwd : cwd));
    JS_SetPropertyStr(c, o, "process", JS_NewString(c, proc));
    JS_SetPropertyStr(c, o, "shell", JS_NewString(c, shell));
    JS_SetPropertyStr(c, o, "busy", JS_NewBool(c, proc[0] && shell[0] && strcmp(proc, shell)));
    JS_SetPropertyStr(c, o, "pid", JS_NewInt32(c, s->pid));
    /* the foreground program's process group (its leader's pid: Claude Code's ~/.claude/sessions/<pid>.json) */
    JS_SetPropertyStr(c, o, "fgPid", JS_NewInt32(c, pg > 0 ? (int)pg : (int)s->pid));
    JS_SetPropertyStr(c, o, "alive", JS_NewBool(c, s->alive));
    JS_SetPropertyStr(c, o, "exitCode", JS_NewInt32(c, s->exit_code));
    JS_SetPropertyStr(c, o, "cols", JS_NewInt32(c, t->cols));
    JS_SetPropertyStr(c, o, "rows", JS_NewInt32(c, t->rows));
    JS_SetPropertyStr(c, o, "scrollback", JS_NewInt32(c, t->buf->count - t->rows));
    JS_SetPropertyStr(c, o, "view", JS_NewInt32(c, t->view));
    JS_SetPropertyStr(c, o, "altScreen", JS_NewBool(c, t->buf == &t->alt));
    JS_SetPropertyStr(c, o, "mouseReporting", JS_NewBool(c, (t->modes & M_MOUSE) != 0));
    JS_SetPropertyStr(c, o, "hasSelection", JS_NewBool(c, t->sel_active));
    JS_SetPropertyStr(c, o, "badge", JS_NewString(c, t->badge));
    JS_SetPropertyStr(c, o, "host", JS_NewString(c, t->host));
    JS_SetPropertyStr(c, o, "key", JS_NewString(c, s->key));
    JS_SetPropertyStr(c, o, "restored", JS_NewBool(c, s->restored));
    JS_SetPropertyStr(c, o, "tmux", JS_NewBool(c, s->tmux));
    JS_SetPropertyStr(c, o, "gateway", JS_NewInt32(c, s->gateway));
    JS_SetPropertyStr(c, o, "pane", JS_NewInt32(c, s->gateway ? s->pane : -1));
    JS_SetPropertyStr(c, o, "cellWidth", JS_NewFloat64(c, t->cell_w));
    JS_SetPropertyStr(c, o, "cellHeight", JS_NewFloat64(c, t->cell_h));
    JS_SetPropertyStr(c, o, "cursorX", JS_NewInt32(c, t->cx));       /* cursor cell (input methods) */
    JS_SetPropertyStr(c, o, "cursorY", JS_NewInt32(c, t->cy));
    JS_SetPropertyStr(c, o, "padding", JS_NewFloat64(c, s->prof.padding));
    JS_SetPropertyStr(c, o, "copyMode", JS_NewBool(c, t->copy_active));
    JS_SetPropertyStr(c, o, "replay", JS_NewBool(c, s->replay >= 0));
    JS_SetPropertyStr(c, o, "logging", s->log ? JS_NewString(c, s->log_path) : JS_NULL);
    return o;
}

/* ---- clipboard (of the LordOS session, in memory) ---- */

static JSValue f_clip_get(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewString(c, term_clipboard());
}

/* lord.clipboard.set(text, { secure, origin }): secure marks the copy as sensitive (a
 * password: the history doesn't keep it); origin is a short name ("lordos", the app's id). */
static JSValue f_clip_set(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    size_t n;
    const char *s = JS_ToCStringLen(c, &n, argv[0]);
    if (!s) return JS_EXCEPTION;
    term_set_clipboard(s, n > (16u << 20) ? 16u << 20 : n);
    JS_FreeCString(c, s);
    int secure = 0;
    char origin[64] = "lordos";
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(c, argv[1], "secure");
        secure = JS_ToBool(c, v);
        JS_FreeValue(c, v);
        v = JS_GetPropertyStr(c, argv[1], "origin");
        const char *o = JS_IsString(v) ? JS_ToCString(c, v) : NULL;
        if (o) {
            int ok = strlen(o) < sizeof origin;
            for (const char *p = o; ok && *p; p++) ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr(".-_", *p);
            if (ok && o[0]) snprintf(origin, sizeof origin, "%s", o);
            JS_FreeCString(c, o);
        }
        JS_FreeValue(c, v);
    }
    term_clipboard_mark(origin, secure);
    return JS_UNDEFINED;
}

/* Passwords in the Terminal (term/termfill.c): what asks for a password now, or null:
 * { prompt, program, exe, host, user, port }. The program and the account come from the asking process's own
 * executable and arguments (/proc), not from the text it printed, so a script can't pass for ssh. */
static JSValue f_asking(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Session *s = arg_session(c, argv[0]);
    char prompt[200], exe[512];
    TcapTarget t;
    if (!s || !term_asks_secret(s, prompt, sizeof prompt)) return JS_NULL;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "prompt", JS_NewString(c, prompt));
    if (!term_secret_target(s, prompt, &t, exe, sizeof exe)) {
        JS_SetPropertyStr(c, o, "program", JS_NewString(c, t.program));
        JS_SetPropertyStr(c, o, "exe", JS_NewString(c, exe));
        JS_SetPropertyStr(c, o, "host", JS_NewString(c, t.host));
        JS_SetPropertyStr(c, o, "user", JS_NewString(c, t.user));
        JS_SetPropertyStr(c, o, "port", JS_NewString(c, t.port));
    }
    return o;
}

static int arg_token(JSContext *c, JSValueConst v, unsigned *token) {
    double d = 0;
    if (JS_ToFloat64(c, &d, v) || !(d >= 1 && d <= 4294967295.0) || d != (double)(unsigned)d) return -1;
    *token = (unsigned)d;
    return 0;
}

/* saveSecret(sid, token): the typed password the "save" event offered goes to Passwords (termfill_save); JS never
 * has it. false if the offer is gone (declined, expired, another prompt). The "saved" event says how it ended. */
static JSValue f_save_secret(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t sid = 0;
    unsigned token = 0;
    if (argc < 2 || JS_ToInt32(c, &sid, argv[0]) || arg_token(c, argv[1], &token)) return JS_FALSE;
    return JS_NewBool(c, termfill_save(sid, token) == 0);
}

/* dropSecret(sid, token): "Not Now": the typed password is wiped */
static JSValue f_drop_secret(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t sid = 0;
    unsigned token = 0;
    if (argc < 2 || JS_ToInt32(c, &sid, argv[0]) || arg_token(c, argv[1], &token)) return JS_FALSE;
    if (!tcap_value(sid, token, NULL, NULL)) return JS_FALSE;
    tcap_drop(sid);
    return JS_TRUE;
}

/* fill(sid, vault, entry, field, index): the entry's field goes from lord-vault to the pty without passing
 * through here; the "fill" event says how it ended. false when it couldn't be asked for. */
static JSValue f_fill(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 5) return JS_FALSE;
    int32_t sid = 0, idx = 0;
    JS_ToInt32(c, &sid, argv[0]);
    JS_ToInt32(c, &idx, argv[4]);
    const char *v = JS_ToCString(c, argv[1]), *e = JS_ToCString(c, argv[2]), *f = JS_ToCString(c, argv[3]);
    int ok = v && e && f && termfill_start(sid, v, e, f, idx) == 0;
    if (v) JS_FreeCString(c, v);
    if (e) JS_FreeCString(c, e);
    if (f) JS_FreeCString(c, f);
    return JS_NewBool(c, ok);
}

static const JSCFunctionListEntry term_funcs[] = {
    JS_CFUNC_DEF("asking", 1, f_asking),
    JS_CFUNC_DEF("saveSecret", 2, f_save_secret),
    JS_CFUNC_DEF("dropSecret", 2, f_drop_secret),
    JS_CFUNC_DEF("fill", 5, f_fill),
    JS_CFUNC_DEF("create", 1, f_create),
    JS_CFUNC_DEF("list", 0, f_list),
    JS_CFUNC_DEF("close", 1, f_close),
    JS_CFUNC_DEF("configure", 2, f_configure),
    JS_CFUNC_DEF("write", 2, f_write),
    JS_CFUNC_DEF("key", 5, f_key),
    JS_CFUNC_DEF("paste", 2, f_paste),
    JS_CFUNC_DEF("mouse", 6, f_mouse),
    JS_CFUNC_DEF("wheel", 5, f_wheel),
    JS_CFUNC_DEF("focus", 2, f_focus),
    JS_CFUNC_DEF("scroll", 2, f_scroll),
    JS_CFUNC_DEF("select", 2, f_select),
    JS_CFUNC_DEF("text", 2, f_text),
    JS_CFUNC_DEF("copy", 1, f_copy),
    JS_CFUNC_DEF("clear", 2, f_clear),
    JS_CFUNC_DEF("search", 3, f_search),
    JS_CFUNC_DEF("searchMove", 2, f_search_move),
    JS_CFUNC_DEF("mark", 2, f_mark),
    JS_CFUNC_DEF("urlAt", 3, f_url_at),
    JS_CFUNC_DEF("words", 2, f_words),
    JS_CFUNC_DEF("cursorWord", 1, f_cursor_word),
    JS_CFUNC_DEF("info", 1, f_info),
    JS_CFUNC_DEF("logicalText", 1, f_logical_text),
    JS_CFUNC_DEF("setMatches", 2, f_set_matches),
    JS_CFUNC_DEF("copyMode", 2, f_copy_mode),
    JS_CFUNC_DEF("copyKey", 3, f_copy_key),
    JS_CFUNC_DEF("replay", 2, f_replay),
    JS_CFUNC_DEF("log", 2, f_log),
    JS_CFUNC_DEF("export", 1, f_export),
    JS_CFUNC_DEF("highlight", 4, f_highlight),
    JS_CFUNC_DEF("tmuxPane", 2, f_tmux_pane),
    JS_CFUNC_DEF("tmuxCommand", 2, f_tmux_command),
    JS_CFUNC_DEF("feed", 2, f_feed),
    JS_CFUNC_DEF("tmuxReady", 1, f_tmux_ready),
    JS_CFUNC_DEF("ackRestored", 0, f_ack_restored),
};

/* lord.clipboard.state() → { gen, kind: 'text' | 'image', image (PNG preview), imageGen } */
static JSValue f_clip_state(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "gen", JS_NewInt64(c, term_clipboard_gen()));
    JS_SetPropertyStr(c, o, "kind", JS_NewString(c, wl_clip_is_image() ? "image" : "text"));
    JS_SetPropertyStr(c, o, "image", JS_NewString(c, wl_clip_image_path()));
    JS_SetPropertyStr(c, o, "imageGen", JS_NewInt64(c, wl_clip_image_gen()));
    int img = wl_clip_is_image();
    JS_SetPropertyStr(c, o, "origin", JS_NewString(c, img ? wl_clip_image_origin() : term_clipboard_origin()));
    JS_SetPropertyStr(c, o, "sensitive", JS_NewBool(c, img ? wl_clip_image_sensitive() : term_clipboard_sensitive()));
    return o;
}

/* lord.clipboard.saveImage(path): saves the copied image (if the last thing copied is an
 * image) as PNG to a path in the home folder (same rules as lord.fs.write). For the
 * persistent history and for notes. */
static JSValue f_clip_save_image(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    const char *src = wl_clip_image_path();
    if (!wl_clip_is_image() || !src[0]) return JS_FALSE;
    const char *p = JS_ToCString(c, argv[0]);
    if (!p) return JS_EXCEPTION;
    char dst[PATH_MAX + 300];
    int r = js_sys_resolve_target(p, dst, sizeof dst);
    JS_FreeCString(c, p);
    if (r) return js_throw_code(c, "fs.outsideHome", NULL);
    size_t n = 0;
    FILE *in = fopen(src, "rb");
    if (!in) return JS_FALSE;
    char tmp[PATH_MAX + 320];
    snprintf(tmp, sizeof tmp, "%s.lordos-tmp", dst);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    FILE *out = fd >= 0 ? fdopen(fd, "wb") : NULL;
    int ok = out != NULL;
    char buf[65536];
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, n, out) != n) ok = 0;
    fclose(in);
    if (out && fclose(out)) ok = 0;
    if (!out && fd >= 0) close(fd);
    if (ok && rename(tmp, dst)) ok = 0;
    if (!ok) unlink(tmp);
    return JS_NewBool(c, ok);
}

/* lord.clipboard.useImage(path): copies an image from the history again (only the
 * previews the compositor saved in $XDG_RUNTIME_DIR/lordos). */
static JSValue f_clip_use_image(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    const char *p = JS_ToCString(c, argv[0]);
    int ok = p && wl_clip_use_image(p) == 0;
    if (p) JS_FreeCString(c, p);
    return JS_NewBool(c, ok);
}

/* lord.clipboard.imageData() → ArrayBuffer with the copied PNG (or null). To save it, the app
 * uses lord.fs.write, which has its own rules (only the home folder). */
static JSValue f_clip_image_data(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    size_t n = 0;
    const unsigned char *png = wl_clip_png(&n);
    return png ? JS_NewArrayBufferCopy(c, png, n) : JS_NULL;
}

/* lord.clipboard.deleteSaved(name): deletes an image from the saved history. Only names
 * img-<letters, numbers, hyphens>.png inside ~/.local/share/lordos/clipboard (there is no general
 * file deletion). */
static JSValue f_clip_delete_saved(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    const char *n = JS_ToCString(c, argv[0]);
    if (!n) return JS_EXCEPTION;
    size_t len = strlen(n);
    int ok = len > 8 && len < 80 && !strncmp(n, "img-", 4) && !strcmp(n + len - 4, ".png");
    for (size_t i = 4; ok && i < len - 4; i++) ok = (n[i] >= 'a' && n[i] <= 'z') || (n[i] >= '0' && n[i] <= '9') || n[i] == '-';
    char path[PATH_MAX];
    const char *home = getenv("HOME");
    if (ok && home) snprintf(path, sizeof path, "%s/.local/share/lordos/clipboard/%s", home, n);
    JS_FreeCString(c, n);
    return JS_NewBool(c, ok && home && unlink(path) == 0);
}

static const JSCFunctionListEntry clip_funcs[] = {
    JS_CFUNC_DEF("get", 0, f_clip_get),
    JS_CFUNC_DEF("set", 1, f_clip_set),
    JS_CFUNC_DEF("state", 0, f_clip_state),
    JS_CFUNC_DEF("imageData", 0, f_clip_image_data),
    JS_CFUNC_DEF("useImage", 1, f_clip_use_image),
    JS_CFUNC_DEF("saveImage", 1, f_clip_save_image),
    JS_CFUNC_DEF("deleteSaved", 1, f_clip_delete_saved),
};

/* Files mode (term/sftpc.c): a session's foreground process group, to find its ssh master by pid */
static pid_t fg_of_session(int sid) {
    Session *s = term_get(sid);
    if (!s || !s->alive || s->fd < 0) return -1;
    pid_t pg = tcgetpgrp(s->fd);
    return pg > 0 ? pg : -1;
}

void js_term_register(JSContext *c, JSValue lord) {
    sftp_set_fg(fg_of_session);
    sftp_mux_dir();                     /* the ssh masters' folder, before the first shell starts */
    JSValue t = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, t, term_funcs, sizeof term_funcs / sizeof term_funcs[0]);
    JS_SetPropertyStr(c, lord, "term", t);
    JSValue k = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, k, clip_funcs, sizeof clip_funcs / sizeof clip_funcs[0]);
    JS_SetPropertyStr(c, lord, "clipboard", k);
}
