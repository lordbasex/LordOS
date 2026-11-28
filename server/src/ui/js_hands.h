/* js_hands.h — lord.hands: the shell's side of Hands (see js_hands.c, docs/HANDS.md). */
#ifndef LORD_JS_HANDS_H
#define LORD_JS_HANDS_H

#include "quickjs.h"

void js_hands_register(JSContext *c, JSValue lord);
/* the shell's answer to an agent's call (sdk/hands.jsx __lordHands(op, json)): malloc'd JSON, NULL none */
char *js_hands_call(const char *op, const char *json);
/* a change for the shell: the clients, a prompt, Stop, a log line (__lordHandsEvent(json)) */
void js_hands_event(const char *json);

/* where a security-state action runs: refused while an AI agent's input set it off (hands.h hands_agent_owns_input) */
#include "../hands/hands.h"
#include "js_err.h"
#define HANDS_GUARD(c) do { if (hands_agent_owns_input()) return js_throw_code((c), "hands.refused", NULL); } while (0)

#endif
