/* toolkit_audit_test.c — `lordos-testbench --toolkits` (src/testbench/toolkits.c, tkrun.c; docs/TOOLKIT-AUDIT.md):
 * the scoring, the WAYLAND_DEBUG and stderr reader, the harness's JSON line with hostile output (oversized lines,
 * bad JSON, control characters), the environment a harness gets, the cache file, and real runs of fake harnesses
 * (shell scripts behind a fake python3 written into a temporary folder: fixed JSON, a protocol error, a crash, a
 * hang killed at the deadline with a child left behind, missing bindings). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../src/testbench/toolkits.h"
#include "../src/testbench/tkrun.h"
#include "../src/sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int gidx(const char *iface) {
    /* the matrix's order is expected_globals.h's: find it through the JSON's "globals" */
    static const char *names[TK_GLOBALS];
    static int loaded;
    if (!loaded) {
        TkReport rep;
        memset(&rep, 0, sizeof rep);
        char *buf = NULL;
        size_t len = 0;
        FILE *f = open_memstream(&buf, &len);
        tk_report_json(f, &rep);
        fclose(f);
        JDoc d = { 0 };
        if (!json_parse(buf, len, &d, NULL, 0)) {
            const JVal *g = json_get(json_get(d.root, "matrix"), "globals");
            int i = 0;
            for (const JVal *v = g ? g->kid : NULL; v && i < TK_GLOBALS; v = v->next) names[i++] = strdup(v->str);
        }
        json_free(&d);
        free(buf);
        loaded = 1;
    }
    for (int i = 0; i < TK_GLOBALS; i++) if (names[i] && !strcmp(names[i], iface)) return i;
    return -1;
}

static void feed(TkLog *l, const char *s) { tk_log_feed(l, s, strlen(s)); }

static int combo(const char *id) { return tk_combo_find(id, strlen(id)); }

/* a harness's line with every check passing except the ones listed as "id=false" or "id=null" */
static const char *line_for(const char *toolkit, const char *over) {
    static char out[4096];
    size_t k = (size_t)snprintf(out, sizeof out, "{\"toolkit\":\"%s\",\"version\":\"1.2.3\",\"backend\":\"wayland\",\"checks\":{", toolkit);
    for (int i = 0; i < TK_CHECKS; i++) {
        const char *id = tk_check_id(i), *val = "true";
        char pat[48];
        snprintf(pat, sizeof pat, "%s=false", id);
        if (over && strstr(over, pat)) val = "false";
        snprintf(pat, sizeof pat, "%s=null", id);
        if (over && strstr(over, pat)) val = "null";
        k += (size_t)snprintf(out + k, sizeof out - k, "%s\"%s\":{\"ok\":%s,\"ms\":%d,\"detail\":\"d%d\"}", i ? "," : "", id, val, 10 * i, i);
    }
    snprintf(out + k, sizeof out - k, "}}\n");
    return out;
}

/* ---- the log ---- */

