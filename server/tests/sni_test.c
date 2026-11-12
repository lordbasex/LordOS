/* sni_test.c — what the tray host reads from Linux apps (src/ui/snimodel.c), over real messages built and validated
 * by our own D-Bus code:
 *   - RegisterStatusNotifierItem's two forms (an object path, a bus name) and what is refused;
 *   - IconPixmap: the size nearest the bar's 22-32 px, network-order ARGB to RGBA, entries with a wrong size or byte
 *     count skipped, too many entries or too many bytes and none is kept, the reader ends after the array;
 *   - texts: mnemonics, control and bidi characters, broken UTF-8, cut on a character;
 *   - a com.canonical.dbusmenu layout: labels, separators, toggles, hidden and disabled entries, submenus, unknown
 *     properties and children of another type, the depth and item caps, the ids that can be clicked;
 *   - a menu the app built again with new ids: the entry chosen found by its place and label;
 *   - the Event a chosen entry sends: (isv u) with "clicked" and an int32 variant, as a real call to the menu's object. */
#include "ui/snimodel.h"
#include "vault/dbus.h"
#include "hw/jbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, total;
static void check(int ok, const char *what) { total++; if (!ok) { fails++; printf("FAIL: %s\n", what); } }

/* A method return with that body, validated as one from the bus would be; dr reads it. */
static int message(const char *sig, const DBuf *body, DMsg *m) {
    DHdr h = { .type = DBUS_RETURN, .serial = 9, .reply_serial = 1, .sig = sig, .sender = ":1.5", .dest = ":1.1" };
    DBuf out;
    if (dbus_build(&h, body, &out)) { dw_free(&out); return -1; }
    int r = dbus_msg_parse(out.p, out.n, m);
    dw_free(&out);
    return r;
}

/* ---------- the registered service ---------- */
static void t_service(void) {
    char dest[SNI_NAME_MAX], path[SNI_NAME_MAX];
    check(!sni_service("/org/ayatana/NotificationItem/sync", ":1.42", dest, path) && !strcmp(dest, ":1.42")
          && !strcmp(path, "/org/ayatana/NotificationItem/sync"), "path form: on the sender's connection");
    check(!sni_service("org.kde.StatusNotifierItem-1234-1", ":1.42", dest, path) && !strcmp(dest, "org.kde.StatusNotifierItem-1234-1")
          && !strcmp(path, "/StatusNotifierItem"), "name form: /StatusNotifierItem there");
    check(!sni_service(":1.42", ":1.42", dest, path) && !strcmp(dest, ":1.42"), "its own unique name");
    check(sni_service(":1.7", ":1.42", dest, path), "another connection's unique name is refused");
    check(sni_service("/a//b", ":1.42", dest, path) && sni_service("/a/", ":1.42", dest, path), "a malformed path is refused");
    check(sni_service("nodots", ":1.42", dest, path) && sni_service("", ":1.42", dest, path), "a malformed name is refused");
    check(sni_service("/ok", "org.not.unique", dest, path), "a sender that is not a unique name");
    char longp[400];
    longp[0] = '/';
    memset(longp + 1, 'a', 300);
    longp[301] = 0;
    check(sni_service(longp, ":1.42", dest, path), "a path past SNI_NAME_MAX is refused");
}

/* ---------- pixmaps ---------- */

/* one (iiay) entry: w x h, len bytes (w * h * 4 when len < 0), every pixel A R G B = a r g b */
static void pix_entry(DBuf *b, int w, int h, long len, uint8_t a, uint8_t r, uint8_t g, uint8_t bl) {
    dw_struct(b);
    dw_i32(b, w);
    dw_i32(b, h);
    size_t n = len < 0 ? (size_t)w * h * 4 : (size_t)len;
    uint8_t *px = malloc(n ? n : 1);
    for (size_t i = 0; i + 3 < n; i += 4) { px[i] = a; px[i + 1] = r; px[i + 2] = g; px[i + 3] = bl; }
    dw_bytes(b, px, n);
    free(px);
}

