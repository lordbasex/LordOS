/* sql_e2e_test.c — lord-sql (build/lord-sql) end to end against a fake MySQL / MariaDB server that runs in this
 * test, over a socketpair: the real helper, sandboxed, with its channel on fd 3 and the "server" on fd 4. Covers
 * mysql_native_password, caching_sha2_password (fast path, full auth with the server's RSA key, full auth over
 * TLS), the auth switch, a refused login, an ERR greeting and a garbage one, TLS (preferred, verify with the
 * cert event, a trusted fingerprint, a CA file, a wrong host name, required without TLS), queries with two result
 * sets, NULLs, binary values, an ERR, an ERR after a first result, maxRows, the 64 MB limit (sent as a memfd),
 * COM_INIT_DB, COM_PING, COM_QUIT and a server that goes away while idle. Then lord-server's own side
 * (src/sql/sqlc.c, what lord.sql calls) over a TCP listener on 127.0.0.1: favorites, open, query, busy, close.
 * No MySQL server is needed; nothing leaves this machine.
 *
 *   make -C server build/sql-e2e-test        (also in `make test` when OpenSSL's headers are there) */
#include "../src/sql/mysqlproto.h"
#include "../src/sql/sqlproto.h"
#include "../src/sql/sqlc.h"
#include "../src/doc/wire.h"
#include "../src/sec/json.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

static int fails, checks;
static const char *lordsql;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t SCRAMBLE[21] = "abcdefghijklmnopqrst";
static const uint8_t SCRAMBLE2[21] = "ABCDEFGHIJKLMNOPQRST";

/* ---- the certificate and key the fake server uses ---- */
static EVP_PKEY *key;
static X509 *cert;
static char cert_path[128], fp[100];

static void make_cert(void) {
    key = EVP_RSA_gen(2048);
    cert = X509_new();
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 7);
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert), 86400);
    X509_set_pubkey(cert, key);
    X509_NAME *nm = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC, (const unsigned char *)"db.test", -1, -1, 0);
    X509_set_issuer_name(cert, nm);
    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, cert, cert, NULL, NULL, 0);
    X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &v3, NID_subject_alt_name, "DNS:db.test");
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
    ext = X509V3_EXT_conf_nid(NULL, &v3, NID_basic_constraints, "critical,CA:TRUE");
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
    X509_sign(cert, key, EVP_sha256());
    snprintf(cert_path, sizeof cert_path, "/tmp/sql-e2e-ca.%d.pem", (int)getpid());
    FILE *f = fopen(cert_path, "w");
    PEM_write_X509(f, cert);
    fclose(f);
    unsigned char md[32];
    unsigned n = 0;
    X509_digest(cert, EVP_sha256(), md, &n);
    size_t o = 0;
    for (unsigned i = 0; i < n; i++) o += (size_t)snprintf(fp + o, sizeof fp - o, "%s%02X", i ? ":" : "", md[i]);
}

/* ---- the fake server ---- */

