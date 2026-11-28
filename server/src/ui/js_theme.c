/* js_theme.c — community themes in a session (js_theme.h). */
#include "js_theme.h"
#include "js_err.h"
#include "themestore.h"
#include "ui.h"
#include "../fsops.h"
#include "../sec/json.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static struct {
    int active;
    unsigned caps;
    JSContext *realm;
    int started, stopped;
    char failure[256];
} S;

/* ---- the folder ---- */

const char *jst_root(void) {
    static char root[PATH_MAX];
    if (!root[0]) {
        const char *home = getenv("HOME");
        if (home && home[0] == '/' && strlen(home) < PATH_MAX - 40) snprintf(root, sizeof root, "%s/.local/share/lordos/themes", home);
    }
    return root;
}

int jst_bundle_path(const char *id, char *out, size_t n) {
    if (!ts_valid_id(id) || !jst_root()[0]) return -1;
    return (size_t)snprintf(out, n, "%s/%s/theme.js", jst_root(), id) < n ? 0 : -1;
}

int jst_is_user_bundle(const char *path, char *id, size_t n) {
    const char *root = jst_root();
    size_t rl = strlen(root);
    if (!rl || strncmp(path, root, rl) || path[rl] != '/') return 0;
    const char *p = path + rl + 1, *s = strchr(p, '/');
    if (!s || strcmp(s, "/theme.js") || (size_t)(s - p) >= n) return 0;
    memcpy(id, p, (size_t)(s - p));
    id[s - p] = 0;
    return ts_valid_id(id);
}

/* the folder, made 0700 on first use (each level), as a descriptor */
static int root_fd(int make) {
    const char *root = jst_root();
    if (!root[0]) return -1;
    if (make) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s", root);
        for (char *s = strchr(p + 1, '/'); s; s = strchr(s + 1, '/')) {
            *s = 0;
            mkdir(p, 0700);
            *s = '/';
        }
        mkdir(p, 0700);
    }
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 022)) { close(fd); return -1; }
    return fd;
}

/* ---- the mode ---- */

void jst_begin(unsigned caps) {
    S.active = 1;
    S.caps = caps;
    S.realm = NULL;
    S.started = 0;
    S.stopped = 0;
    S.failure[0] = 0;
}
void jst_set_realm(JSContext *realm) {
    /* its jobs, and the calls made on its behalf, run tainted (QuickJS patch, JS_StackHasRealm) */
    if (realm) JS_SetUntrustedRealm(JS_GetRuntime(realm), realm);
    else if (S.realm) JS_SetUntrustedRealm(JS_GetRuntime(S.realm), NULL);
    S.realm = realm;
    if (!realm) S.active = 0;
}
JSContext *jst_realm(void) { return S.realm; }
int jst_active(void) { return S.active; }
unsigned jst_caps(void) { return S.caps; }
int jst_started(void) { return S.started; }
void jst_note_stop(void) { if (S.active) S.stopped = 1; }
int jst_stopped(void) { return S.stopped; }
const char *jst_failure(void) { return S.failure[0] ? S.failure : NULL; }

/* ---- the policy ----
 * What a community theme may reach while its code is on the stack (directly, or through the SDK it called). Exact
 * names, or a prefix ending in '.'. Everything else is refused: the terminal, files, administrator actions, permission
 * answers, packages, launching programs other than through the shell's app list, the keyring, the clipboard, the
 * network's settings, theme switching, the session's power. Capabilities give read access only (approved list,
 * 2026-11-26): no setter of Wi-Fi, Bluetooth, sound, brightness or media is here. */
