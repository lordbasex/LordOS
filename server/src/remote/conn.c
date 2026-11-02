/* conn.c — one remote desktop connection, in a child process of lord-sessiond.
 *
 *   1. TLS (OpenSSL) and one HTTP request, parsed strictly (rsec.c) under a deadline: the login
 *      page (GET /), the login itself (POST /login), the desktop (GET /desk), logout (POST /logout)
 *      or the WebSocket (GET /ws). Anything else is refused and the connection closed.
 *   2. Login: system user and password, checked against /etc/shadow with crypt(3) (sess.c). Only
 *      regular accounts (uid 1000..59999, a shell from /etc/shells, not locked, not expired, never
 *      root; if the configuration asks for it, in the lordos-remote group). Unknown and refused
 *      accounts cost the same time as a wrong password. 3 failures block the source (lockout.c):
 *      15 min, then 1 h, then 24 h. A random cookie (__Host-, Secure, HttpOnly, SameSite=Strict,
 *      only its SHA-256 is kept) lasts 12 hours.
 *   3. WebSocket: finds the user's session (the same one that may be showing on the monitor) or
 *      creates it (sess.c, docs/SESSIONS.md). Then this process forks the bridge, which unmaps the
 *      shared state, closes every other descriptor and DROPS root for good: TLS, compression and
 *      the bridge run as the user. This root process only waits, and when the bridge says the
 *      session was locked it ends every cookie of that user.
 *   4. Bridge: the screen only ever reaches the browser over WebRTC (rtc.c, docs/REMOTE.md "Video
 *      (WebRTC)"). The browser sends its offer over this WebSocket; the bridge answers, the session's
 *      shared encoder feeds it H.264 or VP8 and Opus (RP_VFRAME, RP_AFRAME), and input and the
 *      lossless PNG refresh of still areas go over data channels. The WebSocket carries the sign-in,
 *      the signaling, the cursor, the clipboard and pings; mouse, keyboard and clipboard from either
 *      are validated by rsec.c rs_browser_input. When a peer fails the bridge says why and makes a new
 *      one; the page restarts ICE or reconnects on its own. There is no other way to show the screen.
 *   5. ICE over TCP: a connection to this same port that starts with a framed STUN check instead of
 *      a TLS ClientHello is handed, by this root child, to the bridge its check names (ice_tcp()).
 * Browser protocol and the security analysis: docs/REMOTE.md. */
#include "sessiond.h"
#include "proto.h"
#include "rsec.h"
#include "rtc.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>

extern const char _binary_src_remote_login_html_start[], _binary_src_remote_login_html_end[];
extern const char _binary_src_remote_client_html_start[], _binary_src_remote_client_html_end[];

#define HEAD_SECONDS 15         /* TLS handshake + request head + body, from accept */
#define ALARM_SECONDS 90        /* hard limit before the bridge (covers starting a session) */
#define TOKEN_SECONDS (12 * 3600)

static SSL *ssl;
static int sock = -1;
static char peer[48];           /* address as shown */
static char peer_key[48];       /* lockout key (IPv4, or the IPv6 /64) */
static double deadline;
static int lock_fd = -1;        /* bridge → root parent: "L" = the session was locked */
static char *turn_pass;         /* read as root before the bridge drops root; the bridge's rtc.c owns it */
static int ice_lfd = -1;        /* ICE-TCP handoff: root's /run/lordos/rtc/<route>, kept by the bridge */
static int turn_has_key;        /* the TURN key a peer derived, for the next peer (the password is gone) */
static uint8_t turn_key_saved[16];
static char turn_realm_saved[256];
static char ice_route[9];
static char ice_path[64];

