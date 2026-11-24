/* webshm.h — the libwayland-server side of wpebackend-fdo's exported frames, for webback_legacy.c only: its nested
 * compositor, where WebKit's web process draws, hands lord-web wl_shm buffers and wl_resources of its own. Kept apart
 * from the libwayland-client code (webwin.c, webback_legacy.c). */
#ifndef LORD_WEB_WEBSHM_H
#define LORD_WEB_WEBSHM_H

#include <stdint.h>

struct wpe_fdo_shm_exported_buffer;
struct wl_resource;
/* the exported buffer's size, format and pixels, readable until webshm_end (called whatever this returned, with ok
 * 1 when it returned 0); -1 when it isn't a frame lord-web can show (not ARGB8888 or XRGB8888) */
int webshm_begin(struct wpe_fdo_shm_exported_buffer *b, int *w, int *h, int *stride, uint32_t *format, const void **pixels);
void webshm_end(struct wpe_fdo_shm_exported_buffer *b, int ok);
/* calls gone(data) once when the resource is destroyed */
void webshm_on_destroy(struct wl_resource *res, void (*gone)(void *data), void *data);

#endif
