/* wlfixes.h — the core protocol's wl_fixes global (wayland.xml 1.24, version 1): destroy_registry lets a
 * long-lived client destroy a wl_registry it no longer needs. wl_registry has no destructor request, so before
 * this every wl_display_get_registry a client made stayed in lord-server, and kept getting global and
 * global_remove events, until the client disconnected. See wlfixes.c and docs/WAYLAND.md. */
#ifndef LORD_WL_WLFIXES_H
#define LORD_WL_WLFIXES_H

struct wl_display;

/* Advertises wl_fixes on dpy (wl.c's wl_init). 0, or -1 if the global couldn't be created. */
int wlfixes_init(struct wl_display *dpy);

#endif
