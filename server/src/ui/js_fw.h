/* js_fw.h — lord.fw: per-app network permissions (see js_fw.c and docs/FIREWALL.md). */
#ifndef LORD_JS_FW_H
#define LORD_JS_FW_H

#include "quickjs.h"
#include <stdint.h>

void js_fw_register(JSContext *c, JSValue lord);
/* the latest per-app counters from axia-netperm (its JSON, or NULL) and a number that changes with them */
const char *js_fw_counts(uint64_t *seq);

#endif
