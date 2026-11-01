/* share_test.c — the screen-sharing API's pure parts: framing, each message, the rate limits and the grants'
 * file (src/remote/sharewire.c, sharegrant.c). Exits 1 on any failure. */
#include "remote/sharewire.h"
#include "remote/sharegrant.h"
#include "remote/proto.h"
#include "sec/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int parse(int type, const void *d, uint32_t n, SwMsg *m) {
    SwFrame f = { type, d, n, 0 };
    return sw_parse(&f, m);
}

static void test_framing(void) {
    uint8_t b[16] = { 3, 0, 0, 0, RP_BTN, 1, 1, 9 };
    SwFrame f;
    CHECK(sw_next(b, 0, &f) == 0);
    CHECK(sw_next(b, 3, &f) == 0);
    CHECK(sw_next(b, 6, &f) == 0);
    CHECK(sw_next(b, 7, &f) == 1 && f.type == RP_BTN && f.n == 2 && f.used == 7);
    uint8_t zero[5] = { 0 };
    CHECK(sw_next(zero, 5, &f) == -1);
    uint32_t big = SW_MAX_MSG + 2;
    memcpy(zero, &big, 4);
    CHECK(sw_next(zero, 5, &f) == -1);
    big = SW_MAX_MSG + 1;
    memcpy(zero, &big, 4);
    CHECK(sw_next(zero, 5, &f) == 0);
}

static void test_messages(void) {
    SwMsg m;
    uint8_t s[8] = { SW_F_VIDEO | SW_F_INPUT, 1, 60, 0 };
    uint32_t k = 4000;
    memcpy(s + 4, &k, 4);
    CHECK(!parse(SHARE_START, s, 8, &m) && m.codec == 1 && m.hz == 60 && m.kbps == 4000 && m.flags == 5);
    CHECK(parse(SHARE_START, s, 7, &m));
    s[0] = SW_F_INPUT;                  CHECK(parse(SHARE_START, s, 8, &m));   /* input with no picture */
    s[0] = 0x11;                        CHECK(parse(SHARE_START, s, 8, &m));   /* an unknown flag */
    s[0] = 1; s[1] = 3;                 CHECK(parse(SHARE_START, s, 8, &m));
    s[1] = 2; s[2] = 23;                CHECK(parse(SHARE_START, s, 8, &m));
    s[2] = 241;                         CHECK(parse(SHARE_START, s, 8, &m));
    s[2] = 60; k = 299; memcpy(s + 4, &k, 4); CHECK(parse(SHARE_START, s, 8, &m));
    k = 20001; memcpy(s + 4, &k, 4);    CHECK(parse(SHARE_START, s, 8, &m));

    CHECK(!parse(SHARE_STOP, NULL, 0, &m));
    CHECK(parse(SHARE_STOP, s, 1, &m));
    uint8_t on = 2;
    CHECK(parse(SHARE_AWAKE, &on, 1, &m));
    on = 1;
    CHECK(!parse(SHARE_HEAR, &on, 1, &m) && m.on == 1);

    int16_t p[2] = { 10, 20 };
    CHECK(!parse(RP_PTR, p, 4, &m) && m.x == 10 && m.y == 20);
    p[0] = -1;                          CHECK(parse(RP_PTR, p, 4, &m));
    CHECK(parse(RP_PTR, p, 5, &m));
    uint8_t b[2] = { 3, 1 };            CHECK(parse(RP_BTN, b, 2, &m));
    b[0] = 2; b[1] = 2;                 CHECK(parse(RP_BTN, b, 2, &m));
    uint8_t key[3] = { 30, 0, 1 };
    CHECK(!parse(RP_KEY, key, 3, &m) && m.code == 30 && m.value == 1);
    key[2] = 3;                         CHECK(parse(RP_KEY, key, 3, &m));
    key[0] = 0; key[2] = 1;             CHECK(parse(RP_KEY, key, 3, &m));
    key[0] = 0xff; key[1] = 2;          CHECK(parse(RP_KEY, key, 3, &m));

    uint8_t sc[9] = { 4 };              CHECK(parse(RP_SCROLL, sc, 9, &m));
    sc[0] = 1;                          CHECK(!parse(RP_SCROLL, sc, 9, &m));
    uint16_t sz[2] = { 639, 480 };      CHECK(parse(RP_RESIZE, sz, 4, &m));
    sz[0] = 1280;                       CHECK(!parse(RP_RESIZE, sz, 4, &m));
    sz[1] = 2401;                       CHECK(parse(RP_RESIZE, sz, 4, &m));

    /* the session socket's root messages and anything unknown are not part of this door */
    static const int never[] = { RP_SEAT, RP_DISPLAY, RP_UNDISPLAY, RP_GREETER_IN, RP_INPUT_ADD, RP_KEYRING, RP_NOTICE, RP_HELLO, RP_READY, RP_VIDEO,
                                 RP_VMODE, RP_PINCH, RP_SESSION, RP_GREETER, 0, 200, 255 };
    for (size_t i = 0; i < sizeof never / sizeof never[0]; i++) CHECK(parse(never[i], s, 8, &m));

    char t[SW_TEXT_MAX + 1];
    memset(t, 'a', sizeof t);
    CHECK(!parse(SHARE_TEXT, t, SW_TEXT_MAX, &m));
    CHECK(parse(SHARE_TEXT, t, SW_TEXT_MAX + 1, &m));
    CHECK(parse(SHARE_TEXT, t, 0, &m));
    CHECK(!parse(SHARE_TEXT, "h\xc3\xa9", 3, &m));
    CHECK(parse(SHARE_TEXT, "a\0b", 3, &m));
    CHECK(parse(RP_SETCLIP, "\xff", 1, &m));
}