static void test_log(void) {
    TkLog *l = malloc(sizeof *l);
    tk_log_init(l);
    feed(l, "[1234567.890] {Default Queue}  -> wl_display#1.get_registry(new id wl_registry#2)\n"
            "[1234567.891] {Default Queue} wl_registry#2.global(1, \"wl_compositor\", 6)\n"
            "[1234567.892] {Default Queue}  -> wl_registry#2.bind(1, \"wl_compositor\", 6, new id [unknown]#3)\n"
            "[1234567.893]  -> wl_registry#2.bind(2, \"xdg_wm_base\", 5, new id [unknown]#4)\n"
            "[1234567.894] {Default Queue}  -> wl_registry#2.bind(3, \"wl_output\", 2, new id [unknown]#5)\n"
            "[1234567.895] {Default Queue}  -> wl_registry#2.bind(9, \"wl_output\", 4, new id [unknown]#6)\n"
            "[1234567.896] {Default Queue}  -> wl_registry#2.bind(4, \"foo_private_v1\", 1, new id [unknown]#7)\n"
            "[1234567.897] {Default Queue} discarded  -> wl_registry#2.bind(5, \"wl_seat\", 9, new id [unknown]#8)\n"
            "[1234567.898] {Default Queue}  -> wl_registry#2.bind(6, \"wl_shm\", 99999999999, new id [unknown]#9)\n"
            "[1234567.899] {Default Queue}  -> wl_registry#2.bind(7, \"wl seat\", 9, new id [unknown]#10)\n"
            "[1234567.900] {Default Queue}  -> wl_registry#2.bind(8, \"wp_viewporter\", 0, new id [unknown]#11)\n"
            "[1234567.901] {Default Queue}  -> wl_compositor#3.create_surface(new id wl_surface#12)\n"
            "[1234567.902] {Default Queue}  -> xdg_wm_base#4.get_xdg_surface(new id xdg_surface#13, wl_surface#12)\n"
            "[1234567.903] {Default Queue}  -> xdg_surface#13.get_toplevel(new id xdg_toplevel#14)\n"
            "[1234567.904] {Default Queue}  -> wl_surface#12.fake_text(\"x new id wl_surface#14 y\")\n");
    int comp = gidx("wl_compositor"), wm = gidx("xdg_wm_base"), out = gidx("wl_output"), seat = gidx("wl_seat"),
        shm = gidx("wl_shm"), vp = gidx("wp_viewporter");
    CHECK(comp >= 0 && wm >= 0 && out >= 0 && seat >= 0 && shm >= 0 && vp >= 0);
    CHECK(l->bound[comp] == 6);
    CHECK(l->bound[wm] == 5);
    CHECK(l->bound[out] == 4);              /* the highest of two binds */
    CHECK(l->bound[seat] == 0);             /* discarded, and the name with a space never counts */
    CHECK(l->bound[shm] == 0);              /* a version that overflows */
    CHECK(l->bound[vp] == 0);               /* version 0 */
    CHECK(l->extra == 1);
    CHECK(l->fatal == TK_F_NONE);
    /* a protocol error on the toplevel goes to xdg_wm_base, through the objects it created (a fake
     * "new id" inside a quoted string on a wl_compositor object is ignored, so xdg_toplevel#14 stays xdg_wm_base's) */
    feed(l, "[1234567.950] {Display Queue} wl_display#1.error(xdg_toplevel#14, 2, \"invalid size\")\n"
            "xdg_toplevel#14: error 2: invalid size\n");
    CHECK(l->perr[wm] == 1);
    CHECK(l->perr[comp] == 0);
    CHECK(l->protocol_errors == 1);
    CHECK(l->fatal == TK_F_PROTOCOL);
    CHECK(l->unattributed == 0);
    free(l);

    /* chunking: byte by byte gives the same */
    l = malloc(sizeof *l);
    tk_log_init(l);
    const char *s = "[ 12.5]  -> wl_registry#2.bind(1, \"wl_seat\", 7, new id wl_seat#3)\nGdk-Message: hello\n";
    for (size_t i = 0; s[i]; i++) tk_log_feed(l, s + i, 1);
    CHECK(l->bound[seat] == 7);
    CHECK(l->lines == 2);
    free(l);

    /* libwayland before 1.23: '@' instead of '#', no queue name */
    l = malloc(sizeof *l);
    tk_log_init(l);
    feed(l, "[3266187.290]  -> wl_registry@2.bind(5, \"zxdg_output_manager_v1\", 3, new id [unknown]@9)\n"
            "[3266187.291]  -> zxdg_output_manager_v1@9.get_xdg_output(new id zxdg_output_v1@10, wl_output@4)\n"
            "[3266187.300] wl_display@1.error(zxdg_output_v1@10, 0, \"x\")\n");
    CHECK(l->bound[gidx("zxdg_output_manager_v1")] == 3 && l->perr[gidx("zxdg_output_manager_v1")] == 1);
    free(l);

    /* the fatal texts, in non-debug lines only; warnings are not fatal */
    static const struct { const char *line; TkFatal f; } fx[] = {
        { "Gdk-Message: 12:00:00.000: Error 71 (Protocol error) dispatching to Wayland display.", TK_F_PROTOCOL },
        { "Gdk-Message: Lost connection to Wayland compositor.", TK_F_LOST },
        { "The Wayland connection broke. Did the Wayland compositor die?", TK_F_LOST },
        { "EGL_BAD_ALLOC while creating the surface", TK_F_EGL },
        { "qt.qpa.wayland: EGL Error : Could not create the egl surface: error = 0x300b", TK_F_EGL },
        { "VK_ERROR_DEVICE_LOST", TK_F_VULKAN },
        { "qt.qpa.plugin: Could not load the Qt platform plugin \"wayland\" in \"\" even though it was found.", TK_F_PLUGIN },
        { "[destroyed object]: error 1: invalid object", TK_F_PROTOCOL },
        { "libEGL warning: DRI3: failed to query the version", TK_F_NONE },
        { "Gtk-WARNING **: no trigger event for menu popup", TK_F_NONE },
        { "MESA: VK_ERROR_INCOMPATIBLE_DRIVER, falling back to GL", TK_F_NONE },
        { "[1.2] -> xdg_toplevel#9.set_title(\"Protocol error EGL_BAD_ALLOC\")", TK_F_NONE },   /* a debug line's text */
    };
    for (size_t i = 0; i < sizeof fx / sizeof fx[0]; i++) {
        l = malloc(sizeof *l);
        tk_log_init(l);
        tk_log_line(l, fx[i].line, strlen(fx[i].line));
        if (l->fatal != fx[i].f) fprintf(stderr, "fatal text %zu: got %d\n", i, l->fatal);
        CHECK(l->fatal == fx[i].f);
        free(l);
    }

    /* an oversized line: only its first TK_LINE_MAX bytes are read, and the next line is whole */
    l = malloc(sizeof *l);
    tk_log_init(l);
    char *big = malloc(TK_LINE_MAX * 3);
    memset(big, 'a', TK_LINE_MAX * 3);
    tk_log_feed(l, big, TK_LINE_MAX * 3);
    feed(l, "EGL_BAD_ALLOC\n");                     /* beyond the limit, part of the long line */
    CHECK(l->fatal == TK_F_NONE);
    CHECK(l->long_lines == 1);
    feed(l, "Gdk-Message: Lost connection to Wayland compositor.\n");
    CHECK(l->fatal == TK_F_LOST);
    /* control characters and invalid UTF-8 in the log are just bytes */
    feed(l, "\x01\x02\xff\xfe[\x1b[31m]\n[1.0]  -> wl_registry#2.bind(\n[1.0]  -> #.(\n");
    tk_log_end(l);
    free(big);
    free(l);

    /* object ids reused and many objects: the table never overflows */
    l = malloc(sizeof *l);
    tk_log_init(l);
    feed(l, "[1.0]  -> wl_registry#2.bind(1, \"wl_compositor\", 6, new id [unknown]#3)\n");
    char ln[160];
    for (unsigned i = 0; i < 30000; i++) {
        snprintf(ln, sizeof ln, "[1.0]  -> wl_compositor#3.create_surface(new id wl_surface#%u)\n", 100 + i);
        feed(l, ln);
    }
    feed(l, "[1.0] wl_display#1.error(wl_surface#120, 2, \"x\")\n");
    CHECK(l->perr[comp] == 1);
    free(l);
}

