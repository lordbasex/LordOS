/* kbinhibit_test.c — a Linux app that takes the desktop's shortcuts (src/wl/kbinhibit.c): whose decision it is,
 * when the person is asked, what holds after allow, deny, revoke and ❖Esc, and which keys reach the app. */
#include "../src/wl/kbinhibit.h"
#include "../src/wl/keygrab.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

int main(void) {
    char k[KBI_ID_MAX];

    /* the decision's name: the app_id, else the program; never anything that could break the file or the prompt */
    CHECK(kbi_app_key("org.gnome.Boxes", "/usr/bin/gnome-boxes", k, sizeof k) && !strcmp(k, "desktop:org.gnome.Boxes"));
    CHECK(kbi_app_key("", "/usr/bin/remmina", k, sizeof k) && !strcmp(k, "exe:/usr/bin/remmina"));
    CHECK(kbi_app_key(NULL, "/usr/bin/remmina", k, sizeof k) && !strcmp(k, "exe:/usr/bin/remmina"));
    CHECK(kbi_app_key("bad\"id", "/usr/bin/x", k, sizeof k) && !strcmp(k, "exe:/usr/bin/x"));
    CHECK(!kbi_app_key("", "", k, sizeof k) && !k[0]);
    CHECK(!kbi_app_key("", "relative/x", k, sizeof k));
    CHECK(!kbi_app_key("", "/usr/../tmp/x", k, sizeof k));
    CHECK(!kbi_app_key("", "/tmp/x y", k, sizeof k));
    CHECK(!kbi_app_key(".hidden", "/a\n", k, sizeof k));
    CHECK(!kbi_app_key("a", "/usr/bin/a", k, 10));                   /* too small a buffer: nothing */
    char longid[200];
    memset(longid, 'a', sizeof longid - 1);
    longid[sizeof longid - 1] = 0;
    CHECK(kbi_app_key(longid, "/usr/bin/a", k, sizeof k) && !strcmp(k, "exe:/usr/bin/a"));   /* too long an app_id */
    CHECK(kbi_key_valid("desktop:org.gnome.Boxes"));
    CHECK(kbi_key_valid("exe:/usr/bin/remmina"));
    CHECK(!kbi_key_valid("org.gnome.Boxes"));                        /* an .lpk id never passes as a Linux app's */
    CHECK(!kbi_key_valid("desktop:"));
    CHECK(!kbi_key_valid("desktop:a\"b"));
    CHECK(!kbi_key_valid("exe:rel"));
    CHECK(!kbi_key_valid("exe:/a/../b"));
    CHECK(!kbi_key_valid(NULL));

    /* asked once; allowed holds right away; denied never asks again; never for isolated apps or the login screen */
    CHECK(kbi_decide(0, 0, 0, 1) == KBI_PROMPT);
    CHECK(kbi_decide(1, 0, 0, 1) == KBI_GRANT);
    CHECK(kbi_decide(2, 0, 0, 1) == KBI_NEVER);
    CHECK(kbi_decide(1, 1, 0, 1) == KBI_NEVER);
    CHECK(kbi_decide(0, 1, 0, 1) == KBI_NEVER);
    CHECK(kbi_decide(1, 0, 1, 1) == KBI_NEVER);
    CHECK(kbi_decide(0, 0, 0, 0) == KBI_NEVER);

    /* allowed: active once; ❖Esc off and back on; revoked: inactive, and ❖Esc does nothing */
    KbiState s = { 0 };
    CHECK(kbi_toggle(&s) == KBI_SAME && !s.active);                  /* not allowed: ❖Esc is LordOS's */
    CHECK(kbi_set_allowed(&s, 1) == KBI_ACTIVE && s.active);
    CHECK(kbi_set_allowed(&s, 1) == KBI_SAME);
    CHECK(kbi_toggle(&s) == KBI_INACTIVE && !s.active && s.off);
    CHECK(kbi_set_allowed(&s, 1) == KBI_SAME && !s.active);          /* still off: allowing again doesn't undo ❖Esc */
    CHECK(kbi_toggle(&s) == KBI_ACTIVE && s.active);
    CHECK(kbi_set_allowed(&s, 0) == KBI_INACTIVE && !s.active);
    CHECK(kbi_toggle(&s) == KBI_SAME && !s.active);
    CHECK(kbi_toggle(&s) == KBI_SAME);
    CHECK(kbi_toggle(&s) == KBI_SAME);
    s.off = 1;                                                       /* revoked while off, allowed again: it holds */
    CHECK(kbi_set_allowed(&s, 0) == KBI_SAME && !s.off);
    CHECK(kbi_set_allowed(&s, 1) == KBI_ACTIVE);

    /* active: the desktop's shortcuts reach the app */
    CHECK(kbi_route(KEY_TAB, 0, KG_META, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_TAB, 0, KG_ALT, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_TAB, 0, KG_ALT | KG_SHIFT, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_LEFTMETA, 0, KG_META, 1) == KBI_KEY_APP);    /* ❖ alone: no launcher */
    CHECK(kbi_route(KEY_SPACE, 0, KG_META, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_Q, 'q', KG_META, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_M, 'm', KG_META, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_F4, 0, KG_ALT, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_DELETE, 0, KG_CTRL | KG_ALT, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_ESC, 0, KG_META | KG_SHIFT, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_ESC, 0, 0, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_A, 'a', 0, 1) == KBI_KEY_APP);
    CHECK(kbi_route(KEY_F2, 0, KG_META | KG_CTRL | KG_ALT, 1) == KBI_KEY_APP);
    /* …but ❖Esc, the lock, the console switch and the media row stay LordOS's */
    CHECK(kbi_route(KEY_ESC, 0, KG_META, 1) == KBI_KEY_TOGGLE);
    CHECK(kbi_route(KEY_L, 'l', KG_META, 1) == KBI_KEY_LORD);
    CHECK(kbi_route(KEY_Q, 'q', KG_META | KG_CTRL, 1) == KBI_KEY_LORD);
    CHECK(kbi_route(KEY_F1, 0, KG_CTRL | KG_ALT, 1) == KBI_KEY_LORD);
    CHECK(kbi_route(KEY_F12, 0, KG_CTRL | KG_ALT | KG_SHIFT, 1) == KBI_KEY_LORD);
    CHECK(kbi_route(KEY_VOLUMEUP, 0, 0, 1) == KBI_KEY_LORD);
    CHECK(kbi_route(KEY_SYSRQ, 0, 0, 1) == KBI_KEY_LORD);
    /* off after ❖Esc: every key goes as it would without an inhibitor, and ❖Esc turns it back on */
    CHECK(kbi_route(KEY_TAB, 0, KG_META, 0) == KBI_KEY_LORD);
    CHECK(kbi_route(KEY_A, 'a', 0, 0) == KBI_KEY_LORD);
    CHECK(kbi_route(KEY_ESC, 0, KG_META, 0) == KBI_KEY_TOGGLE);

    if (fails) { printf("kbinhibit-test: %d failures\n", fails); return 1; }
    printf("kbinhibit-test: ok\n");
    return 0;
}
