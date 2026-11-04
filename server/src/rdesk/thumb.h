/* thumb.h — Remote Desk's card pictures (thumb.c, docs/FILES.md "Remote desktop"). */
#ifndef LORD_RDESK_THUMB_H
#define LORD_RDESK_THUMB_H

#include <stdint.h>

#define RD_THUMB_W 640

/* a device id as the app makes them: 1..64 of [A-Za-z0-9_-] */
int rd_thumb_id_ok(const char *id);
/* BGRA w×h → RGB at most maxw wide (box filter); malloc'd, *ow × *oh. NULL on bad input or no memory. */
unsigned char *rd_thumb_scale(const uint32_t *px, int w, int h, int maxw, int *ow, int *oh);
/* writes <home>/.local/share/lordos/remote-desk/thumbs/<id>.png (replaced atomically). 0 written. */
int rd_thumb_save(const char *home, const char *id, const uint32_t *px, int w, int h, int maxw);
/* removes it; 0 also when there was none */
int rd_thumb_remove(const char *home, const char *id);

#endif
