/* syncobj.h — explicit sync for Linux apps (linux-drm-syncobj-v1, docs/SYNCOBJ-PLAN.md, docs/WAYLAND.md).
 *
 * An app hands lord-server two timeline points with each buffer it commits: the acquire point (its rendering is done)
 * and the release point (lord-server is done with the buffer). A commit whose acquire point has not been signalled yet
 * is not applied: it waits in its surface's queue (the commits after it wait behind it, in order) while the main loop
 * polls an eventfd the kernel signals with the point (DRM_IOCTL_SYNCOBJ_EVENTFD); nothing ever blocks. The release
 * point is signalled when lord-server stops using the buffer: at once after a CPU copy, after the frame that replaced
 * it on the GPU path (with the buffer's own fences, so the GPU's last read is waited for in the kernel, not here), and
 * always when a frame is dropped (its surface destroyed, its client gone).
 *
 * The first half is pure (tests/syncobj_test.c): the protocol's error rules, the per-client limits and the commit
 * queue. The second half is the kernel side (syncobj.c): raw DRM ioctls on lord-server's own render node fd, without
 * libdrm, as kms.c. */
#ifndef LORD_WL_SYNCOBJ_H
#define LORD_WL_SYNCOBJ_H

#include <stdint.h>
#include <sys/types.h>

/* ---- pure ---- */

/* wp_linux_drm_syncobj_surface_v1's errors (the protocol's values); 0 = the commit is fine */
enum {
    SOBJ_OK = 0,
    SOBJ_ERR_NO_SURFACE = 1,
    SOBJ_ERR_UNSUPPORTED_BUFFER = 2,
    SOBJ_ERR_NO_BUFFER = 3,
    SOBJ_ERR_NO_ACQUIRE_POINT = 4,
    SOBJ_ERR_NO_RELEASE_POINT = 5,
    SOBJ_ERR_CONFLICTING_POINTS = 6,
};

/* Limits (all fail closed: past one the client is disconnected with no_memory). SOBJ_MAX_PENDING is 8, not the plan's
 * first 4: a Vulkan swapchain in mailbox mode has up to 5 images, all of which can be committed before the first one's
 * rendering is done, and a state-only commit between them (a resize's ack) waits in the same queue. */
#define SOBJ_MAX_TIMELINES 64       /* imported timelines alive, per client */
#define SOBJ_MAX_SURFACES 256       /* wp_linux_drm_syncobj_surface_v1 objects alive, per client */
#define SOBJ_MAX_PENDING 8          /* commits waiting in one surface's queue */
#define SOBJ_MAX_WAITS_CLIENT 32    /* eventfds alive, per client (one per surface whose first queued commit waits) */
#define SOBJ_MAX_WAITS 128          /* eventfds alive in the session (half of wl.c's 256 surfaces) */
#define SOBJ_POLL_MS 4              /* past the session's eventfds: how often a waiting point is checked instead */
#define SOBJ_MAX_POLLS 64           /* surfaces waiting that way, in the session (one shared timer checks them all);
                                     * each counts in its client's and process's waits like an eventfd. Past it a
                                     * commit is applied as it is (implicit sync), never a disconnection */
#define SOBJ_WAIT_LOG_MS 5000       /* a commit waiting this long is logged, once per surface */

static inline uint64_t sobj_point(uint32_t hi, uint32_t lo) { return (uint64_t)hi << 32 | lo; }

/* The rules checked at wl_surface.commit, for a surface with a syncobj object: points iff a non-null buffer is
 * attached in this commit, that buffer a dmabuf, and on one timeline the acquire point strictly below the release
 * point. attached: wl_surface.attach since the last commit; buffer: it was not null. */
static inline int sobj_commit_error(int attached, int buffer, int dmabuf, int has_acquire, int has_release,
                                    int same_timeline, uint64_t acquire, uint64_t release) {
    int with_buffer = attached && buffer;
    if (!with_buffer) return has_acquire || has_release ? SOBJ_ERR_NO_BUFFER : SOBJ_OK;
    if (!dmabuf) return SOBJ_ERR_UNSUPPORTED_BUFFER;
    if (!has_acquire) return SOBJ_ERR_NO_ACQUIRE_POINT;
    if (!has_release) return SOBJ_ERR_NO_RELEASE_POINT;
    if (same_timeline && acquire >= release) return SOBJ_ERR_CONFLICTING_POINTS;
    return SOBJ_OK;
}