static void test_utf8(void) {
    CHECK(sw_utf8_valid("", 0));
    CHECK(sw_utf8_valid("a\xc3\xa9\xe2\x82\xac\xf0\x9f\x91\x91", 10));
    CHECK(!sw_utf8_valid("\xc0\xaf", 2));              /* overlong */
    CHECK(!sw_utf8_valid("\xe0\x80\xaf", 3));
    CHECK(!sw_utf8_valid("\xed\xa0\x80", 3));          /* surrogate */
    CHECK(!sw_utf8_valid("\xf4\x90\x80\x80", 4));      /* above U+10FFFF */
    CHECK(!sw_utf8_valid("\xe2\x82", 2));              /* cut */
    uint32_t cp;
    CHECK(sw_utf8_next("\xe2\x82\xac", 3, &cp) == 3 && cp == 0x20ac);
    CHECK(sw_utf8_next("\xf0\x9f\x91\x91", 4, &cp) == 4 && cp == 0x1f451);
}

static void test_limits(void) {
    SwBucket b = { 0 };
    int n = 0;
    for (int i = 0; i < 1000; i++) n += sw_bucket_take(&b, 1000, 1000, 100);
    CHECK(n == 100);
    CHECK(sw_bucket_take(&b, 1100, 1000, 100));

    /* a viewer asking every 3 s on a still screen, for an hour */
    SwKeyLimit k;
    sw_key_init(&k, 0);
    int keys = 0;
    for (double t = 0; t < 3600000; t += 100) {
        if ((long)t % 3000 == 0) sw_key_request(&k);
        keys += sw_key_due(&k, t);
    }
    CHECK(keys <= 3 + 121);             /* the burst, and one every 30 s: not 1,200 */
    CHECK(keys >= 100);
    printf("still screen, a request every 3 s for an hour: %d keyframes\n", keys);

    /* the same on a changing screen: served */
    sw_key_init(&k, 0);
    keys = 0;
    for (double t = 0; t < 60000; t += 100) {
        sw_key_changed(&k);
        if ((long)t % 3000 == 0) sw_key_request(&k);
        keys += sw_key_due(&k, t);
    }
    CHECK(keys == 20);

    /* never more than one in SW_KEY_MIN_MS, however much it changes */
    sw_key_init(&k, 0);
    keys = 0;
    for (double t = 0; t < 10000; t += 10) { sw_key_changed(&k); sw_key_request(&k); keys += sw_key_due(&k, t); }
    CHECK(keys >= 19 && keys <= 21);

    /* a request isn't lost: with the tokens spent it waits, and is served */
    sw_key_init(&k, 0);
    double t = 0;
    for (int i = 0; i < 3; i++) { t += 1000; sw_key_request(&k); CHECK(sw_key_due(&k, t)); }
    t += 1000;
    sw_key_request(&k);
    CHECK(!sw_key_due(&k, t));
    double w = sw_key_wait(&k, t);
    CHECK(w > 0 && w <= SW_KEY_STILL_MS);
    CHECK(sw_key_due(&k, t + w + 1));
    CHECK(sw_key_wait(&k, t + w + 1) == -1);

    /* a new viewer after a quiet spell: at once */
    sw_key_init(&k, 0);
    sw_key_request(&k);
    CHECK(sw_key_due(&k, 7200000));
}

