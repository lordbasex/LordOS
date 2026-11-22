/* wlshell.h — the "shell extras" protocols, one file each (docs/WAYLAND.md, "Shell extras"):
 *   tldrag.c    xdg-toplevel-drag-v1      a window that follows a drag (a browser tab torn off)
 *   tltag.c     xdg-toplevel-tag-v1       a window's untranslated tag and translated description
 *   sysbell.c   xdg-system-bell-v1        the bell: LordOS's alert sound and the window marked, rate-limited
 *   xsession.c  xdg-session-management-v1 each window back where it was when its app restarts
 *   bgeffect.c  ext-background-effect-v1  blur behind part of a window, only where it can really be drawn
 *   xwshell.c   xwayland-shell-v1         Xwayland's windows matched to their surfaces by serial (only Xwayland)
 *   wlshell.c   the hooks shared by several of them
 * wl.c calls the few hooks below and lends them the wl__ helpers; nothing here reaches into its structures. */
#ifndef LORD_WLSHELL_H
#define LORD_WLSHELL_H

#include <stddef.h>
#include <stdint.h>

struct wl_display;
struct wl_client;
struct wl_resource;
struct Node;

/* ---- hooks wl.c calls ---- */
void tldrag_globals(struct wl_display *dpy);
void tltag_globals(struct wl_display *dpy);
void sysbell_globals(struct wl_display *dpy);
void xsession_globals(struct wl_display *dpy);
void bgeffect_globals(struct wl_display *dpy);
void xwshell_globals(struct wl_display *dpy);  /* xwayland-shell-v1, for Xwayland only (xwshell.c) */
void wlshell_mapped(int id);                    /* a toplevel was announced to JS (emit_map) */
void wlshell_commit(int id);                    /* a surface's commit, after its own state was applied */
/* the drag: wl_data_device.start_drag took this source (NULL for a drag inside one client); the drag ended */
void tldrag_started(struct wl_resource *source);
void tldrag_ended(void);
/* a window follows the running drag: it is a window, not files for LordOS, and let go where nothing takes it, it is
 * dropped there (dnd_drop_performed and dnd_finished, not cancelled: the app would take the tab back) */
int  tldrag_following(void);
/* 1 if source belongs to a toplevel drag (set_selection with it is an error, already posted) */
int  tldrag_claims(struct wl_resource *source);
/* the node the drag goes to: the window being dragged is not a target (its frame is skipped) */
struct Node *tldrag_hit(float x, float y, struct Node *hit);
void bgeffect_caps_changed(void);               /* the GPU came or went: blur can or can't be drawn now */
/* what wlshell.c (wlshell_mapped, wlshell_commit) passes on */
void tldrag_mapped(int id);
void tltag_mapped(int id);
void bgeffect_commit(int id);
void xwshell_commit(int id);
void bgeffect_mapped(int id);

/* ---- for JS (lord.wayland, ui/js_sys.c) ---- */
/* The window following a drag: 1 with its offset (geometry coordinates) and, when it is placed, where its geometry
 * is now (*placed = 1); frame_node, if it is that window's frame (an ancestor of its surface), is skipped as a target. */
int  tldrag_follow(int id, int frame_node, int *ox, int *oy, int *gx, int *gy, int *placed);
/* A window's tag and description ("" when not set); 0 if it has neither. */
int  tltag_get(int id, const char **tag, const char **description);
/* Where a restored window goes (xdg-session-management): its geometry rect and maximized, once. 0 if none. */
int  xsession_frame(int id, int r[4], int *maximized);
void bgeffect_set_effects(int on);              /* Settings → transparency with blur (settings.effects) */

/* ---- from wl.c, only for these files ---- */
int  wl__surface_id(struct wl_resource *surface);          /* a wl_surface's id, 0 if none */
int  wl__toplevel_id(struct wl_resource *toplevel);        /* an xdg_toplevel's surface id, 0 if inert */
int  wl__window_of(int id);                                /* the toplevel a surface belongs to, 0 if none */
/* 0 if no such surface; mapped: shown as a window; committed: its first commit was made (configure sent) */
int  wl__surface_state(int id, int *mapped, int *committed, int *toplevel);
/* a window's geometry on screen (logical) and maximized/fullscreen; 0 if it isn't placed */
int  wl__window_rect(int id, int r[4], int *maximized, int *fullscreen);
/* a surface's window geometry in its own coordinates (x, y, w, h) and its logical size */
int  wl__surface_geom(int id, int g[4], int *lw, int *lh);
int  wl__surface_node(int id);                             /* the UI node that shows it (windows), 0 none */
struct wl_client *wl__surface_client(int id);
/* who a client is, as lord-server checked it: "app:<isolated LordOS app>", "program:<exe>", "flatpak:<id>";
 * "" for Xwayland, the login screen or anything it can't tell */
const char *wl__client_owner(struct wl_client *c);
int  wl__source_used(struct wl_resource *source);          /* wl_data_source: 0 unused, 1 selection, 2 drag */
void wl__emit(int id, const char *type, const char *json); /* an event for JS (id 0: no window) */
void wl__pointer(float *x, float *y);                      /* where the pointer is (logical) */
void wl__json_str(char *out, size_t size, const char *s);  /* a JSON string's inside */

#endif
