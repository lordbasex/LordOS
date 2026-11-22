/* drmlcore.c — the DRM lease state machine (see drmlcore.h). No Wayland, no ioctls: everything goes through DrmlOps. */
#include "drmlcore.h"

#include <stdio.h>
#include <string.h>

void drml_init(Drml *d, const DrmlOps *ops) {
    memset(d, 0, sizeof *d);
    d->ops = ops;
}

const DrmlConn *drml_conn(const Drml *d, uint32_t id) {
    for (int i = 0; i < d->nconn; i++) if (d->conn[i].id == id) return &d->conn[i];
    return NULL;
}

static int leased(const Drml *d, uint32_t id) {
    for (int i = 0; i < DRML_LEASE_MAX; i++) {
        const DrmlLease *l = &d->lease[i];
        if (l->state != DRML_ACTIVE) continue;
        for (int j = 0; j < l->n; j++) if (l->conns[j] == id) return 1;
    }
    return 0;
}

int drml_present(const Drml *d) { return d->attached ? d->nconn : 0; }
int drml_offered(const Drml *d, uint32_t id) { return d->attached && drml_conn(d, id) && !leased(d, id); }

int drml_waiting(const Drml *d, const char *app) {
    for (int i = 0; app && app[0] && i < DRML_LEASE_MAX; i++)
        if (d->lease[i].state == DRML_PENDING && !strcmp(d->lease[i].app, app)) return 1;
    return 0;
}

static uint32_t crtcs_leased(const Drml *d) {
    uint32_t m = 0;
    for (int i = 0; i < DRML_LEASE_MAX; i++) if (d->lease[i].state == DRML_ACTIVE) m |= d->lease[i].crtcs;
    return m;
}

/* A lease that ends: revoked in the kernel when it was granted (while lord-server is master it always can be; after
 * lord-sessiond took master away it can't, but such a lessee can't use the card either, see docs/WAYLAND.md), the
 * client told "finished" (send: its object is still there), the slot freed. */
static void end(Drml *d, DrmlLease *l, int send) {
    int was_active = l->state == DRML_ACTIVE;
    if (was_active && d->ops->revoke(d->ops->u, l->lessee))
        fprintf(stderr, "wayland: drm-lease: lessee %u not revoked (no longer DRM master)\n", l->lessee);
    void *token = l->token;
    memset(l, 0, sizeof *l);
    if (was_active) d->ops->reserve(d->ops->u, crtcs_leased(d));
    if (send) d->ops->finished(d->ops->u, token);
    if (was_active) d->ops->offers(d->ops->u);
}

/* Every connector of the request still offered: the lease is made and its fd sent; otherwise "finished". */
static int grant(Drml *d, DrmlLease *l, int always) {
    for (int j = 0; j < l->n; j++)
        if (!drml_offered(d, l->conns[j])) { end(d, l, 1); return DRML_NEVER; }
    uint32_t lessee = 0, crtcs = 0;
    int fd = d->ops->lease(d->ops->u, l->conns, l->n, crtcs_leased(d), &lessee, &crtcs);
    if (fd < 0) { end(d, l, 1); return DRML_NEVER; }
    l->state = DRML_ACTIVE;
    l->always = always;
    l->lessee = lessee;
    l->crtcs = crtcs;
    d->ops->reserve(d->ops->u, crtcs_leased(d));
    d->ops->granted(d->ops->u, l->token, fd);
    d->ops->offers(d->ops->u);          /* its connectors are withdrawn from everyone while it lasts */
    return DRML_GRANT;
}

