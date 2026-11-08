/* bt.c — BlueZ client and pairing agent (see bt.h). */
#include "bt.h"
#include "jbuf.h"
#include "sysbus.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLUEZ "org.bluez"
#define AGENT_PATH "/org/lordos/BluetoothAgent"
#define AGENT_IFACE "org.bluez.Agent1"

static BtModel model;
static BtEvent emit;
static char owner[128];                 /* org.bluez's unique name, "" if not running */
static int dirty, agent_ok;

/* the agent request waiting for the user (one at a time) */
static struct {
    int id, active;
    DMsg call;                          /* kept to reply later (we own call.raw) */
    char kind[20], dev[128], value[20];
} prompt;
static int next_prompt = 1;
static void trust(BtDevice *d);           /* Trusted = true (see bt_should_trust) */

BtModel *bt_model(void) { return &model; }
int bt_available(void) { return owner[0] != 0; }

/* ---- model ---- */

int bt_path_valid(const char *p) {
    /* /org/bluez/hci<N>[/dev_XX_XX_XX_XX_XX_XX] */
    if (!p || strncmp(p, "/org/bluez/hci", 14) || !isdigit((unsigned char)p[14])) return 0;
    const char *s = p + 14;
    while (isdigit((unsigned char)*s)) s++;
    if (!*s) return 1;
    if (strncmp(s, "/dev_", 5)) return 0;
    s += 5;
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) return 0;
        s += 2;
        if (i < 5) { if (*s != '_') return 0; s++; }
    }
    return *s == 0;
}

static int is_device_path(const char *p) { return bt_path_valid(p) && strstr(p, "/dev_"); }

static BtAdapter *adapter(BtModel *m, const char *path, int create) {
    for (int i = 0; i < m->nadapters; i++) if (!strcmp(m->adapters[i].path, path)) return &m->adapters[i];
    if (!create || m->nadapters >= BT_MAX_ADAPTERS || strlen(path) >= sizeof m->adapters[0].path) return NULL;
    BtAdapter *a = &m->adapters[m->nadapters++];
    memset(a, 0, sizeof *a);
    snprintf(a->path, sizeof a->path, "%s", path);
    return a;
}

static BtDevice *device(BtModel *m, const char *path, int create) {
    for (int i = 0; i < m->ndevices; i++) if (!strcmp(m->devices[i].path, path)) return &m->devices[i];
    if (!create || m->ndevices >= BT_MAX_DEVICES || strlen(path) >= sizeof m->devices[0].path) return NULL;
    BtDevice *d = &m->devices[m->ndevices++];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    const char *slash = strstr(path, "/dev_");
    size_t al = slash ? (size_t)(slash - path) : 0;
    if (al < sizeof d->adapter) { memcpy(d->adapter, path, al); d->adapter[al] = 0; }
    d->battery = -1;
    return d;
}

static void copy_str(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }

/* One variant value: reads strings, booleans and integers; skips the rest. */
typedef struct { const char *s; long long i; int is_str, is_num; } Val;

static Val read_val(DRd *r, const char *sig) {
    Val v = { 0 };
    if (!sig || !sig[0] || sig[1]) { dr_skip(r, sig ? sig : "v"); return v; }
    switch (sig[0]) {
    case 's': case 'o': v.s = dr_str(r, NULL); v.is_str = 1; break;
    case 'b': v.i = dr_bool(r); v.is_num = 1; break;
    case 'y': v.i = dr_u8(r); v.is_num = 1; break;
    case 'n': v.i = sb_i16(r); v.is_num = 1; break;
    case 'q': v.i = sb_u16(r); v.is_num = 1; break;
    case 'u': v.i = dr_u32(r); v.is_num = 1; break;
    case 'i': v.i = dr_i32(r); v.is_num = 1; break;
    default: dr_skip(r, sig); break;
    }
    return v;
}