typedef struct { int w, h; long len; } PixSpec;

/* the array of those entries, then the string "after"; *out the pick. Returns sni_pixmap's result, -2 if the reader
 * did not end where the string starts. */
static int pick(const PixSpec *s, int n, SniPix *out) {
    DBuf b;
    dw_init(&b);
    size_t a = dw_arr_begin(&b, 8);
    for (int i = 0; i < n; i++) pix_entry(&b, s[i].w, s[i].h, s[i].len, 0x80, (uint8_t)s[i].w, 0x20, 0x30);
    dw_arr_end(&b, a);
    dw_str(&b, "after");
    DMsg m;
    int r = -3;
    if (!message("a(iiay)s", &b, &m)) {
        DRd rd;
        dr_init(&rd, &m);
        r = sni_pixmap(&rd, out);
        if (rd.err || strcmp(dr_str(&rd, NULL), "after")) r = -2;
        dbus_msg_free(&m);
    }
    dw_free(&b);
    return r;
}

static void t_pixmap(void) {
    check(sni_pix_score(32, 32) < sni_pix_score(24, 24) && sni_pix_score(24, 24) < sni_pix_score(22, 22), "22-32: the largest");
    check(sni_pix_score(22, 22) < sni_pix_score(48, 48) && sni_pix_score(48, 48) < sni_pix_score(64, 64), "then the nearest larger");
    check(sni_pix_score(256, 256) < sni_pix_score(16, 16), "a smaller one last");

    SniPix p;
    PixSpec a[] = { { 16, 16, -1 }, { 24, 24, -1 }, { 48, 48, -1 } };
    check(!pick(a, 3, &p) && p.w == 24 && p.h == 24, "16, 24, 48: 24");
    check(p.rgba && p.rgba[0] == 24 && p.rgba[1] == 0x20 && p.rgba[2] == 0x30 && p.rgba[3] == 0x80, "ARGB (network order) to RGBA");
    sni_pix_free(&p);
    PixSpec b[] = { { 64, 64, -1 }, { 48, 48, -1 } };
    check(!pick(b, 2, &p) && p.w == 48, "64, 48: 48");
    sni_pix_free(&p);
    PixSpec c[] = { { 8, 8, -1 }, { 16, 16, -1 } };
    check(!pick(c, 2, &p) && p.w == 16, "8, 16: 16");
    sni_pix_free(&p);
    PixSpec d[] = { { 22, 22, -1 }, { 32, 32, -1 }, { 24, 24, -1 } };
    check(!pick(d, 3, &p) && p.w == 32, "22, 32, 24: 32");
    sni_pix_free(&p);
    PixSpec e[] = { { 30, 20, -1 } };
    check(!pick(e, 1, &p) && p.w == 30 && p.h == 20, "not square");
    sni_pix_free(&p);

    PixSpec bad[] = { { 24, 24, 24 * 24 * 4 - 1 }, { 0, 24, 0 }, { 300, 1, 1200 }, { -4, -4, 64 }, { 64, 64, -1 } };
    check(!pick(bad, 5, &p) && p.w == 64, "short bytes, zero, too wide and negative sizes are skipped");
    sni_pix_free(&p);
    PixSpec none[] = { { 24, 24, 10 } };
    check(pick(none, 1, &p) == -1 && !p.rgba, "nothing usable: none");
    check(pick(none, 0, &p) == -1, "an empty array: none");

    PixSpec many[SNI_PIX_COUNT + 1];
    for (int i = 0; i <= SNI_PIX_COUNT; i++) many[i] = (PixSpec){ 2, 2, -1 };
    check(pick(many, SNI_PIX_COUNT, &p) == 0, "SNI_PIX_COUNT entries are read");
    sni_pix_free(&p);
    check(pick(many, SNI_PIX_COUNT + 1, &p) == -1, "one more and none is kept (the reader still ends after it)");
    PixSpec big[9];
    for (int i = 0; i < 9; i++) big[i] = (PixSpec){ 256, 256, -1 };
    check(pick(big, 8, &p) == 0 && p.w == 256, "8 x 256 KiB: within SNI_PIX_BYTES");
    sni_pix_free(&p);
    check(pick(big, 9, &p) == -1, "past SNI_PIX_BYTES in all: none");
}

