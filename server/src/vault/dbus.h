/* dbus.h — own minimal D-Bus client (no libdbus or sd-bus), just enough to serve the
 * Secret Service (secretsvc.c) from axia-vault. See docs/PASSWORDS.md, "Secret Service".
 *
 *   - Messages (D-Bus specification 0.41): fixed header + a(yv) fields + body, per-type
 *     alignment, little endian when writing and both orders when reading. Types: y b n q i u x t d s
 *     o g a ( ) { } v (no h: fds are not passed).
 *   - Strict reading: dbus_msg_parse validates EVERYTHING before delivering the message (lengths, signatures,
 *     depth, NUL-terminated strings with no embedded NUL, booleans 0/1, object paths,
 *     zero alignment and padding, 128 MiB cap). After that the typed reader (DRd) only reads
 *     what was already validated and still checks the bounds.
 *   - Connection: SASL EXTERNAL over an ALREADY connected socket (axia-vault cannot open sockets:
 *     axia-server hands it over), Hello, RequestName; non-blocking send with an output queue.
 *
 * The connection buffers are malloc'd (not in the secrets arena, which is wiped entirely on
 * lock) and are wiped with explicit_bzero before being released or reused. */
#ifndef LORD_VAULT_DBUS_H
#define LORD_VAULT_DBUS_H

#include <stddef.h>
#include <stdint.h>

#define DBUS_MAX_MSG    (128u * 1024 * 1024)
#define DBUS_MAX_DEPTH  32               /* nested containers (the specification allows 64) */

enum { DBUS_CALL = 1, DBUS_RETURN = 2, DBUS_ERROR = 3, DBUS_SIGNAL = 4 };
enum { DBUS_NO_REPLY = 1, DBUS_NO_AUTOSTART = 2 };

/* A received message. The strings point inside raw (NUL-terminated); NULL if absent. */
typedef struct {
    uint8_t type, flags, big_endian;
    uint32_t serial, reply_serial;
    const char *path, *iface, *member, *error, *dest, *sender, *sig;
    const uint8_t *body;
    uint32_t body_len;
    uint8_t *raw;                        /* malloc; dbus_msg_free wipes it */
    size_t raw_len;
} DMsg;

/* Total length of the message starting at buf (at least 16 bytes), 0 if more bytes are needed to know
 * or -1 if the header is invalid or exceeds the cap. */
long dbus_msg_size(const uint8_t *buf, size_t len);
/* Validates and builds m from a COPY of buf[0..len) (len = dbus_msg_size). 0 ok. */
int  dbus_msg_parse(const uint8_t *buf, size_t len, DMsg *m);
void dbus_msg_free(DMsg *m);

/* Standalone validations */
int  dbus_sig_valid(const char *sig);            /* complete signature (several types), ≤ 255 */
int  dbus_sig_single(const char *sig, size_t *n);/* length of ONE complete type at the start; 0 ok */
int  dbus_path_valid(const char *p);
int  dbus_name_valid(const char *n);             /* interface/bus name (no unique ':') */
/* Validates a body with that signature (alignment relative to 'base', which must be 8-aligned). */
int  dbus_body_valid(const char *sig, const uint8_t *body, size_t len, int big_endian);

/* ---- typed body reader ---- */
typedef struct {
    const uint8_t *b;
    size_t len, pos;
    int be, err;
} DRd;
void     dr_init(DRd *r, const DMsg *m);
uint8_t  dr_u8(DRd *r);
int      dr_bool(DRd *r);
uint32_t dr_u32(DRd *r);
int32_t  dr_i32(DRd *r);
uint64_t dr_u64(DRd *r);
const char *dr_str(DRd *r, uint32_t *len);       /* s, or (y g with dr_sig) */
const char *dr_sig(DRd *r);
/* ay: pointer to the bytes and length */
const uint8_t *dr_bytes(DRd *r, uint32_t *len);
/* array: dr_arr returns the end; while dr_more(r, end) there are elements. elem_align: 1/4/8 */
size_t   dr_arr(DRd *r, int elem_align);
int      dr_more(DRd *r, size_t end);
void     dr_struct(DRd *r);                      /* aligns to 8 */
/* v: signature of the content (to compare) */
const char *dr_variant(DRd *r);
/* Skips a value of complete type sig (already validated). */
void     dr_skip(DRd *r, const char *sig);

