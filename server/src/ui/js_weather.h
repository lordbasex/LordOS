/* js_weather.h — lord.weather: the weather widget's forecasts (fetch/wxsvc.h), the desktop's own context only. */
#ifndef LORD_JS_WEATHER_H
#define LORD_JS_WEATHER_H
#include "quickjs.h"
void js_weather_register(JSContext *c, JSValue lord);
#endif
