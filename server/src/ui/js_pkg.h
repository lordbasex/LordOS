/* js_pkg.h — lord.pkg (Debian package queries, see js_pkg.c). */
#ifndef LORD_JS_PKG_H
#define LORD_JS_PKG_H

#include "quickjs.h"
#include <stddef.h>

void js_pkg_register(JSContext *c, JSValue lord);
/* Where lord.pkg.ownerOfApp finds a launcher app's .desktop file: apps_system_file (apps.h) in
 * lord-server; tests give their own. Without it, ownerOfApp answers null. */
void js_pkg_set_app_file(int (*fn)(const char *id, char *out, size_t n));

#endif