static const struct { const char *name; unsigned cap; int policy; } rules[] = {
    /* the host's own natives (jst_register_main): never in the theme's realm, and called by the facade on its behalf */
    { "themeRealmFn", 0, JST_ALLOW }, { "themeCall", 0, JST_ALLOW }, { "themeOwn", 0, JST_ALLOW },
    { "hostMicrotask", 0, JST_ALLOW },
    { "themes.started", 0, JST_ALLOW }, { "themes.fail", 0, JST_ALLOW }, { "themes.quitByTheme", 0, JST_ALLOW },
    /* drawing and measuring its own nodes */
    { "getLayout", 0, JST_ALLOW }, { "now", 0, JST_ALLOW }, { "log", 0, JST_ALLOW }, { "measureText", 0, JST_ALLOW },
    /* the prelude's timers keep their origin (hands.h HANDS_ORIGIN_*): taken and deleted before any bundle runs */
    { "__origin", 0, JST_ALLOW }, { "__originSet", 0, JST_ALLOW },
    { "stats", 0, JST_ALLOW }, { "setStyle", 0, JST_NODE }, { "setProp", 0, JST_NODE_PROP }, { "setText", 0, JST_NODE },
    /* text, pictures, fonts and translations (read only) */
    { "text.", 0, JST_ALLOW }, { "image.", 0, JST_ALLOW }, { "fonts.list", 0, JST_ALLOW },
    { "locale.catalog", 0, JST_ALLOW }, { "locale.languages", 0, JST_ALLOW },
    { "time.", 0, JST_ALLOW }, { "system.info", 0, JST_ALLOW }, { "system.seen", 0, JST_ALLOW },
    { "system.load", 0, JST_ALLOW }, { "system.cpuTimes", 0, JST_ALLOW },
    { "themes.list", 0, JST_ALLOW }, { "admin.available", 0, JST_ALLOW }, { "studio.status", 0, JST_ALLOW }, { "guestwin.active", 0, JST_ALLOW },
    /* windows: moving and resizing in C, focus, the work areas, Linux windows' frames */
    { "wm.", 0, JST_ALLOW }, { "wayland.focus", 0, JST_ALLOW }, { "wayland.close", 0, JST_ALLOW },
    { "wayland.state", 0, JST_ALLOW }, { "wayland.workAreas", 0, JST_ALLOW }, { "wayland.hasKeyboard", 0, JST_ALLOW },
    { "wayland.fullscreen", 0, JST_ALLOW }, { "wayland.icon", 0, JST_ALLOW }, { "wayland.info", 0, JST_ALLOW },
    { "wayland.effects", 0, JST_ALLOW }, { "wayland.sessionFrame", 0, JST_ALLOW },
    /* launching apps through the shell's list (the launcher's own rules in apps.c; no file, no arguments) */
    { "apps.list", 0, JST_ALLOW }, { "apps.launch", 0, JST_ALLOW }, { "apps.actions", 0, JST_ALLOW },
    { "apps.launchPackage", 0, JST_ALLOW }, { "apps.packages", 0, JST_ALLOW }, { "apps.packagedStamp", 0, JST_ALLOW },
    { "keyboard.current", 0, JST_ALLOW }, { "keyboard.list", 0, JST_ALLOW },
    { "input.pointerAccent", 0, JST_ALLOW },
    /* a style pack (sdk/appskins.jsx, docs/THEMES-GUIDE.md "Style packs"): data only, checked by lord-server: two pointer
     * colours, tunes of plain notes, an installed font family by name; nothing is read from the theme */
    { "input.pointerScheme", 0, JST_ALLOW }, { "sound.scheme", 0, JST_ALLOW }, { "fonts.useForTheme", 0, JST_ALLOW },
    /* the session: lock and the 60 s power confirmations the SDK draws (sdk/power.jsx) */
    { "session.lock", 0, JST_ALLOW }, { "quit", 0, JST_ALLOW }, { "session.managed", 0, JST_ALLOW }, { "session.onMonitor", 0, JST_ALLOW },
    { "session.status", 0, JST_ALLOW },
    /* capabilities: read access to a system model */
    { "net.status", TS_CAP_WIFI, JST_ALLOW }, { "net.interfaces", TS_CAP_WIFI, JST_ALLOW },
    { "net.wifiList", TS_CAP_WIFI, JST_ALLOW }, { "net.wifi", TS_CAP_WIFI, JST_ALLOW },
    { "bt.state", TS_CAP_BLUETOOTH, JST_ALLOW }, { "bt.devices", TS_CAP_BLUETOOTH, JST_ALLOW },
    { "bt.status", TS_CAP_BLUETOOTH, JST_ALLOW },
    { "sound.state", TS_CAP_SOUND, JST_ALLOW }, { "sound.sinks", TS_CAP_SOUND, JST_ALLOW },
    { "sound.sources", TS_CAP_SOUND, JST_ALLOW }, { "sound.status", TS_CAP_SOUND, JST_ALLOW },
    { "power.battery", TS_CAP_BATTERY, JST_ALLOW }, { "power.status", TS_CAP_BATTERY, JST_ALLOW },
    { "display.backlight", TS_CAP_BRIGHTNESS, JST_ALLOW }, { "display.brightness", TS_CAP_BRIGHTNESS, JST_ALLOW },
    { "mpris.list", TS_CAP_MEDIA, JST_ALLOW }, { "mpris.state", TS_CAP_MEDIA, JST_ALLOW },
};

