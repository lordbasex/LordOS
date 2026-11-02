/* rtc.c — the remote desktop's WebRTC peer (docs/REMOTE.md, "Video (WebRTC)").
 *
 * It lives in the bridge, the process that already dropped root for good (conn.c): everything here
 * runs as the user who signed in. Debian 13 packages no WebRTC library we could use (no
 * libdatachannel, no libjuice), so this is our own small peer on top of what Debian does ship:
 *   - ICE-lite (RFC 8445 §2.5): the browser is the controlling agent and checks us; we only answer
 *     checks signed with our ICE password, from addresses the remote desktop's rules allow. A second
 *     UDP socket is a plain STUN server for this connection only (the browser is told to use it), so a
 *     LAN works with no outside server. It has to be another port: browsers hand every STUN packet
 *     from their STUN server's address to the server's own request handler, ICE answers included.
 *   - ICE over TCP (RFC 6544): passive candidates on the remote desktop's own TCP port, for networks
 *     that drop UDP. lord-sessiond's root connection child reads the first framed check of such a
 *     connection, picks the bridge by the first 8 characters of its user name (our "route") and hands
 *     the socket over; everything after that, the check's integrity included, is ours.
 *   - ICE restarts: a later offer brings new credentials for both sides over the same DTLS association,
 *     so a changed or interrupted network path recovers without a new sign-in or a new peer.
 *   - DTLS 1.2 with OpenSSL (lord-sessiond already links it), we are the server; the browser's
 *     certificate must hash to the fingerprint in the offer that came over the authenticated
 *     WebSocket, or the connection ends.
 *   - SRTP with libsrtp2 (keys exported from DTLS), RTCP feedback (NACK, PLI, FIR, receiver reports).
 *   - The data channels over SCTP with usrsctp (no threads: its timers run from rtc_tick).
 *   - Optional external STUN (our public address) and TURN (a relayed candidate; the relay only ever
 *     sees DTLS and SRTP), reached over UDP, TCP or TLS (checked against the system's CAs and the
 *     server's name, or a pinned fingerprint). The TURN password never leaves this process and is never
 *     logged.
 * Every packet goes through the strict parsers in rtcwire.c first. */
#include "rtc.h"
#include "sessiond.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <srtp2/srtp.h>
#include <usrsctp.h>

#define MTU 1100                /* RTP payload: fits a 1200-byte path with SRTP and a TURN header */
#define HIST 1024               /* sent video packets kept for retransmission (NACK) */
#define MAX_PATHS 8
#define MAX_DC_MSG 262144       /* what our answer advertises (a=max-message-size) */
#define CONSENT_MS 45000        /* no valid check for this long: the browser is gone (restarts can't help) */
#define CONNECT_MS 20000        /* from an offer (or a restart) until DTLS is up */
#define ANSWER_WAIT_MS 1200     /* for the external STUN/TURN answers before sending ours */
#define ANSWER_WAIT_TCP_MS 3000 /* the same with a TURN server over TCP or TLS (connect, handshake) */
#define MAX_TCP 4               /* ICE-TCP connections per peer */
#define TCP_CHECK_MS 10000      /* a new ICE-TCP connection must bring a valid check by then */
#define TCP_OUT_MAX (1u << 20)  /* queued bytes per stream; past it a packet is dropped (media is lossy) */

typedef struct { uint16_t seq, len; uint8_t b[MTU + 64]; } Hist;
typedef struct {
    uint32_t ssrc, ts_off, last_ts, packets, octets;
    uint32_t last_in;           /* the encoder's last time (ms or samples), to keep the clock going forward */
    int have_in;
    uint16_t seq;
    int pt;
    double last_ms;
    Hist *hist;
} Track;
/* tcp: 0 for UDP, else the ICE-TCP connection's slot + 1 */
typedef struct { RwAddr a; int relay, tcp, valid, nominated, proven; double last; } Path;
/* A byte stream we write packets to without blocking: an ICE-TCP connection or the TURN server's */
typedef struct { uint8_t *b; size_t len, off, cap; } OutQ;
typedef struct {
    int fd;
    RwAddr peer;
    uint8_t in[RW_TCP_FRAME_MAX + 2];
    size_t inl;
    OutQ out;
    double born;
    int checked;                /* a valid ICE check came over it */
} TcpConn;
typedef struct {
    int state;                  /* 0 off, 1 asked, 2 asked with credentials, 3 allocated, -1 failed */
    RwAddr srv, relayed, mapped;
    uint8_t tid[12], key[16];
    char realm[256];
    uint8_t nonce[256];
    size_t nlen;
    double refresh_at, perm_at, sent_at;
    int tries;
    RwAddr perm[8];
    int nperm, perm_dirty;
    /* over TCP or TLS: the stream (fd), its TLS, whether it is connected and handshaken */
    int transport, fd, up, connecting;
    char host[96];
    SSL_CTX *tctx;
    SSL *tssl;
    uint8_t in[RW_STUN_MAX];
    size_t inl;
    OutQ out;
    double started;
} Turn;

struct Rtc {
    RtcCallbacks cb;
    RtcConfig cfg;
    int fd, port, sfd, sport;          /* media socket; the built-in STUN server's socket */
    char ufrag[13], pwd[25];           /* ufrag: the route (8) + 4 random, new at each ICE restart */
    TcpConn tcp[MAX_TCP];
    unsigned restarts;
    unsigned long checks;              /* valid ICE checks since the last offer */
    /* DTLS */
    SSL_CTX *ctx;
    SSL *ssl;
    BIO *rbio;
    EVP_PKEY *key;
    X509 *cert;
    uint8_t fp[32];
    int dtls_up;
    /* the browser */
    RwOffer *of;
    int offered, codec, answered;
    double answer_due;
    RwAddr rcand[16];
    int nrcand;
    Path path[MAX_PATHS];
    int npath, sel;
    /* SRTP */
    srtp_t tx, rx;
    int srtp_up;
    /* SCTP and the data channels */
    struct socket *sctp;
    int sctp_on, dc_sid[2], dc_open[2];
    uint8_t *rasm;              /* a message being reassembled */
    size_t rasm_len;
    int rasm_sid, rasm_ppid;
    double sctp_t;
    Track v, a;
    /* congestion: the viewer's target */
    unsigned kbps, remb;        /* remb: the browser's last estimate (kbit/s) */
    double rate_at, kf_at, lost, congested_at;
    unsigned long nacks, plis, resent, dtls_rx;
    double last_sr, created, last_consent, stun_win;
    int stun_n, failed;
    /* external STUN, TURN */
    RwAddr stun_srv[RTC_MAX_STUN];
    uint8_t stun_tid[RTC_MAX_STUN][12];
    int nstun_srv;
    RwAddr srflx;
    Turn turn;
};

static int srtp_ready, sctp_ready, gcm_ok = -1;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void rnd(void *p, size_t n) { if (getrandom(p, n, 0) != (ssize_t)n) memset(p, 0x5a, n); }

static void rnd_text(char *out, size_t n) {
    static const char al[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    uint8_t b[64];
    rnd(b, n);
    for (size_t i = 0; i < n; i++) out[i] = al[b[i] % (sizeof al - 1)];
    out[n] = 0;
}

/* code: RTC_WHY_* for the browser; why: the journal's line */
static void fail(Rtc *r, const char *code, const char *why) {
    if (r->failed) return;
    r->failed = 1;
    sd_log("video: %s", why);
    if (r->cb.state) r->cb.state(r->cb.ud, RTC_FAILED, code);
}

static void hmac_sha1(const uint8_t *key, size_t klen, const uint8_t *msg, size_t n, uint8_t out[20]) {
    unsigned int l = 20;
    if (!HMAC(EVP_sha1(), key, (int)klen, msg, n, out, &l)) memset(out, 0, 20);
}

/* ---- addresses ---- */

static void to_sockaddr(const RwAddr *a, struct sockaddr_in6 *s) {
    memset(s, 0, sizeof *s);
    s->sin6_family = AF_INET6;
    s->sin6_port = htons(a->port);
    if (a->family == 4) { s->sin6_addr.s6_addr[10] = 0xff; s->sin6_addr.s6_addr[11] = 0xff; memcpy(s->sin6_addr.s6_addr + 12, a->a, 4); }
    else memcpy(s->sin6_addr.s6_addr, a->a, 16);
}

static void from_sockaddr(const struct sockaddr_in6 *s, RwAddr *a) {
    static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    memset(a, 0, sizeof *a);
    a->port = ntohs(s->sin6_port);
    if (!memcmp(s->sin6_addr.s6_addr, mapped, 12)) { a->family = 4; memcpy(a->a, s->sin6_addr.s6_addr + 12, 4); }
    else { a->family = 6; memcpy(a->a, s->sin6_addr.s6_addr, 16); }
}

static int same_ip(const RwAddr *x, const RwAddr *y) { return x->family == y->family && !memcmp(x->a, y->a, x->family == 4 ? 4 : 16); }

/* host:port (a name, an IPv4 or [IPv6]) → address; the names configured in Settings are resolved
 * here, as the user, once per connection. */
static int resolve(const char *hp, RwAddr *out) {
    char host[96];
    const char *colon = strrchr(hp, ':');
    if (!colon || colon == hp || strlen(hp) >= sizeof host) return -1;
    size_t hl = (size_t)(colon - hp);
    memcpy(host, hp, hl);
    host[hl] = 0;
    if (host[0] == '[' && hl > 2 && host[hl - 1] == ']') { memmove(host, host + 1, hl - 2); host[hl - 2] = 0; }
    long port = strtol(colon + 1, NULL, 10);
    if (port < 1 || port > 65535) return -1;
    if (!rw_addr_text(host, out)) { out->port = (uint16_t)port; return 0; }
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM }, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) || !res) return -1;
    int ok = -1;
    for (struct addrinfo *i = res; i && ok; i = i->ai_next) {
        if (i->ai_family == AF_INET) { out->family = 4; memset(out->a, 0, 16); memcpy(out->a, &((struct sockaddr_in *)i->ai_addr)->sin_addr, 4); ok = 0; }
        else if (i->ai_family == AF_INET6) { from_sockaddr((struct sockaddr_in6 *)i->ai_addr, out); ok = 0; }
    }
    freeaddrinfo(res);
    out->port = (uint16_t)port;
    return ok;
}

/* The same rules as the WebSocket: the signed-in browser's own address, or what ipnet.c allows
 * (a manual block always wins). Through our TURN relay: anyone not blocked (the admin chose to add a
 * relay; the WebSocket itself still had to get in). */
static int src_allowed(const Rtc *r, const RwAddr *a, int relay) {
    char t[64];
    RsAddr x;
    rw_addr_format(a, t, sizeof t);
    if (rs_addr_parse(t, &x)) return 0;
    if (!r->cfg.access) return 0;
    if (relay) return rs_access_decide(r->cfg.access, RS_NET_ANY, &x, 0) == RS_ACC_OK;
    if (same_ip(a, &r->cfg.ws_peer)) return rs_access_decide(r->cfg.access, RS_NET_ANY, &x, 0) == RS_ACC_OK;
    return rs_access_decide(r->cfg.access, r->cfg.net_mode, &x, 0) == RS_ACC_OK;
}

