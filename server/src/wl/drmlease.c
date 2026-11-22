/* drmlease.c — drm-lease-v1 on lord-server's card (see drmlease.h and drmlcore.h).
 *
 * One wp_drm_lease_device_v1 global, for the card lord-server is DRM master of, and only while a connector the kernel
 * marks "non-desktop" is plugged into it; it is hidden (secctx_set_gate) from every client that may never lease. A
 * bound device gets drm_fd: a fresh open of the same card that is not DRM master (and never was one: a file that was
 * master could take master back later), unauthenticated. Leases are made with DRM_IOCTL_MODE_CREATE_LEASE (the
 * connector, a free CRTC and its primary plane) and ended with DRM_IOCTL_MODE_REVOKE_LEASE. No libdrm. */
#include "drmlease.h"
#include "drmlcore.h"
#include "secctx.h"
#include "../kms.h"
#include "../hw/edid.h"
#include "../sec/perms.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <wayland-server-core.h>
#include "drm-lease-v1-protocol.h"

static struct wl_display *ldpy;
static DrmlHooks hooks;
static Drml D;
static int card_fd = -1;
static struct wl_global *global;
static struct wl_list devs;             /* Dev.link */

typedef struct {
    struct wl_resource *res;
    struct wl_list link, conns;         /* ConnRes.link */
    int dead;                           /* its global went away, or the client may not lease: nothing is offered */
    int fd_sent;
    unsigned serial;                    /* how its connectors and requests name it (they may outlive it) */
} Dev;
typedef struct {
    struct wl_resource *res;
    struct wl_list link;
    unsigned dev;                       /* its device's serial */
    uint32_t id;
    int withdrawn;
} ConnRes;
typedef struct {
    unsigned dev;                       /* its device's serial */
    uint32_t ids[DRML_PER_LEASE];
    int n, stale;                       /* stale: a withdrawn connector was named, or more than DRML_PER_LEASE */
} Req;
typedef struct { struct wl_resource *res; } LeaseObj;

static unsigned next_serial = 1;
static Dev *dev_of(unsigned serial) {
    Dev *d;
    wl_list_for_each(d, &devs, link) if (d->serial == serial) return d;
    return NULL;
}

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

/* DRM master on fd: AUTH_MAGIC is a master-only ioctl, so anything but EACCES means master (libdrm's drmIsMaster) */
static int is_master(int fd) {
    struct drm_auth a = { .magic = 0 };
    return xioctl(fd, DRM_IOCTL_AUTH_MAGIC, &a) == 0 || errno != EACCES;
}

/* drm_fd: the card opened again, only while lord-server is master of it (then a new open never becomes master). A file
 * that became master anyway (the master went away meanwhile) is closed, never handed out. -1 when it can't be done. */
