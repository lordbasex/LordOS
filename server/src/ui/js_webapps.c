/* js_webapps.c — lord.webapps: web apps and their profiles (docs/WEBAPPS.md). The registry, the probe and the
 * lord-web processes are web/webmgr.c's; the permission questions are webask.c's. Desktop only: not in the
 * broker, so isolated .lpk apps get no lord.webapps. JS never names a URL to start or a command: launch takes
 * an app and a profile that must be in the registry, install takes a catalog id or a finished probe.
 *
 *   available()                         is lord-web (the lordos-web package) installed?
 *   list()                              [registry entry + iconPath], sorted by id
 *   get(app)                            one entry, or null
 *   probe(url)                          → probe id; starts lord-web --probe (https: only); throws a code
 *   probeResult(id)                     null while it runs; { done, ok, name, startUrl, scope, origin, themeColor,
 *                                       manifest, iconPath, iconKind } or { done, ok: false, error }
 *   install(entry)                      → app id; entry { source: 'catalog', id, profile } or
 *                                       { source: 'url', probe, name, profile } (profile: the first profile's name)
 *   addProfile(app, name)               → profile id
 *   renameProfile(app, profile, name) · removeProfile(app, profile, deleteData)
 *   setPermission(app, kind, value)     kind: notifications|camera|microphone|screen|location; value: allow|ask|deny
 *   setBackground(app, on) · uninstall(app, deleteData)
 *   setProfileColor(app, profile, color) red|orange|yellow|green|blue|purple|gray|none; a new profile gets one by itself
 *                                       (webreg_profile_default_color). Each colored profile's icon (the app's icon
 *                                       with the color's dot and the profile's initial) is kept as
 *                                       ~/.config/lordos/web/icons/<app>.<profile>.png for lord-web's tray icon
 *   setTray(app, on)                    the app's tray icon, Open and Quit (registry "tray"; lord-web reads it); background
 *                                       mode (setBackground) stays its own switch
 *   setNotifySound(app, value)          auto|app|system|none (registry "notifySound": whose sound a notification makes)
 *   fetchIcon(app)                      → probe id; the site's own icon for an installed or a catalog app: the probe
 *                                       of "Add from an address" on the app's own start address (lord-web needed)
 *   iconResult(app)                     null while it runs; { done, ok, iconPath } once the icon is kept in
 *                                       ~/.cache/lordos/web/icons/<app>.<png|svg> (webicon.c), or { done, ok: false, error }
 *   icons()                             { [app]: path } of the cached icons (list() and get() already use them)
 *   badges()                            [{ app, profile, count }]: the unread counts lord-web set (SetBadge, webask.c)
 *   launch(app, profile)                → 'started' or 'running' (lord-server then asks it to show its window)
 *   running()                           [{ app, profile, pid }]
 *   memory(app)                         null when none of its profiles runs; else { kb, processes, unreadable,
 *                                       profiles: [{ profile, kb, processes }] }: the Pss of each running lord-web of
 *                                       the app and of its WebKit processes (web/webmem.c; read-only, bounded)
 *   owner(surface)                      { app, profile } when window surface is one of our lord-web processes, else null
 *   answer(id, value)                   a permission question (the "webapps-ask" event): allow|deny|once|deny-once
 *   asks()                              the questions still waiting
 *   edit(app, profile, command)         the Edit menu on that web app's window: undo|redo|cut|copy|paste|selectAll, to its
 *                                       lord-web (org.lordos.WebApp1.Edit, which checks the caller is lord-server)
 *   menuAnswer(id, item)                the person's choice in a page's context menu (the "webapps-menu" event), or "" */
#include "js_webapps.h"
#include "js_err.h"
#include "js_sys.h"
#include "webask.h"
#include "webicon.h"
#include "../hw/notifyd.h"
#include "../sec/json.h"
#include "../web/webedit.h"
#include "../web/webmem.h"
#include "../web/webmgr.h"
#include "../wl/wl.h"
#include "../wl/wl.h"
#include "../wl/xdgact.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* argument i as a C string (freed by the caller with JS_FreeCString), or NULL */
static const char *arg_str(JSContext *c, int argc, JSValueConst *argv, int i) {
    return i < argc && JS_IsString(argv[i]) ? JS_ToCString(c, argv[i]) : NULL;
}

