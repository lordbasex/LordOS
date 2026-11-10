/* sysbus.c — system bus connection for Settings (see sysbus.h). */
#include "sysbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PENDING 64
#define MAX_HANDLERS 8                  /* bt, ppd, timedate, sleeplock and logindlock today */
#define MAX_WATCH 8                     /* BlueZ, two power-profiles names, polkit and logind (twice) today */

typedef struct { uint32_t serial; SbReply cb; void *ctx; } Pending;
typedef struct { char name[64]; SbOwner fn; } Watch;

static DConn conn = { .fd = -1 };
static int up;
static Pending pending[MAX_PENDING];
static SbSignal signals[MAX_HANDLERS];
static SbCall calls[MAX_HANDLERS];
static Watch watches[MAX_WATCH];
static int nsig, ncall, nwatch;

int sysbus_up(void) { return up; }
int sysbus_fd(void) { return up ? conn.fd : -1; }
int sysbus_wants_write(void) { return up && dbus_pending_out(&conn); }
DConn *sysbus_conn(void) { return &conn; }

int16_t sb_i16(DRd *r) {
    r->pos = (r->pos + 1) & ~(size_t)1;
    if (r->err || r->pos + 2 > r->len) { r->err = 1; return 0; }
    const uint8_t *p = r->b + r->pos;
    r->pos += 2;
    return (int16_t)(r->be ? (p[0] << 8 | p[1]) : (p[1] << 8 | p[0]));
}
uint16_t sb_u16(DRd *r) { return (uint16_t)sb_i16(r); }

uint32_t sysbus_call(const char *dest, const char *path, const char *iface, const char *member,
                     const char *sig, const DBuf *body, SbReply cb, void *ctx) {
    if (!up) return 0;
    int slot = -1;
    if (cb) {
        for (int i = 0; i < MAX_PENDING && slot < 0; i++) if (!pending[i].serial) slot = i;
        if (slot < 0) return 0;
    }
    DHdr h = { .type = DBUS_CALL, .flags = cb ? 0 : DBUS_NO_REPLY, .path = path, .iface = iface, .member = member,
               .dest = dest, .sig = sig && *sig ? sig : NULL };
    uint32_t s = dbus_send(&conn, &h, body);
    if (s && cb) pending[slot] = (Pending){ s, cb, ctx };
    return s;
}

void sysbus_add_match(const char *rule) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, rule);
    sysbus_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch", "s", &b, NULL, NULL);
    dw_free(&b);
}

void sysbus_on_signal(SbSignal fn) {
    if (nsig < MAX_HANDLERS) signals[nsig++] = fn;
    else fprintf(stderr, "sysbus: too many signal handlers (MAX_HANDLERS)\n");
}
void sysbus_on_call(SbCall fn) {
    if (ncall < MAX_HANDLERS) calls[ncall++] = fn;
    else fprintf(stderr, "sysbus: too many call handlers (MAX_HANDLERS)\n");
}

static void got_owner(const DMsg *m, void *ctx) {
    Watch *w = ctx;
    const char *owner = "";
    if (m && m->type == DBUS_RETURN && m->sig && !strcmp(m->sig, "s")) {
        DRd r;
        dr_init(&r, m);
        const char *o = dr_str(&r, NULL);
        if (!r.err && o) owner = o;
    }
    w->fn(w->name, owner);
}

static void ask_owner(Watch *w) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, w->name);
    sysbus_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner", "s", &b, got_owner, w);
    dw_free(&b);
}

void sysbus_watch_name(const char *name, SbOwner fn) {
    if (nwatch >= MAX_WATCH || strlen(name) >= sizeof watches[0].name) {
        fprintf(stderr, "sysbus: cannot watch %s (MAX_WATCH)\n", name);
        return;
    }
    Watch *w = &watches[nwatch++];
    snprintf(w->name, sizeof w->name, "%s", name);
    w->fn = fn;
    char rule[160];
    snprintf(rule, sizeof rule, "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0='%s'", name);
    sysbus_add_match(rule);
    ask_owner(w);
}

int sysbus_start(void) {
    if (up) return 0;
    int fd = dbus_connect_system();
    /* fds: logind's Inhibit answers with one (hw/sleeplock.c); only replies may bring them (handle) */
    if (fd < 0 || dbus_open_fds(&conn, fd, 2000)) {
        if (fd >= 0) dbus_close(&conn);
        fprintf(stderr, "sysbus: no system bus (Bluetooth and power profiles unavailable)\n");
        return -1;
    }
    up = 1;
    return 0;
}

static void lost(void) {
    up = 0;
    dbus_close(&conn);
    for (int i = 0; i < MAX_PENDING; i++)
        if (pending[i].serial) { Pending p = pending[i]; pending[i].serial = 0; p.cb(NULL, p.ctx); }
    for (int i = 0; i < nwatch; i++) watches[i].fn(watches[i].name, "");
    fprintf(stderr, "sysbus: the system bus connection was lost\n");
}

static void handle(DMsg *m) {
    if (m->type != DBUS_RETURN) dbus_msg_drop_fds(m);  /* only a reply to our own call may bring fds */
    if (m->type == DBUS_RETURN || m->type == DBUS_ERROR) {
        for (int i = 0; i < MAX_PENDING; i++)
            if (pending[i].serial && pending[i].serial == m->reply_serial) {
                Pending p = pending[i];
                pending[i].serial = 0;
                p.cb(m, p.ctx);
                return;
            }
        return;
    }
    if (m->type == DBUS_SIGNAL) {
        if (m->member && !strcmp(m->member, "NameOwnerChanged") && m->sender && !strcmp(m->sender, "org.freedesktop.DBus") &&
            m->sig && !strcmp(m->sig, "sss")) {
            DRd r;
            dr_init(&r, m);
            const char *name = dr_str(&r, NULL), *old = dr_str(&r, NULL), *now = dr_str(&r, NULL);
            (void)old;
            for (int i = 0; !r.err && i < nwatch; i++) if (!strcmp(watches[i].name, name)) watches[i].fn(name, now);
            return;
        }
        for (int i = 0; i < nsig; i++) signals[i](m);
        return;
    }
    if (m->type == DBUS_CALL) {
        for (int i = 0; i < ncall; i++) if (calls[i](m)) return;
        if (!(m->flags & DBUS_NO_REPLY)) dbus_error(&conn, m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
    }
}

void sysbus_dispatch(void) {
    if (!up) return;
    DMsg m;
    int r;
    while ((r = dbus_next(&conn, &m)) == 1) { handle(&m); dbus_msg_free(&m); }
    if (r < 0 || dbus_flush(&conn) < 0) lost();
}
