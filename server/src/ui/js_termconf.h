/* js_termconf.h — lord.termconf: the Terminal's configuration file (see js_termconf.c, ../termconf.h). */
#ifndef LORD_JS_TERMCONF_H
#define LORD_JS_TERMCONF_H

#include "quickjs.h"

void js_termconf_register(JSContext *c, JSValue lord);

#endif
