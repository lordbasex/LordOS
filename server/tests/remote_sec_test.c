/* remote_sec_test.c — the remote desktop's strict parsers and rules (src/remote/rsec.c, ipnet.c,
 * lockout.c; docs/REMOTE.md, "Security analysis (0.8.3)"). Every case is a request, a form, a frame
 * or an address our own pages would (or would never) send. */
#include "remote/rsec.h"
#include "remote/ipnet.h"
#include "remote/lockout.h"
#include "remote/rtcwire.h"
#include "rtc_offer_chrome.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int head(const char *s, RsReq *r) { return rs_parse_head(s, strlen(s), r); }

static void test_head(void) {
    RsReq r;
    CHECK(head("GET / HTTP/1.1\r\nHost: 10.0.0.5:5673\r\n\r\n", &r) > 0);
    CHECK(!strcmp(r.method, "GET") && !strcmp(r.path, "/") && !strcmp(r.host, "10.0.0.5:5673") && r.content_length == -1);
    CHECK(head("GET /?e=1 HTTP/1.1\r\nHost: a\r\n\r\n", &r) > 0 && !strcmp(r.query, "e=1"));
    CHECK(head("POST /login HTTP/1.1\r\nHost: a\r\nContent-Length: 20\r\nContent-Type: application/x-www-form-urlencoded\r\n"
               "Origin: https://a\r\nCookie: x=1\r\n\r\n", &r) > 0 && r.content_length == 20 && !strcmp(r.origin, "https://a"));
    CHECK(head("GET / HTTP/1.1\r\nHost: a\r\n", &r) == RS_NEED_MORE);              /* not complete */
    CHECK(head("GET / HTTP/1.1\r\n", &r) == RS_NEED_MORE);
    CHECK(head("GET / HTTP/1.1\r\n\r\n", &r) == -400);                            /* 1.1 without Host */
    CHECK(head("GET / HTTP/1.0\r\n\r\n", &r) > 0);
    CHECK(head("GET / HTTP/2.0\r\nHost: a\r\n\r\n", &r) == -400);
    CHECK(head("GET  / HTTP/1.1\r\nHost: a\r\n\r\n", &r) == -400);                /* double space */
    CHECK(head("get / HTTP/1.1\r\nHost: a\r\n\r\n", &r) == -400);
    CHECK(head("GET http://x/ HTTP/1.1\r\nHost: a\r\n\r\n", &r) == -400);         /* absolute form */
    CHECK(head("GET /#x HTTP/1.1\r\nHost: a\r\n\r\n", &r) == -400);
    CHECK(head("GET / HTTP/1.1\nHost: a\r\n\r\n", &r) == -400);                   /* bare LF */
    CHECK(head("GET / HTTP/1.1\r\nHost: a\rb\r\n\r\n", &r) == -400);              /* bare CR */
    CHECK(head("GET / HTTP/1.1\r\nHost: a\r\nX: \x01\r\n\r\n", &r) == -400);      /* control byte */
    CHECK(head("GET / HTTP/1.1\r\nHost: \xc3\xa1\r\n\r\n", &r) == -400);          /* non-ASCII */
    CHECK(head("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", &r) == -400);      /* duplicate Host */
    CHECK(head("GET / HTTP/1.1\r\nHost: a\r\nCookie: a=1\r\nCookie: b=2\r\n\r\n", &r) == -400);
    CHECK(head("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n", &r) == -400);
    CHECK(head("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", &r) == -501);   /* smuggling */
    CHECK(head("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n", &r) == -400);
    CHECK(head("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1e3\r\n\r\n", &r) == -400);
    CHECK(head("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 4097\r\n\r\n", &r) == -413);
    CHECK(head("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999\r\n\r\n", &r) == -413);
    CHECK(head("GET / HTTP/1.1\r\nHost : a\r\n\r\n", &r) == -400);                /* space before the colon */
    CHECK(head("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n", &r) == -400);      /* obs-fold */
    CHECK(head("GET / HTTP/1.1\r\nHost: a\r\nNoColon\r\n\r\n", &r) == -400);
    CHECK(head("\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n", &r) == -400);
    {   /* too many headers, a too-long field, a too-long head, a too-long path */
        char b[9000];
        int o = sprintf(b, "GET / HTTP/1.1\r\nHost: a\r\n");
        for (int i = 0; i < 41; i++) o += sprintf(b + o, "X-%d: 1\r\n", i);
        strcpy(b + o, "\r\n");
        CHECK(head(b, &r) == -431);
        o = sprintf(b, "GET / HTTP/1.1\r\nHost: a\r\nCookie: ");
        memset(b + o, 'a', 1100);
        strcpy(b + o + 1100, "\r\n\r\n");
        CHECK(head(b, &r) == -431);
        memset(b, 'a', sizeof b);
        memcpy(b, "GET / HTTP/1.1\r\nX: ", 19);
        CHECK(rs_parse_head(b, sizeof b, &r) == -431);
        o = sprintf(b, "GET /");
        memset(b + o, 'p', 200);
        strcpy(b + o + 200, " HTTP/1.1\r\nHost: a\r\n\r\n");
        CHECK(head(b, &r) == -414);
    }
    CHECK(head("GET / HTTP/1.1\r\nhOsT: x\r\nsec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", &r) > 0 &&
          !strcmp(r.host, "x") && !strcmp(r.wskey, "dGhlIHNhbXBsZSBub25jZQ==") && !strcmp(r.wsversion, "13"));
}

static void test_small(void) {
    CHECK(rs_valid_username("lord") && rs_valid_username("_x") && rs_valid_username("a-b_9"));
    CHECK(!rs_valid_username("") && !rs_valid_username("Lord") && !rs_valid_username("9a") && !rs_valid_username("-a"));
    CHECK(!rs_valid_username("a b") && !rs_valid_username("a$") && !rs_valid_username("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    CHECK(rs_valid_username("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));      /* 32 */
    CHECK(rs_valid_host("192.168.1.5:5673") && rs_valid_host("lordos.local") && rs_valid_host("[fe80::1]:5673") && rs_valid_host("[::1]"));
    CHECK(!rs_valid_host("") && !rs_valid_host("a b") && !rs_valid_host("a:0") && !rs_valid_host("a:65536") && !rs_valid_host("[zz]"));
    CHECK(!rs_valid_host("a..b") && !rs_valid_host("-a") && !rs_valid_host("a:5673x") && !rs_valid_host("a/b"));
    CHECK(rs_same_origin("https://10.0.0.5:5673", "10.0.0.5:5673"));
    CHECK(rs_same_origin("https://LordOS.local:5673", "lordos.local:5673"));
    CHECK(!rs_same_origin("http://10.0.0.5:5673", "10.0.0.5:5673"));
    CHECK(!rs_same_origin("https://evil.example", "10.0.0.5:5673"));
    CHECK(!rs_same_origin("https://10.0.0.5:5673.evil", "10.0.0.5:5673"));
    CHECK(!rs_same_origin("", "10.0.0.5:5673") && !rs_same_origin("null", "10.0.0.5:5673"));
    CHECK(rs_has_token("keep-alive, Upgrade", "upgrade") && rs_has_token("Upgrade", "upgrade"));
    CHECK(!rs_has_token("keep-alive", "upgrade") && !rs_has_token("upgraded", "upgrade"));
    CHECK(rs_valid_wskey("dGhlIHNhbXBsZSBub25jZQ==") && !rs_valid_wskey("dGhlIHNhbXBsZSBub25jZR==") && !rs_valid_wskey("abc"));
    CHECK(rs_form_type("application/x-www-form-urlencoded") && rs_form_type("application/x-www-form-urlencoded;charset=UTF-8"));
    CHECK(rs_form_type("application/x-www-form-urlencoded; charset=\"utf-8\""));
    CHECK(!rs_form_type("multipart/form-data") && !rs_form_type("application/x-www-form-urlencodedX") && !rs_form_type("text/plain"));
    CHECK(!rs_form_type("application/x-www-form-urlencoded; charset=latin1"));
    char tok[65], hex[65];
    memset(hex, 'a', 64);
    hex[64] = 0;
    char c[300];
    snprintf(c, sizeof c, "theme=dark; __Host-lordos_remote=%s; x=y", hex);
    CHECK(rs_cookie_token(c, "__Host-lordos_remote", tok) == 0 && !strcmp(tok, hex));
    snprintf(c, sizeof c, "x__Host-lordos_remote=%s", hex);
    CHECK(rs_cookie_token(c, "__Host-lordos_remote", tok) == -1);                  /* suffix match is not a match */
    snprintf(c, sizeof c, "__Host-lordos_remote=%s; __Host-lordos_remote=%s", hex, hex);
    CHECK(rs_cookie_token(c, "__Host-lordos_remote", tok) == -2);                  /* cookie tossing */
    CHECK(rs_cookie_token("__Host-lordos_remote=ABC", "__Host-lordos_remote", tok) == -2);
    hex[10] = 'G';
    snprintf(c, sizeof c, "__Host-lordos_remote=%s", hex);
    CHECK(rs_cookie_token(c, "__Host-lordos_remote", tok) == -2);
    CHECK(rs_cookie_token("", "__Host-lordos_remote", tok) == -1);
    char clean[33];
    rs_log_clean("a\nb\"c\\d\x1b", 8, clean, sizeof clean);
    CHECK(!strcmp(clean, "a?b?c?d?"));
}

static int login(const char *body, char *user, char *pass, size_t *pl) {
    char raw[33];
    return rs_parse_login(body, strlen(body), user, pass, pl, raw);
}

