/* doc.h — axia-server side of the axia-doc document helper (see proto.h).
 *
 * A «session» is a axia-doc process: an open PDF, an extraction, a compression or the
 * utilities helper (fonts, PDF for printing, images). Everything is asynchronous: a dedicated
 * thread talks to the helpers and leaves the results in a queue that JS reads with doc_poll()
 * (lord.doc.poll). The compositor never waits for a helper; if one hangs, it is killed when
 * the request deadline expires, and if it crashes, its requests end with a "crash" error. */
#ifndef LORD_DOC_DOC_H
#define LORD_DOC_DOC_H

#include <stddef.h>
#include <stdint.h>

typedef struct { int fd; const char *role; } DocInitFd;      /* role: "file" | "ro" | "rw" | "out" */

/* Fixed path of the helper: <folder of this binary>/axia-doc. 0 if it exists. */
int  doc_available(void);
/* New session with these startup fds (doc_session keeps them, also on failure).
 * mode: "pdf" | "arc" | "util". Returns the id (> 0) or -1. */
int  doc_session(const char *mode, DocInitFd *fds, int n);
/* Request to a session: op DQ_*, JSON arguments, extra fds (it keeps them), priority (higher
 * first) and deadline in ms (0 = no deadline, but it's killed if 60 s pass without progress). id > 0 or -1. */
int  doc_request(int sid, uint32_t op, const char *json, size_t len, int *fds, int nfds, int prio, int timeout_ms);
int  doc_cancel(int sid, int req);         /* removes from the queue a request that hasn't started: 1 if it was there */
void doc_close(int sid);
int  doc_alive(int sid);                   /* 1 if the session exists and hasn't crashed */
/* Pending events as a JSON array (malloc) or NULL if there are none. Each one:
 * {sid, req, type: ready|done|err|prog|part|img|blob|crash, data, image, w, h, blob, size} */
char *doc_poll(void);
int  doc_pending(void);                     /* unfinished requests (to know whether to keep polling) */

/* Returned images (premultiplied ARGB32). doc_image_lock keeps the pointer valid until
 * doc_image_unlock. */
const uint32_t *doc_image_lock(int handle, int *w, int *h);
void doc_image_unlock(void);
void doc_image_release(int handle);
size_t doc_image_bytes(void);

/* Returned bytes (an assembled or saved PDF): sealed memfd. */
int  doc_blob_dup(int handle, size_t *size);   /* new fd (CLOEXEC) or -1 */
void doc_blob_release(int handle);
/* Saves a blob as a new file (fd already open for writing). 0 on success. */
int  doc_blob_copy(int handle, int outfd);

#endif
