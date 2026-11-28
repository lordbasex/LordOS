/* fsfocus.h — who has the keyboard while a Linux window is fullscreen (wl.c: wl_set_focus, wl_key, wl_pointer,
 * enter_fullscreen; issue #175).
 *
 * A fullscreen window owns its own display only (wl.c on_fs). The keys follow the window that was chosen (a click,
 * the theme's focus), not where the pointer happens to be: a window chosen on another display keeps them when the
 * pointer goes back over the fullscreen one, and clicking the fullscreen window gives them back to it. A window
 * under the fullscreen one (on its display, so not seen) never takes them: the fullscreen window keeps them. With
 * one display every other window is under it, so the fullscreen window always has the keys, as before. */
#ifndef LORD_FSFOCUS_H
#define LORD_FSFOCUS_H

typedef struct { float x, y, w, h; } FsfRect;

/* A window's box on the fullscreen window's display: hidden under it. whole: the fullscreen window covers the
 * whole space (one display, or its display unknown), so every window is. The display a window is on is the one
 * with its centre. */
static inline int fsf_hidden(FsfRect win, FsfRect fs, int whole) {
    if (whole) return 1;
    float cx = win.x + win.w / 2, cy = win.y + win.h / 2;
    return cx >= fs.x && cx < fs.x + fs.w && cy >= fs.y && cy < fs.y + fs.h;
}

/* 1 if the fullscreen window takes the keys instead of the window wanted (asked for by wl_set_focus, or holding
 * them at a key press in wl_key): never instead of itself; instead of a window hidden under it; and instead of no
 * window (a LordOS window or the desktop) only when that was chosen with the pointer on its display (one display:
 * always, as before; another display: never, wherever the pointer goes afterwards). ptr_on_fs is where the pointer
 * was when "no window" was chosen (wl.c none_on_fs), not where it is at each key: a LordOS window chosen on another
 * display lost the keys to the fullscreen window as soon as the pointer rested on the fullscreen display again
 * (BUG-30 of the ws170 QA). */
static inline int fsf_take(int want_is_fs, int want_none, int want_hidden, int ptr_on_fs) {
    if (want_is_fs) return 0;
    return want_none ? ptr_on_fs : want_hidden;
}

/* The pointer side for "no window": where it was when that was chosen (chosen: 1 on the fullscreen display, 0 on
 * another, -1 not chosen since this fullscreen began), else where it is now. */
static inline int fsf_none_ptr(int chosen, int now) { return chosen >= 0 ? chosen : now; }

#endif