/* ---- the harness's line ---- */

static void test_parse(void) {
    TkResult r;
    int c = combo("gtk4-wayland");
    CHECK(c >= 0);

    tk_result_init(&r, c);
    const char *ok = line_for("gtk4", "dnd=null theme=false");
    CHECK(!tk_parse_output(&r, ok, strlen(ok), 0));
    CHECK(r.json_ok && !strcmp(r.version, "1.2.3") && !strcmp(r.backend_seen, "wayland"));
    CHECK(r.checks[0].result == TK_PASS && r.checks[0].ms == 0);
    for (int i = 0; i < TK_CHECKS; i++) {
        if (!strcmp(tk_check_id(i), "dnd")) CHECK(r.checks[i].result == TK_SKIP);
        if (!strcmp(tk_check_id(i), "theme")) CHECK(r.checks[i].result == TK_FAIL);
    }

    /* another toolkit's line, bad JSON, raw control characters, deep nesting and duplicate keys are not results;
     * the last valid line wins */
    char buf[8192];
    snprintf(buf, sizeof buf, "Qt says hello\n{\"toolkit\":\"qt5\",\"checks\":{}}\n{\"toolkit\":\"gtk4\",\"checks\":{\"map\":{\"ok\":tru}}}\n"
             "{\"toolkit\":\"gtk4\",\"checks\":{\"map\":{\"ok\":true,\"detail\":\"a\x01" "b\"}}}\n"
             "{\"toolkit\":\"gtk4\",\"toolkit\":\"gtk4\",\"checks\":{}}\n"
             "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]\n");
    tk_result_init(&r, c);
    CHECK(tk_parse_output(&r, buf, strlen(buf), 0) == -1);
    CHECK(!r.json_ok);
    CHECK(r.bad_lines == 6);

    /* hostile values: a version with shell characters, an unknown backend, "ok" of the wrong type, odd ms, a detail
     * with control characters and a path */
    const char *h = "{\"toolkit\":\"gtk4\",\"version\":\"4.1;rm -rf\",\"backend\":\"evil\",\"checks\":{"
                    "\"map\":{\"ok\":\"yes\"},\"size\":{\"ok\":true,\"ms\":-5},\"scale\":{\"ok\":true,\"ms\":1e300},"
                    "\"theme\":{\"ok\":false,\"detail\":\"bad\\u0001x\\u001b[31m /home/someone/secret\"},\"backend\":7,"
                    "\"unknown_id\":{\"ok\":true}}}";
    tk_result_init(&r, c);
    CHECK(!tk_parse_output(&r, h, strlen(h), 0));
    CHECK(!strcmp(r.version, "4.1"));
    CHECK(!strcmp(r.backend_seen, ""));
    CHECK(r.checks[0].result == TK_FAIL);   /* map: "yes" isn't a boolean */
    CHECK(r.checks[1].result == TK_PASS && r.checks[1].ms == -1);
    CHECK(r.checks[2].ms == -1);
    CHECK(!strcmp(r.checks[3].detail, "bad?x?[31m "));
    CHECK(r.checks[4].result == TK_FAIL);   /* backend: not an object */
    CHECK(r.checks[5].result == TK_SKIP);   /* decoration: missing */

    /* an oversized stdout: the cut last line is dropped, the result before it is kept */
    size_t n = TK_OUT_MAX;
    char *big = malloc(n);
    size_t k = (size_t)snprintf(big, n, "%s", line_for("gtk4", NULL));
    memset(big + k, 'x', n - k);
    tk_result_init(&r, c);
    CHECK(!tk_parse_output(&r, big, n, 1));
    CHECK(r.json_ok);
    /* only an oversized line: nothing */
    memset(big, '{', n);
    tk_result_init(&r, c);
    CHECK(tk_parse_output(&r, big, n, 1) == -1);
    free(big);

    /* not installed */
    const char *ni = "{\"toolkit\":\"gtk4\",\"version\":null,\"backend\":null,\"installed\":false,\"error\":\"not-installed\"}\n";
    tk_result_init(&r, c);
    CHECK(!tk_parse_output(&r, ni, strlen(ni), 0));
    tk_finish(&r, 1, 3);
    CHECK(r.status == TK_ST_NOT_INSTALLED && r.score < 0);
}

/* ---- the score ---- */

