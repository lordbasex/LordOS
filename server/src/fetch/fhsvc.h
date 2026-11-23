/* fhsvc.h — lord-server's side of the Store's rankings from Flathub's API (E10, approved by the maintainer on
 * 2026-11-22; docs/FLATPAK.md, "Rankings from Flathub's API").
 *
 * The Store asks (lord.flatpak.rankRequest) while it shows Flathub's home and the person left the rankings on. A worker
 * thread connects to flathub.org:443 through the flathub connector (sec/netconn.h, NC_FLATHUB: lordos-net-flathub.scope,
 * the firewall component "lordos:flathub"), hands the socket to lord-fetch with only the list's number (FQ_FLATHUB) and
 * reads back app ids, checked again here (fh_from_compact). Each list is fetched at most once a UTC day; a failure waits
 * FHS_RETRY seconds, doubled after each failure in a row up to a day. The lists are kept on disk in
 * ~/.cache/lordos/flathub/<name>.json (the Store's cache folder: ~/.cache/lordos), every id checked again when read.
 * Thread-safe; nothing blocks the caller. */
#ifndef LORD_FETCH_FHSVC_H
#define LORD_FETCH_FHSVC_H

#include "flathub.h"

#define FHS_RETRY 900                   /* seconds after a first failure */
#define FHS_DIR ".cache/lordos/flathub"

/* lord-fetch's folder (lord-server's own) and the home folder. 0 ok. */
int fhs_init(const char *bindir, const char *home);
/* Starts fetching every list that is stale (not fetched today) and not waiting after a failure. 1 when a fetch started
 * or is running, 0 when nothing needed it, -1 when not initialized. */
int fhs_request(void);
/* 1 while the worker runs. */
int fhs_busy(void);
/* The last good copy of list: 1 with *out and *at (when it came), 0 with none. */
int fhs_get(int list, FhIds *out, long long *at);
/* The clock (tests): seconds since 1970. NULL puts time() back. */
void fhs_set_clock(long long (*now)(void));
/* Whether list may be fetched at now: never fetched, or last fetched on another UTC day (or in the future), and not
 * within its wait after fails failures in a row since tried. Pure (exposed for tests). */
int fhs_due(long long now, long long at, long long tried, int fails);

#endif
