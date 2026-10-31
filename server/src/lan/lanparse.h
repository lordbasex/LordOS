/* lanparse.h — the wire formats of Files → Network, as pure functions over byte buffers: what
 * lord-lan sends and how it reads the answers. Everything read here comes from the network and is
 * untrusted (tests/lan_test.c, with a fuzz run under ASan/UBSan). */
#ifndef LORD_LAN_PARSE_H
#define LORD_LAN_PARSE_H

#include <stddef.h>
#include <stdint.h>

#define LAN_MAX_LIST 64                 /* shares or exports kept from one server */

typedef struct { char name[128], comment[128]; } LanShare;
typedef struct { char path[256]; int open; } LanExport;

/* ---- ONC RPC over TCP (portmapper and mountd) ---- */

/* Builds a call: record mark + header + AUTH_NULL. Returns its length. */
size_t rpc_portmap_call(uint8_t *out, size_t cap, uint32_t xid);       /* GETPORT mountd v3/tcp */
size_t rpc_export_call(uint8_t *out, size_t cap, uint32_t xid);        /* MOUNTPROC3_EXPORT */
/* buf: the reply's body, without record marks. */
int    rpc_portmap_reply(const uint8_t *buf, size_t len, uint32_t xid);/* the port, 0 none, -1 invalid */
int    rpc_export_reply(const uint8_t *buf, size_t len, uint32_t xid, LanExport *out, int max); /* how many, -1 invalid */

/* ---- SMB2 ---- */

#define SMB_HDR 64
#define SMB_STATUS_MORE 0xc0000016u
#define SMB_STATUS_PENDING 0x00000103u

typedef struct {
    uint32_t status;
    uint16_t cmd;
    uint64_t session;
    uint32_t tree;
    const uint8_t *body;
    size_t blen;
} SmbMsg;

/* Reads one SMB2 message (without the 4-byte transport header): 0 ok, -1 invalid. */
int    smb_parse(const uint8_t *buf, size_t len, SmbMsg *m);

/* Requests: each writes the transport header too and returns the total length, 0 if it doesn't fit. */
size_t smb_negotiate(uint8_t *out, size_t cap, uint64_t msgid);
size_t smb_session_setup(uint8_t *out, size_t cap, uint64_t msgid, uint64_t session, const uint8_t *blob, size_t blen);
size_t smb_tree_connect(uint8_t *out, size_t cap, uint64_t msgid, uint64_t session, const char *ip);
size_t smb_create_pipe(uint8_t *out, size_t cap, uint64_t msgid, uint64_t session, uint32_t tree);
size_t smb_pipe_ioctl(uint8_t *out, size_t cap, uint64_t msgid, uint64_t session, uint32_t tree, const uint8_t fid[16], const uint8_t *in, size_t ilen);

/* NEGOTIATE reply: the dialect, 0 if invalid. signing: the server requires it. */
int    smb_negotiate_reply(const SmbMsg *m, int *signing);
/* SESSION_SETUP reply: the security blob. flags: SessionFlags (1 guest, 2 null). -1 invalid. */
int    smb_session_reply(const SmbMsg *m, const uint8_t **blob, size_t *blen, unsigned *flags);
/* CREATE reply: the file id. 0 ok. */
int    smb_create_reply(const SmbMsg *m, uint8_t fid[16]);
/* IOCTL reply: its output. 0 ok. */
int    smb_ioctl_reply(const SmbMsg *m, const uint8_t **out, size_t *olen);

/* ---- NTLMSSP: only the anonymous exchange, and the names a challenge carries ---- */
size_t ntlm_negotiate(uint8_t *out, size_t cap);
size_t ntlm_anonymous_auth(uint8_t *out, size_t cap);
/* name: the server's NetBIOS or DNS computer name, domain: its NetBIOS domain. 0 ok, -1 invalid. */
int    ntlm_challenge_names(const uint8_t *blob, size_t len, char *name, size_t nn, char *domain, size_t dn);

/* ---- DCE/RPC: srvsvc NetrShareEnum, level 1 ---- */
size_t dce_bind(uint8_t *out, size_t cap);
int    dce_bind_ack(const uint8_t *buf, size_t len);    /* 0: the interface was accepted */
size_t dce_share_enum(uint8_t *out, size_t cap, const char *ip);
/* Disk shares only, without the administrative ones (type bit 31, or a name ending in $). Returns how many, -1 invalid. */
int    dce_share_reply(const uint8_t *buf, size_t len, LanShare *out, int max);

#endif
