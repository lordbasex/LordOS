/* xwm_test.c — the X11 wire helpers of the window manager (src/wl/x11wire.c): request encoding,
 * the setup reply, packet framing, sequence numbers and the ICCCM/EWMH/Motif properties. No X
 * server needed; the window manager itself is tested on a VM (docs/WAYLAND.md, "X11 apps"). */
#include "../src/wl/x11wire.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static void test_requests(void) {
    XOut o = { 0 };
    xo_begin(&o, X_InternAtom, 0);
    xo_u16(&o, 5); xo_u16(&o, 0);
    xo_bytes(&o, "WM_S0", 5);
    xo_end(&o);
    CHECK(o.n == 16);                                       /* 4 header + 4 + "WM_S0" padded to 8 */
    CHECK(o.b[0] == X_InternAtom && x11_get16(o.b + 2) == 4);
    CHECK(!memcmp(o.b + 8, "WM_S0\0\0\0", 8));
    size_t first = o.n;
    xo_begin(&o, X_MapWindow, 0);
    xo_u32(&o, 0x400001);
    xo_end(&o);
    CHECK(o.n == first + 8 && x11_get16(o.b + first + 2) == 2 && x11_get32(o.b + first + 4) == 0x400001);
    xo_drop(&o, first);
    CHECK(o.n == 8 && o.b[0] == X_MapWindow);
    xo_free(&o);
    uint8_t setup[12];
    x11_setup_request(setup);
    CHECK(setup[0] == 'l' && x11_get16(setup + 2) == 11 && x11_get16(setup + 4) == 0 && x11_get16(setup + 6) == 0);
}

static void test_setup(void) {
    uint8_t r[200] = { 0 };
    const char vendor[] = "The X.Org Foundation";      /* 20 bytes */
    size_t vlen = 20, nformats = 2;
    size_t data = 32 + vlen + nformats * 8 + 40 + 8 + 24;  /* one screen with one depth and one visual */
    r[0] = 1;
    put16(r + 2, 11);
    put16(r + 6, (uint16_t)(data / 4));
    uint8_t *d = r + 8;
    put32(d + 4, 0x200000);
    put32(d + 8, 0x1fffff);
    put16(d + 16, (uint16_t)vlen);
    put16(d + 18, 65535);
    d[20] = 1; d[21] = (uint8_t)nformats; d[26] = 8; d[27] = 255;
    memcpy(d + 32, vendor, vlen);
    uint8_t *scr = d + 32 + vlen + nformats * 8;
    put32(scr, 0x221);
    put16(scr + 20, 2560); put16(scr + 22, 1440);
    put32(scr + 32, 0x21);
    scr[38] = 24;
    XSetup s;
    size_t used = 0;
    CHECK(x11_parse_setup(r, 8, &s, &used) == 0);          /* not complete */
    CHECK(x11_parse_setup(r, 8 + data, &s, &used) == 1);
    CHECK(used == 8 + data);
    CHECK(s.root == 0x221 && s.root_visual == 0x21 && s.root_depth == 24);
    CHECK(s.width == 2560 && s.height == 1440);
    CHECK(s.id_base == 0x200000 && s.id_mask == 0x1fffff);
    CHECK(s.max_req_bytes == 65535u * 4 && s.min_keycode == 8 && s.max_keycode == 255);
    r[0] = 0;                                               /* refused */
    CHECK(x11_parse_setup(r, 8 + data, &s, &used) == -1);
}

static void test_framing(void) {
    uint8_t p[64] = { 0 };
    p[0] = X_MapRequest;
    CHECK(x11_packet_len(p, 31) == 0);
    CHECK(x11_packet_len(p, 32) == 32);
    p[0] = X_Reply;
    put32(p + 4, 2);                                        /* 8 more bytes */
    CHECK(x11_packet_len(p, 32) == 0);
    CHECK(x11_packet_len(p, 40) == 40);
    p[0] = X_GenericEvent | 0x80;
    CHECK(x11_packet_len(p, 40) == 40);
    p[0] = X_Error;
    CHECK(x11_packet_len(p, 64) == 32);
    CHECK(x11_seq_widen(5, 5) == 5);
    CHECK(x11_seq_widen(0x10003, 0xfffe) == 0xfffe);       /* sent before the wrap */
    CHECK(x11_seq_widen(0x20010, 0x0005) == 0x20005);
}