/* ---- writer ---- */
typedef struct { uint8_t *p; size_t n, cap; int oom; } DBuf;
void   dw_init(DBuf *w);
void   dw_free(DBuf *w);                         /* wipes and frees */
void   dw_align(DBuf *w, int a);
void   dw_u8(DBuf *w, uint8_t v);
void   dw_bool(DBuf *w, int v);
void   dw_u32(DBuf *w, uint32_t v);
void   dw_i32(DBuf *w, int32_t v);
void   dw_u64(DBuf *w, uint64_t v);
void   dw_str(DBuf *w, const char *s);           /* s or o */
void   dw_strn(DBuf *w, const char *s, size_t n);
void   dw_sig(DBuf *w, const char *s);           /* g */
void   dw_bytes(DBuf *w, const uint8_t *p, size_t n);   /* ay */
size_t dw_arr_begin(DBuf *w, int elem_align);   /* returns a mark for dw_arr_end */
void   dw_arr_end(DBuf *w, size_t mark);
void   dw_struct(DBuf *w);                       /* aligns to 8 */
void   dw_variant(DBuf *w, const char *sig);     /* writes the signature; then the value */

/* Builds a complete message (header + body). Returns 0 ok; out in malloc. */
typedef struct {
    uint8_t type, flags;
    uint32_t serial, reply_serial;
    const char *path, *iface, *member, *error, *dest, *sig;
    const char *sender;                  /* only set by the bus (and by tests acting as the bus) */
} DHdr;
int dbus_build(const DHdr *h, const DBuf *body, DBuf *out);

/* ---- connection ---- */
typedef struct {
    int fd;
    uint32_t serial;
    char unique[256];
    uint8_t *in; size_t in_n, in_cap;
    uint8_t *out; size_t out_n, out_off, out_cap;
    int dead;
} DConn;

/* Connects (without authenticating) to the session bus: DBUS_SESSION_BUS_ADDRESS (unix:path= or
 * unix:abstract=) or $XDG_RUNTIME_DIR/bus. For axia-server, outside the sandbox. fd or -1. */
int  dbus_connect_session(void);
/* System bus: DBUS_SYSTEM_BUS_ADDRESS (unix:path=) or /run/dbus/system_bus_socket. fd or -1. */
int  dbus_connect_system(void);
/* SASL EXTERNAL + Hello over a connected socket (waits up to timeout_ms). Leaves the fd
 * non-blocking. 0 ok. */
int  dbus_open(DConn *c, int fd, int timeout_ms);
void dbus_close(DConn *c);                        /* closes the fd and wipes the buffers */
/* Queues a message (h->serial is filled in if 0) and tries to send it. Returns the serial or 0. */
uint32_t dbus_send(DConn *c, DHdr *h, const DBuf *body);
int  dbus_flush(DConn *c);                        /* sends what it can; -1 if disconnected */
int  dbus_pending_out(const DConn *c);
/* Reads what is available (non-blocking) and delivers the next complete message in m: 1 message,
 * 0 none (for now), -1 disconnected or something invalid arrived. */
int  dbus_next(DConn *c, DMsg *m);
/* Blocks (up to timeout_ms) waiting for the reply to serial; other messages are discarded.
 * Startup only. 1 reply (in m), 0 D-Bus error (in m), -1 disconnect or timeout. */
int  dbus_wait_reply(DConn *c, uint32_t serial, DMsg *m, int timeout_ms);

/* Convenience replies and signals */
uint32_t dbus_reply(DConn *c, const DMsg *call, const char *sig, const DBuf *body);
uint32_t dbus_error(DConn *c, const DMsg *call, const char *name, const char *text);
uint32_t dbus_signal(DConn *c, const char *path, const char *iface, const char *member, const char *sig, const DBuf *body);

#endif
