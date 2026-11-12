/* lid.c — the laptop's lid, from logind (see lid.h). */
#include "lid.h"
#include "display.h"
#include "sysbus.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LOGIN1      "org.freedesktop.login1"
#define LOGIN1_PATH "/org/freedesktop/login1"
#define LOGIN1_MGR  "org.freedesktop.login1.Manager"
#define PROPS       "org.freedesktop.DBus.Properties"

static char owner[128];                 /* logind's unique name; "" while it has none */
static unsigned asked;                  /* the Get being answered; a newer one replaces it */

static void got_lid(const DMsg *m, void *ctx) {
    if ((unsigned)(uintptr_t)ctx != asked || !owner[0]) return;    /* stale: logind went away or was asked again */
    if (!m || m->type != DBUS_RETURN || !m->sig || strcmp(m->sig, "v")) return;
    DRd r;
    dr_init(&r, m);
    const char *sig = dr_variant(&r);
    if (r.err || !sig || strcmp(sig, "b")) return;
    int closed = dr_bool(&r);
    if (!r.err) display_set_lid(closed);
}

static void ask(void) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, LOGIN1_MGR);
    dw_str(&b, "LidClosed");
    asked++;
    sysbus_call(LOGIN1, LOGIN1_PATH, PROPS, "Get", "ss", &b, got_lid, (void *)(uintptr_t)asked);
    dw_free(&b);
}

static void on_owner(const char *name, const char *o) {
    (void)name;
    snprintf(owner, sizeof owner, "%s", o ? o : "");
    if (owner[0]) ask();
    else { asked++; display_set_lid(0); }   /* no logind: nothing says the lid is closed */
}

/* PropertiesChanged(s interface, a{sv} changed, as invalidated) */
static void on_signal(const DMsg *m) {
    if (m->type != DBUS_SIGNAL || !m->member || strcmp(m->member, "PropertiesChanged") || !m->iface ||
        strcmp(m->iface, PROPS) || !m->path || strcmp(m->path, LOGIN1_PATH) || !m->sig || strcmp(m->sig, "sa{sv}as") ||
        !m->sender || !owner[0] || strcmp(m->sender, owner))
        return;                         /* only logind itself (the bus sets the sender) */
    DRd r;
    dr_init(&r, m);
    const char *iface = dr_str(&r, NULL);
    if (r.err || !iface || strcmp(iface, LOGIN1_MGR)) return;
    size_t end = dr_arr(&r, 8);
    while (!r.err && dr_more(&r, end)) {
        dr_struct(&r);
        const char *k = dr_str(&r, NULL), *sig = dr_variant(&r);
        if (r.err || !k || !sig) return;
        if (!strcmp(k, "LidClosed") && !strcmp(sig, "b")) {
            int closed = dr_bool(&r);
            if (r.err) return;
            asked++;                    /* newer than any Get still on its way */
            display_set_lid(closed);
        } else dr_skip(&r, sig);
    }
    if (r.err) return;
    end = dr_arr(&r, 4);
    while (!r.err && dr_more(&r, end)) {
        const char *k = dr_str(&r, NULL);
        if (!r.err && k && !strcmp(k, "LidClosed")) { ask(); return; }   /* changed without its value */
    }
}

void lid_start(void) {
    if (!sysbus_up()) return;
    sysbus_on_signal(on_signal);
    sysbus_add_match("type='signal',sender='" LOGIN1 "',path='" LOGIN1_PATH "',interface='" PROPS "',member='PropertiesChanged',arg0='" LOGIN1_MGR "'");
    sysbus_watch_name(LOGIN1, on_owner);
}
