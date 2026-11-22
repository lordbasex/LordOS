/* portal_cast_test.c — the ScreenCast restore token, the Screenshot portal (Screenshot and PickColor) and the
 * FileChooser's lending to Flatpak apps (src/ui/portal.c; approved by the maintainer 2026-11-22) over a private session
 * bus: a second connection plays the app, stubs stand in for the shell (hw_emit), lord-screencast (cast_begin), the
 * pictures (portalshot.c) and the lending (fplend.c). Run by tests/run_suites.sh under dbus-run-session. */
#include "../src/ui/portal.h"
#include "../src/ui/webask.h"
#include "../src/vault/dbus.h"
#include "../src/fplend.h"
#include "../src/cast/castproto.h"
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, passes;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } else passes++; } while (0)

static char last_kind[64], last_json[8192];
static int emits_cast, emits_shot, emits_taken;
void hw_emit(const char *kind, const char *json) {
    snprintf(last_kind, sizeof last_kind, "%s", kind);
    snprintf(last_json, sizeof last_json, "%s", json);
    emits_cast += !strcmp(kind, "portal-cast");
    emits_shot += !strcmp(kind, "portal-shot");
    emits_taken += !strcmp(kind, "portal-shot-taken");
}
int wl_focus_pid(void) { return 0; }
int wl_foreign_surface(const char *h) { (void)h; return 0; }
static const char *fake_app = "flatpak:org.chromium.Chromium";
static int screen_blocked;
int appid_of_pid(unsigned pid, char *out, size_t n) { (void)pid; if (!fake_app) return 1; snprintf(out, n, "%s", fake_app); return 0; }
int appid_running(int (*match)(const char *app, void *u), void *u) { (void)match; (void)u; return 0; }
/* only to name an unknown caller in the indicator (sec/appid.h): nothing here */
int appid_of_lineage(unsigned pid, char *out, size_t n) { (void)pid; if (n) out[0] = 0; return -1; }
int appid_exe_name(unsigned pid, char *out, size_t n) { (void)pid; if (n) out[0] = 0; return -1; }
int restrict_blocks(const char *fwid, uint32_t flag) { (void)fwid; (void)flag; return screen_blocked; }
int cast_available(void) { return 1; }
static int begun, begun_n, begun_id;
static CastSource begun_src[4];
int cast_begin(int id, const CastSource *src, int n, int cursor_embedded) {
    (void)cursor_embedded;
    begun++;
    begun_id = id;
    begun_n = n;
    memcpy(begun_src, src, sizeof src[0] * (size_t)n);
    return 0;
}
void cast_end(int id) { (void)id; }
int cast_remote(int id, uint32_t req) { (void)id; (void)req; return -1; }
void cast_shown(int id) { (void)id; }
int cast_thumb(const CastSource *src, int max) { (void)src; (void)max; return 0; }
/* the pictures: recorded; the test says when the file is there */
static int captured;
static char captured_path[512];
static int captured_area;
int portal_shot_capture(int tag, const CastSource *src, const int *area, const char *path) {
    (void)tag; (void)src;
    captured++;
    captured_area = area != NULL;
    snprintf(captured_path, sizeof captured_path, "%s", path);
    return 0;
}
int portal_shot_pixel(const CastSource *src, int x, int y, double rgb[3]) { (void)src; rgb[0] = x / 100.0; rgb[1] = y / 100.0; rgb[2] = 1; return 0; }
/* lending: a Flatpak caller gets the lent path */
static int lent;
int fplend_lend(unsigned pid, const char *path, int kind, char *out, size_t n) {
    (void)pid;
    if (!fake_app || strncmp(fake_app, "flatpak:", 8)) return 1;
    lent++;
    const char *b = strrchr(path, '/');
    snprintf(out, n, "/run/flatpak/app/org.example.App/doc/%032d/%s", kind, b ? b + 1 : path);
    return 0;
}
void webask_attach(DConn *c) { (void)c; }
void webask_call(DConn *c, const DMsg *m) { (void)c; (void)m; }
int webask_reply(DConn *c, const DMsg *m) { (void)c; (void)m; return 0; }
void webask_gone(const char *name) { (void)name; }
void webask_appeared(const char *name) { (void)name; }
void webask_lost(void) {}

