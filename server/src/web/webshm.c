/* webshm.c — the libwayland-server side of wpebackend-fdo's exported frames (see webwin.h): its nested compositor,
 * where WebKit's web process draws, hands lord-web wl_shm buffers and wl_resources of its own. Kept apart from
 * webwin.c, which is a libwayland-client program. */
#include "webwin.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wpe/unstable/fdo-shm.h>

int webshm_info(struct wpe_fdo_shm_exported_buffer *b, int *w, int *h, int *stride, unsigned *format) {
    struct wl_shm_buffer *sb = wpe_fdo_shm_exported_buffer_get_shm_buffer(b);
    if (!sb) return -1;
    *w = wl_shm_buffer_get_width(sb);
    *h = wl_shm_buffer_get_height(sb);
    *stride = wl_shm_buffer_get_stride(sb);
    *format = wl_shm_buffer_get_format(sb);
    return *w > 0 && *h > 0 && *stride >= *w * 4 ? 0 : -1;
}

void webshm_copy(struct wpe_fdo_shm_exported_buffer *b, void *dst, int dst_stride, int rows) {
    struct wl_shm_buffer *sb = wpe_fdo_shm_exported_buffer_get_shm_buffer(b);
    if (!sb) return;
    int h = wl_shm_buffer_get_height(sb), stride = wl_shm_buffer_get_stride(sb), w = wl_shm_buffer_get_width(sb);
    if (rows > h) rows = h;
    size_t line = (size_t)w * 4;
    if (line > (size_t)dst_stride) line = (size_t)dst_stride;
    wl_shm_buffer_begin_access(sb);
    const unsigned char *src = wl_shm_buffer_get_data(sb);
    unsigned char *out = dst;
    if (stride == dst_stride && line == (size_t)stride) memcpy(out, src, (size_t)stride * (size_t)rows);
    else for (int y = 0; y < rows; y++) memcpy(out + (size_t)y * (size_t)dst_stride, src + (size_t)y * (size_t)stride, line);
    wl_shm_buffer_end_access(sb);
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
