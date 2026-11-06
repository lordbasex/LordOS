/* dav.h — WebDAV requests (CalDAV RFC 4791, CardDAV RFC 6352) over our HTTP client, for the
 * account's DAV worker (dav.c) and the calendar/contacts sync (pim.c). */
#ifndef LORD_MAIL_DAV_H
#define LORD_MAIL_DAV_H

#include "maild.h"
#include "http.h"

typedef struct {
    Account *a;
    Conn *c;
    char host[256];
    int port, tls;
    char auth[600];             /* "Authorization: Basic …\r\n" */
    char err[200];
    int auth_failed;
    char url[2048];             /* the URL that gave the last answer (after redirects) */
} Dav;

int  dav_begin(Dav *d, Account *a);
void dav_end(Dav *d);
/* target: absolute URL or a path on the current host. depth < 0: no Depth header.
 * Follows up to 3 redirects. 0 when a response arrived (check out->status). */
int  dav_req(Dav *d, const char *method, const char *target, int depth, const char *body, const char *ctype, const char *extra, HttpResp *out);
/* absolute URL of a path on the current host */
void dav_abs(const Dav *d, const char *path, char *out, size_t cap);

#endif
