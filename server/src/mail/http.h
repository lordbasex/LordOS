/* http.h — a small HTTP/1.1 client for axia-maild: WebDAV (CalDAV, CardDAV) and remote images.
 * Responses are bounded (max body bytes), chunked transfer coding is decoded, and every read has
 * a deadline. The connection comes from axia-server (see net.h). */
#ifndef LORD_MAIL_HTTP_H
#define LORD_MAIL_HTTP_H

#include <stddef.h>
#include "net.h"

typedef struct {
    int status;
    char location[1024];
    char etag[256];
    char content_type[128];
    char *body;                 /* malloc, NUL-terminated */
    size_t blen;
    int keep_alive;
} HttpResp;

/* URL pieces: https only unless http is allowed. 0 ok. */
int  http_split_url(const char *url, int *tls, char *host, size_t hcap, int *port, char *path, size_t pcap);
/* One request on an open connection. extra_headers: "Name: value\r\n…" or NULL. 0 ok (any status). */
int  http_request(Conn *c, const char *method, const char *host, const char *path, const char *extra_headers,
                  const char *body, size_t blen, size_t max_body, int deadline_ms, HttpResp *out);
void http_resp_free(HttpResp *r);
/* strict parsers (exposed for tests): 0 ok, -1 invalid */
int  http_chunk_size(const char *line, unsigned long long *out);
int  http_content_length(const char *v, unsigned long long *out);

#endif