int jst_policy(const char *name) {
    int best = JST_DENY;
    size_t best_len = 0;
    for (size_t i = 0; i < sizeof rules / sizeof rules[0]; i++) {
        const char *r = rules[i].name;
        size_t l = strlen(r);
        int hit = r[l - 1] == '.' ? !strncmp(name, r, l) : !strcmp(name, r);
        if (!hit || l < best_len) continue;
        if (rules[i].cap && !(S.caps & rules[i].cap)) { best = JST_DENY; best_len = l; continue; }
        best = rules[i].policy;
        best_len = l;
    }
    return best;
}

/* setProp keys a theme never sets on a node (another app's terminal, window, video or pixels) */
static int prop_blocked(JSContext *c, JSValueConst key) {
    const char *k = JS_ToCString(c, key);
    int bad = !k || !strcmp(k, "session") || !strcmp(k, "surface") || !strcmp(k, "media") || !strcmp(k, "crop") ||
              !strcmp(k, "pixels") || !strcmp(k, "wave");
    if (k) JS_FreeCString(c, k);
    return bad;
}

static void refused(const char *name, const char *why) {
    static int lines;
    if (lines < 64) {
        lines++;
        fprintf(stderr, "lord-server: community theme: lord.%s refused (%s)%s\n", name, why,
                lines == 64 ? " (further refusals are not logged)" : "");
    }
}

/* a picture a theme's own node may show: assets/<…> of its folder, LordOS's asset:<…> or the person's wallpaper
 * (jst_wallpaper_file), never ".." or another path */
static int src_ok(JSContext *c, JSValueConst v) {
    if (JS_IsNull(v) || JS_IsUndefined(v)) return 1;
    const char *s = JS_ToCString(c, v);
    int ok = s && !strstr(s, "..") && (!strncmp(s, "assets/", 7) || !strncmp(s, "asset:", 6) || jst_wallpaper_file(s, fsops_home())) && strlen(s) < 512;
    for (const char *p = s ? s : ""; ok && *p; p++) if ((unsigned char)*p < 0x20) ok = 0;
    if (s) JS_FreeCString(c, s);
    return ok;
}

int jst_prop_guard(JSContext *c, int argc, JSValueConst *argv) {
    if (!S.active) return 0;
    int32_t id = -1;
    Node *n = argc > 0 && !JS_ToInt32(c, &id, argv[0]) ? node_get(id) : NULL;
    if (!n || !n->theme_own) return 0;
    const char *k = argc > 1 ? JS_ToCString(c, argv[1]) : NULL;
    int bad = !k || prop_blocked(c, argv[1]) || (!strcmp(k, "src") && !src_ok(c, argc > 2 ? argv[2] : JS_UNDEFINED));
    if (k) JS_FreeCString(c, k);
    if (!bad) return 0;
    refused("setProp", "a property a theme's own node can't have");
    js_throw_code(c, "theme.notAllowed", "call", "setProp", NULL);
    return -1;
}

int jst_on_stack(JSContext *c) { return S.active && S.realm && JS_StackHasRealm(c, S.realm); }

static int check(JSContext *c, int policy, const char *name, int argc, JSValueConst *argv, int force) {
    if (!S.active || policy == JST_ALLOW || !S.realm) return 0;
    if (!force && !JS_StackHasRealm(c, S.realm)) return 0;
    if (policy == JST_NODE || policy == JST_NODE_PROP) {
        int32_t id = -1;
        Node *n = argc > 0 && !JS_ToInt32(c, &id, argv[0]) ? node_get(id) : NULL;
        if (n && n->theme_own && !(n->protect) && !(!strcmp(name, "setProp") && (argc < 2 || prop_blocked(c, argv[1])))) return 0;
        char why[64];
        snprintf(why, sizeof why, "node %d is not one of the theme's own", (int)id);
        refused(name, why);
    } else refused(name, "not available to community themes");
    js_throw_code(c, "theme.notAllowed", "call", name, NULL);
    return -1;
}

int jst_check(JSContext *c, int policy, const char *name, int argc, JSValueConst *argv) {
    return check(c, policy, name, argc, argv, 0);
}

/* ---- the theme's own lord ----
 * Natives of the theme's realm, each holding (out of JavaScript's reach) LordOS's guarded function and its name. They
 * always apply the policy, whatever the stack: a theme that hands one to a timer, a promise, a bound function or a
 * callback LordOS calls later gets the same answer as when it calls it itself. */
