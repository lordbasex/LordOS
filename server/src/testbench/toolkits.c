/* toolkits.c — the toolkit audit's model (toolkits.h): pure, no process or file here. */
#include "toolkits.h"
#include "globtable.h"
#include "../sec/json.h"

#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "expected_globals.h"

_Static_assert(TB_EXPECTED_N == TK_GLOBALS, "TK_GLOBALS must follow expected_globals.h");

/* ---- the combinations ---- */

#define PROBE_GTK(v) "import gi; gi.require_version('Gtk', '" v "')"
static const TkCombo combos[TK_COMBOS] = {
    { "gtk3-wayland", "gtk3", "GTK 3", "wayland", "GDK_BACKEND", 1, PROBE_GTK("3.0") },
    { "gtk3-x11", "gtk3", "GTK 3", "x11", "GDK_BACKEND", 0, PROBE_GTK("3.0") },
    { "gtk4-wayland", "gtk4", "GTK 4", "wayland", "GDK_BACKEND", 1, PROBE_GTK("4.0") },
    { "gtk4-x11", "gtk4", "GTK 4", "x11", "GDK_BACKEND", 0, PROBE_GTK("4.0") },
    { "qt5-wayland", "qt5", "Qt 5", "wayland", "QT_QPA_PLATFORM", 1, "import PyQt5.QtCore, PyQt5.QtWidgets" },
    { "qt5-xcb", "qt5", "Qt 5", "xcb", "QT_QPA_PLATFORM", 0, "import PyQt5.QtCore, PyQt5.QtWidgets" },
    { "qt6-wayland", "qt6", "Qt 6", "wayland", "QT_QPA_PLATFORM", 1, "import PyQt6.QtCore, PyQt6.QtWidgets" },
    { "qt6-xcb", "qt6", "Qt 6", "xcb", "QT_QPA_PLATFORM", 0, "import PyQt6.QtCore, PyQt6.QtWidgets" },
    { "x11-xwayland", "x11", "X11", "x11", NULL, 0, "import socket, struct, json" },
};

const TkCombo *tk_combo(int i) { return i >= 0 && i < TK_COMBOS ? &combos[i] : NULL; }

int tk_combo_find(const char *id, size_t len) {
    for (int i = 0; i < TK_COMBOS; i++)
        if (strlen(combos[i].id) == len && !memcmp(combos[i].id, id, len)) return i;
    return -1;
}

static const struct { const char *id; int w; } check_tab[TK_CHECKS] = {
    { "map", 2 }, { "size", 2 }, { "scale", 1 }, { "theme", 1 }, { "backend", 2 }, { "decoration", 2 },
    { "first_frame", 2 }, { "menu", 3 }, { "combo", 3 }, { "tooltip", 1 }, { "clipboard", 3 }, { "dnd", 3 },
    { "text", 1 }, { "fullscreen", 1 },
};
const char *tk_check_id(int i) { return i >= 0 && i < TK_CHECKS ? check_tab[i].id : NULL; }
int tk_check_weight(int i) { return i >= 0 && i < TK_CHECKS ? check_tab[i].w : 0; }

const char *tk_fatal_id(TkFatal f) {
    static const char *n[TK_F_N] = { "", "timeout", "crash", "protocol-error", "connection-lost", "egl", "vulkan",
                                     "platform-plugin", "no-result" };
    return f >= 0 && f < TK_F_N ? n[f] : "";
}

const char *tk_status_id(TkStatus s) {
    static const char *n[] = { "not-run", "ran", "fatal", "not-installed", "no-display", "no-harness" };
    return s <= TK_ST_NO_HARNESS ? n[s] : "not-run";
}

/* ---- the log: WAYLAND_DEBUG and stderr ---- */

static int global_index(const char *name, size_t n) {
    for (int i = 0; i < TB_EXPECTED_N; i++)
        if (strlen(tb_expected[i].iface) == n && !memcmp(tb_expected[i].iface, name, n)) return i;
    return -1;
}

void tk_log_init(TkLog *l) {
    memset(l, 0, sizeof *l);
    for (int i = 0; i < TK_OBJ_SLOTS; i++) l->obj[i].root = -2;     /* -2: empty slot */
}

static uint32_t slot_of(uint32_t id) { return (id * 2654435761u) & (TK_OBJ_SLOTS - 1); }

static void obj_set(TkLog *l, uint32_t id, int root) {
    if (!id) return;
    for (uint32_t k = 0, s = slot_of(id); k < 64; k++, s = (s + 1) & (TK_OBJ_SLOTS - 1)) {
        if (l->obj[s].root == -2 || l->obj[s].id == id) { l->obj[s].id = id; l->obj[s].root = (int16_t)root; return; }
    }
    /* full around here: that object stays unattributed */
}

static int obj_root(const TkLog *l, uint32_t id) {
    if (!id) return -1;
    for (uint32_t k = 0, s = slot_of(id); k < 64; k++, s = (s + 1) & (TK_OBJ_SLOTS - 1)) {
        if (l->obj[s].root == -2) return -1;
        if (l->obj[s].id == id) return l->obj[s].root;
    }
    return -1;
}