typedef struct { uint8_t *p; size_t n, cap; } Buf;
static void put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put8(Buf *b, unsigned v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void put16(Buf *b, unsigned v) { put8(b, v & 255); put8(b, v >> 8); }
static void put32(Buf *b, uint32_t v) { put16(b, v & 0xffff); put16(b, v >> 16); }
static void lenenc(Buf *b, uint64_t v) {
    if (v < 251) put8(b, (unsigned)v);
    else if (v < 65536) { put8(b, 0xfc); put16(b, (unsigned)v); }
    else { put8(b, 0xfd); put8(b, v & 255); put16(b, (unsigned)(v >> 8)); }
}
static void lstr(Buf *b, const char *s) { lenenc(b, strlen(s)); put(b, s, strlen(s)); }

typedef struct {
    /* what the server does */
    const char *version, *plugin;
    uint32_t caps;
    int offer_tls, full, switch_native, deny, greet_err, garbage, session, drop_after_login;
    const char *password;
    /* what it found */
    int login_ok, tls_used;
    char why[160];
    int fd;
    SSL *ssl;
    uint8_t seq;
} Scn;

static int srv_rd(Scn *s, void *buf, size_t n) {
    uint8_t *o = buf;
    while (n) {
        ssize_t k = s->ssl ? SSL_read(s->ssl, o, (int)n) : read(s->fd, o, n);
        if (k <= 0) return -1;
        o += k;
        n -= (size_t)k;
    }
    return 0;
}
static int srv_wr(Scn *s, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t k = s->ssl ? SSL_write(s->ssl, p, (int)n) : write(s->fd, p, n);
        if (k <= 0) return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}
static int srv_send(Scn *s, const Buf *pl) {
    uint8_t h[4] = { pl->n & 255, (pl->n >> 8) & 255, (pl->n >> 16) & 255, s->seq++ };
    return srv_wr(s, h, 4) || srv_wr(s, pl->p, pl->n);
}
static int srv_recv(Scn *s, Buf *pl) {
    uint8_t h[4];
    if (srv_rd(s, h, 4)) return -1;
    size_t n = (size_t)h[0] | (size_t)h[1] << 8 | (size_t)h[2] << 16;
    if (h[3] != s->seq) { snprintf(s->why, sizeof s->why, "client sequence %u, expected %u", h[3], s->seq); return -1; }
    s->seq++;
    pl->n = 0;
    uint8_t *tmp = malloc(n + 1);
    if (n && srv_rd(s, tmp, n)) { free(tmp); return -1; }
    put(pl, tmp, n);
    free(tmp);
    return 0;
}
static void srv_ok(Scn *s, uint8_t head, uint64_t affected, uint64_t insert_id, uint16_t status, uint16_t warnings, const char *info) {
    Buf pl = { 0 };
    put8(&pl, head); lenenc(&pl, affected); lenenc(&pl, insert_id); put16(&pl, status); put16(&pl, warnings);
    if (info) put(&pl, info, strlen(info));
    srv_send(s, &pl);
    free(pl.p);
}
static void srv_err(Scn *s, int code, const char *state, const char *msg) {
    Buf pl = { 0 };
    put8(&pl, 0xff); put16(&pl, (unsigned)code);
    if (state) { put8(&pl, '#'); put(&pl, state, 5); }
    put(&pl, msg, strlen(msg));
    srv_send(s, &pl);
    free(pl.p);
}
static void srv_column(Scn *s, const char *name, uint8_t type, uint16_t charset, uint16_t flags) {
    Buf pl = { 0 };
    lstr(&pl, "def"); lstr(&pl, "shop"); lstr(&pl, "orders"); lstr(&pl, "orders"); lstr(&pl, name); lstr(&pl, name);
    lenenc(&pl, 12); put16(&pl, charset); put32(&pl, 255); put8(&pl, type); put16(&pl, flags); put8(&pl, 0); put16(&pl, 0);
    srv_send(s, &pl);
    free(pl.p);
}
static int deprecate_eof(const Scn *s) { return (s->caps & MY_CLIENT_DEPRECATE_EOF) != 0; }
static void srv_eof(Scn *s, uint16_t status) {
    Buf pl = { 0 };
    put8(&pl, 0xfe); put16(&pl, 0); put16(&pl, status);
    srv_send(s, &pl);
    free(pl.p);
}
static void srv_end_rows(Scn *s, uint16_t status, uint16_t warnings) {
    if (deprecate_eof(s)) srv_ok(s, 0xfe, 0, 0, status, warnings, NULL);
    else { Buf pl = { 0 }; put8(&pl, 0xfe); put16(&pl, warnings); put16(&pl, status); srv_send(s, &pl); free(pl.p); }
}
/* a result set: id INT, name VARCHAR, data VARBINARY; rows of (i, name or NULL, 00 ff) */
static void srv_result(Scn *s, int rows, uint16_t more) {
    Buf pl = { 0 };
    lenenc(&pl, 3); srv_send(s, &pl);
    srv_column(s, "id", 3, 63, MY_NOT_NULL | MY_PRI_KEY | MY_AUTO_INCREMENT | MY_UNSIGNED);
    srv_column(s, "name", 253, 255, 0);
    srv_column(s, "data", 253, 63, MY_BINARY);
    if (!deprecate_eof(s)) srv_eof(s, 0);
    for (int i = 0; i < rows; i++) {
        char id[16];
        snprintf(id, sizeof id, "%d", i + 1);
        pl.n = 0;
        lstr(&pl, id);
        if (i % 2) put8(&pl, 0xfb); else lstr(&pl, "caf\xc3\xa9");
        lenenc(&pl, 2); put8(&pl, 0); put8(&pl, 0xff);
        srv_send(s, &pl);
    }
    srv_end_rows(s, more ? MY_SERVER_MORE_RESULTS : 0, 0);
    free(pl.p);
}

static void session(Scn *s) {
    Buf pl = { 0 };
    for (;;) {
        s->seq = 0;
        if (srv_recv(s, &pl) || !pl.n) break;
        uint8_t cmd = pl.p[0];
        char *arg = strndup((char *)pl.p + 1, pl.n - 1);
        if (cmd == MY_COM_QUIT) { free(arg); break; }
        if (cmd == MY_COM_PING) srv_ok(s, 0, 0, 0, 2, 0, NULL);
        else if (cmd == MY_COM_INIT_DB) { if (!strcmp(arg, "shop2")) srv_ok(s, 0, 0, 0, 2, 0, NULL); else srv_err(s, 1049, "42000", "Unknown database"); }
        else if (cmd == MY_COM_QUERY) {
            if (!strcmp(arg, "SELECT two")) { srv_result(s, 3, 1); srv_ok(s, 0, 3, 7, 0, 1, "Records: 3  Duplicates: 0  Warnings: 1"); }
            else if (!strcmp(arg, "SELECT err")) srv_err(s, 1146, "42S02", "Table 'shop.x' doesn't exist");
            else if (!strcmp(arg, "SELECT one; bad")) { srv_result(s, 1, 1); srv_err(s, 1064, "42000", "You have an error in your SQL syntax"); }
            else if (!strcmp(arg, "SELECT many")) srv_result(s, 10, 0);
            else if (!strcmp(arg, "SELECT huge")) {
                /* 80 rows of 1 MB: past the 64 MB limit */
                Buf r = { 0 };
                lenenc(&r, 1); srv_send(s, &r);
                srv_column(s, "blob", 252, 255, MY_BLOB);
                if (!deprecate_eof(s)) srv_eof(s, 0);
                char *mb = malloc(1 << 20);
                memset(mb, 'x', 1 << 20);
                for (int i = 0; i < 80; i++) { r.n = 0; lenenc(&r, 1 << 20); put(&r, mb, 1 << 20); srv_send(s, &r); }
                free(mb);
                free(r.p);
                srv_end_rows(s, 0, 0);
            } else srv_err(s, 1064, "42000", "unknown test query");
        }
        free(arg);
    }
    free(pl.p);
}

static void *server(void *arg) {
    Scn *s = arg;
    Buf pl = { 0 };
    s->seq = 0;
    if (s->garbage) { put(&pl, "\x09garbage", 8); srv_send(s, &pl); goto out; }
    if (s->greet_err) { srv_err(s, 1130, NULL, "Host '10.0.0.9' is not allowed to connect to this MySQL server"); goto out; }
    uint32_t caps = s->caps | (s->offer_tls ? MY_CLIENT_SSL : 0);
    put8(&pl, 10);
    put(&pl, s->version, strlen(s->version) + 1);
    put32(&pl, 4242);
    put(&pl, SCRAMBLE, 8);
    put8(&pl, 0);
    put16(&pl, caps & 0xffff);
    put8(&pl, 255);
    put16(&pl, 2);
    put16(&pl, caps >> 16);
    put8(&pl, 21);
    put(&pl, "\0\0\0\0\0\0\0\0\0\0", 10);
    put(&pl, SCRAMBLE + 8, 12);
    put8(&pl, 0);
    put(&pl, s->plugin, strlen(s->plugin) + 1);
    srv_send(s, &pl);
    if (srv_recv(s, &pl)) { snprintf(s->why, sizeof s->why, "no login"); goto out; }
    uint32_t cc = (uint32_t)pl.p[0] | (uint32_t)pl.p[1] << 8 | (uint32_t)pl.p[2] << 16 | (uint32_t)pl.p[3] << 24;
    if (cc & MY_CLIENT_SSL) {
        if (pl.n != 32 || !s->offer_tls) { snprintf(s->why, sizeof s->why, "bad SSLRequest"); goto out; }
        SSL_CTX *sc = SSL_CTX_new(TLS_server_method());
        SSL_CTX_use_certificate(sc, cert);
        SSL_CTX_use_PrivateKey(sc, key);
        s->ssl = SSL_new(sc);
        SSL_CTX_free(sc);
        SSL_set_fd(s->ssl, s->fd);
        if (SSL_accept(s->ssl) != 1) { snprintf(s->why, sizeof s->why, "TLS refused by the client"); goto out; }
        s->tls_used = 1;
        if (srv_recv(s, &pl)) { snprintf(s->why, sizeof s->why, "no login after TLS"); goto out; }
        cc = (uint32_t)pl.p[0] | (uint32_t)pl.p[1] << 8 | (uint32_t)pl.p[2] << 16 | (uint32_t)pl.p[3] << 24;
    }
    if (cc & MY_CLIENT_LOCAL_FILES) { snprintf(s->why, sizeof s->why, "client asked for LOCAL FILES"); goto out; }
    /* HandshakeResponse41: user, auth, db, plugin */
    size_t o = 32;
    const char *user = (char *)pl.p + o;
    o += strlen(user) + 1;
    size_t al = pl.p[o++];
    uint8_t auth[64];
    memcpy(auth, pl.p + o, al);
    o += al;
    const char *db = (cc & MY_CLIENT_CONNECT_WITH_DB) ? (char *)pl.p + o : "";
    if (cc & MY_CLIENT_CONNECT_WITH_DB) o += strlen(db) + 1;
    const char *plugin = (char *)pl.p + o;
    if (strcmp(user, "shop") || strcmp(db, "shop")) { snprintf(s->why, sizeof s->why, "user %s db %s", user, db); goto out; }
    if (strcmp(plugin, s->plugin)) { snprintf(s->why, sizeof s->why, "plugin %s", plugin); goto out; }
    if (s->deny) { srv_err(s, 1045, "28000", "Access denied for user 'shop'@'10.0.0.9' (using password: YES)"); goto out; }
    int good = 0;
    if (s->switch_native) {
        Buf sw = { 0 };
        put8(&sw, 0xfe); put(&sw, "mysql_native_password", 22); put(&sw, SCRAMBLE2, 21);
        srv_send(s, &sw);
        free(sw.p);
        if (srv_recv(s, &pl)) goto out;
        uint8_t want[20];
        my_scramble_native(SCRAMBLE2, 20, s->password, want);
        good = pl.n == 20 && !memcmp(pl.p, want, 20);
    } else if (!strcmp(s->plugin, "mysql_native_password")) {
        uint8_t want[20];
        my_scramble_native(SCRAMBLE, 20, s->password, want);
        good = al == 20 && !memcmp(auth, want, 20);
    } else {
        uint8_t want[32];
        my_scramble_sha2(SCRAMBLE, 20, s->password, want);
        if (al != 32 || memcmp(auth, want, 32)) { snprintf(s->why, sizeof s->why, "bad sha2 scramble"); goto out; }
        Buf m = { 0 };
        put8(&m, 1); put8(&m, s->full ? 4 : 3);
        srv_send(s, &m);
        if (!s->full) good = 1;
        else if (s->ssl) {
            /* over TLS: the password itself */
            if (srv_recv(s, &pl)) goto out;
            good = pl.n == strlen(s->password) + 1 && !memcmp(pl.p, s->password, pl.n);
        } else {
            /* without TLS: the client asks for the public key and sends the password encrypted */
            if (srv_recv(s, &pl) || pl.n != 1 || pl.p[0] != 2) { snprintf(s->why, sizeof s->why, "no key request"); free(m.p); goto out; }
            BIO *b = BIO_new(BIO_s_mem());
            PEM_write_bio_PUBKEY(b, key);
            char *pem;
            long pn = BIO_get_mem_data(b, &pem);
            m.n = 0; put8(&m, 1); put(&m, pem, (size_t)pn);
            srv_send(s, &m);
            BIO_free(b);
            if (srv_recv(s, &pl)) goto out;
            EVP_PKEY_CTX *x = EVP_PKEY_CTX_new(key, NULL);
            uint8_t dec[512];
            size_t dn = sizeof dec;
            EVP_PKEY_decrypt_init(x);
            EVP_PKEY_CTX_set_rsa_padding(x, RSA_PKCS1_OAEP_PADDING);
            if (EVP_PKEY_decrypt(x, dec, &dn, pl.p, pl.n) == 1) {
                for (size_t i = 0; i < dn; i++) dec[i] ^= SCRAMBLE[i % 20];
                good = dn == strlen(s->password) + 1 && !memcmp(dec, s->password, dn);
            }
            EVP_PKEY_CTX_free(x);
        }
        free(m.p);
    }
    if (!good) { snprintf(s->why, sizeof s->why, "wrong password data"); srv_err(s, 1045, "28000", "Access denied"); goto out; }
    srv_ok(s, 0, 0, 0, 2, 0, NULL);
    s->login_ok = 1;
    if (s->drop_after_login) goto out;
    if (s->session) session(s);
out:
    free(pl.p);
    if (s->ssl) { SSL_shutdown(s->ssl); SSL_free(s->ssl); s->ssl = NULL; }
    close(s->fd);
    return NULL;
}

/* ---- the helper ---- */

typedef struct { pid_t pid; int chan; pthread_t th; Scn *s; } Helper;

static int start(Helper *h, Scn *s) {
    int ch[2], db[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ch) || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, db)) return -1;
    int a = fcntl(ch[1], F_DUPFD_CLOEXEC, 20), b = fcntl(db[1], F_DUPFD_CLOEXEC, 20);
    h->pid = fork();
    if (h->pid == 0) {
        dup2(a, 3);
        dup2(b, 4);
        char *argv[] = { "lord-sql", NULL }, *envp[] = { "LANG=C.UTF-8", NULL };
        execve(lordsql, argv, envp);
        _exit(127);
    }
    close(a); close(b); close(ch[1]); close(db[1]);
    h->chan = ch[0];
    h->s = s;
    s->fd = db[0];
    pthread_create(&h->th, NULL, server, s);
    return 0;
}

