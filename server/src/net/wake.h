/* wake.h — Wi-Fi after the computer wakes from sleep (issue #14; see wake.c). */
#ifndef LORD_WAKE_H
#define LORD_WAKE_H

#define WAKE_MAX        16
#define WAKE_SETTLE_MS  3000    /* first look: the driver and wpa_supplicant get a moment after waking */
#define WAKE_CHECK_MS   1000    /* then one look per second */
#define WAKE_REASSOC_MS 8000    /* not working this long after waking: re-associate and renew DHCP */
#define WAKE_RESET_MS   12000   /* not working this long after re-associating: the card off and on */
#define WAKE_STOP_MS    5000    /* at most this long waiting for its daemons to exit */
#define WAKE_GIVEUP_MS  20000   /* after the card came back on: give up and say so */

/* What failed (the first thing that did). */
enum { WK_OK = 0, WK_NOT_ASSOCIATED, WK_NO_CARRIER, WK_NO_ADDRESS, WK_NO_GATEWAY };

typedef struct {
    int  (*active)(const char *name);       /* still LordOS's and switched on (net-down: leave it alone) */
    int  (*health)(const char *name);       /* WK_OK, or what fails */
    void (*renew)(const char *name);        /* the DHCP client confirms its lease */
    void (*reassociate)(const char *name);  /* wpa_supplicant drops the association and joins again; renews DHCP */
    void (*reset_stop)(const char *name);   /* "the card off": its daemons stop and the link goes down */
    int  (*busy)(const char *name);         /* those daemons haven't exited yet */
    void (*reset_start)(const char *name);  /* "the card on": link up and its saved config, as at boot */
    void (*log)(const char *name, const char *what, int why);
} WakeOps;

enum { WS_WATCH = 1, WS_REASSOC, WS_STOPPING, WS_RESET };
typedef struct { char name[16]; int phase, why; long since_ms, next_ms; } WakeIf;
typedef struct { WakeIf v[WAKE_MAX]; int n; } WakeTable;

/* The computer woke at ms: these Wi-Fi interfaces get looked at (an earlier round still going is dropped). */
void wake_begin(WakeTable *t, const char *const *names, int n, long ms);
/* One pass. Returns the ms at which it wants the next one, or -1 when nothing is left. */
long wake_step(WakeTable *t, const WakeOps *o, long ms);
const char *wake_why(int why);

#endif