void bt_apply_props(BtModel *m, const char *path, const char *iface, DRd *r) {
    int is_adapter = !strcmp(iface, "org.bluez.Adapter1"), is_dev = !strcmp(iface, "org.bluez.Device1");
    int is_batt = !strcmp(iface, "org.bluez.Battery1");
    BtAdapter *a = is_adapter && bt_path_valid(path) && !is_device_path(path) ? adapter(m, path, 1) : NULL;
    BtDevice *d = (is_dev || is_batt) && is_device_path(path) ? device(m, path, 1) : NULL;
    size_t end = dr_arr(r, 8);
    while (!r->err && dr_more(r, end)) {
        dr_struct(r);
        const char *k = dr_str(r, NULL);
        const char *sig = dr_variant(r);
        if (r->err || !k) break;
        Val v = read_val(r, sig);
        if (r->err) break;
        if (a) {
            if (!strcmp(k, "Address") && v.is_str) copy_str(a->address, sizeof a->address, v.s);
            else if (!strcmp(k, "Alias") && v.is_str) copy_str(a->alias, sizeof a->alias, v.s);
            else if (!strcmp(k, "Powered") && v.is_num) a->powered = (int)v.i;
            else if (!strcmp(k, "Discoverable") && v.is_num) a->discoverable = (int)v.i;
            else if (!strcmp(k, "Discovering") && v.is_num) a->discovering = (int)v.i;
            else if (!strcmp(k, "Pairable") && v.is_num) a->pairable = (int)v.i;
        } else if (d && is_dev) {
            if (!strcmp(k, "Address") && v.is_str) copy_str(d->address, sizeof d->address, v.s);
            else if (!strcmp(k, "Name") && v.is_str) copy_str(d->name, sizeof d->name, v.s);
            else if (!strcmp(k, "Alias") && v.is_str) copy_str(d->alias, sizeof d->alias, v.s);
            else if (!strcmp(k, "Icon") && v.is_str) copy_str(d->icon, sizeof d->icon, v.s);
            else if (!strcmp(k, "Adapter") && v.is_str) copy_str(d->adapter, sizeof d->adapter, v.s);
            else if (!strcmp(k, "Class") && v.is_num) d->cls = (uint32_t)v.i;
            else if (!strcmp(k, "Appearance") && v.is_num) d->appearance = (uint16_t)v.i;
            else if (!strcmp(k, "Paired") && v.is_num) d->paired = (int)v.i;
            else if (!strcmp(k, "Bonded") && v.is_num) d->bonded = (int)v.i;
            else if (!strcmp(k, "Trusted") && v.is_num) d->trusted = (int)v.i;
            else if (!strcmp(k, "Connected") && v.is_num) d->connected = (int)v.i;
            else if (!strcmp(k, "Blocked") && v.is_num) d->blocked = (int)v.i;
            else if (!strcmp(k, "LegacyPairing") && v.is_num) d->legacy = (int)v.i;
            else if (!strcmp(k, "RSSI") && v.is_num) { d->rssi = (int)v.i; d->has_rssi = 1; }
        } else if (d && is_batt) {
            if (!strcmp(k, "Percentage") && v.is_num) d->battery = (int)v.i;
        }
    }
}

void bt_remove_object(BtModel *m, const char *path, const char *iface) {
    if (!strcmp(iface, "org.bluez.Device1")) {
        for (int i = 0; i < m->ndevices; i++)
            if (!strcmp(m->devices[i].path, path)) { m->devices[i] = m->devices[--m->ndevices]; break; }
    } else if (!strcmp(iface, "org.bluez.Battery1")) {
        BtDevice *d = device(m, path, 0);
        if (d) d->battery = -1;
    } else if (!strcmp(iface, "org.bluez.Adapter1")) {
        for (int i = 0; i < m->nadapters; i++)
            if (!strcmp(m->adapters[i].path, path)) { m->adapters[i] = m->adapters[--m->nadapters]; break; }
        size_t pl = strlen(path);
        for (int i = 0; i < m->ndevices;)
            if (!strncmp(m->devices[i].path, path, pl) && m->devices[i].path[pl] == '/') m->devices[i] = m->devices[--m->ndevices];
            else i++;
    }
}

/* a{sa{sv}} for one object */
static void parse_ifaces(BtModel *m, const char *path, DRd *r) {
    size_t end = dr_arr(r, 8);
    while (!r->err && dr_more(r, end)) {
        dr_struct(r);
        const char *iface = dr_str(r, NULL);
        if (r->err || !iface) break;
        bt_apply_props(m, path, iface, r);
    }
}