/* ---- sending ---- */

static void raw_send(Rtc *r, const RwAddr *to, const void *p, size_t n) {
    struct sockaddr_in6 s;
    to_sockaddr(to, &s);
    if (sendto(r->fd, p, n, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&s, sizeof s) < 0 && errno != EAGAIN) {}
}

/* ---- byte streams (ICE-TCP, TURN over TCP/TLS) ---- */

/* Queues a whole packet (with an optional 2-byte length header) or drops it: never half of one. */
static int q_put(OutQ *q, const void *hdr, size_t hl, const void *p, size_t n) {
    if (q->len - q->off + hl + n > TCP_OUT_MAX) return -1;
    if (q->off && q->off == q->len) q->off = q->len = 0;
    if (q->len + hl + n > q->cap) {
        if (q->off) { memmove(q->b, q->b + q->off, q->len - q->off); q->len -= q->off; q->off = 0; }
        size_t cap = q->cap ? q->cap : 16384;
        while (cap < q->len + hl + n) cap *= 2;
        if (cap > q->cap) {
            uint8_t *nb = realloc(q->b, cap);
            if (!nb) return -1;
            q->b = nb;
            q->cap = cap;
        }
    }
    if (hl) memcpy(q->b + q->len, hdr, hl);
    memcpy(q->b + q->len + hl, p, n);
    q->len += hl + n;
    return 0;
}
static void q_free(OutQ *q) { free(q->b); memset(q, 0, sizeof *q); }

static void tcp_close(Rtc *r, int i, const char *why);

static void tcp_flush(Rtc *r, int i) {
    TcpConn *c = &r->tcp[i];
    while (c->fd >= 0 && c->out.off < c->out.len) {
        ssize_t w = send(c->fd, c->out.b + c->out.off, c->out.len - c->out.off, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (w <= 0) { tcp_close(r, i, "write failed"); return; }
        c->out.off += (size_t)w;
    }
}

/* One packet over an ICE-TCP connection, framed (RFC 4571). */
static void tcp_send(Rtc *r, int slot, const void *p, size_t n) {
    if (slot < 1 || slot > MAX_TCP || n > RW_TCP_FRAME_MAX) return;
    TcpConn *c = &r->tcp[slot - 1];
    if (c->fd < 0) return;
    uint8_t h[2] = { (uint8_t)(n >> 8), (uint8_t)n };
    if (q_put(&c->out, h, 2, p, n)) return;
    tcp_flush(r, slot - 1);
}

static void turn_flush(Rtc *r) {
    Turn *t = &r->turn;
    while (t->up && t->out.off < t->out.len) {
        size_t left = t->out.len - t->out.off;
        if (t->tssl) {
            int w = SSL_write(t->tssl, t->out.b + t->out.off, left > (1u << 20) ? 1 << 20 : (int)left);
            if (w <= 0) {
                int e = SSL_get_error(t->tssl, w);
                if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) return;
                ERR_clear_error();
                t->up = 0;
                t->state = -1;
                sd_log("video: the TURN server's TLS connection broke");
                return;
            }
            t->out.off += (size_t)w;
        } else {
            ssize_t w = send(t->fd, t->out.b + t->out.off, left, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (w <= 0) { t->up = 0; t->state = -1; sd_log("video: the TURN server's TCP connection broke"); return; }
            t->out.off += (size_t)w;
        }
    }
}

/* A STUN message to the TURN server: a datagram, or on its stream (queued until it's connected). */
static void turn_raw(Rtc *r, const void *p, size_t n) {
    Turn *t = &r->turn;
    if (t->transport == RW_TURN_UDP) { raw_send(r, &t->srv, p, n); return; }
    if (t->fd < 0 || q_put(&t->out, NULL, 0, p, n)) return;
    turn_flush(r);
}

/* To a peer: directly (UDP or an ICE-TCP connection) or wrapped in a TURN Send indication. */
static void send_to(Rtc *r, const RwAddr *to, int relay, int tcp, const void *p, size_t n) {
    if (tcp) { tcp_send(r, tcp, p, n); return; }
    if (!relay) { raw_send(r, to, p, n); return; }
    if (r->turn.state != 3 || n > 1400) return;
    static uint8_t b[1500];
    uint8_t tid[12];
    rnd(tid, 12);
    RwStunOut hdr;
    rw_so_init(&hdr, RW_TURN_SEND_IND, tid);
    rw_so_xaddr(&hdr, RW_ATTR_XOR_PEER, to);
    if (hdr.bad) return;
    size_t o = hdr.n, padded = (n + 3) & ~(size_t)3;
    if (o + 4 + padded > sizeof b) return;
    memcpy(b, hdr.b, o);
    b[o] = 0; b[o + 1] = 0x13; b[o + 2] = (uint8_t)(n >> 8); b[o + 3] = (uint8_t)n;
    memcpy(b + o + 4, p, n);
    memset(b + o + 4 + n, 0, padded - n);
    size_t total = o + 4 + padded;
    b[2] = (uint8_t)((total - 20) >> 8);
    b[3] = (uint8_t)(total - 20);
    turn_raw(r, b, total);
}

static Path *sel_path(Rtc *r) { return r->sel >= 0 ? &r->path[r->sel] : NULL; }

static void send_sel(Rtc *r, const void *p, size_t n) {
    Path *s = sel_path(r);
    if (s) send_to(r, &s->a, s->relay, s->tcp, p, n);
}

/* ---- DTLS: our certificate, a write BIO that sends each record as a datagram ---- */

static int bio_write(BIO *b, const char *p, int n) {
    Rtc *r = BIO_get_data(b);
    if (r && n > 0) send_sel(r, p, (size_t)n);
    return n;
}
static long bio_ctrl(BIO *b, int cmd, long num, void *ptr) {
    (void)b; (void)num; (void)ptr;
    switch (cmd) {
    case BIO_CTRL_FLUSH: return 1;
    case BIO_CTRL_DGRAM_QUERY_MTU: return 1200;
    case BIO_CTRL_DGRAM_GET_MTU_OVERHEAD: return 0;
    case BIO_CTRL_WPENDING: case BIO_CTRL_PENDING: return 0;
    default: return 0;
    }
}
static int bio_create(BIO *b) { BIO_set_init(b, 1); return 1; }
static BIO_METHOD *bio_method(void) {
    static BIO_METHOD *m;
    if (!m) {
        m = BIO_meth_new(BIO_TYPE_SOURCE_SINK | BIO_get_new_index(), "lordos-dtls");
        BIO_meth_set_write(m, bio_write);
        BIO_meth_set_ctrl(m, bio_ctrl);
        BIO_meth_set_create(m, bio_create);
    }
    return m;
}

static int accept_any(int ok, X509_STORE_CTX *c) { (void)ok; (void)c; return 1; }     /* checked by fingerprint */

static int make_cert(Rtc *r) {
    r->key = EVP_EC_gen("P-256");
    r->cert = X509_new();
    if (!r->key || !r->cert) return -1;
    uint8_t serial[8];
    rnd(serial, sizeof serial);
    serial[0] &= 0x7f;
    BIGNUM *bn = BN_bin2bn(serial, sizeof serial, NULL);
    ASN1_INTEGER *ai = bn ? BN_to_ASN1_INTEGER(bn, NULL) : NULL;
    BN_free(bn);
    if (!ai) return -1;
    X509_set_serialNumber(r->cert, ai);
    ASN1_INTEGER_free(ai);
    X509_set_version(r->cert, 2);
    X509_gmtime_adj(X509_getm_notBefore(r->cert), -86400);
    X509_gmtime_adj(X509_getm_notAfter(r->cert), 30L * 86400);
    X509_NAME *nm = X509_get_subject_name(r->cert);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC, (const unsigned char *)"lordos", -1, -1, 0);
    X509_set_issuer_name(r->cert, nm);
    X509_set_pubkey(r->cert, r->key);
    if (!X509_sign(r->cert, r->key, EVP_sha256())) return -1;
    unsigned int l = 32;
    if (!X509_digest(r->cert, EVP_sha256(), r->fp, &l) || l != 32) return -1;
    return 0;
}

/* libsrtp2 has AES-GCM only when built with a crypto library; Debian's is (OpenSSL), but check. */
static int srtp_gcm_works(void) {
    if (gcm_ok >= 0) return gcm_ok;
    srtp_policy_t p;
    memset(&p, 0, sizeof p);
    srtp_crypto_policy_set_aes_gcm_128_16_auth(&p.rtp);
    srtp_crypto_policy_set_aes_gcm_128_16_auth(&p.rtcp);
    uint8_t key[28] = { 1 };
    p.key = key;
    p.ssrc.type = ssrc_any_outbound;
    srtp_t s;
    gcm_ok = srtp_create(&s, &p) == srtp_err_status_ok;
    if (gcm_ok) srtp_dealloc(s);
    return gcm_ok;
}

static int dtls_init(Rtc *r) {
    if (make_cert(r)) return -1;
    r->ctx = SSL_CTX_new(DTLS_server_method());
    if (!r->ctx) return -1;
    SSL_CTX_set_min_proto_version(r->ctx, DTLS1_2_VERSION);
    SSL_CTX_set_options(r->ctx, SSL_OP_NO_QUERY_MTU | SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION | SSL_OP_NO_TICKET);
    if (SSL_CTX_set_cipher_list(r->ctx, "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305") != 1) return -1;
    SSL_CTX_set1_groups_list(r->ctx, "X25519:P-256:P-384");
    if (SSL_CTX_use_certificate(r->ctx, r->cert) != 1 || SSL_CTX_use_PrivateKey(r->ctx, r->key) != 1) return -1;
    SSL_CTX_set_verify(r->ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, accept_any);
    /* returns 0 on success */
    if (SSL_CTX_set_tlsext_use_srtp(r->ctx, srtp_gcm_works() ? "SRTP_AEAD_AES_128_GCM:SRTP_AES128_CM_SHA1_80" : "SRTP_AES128_CM_SHA1_80")) return -1;
    r->ssl = SSL_new(r->ctx);
    if (!r->ssl) return -1;
    r->rbio = BIO_new(BIO_s_mem());
    BIO *w = BIO_new(bio_method());
    if (!r->rbio || !w) return -1;
    BIO_set_mem_eof_return(r->rbio, -1);
    BIO_set_data(w, r);
    SSL_set_bio(r->ssl, r->rbio, w);
    SSL_set_mtu(r->ssl, 1200);
    DTLS_set_link_mtu(r->ssl, 1200);
    SSL_set_accept_state(r->ssl);
    return 0;
}

/* ---- SCTP and the data channels ---- */