static double mono(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

/* A failed login is answered exactly FAIL_SECONDS after the password check began, whatever the
 * account (unknown, refused or a wrong password): the time says nothing about who exists. */
#define FAIL_SECONDS 1.25
static void sleep_until(double t) {
    struct timespec ts = { (time_t)t, (long)((t - (double)(time_t)t) * 1e9) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

/* ---- blocking TLS I/O (before the bridge) ---- */

static int wait_io(int out) {
    if (!out && SSL_pending(ssl)) return 0;
    double left = deadline - mono();
    if (left <= 0) return -1;
    struct pollfd p = { sock, (short)(out ? POLLOUT : POLLIN), 0 };
    int ms = (int)(left * 1000) + 1;
    return poll(&p, 1, ms) > 0 ? 0 : -1;
}

static int tls_write_all(const void *p, size_t n) {
    const char *c = p;
    while (n) {
        int w = SSL_write(ssl, c, n > 1u << 30 ? 1 << 30 : (int)n);
        if (w <= 0) {
            int e = SSL_get_error(ssl, w);
            if ((e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) && !wait_io(e == SSL_ERROR_WANT_WRITE)) continue;
            return -1;
        }
        c += w;
        n -= (size_t)w;
    }
    return 0;
}

/* ---- responses ---- */

/* CSP hashes of the inline <script> and <style> of each page, so nothing else can run there */
typedef struct { const char *start, *end; char script[64], style[64]; } Page;
static Page login_page = { _binary_src_remote_login_html_start, _binary_src_remote_login_html_end, "", "" };
static Page desk_page = { _binary_src_remote_client_html_start, _binary_src_remote_client_html_end, "", "" };

static void hash_block(const char *s, const char *e, const char *open, const char *close, char out[64]) {
    out[0] = 0;
    size_t ol = strlen(open);
    const char *a = NULL;
    for (const char *p = s; p + ol <= e; p++) if (!memcmp(p, open, ol)) { a = p + ol; break; }
    if (!a) return;
    size_t cl = strlen(close);
    const char *b = NULL;
    for (const char *p = a; p + cl <= e; p++) if (!memcmp(p, close, cl)) { b = p; break; }
    if (!b) return;
    unsigned char md[32];
    unsigned int ml = 0;
    if (!EVP_Digest(a, (size_t)(b - a), md, &ml, EVP_sha256(), NULL)) return;
    EVP_EncodeBlock((unsigned char *)out, md, 32);
}

static void page_hashes(Page *p) {
    if (p->script[0]) return;
    hash_block(p->start, p->end, "<script>", "</script>", p->script);
    hash_block(p->start, p->end, "<style>", "</style>", p->style);
}

static const char *status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 303: return "See Other";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 426: return "Upgrade Required";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

/* Every answer: no caching, no framing, no referrer to other sites (same-origin, not no-referrer: with
 * no-referrer browsers send "Origin: null" on our own form posts, and the Origin check needs it), no sniffing, no powerful features, and a CSP
 * that only lets the page's own inline code run (or nothing, for non-HTML answers). No HSTS: the
 * certificate is self-signed, and HSTS would make its warning impossible to get past. */
static void respond_page(int code, const char *type, const char *extra, const char *body, size_t len, const Page *pg, const char *host) {
    char csp[600];
    if (pg) {
        char wss[300] = "";
        if (rs_valid_host(host)) snprintf(wss, sizeof wss, " wss://%s", host);
        snprintf(csp, sizeof csp,
                 "default-src 'none'; script-src 'sha256-%s'; style-src 'sha256-%s'; img-src 'self' blob: data:; "
                 "connect-src 'self'%s; form-action 'self'; base-uri 'none'; frame-ancestors 'none'",
                 pg->script, pg->style, wss);
    } else snprintf(csp, sizeof csp, "default-src 'none'; frame-ancestors 'none'; form-action 'none'; base-uri 'none'");
    char head[2048];
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
        "Cache-Control: no-store, max-age=0\r\nPragma: no-cache\r\n"
        "X-Frame-Options: DENY\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: same-origin\r\n"
        "Cross-Origin-Opener-Policy: same-origin\r\nCross-Origin-Resource-Policy: same-origin\r\n"
        "Permissions-Policy: camera=(), microphone=(), geolocation=(), usb=(), serial=(), hid=(), bluetooth=(), payment=(), display-capture=()\r\n"
        "Content-Security-Policy: %s\r\n%s\r\n", code, status_text(code), type, len, csp, extra ? extra : "");
    if (n < 0 || (size_t)n >= sizeof head) return;
    tls_write_all(head, (size_t)n);
    if (len) tls_write_all(body, len);
}

static void respond(int code, const char *extra) {
    const char *t = status_text(code);
    respond_page(code, "text/plain; charset=utf-8", extra, t, strlen(t), NULL, NULL);
}

static void redirect(const char *to, const char *extra) {
    char h[700];
    snprintf(h, sizeof h, "Location: %s\r\n%s", to, extra ? extra : "");
    respond_page(303, "text/plain; charset=utf-8", h, "", 0, NULL, NULL);
}

/* The login page; with an error code e (1 wrong, 2 wait, 4 blocked) marked on <html data-e>, for the
 * answers that can't be a redirect (a blocked source's next request is refused). */
static void send_login(int code, int e, const char *host) {
    page_hashes(&login_page);
    size_t len = (size_t)(login_page.end - login_page.start);
    static const char tag[] = "<html lang=\"en\" translate=\"no\">";
    const char *at = e ? memmem(login_page.start, len, tag, sizeof tag - 1) : NULL;
    if (!at) { respond_page(code, "text/html; charset=utf-8", NULL, login_page.start, len, &login_page, host); return; }
    char mark[48];
    int ml = snprintf(mark, sizeof mark, "<html lang=\"en\" translate=\"no\" data-e=\"%d\">", e);
    char *b = malloc(len + (size_t)ml);
    if (!b) return;
    size_t pre = (size_t)(at - login_page.start);
    memcpy(b, login_page.start, pre);
    memcpy(b + pre, mark, (size_t)ml);
    memcpy(b + pre + (size_t)ml, at + sizeof tag - 1, len - pre - (sizeof tag - 1));
    respond_page(code, "text/html; charset=utf-8", NULL, b, len - (sizeof tag - 1) + (size_t)ml, &login_page, host);
    free(b);
}

/* ---- HTTP request ---- */

static char reqbuf[RS_MAX_HEAD + RS_MAX_BODY + 1];
static char *body;
static size_t body_len;

/* 0, -status to answer with, or -1 to just close */
static int read_request(RsReq *r) {
    size_t len = 0;
    int rc;
    while ((rc = rs_parse_head(reqbuf, len, r)) == RS_NEED_MORE) {
        if (len >= RS_MAX_HEAD) return -431;
        if (wait_io(0)) return len ? -408 : -1;
        int n = SSL_read(ssl, reqbuf + len, (int)(RS_MAX_HEAD - len));
        if (n <= 0) {
            int e = SSL_get_error(ssl, n);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
            return -1;
        }
        len += (size_t)n;
    }
    if (rc < 0) return rc;
    size_t cl = r->content_length > 0 ? (size_t)r->content_length : 0, have = len - r->head_len;
    if (have > cl) return -400;                 /* one request per connection: nothing after it */
    body = reqbuf + r->head_len;
    while (have < cl) {
        if (wait_io(0)) return -408;
        int n = SSL_read(ssl, body + have, (int)(cl - have));
        if (n <= 0) {
            int e = SSL_get_error(ssl, n);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
            return -1;
        }
        have += (size_t)n;
    }
    body[cl] = 0;
    body_len = cl;
    return 0;
}

/* "w=1280&h=800": a number of up to 5 digits, or def */
static int query_num(const char *q, const char *key, int def) {
    size_t kl = strlen(key);
    for (const char *p = q; *p;) {
        const char *e = strchr(p, '&');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > kl + 1 && l <= kl + 6 && !strncmp(p, key, kl) && p[kl] == '=') {
            int v = 0, ok = 1;
            for (size_t i = kl + 1; i < l; i++) { if (!isdigit((unsigned char)p[i])) { ok = 0; break; } v = v * 10 + (p[i] - '0'); }
            if (ok) return v;
        }
        p += l;
        if (*p == '&') p++;
    }
    return def;
}

/* Same origin, strictly: browsers always send Origin on a POST and on a WebSocket; a request
 * without it is not from our page. Sec-Fetch-Site, when sent, must agree. */
static int origin_ok(const RsReq *r) {
    if (!rs_same_origin(r->origin, r->host)) return 0;
    if (r->fetch_site[0] && strcmp(r->fetch_site, "same-origin") && strcmp(r->fetch_site, "none")) return 0;
    return 1;
}

/* ---- cookies (shared memory; only root processes map it) ---- */

static void sha256(const void *p, size_t n, unsigned char out[32]) {
    unsigned int l = 0;
    if (!EVP_Digest(p, n, out, &l, EVP_sha256(), NULL)) memset(out, 0, 32);
}

/* a digest of the account's shadow hash: if the password changes or gets locked, cookies end */
static void pw_digest(const char *user, unsigned char out[16]) {
    struct spwd *sp = getspnam(user);
    unsigned char md[32];
    const char *h = sp && sp->sp_pwdp ? sp->sp_pwdp : "";
    sha256(h, strlen(h), md);
    memcpy(out, md, 16);
    explicit_bzero(md, sizeof md);
}

static int new_token(uid_t uid, const char *user, char out[65]) {
    unsigned char r[32];
    if (getrandom(r, sizeof r, 0) != sizeof r) { out[0] = 0; return -1; }
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", r[i]);
    explicit_bzero(r, sizeof r);
    RdToken t = { .uid = uid, .expires = rd_now() + TOKEN_SECONDS };
    sha256(out, 64, t.hash);
    pw_digest(user, t.pwh);
    snprintf(t.user, sizeof t.user, "%.32s", user);
    double now = rd_now();
    rd_lock();
    int slot = 0;
    for (int i = 0; i < 64; i++) {
        RdToken *x = &rd_shared->tokens[i];
        if (x->expires <= now) { slot = i; break; }
        if (x->expires < rd_shared->tokens[slot].expires) slot = i;
    }
    rd_shared->tokens[slot] = t;
    rd_unlock();
    return 0;
}

static void token_drop_hash(const unsigned char h[32]) {
    rd_lock();
    for (int i = 0; i < 64; i++)
        if (!CRYPTO_memcmp(rd_shared->tokens[i].hash, h, 32)) memset(&rd_shared->tokens[i], 0, sizeof rd_shared->tokens[i]);
    rd_unlock();
}

void rd_tokens_drop_uid(uid_t uid) {
    rd_lock();
    for (int i = 0; i < 64; i++)
        if (rd_shared->tokens[i].expires > 0 && rd_shared->tokens[i].uid == uid) memset(&rd_shared->tokens[i], 0, sizeof rd_shared->tokens[i]);
    rd_unlock();
}

void rd_tokens_clear(void) {
    rd_lock();
    memset(rd_shared->tokens, 0, sizeof rd_shared->tokens);
    rd_unlock();
}

/* user from the cookie, or -1; *hash = the cookie's digest (to drop it on logout) */
static int session_user(const RsReq *r, char user[33], uid_t *uid, unsigned char hash[32]) {
    char tok[65];
    memset(hash, 0, 32);
    if (rs_cookie_token(r->cookie, RD_COOKIE, tok)) return -1;
    sha256(tok, 64, hash);
    explicit_bzero(tok, sizeof tok);
    double now = rd_now();
    int ok = -1;
    unsigned char pwh[16];
    rd_lock();
    for (int i = 0; i < 64; i++) {
        RdToken *t = &rd_shared->tokens[i];
        if (t->expires > now && !CRYPTO_memcmp(t->hash, hash, 32)) {
            snprintf(user, 33, "%s", t->user);
            *uid = t->uid;
            memcpy(pwh, t->pwh, 16);
            ok = 0;
        }
    }
    rd_unlock();
    if (ok == 0) {     /* the account may have changed since login */
        struct passwd *pw = getpwnam(user);
        unsigned char now_pwh[16];
        pw_digest(user, now_pwh);
        if (!pw || pw->pw_uid != *uid || !sd_remote_account_ok(user) || CRYPTO_memcmp(now_pwh, pwh, 16)) {
            token_drop_hash(hash);
            ok = -1;
        }
    }
    return ok;
}

#define CLEAR_COOKIES "Set-Cookie: " RD_COOKIE "=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0\r\n" \
                      "Set-Cookie: lordos_remote=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0\r\n"

/* ---- login ---- */

static void push_notice(const char *key, double until, int strikes) {
    unsigned i = rd_shared->notice_seq % RD_NOTICES;
    RdNotice *n = &rd_shared->notices[i];
    snprintf(n->key, sizeof n->key, "%s", key);
    n->until = until;
    n->at = rd_now();
    n->strikes = strikes;
    rd_shared->notice_seq++;
}

/* the recent failed sign-ins (under rd_lock): one row per source and attempted name */
static void note_failure(const char *key, const char *user) {
    double now = rd_now();
    RdFailLog *slot = NULL, *old = &rd_shared->faillog[0];
    for (int i = 0; i < RD_FAILLOG; i++) {
        RdFailLog *f = &rd_shared->faillog[i];
        if (f->count && !strcmp(f->key, key) && !strcmp(f->user, user)) { slot = f; break; }
        if (!f->count) { if (old->count) old = f; }
        else if (old->count && f->last < old->last) old = f;
    }
    if (!slot) {
        slot = old;
        memset(slot, 0, sizeof *slot);
        snprintf(slot->key, sizeof slot->key, "%s", key);
        snprintf(slot->user, sizeof slot->user, "%s", user);
        slot->first = now;
    }
    slot->count++;
    slot->last = now;
}

static void handle_login(const RsReq *r) {
    if (r->query[0]) { respond(400, NULL); return; }            /* credentials never travel in a URL */
    if (!origin_ok(r)) {
        char o[64], h[64], f[24];
        rs_log_clean(r->origin, strlen(r->origin), o, sizeof o);
        rs_log_clean(r->host, strlen(r->host), h, sizeof h);
        rs_log_clean(r->fetch_site, strlen(r->fetch_site), f, sizeof f);
        sd_log("login refused: not from this page (Origin \"%s\", Host \"%s\", Sec-Fetch-Site \"%s\")", o, h, f);
        respond(403, NULL);
        return;
    }
    if (!rs_form_type(r->ctype)) { respond(415, NULL); return; }
    rd_lock();
    double left = rl_blocked(&rd_shared->lockout, peer_key, rd_now());
    rd_unlock();
    if (left > 0) { send_login(403, 4, r->host); return; }
    if (sd_fail_wait(peer_key) > 0) { redirect("/?e=2", NULL); return; }

    char user[33], pass[RS_PASS_MAX + 1], raw[33];
    size_t plen = 0;
    int fr = rs_parse_login(body, body_len, user, pass, &plen, raw);
    explicit_bzero(reqbuf, sizeof reqbuf);
    if (fr == RS_FORM_BAD) {
        sd_log("login refused: malformed form");
        respond(400, NULL);
        return;
    }
    int ok = 0;
    char ukey[40] = "";
    double t0 = mono();
    if (fr == RS_FORM_OK) {
        snprintf(ukey, sizeof ukey, "u:%s", user);
        double d = rl_user_delay(sd_fail_count(ukey));        /* per account: slower, never locked */
        if (d > 0) sleep((unsigned)d);
        t0 = mono();
        ok = sd_check_password(user, pass, 1);
    }
    explicit_bzero(pass, sizeof pass);
    if (!ok) {
        sd_fail_note(peer_key, 1);
        if (ukey[0]) sd_fail_note(ukey, 1);
        double until = 0;
        int strikes = 0;
        rd_lock();
        int blocked = rl_fail(&rd_shared->lockout, peer_key, rd_now(), &until, &strikes);
        if (blocked) push_notice(peer_key, until, strikes);
        note_failure(peer_key, raw[0] ? raw : "?");
        rd_shared->lockout_dirty = 1;
        rd_shared->version++;
        rd_unlock();
        sd_log("failed login for \"%s\"%s", raw[0] ? raw : "?", fr == RS_FORM_USER ? " (not a valid user name)" : "");
        if (blocked) {
            sd_log("blocked %s for %.0f minutes after %d failed logins (block %d in 24 h)", peer_key, (until - rd_now()) / 60 + 0.5,
                   RL_FAILS, strikes);
            sleep_until(t0 + FAIL_SECONDS);
            send_login(403, 4, r->host);
            return;
        }
        sleep_until(t0 + FAIL_SECONDS);
        redirect("/?e=1", NULL);
        return;
    }
    rd_lock();
    rl_success(&rd_shared->lockout, peer_key);
    rd_shared->lockout_dirty = 1;
    rd_unlock();
    sd_fail_note(peer_key, 0);
    sd_fail_note(ukey, 0);
    struct passwd *pw = getpwnam(user);
    if (!pw) { respond(503, NULL); return; }
    /* a cookie the browser already had is never kept (no fixation): it ends here */
    unsigned char oldh[32];
    char olduser[33];
    uid_t olduid;
    if (session_user(r, olduser, &olduid, oldh) == 0) token_drop_hash(oldh);
    char tok[65], cookie[400];
    if (new_token(pw->pw_uid, user, tok)) { respond(503, NULL); return; }
    snprintf(cookie, sizeof cookie, "Set-Cookie: " RD_COOKIE "=%s; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=%d\r\n"
             "Set-Cookie: lordos_remote=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0\r\n", tok, TOKEN_SECONDS);
    explicit_bzero(tok, sizeof tok);
    sd_log("%s logged in", user);
    redirect("/desk", cookie);
    explicit_bzero(cookie, sizeof cookie);
}

/* ---- WebSocket ---- */

static unsigned char *wout;
static size_t wout_len, wout_off, wout_cap;
static unsigned long long bytes_rx, bytes_tx;   /* from / to the browser, reported to the root parent */

static void wput(const void *p, size_t n) {
    if (wout_len + n > wout_cap) {
        size_t cap = wout_cap ? wout_cap * 2 : 1u << 20;
        while (cap < wout_len + n) cap *= 2;
        unsigned char *nb = realloc(wout, cap);
        if (!nb) return;
        wout = nb;
        wout_cap = cap;
    }
    memcpy(wout + wout_len, p, n);
    wout_len += n;
}

static void ws_frame(int opcode, const void *a, size_t an, const void *b, size_t bn) {
    unsigned char h[10];
    size_t n = an + bn, hl;
    h[0] = (unsigned char)(0x80 | opcode);
    if (n < 126) { h[1] = (unsigned char)n; hl = 2; }
    else if (n < 65536) { h[1] = 126; h[2] = (unsigned char)(n >> 8); h[3] = (unsigned char)n; hl = 4; }
    else { h[1] = 127; for (int i = 0; i < 8; i++) h[2 + i] = (unsigned char)((uint64_t)n >> (56 - 8 * i)); hl = 10; }
    wput(h, hl);
    if (an) wput(a, an);
    if (bn) wput(b, bn);
}

static void ws_text(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ws_text(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof buf) ws_frame(1, buf, (size_t)n, NULL, 0);
}

static void ws_close(int code) {
    unsigned char c[2] = { (unsigned char)(code >> 8), (unsigned char)code };
    ws_frame(8, c, 2, NULL, 0);
}

/* text as a JSON string (for the clipboard); invalid UTF-8 from the session is not sent */
static void ws_clip(const char *t, size_t n) {
    if (n > RS_CLIP_MAX || !rs_utf8_valid((const unsigned char *)t, n)) return;
    size_t cap = n * 6 + 32, o = 0;
    char *j = malloc(cap);
    if (!j) return;
    o += (size_t)snprintf(j, cap, "{\"t\":\"clip\",\"text\":\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)t[i];
        if (c == '"' || c == '\\') { j[o++] = '\\'; j[o++] = (char)c; }
        else if (c < 0x20 || c == 0x7f) o += (size_t)snprintf(j + o, cap - o, "\\u%04x", c);
        else j[o++] = (char)c;
    }
    o += (size_t)snprintf(j + o, cap - o, "\"}");
    ws_frame(1, j, o, NULL, 0);
    free(j);
}

static int ws_flush(void) {
    while (wout_off < wout_len) {
        int w = SSL_write(ssl, wout + wout_off, (int)(wout_len - wout_off > (1u << 20) ? (1u << 20) : wout_len - wout_off));
        if (w <= 0) {
            int e = SSL_get_error(ssl, w);
            if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) return 0;
            return -1;
        }
        wout_off += (size_t)w;
        bytes_tx += (unsigned long long)w;
    }
    wout_off = wout_len = 0;
    return 0;
}