static JSValue theme_native(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)this_val; (void)magic;
    const char *name = JS_ToCString(c, data[1]);
    int pol = name ? jst_policy(name) : JST_DENY;
    int r = check(c, pol, name ? name : "?", argc, argv, 1);
    if (name) JS_FreeCString(c, name);
    if (r) return JS_EXCEPTION;
    JS_RealmTaint(JS_GetRuntime(c), 1);
    JSValue v = JS_Call(c, data[0], JS_UNDEFINED, argc, argv);
    JS_RealmTaint(JS_GetRuntime(c), -1);
    return v;
}

JSValue jst_theme_lord(JSContext *t, JSContext *main, JSValueConst main_lord) {
    static const char *const names[] = { "getLayout", "setStyle", "measureText", "now", "log", NULL };
    JSValue o = JS_NewObject(t);
    for (int i = 0; names[i]; i++) {
        JSValue f = JS_GetPropertyStr(main, main_lord, names[i]);
        if (!JS_IsFunction(main, f)) { JS_FreeValue(main, f); continue; }
        JSValue data[2] = { f, JS_NewString(t, names[i]) };
        JS_SetPropertyStr(t, o, names[i], JS_NewCFunctionData(t, theme_native, 3, 0, 2, data));
        JS_FreeValue(t, data[1]);
        JS_FreeValue(main, f);
    }
    return o;
}

/* ---- the main realm's natives in a community session ---- */

/* lord.themeRealmFn(f): 2 the theme's own function, 1 anything that can't be told to be LordOS's code (a native of the
 * theme's realm, a bound function, a proxy), 0 LordOS's (React's host guard and the SDK's facade ask) */
static JSValue f_realm_fn(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    return JS_NewInt32(c, argc > 0 && S.realm ? JS_IsFunctionOfRealm(c, argv[0], S.realm) : 0);
}
/* lord.themeCall(f, ...args): LordOS's code calls something the theme gave it (a callback, a timer, an effect): the call
 * runs tainted, so every native it reaches is under the theme's policy even with no frame of the theme's on the stack */
static JSValue f_theme_call(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsFunction(c, argv[0])) return JS_UNDEFINED;
    JS_RealmTaint(JS_GetRuntime(c), 1);
    JSValue v = JS_Call(c, argv[0], JS_UNDEFINED, argc - 1, argv + 1);
    JS_RealmTaint(JS_GetRuntime(c), -1);
    return v;
}
/* lord.hostMicrotask(fn): React's own scheduleMicrotask (runtime/index.js) while a community theme runs. A state update
 * the theme makes (a click's setState, a timer) queues React's render as a microtask from a tainted call, and a job queued
 * then would run tainted: React's commit of the SDK's own nodes (lord.createNode, removeChild, destroy) would be refused
 * and the theme taken off at its first click that opens something. The render is LordOS's code, as at the theme's start:
 * every function of the theme it reaches still runs through lord.themeCall, and React's host guard still checks every
 * element. Only a function of LordOS's realm is queued (one of the theme's, or a bound or native one, is refused), and
 * QuickJS still taints the job if its function isn't trusted bytecode. Never in the theme's realm. */
static JSValue job_call(JSContext *c, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    return JS_Call(c, argv[0], JS_UNDEFINED, 0, NULL);
}
static JSValue f_host_microtask(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsFunction(c, argv[0])) return js_throw_code(c, "theme.notAllowed", "call", "hostMicrotask", NULL);
    if (S.realm && JS_IsFunctionOfRealm(c, argv[0], S.realm)) return js_throw_code(c, "theme.notAllowed", "call", "hostMicrotask", NULL);
    if (JS_EnqueueTrustedJob(c, job_call, 1, argv) < 0) return JS_EXCEPTION;
    return JS_TRUE;                 /* queued (runtime/index.js queues it itself otherwise) */
}
/* lord.themeOwn(id): this node is one of the theme's own (made by the facade's View, Text or Image) */
static JSValue f_theme_own(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t id = -1;
    Node *n = argc > 0 && !JS_ToInt32(c, &id, argv[0]) ? node_get(id) : NULL;
    if (n) n->theme_own = 1;
    return JS_UNDEFINED;
}
/* lord.themes.started(): the theme's tree committed (the host's commit watcher) */
static JSValue f_started(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    S.started = 1;
    return JS_UNDEFINED;
}
/* lord.themes.fellBack(): true once when this desktop was loaded because the theme before it failed or was left
 * (main.c community_back, switch_theme): session restore then takes that theme's unclean marker as no crash */
static JSValue f_fell_back(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    int v = ui.theme_fell_back;
    ui.theme_fell_back = 0;
    return JS_NewBool(c, v);
}
/* lord.themes.quitByTheme(): the facade's session.quit, before LordOS's own code logs out for the theme (sdk/themeapi.js):
 * this end is the theme's, not the person's, so its crash count stays (main.c) */