static void test_grants(void) {
    CHECK(sg_id_valid("desktop:org.go-link.desk"));
    CHECK(sg_id_valid("lpk:go-link-desk"));
    CHECK(!sg_id_valid("lordos"));
    CHECK(!sg_id_valid("desktop:"));
    CHECK(!sg_id_valid("desktop:a b"));
    CHECK(!sg_id_valid("desktop:a\"b"));
    CHECK(!sg_id_valid("desktop:../x"));
    CHECK(!sg_id_valid(NULL));
    CHECK(sg_exe_valid("/usr/bin/foo"));
    CHECK(!sg_exe_valid("usr/bin/foo"));
    CHECK(!sg_exe_valid("/usr/bin/../foo"));
    CHECK(!sg_exe_valid("/usr/bin/a b"));
    CHECK(!sg_exe_valid("/usr/bin/a\"\n"));
    CHECK(sg_exe_valid("/opt/app-1.2/bin/foo_bar+x@y~z"));
    /* JSON's and the shell's characters, control characters and bytes above ASCII */
    static const char *const nasty[] = { "/a\"b", "/a\\b", "/a\nb", "/a\tb", "/a\rb", "/a\x01z", "/a\x7fz", "/a\xc3\xa9", "/a{b", "/a,b", "/a:b", "/a'b", "/a$b", "/a;b" };
    for (size_t i = 0; i < sizeof nasty / sizeof nasty[0]; i++) CHECK(!sg_exe_valid(nasty[i]));
    /* the attack itself: a path that closes its string and adds a grant */
    CHECK(!sg_exe_valid("/x\"},{\"id\":\"lpk:evil\",\"exe\":\"\",\"perm\":\"screen-share\",\"state\":\"allow\"},{\"id\":\"lpk:y\",\"exe\":\""));

    static SgStore s, r;
    memset(&s, 0, sizeof s);
    CHECK(sg_get(&s, "desktop:a", "/bin/a", SG_SCREEN) == SG_ASK);
    CHECK(!sg_set(&s, "desktop:a", "/bin/a", SG_SCREEN, SG_ALLOW));
    CHECK(sg_get(&s, "desktop:a", "/bin/a", SG_SCREEN) == SG_ALLOW);
    CHECK(sg_get(&s, "desktop:a", "/bin/b", SG_SCREEN) == SG_ASK);   /* another program asks again */
    CHECK(sg_get(&s, "desktop:a", NULL, SG_SCREEN) == SG_ASK);
    CHECK(sg_get(&s, "desktop:a", "/bin/a", SG_INPUT) == SG_ASK);    /* one permission doesn't give the other */
    CHECK(!sg_set(&s, "lpk:b", "", SG_INPUT, SG_DENY));
    CHECK(sg_get(&s, "lpk:b", "/whatever", SG_INPUT) == SG_DENY);
    CHECK(!sg_set(&s, "desktop:c", "", SG_SCREEN, SG_ALLOW));
    CHECK(sg_get(&s, "desktop:c", "/bin/c", SG_SCREEN) == SG_ASK);   /* a Linux app's grant with no path is worth nothing */

    char text[SG_FILE_MAX];
    int n = sg_write(&s, text, sizeof text);
    CHECK(n > 0);
    CHECK(!sg_parse(text, (size_t)n, &r) && r.n == 3);
    CHECK(sg_get(&r, "desktop:a", "/bin/a", SG_SCREEN) == SG_ALLOW);
    CHECK(sg_get(&r, "lpk:b", NULL, SG_INPUT) == SG_DENY);

    CHECK(!sg_set(&r, "desktop:a", "", SG_SCREEN, SG_ASK));
    CHECK(sg_get(&r, "desktop:a", "/bin/a", SG_SCREEN) == SG_ASK);
    sg_forget(&r, "lpk:b");
    CHECK(sg_get(&r, "lpk:b", NULL, SG_INPUT) == SG_ASK);

    /* sg_write escapes on its own, even for a value that never passed the checks: the text stays one entry */
    {
        static const char *const vals[] = {
            "/x\"},{\"id\":\"lpk:evil\",\"perm\":\"screen-share\",\"state\":\"allow\",\"exe\":\"", "/a\\", "/a\\\"", "/a\nb\tc\rd\x01\x1f",
        };
        for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++) {
            static SgStore w;
            memset(&w, 0, sizeof w);
            CHECK(!sg_set(&w, "desktop:a", vals[i], SG_SCREEN, SG_DENY));
            n = sg_write(&w, text, sizeof text);
            JDoc d;
            CHECK(n > 0 && !json_parse(text, (size_t)n, &d, NULL, 0));
            if (n > 0 && d.root) {
                const JVal *g = json_get(d.root, "grants");
                CHECK(g && g->len == 1 && !strcmp(json_str(g->kid, "exe"), vals[i]));
                json_free(&d);
            }
            /* and the file as a whole refuses such a path when read back: nothing is granted */
            CHECK(sg_parse(text, (size_t)n, &r) && r.n == 0);
            CHECK(sg_get(&r, "lpk:evil", NULL, SG_SCREEN) == SG_ASK);
        }
    }

    /* a full store refuses, a full file still fits */
    memset(&s, 0, sizeof s);
    for (int i = 0; i < SG_MAX; i++) {
        char id[32];
        snprintf(id, sizeof id, "lpk:app%d", i);
        CHECK(!sg_set(&s, id, "", SG_SCREEN, SG_ALLOW));
    }
    CHECK(sg_set(&s, "lpk:more", "", SG_SCREEN, SG_ALLOW));
    CHECK(sg_write(&s, text, sizeof text) > 0);

    /* a damaged file grants nothing, all of it */
    static const char *const bad[] = {
        "", "{", "[]", "{\"v\":2,\"grants\":[]}", "{\"v\":1}",
        "{\"v\":1,\"grants\":[{\"id\":\"lpk:a\",\"perm\":\"screen-share\",\"state\":\"allow\"},{\"id\":\"nope\",\"perm\":\"screen-share\",\"state\":\"allow\"}]}",
        "{\"v\":1,\"grants\":[{\"id\":\"lpk:a\",\"perm\":\"root\",\"state\":\"allow\"}]}",
        "{\"v\":1,\"grants\":[{\"id\":\"lpk:a\",\"perm\":\"screen-share\",\"state\":\"ask\"}]}",
        "{\"v\":1,\"grants\":[{\"id\":\"lpk:a\",\"perm\":\"screen-share\",\"state\":\"allow\",\"exe\":\"rel\"}]}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(sg_parse(bad[i], strlen(bad[i]), &r));
        CHECK(r.n == 0);
    }
    CHECK(sg_perm_of("screen") == -1);
    CHECK(sg_state_of("yes") == -1);
    CHECK(!strcmp(sw_reason_name(SW_LOCKED), "locked"));
    CHECK(!strcmp(sw_reason_name(999), "unknown"));
}

int main(void) {
    test_framing();
    test_messages();
    test_utf8();
    test_limits();
    test_grants();
    printf("share-test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
