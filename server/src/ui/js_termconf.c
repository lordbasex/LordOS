/* js_termconf.c — lord.termconf: the Terminal's configuration file (../termconf.h, docs/TERMINAL.md,
 * "Configuration file"). Only the shell's JS has it (not the broker: isolated apps never see it).
 *
 *   setPath(path)   the file to use ("" = ~/.config/lordos/terminal.json) → its resolved path; throws a code
 *   where()         { path, cache, custom }
 *   read(which)     'active' | 'cache' → { text, mtime }; throws termconf.missing, termconf.unavailable, …
 *   write(text)     the active file (and the cache with it when it is a chosen one) → its mtime; throws
 *   writeCache(text)  only the default file, when a chosen file changed from outside (that file is left as it is)
 *   mtime()         the active file's mtime, or null (watching for changes made outside)
 *   exportTo(path, text)  Export Profiles…: a file of the user's (not a protected one), 0600 → true; throws
 *   migrate(text)   settings.json's "terminal" section into the default file (backup first) → true; throws */
#include "js_termconf.h"
#include "js_err.h"
#include "../termconf.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static JSValue f_set_path(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *p = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    char err[256];
    int r = termconf_set_custom(p ? p : "", err, sizeof err);
    if (p) JS_FreeCString(c, p);
    if (r) return js_throw_line(c, err);
    return JS_NewString(c, termconf_path());
}

static JSValue f_where(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "path", JS_NewString(c, termconf_path()));
    JS_SetPropertyStr(c, o, "cache", JS_NewString(c, termconf_cache()));
    JS_SetPropertyStr(c, o, "custom", JS_NewBool(c, termconf_is_custom()));
    return o;
}

static JSValue f_read(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *w = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    int cache = w && !strcmp(w, "cache");
    if (w) JS_FreeCString(c, w);
    char err[256];
    size_t len = 0;
    double mt = 0;
    char *txt = termconf_read(cache ? termconf_cache() : termconf_path(), &len, &mt, err, sizeof err);
    if (!txt) return js_throw_line(c, err);
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "text", JS_NewStringLen(c, txt, len));
    JS_SetPropertyStr(c, o, "mtime", JS_NewFloat64(c, mt));
    free(txt);
    return o;
}

static JSValue f_write(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    size_t len = 0;
    const char *txt = argc > 0 && JS_IsString(argv[0]) ? JS_ToCStringLen(c, &len, argv[0]) : NULL;
    if (!txt) return js_throw_code(c, "sys.badArgument", NULL);
    char err[256];
    double mt = 0, cmt = 0;
    int r = termconf_write(termconf_path(), txt, len, &mt, err, sizeof err);
    /* a chosen file: the default one keeps the last good version, for when that place isn't there */
    if (!r && termconf_is_custom()) { char e2[256]; termconf_write(termconf_cache(), txt, len, &cmt, e2, sizeof e2); }
    JS_FreeCString(c, txt);
    if (r) return js_throw_line(c, err);
    return JS_NewFloat64(c, mt);
}

static JSValue f_write_cache(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    size_t len = 0;
    const char *txt = argc > 0 && JS_IsString(argv[0]) ? JS_ToCStringLen(c, &len, argv[0]) : NULL;
    if (!txt) return js_throw_code(c, "sys.badArgument", NULL);
    char err[256];
    double mt = 0;
    int r = termconf_is_custom() ? termconf_write(termconf_cache(), txt, len, &mt, err, sizeof err) : 0;
    JS_FreeCString(c, txt);
    if (r) return js_throw_line(c, err);
    return JS_TRUE;
}

static JSValue f_export(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *p = argc > 1 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    size_t len = 0;
    const char *txt = p && JS_IsString(argv[1]) ? JS_ToCStringLen(c, &len, argv[1]) : NULL;
    if (!txt) { if (p) JS_FreeCString(c, p); return js_throw_code(c, "sys.badArgument", NULL); }
    char err[256];
    int r = termconf_export(p, txt, len, err, sizeof err);
    JS_FreeCString(c, p);
    JS_FreeCString(c, txt);
    if (r) return js_throw_line(c, err);
    return JS_TRUE;
}

static JSValue f_mtime(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    struct stat st;
    if (lstat(termconf_path(), &st) || !S_ISREG(st.st_mode)) return JS_NULL;
    return JS_NewFloat64(c, st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9);
}

static JSValue f_migrate(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    size_t len = 0;
    const char *txt = argc > 0 && JS_IsString(argv[0]) ? JS_ToCStringLen(c, &len, argv[0]) : NULL;
    if (!txt) return js_throw_code(c, "sys.badArgument", NULL);
    char err[256];
    int r = termconf_migrate(txt, len, err, sizeof err);
    JS_FreeCString(c, txt);
    if (r) return js_throw_line(c, err);
    return JS_TRUE;
}

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("setPath", 1, f_set_path), JS_CFUNC_DEF("where", 0, f_where), JS_CFUNC_DEF("read", 1, f_read),
    JS_CFUNC_DEF("write", 1, f_write), JS_CFUNC_DEF("writeCache", 1, f_write_cache), JS_CFUNC_DEF("mtime", 0, f_mtime), JS_CFUNC_DEF("exportTo", 2, f_export), JS_CFUNC_DEF("migrate", 1, f_migrate),
};

void js_termconf_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "termconf", o);
}
