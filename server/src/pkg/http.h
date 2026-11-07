/* http.h — our own HTTP/1.1 downloads for the repositories.
 *
 * fetch_batch() runs in the root helper: it creates a child process that drops privileges
 * (switches to nobody, no groups, unable to escalate again) before touching the network, and
 * that sends it the bytes over a pipe. The root process only writes those bytes to a temporary
 * file in its folder, counts the size and computes the SHA-256: it interprets nothing that arrives. */
#ifndef LORD_PKG_HTTP_H
#define LORD_PKG_HTTP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char url[1024];                     /* http://host[:port]/path */
    char dest[512];                     /* final file (written to dest.part and renamed) */
    int64_t ims;                        /* If-Modified-Since (0: no) */
    uint64_t max;                       /* maximum accepted size */
    uint64_t expect;                    /* expected size (0: unknown), for progress */
    int resume;                         /* keep dest.part when the download is cut, and continue it next time with a
                                           Range request (a server that ignores it sends the whole file: started
                                           again); the SHA-256 is still over the whole file */
    /* result */
    int status;                         /* 200, 304, 404… or 0 if the connection failed */
    int64_t last_modified;
    uint64_t size;
    uint64_t resumed;                   /* bytes of dest.part kept from before (0: from the start) */
    uint8_t sha256[32];
    char err[200];
} FetchItem;

typedef void (*FetchProgress)(uint64_t done, uint64_t total, void *ud);

/* Downloads the n files in order (reusing the connection to the same server). Returns the
 * number that ended with 200 or 304; each item reports its own. */
int fetch_batch(FetchItem *items, int n, FetchProgress cb, void *ud);
/* The same, with the download child in another component's scope (a root-owned lordos-net-<name>.scope of the
 * session: fw/rules.h FW_COMPONENTS) instead of the Store's: Machines' system downloads use lordos-net-machines.scope. */
int fetch_batch_in(FetchItem *items, int n, FetchProgress cb, void *ud, const char *scope);

/* HTTP dates (RFC 1123) */
int64_t http_parse_date(const char *s);
void    http_format_date(int64_t t, char *out, size_t n);

#endif
