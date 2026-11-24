/* webshm.c — the libwayland-server side of wpebackend-fdo's exported frames (webshm.h, for webback_legacy.c): its
 * nested compositor, where WebKit's web process draws, hands lord-web wl_shm buffers and wl_resources of its own. */
#include "webshm.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wpe/unstable/fdo-shm.h>

int webshm_begin(struct wpe_fdo_shm_exported_buffer *b, int *w, int *h, int *stride, uint32_t *format, const void **pixels) {
    struct wl_shm_buffer *sb = wpe_fdo_shm_exported_buffer_get_shm_buffer(b);
    if (!sb) return -1;
    *w = wl_shm_buffer_get_width(sb);
    *h = wl_shm_buffer_get_height(sb);
    *stride = wl_shm_buffer_get_stride(sb);
    *format = wl_shm_buffer_get_format(sb);
    if (*w <= 0 || *h <= 0 || *stride < *w * 4 || (*format != WL_SHM_FORMAT_ARGB8888 && *format != WL_SHM_FORMAT_XRGB8888)) return -1;
    wl_shm_buffer_begin_access(sb);
    *pixels = wl_shm_buffer_get_data(sb);
    return 0;
}

void webshm_end(struct wpe_fdo_shm_exported_buffer *b, int ok) {
    struct wl_shm_buffer *sb = ok ? wpe_fdo_shm_exported_buffer_get_shm_buffer(b) : NULL;
    if (sb) wl_shm_buffer_end_access(sb);
}

typedef struct {
    struct wl_listener l;
    void (*gone)(void *data);
    void *data;
} Watch;

static void watch_fire(struct wl_listener *l, void *res) {
    (void)res;
    Watch *wt = wl_container_of(l, wt, l);
    wl_list_remove(&wt->l.link);
    wt->gone(wt->data);
    free(wt);
}

void webshm_on_destroy(struct wl_resource *res, void (*gone)(void *data), void *data) {
    Watch *wt = calloc(1, sizeof *wt);
    if (!wt) return;
    wt->gone = gone;
    wt->data = data;
    wt->l.notify = watch_fire;
    wl_resource_add_destroy_listener(res, &wt->l);
}
