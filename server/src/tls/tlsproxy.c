/* tlsproxy.c — lord-tls: TLS for the package engine's downloads (0.8.5; docs/PACKAGES.md, "HTTPS").
 *
 * The package engine's download child (nobody, in lordos-net-store.scope; src/pkg/http.c) connects the
 * TCP socket itself and starts this program with a fixed path, no arguments and an empty environment:
 *   fd 3  the connected TCP socket to the server
 *   fd 4  a socketpair to the download child: first a line "<host>\n", then plain HTTP both ways
 * lord-tls loads the system's CA bundle, applies the helpers' sandbox (no sockets, no exec, no files,
 * sandbox_apply_helper; it stays in the caller's scope), does a TLS 1.2+ handshake with SNI, checks the
 * certificate chain and the host name, answers one byte on fd 4 ('1' verified, '0' refused) and then
 * only moves bytes: plaintext from fd 4 is encrypted to fd 3, what arrives on fd 3 is decrypted to fd 4.
 * It never parses HTTP and never decides anything about packages: those are still checked by their
 * signed index (http.c and pkg.c), TLS adds privacy and refuses a server that isn't who it claims.
 * With lord-sessiond, lord-maild and lord-fetch, the only binaries that link OpenSSL. */
#include "../sec/sandbox.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define NET 3
#define PLAIN 4
#define HANDSHAKE_MS 20000
#define IDLE_MS 60000

static int write_all(int fd, const void *p, size_t n) {
    const char *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

/* the host line: a DNS name (letters, digits, '-', '.'), at most 253 characters */
static int read_host(char *host, size_t cap) {
    size_t n = 0;
    for (;;) {
        char ch;
        ssize_t r = read(PLAIN, &ch, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r != 1) return -1;
        if (ch == '\n') break;
        if (n + 1 >= cap || !(isalnum((unsigned char)ch) || ch == '-' || ch == '.')) return -1;
        host[n++] = ch;
    }
    host[n] = 0;
    return n && host[0] != '.' && host[0] != '-' ? 0 : -1;
}

/* waits until the TLS socket can make progress for `want` (SSL_ERROR_WANT_READ/WRITE) */
static int wait_net(int want, int ms) {
    struct pollfd p = { NET, (short)(want == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN), 0 };
    return poll(&p, 1, ms) == 1 ? 0 : -1;
}

int main(void) {
    prctl(PR_SET_DUMPABLE, 0);
    struct rlimit nocore = { 0, 0 };
    setrlimit(RLIMIT_CORE, &nocore);
    signal(SIGPIPE, SIG_IGN);
    struct stat st;
    if (fstat(NET, &st) || !S_ISSOCK(st.st_mode) || fstat(PLAIN, &st) || !S_ISSOCK(st.st_mode)) {
        fprintf(stderr, "lord-tls: started by the package engine only\n");
        return 2;
    }
    char host[254];
    if (read_host(host, sizeof host)) return 2;

    OPENSSL_init_ssl(0, NULL);
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return 3;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    /* the whole bundle now: inside the sandbox there are no files */
    static const char *const bundles[] = { "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", NULL };
    int have_ca = 0;
    for (int i = 0; bundles[i] && !have_ca; i++) have_ca = SSL_CTX_load_verify_locations(ctx, bundles[i], NULL) == 1;
    SSL *s = SSL_new(ctx);
    if (!have_ca || !s) { write_all(PLAIN, "0", 1); return 3; }

    SandboxHelperCfg cfg = {
        .name = NULL,                     /* stays in the caller's scope (lordos-net-store.scope) */
        .ro_paths = NULL,
        .extra_syscalls = (int[]){ -1 },
        .allow_ioctl_classes = 0,
        .mem_max_mb = 0,
        .ro_dir_fds = NULL,
        .rw_dir_fds = NULL,
    };
    sandbox_apply_helper(&cfg);

    SSL_set_fd(s, NET);
    SSL_set_tlsext_host_name(s, host);
    SSL_set_hostflags(s, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (!SSL_set1_host(s, host)) { write_all(PLAIN, "0", 1); return 3; }
    /* the socket is non-blocking (http.c connects it that way): handshake with a deadline */
    for (;;) {
        int r = SSL_connect(s);
        if (r == 1) break;
        int e = SSL_get_error(s, r);
        if ((e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) || wait_net(e, HANDSHAKE_MS)) {
            write_all(PLAIN, "0", 1);
            return 4;
        }
    }
    if (SSL_get_verify_result(s) != X509_V_OK) { write_all(PLAIN, "0", 1); return 4; }
    if (write_all(PLAIN, "1", 1)) return 5;

    /* the relay: plaintext ↔ TLS until either side closes or nothing moves for IDLE_MS */
    char buf[16384];
    for (;;) {
        if (SSL_pending(s) <= 0) {
            struct pollfd p[2] = { { NET, POLLIN, 0 }, { PLAIN, POLLIN, 0 } };
            int r = poll(p, 2, IDLE_MS);
            if (r <= 0) break;
            if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t n = read(PLAIN, buf, sizeof buf);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                for (int off = 0; off < n;) {
                    int w = SSL_write(s, buf + off, (int)(n - off));
                    if (w > 0) { off += w; continue; }
                    int e = SSL_get_error(s, w);
                    if ((e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) || wait_net(e, IDLE_MS)) goto done;
                }
            }
            if (!(p[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        }
        int n = SSL_read(s, buf, sizeof buf);
        if (n > 0) { if (write_all(PLAIN, buf, (size_t)n)) break; continue; }
        int e = SSL_get_error(s, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
        break;                            /* closed (ZERO_RETURN) or an error */
    }
done:
    SSL_shutdown(s);
    SSL_free(s);
    SSL_CTX_free(ctx);
    return 0;
}
