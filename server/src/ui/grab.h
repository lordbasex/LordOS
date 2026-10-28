/* grab.h — moving and resizing windows in C (docs/WINDOWS.md).
 *
 * The theme starts the drag (lord.wm.grab) and lord-server follows it on its own: each frame moves the
 * window's node with the pointer, without calling JS or repainting its content (when moving,
 * the window is painted once into a layer and then only copied). On release, JS receives the
 * result (__lordWm('end', …)) and updates the window manager. */
#ifndef LORD_GRAB_H
#define LORD_GRAB_H

#include "ui.h"

typedef struct {
    float inset;            /* the node is the frame enlarged by `inset` px per side (edges for resizing) */
    float min_w, min_h;     /* of the frame (manifest minSize) */
    float max_w, max_h;     /* 0 = no maximum */
    float min_top;          /* the frame goes no higher than this (menu bar) */
    /* several displays (docs/DISPLAYS.md): the top of each one's work area for the horizontal span
     * [ax, ax + aw); the frame's center decides which applies (min_top if none) */
    int nareas;
    float ax[4], aw[4], ay[4];
} GrabLimits;

/* main.c: where the pointer is and which buttons are pressed (mask, bit = DOM button). */
void grab_note_pointer(float x, float y, int buttons);
/* edges: 0 = move; otherwise xdg edges (1 top, 2 bottom, 4 left, 8 right). Only with a
 * button pressed. 0 if it started. */
int  grab_begin(int node, int edges, const GrabLimits *lim);
int  grab_active(void);
int  grab_cursor(void);                     /* cursor shape during the drag (cursor.h) */
/* Where the pointer is now and the drag's edges (0 = moving); 1 while a drag is active. Read by the
 * theme a few times per second for the window snapping preview (lord.wm.pointer, docs/WINDOWS.md). */
int  grab_state(float *x, float *y, int *edges);
void grab_motion(float x, float y);         /* once per frame, with the accumulated pointer */
/* Finished (the button was released): 1 and the node's change relative to the start. */
int  grab_finish(int *node, int *edges, float *dx, float *dy, float *dw, float *dh);
void grab_cancel(void);
/* LordBench's desktop scene (bench/lbframes.h, docs/BENCHMARK.md): the window's node moves along a
 * figure of eight (rx, ry px; one turn every period_ms) through the same path as a drag, one step per
 * frame, with no button held and no JS per frame. Pointer input or grab_cancel() stops it. 0 if it
 * started. main.c calls grab_script_tick() before deciding whether to draw. */
int  grab_script_begin(int node, const GrabLimits *lim, float rx, float ry, float period_ms);
int  grab_scripted(void);
void grab_script_tick(double now_ms);

/* paint.c: layer of the moving window and of what is below. When resizing (resize = 1) the
 * window is repainted every frame, but what is below is still saved. */
void paint_layer_begin(int node, int resize);
void paint_layer_end(void);
void paint_layer_external_damage(void);     /* something changed inside (terminal, Linux app, video) */
/* For drag measurements: times the layer was painted and times it was only copied (and resets to 0). */
void paint_layer_stats(long *renders, long *blits);
long paint_layer_parts(void);               /* of those renders, the ones of only a part (layer.part) */

#endif