static void test_login(void) {
    char u[33], p[RS_PASS_MAX + 1], raw[33];
    size_t pl;
    CHECK(login("user=lord&pass=s3cret", u, p, &pl) == RS_FORM_OK && !strcmp(u, "lord") && !strcmp(p, "s3cret") && pl == 6);
    CHECK(login("pass=a+b%26c%3D&user=bob", u, p, &pl) == RS_FORM_OK && !strcmp(u, "bob") && !strcmp(p, "a b&c="));
    CHECK(login("user=lord&pass=%C3%B1", u, p, &pl) == RS_FORM_OK && pl == 2);
    CHECK(login("user=lord", u, p, &pl) == RS_FORM_BAD);
    CHECK(login("pass=x", u, p, &pl) == RS_FORM_BAD);
    CHECK(login("user=lord&pass=x&x=1", u, p, &pl) == RS_FORM_BAD);                 /* unknown field */
    CHECK(login("user=lord&user=root&pass=x", u, p, &pl) == RS_FORM_BAD);           /* duplicate */
    CHECK(login("user=lord&pass=x&pass=y", u, p, &pl) == RS_FORM_BAD);
    CHECK(login("user=lord&pass=x&", u, p, &pl) == RS_FORM_BAD);
    CHECK(login("&user=lord&pass=x", u, p, &pl) == RS_FORM_BAD);
    CHECK(login("user=lord&pass=%4", u, p, &pl) == RS_FORM_BAD);                    /* truncated escape */
    CHECK(login("user=lord&pass=%zz", u, p, &pl) == RS_FORM_BAD);
    CHECK(login("user=lord&pass=a b", u, p, &pl) == RS_FORM_BAD);                   /* raw space */
    CHECK(login("user=lord&pass=a\"", u, p, &pl) == RS_FORM_BAD);
    CHECK(login("user=lord&pass", u, p, &pl) == RS_FORM_BAD);
    CHECK(login("user=Lord&pass=x", u, p, &pl) == RS_FORM_USER);
    CHECK(login("user=lord%00x&pass=x", u, p, &pl) == RS_FORM_USER);                /* NUL in the name */
    CHECK(login("user=&pass=x", u, p, &pl) == RS_FORM_USER);
    CHECK(login("user=lo%20rd&pass=x", u, p, &pl) == RS_FORM_USER);
    CHECK(login("user=lord&pass=", u, p, &pl) == RS_FORM_PASS);
    CHECK(login("user=lord&pass=ab%00cd", u, p, &pl) == RS_FORM_PASS);              /* NUL in the password */
    char big[4200];
    int o = sprintf(big, "user=lord&pass=");
    memset(big + o, 'x', 1024);
    big[o + 1024] = 0;
    CHECK(login(big, u, p, &pl) == RS_FORM_OK && pl == 1024);
    big[o + 1024] = 'x';
    big[o + 1025] = 0;
    CHECK(login(big, u, p, &pl) == RS_FORM_PASS);                                   /* 1025 bytes */
    o = sprintf(big, "user=");
    memset(big + o, 'a', 40);
    strcpy(big + o + 40, "&pass=x");
    CHECK(login(big, u, p, &pl) == RS_FORM_USER);
    CHECK(rs_parse_login("user=root\n&pass=x", 17, u, p, &pl, raw) == RS_FORM_BAD);
    CHECK(login("user=x%0Ay&pass=x", u, p, &pl) == RS_FORM_USER);
    rs_parse_login("user=x%0Ay&pass=x", 17, u, p, &pl, raw);
    CHECK(!strcmp(raw, "x?y"));                                                     /* sanitized for the journal */
}

typedef struct { int n, op[8]; char data[8][256]; size_t len[8]; } Got;

static void on(void *ud, int op, unsigned char *d, size_t l) {
    Got *g = ud;
    if (g->n >= 8) return;
    g->op[g->n] = op;
    memcpy(g->data[g->n], d, l < 255 ? l : 255);
    g->data[g->n][l < 255 ? l : 255] = 0;
    g->len[g->n++] = l;
}

/* a masked client frame */
static size_t frame(unsigned char *out, int fin, int op, const char *p, size_t n) {
    static const unsigned char m[4] = { 0x12, 0x34, 0x56, 0x78 };
    size_t o = 0;
    out[o++] = (unsigned char)((fin ? 0x80 : 0) | op);
    if (n < 126) out[o++] = (unsigned char)(0x80 | n);
    else if (n < 65536) { out[o++] = 0x80 | 126; out[o++] = (unsigned char)(n >> 8); out[o++] = (unsigned char)n; }
    else { out[o++] = 0x80 | 127; for (int i = 0; i < 8; i++) out[o++] = (unsigned char)((uint64_t)n >> (56 - 8 * i)); }
    memcpy(out + o, m, 4);
    o += 4;
    for (size_t i = 0; i < n; i++) out[o + i] = (unsigned char)p[i] ^ m[i & 3];
    return o + n;
}

static int ws(unsigned char *b, size_t n, Got *g, size_t *used) {
    RsWs w;
    rs_ws_init(&w, 70000);
    memset(g, 0, sizeof *g);
    int rc = rs_ws_consume(&w, b, n, used, on, g);
    rs_ws_free(&w);
    return rc;
}

static void test_ws(void) {
    unsigned char b[200000];
    Got g;
    size_t used, n;
    n = frame(b, 1, 1, "{\"t\":\"ack\"}", 11);
    CHECK(ws(b, n, &g, &used) == 0 && g.n == 1 && g.op[0] == 1 && !strcmp(g.data[0], "{\"t\":\"ack\"}") && used == n);
    CHECK(ws(b, n - 1, &g, &used) == 0 && g.n == 0 && used == 0);                   /* incomplete: waits */
    n = frame(b, 0, 1, "ab", 2);
    n += frame(b + n, 1, 9, "p", 1);                                                /* ping between fragments */
    n += frame(b + n, 1, 0, "cd", 2);
    CHECK(ws(b, n, &g, &used) == 0 && g.n == 2 && g.op[0] == 9 && g.op[1] == 1 && !strcmp(g.data[1], "abcd"));
    n = frame(b, 1, 0, "x", 1);
    CHECK(ws(b, n, &g, &used) == 1002);                                             /* continuation of nothing */
    n = frame(b, 0, 1, "a", 1);
    n += frame(b + n, 1, 1, "b", 1);
    CHECK(ws(b, n, &g, &used) == 1002);                                             /* new message inside one */
    n = frame(b, 1, 1, "x", 1);
    b[1] &= 0x7f;
    CHECK(ws(b, n, &g, &used) == 1002);                                             /* unmasked */
    n = frame(b, 1, 1, "x", 1);
    b[0] |= 0x40;
    CHECK(ws(b, n, &g, &used) == 1002);                                             /* RSV bit */
    n = frame(b, 1, 3, "x", 1);
    CHECK(ws(b, n, &g, &used) == 1002);                                             /* reserved opcode */
    n = frame(b, 0, 9, "x", 1);
    CHECK(ws(b, n, &g, &used) == 1002);                                             /* fragmented control */
    char big[70001];
    memset(big, 'a', sizeof big);
    n = frame(b, 1, 9, big, 126);
    CHECK(ws(b, n, &g, &used) == 1002);                                             /* control > 125 */
    n = frame(b, 1, 1, big, 70001);
    CHECK(ws(b, n, &g, &used) == 1009);                                             /* message too big */
    n = frame(b, 0, 1, big, 40000);
    n += frame(b + n, 1, 0, big, 40000);
    CHECK(ws(b, n, &g, &used) == 1009);                                             /* too big in pieces */
    n = frame(b, 1, 1, "\xc3\x28", 2);
    CHECK(ws(b, n, &g, &used) == 1007);                                             /* invalid UTF-8 */
    n = frame(b, 1, 1, "\xed\xa0\x80", 3);
    CHECK(ws(b, n, &g, &used) == 1007);                                             /* surrogate */
    n = frame(b, 1, 2, "\xff", 1);
    CHECK(ws(b, n, &g, &used) == 0 && g.op[0] == 2);                                /* binary is passed up */
    n = frame(b, 1, 8, "\x03\xe8", 2);
    n += frame(b + n, 1, 1, "late", 4);
    CHECK(ws(b, n, &g, &used) == 0 && g.n == 1 && g.op[0] == 8);                    /* nothing after close */
    /* non-minimal length encoding */
    unsigned char nm[16] = { 0x81, 0x80 | 126, 0, 5, 1, 2, 3, 4, 'a', 'b', 'c', 'd', 'e' };
    CHECK(ws(nm, 13, &g, &used) == 1002);
    unsigned char huge[14] = { 0x81, 0x80 | 127, 0x80, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0 };
    CHECK(ws(huge, 14, &g, &used) == 1002);
    CHECK(rs_utf8_valid((const unsigned char *)"\xf0\x9f\x91\x91", 4) && !rs_utf8_valid((const unsigned char *)"\xf4\x90\x80\x80", 4));
    CHECK(!rs_utf8_valid((const unsigned char *)"\xc0\xaf", 2) && !rs_utf8_valid((const unsigned char *)"\xe2\x82", 2));
}

static int jin(const char *j, int W, int H, RsInput *in) {
    static char sb[4096];
    RsJson o;
    if (rs_json_parse(j, strlen(j), &o, sb, sizeof sb)) return -1;
    return rs_browser_input(&o, W, H, in);
}

