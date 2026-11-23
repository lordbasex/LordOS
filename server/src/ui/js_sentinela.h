/* js_sentinela.h — lord.sentinela: Sentinela's local model engine (see js_sentinela.c, docs/SENTINELA.md). */
#ifndef LORD_JS_SENTINELA_H
#define LORD_JS_SENTINELA_H

#include "quickjs.h"

void js_sentinela_register(JSContext *c, JSValue lord);

#endif
