/* lordsql.c — lord-sql: one MySQL / MariaDB connection for the Databases app, sandboxed (see
 * sqlproto.h and docs/DATABASES.md). lord-server connected the socket (fd 4) and talks to us on
 * fd 3. We load the CA bundle, lock ourselves in with sandbox_apply_helper and only then read the
 * server's first byte. With lord-sessiond, lord-maild, lord-fetch and lord-tls, the only binaries
 * that link OpenSSL: TLS starts in the middle of the MySQL handshake, and caching_sha2_password
 * without TLS encrypts the password with the server's RSA key.
 *
 * The password arrives in memory in SQ_OPEN, is used for the handshake and is zeroed right after;
 * it is never written anywhere. Nothing from the server is logged. */
#include "sqlproto.h"
#include "mysqlproto.h"
#include "../doc/wire.h"
#include "../sec/json.h"
#include "../sec/sandbox.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#define CHAN 3
#define SOCK 4

enum { TLS_OFF = 0, TLS_PREFERRED = 1, TLS_REQUIRED = 2, TLS_VERIFY = 3 };

typedef struct {
    int fd;
    SSL *ssl;
    uint8_t rb[65536];
    size_t rpos, rlen;
    /* TLS settings and what came of them */
    int mode;
    char host[256], trust[160];
    int verified, pinned;
    char version[24], cipher[96];
    char sha256[100], subject[256], issuer[256], problem[160];
} Io;

static SSL_CTX *ctx;
static Io io;

/* ---- the socket, plain or TLS ---- */

static int io_rd(void *p, void *buf, size_t n) {
    Io *s = p;
    uint8_t *o = buf;
    while (n) {
        if (s->rpos == s->rlen) {
            ssize_t k;
            if (s->ssl) {
                k = SSL_read(s->ssl, s->rb, (int)sizeof s->rb);
                if (k <= 0) return -1;
            } else {
                do k = read(s->fd, s->rb, sizeof s->rb); while (k < 0 && errno == EINTR);
                if (k <= 0) return -1;
            }
            s->rpos = 0;
            s->rlen = (size_t)k;
        }
        size_t take = s->rlen - s->rpos < n ? s->rlen - s->rpos : n;
        memcpy(o, s->rb + s->rpos, take);
        s->rpos += take;
        o += take;
        n -= take;
    }
    return 0;
}

