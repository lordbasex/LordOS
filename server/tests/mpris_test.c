/* mpris_test.c — reading the Linux players' MPRIS properties (src/hw/mprismeta.c), without a bus:
 * bodies are built with our own D-Bus writer (vault/dbus.c), parsed by the strict reader and read
 * into the bounded fields. Covers GetAll, PropertiesChanged-style dictionaries, Metadata replacing
 * the track, long and broken strings, lengths of every integer type, cover art (only local files),
 * the names a player may own and Get(Position). Run by `make test` (mk/settings.mk). */
#include "hw/mprismeta.h"
#include "vault/dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void prop_s(DBuf *b, const char *k, const char *v) { dw_struct(b); dw_str(b, k); dw_variant(b, "s"); dw_str(b, v); }
static void prop_b(DBuf *b, const char *k, int v) { dw_struct(b); dw_str(b, k); dw_variant(b, "b"); dw_bool(b, v); }
static void prop_x(DBuf *b, const char *k, int64_t v) { dw_struct(b); dw_str(b, k); dw_variant(b, "x"); dw_u64(b, (uint64_t)v); }
static void prop_t(DBuf *b, const char *k, uint64_t v) { dw_struct(b); dw_str(b, k); dw_variant(b, "t"); dw_u64(b, v); }
static void prop_i(DBuf *b, const char *k, int32_t v) { dw_struct(b); dw_str(b, k); dw_variant(b, "i"); dw_i32(b, v); }
static void prop_o(DBuf *b, const char *k, const char *v) { dw_struct(b); dw_str(b, k); dw_variant(b, "o"); dw_str(b, v); }
static void prop_as(DBuf *b, const char *k, const char *const *v, int n) {
    dw_struct(b); dw_str(b, k); dw_variant(b, "as");
    size_t m = dw_arr_begin(b, 4);
    for (int i = 0; i < n; i++) dw_str(b, v[i]);
    dw_arr_end(b, m);
}

/* Builds a full reply with that body and parses it back like one from the bus. */
static int as_msg(const char *sig, DBuf *body, DMsg *m) {
    DHdr h = { .type = DBUS_RETURN, .serial = 7, .reply_serial = 3, .sig = sig, .sender = ":1.5" };
    DBuf out;
    dw_init(&out);
    if (dbus_build(&h, body, &out)) return -1;
    long n = dbus_msg_size(out.p, out.n);
    int r = n == (long)out.n ? dbus_msg_parse(out.p, out.n, m) : -1;
    dw_free(&out);
    return r;
}

/* Reads the a{sv} body into p; the MP_* bits, or -2 if the message didn't build. */
static int read_body(DBuf *b, MprisProps *p) {
    DMsg m;
    if (as_msg("a{sv}", b, &m)) { dw_free(b); return -2; }
    DRd r;
    dr_init(&r, &m);
    int got = mpris_read_props(&r, p);
    dbus_msg_free(&m);
    dw_free(b);
    return got;
}