static JSValue done(JSContext *c, const char *err, JSValue ok) {
    if (err) { JS_FreeValue(c, ok); return js_throw_code(c, err, NULL); }
    return ok;
}

static JSValue f_available(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewBool(c, webmgr_available());
}

static const char *home_dir(void) {
    const char *h = getenv("HOME");
    return h && h[0] == '/' && strlen(h) < PATH_MAX - 128 ? h : NULL;
}

typedef struct { JSContext *c; JSValue arr; uint32_t n; } ListCtx;

/* the site's own icon when one is cached (webicon.c), else the registry's (the letter tile for a catalog app) */
static JSValue entry_value(JSContext *c, const char *id, const char *text, size_t len, const char *icon) {
    JSValue o = JS_ParseJSON(c, text, len, "<webapps>");
    if (JS_IsException(o)) { JS_FreeValue(c, JS_GetException(c)); return JS_NULL; }
    char cached[PATH_MAX];
    int own = !webicon_cached(home_dir(), id, cached, sizeof cached);
    JS_SetPropertyStr(c, o, "iconPath", JS_NewString(c, own ? cached : icon));
    JS_SetPropertyStr(c, o, "siteIcon", JS_NewBool(c, own));
    return o;
}

static void list_one(const WebApp *a, const char *text, size_t len, const char *icon, void *ctx) {
    ListCtx *l = ctx;
    JSValue o = entry_value(l->c, a->id, text, len, icon);
    if (!JS_IsNull(o)) JS_SetPropertyUint32(l->c, l->arr, l->n++, o);
}

static JSValue f_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    ListCtx l = { c, JS_NewArray(c), 0 };
    webmgr_each(list_one, &l);
    return l.arr;
}

typedef struct { JSContext *c; const char *want; JSValue v; } GetCtx;
static void get_one(const WebApp *a, const char *text, size_t len, const char *icon, void *ctx) {
    GetCtx *g = ctx;
    if (JS_IsNull(g->v) && !strcmp(a->id, g->want)) g->v = entry_value(g->c, a->id, text, len, icon);
}

static JSValue f_get(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0);
    if (!app) return js_throw_code(c, "sys.badArgument", NULL);
    GetCtx g = { c, app, JS_NULL };
    if (webreg_id_ok(app)) webmgr_each(get_one, &g);
    JS_FreeCString(c, app);
    return g.v;
}

static JSValue f_probe(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *url = arg_str(c, argc, argv, 0);
    if (!url) return js_throw_code(c, "sys.badArgument", NULL);
    const char *err = NULL;
    int id = webmgr_probe(url, &err);
    JS_FreeCString(c, url);
    return done(c, id ? NULL : err, JS_NewInt32(c, id));
}

static JSValue f_probe_result(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t id = 0;
    if (argc < 1 || JS_ToInt32(c, &id, argv[0])) return js_throw_code(c, "sys.badArgument", NULL);
    int gone = 0;
    char *j = webmgr_probe_result(id, &gone);
    if (gone) return JS_ParseJSON(c, "{\"done\":true,\"ok\":false,\"error\":\"webapps.probeGone\"}", 53, "<probe>");
    if (!j) return JS_NULL;
    JSValue v = JS_ParseJSON(c, j, strlen(j), "<probe>");
    free(j);
    return v;
}

static JSValue f_install(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return js_throw_code(c, "sys.badArgument", NULL);
    JSValue s = JS_IsString(argv[0]) ? JS_DupValue(c, argv[0]) : JS_JSONStringify(c, argv[0], JS_UNDEFINED, JS_UNDEFINED);
    size_t len = 0;
    const char *j = JS_IsString(s) ? JS_ToCStringLen(c, &len, s) : NULL;
    JS_FreeValue(c, s);
    if (!j) return js_throw_code(c, "sys.badArgument", NULL);
    char app[WEBREG_ID_MAX + 1] = "";
    const char *err = webmgr_install(j, len, app, sizeof app);
    JS_FreeCString(c, j);
    return done(c, err, JS_NewString(c, app));
}

