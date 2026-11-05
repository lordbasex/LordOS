/* vmthumb.h — a machine's card picture (vmthumb.c). */
#ifndef LORD_VMTHUMB_H
#define LORD_VMTHUMB_H

#include <stdint.h>

#define VM_THUMB_W 480

/* BGRX w×h → RGB at most maxw wide (box filter); malloc'd, *ow × *oh. NULL on bad input or no memory. */
unsigned char *vm_thumb_scale(const uint32_t *px, int w, int h, int maxw, int *ow, int *oh);
/* writes <dir>/<name> (a PNG at most maxw wide, replaced atomically, 0600). 0 written. */
int vm_png_save(const char *dir, const char *name, const uint32_t *px, int w, int h, int maxw);
#define vm_thumb_save(dir, px, w, h, maxw) vm_png_save((dir), "thumb.png", (px), (w), (h), (maxw))

#endif