static int sctp_out(void *addr, void *buf, size_t len, uint8_t tos, uint8_t set_df) {
    (void)tos; (void)set_df;
    Rtc *r = addr;
    if (r && r->dtls_up && !r->failed && len <= 1400) SSL_write(r->ssl, buf, (int)len);
    return 0;
}

static int dc_raw(Rtc *r, int sid, uint32_t ppid, const void *p, size_t n) {
    if (!r->sctp) return -1;
    struct sctp_sndinfo si;
    memset(&si, 0, sizeof si);
    si.snd_sid = (uint16_t)sid;
    si.snd_flags = SCTP_EOR;
    si.snd_ppid = htonl(ppid);
    static const uint8_t zero = 0;
    if (!n) { p = &zero; n = 1; ppid = ppid == 51 ? 56 : 57; si.snd_ppid = htonl(ppid); }
    ssize_t w = usrsctp_sendv(r->sctp, p, n, NULL, 0, &si, sizeof si, SCTP_SENDV_SNDINFO, 0);
    return w < 0 ? -1 : 0;
}

static void on_dc_message(Rtc *r, int sid, int ppid, const uint8_t *d, size_t n) {
    if (ppid == 50) {                                              /* DCEP */
        RwDcOpen op;
        if (n >= 1 && d[0] == 0x02) return;                       /* an ACK (we never open channels) */
        if (rw_dcep_open(d, n, &op)) { sd_log("video: malformed data channel open"); return; }
        int ch = !strcmp(op.label, "input") ? RTC_CH_INPUT : !strcmp(op.label, "tiles") ? RTC_CH_TILES : -1;
        if (ch < 0 || r->dc_open[ch]) { sd_log("video: refused data channel \"%s\"", op.label); return; }
        r->dc_sid[ch] = sid;
        r->dc_open[ch] = 1;
        uint8_t ack = 0x02;
        dc_raw(r, sid, 50, &ack, 1);
        if (r->cb.state) r->cb.state(r->cb.ud, RTC_DC_OPEN, op.label);
        return;
    }
    int ch = -1;
    for (int i = 0; i < 2; i++) if (r->dc_open[i] && r->dc_sid[i] == sid) ch = i;
    if (ch < 0) return;
    if (ppid == 56 || ppid == 57) { n = 0; ppid = ppid == 56 ? 51 : 53; }
    if (ppid != 51 && ppid != 53) return;
    if (r->cb.message) r->cb.message(r->cb.ud, ch, d, n, ppid == 53);
}

static int sctp_recv(struct socket *sock, union sctp_sockstore addr, void *data, size_t len, struct sctp_rcvinfo rcv, int flags, void *ulp) {
    (void)sock; (void)addr;
    Rtc *r = ulp;
    if (!data) return 1;
    if (!r || r->failed) { free(data); return 1; }
    if (flags & MSG_NOTIFICATION) {
        const union sctp_notification *nt = data;
        if (len >= sizeof nt->sn_assoc_change && nt->sn_header.sn_type == SCTP_ASSOC_CHANGE) {
            int st = nt->sn_assoc_change.sac_state;
            if (st == SCTP_COMM_LOST || st == SCTP_SHUTDOWN_COMP) fail(r, RTC_WHY_CLOSED, "the browser closed the data channels");
        }
        free(data);
        return 1;
    }
    int sid = rcv.rcv_sid, ppid = (int)ntohl(rcv.rcv_ppid);
    /* a message larger than the receive buffer comes in parts: joined up to our limit */
    if (r->rasm_len && (r->rasm_sid != sid || r->rasm_ppid != ppid)) { free(r->rasm); r->rasm = NULL; r->rasm_len = 0; }
    if (!(flags & MSG_EOR) || r->rasm_len) {
        if (r->rasm_len + len > MAX_DC_MSG) { free(data); fail(r, RTC_WHY_CLOSED, "data channel message too large"); return 1; }
        uint8_t *nb = realloc(r->rasm, r->rasm_len + len);
        if (!nb) { free(data); fail(r, RTC_WHY_SERVER, "out of memory"); return 1; }
        memcpy(nb + r->rasm_len, data, len);
        r->rasm = nb;
        r->rasm_len += len;
        r->rasm_sid = sid;
        r->rasm_ppid = ppid;
        free(data);
        if (!(flags & MSG_EOR)) return 1;
        on_dc_message(r, sid, ppid, r->rasm, r->rasm_len);
        free(r->rasm);
        r->rasm = NULL;
        r->rasm_len = 0;
        return 1;
    }
    if (len > MAX_DC_MSG) { free(data); fail(r, RTC_WHY_CLOSED, "data channel message too large"); return 1; }
    on_dc_message(r, sid, ppid, data, len);
    free(data);
    return 1;
}

static void sctp_start(Rtc *r) {
    if (!sctp_ready) {
        usrsctp_init_nothreads(0, sctp_out, NULL);
        usrsctp_sysctl_set_sctp_ecn_enable(0);
        usrsctp_sysctl_set_sctp_max_chunks_on_queue(10240);
        sctp_ready = 1;
    }
    usrsctp_register_address(r);
    r->sctp = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, sctp_recv, NULL, 0, r);
    if (!r->sctp) { fail(r, RTC_WHY_SERVER, "no SCTP socket"); return; }
    usrsctp_set_non_blocking(r->sctp, 1);
    struct linger lg = { 1, 0 };
    usrsctp_setsockopt(r->sctp, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    int on = 1;
    usrsctp_setsockopt(r->sctp, IPPROTO_SCTP, SCTP_NODELAY, &on, sizeof on);
    usrsctp_setsockopt(r->sctp, IPPROTO_SCTP, SCTP_RECVRCVINFO, &on, sizeof on);
    int buf = 1 << 20;
    usrsctp_setsockopt(r->sctp, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
    usrsctp_setsockopt(r->sctp, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    struct sctp_initmsg im = { .sinit_num_ostreams = 1024, .sinit_max_instreams = 1024 };
    usrsctp_setsockopt(r->sctp, IPPROTO_SCTP, SCTP_INITMSG, &im, sizeof im);
    struct sctp_assoc_value av = { .assoc_id = SCTP_ALL_ASSOC, .assoc_value = SCTP_ENABLE_RESET_STREAM_REQ };
    usrsctp_setsockopt(r->sctp, IPPROTO_SCTP, SCTP_ENABLE_STREAM_RESET, &av, sizeof av);
    struct sctp_event ev = { .se_assoc_id = SCTP_ALL_ASSOC, .se_on = 1, .se_type = SCTP_ASSOC_CHANGE };
    usrsctp_setsockopt(r->sctp, IPPROTO_SCTP, SCTP_EVENT, &ev, sizeof ev);
    struct sockaddr_conn sc;
    memset(&sc, 0, sizeof sc);
    sc.sconn_family = AF_CONN;
    sc.sconn_port = htons(5000);
    sc.sconn_addr = r;
    if (usrsctp_bind(r->sctp, (struct sockaddr *)&sc, sizeof sc) < 0) { fail(r, RTC_WHY_SERVER, "SCTP bind failed"); return; }
    int dm = r->of ? r->of->dm : -1;
    sc.sconn_port = htons((uint16_t)(dm >= 0 ? r->of->m[dm].sctp_port : 5000));
    if (usrsctp_connect(r->sctp, (struct sockaddr *)&sc, sizeof sc) < 0 && errno != EINPROGRESS) { fail(r, RTC_WHY_SERVER, "SCTP connect failed"); return; }
    r->sctp_on = 1;
    r->sctp_t = now_ms();
}

/* ---- DTLS up: the fingerprint, SRTP keys, then SCTP ---- */

static void dtls_up(Rtc *r) {
    X509 *peer = SSL_get1_peer_certificate(r->ssl);
    uint8_t fp[32];
    unsigned int l = 32;
    int ok = peer && X509_digest(peer, EVP_sha256(), fp, &l) && l == 32 && !CRYPTO_memcmp(fp, r->of->fp, 32);
    X509_free(peer);
    if (!ok) { fail(r, RTC_WHY_DTLS, "the browser's DTLS certificate does not match the fingerprint of the signed-in offer"); return; }
    const SRTP_PROTECTION_PROFILE *pr = SSL_get_selected_srtp_profile(r->ssl);
    if (!pr || (pr->id != SRTP_AES128_CM_SHA1_80 && pr->id != SRTP_AEAD_AES_128_GCM)) { fail(r, RTC_WHY_DTLS, "no SRTP profile"); return; }
    int gcm = pr->id == SRTP_AEAD_AES_128_GCM;
    size_t kl = 16, sl = gcm ? 12 : 14;
    uint8_t km[2 * (16 + 14)], txk[30], rxk[30];
    if (SSL_export_keying_material(r->ssl, km, 2 * (kl + sl), "EXTRACTOR-dtls_srtp", 19, NULL, 0, 0) != 1) { fail(r, RTC_WHY_DTLS, "no SRTP keys"); return; }
    /* client key, server key, client salt, server salt; we are the server */
    memcpy(txk, km + kl, kl);
    memcpy(txk + kl, km + 2 * kl + sl, sl);
    memcpy(rxk, km, kl);
    memcpy(rxk + kl, km + 2 * kl, sl);
    explicit_bzero(km, sizeof km);
    srtp_policy_t p;
    memset(&p, 0, sizeof p);
    if (gcm) { srtp_crypto_policy_set_aes_gcm_128_16_auth(&p.rtp); srtp_crypto_policy_set_aes_gcm_128_16_auth(&p.rtcp); }
    else { srtp_crypto_policy_set_rtp_default(&p.rtp); srtp_crypto_policy_set_rtcp_default(&p.rtcp); }
    p.window_size = 1024;
    p.allow_repeat_tx = 1;
    p.key = txk;
    p.ssrc.type = ssrc_any_outbound;
    int e1 = srtp_create(&r->tx, &p);
    p.key = rxk;
    p.ssrc.type = ssrc_any_inbound;
    int e2 = srtp_create(&r->rx, &p);
    explicit_bzero(txk, sizeof txk);
    explicit_bzero(rxk, sizeof rxk);
    if (e1 || e2) { fail(r, RTC_WHY_SERVER, "SRTP setup failed"); return; }
    r->srtp_up = 1;
    r->dtls_up = 1;
    Path *s = sel_path(r);
    if (s) s->proven = 1;
    char a[64] = "";
    if (s) rw_addr_format(&s->a, a, sizeof a);
    sd_log("video: connected (%s, %s, %s%s)", r->codec == RW_CODEC_H264 ? "H.264" : "VP8", gcm ? "AES-GCM" : "AES-CM", a,
           s && s->relay ? " via TURN" : s && s->tcp ? " over TCP" : "");
    if (r->of->dm >= 0) sctp_start(r);
    if (r->cb.state) r->cb.state(r->cb.ud, RTC_CONNECTED, NULL);
}

static void dtls_input(Rtc *r, const uint8_t *p, size_t n) {
    BIO_write(r->rbio, p, (int)n);
    if (!r->dtls_up) {
        int k = SSL_do_handshake(r->ssl);
        if (k == 1) dtls_up(r);
        else {
            int e = SSL_get_error(r->ssl, k);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) { ERR_clear_error(); fail(r, RTC_WHY_DTLS, "DTLS handshake failed"); return; }
        }
    }
    static uint8_t buf[65536];
    while (r->dtls_up && !r->failed) {
        int k = SSL_read(r->ssl, buf, sizeof buf);
        if (k > 0) { r->dtls_rx++; if (r->sctp_on) usrsctp_conninput(r, buf, (size_t)k, 0); continue; }
        int e = SSL_get_error(r->ssl, k);
        if (e == SSL_ERROR_ZERO_RETURN) fail(r, RTC_WHY_CLOSED, "the browser closed DTLS");
        else if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) { ERR_clear_error(); fail(r, RTC_WHY_DTLS, "DTLS error"); }
        break;
    }
}

