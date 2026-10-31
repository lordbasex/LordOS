/* js_input.h — lord.input: touchpads, touchscreens and the on-screen keyboard (see js_input.c, docs/TRACKPAD.md
 * and docs/ON-SCREEN-KEYBOARD.md). */
#ifndef LORD_JS_INPUT_H
#define LORD_JS_INPUT_H

#include "quickjs.h"

void js_input_register(JSContext *c, JSValue lord);
/* The main loop says whether a keyboard is attached (input.c). */
void js_input_set_keyboard(int present);
/* Called by the main loop: the taps queued by lord.input.tap go through js_key, as physical keys. */
void js_input_flush(void);
/* The session left the monitor: taps not sent yet are dropped. */
void js_input_drop(void);

#endif