static int is_ident(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* "name#123" (or "name@123") at s (end e): the name's span and the id; returns the position after it, NULL if it isn't one */
static const char *obj_token(const char *s, const char *e, const char **name, size_t *nlen, uint32_t *id) {
    const char *p = s;
    if (p < e && *p == '[') {                       /* [unknown] */
        while (p < e && *p != ']') p++;
        if (p >= e) return NULL;
        p++;
    } else {
        while (p < e && is_ident(*p)) p++;
    }
    if (p == s || p >= e || (*p != '#' && *p != '@')) return NULL;      /* libwayland 1.23 prints '#', older '@' */
    *name = s;
    *nlen = (size_t)(p - s);
    p++;
    uint64_t v = 0;
    const char *d = p;
    while (p < e && *p >= '0' && *p <= '9' && v <= 0xffffffffu) v = v * 10 + (uint64_t)(*p++ - '0');
    if (p == d || v > 0xffffffffu) return NULL;
    *id = (uint32_t)v;
    return p;
}

static void attribute_error(TkLog *l, const char *name, size_t nlen, uint32_t id) {
    int g = obj_root(l, id);
    if (g < 0) g = global_index(name, nlen);
    if (g >= 0) l->perr[g] = 1;
    else l->unattributed++;
}

static uint32_t parse_u32(const char **pp, const char *e, int *ok) {
    const char *p = *pp;
    uint64_t v = 0;
    const char *d = p;
    while (p < e && *p >= '0' && *p <= '9' && v <= 0xffffffffu) v = v * 10 + (uint64_t)(*p++ - '0');
    *ok = p != d && v <= 0xffffffffu;
    *pp = p;
    return (uint32_t)v;
}

/* one WAYLAND_DEBUG line: "[1234.567] {Default Queue}  -> wl_registry#2.bind(1, "wl_seat", 9, new id [unknown]#5)" */
static void debug_line(TkLog *l, const char *s, const char *e) {
    const char *p = memchr(s, ']', (size_t)(e - s));
    if (!p) return;
    p++;
    while (p < e && *p == ' ') p++;
    if (p < e && *p == '{') {                       /* the queue's name */
        p = memchr(p, '}', (size_t)(e - p));
        if (!p) return;
        p++;
        while (p < e && *p == ' ') p++;
    }
    if (e - p >= 10 && !memcmp(p, "discarded ", 10)) return;     /* never delivered */
    int send = 0;
    if (e - p >= 3 && !memcmp(p, "-> ", 3)) { send = 1; p += 3; }
    const char *iface;
    size_t ilen;
    uint32_t self;
    p = obj_token(p, e, &iface, &ilen, &self);
    if (!p || p >= e || *p != '.') return;
    const char *m = ++p;
    while (p < e && is_ident(*p)) p++;
    size_t mlen = (size_t)(p - m);
    if (!mlen || p >= e || *p != '(') return;
    const char *a = p + 1, *ae = e;
    while (ae > a && ae[-1] != ')') ae--;
    if (ae <= a) return;
    ae--;                                           /* the closing ')' */

    if (send && ilen == 11 && !memcmp(iface, "wl_registry", 11) && mlen == 4 && !memcmp(m, "bind", 4)) {
        int ok;
        const char *q = a;
        parse_u32(&q, ae, &ok);
        if (!ok || ae - q < 3 || memcmp(q, ", \"", 3)) return;
        q += 3;
        const char *nm = q;
        while (q < ae && is_ident(*q)) q++;
        size_t nlen = (size_t)(q - nm);
        if (!nlen || nlen > 63 || q >= ae || *q != '"') return;
        q++;
        if (ae - q < 2 || memcmp(q, ", ", 2)) return;
        q += 2;
        uint32_t ver = parse_u32(&q, ae, &ok);
        if (!ok || !ver) return;
        int g = global_index(nm, nlen);
        if (g >= 0 && ver > l->bound[g]) l->bound[g] = ver;
        if (g < 0) l->extra++;
        const char *nid = q;
        if (ae - nid >= 9 && !memcmp(nid, ", new id ", 9)) {
            const char *cn;
            size_t cl;
            uint32_t cid;
            if (obj_token(nid + 9, ae, &cn, &cl, &cid)) obj_set(l, cid, g);
        }
        return;
    }
    if (!send && ilen == 10 && !memcmp(iface, "wl_display", 10) && mlen == 5 && !memcmp(m, "error", 5)) {
        const char *on;
        size_t ol;
        uint32_t oid;
        l->protocol_errors++;
        if (!l->fatal) l->fatal = TK_F_PROTOCOL;
        if (obj_token(a, ae, &on, &ol, &oid)) attribute_error(l, on, ol, oid);
        else l->unattributed++;
        return;
    }
    /* objects created here belong to the global this object came from (quoted strings skipped) */
    int root = obj_root(l, self);
    int inq = 0;
    for (const char *q = a; q < ae; q++) {
        if (*q == '"') { inq = !inq; continue; }
        if (inq || ae - q < 7 || memcmp(q, "new id ", 7)) continue;
        const char *cn;
        size_t cl;
        uint32_t cid;
        const char *after = obj_token(q + 7, ae, &cn, &cl, &cid);
        if (after) { obj_set(l, cid, root); q = after - 1; }
    }
}

static int is_debug_line(const char *s, const char *e) {
    if (s >= e || *s != '[') return 0;
    const char *p = s + 1;
    while (p < e && *p == ' ') p++;
    const char *d = p;
    while (p < e && *p >= '0' && *p <= '9') p++;
    if (p == d || p >= e || *p != '.') return 0;
    p++;
    d = p;
    while (p < e && *p >= '0' && *p <= '9') p++;
    return p > d && p < e && *p == ']';
}

/* case-insensitive substring */
static int has(const char *s, size_t n, const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= n; i++) {
        size_t j = 0;
        while (j < k) {
            char c = s[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != needle[j]) break;
            j++;
        }
        if (j == k) return 1;
    }
    return 0;
}

