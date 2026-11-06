/* davutil.h — the pure parts of the CalDAV/CardDAV sync (pim.c), kept apart so tests/mail_test.c
 * can check them without a network: resolving hrefs, reading the discovery answers, planning what
 * to download and what to drop, deciding what an upload's status means, and giving a kept copy
 * a new UID. Everything here works on bounded buffers and validates what it reads. */
#ifndef LORD_MAIL_DAVUTIL_H
#define LORD_MAIL_DAVUTIL_H

#include <stddef.h>

#define DAV_URL_MAX 1024

/* Resolves href against base (an absolute http(s) URL) into a normalized absolute URL: scheme
 * and host in lower case, the default port dropped (":443" for https, ":80" for http), the path
 * kept as the server wrote it (spaces percent-encoded). href may be absolute ("https://h/p"), absolute-path ("/p") or
 * relative ("p.ics", against base's folder). -1 for a bad base or href (no scheme, control
 * characters, user info, a bad port, too long). */
int dav_href_resolve(const char *base, const char *href, char *out, size_t cap);

/* 1 when two absolute URLs have the same scheme, host and port (an item and its collection). */
int dav_same_origin(const char *a, const char *b);
/* 1 when two absolute https URLs are on the same site: the host's last two labels match
 * ("p07-caldav.example.test" and "caldav.example.test"), the rule lord-server applies to discovery
 * hosts. A server's answer may move the homes only within its own site, so the account's
 * credentials never go to another account's server. */
int dav_same_site(const char *a, const char *b);

/* The path of an absolute URL (what follows the authority, "/" if none); href itself when it is
 * not absolute. Used for the hrefs inside a multiget, which servers expect as paths. */
const char *dav_url_path(const char *href);

/* What a PROPFIND for the homes said (RFC 4791 §6.2.1, RFC 6352 §7.1.1, RFC 5397): each URL
 * absolute (resolved against base, the URL that answered), "" when not advertised. */
typedef struct {
    char principal[DAV_URL_MAX];
    char calhome[DAV_URL_MAX];
    char cardhome[DAV_URL_MAX];
    int is_cal, is_book;          /* the answering resource is itself a calendar / address book */
} DavHomes;
/* 0 ok (a 207 body parsed; fields may still be empty), -1 bad XML */
int dav_parse_homes(const char *body, size_t len, const char *base, DavHomes *out);

/* Sync plan for one collection: which server items to download and which local items are gone
 * from the server. All hrefs must already be resolved (dav_href_resolve). */
typedef struct { const char *href, *etag; int dirty; } DavLocal;
typedef struct { const char *href, *etag; } DavRemote;
/* fetch[i] = 1: download rem[i] (new, or its ETag changed, and no local change waiting).
 * drop[k] = 1: loc[k] is clean and the server no longer has it. The collection's own entry
 * (coll_href, or any href ending in '/') is never an item. */
void dav_plan(const char *coll_href, const DavLocal *loc, int nl, const DavRemote *rem, int nr,
              unsigned char *fetch, unsigned char *drop);

/* What an upload's answer means (the conflict policy: the server wins, a refused local version
 * is kept as a copy on this computer). */
enum {
    DAV_PUSH_DONE = 0,      /* stored (PUT 2xx) or deleted (DELETE 2xx, 404, 410) */
    DAV_PUSH_RETRY,         /* try again on the next sync (5xx, 401, 408, 429, other) */
    DAV_PUSH_CONFLICT,      /* 412: someone changed it on the server; take the server's version */
    DAV_PUSH_REJECTED,      /* refused for good (other 4xx): take the server's version */
};
int dav_push_outcome(int is_delete, int status);
/* Whether the local version must be kept as a copy (a PUT that lost) */
int dav_push_keeps_copy(int is_delete, int outcome);

/* text with every UID property (its folded continuation lines too) set to uid; one is added
 * after the first BEGIN:VEVENT / VTODO / VJOURNAL / VCARD when there is none. uid is checked:
 * letters, digits, '-', '_', '.', '@', at most 200 bytes. malloc'd, NUL-terminated; NULL on a
 * bad uid, a text with no component, or no memory. */
char *dav_set_uid(const char *text, size_t len, const char *uid, size_t *olen);

/* 1 when the iCalendar text has a VTODO and no VEVENT (a to-do, for Reminders) */
int dav_is_todo(const char *text, size_t len);

#endif
