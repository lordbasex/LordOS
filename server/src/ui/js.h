/* js.h — JavaScript engine (QuickJS) that runs the React theme. */
#ifndef LORD_JS_H
#define LORD_JS_H

#include <stddef.h>

int    js_init(const char *bundle_path);
/* js_init/js_init_app return JS_REFUSED when the code was refused by the verified-code check
 * (sec/codesig.h): the caller falls back to another theme or to js_init_recovery(). */
#define JS_REFUSED (-2)
/* the running theme is a community theme (its code in its own realm, js_theme.c) */
int js_community_theme(void);
/* A theme switch (lord.themes.use, main.c switch_theme). js_theme_prepare reads and verifies the bundle asked for
 * while the running theme still runs: 0, JS_REFUSED (the verified-code check refused it) or -1 (unreadable).
 * js_init_prepared starts a new context with those same bytes (js_init if none were prepared for that path).
 * js_init_previous starts the theme that was running again from the bytes verified when it was loaded. */
int    js_theme_prepare(const char *bundle_path);
int    js_init_prepared(const char *bundle_path);
int    js_init_previous(const char *bundle_path);
/* The last exception js_init's code threw (its message and the first lines of its stack), or "" */
const char *js_init_error(void);
int    js_init_app(const char *host_bundle, const char *app_id);   /* lord-server --app <id> */
int    js_init_recovery(int greeter);   /* the built-in "component was modified" screen */
int    js_app_recovery(void);           /* the same in an isolated app's window */
void   js_shutdown(void);
double js_tick(double now);        /* runs expired timers; ms until the next one or -1 */
int    js_wants_frame(void);       /* there are pending requestAnimationFrame callbacks */
void   js_frame(double now);
void   js_mem_line(char *buf, size_t n);
/* The stall detector (js.c): a main-thread task of `kind` ("layout", "paint") that took ms; logged past the limit */
void   js_stall_note(const char *kind, const char *what, double ms);
int    js_stall_count(void);
void   js_stall_begin(void);           /* a call into JS made outside js.c (js_wm_end): timed as one run */
void   js_stall_end(const char *what);   /* (memory report) the session runtime's QuickJS usage, one line */
void   js_pointer(const char *type, int target, float x, float y, int button);
/* A press with the window drag modifier held (grab.h grab_mod_held): __lordEvent('modgrab', …), 1 when the shell took it
 * for a window's move or resize (sdk/moddrag.js); the press then reaches neither the app nor the theme's handlers. */
int    js_pointer_modgrab(int target, float x, float y, int button);
/* Touchpad and remote desktop (tp/tp_route.c) → __lordGesture(kind, phase, id, x, y, a, b, c, d):
 * "scroll" (dx, dy, source), "pinch" (scale, rotation, dx, dy), "swipe" (dx, dy, fingers, system). */
void   js_gesture(const char *kind, const char *phase, int target, float x, float y, double a, double b, double c, double d);
void   js_key(int code, int value);
/* Finished text, as an input method commits it, to whoever has the keyboard: a Linux app's field with text input on,
 * or a LordOS field. The screen-sharing API's text that no key of the layout types (main.c). 0: nobody can take it. */
int    js_commit_text(const char *text);
/* A Linux app's field with text input on has the keyboard: text for it goes whole as one commit, because its keys and
 * commits mixed don't keep their order in the app (GTK 4 on VM 127 put the committed letters before the typed ones). */
int    js_text_input_app(void);
void   js_keyboards_event(int present);
void   js_wl_event(int id, const char *type, const char *json);
void   js_before_exit(void);             /* __lordBeforeExit(): the session's last word, once */
void   js_media_event(int id, const char *type, const char *json);
void   js_term_event(int id, const char *type, const char *data);
void   js_sound_event(const char *type, const char *json);
void   js_net_event(const char *what);
void   js_vm_library_event(void);           /* Machines: the library changed (vm/vmlistcache.h) → __lordVm('library') */
void   js_vm_state_event(const char *json); /* Machines: what runs or is in apps mode changed → __lordVm('state', json) */
void   js_disk_event(const char *what);      /* Disks app: something changed (disk/disk.c) */
void   js_backup_event(const char *json);    /* Backups → __lordBackup */
void   js_cloud_event(const char *json);     /* Cloud → __lordCloud */
void   js_cloud_reply(int req, const char *json);   /* Cloud: a link's answer → __lordCloudReply */
void   js_fswatch_dispatch(double now);  /* every pass: lord.fs.watch's calls that are due (fswatch.c) */
void   js_fs_dispatch(void);             /* fsasync_fd() is readable: lord.fs.*Async answers → __lordFsReply */
void   js_admin_event(int id, const char *kind, const char *text);
void   js_logs_event(const char *kind, int id, const char *name);   /* app Registros */
void   js_clipboard_poll(void);
void   js_remote_event(const char *json);
void   js_share_event(const char *json);      /* the screen-sharing API → __lordShare(json) */
void   js_remote_notice(const char *json);   /* RP_NOTICE from lord-sessiond → __lordRemoteNotice */
void   js_greeter_event(const char *json);
void   js_screen_resized(void);
/* lord.screen.displays / .mirror: {"mirror":…,"displays":[{id,name,x,y,w,h,pw,ph,scale,main}]} (main.c) */
void   js_set_displays(const char *json);
void   js_hw_event(const char *kind, const char *json);   /* Settings' hardware (src/hw) → __lordHw */
void   js_broker_event(const char *kind, const char *json);   /* broker notices → theme */
void   js_broker_app_event(const char *kind, const char *json, size_t len);   /* lord-server → isolated app */

#endif
