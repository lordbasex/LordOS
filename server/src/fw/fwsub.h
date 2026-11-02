/* fwsub.h — the user's blocklist subscriptions, lord-server's side (0.8.5, docs/FIREWALL.md,
 * "Blocklists").
 *
 *   assets/fw/blocklists.json        the curated catalog (our own asset: id, topic, url, format,
 *                                    licence, source); every entry's licence was checked before it
 *                                    went in
 *   ~/.config/lordos/fwlists.json    which ones are on, and the custom lists (https URLs)
 *   ~/.cache/lordos/fwlists/<id>.txt the normalized download of each list, and <id>.json its state
 *   ~/.config/lordos/fwlists.txt     every enabled list compiled (fw/fwlists.h); lord-netperm reads it
 *
 * All three user files are written only here (fsops_protected). The work (downloading through
 * lord-fetch and compiling) runs on its own thread: the screen never waits. When it ends,
 * fws_poll() swaps the set in and says so, and lord.fw sends lord-netperm its RELOAD. Lists are
 * downloaded when enabled, and again when the last download is older than a day (fws_update(0)
 * every hour from the SDK). */
#ifndef LORD_FW_FWSUB_H
#define LORD_FW_FWSUB_H

#include <stddef.h>
#include <stdint.h>

#define FWS_MAX_LISTS   48              /* catalog + custom */
#define FWS_MAX_CUSTOM  16
#define FWS_URL_MAX     400
#define FWS_STALE_S     (20 * 3600)     /* "daily": older than this is downloaded again */

/* home: the user's real home folder; assets: LordOS's assets folder; bindir: where lord-fetch is. */
int   fws_init(const char *home, const char *assets, const char *bindir);
/* {"catalog":[…],"custom":[…],"enabled":[…],"state":{id:{…}},"busy":bool,"domains":N,"nets":N} (malloc'd) */
char *fws_json(void);
/* 0 ok; -1 unknown id. Starts a job (compile, and download what has no copy yet). */
int   fws_set(const char *id, int on);
/* Adds a custom list; 0 with its id in id_out (32 bytes), or -1 with a code in err
 * ("fw.list.badUrl", "fw.list.badFormat", "fw.list.tooMany", "fw.list.exists"). */
int   fws_add_custom(const char *url, const char *format, char *id_out, char *err, size_t errlen);
int   fws_remove_custom(const char *id);
/* Downloads the enabled lists (all of them with force, else only the stale ones). 0 started, 1 nothing
 * to do, -1 a job is already running (it runs again when that one ends). */
int   fws_update(int force);
/* Main thread: 1 if a job just ended and the compiled set changed (send RELOAD). */
int   fws_poll(void);
/* Which enabled list covers these names or this address (fam 4 or 6, or 0): its id, or NULL. */
const char *fws_match(const char *const *names, int n, int fam, const uint8_t *addr);

/* ---- exposed for tests ---- */
/* https://host[:port]/path → parts; 0 ok. host: lowercase name or IPv4, port 1..65535. */
int fws_split_url(const char *url, char *host, size_t hcap, int *port, char *path, size_t pcap);

#endif