static void test_score(void) {
    TkResult r;
    TkLog *l = malloc(sizeof *l);
    int c = combo("gtk3-wayland"), all = 0;
    for (int i = 0; i < TK_CHECKS; i++) all += tk_check_weight(i);
    CHECK(all == 27);
    CHECK(tk_check_weight(0) == 2);         /* map */

    /* every check passes, three globals bound: 30 of 30 */
    tk_log_init(l);
    feed(l, "[1.0]  -> wl_registry#2.bind(1, \"wl_compositor\", 6, new id [unknown]#3)\n"
            "[1.0]  -> wl_registry#2.bind(2, \"xdg_wm_base\", 6, new id [unknown]#4)\n"
            "[1.0]  -> wl_registry#2.bind(3, \"wl_seat\", 9, new id [unknown]#5)\n");
    tk_result_init(&r, c);
    r.log = l;
    const char *s = line_for("gtk3", NULL);
    tk_parse_output(&r, s, strlen(s), 0);
    tk_finish(&r, 1, 0);
    CHECK(r.status == TK_ST_RAN && r.total == 30 && r.points == 30 && r.score == 100.0);

    /* dnd skipped (not counted), theme failed: 26 of 27 → 96.2 (rounded down, one decimal) */
    tk_result_init(&r, c);
    r.log = l;
    s = line_for("gtk3", "dnd=null theme=false");
    tk_parse_output(&r, s, strlen(s), 0);
    tk_finish(&r, 1, 0);
    CHECK(r.total == 27 && r.points == 26 && r.skipped == 1);
    CHECK(r.score > 96.15 && r.score < 96.25);

    /* the same on X11: the globals are not counted */
    tk_result_init(&r, combo("gtk3-x11"));
    r.log = l;
    tk_parse_output(&r, s, strlen(s), 0);
    tk_finish(&r, 1, 0);
    CHECK(r.total == 24 && r.points == 23);

    /* a protocol error: the combination is 0 %, and its global failed */
    feed(l, "[1.0] wl_display#1.error(wl_seat#5, 0, \"x\")\n");
    tk_result_init(&r, c);
    r.log = l;
    tk_parse_output(&r, s, strlen(s), 0);
    tk_finish(&r, 1, 0);
    CHECK(r.status == TK_ST_FATAL && r.fatal == TK_F_PROTOCOL && r.score == 0 && r.failed_globals == 1);

    /* a crash, a timeout and no result are 0 % too */
    tk_log_init(l);
    tk_result_init(&r, c);
    r.log = l;
    tk_parse_output(&r, s, strlen(s), 0);
    tk_finish(&r, 2, SIGSEGV);
    CHECK(r.status == TK_ST_FATAL && r.fatal == TK_F_CRASH && r.signal_no == SIGSEGV && r.score == 0);
    tk_result_init(&r, c);
    r.log = l;
    tk_parse_output(&r, s, strlen(s), 0);
    tk_finish(&r, 3, SIGKILL);
    CHECK(r.fatal == TK_F_TIMEOUT && r.score == 0);
    tk_result_init(&r, c);
    r.log = l;
    tk_finish(&r, 1, 0);
    CHECK(r.fatal == TK_F_NO_RESULT && r.score == 0);

    /* every check skipped: nothing counted, no score, not in the mean */
    tk_result_init(&r, combo("gtk3-x11"));
    r.log = l;
    char none[512] = "";
    for (int i = 0; i < TK_CHECKS; i++) { strcat(none, tk_check_id(i)); strcat(none, "=null "); }
    s = line_for("gtk3", none);
    tk_parse_output(&r, s, strlen(s), 0);
    tk_finish(&r, 1, 0);
    CHECK(r.status == TK_ST_RAN && r.total == 0 && r.score < 0);

    /* the global mean and the recommendation */
    TkReport rep;
    memset(&rep, 0, sizeof rep);
    rep.n = 3;
    tk_result_init(&rep.r[0], combo("qt5-wayland"));
    rep.r[0].status = TK_ST_FATAL;
    rep.r[0].fatal = TK_F_CRASH;
    rep.r[0].score = 0;
    tk_result_init(&rep.r[1], combo("qt5-xcb"));
    rep.r[1].status = TK_ST_RAN;
    rep.r[1].score = 90;
    tk_result_init(&rep.r[2], combo("qt6-wayland"));
    rep.r[2].status = TK_ST_NOT_INSTALLED;
    tk_report_global(&rep);
    CHECK(rep.ran == 2 && rep.global == 45.0);
    const char *rec = tk_recommend(&rep.r[0], &rep.r[1]);
    CHECK(rec && !strcmp(rec, "QT_QPA_PLATFORM=xcb"));
    rep.r[0].status = TK_ST_RAN;
    rep.r[0].score = 75;                    /* 15 points apart: no advice */
    CHECK(!tk_recommend(&rep.r[0], &rep.r[1]));
    rep.r[0].score = 20;
    rep.r[1].score = 40;                    /* X11 itself under 50 %: no advice */
    CHECK(!tk_recommend(&rep.r[0], &rep.r[1]));
    CHECK(!tk_recommend(&rep.r[1], &rep.r[0]));     /* arguments the wrong way round */
    free(l);
}

/* ---- JSON and the table ---- */

static const char *Tkey(const char *k) { return k; }