static int io_wr(void *p, const void *buf, size_t n) {
    Io *s = p;
    const uint8_t *b = buf;
    while (n) {
        ssize_t k;
        if (s->ssl) {
            k = SSL_write(s->ssl, b, (int)(n > (1u << 30) ? (1u << 30) : n));
            if (k <= 0) return -1;
        } else {
            do k = write(s->fd, b, n); while (k < 0 && errno == EINTR);
            if (k <= 0) return -1;
        }
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

static void fingerprint(X509 *x, char *out, size_t cap) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    out[0] = 0;
    if (!X509_digest(x, EVP_sha256(), md, &n)) return;
    size_t o = 0;
    for (unsigned i = 0; i < n && o + 3 < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%s%02X", i ? ":" : "", md[i]);
}

/* "AB:CD:…" or "abcd…": the same fingerprint, ignoring case and colons */
static int same_fp(const char *a, const char *b) {
    if (!a[0] || !b[0]) return 0;
    for (;;) {
        while (*a == ':') a++;
        while (*b == ':') b++;
        if (!*a || !*b) return !*a && !*b;
        if ((*a | 0x20) != (*b | 0x20)) return 0;
        a++, b++;
    }
}

static int io_tls(void *p) {
    Io *s = p;
    if (!ctx || s->rpos != s->rlen) return MY_E_TLS;     /* nothing may come before the server's TLS */
    s->ssl = SSL_new(ctx);
    if (!s->ssl) return MY_E_TLS;
    SSL_set_fd(s->ssl, s->fd);
    unsigned char addr[16];
    int ip = inet_pton(AF_INET, s->host, addr) == 1 || inet_pton(AF_INET6, s->host, addr) == 1;
    X509_VERIFY_PARAM *vp = SSL_get0_param(s->ssl);
    if (ip) X509_VERIFY_PARAM_set1_ip_asc(vp, s->host);
    else if (s->host[0]) {
        SSL_set_tlsext_host_name(s->ssl, s->host);
        SSL_set1_host(s->ssl, s->host);
    }
    /* verification errors are collected, not fatal inside OpenSSL: the mode decides below */
    SSL_set_verify(s->ssl, SSL_VERIFY_NONE, NULL);
    if (SSL_connect(s->ssl) != 1) {
        unsigned long e = ERR_get_error();
        snprintf(s->problem, sizeof s->problem, "%s", e ? ERR_reason_error_string(e) : "closed");
        return MY_E_TLS;
    }
    snprintf(s->version, sizeof s->version, "%s", SSL_get_version(s->ssl));
    snprintf(s->cipher, sizeof s->cipher, "%s", SSL_get_cipher_name(s->ssl));
    X509 *peer = SSL_get1_peer_certificate(s->ssl);
    long vr = peer ? SSL_get_verify_result(s->ssl) : X509_V_ERR_UNSPECIFIED;
    if (peer) {
        fingerprint(peer, s->sha256, sizeof s->sha256);
        X509_NAME_oneline(X509_get_subject_name(peer), s->subject, (int)sizeof s->subject);
        X509_NAME_oneline(X509_get_issuer_name(peer), s->issuer, (int)sizeof s->issuer);
        X509_free(peer);
    }
    s->verified = vr == X509_V_OK;
    if (!s->host[0]) s->verified = 0;            /* no name to check it against */
    if (!s->verified) snprintf(s->problem, sizeof s->problem, "%s", peer ? X509_verify_cert_error_string(vr) : "no certificate");
    if (s->mode == TLS_VERIFY && !s->verified) {
        if (same_fp(s->trust, s->sha256)) { s->pinned = 1; return MY_OK; }
        return MY_E_CERT;
    }
    return MY_OK;
}

/* ---- replies ---- */

static void send_json(uint32_t op, uint32_t id, const char *j, size_t n) {
    if (op == SR_OK && n > DOC_INLINE_MAX) {
        int fd = doc_memfd("lord-sql-reply", j, n);
        if (fd >= 0) { doc_send(CHAN, SR_BIG, id, "{}", 2, &fd, 1); close(fd); return; }
        doc_send(CHAN, SR_ERR, id, "{\"error\":\"toolarge\"}", 20, NULL, 0);
        return;
    }
    doc_send(CHAN, op, id, j, n, NULL, 0);
}

static void reply_err(uint32_t id, const char *code, const MyErr *e, const char *results, size_t rlen) {
    MyBuf b;
    mb_init(&b, SQL_REPLY_MAX);
    mb_raw(&b, "{\"error\":");
    mb_strn(&b, code, strlen(code));
    if (e && e->code) {
        mb_printf(&b, ",\"errno\":%d,\"sqlstate\":", e->code);
        mb_strn(&b, e->state, strlen(e->state));
        mb_raw(&b, ",\"message\":");
        mb_strn(&b, e->msg, strlen(e->msg));
    }
    if (results) { mb_raw(&b, ",\"results\":"); mb_rawn(&b, results, rlen); }
    mb_raw(&b, "}");
    if (b.oom) { mb_free(&b); mb_init(&b, 0); mb_raw(&b, "{\"error\":"); mb_strn(&b, code, strlen(code)); mb_raw(&b, "}"); }
    /* SR_ERR with partial results can be large: those go as SR_BIG with "error" inside */
    if (b.n > DOC_INLINE_MAX) {
        int fd = doc_memfd("lord-sql-reply", b.p, b.n);
        if (fd >= 0) { doc_send(CHAN, SR_BIG, id, "{\"err\":1}", 9, &fd, 1); close(fd); }
    } else doc_send(CHAN, SR_ERR, id, b.p, b.n, NULL, 0);
    mb_free(&b);
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* ---- before the sandbox: the TLS context, CA bundle and client certificate ---- */

static char *read_fd(int fd, size_t max, size_t *len) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (size_t)st.st_size > max) return NULL;
    char *b = malloc((size_t)st.st_size + 1);
    if (!b) return NULL;
    size_t o = 0;
    while (o < (size_t)st.st_size) {
        ssize_t k = read(fd, b + o, (size_t)st.st_size - o);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        o += (size_t)k;
    }
    b[o] = 0;
    *len = o;
    return b;
}

static int load_ca_fd(SSL_CTX *x, int fd) {
    size_t n = 0;
    char *pem = read_fd(fd, 4u << 20, &n);
    if (!pem) return -1;
    BIO *b = BIO_new_mem_buf(pem, (int)n);
    STACK_OF(X509_INFO) *list = b ? PEM_X509_INFO_read_bio(b, NULL, NULL, NULL) : NULL;
    X509_STORE *st = SSL_CTX_get_cert_store(x);
    int count = 0;
    for (int i = 0; list && i < sk_X509_INFO_num(list); i++) {
        X509_INFO *in = sk_X509_INFO_value(list, i);
        if (in->x509 && X509_STORE_add_cert(st, in->x509) == 1) count++;
    }
    sk_X509_INFO_pop_free(list, X509_INFO_free);
    BIO_free(b);
    free(pem);
    return count ? 0 : -1;
}

static int load_client_cert(SSL_CTX *x, int cfd, int kfd) {
    size_t cn = 0, kn = 0;
    char *c = read_fd(cfd, 1u << 20, &cn), *k = read_fd(kfd, 1u << 20, &kn);
    int ok = 0;
    if (c && k) {
        BIO *cb = BIO_new_mem_buf(c, (int)cn), *kb = BIO_new_mem_buf(k, (int)kn);
        X509 *cert = cb ? PEM_read_bio_X509(cb, NULL, NULL, NULL) : NULL;
        /* an empty passphrase callback: an encrypted key is refused, never prompted for */
        EVP_PKEY *key = kb ? PEM_read_bio_PrivateKey(kb, NULL, NULL, (void *)"") : NULL;
        ok = cert && key && SSL_CTX_use_certificate(x, cert) == 1 && SSL_CTX_use_PrivateKey(x, key) == 1 &&
             SSL_CTX_check_private_key(x) == 1;
        X509_free(cert);
        EVP_PKEY_free(key);
        BIO_free(cb);
        BIO_free(kb);
    }
    if (k) { explicit_bzero(k, kn); free(k); }
    free(c);
    return ok ? 0 : -1;
}

static void init_tls(void) {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS | OPENSSL_INIT_LOAD_CONFIG, NULL);
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    /* what the handshake uses later, fetched now while files can still be read */
    uint8_t d[32];
    my_scramble_sha2((const uint8_t *)"0123456789abcdefghij", 20, "x", d);
    my_scramble_native((const uint8_t *)"0123456789abcdefghij", 20, "x", d);
}

