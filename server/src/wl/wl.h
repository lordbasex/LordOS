/* wl.h — lord-server's Wayland compositor: GTK and Qt apps as LordOS windows (see wl.c). */
#ifndef LORD_WL_H
#define LORD_WL_H

#include "../canvas.h"
#include "../keyboard.h"
#include <stdint.h>
#include <sys/types.h>

struct Node;

int  wl_start(void);                    /* after dropping privileges; 0 ok */
void wl_stop(void);
int  wl_fd(void);                       /* for poll; -1 if it did not start */
void wl_dispatch(void);                 /* there is something on wl_fd */
void wl_flush(void);                    /* sends what is pending to the clients */
const char *wl_socket(void);            /* "wayland-0" */
void wl_frame_done(unsigned time_ms);   /* after presenting a frame */
/* Nothing is being drawn (screen off, the session away from the monitor, nobody watching): apps
 * still waiting for a frame callback get it and their presentation feedback is discarded, so
 * none stalls. 1 if some app was waiting. */
int  wl_frame_unseen(unsigned time_ms);

/* Input (main.c). hit = node under the pointer; down[i]/button[i] = buttons of this event
 * (0 left, 1 middle, 2 right). Returns 1 if the pointer is over a Wayland app and 2 if
 * it is over one of its popup menus (drawn on top of everything: the click is not the theme's). */
int  wl_pointer(float x, float y, struct Node *hit, int moved, int nbuttons, const int *down, const int *button, int wheel);
/* While lord-server moves or resizes a window (ui/grab.c): no app receives the pointer
 * (they are sent leave) and only the button count is tracked. */
void wl_pointer_grab(int nbuttons, const int *down);
/* 0: the key is the shell's; 1: it went to the focused app; 2: it went to the app as another key
 * (Lord Flow's ❖ → Ctrl): the shell is only told, so ❖ does not count as pressed alone. */
int  wl_key(int code, int value, const KeyInfo *k);
void wl_keymap_changed(void);
int  wl_send_paste(void);               /* synthetic Ctrl+V to the Linux app with the keyboard */
int  wl_has_key_focus(void);            /* does a Linux app have the keyboard? */
/* Where the input method's candidate window goes: the caret of the Linux app's field that composes
 * (zwp_text_input_v3), on screen. 0 without one. */
int  wl_text_caret(float *x, float *y, float *w, float *h);
int  wl_focus_protected(void);          /* is that app's window a protected one? */
const char *wl_focus_app(void);         /* the isolated app with the keyboard, or NULL */
int  wl_focus_pid(void);                 /* the process of the app with the keyboard, 0 if none (ui/portal.c) */
const char *wl_focus_primary(size_t *n);    /* the text the app with the keyboard selected (primary selection), or NULL */
/* The primary selection (select to copy, middle click to paste), for LordOS's own text fields and the Terminal:
 * wl_set_primary makes `t` the selection the apps are offered (-1 if empty or over 16 MB); wl_primary is the
 * current one, whoever made it ("" if none); wl_primary_paste(0) offers it to no app (Settings → Mouse &
 * Trackpad, "Middle click pastes"). */
int  wl_set_primary(const char *t, size_t n);
const char *wl_primary(size_t *n);
void wl_primary_paste(int on);
int  wl_primary_paste_on(void);
const char *wl_node_app(int node);      /* the isolated app whose window that node is, or NULL */

/* From JS (lord.wayland) */
void wl_set_focus(int id);              /* 0: no Wayland app has the keyboard */
void wl_focus_terminal(int on);         /* the app with the keyboard is a terminal emulator: ❖C is Ctrl+Shift+C */
void wl_close(int id);                  /* asks the app to close the window */
void wl_close_all(int ms);              /* every app asked to close, then waits for them: at most ms */
const char *wl_kill(int id);            /* Force Quit: SIGKILL to its client (same user only); NULL or an error code */
void wl_set_state(int id, int maximized, int fullscreen);
void wl_set_minimized(int id, int minimized);   /* X11 windows: iconic and at the bottom of X's stack */
void wl_set_parked(int id, int parked);         /* on a hidden desktop: no keyboard and no fullscreen until back */
int  wl_count(void);
int  wl_idle_inhibited(void);             /* idle-inhibit: a shown surface asks the screen to stay on */
void wl_output_resized(void);               /* headless: one display of ui.width × ui.height */
void wl_remap_all(void);                    /* a new JS context (theme switched): announce the open windows again */
/* The displays (docs/DISPLAYS.md): one wl_output (+ xdg-output) each. x, y, w, h: logical rect in the
 * global space; pw × ph: physical mode; scale: percent (wl_output.scale is its ceiling, the exact
 * one goes by fractional-scale-v1). Surfaces get enter/leave and their preferred scale. */
