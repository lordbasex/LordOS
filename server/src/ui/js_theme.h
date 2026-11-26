/* js_theme.h — community themes in a session (docs/THEMES-GUIDE.md, "Community themes"; AGENTS.md "Community themes").
 *
 * A community theme is source a person installed from a .zip; LordOS built its code (lord-themec) into the per-user
 * themes folder (themestore.h). It runs in a second QuickJS realm of the session's runtime with a restricted `lord`,
 * next to LordOS's own code (the signed theme host: React, the runtime, the SDK, the apps and the system layer), which
 * keeps the full `lord` in the first realm. Every native lord.* call is guarded: made while the theme's code is
 * anywhere on the JavaScript stack, it runs only when this file's policy allows it (the theme's declared capabilities),
 * so calling into the SDK gives a theme nothing the SDK's own native calls wouldn't be allowed to do for it. */
#ifndef LORD_JS_THEME_H
#define LORD_JS_THEME_H

#include "quickjs.h"
#include <stddef.h>

/* the per-user themes folder, $HOME/.local/share/lordos/themes ("" when HOME is unusable) */
const char *jst_root(void);
/* path is <root>/<id>/theme.js of a valid id: 1 and id (at least 27 bytes) filled */
int jst_is_user_bundle(const char *path, char *id, size_t n);
/* <root>/<id>/theme.js for a valid id: 0 */
int jst_bundle_path(const char *id, char *out, size_t n);

/* the system's themes folder (/usr/lib/lordos/themes), set once by main.c */
void jst_set_system_themes(const char *dir);
const char *jst_system_themes(void);

/* ---- the session's mode ---- */
void jst_begin(unsigned caps);          /* a community theme's session context is being made */
void jst_set_realm(JSContext *realm);   /* its realm, once made (NULL when the context ends) */
JSContext *jst_realm(void);
int jst_active(void);                   /* this context runs a community theme */
unsigned jst_caps(void);

/* The policy for one native function, by its lord.* name ("setStyle", "fs.write"): decided once when the function is
 * wrapped. JST_ALLOW: also with the theme on the stack; JST_NODE: then only on a node the theme owns (its own View,
 * Text, Image); JST_NODE_PROP: the same, for lord.setProp, also checked by jst_prop_guard on every call; JST_DENY:
 * never with the theme on the stack. */
enum { JST_DENY = 0, JST_ALLOW = 1, JST_NODE = 2, JST_NODE_PROP = 3 };
/* lord.setProp on a node a community theme owns, whatever the stack holds (React's commit included): never another
 * app's terminal, window, video or pixels, and a picture only from the theme's assets/ or LordOS's "asset:" ones.
 * 0, or -1 with an exception thrown. */
int jst_prop_guard(JSContext *c, int argc, JSValueConst *argv);
int jst_policy(const char *name);
/* the theme's code is on the JavaScript stack now */
int jst_on_stack(JSContext *c);
/* With the theme's code on the stack: 0 when the call may go on, -1 (an exception thrown) when not. */
int jst_check(JSContext *c, int policy, const char *name, int argc, JSValueConst *argv);

/* the theme realm's lord: its own natives, always under the policy (main is LordOS's realm, main_lord its lord) */
JSValue jst_theme_lord(JSContext *t, JSContext *main, JSValueConst main_lord);
/* the main realm's extra natives in a community session: lord.themeRealmFn, lord.themeOwn, lord.themes.started/fail */
void jst_register_main(JSContext *c, JSValue lord);

/* ---- lord.themes for user themes (js_sys.c) ---- */
/* adds the installed community themes to lord.themes.list's array */
void jst_list(JSContext *c, JSValue arr, uint32_t *i, const char *current_bundle);
/* lord.themes.install/installJob/trust/remove/info */
void jst_register_themes(JSContext *c, JSValue themes);
/* the record says it may run (installed, confirmed, not failed): 0, and the code verified against its SHA-256 */
char *jst_load_code(const char *id, size_t *len, unsigned *caps, char *why, size_t why_n);

/* ---- the watchdog (main.c) ---- */
/* a community theme took too long, stalled or crashed: marks it failed (persisted) with why */
void jst_mark_failed(const char *id, const char *why);
/* at session start: one more start of this community theme; 1 when it already crashed too many sessions */
int jst_session_starting(const char *id);
/* the theme ran a healthy minute: its crash count goes back to 0 */
void jst_session_healthy(const char *id);
/* the theme's React tree committed something (lord.themes.started) since jst_begin */
int jst_started(void);
/* the main-loop watchdog stopped a JavaScript task while it ran (js.c on_interrupt) */
void jst_note_stop(void);
int jst_stopped(void);
/* a failure lord.themes.fail reported (the theme's tree died): why, or NULL */
const char *jst_failure(void);

#endif