static void test_getall(void) {
    MprisProps p;
    mpris_props_init(&p);
    CHECK(p.can_control == 1 && p.status == MPRIS_STOPPED);
    DBuf b;
    dw_init(&b);
    size_t a = dw_arr_begin(&b, 8);
    prop_s(&b, "PlaybackStatus", "Playing");
    prop_s(&b, "LoopStatus", "None");
    prop_x(&b, "Position", 61500000);
    prop_b(&b, "CanPlay", 1);
    prop_b(&b, "CanPause", 1);
    prop_b(&b, "CanGoNext", 1);
    prop_b(&b, "CanGoPrevious", 0);
    prop_b(&b, "CanSeek", 1);
    dw_struct(&b); dw_str(&b, "Metadata"); dw_variant(&b, "a{sv}");
    size_t md = dw_arr_begin(&b, 8);
    prop_o(&b, "mpris:trackid", "/org/example/track/7");
    prop_x(&b, "mpris:length", 245000000);
    prop_s(&b, "xesam:title", "  Night   drive\n");
    static const char *const artists[] = { "First", "", "Second" };
    prop_as(&b, "xesam:artist", artists, 3);
    prop_s(&b, "xesam:album", "Album");
    prop_s(&b, "mpris:artUrl", "file:///home/someone/.cache/art/cover%20one.png");
    prop_i(&b, "xesam:trackNumber", 4);
    dw_arr_end(&b, md);
    dw_arr_end(&b, a);
    int got = read_body(&b, &p);
    CHECK(got == (MP_STATUS | MP_POS | MP_CAPS | MP_META));
    CHECK(p.status == MPRIS_PLAYING);
    CHECK(p.position_us == 61500000);
    CHECK(p.length_us == 245000000);
    CHECK(!strcmp(p.title, "Night drive"));
    CHECK(!strcmp(p.artist, "First, Second"));
    CHECK(!strcmp(p.album, "Album"));
    CHECK(!strcmp(p.art, "/home/someone/.cache/art/cover one.png"));
    CHECK(!strcmp(p.trackid, "/org/example/track/7"));
    CHECK(p.can_play && p.can_pause && p.can_next && !p.can_prev && p.can_seek);

    /* a new Metadata replaces the whole track: no art or album left from the last one */
    dw_init(&b);
    a = dw_arr_begin(&b, 8);
    dw_struct(&b); dw_str(&b, "Metadata"); dw_variant(&b, "a{sv}");
    md = dw_arr_begin(&b, 8);
    prop_s(&b, "xesam:title", "Next one");
    prop_s(&b, "xesam:artist", "Only a string");
    prop_t(&b, "mpris:length", 30000000);
    prop_s(&b, "mpris:artUrl", "https://example.org/cover.jpg");
    dw_arr_end(&b, md);
    prop_s(&b, "PlaybackStatus", "Sideways");          /* not a state: kept as it was */
    dw_arr_end(&b, a);
    got = read_body(&b, &p);
    CHECK(got == MP_META);
    CHECK(p.status == MPRIS_PLAYING);
    CHECK(!strcmp(p.title, "Next one") && !strcmp(p.artist, "Only a string") && !p.album[0] && !p.art[0] && !p.trackid[0]);
    CHECK(p.length_us == 30000000);

    /* the root interface and CanControl */
    dw_init(&b);
    a = dw_arr_begin(&b, 8);
    prop_s(&b, "Identity", "Music\tPlayer");
    prop_b(&b, "CanControl", 0);
    prop_x(&b, "Identity", 5);                         /* an unexpected type is skipped */
    dw_arr_end(&b, a);
    got = read_body(&b, &p);
    CHECK(got == (MP_IDENTITY | MP_CAPS));
    CHECK(!strcmp(p.identity, "Music Player") && !p.can_control);
}

static void test_clamps(void) {
    MprisProps p;
    mpris_props_init(&p);
    DBuf b;
    dw_init(&b);
    size_t a = dw_arr_begin(&b, 8);
    prop_x(&b, "Position", -5);
    dw_struct(&b); dw_str(&b, "Metadata"); dw_variant(&b, "a{sv}");
    size_t md = dw_arr_begin(&b, 8);
    prop_t(&b, "mpris:length", UINT64_MAX);
    char big[2000];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = 0;
    prop_s(&b, "xesam:title", big);
    /* "é" right at the cut: never half a character */
    char cut[300];
    memset(cut, 'b', 254);
    memcpy(cut + 254, "\xc3\xa9", 3);
    prop_s(&b, "xesam:album", cut);
    dw_arr_end(&b, md);
    dw_arr_end(&b, a);
    CHECK(read_body(&b, &p) == (MP_POS | MP_META));
    CHECK(p.position_us == 0);
    CHECK(p.length_us == MPRIS_MAX_US);
    CHECK(strlen(p.title) == sizeof p.title - 1);
    CHECK(strlen(p.album) == 254);

    CHECK(mpris_clamp_us(-1) == 0 && mpris_clamp_us(MPRIS_MAX_US + 1) == MPRIS_MAX_US && mpris_clamp_us(42) == 42);

    char out[16];
    mpris_clean("a\x01" "b\x7f" "c\xff" "d", out, sizeof out);
    CHECK(!strcmp(out, "a b c d"));
    mpris_clean("\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac", out, sizeof out);   /* six €, room for five */
    CHECK(strlen(out) == 15);
    mpris_clean("\xed\xa0\x80x", out, sizeof out);      /* a surrogate is not UTF-8 */
    CHECK(!strcmp(out, "x"));
    mpris_clean(NULL, out, sizeof out);
    CHECK(!out[0]);

    /* not a dictionary at all */
    dw_init(&b);
    dw_str(&b, "Playing");
    DMsg m;
    CHECK(as_msg("s", &b, &m) == 0);
    DRd r;
    dr_init(&r, &m);
    mpris_props_init(&p);
    CHECK(mpris_read_props(&r, &p) == -1);
    dbus_msg_free(&m);
    dw_free(&b);
}