static int open_req(Helper *h, const char *json, const int *fds, int nfds) {
    return doc_send(h->chan, SQ_OPEN, 1, json, strlen(json), fds, nfds);
}

/* the next reply: op and its JSON (malloc'd; SR_BIG read from its memfd) */
static int reply(Helper *h, char **json, size_t *len, int ms) {
    struct pollfd p = { h->chan, POLLIN, 0 };
    *json = NULL;
    *len = 0;
    if (poll(&p, 1, ms) != 1) return -2;
    DocMsg m;
    int r = doc_recv(h->chan, &m);
    if (r) return r == 1 ? 0 : -1;
    int op = (int)m.h.op;
    if (op == SR_BIG && m.nfds == 1) {
        op = SR_OK;
        struct stat st;
        fstat(m.fds[0], &st);
        *json = malloc((size_t)st.st_size + 1);
        *len = (size_t)pread(m.fds[0], *json, (size_t)st.st_size, 0);
        (*json)[*len] = 0;
        if (m.json && strstr(m.json, "err")) op = SR_ERR;
    } else if (m.json) { *json = m.json; *len = m.len; m.json = NULL; }
    doc_msg_free(&m);
    return op;
}

static int finish(Helper *h) {
    close(h->chan);
    int st = 0;
    for (int i = 0; i < 300; i++) { if (waitpid(h->pid, &st, WNOHANG) == h->pid) goto done; struct timespec ts = { 0, 10000000 }; nanosleep(&ts, NULL); }
    kill(h->pid, SIGKILL);
    waitpid(h->pid, &st, 0);
done:
    pthread_join(h->th, NULL);
    return st;
}

