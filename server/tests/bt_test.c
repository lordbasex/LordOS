/* bt_test.c — BlueZ parsing for Settings → Bluetooth (src/hw/bt.c), without a bus: messages
 * are built with our own D-Bus writer (vault/dbus.c), parsed by the strict reader and fed to
 * the model. Covers GetManagedObjects, PropertiesChanged-style dictionaries, Battery1, removal,
 * object path validation, the state JSON and which devices are trusted (bt_should_trust). Run by
 * `make test` (mk/settings.mk). */
#include "hw/bt.h"
#include "vault/dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void prop_s(DBuf *b, const char *k, const char *v) { dw_struct(b); dw_str(b, k); dw_variant(b, "s"); dw_str(b, v); }
static void prop_o(DBuf *b, const char *k, const char *v) { dw_struct(b); dw_str(b, k); dw_variant(b, "o"); dw_str(b, v); }
static void prop_b(DBuf *b, const char *k, int v) { dw_struct(b); dw_str(b, k); dw_variant(b, "b"); dw_bool(b, v); }
static void prop_u(DBuf *b, const char *k, uint32_t v) { dw_struct(b); dw_str(b, k); dw_variant(b, "u"); dw_u32(b, v); }
static void prop_y(DBuf *b, const char *k, uint8_t v) { dw_struct(b); dw_str(b, k); dw_variant(b, "y"); dw_u8(b, v); }
static void prop_n(DBuf *b, const char *k, int16_t v) {
    dw_struct(b); dw_str(b, k); dw_variant(b, "n");
    dw_align(b, 2); dw_u8(b, (uint8_t)(v & 0xff)); dw_u8(b, (uint8_t)((uint16_t)v >> 8));
}
static void prop_as(DBuf *b, const char *k, const char *a, const char *c) {
    dw_struct(b); dw_str(b, k); dw_variant(b, "as");
    size_t m = dw_arr_begin(b, 4); dw_str(b, a); dw_str(b, c); dw_arr_end(b, m);
}

/* Builds a full message with that body and parses it back like one from the bus. */
static int as_msg(const char *sig, DBuf *body, uint8_t type, DMsg *m) {
    DHdr h = { .type = type, .serial = 7, .reply_serial = type == DBUS_RETURN ? 3 : 0, .sig = sig,
               .path = type == DBUS_SIGNAL ? "/org/bluez/hci0/dev_00_AA_01_01_00_01" : NULL,
               .iface = type == DBUS_SIGNAL ? "org.freedesktop.DBus.Properties" : NULL,
               .member = type == DBUS_SIGNAL ? "PropertiesChanged" : NULL, .sender = ":1.5" };
    DBuf out;
    dw_init(&out);
    if (dbus_build(&h, body, &out)) return -1;
    long n = dbus_msg_size(out.p, out.n);
    int r = n == (long)out.n ? dbus_msg_parse(out.p, out.n, m) : -1;
    dw_free(&out);
    return r;
}

static void managed_body(DBuf *b) {
    dw_init(b);
    size_t objs = dw_arr_begin(b, 8);
    /* the adapter */
    dw_struct(b); dw_str(b, "/org/bluez/hci0");
    size_t ifs = dw_arr_begin(b, 8);
    dw_struct(b); dw_str(b, "org.freedesktop.DBus.Introspectable");
    size_t p0 = dw_arr_begin(b, 8); dw_arr_end(b, p0);
    dw_struct(b); dw_str(b, "org.bluez.Adapter1");
    size_t p1 = dw_arr_begin(b, 8);
    prop_s(b, "Address", "00:AA:01:00:00:00");
    prop_s(b, "Alias", "lordos-notes");
    prop_b(b, "Powered", 1);
    prop_b(b, "Discoverable", 0);
    prop_b(b, "Discovering", 1);
    prop_u(b, "Class", 0x6c010c);
    prop_as(b, "UUIDs", "0000110e-0000-1000-8000-00805f9b34fb", "0000110c-0000-1000-8000-00805f9b34fb");
    dw_arr_end(b, p1);
    dw_arr_end(b, ifs);
    /* a paired headset with a battery */
    dw_struct(b); dw_str(b, "/org/bluez/hci0/dev_00_AA_01_01_00_01");
    ifs = dw_arr_begin(b, 8);
    dw_struct(b); dw_str(b, "org.bluez.Device1");
    p1 = dw_arr_begin(b, 8);
    prop_s(b, "Address", "00:AA:01:01:00:01");
    prop_s(b, "Name", "Auriculares de prueba");
    prop_s(b, "Alias", "Auriculares de prueba");
    prop_s(b, "Icon", "audio-headset");
    prop_o(b, "Adapter", "/org/bluez/hci0");
    prop_u(b, "Class", 0x240404);
    prop_s(b, "AddressType", "public");
    prop_b(b, "Paired", 1);
    prop_b(b, "Trusted", 1);
    prop_b(b, "Connected", 0);
    prop_n(b, "RSSI", -67);
    prop_as(b, "UUIDs", "0000110b-0000-1000-8000-00805f9b34fb", "0000111e-0000-1000-8000-00805f9b34fb");
    dw_arr_end(b, p1);
    dw_struct(b); dw_str(b, "org.bluez.Battery1");
    p1 = dw_arr_begin(b, 8);
    prop_y(b, "Percentage", 80);
    dw_arr_end(b, p1);
    dw_arr_end(b, ifs);
    /* an unnamed LE device on a second adapter's path that does not exist (kept) */
    dw_struct(b); dw_str(b, "/org/bluez/hci1/dev_11_22_33_44_55_66");
    ifs = dw_arr_begin(b, 8);
    dw_struct(b); dw_str(b, "org.bluez.Device1");
    p1 = dw_arr_begin(b, 8);
    prop_s(b, "Address", "11:22:33:44:55:66");
    dw_arr_end(b, p1);
    dw_arr_end(b, ifs);
    /* something that is not BlueZ's: ignored */
    dw_struct(b); dw_str(b, "/org/bluez");
    ifs = dw_arr_begin(b, 8);
    dw_struct(b); dw_str(b, "org.bluez.AgentManager1");
    p1 = dw_arr_begin(b, 8); dw_arr_end(b, p1);
    dw_arr_end(b, ifs);
    dw_arr_end(b, objs);
}