/* libwayland's own report of a protocol error: "xdg_surface#12: error 3: ..." ("@12" before 1.23) or "[destroyed object]: error 1: ..." */
static int wl_log_error(TkLog *l, const char *s, const char *e) {
    for (const char *p = s; p + 8 <= e; p++) {
        if (memcmp(p, ": error ", 8)) continue;
        if (p - s >= 18 && !memcmp(p - 18, "[destroyed object]", 18)) { l->unattributed++; return 1; }
        const char *d = p;
        while (d > s && d[-1] >= '0' && d[-1] <= '9') d--;
        if (d == p || d == s || (d[-1] != '#' && d[-1] != '@')) continue;
        const char *n = d - 1;
        while (n > s && is_ident(n[-1])) n--;
        const char *on;
        size_t ol;
        uint32_t oid;
        if (n < d - 1 && obj_token(n, p, &on, &ol, &oid)) attribute_error(l, on, ol, oid);
        return 1;
    }
    return 0;
}

static const struct { TkFatal f; const char *s; } fatal_text[] = {
    { TK_F_PROTOCOL, "protocol error" },
    { TK_F_LOST, "lost connection to wayland compositor" },
    { TK_F_LOST, "wayland connection broke" },
    { TK_F_LOST, "error reading events from display" },
    { TK_F_LOST, "error flushing display" },
    { TK_F_EGL, "egl_bad_" },
    { TK_F_EGL, "egl error" },
    { TK_F_EGL, "failed to initialize egl" },
    { TK_F_EGL, "failed to create egl" },
    { TK_F_EGL, "could not create the egl" },
    { TK_F_EGL, "could not initialize egl" },
    { TK_F_EGL, "eglinitialize failed" },
    { TK_F_VULKAN, "vk_error_device_lost" },
    { TK_F_VULKAN, "vk_error_out_of_device_memory" },
    { TK_F_VULKAN, "vk_error_surface_lost" },
    { TK_F_PLUGIN, "could not load the qt platform plugin" },
    { TK_F_PLUGIN, "no qt platform plugin could be initialized" },
};

void tk_log_line(TkLog *l, const char *s, size_t n) {
    const char *e = s + n;
    l->lines++;
    if (is_debug_line(s, e)) { debug_line(l, s, e); return; }
    if (wl_log_error(l, s, e)) {
        if (!l->fatal) l->fatal = TK_F_PROTOCOL;
        return;
    }
    if (l->fatal) return;
    for (size_t i = 0; i < sizeof fatal_text / sizeof fatal_text[0]; i++)
        if (has(s, n, fatal_text[i].s)) { l->fatal = fatal_text[i].f; return; }
}

void tk_log_feed(TkLog *l, const char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
            tk_log_line(l, l->line, l->llen);
            l->llen = 0;
            l->overlong = 0;
            continue;
        }
        if (l->overlong) continue;
        if (l->llen < sizeof l->line) { l->line[l->llen++] = c; continue; }
        l->overlong = 1;                            /* the first TK_LINE_MAX bytes are read when it ends */
        l->long_lines++;
    }
}

void tk_log_end(TkLog *l) {
    if (l->llen) tk_log_line(l, l->line, l->llen);
    l->llen = 0;
    l->overlong = 0;
}

/* ---- show mode: progress lines (toolkits.h) ---- */

int tk_progress_parse(const char *s, size_t n, TkProg *p) {
    static const char pre[] = TK_PROG_PREFIX;
    const size_t pl = sizeof pre - 1;
    memset(p, 0, sizeof *p);
    if (n < pl || n >= TK_PROG_LINE_MAX || memcmp(s, pre, pl)) return -1;
    const char *q = s + pl, *e = s + n;
    for (const char *c = q; c < e; c++)            /* only [a-z_] and single spaces */
        if (!((*c >= 'a' && *c <= 'z') || *c == '_' || (*c == ' ' && c > q && c[-1] != ' ' && c + 1 < e))) return -1;
    const char *w[4];
    size_t wl[4];
    int nw = 0;
    for (const char *c = q; c < e && nw < 4;) {
        const char *b = c;
        while (c < e && *c != ' ') c++;
        w[nw] = b;
        wl[nw++] = (size_t)(c - b);
        if (c < e) c++;
    }
    if (nw < 2 || nw > 3) return -1;
    int check = -1;
    for (int i = 0; i < TK_CHECKS; i++)
        if (strlen(check_tab[i].id) == wl[1] && !memcmp(check_tab[i].id, w[1], wl[1])) { check = i; break; }
    if (check < 0) return -1;
    if (nw == 2 && wl[0] == 4 && !memcmp(w[0], "step", 4)) {
        p->kind = TK_PROG_STEP;
        p->check = check;
        return 0;
    }
    if (nw == 3 && wl[0] == 5 && !memcmp(w[0], "check", 5)) {
        int r = wl[2] == 4 && !memcmp(w[2], "pass", 4) ? TK_PASS : wl[2] == 4 && !memcmp(w[2], "fail", 4) ? TK_FAIL
              : wl[2] == 4 && !memcmp(w[2], "skip", 4) ? TK_SKIP : -1;
        if (r < 0) return -1;
        p->kind = TK_PROG_CHECK;
        p->check = check;
        p->result = r;
        return 0;
    }
    memset(p, 0, sizeof *p);
    return -1;
}