static JSValue f_quit_by_theme(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    ui.quit_by_theme = 1;
    return JS_UNDEFINED;
}
/* lord.themes.fail(why): the theme's tree died (React's uncaught error): the watchdog restores the previous theme */
static JSValue f_fail(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *w = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    if (!S.failure[0]) {
        snprintf(S.failure, sizeof S.failure, "%s", w && w[0] ? w : "the theme stopped drawing");
        for (char *p = S.failure; *p; p++) if ((unsigned char)*p < 0x20) *p = ' ';
    }
    if (w) JS_FreeCString(c, w);
    return JS_UNDEFINED;
}

void jst_register_main(JSContext *c, JSValue lord) {
    JS_SetPropertyStr(c, lord, "themeRealmFn", JS_NewCFunction(c, f_realm_fn, "themeRealmFn", 1));
    JS_SetPropertyStr(c, lord, "themeOwn", JS_NewCFunction(c, f_theme_own, "themeOwn", 1));
    JS_SetPropertyStr(c, lord, "themeCall", JS_NewCFunction(c, f_theme_call, "themeCall", 1));
    JS_SetPropertyStr(c, lord, "hostMicrotask", JS_NewCFunction(c, f_host_microtask, "hostMicrotask", 1));
    JSValue th = JS_GetPropertyStr(c, lord, "themes");
    if (JS_IsObject(th)) {
        JS_SetPropertyStr(c, th, "started", JS_NewCFunction(c, f_started, "started", 0));
        JS_SetPropertyStr(c, th, "fail", JS_NewCFunction(c, f_fail, "fail", 1));
        JS_SetPropertyStr(c, th, "quitByTheme", JS_NewCFunction(c, f_quit_by_theme, "quitByTheme", 0));
    }
    JS_FreeValue(c, th);
}

/* ---- the list ---- */

static JSValue caps_array(JSContext *c, unsigned caps) {
    JSValue a = JS_NewArray(c);
    uint32_t k = 0;
    for (unsigned b = 1; b && b <= TS_CAP_NOTIFICATIONS; b <<= 1)
        if ((caps & b) && ts_cap_name(b)) JS_SetPropertyUint32(c, a, k++, JS_NewString(c, ts_cap_name(b)));
    return a;
}

static char *small_file(int dir, const char *name, size_t max) {
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    char *b = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && (size_t)st.st_size <= max && (b = malloc((size_t)st.st_size + 1))) {
        ssize_t r = read(fd, b, (size_t)st.st_size);
        if (r != st.st_size) { free(b); b = NULL; } else b[r] = 0;
    }
    close(fd);
    return b;
}

void jst_list(JSContext *c, JSValue arr, uint32_t *i, const char *current_bundle) {
    int rfd = root_fd(0);
    if (rfd < 0) return;
    int dup = fcntl(rfd, F_DUPFD_CLOEXEC, 3);
    DIR *d = dup >= 0 ? fdopendir(dup) : NULL;
    if (!d) { if (dup >= 0) close(dup); close(rfd); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        TsRecord r;
        if (!ts_valid_id(e->d_name) || ts_record_read(rfd, e->d_name, &r)) continue;
        int tfd = openat(rfd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        char *meta = tfd >= 0 ? small_file(tfd, "theme.json", 4096) : NULL;
        if (tfd >= 0) close(tfd);
        char bundle[PATH_MAX];
        jst_bundle_path(e->d_name, bundle, sizeof bundle);
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "id", JS_NewString(c, e->d_name));
        JS_SetPropertyStr(c, o, "meta", meta ? JS_NewString(c, meta) : JS_NULL);
        JS_SetPropertyStr(c, o, "current", JS_NewBool(c, current_bundle && !strcmp(current_bundle, bundle)));
        JS_SetPropertyStr(c, o, "user", JS_TRUE);
        JS_SetPropertyStr(c, o, "trusted", JS_NewBool(c, r.trusted));
        JS_SetPropertyStr(c, o, "installedAuthor", JS_NewString(c, r.author));
        JS_SetPropertyStr(c, o, "caps", caps_array(c, r.caps));
        int session_failed = !strcmp(ui.theme_failed, bundle);
        if (r.failed[0] || session_failed) {
            JS_SetPropertyStr(c, o, "failed", JS_TRUE);
            JS_SetPropertyStr(c, o, "failedWhy", JS_NewString(c, session_failed && ui.theme_failed_why[0] ? ui.theme_failed_why : r.failed));
        }
        JS_SetPropertyUint32(c, arr, (*i)++, o);
        free(meta);
    }
    closedir(d);
    close(rfd);
}

