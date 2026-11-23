/* guest_dbus_test.c — lordos-guest's own D-Bus client (tools/guest/dbusmin.c) and the performance profile's record
 * (tools/guest/perf.c): the method calls it builds, the replies it accepts, dconf's change set (byte for byte what GLib
 * makes for the same a{smv}), and the record's strict reading. The calls against real buses (dbus-daemon, dconf-service
 * and stand-ins for power-profiles-daemon and the portal) are tests/guest_perf_test.sh. */
#include "../../tools/guest/dbusmin.h"
#include "../../tools/guest/perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* a reply as a bus would send it (little-endian): REPLY_SERIAL, SIGNATURE (if any), maybe ERROR_NAME */
static size_t reply(uint8_t *o, int type, uint32_t serial, uint32_t rs, const char *sig, const char *err, const uint8_t *body, size_t bn) {
    size_t n = 0;
    o[n++] = 'l'; o[n++] = (uint8_t)type; o[n++] = 1; o[n++] = 1;
    put32(o + n, (uint32_t)bn); n += 4;
    put32(o + n, serial); n += 4;
    n += 4;
    /* REPLY_SERIAL */
    o[n++] = 5; o[n++] = 1; o[n++] = 'u'; o[n++] = 0; put32(o + n, rs); n += 4;
    if (sig) { while (n % 8) o[n++] = 0; o[n++] = 8; o[n++] = 1; o[n++] = 'g'; o[n++] = 0; o[n++] = (uint8_t)strlen(sig); memcpy(o + n, sig, strlen(sig) + 1); n += strlen(sig) + 1; }
    if (err) { while (n % 8) o[n++] = 0; o[n++] = 4; o[n++] = 1; o[n++] = 's'; o[n++] = 0; while (n % 4) o[n++] = 0; put32(o + n, (uint32_t)strlen(err)); n += 4; memcpy(o + n, err, strlen(err) + 1); n += strlen(err) + 1; }
    put32(o + 12, (uint32_t)(n - 16));
    while (n % 8) o[n++] = 0;
    if (bn) memcpy(o + n, body, bn);
    return n + bn;
}

static void test_build(void) {
    uint8_t m[1024], body[256];
    DbW w;
    dbw_init(&w, body, sizeof body);
    dbw_str(&w, "net.hadess.PowerProfiles");
    dbw_str(&w, "ActiveProfile");
    dbw_variant_str(&w, "performance");
    CHECK(!w.bad);
    /* s: 4 length + 24 + NUL = 29, padded to 32; s: 4 + 13 + 1 = 18 -> 50; g "s": 3 -> 53, padded to 56; s 4 + 11 + 1 */
    CHECK(w.n == 72);
    static const uint8_t want_body_start[] = { 24, 0, 0, 0, 'n', 'e', 't', '.' };
    CHECK(!memcmp(body, want_body_start, sizeof want_body_start));
    CHECK(body[50] == 1 && body[51] == 's' && body[52] == 0 && body[56] == 11 && !memcmp(body + 60, "performance", 12));
    size_t n = db_call_build(m, sizeof m, 7, "net.hadess.PowerProfiles", "/net/hadess/PowerProfiles", "org.freedesktop.DBus.Properties", "Set", "ssv", body, w.n);
    CHECK(n > 16 && n % 8 == 0);
    CHECK(m[0] == 'l' && m[1] == DB_CALL && m[2] == 0 && m[3] == 1 && m[4] == 72 && m[8] == 7);
    /* what it built reads back as a call with that signature and body */
    DbMsg r;
    CHECK(db_msg_parse(m, n, &r) == (int)n && r.type == DB_CALL && r.serial == 7 && !strcmp(r.sig, "ssv") && r.body_n == 72 && !memcmp(r.body, body, 72));
    CHECK(db_msg_parse(m, n - 1, &r) == 0 && db_msg_parse(m, 15, &r) == 0);
    /* names D-Bus wouldn't take, an empty serial, a body without a signature, too small a buffer */
    CHECK(!db_call_build(m, sizeof m, 7, NULL, "net/hadess", NULL, "Get", NULL, NULL, 0));
    CHECK(!db_call_build(m, sizeof m, 7, NULL, "/net//hadess", NULL, "Get", NULL, NULL, 0));
    CHECK(!db_call_build(m, sizeof m, 7, NULL, "/net/hadess/", NULL, "Get", NULL, NULL, 0));
    CHECK(!db_call_build(m, sizeof m, 7, NULL, "/n", NULL, "Get.x", NULL, NULL, 0));
    CHECK(!db_call_build(m, sizeof m, 7, "nodots", "/n", NULL, "Get", NULL, NULL, 0));
    CHECK(!db_call_build(m, sizeof m, 7, NULL, "/n", "a..b", "Get", NULL, NULL, 0));
    CHECK(!db_call_build(m, sizeof m, 7, NULL, "/n", "a.b-c", "Get", NULL, NULL, 0));      /* '-' only in a bus name */
    CHECK(db_call_build(m, sizeof m, 7, "ca.desrt-x.dconf", "/n", NULL, "Get", NULL, NULL, 0));
    CHECK(!db_call_build(m, sizeof m, 0, NULL, "/n", NULL, "Get", NULL, NULL, 0));
    CHECK(!db_call_build(m, sizeof m, 7, NULL, "/n", NULL, "Get", NULL, body, 4));
    CHECK(!db_call_build(m, sizeof m, 7, NULL, "/n", NULL, "Get", "s\n", body, 4));
    CHECK(!db_call_build(m, 40, 7, "net.hadess.PowerProfiles", "/net/hadess/PowerProfiles", NULL, "Get", NULL, NULL, 0));
    /* the writer: a string D-Bus would refuse (control characters), a full buffer */
    dbw_init(&w, body, sizeof body);
    dbw_str(&w, "a\nb");
    CHECK(w.bad);
    dbw_init(&w, body, 6);
    dbw_str(&w, "abc");
    CHECK(w.bad);
    dbw_init(&w, body, sizeof body);
    static const uint8_t ay[3] = { 1, 2, 3 };
    dbw_sig(&w, "s");
    dbw_bytes(&w, ay, 3);
    CHECK(!w.bad && w.n == 4 + 4 + 3 && body[4] == 3 && body[8] == 1 && body[10] == 3);
}