static void load_system_ca(void) {
    static const char *const bundles[] = { "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", NULL };
    int ok = 0;
    for (int i = 0; bundles[i] && !ok; i++) ok = SSL_CTX_load_verify_locations(ctx, bundles[i], NULL) == 1;
    if (!ok) fprintf(stderr, "lord-sql: no CA bundle found; certificates will not verify\n");
}

/* ---- requests ---- */

static int auth_errno(int code) {
    switch (code) {
    case 1044: case 1045: case 1129: case 1130: case 1131: case 1132: case 1227: case 1251: case 1698:
    case 1820: case 1862: case 2049: case 3118: case 3159: case 4150: return 1;
    default: return 0;
    }
}

static void do_query(uint32_t id, MyConn *c, const char *json, size_t len) {
    JDoc d;
    if (json_parse(json, len, &d, NULL, 0) || !d.root || d.root->type != J_OBJ) { reply_err(id, "unsupported", NULL, NULL, 0); return; }
    const JVal *sv = json_get(d.root, "sql");
    double mr = json_num(d.root, "maxRows", SQL_ROWS_DEFAULT);
    long max_rows = mr >= 1 && mr <= SQL_ROWS_MAX ? (long)mr : SQL_ROWS_DEFAULT;
    if (!sv || sv->type != J_STR) { json_free(&d); reply_err(id, "unsupported", NULL, NULL, 0); return; }
    MyBuf out;
    mb_init(&out, SQL_REPLY_MAX);
    mb_raw(&out, "{\"results\":");
    size_t start = out.n;
    double t0 = now_ms();
    int rc = my_query(c, sv->str, sv->len, max_rows, SQL_RESULT_MAX + start, &out);
    json_free(&d);
    if (rc == MY_OK) {
        mb_printf(&out, ",\"ms\":%.0f}", now_ms() - t0);
        if (out.oom) { mb_free(&out); reply_err(id, "toolarge", NULL, NULL, 0); return; }
        send_json(SR_OK, id, out.p, out.n);
        mb_free(&out);
        return;
    }
    if (rc == MY_E_SQL && !out.oom) {
        reply_err(id, "sql", &c->err, out.p + start, out.n - start);
        mb_free(&out);
        return;
    }
    /* the connection is no longer in step with the server: end it */
    mb_free(&out);
    reply_err(id, rc == MY_E_OOM ? "toolarge" : "closed", NULL, NULL, 0);
    _exit(0);
}

