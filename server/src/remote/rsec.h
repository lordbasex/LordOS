/* rsec.h — strict parsers of everything a browser sends to the remote desktop (docs/REMOTE.md,
 * "Security analysis (0.8.3)"): the HTTP request head, the login form, the cookie, WebSocket
 * frames and the JSON input messages. Plain libc, no I/O and no OpenSSL, so they are unit-tested
 * and fuzzed with ASan+UBSan (build/remote-sec-test, build/remote-fuzz).
 *
 * Every function rejects instead of guessing: a request that is not exactly what our own pages
 * send gets an error status and the connection is closed. */
#ifndef LORD_REMOTE_RSEC_H
#define LORD_REMOTE_RSEC_H

#include <stddef.h>
#include <stdint.h>

/* ---- HTTP/1.1 request head ---- */

#define RS_MAX_HEAD 8192        /* request line + headers + blank line */
#define RS_MAX_HEADERS 40
#define RS_MAX_BODY 4096        /* only POST /login has a body */

typedef struct {
    char method[8];
    char path[128];
    char query[256];
    char host[256], origin[256], cookie[1024], upgrade[32], connection[128];
    char wskey[32], wsversion[8], ctype[96], fetch_site[24];
    long content_length;        /* -1: no Content-Length */
    size_t head_len;            /* bytes of the head, CRLFCRLF included */
} RsReq;

/* Results < 0 are HTTP statuses to answer with (negated). */
enum { RS_NEED_MORE = 0 };
/* Parses the head at the start of buf[0..len). Returns the head length (> 0), RS_NEED_MORE (0) if
 * it isn't complete yet, or -status: -400 malformed, -405 unknown method, -414 target too long,
 * -431 head or a header too large, -413 body too large, -501 Transfer-Encoding. */
int  rs_parse_head(const char *buf, size_t len, RsReq *r);

int  rs_valid_username(const char *u);          /* ^[a-z_][a-z0-9_-]{0,31}$ */
int  rs_valid_host(const char *h);              /* host[:port]: name, IPv4 or [IPv6] */
/* Origin must be exactly "https://" + Host (the host part case-insensitive). */
int  rs_same_origin(const char *origin, const char *host);
/* comma-separated list contains this token (case-insensitive) */
int  rs_has_token(const char *list, const char *token);
int  rs_valid_wskey(const char *k);             /* base64 of 16 bytes */
/* Content-Type of the login form: application/x-www-form-urlencoded, optionally ;charset=UTF-8 */
int  rs_form_type(const char *ctype);

/* The session cookie: exactly one `name` pair with 64 lowercase hex digits.
 * 0 found, -1 absent, -2 malformed or present twice. */
int  rs_cookie_token(const char *cookie, const char *name, char out[65]);

/* ---- login form (application/x-www-form-urlencoded) ---- */

#define RS_PASS_MAX 1024
enum { RS_FORM_OK = 0, RS_FORM_BAD = -1, RS_FORM_USER = -2, RS_FORM_PASS = -3 };
/* Exactly the fields "user" and "pass", once each, nothing else; strict %XX decoding; the user a
 * valid login name; the password 1..1024 bytes without NUL. *ulen_raw gets the decoded length of
 * the user field even if invalid (for logging a sanitized copy into rawuser). */
int  rs_parse_login(const char *body, size_t len, char user[33], char pass[RS_PASS_MAX + 1], size_t *passlen,
                    char rawuser[33]);

/* Printable ASCII copy for the journal (anything else becomes '?'), at most n-1 bytes. */
void rs_log_clean(const char *in, size_t inlen, char *out, size_t n);

/* ---- WebSocket (RFC 6455, server side: client frames must be masked) ---- */

#define RS_WS_MAX_MSG (2u << 20)        /* largest message from the browser (a clipboard) */

int  rs_utf8_valid(const unsigned char *p, size_t n);

typedef struct {
    int frag_op;                /* opcode of a fragmented message in progress, 0 if none */
    unsigned char *msg;
    size_t msg_len, msg_cap, max;
} RsWs;

/* op: 1 text (valid UTF-8), 2 binary, 8 close, 9 ping, 10 pong. data is writable and NUL-terminated. */
typedef void (*RsWsOn)(void *ud, int op, unsigned char *data, size_t len);

void rs_ws_init(RsWs *w, size_t max);
void rs_ws_free(RsWs *w);
/* Consumes complete frames from buf[0..n) (unmasking in place) and calls `on` for every complete
 * message. *used = bytes consumed. Returns 0, or a close code: 1002 protocol error, 1007 invalid
 * UTF-8, 1009 too big, 1011 out of memory. */
int  rs_ws_consume(RsWs *w, unsigned char *buf, size_t n, size_t *used, RsWsOn on, void *ud);

/* ---- flat JSON objects from the browser: {"t":"m","x":1,"y":2} ---- */

#define RS_JMAX 8
typedef struct {
    int n;
    char key[RS_JMAX][12];
    int is_str[RS_JMAX];
    long long num[RS_JMAX];
    const char *str[RS_JMAX];   /* decoded, NUL-terminated, inside the caller's buffer */
    size_t slen[RS_JMAX];
} RsJson;

/* Strict: one object, keys [a-z]{1,11} without duplicates, values integers (|v| < 2^53) or strings
 * (escapes decoded, surrogate pairs joined, no lone surrogates, no NUL). strbuf must hold n+1 bytes. */
int  rs_json_parse(const char *j, size_t n, RsJson *o, char *strbuf, size_t strcap);
const char *rs_json_str(const RsJson *o, const char *key, size_t *len);
int  rs_json_num(const RsJson *o, const char *key, long long *v);

/* A browser message, validated and clamped against a W×H screen. */
enum { RS_IN_NONE = 0, RS_IN_PTR, RS_IN_BTN, RS_IN_WHEEL, RS_IN_SCROLL, RS_IN_PINCH, RS_IN_KEY, RS_IN_RESIZE,
       RS_IN_PING, RS_IN_CLIP, RS_IN_RTC, RS_IN_ICE, RS_IN_VMODE };
#define RS_SDP_MAX 32768            /* {"t":"rtc","sdp":…} (rtcwire.h RW_SDP_MAX) */
#define RS_CLIP_MAX (1u << 20)
typedef struct {
    int type;
    int a, b;                   /* PTR x,y · BTN button,down · WHEEL steps · KEY code,down · RESIZE w,h · SCROLL/PINCH phase · VMODE on
                                 * · RTC/ICE the peer number "p" (0 if absent) */
    int32_t v1, v2;             /* SCROLL dx,dy (1/256 px) · PINCH scale ×65536, rotation ×256 · RESIZE v1: hz (0 unknown) */
    long long ts;               /* PING */
    const char *text;           /* CLIP, RTC (the SDP offer), ICE (a candidate; "" ends) (inside the RsJson buffer) */
    size_t tlen;
} RsInput;
int  rs_browser_input(const RsJson *j, int W, int H, RsInput *in);

#endif
