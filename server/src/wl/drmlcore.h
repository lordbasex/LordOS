/* drmlcore.h — DRM leases (drm-lease-v1) without Wayland or the kernel: which connectors are offered, who may lease
 * one, and when a lease ends (docs/WAYLAND.md, "DRM leases"; approved by the maintainer 2026-11-22). wl/drmlease.c
 * speaks the protocol and gives the real DRM calls; tests/drmlease_test.c gives fake ones.
 *
 * Only connectors the kernel marks "non-desktop" (a VR headset) are ever offered: LordOS never lights them itself
 * (kms.c counts them as not connected), and a display LordOS drives is never leased. A lease is granted only to a
 * client lord-server could name (an identity it checked, never one an app set) that is not sandboxed, not an isolated
 * LordOS app, not Xwayland and not on the login screen, and only with the "vr" permission: allowed always, or allowed
 * once from the prompt. It ends (REVOKE_LEASE, then "finished") when the client destroys it or leaves, when the
 * permission is taken back, when the headset is unplugged, and when the session leaves the monitor (a lock, a user
 * switch, a text console, logging out). */
#ifndef LORD_WL_DRMLCORE_H
#define LORD_WL_DRMLCORE_H

#include <stdint.h>

#define DRML_CONN_MAX 8         /* non-desktop connectors on the card */
#define DRML_LEASE_MAX 16       /* lease objects alive (waiting for the person, or leased) */
#define DRML_PER_LEASE 4        /* connectors in one lease */
#define DRML_APP_MAX 192        /* an identity with its NUL (perms.h PERM_APP_MAX, kbinhibit.h KBI_ID_MAX) */

typedef struct {
    uint32_t id;
    int connected, non_desktop;
    char name[32];              /* "DP-2" (as a wl_output would be named) */
    char desc[96];              /* the EDID's name ("Valve Index"), or the name */
} DrmlConn;

/* A permission's saved state, as perms.h's PermState: 0 ask, 1 allow, 2 deny */
enum { DRML_ASK = 0, DRML_ALLOW = 1, DRML_DENY = 2 };
/* What a lease request gets: never (a client that may not lease, or the person said no), the prompt, or the lease */
enum { DRML_NEVER, DRML_PROMPT, DRML_GRANT };
/* The person's answer to the prompt */
enum { DRML_ANSWER_DENY, DRML_ANSWER_ONCE, DRML_ANSWER_ALWAYS };

typedef struct {
    /* every connector of the card (the core keeps the connected non-desktop ones); how many */
    int  (*scan)(void *u, DrmlConn *out, int max);
    /* a lease of these connectors, each with a free CRTC not in avoid and its primary plane: the lessee's fd (-1 when
     * the kernel or the card refuses), *lessee its id and *crtcs the CRTC bits it took */
    int  (*lease)(void *u, const uint32_t *conns, int n, uint32_t avoid, uint32_t *lessee, uint32_t *crtcs);
    int  (*revoke)(void *u, uint32_t lessee);              /* 0 if the kernel revoked it */
    void (*reserve)(void *u, uint32_t crtcs);              /* the CRTCs leased now: no head of LordOS takes them */
    int  (*perm)(void *u, const char *app);                /* the "vr" decision saved for app (DRML_ASK…) */
    void (*granted)(void *u, void *token, int fd);         /* lease_fd (the callee owns fd) */
    void (*finished)(void *u, void *token);                /* finished: refused, or revoked */
    void (*offers)(void *u);                               /* the connectors on offer changed (drml_offered) */
    void (*ask)(void *u, const char *app, const DrmlConn *c);   /* show the prompt for app, naming the headset */
    void *u;
} DrmlOps;

enum { DRML_FREE, DRML_PENDING, DRML_ACTIVE };
typedef struct {
    int state, always;          /* always: granted by a saved "allow" (it ends when that is taken back) */
    void *token;                /* the protocol's lease object */
    char app[DRML_APP_MAX];
    uint32_t conns[DRML_PER_LEASE];
    int n;
    uint32_t lessee, crtcs;
} DrmlLease;

typedef struct {
    const DrmlOps *ops;
    int attached;               /* lord-server has the card (it is DRM master) */
    DrmlConn conn[DRML_CONN_MAX];
    int nconn;
    DrmlLease lease[DRML_LEASE_MAX];
} Drml;

/* Pure decisions. trusted: a client that may lease at all (not sandboxed, isolated, Xwayland or the login screen);
 * named: lord-server could tell who it is. state: the saved "vr" decision. */
static inline int drml_decide(int state, int trusted, int named) {
    if (!trusted || !named || state == DRML_DENY) return DRML_NEVER;
    return state == DRML_ALLOW ? DRML_GRANT : DRML_PROMPT;
}
static inline int drml_offerable(const DrmlConn *c) { return c->connected && c->non_desktop; }

void drml_init(Drml *d, const DrmlOps *ops);
/* lord-server got the card (1) or lost it (0: every lease ends, nothing is offered) */
void drml_card(Drml *d, int attached);
void drml_rescan(Drml *d);                 /* a hotplug: connectors come and go, a lease of one unplugged ends */
int  drml_present(const Drml *d);          /* how many non-desktop connectors are plugged in (the global exists) */
int  drml_offered(const Drml *d, uint32_t id);     /* offered now: plugged in, and not leased */
const DrmlConn *drml_conn(const Drml *d, uint32_t id);
/* A submitted request (token: the lease object). Returns DRML_NEVER (finished already sent), DRML_PROMPT (waiting for
 * the person; the prompt is shown once per app) or DRML_GRANT (lease_fd sent). */
int  drml_submit(Drml *d, void *token, const char *app, const uint32_t *conns, int n, int trusted);
/* The person's answer for app (the caller saved it first when ALWAYS or DENY). 0 if no request of app was waiting. */
int  drml_answer(Drml *d, const char *app, int answer);
void drml_perm_changed(Drml *d);           /* a "vr" decision changed (Settings → Privacy) */
void drml_destroy(Drml *d, void *token);   /* the client destroyed its lease object, or left */
int  drml_waiting(const Drml *d, const char *app);   /* a request of app waits for the person */

#endif
