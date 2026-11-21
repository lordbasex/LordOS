/* js_vm.h — lord.vm (js_vm.c). */
#ifndef LORD_UI_JS_VM_H
#define LORD_UI_JS_VM_H

#include "quickjs.h"

#include <poll.h>

void js_vm_register(JSContext *c, JSValue lord);

/* lord-server's main loop: each machine's picture socket (and a 3D frame's dmabuf waiting for its fence) in its
 * poll(); the frames read when they come (js_vm_dispatch), and acknowledged to lord-vmdisplay right after the paint
 * that showed them (js_vm_painted, after render()) */
int  js_vm_pollfds(struct pollfd *p, int max);
void js_vm_dispatch(const struct pollfd *p, int n);
void js_vm_painted(void);

#endif
