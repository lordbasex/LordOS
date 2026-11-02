/* netrec.h — records the traffic history (nethist.h) from what lord-server already reads: the
 * per-app counters lord-netperm sends (js_fw.c) and /proc/net/dev, every 5 s. See docs/MONITOR.md,
 * "Traffic history". */
#ifndef LORD_NETREC_H
#define LORD_NETREC_H

#include "nethist.h"

/* allowed: a user session (not the login screen, not an isolated app). Until then nothing runs. */
void     netrec_init(int allowed);
/* Settings → Privacy → Monitor → "Keep a traffic history" (the SDK calls it with the setting). */
void     netrec_enable(int on);
int      netrec_enabled(void);
void     netrec_tick(void);             /* from the main loop; does something every 5 s */
double   netrec_timeout(void);          /* ms until the next tick, -1 when off */
NetHist *netrec_hist(void);             /* NULL when not allowed */
long long netrec_now(void);             /* Unix time */
void     netrec_shutdown(void);         /* writes the minute in progress */
int      netrec_totals(uint64_t *rx, uint64_t *tx);   /* (0.8.5) every interface but lo, now; 0 ok */

#endif
