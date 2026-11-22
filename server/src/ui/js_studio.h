/* js_studio.h — lord.studio, LordOS Studio's settings (js_studio.c); the desktop's own context only. */
#ifndef LORD_JS_STUDIO_H
#define LORD_JS_STUDIO_H

#include "quickjs.h"

void js_studio_register(JSContext *c, JSValue lord);

#endif