/* ---- RTP out ---- */

typedef struct { Rtc *r; Track *t; uint32_t ts; } PktCtx;

static void rtp_out(void *ud, const uint8_t *hdr, size_t hl, const uint8_t *data, size_t n, int marker) {
    PktCtx *c = ud;
    Rtc *r = c->r;
    Track *t = c->t;
    uint8_t b[MTU + 64 + SRTP_MAX_TRAILER_LEN];
    if (12 + hl + n > MTU + 64) return;
    b[0] = 0x80;
    b[1] = (uint8_t)((marker ? 0x80 : 0) | t->pt);
    b[2] = (uint8_t)(t->seq >> 8); b[3] = (uint8_t)t->seq;
    uint32_t ts = c->ts;
    b[4] = (uint8_t)(ts >> 24); b[5] = (uint8_t)(ts >> 16); b[6] = (uint8_t)(ts >> 8); b[7] = (uint8_t)ts;
    b[8] = (uint8_t)(t->ssrc >> 24); b[9] = (uint8_t)(t->ssrc >> 16); b[10] = (uint8_t)(t->ssrc >> 8); b[11] = (uint8_t)t->ssrc;
    if (hl) memcpy(b + 12, hdr, hl);
    memcpy(b + 12 + hl, data, n);
    int len = (int)(12 + hl + n);
    if (srtp_protect(r->tx, b, &len) != srtp_err_status_ok) return;
    send_sel(r, b, (size_t)len);
    if (t->hist && (size_t)len <= sizeof t->hist[0].b) {
        Hist *h = &t->hist[t->seq % HIST];
        h->seq = t->seq;
        h->len = (uint16_t)len;
        memcpy(h->b, b, (size_t)len);
    }
    t->packets++;
    t->octets += (uint32_t)(hl + n);
    t->seq++;
}

int rtc_media_ready(const Rtc *r) { return r && r->srtp_up && !r->failed && r->sel >= 0; }
int rtc_codec(const Rtc *r) { return r ? r->codec : 0; }

/* The session's encoder counts from its own start, and it starts again when the screen size changes
 * (or after a crash) while this peer goes on: its time going back moves the offset instead, so the
 * RTP timestamps keep going forward. Going back made the browser drop the new frames as old ones,
 * freeze and ask for keyframes over and over. */
static uint32_t track_ts(Track *t, uint32_t in, uint32_t step, uint32_t mul) {
    if (t->have_in && (int32_t)(in - t->last_in) <= 0) t->ts_off += (t->last_in - in + step) * mul;
    t->last_in = in;
    t->have_in = 1;
    return t->ts_off + in * mul;
}

void rtc_video(Rtc *r, const uint8_t *f, size_t n, uint32_t ms) {
    if (!rtc_media_ready(r) || !n) return;
    PktCtx c = { r, &r->v, track_ts(&r->v, ms, 33, 90) };
    r->v.last_ts = c.ts;
    r->v.last_ms = now_ms();
    if (r->codec == RW_CODEC_H264) rw_h264_packetize(f, n, MTU, rtp_out, &c);
    else rw_vp8_packetize(f, n, MTU, rtp_out, &c);
}

void rtc_audio(Rtc *r, const uint8_t *p, size_t n, uint32_t samples) {
    if (!rtc_media_ready(r) || !n || n > MTU || r->of->am < 0) return;
    PktCtx c = { r, &r->a, track_ts(&r->a, samples, 960, 1) };
    r->a.last_ts = c.ts;
    r->a.last_ms = now_ms();
    rtp_out(&c, NULL, 0, p, n, 0);
}

static void send_sr(Rtc *r, Track *t) {
    if (!t->packets) return;
    uint8_t b[28 + SRTP_MAX_TRAILER_LEN + 4];
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    uint64_t sec = (uint64_t)rt.tv_sec + 2208988800ull, frac = ((uint64_t)rt.tv_nsec << 32) / 1000000000ull;
    uint32_t ts = t->last_ts + (uint32_t)((now_ms() - t->last_ms) * (t == &r->v ? 90 : 48));
    uint32_t w[7] = { t->ssrc, (uint32_t)sec, (uint32_t)frac, ts, t->packets, t->octets, 0 };
    b[0] = 0x80; b[1] = 200; b[2] = 0; b[3] = 6;
    for (int i = 0; i < 6; i++) { uint32_t v = w[i]; b[4 + i * 4] = (uint8_t)(v >> 24); b[5 + i * 4] = (uint8_t)(v >> 16); b[6 + i * 4] = (uint8_t)(v >> 8); b[7 + i * 4] = (uint8_t)v; }
    int len = 28;
    if (srtp_protect_rtcp(r->tx, b, &len) == srtp_err_status_ok) send_sel(r, b, (size_t)len);
}

/* ---- feedback from the browser ---- */

static void set_rate(Rtc *r, double k) {
    unsigned lo = 300, hi = r->cfg.max_kbps ? r->cfg.max_kbps : 8000;
    unsigned nk = k < lo ? lo : k > hi ? hi : (unsigned)k;
    if (nk == r->kbps) return;
    unsigned old = r->kbps;
    r->kbps = nk;
    r->rate_at = now_ms();
    if (r->cb.rate && (nk > old * 1.04 || nk < old * 0.96)) r->cb.rate(r->cb.ud, nk);
}

static void on_fb(void *ud, const RwFb *f) {
    Rtc *r = ud;
    if (f->ssrc != r->v.ssrc && f->ssrc != r->a.ssrc && f->kind != RW_FB_REMB) return;
    double t = now_ms();
    switch (f->kind) {
    case RW_FB_RR:
        if (f->ssrc != r->v.ssrc) break;
        r->lost = r->lost * 0.7 + f->lost_frac / 256.0 * 0.3;
        if (f->lost_frac > 25) { r->congested_at = t; set_rate(r, r->kbps * 0.75); }
        else if (f->lost_frac > 5) { r->congested_at = t; set_rate(r, r->kbps * 0.92); }
        else if (f->lost_frac == 0 && t - r->rate_at > 1000)
            /* no loss: up fast (a LAN reaches the top in about 10 s), slowly for 5 s after congestion */
            set_rate(r, r->kbps * (t - r->congested_at > 5000 ? 1.25 : 1.06) + 50);
        break;
    case RW_FB_PLI:
    case RW_FB_FIR:
        if (f->ssrc != r->v.ssrc) break;
        r->plis++;
        if (t - r->kf_at > 300 && r->cb.keyframe) { r->kf_at = t; r->cb.keyframe(r->cb.ud); }
        break;
    case RW_FB_NACK:
        if (f->ssrc != r->v.ssrc || !r->v.hist) break;
        r->nacks++;
        for (int i = -1; i < 16; i++) {
            if (i >= 0 && !(f->blp & (1u << i))) continue;
            uint16_t seq = (uint16_t)(f->pid + (i < 0 ? 0 : i + 1));
            Hist *h = &r->v.hist[seq % HIST];
            if (h->len && h->seq == seq) { send_sel(r, h->b, h->len); r->resent++; }
        }
        break;
    case RW_FB_REMB: {
        /* The browser's estimate only grows with what it receives, so as a fixed ceiling it kept a
         * new connection at ~3 Mbit/s for minutes. It counts as congestion when it falls (the
         * browser saw the delay grow) or right after loss; then it is a ceiling. */
        unsigned b = (unsigned)(f->bitrate / 1000);
        if (r->remb && b < r->remb * 0.85) r->congested_at = t;
        if (t - r->congested_at < 5000 && b < r->kbps) set_rate(r, b);
        r->remb = b;
        break;
    }
    }
}

/* ---- ICE-lite, the built-in STUN server, external STUN and TURN ---- */

static Path *path_for(Rtc *r, const RwAddr *a, int relay, int tcp, int add) {
    for (int i = 0; i < r->npath; i++)
        if (r->path[i].relay == relay && r->path[i].tcp == tcp && rw_addr_equal(&r->path[i].a, a)) return &r->path[i];
    if (!add) return NULL;
    Path *p = NULL;
    for (int i = 0; i < r->npath && !p; i++) if (!r->path[i].a.family && i != r->sel) p = &r->path[i];     /* a dead one */
    if (!p && r->npath < MAX_PATHS) p = &r->path[r->npath++];
    if (!p) return NULL;
    memset(p, 0, sizeof *p);
    p->a = *a;
    p->relay = relay;
    p->tcp = tcp;
    return p;
}

/* The media's path went away (its TCP connection closed): another proven path, if any, takes over. */
static void reselect(Rtc *r) {
    int best = -1;
    for (int i = 0; i < r->npath; i++) {
        const Path *p = &r->path[i];
        if (!p->a.family || !p->valid || !p->proven) continue;
        if (best < 0 || (p->nominated && !r->path[best].nominated) || (p->nominated == r->path[best].nominated && p->last > r->path[best].last)) best = i;
    }
    r->sel = best;
    if (best >= 0 && r->cb.keyframe) r->cb.keyframe(r->cb.ud);
}

static void stun_answer(Rtc *r, const RwStun *s, const RwAddr *from, int relay, int tcp) {
    RwStunOut o;
    rw_so_init(&o, RW_STUN_BINDING_OK, s->tid);
    rw_so_xaddr(&o, RW_ATTR_XOR_MAPPED, from);
    rw_so_mi(&o, (const uint8_t *)r->pwd, strlen(r->pwd), hmac_sha1);
    rw_so_fingerprint(&o);
    if (!o.bad) send_to(r, from, relay, tcp, o.b, o.n);
}

/* The built-in STUN server: a plain binding request (no credentials) gets the address it came from,
 * on its own socket, at most 20 a second, only from addresses the rules allow. */