int bt_parse_managed(BtModel *m, const DMsg *msg) {
    if (!msg->sig || strcmp(msg->sig, "a{oa{sa{sv}}}")) return -1;
    DRd r;
    dr_init(&r, msg);
    size_t end = dr_arr(&r, 8);
    while (!r.err && dr_more(&r, end)) {
        dr_struct(&r);
        const char *path = dr_str(&r, NULL);
        if (r.err || !path) break;
        parse_ifaces(m, path, &r);
    }
    return r.err ? -1 : 0;
}

/* ---- events ---- */

static void dev_json(HBuf *b, const BtDevice *d) {
    hb_printf(b, "{\"path\":");
    hb_str(b, d->path);
    hb_printf(b, ",\"address\":");
    hb_str(b, d->address);
    hb_printf(b, ",\"name\":");
    hb_str(b, d->alias[0] ? d->alias : d->name[0] ? d->name : d->address);
    hb_printf(b, ",\"named\":%s,\"icon\":", d->name[0] || (d->alias[0] && strcmp(d->alias, d->address)) ? "true" : "false");
    hb_str(b, d->icon);
    hb_printf(b, ",\"class\":%u,\"appearance\":%u,\"paired\":%s,\"trusted\":%s,\"connected\":%s,\"blocked\":%s,\"busy\":%s,\"battery\":%d",
              d->cls, d->appearance, d->paired || d->bonded ? "true" : "false", d->trusted ? "true" : "false", d->connected ? "true" : "false",
              d->blocked ? "true" : "false", d->busy ? "true" : "false", d->battery);
    if (d->has_rssi) hb_printf(b, ",\"rssi\":%d", d->rssi);
    hb_printf(b, "}");
}

char *bt_state_json(void) {
    HBuf b = { 0 };
    const BtAdapter *a = model.nadapters ? &model.adapters[0] : NULL;
    hb_printf(&b, "{\"available\":%s,\"agent\":%s,\"adapter\":", owner[0] ? "true" : "false", agent_ok ? "true" : "false");
    if (a) {
        hb_printf(&b, "{\"path\":");
        hb_str(&b, a->path);
        hb_printf(&b, ",\"address\":");
        hb_str(&b, a->address);
        hb_printf(&b, ",\"name\":");
        hb_str(&b, a->alias);
        hb_printf(&b, ",\"powered\":%s,\"discoverable\":%s,\"discovering\":%s}", a->powered ? "true" : "false",
                  a->discoverable ? "true" : "false", a->discovering ? "true" : "false");
    } else hb_printf(&b, "null");
    hb_printf(&b, ",\"devices\":[");
    int first = 1;
    for (int i = 0; i < model.ndevices; i++) {
        const BtDevice *d = &model.devices[i];
        if (a && strcmp(d->adapter, a->path)) continue;     /* the first adapter's devices */
        if (!first) hb_printf(&b, ",");
        first = 0;
        dev_json(&b, d);
    }
    hb_printf(&b, "],\"prompt\":");
    if (prompt.active) {
        const BtDevice *d = device(&model, prompt.dev, 0);
        hb_printf(&b, "{\"id\":%d,\"kind\":", prompt.id);
        hb_str(&b, prompt.kind);
        hb_printf(&b, ",\"device\":");
        hb_str(&b, prompt.dev);
        hb_printf(&b, ",\"name\":");
        hb_str(&b, d ? (d->alias[0] ? d->alias : d->name[0] ? d->name : d->address) : "");
        hb_printf(&b, ",\"value\":");
        hb_str(&b, prompt.value);
        hb_printf(&b, "}");
    } else hb_printf(&b, "null");
    hb_printf(&b, "}");
    return hb_take(&b);
}

void bt_flush_events(void) {
    if (!dirty || !emit) return;
    dirty = 0;
    char *j = bt_state_json();
    if (j) { emit("bt", j); free(j); }
}

typedef struct { char op[16], path[128]; } Op;