static const char *set_color(const char *app, const char *profile, const char *color);

static JSValue f_add_profile(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0), *name = arg_str(c, argc, argv, 1);
    const char *err = !app || !name ? "sys.badArgument" : NULL;
    char id[WEBREG_ID_MAX + 1] = "";
    if (!err) err = webmgr_add_profile(app, name, id, sizeof id);
    if (!err && set_color(app, id, NULL)) fprintf(stderr, "webapps: no color for %s (%s)\n", app, id);   /* it is added all the same */
    JS_FreeCString(c, app);
    JS_FreeCString(c, name);
    return done(c, err, JS_NewString(c, id));
}

static JSValue f_rename_profile(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0), *prof = arg_str(c, argc, argv, 1), *name = arg_str(c, argc, argv, 2);
    const char *err = !app || !prof || !name ? "sys.badArgument" : webmgr_rename_profile(app, prof, name);
    JS_FreeCString(c, app);
    JS_FreeCString(c, prof);
    JS_FreeCString(c, name);
    return done(c, err, JS_TRUE);
}

static JSValue f_remove_profile(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0), *prof = arg_str(c, argc, argv, 1);
    int del = argc > 2 && JS_ToBool(c, argv[2]) == 1;
    const char *err = !app || !prof ? "sys.badArgument" : webmgr_remove_profile(app, prof, del);
    if (!err) webicon_profile_remove(webmgr_config_dir(), app, prof);
    JS_FreeCString(c, app);
    JS_FreeCString(c, prof);
    return done(c, err, JS_TRUE);
}

static JSValue f_set_permission(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0), *kind = arg_str(c, argc, argv, 1), *val = arg_str(c, argc, argv, 2);
    const char *err = !app || !kind || !val ? "sys.badArgument" : webmgr_set_permission(app, kind, val);
    JS_FreeCString(c, app);
    JS_FreeCString(c, kind);
    JS_FreeCString(c, val);
    return done(c, err, JS_TRUE);
}

static JSValue f_set_background(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0);
    int on = argc > 1 && JS_ToBool(c, argv[1]) == 1;
    const char *err = !app ? "sys.badArgument" : webmgr_set_background(app, on);
    JS_FreeCString(c, app);
    return done(c, err, JS_TRUE);
}

/* The registry file again, with one field changed (webmgr.c writes the others the same way: checked by
 * webreg_write, written atomically, 0600). */
static const char *save_entry(const WebApp *a) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/apps/%s.json", webmgr_config_dir(), a->id) >= (int)sizeof path) return "webapps.writeFailed";
    char *buf = malloc(WEBREG_FILE_MAX + 1);
    size_t n = buf ? webreg_write(a, buf, WEBREG_FILE_MAX + 1) : 0;
    int r = n ? webreg_write_file(path, buf, n) : -1;
    free(buf);
    if (!n) return "webapps.badEntry";
    return r ? "webapps.writeFailed" : NULL;
}

/* the letter in a profile's dot: its name's first character when it is a Latin letter or a digit, else its id's */
static char profile_letter(const WebProfile *p) {
    char c = p->name[0];
    int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    return ok ? c : p->id[0];
}

/* Every profile's icon of app again: the colored ones written, the others removed. The base is the site's own PNG
 * when one is cached, else the registry's PNG (a catalog app's letter tile). Never fails the call it follows. */
static void profile_icons(const WebApp *a) {
    char path[PATH_MAX];
    size_t len = 0;
    unsigned char *base = NULL;
    if (!webicon_cached(home_dir(), a->id, path, sizeof path) && strstr(path, ".png"))
        base = (unsigned char *)webreg_read_file(path, WEBREG_ICON_BYTES, &len);
    if (!base && a->icon[0] && strstr(a->icon, ".png") && snprintf(path, sizeof path, "%s/%s", webmgr_config_dir(), a->icon) < (int)sizeof path)
        base = (unsigned char *)webreg_read_file(path, WEBREG_ICON_BYTES, &len);
    for (int i = 0; i < a->nprofiles; i++) {
        const WebProfile *p = &a->profiles[i];
        if (webicon_profile_write(webmgr_config_dir(), a->id, p->id, base, len, webicon_color_rgb(p->color), profile_letter(p)))
            fprintf(stderr, "webapps: the icon of %s (%s) could not be written\n", a->id, p->id);
    }
    free(base);
}

