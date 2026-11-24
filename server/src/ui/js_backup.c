/* js_backup.c — lord.backup: Backups (see backup/backup.c and docs/SYSTEM.md).
 *
 *   status()                         status (also pushed by itself: __lordBackup(status))
 *   config() / configure(obj)        { dest, interval: 'hourly'|'daily'|'weekly'|'manual',
 *                                      pauseOnBattery, exclude: ['rel', …], encrypt }
 *   runNow() / pause(bool) / cancel()
 *   list()                           [{ id, time }] oldest first
 *   ls(id, rel)                      [{ name, dir, link, size, mtime }] inside a snapshot
 *   path(id)                         real path of the snapshot (to open it in Files)
 *   restore(id, rel | [rel, …], { replace, suffix, to })   rel '' = everything (alone); a list: up to 64 files and
 *                                      folders, each checked as one (#231); runs in the thread, the
 *                                      result arrives in status().restore
 *   remove(id)                       delete a snapshot
 *   unlock(pass, create) / lock()    encryption: the key is derived from the passphrase (Argon2id, in
 *                                    the thread) and kept only in memory; result in status().unlock
 * Errors are thrown as exceptions carrying the reason. */
#include "js_backup.h"
#include "js_err.h"
#include "../backup/backup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JSValue parsed(JSContext *c, char *j, const char *what) {
    if (!j) return JS_NULL;
    JSValue v = JS_ParseJSON(c, j, strlen(j), what);
    free(j);
    return v;
}

static JSValue fail(JSContext *c, const char *err) { return js_throw_line(c, err[0] ? err : "sys.unknown"); }

static JSValue f_status(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return parsed(c, backup_status_json(), "<backup.status>");
}

static JSValue f_config(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return parsed(c, backup_config_json(), "<backup.config>");
}

static JSValue f_configure(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return js_throw_code(c, "backup.badConfig", NULL);
    JSValue s = JS_JSONStringify(c, argv[0], JS_UNDEFINED, JS_UNDEFINED);
    const char *j = JS_IsException(s) ? NULL : JS_ToCString(c, s);
    JS_FreeValue(c, s);
    if (!j) return JS_EXCEPTION;
    char err[256] = "";
    int r = backup_configure(j, err, sizeof err);
    JS_FreeCString(c, j);
    return r ? fail(c, err) : JS_TRUE;
}

static JSValue f_run(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char err[256] = "";
    return backup_run_now(err, sizeof err) ? fail(c, err) : JS_TRUE;
}

static JSValue f_pause(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    backup_pause(argc > 0 && JS_ToBool(c, argv[0]) > 0);
    return JS_TRUE;
}

static JSValue f_cancel(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    backup_cancel();
    return JS_TRUE;
}

static JSValue f_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return parsed(c, backup_list_json(), "<backup.list>");
}

static const char *arg_str(JSContext *c, int argc, JSValueConst *argv, int i) {
    return i < argc && JS_IsString(argv[i]) ? JS_ToCString(c, argv[i]) : NULL;
}

static JSValue f_ls(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = arg_str(c, argc, argv, 0), *rel = arg_str(c, argc, argv, 1);
    char err[256] = "";
    char *j = id ? backup_ls_json(id, rel ? rel : "", err, sizeof err) : NULL;
    if (id) JS_FreeCString(c, id);
    if (rel) JS_FreeCString(c, rel);
    if (!j) return fail(c, id ? err : "missing backup id");
    return parsed(c, j, "<backup.ls>");
}

static JSValue f_path(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = arg_str(c, argc, argv, 0);
    char p[4096];
    int r = id ? backup_snapshot_path(id, p, sizeof p) : -1;
    if (id) JS_FreeCString(c, id);
    return r ? JS_NULL : JS_NewString(c, p);
}

