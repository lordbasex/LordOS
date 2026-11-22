/* drmlease_e2e_test.c — drm-lease-v1 end to end (src/wl/drmlease.c with drmlcore.c and secctx.c's global filter): a
 * server and two clients in this process over socketpairs, the card's kernel part faked (drmlease_test_drm). Checks
 * what a client sees and gets: the global only with a headset plugged in and only for a client that may lease, drm_fd
 * then the headset (never the desktop display) and done, the prompt before any lease, lease_fd after "allow once",
 * the connector withdrawn while leased and offered again after, the protocol errors, and "finished" when the session
 * leaves the monitor (docs/WAYLAND.md, "DRM leases"). */
#include "wl/drmlease.h"
#include "wl/drmlcore.h"
#include "wl/secctx.h"
#include "kms.h"
#include "hw/edid.h"
#include "sec/perms.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-client.h>
#include "client/drm-lease-v1-client-protocol.h"

void drmlease_test_drm(int (*scan)(void *, DrmlConn *, int), int (*lease)(void *, const uint32_t *, int, uint32_t, uint32_t *, uint32_t *),
                       int (*revoke)(void *, uint32_t), int (*perm)(void *, const char *), int (*open)(void));

static int fails;
static void check(const char *what, long got, long want) {
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

/* ---- what drmlease.c links that this test doesn't exercise (the real DRM ops are replaced) ---- */
int kms_outputs(int fd, KmsOutput *out, int max) { (void)fd; (void)out; (void)max; return 0; }
void kms_outputs_free(KmsOutput *o, int n) { (void)o; (void)n; }
const char *kms_connector_type_name(uint32_t t) { (void)t; return "?"; }
int kms_lease_objects(int fd, const KmsOutput *o, uint32_t avoid, uint32_t objs[3], uint32_t *bit) { (void)fd; (void)o; (void)avoid; (void)objs; (void)bit; return -1; }
static uint32_t reserved;
void kms_set_leased_crtcs(uint32_t mask) { reserved = mask; }
int edid_parse(const uint8_t *d, size_t n, Edid *e) { (void)d; (void)n; (void)e; return -1; }
void edid_display_name(const Edid *e, char *out, size_t n) { (void)e; if (n) out[0] = 0; }
static int saved = PERM_ASK;
PermState perm_get(const char *app, Perm p) { (void)app; (void)p; return (PermState)saved; }
int perm_set(const char *app, Perm p, PermState s) { (void)app; check("only the vr permission is saved", p, PERM_VR); saved = (int)s; return 0; }

/* ---- the fake card ---- */
static int plugged = 1, leases_alive, revoked, last_lessee;
static int f_scan(void *u, DrmlConn *out, int max) {
    (void)u;
    int n = 0;
    if (max < 2) return 0;
    out[n++] = (DrmlConn){ .id = 10, .connected = 1, .non_desktop = 0, .name = "DP-1", .desc = "DELL U2720Q" };
    if (plugged) out[n++] = (DrmlConn){ .id = 12, .connected = 1, .non_desktop = 1, .name = "DP-2", .desc = "Valve Index" };
    return n;
}
static int f_lease(void *u, const uint32_t *c, int n, uint32_t avoid, uint32_t *lessee, uint32_t *crtcs) {
    (void)u; (void)avoid;
    check("the lease is of the headset alone", n == 1 && c[0] == 12, 1);
    *lessee = (uint32_t)++last_lessee;
    *crtcs = 0x2;
    leases_alive++;
    return memfd_create("lease", MFD_CLOEXEC);
}
static int f_revoke(void *u, uint32_t lessee) { (void)u; (void)lessee; leases_alive--; revoked++; return 0; }
static int f_perm(void *u, const char *app) { (void)u; (void)app; return saved; }
static int f_open(void) { return memfd_create("card", MFD_CLOEXEC); }

/* ---- the server's view of its clients ---- */
static struct wl_display *sd;
static struct wl_client *trusted_client;
static int asks;
static char asked_app[192], asked_headset[160];
static int h_trusted(struct wl_client *c) { return c == trusted_client; }
static const char *h_identity(struct wl_client *c) { return c == trusted_client ? "program:/usr/bin/monado-service" : ""; }
static void h_ask(const char *app, const char *headset) {
    asks++;
    snprintf(asked_app, sizeof asked_app, "%s", app);
    snprintf(asked_headset, sizeof asked_headset, "%s", headset);
}

/* ---- a client ---- */
typedef struct {
    struct wl_display *d;
    struct wl_registry *reg;
    struct wp_drm_lease_device_v1 *dev;
    uint32_t dev_name;
    int seen_global, removed_global, drm_fd, dones, connectors, withdrawn, lease_fd, finished;
    struct wp_drm_lease_connector_v1 *conn;
    uint32_t conn_id;
    char conn_name[32], conn_desc[96];
} Cl;

static void c_drm_fd(void *data, struct wp_drm_lease_device_v1 *d, int32_t fd) { (void)d; Cl *c = data; c->drm_fd = fd; }
static void k_name(void *data, struct wp_drm_lease_connector_v1 *k, const char *n) { (void)k; Cl *c = data; snprintf(c->conn_name, sizeof c->conn_name, "%s", n); }
static void k_desc(void *data, struct wp_drm_lease_connector_v1 *k, const char *n) { (void)k; Cl *c = data; snprintf(c->conn_desc, sizeof c->conn_desc, "%s", n); }
static void k_id(void *data, struct wp_drm_lease_connector_v1 *k, uint32_t id) { (void)k; Cl *c = data; c->conn_id = id; }
static void k_done(void *data, struct wp_drm_lease_connector_v1 *k) { (void)data; (void)k; }
static void k_withdrawn(void *data, struct wp_drm_lease_connector_v1 *k) {
    Cl *c = data;
    c->withdrawn++;
    if (k == c->conn) c->conn = NULL;
    wp_drm_lease_connector_v1_destroy(k);
}
static const struct wp_drm_lease_connector_v1_listener k_l = { k_name, k_desc, k_id, k_done, k_withdrawn };
static void c_connector(void *data, struct wp_drm_lease_device_v1 *d, struct wp_drm_lease_connector_v1 *k) {
    (void)d;
    Cl *c = data;
    c->connectors++;
    c->conn = k;
    wp_drm_lease_connector_v1_add_listener(k, &k_l, c);
}
static void c_done(void *data, struct wp_drm_lease_device_v1 *d) { (void)d; ((Cl *)data)->dones++; }
static void c_released(void *data, struct wp_drm_lease_device_v1 *d) { (void)data; (void)d; }
static const struct wp_drm_lease_device_v1_listener dev_l = { c_drm_fd, c_connector, c_done, c_released };

static void l_fd(void *data, struct wp_drm_lease_v1 *l, int32_t fd) { (void)l; Cl *c = data; c->lease_fd = fd; }
static void l_finished(void *data, struct wp_drm_lease_v1 *l) { (void)l; ((Cl *)data)->finished++; }
static const struct wp_drm_lease_v1_listener lease_l = { l_fd, l_finished };

static void r_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    Cl *c = data;
    (void)ver;
    if (strcmp(iface, wp_drm_lease_device_v1_interface.name)) return;
    c->seen_global++;
    c->dev_name = name;
    c->dev = wl_registry_bind(r, name, &wp_drm_lease_device_v1_interface, 1);
    wp_drm_lease_device_v1_add_listener(c->dev, &dev_l, c);
}
static void r_remove(void *data, struct wl_registry *r, uint32_t name) {
    (void)r;
    Cl *c = data;
    if (name == c->dev_name) c->removed_global++;
}
static const struct wl_registry_listener reg_l = { r_global, r_remove };

