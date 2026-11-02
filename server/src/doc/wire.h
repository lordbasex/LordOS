/* wire.h — axia-doc protocol messages (proto.h): send and receive a DocHdr + JSON + fds.
 * Both sides use it: axia-server (doc.c) and the helper (helper.c). */
#ifndef LORD_DOC_WIRE_H
#define LORD_DOC_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include "proto.h"

typedef struct {
    DocHdr h;
    char *json;             /* NUL-terminated (malloc); NULL if none came */
    size_t len;
    int fds[DOC_MAX_FDS];
    int nfds;
} DocMsg;

/* Sends op/id + JSON (len bytes; if it exceeds DOC_INLINE_MAX it goes in a sealed memfd) + fds.
 * 0 on success, -1 error (errno). */
int  doc_send(int sock, uint32_t op, uint32_t id, const char *json, size_t len, const int *fds, int nfds);
/* Receives a complete message (blocking). 0 on success, 1 end of channel, -1 error or invalid message.
 * The received fds stay in m->fds (with CLOEXEC); free with doc_msg_free. */
int  doc_recv(int sock, DocMsg *m);
void doc_msg_free(DocMsg *m);

/* sealed memfd (SHRINK/GROW/WRITE) with the given bytes; -1 on failure. */
int  doc_memfd(const char *name, const void *data, size_t len);
/* memfd of len bytes to write with mmap and seal later with doc_seal. */
int  doc_memfd_new(const char *name, size_t len);
int  doc_seal(int fd);
/* Checks that a foreign memfd is sealed against shrinking (otherwise an ftruncate from the other side
 * would cause SIGBUS when reading it) and has at least min bytes. Returns the size or -1. */
long doc_check_sealed(int fd, size_t min);

/* JSON text builder (grows on its own). */
typedef struct { char *p; size_t n, cap; int oom; } JBuf;
void jb_init(JBuf *b);
void jb_free(JBuf *b);
void jb_raw(JBuf *b, const char *s);
void jb_printf(JBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void jb_str(JBuf *b, const char *s);        /* "text" with escapes (invalid UTF-8 → U+FFFD) */
void jb_strn(JBuf *b, const char *s, size_t n);
void jb_num(JBuf *b, double v);             /* JSON number (no NaN/Inf) */

#endif