static void profile_icons_of(const char *app) {
    WebApp *a = malloc(sizeof *a);
    if (a && !webmgr_load(app, a)) profile_icons(a);
    free(a);
}

/* field: 0 tray (value 0/1), 1 notifySound (value WEBREG_SOUND_*) */
static const char *set_field(const char *app, int field, int value) {
    if (!webreg_id_ok(app)) return "webapps.badId";
    WebApp *a = malloc(sizeof *a);
    if (!a) return "webapps.writeFailed";
    const char *e = webmgr_load(app, a);
    int *f = field ? &a->notify_sound : &a->tray;
    if (!e && *f != value) { *f = value; e = save_entry(a); }
    free(a);
    return e;
}

/* profile's color; also a new profile's own one (the first profile none, then orange, purple, blue…) */
static const char *set_color(const char *app, const char *profile, const char *color) {
    if (!webreg_id_ok(app) || !webreg_id_ok(profile)) return "webapps.badId";
    if (color && !webreg_color_ok(color)) return "webapps.badColor";
    WebApp *a = malloc(sizeof *a);
    if (!a) return "webapps.writeFailed";
    const char *e = webmgr_load(app, a);
    int i = e ? -1 : webreg_profile_index(a, profile);
    if (!e && i < 0) e = "webapps.noProfile";
    if (!e) {
        WebProfile *p = &a->profiles[i];
        if (!color) {                                   /* the default for the profile just added */
            p->color[0] = 0;                            /* chosen among the others' colors */
            color = i == 0 ? "none" : webreg_profile_default_color(a);
        }
        if (strcmp(p->color, color)) { snprintf(p->color, sizeof p->color, "%s", color); e = save_entry(a); }
        if (!e) profile_icons(a);
    }
    free(a);
    return e;
}

static JSValue f_set_profile_color(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0), *prof = arg_str(c, argc, argv, 1), *col = arg_str(c, argc, argv, 2);
    const char *err = !app || !prof || !col ? "sys.badArgument" : set_color(app, prof, col);
    JS_FreeCString(c, app);
    JS_FreeCString(c, prof);
    JS_FreeCString(c, col);
    return done(c, err, JS_TRUE);
}

static JSValue f_set_tray(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0);
    int ok = argc > 1 && JS_IsBool(argv[1]);
    const char *err = !app || !ok ? "sys.badArgument" : set_field(app, 0, JS_ToBool(c, argv[1]) == 1);
    JS_FreeCString(c, app);
    return done(c, err, JS_TRUE);
}

static JSValue f_set_notify_sound(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0), *val = arg_str(c, argc, argv, 1);
    int v = val ? webreg_sound_value(val) : -1;
    const char *err = !app || !val ? "sys.badArgument" : v < 0 ? "webapps.badSound" : set_field(app, 1, v);
    JS_FreeCString(c, app);
    JS_FreeCString(c, val);
    return done(c, err, JS_TRUE);
}

/* ---- the site's own icon ---- */