static const char *open_json(const char *tls, const char *host, const char *trust, int ca) {
    static char j[512];
    snprintf(j, sizeof j, "{\"user\":\"shop\",\"password\":\"s3cret pass\",\"database\":\"shop\",\"host\":\"%s\",\"tls\":\"%s\",\"trust\":\"%s\",\"ca\":%d,\"cert\":0,\"key\":0}",
             host, tls, trust, ca);
    return j;
}

/* one login: the expected op and a text the reply must hold */
static void login(const char *name, Scn *s, const char *json, int ca_fd, int want_op, const char *want_text, int want_login) {
    Helper h;
    s->password = "s3cret pass";
    if (start(&h, s)) { CHECK(0, "%s: start", name); return; }
    int fds[1] = { ca_fd };
    open_req(&h, json, ca_fd >= 0 ? fds : NULL, ca_fd >= 0 ? 1 : 0);
    char *j;
    size_t n;
    int op = reply(&h, &j, &n, 15000);
    if (op == SR_EVT) {
        CHECK(strstr(j, "\"ev\":\"cert\"") && strstr(j, fp), "%s: cert event %s", name, j);
        free(j);
        op = reply(&h, &j, &n, 15000);
    }
    CHECK(op == want_op && j && strstr(j, want_text), "%s: op %d, %s (server: %s)", name, op, j ? j : "(none)", s->why);
    if (want_login >= 0) CHECK(s->login_ok == want_login, "%s: server login %d (%s)", name, s->login_ok, s->why);
    free(j);
    finish(&h);
}

static int has(const char *j, const char *t) { return j && strstr(j, t); }
static void q(Helper *h, int op, int id, const char *json) { doc_send(h->chan, (uint32_t)op, (uint32_t)id, json, strlen(json), NULL, 0); }

