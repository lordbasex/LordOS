/* js_sys.h — system APIs exposed to JS (files, system, settings, themes). */
#ifndef LORD_JS_SYS_H
#define LORD_JS_SYS_H

#include "quickjs.h"

void js_sys_init(void);                         /* takes $HOME (already unprivileged) */
void js_sys_register(JSContext *c, JSValue lord);
/* Process of an isolated app: only lord.text and lord.image (drawing and text direction). */
void js_sys_register_app(JSContext *c, JSValue lord);
void js_sys_set_greeter(int on);             /* login screen theme: lord.greeter */
void js_sys_set_assets(const char *dir);   /* assets/ folder (language catalogs) */
int js_sys_reserved_id(const char *id);     /* one of LordOS's string namespaces: no package may have it */
const char *js_sys_assets(void);          /* for src: 'asset:icons/x.png' */
void js_sys_cpu_name(char *out, size_t size);   /* processor (x86 or ARM; the Monitor uses it too) */
/* Valid destination path for writing (home folder or user mounts, never the
 * protected folders): 0 if it is OK; out receives the real path. */
int js_sys_resolve_target(const char *path, char *out, size_t n);

#endif