void tk_progress_feed(TkProgBuf *b, const char *buf, size_t n, TkProgFn cb, void *ctx) {
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
            TkProg p;
            if (!b->overlong && b->events < TK_PROG_MAX_EVENTS && !tk_progress_parse(b->line, b->len, &p)) {
                b->events++;
                if (cb) cb(ctx, &p);
            }
            b->len = 0;
            b->overlong = 0;
            continue;
        }
        if (b->overlong) continue;
        if (b->len + 1 < sizeof b->line) b->line[b->len++] = c;
        else b->overlong = 1;                       /* too long for a progress line: the whole line is ignored */
    }
}

/* ---- the harness's line ---- */

void tk_result_init(TkResult *r, int combo) {
    memset(r, 0, sizeof *r);
    r->combo = combo;
    r->exit_code = -1;
    r->score = -1;
    for (int i = 0; i < TK_CHECKS; i++) r->checks[i].ms = -1;
}

static void keep_chars(char *out, size_t n, const char *s, const char *allowed_extra) {
    size_t k = 0;
    for (; s && *s && k + 1 < n; s++) {
        char c = *s;
        if (is_ident(c) || (allowed_extra && strchr(allowed_extra, c))) out[k++] = c;
        else break;                                 /* anything else ends it */
    }
    out[k] = 0;
}

/* free text from the harness: printable ASCII, no path (a '/' ends it), for the table only */
static void keep_detail(char *out, size_t n, const char *s) {
    size_t k = 0;
    for (; s && *s && k + 1 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '/') break;
        out[k++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
    }
    out[k] = 0;
}

static int take_line(TkResult *r, const char *s, size_t n) {
    const TkCombo *c = tk_combo(r->combo);
    JDoc d = { 0 };
    if (!c || json_parse(s, n, &d, NULL, 0) || !d.root || d.root->type != J_OBJ) { json_free(&d); return -1; }
    const char *tk = json_str(d.root, "toolkit");
    if (!tk || strcmp(tk, c->toolkit)) { json_free(&d); return -1; }
    const JVal *inst = json_get(d.root, "installed");
    const JVal *checks = json_get(d.root, "checks");
    int not_installed = inst && inst->type == J_BOOL && !inst->num;
    if (!not_installed && (!checks || checks->type != J_OBJ)) { json_free(&d); return -1; }
    r->json_ok = 1;
    keep_chars(r->version, sizeof r->version, json_str(d.root, "version"), ".+~-");
    const char *be = json_str(d.root, "backend");
    snprintf(r->backend_seen, sizeof r->backend_seen, "%s", be && (!strcmp(be, "wayland") || !strcmp(be, "x11")) ? be : "");
    if (not_installed) {
        r->status = TK_ST_NOT_INSTALLED;
        json_free(&d);
        return 0;
    }
    for (int i = 0; i < TK_CHECKS; i++) {
        TkCheck *k = &r->checks[i];
        memset(k, 0, sizeof *k);
        k->ms = -1;
        k->result = TK_SKIP;
        const JVal *v = json_get(checks, check_tab[i].id);
        if (!v) continue;
        if (v->type != J_OBJ) { k->result = TK_FAIL; continue; }       /* not what the harness promised */
        const JVal *ok = json_get(v, "ok");
        if (!ok || ok->type == J_NULL) k->result = TK_SKIP;
        else if (ok->type == J_BOOL) k->result = ok->num ? TK_PASS : TK_FAIL;
        else k->result = TK_FAIL;
        double ms = json_num(v, "ms", -1);
        if (isfinite(ms) && ms >= 0 && ms <= 600000) k->ms = (int)ms;
        keep_detail(k->detail, sizeof k->detail, json_str(v, "detail"));
    }
    json_free(&d);
    return 0;
}

int tk_parse_output(TkResult *r, const char *out, size_t n, int truncated) {
    /* the lines, last first: the harness prints its result last */
    size_t end = n;
    if (truncated) {                                /* the last, cut line is not a result */
        while (end > 0 && out[end - 1] != '\n') end--;
    }
    int found = 0;
    while (end > 0 && !found) {
        size_t e = end;
        while (e > 0 && (out[e - 1] == '\n' || out[e - 1] == '\r')) e--;
        size_t b = e;
        while (b > 0 && out[b - 1] != '\n') b--;
        if (e > b) {
            size_t s = b;
            while (s < e && (out[s] == ' ' || out[s] == '\t')) s++;
            if (s < e && out[s] == '{' && !take_line(r, out + s, e - s)) found = 1;
            else r->bad_lines++;
        }
        end = b;
    }
    return found ? 0 : -1;
}

/* ---- the score ---- */

void tk_score(TkResult *r) {
    const TkCombo *c = tk_combo(r->combo);
    r->points = r->total = r->skipped = 0;
    r->failed_globals = 0;
    r->score = -1;
    if (r->status != TK_ST_RAN && r->status != TK_ST_FATAL) return;
    for (int i = 0; i < TK_CHECKS; i++) {
        if (r->checks[i].result == TK_SKIP) { r->skipped++; continue; }
        r->total += check_tab[i].w;
        if (r->checks[i].result == TK_PASS) r->points += check_tab[i].w;
    }
    if (c && c->wayland && r->log) {
        for (int g = 0; g < TK_GLOBALS; g++) {
            if (!r->log->bound[g]) continue;
            r->total += TK_W_GLOBAL;
            if (r->log->perr[g]) r->failed_globals++;
            else r->points += TK_W_GLOBAL;
        }
    }
    if (r->status == TK_ST_FATAL) { r->points = 0; r->score = 0; return; }
    if (r->total) r->score = floor((double)r->points * 1000.0 / r->total) / 10.0;
}