static void test_props(void) {
    char inst[32], cls[32];
    const uint8_t wc[] = "xterm\0XTerm";
    x11_wm_class(wc, sizeof wc, inst, sizeof inst, cls, sizeof cls);
    CHECK(!strcmp(inst, "xterm") && !strcmp(cls, "XTerm"));
    const uint8_t only[] = "neverball";
    x11_wm_class(only, 9, inst, sizeof inst, cls, sizeof cls);
    CHECK(!strcmp(inst, "neverball") && !cls[0]);
    const uint8_t evil[] = "a\x1b[2Jb\0C\x7fz";             /* control bytes are dropped */
    x11_wm_class(evil, sizeof evil, inst, sizeof inst, cls, sizeof cls);
    CHECK(!strcmp(inst, "a[2Jb") && !strcmp(cls, "Cz"));

    char t[64];
    x11_title((const uint8_t *)"Caf\xc3\xa9 \xe2\x80\x94 ok", 13, 0, t, sizeof t);
    CHECK(!strcmp(t, "Caf\xc3\xa9 \xe2\x80\x94 ok"));
    x11_title((const uint8_t *)"Caf\xe9", 4, 1, t, sizeof t);  /* Latin-1 STRING */
    CHECK(!strcmp(t, "Caf\xc3\xa9"));
    x11_title((const uint8_t *)"a\xff" "b\xc0\xaf" "c", 6, 0, t, sizeof t);   /* invalid and overlong */
    CHECK(!strcmp(t, "a\xef\xbf\xbd" "b\xef\xbf\xbd\xef\xbf\xbd" "c"));
    x11_title((const uint8_t *)"line\nbreak\ttab", 14, 0, t, sizeof t);
    CHECK(!strcmp(t, "linebreaktab"));
    x11_title((const uint8_t *)"\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac", 9, 0, t, 7);   /* never a cut character */
    CHECK(!strcmp(t, "\xe2\x82\xac\xe2\x82\xac"));

    uint32_t motif_off[5] = { 2, 0, 0, 0, 0 }, motif_on[5] = { 2, 0, 1, 0, 0 }, motif_none[5] = { 1, 0, 0, 0, 0 };
    CHECK(x11_motif_decorated(motif_off, 5) == 0);
    CHECK(x11_motif_decorated(motif_on, 5) == 1);
    CHECK(x11_motif_decorated(motif_none, 5) == -1);
    CHECK(x11_motif_decorated(NULL, 0) == -1);

    uint32_t hints[18] = { 16 | 32, 0, 0, 0, 0, 200, 100, 800, 600 };
    XSizeHints h;
    x11_size_hints(hints, 18, &h);
    CHECK(h.min_w == 200 && h.min_h == 100 && h.max_w == 800 && h.max_h == 600);
    hints[5] = 0xffffffffu;                                 /* nonsense is ignored */
    x11_size_hints(hints, 18, &h);
    CHECK(h.min_w == 0);
    x11_size_hints(hints, 3, &h);
    CHECK(h.min_w == 0 && h.max_w == 0);

    uint32_t wmh_no[2] = { 1, 0 }, wmh_yes[2] = { 1, 1 }, wmh_unset[2] = { 0, 0 };
    CHECK(x11_wm_hints_input(wmh_no, 2) == 0);
    CHECK(x11_wm_hints_input(wmh_yes, 2) == 1);
    CHECK(x11_wm_hints_input(wmh_unset, 2) == 1);
    CHECK(x11_wm_hints_input(NULL, 0) == 1);
}

static void test_policy(void) {
    CHECK(x11_moveresize_edges(0) == (1 | 4));              /* top-left */
    CHECK(x11_moveresize_edges(3) == 8);                    /* right */
    CHECK(x11_moveresize_edges(4) == (2 | 8));              /* bottom-right */
    CHECK(x11_moveresize_edges(8) == -1);                   /* move */
    CHECK(x11_moveresize_edges(9) == 0 && x11_moveresize_edges(11) == 0 && x11_moveresize_edges(99) == 0);
    CHECK(x11_text_rank("UTF8_STRING") > x11_text_rank("text/plain;charset=utf-8"));
    CHECK(x11_text_rank("STRING") > x11_text_rank("text/plain"));
    CHECK(x11_text_target("TEXT") && !x11_text_target("image/png") && !x11_text_target(NULL));
    /* a paste request is answered only for the focused window's client */
    uint32_t mask = 0x1fffff;
    CHECK(x11_same_client(0x400001, 0x40000c, mask));
    CHECK(!x11_same_client(0x400001, 0x600001, mask));
    CHECK(!x11_same_client(0x200001, 0x400001, mask));
}

static int taken_set[64];
static int taken(int n, void *ctx) { (void)ctx; return taken_set[n]; }
static void test_display(void) {
    CHECK(x11_pick_display(taken, NULL) == 0);
    taken_set[0] = taken_set[1] = 1;
    CHECK(x11_pick_display(taken, NULL) == 2);
    for (int i = 0; i < 64; i++) taken_set[i] = 1;
    CHECK(x11_pick_display(taken, NULL) == -1);
}