static void test_art(void) {
    char o[64];
    CHECK(mpris_art_path("file:///tmp/a.png", o, sizeof o) == 0 && !strcmp(o, "/tmp/a.png"));
    CHECK(mpris_art_path("file://localhost/tmp/b%C3%A9.jpg", o, sizeof o) == 0 && !strcmp(o, "/tmp/b\xc3\xa9.jpg"));
    CHECK(mpris_art_path("http://example.org/a.png", o, sizeof o) == -1 && !o[0]);
    CHECK(mpris_art_path("data:image/png;base64,AAAA", o, sizeof o) == -1);
    CHECK(mpris_art_path("file://otherhost/tmp/a.png", o, sizeof o) == -1);
    CHECK(mpris_art_path("file:///tmp/../etc/passwd", o, sizeof o) == -1);
    CHECK(mpris_art_path("file:///tmp/%2E%2E/etc", o, sizeof o) == -1);
    CHECK(mpris_art_path("file:///tmp/..", o, sizeof o) == -1);
    CHECK(mpris_art_path("file:///tmp/..x/a.png", o, sizeof o) == 0);
    CHECK(mpris_art_path("file:///tmp/a%00.png", o, sizeof o) == -1);
    CHECK(mpris_art_path("file:///tmp/a%2Fb", o, sizeof o) == -1);
    CHECK(mpris_art_path("file:///tmp/a%0A.png", o, sizeof o) == -1);
    CHECK(mpris_art_path("file:///tmp/a%G1", o, sizeof o) == -1);
    CHECK(mpris_art_path("file:///tmp/a.png?x=1", o, sizeof o) == -1);
    CHECK(mpris_art_path("file:///", o, sizeof o) == -1);
    CHECK(mpris_art_path("file:///tmp/a-name-much-much-longer-than-the-sixty-four-byte-output-buffer-it-goes-into.png", o, sizeof o) == -1);
    CHECK(mpris_art_path(NULL, o, sizeof o) == -1);
}

static void test_names(void) {
    CHECK(mpris_name_ok("org.mpris.MediaPlayer2.player"));
    CHECK(mpris_name_ok("org.mpris.MediaPlayer2.player.instance42"));
    CHECK(!mpris_name_ok("org.mpris.MediaPlayer2."));
    CHECK(!mpris_name_ok("org.mpris.MediaPlayer2"));
    CHECK(!mpris_name_ok("org.mpris.MediaPlayer3.player"));
    CHECK(!mpris_name_ok("org.mpris.MediaPlayer2..x"));
    CHECK(!mpris_name_ok(":1.42"));
    CHECK(!mpris_name_ok(NULL));
    char longname[300] = "org.mpris.MediaPlayer2.";
    memset(longname + strlen(longname), 'a', 260);
    longname[290] = 0;
    CHECK(!mpris_name_ok(longname));
    CHECK(mpris_status_of("Paused") == MPRIS_PAUSED && mpris_status_of("Stopped") == MPRIS_STOPPED && mpris_status_of("playing") == -1);
}

static void test_position(void) {
    DBuf b;
    dw_init(&b);
    dw_variant(&b, "x");
    dw_u64(&b, (uint64_t)12345678);
    DMsg m;
    CHECK(as_msg("v", &b, &m) == 0);
    DRd r;
    dr_init(&r, &m);
    int64_t us = -1;
    CHECK(mpris_read_position(&r, &us) == 0 && us == 12345678);
    dbus_msg_free(&m);
    dw_free(&b);
    dw_init(&b);
    dw_variant(&b, "s");
    dw_str(&b, "soon");
    CHECK(as_msg("v", &b, &m) == 0);
    dr_init(&r, &m);
    CHECK(mpris_read_position(&r, &us) == -1);
    dbus_msg_free(&m);
    dw_free(&b);
}

int main(void) {
    test_getall();
    test_clamps();
    test_art();
    test_names();
    test_position();
    if (fails) { fprintf(stderr, "mpris-test: %d of %d checks failed\n", fails, checks); return 1; }
    printf("mpris-test: %d checks ok\n", checks);
    return 0;
}