void tk_finish(TkResult *r, int how, int code) {
    if (how == 1) r->exit_code = code;
    if (r->status == TK_ST_NOT_INSTALLED) { tk_score(r); return; }
    r->fatal = TK_F_NONE;
    if (how == 3) r->fatal = TK_F_TIMEOUT;
    else if (how == 2) { r->fatal = TK_F_CRASH; r->signal_no = code; }
    else if (r->log && r->log->fatal) r->fatal = r->log->fatal;
    else if (!r->json_ok) r->fatal = TK_F_NO_RESULT;
    if (r->fatal == TK_F_PROTOCOL && r->log && !r->log->protocol_errors) r->log->protocol_errors = 1;
    r->status = r->fatal ? TK_ST_FATAL : TK_ST_RAN;
    tk_score(r);
}

void tk_report_global(TkReport *rep) {
    double sum = 0;
    rep->ran = 0;
    for (int i = 0; i < rep->n; i++) {
        const TkResult *r = &rep->r[i];
        if ((r->status == TK_ST_RAN || r->status == TK_ST_FATAL) && r->score >= 0) { sum += r->score; rep->ran++; }
    }
    rep->global = rep->ran ? floor(sum * 10.0 / rep->ran) / 10.0 : -1;
}

static double run_score(const TkResult *r) {
    if (!r) return -1;
    if (r->status == TK_ST_FATAL) return 0;
    if (r->status == TK_ST_RAN) return r->score;
    return -1;
}

const char *tk_recommend(const TkResult *wl, const TkResult *x) {
    static char buf[64];
    if (!wl || !x) return NULL;
    const TkCombo *cx = tk_combo(x->combo), *cw = tk_combo(wl->combo);
    if (!cx || !cw || !cx->env_var || !cw->wayland || cx->wayland || strcmp(cx->toolkit, cw->toolkit)) return NULL;
    double sw = run_score(wl), sx = run_score(x);
    if (sw < 0 || x->status != TK_ST_RAN || sx < 50 || sx - sw < 20) return NULL;
    snprintf(buf, sizeof buf, "%s=%s", cx->env_var, cx->backend);
    return buf;
}

/* the X11 run of a Wayland combination's toolkit in the report, or NULL */
static const TkResult *partner(const TkReport *rep, const TkResult *wl) {
    const TkCombo *cw = tk_combo(wl->combo);
    for (int i = 0; i < rep->n; i++) {
        const TkCombo *c = tk_combo(rep->r[i].combo);
        if (c && cw && !c->wayland && c->env_var && !strcmp(c->toolkit, cw->toolkit)) return &rep->r[i];
    }
    return NULL;
}

/* ---- JSON ---- */

static void json_score(FILE *o, double s) {
    if (s < 0) fputs("null", o);
    else fprintf(o, "%.1f", s);
}