static void test_output(void) {
    TkReport rep;
    memset(&rep, 0, sizeof rep);
    rep.n = 2;
    TkLog *l = malloc(sizeof *l);
    tk_log_init(l);
    feed(l, "[1.0]  -> wl_registry#2.bind(1, \"wl_compositor\", 6, new id [unknown]#3)\n");
    tk_result_init(&rep.r[0], combo("gtk4-wayland"));
    rep.r[0].log = l;
    const char *s = "{\"toolkit\":\"gtk4\",\"version\":\"4.18.6\",\"backend\":\"wayland\",\"checks\":{\"map\":{\"ok\":false,\"detail\":\"SECRET-DETAIL\"}}}";
    tk_parse_output(&rep.r[0], s, strlen(s), 0);
    tk_finish(&rep.r[0], 1, 0);
    tk_result_init(&rep.r[1], combo("qt6-xcb"));
    rep.r[1].status = TK_ST_NOT_INSTALLED;
    tk_report_global(&rep);
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    tk_report_json(f, &rep);
    fclose(f);
    JDoc d = { 0 };
    CHECK(!json_parse(buf, len, &d, NULL, 0));
    CHECK(!strstr(buf, "SECRET"));          /* the harness's free text never reaches the JSON */
    CHECK(json_num(d.root, "expected", 0) == TK_GLOBALS);
    const JVal *cs = json_get(d.root, "combos");
    CHECK(cs && cs->len == 2);
    const JVal *c0 = cs ? cs->kid : NULL;
    CHECK(c0 && !strcmp(json_str(c0, "status"), "ran") && !strcmp(json_str(c0, "version"), "4.18.6"));
    /* map failed (2 of 2 counted), wl_compositor passed: 1 of 3 → 33.3 */
    CHECK(c0 && json_num(c0, "score", -1) == 33.3);
    const JVal *ck = c0 ? json_get(c0, "checks") : NULL;
    CHECK(ck && ck->len == TK_CHECKS + 1);
    const JVal *c1 = c0 ? c0->next : NULL;
    CHECK(c1 && !strcmp(json_str(c1, "status"), "not-installed") && json_get(c1, "score")->type == J_NULL);
    const JVal *rows = json_get(json_get(d.root, "matrix"), "rows");
    CHECK(rows && rows->len == 1 && rows->kid && json_get(rows->kid, "cells")->len == TK_GLOBALS);
    CHECK(json_get(d.root, "weights") && json_num(json_get(d.root, "weights"), "menu", 0) == 3);
    json_free(&d);
    free(buf);

    buf = NULL;
    f = open_memstream(&buf, &len);
    tk_report_table(f, &rep, Tkey);
    fclose(f);
    CHECK(strstr(buf, "gtk4-wayland") && strstr(buf, "33.3 %") && strstr(buf, "status.notInstalled") && strstr(buf, "SECRET-DETAIL"));
    free(buf);
    free(l);

    char out[128];
    const char *kv[] = { "a", "1", "bb", "22", NULL };
    tk_fill("{a}-{bb}-{c}-{", kv, out, sizeof out);
    CHECK(!strcmp(out, "1-22-{c}-{"));
    tk_fill("{bb}{bb}{bb}", kv, out, 5);
    CHECK(!strcmp(out, "2222"));
}

/* ---- the environment, the cache and real runs ---- */

static char tmp[256];

static void write_file(const char *name, const char *text, int mode) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", tmp, name);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(1); }
    fputs(text, f);
    fclose(f);
    chmod(p, (mode_t)mode);
}

static int has_var(char **env, const char *kv) {
    for (int i = 0; env[i]; i++) if (!strcmp(env[i], kv)) return 1;
    return 0;
}
static int has_key(char **env, const char *k) {
    size_t n = strlen(k);
    for (int i = 0; env[i]; i++) if (!strncmp(env[i], k, n) && env[i][n] == '=') return 1;
    return 0;
}

static void test_env(void) {
    char rt[300], xa[400];
    snprintf(rt, sizeof rt, "XDG_RUNTIME_DIR=%s", tmp);
    write_file("lord-xauth.7", "cookie", 0600);
    snprintf(xa, sizeof xa, "XAUTHORITY=%s/lord-xauth.7", tmp);
    char *in[] = { "HOME=/home/x", rt, "WAYLAND_DISPLAY=wayland-1", "DISPLAY=:7", "LD_PRELOAD=/evil.so", "SECRET_TOKEN=1",
                   "PYTHONPATH=/evil", "GDK_BACKEND=broadway", "LANG=es_AR.UTF-8", NULL };
    TkRunOpts o = { tmp, "/bin/false", 1000, 1000, 0, NULL, in };
    char buf[8192], *env[48];
    int n = tk_env(&o, combo("gtk4-wayland"), buf, sizeof buf, env, 48);
    CHECK(n > 0);
    CHECK(has_var(env, "WAYLAND_DISPLAY=wayland-1") && has_var(env, "GDK_BACKEND=wayland") && has_var(env, "WAYLAND_DEBUG=1"));
    CHECK(has_var(env, "LANG=C.UTF-8") && has_var(env, "HOME=/home/x"));
    CHECK(!has_key(env, "DISPLAY") && !has_key(env, "LD_PRELOAD") && !has_key(env, "SECRET_TOKEN") && !has_key(env, "PYTHONPATH"));
    CHECK(!has_var(env, "GDK_BACKEND=broadway"));
    n = tk_env(&o, combo("qt5-xcb"), buf, sizeof buf, env, 48);
    CHECK(n > 0 && has_var(env, "DISPLAY=:7") && has_var(env, xa) && has_var(env, "QT_QPA_PLATFORM=xcb") && !has_key(env, "WAYLAND_DISPLAY"));
    n = tk_env(&o, combo("x11-xwayland"), buf, sizeof buf, env, 48);
    CHECK(n > 0 && has_var(env, "DISPLAY=:7") && !has_key(env, "GDK_BACKEND") && !has_key(env, "QT_QPA_PLATFORM"));
    /* no display of that kind: no-display */
    char *in2[] = { "HOME=/home/x", rt, NULL };
    o.env = in2;
    CHECK(tk_env(&o, combo("gtk3-wayland"), buf, sizeof buf, env, 48) == -1);
    CHECK(tk_env(&o, combo("gtk3-x11"), buf, sizeof buf, env, 48) == -1);
    /* a small buffer fails instead of cutting a variable */
    o.env = in;
    CHECK(tk_env(&o, combo("gtk3-wayland"), buf, 40, env, 48) == -1);
}