static int sandboxed_pid;
static int fake_sandboxed(int pid) { return pid == sandboxed_pid; }

static void test_xdnd(void) {
    uint32_t d[5];
    /* versions: ours is 5, an older target's is used, one older than 3 is not spoken */
    CHECK(x11_xdnd_version(5) == 5 && x11_xdnd_version(9) == 5 && x11_xdnd_version(4) == 4 && x11_xdnd_version(3) == 3);
    CHECK(x11_xdnd_version(2) == 0 && x11_xdnd_version(0) == 0);
    /* XdndEnter: up to three types in the message, more in XdndTypeList */
    uint32_t types[5] = { 101, 102, 103, 104, 105 };
    x11_xdnd_enter(d, 0x200001, 5, types, 2);
    CHECK(d[0] == 0x200001 && d[1] == (5u << 24) && d[2] == 101 && d[3] == 102 && d[4] == 0);
    x11_xdnd_enter(d, 0x200001, 5, types, 5);
    CHECK(d[1] == (5u << 24 | 1) && d[2] == 101 && d[3] == 102 && d[4] == 103);
    XdndEnter e;
    CHECK(x11_xdnd_parse_enter(d, &e) && e.win == 0x200001 && e.version == 5 && e.more && e.n == 3 && e.types[2] == 103);
    uint32_t holes[5] = { 0x400002, 4u << 24, 0, 77, 0 };
    CHECK(x11_xdnd_parse_enter(holes, &e) && e.version == 4 && !e.more && e.n == 1 && e.types[0] == 77);
    uint32_t old[5] = { 0x400002, 2u << 24, 77, 0, 0 }, nowin[5] = { 0, 5u << 24, 77, 0, 0 }, future[5] = { 0x400002, 200u << 24, 0, 0, 0 };
    CHECK(!x11_xdnd_parse_enter(old, &e) && !x11_xdnd_parse_enter(nowin, &e));
    CHECK(x11_xdnd_parse_enter(future, &e) && e.version == 5 && e.n == 0);
    /* XdndPosition: root coordinates packed, clamped */
    x11_xdnd_position(d, 0x200001, 640, 360, 1234, 55);
    CHECK(d[0] == 0x200001 && d[1] == 0 && d[2] == (640u << 16 | 360) && d[3] == 1234 && d[4] == 55);
    int x, y;
    x11_xdnd_xy(d[2], &x, &y);
    CHECK(x == 640 && y == 360);
    x11_xdnd_position(d, 1, -5, 99999, 0, 0);
    x11_xdnd_xy(d[2], &x, &y);
    CHECK(x == 0 && y == 32767);
    /* XdndStatus both ways: an empty rectangle (every motion is reported), no action when refused */
    x11_xdnd_status(d, 0x600003, 1, 55);
    CHECK(d[0] == 0x600003 && d[1] == 1 && d[2] == 0 && d[3] == 0 && d[4] == 55);
    x11_xdnd_status(d, 0x600003, 0, 55);
    CHECK(d[1] == 0 && d[4] == 0);
    uint32_t st[5] = { 0x600003, 3, 0x00100010, 0x00200020, 56 };
    XdndStatus s;
    x11_xdnd_parse_status(st, &s);
    CHECK(s.win == 0x600003 && s.accept && s.action == 56);
    st[1] = 2;
    x11_xdnd_parse_status(st, &s);
    CHECK(!s.accept && s.action == 0);
    x11_xdnd_finished(d, 0x600003, 1, 55);
    CHECK(d[0] == 0x600003 && d[1] == 1 && d[2] == 55 && d[3] == 0);
    x11_xdnd_finished(d, 0x600003, 0, 55);
    CHECK(d[1] == 0 && d[2] == 0);
    /* XdndTypeList: ATOM, format 32, bounded by the bytes and the room, zeros skipped */
    uint8_t list[40];
    for (int i = 0; i < 10; i++) put32(list + 4 * i, i == 3 ? 0 : 200u + (uint32_t)i);
    uint32_t out[XDND_MAX_TYPES];
    CHECK(x11_atom_list(list, 40, 32, 4, out, XDND_MAX_TYPES) == 9 && out[0] == 200 && out[3] == 204);
    CHECK(x11_atom_list(list, 40, 32, 4, out, 4) == 4);
    CHECK(x11_atom_list(list, 6, 32, 4, out, XDND_MAX_TYPES) == 1);         /* a cut-off atom is not read */
    CHECK(x11_atom_list(list, 40, 8, 4, out, XDND_MAX_TYPES) == 0);          /* wrong format */
    CHECK(x11_atom_list(list, 40, 32, 31, out, XDND_MAX_TYPES) == 0);        /* STRING, not ATOM */
    CHECK(x11_atom_list(NULL, 40, 32, 4, out, XDND_MAX_TYPES) == 0);
    /* type names → Wayland MIME types */
    char m[256];
    CHECK(x11_dnd_mime((const uint8_t *)"text/uri-list", 13, m, sizeof m) && !strcmp(m, "text/uri-list"));
    CHECK(x11_dnd_mime((const uint8_t *)"UTF8_STRING", 11, m, sizeof m) && !strcmp(m, "text/plain;charset=utf-8"));
    CHECK(!x11_dnd_mime((const uint8_t *)"TARGETS", 7, m, sizeof m) && !m[0]);
    CHECK(!x11_dnd_mime((const uint8_t *)"STRING", 6, m, sizeof m));
    CHECK(!x11_dnd_mime((const uint8_t *)"text/pl\nain", 11, m, sizeof m));   /* a control character */
    CHECK(!x11_dnd_mime((const uint8_t *)"text/a b", 8, m, sizeof m));
    CHECK(!x11_dnd_mime((const uint8_t *)"/etc/passwd", 11, m, sizeof m));
    CHECK(!x11_dnd_mime((const uint8_t *)"image/png", 9, m, 9));               /* no room for its NUL */
    CHECK(x11_dnd_mime((const uint8_t *)"image/png", 9, m, 10) && !strcmp(m, "image/png"));
    uint8_t hi[] = { 't', '/', 0xc3, 0xa9 };
    CHECK(!x11_dnd_mime(hi, sizeof hi, m, sizeof m));
}