void tk_report_json(FILE *o, const TkReport *rep) {
    fprintf(o, "{\"tool\":\"lordos-testbench\",\"v\":1,\"mode\":\"toolkits\",\"at\":%lld,\"source\":\"docs/wayland-protocols.json\",\"expected\":%d,",
            rep->at_ms, TK_GLOBALS);
    fputs("\"weights\":{", o);
    for (int i = 0; i < TK_CHECKS; i++) fprintf(o, "\"%s\":%d,", check_tab[i].id, check_tab[i].w);
    fprintf(o, "\"global\":%d},\"score\":", TK_W_GLOBAL);
    json_score(o, rep->global);
    fprintf(o, ",\"ran\":%d,\"combos\":[", rep->ran);
    for (int i = 0; i < rep->n; i++) {
        const TkResult *r = &rep->r[i];
        const TkCombo *c = tk_combo(r->combo);
        fprintf(o, "%s{\"id\":\"%s\",\"toolkit\":\"%s\",\"backend\":\"%s\",\"status\":\"%s\",\"fatal\":", i ? "," : "", c->id,
                c->toolkit, c->backend, tk_status_id(r->status));
        if (r->fatal) fprintf(o, "\"%s\"", tk_fatal_id(r->fatal));
        else fputs("null", o);
        if (r->fatal == TK_F_CRASH) fprintf(o, ",\"signal\":%d", r->signal_no);
        fprintf(o, ",\"version\":\"%s\",\"backendSeen\":\"%s\",\"exit\":%d,\"ms\":%d,\"score\":", r->version, r->backend_seen,
                r->exit_code, r->elapsed_ms);
        json_score(o, r->score);
        fprintf(o, ",\"points\":%d,\"total\":%d,\"skipped\":%d,\"checks\":[", r->points, r->total, r->skipped);
        int any = 0;
        if (r->status == TK_ST_RAN || r->status == TK_ST_FATAL) {
            static const char *res[] = { "skip", "pass", "fail" };
            for (int k = 0; k < TK_CHECKS; k++) {
                fprintf(o, "%s{\"id\":\"%s\",\"weight\":%d,\"result\":\"%s\"", any++ ? "," : "", check_tab[k].id, check_tab[k].w,
                        res[r->checks[k].result % 3]);
                if (r->checks[k].ms >= 0) fprintf(o, ",\"ms\":%d", r->checks[k].ms);
                fputc('}', o);
            }
            if (c->wayland && r->log)
                for (int g = 0; g < TK_GLOBALS; g++)
                    if (r->log->bound[g])
                        fprintf(o, ",{\"id\":\"global.%s\",\"weight\":%d,\"result\":\"%s\",\"version\":%u}", tb_expected[g].iface,
                                TK_W_GLOBAL, r->log->perr[g] ? "fail" : "pass", r->log->bound[g]);
        }
        fputs("]", o);
        if (c->wayland && r->log && (r->status == TK_ST_RAN || r->status == TK_ST_FATAL)) {
            int bound = 0;
            for (int g = 0; g < TK_GLOBALS; g++) bound += r->log->bound[g] != 0;
            fprintf(o, ",\"protocol\":{\"bound\":%d,\"failed\":%d,\"extra\":%d,\"errors\":%d,\"unattributed\":%d}", bound,
                    r->failed_globals, r->log->extra, r->log->protocol_errors, r->log->unattributed);
        }
        const char *rec = c->wayland ? tk_recommend(r, partner(rep, r)) : NULL;
        if (rec) fprintf(o, ",\"recommend\":\"%s\"", rec);
        else fputs(",\"recommend\":null", o);
        fputc('}', o);
    }
    /* toolkit × protocol: one row per Wayland combination that ran, one cell per expected global */
    fputs("],\"matrix\":{\"globals\":[", o);
    for (int g = 0; g < TK_GLOBALS; g++) fprintf(o, "%s\"%s\"", g ? "," : "", tb_expected[g].iface);
    fputs("],\"rows\":[", o);
    int nr = 0;
    for (int i = 0; i < rep->n; i++) {
        const TkResult *r = &rep->r[i];
        const TkCombo *c = tk_combo(r->combo);
        if (!c->wayland || !r->log || (r->status != TK_ST_RAN && r->status != TK_ST_FATAL)) continue;
        fprintf(o, "%s{\"combo\":\"%s\",\"cells\":[", nr++ ? "," : "", c->id);
        for (int g = 0; g < TK_GLOBALS; g++)
            fprintf(o, "%s%lld", g ? "," : "", r->log->perr[g] ? -(long long)(r->log->bound[g] ? r->log->bound[g] : 1) : (long long)r->log->bound[g]);
        fputs("]}", o);
    }
    fputs("]}}\n", o);
}

/* ---- the table ---- */

void tk_fill(const char *tmpl, const char *const *kv, char *out, size_t n) {
    size_t k = 0;
    if (!n) return;
    for (const char *p = tmpl; *p && k + 1 < n;) {
        if (*p == '{') {
            const char *e = strchr(p, '}');
            int done = 0;
            if (e) {
                for (int i = 0; kv && kv[i] && kv[i + 1]; i += 2) {
                    size_t l = strlen(kv[i]);
                    if ((size_t)(e - p - 1) == l && !memcmp(p + 1, kv[i], l)) {
                        for (const char *v = kv[i + 1]; *v && k + 1 < n; v++) out[k++] = *v;
                        p = e + 1;
                        done = 1;
                        break;
                    }
                }
            }
            if (done) continue;
        }
        out[k++] = *p++;
    }
    out[k] = 0;
}

static const char *say(TkText T, const char *key, ...) {
    static char out[4][1024];
    static int slot;
    const char *kv[13];
    int n = 0;
    va_list ap;
    va_start(ap, key);
    while (n < 12) {
        const char *k = va_arg(ap, const char *);
        if (!k) break;
        kv[n++] = k;
        kv[n++] = va_arg(ap, const char *);
    }
    va_end(ap);
    kv[n] = NULL;
    char *o = out[slot++ & 3];
    tk_fill(T(key), kv, o, sizeof out[0]);
    return o;
}

/* display columns of UTF-8 text (one per code point: the catalogs' scripts are close enough for padding) */
static int cols(const char *s) {
    int c = 0;
    for (; *s; s++) c += ((unsigned char)*s & 0xC0) != 0x80;
    return c;
}
static void pad(FILE *o, const char *s, int w) {
    fputs(s, o);
    for (int k = cols(s); k < w; k++) fputc(' ', o);
}

static const char *fatal_text_key(TkFatal f) {
    switch (f) {
    case TK_F_TIMEOUT: return "fatal.timeout";
    case TK_F_CRASH: return "fatal.crash";
    case TK_F_PROTOCOL: return "fatal.protocol";
    case TK_F_LOST: return "fatal.lost";
    case TK_F_EGL: return "fatal.egl";
    case TK_F_VULKAN: return "fatal.vulkan";
    case TK_F_PLUGIN: return "fatal.plugin";
    default: return "fatal.noResult";
    }
}

