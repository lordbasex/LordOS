/* js_clean.h — lord.clean (Cleaner; see js_clean.c). */
#ifndef LORD_JS_CLEAN_H
#define LORD_JS_CLEAN_H

#include "quickjs.h"

void js_clean_register(JSContext *c, JSValue lord);

#endif
