/* js_voice.h — lord.voice: reading text aloud (see js_voice.c). */
#ifndef LORD_JS_VOICE_H
#define LORD_JS_VOICE_H

#include "quickjs.h"

void js_voice_register(JSContext *c, JSValue lord);

#endif
