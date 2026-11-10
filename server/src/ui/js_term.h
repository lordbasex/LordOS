/* js_term.h — lord.term.* (terminals), lord.clipboard and lord.primary. */
#ifndef LORD_JS_TERM_H
#define LORD_JS_TERM_H

#include "quickjs.h"

void js_term_register(JSContext *c, JSValue lord);
/* js.c, around a middle-button press: lord.primary.get() gives the selection only while it is handled */
void js_primary_gesture(int on);

#endif