static int ws_handshake(const RsReq *r) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char k[128];
    snprintf(k, sizeof k, "%s%s", r->wskey, guid);
    unsigned char sha[20];
    unsigned int sl = 0;
    if (!EVP_Digest(k, strlen(k), sha, &sl, EVP_sha1(), NULL)) return -1;
    unsigned char acc[64];
    EVP_EncodeBlock(acc, sha, 20);
    char head[512];
    int n = snprintf(head, sizeof head, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n", acc);
    return tls_write_all(head, (size_t)n);
}

/* ---- bridge (runs as the user) ---- */

static int sess = -1;                  /* session socket */
static int vmode;                      /* the video plays: the lossless refresh goes over the data channel */
static int tiles_lost;                 /* a refresh tile didn't fit the data channel: ask for them all again */
static double tiles_lost_at;
static Rtc *rtc;
static int view_hz;                 /* the browser screen's refresh rate (resize "hz"), 0 unknown */
static void video_want(void);
static const char *rtc_dead;           /* RTC_WHY_* of a peer that just failed */
static int resets;               /* peers made after the first; the current one is number resets + 1 */
static double reset_at;
static int W, H;
static int locked, closing;

static void sess_send(int type, const void *a, size_t an) {
    unsigned char h[5];
    uint32_t len = (uint32_t)(1 + an);
    memcpy(h, &len, 4);
    h[4] = (unsigned char)type;
    if (write(sess, h, 5) != 5) return;
    if (an && write(sess, a, an) != (ssize_t)an) return;
}