static void stun_server(Rtc *r, const uint8_t *p, size_t n, const RwAddr *from) {
    RwStun s;
    if (!rw_is_stun(p, n) || rw_stun_parse(p, n, &s) || s.type != RW_STUN_BINDING_REQ || s.username || s.mi_off) return;
    double t = now_ms();
    if (t - r->stun_win > 1000) { r->stun_win = t; r->stun_n = 0; }
    if (++r->stun_n > 20 || !src_allowed(r, from, 0)) return;
    RwStunOut o;
    rw_so_init(&o, RW_STUN_BINDING_OK, s.tid);
    rw_so_xaddr(&o, RW_ATTR_XOR_MAPPED, from);
    rw_so_fingerprint(&o);
    struct sockaddr_in6 sa;
    to_sockaddr(from, &sa);
    if (!o.bad) sendto(r->sfd, o.b, o.n, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&sa, sizeof sa);
}

static void ice_check(Rtc *r, const uint8_t *p, size_t n, const RwStun *s, const RwAddr *from, int relay, int tcp) {
    if (!r->offered || !s->username || !s->mi_off || !s->has_fp) return;
    char want[300];
    int wl = snprintf(want, sizeof want, "%s:%s", r->ufrag, r->of->ufrag);
    if (wl < 0 || (size_t)wl != s->ulen || memcmp(want, s->username, s->ulen)) return;
    if (!rw_stun_check_mi(p, n, s, (const uint8_t *)r->pwd, strlen(r->pwd), hmac_sha1)) return;
    if (!src_allowed(r, from, relay)) {
        static int said;
        char a[64];
        rw_addr_format(from, a, sizeof a);
        if (!said++) sd_log("video: refused ICE check from %s (Settings -> Sharing: who can connect)", a);
        return;
    }
    Path *pa = path_for(r, from, relay, tcp, 1);
    if (!pa) return;
    if (tcp) r->tcp[tcp - 1].checked = 1;
    pa->valid = 1;
    pa->last = now_ms();
    r->last_consent = pa->last;
    r->checks++;
    if (s->use_candidate && !pa->nominated) {
        for (int i = 0; i < r->npath; i++) r->path[i].nominated = 0;
        pa->nominated = 1;
        /* once DTLS is up, media only moves to a path that proved to be the browser (an authenticated
         * DTLS or SRTCP packet from it, see prove()): ICE credentials alone can't redirect the stream */
        if (!r->dtls_up || pa->proven) {
            int was = r->sel;
            r->sel = (int)(pa - r->path);
            if (r->dtls_up && was != r->sel && r->cb.keyframe) r->cb.keyframe(r->cb.ud);
        }
    }
    if (r->sel < 0 && !r->dtls_up) r->sel = (int)(pa - r->path);
    stun_answer(r, s, from, relay, tcp);
}

static void turn_send(Rtc *r, uint16_t type, int auth, const RwAddr *peers, int npeers, uint32_t lifetime) {
    Turn *t = &r->turn;
    RwStunOut o;
    rnd(t->tid, 12);
    rw_so_init(&o, type, t->tid);
    if (type == RW_TURN_ALLOCATE_REQ) { uint8_t tr[4] = { 17, 0, 0, 0 }; rw_so_attr(&o, RW_ATTR_TRANSPORT, tr, 4); }
    if (lifetime) rw_so_u32(&o, RW_ATTR_LIFETIME, lifetime);
    for (int i = 0; i < npeers; i++) rw_so_xaddr(&o, RW_ATTR_XOR_PEER, &peers[i]);
    if (auth) {
        rw_so_attr(&o, RW_ATTR_USERNAME, r->cfg.turn_user, strlen(r->cfg.turn_user));
        rw_so_attr(&o, RW_ATTR_REALM, t->realm, strlen(t->realm));
        rw_so_attr(&o, RW_ATTR_NONCE, t->nonce, t->nlen);
        rw_so_mi(&o, t->key, 16, hmac_sha1);
    }
    rw_so_fingerprint(&o);
    if (!o.bad) turn_raw(r, o.b, o.n);
    t->sent_at = now_ms();
}

/* The TURN password (first peer) or the key a previous peer derived from it: gone once used, or when
 * there will be no relay. */
static void wipe_pass(Rtc *r) {
    explicit_bzero(r->cfg.turn_key, sizeof r->cfg.turn_key);
    r->cfg.turn_has_key = 0;
    if (!r->cfg.turn_pass) return;
    explicit_bzero(r->cfg.turn_pass, strlen(r->cfg.turn_pass));
    free(r->cfg.turn_pass);
    r->cfg.turn_pass = NULL;
}

/* long-term credential key: MD5(user ":" realm ":" password) (RFC 8489 §9.2.2) */
static int turn_key(Rtc *r) {
    /* a later peer of the same bridge: the key derived by the first one, for the same realm only */
    if (!r->cfg.turn_pass && r->cfg.turn_has_key && !strcmp(r->cfg.turn_realm, r->turn.realm)) {
        memcpy(r->turn.key, r->cfg.turn_key, 16);
        wipe_pass(r);
        return 0;
    }
    if (!r->cfg.turn_pass) return -1;
    char buf[1024];
    int k = snprintf(buf, sizeof buf, "%s:%s:%s", r->cfg.turn_user, r->turn.realm, r->cfg.turn_pass);
    unsigned int l = 16;
    int ok = k > 0 && (size_t)k < sizeof buf && EVP_Digest(buf, (size_t)k, r->turn.key, &l, EVP_md5(), NULL) && l == 16;
    explicit_bzero(buf, sizeof buf);
    wipe_pass(r);
    return ok ? 0 : -1;
}

static void turn_perms(Rtc *r) {
    Turn *t = &r->turn;
    if (t->state != 3 || !t->nperm) return;
    turn_send(r, RW_TURN_PERM_REQ, 1, t->perm, t->nperm, 0);
    t->perm_at = now_ms() + 240000;
    t->perm_dirty = 0;
}

static void turn_peer(Rtc *r, const RwAddr *a) {
    Turn *t = &r->turn;
    if (!t->state || t->state < 0 || a->family == 0) return;
    for (int i = 0; i < t->nperm; i++) if (same_ip(&t->perm[i], a)) return;
    if (t->nperm >= 8) return;
    t->perm[t->nperm] = *a;
    t->perm[t->nperm].port = 0;
    t->nperm++;
    t->perm_dirty = 1;
}

static void handle_packet(Rtc *r, const uint8_t *p, size_t n, const RwAddr *from, int relay, int tcp);
static void prove(Rtc *r, Path *pa);

static void turn_input(Rtc *r, const uint8_t *p, size_t n, const RwStun *s) {
    Turn *t = &r->turn;
    if (s->type == RW_TURN_DATA_IND) {                             /* a packet from a peer, relayed */
        if (t->state == 3 && s->data && s->xor_peer.family && s->dlen <= 1400) handle_packet(r, s->data, s->dlen, &s->xor_peer, 1, 0);
        return;
    }
    if (memcmp(s->tid, t->tid, 12)) return;
    int cls = s->type & 0x0110;
    if (cls == 0x0100 && s->mi_off && !rw_stun_check_mi(p, n, s, t->key, 16, hmac_sha1)) return;   /* forged answer */
    if (cls == 0x0110 && (s->error == 401 || s->error == 438)) {
        if (!s->realm || !s->nonce || s->rlen >= sizeof t->realm || s->nlen > sizeof t->nonce || ++t->tries > 3) { t->state = -1; sd_log("video: the TURN server refused us"); return; }
        memcpy(t->realm, s->realm, s->rlen);
        t->realm[s->rlen] = 0;
        memcpy(t->nonce, s->nonce, s->nlen);
        t->nlen = s->nlen;
        if (t->state == 1 && turn_key(r)) { t->state = -1; return; }
        if (t->state <= 2) { t->state = 2; turn_send(r, RW_TURN_ALLOCATE_REQ, 1, NULL, 0, 600); }
        else if (s->type == RW_TURN_PERM_ERR) turn_perms(r);
        else turn_send(r, RW_TURN_REFRESH_REQ, 1, NULL, 0, 600);
        return;
    }
    if (s->type == RW_TURN_ALLOCATE_OK) {
        if (!s->mi_off || !s->xor_relayed.family) { t->state = -1; return; }
        t->relayed = s->xor_relayed;
        t->mapped = s->xor_mapped;
        t->state = 3;
        t->tries = 0;
        unsigned life = s->lifetime ? s->lifetime : 600;
        t->refresh_at = now_ms() + (life > 120 ? life - 60 : life / 2) * 1000.0;
        char a[64];
        rw_addr_format(&t->relayed, a, sizeof a);
        sd_log("video: TURN relay %s:%u", a, t->relayed.port);
        for (int i = 0; i < r->nrcand; i++) turn_peer(r, &r->rcand[i]);
        turn_perms(r);
    } else if (s->type == RW_TURN_REFRESH_OK) {
        unsigned life = s->lifetime ? s->lifetime : 600;
        t->refresh_at = now_ms() + (life > 120 ? life - 60 : life / 2) * 1000.0;
    } else if (cls == 0x0110) {
        sd_log("video: TURN error %d", s->error);
        if (t->state < 3) t->state = -1;
    }
}

static void handle_packet(Rtc *r, const uint8_t *p, size_t n, const RwAddr *from, int relay, int tcp) {
    if (!n || r->failed) return;
    if (rw_is_stun(p, n)) {
        RwStun s;
        if (rw_stun_parse(p, n, &s)) return;
        if (!relay && !tcp && r->turn.state && r->turn.transport == RW_TURN_UDP && rw_addr_equal(from, &r->turn.srv)) { turn_input(r, p, n, &s); return; }
        if (s.type == RW_STUN_BINDING_REQ) { ice_check(r, p, n, &s, from, relay, tcp); return; }
        if (s.type == RW_STUN_BINDING_OK && !relay && !tcp)
            for (int i = 0; i < r->nstun_srv; i++)
                if (rw_addr_equal(from, &r->stun_srv[i]) && !memcmp(s.tid, r->stun_tid[i], 12) && s.xor_mapped.family && !r->srflx.family) r->srflx = s.xor_mapped;
        return;
    }
    /* DTLS and SRTP only from an address that passed an ICE check */
    Path *pa = path_for(r, from, relay, tcp, 0);
    if (!pa || !pa->valid) return;
    if (p[0] >= 20 && p[0] <= 63) {
        if (!r->offered) return;
        if (!r->dtls_up && r->sel != (int)(pa - r->path)) r->sel = (int)(pa - r->path);
        unsigned long before = r->dtls_rx;
        dtls_input(r, p, n);
        if (r->dtls_rx != before) prove(r, pa);           /* a record that decrypted: the browser */
    } else if (p[0] >= 128 && p[0] <= 191 && n >= 8 && p[1] >= 192 && p[1] <= 223 && r->srtp_up) {
        uint8_t b[1500 + SRTP_MAX_TRAILER_LEN];
        if (n > 1500) return;
        memcpy(b, p, n);
        int len = (int)n;
        if (srtp_unprotect_rtcp(r->rx, b, &len) != srtp_err_status_ok) return;
        prove(r, pa);
        rw_rtcp_parse(b, (size_t)len, on_fb, r);
    }
}

