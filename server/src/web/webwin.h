/* webwin.h — lord-web's Wayland window (see web.h). */
#ifndef LORD_WEB_WEBWIN_H
#define LORD_WEB_WEBWIN_H

#include "web.h"

/* Connects to the session's compositor and reads its globals. NULL when there is no display or no xdg-shell. */
WebWin *webwin_new(Web *w);
/* After webwin_new, before any view exists: sets up wpebackend-fdo for this window (dmabuf with EGL when the
 * compositor offers linux-dmabuf and EGL works, shared memory otherwise: the CPU path) and creates w->exportable. */
int webwin_init_backend(WebWin *win);
/* Once WebKit's view exists: input, the input method, the clipboard */
void webwin_attach(WebWin *win);
void webwin_show(WebWin *win, const char *activation_token);
void webwin_hide(WebWin *win);
int webwin_visible(WebWin *win);
void webwin_set_title(WebWin *win, const char *title);
void webwin_set_cursor_hint(WebWin *win, int link, int text);
/* the person pressed a key, clicked or touched the window less than a few seconds ago and it has the keyboard: what a
 * page may write to the clipboard needs it */
int webwin_user_active(WebWin *win);
/* the page copied: offer this text (mime NULL) or image (image/png, image/jpeg) to the session; data taken over */
void webwin_offer(WebWin *win, const char *mime, GBytes *data);
void webwin_free(WebWin *win);

/* webshm.c: wpebackend-fdo's shared memory buffers and resources are libwayland-server objects */
struct wpe_fdo_shm_exported_buffer;
struct wl_resource;
/* copies the exported buffer into dst (stride dst_stride, at most dst_h rows); its size and format first */
int webshm_info(struct wpe_fdo_shm_exported_buffer *b, int *w, int *h, int *stride, unsigned *format);
void webshm_copy(struct wpe_fdo_shm_exported_buffer *b, void *dst, int dst_stride, int rows);
/* calls gone(data) once when the resource is destroyed */
void webshm_on_destroy(struct wl_resource *res, void (*gone)(void *data), void *data);

#endif