/* A lossless tile over the "tiles" data channel: [1][x, y, w, h u16 BE][png]; halved until it fits.
 * px: the rectangle's own pixels (stride w), straight from the session's message. */
static void dc_tile(const uint32_t *px, int stride, int x, int y, int w, int h) {
    int len = 0;
    unsigned char *img = rd_encode_png(px, stride, w, h, &len);
    if (!img) return;
    if (len > 250000 && h > 1) {
        free(img);
        dc_tile(px, stride, x, y, w, h / 2);
        dc_tile(px + (size_t)(h / 2) * stride, stride, x, y + h / 2, w, h - h / 2);
        return;
    }
    unsigned char *m = malloc((size_t)len + 9);
    if (m) {
        unsigned char hd[9] = { 1, (unsigned char)(x >> 8), (unsigned char)x, (unsigned char)(y >> 8), (unsigned char)y,
                                (unsigned char)(w >> 8), (unsigned char)w, (unsigned char)(h >> 8), (unsigned char)h };
        memcpy(m, hd, 9);
        memcpy(m + 9, img, (size_t)len);
        if (rtc_send(rtc, RTC_CH_TILES, m, (size_t)len + 9, 1)) { tiles_lost = 1; tiles_lost_at = rd_now(); }
        free(m);
    }
    free(img);
}

/* Session message → browser. */
static void from_session(int type, const unsigned char *d, uint32_t n) {
    switch (type) {
    case RP_SIZE:
        if (n < 4) break;
        {
            uint16_t s[2];
            memcpy(s, d, 4);
            if (!s[0] || !s[1] || s[0] > 8192 || s[1] > 8192) break;
            W = s[0];
            H = s[1];
            ws_text("{\"t\":\"size\",\"w\":%d,\"h\":%d}", W, H);
        }
        break;
    case RP_RECT: {
        /* the session sends rectangles only to a viewer whose video plays, once the screen is still:
         * the lossless refresh over the video (mirror.c) */
        if (n < 8 || !vmode || !rtc) break;
        uint16_t r[4];
        memcpy(r, d, 8);
        if (!r[2] || !r[3] || (uint32_t)r[0] + r[2] > (uint32_t)W || (uint32_t)r[1] + r[3] > (uint32_t)H || n < 8 + (uint32_t)r[2] * r[3] * 4) break;
        uint32_t *px = malloc((size_t)r[2] * r[3] * 4);              /* aligned copy of the message's pixels */
        if (!px) break;
        memcpy(px, d + 8, (size_t)r[2] * r[3] * 4);
        dc_tile(px, r[2], r[0], r[1], r[2], r[3]);
        free(px);
        break;
    }
    case RP_END:
        if (vmode && rtc) {             /* the data channel is reliable: no browser ack needed */
            unsigned char e = 3;
            rtc_send(rtc, RTC_CH_TILES, &e, 1, 1);
        }
        sess_send(RP_READY, NULL, 0);
        break;
    case RP_VFRAME:
        if (n > 8 && rtc && d[0] == rtc_codec(rtc)) {
            uint32_t ms;
            memcpy(&ms, d + 4, 4);
            rtc_video(rtc, d + 8, n - 8, ms);
        }
        break;
    case RP_AFRAME:
        if (n > 4 && n <= 4 + 1500 && rtc) {
            uint32_t smp;
            memcpy(&smp, d, 4);
            rtc_audio(rtc, d + 4, n - 4, smp);
        }
        break;
    case RP_VINFO:
        /* u8 hardware encoder, u8 GPU composition, u16 fps, u16 1/100 ms per frame: numbers only, to the page */
        if (n >= 6) {
            uint16_t f, e;
            memcpy(&f, d + 2, 2);
            memcpy(&e, d + 4, 2);
            ws_text("{\"t\":\"vinfo\",\"hw\":%d,\"gpu\":%d,\"fps\":%u,\"ms\":%u.%02u}", d[0] != 0, d[1] != 0, (unsigned)f, e / 100u, e % 100u);
        }
        break;
    case RP_DAMAGE:
        /* [2][u16 BE x, y, w, h …]: the browser drops its lossless layer there */
        if (vmode && rtc && n >= 8 && n % 8 == 0 && n <= 8 * 256) {
            unsigned char m[1 + 8 * 256];
            m[0] = 2;
            for (uint32_t i = 0; i < n / 2; i++) {
                uint16_t v;
                memcpy(&v, d + i * 2, 2);
                m[1 + i * 2] = (unsigned char)(v >> 8);
                m[2 + i * 2] = (unsigned char)v;
            }
            rtc_send(rtc, RTC_CH_TILES, m, 1 + n, 1);
        }
        break;
    case RP_CURSOR:
        if (n >= 5) {
            int16_t x, y;
            memcpy(&x, d, 2);
            memcpy(&y, d + 2, 2);
            ws_text("{\"t\":\"cur\",\"x\":%d,\"y\":%d,\"m\":%d}", x, y, d[4] > 2 ? 0 : d[4]);
        }
        break;
    case RP_SHAPE:
        if (n >= 8) {
            uint16_t s[4];
            memcpy(s, d, 8);
            if (!s[0] || !s[1] || s[0] > 512 || s[1] > 512 || n < 8 + (uint32_t)s[0] * s[1] * 4) break;
            uint32_t *px = malloc((size_t)s[0] * s[1] * 4);
            if (!px) break;
            memcpy(px, d + 8, (size_t)s[0] * s[1] * 4);
            int len = 0;
            unsigned char *png = rd_encode_rgba(px, s[0], s[1], &len);
            free(px);
            if (png) {
                unsigned char hd[9] = { 2, (unsigned char)(s[0] >> 8), (unsigned char)s[0], (unsigned char)(s[1] >> 8), (unsigned char)s[1],
                                        (unsigned char)(s[2] >> 8), (unsigned char)s[2], (unsigned char)(s[3] >> 8), (unsigned char)s[3] };
                ws_frame(2, hd, 9, png, (size_t)len);
                free(png);
            }
        }
        break;
    case RP_CLIP: ws_clip((const char *)d, n); break;
    case RP_SESSION:
        /* the user locked their session: every browser of this user has to log in again */
        if (n >= 10 && memmem(d, n, "\"lock\"", 6)) {
            if (lock_fd >= 0 && write(lock_fd, "L\n", 2) != 2) { /* the root parent is gone: the cookie expires anyway */ }
            ws_text("{\"t\":\"bye\",\"lock\":1}");
            locked = 1;
        }
        break;
    default: break;
    }
}