static JSValue f_restore(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = arg_str(c, argc, argv, 0), *rel = arg_str(c, argc, argv, 1);
    int replace = 0;
    const char *suffix = NULL, *to = NULL;
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v = JS_GetPropertyStr(c, argv[2], "replace");
        replace = JS_ToBool(c, v) > 0;
        JS_FreeValue(c, v);
        v = JS_GetPropertyStr(c, argv[2], "suffix");
        if (JS_IsString(v)) suffix = JS_ToCString(c, v);
        JS_FreeValue(c, v);
        v = JS_GetPropertyStr(c, argv[2], "to");
        if (JS_IsString(v)) to = JS_ToCString(c, v);
        JS_FreeValue(c, v);
    }
    char err[256] = "";
    /* several files and folders (#231): an array of 1..BACKUP_RESTORE_MAX strings, each checked as one restore is */
    const char *many[BACKUP_RESTORE_MAX] = { 0 };
    int nmany = 0, bad = 0;
    if (argc > 1 && JS_IsArray(c, argv[1])) {
        uint32_t len = 0;
        JSValue lv = JS_GetPropertyStr(c, argv[1], "length");
        if (JS_ToUint32(c, &len, lv)) len = 0;
        JS_FreeValue(c, lv);
        if (len < 1 || len > BACKUP_RESTORE_MAX) bad = 1;
        for (uint32_t i = 0; !bad && i < len; i++) {
            JSValue v = JS_GetPropertyUint32(c, argv[1], i);
            if (JS_IsString(v)) many[nmany++] = JS_ToCString(c, v);
            if (!many[i]) bad = 1;
            JS_FreeValue(c, v);
        }
    }
    int r;
    if (!id) r = -1;
    else if (bad) { snprintf(err, sizeof err, "backup.badPath"); r = -1; }
    else if (nmany) r = backup_restore_list(id, many, nmany, replace, suffix ? suffix : " (restaurado)", to, err, sizeof err);
    else r = backup_restore(id, rel ? rel : "", replace, suffix ? suffix : " (restaurado)", to, err, sizeof err);
    for (int i = 0; i < nmany; i++) JS_FreeCString(c, many[i]);
    if (id) JS_FreeCString(c, id);
    if (rel) JS_FreeCString(c, rel);
    if (suffix) JS_FreeCString(c, suffix);
    if (to) JS_FreeCString(c, to);
    return r ? fail(c, id ? err : "missing backup id") : JS_TRUE;
}

static JSValue f_remove(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = arg_str(c, argc, argv, 0);
    char err[256] = "";
    int r = id ? backup_delete(id, err, sizeof err) : -1;
    if (id) JS_FreeCString(c, id);
    return r ? fail(c, id ? err : "missing backup id") : JS_TRUE;
}

static JSValue f_unlock(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    size_t len = 0;
    const char *pass = argc > 0 && JS_IsString(argv[0]) ? JS_ToCStringLen(c, &len, argv[0]) : NULL;
    int create = argc > 1 && JS_ToBool(c, argv[1]) > 0;
    char err[256] = "";
    int r = pass && strlen(pass) == len ? backup_unlock(pass, create, err, sizeof err) : -1;
    if (pass) JS_FreeCString(c, pass);      /* not wiped: it may be the JS string's own memory */
    return r ? fail(c, pass ? err : "missing passphrase") : JS_TRUE;
}

static JSValue f_lock(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    backup_lock();
    return JS_TRUE;
}

static const JSCFunctionListEntry fns[] = {
    JS_CFUNC_DEF("status", 0, f_status),
    JS_CFUNC_DEF("config", 0, f_config),
    JS_CFUNC_DEF("configure", 1, f_configure),
    JS_CFUNC_DEF("runNow", 0, f_run),
    JS_CFUNC_DEF("pause", 1, f_pause),
    JS_CFUNC_DEF("cancel", 0, f_cancel),
    JS_CFUNC_DEF("list", 0, f_list),
    JS_CFUNC_DEF("ls", 2, f_ls),
    JS_CFUNC_DEF("path", 1, f_path),
    JS_CFUNC_DEF("restore", 3, f_restore),
    JS_CFUNC_DEF("remove", 1, f_remove),
    JS_CFUNC_DEF("unlock", 2, f_unlock),
    JS_CFUNC_DEF("lock", 0, f_lock),
};

void js_backup_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, fns, sizeof fns / sizeof fns[0]);
    JS_SetPropertyStr(c, lord, "backup", o);
}