static void test_json(void) {
    RsInput in;
    CHECK(jin("{\"t\":\"m\",\"x\":10,\"y\":20}", 1280, 800, &in) == RS_IN_PTR && in.a == 10 && in.b == 20);
    CHECK(jin("{\"t\":\"m\",\"x\":-5,\"y\":99999}", 1280, 800, &in) == RS_IN_PTR && in.a == 0 && in.b == 799);
    CHECK(jin(" { \"y\" : 1 , \"t\" : \"m\" , \"x\" : 2 } ", 1280, 800, &in) == RS_IN_PTR);
    CHECK(jin("{\"t\":\"m\",\"x\":1}", 1280, 800, &in) == RS_IN_NONE);
    CHECK(jin("{\"t\":\"m\",\"x\":1,\"y\":2}", 0, 0, &in) == RS_IN_NONE);            /* no screen yet */
    CHECK(jin("{\"t\":\"m\",\"x\":\"\\\"y\\\":5\",\"y\":2}", 1280, 800, &in) == RS_IN_NONE);   /* key confusion */
    CHECK(jin("{\"t\":\"m\",\"t\":\"k\",\"x\":1,\"y\":2}", 1280, 800, &in) == -1);   /* duplicate key */
    CHECK(jin("{\"t\":\"m\",\"x\":1.5,\"y\":2}", 1280, 800, &in) == -1);
    CHECK(jin("{\"t\":\"m\",\"x\":01,\"y\":2}", 1280, 800, &in) == -1);
    CHECK(jin("{\"t\":\"m\",\"x\":9007199254740992,\"y\":2}", 1280, 800, &in) == -1);
    CHECK(jin("{\"t\":\"m\",\"x\":1,\"y\":2}x", 1280, 800, &in) == -1);             /* trailing bytes */
    CHECK(jin("{\"t\":\"m\",\"x\":{\"a\":1},\"y\":2}", 1280, 800, &in) == -1);     /* nested */
    CHECK(jin("[1]", 1280, 800, &in) == -1 && jin("", 1280, 800, &in) == -1 && jin("{", 1280, 800, &in) == -1);
    CHECK(jin("{\"T\":\"m\"}", 1280, 800, &in) == -1);
    CHECK(jin("{\"t\":\"b\",\"b\":0,\"d\":1}", 1280, 800, &in) == RS_IN_BTN && in.a == 0 && in.b == 1);
    CHECK(jin("{\"t\":\"b\",\"b\":3,\"d\":1}", 1280, 800, &in) == RS_IN_NONE);
    CHECK(jin("{\"t\":\"w\",\"d\":-500}", 1280, 800, &in) == RS_IN_WHEEL && in.a == -10);
    CHECK(jin("{\"t\":\"k\",\"c\":30,\"d\":1}", 1280, 800, &in) == RS_IN_KEY && in.a == 30);
    CHECK(jin("{\"t\":\"k\",\"c\":272,\"d\":1}", 1280, 800, &in) == RS_IN_NONE);    /* BTN_LEFT is not a key */
    CHECK(jin("{\"t\":\"k\",\"c\":0,\"d\":1}", 1280, 800, &in) == RS_IN_NONE);
    CHECK(jin("{\"t\":\"s\",\"p\":2,\"x\":99999999,\"y\":-99999999}", 1280, 800, &in) == RS_IN_SCROLL && in.v1 == 4000 * 256 && in.v2 == -4000 * 256);
    CHECK(jin("{\"t\":\"s\",\"p\":4,\"x\":1,\"y\":1}", 1280, 800, &in) == RS_IN_NONE);
    CHECK(jin("{\"t\":\"pz\",\"p\":1,\"s\":65536,\"r\":0}", 1280, 800, &in) == RS_IN_PINCH && in.v1 == 65536);
    CHECK(jin("{\"t\":\"pz\",\"p\":2,\"s\":0,\"r\":0}", 1280, 800, &in) == RS_IN_PINCH && in.v1 == 3277);
    CHECK(jin("{\"t\":\"resize\",\"w\":1921,\"h\":1080}", 1280, 800, &in) == RS_IN_RESIZE && in.a == 1920);
    CHECK(jin("{\"t\":\"resize\",\"w\":100000,\"h\":1080}", 1280, 800, &in) == RS_IN_NONE);
    CHECK(jin("{\"t\":\"resize\",\"w\":100,\"h\":100}", 1280, 800, &in) == RS_IN_NONE);
    CHECK(jin("{\"t\":\"resize\",\"w\":1920,\"h\":1080}", 1280, 800, &in) == RS_IN_RESIZE && in.v1 == 0);
    CHECK(jin("{\"t\":\"resize\",\"w\":1920,\"h\":1080,\"hz\":144}", 1280, 800, &in) == RS_IN_RESIZE && in.v1 == 144);
    CHECK(jin("{\"t\":\"resize\",\"w\":1920,\"h\":1080,\"hz\":1000}", 1280, 800, &in) == RS_IN_NONE);
    CHECK(jin("{\"t\":\"resize\",\"w\":1920,\"h\":1080,\"hz\":-60}", 1280, 800, &in) == RS_IN_NONE);
    CHECK(jin("{\"t\":\"ping\",\"ts\":123}", 1280, 800, &in) == RS_IN_PING && in.ts == 123);
    CHECK(jin("{\"t\":\"ack\"}", 1280, 800, &in) == RS_IN_NONE);       /* the image path's ack is gone (0.8.4) */
    CHECK(jin("{\"t\":\"clip\",\"text\":\"h\\u00e9\\ud83d\\udc51\\n\"}", 1280, 800, &in) == RS_IN_CLIP && in.tlen == 8 &&
          !memcmp(in.text, "h\xc3\xa9\xf0\x9f\x91\x91\n", 8));
    CHECK(jin("{\"t\":\"clip\",\"text\":\"\\ud83d\"}", 1280, 800, &in) == -1);        /* lone surrogate */
    CHECK(jin("{\"t\":\"clip\",\"text\":\"\\udc51\"}", 1280, 800, &in) == -1);
    CHECK(jin("{\"t\":\"clip\",\"text\":\"\\u0000\"}", 1280, 800, &in) == -1);        /* NUL */
    CHECK(jin("{\"t\":\"clip\",\"text\":\"a\\x\"}", 1280, 800, &in) == -1);
    CHECK(jin("{\"t\":\"clip\",\"text\":\"\"}", 1280, 800, &in) == RS_IN_NONE);
    CHECK(jin("{\"t\":\"clip\",\"text\":\"a\x01\"}", 1280, 800, &in) == -1);           /* raw control */
    CHECK(jin("{\"t\":\"nope\"}", 1280, 800, &in) == RS_IN_NONE);
}

