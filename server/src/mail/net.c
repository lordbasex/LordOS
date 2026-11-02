/* net.c — see net.h. */
#include "net.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

struct Conn {
    int fd;
    SSL *ssl;
    char host[256];
    int timeout;
    char rb[16384];
    size_t rpos, rlen;
    char err[160];
};

static SSL_CTX *ctx;

int net_init(void) {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return -1;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    /* the whole bundle is read now: inside the sandbox there is no /etc */
    static const char *const bundles[] = { "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", NULL };
    int ok = 0;
    for (int i = 0; bundles[i] && !ok; i++) ok = SSL_CTX_load_verify_locations(ctx, bundles[i], NULL) == 1;
    if (!ok) { fprintf(stderr, "axia-maild: no CA bundle found; every TLS connection will fail verification\n"); }
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    return 0;
}

Conn *conn_new(int fd, const char *host, int timeout_ms) {
    Conn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd = fd;
    snprintf(c->host, sizeof c->host, "%s", host ? host : "");
    conn_set_timeout(c, timeout_ms);
    return c;
}

void conn_set_timeout(Conn *c, int ms) {
    c->timeout = ms > 0 ? ms : 60000;
    struct timeval tv = { c->timeout / 1000, (c->timeout % 1000) * 1000 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

void conn_close(Conn *c) {
    if (!c) return;
    if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); }
    if (c->fd >= 0) close(c->fd);
    free(c);
}

int conn_fd(const Conn *c) { return c->fd; }
int conn_is_tls(const Conn *c) { return c->ssl != NULL; }
const char *conn_error(const Conn *c) { return c->err; }

static void fingerprint(X509 *x, char *out, size_t cap) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    out[0] = 0;
    if (!X509_digest(x, EVP_sha256(), md, &n)) return;
    size_t o = 0;
    for (unsigned i = 0; i < n && o + 4 < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%s%02X", i ? ":" : "", md[i]);
}

static void name_line(X509_NAME *nm, char *out, size_t cap) {
    out[0] = 0;
    if (!nm) return;
    X509_NAME_oneline(nm, out, (int)cap);
}

int conn_tls(Conn *c, const char *pin, TlsInfo *info) {
    memset(info, 0, sizeof *info);
    if (!ctx) { snprintf(c->err, sizeof c->err, "tls-init"); return -1; }
    c->ssl = SSL_new(ctx);
    if (!c->ssl) { snprintf(c->err, sizeof c->err, "tls-memory"); return -1; }
    SSL_set_fd(c->ssl, c->fd);
    SSL_set_tlsext_host_name(c->ssl, c->host);
    SSL_set1_host(c->ssl, c->host);            /* host name check (SAN / CN) */
    /* verification errors are collected, not fatal inside OpenSSL: we decide below */
    SSL_set_verify(c->ssl, SSL_VERIFY_NONE, NULL);
    int r = SSL_connect(c->ssl);
    if (r != 1) {
        unsigned long e = ERR_get_error();
        snprintf(c->err, sizeof c->err, "tls-handshake: %s", e ? ERR_reason_error_string(e) : "closed");
        snprintf(info->reason, sizeof info->reason, "%s", c->err);
        return -1;
    }
    snprintf(info->version, sizeof info->version, "%s", SSL_get_version(c->ssl));
    X509 *peer = SSL_get1_peer_certificate(c->ssl);
    long vr = SSL_get_verify_result(c->ssl);
    if (!peer) { info->code = X509_V_ERR_UNSPECIFIED; snprintf(info->reason, sizeof info->reason, "no certificate"); snprintf(c->err, sizeof c->err, "cert"); return -1; }
    fingerprint(peer, info->sha256, sizeof info->sha256);
    name_line(X509_get_subject_name(peer), info->subject, sizeof info->subject);
    name_line(X509_get_issuer_name(peer), info->issuer, sizeof info->issuer);
    const ASN1_TIME *na = X509_get0_notAfter(peer);
    if (na) {
        BIO *b = BIO_new(BIO_s_mem());
        if (b) { ASN1_TIME_print(b, na); int n = BIO_read(b, info->not_after, (int)sizeof info->not_after - 1); if (n > 0) info->not_after[n] = 0; BIO_free(b); }
    }
    X509_free(peer);
    if (vr != X509_V_OK) {
        info->code = (int)vr;
        snprintf(info->reason, sizeof info->reason, "%s", X509_verify_cert_error_string(vr));
        if (pin && *pin && !strcasecmp(pin, info->sha256)) { info->pinned = 1; return 0; }
        snprintf(c->err, sizeof c->err, "cert");
        return -1;
    }
    return 0;
}

static int wait_in(Conn *c, int ms) {
    if (c->ssl && SSL_pending(c->ssl) > 0) return 1;
    struct pollfd p = { c->fd, POLLIN, 0 };
    int r;
    do r = poll(&p, 1, ms); while (r < 0 && errno == EINTR);
    return r;
}