static void test_session(const char *name, Scn *s) {
    Helper h;
    s->password = "s3cret pass";
    s->session = 1;
    if (start(&h, s)) { CHECK(0, "start"); return; }
    /* the certificate trusted by fingerprint: full auth may send the password (an unverified peer never gets it) */
    open_req(&h, open_json("preferred", "db.test", fp, 0), NULL, 0);
    char *j;
    size_t n;
    int op = reply(&h, &j, &n, 15000);
    int maria = strstr(s->version, "MariaDB") != NULL;
    CHECK(op == SR_OK && has(j, maria ? "\"version\":\"10.11.6-MariaDB-0+deb12u1\",\"flavor\":\"mariadb\"" : "\"version\":\"8.0.36\",\"flavor\":\"mysql\"") &&
          has(j, s->offer_tls ? "\"threadId\":4242,\"tls\":{\"version\":\"TLSv1.3\"" : "\"threadId\":4242,\"tls\":null,\"db\":\"shop\"}") &&
          has(j, "\"db\":\"shop\"}"), "%s open: %d %s (%s)", name, op, j, s->why);
    free(j);
    /* two result sets: NULL, binary as \u0000hex, the OK's numbers */
    q(&h, SQ_QUERY, 2, "{\"op\":\"query\",\"sql\":\"SELECT two\"}");
    op = reply(&h, &j, &n, 15000);
    CHECK(op == SR_OK && has(j, "{\"results\":[{\"columns\":[{\"name\":\"id\",\"orgName\":\"id\",\"table\":\"orders\",\"orgTable\":\"orders\",\"db\":\"shop\",\"type\":3,\"typeName\":\"INT\"") &&
          has(j, "\"flags\":{\"notNull\":true,\"pk\":true,\"unique\":false,\"multiKey\":false,\"unsigned\":true,\"zerofill\":false,\"binary\":false,\"autoInc\":true,\"blob\":false}") &&
          has(j, "{\"name\":\"data\",\"orgName\":\"data\",\"table\":\"orders\",\"orgTable\":\"orders\",\"db\":\"shop\",\"type\":253,\"typeName\":\"VARBINARY\"") &&
          has(j, "\"rows\":[[\"1\",\"caf\xc3\xa9\",\"\\u0000hex:00ff\"],[\"2\",null,\"\\u0000hex:00ff\"],[\"3\",\"caf\xc3\xa9\",\"\\u0000hex:00ff\"]],\"affected\":0") &&
          has(j, "{\"columns\":[],\"rows\":[],\"affected\":3,\"insertId\":7,\"warnings\":1,\"info\":\"Records: 3  Duplicates: 0  Warnings: 1\",\"truncated\":false}],\"ms\":"),
          "%s two results: %s", name, j);
    free(j);
    /* ERR */
    q(&h, SQ_QUERY, 3, "{\"sql\":\"SELECT err\"}");
    op = reply(&h, &j, &n, 15000);
    CHECK(op == SR_ERR && has(j, "{\"error\":\"sql\",\"errno\":1146,\"sqlstate\":\"42S02\",\"message\":\"Table 'shop.x' doesn't exist\",\"results\":[]}"), "%s ERR: %s", name, j);
    free(j);
    /* a first result, then ERR: the result comes with the error */
    q(&h, SQ_QUERY, 4, "{\"sql\":\"SELECT one; bad\"}");
    op = reply(&h, &j, &n, 15000);
    CHECK(op == SR_ERR && has(j, "\"errno\":1064") && has(j, "\"results\":[{\"columns\":[{") && has(j, "\"rows\":[[\"1\","), "%s result then ERR: %s", name, j);
    free(j);
    /* maxRows */
    q(&h, SQ_QUERY, 5, "{\"sql\":\"SELECT many\",\"maxRows\":3}");
    op = reply(&h, &j, &n, 15000);
    CHECK(op == SR_OK && has(j, "[\"3\",\"caf\xc3\xa9\",\"\\u0000hex:00ff\"]],\"affected\":0,\"insertId\":0,\"warnings\":0,\"info\":\"\",\"truncated\":true}") &&
          !has(j, "[\"4\","), "%s maxRows: %s", name, j);
    free(j);
    /* COM_INIT_DB, its ERR, COM_PING */
    q(&h, SQ_USE, 6, "{\"db\":\"shop2\"}");
    op = reply(&h, &j, &n, 15000);
    CHECK(op == SR_OK && has(j, "{}"), "%s use: %s", name, j);
    free(j);
    q(&h, SQ_USE, 7, "{\"db\":\"nope\"}");
    op = reply(&h, &j, &n, 15000);
    CHECK(op == SR_ERR && has(j, "\"errno\":1049"), "%s use ERR: %s", name, j);
    free(j);
    q(&h, SQ_PING, 8, "{}");
    op = reply(&h, &j, &n, 15000);
    CHECK(op == SR_OK && has(j, "{\"ms\":"), "%s ping: %s", name, j);
    free(j);
    if (!maria) {
        /* 80 MB of rows: stopped at 64 MB, sent in a memfd, and the connection is still in step */
        q(&h, SQ_QUERY, 9, "{\"sql\":\"SELECT huge\"}");
        op = reply(&h, &j, &n, 60000);
        CHECK(op == SR_OK && n > ((size_t)60 << 20) && n <= SQL_REPLY_MAX && has(j + n - 200, "\"truncated\":true}],\"ms\":"), "%s 64 MB: op %d, %zu bytes", name, op, n);
        free(j);
        q(&h, SQ_PING, 10, "{}");
        op = reply(&h, &j, &n, 15000);
        CHECK(op == SR_OK, "%s ping after 64 MB", name);
        free(j);
    }
    q(&h, SQ_END, 0, "{}");
    struct pollfd p = { h.chan, POLLIN, 0 };
    CHECK(poll(&p, 1, 5000) == 1 && reply(&h, &j, &n, 10) == 0, "%s: the helper ends after COM_QUIT", name);
    int st = finish(&h);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "%s: exit status %d", name, st);
}

