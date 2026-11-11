/* framegate.h — the frame callbacks of a window nobody can see (docs/WAYLAND.md, "Hidden windows").
 *
 * A Linux app's frame callback is its "draw the next frame now". lord-server sends them after every frame it
 * presents, so a window that is minimized or on a hidden desktop, and keeps drawing (a page's animation, a game, a
 * video), was paced at the display's rate though nothing of it was on screen. Such a window gets them at most once a
 * second instead (FRAMEGATE_HIDDEN_MS), as when the screen is off (wl_frame_unseen), and on its own tick when
 * lord-server draws nothing: an app that waits for one (eglSwapBuffers) never stalls for good. It is also told
 * through xdg_toplevel's "suspended" state, so an app that knows it (lord-web, GTK 4, the browsers) stops on its own.
 * Times are lord-server's milliseconds (they wrap). Pure: tests/framegate_test.c. */
#ifndef LORD_WL_FRAMEGATE_H
#define LORD_WL_FRAMEGATE_H

#define FRAMEGATE_HIDDEN_MS 1000u

typedef struct { unsigned last; int sent; } FrameGate;

/* 1 when the window's waiting callbacks go out now: always while it can be seen, else once a second */
static inline int framegate_due(const FrameGate *g, int hidden, unsigned now) {
    return !hidden || !g->sent || now - g->last >= FRAMEGATE_HIDDEN_MS;
}

/* its callbacks went out at now */
static inline void framegate_sent(FrameGate *g, unsigned now) { g->last = now; g->sent = 1; }

/* ms until a hidden window's waiting callbacks are due (0: now) */
static inline unsigned framegate_wait(const FrameGate *g, unsigned now) {
    if (!g->sent || now - g->last >= FRAMEGATE_HIDDEN_MS) return 0;
    return FRAMEGATE_HIDDEN_MS - (now - g->last);
}

#endif