static void video_reset(const char *code, const char *why);
/* The browser says the video plays: the lossless refresh of still areas starts (RP_VMODE). */
static void video_mode(void) {
    if (!rtc || !rtc_media_ready(rtc) || !rtc_dc_open(rtc, RTC_CH_TILES) || vmode) return;
    vmode = 1;
    unsigned char v = 1;
    sess_send(RP_VMODE, &v, 1);
    sd_log("video: playing in the browser (%s)", rtc_path_kind(rtc));
}

/* One validated browser message → session. From a data channel (dc) only the input messages count. */
static void from_browser(const char *j, size_t n, int dc) {
    char *sb = malloc(n + 1);
    if (!sb) return;
    RsJson o;
    RsInput in;
    if (rs_json_parse(j, n, &o, sb, n + 1) == 0) {
        int type = rs_browser_input(&o, W, H, &in);
        if (dc && (type == RS_IN_PING || type == RS_IN_RTC || type == RS_IN_ICE || type == RS_IN_VMODE)) type = RS_IN_NONE;
        switch (type) {
        case RS_IN_PTR: { int16_t p[2] = { (int16_t)in.a, (int16_t)in.b }; sess_send(RP_PTR, p, 4); break; }
        case RS_IN_BTN: { unsigned char p[2] = { (unsigned char)in.a, (unsigned char)in.b }; sess_send(RP_BTN, p, 2); break; }
        case RS_IN_WHEEL: { signed char d = (signed char)in.a; sess_send(RP_WHEEL, &d, 1); break; }
        case RS_IN_SCROLL:
        case RS_IN_PINCH: {
            unsigned char p[9];
            int32_t w[2] = { in.v1, in.v2 };
            p[0] = (unsigned char)in.a;
            memcpy(p + 1, w, 8);
            sess_send(in.type == RS_IN_SCROLL ? RP_SCROLL : RP_PINCH, p, 9);
            break;
        }
        case RS_IN_KEY: {
            unsigned char p[3];
            uint16_t c = (uint16_t)in.a;
            memcpy(p, &c, 2);
            p[2] = (unsigned char)in.b;
            sess_send(RP_KEY, p, 3);
            break;
        }
        case RS_IN_RESIZE: {
            uint16_t sz[2] = { (uint16_t)in.a, (uint16_t)in.b };
            sess_send(RP_RESIZE, sz, 4);
            if (in.v1 && in.v1 != view_hz) { view_hz = in.v1; if (rtc && rtc_media_ready(rtc)) video_want(); }
            break;
        }
        case RS_IN_PING: ws_text("{\"t\":\"pong\",\"ts\":%lld}", in.ts); break;
        case RS_IN_CLIP: sess_send(RP_SETCLIP, in.text, in.tlen); break;
        case RS_IN_RTC:
            /* an offer for an older peer of this connection (sent before the page learned of the new
             * one): never the new peer's first offer */
            if (rtc && in.a && in.a != resets + 1) { sd_log("video: ignored an offer for an older peer"); break; }
            if (rtc) {
                int first = rtc_codec(rtc) == 0, e = rtc_offer(rtc, in.text, in.tlen, 1);
                if (e < 0) video_reset(e == RW_ECODEC ? "codec" : first ? "offer" : "restart",
                                       e == RW_ECODEC ? "the browser offers neither H.264 nor VP8" : first ? "the offer was refused" : "the ICE restart was refused");
            }
            break;
        case RS_IN_ICE: if (rtc && (!in.a || in.a == resets + 1)) rtc_candidate(rtc, in.text, in.tlen); break;
        case RS_IN_VMODE: if (in.a) video_mode(); else video_reset("browser", "the browser closed this peer"); break;
        default: break;
        }
    }
    explicit_bzero(sb, n + 1);
    free(sb);
}

static void on_ws(void *ud, int op, unsigned char *data, size_t len) {
    (void)ud;
    if (closing) return;
    if (op == 1) from_browser((const char *)data, len, 0);
    else if (op == 9) ws_frame(10, data, len, NULL, 0);
    else if (op == 8) { ws_close(1000); closing = 1; }
    else if (op == 2) { ws_close(1003); closing = 1; }        /* our page never sends binary */
}

/* Reads frames from the browser (rsec.c). Returns -1 if the connection is over. */
static unsigned char *win;
static size_t win_len, win_cap;
static RsWs wsr;

static int ws_read(void) {
    for (;;) {
        if (win_cap - win_len < 16384) {
            size_t cap = win_cap ? win_cap * 2 : 65536;
            if (cap > RS_WS_MAX_MSG + 65536) return -1;
            unsigned char *nb = realloc(win, cap);
            if (!nb) return -1;
            win = nb;
            win_cap = cap;
        }
        int r = SSL_read(ssl, win + win_len, (int)(win_cap - win_len));
        if (r <= 0) {
            int e = SSL_get_error(ssl, r);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) break;
            return -1;
        }
        win_len += (size_t)r;
        bytes_rx += (unsigned long long)r;
    }
    size_t used = 0;
    int rc = rs_ws_consume(&wsr, win, win_len, &used, on_ws, NULL);
    memmove(win, win + used, win_len - used);
    win_len -= used;
    if (rc) { ws_close(rc); closing = 1; }
    return 0;
}

static void conn_register(const char *user) {
    rd_lock();
    for (int i = 0; i < RD_MAX_CONNS; i++)
        if (!rd_shared->conns[i].pid) {
            rd_shared->conns[i] = (RdConn){ .pid = getpid(), .since = rd_now() };
            snprintf(rd_shared->conns[i].ip, 48, "%s", peer);
            snprintf(rd_shared->conns[i].user, 33, "%s", user);
            rd_shared->version++;
            break;
        }
    rd_unlock();
}

static int conns_full(void) {
    int n = 0;
    rd_lock();
    for (int i = 0; i < RD_MAX_CONNS; i++) if (rd_shared->conns[i].pid) n++;
    rd_unlock();
    return n >= RD_MAX_CONNS;
}

/* this viewer takes the session's encoded video: u8 codec, u8 sound, u16 frames a second (the browser
 * screen's refresh; 0 unknown), u32 kbit/s */
static void video_want(void) {
    unsigned char v[8] = { (unsigned char)rtc_codec(rtc), 1, 0, 0 };
    uint16_t hz = (uint16_t)view_hz;
    uint32_t k = 4000;
    memcpy(v + 2, &hz, 2);
    memcpy(v + 4, &k, 4);
    sess_send(RP_VIDEO, v, 8);
}
static void rtc_signal(void *ud, const char *json) { (void)ud; ws_frame(1, json, strlen(json), NULL, 0); }
static void rtc_state(void *ud, int st, const char *why) {
    (void)ud;
    if (st == RTC_CONNECTED) {
        video_want();
    } else if (st == RTC_FAILED) rtc_dead = why ? why : RTC_WHY_SERVER;
}
static void rtc_message(void *ud, int chan, const uint8_t *p, size_t n, int binary) {
    (void)ud;
    if (chan == RTC_CH_INPUT && !binary && rs_utf8_valid(p, n)) from_browser((const char *)p, n, 1);
}
static void rtc_key(void *ud) { (void)ud; sess_send(RP_KEYFRAME, NULL, 0); }
static void rtc_rate(void *ud, unsigned kbps) { (void)ud; uint32_t k = kbps; sess_send(RP_RATE, &k, 4); }

/* The ICE servers the page may use: our own STUN server (this connection's UDP port, at the address
 * the page used) and the external ones from Settings. The TURN relay is never given to the browser. */
static void ice_list(char *ice, size_t cap) {
    char ip[64] = "";
    size_t o = 0;
    RwAddr la;
    struct sockaddr_storage ls;
    socklen_t ll = sizeof ls;
    if (!getsockname(sock, (struct sockaddr *)&ls, &ll)) {
        if (ls.ss_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)&ls)->sin_addr, ip, sizeof ip);
        else if (ls.ss_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ls)->sin6_addr, ip, sizeof ip);
    }
    if (!strncmp(ip, "::ffff:", 7)) memmove(ip, ip + 7, strlen(ip + 7) + 1);
    int v6 = !rw_addr_text(ip, &la) && la.family == 6;
    o += (size_t)snprintf(ice + o, cap - o, "[\"stun:%s%s%s:%d\"", v6 ? "[" : "", ip, v6 ? "]" : "", rtc_port(rtc));
    for (int i = 0; i < rd_conf.nstun && o < cap - 120; i++) o += (size_t)snprintf(ice + o, cap - o, ",\"stun:%s\"", rd_conf.stun[i]);
    snprintf(ice + o, cap - o, "]");
}