/* the fake python3: "-I -c code" is the import probe (PyQt6 is "missing"), "-I file" runs the file with sh */
static const char *fake_python =
    "#!/bin/sh\n"
    "[ \"$1\" = -I ] && shift\n"
    "if [ \"$1\" = -c ]; then case \"$2\" in *PyQt6*) exit 1;; esac; exit 0; fi\n"
    "exec /bin/sh \"$@\"\n";

/* gtk3: binds three globals on Wayland and passes all but theme; on X11 its result comes before a 100 KiB line */
static const char *fake_gtk3 =
    "if [ \"$GDK_BACKEND\" = wayland ]; then\n"
    "  [ -n \"$WAYLAND_DEBUG\" ] && [ -z \"$DISPLAY\" ] || exit 9\n"
    "  echo '[1234567.892] {Default Queue}  -> wl_registry#2.bind(1, \"wl_compositor\", 6, new id [unknown]#3)' >&2\n"
    "  echo '[1234567.893] {Default Queue}  -> wl_registry#2.bind(2, \"xdg_wm_base\", 6, new id [unknown]#4)' >&2\n"
    "  echo '[1234567.894] {Default Queue}  -> wl_registry#2.bind(3, \"wl_seat\", 9, new id [unknown]#5)' >&2\n"
    "  echo 'Gtk-WARNING **: no trigger event for menu popup' >&2\n"
    "  echo '%WL%'\n"
    "else\n"
    "  echo '%X%'\n"
    "  head -c 102400 /dev/zero | tr '\\0' 'x'\n"
    "fi\n";

/* gtk4: a protocol error on Wayland (no line); on X11 everything but the menu */
static const char *fake_gtk4 =
    "if [ \"$GDK_BACKEND\" = wayland ]; then\n"
    "  echo '[1.0]  -> wl_registry#2.bind(1, \"xdg_wm_base\", 6, new id [unknown]#5)' >&2\n"
    "  echo '[1.0]  -> xdg_wm_base#5.get_xdg_surface(new id xdg_surface#12, wl_surface#9)' >&2\n"
    "  echo '[1.0] wl_display#1.error(xdg_surface#12, 3, \"bad\")' >&2\n"
    "  echo 'xdg_surface#12: error 3: bad' >&2\n"
    "  exit 1\n"
    "fi\n"
    "echo '%X%'\n"
    "printf 'noise \\001\\002\\033[31m\\n{\"toolkit\":\"gtk4\",\"checks\":\\n'\n";

/* qt5: crashes on Wayland; hangs on xcb with a child of its own still running */
static const char *fake_qt5 =
    "if [ \"$QT_QPA_PLATFORM\" = wayland ]; then kill -SEGV $$; fi\n"
    "sleep 30.123 &\n"
    "echo '{\"toolkit\":\"qt5\",\"checks\":{}}' >&2\n"
    "sleep 30\n";

static const char *fake_qt6 = "echo never\n";
static const char *fake_x11 = "echo '{\"toolkit\":\"x11\",\"version\":null,\"backend\":null,\"installed\":false}'\nexit 3\n";

static void put_harness(const char *name, const char *tmpl, const char *wl, const char *x) {
    char out[8192];
    size_t k = 0;
    for (const char *p = tmpl; *p && k + 1 < sizeof out;) {
        const char *rep = !strncmp(p, "%WL%", 4) ? wl : !strncmp(p, "%X%", 3) ? x : NULL;
        if (rep) {
            size_t n = strlen(rep);
            while (n && rep[n - 1] == '\n') n--;
            if (k + n >= sizeof out) break;
            memcpy(out + k, rep, n);
            k += n;
            p += !strncmp(p, "%WL%", 4) ? 4 : 3;
            continue;
        }
        out[k++] = *p++;
    }
    out[k] = 0;
    write_file(name, out, 0644);
}

