/* js_spell.h — lord.spell: the spell checker through the lord-spell helper (see js_spell.c, docs/SPELLING.md). */
#ifndef LORD_JS_SPELL_H
#define LORD_JS_SPELL_H

#include "quickjs.h"

void js_spell_register(JSContext *c, JSValue lord);

#endif