static int open_nonmaster(void) {
    if (card_fd < 0 || !is_master(card_fd)) return -1;
    char link[64], path[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", card_fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n <= 0 || n >= (ssize_t)sizeof path - 1) return -1;
    path[n] = 0;
    if (strncmp(path, "/dev/dri/card", 13) || strstr(path, "..")) return -1;
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return -1;
    struct stat a, b;
    if (fstat(fd, &a) || fstat(card_fd, &b) || !S_ISCHR(a.st_mode) || a.st_rdev != b.st_rdev || is_master(fd)) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- the real DRM layer (drmlcore.h DrmlOps) ---- */

static int op_scan(void *u, DrmlConn *out, int max) {
    (void)u;
    KmsOutput o[KMS_MAX_OUTPUTS];
    int n = card_fd >= 0 ? kms_outputs(card_fd, o, KMS_MAX_OUTPUTS) : 0, k = 0;
    for (int i = 0; i < n && k < max; i++) {
        DrmlConn *c = &out[k++];
        memset(c, 0, sizeof *c);
        c->id = o[i].id;
        c->connected = o[i].connected || o[i].non_desktop;
        c->non_desktop = o[i].non_desktop;
        snprintf(c->name, sizeof c->name, "%s-%u", kms_connector_type_name(o[i].type), o[i].type_id);
        Edid e;
        if (o[i].edid && edid_parse(o[i].edid, o[i].edid_len, &e) == 0) edid_display_name(&e, c->desc, sizeof c->desc);
        if (!c->desc[0]) snprintf(c->desc, sizeof c->desc, "%s", c->name);
    }
    kms_outputs_free(o, n);
    return k;
}

static int op_lease(void *u, const uint32_t *conns, int n, uint32_t avoid, uint32_t *lessee, uint32_t *crtcs) {
    (void)u;
    if (card_fd < 0 || n < 1 || n > DRML_PER_LEASE) return -1;
    KmsOutput o[KMS_MAX_OUTPUTS];
    int no = kms_outputs(card_fd, o, KMS_MAX_OUTPUTS), nobj = 0, ok = 1;
    uint32_t objs[DRML_PER_LEASE * 3], taken = 0;
    for (int j = 0; j < n && ok; j++) {
        const KmsOutput *k = NULL;
        for (int i = 0; i < no; i++) if (o[i].id == conns[j]) k = &o[i];
        uint32_t bit = 0;
        ok = k && kms_lease_objects(card_fd, k, avoid | taken, &objs[nobj], &bit) == 0;
        if (ok) { nobj += 3; taken |= bit; }
    }
    kms_outputs_free(o, no);
    if (!ok) { fprintf(stderr, "wayland: drm-lease: no free CRTC and plane for the headset\n"); return -1; }
    struct drm_mode_create_lease cl = { .object_ids = (uintptr_t)objs, .object_count = (uint32_t)nobj, .flags = O_CLOEXEC };
    if (xioctl(card_fd, DRM_IOCTL_MODE_CREATE_LEASE, &cl)) {
        fprintf(stderr, "wayland: drm-lease: CREATE_LEASE: %s\n", strerror(errno));
        return -1;
    }
    *lessee = cl.lessee_id;
    *crtcs = taken;
    fprintf(stderr, "wayland: drm-lease: lessee %u (%d connector(s))\n", cl.lessee_id, n);
    return (int)cl.fd;
}

static int op_revoke(void *u, uint32_t lessee) {
    (void)u;
    struct drm_mode_revoke_lease r = { .lessee_id = lessee };
    if (card_fd < 0 || xioctl(card_fd, DRM_IOCTL_MODE_REVOKE_LEASE, &r)) return -1;
    fprintf(stderr, "wayland: drm-lease: lessee %u revoked\n", lessee);
    return 0;
}

static void op_reserve(void *u, uint32_t crtcs) { (void)u; kms_set_leased_crtcs(crtcs); }
static int op_perm(void *u, const char *app) { (void)u; return (int)perm_get(app, PERM_VR); }

static void op_granted(void *u, void *token, int fd) {
    (void)u;
    LeaseObj *l = token;
    wp_drm_lease_v1_send_lease_fd(l->res, fd);     /* libwayland dups it for the message */
    close(fd);
    wl_client_flush(wl_resource_get_client(l->res));
}
static void op_finished(void *u, void *token) {
    (void)u;
    LeaseObj *l = token;
    wp_drm_lease_v1_send_finished(l->res);
    wl_client_flush(wl_resource_get_client(l->res));
}

static void sync_offers(void);
static void op_offers(void *u) { (void)u; sync_offers(); }
static void op_ask(void *u, const char *app, const DrmlConn *c) {
    (void)u;
    char h[160];
    if (c && strcmp(c->desc, c->name)) snprintf(h, sizeof h, "%s (%s)", c->desc, c->name);
    else snprintf(h, sizeof h, "%s", c ? c->name : "");
    if (hooks.ask) hooks.ask(app, h);
}

static DrmlOps ops = { .scan = op_scan, .lease = op_lease, .revoke = op_revoke, .reserve = op_reserve, .perm = op_perm,
                             .granted = op_granted, .finished = op_finished, .offers = op_offers, .ask = op_ask };

static int (*open_fd)(void) = open_nonmaster;
#ifdef DRMLEASE_TEST
/* tests/drmlease_e2e_test.c: the protocol end to end over a fake card (the kernel's part replaced, nothing else) */
void drmlease_test_drm(int (*scan)(void *, DrmlConn *, int), int (*lease)(void *, const uint32_t *, int, uint32_t, uint32_t *, uint32_t *),
                       int (*revoke)(void *, uint32_t), int (*perm)(void *, const char *), int (*open)(void)) {
    ops.scan = scan;
    ops.lease = lease;
    ops.revoke = revoke;
    ops.perm = perm;
    open_fd = open;
}
#endif

/* ---- the conformance tool's fake card (lord-server --headless --test-hooks, testhooks.h "lease") ----
 * A desktop display and, while "plugged", a non-desktop one (a VR headset). drm_fd and every lease_fd are memfds and
 * the lessee ids are counted here: no ioctl reaches a card. Everything else is the real code: drmlcore.c's rules, the
 * "vr" permission (perms.c, the person's answer from the theme's prompt) and the protocol. */
static int fake_plugged, fake_card = -1;
static uint32_t fake_lessee;
static int fk_scan(void *u, DrmlConn *out, int max) {
    (void)u;
    int n = 0;
    if (max > n) out[n++] = (DrmlConn){ .id = 41, .connected = 1, .non_desktop = 0, .name = "DP-1", .desc = "LordOS test display" };
    if (fake_plugged && max > n) out[n++] = (DrmlConn){ .id = 42, .connected = 1, .non_desktop = 1, .name = "DP-2", .desc = "LordOS test headset" };
    return n;
}
static int fk_lease(void *u, const uint32_t *conns, int n, uint32_t avoid, uint32_t *lessee, uint32_t *crtcs) {
    (void)u; (void)conns; (void)avoid;
    if (n < 1 || n > DRML_PER_LEASE) return -1;
    *lessee = ++fake_lessee;
    *crtcs = 0;                         /* no head of the headless frame is ever reserved */
    return memfd_create("lordos-test-lease", MFD_CLOEXEC);
}
static int fk_revoke(void *u, uint32_t lessee) { (void)u; (void)lessee; return 0; }
static int fk_open(void) { return memfd_create("lordos-test-drm", MFD_CLOEXEC); }
int drmlease_fake_card(int plugged) {
    if (!ldpy) return -1;
    if (fake_card < 0) {
        fake_card = memfd_create("lordos-test-card", MFD_CLOEXEC);
        if (fake_card < 0) return -1;
        ops.scan = fk_scan;
        ops.lease = fk_lease;
        ops.revoke = fk_revoke;
        open_fd = fk_open;
    }
    fake_plugged = !!plugged;
    drmlease_card(fake_card);           /* the first time the card arrives, later a rescan (hotplug) */
    return 0;
}

/* ---- wp_drm_lease_connector_v1 ---- */

static void conn_gone(struct wl_resource *r) {
    ConnRes *c = wl_resource_get_user_data(r);
    if (!c) return;
    wl_list_remove(&c->link);
    free(c);
}
static void conn_destroy(struct wl_client *cl, struct wl_resource *r) { (void)cl; wl_resource_destroy(r); }
static const struct wp_drm_lease_connector_v1_interface conn_impl = { .destroy = conn_destroy };

/* A device gets drm_fd once, then the connectors offered now that it lacks, the withdrawn event for those no longer
 * offered, and done when anything changed. */
static void sync_dev(Dev *d) {
    int changed = 0;
    if (!d->fd_sent && !d->dead) {
        int fd = open_fd();
        if (fd < 0) return;             /* again at the next attach or hotplug (the spec lets drm_fd wait) */
        wp_drm_lease_device_v1_send_drm_fd(d->res, fd);
        close(fd);
        d->fd_sent = 1;
        changed = 1;                    /* the first list ends with done, even an empty one */
    }
    ConnRes *c;
    wl_list_for_each(c, &d->conns, link) {
        if (c->withdrawn || (!d->dead && drml_offered(&D, c->id))) continue;
        wp_drm_lease_connector_v1_send_withdrawn(c->res);
        c->withdrawn = 1;
        changed = 1;
    }
    for (int i = 0; !d->dead && d->fd_sent && i < D.nconn; i++) {
        const DrmlConn *k = &D.conn[i];
        int have = 0;
        wl_list_for_each(c, &d->conns, link) have |= !c->withdrawn && c->id == k->id;
        if (have || !drml_offered(&D, k->id)) continue;
        struct wl_client *cl = wl_resource_get_client(d->res);
        ConnRes *n = calloc(1, sizeof *n);
        struct wl_resource *r = n ? wl_resource_create(cl, &wp_drm_lease_connector_v1_interface, wl_resource_get_version(d->res), 0) : NULL;
        if (!r) { free(n); wl_client_post_no_memory(cl); return; }
        n->res = r;
        n->dev = d->serial;
        n->id = k->id;
        wl_list_insert(d->conns.prev, &n->link);
        wl_resource_set_implementation(r, &conn_impl, n, conn_gone);
        wp_drm_lease_device_v1_send_connector(d->res, r);
        wp_drm_lease_connector_v1_send_name(r, k->name);
        wp_drm_lease_connector_v1_send_description(r, k->desc);
        wp_drm_lease_connector_v1_send_connector_id(r, k->id);
        wp_drm_lease_connector_v1_send_done(r);
        changed = 1;
    }
    if (changed && d->fd_sent) wp_drm_lease_device_v1_send_done(d->res);
    if (changed) wl_client_flush(wl_resource_get_client(d->res));
}
static void sync_offers(void) {
    Dev *d;
    if (ldpy) wl_list_for_each(d, &devs, link) sync_dev(d);
}

/* ---- wp_drm_lease_v1 ---- */

static void lease_gone(struct wl_resource *r) {
    LeaseObj *l = wl_resource_get_user_data(r);
    drml_destroy(&D, l);                /* revoked when it was granted; a waiting request is dropped */
    free(l);
}
static void lease_destroy(struct wl_client *cl, struct wl_resource *r) { (void)cl; wl_resource_destroy(r); }
static const struct wp_drm_lease_v1_interface lease_impl = { .destroy = lease_destroy };

/* ---- wp_drm_lease_request_v1 ---- */

static void req_gone(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }
static void req_connector(struct wl_client *cl, struct wl_resource *r, struct wl_resource *conn) {
    (void)cl;
    Req *q = wl_resource_get_user_data(r);
    ConnRes *c = conn ? wl_resource_get_user_data(conn) : NULL;
    if (!c || c->dev != q->dev) {
        wl_resource_post_error(r, WP_DRM_LEASE_REQUEST_V1_ERROR_WRONG_DEVICE, "connector of another lease device");
        return;
    }
    for (int j = 0; j < q->n; j++)
        if (q->ids[j] == c->id) {
            wl_resource_post_error(r, WP_DRM_LEASE_REQUEST_V1_ERROR_DUPLICATE_CONNECTOR, "connector requested twice");
            return;
        }
    if (c->withdrawn || q->n >= DRML_PER_LEASE) { q->stale = 1; return; }
    q->ids[q->n++] = c->id;
}
static void req_submit(struct wl_client *cl, struct wl_resource *r, uint32_t id) {
    Req *q = wl_resource_get_user_data(r);
    if (!q->n && !q->stale) {
        wl_resource_post_error(r, WP_DRM_LEASE_REQUEST_V1_ERROR_EMPTY_LEASE, "no connector requested");
        return;
    }
    LeaseObj *l = calloc(1, sizeof *l);
    struct wl_resource *lr = l ? wl_resource_create(cl, &wp_drm_lease_v1_interface, wl_resource_get_version(r), id) : NULL;
    if (!lr) { free(l); wl_client_post_no_memory(cl); wl_resource_destroy(r); return; }
    l->res = lr;
    wl_resource_set_implementation(lr, &lease_impl, l, lease_gone);
    Dev *d = dev_of(q->dev);            /* a device released meanwhile: its request still gets an answer, never a lease */
    int trusted = d && !d->dead && !q->stale && hooks.trusted && hooks.trusted(cl);
    const char *app = hooks.identity ? hooks.identity(cl) : "";
    drml_submit(&D, l, app, q->ids, q->n, trusted);    /* finished, the prompt, or lease_fd */
    wl_resource_destroy(r);
}
static const struct wp_drm_lease_request_v1_interface req_impl = { .request_connector = req_connector, .submit = req_submit };

/* ---- wp_drm_lease_device_v1 ---- */

static void dev_gone(struct wl_resource *r) {
    Dev *d = wl_resource_get_user_data(r);
    ConnRes *c, *t;
    wl_list_for_each_safe(c, t, &d->conns, link) wl_list_init(&c->link);   /* its connector objects stay until destroyed */
    wl_list_remove(&d->link);
    free(d);
}
static void dev_create_request(struct wl_client *cl, struct wl_resource *r, uint32_t id) {
    Req *q = calloc(1, sizeof *q);
    struct wl_resource *qr = q ? wl_resource_create(cl, &wp_drm_lease_request_v1_interface, wl_resource_get_version(r), id) : NULL;
    if (!qr) { free(q); wl_client_post_no_memory(cl); return; }
    q->dev = ((Dev *)wl_resource_get_user_data(r))->serial;
    wl_resource_set_implementation(qr, &req_impl, q, req_gone);
}
static void dev_release(struct wl_client *cl, struct wl_resource *r) {
    (void)cl;
    wp_drm_lease_device_v1_send_released(r);
    wl_resource_destroy(r);
}
static const struct wp_drm_lease_device_v1_interface dev_impl = { .create_lease_request = dev_create_request, .release = dev_release };

static void bind_dev(struct wl_client *cl, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    Dev *d = calloc(1, sizeof *d);
    struct wl_resource *r = d ? wl_resource_create(cl, &wp_drm_lease_device_v1_interface, (int)ver, id) : NULL;
    if (!r) { free(d); wl_client_post_no_memory(cl); return; }
    d->res = r;
    d->serial = next_serial++;
    wl_list_init(&d->conns);
    wl_list_insert(&devs, &d->link);
    wl_resource_set_implementation(r, &dev_impl, d, dev_gone);
    /* hidden from such a client already (gate); bound anyway (a race with the filter): it gets nothing, its leases end */
    d->dead = !hooks.trusted || !hooks.trusted(cl);
    sync_dev(d);
}

static int gate(struct wl_client *c, const char *iface) {
    return strcmp(iface, wp_drm_lease_device_v1_interface.name) || (hooks.trusted && hooks.trusted(c));
}

/* the global exists while the card has a headset plugged in; removed first, destroyed a while later (clients that are
 * binding it meanwhile still can) */
static struct wl_global *dying;
static struct wl_event_source *dying_timer;
static int dying_done(void *data) {
    (void)data;
    if (dying) wl_global_destroy(dying);
    dying = NULL;
    return 0;
}
static void sync_global(void) {
    int want = card_fd >= 0 && drml_present(&D) > 0;
    if (want && !global) {
        global = wl_global_create(ldpy, &wp_drm_lease_device_v1_interface, 1, NULL, bind_dev);
        if (global) fprintf(stderr, "wayland: drm-lease: a non-desktop display (VR headset) is plugged in, offered for lease\n");
    } else if (!want && global) {
        Dev *d;
        wl_list_for_each(d, &devs, link) d->dead = 1;   /* old devices offer nothing again: a new global, new binds */
        sync_offers();
        wl_global_remove(global);
        if (dying) wl_global_destroy(dying);
        dying = global;
        global = NULL;
        if (!dying_timer) dying_timer = wl_event_loop_add_timer(wl_display_get_event_loop(ldpy), dying_done, NULL);
        if (dying_timer) wl_event_source_timer_update(dying_timer, 5000);
    }
}

void drmlease_init(struct wl_display *dpy, const DrmlHooks *h) {
    ldpy = dpy;
    hooks = *h;
    wl_list_init(&devs);
    drml_init(&D, &ops);
    secctx_set_gate(gate);
}

void drmlease_card(int fd) {
    if (!ldpy) return;
    if (fd != card_fd && card_fd >= 0) drml_card(&D, 0);   /* another card (or none): every lease of the old one ends */
    if (fd < 0) {
        card_fd = -1;
        kms_set_leased_crtcs(0);
        sync_global();
        return;
    }
    int was = card_fd;
    card_fd = fd;
    if (was != fd) drml_card(&D, 1);
    else drml_rescan(&D);
    sync_global();
    sync_offers();                      /* a device still waiting for drm_fd gets it now that lord-server is master */
}

int drmlease_answer(const char *app, const char *how) {
    if (!ldpy || !app || !how || !drml_waiting(&D, app)) return 0;
    int a = !strcmp(how, "always") ? DRML_ANSWER_ALWAYS : !strcmp(how, "once") ? DRML_ANSWER_ONCE : DRML_ANSWER_DENY;
    if (a == DRML_ANSWER_ALWAYS) perm_set(app, PERM_VR, PERM_ALLOW);
    else if (a == DRML_ANSWER_DENY) perm_set(app, PERM_VR, PERM_DENY);
    return drml_answer(&D, app, a);
}

void drmlease_perm_changed(void) {
    if (ldpy) drml_perm_changed(&D);
}