static void test_ipnet(void) {
    RsAddr a;
    RsCidr c;
    char t[64];
    CHECK(!rs_addr_parse("192.168.1.10", &a) && a.v4);
    CHECK(!rs_addr_parse("::ffff:192.168.1.10", &a) && a.v4);
    CHECK(!rs_addr_parse("2001:db8::1", &a) && !a.v4);
    CHECK(rs_addr_parse("192.168.01.10", &a) && rs_addr_parse("1.2.3", &a) && rs_addr_parse("fe80::1%eth0", &a) && rs_addr_parse(" 1.2.3.4", &a));
    CHECK(rs_addr_parse("256.1.1.1", &a) && rs_addr_parse("", &a));
    CHECK(!rs_cidr_parse("192.168.1.77/24", &c));
    rs_cidr_format(&c, t, sizeof t);
    CHECK(!strcmp(t, "192.168.1.0/24"));                                            /* host bits cleared */
    CHECK(!rs_cidr_parse("10.0.0.1", &c) && c.prefix == 128);
    rs_cidr_format(&c, t, sizeof t);
    CHECK(!strcmp(t, "10.0.0.1"));
    CHECK(rs_cidr_parse("0.0.0.0/0", &c) && rs_cidr_parse("10.0.0.0/7", &c) && rs_cidr_parse("10.0.0.0/33", &c));
    CHECK(rs_cidr_parse("10.0.0.0/024", &c) && rs_cidr_parse("10.0.0.0/", &c) && rs_cidr_parse("10.0.0.0/2a", &c) && rs_cidr_parse("::/0", &c));
    CHECK(!rs_cidr_parse("2001:db8::/32", &c) && !rs_cidr_parse("10.0.0.0/8", &c));
    CHECK(!rs_addr_parse("10.200.3.4", &a) && rs_cidr_contains(&c, &a));
    CHECK(!rs_addr_parse("11.0.0.1", &a) && !rs_cidr_contains(&c, &a));
    CHECK(!rs_addr_parse("2001:db8:1:2:3:4:5:6", &a));
    rs_addr_key(&a, t, sizeof t);
    CHECK(!strcmp(t, "2001:db8:1:2::/64"));
    CHECK(!rs_key_parse("2001:db8:1:2::/64", &c) && rs_cidr_contains(&c, &a));
    CHECK(!rs_key_parse("10.0.0.1", &c) && rs_key_parse("10.0.0.0/24", &c) && rs_key_parse("2001:db8::/48", &c));
    CHECK(!rs_addr_parse("192.168.1.1", &a) && rs_addr_is_local(&a));
    CHECK(!rs_addr_parse("172.20.0.1", &a) && rs_addr_is_local(&a));
    CHECK(!rs_addr_parse("172.32.0.1", &a) && !rs_addr_is_local(&a));
    CHECK(!rs_addr_parse("8.8.8.8", &a) && !rs_addr_is_local(&a));
    CHECK(!rs_addr_parse("fe80::5", &a) && rs_addr_is_local(&a));
    CHECK(!rs_addr_parse("fd12::5", &a) && rs_addr_is_local(&a));
    CHECK(!rs_addr_parse("2a00::5", &a) && !rs_addr_is_local(&a));
    CHECK(!rs_addr_parse("::ffff:8.8.8.8", &a) && !rs_addr_is_local(&a));

    RsAccess acc;
    int bad;
    rs_access_parse("# x\nallow 203.0.113.0/24\nblock 192.168.1.66\nblock nope\nallow 1.2.3.4 extra\nweird line\n", &acc, &bad);
    CHECK(acc.nallow == 1 && acc.nblock == 1 && bad == 3);
    char text[4096];
    CHECK(rs_access_format(&acc, text, sizeof text) > 0 && strstr(text, "allow 203.0.113.0/24\n") && strstr(text, "block 192.168.1.66\n"));
    RsAccess back;
    rs_access_parse(text, &back, &bad);
    CHECK(back.nallow == 1 && back.nblock == 1 && bad == 0);
    rs_addr_parse("192.168.1.66", &a);
    CHECK(rs_access_decide(&acc, RS_NET_ANY, &a, 0) == RS_ACC_BLOCKED);            /* manual block always wins */
    rs_addr_parse("192.168.1.67", &a);
    CHECK(rs_access_decide(&acc, RS_NET_LAN, &a, 0) == RS_ACC_OK);
    CHECK(rs_access_decide(&acc, RS_NET_LIST, &a, 0) == RS_ACC_NOT_ALLOWED);
    rs_addr_parse("203.0.113.9", &a);
    CHECK(rs_access_decide(&acc, RS_NET_LAN, &a, 0) == RS_ACC_NOT_ALLOWED);         /* public, default */
    CHECK(rs_access_decide(&acc, RS_NET_LAN, &a, 1) == RS_ACC_OK);                  /* ...unless over a VPN */
    CHECK(rs_access_decide(&acc, RS_NET_LIST, &a, 0) == RS_ACC_OK);
    CHECK(rs_access_decide(&acc, RS_NET_ANY, &a, 0) == RS_ACC_OK);
    rs_cidr_parse("192.168.1.0/24", &c);
    CHECK(rs_access_add(acc.block, &acc.nblock, &c) == 0 && rs_access_add(acc.block, &acc.nblock, &c) == 1);
    CHECK(rs_access_del(acc.block, &acc.nblock, &c) == 2 && acc.nblock == 0);     /* the /24 and the host inside */
    for (int i = 0; i < RS_ACCESS_MAX; i++) { char s[32]; snprintf(s, sizeof s, "10.0.%d.0/24", i); rs_cidr_parse(s, &c); rs_access_add(acc.block, &acc.nblock, &c); }
    rs_cidr_parse("10.9.9.9", &c);
    CHECK(rs_access_add(acc.block, &acc.nblock, &c) == -1);
    CHECK(rs_net_mode("lan") == RS_NET_LAN && rs_net_mode("any") == RS_NET_ANY && rs_net_mode("list") == RS_NET_LIST && rs_net_mode("all") < 0);
    /* VPN interfaces: only the names LordOS's VPN manager gives */
    CHECK(rs_vpn_ifname("wg-home") && rs_vpn_ifname("ovpn-work") && rs_vpn_ifname("ips-office") && rs_vpn_ifname("ppp-pptp") &&
          rs_vpn_ifname("oc-corp") && rs_vpn_ifname("fvpn-x"));
    CHECK(!rs_vpn_ifname("ppp0") && !rs_vpn_ifname("tun0") && !rs_vpn_ifname("wg0") && !rs_vpn_ifname("eth0") &&
          !rs_vpn_ifname("wlan0") && !rs_vpn_ifname("wg-") && !rs_vpn_ifname(NULL));
    {
        RsAddr ifa, peer, ptp, other;
        rs_addr_parse("10.8.0.2", &ifa);                                              /* wg-home 10.8.0.2/24 */
        rs_addr_parse("10.8.0.7", &peer);
        CHECK(rs_vpn_accepts("wg-home", &ifa, 96 + 24, NULL, &ifa, &peer));           /* in its subnet: OK */
        rs_addr_parse("203.0.113.9", &peer);
        CHECK(!rs_vpn_accepts("wg-home", &ifa, 96 + 24, NULL, &ifa, &peer));          /* public, outside: no */
        rs_addr_parse("10.8.0.7", &peer);
        rs_addr_parse("192.168.1.2", &other);
        CHECK(!rs_vpn_accepts("wg-home", &ifa, 96 + 24, NULL, &other, &peer));        /* arrived on another address */
        CHECK(!rs_vpn_accepts("wg-home", &ifa, 96 + 0, NULL, &ifa, &peer));           /* a /0 "subnet": never */
        rs_addr_parse("100.64.3.4", &ifa);                                            /* ppp0, PPPoE uplink */
        rs_addr_parse("198.51.100.20", &peer);
        CHECK(!rs_vpn_accepts("ppp0", &ifa, 96 + 32, &peer, &ifa, &peer));            /* a public peer on ppp0: no */
        rs_addr_parse("10.20.0.1", &ptp);                                             /* ppp-pptp point to point */
        rs_addr_parse("10.20.0.2", &ifa);
        CHECK(rs_vpn_accepts("ppp-pptp", &ifa, 96 + 32, &ptp, &ifa, &ptp));
        CHECK(!rs_vpn_accepts("ppp-pptp", &ifa, 96 + 32, &ptp, &ifa, &peer));
        /* in lan mode, the decision with that result */
        RsAccess none;
        memset(&none, 0, sizeof none);
        CHECK(rs_access_decide(&none, RS_NET_LAN, &peer, rs_vpn_accepts("ppp0", &ifa, 96 + 32, &peer, &ifa, &peer)) == RS_ACC_NOT_ALLOWED);
        rs_addr_parse("fd00:8::2", &ifa);
        rs_addr_parse("fd00:8::9", &peer);
        CHECK(rs_vpn_accepts("wg-v6", &ifa, 64, NULL, &ifa, &peer));
        rs_addr_parse("2001:db8::9", &peer);
        CHECK(!rs_vpn_accepts("wg-v6", &ifa, 64, NULL, &ifa, &peer));
    }
}

static void test_lockout(void) {
    static RlTable t;
    memset(&t, 0, sizeof t);
    double now = 1e9, until = 0;
    int strikes = 0;
    const char *k = "192.168.1.50";
    CHECK(!rl_fail(&t, k, now, &until, &strikes) && !rl_fail(&t, k, now + 1, &until, &strikes));
    CHECK(rl_blocked(&t, k, now + 2) == 0);
    CHECK(rl_fail(&t, k, now + 2, &until, &strikes) && strikes == 1 && until == now + 2 + 900);   /* 3rd: 15 min */
    CHECK(rl_blocked(&t, k, now + 3) > 890 && rl_blocked(&t, k, now + 903) == 0);
    rl_fail(&t, k, now + 1000, &until, &strikes);
    rl_fail(&t, k, now + 1001, &until, &strikes);
    CHECK(rl_fail(&t, k, now + 1002, &until, &strikes) && strikes == 2 && until == now + 1002 + 3600);   /* 1 h */
    rl_fail(&t, k, now + 5000, &until, &strikes);
    rl_fail(&t, k, now + 5001, &until, &strikes);
    CHECK(rl_fail(&t, k, now + 5002, &until, &strikes) && strikes == 3 && until == now + 5002 + 86400);  /* 24 h */
    /* a day later the strikes are forgotten: 15 min again */
    double later = now + 5002 + 86400 + RL_WINDOW + 10;
    rl_fail(&t, k, later, &until, &strikes);
    rl_fail(&t, k, later + 1, &until, &strikes);
    CHECK(rl_fail(&t, k, later + 2, &until, &strikes) && strikes == 1 && until == later + 2 + 900);
    /* a success clears the count */
    const char *k2 = "192.168.1.51";
    rl_fail(&t, k2, now, &until, &strikes);
    rl_fail(&t, k2, now, &until, &strikes);
    rl_success(&t, k2);
    CHECK(!rl_fail(&t, k2, now, &until, &strikes) && !rl_fail(&t, k2, now, &until, &strikes) && rl_fail(&t, k2, now, &until, &strikes));
    /* persistence */
    char text[RL_MAX * 128 + 256];
    CHECK(rl_format(&t, later + 3, text, sizeof text) > 0);
    static RlTable back;
    rl_parse(&back, text, later + 3);
    CHECK(rl_blocked(&back, k, later + 3) > 800);
    rl_parse(&back, "2001:db8:1:2::/64 1 1000 1 1000 9999999999\n10.0.0.1 2 1000 0 0 0\nbad\n10.0.0.01 1 1 1 1 1\n"
                    "10.0.0.2 7 1000 0 0 0\n10.0.0.3 1 1000 0 0 1e300\n", 2000);
    CHECK(rl_blocked(&back, "2001:db8:1:2::/64", 2000) == 0);                       /* until too far: refused */
    int n = 0;
    for (int i = 0; i < RL_MAX; i++) if (back.e[i].key[0]) n++;
    CHECK(n == 1);                                                                  /* only 10.0.0.1 is valid */
    rl_parse(&back, "2001:db8:1:2::/64 0 1990 1 1990 2500\n", 2000);
    CHECK(rl_blocked(&back, "2001:db8:1:2::/64", 2000) == 500);
    CHECK(rl_unblock(&back, "2001:db8:1:2:aaaa::1") == 1 && rl_blocked(&back, "2001:db8:1:2::/64", 2000) == 0);
    rl_fail(&t, "10.1.1.1", now, NULL, NULL);
    rl_fail(&t, "10.1.1.1", now, NULL, NULL);
    rl_fail(&t, "10.1.1.1", now, NULL, NULL);
    CHECK(rl_unblock(&t, "10.1.0.0/16") == 1 && rl_blocked(&t, "10.1.1.1", now) == 0);
    CHECK(rl_unblock(&t, "garbage") == 0);
    /* a flood of other addresses doesn't evict a running block */
    memset(&t, 0, sizeof t);
    for (int i = 0; i < 3; i++) rl_fail(&t, "192.168.9.9", now, NULL, NULL);
    for (int i = 0; i < 5000; i++) { char s[32]; snprintf(s, sizeof s, "10.%d.%d.%d", i >> 16 & 255, i >> 8 & 255, i & 255); rl_fail(&t, s, now + 1, NULL, NULL); }
    CHECK(rl_blocked(&t, "192.168.9.9", now + 2) > 0);
    CHECK(rl_user_delay(0) == 0 && rl_user_delay(4) == 0 && rl_user_delay(5) == 1 && rl_user_delay(7) == 4 && rl_user_delay(100) == 8);
    static RcTable rc;
    int ok = 0;
    for (int i = 0; i < 100; i++) ok += rc_take(&rc, "1.2.3.4", 10.0);
    CHECK(ok == 20);                                                                /* the burst */
    CHECK(rc_take(&rc, "1.2.3.4", 10.6) == 1 && rc_take(&rc, "1.2.3.4", 10.6) == 0);  /* 2 per second */
    CHECK(rc_take(&rc, "1.2.3.5", 10.6) == 1);                                      /* others are not affected */
}

/* ---- the video path's wire formats (rtcwire.c; docs/REMOTE.md, "Video (WebRTC)") ---- */

