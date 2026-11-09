/* mysqlproto.h — the MySQL / MariaDB client protocol for lord-sql (docs/DATABASES.md): packet
 * framing, the v10 handshake, HandshakeResponse41 and SSLRequest, the auth plugins
 * mysql_native_password and caching_sha2_password (fast path; full auth sends the password over
 * TLS, or encrypts it with the server's RSA key without TLS), the auth switch, COM_QUERY text
 * result sets with several statements, OK / ERR / EOF (also with CLIENT_DEPRECATE_EOF), COM_INIT_DB,
 * COM_PING and COM_QUIT.
 *
 * Everything here reads bytes from a server we don't trust: every length is checked against what
 * is left in the packet, packets are reassembled up to a fixed maximum, sequence numbers must
 * follow, column counts are capped and every row must have exactly that many values. The code
 * does no I/O of its own: MyConn's callbacks read and write (lord-sql: the socket or TLS; the
 * tests: memory). */
#ifndef LORD_SQL_MYSQLPROTO_H
#define LORD_SQL_MYSQLPROTO_H

#include <stddef.h>
#include <stdint.h>


#define MY_MAX_COLUMNS 4096                 /* MySQL's and MariaDB's own limit per table / result */
#define MY_PACKET_MAX  ((size_t)64 << 20)   /* one reassembled packet; past it the packet is skipped */

/* capability flags */
enum {
    MY_CLIENT_LONG_PASSWORD = 0x1, MY_CLIENT_LONG_FLAG = 0x4, MY_CLIENT_CONNECT_WITH_DB = 0x8,
    MY_CLIENT_LOCAL_FILES = 0x80, MY_CLIENT_PROTOCOL_41 = 0x200, MY_CLIENT_SSL = 0x800,
    MY_CLIENT_TRANSACTIONS = 0x2000, MY_CLIENT_SECURE_CONNECTION = 0x8000,
    MY_CLIENT_MULTI_STATEMENTS = 0x10000, MY_CLIENT_MULTI_RESULTS = 0x20000,
    MY_CLIENT_PS_MULTI_RESULTS = 0x40000, MY_CLIENT_PLUGIN_AUTH = 0x80000,
    MY_CLIENT_PLUGIN_AUTH_LENENC = 0x200000, MY_CLIENT_SESSION_TRACK = 0x800000,
    MY_CLIENT_DEPRECATE_EOF = 0x1000000,
};
enum { MY_SERVER_MORE_RESULTS = 0x0008 };
enum { MY_COM_QUIT = 0x01, MY_COM_INIT_DB = 0x02, MY_COM_QUERY = 0x03, MY_COM_PING = 0x0e };

/* column flags */
enum {
    MY_NOT_NULL = 1, MY_PRI_KEY = 2, MY_UNIQUE_KEY = 4, MY_MULTIPLE_KEY = 8, MY_BLOB = 16,
    MY_UNSIGNED = 32, MY_ZEROFILL = 64, MY_BINARY = 128, MY_ENUM = 256, MY_AUTO_INCREMENT = 512,
    MY_SET = 2048,
};

/* errors (MyConn.err, and the return value of the calls below) */
enum {
    MY_OK = 0, MY_E_IO = -1, MY_E_PROTO = -2, MY_E_BIG = -3, MY_E_SEQ = -4, MY_E_SQL = -5,
    MY_E_AUTH = -6, MY_E_UNSUPPORTED = -7, MY_E_TLS = -8, MY_E_CERT = -9, MY_E_OOM = -10,
};

typedef struct { int code; char state[6]; char msg[512]; } MyErr;

typedef struct MyConn {
    int (*rd)(void *io, void *buf, size_t n);   /* exactly n bytes: 0, or -1 */
    int (*wr)(void *io, const void *buf, size_t n);
    int (*tls)(void *io);                       /* starts TLS: 0, MY_E_TLS or MY_E_CERT */
    void *io;
    uint8_t seq;
    uint8_t *pkt;                               /* the last packet (reassembled), NUL after it */
    size_t len, cap;
    size_t max;                                 /* MY_PACKET_MAX unless a test sets less */
    uint32_t caps;                              /* negotiated */
    int is_tls;
    MyErr err;                                  /* the server's last ERR */
    char why[96];                               /* what went wrong, for the logs (never data) */
} MyConn;

typedef struct {
    char version[64];
    uint32_t thread_id;
    uint8_t scramble[20];
    uint32_t caps;
    uint8_t charset;
    uint16_t status;
    char plugin[64];
    int mariadb;
} MyHandshake;

