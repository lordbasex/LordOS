/* js_share.h — lord.share: the shell's side of the screen-sharing API (see js_share.c). */
#ifndef LORD_JS_SHARE_H
#define LORD_JS_SHARE_H

#include "quickjs.h"

void js_share_register(JSContext *c, JSValue lord);

#endif