typedef struct {
    int x, y, w, h, pw, ph, refresh, scale, mm_w, mm_h;
    char name[32], desc[96];
} WlOutput;
void wl_set_outputs(const WlOutput *o, int n, int main);
void wl_set_pointer_scale(int pct);         /* the display under the pointer: cursor-shape images */               /* ui.width/ui.height changed */

/* Drawing (paint.c): surface id in the node's box, and popup menus on top of everything.
 * On the GPU path (src/gpu) the UI only leaves a hole and the surfaces are drawn when presenting. */
void wl_paint_begin(void);
void wl_paint_keep(void);                /* saves the windows noted so far (paint.c, «what is below») */
void wl_paint_kept(void);                /* and notes them again in another frame */
void wl_paint_save(void);                /* keeps the frame's list of windows (LORD_PAINT_VERIFY) */
void wl_paint_restore(void);             /* and puts it back */
void wl_paint_holes(int on);             /* 0: wl_paint only notes the window, without opening the hole */
int  wl_draws_meet(Rect r);              /* GPU path: an app window noted below the UI (layer 0) meets r (physical px of the view being painted) */
void wl_tf_begin(int node, float sx, float sy, float tx, float ty, float op, int skip);  /* GPU path: windows painted until wl_tf_end are in a transformed node's raster */
void wl_tf_end(void);
void wl_tf_nested(int d);                /* +1 / -1: inside another transform: no quad */
void wl_tf_open_holes(Canvas *c);        /* in the canvas the raster goes to, before it is blended */
void wl_paint(Canvas *c, int id, int node, float x, float y, float w, float h, float op);
void wl_paint_popups(Canvas *c);

/* Presentation (main.c). */
int  wl_gpu_layers_empty(void);             /* no app window, fullscreen or menu to draw (the buffer-age scissor) */
void wl_gpu_draw_layer(int layer);          /* 0 = windows (below the UI), 1 = menus and fullscreen */
void wl_paint_fullscreen(Canvas *out);      /* CPU path: the fullscreen window over the frame */
int  wl_fullscreen_active(void);
/* Sharing a screen or a window (cast/cast.c): while on, the boxes of the windows and menus whose content changed are
 * kept (logical px), also on the GPU path, where an app's new frame is only a texture and no UI damage. take: their
 * box since the last take, 0 if none. */
void wl_content_track(int on);
int  wl_content_take(float *x0, float *y0, float *x1, float *y1);
void wl_fullscreen_allow(int on);           /* 0 while the sharing indicator must stay on screen */
int  wl_try_scanout(int cursor_free);       /* GPU path: 1 if fullscreen went straight to the card */
int  wl_wants_frame(void);                  /* an app changed its content */
void wl_presented(uint64_t ns, unsigned seq, unsigned refresh_ns);   /* a flip finished (wp_presentation) */
void wl_set_drm_device(dev_t dev);          /* DRM device for linux-dmabuf (CPU path) */

/* Pointer: relative motion (games), locking and confinement (pointer-constraints). */
void wl_pointer_relative(double dx, double dy);
int  wl_pointer_locked(void);
void wl_pointer_confine(int *x, int *y);
/* Cursor requested by the app under the pointer: 0 = LordOS's, 1 = image (ARGB px), 2 = hidden.
 * gen changes when it must be redrawn. */
int  wl_cursor(const uint32_t **px, int *w, int *h, int *hx, int *hy, unsigned *gen);

/* Keyboard shortcuts inhibitors (wl/kbinhibit.h): the login screen never grants one (before wl_start); the
 * person's answer to a "shortcuts" prompt for app (a kbinhibit key that is waiting for one: 0 otherwise); and
 * the saved decisions changed (Settings → Privacy), so every inhibitor is told whether it holds. */