int main(void) {
    CHECK(bt_path_valid("/org/bluez/hci0"));
    CHECK(bt_path_valid("/org/bluez/hci12/dev_00_AA_01_01_00_01"));
    CHECK(!bt_path_valid("/org/bluez/hci0/dev_00_AA_01_01_00"));
    CHECK(!bt_path_valid("/org/bluez/hci0/dev_00_AA_01_01_00_0G"));
    CHECK(!bt_path_valid("/org/bluez/hci0/dev_00_AA_01_01_00_01/sep1"));
    CHECK(!bt_path_valid("/org/bluez/hcix"));
    CHECK(!bt_path_valid("/org/evil/hci0"));
    CHECK(!bt_path_valid(NULL));

    BtModel *m = bt_model();
    memset(m, 0, sizeof *m);
    DBuf b;
    managed_body(&b);
    DMsg msg;
    CHECK(as_msg("a{oa{sa{sv}}}", &b, DBUS_RETURN, &msg) == 0);
    dw_free(&b);
    CHECK(bt_parse_managed(m, &msg) == 0);
    dbus_msg_free(&msg);
    CHECK(m->nadapters == 1);
    CHECK(!strcmp(m->adapters[0].address, "00:AA:01:00:00:00") && !strcmp(m->adapters[0].alias, "lordos-notes"));
    CHECK(m->adapters[0].powered && !m->adapters[0].discoverable && m->adapters[0].discovering);
    CHECK(m->ndevices == 2);
    BtDevice *d = &m->devices[0];
    CHECK(!strcmp(d->path, "/org/bluez/hci0/dev_00_AA_01_01_00_01") && !strcmp(d->adapter, "/org/bluez/hci0"));
    CHECK(!strcmp(d->name, "Auriculares de prueba") && !strcmp(d->icon, "audio-headset") && d->cls == 0x240404);
    CHECK(d->paired && d->trusted && !d->connected && d->has_rssi && d->rssi == -67 && d->battery == 80);
    CHECK(m->devices[1].battery == -1 && !strcmp(m->devices[1].adapter, "/org/bluez/hci1"));
    /* the audio services (A2DP sink, hands-free) as 16-bit numbers, and the address type */
    CHECK(d->nservices == 2 && d->services[0] == 0x110b && d->services[1] == 0x111e && !strcmp(d->addr_type, "public"));
    CHECK(m->devices[1].nservices == 0 && !m->devices[1].addr_type[0] && !m->devices[1].cls);

    /* UUIDs on the Bluetooth base UUID only (issue #78: the low-energy side of a headset lists a
     * vendor's 16-bit service and no audio one) */
    CHECK(bt_uuid16("0000febe-0000-1000-8000-00805f9b34fb") == 0xfebe);
    CHECK(bt_uuid16("0000110B-0000-1000-8000-00805F9B34FB") == 0x110b);
    CHECK(bt_uuid16("0000110b-0000-1000-8000-00805f9b34fc") == -1);
    CHECK(bt_uuid16("1000110b-0000-1000-8000-00805f9b34fb") == -1);
    CHECK(bt_uuid16("0000110g-0000-1000-8000-00805f9b34fb") == -1);
    CHECK(bt_uuid16("9ec813b4-256b-4090-93a8-a4f0e9107733") == -1 && bt_uuid16("0000110b") == -1 && bt_uuid16(NULL) == -1);

    /* PropertiesChanged on the device: connected, battery gone */
    dw_init(&b);
    dw_str(&b, "org.bluez.Device1");
    size_t a = dw_arr_begin(&b, 8);
    prop_b(&b, "Connected", 1);
    prop_s(&b, "Alias", "Mis auriculares");
    prop_as(&b, "UUIDs", "0000110b-0000-1000-8000-00805f9b34fb", "9ec813b4-256b-4090-93a8-a4f0e9107733");
    prop_s(&b, "AddressType", "bogus");
    dw_arr_end(&b, a);
    a = dw_arr_begin(&b, 4);
    dw_str(&b, "RSSI");
    dw_arr_end(&b, a);
    CHECK(as_msg("sa{sv}as", &b, DBUS_SIGNAL, &msg) == 0);
    dw_free(&b);
    DRd r;
    dr_init(&r, &msg);
    const char *iface = dr_str(&r, NULL);
    CHECK(iface && !strcmp(iface, "org.bluez.Device1"));
    bt_apply_props(m, msg.path, iface, &r);
    CHECK(!r.err);
    dbus_msg_free(&msg);
    CHECK(d->connected && !strcmp(d->alias, "Mis auriculares"));
    CHECK(d->nservices == 1 && d->services[0] == 0x110b && !d->addr_type[0]);   /* the new list replaces the old; vendor UUID dropped */
    bt_remove_object(m, d->path, "org.bluez.Battery1");
    CHECK(d->battery == -1);

    char *j = bt_state_json();
    CHECK(j != NULL);
    /* without bluetoothd on the bus the state says so, but still lists what is in the model */
    CHECK(j && strstr(j, "\"available\":false") && strstr(j, "\"name\":\"Mis auriculares\"") && strstr(j, "\"connected\":true"));
    CHECK(j && strstr(j, "\"rssi\":-67") && !strstr(j, "11:22:33:44:55:66"));     /* other adapter's device: not listed */
    CHECK(j && strstr(j, "\"addressType\":\"\",\"services\":[\"110b\"]"));
    free(j);

    /* a truncated dictionary is rejected by the reader, the model is not corrupted */
    dw_init(&b);
    dw_str(&b, "org.bluez.Device1");
    a = dw_arr_begin(&b, 8);
    prop_b(&b, "Paired", 0);
    dw_arr_end(&b, a);
    a = dw_arr_begin(&b, 4);
    dw_arr_end(&b, a);
    CHECK(as_msg("sa{sv}as", &b, DBUS_SIGNAL, &msg) == 0);
    msg.body_len -= 6;                      /* cut inside the dictionary */
    dr_init(&r, &msg);
    iface = dr_str(&r, NULL);
    bt_apply_props(m, msg.path, iface, &r);
    CHECK(r.err);
    dbus_msg_free(&msg);
    dw_free(&b);

    /* removing the adapter takes its devices */
    bt_remove_object(m, "/org/bluez/hci0", "org.bluez.Adapter1");
    CHECK(m->nadapters == 0 && m->ndevices == 1 && !strcmp(m->devices[0].address, "11:22:33:44:55:66"));
    bt_remove_object(m, "/org/bluez/hci1/dev_11_22_33_44_55_66", "org.bluez.Device1");
    CHECK(m->ndevices == 0);

    /* a reply with the wrong signature */
    dw_init(&b);
    dw_str(&b, "x");
    CHECK(as_msg("s", &b, DBUS_RETURN, &msg) == 0);
    CHECK(bt_parse_managed(m, &msg) != 0);
    dbus_msg_free(&msg);
    dw_free(&b);

    /* which devices are made Trusted, so they reconnect at boot before anyone logs in (issue #75) */
    BtDevice t;
    memset(&t, 0, sizeof t);
    t.paired = 1;
    CHECK(bt_should_trust("pair", NULL, &t));
    CHECK(bt_should_trust("pair", "org.bluez.Error.AlreadyExists", &t));             /* the bond was there already */
    CHECK(bt_should_trust("pair", "org.bluez.Error.AlreadyExists: Already Paired", &t));
    CHECK(!bt_should_trust("pair", "org.bluez.Error.AuthenticationFailed", &t));
    CHECK(bt_should_trust("connect", NULL, &t));                                       /* the user connected a paired one */
    CHECK(!bt_should_trust("connect", "org.bluez.Error.Failed", &t));
    CHECK(bt_should_trust("authorize", NULL, &t));                                     /* our agent let it in anyway */
    CHECK(!bt_should_trust("disconnect", NULL, &t) && !bt_should_trust("forget", NULL, &t) && !bt_should_trust("power", NULL, &t));
    t.paired = 0;
    t.bonded = 1;
    CHECK(bt_should_trust("connect", NULL, &t) && bt_should_trust("authorize", NULL, &t));
    t.bonded = 0;                                                                      /* never paired: never trusted */
    CHECK(!bt_should_trust("connect", NULL, &t) && !bt_should_trust("authorize", NULL, &t));
    t.paired = t.trusted = 1;                                                          /* already trusted: nothing to send */
    CHECK(!bt_should_trust("pair", NULL, &t) && !bt_should_trust("connect", NULL, &t) && !bt_should_trust("authorize", NULL, &t));
    CHECK(!bt_should_trust(NULL, NULL, &t) && !bt_should_trust("pair", NULL, NULL));

    printf("bt: %d/%d passed\n", checks - fails, checks);
    return fails != 0;
}
