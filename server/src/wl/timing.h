/* timing.h — frame timing and presentation hints of a Linux app's surface (docs/WAYLAND.md, "Frame timing"):
 *   wp_fifo_manager_v1            fifo-v1: an update waits for the display's refresh after the one before it (Mesa's
 *                                 Vulkan FIFO present mode, without frame callbacks)
 *   wp_commit_timing_manager_v1   commit-timing-v1: an update is not presented before its target time
 *   wp_tearing_control_manager_v1 tearing-control-v1: "async" lets a fullscreen game's buffer go to the screen with
 *                                 an asynchronous page flip, when the card can do it (wl.c wl_try_scanout, kms.c)
 *   wp_content_type_manager_v1    content-type-v1: none, photo, video or game, kept per surface (only kept: see the doc)
 * timing.c keeps the objects and the double-buffered state; wl.c holds the updates that may not apply yet, in the one
 * queue per surface explicit sync also uses (syncobj.h's SobjQueue: an update applies once its acquire point is
 * signalled AND wltiming.h says it is ready), and applies them at its latches. */
#ifndef LORD_WL_TIMING_H
#define LORD_WL_TIMING_H

#include "wltiming.h"

struct wl_display;
struct wl_resource;

enum { WLT_HINT_VSYNC = 0, WLT_HINT_ASYNC = 1 };
enum { WLT_CT_NONE = 0, WLT_CT_PHOTO, WLT_CT_VIDEO, WLT_CT_GAME };

/* One surface's state for these protocols, made the first time a client asks for one of them on that surface. */
typedef struct WlTiming {
    struct wl_resource *fifo_r, *timer_r, *tear_r, *ct_r;  /* each at most one per surface (the protocols' errors) */
    WltUpdate p;                        /* pending: set_barrier, wait_barrier, the target; hint and ctype as set */
    int barrier;                        /* the fifo barrier is up */
    int hint, ctype;                    /* current (applied) */
} WlTiming;

/* The four globals. slot: the WlTiming pointer of a wl_surface (wl.c's Surf), never NULL for a live surface. */
void wlt_globals(struct wl_display *d, WlTiming **(*slot)(struct wl_resource *surface));
/* wl_surface.commit: this commit's update, from the pending state (its one-shot parts start over) */
WltUpdate wlt_take(WlTiming *t);
/* an update became current: its barrier and hints */
void wlt_applied(WlTiming *t, const WltUpdate *u);
/* The surface is gone (wl.c drops its held updates): its objects turn inert, t is freed. */
void wlt_gone(WlTiming *t);

/* The vblank grid of display `key` (wl_presented's; -1: every display, no monitor). */
void wlt_clock_set(int key, uint64_t ns, uint32_t refresh_ns);
const WltClock *wlt_clock(int key);

#endif