/* ---------- texts ---------- */
static void t_text(void) {
    char o[SNI_TEXT_MAX];
    sni_text("_Open", o, sizeof o, 1);
    check(!strcmp(o, "Open"), "mnemonic dropped");
    sni_text("Save__As _Copy", o, sizeof o, 1);
    check(!strcmp(o, "Save_As Copy"), "\"__\" is one '_'");
    sni_text("snake_case", o, sizeof o, 0);
    check(!strcmp(o, "snake_case"), "no mnemonics in a title");
    sni_text("a\xe2\x80\xae" "b\xe2\x81\xa6" "c\xef\xbb\xbf" "d", o, sizeof o, 0);
    check(!strcmp(o, "abcd"), "bidi overrides, isolates and BOM dropped");
    sni_text("one\ntwo\tthree\x01\x7f", o, sizeof o, 0);
    check(!strcmp(o, "one two three"), "newline and tab read as a space, controls dropped");
    sni_text("x\xc2\x85y\xff\xc3(z\xc0\xaf", o, sizeof o, 0);
    check(!strcmp(o, "xy(z"), "C1 control, stray and overlong bytes dropped");
    sni_text("caf\xc3\xa9 \xe2\x82\xac", o, sizeof o, 0);
    check(!strcmp(o, "caf\xc3\xa9 \xe2\x82\xac"), "UTF-8 kept");
    char s[6];
    sni_text("abcd\xe2\x82\xac", s, sizeof s, 0);
    check(!strcmp(s, "abcd"), "cut on a character, never inside one");
    sni_text(NULL, o, sizeof o, 0);
    check(!o[0], "NULL: empty");
}

/* ---------- dbusmenu ---------- */
typedef struct E {
    int id;
    const char *label, *type, *toggle;
    int off, hidden, state, submenu;
    int nkids;
    const struct E *kids;
} E;

static void put_entry(DBuf *b, const E *e) {
    dw_struct(b);
    dw_i32(b, e->id);
    size_t a = dw_arr_begin(b, 8);
    if (e->label) { dw_struct(b); dw_str(b, "label"); dw_variant(b, "s"); dw_str(b, e->label); }
    if (e->type) { dw_struct(b); dw_str(b, "type"); dw_variant(b, "s"); dw_str(b, e->type); }
    if (e->toggle) {
        dw_struct(b); dw_str(b, "toggle-type"); dw_variant(b, "s"); dw_str(b, e->toggle);
        dw_struct(b); dw_str(b, "toggle-state"); dw_variant(b, "i"); dw_i32(b, e->state);
    }
    if (e->off) { dw_struct(b); dw_str(b, "enabled"); dw_variant(b, "b"); dw_bool(b, 0); }
    if (e->hidden) { dw_struct(b); dw_str(b, "visible"); dw_variant(b, "b"); dw_bool(b, 0); }
    if (e->submenu) { dw_struct(b); dw_str(b, "children-display"); dw_variant(b, "s"); dw_str(b, "submenu"); }
    /* what we don't read: an icon's bytes, a shortcut, a label of another type */
    dw_struct(b); dw_str(b, "icon-data"); dw_variant(b, "ay"); dw_bytes(b, (const uint8_t *)"\x89PNG", 4);
    dw_struct(b); dw_str(b, "shortcut"); dw_variant(b, "aas"); { size_t o = dw_arr_begin(b, 4); size_t i = dw_arr_begin(b, 4); dw_str(b, "Control"); dw_str(b, "q"); dw_arr_end(b, i); dw_arr_end(b, o); }
    dw_arr_end(b, a);
    size_t c = dw_arr_begin(b, 1);
    for (int i = 0; i < e->nkids; i++) { dw_variant(b, "(ia{sv}av)"); put_entry(b, &e->kids[i]); }
    if (e->nkids) { dw_variant(b, "s"); dw_str(b, "not an entry"); }
    dw_arr_end(b, c);
}