static void do_simple(uint32_t id, MyConn *c, uint8_t cmd, const char *json, size_t len) {
    const char *arg = "";
    size_t alen = 0;
    JDoc d;
    int have = 0;
    if (cmd == MY_COM_INIT_DB) {
        if (json_parse(json, len, &d, NULL, 0) || !d.root || d.root->type != J_OBJ) { reply_err(id, "unsupported", NULL, NULL, 0); return; }
        have = 1;
        const JVal *v = json_get(d.root, "db");
        if (!v || v->type != J_STR || !v->len || v->len > 256) { json_free(&d); reply_err(id, "unsupported", NULL, NULL, 0); return; }
        arg = v->str;
        alen = v->len;
    }
    double t0 = now_ms();
    int rc = my_command(c, cmd, arg, alen);
    if (have) json_free(&d);
    if (rc == MY_OK) {
        char j[64];
        if (cmd == MY_COM_PING) snprintf(j, sizeof j, "{\"ms\":%.0f}", now_ms() - t0);
        else snprintf(j, sizeof j, "{}");
        send_json(SR_OK, id, j, strlen(j));
        return;
    }
    if (rc == MY_E_SQL) { reply_err(id, "sql", &c->err, NULL, 0); return; }
    reply_err(id, "closed", NULL, NULL, 0);
    _exit(0);
}

/* While no request runs, the server has nothing to say: anything from it (the ERR of a timeout,
 * the end of the connection) ends this connection. buffered: bytes already read and waiting.
 * readable: the socket polled readable; TLS 1.3 may still send session tickets, which are read
 * here and are not data. */
static int server_buffered(void) { return io.rpos != io.rlen || (io.ssl && SSL_pending(io.ssl) > 0); }

static int server_spoke(void) {
    if (server_buffered() || !io.ssl) return 1;
    int fl = fcntl(io.fd, F_GETFL);
    if (fl < 0 || fcntl(io.fd, F_SETFL, fl | O_NONBLOCK) < 0) return 1;
    int k = SSL_read(io.ssl, io.rb, (int)sizeof io.rb);
    int e = k > 0 ? 0 : SSL_get_error(io.ssl, k);
    fcntl(io.fd, F_SETFL, fl);
    if (k > 0) { io.rpos = 0; io.rlen = (size_t)k; return 1; }
    return e != SSL_ERROR_WANT_READ;
}

