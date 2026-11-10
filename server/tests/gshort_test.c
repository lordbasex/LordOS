/* gshort_test.c — the global shortcuts portal's rules without a bus (src/ui/gshort.c): triggers, the
 * combinations LordOS keeps, a session's states and the remembered decisions. */
#include "../src/ui/gshort.h"
#include "../src/wl/keygrab.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static const char *fmt(const char *trigger) {
    static char out[64];
    GsKey k;
    if (gs_parse(trigger, &k)) return "(invalid)";
    gs_format(&k, out, sizeof out);
    return out;
}

static int kept(const char *trigger) {
    GsKey k;
    return gs_parse(trigger, &k) ? -1 : gs_kept(&k);
}

int main(void) {
    GsKey k;
    /* the spec's text and sdk/shortcuts.js's, in any case and order */
    CHECK(!gs_parse("CTRL+SHIFT+a", &k) && k.mods == (KG_CTRL | KG_SHIFT) && k.letter == 'a' && !k.code);
    CHECK(!gs_parse("LOGO+space", &k) && k.mods == KG_META && !k.letter && k.code == KEY_SPACE);
    CHECK(!strcmp(fmt("CTRL+SHIFT+a"), "Ctrl+Shift+A"));
    CHECK(!strcmp(fmt("shift+ctrl+A"), "Ctrl+Shift+A"));
    CHECK(!strcmp(fmt("LOGO+space"), "Meta+Space"));
    CHECK(!strcmp(fmt("Control+Alt+F9"), "Ctrl+Alt+F9"));
    CHECK(!strcmp(fmt("F24"), "F24"));
    CHECK(!strcmp(fmt("F13"), "F13"));
    CHECK(!strcmp(fmt("ALT+Return"), "Alt+Enter"));
    CHECK(!strcmp(fmt("CTRL+Page_Up"), "Ctrl+PageUp"));
    CHECK(!strcmp(fmt("CTRL+comma"), "Ctrl+,"));
    CHECK(!strcmp(fmt("Ctrl+,"), "Ctrl+,"));
    CHECK(!strcmp(fmt("ALT+0"), "Alt+0"));
    CHECK(!strcmp(fmt("ALT+7"), "Alt+7"));
    CHECK(!strcmp(fmt("Ctrl+Shift+ArrowUp"), "Ctrl+Shift+ArrowUp"));
    /* what is formatted reads back as the same key */
    static const char *const round[] = { "Ctrl+Shift+A", "Alt+F9", "Ctrl+Alt+Space", "Ctrl+/", "Ctrl+`", "Pause", "Ctrl+\\", NULL };
    for (int i = 0; round[i]; i++) CHECK(!strcmp(fmt(round[i]), round[i]));
    /* invalid ones */
    static const char *const bad[] = { "", "+", "CTRL+", "+a", "CTRL++", "CTRL+CTRL+a", "NUM+a", "HYPER+a", "CTRL+SHIFT",
                                       "CTRL+ab", "CTRL+é", "F0", "F25", "F100", "CTRL+Kp_1", "CTRL a", "CTRL+SHIFT+a+b",
                                       "CTRL+SHIFT+ALT+LOGO+aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL };
    for (int i = 0; bad[i]; i++) { CHECK(gs_parse(bad[i], &k) == -1); CHECK(!k.mods && !k.letter && !k.code); }
    CHECK(gs_parse(NULL, &k) == -1);

    /* matching: exact modifiers, letters by what the layout types, other keys by position */
    gs_parse("CTRL+SHIFT+a", &k);
    CHECK(gs_match(&k, KEY_Q, 'a', KG_CTRL | KG_SHIFT));          /* the key printed A on AZERTY */
    CHECK(!gs_match(&k, KEY_A, 'q', KG_CTRL | KG_SHIFT));
    CHECK(!gs_match(&k, KEY_A, 'a', KG_CTRL));
    CHECK(!gs_match(&k, KEY_A, 'a', KG_CTRL | KG_SHIFT | KG_ALT));
    gs_parse("ALT+F9", &k);
    CHECK(gs_match(&k, KEY_F9, 0, KG_ALT));
    CHECK(!gs_match(&k, KEY_F9, 0, KG_ALT | KG_CTRL));
    GsKey a, b;
    gs_parse("ctrl+shift+a", &a);
    gs_parse("Shift+Ctrl+A", &b);
    CHECK(gs_same(&a, &b));
    gs_parse("Ctrl+B", &b);
    CHECK(!gs_same(&a, &b));

    /* the combinations LordOS keeps, without a theme table */
    CHECK(kept("CTRL+SHIFT+a") == GS_FREE);
    CHECK(kept("ALT+F9") == GS_FREE);
    CHECK(kept("F9") == GS_FREE);
    CHECK(kept("Shift+F12") == GS_FREE);
    CHECK(kept("Pause") == GS_FREE);
    CHECK(kept("CTRL+ALT+space") == GS_FREE);                     /* the shell checks its own (sdk/shortcuts.js) */
    CHECK(kept("LOGO+space") == GS_LOGO);
    CHECK(kept("LOGO+F9") == GS_LOGO);
    CHECK(kept("CTRL+LOGO+q") == GS_LOGO);
    CHECK(kept("Print") == GS_SYSTEM);
    CHECK(kept("CTRL+Print") == GS_SYSTEM);
    CHECK(kept("ALT+Tab") == GS_SYSTEM);
    CHECK(kept("CTRL+Tab") == GS_SYSTEM);
    CHECK(kept("CTRL+ALT+SHIFT+Escape") == GS_SYSTEM);           /* ends a screen share */
    CHECK(kept("CTRL+ALT+F2") == GS_SYSTEM);                     /* another console */
    CHECK(kept("CTRL+ALT+BackSpace") == GS_SYSTEM);
    CHECK(kept("CTRL+ALT+Delete") == GS_SYSTEM);
    CHECK(kept("a") == GS_TYPING);
    CHECK(kept("SHIFT+a") == GS_TYPING);
    CHECK(kept("space") == GS_TYPING);
    CHECK(kept("Return") == GS_TYPING);
    CHECK(kept("Up") == GS_TYPING);
    CHECK(kept("SHIFT+1") == GS_TYPING);
    CHECK(kept("Delete") == GS_TYPING);
    /* the theme's table: Lord Classic keeps ⌥F4, Lord Flow with Desktops arrows ⌃← */
    CHECK(kept("ALT+F4") == GS_FREE);
    CHECK(kept("CTRL+Left") == GS_FREE);
    CHECK(keygrab_set("classic"));
    CHECK(kept("ALT+F4") == GS_THEME);
    CHECK(keygrab_set("flow-desktops"));
    CHECK(kept("CTRL+Left") == GS_THEME);
    CHECK(kept("CTRL+SHIFT+Left") == GS_FREE);
    CHECK(keygrab_set(""));

    /* a session: bound once, keys only while bound, nothing after it closed */
    int s = gs_step(GSS_NONE, GSO_CREATE);
    CHECK(s == GSS_NEW);
    CHECK(gs_step(s, GSO_KEY) == -1);
    CHECK(gs_step(s, GSO_ANSWER) == -1);                        /* no sheet was asked for */
    CHECK(gs_step(s, GSO_LIST) == GSS_NEW);
    s = gs_step(s, GSO_BIND);
    CHECK(s == GSS_ASKING);
    CHECK(gs_step(s, GSO_BIND) == -1);                          /* one sheet at a time */
    CHECK(gs_step(s, GSO_KEY) == -1);
    CHECK(gs_step(s, GSO_LIST) == GSS_ASKING);
    CHECK(gs_step(s, GSO_CANCEL) == GSS_NEW);                   /* cancelled: may ask again */
    s = gs_step(s, GSO_ANSWER);
    CHECK(s == GSS_BOUND);
    CHECK(gs_step(s, GSO_KEY) == GSS_BOUND);
    CHECK(gs_step(s, GSO_BIND) == -1);                          /* once per session; Settings changes it */
    CHECK(gs_step(s, GSO_CANCEL) == -1);
    CHECK(gs_step(s, GSO_CREATE) == -1);
    s = gs_step(s, GSO_CLOSE);
    CHECK(s == GSS_CLOSED);
    for (int op = GSO_CREATE; op <= GSO_CLOSE; op++) CHECK(gs_step(GSS_CLOSED, op) == -1);
    CHECK(gs_step(GSS_ASKING, GSO_CLOSE) == GSS_CLOSED);
    CHECK(gs_step(GSS_NEW, GSO_CLOSE) == GSS_CLOSED);
    CHECK(gs_step(GSS_NONE, GSO_CLOSE) == -1);
    CHECK(gs_step(7, GSO_KEY) == -1 && gs_step(GSS_BOUND, 9) == -1 && gs_step(-1, GSO_KEY) == -1);

    /* ids and descriptions */
    CHECK(gs_id_valid("push-to-talk"));
    CHECK(gs_id_valid("a"));
    CHECK(!gs_id_valid(""));
    CHECK(!gs_id_valid(NULL));
    CHECK(!gs_id_valid("with space"));
    CHECK(!gs_id_valid("tab\there"));
    CHECK(!gs_id_valid("ñ"));
    CHECK(!gs_id_valid("0123456789012345678901234567890123456789012345678901234567890123"));
    char d[8];
    gs_text(d, sizeof d, "a\nb\x1b" "c");
    CHECK(!strcmp(d, "abc"));
    gs_text(d, sizeof d, "abcdeñ");                             /* ñ is 2 bytes and doesn't fit in 7 */
    CHECK(!strcmp(d, "abcdeñ") || !strcmp(d, "abcde"));
    gs_text(d, 7, "abcdeñ");
    CHECK(!strcmp(d, "abcde"));
    gs_text(d, sizeof d, "\xc3");                               /* a cut sequence */
    CHECK(!strcmp(d, ""));

    /* the store: decisions per app, the same combination never for two */
    static GsStore st;
    GsKey ptt, rec;
    gs_parse("CTRL+SHIFT+t", &ptt);
    gs_parse("ALT+F9", &rec);
    CHECK(gs_store_set(&st, "desktop:com.example.Talk", "ptt", "Push to talk", &ptt));
    CHECK(gs_store_set(&st, "desktop:com.example.Talk", "mute", "Mute", NULL));
    CHECK(gs_store_set(&st, "desktop:com.example.Rec", "rec", "Record\n", &rec));
    CHECK(!gs_store_set(&st, "", "x", "", &rec));
    CHECK(!gs_store_set(&st, "desktop:x", "bad id", "", &rec));
    CHECK(st.n == 3);
    CHECK(gs_store_owner(&st, &ptt, "desktop:com.example.Rec", "rec") != NULL);     /* another app has it */
    CHECK(gs_store_owner(&st, &ptt, "desktop:com.example.Talk", "mute") != NULL);   /* another shortcut of the app */
    CHECK(gs_store_owner(&st, &ptt, "desktop:com.example.Talk", "ptt") == NULL);    /* its own */
    GsKey other;
    gs_parse("CTRL+ALT+x", &other);
    CHECK(gs_store_owner(&st, &other, "desktop:com.example.Rec", "rec") == NULL);
    char *json = gs_store_json(&st);
    CHECK(json != NULL);
    static GsStore back;
    CHECK(json && !gs_store_load(&back, json, strlen(json)));
    CHECK(back.n == 3);
    GsEntry *e = gs_store_find(&back, "desktop:com.example.Talk", "ptt");
    CHECK(e && e->bound && gs_same(&e->key, &ptt) && !strcmp(e->desc, "Push to talk"));
    e = gs_store_find(&back, "desktop:com.example.Talk", "mute");
    CHECK(e && !e->bound);
    e = gs_store_find(&back, "desktop:com.example.Rec", "rec");
    CHECK(e && e->bound && !strcmp(e->desc, "Record"));
    free(json);
    /* forgetting an app takes all its decisions and nothing else */
    CHECK(gs_store_forget(&back, "desktop:com.example.Talk") == 2);
    CHECK(back.n == 1 && gs_store_find(&back, "desktop:com.example.Rec", "rec"));
    CHECK(gs_store_forget(&back, "desktop:none") == 0);

    /* a file written by hand: kept keys, a second owner and bad entries are dropped, not the rest */
    static const char hand[] =
        "{\"apps\":{\"desktop:a\":{\"one\":{\"description\":\"One\",\"trigger\":\"LOGO+l\"},"
        "\"two\":{\"description\":\"Two\",\"trigger\":\"CTRL+ALT+k\"},\"bad id\":{\"trigger\":\"CTRL+ALT+j\"},"
        "\"three\":{\"trigger\":\"nonsense\"},\"four\":7},"
        "\"desktop:b\":{\"dup\":{\"trigger\":\"Ctrl+Alt+K\"}},\"desktop:c\":[]}}";
    CHECK(!gs_store_load(&back, hand, strlen(hand)));
    e = gs_store_find(&back, "desktop:a", "one");
    CHECK(e && !e->bound);                                      /* ❖L is the lock screen's */
    e = gs_store_find(&back, "desktop:a", "two");
    CHECK(e && e->bound && !strcmp(e->desc, "Two"));
    CHECK(!gs_store_find(&back, "desktop:a", "bad id"));
    e = gs_store_find(&back, "desktop:a", "three");
    CHECK(e && !e->bound);
    CHECK(!gs_store_find(&back, "desktop:a", "four"));
    e = gs_store_find(&back, "desktop:b", "dup");
    CHECK(e && !e->bound);                                      /* desktop:a had it first */
    CHECK(gs_store_load(&back, "{", 1) == -1 && back.n == 0);
    CHECK(!gs_store_load(&back, "[]", 2) && back.n == 0);

    if (fails) { printf("gshort-test: %d failures\n", fails); return 1; }
    printf("gshort-test: ok\n");
    return 0;
}