#define CAPS (MY_CLIENT_PROTOCOL_41 | MY_CLIENT_SECURE_CONNECTION | MY_CLIENT_PLUGIN_AUTH | MY_CLIENT_PLUGIN_AUTH_LENENC | \
              MY_CLIENT_DEPRECATE_EOF | MY_CLIENT_MULTI_RESULTS | MY_CLIENT_MULTI_STATEMENTS | MY_CLIENT_CONNECT_WITH_DB | \
              MY_CLIENT_LONG_PASSWORD | MY_CLIENT_TRANSACTIONS | MY_CLIENT_LOCAL_FILES)

static void test_helper(void) {
    Scn s;
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS };
    test_session("native, MySQL", &s);
    s = (Scn){ .version = "5.5.5-10.11.6-MariaDB-0+deb12u1", .plugin = "caching_sha2_password", .caps = CAPS & ~MY_CLIENT_DEPRECATE_EOF };
    test_session("sha2 fast, MariaDB, EOF packets", &s);
    s = (Scn){ .version = "8.0.36", .plugin = "caching_sha2_password", .caps = CAPS, .offer_tls = 1, .full = 1 };
    test_session("sha2 full over TLS, a whole session", &s);

    s = (Scn){ .version = "8.0.36", .plugin = "caching_sha2_password", .caps = CAPS, .full = 1 };
    /* the server's RSA key is never requested over plain text (a man in the middle could swap it) */
    login("sha2 full auth without TLS is refused", &s, open_json("preferred", "db.test", "", 0), -1, SR_ERR, "\"error\":\"tls\"", 0);
    s = (Scn){ .version = "8.0.36", .plugin = "caching_sha2_password", .caps = CAPS, .switch_native = 1 };
    login("auth switch", &s, open_json("off", "db.test", "", 0), -1, SR_OK, "\"threadId\":4242", 1);
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .deny = 1 };
    login("denied", &s, open_json("off", "db.test", "", 0), -1, SR_ERR, "{\"error\":\"auth\",\"errno\":1045,\"sqlstate\":\"28000\",\"message\":\"Access denied for user", 0);
    s = (Scn){ .greet_err = 1 };
    login("ERR greeting", &s, open_json("off", "db.test", "", 0), -1, SR_ERR, "{\"error\":\"auth\",\"errno\":1130,\"sqlstate\":\"\",\"message\":\"Host '10.0.0.9'", 0);
    s = (Scn){ .garbage = 1 };
    login("garbage greeting", &s, open_json("off", "db.test", "", 0), -1, SR_ERR, "{\"error\":\"unsupported\"}", 0);

    /* TLS */
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .offer_tls = 1 };
    login("TLS preferred, self-signed", &s, open_json("preferred", "db.test", "", 0), -1, SR_OK, "\"tls\":{\"version\":\"TLSv1.3\",\"cipher\":\"", 1);
    CHECK(s.tls_used, "TLS used");
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .offer_tls = 1 };
    login("TLS preferred reports unverified", &s, open_json("preferred", "db.test", "", 0), -1, SR_OK, "\"verified\":false,\"pinned\":false", 1);
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .offer_tls = 1 };
    login("TLS verify fails: cert event, then cert", &s, open_json("verify", "db.test", "", 0), -1, SR_ERR, "{\"error\":\"cert\"}", 0);
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .offer_tls = 1 };
    login("TLS verify with the trusted fingerprint", &s, open_json("verify", "db.test", fp, 0), -1, SR_OK, "\"verified\":false,\"pinned\":true", 1);
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .offer_tls = 1 };
    login("TLS verify, another fingerprint", &s, open_json("verify", "db.test", "AA:BB", 0), -1, SR_ERR, "{\"error\":\"cert\"}", 0);
    int ca = open(cert_path, O_RDONLY | O_CLOEXEC);
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .offer_tls = 1 };
    login("TLS verify with the CA file", &s, open_json("verify", "db.test", "", 1), ca, SR_OK, "\"verified\":true", 1);
    close(ca);
    ca = open(cert_path, O_RDONLY | O_CLOEXEC);
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .offer_tls = 1 };
    login("TLS verify, CA fine, wrong host name", &s, open_json("verify", "other.test", "", 1), ca, SR_ERR, "{\"error\":\"cert\"}", 0);
    close(ca);
    ca = open(cert_path, O_RDONLY | O_CLOEXEC);
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .offer_tls = 1 };
    login("TLS verify by IP address", &s, open_json("verify", "127.0.0.1", "", 1), ca, SR_ERR, "{\"error\":\"cert\"}", 0);
    close(ca);
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS };
    login("TLS required, not offered", &s, open_json("required", "db.test", "", 0), -1, SR_ERR, "{\"error\":\"tls\"}", 0);
    s = (Scn){ .version = "8.0.36", .plugin = "caching_sha2_password", .caps = CAPS, .offer_tls = 1, .full = 1 };
    login("sha2 full auth over unverified TLS is refused", &s, open_json("required", "db.test", "", 0), -1, SR_ERR, "\"error\":\"tls\"", 0);
    s = (Scn){ .version = "8.0.36", .plugin = "caching_sha2_password", .caps = CAPS, .offer_tls = 1, .full = 1 };
    login("sha2 full auth over preferred TLS, self-signed, is refused", &s, open_json("preferred", "db.test", "", 0), -1, SR_ERR, "\"error\":\"tls\"", 0);
    s = (Scn){ .version = "8.0.36", .plugin = "caching_sha2_password", .caps = CAPS, .offer_tls = 1, .full = 1 };
    login("sha2 full auth over TLS with the trusted fingerprint", &s, open_json("required", "db.test", fp, 0), -1, SR_OK, "\"verified\":false,\"pinned\":true", 1);
    ca = open(cert_path, O_RDONLY | O_CLOEXEC);
    s = (Scn){ .version = "8.0.36", .plugin = "caching_sha2_password", .caps = CAPS, .offer_tls = 1, .full = 1 };
    login("sha2 full auth over verified TLS", &s, open_json("verify", "db.test", "", 1), ca, SR_OK, "\"verified\":true", 1);
    close(ca);
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .offer_tls = 1 };
    login("TLS off, offered", &s, open_json("off", "db.test", "", 0), -1, SR_OK, "\"tls\":null", 1);
    CHECK(!s.tls_used, "TLS off stays plain");
    int bad = open("/dev/null", O_RDONLY | O_CLOEXEC);
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .offer_tls = 1 };
    login("an empty CA file", &s, open_json("verify", "db.test", "", 1), bad, SR_ERR, "{\"error\":\"tls\",\"message\":\"ca\"}", 0);
    close(bad);

    /* the server goes away while the connection is idle: the helper ends */
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .drop_after_login = 1, .password = "s3cret pass" };
    Helper h;
    start(&h, &s);
    open_req(&h, open_json("off", "db.test", "", 0), NULL, 0);
    char *j;
    size_t n;
    int op = reply(&h, &j, &n, 15000);
    free(j);
    struct pollfd p = { h.chan, POLLIN, 0 };
    CHECK(op == SR_OK && poll(&p, 1, 5000) == 1 && reply(&h, &j, &n, 10) == 0, "idle drop ends the helper");
    int st = finish(&h);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "idle drop exit %d", st);
}