/* A small SHA-1 + HMAC for the STUN tests (the test program links only libc). */
typedef struct { uint32_t h[5]; uint64_t n; uint8_t b[64]; size_t bl; } Sha1;
static uint32_t rol(uint32_t x, int k) { return x << k | x >> (32 - k); }
static void sha1_block(Sha1 *s, const uint8_t *p) {
    uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(Sha1 *s) { *s = (Sha1){ { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 }, 0, { 0 }, 0 }; }
static void sha1_add(Sha1 *s, const uint8_t *p, size_t n) {
    s->n += n;
    while (n--) { s->b[s->bl++] = *p++; if (s->bl == 64) { sha1_block(s, s->b); s->bl = 0; } }
}
static void sha1_end(Sha1 *s, uint8_t out[20]) {
    uint64_t bits = s->n * 8;
    uint8_t pad = 0x80, z = 0;
    sha1_add(s, &pad, 1);
    while (s->bl != 56) sha1_add(s, &z, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_add(s, l, 8);
    for (int i = 0; i < 5; i++) { out[i * 4] = (uint8_t)(s->h[i] >> 24); out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16); out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8); out[i * 4 + 3] = (uint8_t)s->h[i]; }
}
static void hmac1(const uint8_t *key, size_t kl, const uint8_t *m, size_t n, uint8_t out[20]) {
    uint8_t k[64] = { 0 }, ip[64], op[64], in[20];
    if (kl > 64) { Sha1 s; sha1_init(&s); sha1_add(&s, key, kl); sha1_end(&s, k); } else memcpy(k, key, kl);
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    Sha1 s;
    sha1_init(&s); sha1_add(&s, ip, 64); sha1_add(&s, m, n); sha1_end(&s, in);
    sha1_init(&s); sha1_add(&s, op, 64); sha1_add(&s, in, 20); sha1_end(&s, out);
}

static int hexbytes(const char *h, uint8_t *out) {
    int n = 0;
    for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); out[n++] = (uint8_t)v; }
    return n;
}

/* the offer with one line replaced (or removed, rep = NULL), or one line added after `after` */
static char *offer_edit(const char *find, const char *rep) {
    static char buf[40000];
    const char *at = strstr(chrome_offer, find);
    if (!at) { snprintf(buf, sizeof buf, "%s", chrome_offer); return buf; }
    const char *end = strstr(at, "\r\n");
    size_t pre = (size_t)(at - chrome_offer);
    snprintf(buf, sizeof buf, "%.*s%s%s", (int)pre, chrome_offer, rep ? rep : "", end ? end + (rep ? 0 : 2) : "");
    return buf;
}

static void test_sdp(void) {
    RwOffer o;
    size_t n = strlen(chrome_offer);
    CHECK(rw_sdp_parse_offer(chrome_offer, n, 1, &o) == RW_OK);
    CHECK(o.nmedia == 3 && o.vm == 0 && o.am == 1 && o.dm == 2 && o.vcodec == RW_CODEC_H264);
    CHECK(o.m[o.vm].pt[0].pt == 96 && o.m[o.dm].sctp_port == 5000 && o.m[o.dm].max_msg == 262144);
    int h264 = 0;
    for (int i = 0; i < o.m[0].npt; i++) if (o.m[0].pt[i].pt == o.vpt) h264 = o.m[0].pt[i].plid == 0x42e01f && o.m[0].pt[i].pmode == 1;
    CHECK(h264);                                                        /* constrained baseline, mode 1 */
    CHECK(!strcmp(o.ufrag, "z1TV") && !strcmp(o.pwd, "RifL1wLzVBkO+l3mNSAqrSe8") && o.fp[0] == 0x70 && o.fp[31] == 0x39);
    CHECK(o.setup == RW_SETUP_ACTPASS && o.ncand >= 1 && o.cand[0].mdns);
    CHECK(rw_sdp_parse_offer(chrome_offer, n, 0, &o) == RW_OK && o.vcodec == RW_CODEC_VP8 && o.vpt == 96);   /* no H.264 encoder */
    /* broken offers */
    const char *e;
    e = offer_edit("a=ice-ufrag:z1TV", NULL); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_OK);          /* the other sections carry it */
    e = offer_edit("a=ice-ufrag:z1TV", "a=ice-ufrag:zzzz"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_EICE);   /* two in one bundle */
    e = offer_edit("a=ice-ufrag:z1TV", "a=ice-ufrag:z1"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_EICE);       /* too short */
    e = offer_edit("a=ice-ufrag:z1TV", "a=ice-ufrag:z1T;V"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_EICE);
    e = offer_edit("a=ice-pwd:RifL1wLzVBkO+l3mNSAqrSe8", "a=ice-pwd:short"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_EICE);
    e = offer_edit("a=fingerprint:sha-256 70:59", NULL); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_OK);      /* the others remain */
    e = offer_edit("a=fingerprint:sha-256 70:59", "a=fingerprint:sha-256 70:59:D6"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_EFP);
    e = offer_edit("a=fingerprint:sha-256 70:59", "a=fingerprint:sha-256 71:59:D6:4B:A3:67:6E:BD:AC:6A:5E:0D:3B:95:92:BA:40:F4:86:12:84:69:6C:F8:F8:D1:A0:A3:D1:C6:46:39");
    CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_EFP);           /* two different fingerprints in one bundle */
    e = offer_edit("a=setup:actpass", "a=setup:passive"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ESYNTAX);
    e = offer_edit("a=group:BUNDLE 0 1 2", NULL); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ESYNTAX);
    e = offer_edit("a=mid:1", "a=mid:0"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ESYNTAX);              /* duplicate mid */
    e = offer_edit("a=mid:1", "a=mid:bad mid"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ESYNTAX);
    e = offer_edit("a=rtcp-mux", NULL); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ESYNTAX);                /* video without rtcp-mux */
    e = offer_edit("a=rtpmap:96 VP8/90000", "a=rtpmap:96 VP8/90000/"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ESYNTAX);
    e = offer_edit("a=rtpmap:96 VP8/90000", "a=rtpmap:200 VP8/90000"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ESYNTAX);  /* pt not listed */
    e = offer_edit("a=rtpmap:96 VP8/90000", "a=rtpmap:97 VP8/90000"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ESYNTAX);   /* twice */
    e = offer_edit("a=fmtp:97 apt=96", "a=fmtp:97 apt=96\r\na=fmtp:97 apt=96"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ESYNTAX);
    e = offer_edit("a=recvonly", "a=sendonly"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ECODEC);           /* no video to send */
    e = offer_edit("v=0", "v=1"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ESYNTAX);
    e = offer_edit("s=-", "s=\x01"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ESYNTAX);                     /* control byte */
    e = offer_edit("s=-", "s=\xc3\xa9"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ESYNTAX);                 /* non-ASCII */
    e = offer_edit("s=-", "S=-"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ESYNTAX);
    e = offer_edit("a=candidate:2529583990", "a=candidate:x 1 udp 1 1.2.3.4 0 typ host"); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ESYNTAX);
    static char strip[40000];
    for (int pass = 0; pass < 2; pass++) {                              /* no ufrag anywhere; no fingerprint anywhere */
        const char *key = pass ? "a=fingerprint:" : "a=ice-ufrag:";
        size_t o2 = 0;
        for (const char *q = chrome_offer; *q;) {
            const char *eol = strstr(q, "\r\n");
            size_t l = eol ? (size_t)(eol - q) + 2 : strlen(q);
            if (strncmp(q, key, strlen(key))) { memcpy(strip + o2, q, l); o2 += l; }
            q += l;
        }
        strip[o2] = 0;
        CHECK(rw_sdp_parse_offer(strip, o2, 1, &o) == (pass ? RW_EFP : RW_EICE));
    }
    /* sizes */
    static char big[RW_SDP_MAX + 100];
    memset(big, 0, sizeof big);
    snprintf(big, sizeof big, "%s", chrome_offer);
    size_t bl = strlen(big);
    while (bl + 20 < RW_SDP_MAX + 50) { memcpy(big + bl, "a=x-filler:1\r\n", 14); bl += 14; }
    CHECK(rw_sdp_parse_offer(big, bl, 1, &o) == RW_ETOOBIG);
    char longline[2000];
    memset(longline, 'a', sizeof longline);
    memcpy(longline, "a=x:", 4);
    longline[1500] = 0;
    e = offer_edit("s=-", longline); CHECK(rw_sdp_parse_offer(e, strlen(e), 1, &o) == RW_ETOOBIG);
    static char many[RW_SDP_MAX];
    size_t ml = (size_t)snprintf(many, sizeof many, "v=0\r\n");
    for (int i = 0; i < 700 && ml + 8 < sizeof many; i++) { memcpy(many + ml, "a=x\r\n", 5); ml += 5; }
    CHECK(rw_sdp_parse_offer(many, ml, 1, &o) == RW_ETOOBIG);
    CHECK(rw_sdp_parse_offer("", 0, 1, &o) == RW_ESYNTAX);
    /* the answer */
    CHECK(rw_sdp_parse_offer(chrome_offer, n, 1, &o) == RW_OK);
    uint8_t fp[32];
    for (int i = 0; i < 32; i++) fp[i] = (uint8_t)i;
    RwLocalCand c[2] = { { "192.168.1.5", 40000, RW_CAND_HOST, 2130706431, "", 0, 0 }, { "203.0.113.9", 3478, RW_CAND_RELAY, 33554431, "198.51.100.2", 5555, 0 } };
    RwAnswer a = { "AbCdEfGh", "0123456789abcdefABCDEF01", fp, 111, 222, 1, c, 2, 42, 0 };
    char ans[16384];
    int al = rw_sdp_answer(&o, &a, ans, sizeof ans);
    CHECK(al > 0 && strstr(ans, "a=ice-lite\r\n") && strstr(ans, "a=setup:passive\r\n") && strstr(ans, "a=group:BUNDLE 0 1 2\r\n"));
    CHECK(strstr(ans, "a=fingerprint:sha-256 00:01:02:03:") != NULL);
    char want[64];
    snprintf(want, sizeof want, "a=rtpmap:%d H264/90000\r\n", o.vpt);
    CHECK(strstr(ans, want) && strstr(ans, "packetization-mode=1;profile-level-id=42e01f"));
    CHECK(strstr(ans, "a=candidate:10 1 udp 2130706431 192.168.1.5 40000 typ host\r\n"));
    CHECK(strstr(ans, "typ relay raddr 198.51.100.2 rport 5555\r\n") && strstr(ans, "a=sctp-port:5000"));
    CHECK(strstr(ans, "a=ssrc:111 cname:lordos") && strstr(ans, "a=ssrc:222 cname:lordos") && strstr(ans, "a=rtpmap:111 opus/48000/2"));
    CHECK(!strstr(ans, "RifL1w"));                                      /* never echoes the browser's secrets */
    CHECK(rw_sdp_answer(&o, &a, ans, 200) == -1);                       /* doesn't fit */
    CHECK(strstr(ans, "o=- 42 2 IN IP4") != NULL);                      /* first answer: version 2 */
    /* ICE-TCP: a passive TCP host candidate on the remote desktop port; an ICE restart's answer has the
     * next session version */
    RwLocalCand t[2] = { { "192.168.1.5", 40000, RW_CAND_HOST, 2130706431, "", 0, 0 }, { "192.168.1.5", 5673, RW_CAND_HOST, 1358954751, "", 0, 1 } };
    RwAnswer b = { "AbCdEfGhIjKl", "0123456789abcdefABCDEF01", fp, 111, 222, 1, t, 2, 42, 3 };
    al = rw_sdp_answer(&o, &b, ans, sizeof ans);
    CHECK(al > 0 && strstr(ans, "a=candidate:11 1 tcp 1358954751 192.168.1.5 5673 typ host tcptype passive\r\n"));
    CHECK(strstr(ans, "a=candidate:10 1 udp 2130706431 192.168.1.5 40000 typ host\r\n") && strstr(ans, "o=- 42 3 IN IP4"));
    CHECK(strstr(ans, "a=ice-ufrag:AbCdEfGhIjKl\r\n") != NULL);
}