static void result(const char *op, const char *path, int ok, const char *err) {
    if (!emit) return;
    HBuf b = { 0 };
    hb_printf(&b, "{\"op\":");
    hb_str(&b, op);
    hb_printf(&b, ",\"path\":");
    hb_str(&b, path);
    hb_printf(&b, ",\"ok\":%s,\"error\":", ok ? "true" : "false");
    hb_str(&b, err ? err : "");
    hb_printf(&b, "}");
    char *j = hb_take(&b);
    if (j) { emit("bt-result", j); free(j); }
}

static Op *op_new(const char *op, const char *path) {
    Op *o = calloc(1, sizeof *o);
    if (o) { snprintf(o->op, sizeof o->op, "%s", op); snprintf(o->path, sizeof o->path, "%s", path ? path : ""); }
    return o;
}

static const char *err_name(const DMsg *m) { return !m ? "org.freedesktop.DBus.Error.Disconnected" : m->type == DBUS_ERROR ? m->error : NULL; }

/* ---- signals ---- */

static void on_signal(const DMsg *m) {
    if (!owner[0] || !m->sender || strcmp(m->sender, owner) || !m->member || !m->sig) return;
    DRd r;
    dr_init(&r, m);
    if (!strcmp(m->member, "InterfacesAdded") && !strcmp(m->sig, "oa{sa{sv}}")) {
        const char *path = dr_str(&r, NULL);
        if (!r.err && path) parse_ifaces(&model, path, &r);
        dirty = 1;
    } else if (!strcmp(m->member, "InterfacesRemoved") && !strcmp(m->sig, "oas")) {
        const char *path = dr_str(&r, NULL);
        size_t end = dr_arr(&r, 4);
        while (!r.err && path && dr_more(&r, end)) {
            const char *iface = dr_str(&r, NULL);
            if (!r.err && iface) bt_remove_object(&model, path, iface);
        }
        dirty = 1;
    } else if (!strcmp(m->member, "PropertiesChanged") && !strcmp(m->sig, "sa{sv}as") && m->path) {
        const char *iface = dr_str(&r, NULL);
        if (!r.err && iface && (!strcmp(iface, "org.bluez.Adapter1") || !strcmp(iface, "org.bluez.Device1") || !strcmp(iface, "org.bluez.Battery1"))) {
            /* a device's properties change before it is in the model only if we missed it */
            if (strcmp(iface, "org.bluez.Adapter1") || adapter(&model, m->path, 0)) bt_apply_props(&model, m->path, iface, &r);
            dirty = 1;
        }
    }
}

/* ---- agent ---- */

static void prompt_drop(void) {
    if (prompt.active) dbus_msg_free(&prompt.call);
    prompt.active = 0;
    dirty = 1;
}

static void agent_reject(const DMsg *m, const char *why) { dbus_error(sysbus_conn(), m, "org.bluez.Error.Rejected", why); }

