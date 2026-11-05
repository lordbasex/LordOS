/* vmthumb.h — a machine's card picture (vmthumb.c). */
#ifndef LORD_VMTHUMB_H
#define LORD_VMTHUMB_H

#include <stdint.h>

#define VM_THUMB_W 480

/* BGRX w×h → RGB at most maxw wide (box filter); malloc'd, *ow × *oh. NULL on bad input or no memory. */
unsigned char *vm_thumb_scale(const uint32_t *px, int w, int h, int maxw, int *ow, int *oh);
/* writes <dir>/thumb.png (replaced atomically, 0600). 0 written. */
int vm_thumb_save(const char *dir, const uint32_t *px, int w, int h, int maxw);

#endif