void tk_report_table(FILE *o, const TkReport *rep, TkText T) {
    char num[32];
    fprintf(o, "%s\n\n", T("title"));
    pad(o, T("col.combo"), 15);
    fputc(' ', o);
    pad(o, T("col.version"), 12);
    fputc(' ', o);
    pad(o, T("col.score"), 9);
    fprintf(o, " %s\n", T("col.failed"));
    for (int i = 0; i < rep->n; i++) {
        const TkResult *r = &rep->r[i];
        const TkCombo *c = tk_combo(r->combo);
        pad(o, c->id, 15);
        fputc(' ', o);
        pad(o, r->version[0] ? r->version : "-", 12);
        fputc(' ', o);
        if (r->status == TK_ST_RAN || r->status == TK_ST_FATAL) {
            if (r->score >= 0) snprintf(num, sizeof num, "%5.1f %%", r->score);
            else snprintf(num, sizeof num, "    - %%");
            pad(o, num, 9);
            fputc(' ', o);
            if (r->status == TK_ST_FATAL) {
                char sig[16];
                snprintf(sig, sizeof sig, "%d", r->signal_no);
                fputs(say(T, fatal_text_key(r->fatal), "signal", sig, NULL), o);
            } else {
                int any = 0;
                for (int k = 0; k < TK_CHECKS; k++)
                    if (r->checks[k].result == TK_FAIL) fprintf(o, "%s%s", any++ ? ", " : "", check_tab[k].id);
                if (c->wayland && r->log)
                    for (int g = 0; g < TK_GLOBALS; g++)
                        if (r->log->bound[g] && r->log->perr[g]) fprintf(o, "%sglobal.%s", any++ ? ", " : "", tb_expected[g].iface);
                if (!any) fputs(T("none"), o);
            }
        } else {
            pad(o, "", 9);
            fputc(' ', o);
            fputs(T(r->status == TK_ST_NOT_INSTALLED ? "status.notInstalled" : r->status == TK_ST_NO_DISPLAY ? "status.noDisplay"
                    : r->status == TK_ST_NO_HARNESS ? "status.noHarness" : "status.notRun"), o);
        }
        fputc('\n', o);
    }
    /* what the harnesses said about their failed checks */
    int head = 0;
    for (int i = 0; i < rep->n; i++) {
        const TkResult *r = &rep->r[i];
        if (r->status != TK_ST_RAN) continue;
        for (int k = 0; k < TK_CHECKS; k++) {
            if (r->checks[k].result != TK_FAIL || !r->checks[k].detail[0]) continue;
            if (!head++) fprintf(o, "\n%s\n", T("details"));
            fprintf(o, "  %s %s: %s\n", tk_combo(r->combo)->id, check_tab[k].id, r->checks[k].detail);
        }
    }
    fputc('\n', o);
    if (rep->ran) {
        char s[16], ran[16], tot[16];
        snprintf(s, sizeof s, "%.1f", rep->global);
        snprintf(ran, sizeof ran, "%d", rep->ran);
        snprintf(tot, sizeof tot, "%d", rep->n);
        fprintf(o, "%s\n", say(T, "global", "score", s, "ran", ran, "total", tot, NULL));
    } else {
        fprintf(o, "%s\n", T("noneRan"));
    }
    for (int i = 0; i < rep->n; i++) {
        const TkResult *r = &rep->r[i];
        if (!tk_combo(r->combo)->wayland) continue;
        const TkResult *x = partner(rep, r);
        const char *rec = tk_recommend(r, x);
        if (!rec) continue;
        char sw[16], sx[16];
        snprintf(sw, sizeof sw, "%.1f", run_score(r));
        snprintf(sx, sizeof sx, "%.1f", run_score(x));
        fprintf(o, "%s\n", say(T, "recommend", "toolkit", tk_combo(r->combo)->label, "wayland", sw, "x11", sx, "env", rec, NULL));
    }
    /* the matrix: globals any Wayland run bound × those runs */
    int cols_n = 0, idx[TK_COMBOS];
    for (int i = 0; i < rep->n; i++) {
        const TkResult *r = &rep->r[i];
        if (tk_combo(r->combo)->wayland && r->log && (r->status == TK_ST_RAN || r->status == TK_ST_FATAL)) idx[cols_n++] = i;
    }
    if (!cols_n) return;
    fprintf(o, "\n%s\n", T("matrix"));
    pad(o, "", 42);
    for (int j = 0; j < cols_n; j++) fprintf(o, " %-13s", tk_combo(rep->r[idx[j]].combo)->id);
    fputc('\n', o);
    for (int g = 0; g < TK_GLOBALS; g++) {
        int any = 0;
        for (int j = 0; j < cols_n; j++) any |= rep->r[idx[j]].log->bound[g] != 0;
        if (!any) continue;
        pad(o, tb_expected[g].iface, 42);
        for (int j = 0; j < cols_n; j++) {
            const TkLog *l = rep->r[idx[j]].log;
            if (l->bound[g]) snprintf(num, sizeof num, "v%u%s", l->bound[g], l->perr[g] ? " !" : "");
            else snprintf(num, sizeof num, "-");
            fprintf(o, " %-13s", num);
        }
        fputc('\n', o);
    }
}

/* ---- the saved result, summed up (--toolkits --last, for lordos-issue) ---- */

/* The file is the user's and may have been edited by hand: every value printed is checked against the fixed lists
 * first (ids, statuses, fatal words, check ids, a version of [0-9A-Za-z.+~-]), and anything else makes the whole
 * file "unreadable". Only check ids, numbers and those words are printed; never a path or a title. */
static int is_word_in(const char *s, const char *const *list, int n) {
    for (int i = 0; s && i < n; i++) if (!strcmp(s, list[i])) return i;
    return -1;
}

static int version_ok(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n > 31) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '+' || c == '~' || c == '-')) return 0;
    }
    return 1;
}

