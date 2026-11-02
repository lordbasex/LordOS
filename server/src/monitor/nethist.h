/* nethist.h — the Monitor's traffic history (0.8.2, docs/MONITOR.md "Traffic history").
 *
 * Bytes received and sent per minute, for the machine (every interface but lo, as the Monitor's
 * totals) and for each of the user's apps (axia-netperm's per-app counters), kept 7 days per minute
 * and 30 days per hour in one compact file of the user's:
 *
 *   $XDG_STATE_HOME/axia/net-history.bin   (default ~/.local/state/axia/net-history.bin, 0600)
 *
 * The apps are the user's own, so the file is the user's (axia-server writes it as the user; no
 * root process writes into a home folder). It holds app ids and byte counts only: never an address,
 * a domain or a country.
 *
 * File: "LNH1" + u32 version, then records [u8 type][u16 len][payload][u32 crc32]. A record that
 * doesn't check out (a crash in the middle of a write, a bad disk) ends the file: what came before it
 * is kept and the next compaction drops the rest. Payloads use LEB128 varints:
 *   NH_APP   varint index, the app id's bytes (1..127 of [A-Za-z0-9._:@+~/-])
 *   NH_MIN   u32 minute (Unix time / 60), varint rx, varint tx, varint n, n × (index, rx, tx)
 *   NH_HOUR  u32 hour (Unix time / 3600), the same
 * A minute is appended when it ends (one write of a few dozen bytes); compaction folds minutes older
 * than 7 days into hours and drops hours older than 30 days (at startup and every 6 hours). */
#ifndef LORD_NETHIST_H
#define LORD_NETHIST_H

#include <stddef.h>
#include <stdint.h>

#define NH_APPS_MAX   1024              /* app ids per file; later ones go to the "other" slot */
#define NH_ID_MAX     128
#define NH_MIN_KEEP   (7 * 24 * 60)     /* minutes */
#define NH_HOUR_KEEP  (30 * 24)         /* hours */
#define NH_FILE_MAX   (8u << 20)        /* a bigger file is not read (it can't be ours) */
#define NH_BUCKETS_MAX 1500

typedef struct NetHist NetHist;

/* A history for path (its folder is created 0700). Nothing is read until the first call that needs it. */
NetHist *nh_new(const char *path);
void     nh_free(NetHist *h);           /* writes the minute in progress first */

/* Bytes moved since the last call, at Unix time now: app NULL = the machine's total. */
void nh_add(NetHist *h, long long now, const char *app, uint64_t rx, uint64_t tx);
/* Writes the minutes that ended before now; compacts when due. Returns 0, or -1 on a write error. */
int  nh_flush(NetHist *h, long long now);
/* Folds old minutes into hours, drops what is past the limits and any bad tail (atomic rewrite). */
int  nh_compact(NetHist *h, long long now);
/* Deletes everything (the file and what is in memory). */
int  nh_clear(NetHist *h);

/* Buckets [from + i*step, from + (i+1)*step) for i < n: received and sent bytes of app (NULL: the
 * machine). A record longer than a bucket (an hour) is spread over the buckets it covers. Also the
 * apps with the most traffic over the range (up to max_top, by rx + tx) and the oldest time
 * recorded. Returns the finest resolution found in the range (60 or 3600 s), 0 with no data, -1 on
 * bad arguments. Reads the file; the minute in progress is included. */
typedef struct { char id[NH_ID_MAX]; uint64_t rx, tx; } NhTop;
int nh_query(NetHist *h, long long now, long long from, long long step, int n, const char *app,
             double *rx, double *tx, NhTop *top, int max_top, int *ntop, long long *oldest);

/* For tests: the records of a file buffer (0 = all good, otherwise the offset where it stopped). */
size_t nh_check(const uint8_t *buf, size_t len, int *records);

#endif
