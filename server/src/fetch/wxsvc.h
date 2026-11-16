/* wxsvc.h — lord-server's side of the weather widget (0.8.8.10; maintainer's approval 2026-11-16).
 *
 * The desktop names a time zone ("America/Argentina/Buenos_Aires"); its coordinates come from tzdata's
 * zone1970.tab here, never from JS. A worker thread connects to api.open-meteo.com:443 through the
 * weather connector (sec/netconn.h, NC_WEATHER: its own scope and firewall row, "lordos:weather"), hands
 * the socket to lord-fetch (FQ_WEATHER) and reads its answer back strictly (fetch/weather.h). Each zone's
 * forecast is kept 30 minutes; a failure waits 10 before the next try. At most WXS_ZONES zones.
 * Thread-safe; nothing blocks the caller. */
#ifndef LORD_FETCH_WXSVC_H
#define LORD_FETCH_WXSVC_H

#include "weather.h"

#define WXS_ZONES 8
#define WXS_FRESH 1800          /* seconds a forecast is kept */
#define WXS_RETRY 600           /* seconds after a failure */

/* lord-fetch's folder (lord-server's own). 0 ok. */
int wxs_init(const char *bindir);
/* A zone name as tzdata spells it: letters, digits, '_', '-', '+' and '/' (at most 63 bytes). */
int wxs_zone_ok(const char *zone);
/* Asks for zone's forecast unless a fresh one (or a recent failure) is there. -1 if the zone is unknown. */
int wxs_request(const char *zone);
/* The last forecast of zone: 1 with *d and *at (when it came), 0 with none yet (err: the last failure's
 * code or ""), -1 if the zone was never asked. */
int wxs_get(const char *zone, WxData *d, long long *at, char *err, size_t errlen);

#endif