typedef struct { uint64_t affected, insert_id; uint16_t status, warnings; const uint8_t *info; size_t info_len; } MyOk;

typedef struct { const uint8_t *p; size_t n; } MySlice;
typedef struct {
    MySlice catalog, db, table, org_table, name, org_name;
    uint16_t charset;
    uint32_t length;
    uint8_t type, decimals;
    uint16_t flags;
} MyColumn;

typedef struct {
    const char *user, *password, *db;
    int tls;                                    /* 0 off, 1 preferred, 2 required (also verify) */
} MyLogin;

/* JSON text builder with a size limit (past it: oom set, nothing more appended) */
typedef struct { char *p; size_t n, cap, limit; int oom; } MyBuf;
void mb_init(MyBuf *b, size_t limit);
void mb_free(MyBuf *b);
void mb_raw(MyBuf *b, const char *s);
void mb_rawn(MyBuf *b, const char *s, size_t n);
void mb_printf(MyBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void mb_strn(MyBuf *b, const char *s, size_t n);    /* "text": escapes; invalid UTF-8 and NUL → U+FFFD */

/* ---- bounded reading ---- */
typedef struct { const uint8_t *p; size_t n, pos; } MyRd;
int my_rd_u8(MyRd *r, uint8_t *v);
int my_rd_u16(MyRd *r, uint16_t *v);
int my_rd_u24(MyRd *r, uint32_t *v);
int my_rd_u32(MyRd *r, uint32_t *v);
/* length-encoded integer; *null set (if given) for 0xfb, which only rows may use. 0 ok, -1 bad. */
int my_rd_lenenc(MyRd *r, uint64_t *v, int *null);
int my_rd_lenenc_str(MyRd *r, MySlice *s, int *null);
int my_rd_nulstr(MyRd *r, MySlice *s);          /* up to a NUL that must be in the packet */
int my_rd_fixed(MyRd *r, size_t n, MySlice *s);

/* ---- packets ---- */
void my_init(MyConn *c);
void my_free(MyConn *c);                        /* zeroes and frees the packet buffer */
/* Reads one packet into c->pkt / c->len. MY_OK, MY_E_IO, MY_E_SEQ, or MY_E_BIG (the packet was
 * larger than c->max: it was read to its end and dropped, the connection is still in step). */
int my_read_packet(MyConn *c);
int my_write_packet(MyConn *c, const void *p, size_t n);

int my_parse_handshake(const uint8_t *p, size_t n, MyHandshake *h);
int my_parse_err(const uint8_t *p, size_t n, MyErr *e);
int my_parse_ok(const uint8_t *p, size_t n, MyOk *o);
int my_parse_eof(const uint8_t *p, size_t n, uint16_t *warnings, uint16_t *status);
int my_parse_column(const uint8_t *p, size_t n, MyColumn *col);
/* Is this packet the end of the rows (EOF, or OK with 0xfe under CLIENT_DEPRECATE_EOF)? */
int my_is_row_end(const MyConn *c);

const char *my_type_name(uint8_t type, uint16_t flags, uint16_t charset);
/* Values sent as "\u0000hex:…": binary strings (charset 63 on a string, blob, bit or geometry). */
int my_is_binary(uint8_t type, uint16_t charset);

/* ---- auth ---- */
void my_scramble_native(const uint8_t *scramble, size_t sl, const char *pw, uint8_t out[20]);
void my_scramble_sha2(const uint8_t *scramble, size_t sl, const char *pw, uint8_t out[32]);
/* (password + NUL) XOR scramble, RSA-OAEP with the server's PEM key. 0 ok; out holds up to 1024. */
int  my_rsa_password(const uint8_t *pem, size_t pemlen, const char *pw, const uint8_t *scramble, size_t sl,
                     uint8_t *out, size_t *outlen);

/* Handshake, TLS (c->tls) and authentication. MY_OK, or MY_E_* (MY_E_SQL / MY_E_AUTH with c->err). */
int my_connect(MyConn *c, const MyLogin *login, MyHandshake *hs);

/* COM_QUERY: appends a JSON array of Result objects to out. Rows past max_rows in a result, or past
 * max_bytes of out, are read and dropped and that result says truncated. MY_OK, MY_E_SQL (a
 * statement failed: out still holds the results before it, as a complete array), or a connection
 * error. */
int my_query(MyConn *c, const char *sql, size_t len, long max_rows, size_t max_bytes, MyBuf *out);
/* COM_INIT_DB / COM_PING: MY_OK on the server's OK, MY_E_SQL on its ERR. */
int my_command(MyConn *c, uint8_t cmd, const char *arg, size_t len);
void my_quit(MyConn *c);

#endif