/* both sides run until nothing moves (one thread: never a blocking call) */
static void pump(Cl **cs, int n) {
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < n; i++) wl_display_flush(cs[i]->d);
        wl_event_loop_dispatch(wl_display_get_event_loop(sd), 0);
        wl_display_flush_clients(sd);
        for (int i = 0; i < n; i++) {
            if (wl_display_get_error(cs[i]->d)) continue;
            while (wl_display_prepare_read(cs[i]->d) != 0) wl_display_dispatch_pending(cs[i]->d);
            wl_display_read_events(cs[i]->d);
            wl_display_dispatch_pending(cs[i]->d);
        }
    }
}

static Cl *connect_client(struct wl_client **server_side) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) return NULL;
    *server_side = wl_client_create(sd, sv[0]);
    Cl *c = calloc(1, sizeof *c);
    c->drm_fd = c->lease_fd = -1;
    c->d = wl_display_connect_to_fd(sv[1]);
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);
    c->reg = wl_display_get_registry(c->d);
    wl_registry_add_listener(c->reg, &reg_l, c);
    return c;
}

int main(void) {
    sd = wl_display_create();
    secctx_init(sd, NULL);                  /* the display's global filter, which drmlease.c's gate joins */
    drmlease_test_drm(f_scan, f_lease, f_revoke, f_perm, f_open);
    drmlease_init(sd, &(DrmlHooks){ h_trusted, h_identity, h_ask });

    struct wl_client *sa, *sb;
    Cl *a = connect_client(&sa), *b = connect_client(&sb);
    trusted_client = sa;
    Cl *both[2] = { a, b };

    /* no card yet: no global */
    pump(both, 2);
    check("no card: no lease device", a->seen_global, 0);

    /* the card with a headset plugged in */
    drmlease_card(7);
    pump(both, 2);
    check("a trusted client sees the lease device", a->seen_global, 1);
    check("a sandboxed (untrusted) client never does", b->seen_global, 0);
    check("drm_fd first", a->drm_fd >= 0, 1);
    check("one connector offered: the headset, not the desktop display", a->connectors, 1);
    check("... its DRM id", a->conn_id, 12);
    check("... its name", !strcmp(a->conn_name, "DP-2"), 1);
    check("... its description", !strcmp(a->conn_desc, "Valve Index"), 1);
    check("done after the connectors", a->dones, 1);

    /* a lease request: the person is asked first */
    struct wp_drm_lease_request_v1 *rq = wp_drm_lease_device_v1_create_lease_request(a->dev);
    wp_drm_lease_request_v1_request_connector(rq, a->conn);
    struct wp_drm_lease_v1 *lease = wp_drm_lease_request_v1_submit(rq);
    wp_drm_lease_v1_add_listener(lease, &lease_l, a);
    pump(both, 2);
    check("the prompt is shown", asks, 1);
    check("... for the identity lord-server checked", !strcmp(asked_app, "program:/usr/bin/monado-service"), 1);
    check("... naming the headset and its connector", !strcmp(asked_headset, "Valve Index (DP-2)"), 1);
    check("no lease before the answer", a->lease_fd < 0 && leases_alive == 0, 1);
    check("an answer for another app is ignored", drmlease_answer("program:/usr/bin/other", "always"), 0);
    check("allow once", drmlease_answer("program:/usr/bin/monado-service", "once"), 1);
    pump(both, 2);
    check("lease_fd received", a->lease_fd >= 0, 1);
    check("allow once saves nothing", saved, PERM_ASK);
    check("the leased CRTC is reserved from LordOS's heads", reserved, 0x2);
    check("the connector is withdrawn while leased", a->withdrawn, 1);

    /* the client ends it: revoked, offered again */
    wp_drm_lease_v1_destroy(lease);
    pump(both, 2);
    check("destroyed by the client: revoked", revoked == 1 && leases_alive == 0, 1);
    check("... nothing reserved", reserved, 0);
    check("... the headset offered again", a->connectors, 2);

    /* "always": saved, the next request leases without a prompt */
    rq = wp_drm_lease_device_v1_create_lease_request(a->dev);
    wp_drm_lease_request_v1_request_connector(rq, a->conn);
    lease = wp_drm_lease_request_v1_submit(rq);
    wp_drm_lease_v1_add_listener(lease, &lease_l, a);
    pump(both, 2);
    check("asked again (allow once wasn't saved)", asks, 2);
    drmlease_answer("program:/usr/bin/monado-service", "always");
    check("always allow is saved", saved, PERM_ALLOW);
    pump(both, 2);
    check("leased", leases_alive, 1);

    /* the session leaves the monitor (a lock, a user switch, a text console) */
    a->finished = 0;
    drmlease_card(-1);
    pump(both, 2);
    check("leaving the monitor: revoked", leases_alive, 0);
    check("... finished sent", a->finished, 1);
    check("... the global removed", a->removed_global, 1);
    wp_drm_lease_v1_destroy(lease);

    /* back, then the headset unplugged */
    drmlease_card(7);
    pump(both, 2);
    check("back on the monitor: a new global", a->seen_global, 2);
    Cl *a2 = a;
    rq = wp_drm_lease_device_v1_create_lease_request(a2->dev);
    wp_drm_lease_request_v1_request_connector(rq, a2->conn);
    lease = wp_drm_lease_request_v1_submit(rq);
    wp_drm_lease_v1_add_listener(lease, &lease_l, a2);
    pump(both, 2);
    check("always allowed: leased without a prompt", leases_alive == 1 && asks == 2, 1);
    a2->finished = 0;
    plugged = 0;
    drmlease_card(7);                       /* the hotplug */
    pump(both, 2);
    check("unplugged: revoked and finished", leases_alive == 0 && a2->finished == 1, 1);
    check("... the global removed", a2->removed_global, 2);

    /* an empty request is a protocol error */
    plugged = 1;
    drmlease_card(7);
    pump(both, 2);
    rq = wp_drm_lease_device_v1_create_lease_request(a2->dev);
    lease = wp_drm_lease_request_v1_submit(rq);
    pump(both, 2);
    check("submit without a connector: protocol error empty_lease",
          wl_display_get_error(a2->d) == EPROTO && wl_display_get_protocol_error(a2->d, NULL, NULL) == WP_DRM_LEASE_REQUEST_V1_ERROR_EMPTY_LEASE, 1);

    wl_display_destroy_clients(sd);
    wl_display_destroy(sd);
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