/* The WebRTC peer, before the hello and after each failed one: its UDP port is also the browser's
 * STUN server. The TURN password is only there for the first one (rtc.c wipes it), so a later peer
 * has no relay unless the bridge kept its key: it doesn't, by design. */
static int rtc_start(void) {
    RtcConfig c;
    memset(&c, 0, sizeof c);
    c.access = &rd_conf.access;
    c.net_mode = rd_conf.net_mode;
    rw_addr_text(peer, &c.ws_peer);
    struct sockaddr_storage ls;
    socklen_t ll = sizeof ls;
    if (!getsockname(sock, (struct sockaddr *)&ls, &ll)) {
        char t[64] = "";
        if (ls.ss_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)&ls)->sin_addr, t, sizeof t);
        else if (ls.ss_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ls)->sin6_addr, t, sizeof t);
        rw_addr_text(t, &c.ws_local);
    }
    c.nstun = rd_conf.nstun;
    for (int i = 0; i < rd_conf.nstun; i++) snprintf(c.stun[i], sizeof c.stun[i], "%s", rd_conf.stun[i]);
    snprintf(c.turn, sizeof c.turn, "%s", rd_conf.turn);
    snprintf(c.turn_user, sizeof c.turn_user, "%s", rd_conf.turn_user);
    c.turn_transport = rd_conf.turn_transport;
    c.turn_has_fp = rd_conf.turn_has_fp;
    memcpy(c.turn_fp, rd_conf.turn_fp, 32);
    c.turn_pass = turn_pass;                /* rtc.c owns it now and wipes it */
    turn_pass = NULL;
    if (!c.turn_pass && turn_has_key) {
        c.turn_has_key = 1;
        memcpy(c.turn_key, turn_key_saved, 16);
        memcpy(c.turn_realm, turn_realm_saved, sizeof c.turn_realm);
    }
    c.max_kbps = 12000;
    c.ice_lfd = ice_lfd;
    memcpy(c.route, ice_route, sizeof c.route);
    c.tcp_port = rd_conf.port;
    c.peer_no = resets + 1;
    RtcCallbacks cb = { rtc_signal, rtc_state, rtc_message, rtc_key, rtc_rate, NULL };
    rtc = rtc_new(&c, &cb);
    return rtc ? 0 : -1;
}

/* This peer is over (it failed, its offer was refused, or the browser closed it): the session stops
 * sending to it, the browser is told why, and a new peer waits for the browser's next offer. The page
 * decides whether to try again (docs/REMOTE.md, "Reconnecting"); there is nothing to fall back to. */
static void video_reset(const char *code, const char *why) {
    char st[160];
    if (rtc) rtc_stats(rtc, st, sizeof st); else snprintf(st, sizeof st, "-");
    sd_log("video: peer closed (%s; %s)", why, st);
    if (vmode) { unsigned char v = 0; sess_send(RP_VMODE, &v, 1); vmode = 0; }
    unsigned char v[8] = { 0 };
    sess_send(RP_VIDEO, v, 8);
    if (!rtc_turn_key(rtc, turn_key_saved, turn_realm_saved)) turn_has_key = 1;
    rtc_free(rtc);
    rtc = NULL;
    tiles_lost = 0;
    /* a new peer at once, but not more often than every 2 s and at most 60 per connection */
    double now = rd_now();
    if (++resets > 60) { ws_text("{\"t\":\"rtc\",\"fail\":\"%s\"}", code); ws_text("{\"t\":\"bye\"}"); closing = 1; return; }
    if (now - reset_at < 2) { reset_at = now + 2; ws_text("{\"t\":\"rtc\",\"fail\":\"%s\"}", code); return; }
    reset_at = now;
    char ice[600];
    if (rtc_start()) { ws_text("{\"t\":\"rtc\",\"fail\":\"%s\",\"why2\":\"server\"}", code); return; }
    ice_list(ice, sizeof ice);
    ws_text("{\"t\":\"rtc\",\"fail\":\"%s\",\"ready\":1,\"p\":%d,\"ice\":%s}", code, resets + 1, ice);
}

static void bridge(const struct passwd *pw) {
    char hello[96];
    snprintf(hello, sizeof hello, "%s %s", peer, pw->pw_name);
    sess_send(RP_HELLO, hello, strlen(hello));
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
    /* the WebRTC peer is the only way the screen reaches the page: without it, the page says why */
    if (rtc_start() == 0) {
        char ice[600];
        ice_list(ice, sizeof ice);
        ws_text("{\"t\":\"hello\",\"user\":\"%s\",\"rtc\":1,\"p\":1,\"ice\":%s}", pw->pw_name, ice);
    } else ws_text("{\"t\":\"hello\",\"user\":\"%s\",\"rtc\":0}", pw->pw_name);
    rs_ws_init(&wsr, RS_WS_MAX_MSG);
    unsigned char *sin = NULL;
    size_t sin_len = 0, sin_cap = 0;
    double last_input = rd_now(), last_report = 0;
    for (;;) {
        if (ws_flush()) return;
        if (lock_fd >= 0 && rd_now() - last_report >= 2) {      /* traffic, for Settings and Monitor */
            char line[64];
            int n = snprintf(line, sizeof line, "T %llu %llu\n", bytes_rx, bytes_tx);
            if (write(lock_fd, line, (size_t)n) != n) { /* parent gone */ }
            last_report = rd_now();
        }
        if (closing) {                         /* our close frame went out (or will not): done */
            for (int i = 0; i < 20 && wout_len; i++) { struct pollfd q = { sock, POLLOUT, 0 }; poll(&q, 1, 100); if (ws_flush()) break; }
            return;
        }
        struct pollfd p[2 + RTC_MAX_FDS] = { { sock, (short)(POLLIN | (wout_len ? POLLOUT : 0)), 0 }, { sess, POLLIN, 0 } };
        int np = 2;
        /* while there is something to send, nothing more is read from the session (flow control) */
        if (wout_len > (4u << 20)) p[1].events = 0;
        int timeout = 15000;
        if (rtc) {
            np += rtc_pollfds(rtc, p + 2, RTC_MAX_FDS);
            int t = rtc_timeout(rtc);
            if (t >= 0 && t < timeout) timeout = t;
        } else if (reset_at > rd_now()) timeout = 200;
        if (tiles_lost && timeout > 500) timeout = 500;
        int r = SSL_pending(ssl) ? 1 : poll(p, (nfds_t)np, timeout);
        if (r < 0 && errno != EINTR) return;
        if (SSL_pending(ssl) || (p[0].revents & POLLIN)) {
            if (ws_read()) return;
            last_input = rd_now();
        }
        if (p[0].revents & (POLLERR | POLLHUP)) return;
        if (p[1].revents & (POLLERR | POLLHUP)) { ws_text("{\"t\":\"bye\"}"); ws_flush(); return; }
        if (p[1].revents & POLLIN) {
            for (;;) {
                if (sin_cap - sin_len < 262144) {
                    size_t cap = sin_cap ? sin_cap * 2 : 1u << 20;
                    if (cap > RP_MAX_MSG + (1u << 20)) return;
                    unsigned char *nb = realloc(sin, cap);
                    if (!nb) return;
                    sin = nb;
                    sin_cap = cap;
                }
                ssize_t n = recv(sess, sin + sin_len, sin_cap - sin_len, MSG_DONTWAIT);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && errno == EAGAIN) break;
                if (n <= 0) { ws_text("{\"t\":\"bye\"}"); ws_flush(); return; }
                sin_len += (size_t)n;
            }
            size_t off = 0;
            while (sin_len - off >= 5) {
                uint32_t len;
                memcpy(&len, sin + off, 4);
                if (!len || len > RP_MAX_MSG) return;
                if (sin_len - off < 4 + (size_t)len) break;
                from_session(sin[off + 4], sin + off + 5, len - 1);
                off += 4 + len;
            }
            memmove(sin, sin + off, sin_len - off);
            sin_len -= off;
        }
        if (rtc) {
            if (np > 2) rtc_dispatch(rtc, p + 2, np - 2);
            if (rtc) rtc_tick(rtc);
            if (rtc_dead) { const char *why = rtc_dead; rtc_dead = NULL; video_reset(why, "the connection failed"); }
            /* tiles that didn't fit: the whole lossless layer again, at most every 2 s */
            if (rtc && vmode && tiles_lost && rd_now() - tiles_lost_at > 2) {
                tiles_lost = 0;
                unsigned char v = 1;
                sess_send(RP_VMODE, &v, 1);
            }
        } else if (reset_at && rd_now() >= reset_at && !closing) {     /* a reset that had to wait: the new peer */
            reset_at = rd_now();
            char ice[600];
            if (rtc_start() == 0) { ice_list(ice, sizeof ice); ws_text("{\"t\":\"rtc\",\"ready\":1,\"p\":%d,\"ice\":%s}", resets + 1, ice); }
            else { ws_text("{\"t\":\"rtc\",\"fail\":\"server\"}"); reset_at = 0; }
        }
        if (locked) { ws_flush(); return; }
        if (rd_now() - last_input > 60) return;     /* the browser sends a ping every 2 s */
    }
}