static void test_runs(void) {
    write_file("python3", fake_python, 0755);
    char wl[2048], x[2048];
    snprintf(wl, sizeof wl, "%s", line_for("gtk3", "theme=false"));
    snprintf(x, sizeof x, "%s", line_for("gtk3", NULL));
    put_harness("gtk3.py", fake_gtk3, wl, x);
    snprintf(x, sizeof x, "%s", line_for("gtk4", "menu=false"));
    put_harness("gtk4.py", fake_gtk4, "", x);
    write_file("qt5.py", fake_qt5, 0644);
    write_file("qt6.py", fake_qt6, 0644);
    write_file("x11.py", fake_x11, 0644);

    char rt[300], py[300];
    snprintf(rt, sizeof rt, "XDG_RUNTIME_DIR=%s", tmp);
    snprintf(py, sizeof py, "%s/python3", tmp);
    char *in[] = { "HOME=/nonexistent", rt, "WAYLAND_DISPLAY=wayland-test", "DISPLAY=:99", NULL };
    TkRunOpts o = { tmp, py, 1500, 2000, 0, NULL, in };
    int which[TK_COMBOS];
    for (int i = 0; i < TK_COMBOS; i++) which[i] = i;
    TkReport rep;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(!tk_audit(&o, which, TK_COMBOS, &rep));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    CHECK(rep.n == TK_COMBOS);
    const TkResult *r;

    r = &rep.r[combo("gtk3-wayland")];
    CHECK(r->status == TK_ST_RAN && r->total == 30 && r->points == 29);      /* theme failed; 3 globals bound */
    if (r->status != TK_ST_RAN) fprintf(stderr, "gtk3-wayland: status %d fatal %d exit %d\n", r->status, r->fatal, r->exit_code);
    r = &rep.r[combo("gtk3-x11")];
    CHECK(r->status == TK_ST_RAN && r->score == 100.0);                      /* the line before 100 KiB of x */
    r = &rep.r[combo("gtk4-wayland")];
    CHECK(r->status == TK_ST_FATAL && r->fatal == TK_F_PROTOCOL && r->score == 0 && r->log->perr[gidx("xdg_wm_base")]);
    r = &rep.r[combo("gtk4-x11")];
    CHECK(r->status == TK_ST_RAN && r->points == 24 && r->total == 27 && r->bad_lines == 2);
    r = &rep.r[combo("qt5-wayland")];
    CHECK(r->status == TK_ST_FATAL && r->fatal == TK_F_CRASH && r->signal_no == SIGSEGV);
    r = &rep.r[combo("qt5-xcb")];
    CHECK(r->status == TK_ST_FATAL && r->fatal == TK_F_TIMEOUT && r->elapsed_ms >= 1500 && r->elapsed_ms < 4000);
    CHECK(rep.r[combo("qt6-wayland")].status == TK_ST_NOT_INSTALLED);       /* the probe failed */
    CHECK(rep.r[combo("qt6-xcb")].status == TK_ST_NOT_INSTALLED);
    CHECK(rep.r[combo("x11-xwayland")].status == TK_ST_NOT_INSTALLED);      /* {"installed":false} */
    CHECK(s < 8);                                                            /* the hang was killed at 1.5 s */
    /* 96.6 + 100 + 0 + 88.8 + 0 + 0 over 6 */
    CHECK(rep.ran == 6);
    CHECK(rep.global > 47.4 && rep.global < 47.6);
    const char *rec = tk_recommend(&rep.r[combo("gtk4-wayland")], &rep.r[combo("gtk4-x11")]);
    CHECK(rec && !strcmp(rec, "GDK_BACKEND=x11"));
    /* the hang's own child was killed with its group: no "sleep 30.123" is left */
    char cmd[600];
    snprintf(cmd, sizeof cmd, "pgrep -fx 'sleep 30.123' >/dev/null");
    CHECK(system(cmd) != 0);

    /* the cache: 0600 in a 0700 folder, valid JSON, replaced whole */
    char path[600], dir[600];
    snprintf(path, sizeof path, "%s/cache/lordos/toolkit-audit.json", tmp);
    CHECK(!tk_save(&rep, path));
    CHECK(!tk_save(&rep, path));
    struct stat st;
    CHECK(!stat(path, &st) && (st.st_mode & 0777) == 0600);
    snprintf(dir, sizeof dir, "%s/cache/lordos", tmp);
    CHECK(!stat(dir, &st) && (st.st_mode & 0777) == 0700);
    FILE *f = fopen(path, "r");
    char *buf = malloc(1 << 20);
    size_t n = f ? fread(buf, 1, (1 << 20) - 1, f) : 0;
    if (f) fclose(f);
    JDoc d = { 0 };
    CHECK(n > 0 && !json_parse(buf, n, &d, NULL, 0));
    CHECK(!strstr(buf, tmp));               /* no path in the result */
    json_free(&d);
    free(buf);
    char *cin[] = { "HOME=/home/u", NULL }, *cin2[] = { "HOME=/home/u", "XDG_CACHE_HOME=/c", NULL }, *cin3[] = { "HOME=rel", NULL };
    CHECK(!tk_cache_path(path, sizeof path, cin) && !strcmp(path, "/home/u/.cache/lordos/toolkit-audit.json"));
    CHECK(!tk_cache_path(path, sizeof path, cin2) && !strcmp(path, "/c/lordos/toolkit-audit.json"));
    CHECK(tk_cache_path(path, sizeof path, cin3) == -1);
    tk_report_free(&rep);

    /* a missing harness and a missing python3 */
    int one[] = { combo("gtk3-wayland") };
    o.dir = "/nonexistent";
    CHECK(!tk_audit(&o, one, 1, &rep) && rep.r[0].status == TK_ST_NO_HARNESS && rep.ran == 0 && rep.global < 0);
    tk_report_free(&rep);
    o.dir = tmp;
    o.python = "/nonexistent/python3";
    CHECK(!tk_audit(&o, one, 1, &rep) && rep.r[0].status == TK_ST_NOT_INSTALLED);
    tk_report_free(&rep);
}

/* --toolkits --last: the saved result in fixed lines; a hand-edited file prints nothing of its own */
static char *last_of(const char *json, long long now, int *rc) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    *rc = tk_last_summary(f, json, strlen(json), now);
    fclose(f);
    return buf;
}