static DConn app;
typedef struct { char member[64], path[400], token[64], uri[512], session[400]; uint32_t code, persist; int has_color; double color[3]; } Got;
static Got got[64];
static int ngot;

static double get_double(DRd *r) { uint64_t u = dr_u64(r); double d; memcpy(&d, &u, 8); return d; }

static void pump(int ms) {
    for (int left = ms; left > 0; left -= 10) {
        struct pollfd p[2] = { { portal_fd(), POLLIN | (portal_wants_write() ? POLLOUT : 0), 0 }, { app.fd, POLLIN, 0 } };
        poll(p, 2, 10);
        portal_dispatch();
        DMsg m;
        while (dbus_next(&app, &m) == 1) {
            if (ngot < 64 && m.type == DBUS_SIGNAL && m.sig && !strcmp(m.sig, "ua{sv}")) {
                Got *g = &got[ngot++];
                memset(g, 0, sizeof *g);
                snprintf(g->member, sizeof g->member, "%s", m.member ? m.member : "");
                snprintf(g->path, sizeof g->path, "%s", m.path ? m.path : "");
                DRd r;
                dr_init(&r, &m);
                g->code = dr_u32(&r);
                size_t end = dr_arr(&r, 8);
                while (!r.err && dr_more(&r, end)) {
                    dr_struct(&r);
                    const char *k = dr_str(&r, NULL), *sig = dr_variant(&r);
                    if (!strcmp(k, "restore_token") && !strcmp(sig, "s")) snprintf(g->token, sizeof g->token, "%s", dr_str(&r, NULL));
                    else if (!strcmp(k, "persist_mode") && !strcmp(sig, "u")) g->persist = dr_u32(&r);
                    else if (!strcmp(k, "uri") && !strcmp(sig, "s")) snprintf(g->uri, sizeof g->uri, "%s", dr_str(&r, NULL));
                    else if (!strcmp(k, "session_handle") && !strcmp(sig, "s")) snprintf(g->session, sizeof g->session, "%s", dr_str(&r, NULL));
                    else if (!strcmp(k, "uris") && !strcmp(sig, "as")) {
                        size_t e2 = dr_arr(&r, 4);
                        while (!r.err && dr_more(&r, e2)) { const char *u = dr_str(&r, NULL); if (!g->uri[0]) snprintf(g->uri, sizeof g->uri, "%s", u); }
                    } else if (!strcmp(k, "color") && !strcmp(sig, "(ddd)")) {
                        dr_struct(&r);
                        for (int i = 0; i < 3; i++) g->color[i] = get_double(&r);
                        g->has_color = 1;
                    } else dr_skip(&r, sig);
                }
            }
            dbus_msg_free(&m);
        }
        dbus_flush(&app);
    }
}

static Got *last_response(void) {
    for (int i = ngot; i-- > 0;) if (!strcmp(got[i].member, "Response")) return &got[i];
    return NULL;
}

static void call(const char *iface, const char *member, const char *sig, DBuf *b, char *reply_path, char *err) {
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/portal/desktop", .iface = iface, .member = member,
               .dest = "org.freedesktop.portal.Desktop", .sig = sig };
    uint32_t s = dbus_send(&app, &h, b);
    dbus_flush(&app);
    if (reply_path) reply_path[0] = 0;
    if (err) err[0] = 0;
    for (int left = 2000; left > 0; left -= 10) {
        struct pollfd p[2] = { { portal_fd(), POLLIN | (portal_wants_write() ? POLLOUT : 0), 0 }, { app.fd, POLLIN, 0 } };
        poll(p, 2, 10);
        portal_dispatch();
        DMsg m;
        while (dbus_next(&app, &m) == 1) {
            if ((m.type == DBUS_RETURN || m.type == DBUS_ERROR) && m.reply_serial == s) {
                if (m.type == DBUS_ERROR) { if (err) snprintf(err, 128, "%s", m.error ? m.error : "?"); }
                else if (reply_path && m.sig && !strcmp(m.sig, "o")) { DRd r; dr_init(&r, &m); snprintf(reply_path, 400, "%s", dr_str(&r, NULL)); }
                dbus_msg_free(&m);
                pump(150);
                return;
            }
            dbus_msg_free(&m);
        }
    }
}

