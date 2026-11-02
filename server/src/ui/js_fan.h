/* js_fan.h — lord.fan, the session side of axia-fand (see js_fan.c, docs/FANS.md). */
#ifndef LORD_JS_FAN_H
#define LORD_JS_FAN_H

#include "quickjs.h"

void js_fan_register(JSContext *c, JSValue lord);

#endif