static void test_parse(void) {
    uint8_t m[512], b[64];
    DbMsg r;
    char s[32];
    int v;
    /* Get's answer: v holding s */
    size_t bn = 0;
    b[bn++] = 1; b[bn++] = 's'; b[bn++] = 0; b[bn++] = 0; put32(b + bn, 8); bn += 4; memcpy(b + bn, "balanced", 9); bn += 9;
    size_t n = reply(m, DB_RETURN, 9, 3, "v", NULL, b, bn);
    CHECK(db_msg_parse(m, n, &r) == (int)n && r.type == DB_RETURN && r.reply_serial == 3 && !strcmp(r.sig, "v"));
    CHECK(!db_reply_str(&r, s, sizeof s) && !strcmp(s, "balanced"));
    CHECK(db_reply_str(&r, s, 8) == -1);                                /* no room */
    CHECK(db_reply_bool(&r, &v) == -1);                                 /* not a b */
    /* every shorter prefix wants more, never reads past it */
    for (size_t k = 0; k < n; k++) CHECK(db_msg_parse(m, k, &r) == 0);
    /* every single byte changed: either still a message or refused, never a crash (ASan) */
    for (size_t k = 0; k < n; k++)
        for (int x = 0; x < 256; x += 17) {
            uint8_t c[512];
            memcpy(c, m, n);
            c[k] ^= (uint8_t)(x | 1);
            int u = db_msg_parse(c, n, &r);
            CHECK(u >= -1 && u <= (int)n);
            if (u > 0) { db_reply_str(&r, s, sizeof s); db_reply_bool(&r, &v); }
        }
    /* the portal's Read: a variant in a variant holding b; ReadOne: one variant */
    bn = 0;
    b[bn++] = 1; b[bn++] = 'v'; b[bn++] = 0; b[bn++] = 1; b[bn++] = 'b'; b[bn++] = 0; b[bn++] = 0; b[bn++] = 0; put32(b + bn, 1); bn += 4;
    n = reply(m, DB_RETURN, 10, 4, "v", NULL, b, bn);
    CHECK(db_msg_parse(m, n, &r) == (int)n && !db_reply_bool(&r, &v) && v == 1);
    bn = 0;
    b[bn++] = 1; b[bn++] = 'b'; b[bn++] = 0; b[bn++] = 0; put32(b + bn, 0); bn += 4;
    n = reply(m, DB_RETURN, 10, 4, "v", NULL, b, bn);
    CHECK(db_msg_parse(m, n, &r) == (int)n && !db_reply_bool(&r, &v) && v == 0);
    put32(m + n - 4, 2);                                                /* a boolean that is 2 */
    CHECK(db_msg_parse(m, n, &r) == (int)n && db_reply_bool(&r, &v) == -1);
    m[n - 5] = 1;                                                       /* padding that isn't zero */
    put32(m + n - 4, 0);
    CHECK(db_msg_parse(m, n, &r) == (int)n && db_reply_bool(&r, &v) == -1);
    /* variants nested too deep */
    bn = 0;
    for (int i = 0; i < 5; i++) { b[bn++] = 1; b[bn++] = 'v'; b[bn++] = 0; }
    b[bn++] = 1; b[bn++] = 'b'; b[bn++] = 0;
    while (bn % 4) b[bn++] = 0;
    put32(b + bn, 1); bn += 4;
    n = reply(m, DB_RETURN, 10, 4, "v", NULL, b, bn);
    CHECK(db_msg_parse(m, n, &r) == (int)n && db_reply_bool(&r, &v) == -1);
    /* an error, with its name */
    n = reply(m, DB_ERROR, 11, 5, NULL, "org.freedesktop.DBus.Error.AccessDenied", NULL, 0);
    CHECK(db_msg_parse(m, n, &r) == (int)n && r.type == DB_ERROR && r.reply_serial == 5 && !strcmp(r.err, "org.freedesktop.DBus.Error.AccessDenied"));
    /* malformed: an endianness byte, a type, a version, a zero serial, a body without a signature, a reply to nothing */
    n = reply(m, DB_RETURN, 9, 3, "v", NULL, b, 0);
    uint8_t c[512];
    memcpy(c, m, n); c[0] = 'x'; CHECK(db_msg_parse(c, n, &r) == -1);
    memcpy(c, m, n); c[1] = 5; CHECK(db_msg_parse(c, n, &r) == -1);
    memcpy(c, m, n); c[3] = 2; CHECK(db_msg_parse(c, n, &r) == -1);
    memcpy(c, m, n); put32(c + 8, 0); CHECK(db_msg_parse(c, n, &r) == -1);
    n = reply(m, DB_RETURN, 9, 3, NULL, NULL, b, 4);
    CHECK(db_msg_parse(m, n, &r) == -1);
    n = reply(m, DB_RETURN, 9, 0, NULL, NULL, NULL, 0);
    CHECK(db_msg_parse(m, n, &r) == -1);
    n = reply(m, DB_ERROR, 9, 3, NULL, NULL, NULL, 0);                  /* an error without a name */
    CHECK(db_msg_parse(m, n, &r) == -1);
    /* sizes past the limit are refused before waiting for them */
    n = reply(m, DB_RETURN, 9, 3, "v", NULL, b, 0);
    memcpy(c, m, n); put32(c + 4, DB_MSG_MAX); CHECK(db_msg_parse(c, n, &r) == -1);
    memcpy(c, m, n); put32(c + 12, DB_MSG_MAX + 1); CHECK(db_msg_parse(c, n, &r) == -1);
    memcpy(c, m, n); put32(c + 12, 400); CHECK(db_msg_parse(c, n, &r) == 0);   /* longer fields: more is needed */
    CHECK(db_msg_parse(NULL, 16, &r) == -1);
    /* a big-endian reply */
    static const uint8_t be[] = { 'B', DB_RETURN, 0, 1, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 8, 5, 1, 'u', 0, 0, 0, 0, 3 };
    CHECK(db_msg_parse(be, sizeof be, &r) == (int)sizeof be && r.big && r.reply_serial == 3 && r.serial == 9);
}

