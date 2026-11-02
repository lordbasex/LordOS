/* vwire.h — protocol between axia-vault and its clients (axia-server and the Passwords app).
 *
 * SOCK_SEQPACKET socket. Each message: VHdr + JSON (UTF-8). What does not fit in one packet
 * (VW_CHUNK) is sent in several with VWF_MORE, all with the same id; the receiver joins them. The
 * fds (SCM_RIGHTS, up to VW_MAX_FDS) go in the first packet. memfd is not used: that way text with
 * secrets never sits in shared memory (which could be swapped out); on the axia-vault side it is
 * received directly into the secrets arena (smem.h).
 *
 * Types (VHdr.kind):
 *   VW_REQ  client → axia-vault: {"op":"…", …}             (id chosen by the client)
 *   VW_REP  axia-vault → client: {"ok":true,…} | {"error":"code"}   (same id)
 *   VW_EVT  axia-vault → client: {"ev":"locked"|"changed"|"touch"|"keyring"…}  (id 0)
 *   VW_CTL  axia-server → axia-vault, only over the control channel (fd 3): init, attach, lock-all,
 *           session-password, fido-device, bus. The app can never send them. */
#ifndef LORD_VAULT_VWIRE_H
#define LORD_VAULT_VWIRE_H

#include <stddef.h>
#include <stdint.h>

#define VW_CHUNK   (32 * 1024)
#define VW_MAX     (64u << 20)         /* largest message accepted */
#define VW_MAX_FDS 4

enum { VW_REQ = 1, VW_REP = 2, VW_EVT = 3, VW_CTL = 4 };
enum { VWF_MORE = 1 };

typedef struct { uint32_t kind, id, len, flags; } VHdr;

typedef struct {
    uint32_t kind, id;
    char *json; size_t len;            /* 0-terminated */
    int fds[VW_MAX_FDS]; int nfds;
} VMsg;

/* Allocator for received data: in axia-vault, the secrets arena; in axia-server, malloc. */
typedef struct { void *(*alloc)(size_t); void *(*realloc)(void *, size_t); void (*free)(void *); } VwAlloc;
extern const VwAlloc vw_malloc;

int  vw_send(int sock, uint32_t kind, uint32_t id, const char *json, size_t len, const int *fds, int nfds);
/* Receives a complete message (blocks until it has one). 0 ok, 1 end, -1 error/invalid. */
int  vw_recv(int sock, VMsg *m, const VwAlloc *a);
/* Incremental receive for non-blocking sockets (axia-vault serves several channels):
 * vw_feed reads whatever is available; returns 1 if a complete message is in *m, 0 if incomplete, -1 closed. */
typedef struct { VMsg cur; size_t cap; int active; } VwRx;
int  vw_feed(int sock, VwRx *rx, VMsg *m, const VwAlloc *a);
void vw_rx_free(VwRx *rx, const VwAlloc *a);
void vw_msg_free(VMsg *m, const VwAlloc *a);   /* wipes the text (may hold secrets) and closes the fds */

#endif