/* GetLayout's answer for that root; the JSON (malloc) or NULL if sni_menu_json refused it */
static char *layout_keys(const E *root, int32_t *ids, uint32_t *keys, int max, int *nids) {
    DBuf b;
    dw_init(&b);
    dw_u32(&b, 7);
    put_entry(&b, root);
    DMsg m;
    char *j = NULL;
    if (!message("u(ia{sv}av)", &b, &m)) {
        DRd r;
        dr_init(&r, &m);
        dr_u32(&r);
        HBuf h = { 0 };
        if (!sni_menu_json(&r, &h, ids, keys, max, nids) && r.pos == r.len) j = hb_take(&h);
        else free(hb_take(&h));
        dbus_msg_free(&m);
    } else printf("(layout not built)\n");
    dw_free(&b);
    return j;
}

static char *layout(const E *root, int32_t *ids, int max, int *nids) { return layout_keys(root, ids, NULL, max, nids); }

/* a chain of submenus, depth levels below the root */
static E *chain(int depth) {
    E *e = calloc((size_t)depth + 1, sizeof *e);
    for (int i = 0; i <= depth; i++) {
        e[i].id = i;
        e[i].label = "level";
        if (i < depth) { e[i].nkids = 1; e[i].kids = &e[i + 1]; }
    }
    return e;
}

static void t_menu(void) {
    E sub[] = { { .id = 11, .label = "_Pause" }, { .id = 12, .label = "Resume", .off = 1 } };
    E kids[] = {
        { .id = 1, .label = "_Open Sync" },
        { .id = 2, .type = "separator" },
        { .id = 3, .label = "Sync", .submenu = 1, .nkids = 2, .kids = sub },
        { .id = 4, .label = "Notifications", .toggle = "checkmark", .state = 1 },
        { .id = 5, .label = "Secret", .hidden = 1 },
        { .id = 6, .label = "Bad\xe2\x80\xaeLabel" },
        { .id = 7, .label = "Empty submenu", .submenu = 1 },
    };
    E root = { .id = 0, .nkids = 7, .kids = kids };
    int32_t ids[16];
    int n = 0;
    char *j = layout(&root, ids, 16, &n);
    const char *want = "[{\"id\":1,\"label\":\"Open Sync\",\"enabled\":true},{\"sep\":true},"
                       "{\"id\":3,\"label\":\"Sync\",\"enabled\":true,\"sub\":[{\"id\":11,\"label\":\"Pause\",\"enabled\":true},"
                       "{\"id\":12,\"label\":\"Resume\",\"enabled\":false}]},"
                       "{\"id\":4,\"label\":\"Notifications\",\"enabled\":true,\"toggle\":\"checkmark\",\"on\":true},"
                       "{\"id\":6,\"label\":\"BadLabel\",\"enabled\":true},"
                       "{\"id\":7,\"label\":\"Empty submenu\",\"enabled\":true,\"sub\":[]}]";
    check(j && !strcmp(j, want), "layout as JSON");
    if (j && strcmp(j, want)) printf("  got  %s\n  want %s\n", j, want);
    free(j);
    /* clickable: 1, 11, 4, 6 (not the separator, the submenus, the disabled or the hidden one) */
    check(n == 4 && ids[0] == 1 && ids[1] == 11 && ids[2] == 4 && ids[3] == 6, "the ids that can be clicked");
    j = layout(&root, ids, 2, &n);
    check(j && n == 2, "ids bounded by max");
    free(j);

    /* depth: SNI_MENU_DEPTH levels kept; the last one's submenu is cut and shown off */
    E *deep = chain(SNI_MENU_DEPTH + 3);
    j = layout(deep, ids, 16, &n);
    int levels = 0;
    for (const char *p = j; p && (p = strstr(p, "\"sub\":[")); p++) levels++;
    check(j && levels == SNI_MENU_DEPTH - 1, "submenus cut at SNI_MENU_DEPTH");
    check(j && strstr(j, "\"enabled\":false") && n == 0, "the entry at the cut is shown off and not clickable");
    free(j);
    free(deep);

    /* item cap: SNI_MENU_ITEMS kept, the rest read past */
    int many = SNI_MENU_ITEMS + 40;
    E *lots = calloc((size_t)many, sizeof *lots);
    for (int i = 0; i < many; i++) { lots[i].id = i + 1; lots[i].label = "x"; }
    E big = { .id = 0, .nkids = many, .kids = lots };
    int32_t all[SNI_MENU_ITEMS + 64];
    j = layout(&big, all, SNI_MENU_ITEMS + 64, &n);
    char last[32], past[32];
    snprintf(last, sizeof last, "{\"id\":%d,", SNI_MENU_ITEMS);
    snprintf(past, sizeof past, "{\"id\":%d,", SNI_MENU_ITEMS + 1);
    check(j && strstr(j, last) && !strstr(j, past) && n == SNI_MENU_ITEMS, "SNI_MENU_ITEMS entries kept, the reader at the end");
    free(j);
    free(lots);

    /* a root with no children */
    E empty = { .id = 0 };
    j = layout(&empty, ids, 16, &n);
    check(j && !strcmp(j, "[]") && n == 0, "an empty menu");
    free(j);
}