/* ---- lord-server's side: sqlc.c over a TCP listener ---- */

typedef struct { int lfd; Scn *s; } Acc;
static void *acceptor(void *arg) {
    Acc *a = arg;
    int fd = accept4(a->lfd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) return NULL;
    a->s->fd = fd;
    server(a->s);
    return NULL;
}

/* polls sqlc until request id is answered (or an event arrives when id is 0); returns its type and *data
 * (malloc'd). Items for other ids are kept for later calls. */
static SqlItem kept[64];
static int nkept;
static int wait_for(int id, char **data, int ms) {
    *data = NULL;
    for (int t = 0; t <= ms; t += 10) {
        SqlItem *it;
        int n = sql_poll(&it);
        for (int i = 0; i < n && nkept < 64; i++) { kept[nkept++] = it[i]; it[i].data = NULL; }
        sql_items_free(it, n);
        for (int i = 0; i < nkept; i++) {
            if (kept[i].id != id || (!id && kept[i].type != SQL_EVT)) continue;
            int type = kept[i].type;
            *data = kept[i].data;
            memmove(kept + i, kept + i + 1, sizeof *kept * (size_t)(nkept - i - 1));
            nkept--;
            return type;
        }
        struct timespec ts = { 0, 10000000 };
        nanosleep(&ts, NULL);
    }
    return -1;
}