static int on_call(DMsg *m) {
    if (!m->path || strcmp(m->path, AGENT_PATH)) return 0;
    if (!owner[0] || !m->sender || strcmp(m->sender, owner)) {    /* only bluetoothd talks to the agent */
        dbus_error(sysbus_conn(), m, "org.freedesktop.DBus.Error.AccessDenied", "not bluetoothd");
        return 1;
    }
    if (!m->iface || strcmp(m->iface, AGENT_IFACE) || !m->member) {
        dbus_error(sysbus_conn(), m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
        return 1;
    }
    const char *mem = m->member, *sig = m->sig ? m->sig : "";
    DRd r;
    dr_init(&r, m);
    if (!strcmp(mem, "Release")) { dbus_reply(sysbus_conn(), m, NULL, NULL); agent_ok = 0; dirty = 1; return 1; }
    if (!strcmp(mem, "Cancel")) { prompt_drop(); dbus_reply(sysbus_conn(), m, NULL, NULL); return 1; }
    const char *dev = sig[0] == 'o' ? dr_str(&r, NULL) : NULL;
    if (r.err || !dev || !is_device_path(dev)) { dbus_error(sysbus_conn(), m, "org.freedesktop.DBus.Error.InvalidArgs", "device"); return 1; }
    const char *kind = NULL;
    char value[20] = "";
    if (!strcmp(mem, "RequestPinCode") && !strcmp(sig, "o")) kind = "pin";
    else if (!strcmp(mem, "RequestPasskey") && !strcmp(sig, "o")) kind = "passkey";
    else if (!strcmp(mem, "RequestConfirmation") && !strcmp(sig, "ou")) { kind = "confirm"; snprintf(value, sizeof value, "%06u", dr_u32(&r)); }
    else if (!strcmp(mem, "RequestAuthorization") && !strcmp(sig, "o")) kind = "authorize";
    else if (!strcmp(mem, "AuthorizeService") && !strcmp(sig, "os")) {
        BtDevice *d = device(&model, dev, 0);
        /* a device that went through pairing (the user confirmed it) may use its services; trusted from
         * now on, so it also gets in when no session is running (bt_should_trust) */
        if (d && (d->paired || d->bonded)) {
            dbus_reply(sysbus_conn(), m, NULL, NULL);
            if (bt_should_trust("authorize", NULL, d)) trust(d);
            return 1;
        }
        kind = "service";
    } else if (!strcmp(mem, "DisplayPinCode") && !strcmp(sig, "os")) {
        const char *pin = dr_str(&r, NULL);
        kind = "display-pin";
        snprintf(value, sizeof value, "%s", pin && strlen(pin) <= 16 ? pin : "");
    } else if (!strcmp(mem, "DisplayPasskey") && !strcmp(sig, "ouq")) {
        kind = "display-passkey";
        snprintf(value, sizeof value, "%06u", dr_u32(&r));
    } else { dbus_error(sysbus_conn(), m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method"); return 1; }
    if (r.err) { dbus_error(sysbus_conn(), m, "org.freedesktop.DBus.Error.InvalidArgs", "args"); return 1; }
    if (prompt.active) { dbus_error(sysbus_conn(), &prompt.call, "org.bluez.Error.Canceled", "replaced"); prompt_drop(); }
    int display_only = !strncmp(kind, "display-", 8);
    if (display_only) dbus_reply(sysbus_conn(), m, NULL, NULL);       /* informational: answered now */
    prompt.id = next_prompt++;
    prompt.active = 1;
    snprintf(prompt.kind, sizeof prompt.kind, "%s", kind);
    snprintf(prompt.dev, sizeof prompt.dev, "%s", dev);
    snprintf(prompt.value, sizeof prompt.value, "%s", value);
    prompt.call = *m;
    m->raw = NULL;                      /* ours now: freed by prompt_drop */
    if (display_only) prompt.call.flags |= DBUS_NO_REPLY;
    dirty = 1;
    bt_flush_events();
    char *j = bt_state_json();
    if (j && emit) emit("bt-prompt", j);
    free(j);
    return 1;
}

int bt_answer(int id, int accept, const char *value) {
    if (!prompt.active || id != prompt.id) return -1;
    DMsg *m = &prompt.call;
    DConn *c = sysbus_conn();
    const char *k = prompt.kind;
    if (!strncmp(k, "display-", 8)) { prompt_drop(); return 0; }
    if (!accept) { agent_reject(m, "rejected by the user"); prompt_drop(); return 0; }
    if (!strcmp(k, "pin")) {
        size_t n = value ? strlen(value) : 0;
        int ok = n >= 1 && n <= 16;
        for (size_t i = 0; ok && i < n; i++) if (!isalnum((unsigned char)value[i])) ok = 0;
        if (!ok) return -1;
        DBuf b;
        dw_init(&b);
        dw_str(&b, value);
        dbus_reply(c, m, "s", &b);
        dw_free(&b);
    } else if (!strcmp(k, "passkey")) {
        char *e = NULL;
        unsigned long v = value ? strtoul(value, &e, 10) : 0;
        if (!value || !*value || *e || v > 999999) return -1;
        DBuf b;
        dw_init(&b);
        dw_u32(&b, (uint32_t)v);
        dbus_reply(c, m, "u", &b);
        dw_free(&b);
    } else dbus_reply(c, m, NULL, NULL);
    prompt_drop();
    return 0;
}

/* ---- startup and owner changes ---- */

static void got_managed(const DMsg *m, void *ctx) {
    (void)ctx;
    if (!m || m->type != DBUS_RETURN) return;
    BtModel fresh;
    memset(&fresh, 0, sizeof fresh);
    if (bt_parse_managed(&fresh, m) == 0) {
        /* keep our "busy" flags */
        for (int i = 0; i < fresh.ndevices; i++) {
            BtDevice *old = device(&model, fresh.devices[i].path, 0);
            if (old) fresh.devices[i].busy = old->busy;
        }
        model = fresh;
    }
    dirty = 1;
}

static void got_default(const DMsg *m, void *ctx) {
    (void)ctx;
    agent_ok = m && m->type == DBUS_RETURN;
    if (!agent_ok) fprintf(stderr, "bluetooth: could not become the default agent (%s)\n", err_name(m) ? err_name(m) : "?");
    dirty = 1;
}

static void got_register(const DMsg *m, void *ctx) {
    (void)ctx;
    if (!m || m->type != DBUS_RETURN) {
        fprintf(stderr, "bluetooth: RegisterAgent failed (%s)\n", err_name(m) ? err_name(m) : "?");
        return;
    }
    DBuf b;
    dw_init(&b);
    dw_str(&b, AGENT_PATH);
    sysbus_call(BLUEZ, "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent", "o", &b, got_default, NULL);
    dw_free(&b);
}

static void on_owner(const char *name, const char *now) {
    (void)name;
    snprintf(owner, sizeof owner, "%s", now ? now : "");
    if (!owner[0]) {
        memset(&model, 0, sizeof model);
        if (prompt.active) prompt_drop();
        agent_ok = 0;
        dirty = 1;
        return;
    }
    sysbus_call(BLUEZ, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL, NULL, got_managed, NULL);
    DBuf b;
    dw_init(&b);
    dw_str(&b, AGENT_PATH);
    dw_str(&b, "KeyboardDisplay");
    sysbus_call(BLUEZ, "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent", "os", &b, got_register, NULL);
    dw_free(&b);
    dirty = 1;
}

void bt_start(BtEvent fn) {
    emit = fn;
    if (!sysbus_up()) return;
    sysbus_on_signal(on_signal);
    sysbus_on_call(on_call);
    sysbus_add_match("type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager'");
    sysbus_add_match("type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',path_namespace='/org/bluez'");
    sysbus_watch_name(BLUEZ, on_owner);
}

/* ---- actions ---- */

/* A paired device is trusted in BlueZ (Device1.Trusted) so bluetoothd accepts it when it comes back by
 * itself, with no agent asked: at boot, a mouse or keyboard reconnects at the login screen, where no
 * session (and so no agent) is running yet. Without it bluetoothd rejects the device ("Authentication
 * attempt without agent") until someone logs in. Pairing from Settings trusts the device; these cover the
 * devices that ended up paired but not trusted (a Pair call that failed after the bond was made, or that
 * reported AlreadyExists; a device paired with other tools or before): the user connecting one, or our
 * agent letting one use a service (which it does for any paired device), trusts it too. */
int bt_should_trust(const char *op, const char *err, const BtDevice *d) {
    if (!op || !d || d->trusted) return 0;
    if (!strcmp(op, "authorize")) return d->paired || d->bonded;      /* AuthorizeService, already allowed */
    if (!strcmp(op, "pair")) return !err || !strncmp(err, "org.bluez.Error.AlreadyExists", 29);
    if (!strcmp(op, "connect")) return !err && (d->paired || d->bonded);
    return 0;
}

static void trust(BtDevice *d) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, "org.bluez.Device1");
    dw_str(&b, "Trusted");
    dw_variant(&b, "b");
    dw_bool(&b, 1);
    if (sysbus_call(BLUEZ, d->path, "org.freedesktop.DBus.Properties", "Set", "ssv", &b, NULL, NULL))
        fprintf(stderr, "bt: trusting %s so it reconnects by itself\n", d->path);
    dw_free(&b);
}

static void op_done(const DMsg *m, void *ctx) {
    Op *o = ctx;
    const char *err = err_name(m);
    char full[256];                     /* "org.bluez.Error.Failed: br-connection-profile-unavailable" */
    if (err && m && m->sig && !strcmp(m->sig, "s")) {
        DRd r;
        dr_init(&r, m);
        const char *text = dr_str(&r, NULL);
        if (!r.err && text && *text) { snprintf(full, sizeof full, "%s: %s", err, text); err = full; }
    }
    /* a code that was only shown has done its work, whether the pairing took or not: nothing else closes it */
    if (!strcmp(o->op, "pair") && prompt.active && !strncmp(prompt.kind, "display-", 8)) prompt_drop();
    BtDevice *d = o->path[0] ? device(&model, o->path, 0) : NULL;
    if (d && bt_should_trust(o->op, err, d)) trust(d);
    if (!strcmp(o->op, "pair") && !err && d) {
        /* paired (and trusted above): connect */
        Op *c = op_new("connect", o->path);
        if (c && !sysbus_call(BLUEZ, o->path, "org.bluez.Device1", "Connect", NULL, NULL, op_done, c)) free(c);
        else if (c) { result("pair", o->path, 1, NULL); free(o); dirty = 1; return; }
    }
    if (d) d->busy = 0;
    result(o->op, o->path, !err, err);
    dirty = 1;
    free(o);
}

static int call_op(const char *op, const char *path, const char *iface, const char *member, const char *sig, const DBuf *body) {
    Op *o = op_new(op, path);
    if (!o) return -1;
    if (!sysbus_call(BLUEZ, path, iface, member, sig, body, op_done, o)) { free(o); return -1; }
    return 0;
}

static const BtAdapter *first_adapter(void) { return owner[0] && model.nadapters ? &model.adapters[0] : NULL; }

int bt_power(int on) {
    const BtAdapter *a = first_adapter();
    if (!a) return -1;
    DBuf b;
    dw_init(&b);
    dw_str(&b, "org.bluez.Adapter1");
    dw_str(&b, "Powered");
    dw_variant(&b, "b");
    dw_bool(&b, on);
    Op *o = op_new("power", "");
    int ok = o && sysbus_call(BLUEZ, a->path, "org.freedesktop.DBus.Properties", "Set", "ssv", &b, op_done, o);
    if (!ok) free(o);
    dw_free(&b);
    return ok ? 0 : -1;
}

int bt_discover(int on) {
    const BtAdapter *a = first_adapter();
    if (!a || (on && !a->powered)) return -1;
    Op *o = op_new(on ? "discover" : "stop", "");
    int ok = o && sysbus_call(BLUEZ, a->path, "org.bluez.Adapter1", on ? "StartDiscovery" : "StopDiscovery", NULL, NULL, op_done, o);
    if (!ok) free(o);
    return ok ? 0 : -1;
}

static BtDevice *known(const char *path) { return owner[0] && is_device_path(path) ? device(&model, path, 0) : NULL; }

int bt_pair(const char *path) {
    BtDevice *d = known(path);
    if (!d) return -1;
    if (call_op("pair", d->path, "org.bluez.Device1", "Pair", NULL, NULL)) return -1;
    d->busy = 1;
    dirty = 1;
    return 0;
}

int bt_connect(const char *path) {
    BtDevice *d = known(path);
    if (!d || call_op("connect", d->path, "org.bluez.Device1", "Connect", NULL, NULL)) return -1;
    d->busy = 1;
    dirty = 1;
    return 0;
}

int bt_disconnect(const char *path) {
    BtDevice *d = known(path);
    return d ? call_op("disconnect", d->path, "org.bluez.Device1", "Disconnect", NULL, NULL) : -1;
}

int bt_forget(const char *path) {
    BtDevice *d = known(path);
    if (!d || !d->adapter[0]) return -1;
    DBuf b;
    dw_init(&b);
    dw_str(&b, d->path);
    Op *o = op_new("forget", d->path);
    int ok = o && sysbus_call(BLUEZ, d->adapter, "org.bluez.Adapter1", "RemoveDevice", "o", &b, op_done, o);
    if (!ok) free(o);
    dw_free(&b);
    return ok ? 0 : -1;
}
