/* js_webapps.h — lord.webapps: web apps through lord-web (see js_webapps.c and docs/WEBAPPS.md). */
#ifndef LORD_JS_WEBAPPS_H
#define LORD_JS_WEBAPPS_H

#include "quickjs.h"

void js_webapps_register(JSContext *c, JSValue lord);

#endif
