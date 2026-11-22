/* js_vmcli.h — lord.vmcli (js_vmcli.c): the `machines` command's session channel (vm/vmcli.h). */
#ifndef LORD_UI_JS_VMCLI_H
#define LORD_UI_JS_VMCLI_H

#include "quickjs.h"

void js_vmcli_register(JSContext *c, JSValue lord);

#endif