/* ---------- a menu built again with new ids (fyne.io/systray: every change is ResetMenu and AddMenuItem) ---------- */

static void t_rebuilt(void) {
    /* go-link Desk's menu, twice: the same entries, new ids each time */
    E sub1[] = { { .id = 120, .label = "Pause" }, { .id = 121, .label = "Resume" } };
    E a[] = {
        { .id = 101, .label = "Code: 123 456 789", .off = 1 }, { .id = 102, .label = "Disconnect", .off = 1 },
        { .id = 103, .type = "separator" }, { .id = 104, .label = "Open go-link Desk" }, { .id = 105, .label = "Settings..." },
        { .id = 106, .label = "Sync", .nkids = 2, .kids = sub1 }, { .id = 107, .label = "About" }, { .id = 108, .type = "separator" },
        { .id = 109, .label = "Quit" },
    };
    E sub2[] = { { .id = 220, .label = "Pause" }, { .id = 221, .label = "Resume" } };
    E b[] = {
        { .id = 201, .label = "Code: 987 654 321", .off = 1 }, { .id = 202, .label = "Disconnect", .off = 1 },
        { .id = 203, .type = "separator" }, { .id = 204, .label = "Open go-link Desk" }, { .id = 205, .label = "Settings..." },
        { .id = 206, .label = "Sync", .nkids = 2, .kids = sub2 }, { .id = 207, .label = "About" }, { .id = 208, .type = "separator" },
        { .id = 209, .label = "Quit" },
    };
    E ra = { .id = 0, .nkids = 9, .kids = a }, rb = { .id = 0, .nkids = 9, .kids = b };
    int32_t ia[16], ib[16];
    uint32_t ka[16], kb[16];
    int na = 0, nb = 0;
    free(layout_keys(&ra, ia, ka, 16, &na));
    free(layout_keys(&rb, ib, kb, 16, &nb));
    check(na == 6 && nb == 6, "rebuilt: the same clickable entries");
    uint32_t k = 0;
    check(!sni_menu_key(ia, ka, na, 107, &k), "rebuilt: the key of About in the old menu");
    check(sni_menu_find(ib, kb, nb, k) == 207, "rebuilt: About's id in the new menu");
    check(!sni_menu_key(ia, ka, na, 109, &k) && sni_menu_find(ib, kb, nb, k) == 209, "rebuilt: Quit");
    check(!sni_menu_key(ia, ka, na, 121, &k) && sni_menu_find(ib, kb, nb, k) == 221, "rebuilt: an entry of a submenu");
    check(!sni_menu_key(ia, ka, na, 120, &k) && sni_menu_find(ib, kb, nb, k) != 221, "rebuilt: Pause is not Resume");
    check(sni_menu_key(ia, ka, na, 101, &k), "rebuilt: a disabled entry has no key");
    check(sni_menu_key(ia, ka, na, 999, &k), "rebuilt: an id never shown");
    /* a changed label, or a new place, is another entry */
    b[6].label = "About go-link Desk";
    free(layout_keys(&rb, ib, kb, 16, &nb));
    check(!sni_menu_key(ia, ka, na, 107, &k) && sni_menu_find(ib, kb, nb, k) < 0, "rebuilt: a new label is not the old entry");
    E c[] = { { .id = 301, .label = "Quit" }, { .id = 302, .label = "Quit" } };
    E rc = { .id = 0, .nkids = 2, .kids = c };
    int32_t ic[4];
    uint32_t kc[4];
    int nc = 0;
    free(layout_keys(&rc, ic, kc, 4, &nc));
    check(nc == 2 && kc[0] != kc[1], "rebuilt: two alike labels at two places differ");
    uint32_t same[2] = { 5, 5 };
    int32_t two[2] = { 1, 2 };
    check(sni_menu_find(two, same, 2, 5) == -1, "rebuilt: two entries with one key: neither");
}