/* the address to probe and the origin its answer must have: the installed entry's, else the catalog's */
static int icon_source(const char *app, char *start, char *origin) {
    WebApp *a = malloc(sizeof *a);
    if (!a) return -1;
    int rc = -1;
    if (!webmgr_load(app, a)) {
        snprintf(start, WEBREG_URL_MAX + 1, "%s", a->start_url);
        snprintf(origin, WEBREG_URL_MAX + 1, "%s", a->origin);
        rc = 0;
    }
    free(a);
    if (!rc) return 0;
    char path[PATH_MAX];
    size_t len;
    if (snprintf(path, sizeof path, "%s/webapps/catalog.json", js_sys_assets()) >= (int)sizeof path) return -1;
    char *t = webreg_read_file(path, 1u << 20, &len);
    JDoc d;
    if (!t || json_parse(t, len, &d, NULL, 0)) { free(t); return -1; }
    const JVal *apps = d.root && d.root->type == J_OBJ ? json_get(d.root, "apps") : NULL;
    for (const JVal *e = apps && apps->type == J_ARR ? apps->kid : NULL; e && rc; e = e->next) {
        const char *id = e->type == J_OBJ ? json_str(e, "id") : NULL, *s = id ? json_str(e, "startUrl") : NULL, *o = id ? json_str(e, "origin") : NULL;
        char got[WEBREG_URL_MAX + 1];
        if (!id || strcmp(id, app) || !s || !o || strlen(s) > WEBREG_URL_MAX || webreg_url_origin(s, got, sizeof got) || strcmp(got, o)) continue;
        snprintf(start, WEBREG_URL_MAX + 1, "%s", s);
        snprintf(origin, WEBREG_URL_MAX + 1, "%s", o);
        rc = 0;
    }
    json_free(&d);
    free(t);
    return rc;
}

#define ICON_FETCHES 4
static struct { char app[WEBREG_ID_MAX + 1]; int probe; char origin[WEBREG_URL_MAX + 1]; } fetches[ICON_FETCHES];

static int fetch_of(const char *app) {
    for (int i = 0; i < ICON_FETCHES; i++) if (fetches[i].probe && !strcmp(fetches[i].app, app)) return i;
    return -1;
}

static JSValue f_fetch_icon(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0);
    if (!app) return js_throw_code(c, "sys.badArgument", NULL);
    const char *err = NULL;
    int id = 0, i = webreg_id_ok(app) ? fetch_of(app) : -1;
    if (!webreg_id_ok(app)) err = "webapps.badId";
    else if (i >= 0) id = fetches[i].probe;              /* already on its way */
    else {
        static char start[WEBREG_URL_MAX + 1], origin[WEBREG_URL_MAX + 1];
        for (i = 0; i < ICON_FETCHES && fetches[i].probe; i++) {}
        if (i == ICON_FETCHES) err = "webapps.tooMany";
        else if (icon_source(app, start, origin)) err = "webapps.notInstalled";
        else if ((id = webmgr_probe(start, &err))) {
            snprintf(fetches[i].app, sizeof fetches[i].app, "%s", app);
            snprintf(fetches[i].origin, sizeof fetches[i].origin, "%s", origin);
            fetches[i].probe = id;
        }
    }
    JS_FreeCString(c, app);
    return done(c, id ? NULL : err ? err : "webapps.probeFailed", JS_NewInt32(c, id));
}

/* the probe's icon file: only one lord-server wrote, in $XDG_RUNTIME_DIR/lordos-web-probe/ */
static int probe_icon_path_ok(const char *p) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char dir[PATH_MAX];
    if (!rt || rt[0] != '/' || !p || snprintf(dir, sizeof dir, "%s/lordos-web-probe/", rt) >= (int)sizeof dir) return 0;
    size_t n = strlen(dir);
    return !strncmp(p, dir, n) && !strchr(p + n, '/') && p[n] && !strstr(p, "..");
}

static JSValue icon_answer(JSContext *c, const char *err, const char *path) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "done", JS_TRUE);
    JS_SetPropertyStr(c, o, "ok", JS_NewBool(c, !err));
    if (err) JS_SetPropertyStr(c, o, "error", JS_NewString(c, err));
    else JS_SetPropertyStr(c, o, "iconPath", JS_NewString(c, path));
    return o;
}

