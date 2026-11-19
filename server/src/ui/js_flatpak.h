/* js_flatpak.h — lord.flatpak: the Store's Flathub management commands (js_flatpak.c, docs/FLATPAK.md). */
#ifndef LORD_JS_FLATPAK_H
#define LORD_JS_FLATPAK_H
#include "quickjs.h"
void js_flatpak_register(JSContext *c, JSValue lord);
#endif
