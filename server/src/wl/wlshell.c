/* wlshell.c — the hooks wl.c calls once for all the shell extras (wlshell.h). */
#include "wlshell.h"

void wlshell_mapped(int id) {
    tltag_mapped(id);                   /* a tag set before the first frame reaches the shell with the window */
    tldrag_mapped(id);                  /* a new window torn off follows the drag from its first frame */
    bgeffect_mapped(id);                /* its blur, set before its first frame */
}

void wlshell_commit(int id) {
    bgeffect_commit(id);                /* ext_background_effect_surface_v1.set_blur_region is double-buffered */
    xwshell_commit(id);                 /* xwayland_surface_v1.set_serial too */
}