/* An authenticated packet (DTLS application data or SRTCP) came from this path: it is the browser. If
 * the browser nominated it, the media goes there now. */
static void prove(Rtc *r, Path *pa) {
    if (!pa) return;
    int was = r->sel;
    pa->proven = 1;
    if (pa->nominated || r->sel < 0) r->sel = (int)(pa - r->path);
    if (r->dtls_up && was != r->sel && r->cb.keyframe) r->cb.keyframe(r->cb.ud);     /* a new path: a clean picture */
}

/* ---- ICE-TCP connections ---- */

static void tcp_close(Rtc *r, int i, const char *why) {
    TcpConn *c = &r->tcp[i];
    if (c->fd < 0) return;
    if (c->checked) {
        char a[64];
        rw_addr_format(&c->peer, a, sizeof a);
        sd_log("video: ICE-TCP connection from %s closed (%s)", a, why);
    }
    close(c->fd);
    c->fd = -1;
    q_free(&c->out);
    c->inl = 0;
    c->checked = 0;
    int lost_sel = 0;
    for (int k = 0; k < r->npath; k++) {
        Path *p = &r->path[k];
        if (p->tcp != i + 1) continue;
        if (k == r->sel) lost_sel = 1;
        memset(p, 0, sizeof *p);                    /* dead: path_for reuses it */
    }
    if (lost_sel) reselect(r);
}

/* lord-sessiond's root connection child hands us an ICE-TCP connection: the socket and the first
 * framed packet it read (whose check names our route). Only from root, only a TCP socket. */
static void tcp_accept(Rtc *r) {
    int u = accept4(r->cfg.ice_lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (u < 0) return;
    struct ucred cr;
    socklen_t cl = sizeof cr;
    uint8_t first[RW_TCP_FRAME_MAX + 2];
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { first, sizeof first };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    int fd = -1;
    ssize_t got = -1;
    if (!getsockopt(u, SOL_SOCKET, SO_PEERCRED, &cr, &cl) && cr.uid == 0) {
        struct pollfd pw = { u, POLLIN, 0 };
        if (poll(&pw, 1, 200) > 0) got = recvmsg(u, &mh, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
        for (struct cmsghdr *cm = got > 0 ? CMSG_FIRSTHDR(&mh) : NULL; cm; cm = CMSG_NXTHDR(&mh, cm))
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS && cm->cmsg_len == CMSG_LEN(sizeof(int))) memcpy(&fd, CMSG_DATA(cm), sizeof(int));
    }
    close(u);
    if (fd < 0) return;
    int type = 0;
    socklen_t tl = sizeof type;
    struct sockaddr_in6 sa;
    socklen_t sl = sizeof sa;
    if (got < 2 || (mh.msg_flags & MSG_TRUNC) || getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) || type != SOCK_STREAM ||
        getpeername(fd, (struct sockaddr *)&sa, &sl) || sa.sin6_family != AF_INET6) { close(fd); return; }
    int slot = -1;
    double oldest = 0;
    for (int i = 0; i < MAX_TCP; i++) if (r->tcp[i].fd < 0) { slot = i; break; }
    if (slot < 0)                                   /* full: the oldest one that never brought a valid check goes */
        for (int i = 0; i < MAX_TCP; i++) if (!r->tcp[i].checked && (slot < 0 || r->tcp[i].born < oldest)) { slot = i; oldest = r->tcp[i].born; }
    if (slot < 0) { close(fd); return; }
    tcp_close(r, slot, "replaced");
    TcpConn *c = &r->tcp[slot];
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    c->fd = fd;
    from_sockaddr(&sa, &c->peer);
    c->born = now_ms();
    memcpy(c->in, first, (size_t)got);
    c->inl = (size_t)got;
}

static void tcp_input(Rtc *r, int i) {
    TcpConn *c = &r->tcp[i];
    for (int rounds = 0; c->fd >= 0 && rounds < 64; rounds++) {
        if (c->inl < sizeof c->in) {
            ssize_t got = recv(c->fd, c->in + c->inl, sizeof c->in - c->inl, MSG_DONTWAIT);
            if (got == 0) { tcp_close(r, i, "closed by the other end"); return; }
            if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { tcp_close(r, i, "read failed"); return; }
            if (got > 0) c->inl += (size_t)got;
        }
        size_t used = 0, len;
        int k;
        while (c->fd >= 0 && (k = rw_tcp_frame(c->in + used, c->inl - used, &len)) == 1) {
            handle_packet(r, c->in + used + 2, len, &c->peer, 0, i + 1);
            used += 2 + len;
            if (r->failed) return;
        }
        if (c->fd < 0) return;
        if (k < 0) { tcp_close(r, i, "bad framing"); return; }
        memmove(c->in, c->in + used, c->inl - used);
        c->inl -= used;
        if (!used && c->inl < sizeof c->in) {
            /* nothing complete: wait for more (unless the kernel has more right now) */
            int pending = 0;
            if (ioctl(c->fd, FIONREAD, &pending) || pending <= 0) return;
        }
    }
}

/* ---- TURN over TCP or TLS ---- */

static void turn_stream_input(Rtc *r) {
    Turn *t = &r->turn;
    for (int rounds = 0; t->fd >= 0 && t->up && rounds < 64; rounds++) {
        ssize_t got;
        if (t->tssl) {
            int k = SSL_read(t->tssl, t->in + t->inl, (int)(sizeof t->in - t->inl));
            if (k <= 0) {
                int e = SSL_get_error(t->tssl, k);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return;
                ERR_clear_error();
                got = 0;
            } else got = k;
        } else {
            got = recv(t->fd, t->in + t->inl, sizeof t->in - t->inl, MSG_DONTWAIT);
            if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        }
        if (got <= 0) { t->up = 0; t->state = -1; sd_log("video: the TURN server closed the connection"); return; }
        t->inl += (size_t)got;
        size_t used = 0, len;
        int k;
        while ((k = rw_stun_stream(t->in + used, t->inl - used, &len)) == 1) {
            RwStun s;
            if (!rw_stun_parse(t->in + used, len, &s)) turn_input(r, t->in + used, len, &s);
            used += len;
            if (r->failed || !t->up) return;
        }
        if (k < 0) { t->up = 0; t->state = -1; sd_log("video: the TURN server sent something that isn't STUN"); return; }
        memmove(t->in, t->in + used, t->inl - used);
        t->inl -= used;
    }
}

/* The TLS certificate of the TURN server: the pinned fingerprint if Settings has one, else the
 * system's CAs and the server's name (or IP address), checked by OpenSSL during the handshake. */
static int turn_cert_ok(Rtc *r) {
    Turn *t = &r->turn;
    X509 *peer = SSL_get1_peer_certificate(t->tssl);
    if (!peer) return 0;
    int ok;
    if (r->cfg.turn_has_fp) {
        uint8_t fp[32];
        unsigned int l = 32;
        ok = X509_digest(peer, EVP_sha256(), fp, &l) && l == 32 && !CRYPTO_memcmp(fp, r->cfg.turn_fp, 32);
    } else ok = SSL_get_verify_result(t->tssl) == X509_V_OK;
    X509_free(peer);
    return ok;
}

static void turn_stream_ready(Rtc *r) {
    Turn *t = &r->turn;
    if (t->connecting == 1) {
        int err = 0;
        socklen_t el = sizeof err;
        if (getsockopt(t->fd, SOL_SOCKET, SO_ERROR, &err, &el) || err) { t->state = -1; sd_log("video: could not connect to the TURN server (%s)", strerror(err)); return; }
        if (t->transport == RW_TURN_TCP) { t->connecting = 0; t->up = 1; turn_flush(r); return; }
        t->connecting = 2;
        t->tctx = SSL_CTX_new(TLS_client_method());
        if (!t->tctx) { t->state = -1; return; }
        SSL_CTX_set_min_proto_version(t->tctx, TLS1_2_VERSION);
        if (!r->cfg.turn_has_fp) {
            SSL_CTX_set_default_verify_paths(t->tctx);
            SSL_CTX_set_verify(t->tctx, SSL_VERIFY_PEER, NULL);
        } else SSL_CTX_set_verify(t->tctx, SSL_VERIFY_NONE, NULL);     /* the pin is checked after the handshake */
        t->tssl = SSL_new(t->tctx);
        if (!t->tssl) { t->state = -1; return; }
        SSL_set_fd(t->tssl, t->fd);
        SSL_set_mode(t->tssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        RwAddr ip;
        if (rw_addr_text(t->host, &ip)) {           /* a name: SNI and the name check */
            SSL_set_tlsext_host_name(t->tssl, t->host);
            if (!r->cfg.turn_has_fp) SSL_set1_host(t->tssl, t->host);
        } else if (!r->cfg.turn_has_fp) X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(t->tssl), t->host);
        SSL_set_connect_state(t->tssl);
    }
    if (t->connecting == 2) {
        int k = SSL_do_handshake(t->tssl);
        if (k == 1) {
            if (!turn_cert_ok(r)) { t->state = -1; sd_log("video: the TURN server's certificate was refused (%s)", r->cfg.turn_has_fp ? "it doesn't match the pinned fingerprint" : X509_verify_cert_error_string(SSL_get_verify_result(t->tssl))); return; }
            t->connecting = 0;
            t->up = 1;
            turn_flush(r);
            return;
        }
        int e = SSL_get_error(t->tssl, k);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            long v = SSL_get_verify_result(t->tssl);
            ERR_clear_error();
            t->state = -1;
            sd_log("video: TLS with the TURN server failed%s%s", v != X509_V_OK ? ": " : "", v != X509_V_OK ? X509_verify_cert_error_string(v) : "");
        }
    }
}

static void turn_stream_close(Turn *t) {
    if (t->tssl) { SSL_free(t->tssl); t->tssl = NULL; }
    if (t->tctx) { SSL_CTX_free(t->tctx); t->tctx = NULL; }
    if (t->fd >= 0) close(t->fd);
    t->fd = -1;
    t->up = t->connecting = 0;
    q_free(&t->out);
}

/* ---- life cycle ---- */

int rtc_port(const Rtc *r) { return r ? r->sport : 0; }

static void new_credentials(Rtc *r) {
    char tail[5];
    rnd_text(tail, 4);
    snprintf(r->ufrag, sizeof r->ufrag, "%.8s%s", r->cfg.route, tail);
    rnd_text(r->pwd, 24);
}