static JSValue f_icon_result(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0);
    if (!app) return js_throw_code(c, "sys.badArgument", NULL);
    int i = webreg_id_ok(app) ? fetch_of(app) : -1;
    JS_FreeCString(c, app);
    if (i < 0) return JS_NULL;
    int gone = 0;
    char *j = webmgr_probe_result(fetches[i].probe, &gone);
    if (!j && !gone) return JS_NULL;                       /* still running */
    const char *err = gone ? "webapps.probeGone" : NULL;
    char kept[PATH_MAX] = "";
    JDoc d;
    if (j && !json_parse(j, strlen(j), &d, NULL, 0)) {
        const JVal *r = d.root;
        const char *e = json_str(r, "error"), *origin = json_str(r, "origin"), *path = json_str(r, "iconPath");
        static char code[48];
        if (!json_bool(r, "ok", 0)) {
            /* the probe's own code (web.probe.…, webapps.probe…: made by lord-server, never the page's text) */
            int known = e && strlen(e) < sizeof code && (!strncmp(e, "web.probe.", 10) || !strncmp(e, "webapps.", 8));
            snprintf(code, sizeof code, "%s", known ? e : "webapps.probeFailed");
            err = code;
        } else if (!err && (!origin || strcmp(origin, fetches[i].origin))) err = "webapps.iconOrigin";   /* it ended on another site */
        else if (!err && (!path || !path[0])) err = "webapps.noIcon";
        else if (!err && !probe_icon_path_ok(path)) err = "webapps.noIcon";
        else if (!err) {
            size_t len = 0;
            unsigned char *b = (unsigned char *)webreg_read_file(path, WEBREG_ICON_BYTES, &len);
            if (!b || webicon_store(home_dir(), fetches[i].app, b, len, kept, sizeof kept)) err = "webapps.noIcon";
            else profile_icons_of(fetches[i].app);         /* the profiles' icons on the new picture */
            free(b);
        }
        json_free(&d);
    } else if (j) err = "webapps.probeFailed";
    free(j);
    memset(&fetches[i], 0, sizeof fetches[i]);
    return icon_answer(c, err, kept);
}

static JSValue f_icons(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue o = JS_NewObject(c);
    const char *h = home_dir();
    char dir[PATH_MAX];
    if (!h || snprintf(dir, sizeof dir, "%s/" WEBICON_DIR, h) >= (int)sizeof dir) return o;
    DIR *dp = opendir(dir);
    if (!dp) return o;
    struct dirent *de;
    int n = 0;
    while ((de = readdir(dp)) && n < 512) {
        char id[WEBREG_ID_MAX + 8], p[PATH_MAX];
        size_t l = strlen(de->d_name);
        if (l < 5 || l > WEBREG_ID_MAX + 4 || (strcmp(de->d_name + l - 4, ".png") && strcmp(de->d_name + l - 4, ".svg"))) continue;
        memcpy(id, de->d_name, l - 4);
        id[l - 4] = 0;
        if (!webreg_id_ok(id) || webicon_cached(h, id, p, sizeof p)) continue;
        JS_SetPropertyStr(c, o, id, JS_NewString(c, p));
        n++;
    }
    closedir(dp);
    return o;
}

static JSValue f_badges(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char *j = webask_badges();
    if (!j) return JS_NewArray(c);
    JSValue v = JS_ParseJSON(c, j, strlen(j), "<badges>");
    free(j);
    return v;
}

static JSValue f_uninstall(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0);
    int del = argc > 1 && JS_ToBool(c, argv[1]) == 1;
    WebApp *a = app ? malloc(sizeof *a) : NULL;
    int loaded = a && !webmgr_load(app, a), from_url = loaded && a->source == WEBREG_SRC_URL;
    const char *err = !app ? "sys.badArgument" : webmgr_uninstall(app, del);
    for (int i = 0; !err && loaded && i < a->nprofiles; i++) webicon_profile_remove(webmgr_config_dir(), app, a->profiles[i].id);
    free(a);
    if (!err && from_url) webicon_remove(home_dir(), app);   /* a catalog app's icon stays for the Store's card */
    JS_FreeCString(c, app);
    return done(c, err, JS_TRUE);
}

