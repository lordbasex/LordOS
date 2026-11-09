/* sftp.h — the SFTP protocol, version 3 (draft-ietf-secsh-filexfer-02, what OpenSSH's sftp-server
 * speaks), the client's side: packets built and read with their bounds checked. Pure: lord-sftp
 * (lordsftp.c) and tests/sftp_test.c use it. Everything read here came from the server: untrusted. */
#ifndef LORD_TERM_SFTP_H
#define LORD_TERM_SFTP_H

#include <stddef.h>
#include <stdint.h>
#include "sshmux.h"

enum {
    SSH_FXP_INIT = 1, SSH_FXP_VERSION = 2, SSH_FXP_OPEN = 3, SSH_FXP_CLOSE = 4, SSH_FXP_READ = 5,
    SSH_FXP_WRITE = 6, SSH_FXP_LSTAT = 7, SSH_FXP_FSTAT = 8, SSH_FXP_SETSTAT = 9, SSH_FXP_FSETSTAT = 10,
    SSH_FXP_OPENDIR = 11, SSH_FXP_READDIR = 12, SSH_FXP_REMOVE = 13, SSH_FXP_MKDIR = 14, SSH_FXP_RMDIR = 15,
    SSH_FXP_REALPATH = 16, SSH_FXP_STAT = 17, SSH_FXP_RENAME = 18, SSH_FXP_READLINK = 19, SSH_FXP_SYMLINK = 20,
    SSH_FXP_STATUS = 101, SSH_FXP_HANDLE = 102, SSH_FXP_DATA = 103, SSH_FXP_NAME = 104, SSH_FXP_ATTRS = 105,
};
enum { SSH_FX_OK = 0, SSH_FX_EOF = 1, SSH_FX_NO_SUCH_FILE = 2, SSH_FX_PERMISSION_DENIED = 3, SSH_FX_FAILURE = 4,
       SSH_FX_BAD_MESSAGE = 5, SSH_FX_NO_CONNECTION = 6, SSH_FX_CONNECTION_LOST = 7, SSH_FX_OP_UNSUPPORTED = 8 };
enum { SSH_FILEXFER_ATTR_SIZE = 1, SSH_FILEXFER_ATTR_UIDGID = 2, SSH_FILEXFER_ATTR_PERMISSIONS = 4,
       SSH_FILEXFER_ATTR_ACMODTIME = 8, SSH_FILEXFER_ATTR_EXTENDED = 0x80000000u };
enum { SSH_FXF_READ = 1, SSH_FXF_WRITE = 2, SSH_FXF_APPEND = 4, SSH_FXF_CREAT = 8, SSH_FXF_TRUNC = 0x10, SSH_FXF_EXCL = 0x20 };

#define SFTP_PACKET_MAX   (1u << 20)      /* a reply past 1 MB is refused (DATA is asked 32 KB at a time) */
#define SFTP_CHUNK        32768u
#define SFTP_HANDLE_MAX   256u
#define SFTP_NAME_MAX     4096u           /* a path or a name past this is refused */

typedef struct { uint32_t flags; uint64_t size; uint32_t uid, gid, perm, atime, mtime; } SfAttrs;

void mxb_u64(MuxBuf *b, uint64_t v);
int  mxr_u64(MuxRd *r, uint64_t *v);
/* attributes: the extended pairs (at most 64, each bounded by the packet) are skipped. 0 ok, -1 bad. */
int  sf_get_attrs(MuxRd *r, SfAttrs *a);
void sf_put_attrs(MuxBuf *b, const SfAttrs *a);

/* A request packet's body (type, then id and the fields) — the length prefix is added on send. */
void sf_req_path(MuxBuf *b, int type, uint32_t id, const char *path);                 /* LSTAT STAT OPENDIR REMOVE RMDIR REALPATH READLINK */
void sf_req_handle(MuxBuf *b, int type, uint32_t id, const char *h, size_t hn);        /* CLOSE READDIR FSTAT */
void sf_req_open(MuxBuf *b, uint32_t id, const char *path, uint32_t pflags, const SfAttrs *a);
void sf_req_read(MuxBuf *b, uint32_t id, const char *h, size_t hn, uint64_t off, uint32_t len);
void sf_req_write(MuxBuf *b, uint32_t id, const char *h, size_t hn, uint64_t off, const void *data, uint32_t len);
void sf_req_mkdir(MuxBuf *b, uint32_t id, const char *path, uint32_t perm);
void sf_req_rename(MuxBuf *b, uint32_t id, const char *from, const char *to);
void sf_req_setstat(MuxBuf *b, uint32_t id, const char *path, const SfAttrs *a);

/* A reply's head: its type and request id (VERSION has no id: its version goes in *id). */
int  sf_reply_head(MuxRd *r, int *type, uint32_t *id);
/* STATUS: code and message (sanitized: printable ASCII, cut at cap-1). */
int  sf_status(MuxRd *r, uint32_t *code, char *msg, size_t cap);

/* One NAME entry, sanitized: a name with '/' or a control character, or past SFTP_NAME_MAX, is
 * refused (-2: skip it); owner and group come from the long name's third and fourth fields when
 * they look like names ([A-Za-z0-9._-], up to 32), else empty. */
typedef struct { char name[SFTP_NAME_MAX + 1]; char owner[33], group[33]; char kind; SfAttrs a; } SfEntry;
int  sf_name_entry(MuxRd *r, SfEntry *e);

/* A path from the user (JS): absolute, no NUL or control characters, at most SFTP_NAME_MAX. */
int  sf_path_ok(const char *p);
/* 'd' directory, 'l' link, '-' file, '?' other, from the permission bits */
char sf_kind(uint32_t perm);

#endif
