/* dbusmin.h — the few D-Bus calls lordos-guest's performance profile makes (docs/VIRTUALIZATION.md 7.1.4), spoken by
 * the helper itself: no libdbus, no sd-bus, no program started. A connection is a Unix socket path only (the system
 * bus's fixed socket, the session's $XDG_RUNTIME_DIR/bus), authenticated with EXTERNAL (the socket's credentials). It
 * sends method calls built from the caller's constant strings and reads only their replies, each checked (lengths,
 * padding, header fields, signatures) before a value is taken; signals and anything else are skipped. Pure functions
 * (building, parsing, dconf's change set) are tested in tests/guest_dbus_test.c. */
#ifndef LORDOS_GUEST_DBUSMIN_H
#define LORDOS_GUEST_DBUSMIN_H

#include <stddef.h>
#include <stdint.h>

#define DB_MSG_MAX 65536                /* a reply bigger than this is refused */
enum { DB_CALL = 1, DB_RETURN = 2, DB_ERROR = 3, DB_SIGNAL = 4 };

/* a message read: what this side needs of its header, and its body (pointing into the buffer it was read from) */
typedef struct {
    int type, big;                      /* DB_*; big-endian */
    uint32_t serial, reply_serial;      /* reply_serial 0: none */
    char sig[64];                       /* the body's signature ("" none) */
    char err[128];                      /* an error's name */
    char member[64], iface[96];         /* a call's or a signal's member and interface ("" none) */
    const uint8_t *body;
    uint32_t body_n;
} DbMsg;

/* a method call (little-endian): its size, 0 when it doesn't fit or a string isn't one D-Bus takes. dest, iface and sig
 * may be NULL; body is already marshalled for sig */
size_t db_call_build(uint8_t *out, size_t cap, uint32_t serial, const char *dest, const char *path, const char *iface,
                     const char *member, const char *sig, const uint8_t *body, size_t body_n);
/* one message at p: the bytes it takes (> 0), 0 when more are needed, -1 when it isn't a well formed message */
int db_msg_parse(const uint8_t *p, size_t n, DbMsg *m);

/* marshalling a body (alignment from the body's start, which is 8-aligned in a message) */
typedef struct { uint8_t *p; size_t n, cap; int bad; } DbW;
void dbw_init(DbW *w, uint8_t *p, size_t cap);
void dbw_str(DbW *w, const char *s);        /* s or o */
void dbw_sig(DbW *w, const char *s);        /* g */
void dbw_bytes(DbW *w, const uint8_t *p, size_t n);   /* ay */
void dbw_variant_str(DbW *w, const char *s);          /* v holding an s */
void dbw_u32(DbW *w, uint32_t v);                     /* u */
void dbw_strv(DbW *w, const char *const *v, int n);   /* as */

/* the whole size of the message whose header is at p (16 bytes are enough): > 0, 0 more bytes needed, -1 not a message.
 * Unlike db_msg_parse it takes messages up to DB_SKIP_MAX, so a reader can skip one too big to keep */
#define DB_SKIP_MAX (128u << 20)
long db_msg_total(const uint8_t *p, size_t n);
/* a monitored org.freedesktop.Notifications.Notify call (body "susssasa{sv}i"): its app name, summary and body, each cut to
 * its cap - 1 bytes (on a character boundary). 0, -1 not one */
int db_notify_body(const DbMsg *m, char *app, size_t an, char *summary, size_t sn, char *body, size_t bn);
/* a notification's text as plain text for the host (lordos-guest 9, docs/VIRTUALIZATION.md 6.12): markup tags dropped,
 * &amp; &lt; &gt; &quot; &apos; decoded, control and bidi characters as spaces, runs of spaces as one, trimmed, at most
 * max bytes cut on a character boundary; what isn't UTF-8 becomes '?'. Its length (out holds max + 1) */
size_t db_note_plain(const char *in, char *out, size_t max);

/* a reply's value: its body is one variant (nested variants unwrapped) holding an s, or a b. 0, -1 otherwise */
int db_reply_str(const DbMsg *m, char *out, size_t cap);
int db_reply_bool(const DbMsg *m, int *b);

/* dconf's change set (GVariant a{smv}, little-endian) setting one key to a boolean: its size, 0 when it doesn't fit or
 * the key isn't a dconf key (starts with '/', doesn't end with one, no "//") */
size_t db_dconf_bool(uint8_t *out, size_t cap, const char *key, int value);

/* the connection: what was read and not yet used stays in `in` (a signal can come in the same read as a reply) */
typedef struct { int fd; uint32_t serial; size_t in_n, used; uint8_t in[DB_MSG_MAX]; } Db;
/* connects to the Unix socket at path (never a TCP or abstract address), authenticates (EXTERNAL, this uid) and says
 * Hello. 0, -1 (closed again) */
int db_open(Db *b, const char *path, int timeout_ms);
/* calls a method and waits for its answer: 0 a return, 1 an error (reply->err), -1 the connection failed or timed out.
 * The reply's body stays valid until the next call */
int db_call(Db *b, const char *dest, const char *path, const char *iface, const char *member, const char *sig,
            const uint8_t *body, size_t body_n, DbMsg *reply, int timeout_ms);
void db_close(Db *b);

#endif