/* ---- privilege drop (in the bridge process) ---- */

static void close_all_but(const int *keep, int nkeep) {
    int fds[1024], n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        struct dirent *e;
        int dfd = dirfd(d);
        while ((e = readdir(d)) && n < 1024) {
            char *end;
            long fd = strtol(e->d_name, &end, 10);
            if (*end || fd < 3 || fd == dfd) continue;
            int k = 0;
            for (int i = 0; i < nkeep; i++) if (keep[i] == fd) k = 1;
            if (!k) fds[n++] = (int)fd;
        }
        closedir(d);
    }
    for (int i = 0; i < n; i++) close(fds[i]);
}

/* For good: no shared state, no descriptor but the three it needs, no environment, the user's
 * groups and ids (real, effective and saved), checked; no new privileges, no core dumps, and it dies
 * with its root parent. */
static int drop_root(const struct passwd *pw, pid_t parent) {
    if (rd_shared) munmap(rd_shared, sizeof *rd_shared);
    rd_shared = NULL;
    int keep[4] = { sock, sess, lock_fd, ice_lfd };
    close_all_but(keep, 4);
    clearenv();
    struct rlimit core = { 0, 0 };
    setrlimit(RLIMIT_CORE, &core);
    if (pw->pw_uid == 0 || pw->pw_gid == 0) return -1;
    if (initgroups(pw->pw_name, pw->pw_gid) || setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
        setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid)) return -1;
    uid_t ru, eu, su;
    gid_t rg, eg, sg;
    if (getresuid(&ru, &eu, &su) || getresgid(&rg, &eg, &sg)) return -1;
    if (ru != pw->pw_uid || eu != pw->pw_uid || su != pw->pw_uid || rg != pw->pw_gid || eg != pw->pw_gid || sg != pw->pw_gid) return -1;
    if (setuid(0) == 0 || seteuid(0) == 0 || setgid(0) == 0 || setegid(0) == 0) return -1;
    gid_t groups[256];
    int ng = getgroups(256, groups);
    for (int i = 0; i < ng; i++) if (groups[i] == 0) return -1;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_DUMPABLE, 0, 0, 0, 0)) return -1;
    if (prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0) || getppid() != parent) return -1;
    return 0;
}

/* ---- ICE over TCP (docs/REMOTE.md, "ICE over TCP") ---- */

/* As root, before the bridge forks: a listening socket in root's /run/lordos/rtc (0700) named after
 * this connection's random route. Only root can connect to it: the connection children below. */
