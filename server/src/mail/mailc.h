/* mailc.h — lord-server's side of lord-maild (see proto.h and docs/MAIL.md).
 *
 * One lord-maild per session, started on first use with a fixed path (next to this binary), no
 * arguments and a minimal environment. A thread of ours talks to it, so the compositor never
 * waits: results and events queue up and JS reads them with mail_poll() (lord.mail.poll).
 *
 * This side also owns the network policy: lord-maild cannot open sockets, and asks us for a
 * connection to host:port. We connect only to the servers of the configured accounts (read from
 * ~/.config/lordos/mail/accounts.json, which only lord.mail.saveConfig can write) and, for ten
 * minutes, to the image hosts of a message whose remote images the user chose to load. */
#ifndef LORD_MAIL_MAILC_H
#define LORD_MAIL_MAILC_H

#include <stddef.h>
#include <stdint.h>

#include "../doc/wire.h"

int   mail_available(void);                 /* 0 if the helper binary is there */
int   mail_start(void);                     /* 0 if running (starts it if needed) */
/* Request: JSON text + fds of files (mail_request takes ownership). id > 0 or -1. */
int   mail_request(const char *json, size_t len, int *fds, int nfds);
char *mail_poll(void);                      /* JSON array of events (malloc) or NULL */
int   mail_pending(void);
/* Re-reads the accounts file, rebuilds the allowed endpoints and tells lord-maild. */
void  mail_reload_config(void);
/* Validates (JSON object, ≤ MAIL_CONFIG_MAX, no "password"/"secret" in an account) and writes the
 * accounts file (0600, atomic), then reloads it. 0, 1 if refused, 2 if it could not be written. */
int   mail_save_config(const char *json, size_t len);
/* Remote images of one message: allow these http(s) hosts for a while. */
void  mail_allow_hosts(const char *const *urls, int n, int seconds);
const char *mail_config_path(void);         /* ~/.config/lordos/mail/accounts.json */
const char *mail_cache_path(void);          /* ~/.local/share/lordos/mail */

/* Decoded images (ARGB32 premultiplied) returned by lord-maild. */
const uint32_t *mail_image_lock(int handle, int *w, int *h);
void  mail_image_unlock(void);
void  mail_image_release(int handle);

/* ---- isolated system apps (Mail, Calendar, Contacts, Reminders in their own process) ----
 * Each app process that uses lord-maild through the broker (sec/brokermail.c) is an "owner":
 * replies to its requests and its decoded images belong to it alone, and broadcast events reach it
 * according to its mask: MAIL_EV_ALL (Mail) or MAIL_EV_PIM (only pim-changed/ready; no senders or
 * subjects for Calendar, Contacts and Reminders). Owner 0 is the session's JavaScript. */
#define MAIL_OWNERS 8
enum { MAIL_EV_ALL = 1, MAIL_EV_PIM = 2 };
int   mail_owner_open(int mask);            /* → 1..MAIL_OWNERS, or -1 */
void  mail_owner_close(int owner);          /* drops its queue, its images and its pending replies */
int   mail_request_as(int owner, const char *json, size_t len, int *fds, int nfds);
char *mail_poll_owner(int owner);           /* JSON array (malloc) or NULL */
int   mail_owner_pollfd(void);              /* eventfd: some owner queue got data (-1 before the first owner) */
void  mail_owner_clear_wake(void);
/* Sealed memfd with a copy of one of the owner's images (ARGB32, w*h*4 bytes), or -1. */
int   mail_image_fd(int owner, int handle, int *w, int *h);
void  mail_image_release_as(int owner, int handle);

/* Appends one event {"id","type","data"} to a queue (used by the reader thread; public for the
 * test). data is lord-maild's JSON: re-validated (nesting up to HTML_JSON_DEPTH and more), replaced
 * by {"error":"bad-json","detail":…} if it isn't JSON, and by {"error":"too-big"} if the queue
 * would pass its size limit; the queue always stays valid JSON. */
void  mail_ev_append(JBuf *dst, int id, const char *type, const char *data, size_t dlen);

/* Pure check used by the connector and its test: is host:port allowed by this config JSON? */
int   mail_endpoint_allowed(const char *config_json, size_t len, const char *host, int port);

/* Is this resolved address on the public Internet? (Remote-image hosts must be: no loopback,
 * private, link-local, CGNAT, multicast or reserved ranges, also behind v4-mapped/NAT64/6to4.) */
struct sockaddr;
int   mail_addr_public(const struct sockaddr *sa);

#endif