static void test_sqlc(void) {
    char home[] = "/tmp/sql-e2e-home.XXXXXX";
    if (!mkdtemp(home)) { CHECK(0, "mkdtemp"); return; }
    setenv("HOME", home, 1);
    CHECK(sql_available() == 0, "lord-sql next to the test binary");
    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(lfd, (struct sockaddr *)&a, sizeof a);
    socklen_t al = sizeof a;
    getsockname(lfd, (struct sockaddr *)&a, &al);
    listen(lfd, 4);
    char favs[1536];
    snprintf(favs, sizeof favs, "{\"version\":1,\"groups\":[],\"favorites\":[{\"id\":\"local\",\"type\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":%d,"
             "\"user\":\"shop\",\"database\":\"shop\",\"tls\":{\"mode\":\"off\"}},{\"id\":\"tunnel\",\"type\":\"ssh\",\"host\":\"h\"},"
             "{\"id\":\"tunnel2\",\"type\":\"ssh\",\"host\":\"h\",\"ssh\":{\"host\":\"127.0.0.1\",\"user\":\"u\",\"key\":\"no-such-key\","
             "\"hostKey\":\"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIELR56/bq7yBXo5+IGCiVTclE52yTok7jaoB8rrUefpd\"}},"
             "{\"id\":\"closed\",\"type\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":1},"
             "{\"id\":\"tlsv\",\"host\":\"127.0.0.1\",\"port\":%d,\"user\":\"shop\",\"database\":\"shop\",\"tls\":{\"mode\":\"verify\"}}]}",
             ntohs(a.sin_port), ntohs(a.sin_port));
    CHECK(sql_favorites_save(favs, strlen(favs)) == 0, "save favorites");
    char *d;
    int id = sql_request("{\"op\":\"open\",\"fav\":\"nope\"}", 26);
    CHECK(wait_for(id, &d, 2000) == SQL_ERR && has(d, "notfound"), "unknown favorite: %s", d);
    free(d);
    /* an SSH favorite without its server is refused before ssh runs (sshtun.c checks every argument) */
    id = sql_request("{\"op\":\"open\",\"fav\":\"tunnel\",\"password\":\"x\"}", 43);
    CHECK(wait_for(id, &d, 5000) == SQL_ERR && has(d, "\"ssh\""), "ssh without a server: %s", d);
    free(d);
    /* with the server's key kept but no key file of the user's, it stops at the key (no connection is made) */
    id = sql_request("{\"op\":\"open\",\"fav\":\"tunnel2\",\"password\":\"x\"}", 44);
    CHECK(wait_for(id, &d, 5000) == SQL_ERR && has(d, "sshkey"), "ssh without a usable key: %s", d);
    free(d);
    id = sql_request("{\"op\":\"open\",\"fav\":\"closed\",\"password\":\"x\"}", 43);
    CHECK(wait_for(id, &d, 15000) == SQL_ERR && has(d, "\"connect\""), "nothing listening: %s", d);
    free(d);
    id = sql_request("{\"op\":\"query\",\"conn\":99,\"sql\":\"x\"}", 34);
    CHECK(wait_for(id, &d, 2000) == SQL_ERR && has(d, "closed"), "no such connection: %s", d);
    free(d);
    id = sql_request("{\"op\":\"drop\"}", 13);
    CHECK(wait_for(id, &d, 2000) == SQL_ERR && has(d, "unsupported"), "unknown op: %s", d);
    free(d);
    /* a real open, query, busy and close */
    Scn s = { .version = "8.0.36", .plugin = "caching_sha2_password", .caps = CAPS, .session = 1, .password = "s3cret pass" };
    Acc acc = { lfd, &s };
    pthread_t th;
    pthread_create(&th, NULL, acceptor, &acc);
    const char *oj = "{\"op\":\"open\",\"fav\":\"local\",\"password\":\"s3cret pass\"}";
    id = sql_request(oj, strlen(oj));
    CHECK(wait_for(id, &d, 15000) == SQL_OK && has(d, "{\"conn\":") && has(d, "\"server\":{\"version\":\"8.0.36\",\"flavor\":\"mysql\"},\"threadId\":4242,\"tls\":null,\"db\":\"shop\"}"),
          "open through lord-server: %s (%s)", d, s.why);
    int conn = 0;
    if (d) sscanf(d, "{\"conn\":%d", &conn);
    free(d);
    char q[128];
    snprintf(q, sizeof q, "{\"op\":\"query\",\"conn\":%d,\"sql\":\"SELECT many\",\"maxRows\":2}", conn);
    int id1 = sql_request(q, strlen(q));
    int id2 = sql_request(q, strlen(q));
    CHECK(wait_for(id2, &d, 2000) == SQL_ERR && has(d, "busy"), "one query at a time: %s", d);
    free(d);
    CHECK(wait_for(id1, &d, 15000) == SQL_OK && has(d, "\"truncated\":true") && has(d, "\"ms\":"), "query through lord-server: %s", d);
    free(d);
    snprintf(q, sizeof q, "{\"op\":\"close\",\"conn\":%d}", conn);
    id = sql_request(q, strlen(q));
    CHECK(wait_for(id, &d, 2000) == SQL_OK && has(d, "{}"), "close: %s", d);
    free(d);
    snprintf(q, sizeof q, "{\"op\":\"ping\",\"conn\":%d}", conn);
    id = sql_request(q, strlen(q));
    CHECK(wait_for(id, &d, 2000) == SQL_ERR && has(d, "closed"), "closed connection: %s", d);
    free(d);
    pthread_join(th, NULL);
    /* the server drops the connection while idle: a closed event */
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .drop_after_login = 1, .password = "s3cret pass" };
    pthread_create(&th, NULL, acceptor, &acc);
    id = sql_request(oj, strlen(oj));
    CHECK(wait_for(id, &d, 15000) == SQL_OK, "second open: %s", d);
    free(d);
    CHECK(wait_for(0, &d, 10000) == SQL_EVT && has(d, "\"ev\":\"closed\"") && has(d, "\"reason\":\"server\""), "closed event: %s", d);
    free(d);
    pthread_join(th, NULL);
    /* a wrong password through lord-server */
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .password = "other" };
    pthread_create(&th, NULL, acceptor, &acc);
    id = sql_request(oj, strlen(oj));
    CHECK(wait_for(id, &d, 15000) == SQL_ERR && has(d, "\"error\":\"auth\""), "wrong password: %s", d);
    free(d);
    pthread_join(th, NULL);
    /* a certificate that doesn't check: the event names lord-server's conn and favorite */
    s = (Scn){ .version = "8.0.36", .plugin = "mysql_native_password", .caps = CAPS, .offer_tls = 1, .password = "s3cret pass" };
    pthread_create(&th, NULL, acceptor, &acc);
    const char *tj = "{\"op\":\"open\",\"fav\":\"tlsv\",\"password\":\"s3cret pass\"}";
    id = sql_request(tj, strlen(tj));
    CHECK(wait_for(id, &d, 15000) == SQL_ERR && has(d, "{\"error\":\"cert\"}"), "cert through lord-server: %s", d);
    free(d);
    CHECK(wait_for(0, &d, 2000) == SQL_EVT && has(d, "{\"ev\":\"cert\",\"conn\":") && has(d, "\"fav\":\"tlsv\",\"sha256\":\"") && has(d, fp) &&
          has(d, "\"subject\":\"/CN=db.test\""), "cert event: %s", d);
    free(d);
    pthread_join(th, NULL);
    close(lfd);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    if (system(cmd)) { /* nothing */ }
}

int main(int argc, char **argv) {
    lordsql = argc > 1 ? argv[1] : "build/lord-sql";
    unsetenv("LORDOS_SESSIOND");   /* sockets made here, not by a session connector (sec/netconn.h) */
    signal(SIGPIPE, SIG_IGN);
    if (access(lordsql, X_OK)) { printf("sql-e2e-test: skipped: needs %s\n", lordsql); return 0; }
    make_cert();
    test_helper();
    test_sqlc();
    unlink(cert_path);
    printf("sql-e2e-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