static void opt_s(DBuf *b, const char *k, const char *v) { dw_struct(b); dw_str(b, k); dw_variant(b, "s"); dw_str(b, v); }
static void opt_u(DBuf *b, const char *k, uint32_t v) { dw_struct(b); dw_str(b, k); dw_variant(b, "u"); dw_u32(b, v); }

static void add_match(void) {
    DBuf b; dw_init(&b);
    dw_str(&b, "type='signal',interface='org.freedesktop.portal.Request'");
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus", .member = "AddMatch", .dest = "org.freedesktop.DBus", .sig = "s" };
    dbus_send(&app, &h, &b); dw_free(&b);
    dbus_flush(&app);
}

static int json_id(void) { const char *p = strstr(last_json, "\"id\":"); return p ? atoi(p + 5) : 0; }

/* CreateSession + SelectSources (monitor, persist 2, the restore token if any) + Start: the session's path */
static void cast_start(const char *stoken, const char *restore, char *session) {
    char req[400], err[128];
    DBuf b; dw_init(&b);
    size_t a = dw_arr_begin(&b, 8);
    opt_s(&b, "handle_token", "c1");
    opt_s(&b, "session_handle_token", stoken);
    dw_arr_end(&b, a);
    ngot = 0;
    call("org.freedesktop.portal.ScreenCast", "CreateSession", "a{sv}", &b, req, err);
    dw_free(&b);
    Got *g = last_response();
    snprintf(session, 400, "%s", g ? g->session : "");
    dw_init(&b);
    dw_str(&b, session);
    a = dw_arr_begin(&b, 8);
    opt_s(&b, "handle_token", "c2");
    opt_u(&b, "types", 1);
    opt_u(&b, "persist_mode", 2);
    if (restore) opt_s(&b, "restore_token", restore);
    dw_arr_end(&b, a);
    call("org.freedesktop.portal.ScreenCast", "SelectSources", "oa{sv}", &b, req, err);
    dw_free(&b);
    dw_init(&b);
    dw_str(&b, session);
    dw_str(&b, "");
    a = dw_arr_begin(&b, 8);
    opt_s(&b, "handle_token", "c3");
    dw_arr_end(&b, a);
    ngot = 0;
    call("org.freedesktop.portal.ScreenCast", "Start", "osa{sv}", &b, req, err);
    dw_free(&b);
    pump(200);
}

static void session_close(const char *session) {
    DHdr h = { .type = DBUS_CALL, .path = session, .iface = "org.freedesktop.portal.Session", .member = "Close",
               .dest = "org.freedesktop.portal.Desktop", .sig = NULL };
    dbus_send(&app, &h, NULL);
    dbus_flush(&app);
    pump(150);
}

/* a share the person picks (the monitor), started: its session id, and the restore token in Start's answer */
static int shared(const char *stoken, char *session, char *token) {
    cast_start(stoken, NULL, session);
    int id = json_id();
    portal_cast_chosen(id, &(CastSource){ .kind = CAST_SRC_MONITOR, .display = 51 }, 1);
    ngot = 0;
    portal_cast_streams(id, &(CastStreamInfo){ .node = 40, .w = 1440, .h = 900, .kind = CAST_SRC_MONITOR }, 1);
    pump(100);
    Got *g = last_response();
    snprintf(token, 64, "%s", g ? g->token : "");
    return id;
}

/* a session with token: 1 when it went to the chooser (then cancelled), 0 when the token started it (then closed) */
static int asks_again(const char *stoken, const char *token) {
    char s[400];
    emits_cast = 0;
    int was = begun;
    cast_start(stoken, token, s);
    int chooser = emits_cast == 1 && begun == was;
    if (chooser) portal_cast_chosen(json_id(), NULL, 0);
    session_close(s);
    return chooser;
}