/* ---------- the Event a chosen entry sends ---------- */

static void t_event(void) {
    DBuf body;
    dw_init(&body);
    sni_event_body(&body, 7, 1234);
    DHdr h = { .type = DBUS_CALL, .serial = 3, .flags = DBUS_NO_REPLY, .path = "/StatusNotifierItem/menu",
               .iface = "com.canonical.dbusmenu", .member = "Event", .dest = ":1.42", .sig = SNI_EVENT_SIG };
    DBuf out;
    int built = dbus_build(&h, &body, &out);
    dw_free(&body);
    check(!built, "event: the call builds");
    if (built) { dw_free(&out); return; }
    DMsg m;
    int parsed = dbus_msg_parse(out.p, out.n, &m);
    dw_free(&out);
    check(!parsed, "event: a valid message");
    if (parsed) return;
    check(m.sig && !strcmp(m.sig, "isvu"), "event: signature isvu");
    check(m.member && !strcmp(m.member, "Event") && m.path && !strcmp(m.path, "/StatusNotifierItem/menu"), "event: member and path");
    DRd r;
    dr_init(&r, &m);
    int32_t id = dr_i32(&r);
    const char *ev = dr_str(&r, NULL);
    const char *vs = dr_variant(&r);
    int32_t data = vs && !strcmp(vs, "i") ? dr_i32(&r) : -1;
    uint32_t ts = dr_u32(&r);
    check(!r.err, "event: the body reads");
    check(id == 7, "event: the entry's id");
    check(ev && !strcmp(ev, "clicked"), "event: \"clicked\"");
    check(vs && !strcmp(vs, "i") && data == 0, "event: data is <int32 0>");
    check(ts == 1234, "event: the timestamp");
    dbus_msg_free(&m);
}

int main(void) {
    t_service();
    t_pixmap();
    t_text();
    t_menu();
    t_rebuilt();
    t_event();
    printf("sni-test: %d/%d ok\n", total - fails, total);
    return fails ? 1 : 0;
}