static void test_last(void) {
    TkReport rep;
    memset(&rep, 0, sizeof rep);
    rep.n = 3;
    rep.at_ms = 1764072000000LL;
    TkLog *l = malloc(sizeof *l);
    tk_log_init(l);
    feed(l, "[1.0]  -> wl_registry#2.bind(1, \"xdg_wm_base\", 6, new id [unknown]#3)\n");
    tk_result_init(&rep.r[0], combo("gtk4-wayland"));
    rep.r[0].log = l;
    const char *s = "{\"toolkit\":\"gtk4\",\"version\":\"4.18.6\",\"backend\":\"wayland\",\"checks\":{\"theme\":{\"ok\":false,\"detail\":\"/home/alice\"}}}";
    tk_parse_output(&rep.r[0], s, strlen(s), 0);
    tk_finish(&rep.r[0], 1, 0);
    tk_result_init(&rep.r[1], combo("qt6-xcb"));
    rep.r[1].status = TK_ST_NOT_INSTALLED;
    tk_result_init(&rep.r[2], combo("qt5-wayland"));
    tk_finish(&rep.r[2], 2, 11);
    tk_report_global(&rep);
    char *json = NULL;
    size_t jl = 0;
    FILE *f = open_memstream(&json, &jl);
    tk_report_json(f, &rep);
    fclose(f);
    free(l);
    int rc = -1;
    char *out = last_of(json, rep.at_ms + 2 * 3600 * 1000LL, &rc);
    CHECK(rc == 0);
    CHECK(strstr(out, "saved 2 hours ago"));
    CHECK(strstr(out, "global score: "));
    CHECK(strstr(out, "gtk4-wayland"));
    CHECK(strstr(out, "4.18.6"));
    CHECK(strstr(out, "failed: theme"));
    CHECK(strstr(out, "qt6-xcb        not-installed"));
    CHECK(strstr(out, "crash (signal 11)"));
    CHECK(!strstr(out, "alice") && !strstr(out, "/home"));
    free(out);
    out = last_of(json, rep.at_ms + 5 * 60 * 1000LL, &rc);
    CHECK(rc == 0 && strstr(out, "saved 5 minutes ago"));
    free(out);
    out = last_of(json, rep.at_ms + 3 * 86400 * 1000LL, &rc);
    CHECK(rc == 0 && strstr(out, "saved 3 days ago"));
    free(out);

    /* hand-edited files: each one is refused as a whole, and none of its text comes out */
    struct { const char *from, *to; } bad[] = {
        { "\"tool\":\"lordos-testbench\"", "\"tool\":\"other\"" },
        { "\"id\":\"qt6-xcb\"", "\"id\":\"/home/alice\"" },
        { "\"version\":\"4.18.6\"", "\"version\":\"4.18;rm -rf ~\"" },
        { "\"status\":\"not-installed\"", "\"status\":\"Alice's laptop\"" },
        { "\"fatal\":\"crash\"", "\"fatal\":\"/home/alice\"" },
        { "\"id\":\"theme\"", "\"id\":\"global.evil_secret\"" },
        { "\"result\":\"fail\"", "\"result\":\"/home/alice\"" },
        { "\"recommend\":null", "\"recommend\":\"LD_PRELOAD=/home/alice/x.so\"" },
        { "\"signal\":11", "\"signal\":\"alice\"" },
        { "\"v\":1", "\"v\":2" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const char *at = strstr(json, bad[i].from);
        CHECK(at);
        if (!at) continue;
        char *e = malloc(jl + 256);
        snprintf(e, jl + 256, "%.*s%s%s", (int)(at - json), json, bad[i].to, at + strlen(bad[i].from));
        out = last_of(e, rep.at_ms, &rc);
        if (rc != 1 || !strstr(out, "unreadable") || strstr(out, "alice") || strstr(out, "rm -rf") || strstr(out, "evil"))
            { fprintf(stderr, "last: case %zu not refused: %s\n", i, out); fails++; }
        free(out);
        free(e);
    }
    out = last_of("not json", 0, &rc);
    CHECK(rc == 1 && strstr(out, "unreadable"));
    free(out);
    out = last_of("[]", 0, &rc);
    CHECK(rc == 1);
    free(out);

    /* the file itself: a link is not followed, a regular file is read */
    char p[600], lp[600];
    snprintf(p, sizeof p, "%s/saved.json", tmp);
    snprintf(lp, sizeof lp, "%s/link.json", tmp);
    FILE *w = fopen(p, "w");
    if (w) { fputs(json, w); fclose(w); }
    size_t n = 0;
    char *r = tk_read_saved(p, &n);
    CHECK(r && n == jl);
    free(r);
    CHECK(!symlink(p, lp));
    CHECK(!tk_read_saved(lp, &n));
    snprintf(p, sizeof p, "%s/missing.json", tmp);
    CHECK(!tk_read_saved(p, &n));
    free(json);
}

int main(void) {
    snprintf(tmp, sizeof tmp, "/tmp/tk-audit-test-XXXXXX");
    if (!mkdtemp(tmp)) { perror("mkdtemp"); return 1; }
    test_log();
    test_parse();
    test_score();
    test_output();
    test_env();
    test_runs();
    test_last();
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
    if (system(cmd)) { }
    if (fails) { fprintf(stderr, "toolkit-audit-test: %d failed\n", fails); return 1; }
    printf("toolkit-audit-test: ok\n");
    return 0;
}
