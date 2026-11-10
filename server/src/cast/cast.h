/* cast.h — sharing a screen or a window with a Linux app (the ScreenCast portal, ui/portal.c): lord-server's side of
 * lord-screencast (cast.c, castproto.h). Everything is on the main loop and never waits for the helper.
 *
 * Zero cost when nothing is shared: no helper runs, nothing is painted or copied and cast_damage returns at once.
 * While a session runs, its sources are painted again in capture mode (paint_capture: protected windows black), only
 * where they changed, into a canvas of their own; a frame is copied into the ring only when the consumer asked for
 * frames, the indicator is on screen and its period passed. */
#ifndef LORD_CAST_CAST_H
#define LORD_CAST_CAST_H

#include <poll.h>
#include <stdint.h>

enum { CAST_ALSO_MAX = 8 };             /* PAINT_ALSO_MAX */

/* What the person picked: a display (its connector id, as lord.screen.displays' id) or a window (wm.frameNode, and its
 * sheets painted over it). A node id is reused once its node is destroyed, so each id goes with the serial of the node
 * it named when the person picked (Node.serial, node_ref): the share follows that node only, and when it is gone the
 * share ends (a sheet that closed is no longer painted), whatever has the id now. */
typedef struct {
    int kind;                           /* CAST_SRC_MONITOR or CAST_SRC_WINDOW */
    uint32_t display;
    int node;
    uint64_t serial;
    int also[CAST_ALSO_MAX], nalso;
    uint64_t also_serial[CAST_ALSO_MAX];
} CastSource;

/* What the app is told of each stream (Start's streams): the node, where it is and its size in logical px. */
typedef struct { uint32_t node; int x, y, w, h, kind; } CastStreamInfo;

/* lord-screencast is next to lord-server, root's or ours and not writable by others. */
int  cast_available(void);
/* Starts the helper for portal session id with n sources: 0, and later portal_cast_streams or portal_cast_failed. */
int  cast_begin(int id, const CastSource *src, int n, int cursor_embedded);
/* The session's helper goes (a stop, then a kill if it lingers); nothing is said to the portal. */
void cast_end(int id);
/* A connection for the app restricted to the session's streams: 0, and later portal_cast_remote(id, req, fd). */
int  cast_remote(int id, uint32_t req);
/* The indicator for session id is on screen: its frames may go. */
void cast_shown(int id);
/* Sessions with a helper. */
int  cast_running(void);
/* Every session ends (a lock, the shell going): portal_cast_lost for each. */
void cast_end_all(void);

/* main.c: what changed on screen this frame (logical px), and the pointer. */
void cast_damage(float x0, float y0, float x1, float y1);
void cast_pointer(int x, int y);
int  cast_pollfds(struct pollfd *p, int max);
void cast_dispatch(const struct pollfd *p, int n);
void cast_tick(void);                   /* the frames due, the sources' sizes, helpers that are late */
double cast_timeout(void);              /* ms until cast_tick has work, -1 none */

/* The chooser's picture of a source: a media id (lord.media's, closed by the shell) at most max px wide, 0 if the
 * source isn't there. */
int  cast_thumb(const CastSource *src, int max);

#endif