static JSValue f_launch(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0), *prof = arg_str(c, argc, argv, 1);
    int raised = 0;
    const char *err = !app || !prof ? "sys.badArgument" : webmgr_launch(app, prof, &raised);
    if (!err && raised) {
        /* the one way a running app comes forward: lord-server's own activation token, handed to its lord-web over
         * the bus (Show); lord-web activates its window with it, or maps it again from behind its tray icon */
        char wid[2 * WEBREG_ID_MAX + 16];
        snprintf(wid, sizeof wid, "lordos-web.%s.%s", app, prof);
        const char *tk = xact_issue(xact_now(), XACT_LAUNCH_MS, 1, wid);
        if (webask_show(app, prof, tk)) fprintf(stderr, "webapps: %s/%s is running but the session bus is not there to show it\n", app, prof);
    }
    JS_FreeCString(c, app);
    JS_FreeCString(c, prof);
    return done(c, err, JS_NewString(c, raised ? "running" : "started"));
}

static JSValue f_running(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    WebRunning r[WEB_MAX_RUNNING];
    int n = webmgr_running(r, WEB_MAX_RUNNING);
    JSValue arr = JS_NewArray(c);
    for (int i = 0; i < n; i++) {
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "app", JS_NewString(c, r[i].app));
        JS_SetPropertyStr(c, o, "profile", JS_NewString(c, r[i].profile));
        JS_SetPropertyStr(c, o, "pid", JS_NewInt32(c, (int32_t)r[i].pid));
        JS_SetPropertyUint32(c, arr, (uint32_t)i, o);
    }
    return arr;
}

static JSValue f_memory(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0);
    if (!app) return js_throw_code(c, "sys.badArgument", NULL);
    if (!webreg_id_ok(app)) { JS_FreeCString(c, app); return js_throw_code(c, "webapps.badId", NULL); }
    WebRunning r[WEB_MAX_RUNNING];
    int n = webmgr_running(r, WEB_MAX_RUNNING), found = 0;
    long long total = 0;
    int procs = 0, unreadable = 0;
    JSValue arr = JS_NewArray(c);
    for (int i = 0; i < n; i++) {
        WebMem m;
        if (strcmp(r[i].app, app) || webmem_tree("/proc", r[i].pid, &m)) continue;
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "profile", JS_NewString(c, r[i].profile));
        JS_SetPropertyStr(c, o, "kb", JS_NewInt64(c, m.pss_kb));
        JS_SetPropertyStr(c, o, "processes", JS_NewInt32(c, m.procs));
        JS_SetPropertyUint32(c, arr, (uint32_t)found++, o);
        total += m.pss_kb;
        procs += m.procs;
        unreadable += m.unreadable;
    }
    JS_FreeCString(c, app);
    if (!found) { JS_FreeValue(c, arr); return JS_NULL; }
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "kb", JS_NewInt64(c, total));
    JS_SetPropertyStr(c, o, "processes", JS_NewInt32(c, procs));
    JS_SetPropertyStr(c, o, "unreadable", JS_NewInt32(c, unreadable));
    JS_SetPropertyStr(c, o, "profiles", arr);
    return o;
}

/* A window says it is lordos-web.<app>.<profile>: only a window of the lord-web we started for that app and
 * profile is shown as that web app (any client can set any app id). */
static JSValue f_owner(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t sid = 0;
    if (argc < 1 || JS_ToInt32(c, &sid, argv[0])) return js_throw_code(c, "sys.badArgument", NULL);
    int pid = wl_surface_pid(sid);
    char app[WEBREG_ID_MAX + 1], prof[WEBREG_ID_MAX + 1];
    if (pid <= 0 || webmgr_owner((pid_t)pid, app, prof)) return JS_NULL;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "app", JS_NewString(c, app));
    JS_SetPropertyStr(c, o, "profile", JS_NewString(c, prof));
    return o;
}

static JSValue f_answer(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t id = 0;
    const char *v = arg_str(c, argc, argv, 1);
    if (argc < 1 || JS_ToInt32(c, &id, argv[0]) || !v) { JS_FreeCString(c, v); return js_throw_code(c, "sys.badArgument", NULL); }
    int r = webask_answer(id, v);
    JS_FreeCString(c, v);
    return JS_NewBool(c, r == 0);
}

/* The shell's Edit menu on a web app's window: the command goes to that app and profile's lord-web (webask_edit),
 * which runs it only when lord-server itself asked. */
