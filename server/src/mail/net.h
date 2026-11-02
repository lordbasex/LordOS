/* net.h — buffered connections with TLS for axia-maild (OpenSSL: axia-maild and lord-sessiond are
 * the only binaries that link it, see docs/ARCHITECTURE.md).
 *
 * axia-maild never opens sockets itself (its seccomp profile forbids socket/connect): it asks
 * axia-server for a connection to host:port, axia-server checks that the endpoint belongs to a
 * configured account and hands over an already connected TCP socket (SCM_RIGHTS). This file runs
 * TLS on top of that socket.
 *
 * Certificates are always verified against the system CA store (loaded once, before the
 * sandbox) and the host name. A bad certificate is never accepted silently: the handshake fails
 * with the certificate's details, and only a fingerprint the user explicitly trusted in Mail
 * (stored in the account) lets that exact certificate through. */
#ifndef LORD_MAIL_NET_H
#define LORD_MAIL_NET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct Conn Conn;

typedef struct {
    char reason[160];           /* OpenSSL's verify error text ("" if the chain was fine) */
    int  code;                  /* X509 verify result (0 = ok) */
    char subject[256], issuer[256];
    char sha256[96];            /* "AB:CD:…" fingerprint of the leaf certificate */
    char not_after[40];
    char version[24];           /* "TLSv1.3" */
    int  pinned;                /* accepted only because of a user-trusted fingerprint */
} TlsInfo;

/* Loads the CA bundle (call before the sandbox). 0 ok. */
int   net_init(void);
Conn *conn_new(int fd, const char *host, int timeout_ms);
void  conn_close(Conn *c);
int   conn_fd(const Conn *c);
/* Starts TLS (implicit TLS right away, or after STARTTLS). pin: trusted "AB:CD…" or NULL.
 * 0 ok; -1 failure with info filled (certificate problems set info->code != 0). */
int   conn_tls(Conn *c, const char *pin, TlsInfo *info);
int   conn_is_tls(const Conn *c);
/* Reads one line (up to and including LF) into a malloc'd buffer; *len its length.
 * NULL on EOF/error/timeout (conn_error tells which). Lines are capped at max bytes. */
char *conn_line(Conn *c, size_t *len, size_t max);
/* Reads exactly n bytes, appended to *buf (realloc). 0 ok. */
int   conn_read_n(Conn *c, size_t n, char **buf, size_t *len, size_t *cap);
/* Reads whatever is available (at least 1 byte, waiting up to the timeout). */
ssize_t conn_read_some(Conn *c, void *out, size_t n);
int   conn_write(Conn *c, const void *p, size_t n);
int   conn_printf(Conn *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Bytes already buffered (ours or OpenSSL's) that can be read without waiting. */
int   conn_pending(const Conn *c);
/* Waits for input on the connection or on wake_fd (eventfd). 1 data, 2 wake, 0 timeout, -1 error. */
int   conn_wait(Conn *c, int wake_fd, int ms);
void  conn_set_timeout(Conn *c, int ms);
const char *conn_error(const Conn *c);

#endif
