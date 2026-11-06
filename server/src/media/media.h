/* media.h — media in lord-server (see media.c and docs/MEDIA.md).
 *
 * lord-server NEVER links FFmpeg: it launches its helper lord-media (fixed path, no arguments) and
 * passes it the files already open. What lives here:
 *   - the helpers (player, camera, recordings and two «workers» for short jobs);
 *   - the image «sources» that a `media` node (<MediaView>) draws: video frames over
 *     shared memory, decoded photos, screenshots;
 *   - the captures: screen, recording and remote desktop, always with the protected
 *     windows (`protected`) in black. */
#ifndef LORD_MEDIA_H
#define LORD_MEDIA_H

#include <poll.h>
#include <stdint.h>
#include "../canvas.h"

struct Node;

/* main loop (main.c) */
int  media_pollfds(struct pollfd *p, int max);
void media_dispatch(const struct pollfd *p, int n);
/* Helpers already told to stop are waited for here, on every pass of the loop, whether or not any source
 * is left; media_reap_timeout: ms until the next look, -1 with none pending. */
void media_reap(void);
double media_reap_timeout(void);
typedef void (*MediaEventFn)(int id, const char *type, const char *json);
void media_events(MediaEventFn fn);
int  media_wants_frame(void);           /* a recording is waiting for the next frame */
int  media_needs_cpu(void);             /* recording the screen: composite on the CPU (like the remote desktop) */
void media_shutdown(void);              /* close everything (theme change or exit) */

/* drawing of a media node (paint.c) */
void media_paint(Canvas *c, int handle, int node, float x, float y, float w, float h, float radius, float op);

/* The pointer of a source that brings its own (a Machines guest): b, g, r, a bytes with straight alpha, w × h,
 * hotspot; NULL: none (the host's arrow). media_src_cursor_visible: the source hid or showed it. */
void media_src_cursor(int id, const uint8_t *bgra, int w, int h, int hx, int hy);
void media_src_cursor_visible(int id, int on);
/* Over a media node of source id (main.c, cursor_now): 0 the host's pointer (none sent, or the source turned its own
 * off), 1 *px… (premultiplied, at the size the picture is shown, at most 64 × 64), 2 hidden (a fully transparent
 * image). *gen changes when the image does. */
int  media_node_cursor(int id, const uint32_t **px, int *w, int *h, int *hx, int *hy, unsigned *gen);
/* changes with every change of a source's pointer (main.c marks the cursor to draw again) */
unsigned media_cursor_serial(void);

/* Opaque media node that contains the whole region (x0,y0)-(x1,y1), to paint only from it (paint.c). 0 if there is none. */
int  media_cover_node(float x0, float y0, float x1, float y1);

/* captures (main.c): after painting the scene and after assembling each frame. */
void media_scene_painted(const Canvas *scene);
typedef struct { int x, y, mode; const uint32_t *px; int w, h, hx, hy; } MediaCursor;
/* Returns what the remote desktop sees (protected windows in black) and feeds the
 * recording. out = the frame already composited (without cursor). */
const Canvas *media_composed(const Canvas *out, int remote, const MediaCursor *cur);

/* Protected windows present in the tree (counted at the moment). */
int media_protected_count(void);

#endif