char *jst_load_code(const char *id, size_t *len, unsigned *caps, char *why, size_t why_n) {
    int rfd = root_fd(0);
    TsRecord r;
    char *code = NULL;
    if (rfd < 0 || ts_record_read(rfd, id, &r)) snprintf(why, why_n, "not installed");
    else if (!r.trusted) snprintf(why, why_n, "not confirmed by the person");
    else if (r.failed[0]) snprintf(why, why_n, "%s", r.failed);
    else if (!(code = ts_code_read(rfd, &r, len))) snprintf(why, why_n, "its code does not match what LordOS built");
    else *caps = r.caps;
    if (rfd >= 0) close(rfd);
    return code;
}

void jst_mark_failed(const char *id, const char *why) {
    int rfd = root_fd(0);
    TsRecord r;
    if (rfd >= 0 && !ts_record_read(rfd, id, &r)) {
        snprintf(r.failed, sizeof r.failed, "%s", why && why[0] ? why : "it failed");
        for (char *p = r.failed; *p; p++) if ((unsigned char)*p < 0x20) *p = ' ';
        ts_record_write(rfd, &r);
        fprintf(stderr, "lord-server: community theme %s marked failed: %s\n", id, r.failed);
    }
    if (rfd >= 0) close(rfd);
}

#define CRASHES_MAX 2

int jst_session_starting(const char *id) {
    int rfd = root_fd(0), over = 0;
    TsRecord r;
    if (rfd >= 0 && !ts_record_read(rfd, id, &r)) {
        if (r.crashes >= CRASHES_MAX) {
            snprintf(r.failed, sizeof r.failed, "the session ended %d times in a row while it ran", r.crashes);
            over = 1;
        } else r.crashes++;
        ts_record_write(rfd, &r);
        fprintf(stderr, "lord-server: community theme %s starting (%d session(s) in a row ended abnormally before)\n", id, r.crashes - !over);
    }
    if (rfd >= 0) close(rfd);
    return over;
}

void jst_session_ended(const char *id) {
    int rfd = root_fd(0);
    TsRecord r;
    if (rfd >= 0 && !ts_record_read(rfd, id, &r) && r.crashes) {
        r.crashes = 0;
        ts_record_write(rfd, &r);
    }
    if (rfd >= 0) close(rfd);
}

void jst_session_healthy(const char *id) {
    int rfd = root_fd(0);
    TsRecord r;
    if (rfd >= 0 && !ts_record_read(rfd, id, &r) && r.crashes) {
        r.crashes = 0;
        ts_record_write(rfd, &r);
    }
    if (rfd >= 0) close(rfd);
}

/* ---- lord.themes.install: lord-themec builds the theme from its .zip ---- */

#define JOBS 4
#define ZIP_MAX (32u << 20)
#define OUT_MAX (TS_TOTAL_MAX + (1u << 20))

static struct Job {
    int used, done, fd;
    pid_t pid;
    uint8_t *out;
    size_t n, cap;
    char result[16384];                 /* the JSON lord.themes.installJob answers when done */
} jobs[JOBS];

static int helper_path(char *out, size_t n) {
    ssize_t l = readlink("/proc/self/exe", out, n - 32);
    if (l <= 0) return -1;
    out[l] = 0;
    char *s = strrchr(out, '/');
    if (!s) return -1;
    snprintf(s + 1, n - (size_t)(s + 1 - out), "lord-themec");
    struct stat st;
    return stat(out, &st) || !S_ISREG(st.st_mode) || (st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 022) ? -1 : 0;
}

/* the zip the person chose: a readable regular file lord.fs could read, opened without following a link */
static int open_zip(const char *path) {
    char real[PATH_MAX];
    if (!path || path[0] != '/' || !realpath(path, real) || !fsops_readable(real)) return -1;
    int fd = open(real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (size_t)st.st_size > ZIP_MAX) { close(fd); return -1; }
    fcntl(fd, F_SETFL, 0);
    return fd;
}