void drml_rescan(Drml *d) {
    if (!d->attached) return;
    DrmlConn all[DRML_CONN_MAX * 2];
    int n = d->ops->scan(d->ops->u, all, DRML_CONN_MAX * 2), changed = 0;
    DrmlConn now[DRML_CONN_MAX];
    int nn = 0;
    for (int i = 0; i < n && nn < DRML_CONN_MAX; i++) if (drml_offerable(&all[i])) now[nn++] = all[i];
    if (nn != d->nconn) changed = 1;
    for (int i = 0; i < nn && !changed; i++) {
        const DrmlConn *was = drml_conn(d, now[i].id);
        changed = !was || strcmp(was->desc, now[i].desc) || strcmp(was->name, now[i].name);
    }
    memcpy(d->conn, now, sizeof now[0] * (size_t)nn);
    d->nconn = nn;
    /* a lease (or a request) naming a connector that went away ends */
    for (int i = 0; i < DRML_LEASE_MAX; i++) {
        DrmlLease *l = &d->lease[i];
        if (l->state == DRML_FREE) continue;
        int gone = 0;
        for (int j = 0; j < l->n; j++) gone |= !drml_conn(d, l->conns[j]);
        if (gone) end(d, l, 1);
    }
    if (changed) d->ops->offers(d->ops->u);
}

void drml_card(Drml *d, int attached) {
    if (attached) {
        d->attached = 1;
        drml_rescan(d);
        return;
    }
    if (!d->attached) return;
    for (int i = 0; i < DRML_LEASE_MAX; i++) if (d->lease[i].state != DRML_FREE) end(d, &d->lease[i], 1);
    d->attached = 0;
    d->nconn = 0;
    d->ops->reserve(d->ops->u, 0);
    d->ops->offers(d->ops->u);
}

int drml_submit(Drml *d, void *token, const char *app, const uint32_t *conns, int n, int trusted) {
    DrmlLease *l = NULL;
    for (int i = 0; i < DRML_LEASE_MAX && !l; i++) if (d->lease[i].state == DRML_FREE) l = &d->lease[i];
    int named = app && app[0] && strlen(app) < DRML_APP_MAX;
    int how = drml_decide(named ? d->ops->perm(d->ops->u, app) : DRML_ASK, trusted, named);
    if (!l || how == DRML_NEVER || !d->attached || n < 1 || n > DRML_PER_LEASE) { d->ops->finished(d->ops->u, token); return DRML_NEVER; }
    for (int j = 0; j < n; j++)
        if (!drml_offered(d, conns[j])) { d->ops->finished(d->ops->u, token); return DRML_NEVER; }
    memset(l, 0, sizeof *l);
    l->token = token;
    snprintf(l->app, sizeof l->app, "%s", app);
    memcpy(l->conns, conns, sizeof conns[0] * (size_t)n);
    l->n = n;
    if (how == DRML_GRANT) return grant(d, l, 1);
    int asked = drml_waiting(d, app);   /* one prompt per app, whatever number of requests wait */
    l->state = DRML_PENDING;
    if (!asked) d->ops->ask(d->ops->u, app, drml_conn(d, conns[0]));
    return DRML_PROMPT;
}

int drml_answer(Drml *d, const char *app, int answer) {
    if (!drml_waiting(d, app)) return 0;
    for (int i = 0; i < DRML_LEASE_MAX; i++) {
        DrmlLease *l = &d->lease[i];
        if (l->state != DRML_PENDING || strcmp(l->app, app)) continue;
        if (answer == DRML_ANSWER_ONCE || answer == DRML_ANSWER_ALWAYS) grant(d, l, answer == DRML_ANSWER_ALWAYS);
        else end(d, l, 1);
    }
    return 1;
}

void drml_perm_changed(Drml *d) {
    for (int i = 0; i < DRML_LEASE_MAX; i++) {
        DrmlLease *l = &d->lease[i];
        if (l->state == DRML_FREE) continue;
        int st = d->ops->perm(d->ops->u, l->app);
        if (l->state == DRML_PENDING) {
            if (st == DRML_ALLOW) grant(d, l, 1);
            else if (st == DRML_DENY) end(d, l, 1);
        } else if (st == DRML_DENY || (l->always && st != DRML_ALLOW)) {
            end(d, l, 1);               /* taken back: "allow once" ends only on "deny" */
        }
    }
}

void drml_destroy(Drml *d, void *token) {
    for (int i = 0; i < DRML_LEASE_MAX; i++)
        if (d->lease[i].state != DRML_FREE && d->lease[i].token == token) end(d, &d->lease[i], 0);
}