int main(void) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    prctl(PR_SET_DUMPABLE, 0);
    struct rlimit nocore = { 0, 0 };
    setrlimit(RLIMIT_CORE, &nocore);
    signal(SIGPIPE, SIG_IGN);
    struct stat st;
    if (fstat(CHAN, &st) || !S_ISSOCK(st.st_mode) || fstat(SOCK, &st) || !S_ISSOCK(st.st_mode)) {
        fprintf(stderr, "lord-sql: started by lord-server only\n");
        return 2;
    }
    DocMsg m;
    if (doc_recv(CHAN, &m) || m.h.op != SQ_OPEN || !m.json) { fprintf(stderr, "lord-sql: no open request\n"); return 2; }
    uint32_t oid = m.h.id;
    JDoc d;
    if (json_parse(m.json, m.len, &d, NULL, 0) || !d.root || d.root->type != J_OBJ) {
        explicit_bzero(m.json, m.len);
        doc_msg_free(&m);
        reply_err(oid, "unsupported", NULL, NULL, 0);
        return 2;
    }
    explicit_bzero(m.json, m.len);
    const char *mode = json_str(d.root, "tls"), *host = json_str(d.root, "host"), *trust = json_str(d.root, "trust");
    io.fd = SOCK;
    io.mode = !mode || !strcmp(mode, "off") ? TLS_OFF : !strcmp(mode, "required") ? TLS_REQUIRED : !strcmp(mode, "verify") ? TLS_VERIFY : TLS_PREFERRED;
    snprintf(io.host, sizeof io.host, "%s", host ? host : "");
    snprintf(io.trust, sizeof io.trust, "%s", trust ? trust : "");
    int fi = 0, want_ca = json_num(d.root, "ca", 0) == 1, want_cert = json_num(d.root, "cert", 0) == 1, want_key = json_num(d.root, "key", 0) == 1;
    int ca_fd = want_ca && fi < m.nfds ? m.fds[fi++] : -1;
    int cert_fd = want_cert && fi < m.nfds ? m.fds[fi++] : -1;
    int key_fd = want_key && fi < m.nfds ? m.fds[fi++] : -1;
    init_tls();
    const char *tls_fail = NULL;
    if (io.mode != TLS_OFF && ctx) {
        if (want_ca) { if (ca_fd < 0 || load_ca_fd(ctx, ca_fd)) tls_fail = "ca"; }
        else load_system_ca();
        if ((want_cert || want_key) && (cert_fd < 0 || key_fd < 0 || load_client_cert(ctx, cert_fd, key_fd))) tls_fail = "client-cert";
    }
    for (int i = 0; i < m.nfds; i++) if (m.fds[i] >= 0) close(m.fds[i]);
    m.nfds = 0;
    doc_msg_free(&m);
    SandboxHelperCfg cfg = {
        .name = "lordos-sql",           /* cgroup scope lordos-sql.scope */
        .ro_paths = NULL,
        .extra_syscalls = (int[]){ -1 },
        .allow_ioctl_classes = 0,
        .mem_max_mb = 768,
        .ro_dir_fds = NULL,
        .rw_dir_fds = NULL,
    };
    sandbox_apply_helper(&cfg);
    if (tls_fail) {
        const JVal *pv = json_get(d.root, "password");
        if (pv && pv->type == J_STR) explicit_bzero((char *)pv->str, pv->len);
        json_free(&d);
        char j[64];
        snprintf(j, sizeof j, "{\"error\":\"tls\",\"message\":\"%s\"}", tls_fail);
        doc_send(CHAN, SR_ERR, oid, j, strlen(j), NULL, 0);
        return 1;
    }
    /* from here on, bytes from the server */
    MyConn c;
    my_init(&c);
    c.rd = io_rd;
    c.wr = io_wr;
    c.tls = io_tls;
    c.io = &io;
    const JVal *pw = json_get(d.root, "password");
    const char *user = json_str(d.root, "user"), *db = json_str(d.root, "database");
    MyLogin login = { user ? user : "", pw && pw->type == J_STR ? pw->str : "", db && db[0] ? db : NULL,
                      io.mode == TLS_OFF ? 0 : io.mode == TLS_PREFERRED ? 1 : 2 };
    MyHandshake hs;
    int rc = my_connect(&c, &login, &hs);
    if (pw && pw->type == J_STR) explicit_bzero((char *)pw->str, pw->len);
    char dbname[260];
    snprintf(dbname, sizeof dbname, "%s", login.db ? login.db : "");
    json_free(&d);
    if (rc == MY_E_CERT) {
        MyBuf b;
        mb_init(&b, 0);
        mb_raw(&b, "{\"ev\":\"cert\",\"sha256\":");
        mb_strn(&b, io.sha256, strlen(io.sha256));
        mb_raw(&b, ",\"subject\":");
        mb_strn(&b, io.subject, strlen(io.subject));
        mb_raw(&b, ",\"issuer\":");
        mb_strn(&b, io.issuer, strlen(io.issuer));
        mb_raw(&b, ",\"problem\":");
        mb_strn(&b, io.problem, strlen(io.problem));
        mb_raw(&b, "}");
        doc_send(CHAN, SR_EVT, oid, b.p, b.n, NULL, 0);
        mb_free(&b);
        reply_err(oid, "cert", NULL, NULL, 0);
        return 1;
    }
    if (rc) {
        const char *code = rc == MY_E_TLS ? "tls" : rc == MY_E_UNSUPPORTED ? "unsupported"
                         : rc == MY_E_AUTH ? (c.err.code == 1049 ? "sql" : "auth")
                         : rc == MY_E_SQL ? (auth_errno(c.err.code) ? "auth" : "connect") : "connect";
        reply_err(oid, code, rc == MY_E_AUTH || rc == MY_E_SQL ? &c.err : NULL, NULL, 0);
        return 1;
    }
    MyBuf b;
    mb_init(&b, 0);
    mb_raw(&b, "{\"server\":{\"version\":");
    mb_strn(&b, hs.version, strlen(hs.version));
    mb_printf(&b, ",\"flavor\":\"%s\"},\"threadId\":%u,\"tls\":", hs.mariadb ? "mariadb" : "mysql", hs.thread_id);
    if (c.is_tls) {
        mb_raw(&b, "{\"version\":");
        mb_strn(&b, io.version, strlen(io.version));
        mb_raw(&b, ",\"cipher\":");
        mb_strn(&b, io.cipher, strlen(io.cipher));
        mb_printf(&b, ",\"verified\":%s,\"pinned\":%s,\"sha256\":", io.verified ? "true" : "false", io.pinned ? "true" : "false");
        mb_strn(&b, io.sha256, strlen(io.sha256));
        mb_raw(&b, "}");
    } else mb_raw(&b, "null");
    mb_raw(&b, ",\"db\":");
    if (dbname[0]) mb_strn(&b, dbname, strlen(dbname));
    else mb_raw(&b, "null");
    mb_raw(&b, "}");
    send_json(SR_OK, oid, b.p, b.n);
    mb_free(&b);
    for (;;) {
        if (server_buffered()) return 0;
        struct pollfd p[2] = { { CHAN, POLLIN, 0 }, { SOCK, POLLIN, 0 } };
        int r = poll(p, 2, -1);
        if (r < 0) { if (errno == EINTR) continue; return 1; }
        if (p[1].revents) {
            if (server_spoke()) return 0;
            continue;
        }
        if (!(p[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        DocMsg q;
        int k = doc_recv(CHAN, &q);
        if (k) return k == 1 ? 0 : 1;
        for (int i = 0; i < q.nfds; i++) close(q.fds[i]);
        q.nfds = 0;
        switch (q.h.op) {
        case SQ_QUERY: do_query(q.h.id, &c, q.json ? q.json : "", q.len); break;
        case SQ_USE: do_simple(q.h.id, &c, MY_COM_INIT_DB, q.json ? q.json : "", q.len); break;
        case SQ_PING: do_simple(q.h.id, &c, MY_COM_PING, NULL, 0); break;
        case SQ_END: my_quit(&c); doc_msg_free(&q); return 0;
        default: reply_err(q.h.id, "unsupported", NULL, NULL, 0); break;
        }
        doc_msg_free(&q);
    }
}