void wl_set_greeter(int on);
int  wl_shortcuts_answer(const char *app, int allow);
void wl_shortcuts_changed(void);
/* 1 if this key press goes to the window with the keyboard because its inhibitor holds (kbi_route: KBI_KEY_APP, or
 * ❖Esc), so no Linux app's global shortcut (ui/portal.c) may take it first. letter: keygrab_letter; mods: KG_*. */
int  wl_key_to_inhibitor(int code, char letter, unsigned mods);
/* The name to show for a "shortcuts" decision's app (a kbinhibit key): the installed app whose .desktop entry runs
 * that program or Flatpak app, never what a client says; hint (an app_id, or NULL) only says where to look first.
 * out = "" when none does. */
void wl_shortcuts_name(const char *key, const char *hint, char *out, size_t n);

/* Clipboard: last image copied by a Linux app (or chosen from the history). */
unsigned    wl_clip_image_gen(void);
const char *wl_clip_image_path(void);       /* "" if the last copied thing is not an image */
int         wl_clip_is_image(void);
const unsigned char *wl_clip_png(size_t *len);   /* the copied PNG, or NULL */
const char *wl_clip_image_origin(void);
int         wl_clip_image_sensitive(void);
int         wl_clip_use_image(const char *path);   /* only $XDG_RUNTIME_DIR/lordos/clip-N.png */
/* rich text: the HTML that goes with the clipboard's current text (NULL if none); set it right after the text */
const char *wl_clip_html(size_t *len);
void        wl_clip_set_html(const char *html, size_t n);
/* files: the paths that go with the clipboard's current text (a urilist.h plist, NULL if none). own: Files
 * put them there (cut is never set for another app's). Set them right after their text (the paths). */
const char *wl_clip_files(size_t *len, int *count, int *cut, int *own);
char       *wl_clip_files_as(int fmt, size_t *len);      /* malloc'd, in a urilist.h format, or NULL */
const char *wl_clip_files_from(void);                   /* the app that copied them ("" if Files, unknown or none) */
int         wl_clip_set_files(const char *list, size_t len, int count, int cut);

/* Drag and drop from LordOS's Files (lord.wayland.dragFiles): while its drag is over a Linux window the
 * compositor offers these paths (each one already checked: absolute, existing, UTF-8, no NUL or line break;
 * at most URIL_MAX_PATHS) as text/uri-list and text/plain, encoded by urilist.c as Files' copy is. Only while a button is down and no app's drag runs; 1 if it started.
 * The last release drops (or cancels); wl_drag_files_end cancels one still running. wl_drag_files_taken:
 * the action the window under the pointer agreed (1 copy, 2 move), 0 if it takes nothing. */
int  wl_drag_files(const char *const *paths, int n);
void wl_drag_files_end(void);
int  wl_drag_files_taken(void);
/* And the other way: a Linux app's drag of files released over LordOS itself (never an isolated LordOS app's
 * or a sandboxed app's). Files claims it while the UI handles that release (wl_drop_claim: 1 for the first
 * caller, and only then); wl_drop_release_done, right after the release went to JS, cancels a drop nobody
 * claimed. Once its list is read (within DNDG_READ_MS), the "filesdrop" event (window 0) says the local paths
 * are ready: wl_drop_paths gives them (n paths, each ended by its NUL), wl_drop_clear forgets them. */
int  wl_drop_claim(void);
void wl_drop_release_done(void);
const char *wl_drop_paths(int *n);
void wl_drop_clear(void);

/* xdg-foreign-v2: the window an app exported with that handle (a portal's "wayland:<handle>" parent),
 * or 0 if no live window has it */
int  wl_foreign_surface(const char *handle);

/* Events for JS: map, unmap, title, appid, geometry, move, resize, maximize, unmaximize, activate, attention,
 * minimize, fullscreen, unfullscreen, parent (the window a dialog belongs to, and whether it is modal). data is JSON. */
typedef void (*WlEventFn)(int id, const char *type, const char *json);
void wl_events(WlEventFn fn);

#endif