static void shot_call(const char *member) {
    char req[400], err[128];
    DBuf b; dw_init(&b);
    dw_str(&b, "");
    size_t a = dw_arr_begin(&b, 8);
    opt_s(&b, "handle_token", "s1");
    dw_arr_end(&b, a);
    ngot = 0;
    call("org.freedesktop.portal.Screenshot", member, "sa{sv}", &b, req, err);
    dw_free(&b);
    pump(150);
}

int main(void) {
    char home[] = "/tmp/portal-cast-XXXXXX";
    if (!mkdtemp(home)) return 2;
    setenv("HOME", home, 1);
    CHECK(portal_start() == 0);
    int fd = dbus_connect_session();
    CHECK(fd >= 0 && dbus_open(&app, fd, 2000) == 0);
    add_match();
    CastSource mon = { .kind = CAST_SRC_MONITOR, .display = 51 };
    CastStreamInfo info = { .node = 40, .w = 1440, .h = 900, .kind = CAST_SRC_MONITOR };

    /* 1. a verified app's first share: the chooser, then a restore token in Start's answer */
    char s1[400], s2[400], s3[400], s4[400];
    emits_cast = 0;
    cast_start("a1", NULL, s1);
    CHECK(emits_cast == 1);                                     /* the chooser went up */
    int id = json_id();
    portal_cast_chosen(id, &mon, 1);
    CHECK(begun == 1);
    ngot = 0;
    portal_cast_streams(id, &info, 1);
    pump(100);
    Got *g = last_response();
    CHECK(g && g->code == 0 && strlen(g->token) == 32 && g->persist == 1);
    char token[64];
    snprintf(token, sizeof token, "%s", g ? g->token : "");
    session_close(s1);

    /* 2. its next session with that token: no chooser, the same source, and no new token */
    emits_cast = 0;
    cast_start("a2", token, s2);
    CHECK(emits_cast == 0);
    CHECK(begun == 2 && begun_n == 1 && begun_src[0].kind == CAST_SRC_MONITOR && begun_src[0].display == 51);
    ngot = 0;
    portal_cast_streams(begun_id, &info, 1);
    pump(100);
    g = last_response();
    CHECK(g && g->code == 0 && !g->token[0]);                   /* a restored start gives none */
    session_close(s2);

    /* 3. the same token again: spent, so the chooser */
    emits_cast = 0;
    cast_start("a3", token, s3);
    CHECK(emits_cast == 1);
    portal_cast_chosen(json_id(), NULL, 0);
    session_close(s3);

    /* 4. another app with a token of the first one's: the chooser (a token is its app's alone) */
    emits_cast = 0;
    cast_start("a4", NULL, s4);
    id = json_id();
    portal_cast_chosen(id, &mon, 1);
    ngot = 0;
    portal_cast_streams(id, &info, 1);
    pump(100);
    g = last_response();
    snprintf(token, sizeof token, "%s", g ? g->token : "");
    session_close(s4);
    fake_app = "flatpak:org.evil.App";
    emits_cast = 0;
    cast_start("a5", token, s4);
    CHECK(emits_cast == 1);
    portal_cast_chosen(json_id(), NULL, 0);
    session_close(s4);
    /* 5. an unknown program never gets one */
    fake_app = NULL;
    cast_start("a6", NULL, s4);
    id = json_id();
    portal_cast_chosen(id, &mon, 1);
    ngot = 0;
    portal_cast_streams(id, &info, 1);
    pump(100);
    g = last_response();
    CHECK(g && g->code == 0 && !g->token[0]);
    session_close(s4);

    /* 5b. revoked (Claude Security F6/F7, 2026-11-22): the person's Stop, every share ending (cast_end_all: a lock, a
     * user switch, the shell going) and a locked session each kill a token before it is spent */
    fake_app = "flatpak:org.chromium.Chromium";
    char t5[64];
    id = shared("r1", s4, t5);
    CHECK(strlen(t5) == 32);
    portal_cast_stop(id);                                       /* the indicator's Stop */
    pump(100);
    CHECK(asks_again("r2", t5));                                /* dead: the chooser again */
    shared("r3", s4, t5);
    CHECK(strlen(t5) == 32);
    session_close(s4);                                          /* the app's own Close keeps it (OBS restarts)... */
    portal_cast_tokens_drop();                                  /* ...but cast_end_all revokes every one */
    CHECK(asks_again("r4", t5));
    shared("r5", s4, t5);
    CHECK(strlen(t5) == 32);
    session_close(s4);
    portal_cast_away(1);                                        /* the screen locks */
    CHECK(asks_again("r6", t5));                                /* refused while locked, and gone */
    portal_cast_away(0);                                        /* the person is back */
    CHECK(asks_again("r7", t5));                                /* still gone after the unlock */
    shared("r8", s4, t5);
    session_close(s4);
    CHECK(!asks_again("r9", t5));                               /* a new token works again once unlocked */

    /* 6. Screenshot: the chooser (always interactive), the file lent to a Flatpak app, the indicator */
    fake_app = "flatpak:org.flameshot.Flameshot";
    emits_shot = emits_taken = captured = 0;
    shot_call("Screenshot");
    CHECK(emits_shot == 1 && strstr(last_json, "\"shot\":true") && strstr(last_json, "\"color\":false") && strstr(last_json, "org.flameshot.Flameshot"));
    id = json_id();
    portal_shot_chosen(id, &mon, NULL, "/home/x/Pictures/Screenshots/Shot.png");
    CHECK(captured == 1 && !captured_area && !strcmp(captured_path, "/home/x/Pictures/Screenshots/Shot.png"));
    CHECK(!last_response());                                    /* nothing until the file is there */
    portal_shot_saved(id, 1, "/home/x/Pictures/Screenshots/Shot.png");
    pump(100);
    g = last_response();
    CHECK(g && g->code == 0 && !strncmp(g->uri, "file:///run/flatpak/app/org.example.App/doc/", 44) && strstr(g->uri, "/Shot.png"));
    CHECK(emits_taken == 1);
    /* an area, then its file failing */
    shot_call("Screenshot");
    id = json_id();
    int area[4] = { 10, 20, 300, 200 };
    portal_shot_chosen(id, &mon, area, "/home/x/Pictures/Screenshots/Shot (2).png");
    CHECK(captured == 2 && captured_area);
    portal_shot_saved(id, 0, NULL);
    pump(100);
    g = last_response();
    CHECK(g && g->code == 2);
    /* Cancel */
    shot_call("Screenshot");
    portal_shot_chosen(json_id(), NULL, NULL, NULL);
    pump(100);
    g = last_response();
    CHECK(g && g->code == 1 && captured == 2);
    /* a window with an area makes no sense: refused */
    shot_call("Screenshot");
    CastSource win = { .kind = CAST_SRC_WINDOW, .node = 5, .serial = 9 };
    portal_shot_chosen(json_id(), &win, area, "/home/x/a.png");
    pump(100);
    g = last_response();
    CHECK(g && g->code == 2 && captured == 2);
    /* one at a time */
    shot_call("Screenshot");
    char req[400], err[128];
    DBuf b; dw_init(&b);
    dw_str(&b, "");
    size_t a = dw_arr_begin(&b, 8);
    dw_arr_end(&b, a);
    call("org.freedesktop.portal.Screenshot", "Screenshot", "sa{sv}", &b, req, err);
    dw_free(&b);
    CHECK(err[0] != 0);
    portal_shot_chosen(json_id(), NULL, NULL, NULL);
    pump(100);
    /* an app with "no screen capture": refused, no chooser */
    screen_blocked = 1;
    emits_shot = 0;
    shot_call("Screenshot");
    g = last_response();
    CHECK(emits_shot == 0 && g && g->code == 2);
    screen_blocked = 0;

    /* 7. PickColor: the overlay, the point's color */
    shot_call("PickColor");
    CHECK(strstr(last_json, "\"color\":true") != NULL);
    portal_color_chosen(json_id(), &mon, 25, 50);
    pump(100);
    g = last_response();
    CHECK(g && g->code == 0 && g->has_color && g->color[0] > 0.24 && g->color[0] < 0.26 && g->color[1] > 0.49 && g->color[2] == 1);

    /* 8. the FileChooser: a Flatpak app gets the chosen file lent, anyone else the real path */
    fake_app = "flatpak:org.gnome.TextEditor";
    dw_init(&b);
    dw_str(&b, "");
    dw_str(&b, "Open");
    a = dw_arr_begin(&b, 8);
    opt_s(&b, "handle_token", "f1");
    dw_arr_end(&b, a);
    ngot = 0;
    call("org.freedesktop.portal.FileChooser", "OpenFile", "ssa{sv}", &b, req, err);
    dw_free(&b);
    pump(100);
    CHECK(!strcmp(last_kind, "portal-file"));
    const char *paths[] = { "/home/x/Docs/chosen.txt" };
    lent = 0;
    portal_file_chosen(json_id(), paths, 1);
    pump(100);
    g = last_response();
    CHECK(lent == 1 && g && g->code == 0 && strstr(g->uri, "file:///run/flatpak/app/org.example.App/doc/") && strstr(g->uri, "/chosen.txt"));
    fake_app = "desktop:org.gnome.gedit";
    dw_init(&b);
    dw_str(&b, "");
    dw_str(&b, "Open");
    a = dw_arr_begin(&b, 8);
    dw_arr_end(&b, a);
    ngot = 0;
    call("org.freedesktop.portal.FileChooser", "OpenFile", "ssa{sv}", &b, req, err);
    dw_free(&b);
    pump(100);
    lent = 0;
    portal_file_chosen(json_id(), paths, 1);
    pump(100);
    g = last_response();
    CHECK(lent == 0 && g && !strcmp(g->uri, "file:///home/x/Docs/chosen.txt"));
    /* a chosen path not written the plain way (an app's current_folder "/home/x/./.config") never reaches the lending
     * (Claude Security F1/F2): dropped, and with none left the dialog counts as cancelled */
    fake_app = "flatpak:org.gnome.TextEditor";
    const char *odd[] = { "/home/x/./.config", "/home/x//.ssh", "/home/x/.config/", "/home/x/Docs/../.ssh", "/home/x/Docs/chosen.txt" };
    for (int k = 0; k < 5; k++) {
        dw_init(&b);
        dw_str(&b, "");
        dw_str(&b, "Open");
        a = dw_arr_begin(&b, 8);
        dw_arr_end(&b, a);
        ngot = 0;
        call("org.freedesktop.portal.FileChooser", "OpenFile", "ssa{sv}", &b, req, err);
        dw_free(&b);
        pump(100);
        lent = 0;
        portal_file_chosen(json_id(), &odd[k], 1);
        pump(100);
        g = last_response();
        if (k < 4) CHECK(lent == 0 && g && g->code == 1 && !g->uri[0]);
        else CHECK(lent == 1 && g && g->code == 0);                 /* the plain one still goes */
    }
    lent = 0;
    dw_init(&b);
    dw_str(&b, "");
    dw_str(&b, "Open");
    a = dw_arr_begin(&b, 8);
    dw_arr_end(&b, a);
    call("org.freedesktop.portal.FileChooser", "OpenFile", "ssa{sv}", &b, req, err);
    dw_free(&b);
    pump(100);
    portal_file_chosen(json_id(), odd, 5);                          /* several: only the plain one is answered */
    pump(100);
    g = last_response();
    CHECK(lent == 1 && g && g->code == 0 && strstr(g->uri, "/chosen.txt"));

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    if (system(cmd)) {}
    printf("portal-cast-test: %s (%d checks)\n", fails ? "FAILED" : "ok", passes + fails);
    return fails != 0;
}