static JSValue f_install(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int slot = -1;
    for (int k = 0; k < JOBS; k++) if (!jobs[k].used) { slot = k; break; }
    char bin[PATH_MAX];
    const char *path = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    int zfd = slot >= 0 && !helper_path(bin, sizeof bin) ? open_zip(path) : -1;
    if (path) JS_FreeCString(c, path);
    if (zfd < 0) return JS_NewInt32(c, 0);
    int out[2];
    if (pipe2(out, O_CLOEXEC)) { close(zfd); return JS_NewInt32(c, 0); }
    pid_t pid = fork();
    if (!pid) {
        int a = fcntl(zfd, F_DUPFD, 10), b = fcntl(out[1], F_DUPFD, 10), null = open("/dev/null", O_WRONLY);
        if (a < 0 || b < 0 || null < 0 || dup2(a, 0) != 0 || dup2(b, 1) != 1 || dup2(null, 2) != 2) _exit(127);
        syscall(SYS_close_range, 3, ~0u, 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        char *const args[] = { "lord-themec", NULL }, *const envp[] = { NULL };
        execve(bin, args, envp);
        _exit(127);
    }
    close(zfd);
    close(out[1]);
    if (pid < 0) { close(out[0]); return JS_NewInt32(c, 0); }
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    jobs[slot] = (struct Job){ .used = 1, .fd = out[0], .pid = pid };
    return JS_NewInt32(c, slot + 1);
}

typedef struct { const uint8_t *meta; size_t meta_n; } Peek;
static int peek_meta(void *ud, const char *name, const uint8_t *data, size_t len) {
    Peek *p = ud;
    if (!strcmp(name, "theme.json")) { p->meta = data; p->meta_n = len; }
    return 0;
}

/* the system's themes folder (main.c: next to the theme lord-sessiond asked for) */
static char sys_themes[PATH_MAX];
void jst_set_system_themes(const char *dir) { snprintf(sys_themes, sizeof sys_themes, "%s", dir ? dir : ""); }
const char *jst_system_themes(void) { return sys_themes; }

/* a system theme with this id (official themes win: an installed theme never shadows one) */
static int system_theme(const char *id) {
    char p[PATH_MAX * 2];
    snprintf(p, sizeof p, "%s/%s/theme.bundle.js", sys_themes, id);
    return sys_themes[0] && access(p, F_OK) == 0;
}

static void finish(struct Job *j) {
    char why[200] = "", report[12288] = "";
    Peek pk = { 0 };
    int ok = j->n > 0 && !ts_stream_parse(j->out, j->n, report, sizeof report, peek_meta, &pk, why, sizeof why);
    char id[TS_ID_MAX + 1] = "";
    const char *err = NULL;
    TsRecord rec;
    memset(&rec, 0, sizeof rec);
    if (!ok) err = j->n ? "helperAnswer" : "helperFailed";
    else if (!pk.meta) err = "checks";               /* refused by the checks: the report says why */
    else {
        JDoc rd, md;
        int rok = !json_parse(report, strlen(report), &rd, NULL, 0);
        const char *rid = rok ? json_str(rd.root, "id") : NULL;
        if (rid && ts_valid_id(rid)) snprintf(id, sizeof id, "%s", rid);
        if (rok) json_free(&rd);
        if (!id[0]) err = "helperAnswer";
        else if (system_theme(id)) err = "officialId";
        else if (json_parse((const char *)pk.meta, pk.meta_n, &md, NULL, 0)) err = "checks";
        else {
            const char *a = json_str(md.root, "author"), *v = json_str(md.root, "version");
            snprintf(rec.id, sizeof rec.id, "%s", id);
            if (a) snprintf(rec.author, sizeof rec.author, "%s", a);
            if (v) snprintf(rec.version, sizeof rec.version, "%s", v);
            const JVal *caps = json_get(md.root, "capabilities");
            if (caps && caps->type == J_ARR)
                for (const JVal *x = caps->kid; x; x = x->next) if (x->type == J_STR) rec.caps |= ts_cap_bit(x->str);
            json_free(&md);
            char back[1024];
            TsRecord chk = rec;
            snprintf(chk.sha256, sizeof chk.sha256, "%064d", 0);
            if (!ts_record_format(&chk, back, sizeof back) || ts_record_parse(back, strlen(back), &chk)) err = "checks";
            else {
                rec = chk;
                rec.trusted = 0;
                int rfd = root_fd(1);
                if (rfd < 0 || ts_install(rfd, j->out, j->n, &rec, why, sizeof why)) err = "write";
                if (rfd >= 0) close(rfd);
            }
        }
    }
    char qwhy[sizeof why * 6 + 3];
    json_quote(qwhy, sizeof qwhy, why);
    /* the report is the helper's JSON (data for Settings to show); it is passed on only when it parses */
    JDoc chk;
    int rep_ok = report[0] && !json_parse(report, strlen(report), &chk, NULL, 0);
    if (rep_ok) json_free(&chk);
    snprintf(j->result, sizeof j->result, "{\"done\":true,\"ok\":%s,\"id\":\"%s\",\"error\":%s%s%s,\"why\":%s,\"report\":%s}",
             err ? "false" : "true", id, err ? "\"" : "null", err ? err : "", err ? "\"" : "", qwhy, rep_ok ? report : "null");
    if (!err) fprintf(stderr, "lord-server: community theme %s installed (author \"%s\")\n", id, rec.author);
    else fprintf(stderr, "lord-server: a community theme was not installed: %s %s\n", err, why);
}

static JSValue f_install_job(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t i = 0;
    if (argc < 1 || JS_ToInt32(c, &i, argv[0]) || i < 1 || i > JOBS || !jobs[i - 1].used) return JS_NULL;
    struct Job *j = &jobs[i - 1];
    if (!j->done) {
        for (;;) {
            if (j->cap - j->n < 65536) {
                size_t nc = j->cap ? j->cap * 2 : 1u << 20;
                if (nc > OUT_MAX) nc = OUT_MAX;
                if (nc <= j->cap) { kill(j->pid, SIGKILL); break; }
                uint8_t *nb = realloc(j->out, nc);
                if (!nb) { kill(j->pid, SIGKILL); break; }
                j->out = nb;
                j->cap = nc;
            }
            ssize_t r = read(j->fd, j->out + j->n, j->cap - j->n);
            if (r > 0) { j->n += (size_t)r; continue; }
            if (r < 0 && errno == EINTR) continue;
            if (r < 0 && errno == EAGAIN) return JS_ParseJSON(c, "{\"done\":false}", 14, "<themes>");
            break;
        }
        int st = 0;
        waitpid(j->pid, &st, 0);
        close(j->fd);
        if (!WIFEXITED(st) || WEXITSTATUS(st) > 1) j->n = 0;    /* killed, crashed or bad usage: no answer */
        finish(j);
        free(j->out);
        j->out = NULL;
        j->done = 1;
    }
    JSValue v = JS_ParseJSON(c, j->result, strlen(j->result), "<themes>");
    j->used = 0;
    return v;
}

static JSValue f_trust(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    int ok = 0, rfd = root_fd(0);
    TsRecord r;
    if (id && rfd >= 0 && !ts_record_read(rfd, id, &r)) {
        r.trusted = 1;
        ok = !ts_record_write(rfd, &r);
        if (ok) fprintf(stderr, "lord-server: community theme %s confirmed by the person\n", id);
    }
    if (rfd >= 0) close(rfd);
    if (id) JS_FreeCString(c, id);
    return JS_NewBool(c, ok);
}

/* lord.themes.retry(id): a failed theme may be tried again (the person asked, in Settings) */
static JSValue f_retry(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    int ok = 0, rfd = root_fd(0);
    TsRecord r;
    if (id && rfd >= 0 && !ts_record_read(rfd, id, &r)) {
        r.failed[0] = 0;
        r.crashes = 0;
        ok = !ts_record_write(rfd, &r);
        char b[PATH_MAX];
        if (ok && !jst_bundle_path(id, b, sizeof b) && !strcmp(ui.theme_failed, b)) ui.theme_failed[0] = 0;
    }
    if (rfd >= 0) close(rfd);
    if (id) JS_FreeCString(c, id);
    return JS_NewBool(c, ok);
}

static JSValue f_remove(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    int ok = 0, rfd = root_fd(0);
    char b[PATH_MAX];
    if (id && rfd >= 0 && ts_valid_id(id) && !jst_bundle_path(id, b, sizeof b)) {
        char cur[sizeof ui.theme_dir + 16];
        snprintf(cur, sizeof cur, "%s/theme.js", ui.theme_dir);
        if (strcmp(cur, b)) ok = !ts_remove(rfd, id);       /* never the running one */
    }
    if (rfd >= 0) close(rfd);
    if (id) JS_FreeCString(c, id);
    return JS_NewBool(c, ok);
}

static const JSCFunctionListEntry user_funcs[] = {
    JS_CFUNC_DEF("install", 1, f_install),
    JS_CFUNC_DEF("installJob", 1, f_install_job),
    JS_CFUNC_DEF("trust", 1, f_trust),
    JS_CFUNC_DEF("retry", 1, f_retry),
    JS_CFUNC_DEF("fellBack", 0, f_fell_back),
    JS_CFUNC_DEF("remove", 1, f_remove),
};

void jst_register_themes(JSContext *c, JSValue themes) {
    JS_SetPropertyFunctionList(c, themes, user_funcs, sizeof user_funcs / sizeof user_funcs[0]);
}