/* X-Resource QueryClientIds reply: n entries of { client, mask, length, value } */
static size_t xres_reply(uint8_t *r, const uint32_t (*ent)[4], int n, int count) {
    memset(r, 0, 32);
    r[0] = X_Reply;
    put32(r + 8, (uint32_t)count);
    size_t off = 32;
    for (int i = 0; i < n; i++) {
        put32(r + off, ent[i][0]); put32(r + off + 4, ent[i][1]); put32(r + off + 8, ent[i][2]);
        off += 12;
        if (ent[i][2] == 4) { put32(r + off, ent[i][3]); off += 4; }
    }
    put32(r + 4, (uint32_t)(off - 32) / 4);
    return off;
}

static void test_xres(void) {
    uint8_t r[512];
    uint32_t base[8], pid[8], mask = 0x1fffff;
    const uint32_t ents[3][4] = { { 0x200000, 2, 4, 1000 }, { 0x400000, 2, 4, 1001 }, { 0x600000, 2, 0, 0 } };
    size_t n = xres_reply(r, ents, 3, 3);
    CHECK(x11_xres_pids(r, n, base, pid, 8) == 2 && base[1] == 0x400000 && pid[1] == 1001);
    CHECK(x11_xres_pids(r, n, base, pid, 2) == -1);                         /* more clients than room: refused */
    CHECK(x11_xres_pids(r, n - 4, base, pid, 8) == 2);                      /* cut short: what is whole */
    CHECK(x11_xres_pids(r, 40, base, pid, 8) == 0);
    const uint32_t bad[1][4] = { { 0x200000, 2, 0x7ffffff0u, 0 } };         /* a length past the reply */
    n = xres_reply(r, bad, 1, 1);
    CHECK(x11_xres_pids(r, n, base, pid, 8) == 0);
    CHECK(x11_xres_pids(NULL, 0, base, pid, 8) == 0);
    /* the owner's client has a pid, and no client of the display is sandboxed */
    uint32_t b[3] = { 0x200000, 0x400000, 0x600000 }, p[3] = { 1000, 1001, 1002 };
    sandboxed_pid = 0;
    CHECK(x11_xdnd_trusted(0x400005, b, p, 3, mask, fake_sandboxed));
    CHECK(!x11_xdnd_trusted(0x800005, b, p, 3, mask, fake_sandboxed));      /* no pid known for it */
    CHECK(!x11_xdnd_trusted(0, b, p, 3, mask, fake_sandboxed));
    CHECK(!x11_xdnd_trusted(0x400005, b, p, -1, mask, fake_sandboxed));
    sandboxed_pid = 1002;                                                   /* another client is sandboxed */
    CHECK(!x11_xdnd_trusted(0x400005, b, p, 3, mask, fake_sandboxed));
    sandboxed_pid = 1001;                                                   /* the owner is */
    CHECK(!x11_xdnd_trusted(0x400005, b, p, 3, mask, fake_sandboxed));
}

int main(void) {
    test_requests();
    test_setup();
    test_framing();
    test_props();
    test_policy();
    test_display();
    test_xdnd();
    test_xres();
    printf("xwm-test: %d/%d\n", checks - fails, checks);
    return fails ? 1 : 0;
}
