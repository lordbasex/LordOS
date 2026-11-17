/* batthist.h — the computer battery's history for Settings → Battery (docs/BATTERY.md): what the iPhone shows as
 * Battery Usage, kept by lord-server in ~/.local/state/lordos/battery-history, never sent anywhere.
 *
 * Only on a computer with a battery. On every power reading (a uevent, or the 30 s poll in hw.c) it adds:
 *   - the level every 10 minutes, for the last 30 days;
 *   - how much was discharged (in tenths of a percent): per day, and in total for the cycles LordOS counts when the
 *     battery reports none (100 % discharged = one cycle);
 *   - the time on battery with the screen seen and not seen, per day;
 *   - each app's CPU time on battery per day, from its cgroup scope's cpu.stat (the session's scopes: app-lordos-*,
 *     lordos-*, and lord-server's own leaf), for the estimated use per app;
 *   - the last time charging ended, and at what level;
 *   - the first day LordOS saw this battery.
 * The file is plain lines ("s <t> <p> <flags>"…), parsed strictly; anything malformed is skipped. */
#ifndef LORD_HW_BATTHIST_H
#define LORD_HW_BATTHIST_H

#include "power.h"
#include <stddef.h>
#include <time.h>

#define BH_SAMPLES 4320            /* 30 days at one per 10 minutes */
#define BH_DAYS 10
#define BH_APPS 24                 /* scopes kept per day */

typedef struct { long long t; int p, flags; } BhSample;          /* flags: 1 charging, 2 on AC */
typedef struct { char scope[80]; long long usec; } BhApp;
typedef struct { int ymd; long long drain, on_s, off_s; BhApp apps[BH_APPS]; int napps; } BhDay;
typedef struct {
    long long first_seen, drain_total, last_charge_t;
    int last_charge_p;
    BhSample s[BH_SAMPLES];
    int ns;
    BhDay d[BH_DAYS];
    int nd;
    /* not saved: the last tick's view */
    long long prev_t, prev_sample_t, prev_apps_t;
    int prev_p, prev_charging, have_prev;
} BattHist;

/* One tick: now (seconds since the epoch), today (yyyymmdd, local), the system battery (NULL without one), whether
 * someone sees the screen, and each scope's CPU so far (usec; NULL/0 to skip). Pure (tested). */
typedef struct { char scope[80]; long long usec; } BhScope;
void bh_tick(BattHist *h, long long now, int today, const PowerSupply *bat, int on_ac, int seen,
             const BhScope *scopes, int nscopes);
/* the saved text, and back; 0 ok */
size_t bh_save(const BattHist *h, char *out, size_t n);
void bh_load(BattHist *h, const char *text);
/* JSON for lord.power.history(): { firstSeen, cycles (LordOS's count, one decimal), lastCharge {t,p}, samples
 * [[t,p,flags]] of the last 26 hours, days [{ ymd, drain, on, off, apps {scope: usec} }] } */
size_t bh_json(const BattHist *h, long long now, char *out, size_t n);
/* A scope name as the session's cgroups have them: [A-Za-z0-9._-], ends ".scope" or is "server". */
int bh_scope_ok(const char *s);

/* lord-server's side: called from hw.c with the reading it just made */
void batthist_tick(const PowerState *ps, int seen);
size_t batthist_json(char *out, size_t n);

#endif
