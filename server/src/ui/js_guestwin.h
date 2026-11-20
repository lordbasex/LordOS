/* js_guestwin.h — lord.guestwin (js_guestwin.c): apps mode's guest side, when LordOS runs inside a Machines guest. */
#ifndef LORD_UI_JS_GUESTWIN_H
#define LORD_UI_JS_GUESTWIN_H

#include "quickjs.h"

void js_guestwin_register(JSContext *c, JSValue lord);

#endif