/* a shared check id, or "global." and one of the table's interfaces */
static int check_id_ok(const char *s) {
    if (!s) return 0;
    for (int i = 0; i < TK_CHECKS; i++) if (!strcmp(s, check_tab[i].id)) return 1;
    return !strncmp(s, "global.", 7) && global_index(s + 7, strlen(s + 7)) >= 0;
}

static int recommend_ok(const char *s) {
    static const char *const ok[] = { "GDK_BACKEND=x11", "GDK_BACKEND=wayland", "QT_QPA_PLATFORM=xcb", "QT_QPA_PLATFORM=wayland" };
    return is_word_in(s, ok, 4) >= 0;
}

/* a score: null or 0..100; a missing one counts as null */
static int pct_ok(const JVal *v) { return !v || v->type == J_NULL || (v->type == J_NUM && v->num >= 0 && v->num <= 100); }

int tk_last_summary(FILE *o, const char *json, size_t len, long long now_ms) {
    static const char *const status[] = { "ran", "fatal", "not-installed", "no-display", "no-harness", "not-run" };
    static const char *const fatal[] = { "timeout", "crash", "protocol-error", "connection-lost", "egl", "vulkan",
                                         "platform-plugin", "no-result" };
    JDoc doc;
    if (!json || !len || len > TK_LAST_MAX || json_parse(json, len, &doc, NULL, 0)) {
        fputs("the saved result is unreadable\n", o);
        return 1;
    }
    const JVal *r = doc.root, *combos = json_get(r, "combos");
    const JVal *at = json_get(r, "at"), *score = json_get(r, "score");
    int ok = r && r->type == J_OBJ && json_str(r, "tool") && !strcmp(json_str(r, "tool"), "lordos-testbench")
          && json_str(r, "mode") && !strcmp(json_str(r, "mode"), "toolkits") && json_num(r, "v", 0) == 1
          && at && at->type == J_NUM && at->num > 0 && at->num < 1e15 && score && pct_ok(score)
          && combos && combos->type == J_ARR && combos->len <= TK_COMBOS;
    /* every row first, then print */
    for (const JVal *c = ok ? combos->kid : NULL; c && ok; c = c->next) {
        const char *id = json_str(c, "id"), *st = json_str(c, "status");
        const JVal *f = json_get(c, "fatal"), *ch = json_get(c, "checks"), *rec = json_get(c, "recommend");
        const JVal *sig = json_get(c, "signal");
        ok = c->type == J_OBJ && id && tk_combo_find(id, strlen(id)) >= 0 && is_word_in(st, status, 6) >= 0
          && f && (f->type == J_NULL || (f->type == J_STR && is_word_in(f->str, fatal, 8) >= 0))
          && (!json_get(c, "version") || version_ok(json_str(c, "version"))) && pct_ok(json_get(c, "score"))
          && (!rec || rec->type == J_NULL || (rec->type == J_STR && recommend_ok(rec->str)))
          && (!sig || (sig->type == J_NUM && sig->num >= 0 && sig->num < 128))
          && (!ch || (ch->type == J_ARR && ch->len <= TK_CHECKS + TK_GLOBALS));
        for (const JVal *k = ok && ch ? ch->kid : NULL; k && ok; k = k->next) {
            const char *res = json_str(k, "result");
            ok = k->type == J_OBJ && check_id_ok(json_str(k, "id")) && res && (!strcmp(res, "pass") || !strcmp(res, "fail") || !strcmp(res, "skip"));
        }
    }
    if (!ok) {
        fputs("the saved result is unreadable\n", o);
        json_free(&doc);
        return 1;
    }
    long long age = now_ms > (long long)at->num ? (now_ms - (long long)at->num) / 1000 : 0;
    if (age < 3600) fprintf(o, "saved %lld minutes ago\n", age / 60);
    else if (age < 172800) fprintf(o, "saved %lld hours ago\n", age / 3600);
    else fprintf(o, "saved %lld days ago\n", age / 86400);
    double ran = json_num(r, "ran", 0);
    if (score->type == J_NUM) fprintf(o, "global score: %.1f %% (%d of %d combinations ran)\n", score->num, ran >= 0 && ran <= TK_COMBOS ? (int)ran : 0, (int)combos->len);
    else fputs("global score: none (no combination ran)\n", o);
    for (const JVal *c = combos->kid; c; c = c->next) {
        const char *id = json_str(c, "id"), *st = json_str(c, "status"), *ver = json_str(c, "version");
        const JVal *f = json_get(c, "fatal"), *sc = json_get(c, "score"), *ch = json_get(c, "checks"), *rec = json_get(c, "recommend");
        fprintf(o, "  %-14s ", id);
        if (!strcmp(st, "ran") && sc && sc->type == J_NUM) fprintf(o, "%5.1f %%", sc->num);
        else if (!strcmp(st, "fatal")) {
            fprintf(o, "  0.0 %%  %s", f->type == J_STR ? f->str : "fatal");
            if (f->type == J_STR && !strcmp(f->str, "crash")) fprintf(o, " (signal %d)", (int)json_num(c, "signal", 0));
        } else fputs(st, o);
        if (ver && *ver) fprintf(o, "  %s", ver);
        int any = 0;
        for (const JVal *k = ch ? ch->kid : NULL; k; k = k->next)
            if (!strcmp(json_str(k, "result"), "fail")) fprintf(o, "%s%s", any++ ? ", " : "  failed: ", json_str(k, "id"));
        if (rec && rec->type == J_STR) fprintf(o, "  recommended: %s", rec->str);
        fputc('\n', o);
    }
    json_free(&doc);
    return 0;
}