/* ICE over TCP (RFC 6544/4571 framing), the TURN server's stream, the route a root child reads from the
 * first check, the TURN transports and the pinned fingerprint */
static void test_tcp(void) {
    size_t len = 0;
    uint8_t f[8] = { 0, 3, 'a', 'b', 'c', 0, 1 };
    CHECK(rw_tcp_frame(f, 0, &len) == 0 && rw_tcp_frame(f, 1, &len) == 0 && rw_tcp_frame(f, 4, &len) == 0);
    CHECK(rw_tcp_frame(f, 5, &len) == 1 && len == 3);
    CHECK(rw_tcp_frame(f + 5, 2, &len) == 0);                           /* length 1, no byte yet */
    uint8_t z[2] = { 0, 0 }, big[2] = { 0x10, 0x01 }, max[2] = { 0x10, 0x00 };
    CHECK(rw_tcp_frame(z, 2, &len) == -1 && rw_tcp_frame(big, 2, &len) == -1 && rw_tcp_frame(max, 2, &len) == 0);
    /* a STUN stream: a binding answer (from test_stun) whole, cut, and followed by ChannelData */
    uint8_t p[200];
    int n = hexbytes("0101002c2112a442496f307873764d3849326171002000080001df408d03a44700080014891a716ef20e7a305d8e94f44f899dba06a12704802800049abe8fcc", p);
    CHECK(rw_stun_stream(p, (size_t)n, &len) == 1 && len == (size_t)n);
    CHECK(rw_stun_stream(p, 19, &len) == 0 && rw_stun_stream(p, (size_t)n - 1, &len) == 0 && rw_stun_stream(p, 0, &len) == 0);
    uint8_t cd[8] = { 0x40, 0x00, 0x00, 0x04, 1, 2, 3, 4 };
    CHECK(rw_stun_stream(cd, 8, &len) == -1);                           /* ChannelData: we never bind channels */
    uint8_t odd[20];
    memcpy(odd, p, 20);
    odd[3] = 0x2d;                                                      /* length not a multiple of 4 */
    CHECK(rw_stun_stream(odd, 20, &len) == -1);
    memcpy(odd, p, 20);
    odd[4] = 0;                                                         /* no magic cookie */
    CHECK(rw_stun_stream(odd, 20, &len) == -1);
    /* the route: a binding request with MESSAGE-INTEGRITY and a user name "<8 alnum><more>:<theirs>" */
    uint8_t tid[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    RwStunOut o;
    rw_so_init(&o, RW_STUN_BINDING_REQ, tid);
    rw_so_attr(&o, RW_ATTR_USERNAME, "Ab3dEf7hWxYz:brws", 17);
    rw_so_mi(&o, (const uint8_t *)"pw", 2, hmac1);
    rw_so_fingerprint(&o);
    char route[9];
    CHECK(rw_ice_route(o.b, o.n, route) == 0 && !strcmp(route, "Ab3dEf7h"));
    const char *bad_users[] = { "Ab3dEf7:brws", "Ab3d+f7hWx:b", "Ab3dEf7hWxYz", "short:x" };
    for (size_t i = 0; i < sizeof bad_users / sizeof bad_users[0]; i++) {
        rw_so_init(&o, RW_STUN_BINDING_REQ, tid);
        rw_so_attr(&o, RW_ATTR_USERNAME, bad_users[i], strlen(bad_users[i]));
        rw_so_mi(&o, (const uint8_t *)"pw", 2, hmac1);
        rw_so_fingerprint(&o);
        CHECK(rw_ice_route(o.b, o.n, route) == -1);
    }
    rw_so_init(&o, RW_STUN_BINDING_REQ, tid);                           /* no integrity: never routed */
    rw_so_attr(&o, RW_ATTR_USERNAME, "Ab3dEf7hWxYz:brws", 17);
    rw_so_fingerprint(&o);
    CHECK(rw_ice_route(o.b, o.n, route) == -1);
    rw_so_init(&o, RW_STUN_BINDING_OK, tid);                            /* not a request */
    rw_so_attr(&o, RW_ATTR_USERNAME, "Ab3dEf7hWxYz:brws", 17);
    rw_so_mi(&o, (const uint8_t *)"pw", 2, hmac1);
    CHECK(rw_ice_route(o.b, o.n, route) == -1);
    CHECK(rw_ice_route((const uint8_t *)"\x16\x03\x01", 3, route) == -1);   /* a TLS ClientHello */
    /* TURN transports and the pin */
    CHECK(rw_turn_transport("udp") == RW_TURN_UDP && rw_turn_transport("tcp") == RW_TURN_TCP && rw_turn_transport("tls") == RW_TURN_TLS);
    CHECK(rw_turn_transport("TLS") < 0 && rw_turn_transport("") < 0 && rw_turn_transport("dtls") < 0 && rw_turn_transport("tls ") < 0);
    CHECK(!strcmp(rw_turn_transport_name(RW_TURN_TLS), "tls") && !strcmp(rw_turn_transport_name(99), "udp"));
    uint8_t fp[32];
    CHECK(rw_fp_parse("000102030405060708090a0b0c0d0e0f101112131415161718191A1B1C1D1E1F", fp) == 0 && fp[0] == 0 && fp[31] == 0x1f && fp[26] == 0x1a);
    CHECK(rw_fp_parse("00:01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:FF", fp) == 0 && fp[31] == 0xff);
    CHECK(rw_fp_parse("00:01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E", fp) == -1);
    CHECK(rw_fp_parse("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1g", fp) == -1);
    CHECK(rw_fp_parse("00-01-02-03-04-05-06-07-08-09-0A-0B-0C-0D-0E-0F-10-11-12-13-14-15-16-17-18-19-1A-1B-1C-1D-1E-FF", fp) == -1);
    CHECK(rw_fp_parse("", fp) == -1 && rw_fp_parse("00", fp) == -1);
}

static void test_cand(void) {
    RwCand c;
    const char *ok[] = {
        "candidate:1 1 udp 2113937151 192.168.1.20 54321 typ host generation 0 network-cost 999",
        "a=candidate:842163049 1 udp 1677729535 203.0.113.4 61000 typ srflx raddr 0.0.0.0 rport 0 generation 0",
        "candidate:3 1 UDP 33554431 2001:db8::7 3478 typ relay raddr 2001:db8::1 rport 9",
        "candidate:4 1 udp 2113937151 3d9863a1-3b3d-48f6-9ff1-49c2bc04877f.local 64198 typ host",
        "candidate:5 1 tcp 1518280447 192.168.1.20 9 typ host tcptype active",
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) CHECK(rw_cand_parse(ok[i], strlen(ok[i]), &c) == RW_OK);
    CHECK(rw_cand_parse(ok[0], strlen(ok[0]), &c) == 0 && c.type == RW_CAND_HOST && c.addr.family == 4 && c.addr.port == 54321 && !c.mdns);
    CHECK(rw_cand_parse(ok[2], strlen(ok[2]), &c) == 0 && c.type == RW_CAND_RELAY && c.addr.family == 6);
    CHECK(rw_cand_parse(ok[3], strlen(ok[3]), &c) == 0 && c.mdns);
    CHECK(rw_cand_parse(ok[4], strlen(ok[4]), &c) == 0 && c.tcp);
    const char *bad[] = {
        "candidate:1 1 udp 2113937151 192.168.1.20 0 typ host",            /* port 0 */
        "candidate:1 1 udp 2113937151 192.168.1.20 70000 typ host",
        "candidate:1 1 udp 4294967296 192.168.1.20 5 typ host",            /* priority over 32 bits */
        "candidate:1 1 sctp 1 192.168.1.20 5 typ host",
        "candidate:1 1 udp 1 192.168.1.20 5 typ hostx",
        "candidate:1 1 udp 1 192.168.001.20 5 typ host",                  /* leading zeros */
        "candidate:1 1 udp 1 evil.example.com 5 typ host",                 /* only mDNS names */
        "candidate:1 1 udp 1 1.2.3.4 5 typ host raddr",                    /* odd extension */
        "candidate:1 1 udp 1 1.2.3.4 5 typ host X y",
        "candidate:1  1 udp 1 1.2.3.4 5 typ host",                         /* double space */
        "candidate:1 1 udp 1 1.2.3.4 5 typ host ",
        "candidate:a;b 1 udp 1 1.2.3.4 5 typ host",
        "candidate:1 0 udp 1 1.2.3.4 5 typ host",
        "candidate:1 1 udp 1 1.2.3.4 5 typ host\x01",
        "cand:1 1 udp 1 1.2.3.4 5 typ host",
        "",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(rw_cand_parse(bad[i], strlen(bad[i]), &c) != RW_OK);
    char huge[700];
    memset(huge, 'a', sizeof huge);
    memcpy(huge, "candidate:", 10);
    CHECK(rw_cand_parse(huge, sizeof huge, &c) != RW_OK);
    CHECK(rw_ice_server_valid("stun.example.org:3478") && rw_ice_server_valid("192.0.2.1:19302") && rw_ice_server_valid("[2001:db8::1]:3478"));
    CHECK(!rw_ice_server_valid("stun.example.org") && !rw_ice_server_valid("a:0") && !rw_ice_server_valid("a:65536") && !rw_ice_server_valid("a:03478"));
    CHECK(!rw_ice_server_valid("-a.example:1") && !rw_ice_server_valid("a..b:1") && !rw_ice_server_valid("a b:1") && !rw_ice_server_valid("[1.2.3.4]:5"));
    CHECK(!rw_ice_server_valid("turn:x.example:1") && !rw_ice_server_valid("x.example:1\n"));
    CHECK(rw_turn_user_valid("1700000000:alice") && !rw_turn_user_valid("al ice") && !rw_turn_user_valid("a\"b") && !rw_turn_user_valid(""));
    CHECK(rw_turn_pass_valid("p a$$ w0rd") && !rw_turn_pass_valid("a\nb") && !rw_turn_pass_valid(""));
}

static void test_stun(void) {
    /* a real answer of ours to Chrome, checked by hand (ICE password tnGnUTLhcP7YsCoR9B8TOTo3) */
    uint8_t p[200];
    int n = hexbytes("0101002c2112a442496f307873764d3849326171002000080001df408d03a44700080014891a716ef20e7a305d8e94f44f899dba06a12704802800049abe8fcc", p);
    RwStun s;
    CHECK(rw_is_stun(p, (size_t)n) && rw_stun_parse(p, (size_t)n, &s) == 0);
    CHECK(s.type == RW_STUN_BINDING_OK && s.has_fp && s.mi_off == 32 && s.xor_mapped.family == 4 && s.xor_mapped.port == 65106);
    CHECK(s.xor_mapped.a[0] == 172 && s.xor_mapped.a[1] == 17 && s.xor_mapped.a[3] == 5);
    const char *pw = "tnGnUTLhcP7YsCoR9B8TOTo3";
    CHECK(rw_stun_check_mi(p, (size_t)n, &s, (const uint8_t *)pw, strlen(pw), hmac1));
    CHECK(!rw_stun_check_mi(p, (size_t)n, &s, (const uint8_t *)"wrong-password-xxxxxxxx", 23, hmac1));
    /* our builder makes the same bytes */
    RwStunOut o;
    rw_so_init(&o, RW_STUN_BINDING_OK, p + 8);
    rw_so_xaddr(&o, RW_ATTR_XOR_MAPPED, &s.xor_mapped);
    rw_so_mi(&o, (const uint8_t *)pw, strlen(pw), hmac1);
    rw_so_fingerprint(&o);
    CHECK(!o.bad && o.n == (size_t)n && !memcmp(o.b, p, (size_t)n));
    /* a request like Chrome's: username, priority, controlling, use-candidate */
    uint8_t tid[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 }, ctl[8] = { 0 };
    rw_so_init(&o, RW_STUN_BINDING_REQ, tid);
    rw_so_attr(&o, RW_ATTR_USERNAME, "AbCdEfGh:z1TV", 13);
    rw_so_u32(&o, RW_ATTR_PRIORITY, 1853824767);
    rw_so_attr(&o, RW_ATTR_CONTROLLING, ctl, 8);
    rw_so_attr(&o, RW_ATTR_USE_CAND, NULL, 0);
    rw_so_attr(&o, 0xc057, "\0\0\x03\xe7", 4);                         /* comprehension-optional: skipped */
    rw_so_mi(&o, (const uint8_t *)"key", 3, hmac1);
    rw_so_fingerprint(&o);
    CHECK(rw_stun_parse(o.b, o.n, &s) == 0 && s.ulen == 13 && s.use_candidate && s.controlling && s.priority == 1853824767);
    CHECK(rw_stun_check_mi(o.b, o.n, &s, (const uint8_t *)"key", 3, hmac1));
    /* tampering */
    uint8_t t[RW_STUN_MAX];
    memcpy(t, o.b, o.n);
    t[30] ^= 1;
    CHECK(rw_stun_parse(t, o.n, &s) != 0);                              /* the fingerprint catches it */
    memcpy(t, o.b, o.n);
    t[3] += 4;
    CHECK(rw_stun_parse(t, o.n, &s) != 0);                              /* length doesn't match */
    CHECK(rw_stun_parse(o.b, o.n - 1, &s) != 0 && rw_stun_parse(o.b, 19, &s) != 0);
    memcpy(t, o.b, o.n);
    t[4] = 0;
    CHECK(rw_stun_parse(t, o.n, &s) != 0 && !rw_is_stun(t, o.n));       /* no magic cookie */
    /* structure rules */
    rw_so_init(&o, RW_STUN_BINDING_REQ, tid);
    rw_so_attr(&o, RW_ATTR_USERNAME, "a:b", 3);
    rw_so_attr(&o, RW_ATTR_USERNAME, "a:b", 3);
    CHECK(rw_stun_parse(o.b, o.n, &s) != 0);                            /* the same attribute twice */
    rw_so_init(&o, RW_STUN_BINDING_REQ, tid);
    rw_so_mi(&o, (const uint8_t *)"k", 1, hmac1);
    rw_so_attr(&o, RW_ATTR_USERNAME, "a:b", 3);
    CHECK(rw_stun_parse(o.b, o.n, &s) != 0);                            /* something after MESSAGE-INTEGRITY */
    rw_so_init(&o, RW_STUN_BINDING_REQ, tid);
    rw_so_fingerprint(&o);
    rw_so_attr(&o, RW_ATTR_SOFTWARE, "x", 1);
    CHECK(rw_stun_parse(o.b, o.n, &s) != 0);                            /* something after FINGERPRINT */
    rw_so_init(&o, RW_STUN_BINDING_REQ, tid);
    rw_so_attr(&o, 0x0030, "x", 1);
    CHECK(rw_stun_parse(o.b, o.n, &s) != 0);                            /* unknown comprehension-required */
    rw_so_init(&o, RW_STUN_BINDING_REQ, tid);
    rw_so_attr(&o, RW_ATTR_PRIORITY, "abc", 3);
    CHECK(rw_stun_parse(o.b, o.n, &s) != 0);                            /* wrong size */
    rw_so_init(&o, RW_STUN_BINDING_REQ, tid);
    rw_so_attr(&o, RW_ATTR_XOR_MAPPED, "\0\3\0\0\0\0\0\0", 8);
    CHECK(rw_stun_parse(o.b, o.n, &s) != 0);                            /* unknown address family */
    rw_so_init(&o, RW_TURN_ALLOCATE_ERR, tid);
    rw_so_attr(&o, RW_ATTR_ERROR, "\0\0\4\1Unauthorized", 16);
    rw_so_attr(&o, RW_ATTR_REALM, "lordos.test", 11);
    rw_so_attr(&o, RW_ATTR_NONCE, "abcdef", 6);
    CHECK(rw_stun_parse(o.b, o.n, &s) == 0 && s.error == 401 && s.rlen == 11 && s.nlen == 6);
    rw_so_init(&o, RW_TURN_ALLOCATE_ERR, tid);
    rw_so_attr(&o, RW_ATTR_ERROR, "\0\0\x09\1x", 5);
    CHECK(rw_stun_parse(o.b, o.n, &s) != 0);                            /* class 9 */
    RwAddr v6 = { 6, { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7 }, 3478 };
    rw_so_init(&o, RW_TURN_ALLOCATE_OK, tid);
    rw_so_xaddr(&o, RW_ATTR_XOR_RELAYED, &v6);
    rw_so_u32(&o, RW_ATTR_LIFETIME, 600);
    CHECK(rw_stun_parse(o.b, o.n, &s) == 0 && rw_addr_equal(&s.xor_relayed, &v6) && s.lifetime == 600);
    char txt[64];
    rw_addr_format(&v6, txt, sizeof txt);
    CHECK(!strcmp(txt, "2001:db8::7"));
}

typedef struct { int n, rr, pli, fir, nack, remb; uint64_t br; } FbCount;
static void fb_count(void *ud, const RwFb *f) {
    FbCount *c = ud;
    c->n++;
    if (f->kind == RW_FB_RR) c->rr++;
    if (f->kind == RW_FB_PLI) c->pli++;
    if (f->kind == RW_FB_FIR) c->fir++;
    if (f->kind == RW_FB_NACK) c->nack++;
    if (f->kind == RW_FB_REMB) { c->remb++; c->br = f->bitrate; }
}

static void test_rtcp(void) {
    uint8_t p[300];
    FbCount c = { 0 };
    /* RR with one block + PLI + NACK(2 items) + REMB, one compound packet */
    int n = hexbytes("81c90007" "11111111" "22222222" "19000005" "00000100" "00000010" "00000000" "00000000"
                     "81ce0002" "11111111" "22222222"
                     "81cd0004" "11111111" "22222222" "0064000f" "00c80000"
                     "8fce0005" "11111111" "00000000" "52454d42" "0108e800" "22222222", p);
    CHECK(rw_rtcp_parse(p, (size_t)n, fb_count, &c) == 0 && c.rr == 1 && c.pli == 1 && c.nack == 2 && c.remb == 1 && c.br == (uint64_t)0xe800 << 2);
    int k = hexbytes("84ce0004" "11111111" "00000000" "22222222" "05000000", p);
    memset(&c, 0, sizeof c);
    CHECK(rw_rtcp_parse(p, (size_t)k, fb_count, &c) == 0 && c.fir == 1);
    /* broken: wrong version, length past the end, an RR claiming more blocks, a NACK without items,
     * padding not at the end, a non-RTCP type; nothing is reported for a broken packet */
    const char *bad[] = { "41ce000211111111" "22222222", "81ce000311111111" "22222222", "82c9000711111111" "22222222",
                          "81cd000211111111" "22222222", "a1ce0002111111112222222281ce00021111111122222222", "81c8000011111111" "",
                          "81640001" "00000000", "80c9" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memset(&c, 0, sizeof c);
        int bn = hexbytes(bad[i], p);
        CHECK(rw_rtcp_parse(p, (size_t)bn, fb_count, &c) != 0 && c.n == 0);
    }
    /* a valid packet followed by garbage: the good part isn't reported either */
    n = hexbytes("81ce0002" "11111111" "22222222" "ff", p);
    memset(&c, 0, sizeof c);
    CHECK(rw_rtcp_parse(p, (size_t)n, fb_count, &c) != 0 && c.n == 0);
}

static void test_dcep(void) {
    RwDcOpen o;
    uint8_t p[100];
    int n = hexbytes("0300000000000000000500006c6f6c6f6c", p);    /* label "lolol" */
    CHECK(rw_dcep_open(p, (size_t)n, &o) == 0 && !strcmp(o.label, "lolol"));
    n = hexbytes("03800000000000000005000074696c6573", p);
    CHECK(rw_dcep_open(p, (size_t)n, &o) == 0 && o.chan_type == 0x80 && !strcmp(o.label, "tiles"));
    n = hexbytes("0301000000000003000500006c6f6c6f6c", p);     /* partially reliable: refused */
    CHECK(rw_dcep_open(p, (size_t)n, &o) != 0);
    n = hexbytes("0300000000000000000500016c6f6c6f6c78", p);   /* a subprotocol */
    CHECK(rw_dcep_open(p, (size_t)n, &o) != 0);
    n = hexbytes("0300000000000000000600006c6f6c6f6c", p);     /* label longer than the message */
    CHECK(rw_dcep_open(p, (size_t)n, &o) != 0);
    n = hexbytes("030000000000000000050000496e505554", p);     /* capitals */
    CHECK(rw_dcep_open(p, (size_t)n, &o) != 0);
    n = hexbytes("02", p);
    CHECK(rw_dcep_open(p, (size_t)n, &o) != 0);
    CHECK(rw_dcep_open(p, 0, &o) != 0);
}

typedef struct { int n, marker_at, starts, ends; size_t bytes; uint8_t first_hdr[2]; } PkCount;
static void pk_out(void *ud, const uint8_t *h, size_t hl, const uint8_t *d, size_t n, int marker) {
    PkCount *c = ud;
    (void)d;
    if (hl == 2 && (h[1] & 0x80)) c->starts++;
    if (hl == 2 && (h[1] & 0x40)) c->ends++;
    if (!c->n && hl) memcpy(c->first_hdr, h, hl < 2 ? hl : 2);
    if (marker) c->marker_at = c->n;
    c->n++;
    c->bytes += hl + n;
}

static void test_rtp(void) {
    static uint8_t au[4000];
    size_t n = 0;
    const uint8_t sps[] = { 0, 0, 0, 1, 0x67, 0x42, 0xe0, 0x1f, 0xaa }, pps[] = { 0, 0, 1, 0x68, 0xce, 0x3c, 0x80 };
    memcpy(au + n, sps, sizeof sps); n += sizeof sps;
    memcpy(au + n, pps, sizeof pps); n += sizeof pps;
    au[n++] = 0; au[n++] = 0; au[n++] = 0; au[n++] = 1; au[n++] = 0x65;      /* IDR */
    for (int i = 0; i < 3000; i++) au[n++] = (uint8_t)(i % 251 + 1);
    PkCount c = { 0 };
    CHECK(rw_h264_is_idr(au, n));
    int k = rw_h264_packetize(au, n, 1100, pk_out, &c);
    CHECK(k == 2 + 3 && c.n == 5 && c.marker_at == 4 && c.starts == 1 && c.ends == 1);
    CHECK(c.bytes == 5 + 4 + 3 * 2 + 3000);                             /* SPS, PPS, FU-A headers + IDR body */
    uint8_t p[] = { 0, 0, 1, 0x41, 1, 2, 3 };
    memset(&c, 0, sizeof c);
    CHECK(rw_h264_packetize(p, sizeof p, 1100, pk_out, &c) == 1 && !rw_h264_is_idr(p, sizeof p) && c.marker_at == 0);
    uint8_t nostart[] = { 0x65, 1, 2, 3 };
    CHECK(rw_h264_packetize(nostart, sizeof nostart, 1100, pk_out, &c) == -1);
    uint8_t forbidden[] = { 0, 0, 1, 0xe5, 1 };
    CHECK(rw_h264_packetize(forbidden, sizeof forbidden, 1100, pk_out, &c) == -1);
    CHECK(rw_h264_packetize(au, n, 10, pk_out, &c) == -1);
    memset(&c, 0, sizeof c);
    CHECK(rw_vp8_packetize(au, 2500, 1000, pk_out, &c) == 3 && c.marker_at == 2 && c.first_hdr[0] == 0x10);
}

static void test_signal(void) {
    char sb[256];
    RsJson o;
    RsInput in;
    const char *j = "{\"t\":\"rtc\",\"sdp\":\"v=0\\r\\nm=video 9\"}";
    CHECK(rs_json_parse(j, strlen(j), &o, sb, sizeof sb) == 0 && rs_browser_input(&o, 1280, 800, &in) == RS_IN_RTC && in.tlen == 14);
    j = "{\"t\":\"rtc\",\"sdp\":\"v=0\"}";
    CHECK(rs_json_parse(j, strlen(j), &o, sb, sizeof sb) == 0 && rs_browser_input(&o, 1280, 800, &in) == RS_IN_NONE);    /* too short */
    j = "{\"t\":\"rtc\",\"sdp\":5}";
    CHECK(rs_json_parse(j, strlen(j), &o, sb, sizeof sb) == 0 && rs_browser_input(&o, 1280, 800, &in) == RS_IN_NONE);
    j = "{\"t\":\"ice\",\"c\":\"candidate:1 1 udp 1 1.2.3.4 5 typ host\"}";
    CHECK(rs_json_parse(j, strlen(j), &o, sb, sizeof sb) == 0 && rs_browser_input(&o, 1280, 800, &in) == RS_IN_ICE);
    j = "{\"t\":\"ice\",\"c\":\"\"}";
    CHECK(rs_json_parse(j, strlen(j), &o, sb, sizeof sb) == 0 && rs_browser_input(&o, 1280, 800, &in) == RS_IN_ICE && in.tlen == 0);
    /* the peer number: optional, 1..1000000, an integer */
    j = "{\"t\":\"rtc\",\"sdp\":\"v=0\\r\\nm=video 9\",\"p\":3}";
    CHECK(rs_json_parse(j, strlen(j), &o, sb, sizeof sb) == 0 && rs_browser_input(&o, 1280, 800, &in) == RS_IN_RTC && in.a == 3);
    j = "{\"t\":\"rtc\",\"sdp\":\"v=0\\r\\nm=video 9\"}";
    CHECK(rs_json_parse(j, strlen(j), &o, sb, sizeof sb) == 0 && rs_browser_input(&o, 1280, 800, &in) == RS_IN_RTC && in.a == 0);
    j = "{\"t\":\"rtc\",\"sdp\":\"v=0\\r\\nm=video 9\",\"p\":0}";
    CHECK(rs_json_parse(j, strlen(j), &o, sb, sizeof sb) == 0 && rs_browser_input(&o, 1280, 800, &in) == RS_IN_NONE);
    j = "{\"t\":\"ice\",\"c\":\"\",\"p\":1000001}";
    CHECK(rs_json_parse(j, strlen(j), &o, sb, sizeof sb) == 0 && rs_browser_input(&o, 1280, 800, &in) == RS_IN_NONE);
    j = "{\"t\":\"ice\",\"c\":\"\",\"p\":\"2\"}";
    CHECK(rs_json_parse(j, strlen(j), &o, sb, sizeof sb) == 0 && rs_browser_input(&o, 1280, 800, &in) == RS_IN_NONE);   /* a string: refused */
    j = "{\"t\":\"vmode\",\"on\":1}";
    CHECK(rs_json_parse(j, strlen(j), &o, sb, sizeof sb) == 0 && rs_browser_input(&o, 1280, 800, &in) == RS_IN_VMODE && in.a == 1);
    j = "{\"t\":\"vmode\",\"on\":2}";
    CHECK(rs_json_parse(j, strlen(j), &o, sb, sizeof sb) == 0 && rs_browser_input(&o, 1280, 800, &in) == RS_IN_NONE);
    static char big[RS_SDP_MAX + 64], bsb[RS_SDP_MAX + 64];
    size_t bl = (size_t)snprintf(big, sizeof big, "{\"t\":\"rtc\",\"sdp\":\"");
    while (bl < RS_SDP_MAX + 30) big[bl++] = 'a';
    bl += (size_t)snprintf(big + bl, sizeof big - bl, "\"}");
    CHECK(rs_json_parse(big, bl, &o, bsb, sizeof bsb) == 0 && rs_browser_input(&o, 1280, 800, &in) == RS_IN_NONE);   /* oversized offer */
}

int main(void) {
    test_head();
    test_small();
    test_login();
    test_ws();
    test_json();
    test_ipnet();
    test_lockout();
    test_sdp();
    test_cand();
    test_stun();
    test_rtcp();
    test_dcep();
    test_rtp();
    test_signal();
    test_tcp();
    printf("remote-sec-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
