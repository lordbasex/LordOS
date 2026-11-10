/* castproto.h — lord-server ↔ lord-screencast, the screen and window sharing helper (see docs/WAYLAND.md,
 * "Sharing a screen or a window", and docs/FLATPAK.md, "ScreenCast").
 *
 * lord-server owns everything that decides: which source, its pixels (protected windows already black), the PipeWire
 * connections (it connects them; the helper can't make a socket). lord-screencast only turns frames into a PipeWire
 * video source node and restricts a client connection to that node:
 *   - the channel is a SOCK_SEQPACKET socketpair on the helper's fd 3: one CastMsg per packet, at most one fd with it;
 *   - CM_HELLO brings the helper's own PipeWire connection, CM_STREAM one stream each: its size and its ring, a memfd
 *     lord-server writes and the helper only reads (a read-only reopen, sealed against shrinking and growing);
 *   - the ring holds CAST_SLOTS whole frames. lord-server writes a slot no one holds, then sends CM_FRAME; the slot is
 *     the helper's until its CM_DONE. Frames are written only when the source changed and the stream's consumer is
 *     there (CM_WANT on), never faster than the rate it asked for;
 *   - CM_CLIENT brings a second connection of the app's; it comes back in CM_REMOTE, able to see only the streams.
 * Each side checks what the other sends (cast_check_helper, cast_check_server): a message that isn't valid now ends
 * the session. No I/O here but cast_send and cast_recv, so tests/castproto_test.c covers all of it. */
#ifndef LORD_CAST_CASTPROTO_H
#define LORD_CAST_CASTPROTO_H

#include <stddef.h>
#include <stdint.h>

#define CAST_MAX_STREAMS 4              /* sources in one session (multiple = true) */
#define CAST_SLOTS 3
#define CAST_MAX_DIM 8192
#define CAST_MIN_DIM 16
#define CAST_MAX_FPS 60
#define CAST_MAX_REMOTES 4              /* OpenPipeWireRemote calls waiting for the helper at once */
#define CAST_RING_MAGIC 0x54534143u     /* "CAST" */
#define CAST_RING_HDR 4096              /* the header's page; slot 0 starts after it */

enum {
    /* lord-server → helper */
    CM_HELLO = 1,                       /* fd: its PipeWire connection; a: streams that follow */
    CM_STREAM,                          /* stream; fd: the ring; w, h; a: the most frames a second; b: CAST_SRC_* */
    CM_FRAME,                           /* stream; a: slot; w, h as in its CM_STREAM; seq */
    CM_CLIENT,                          /* fd: a new connection of the app's; a: the request's id */
    CM_STOP,
    /* helper → lord-server */
    CM_NODE = 16,                       /* stream; a: its PipeWire node id */
    CM_WANT,                            /* stream; a: 1 a consumer is streaming, 0 not; b/c: the rate it asked for */
    CM_DONE,                            /* stream; a: the slot is free again */
    CM_REMOTE,                          /* fd: the restricted connection; a: the request's id (no fd: it failed) */
    CM_FAIL,                            /* a: CAST_E_*; the helper ends */
};
enum { CAST_SRC_MONITOR = 1, CAST_SRC_WINDOW = 2 };
enum { CAST_E_PIPEWIRE = 1, CAST_E_STREAM, CAST_E_RING, CAST_E_PROTOCOL };

typedef struct {
    uint16_t type, stream;
    uint32_t a, b, c;
    uint32_t w, h;
    uint64_t seq;
} CastMsg;

/* The ring's header, written once by lord-server; the helper checks it against its CM_STREAM. */
typedef struct {
    uint32_t magic, w, h, stride, slots, pad;
    uint64_t slot_bytes;
} CastRingHdr;

typedef struct {
    uint32_t stride;                    /* bytes per row: w * 4 */
    uint64_t slot_bytes;                /* one frame, rounded up to a page */
    uint64_t total;                     /* header + every slot */
} CastLayout;

/* The ring for a w × h source: 0, or -1 for a size outside CAST_MIN_DIM..CAST_MAX_DIM. */
int cast_layout(uint32_t w, uint32_t h, CastLayout *l);
/* Offset of slot s in the ring. */
static inline uint64_t cast_slot_off(const CastLayout *l, uint32_t s) { return CAST_RING_HDR + (uint64_t)s * l->slot_bytes; }
/* The helper's check of a mapped ring of len bytes against its CM_STREAM: 0 ok. */
int cast_ring_ok(const void *map, uint64_t len, uint32_t w, uint32_t h);

/* What lord-server knows of a helper, to check each of its messages. */
typedef struct {
    int nstreams;
    uint32_t node[CAST_MAX_STREAMS];    /* 0 until its CM_NODE */
    uint8_t busy[CAST_MAX_STREAMS][CAST_SLOTS];
    uint32_t remote[CAST_MAX_REMOTES];  /* request ids waiting for a CM_REMOTE; 0 free */
} CastPeer;

/* A message from the helper with nfds fds: 0 valid now (and the peer is updated: a node is noted, a slot freed, a
 * request answered), -1 not (the session ends). */
int cast_check_helper(CastPeer *p, const CastMsg *m, int nfds);
/* lord-server: a free slot of stream s, now the helper's (busy) — -1 if it holds all of them. */
int cast_slot_take(CastPeer *p, int s);
/* lord-server: a request id waiting for its CM_REMOTE; -1 if CAST_MAX_REMOTES already wait. */
int cast_remote_add(CastPeer *p, uint32_t req);

/* What the helper knows, to check lord-server's messages. */
typedef struct {
    int hello;
    int nstreams, have[CAST_MAX_STREAMS];
    uint32_t w[CAST_MAX_STREAMS], h[CAST_MAX_STREAMS];
} CastSide;
int cast_check_server(CastSide *s, const CastMsg *m, int nfds);

/* The rate the consumer asked for (b/c of CM_WANT) as ms between frames, capped at CAST_MAX_FPS; 0/x or none:
 * CAST_MAX_FPS. */
double cast_period_ms(uint32_t num, uint32_t den);

/* One message, with fd (or -1). 0 sent, -1 not (errno; EAGAIN when the socket is full). */
int cast_send(int sock, const CastMsg *m, int fd);
/* One message, non-blocking: 1 read (*fd: the fd that came with it, CLOEXEC, or -1), 0 nothing yet, -1 the channel
 * ended or the packet wasn't one message with at most one fd (any fds that came are closed). */
int cast_recv(int sock, CastMsg *m, int *fd);

#endif