Rtc *rtc_new(const RtcConfig *cfg, const RtcCallbacks *cb) {
    Rtc *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->cfg = *cfg;
    r->cb = *cb;
    r->sel = -1;
    r->fd = r->sfd = -1;
    r->turn.fd = -1;
    for (int i = 0; i < MAX_TCP; i++) r->tcp[i].fd = -1;
    r->created = r->last_consent = now_ms();
    r->kbps = cfg->max_kbps && cfg->max_kbps < 4000 ? cfg->max_kbps : 4000;
    if (strlen(r->cfg.route) != 8) rnd_text(r->cfg.route, 8);
    new_credentials(r);
    if (!srtp_ready) { if (srtp_init() != srtp_err_status_ok) goto bad; srtp_ready = 1; }
    if (dtls_init(r)) goto bad;
    r->fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (r->fd < 0) goto bad;
    int off = 0, buf = 2 << 20;
    setsockopt(r->fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    setsockopt(r->fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
    setsockopt(r->fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    struct sockaddr_in6 any = { .sin6_family = AF_INET6 };
    socklen_t sl = sizeof any;
    if (bind(r->fd, (struct sockaddr *)&any, sizeof any) || getsockname(r->fd, (struct sockaddr *)&any, &sl)) goto bad;
    r->port = ntohs(any.sin6_port);
    r->sfd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (r->sfd < 0) goto bad;
    setsockopt(r->sfd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    struct sockaddr_in6 any2 = { .sin6_family = AF_INET6 };
    sl = sizeof any2;
    if (bind(r->sfd, (struct sockaddr *)&any2, sizeof any2) || getsockname(r->sfd, (struct sockaddr *)&any2, &sl)) goto bad;
    r->sport = ntohs(any2.sin6_port);
    uint32_t ids[4];
    rnd(ids, sizeof ids);
    r->v.ssrc = ids[0] | 1;
    r->a.ssrc = ids[1] | 2;
    r->v.ts_off = ids[2];
    r->a.ts_off = ids[3];
    r->v.seq = (uint16_t)ids[2];
    r->a.seq = (uint16_t)ids[3];
    r->v.hist = calloc(HIST, sizeof(Hist));
    /* external STUN servers: our public address, if any */
    for (int i = 0; i < cfg->nstun && r->nstun_srv < RTC_MAX_STUN; i++) {
        RwAddr a;
        if (resolve(cfg->stun[i], &a)) continue;
        r->stun_srv[r->nstun_srv] = a;
        rnd(r->stun_tid[r->nstun_srv], 12);
        RwStunOut o;
        rw_so_init(&o, RW_STUN_BINDING_REQ, r->stun_tid[r->nstun_srv]);
        rw_so_fingerprint(&o);
        raw_send(r, &a, o.b, o.n);
        r->nstun_srv++;
    }
    if (cfg->turn[0] && cfg->turn_user[0] && (cfg->turn_pass || cfg->turn_has_key)) {
        Turn *t = &r->turn;
        t->transport = cfg->turn_transport;
        const char *colon = strrchr(cfg->turn, ':');
        size_t hl = colon ? (size_t)(colon - cfg->turn) : 0;
        if (hl && hl < sizeof t->host) {
            memcpy(t->host, cfg->turn, hl);
            t->host[hl] = 0;
            if (t->host[0] == '[' && hl > 2) { memmove(t->host, t->host + 1, hl - 2); t->host[hl - 2] = 0; }
        }
        if (resolve(cfg->turn, &t->srv)) sd_log("video: the TURN server's name does not resolve");
        else if (t->transport == RW_TURN_UDP) { t->state = 1; turn_send(r, RW_TURN_ALLOCATE_REQ, 0, NULL, 0, 600); }
        else {
            /* the stream is connected without blocking; the Allocate waits in its queue */
            t->fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            struct sockaddr_in6 ts;
            to_sockaddr(&t->srv, &ts);
            if (t->fd >= 0) {
                int v6only = 0, on = 1;
                setsockopt(t->fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof v6only);
                setsockopt(t->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
            }
            if (t->fd < 0 || (connect(t->fd, (struct sockaddr *)&ts, sizeof ts) && errno != EINPROGRESS)) {
                sd_log("video: could not connect to the TURN server");
                turn_stream_close(t);
            } else {
                t->connecting = 1;
                t->started = now_ms();
                t->state = 1;
                turn_send(r, RW_TURN_ALLOCATE_REQ, 0, NULL, 0, 600);
            }
        }
    }
    if (r->turn.state != 1) wipe_pass(r);
    return r;
bad:
    sd_log("video: could not start the WebRTC peer");
    rtc_free(r);
    return NULL;
}

void rtc_free(Rtc *r) {
    if (!r) return;
    if (r->sctp) { usrsctp_close(r->sctp); r->sctp = NULL; }
    if (r->sctp_on) usrsctp_deregister_address(r);
    if (r->tx) srtp_dealloc(r->tx);
    if (r->rx) srtp_dealloc(r->rx);
    if (r->ssl) SSL_free(r->ssl);
    if (r->ctx) SSL_CTX_free(r->ctx);
    X509_free(r->cert);
    EVP_PKEY_free(r->key);
    if (r->fd >= 0) close(r->fd);
    if (r->sfd >= 0) close(r->sfd);
    for (int i = 0; i < MAX_TCP; i++) if (r->tcp[i].fd >= 0) { close(r->tcp[i].fd); q_free(&r->tcp[i].out); }
    turn_stream_close(&r->turn);
    wipe_pass(r);
    explicit_bzero(r->turn.key, sizeof r->turn.key);
    explicit_bzero(r->pwd, sizeof r->pwd);
    free(r->v.hist);
    free(r->rasm);
    free(r->of);
    free(r);
}

/* host candidates: the address the WebSocket came in on first, then the other interfaces; each also
 * as a passive TCP candidate on the remote desktop's port (lower priority: UDP wins when it works) */
static int local_cands(Rtc *r, RwLocalCand *c, int max) {
    int n = 0;
    RwAddr seen[16];
    int ns = 0;
    if (r->cfg.ws_local.family) {
        rw_addr_format(&r->cfg.ws_local, c[n].ip, sizeof c[n].ip);
        c[n].port = (uint16_t)r->port;
        c[n].type = RW_CAND_HOST;
        c[n].prio = (126u << 24) | (65535u << 8) | 255u;
        seen[ns++] = r->cfg.ws_local;
        n++;
    }
    struct ifaddrs *ifs = NULL;
    if (!getifaddrs(&ifs)) {
        for (struct ifaddrs *i = ifs; i && n < max / 2 && ns < 16; i = i->ifa_next) {
            if (!i->ifa_addr || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
            RwAddr a = { 0 };
            if (i->ifa_addr->sa_family == AF_INET) { a.family = 4; memcpy(a.a, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, 4); }
            else if (i->ifa_addr->sa_family == AF_INET6) {
                const uint8_t *b = ((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr.s6_addr;
                if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) continue;         /* link-local: needs a scope */
                a.family = 6;
                memcpy(a.a, b, 16);
            } else continue;
            int dup = 0;
            for (int k = 0; k < ns; k++) if (same_ip(&seen[k], &a)) dup = 1;
            if (dup) continue;
            seen[ns++] = a;
            rw_addr_format(&a, c[n].ip, sizeof c[n].ip);
            c[n].port = (uint16_t)r->port;
            c[n].type = RW_CAND_HOST;
            c[n].prio = (126u << 24) | ((unsigned)(60000 - n * 10 - (a.family == 6 ? 5 : 0)) << 8) | 255u;
            n++;
        }
        freeifaddrs(ifs);
    }
    int nhost = n;
    if (r->cfg.ice_lfd >= 0 && r->cfg.tcp_port > 0)
        for (int i = 0; i < nhost && n < max - 2; i++) {
            c[n] = c[i];
            c[n].tcp = 1;
            c[n].port = (uint16_t)r->cfg.tcp_port;
            c[n].prio = (80u << 24) | ((unsigned)(60000 - i * 10) << 8) | 255u;
            n++;
        }
    if (r->srflx.family && n < max) {
        rw_addr_format(&r->srflx, c[n].ip, sizeof c[n].ip);
        c[n].port = r->srflx.port;
        c[n].type = RW_CAND_SRFLX;
        c[n].prio = (100u << 24) | (65535u << 8) | 255u;
        snprintf(c[n].raddr, sizeof c[n].raddr, "%s", n ? c[0].ip : "0.0.0.0");
        c[n].rport = (uint16_t)r->port;
        n++;
    }
    if (r->turn.state == 3 && n < max) {
        rw_addr_format(&r->turn.relayed, c[n].ip, sizeof c[n].ip);
        c[n].port = r->turn.relayed.port;
        c[n].type = RW_CAND_RELAY;
        c[n].prio = (2u << 24) | (65535u << 8) | 255u;
        rw_addr_format(&r->turn.mapped, c[n].raddr, sizeof c[n].raddr);
        c[n].rport = r->turn.mapped.port;
        n++;
    }
    return n;
}

static void json_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c >= 0x20 && c < 0x7f) out[o++] = (char)c;
    }
    out[o] = 0;
}

static void send_answer(Rtc *r) {
    if (r->answered) return;
    r->answered = 1;
    RwLocalCand c[24];
    memset(c, 0, sizeof c);
    int nc = local_cands(r, c, 24);
    static uint32_t sid;
    if (!sid) { rnd(&sid, sizeof sid); sid &= 0x7fffffff; if (!sid) sid = 1; }
    RwAnswer an = { r->ufrag, r->pwd, r->fp, r->v.ssrc, r->a.ssrc, r->of->am >= 0, c, nc, sid, 2 + r->restarts };
    static char sdp[16384], esc[20000], msg[20100];
    int l = rw_sdp_answer(r->of, &an, sdp, sizeof sdp);
    if (l < 0) { fail(r, RTC_WHY_SERVER, "the answer does not fit"); return; }
    json_escape(sdp, esc, sizeof esc);
    snprintf(msg, sizeof msg, "{\"t\":\"rtc\",\"sdp\":\"%s\",\"codec\":\"%s\",\"p\":%d%s}", esc, r->codec == RW_CODEC_H264 ? "h264" : "vp8",
             r->cfg.peer_no, r->restarts ? ",\"restart\":1" : "");
    if (r->cb.signal) r->cb.signal(r->cb.ud, msg);
}

static void add_remote_cands(Rtc *r) {
    for (int i = 0; i < r->of->ncand && r->nrcand < 16; i++) {
        const RwCand *c = &r->of->cand[i];
        if (!c->mdns && !c->tcp && c->component == 1) { r->rcand[r->nrcand++] = c->addr; turn_peer(r, &c->addr); }
    }
    turn_perms(r);
}

/* An ICE restart (RFC 8445 §9): the same browser (the DTLS fingerprint of the signed-in offer), the same
 * sections and codec, new credentials on both sides. Paths must be checked again with them; the media
 * keeps its path until a new one is nominated and proven. */
static int restart(Rtc *r, const char *sdp, size_t n, int allow_h264) {
    RwOffer *of = malloc(sizeof *of);
    if (!of) return -1;
    int e = rw_sdp_parse_offer(sdp, n, allow_h264, of);
    int same = !e && !CRYPTO_memcmp(of->fp, r->of->fp, 32) && of->vcodec == r->of->vcodec && of->vpt == r->of->vpt &&
               of->apt == r->of->apt && of->nmedia == r->of->nmedia && (of->dm >= 0) == (r->of->dm >= 0);
    for (int i = 0; same && i < of->nmedia; i++) same = of->m[i].kind == r->of->m[i].kind && !strcmp(of->m[i].mid, r->of->m[i].mid);
    if (!same) {
        sd_log("video: refused an ICE restart (%s)", e ? "the offer doesn't parse" : "it isn't the same peer, codec or sections");
        free(of);
        return e ? e : -1;
    }
    if (!strcmp(of->ufrag, r->of->ufrag) && !strcmp(of->pwd, r->of->pwd)) {      /* not a restart: the same answer again */
        free(of);
        r->answered = 0;
        send_answer(r);
        return r->codec;
    }
    free(r->of);
    r->of = of;
    r->restarts++;
    new_credentials(r);
    for (int i = 0; i < r->npath; i++) { r->path[i].valid = 0; r->path[i].nominated = 0; }
    r->checks = 0;
    r->nrcand = 0;
    r->last_consent = r->created = now_ms();
    add_remote_cands(r);
    r->answered = 0;
    sd_log("video: ICE restart %u", r->restarts);
    send_answer(r);
    return r->codec;
}

int rtc_offer(Rtc *r, const char *sdp, size_t n, int allow_h264) {
    if (!r || r->failed) return -1;
    if (r->offered) return restart(r, sdp, n, allow_h264);
    r->of = malloc(sizeof *r->of);
    if (!r->of) return -1;
    int e = rw_sdp_parse_offer(sdp, n, allow_h264, r->of);
    if (e) { sd_log("video: refused the browser's offer (%d)", e); free(r->of); r->of = NULL; return e; }
    r->offered = 1;
    r->created = r->last_consent = now_ms();
    r->codec = r->of->vcodec;
    r->v.pt = r->of->vpt;
    r->a.pt = r->of->apt;
    add_remote_cands(r);
    int waiting = (r->nstun_srv && !r->srflx.family) || (r->turn.state > 0 && r->turn.state < 3);
    if (waiting) r->answer_due = now_ms() + (r->turn.state > 0 && r->turn.transport != RW_TURN_UDP ? ANSWER_WAIT_TCP_MS : ANSWER_WAIT_MS);
    else send_answer(r);
    return r->codec;
}

int rtc_candidate(Rtc *r, const char *c, size_t n) {
    RwCand k;
    if (!r || !r->offered) return -1;
    if (n == 0) return 0;                                           /* end of candidates */
    if (rw_cand_parse(c, n, &k)) return -1;
    if (!k.mdns && !k.tcp && k.component == 1 && r->nrcand < 16) {
        r->rcand[r->nrcand++] = k.addr;
        turn_peer(r, &k.addr);
        if (r->turn.perm_dirty) turn_perms(r);
    }
    return 0;
}

int rtc_pollfds(Rtc *r, struct pollfd *p, int max) {
    if (!r || r->fd < 0 || max < RTC_MAX_FDS || r->failed) return 0;
    int n = 0;
    p[n++] = (struct pollfd){ r->fd, POLLIN, 0 };
    p[n++] = (struct pollfd){ r->sfd, POLLIN, 0 };
    if (r->cfg.ice_lfd >= 0) p[n++] = (struct pollfd){ r->cfg.ice_lfd, POLLIN, 0 };
    for (int i = 0; i < MAX_TCP; i++)
        if (r->tcp[i].fd >= 0) p[n++] = (struct pollfd){ r->tcp[i].fd, (short)(POLLIN | (r->tcp[i].out.off < r->tcp[i].out.len ? POLLOUT : 0)), 0 };
    Turn *t = &r->turn;
    if (t->fd >= 0 && t->state >= 0) {
        short ev = POLLIN;
        if (t->connecting == 1 || t->out.off < t->out.len) ev |= POLLOUT;
        if (t->connecting == 2 && t->tssl && SSL_want_write(t->tssl)) ev |= POLLOUT;
        p[n++] = (struct pollfd){ t->fd, ev, 0 };
    }
    return n;
}

void rtc_dispatch(Rtc *r, const struct pollfd *p, int n) {
    if (!r) return;
    static uint8_t buf[2048];
    for (int f = 0; f < n && !r->failed; f++) {
        short ev = p[f].revents;
        if (!ev) continue;
        int fd = p[f].fd;
        if (fd == r->fd || fd == r->sfd) {
            if (!(ev & POLLIN)) continue;
            for (int k = 0; k < 256 && !r->failed; k++) {
                struct sockaddr_in6 s;
                socklen_t sl = sizeof s;
                ssize_t got = recvfrom(fd, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&s, &sl);
                if (got < 0) break;
                if (got > 1500 || s.sin6_family != AF_INET6) continue;
                RwAddr from;
                from_sockaddr(&s, &from);
                if (fd == r->sfd) stun_server(r, buf, (size_t)got, &from);
                else handle_packet(r, buf, (size_t)got, &from, 0, 0);
            }
        } else if (fd == r->cfg.ice_lfd) {
            if (ev & POLLIN) for (int k = 0; k < 4; k++) { tcp_accept(r); struct pollfd q = { fd, POLLIN, 0 }; if (poll(&q, 1, 0) <= 0) break; }
            /* the new connection's first packet is already in its buffer */
            for (int i = 0; i < MAX_TCP; i++) if (r->tcp[i].fd >= 0 && r->tcp[i].inl && !r->tcp[i].checked) tcp_input(r, i);
        } else if (fd == r->turn.fd) {
            Turn *t = &r->turn;
            if (t->connecting) turn_stream_ready(r);
            else {
                if (ev & (POLLERR | POLLHUP)) { t->up = 0; t->state = -1; sd_log("video: the TURN server's connection closed"); continue; }
                if (ev & POLLOUT) turn_flush(r);
                if (ev & POLLIN) turn_stream_input(r);
            }
            if (t->state < 0) turn_stream_close(t);
        } else {
            for (int i = 0; i < MAX_TCP; i++) {
                if (r->tcp[i].fd != fd) continue;
                if (ev & POLLOUT) tcp_flush(r, i);
                if (r->tcp[i].fd >= 0 && (ev & (POLLIN | POLLHUP | POLLERR))) tcp_input(r, i);
            }
        }
    }
}

int rtc_timeout(Rtc *r) {
    if (!r || r->failed) return -1;
    int t = 1000;
    if (r->sctp_on) t = 10;
    if (r->answer_due && !r->answered) t = 50;
    if (r->ssl && !r->dtls_up && r->offered) {
        struct timeval tv;
        if (DTLSv1_get_timeout(r->ssl, &tv)) { int ms = (int)(tv.tv_sec * 1000 + tv.tv_usec / 1000); if (ms < t) t = ms < 1 ? 1 : ms; }
    }
    return t;
}

void rtc_tick(Rtc *r) {
    if (!r || r->failed) return;
    double t = now_ms();
    if (!r->answered && r->answer_due && (t >= r->answer_due || ((!r->nstun_srv || r->srflx.family) && (r->turn.state <= 0 || r->turn.state == 3)))) send_answer(r);
    if (r->ssl && !r->dtls_up && r->offered) DTLSv1_handle_timeout(r->ssl);
    if (r->sctp_on) {
        uint32_t el = (uint32_t)(t - r->sctp_t);
        if (el >= 1) { r->sctp_t = t; usrsctp_handle_timers(el); }
    }
    if (r->srtp_up && t - r->last_sr > 1000) { send_sr(r, &r->v); send_sr(r, &r->a); r->last_sr = t; }
    for (int i = 0; i < MAX_TCP; i++)
        if (r->tcp[i].fd >= 0 && !r->tcp[i].checked && t - r->tcp[i].born > TCP_CHECK_MS) tcp_close(r, i, "no valid ICE check");
    Turn *tu = &r->turn;
    if (tu->state == 1 || tu->state == 2) {
        if (tu->transport == RW_TURN_UDP) {
            if (t - tu->sent_at > 500) { if (++tu->tries > 6) { tu->state = -1; wipe_pass(r); sd_log("video: no answer from the TURN server"); } else turn_send(r, RW_TURN_ALLOCATE_REQ, tu->state == 2, NULL, 0, 600); }
        } else if (t - tu->started > 8000) {           /* a stream is reliable: no retransmissions, one deadline */
            tu->state = -1;
            wipe_pass(r);
            sd_log("video: no answer from the TURN server over %s", rw_turn_transport_name(tu->transport));
            turn_stream_close(tu);
        }
    } else if (tu->state == 3) {
        if (t >= tu->refresh_at) { turn_send(r, RW_TURN_REFRESH_REQ, 1, NULL, 0, 600); tu->refresh_at = t + 60000; }
        if (tu->perm_dirty || (tu->nperm && t >= tu->perm_at)) turn_perms(r);
    } else if (tu->state < 0 && tu->fd >= 0) turn_stream_close(tu);
    if (r->offered && r->dtls_up && t - r->last_consent > CONSENT_MS) fail(r, RTC_WHY_LOST, "the browser stopped answering (no ICE checks)");
    if (r->offered && !r->dtls_up && t - r->created > CONNECT_MS)
        fail(r, r->checks ? RTC_WHY_DTLS : RTC_WHY_NOROUTE, r->checks ? "no DTLS after 20 s (ICE checks arrived)" : "no connection after 20 s (no ICE check arrived over UDP or TCP)");
}

int rtc_turn_key(const Rtc *r, uint8_t key[16], char realm[256]) {
    if (!r || r->turn.state != 3) return -1;
    memcpy(key, r->turn.key, 16);
    memcpy(realm, r->turn.realm, 256);
    return 0;
}

const char *rtc_path_kind(const Rtc *r) {
    const Path *s = r && r->sel >= 0 ? &r->path[r->sel] : NULL;
    return !s ? "" : s->relay ? "relay" : s->tcp ? "tcp" : "udp";
}

int rtc_send(Rtc *r, int chan, const void *p, size_t n, int binary) {
    if (!r || r->failed || chan < 0 || chan > 1 || !r->dc_open[chan] || n > MAX_DC_MSG) return -1;
    return dc_raw(r, r->dc_sid[chan], binary ? 53 : 51, p, n);
}

int rtc_dc_open(const Rtc *r, int chan) { return r && !r->failed && chan >= 0 && chan < 2 && r->dc_open[chan]; }

void rtc_stats(const Rtc *r, char *out, size_t n) {
    if (!r) { snprintf(out, n, "-"); return; }
    snprintf(out, n, "%s %u kbps, %.1f %% lost, %lu NACK (%lu resent), %lu PLI", r->codec == RW_CODEC_H264 ? "h264" : r->codec ? "vp8" : "no codec yet", r->kbps,
             r->lost * 100, r->nacks, r->resent, r->plis);
}