static JSValue f_edit(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0), *prof = arg_str(c, argc, argv, 1), *cmd = arg_str(c, argc, argv, 2);
    int ok = app && prof && cmd && webreg_id_ok(app) && webreg_id_ok(prof) && webedit_command(cmd) >= 0;
    int sent = ok && !webask_edit(app, prof, cmd);
    JS_FreeCString(c, app);
    JS_FreeCString(c, prof);
    JS_FreeCString(c, cmd);
    if (!ok) return js_throw_code(c, "sys.badArgument", NULL);
    return JS_NewBool(c, sent);
}

/* The person's choice in a page's context menu (the "webapps-menu" event): one of its items, or "" */
static JSValue f_menu_answer(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t id = 0;
    const char *v = arg_str(c, argc, argv, 1);
    if (argc < 1 || JS_ToInt32(c, &id, argv[0]) || !v) { JS_FreeCString(c, v); return js_throw_code(c, "sys.badArgument", NULL); }
    int r = webask_menu_answer(id, v);
    JS_FreeCString(c, v);
    return JS_NewBool(c, r == 0);
}

static JSValue f_asks(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char *j = webask_pending();
    if (!j) return JS_NewArray(c);
    JSValue v = JS_ParseJSON(c, j, strlen(j), "<asks>");
    free(j);
    return v;
}

static const JSCFunctionListEntry webapps_funcs[] = {
    JS_CFUNC_DEF("available", 0, f_available),
    JS_CFUNC_DEF("list", 0, f_list),
    JS_CFUNC_DEF("get", 1, f_get),
    JS_CFUNC_DEF("probe", 1, f_probe),
    JS_CFUNC_DEF("probeResult", 1, f_probe_result),
    JS_CFUNC_DEF("install", 1, f_install),
    JS_CFUNC_DEF("addProfile", 2, f_add_profile),
    JS_CFUNC_DEF("renameProfile", 3, f_rename_profile),
    JS_CFUNC_DEF("removeProfile", 3, f_remove_profile),
    JS_CFUNC_DEF("setPermission", 3, f_set_permission),
    JS_CFUNC_DEF("setBackground", 2, f_set_background),
    JS_CFUNC_DEF("uninstall", 2, f_uninstall),
    JS_CFUNC_DEF("setTray", 2, f_set_tray),
    JS_CFUNC_DEF("setProfileColor", 3, f_set_profile_color),
    JS_CFUNC_DEF("setNotifySound", 2, f_set_notify_sound),
    JS_CFUNC_DEF("fetchIcon", 1, f_fetch_icon),
    JS_CFUNC_DEF("iconResult", 1, f_icon_result),
    JS_CFUNC_DEF("icons", 0, f_icons),
    JS_CFUNC_DEF("badges", 0, f_badges),
    JS_CFUNC_DEF("launch", 2, f_launch),
    JS_CFUNC_DEF("running", 0, f_running),
    JS_CFUNC_DEF("memory", 1, f_memory),
    JS_CFUNC_DEF("owner", 1, f_owner),
    JS_CFUNC_DEF("answer", 2, f_answer),
    JS_CFUNC_DEF("asks", 0, f_asks),
    JS_CFUNC_DEF("edit", 3, f_edit),
    JS_CFUNC_DEF("menuAnswer", 2, f_menu_answer),
};

/* notifyd's question: is pid one of our lord-web processes (and whose)? */
static int web_of_pid(uint32_t pid, char *app, char *profile) { return pid ? webmgr_owner((pid_t)pid, app, profile) : -1; }
/* the compositor's: is this client a lord-web (a drop's files go to a page there)? */
static int web_pid(int pid) {
    char app[WEBREG_ID_MAX + 1], profile[WEBREG_ID_MAX + 1];
    return pid > 0 && !webmgr_owner((pid_t)pid, app, profile);
}

void js_webapps_register(JSContext *c, JSValue lord) {
    webmgr_init(js_sys_assets());
    notifyd_set_web_of_pid(web_of_pid);
    wl_set_web_pid(web_pid);
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, webapps_funcs, sizeof webapps_funcs / sizeof webapps_funcs[0]);
    JS_SetPropertyStr(c, lord, "webapps", o);
}