static void test_dconf(void) {
    /* GLib.Variant('a{smv}', {'/org/gnome/desktop/interface/enable-animations': GLib.Variant('b', v)}), its bytes */
    static const char *const glib[2] = {
        "2f6f72672f676e6f6d652f6465736b746f702f696e746572666163652f656e61626c652d616e696d6174696f6e730000000062002f35",
        "2f6f72672f676e6f6d652f6465736b746f702f696e746572666163652f656e61626c652d616e696d6174696f6e730000010062002f35" };
    for (int v = 0; v < 2; v++) {
        uint8_t o[256];
        char hex[600];
        size_t n = db_dconf_bool(o, sizeof o, "/org/gnome/desktop/interface/enable-animations", v);
        for (size_t i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", o[i]);
        hex[2 * n] = 0;
        CHECK(n == 54 && !strcmp(hex, glib[v]));
        CHECK(db_dconf_bool(o, 53, "/org/gnome/desktop/interface/enable-animations", v) == 0);
    }
    uint8_t o[300];
    CHECK(!db_dconf_bool(o, sizeof o, "org/gnome", 0) && !db_dconf_bool(o, sizeof o, "/org/gnome/", 0) && !db_dconf_bool(o, sizeof o, "/org//gnome", 0));
    CHECK(!db_dconf_bool(o, sizeof o, "/a b", 0) && !db_dconf_bool(o, sizeof o, "/", 0) && !db_dconf_bool(o, sizeof o, NULL, 0));
    CHECK(db_dconf_bool(o, sizeof o, "/a", 1) == 2 + 1 + 5 + 4 + 1 + 1);
    char longkey[260];
    memset(longkey, 'a', sizeof longkey); longkey[0] = '/'; longkey[250] = 0;
    CHECK(!db_dconf_bool(o, sizeof o, longkey, 1));                     /* past one-byte offsets */
}

static void test_record(void) {
    char t[128], pw[16];
    int a;
    int n = perf_record_write(t, sizeof t, "balanced", 1);
    CHECK(n > 0 && !strcmp(t, "lordos-guest perf 1\npower balanced\nanim 1\n"));
    CHECK(!perf_record_parse(t, n, pw, sizeof pw, &a) && !strcmp(pw, "balanced") && a == 1);
    n = perf_record_write(t, sizeof t, "", -1);
    CHECK(n > 0 && !perf_record_parse(t, n, pw, sizeof pw, &a) && !pw[0] && a == -1);
    n = perf_record_write(t, sizeof t, "power-saver", 0);
    CHECK(n > 0 && !perf_record_parse(t, n, pw, sizeof pw, &a) && !strcmp(pw, "power-saver") && a == 0);
    CHECK(!perf_record_write(t, sizeof t, "performance", 1) && !perf_record_write(t, sizeof t, "turbo", 1) && !perf_record_write(t, sizeof t, "", 2));
    CHECK(!perf_record_write(t, 10, "balanced", 1));
    static const char *const bad[] = {
        "", "lordos-guest perf 1\n", "lordos-guest perf 2\npower balanced\nanim 1\n", "lordos-guest perf 1\npower performance\nanim 1\n",
        "lordos-guest perf 1\npower turbo\nanim 1\n", "lordos-guest perf 1\npower balanced\nanim 2\n", "lordos-guest perf 1\npower balanced\nanim 1",
        "lordos-guest perf 1\npower balanced\nanim 1\n\n", "lordos-guest perf 1\npower \nanim 1\n", "lordos-guest perf 1\npower balanced\nanim  1\n",
        "lordos-guest perf 1\npower balanced\r\nanim 1\n", "lordos-guest perf 1\npower balanced\nanimx1\n", "Lordos-guest perf 1\npower -\nanim -\n" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        if (perf_record_parse(bad[i], (int)strlen(bad[i]), pw, sizeof pw, &a) != -1) fprintf(stderr, "  accepted: %s\n", bad[i]);
        CHECK(perf_record_parse(bad[i], (int)strlen(bad[i]), pw, sizeof pw, &a) == -1);
    }
    static const char nul[] = "lordos-guest perf 1\npower -\nanim -\n";
    char z[sizeof nul];
    memcpy(z, nul, sizeof nul);
    z[25] = 0;
    CHECK(perf_record_parse(nul, (int)strlen(nul), pw, sizeof pw, &a) == 0 && perf_record_parse(z, (int)strlen(nul), pw, sizeof pw, &a) == -1);
    CHECK(perf_record_parse(nul, (int)strlen(nul), pw, 1, &a) == 0);   /* "" fits */
    CHECK(perf_record_write(t, sizeof t, "balanced", 1) && perf_record_parse(t, (int)strlen(t), pw, 8, &a) == -1);
}

int main(void) {
    test_build();
    test_parse();
    test_dconf();
    test_record();
    printf("guest-dbus-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