static void ice_listen(void) {
    struct stat st;
    mkdir(SD_RUN_DIR, 0755);
    mkdir(RD_RTC_DIR, 0700);
    if (lstat(RD_RTC_DIR, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 077)) return;
    static const char al[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    unsigned char b[8];
    if (getrandom(b, 8, 0) != 8) return;
    for (int i = 0; i < 8; i++) ice_route[i] = al[b[i] % (sizeof al - 1)];
    ice_route[8] = 0;
    snprintf(ice_path, sizeof ice_path, RD_RTC_DIR "/%s", ice_route);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", ice_path);
    unlink(ice_path);
    mode_t old = umask(077);
    int ok = fd >= 0 && !bind(fd, (struct sockaddr *)&a, sizeof a) && !listen(fd, 8);
    umask(old);
    if (!ok) { if (fd >= 0) close(fd); unlink(ice_path); ice_path[0] = 0; ice_route[0] = 0; return; }
    ice_lfd = fd;
}

static void ice_unlisten(void) {
    if (ice_lfd >= 0) close(ice_lfd);
    ice_lfd = -1;
    if (ice_path[0]) unlink(ice_path);
    ice_path[0] = 0;
}

static int read_full(int fd, unsigned char *b, size_t n) {
    size_t got = 0;
    while (got < n) {
        double left = deadline - mono();
        struct pollfd p = { fd, POLLIN, 0 };
        if (left <= 0 || poll(&p, 1, (int)(left * 1000) + 1) <= 0) return -1;
        ssize_t k = recv(fd, b + got, n - got, MSG_DONTWAIT);
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (k <= 0) return -1;
        got += (size_t)k;
    }
    return 0;
}

/* A connection to the remote desktop port that starts with an RFC 4571 frame instead of a TLS
 * ClientHello: the browser's ICE-TCP check. This root child reads exactly that first frame, takes the
 * route from its user name (rtcwire.c rw_ice_route, nothing else is trusted here) and hands the
 * socket and the frame to that bridge, which checks the message's integrity with its ICE password.
 * The address already passed the same rules as any other connection (sessiond.c accept_remote). */
static void ice_tcp(int fd) {
    unsigned char b[2 + RW_TCP_FRAME_MAX];
    size_t len = 0;
    if (read_full(fd, b, 2) || rw_tcp_frame(b, 2 + RW_TCP_FRAME_MAX, &len) < 0) return;
    len = (size_t)(b[0] << 8 | b[1]);
    if (len < 20 || len > 1500 || read_full(fd, b + 2, len)) return;
    char route[9], path[64];
    if (rw_ice_route(b + 2, len, route)) return;
    snprintf(path, sizeof path, RD_RTC_DIR "/%s", route);
    struct stat st;
    if (lstat(path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != 0) return;
    int u = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (u < 0 || connect(u, (struct sockaddr *)&a, sizeof a)) { if (u >= 0) close(u); return; }
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof cbuf);
    struct iovec iov = { b, 2 + len };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    if (sendmsg(u, &mh, MSG_NOSIGNAL) < 0) sd_log("ICE-TCP: could not hand the connection over");
    close(u);
}

/* ---- one connection ---- */

static void handle_ws(const RsReq *r, int logged, const char *user, uid_t uid) {
    if (!logged) { respond(403, NULL); return; }
    if (!origin_ok(r) || !rs_has_token(r->upgrade, "websocket") || !rs_has_token(r->connection, "upgrade") || !rs_valid_wskey(r->wskey)) {
        respond(400, NULL);
        return;
    }
    if (strcmp(r->wsversion, "13")) { respond(426, "Sec-WebSocket-Version: 13\r\n"); return; }
    if (conns_full()) { respond(503, NULL); return; }
    struct passwd *pw = getpwnam(user);
    if (!pw || pw->pw_uid != uid) { respond(403, NULL); return; }
    struct passwd me = *pw;
    static char name[33], dir[512];
    snprintf(name, sizeof name, "%s", pw->pw_name);
    snprintf(dir, sizeof dir, "%s", pw->pw_dir);
    me.pw_name = name;
    me.pw_dir = dir;
    int w = query_num(r->query, "w", 1280), h = query_num(r->query, "h", 800);
    if (w < 800) w = 800;
    if (h < 600) h = 600;
    if (w > 2560) w = 2560;
    if (h > 1600) h = 1600;
    sess = sd_session_get(&me, w & ~1, h & ~1);
    if (sess < 0) { respond(503, NULL); return; }
    /* the TURN password, read here as root (RD_TURN_KEY is root 0600) and handed to the bridge's WebRTC
     * peer in memory only; never logged, never sent to the browser */
    if (rd_conf.turn[0] && rd_conf.turn_user[0]) {
        int f = open(RD_TURN_KEY, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat st;
        char buf[160];
        ssize_t got = -1;
        if (f >= 0 && !fstat(f, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 077) && st.st_size < (off_t)sizeof buf)
            got = read(f, buf, sizeof buf - 1);
        if (f >= 0) close(f);
        if (got > 0) {
            buf[got] = 0;
            buf[strcspn(buf, "\n")] = 0;
            if (rw_turn_pass_valid(buf)) turn_pass = strdup(buf);
        }
        explicit_bzero(buf, sizeof buf);
    }
    ice_listen();
    if (ws_handshake(r)) { ice_unlisten(); return; }
    conn_register(name);
    int pp[2];
    if (pipe2(pp, O_CLOEXEC)) return;
    pid_t parent = getpid();
    alarm(0);
    pid_t b = fork();
    if (b < 0) { ice_unlisten(); return; }
    if (b == 0) {
        close(pp[0]);
        lock_fd = pp[1];
        /* from here on, no root: TLS, compression and bridge as the user */
        if (drop_root(&me, parent)) { sd_log("could not drop privileges"); _exit(1); }
        sd_log("connected to %s's session", name);
        bridge(&me);
        if (rtc) {                          /* how the video went, for the log (docs/REMOTE.md) */
            char st[160];
            rtc_stats(rtc, st, sizeof st);
            sd_log("disconnected (video: %s)", st);
        } else sd_log("disconnected");
        _exit(0);
    }
    /* the root side keeps nothing of the connection: it only listens for "the session was locked" */
    if (turn_pass) { explicit_bzero(turn_pass, strlen(turn_pass)); free(turn_pass); turn_pass = NULL; }
    if (ice_lfd >= 0) close(ice_lfd);        /* the bridge has it; the path goes when the bridge does */
    ice_lfd = -1;
    close(pp[1]);
    close(sess);
    close(sock);
    /* lines from the bridge (not trusted beyond its own counters): "L" the session was locked,
     * "T <rx> <tx>" its traffic */
    char line[80];
    size_t ll = 0;
    for (;;) {
        char c;
        ssize_t n = read(pp[0], &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (c != '\n') { if (ll < sizeof line - 1) line[ll++] = c; else ll = sizeof line; continue; }
        if (ll >= sizeof line) { ll = 0; continue; }
        line[ll] = 0;
        ll = 0;
        unsigned long long rx, tx;
        char extra;
        if (!strcmp(line, "L")) { rd_tokens_drop_uid(me.pw_uid); sd_log("%s locked the session: every remote cookie of that user ended", name); }
        else if (sscanf(line, "T %llu %llu %c", &rx, &tx, &extra) == 2) {
            rd_lock();
            for (int i = 0; i < RD_MAX_CONNS; i++)
                if (rd_shared->conns[i].pid == getpid()) { rd_shared->conns[i].rx = rx; rd_shared->conns[i].tx = tx; rd_shared->version++; }
            rd_unlock();
        }
    }
    while (waitpid(b, NULL, 0) < 0 && errno == EINTR) {}
    ice_unlisten();
}

void rd_serve(SSL_CTX *ctx, int fd, const char *ip, const char *key) {
    sock = fd;
    snprintf(peer, sizeof peer, "%s", ip);
    snprintf(peer_key, sizeof peer_key, "%s", key);
    char prefix[96];
    snprintf(prefix, sizeof prefix, "lord-sessiond[%d] %s", (int)getpid(), peer);
    sd_log_prefix(prefix);
    alarm(ALARM_SECONDS);                       /* backstop: SIGALRM ends a stuck connection */
    deadline = mono() + HEAD_SECONDS;
    /* TLS starts with a handshake record (0x16); an ICE-TCP check with its 16-bit length (< 0x10) */
    for (;;) {
        unsigned char first = 0;
        struct pollfd p = { fd, POLLIN, 0 };
        double left = deadline - mono();
        if (left <= 0 || poll(&p, 1, (int)(left * 1000) + 1) <= 0) return;
        ssize_t k = recv(fd, &first, 1, MSG_PEEK | MSG_DONTWAIT);
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (k <= 0) return;
        if (first < 0x10) { ice_tcp(fd); return; }
        break;
    }
    ssl = SSL_new(ctx);
    if (!ssl) return;
    SSL_set_fd(ssl, fd);
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    for (;;) {                                  /* handshake, under the same deadline */
        int a = SSL_accept(ssl);
        if (a == 1) break;
        int e = SSL_get_error(ssl, a);
        if ((e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) && !wait_io(e == SSL_ERROR_WANT_WRITE)) continue;
        return;
    }
    RsReq r;
    int rc = read_request(&r);
    if (rc == -1) return;
    if (rc < 0) {
        if (rc != -408) sd_log("bad request (%d)", -rc);
        respond(-rc, NULL);
        return;
    }
    deadline = mono() + 30;                     /* answering (session start is covered by the alarm) */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);

    int get = !strcmp(r.method, "GET"), post = !strcmp(r.method, "POST");
    if (!post && r.content_length > 0) { respond(400, NULL); return; }
    char user[33] = "";
    uid_t uid = 0;
    unsigned char hash[32];
    int logged = session_user(&r, user, &uid, hash) == 0;

    if (!strcmp(r.path, "/login")) {
        if (!post) { respond(405, "Allow: POST\r\n"); return; }
        handle_login(&r);
        return;
    }
    if (!strcmp(r.path, "/logout")) {
        if (!post) { respond(405, "Allow: POST\r\n"); return; }
        if (!origin_ok(&r)) { respond(403, NULL); return; }
        if (logged) { token_drop_hash(hash); sd_log("%s logged out", user); }
        redirect("/", CLEAR_COOKIES);
        return;
    }
    if (!strcmp(r.path, "/") || !strcmp(r.path, "/index.html")) {
        if (!get) { respond(405, "Allow: GET\r\n"); return; }
        if (logged) redirect("/desk", NULL); else send_login(200, 0, r.host);
        return;
    }
    if (!strcmp(r.path, "/bg.jpg")) {             /* the login screen background (links to Crystal Crown) */
        if (!get) { respond(405, "Allow: GET\r\n"); return; }
        char p[600];
        snprintf(p, sizeof p, "%s", rd_conf.greeter_theme);
        char *slash = strrchr(p, '/');
        if (slash) snprintf(slash, sizeof p - (size_t)(slash - p), "/assets/background.jpg");
        int f = open(p, O_RDONLY | O_CLOEXEC);     /* a fixed path in root's theme folder (a link to the wallpaper) */
        struct stat st;
        char *img = NULL;
        size_t got = 0;
        if (f >= 0 && !fstat(f, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < (8 << 20) && (img = malloc((size_t)st.st_size))) {
            while (got < (size_t)st.st_size) {
                ssize_t n = read(f, img + got, (size_t)st.st_size - got);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                got += (size_t)n;
            }
        }
        if (img && got == (size_t)st.st_size) respond_page(200, "image/jpeg", NULL, img, got, NULL, NULL);
        else respond(404, NULL);
        free(img);
        if (f >= 0) close(f);
        return;
    }
    if (!strcmp(r.path, "/desk")) {
        if (!get) { respond(405, "Allow: GET\r\n"); return; }
        if (!logged) { redirect("/", NULL); return; }
        page_hashes(&desk_page);
        respond_page(200, "text/html; charset=utf-8", NULL, desk_page.start, (size_t)(desk_page.end - desk_page.start), &desk_page, r.host);
        return;
    }
    if (!strcmp(r.path, "/ws")) {
        if (!get) { respond(405, "Allow: GET\r\n"); return; }
        handle_ws(&r, logged, user, uid);
        return;
    }
    respond(404, NULL);
}