static int fill(Conn *c) {
    if (c->rpos < c->rlen) return 1;
    c->rpos = c->rlen = 0;
    int w = wait_in(c, c->timeout);
    if (w == 0) { snprintf(c->err, sizeof c->err, "timeout"); return 0; }
    if (w < 0) { snprintf(c->err, sizeof c->err, "poll"); return 0; }
    ssize_t n;
    if (c->ssl) {
        n = SSL_read(c->ssl, c->rb, (int)sizeof c->rb);
        if (n <= 0) { snprintf(c->err, sizeof c->err, "closed"); return 0; }
    } else {
        do n = read(c->fd, c->rb, sizeof c->rb); while (n < 0 && errno == EINTR);
        if (n <= 0) { snprintf(c->err, sizeof c->err, n ? "read" : "closed"); return 0; }
    }
    c->rlen = (size_t)n;
    return 1;
}

char *conn_line(Conn *c, size_t *len, size_t max) {
    size_t n = 0, cap = 256;
    char *o = malloc(cap);
    if (!o) return NULL;
    for (;;) {
        if (!fill(c)) { free(o); return NULL; }
        char *nl = memchr(c->rb + c->rpos, '\n', c->rlen - c->rpos);
        size_t take = nl ? (size_t)(nl - (c->rb + c->rpos)) + 1 : c->rlen - c->rpos;
        if (n + take > max) { free(o); snprintf(c->err, sizeof c->err, "line too long"); return NULL; }
        if (n + take + 1 > cap) {
            while (n + take + 1 > cap) cap *= 2;
            if (cap > max + 1) cap = max + 1;            /* n + take <= max, so the NUL still fits */
            char *no = realloc(o, cap);
            if (!no) { free(o); return NULL; }
            o = no;
        }
        memcpy(o + n, c->rb + c->rpos, take);
        n += take;
        c->rpos += take;
        if (nl) break;
    }
    o[n] = 0;
    *len = n;
    return o;
}

int conn_read_n(Conn *c, size_t want, char **buf, size_t *len, size_t *cap) {
    if (*len >= SIZE_MAX - 1 || want > SIZE_MAX - *len - 1) return -1;
    size_t need = *len + want + 1;               /* always room for the terminating NUL */
    if (need > *cap) {
        size_t nc = *cap ? *cap : 1024;
        while (nc < need) { if (nc > SIZE_MAX / 2) { nc = need; break; } nc *= 2; }
        char *nb = realloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb; *cap = nc;
    }
    while (want) {
        if (!fill(c)) return -1;
        size_t take = c->rlen - c->rpos < want ? c->rlen - c->rpos : want;
        memcpy(*buf + *len, c->rb + c->rpos, take);
        *len += take;
        c->rpos += take;
        want -= take;
    }
    (*buf)[*len] = 0;
    return 0;
}

ssize_t conn_read_some(Conn *c, void *out, size_t n) {
    if (!fill(c)) return -1;
    size_t take = c->rlen - c->rpos < n ? c->rlen - c->rpos : n;
    memcpy(out, c->rb + c->rpos, take);
    c->rpos += take;
    return (ssize_t)take;
}

int conn_write(Conn *c, const void *p, size_t n) {
    const char *s = p;
    while (n) {
        ssize_t w;
        if (c->ssl) {
            w = SSL_write(c->ssl, s, (int)(n > (1u << 30) ? (1u << 30) : n));
            if (w <= 0) { snprintf(c->err, sizeof c->err, "write"); return -1; }
        } else {
            do w = write(c->fd, s, n); while (w < 0 && errno == EINTR);
            if (w <= 0) { snprintf(c->err, sizeof c->err, "write"); return -1; }
        }
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

int conn_printf(Conn *c, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0) return -1;
    if ((size_t)k < sizeof tmp) return conn_write(c, tmp, (size_t)k);
    char *big = malloc((size_t)k + 1);
    if (!big) return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)k + 1, fmt, ap);
    va_end(ap);
    int r = conn_write(c, big, (size_t)k);
    free(big);
    return r;
}

int conn_pending(const Conn *c) { return (c->rpos < c->rlen) || (c->ssl && SSL_pending(c->ssl) > 0); }

int conn_wait(Conn *c, int wake_fd, int ms) {
    if (conn_pending(c)) return 1;
    struct pollfd p[2] = { { c->fd, POLLIN, 0 }, { wake_fd, POLLIN, 0 } };
    int r;
    do r = poll(p, wake_fd >= 0 ? 2 : 1, ms); while (r < 0 && errno == EINTR);
    if (r < 0) return -1;
    if (r == 0) return 0;
    if (wake_fd >= 0 && (p[1].revents & POLLIN)) return 2;
    return 1;
}
