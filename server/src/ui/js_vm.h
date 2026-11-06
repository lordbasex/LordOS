/* js_vm.h — lord.vm (js_vm.c). */
#ifndef LORD_UI_JS_VM_H
#define LORD_UI_JS_VM_H

#include "quickjs.h"

void js_vm_register(JSContext *c, JSValue lord);

#endif