/* What a commit does: applied now (nothing before it waits, and its acquire point, if any, is signalled), queued
 * behind an earlier waiting commit, or queued as the first one, waiting for its own point (an eventfd). */
enum { SOBJ_APPLY = 0, SOBJ_QUEUE_BEHIND = 1, SOBJ_QUEUE_WAIT = 2 };
static inline int sobj_commit_action(int queued, int has_acquire, int signalled) {
    if (queued) return SOBJ_QUEUE_BEHIND;
    if (has_acquire && !signalled) return SOBJ_QUEUE_WAIT;
    return SOBJ_APPLY;
}

/* A surface's queue of commits not applied yet, oldest first (the entries are the caller's) */
typedef struct { void *e[SOBJ_MAX_PENDING]; int head, n; } SobjQueue;

/* -1 when full (the client is disconnected) */
static inline int sobjq_push(SobjQueue *q, void *e) {
    if (q->n >= SOBJ_MAX_PENDING) return -1;
    q->e[(q->head + q->n) % SOBJ_MAX_PENDING] = e;
    q->n++;
    return 0;
}
static inline void *sobjq_front(const SobjQueue *q) { return q->n ? q->e[q->head] : 0; }
static inline void *sobjq_pop(SobjQueue *q) {
    if (!q->n) return 0;
    void *e = q->e[q->head];
    q->head = (q->head + 1) % SOBJ_MAX_PENDING;
    q->n--;
    return e;
}

/* How a commit waits for its acquire point, from the eventfds alive for this connection, for every connection of the
 * same process, and in the session. Only the client past its own share is ever disconnected: a full session never
 * costs another client its connection, its commit just waits by checking the point every SOBJ_POLL_MS instead. */
enum { SOBJ_WAIT_EVENTFD = 0, SOBJ_WAIT_POLL = 1, SOBJ_WAIT_REFUSE = 2 };
static inline int sobj_wait_mode(int client_waits, int process_waits, int session_waits) {
    if (client_waits >= SOBJ_MAX_WAITS_CLIENT) return SOBJ_WAIT_REFUSE;           /* this connection's own cap */
    if (session_waits < SOBJ_MAX_WAITS) return SOBJ_WAIT_EVENTFD;
    if (process_waits >= SOBJ_MAX_WAITS_CLIENT) return SOBJ_WAIT_REFUSE;          /* the process filling the session */
    return SOBJ_WAIT_POLL;
}

/* ---- the kernel (syncobj.c) ---- */

/* An imported timeline: a handle on lord-server's own DRM fd, shared by the protocol object and every point that still
 * has to be waited for or signalled (a client may destroy the object with points pending: they stay valid) */
typedef struct SobjTimeline { uint32_t handle; int refs; } SobjTimeline;

/* A point to wait for or signal; tl NULL = none */
typedef struct { SobjTimeline *tl; uint64_t point; } SobjPoint;

/* Opens the render node it works on: dev (the GPU context's, or 0) if it has timelines, else the first render node that
 * does. 0 when explicit sync can be offered: timeline syncobjs and SYNCOBJ_EVENTFD (Linux 6.6) are there. Once. */
int sobj_init(dev_t dev);
int sobj_available(void);

/* The client's timeline fd → a timeline with one reference (the fd is the caller's); NULL if it is not a syncobj */
SobjTimeline *sobj_import(int fd);
SobjTimeline *sobj_ref(SobjTimeline *t);
void sobj_unref(SobjTimeline *t);

/* 1 signalled, 0 not yet (or not even submitted), -1 an error */
int sobj_signalled(SobjPoint p);
/* An eventfd (non-blocking) that becomes readable when the point is signalled; -1 on error */
int sobj_eventfd(SobjPoint p);
/* Signals the point now (the CPU is done with the buffer); then forgets it (p->tl unreferenced, NULL) */
void sobj_release(SobjPoint *p);
/* Signals the point when the buffer's fences are (dmabuf_fd: its first plane, -1 = now): the GPU's reads of it are
 * waited for in the kernel; then forgets it. 1 when it went with the buffer's fences, 0 signalled at once (or no point) */
int sobj_release_after(SobjPoint *p, int dmabuf_fd);

#endif
