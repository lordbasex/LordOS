/* js.h — JavaScript engine (QuickJS) that runs the React theme. */
#ifndef LORD_JS_H
#define LORD_JS_H

#include <stddef.h>

int    js_init(const char *bundle_path);
/* js_init/js_init_app return JS_REFUSED when the code was refused by the verified-code check
 * (sec/codesig.h): the caller falls back to another theme or to js_init_recovery(). */
#define JS_REFUSED (-2)
int    js_init_app(const char *host_bundle, const char *app_id);   /* axia-server --app <id> */
int    js_init_recovery(int greeter);   /* the built-in "component was modified" screen */
int    js_app_recovery(void);           /* the same in an isolated app's window */
void   js_shutdown(void);
double js_tick(double now);        /* runs expired timers; ms until the next one or -1 */
int    js_wants_frame(void);       /* there are pending requestAnimationFrame callbacks */
void   js_frame(double now);
void   js_pointer(const char *type, int target, float x, float y, int button);
/* Touchpad and remote desktop (tp/tp_route.c) → __lordGesture(kind, phase, id, x, y, a, b, c, d):
 * "scroll" (dx, dy, source), "pinch" (scale, rotation, dx, dy), "swipe" (dx, dy, fingers, system). */
void   js_gesture(const char *kind, const char *phase, int target, float x, float y, double a, double b, double c, double d);
void   js_key(int code, int value);
void   js_keyboards_event(int present);
void   js_wl_event(int id, const char *type, const char *json);
void   js_before_exit(void);             /* __lordBeforeExit(): the session's last word, once */
void   js_media_event(int id, const char *type, const char *json);
void   js_term_event(int id, const char *type, const char *data);
void   js_sound_event(const char *type, const char *json);
void   js_net_event(const char *what);
void   js_disk_event(const char *what);      /* Disks app: something changed (disk/disk.c) */
void   js_backup_event(const char *json);    /* Backups → __lordBackup */
void   js_admin_event(int id, const char *kind, const char *text);
void   js_logs_event(const char *kind, int id, const char *name);   /* app Registros */
void   js_clipboard_poll(void);
void   js_remote_event(const char *json);
void   js_remote_notice(const char *json);   /* RP_NOTICE from lord-sessiond → __lordRemoteNotice */
void   js_greeter_event(const char *json);
void   js_screen_resized(void);
/* lord.screen.displays / .mirror: {"mirror":…,"displays":[{id,name,x,y,w,h,pw,ph,scale,main}]} (main.c) */
void   js_set_displays(const char *json);
void   js_hw_event(const char *kind, const char *json);   /* Settings' hardware (src/hw) → __lordHw */
void   js_broker_event(const char *kind, const char *json);   /* broker notices → theme */
void   js_broker_app_event(const char *kind, const char *json, size_t len);   /* axia-server → isolated app */

#endif
