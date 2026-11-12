/* portal_gs_test.c — the GlobalShortcuts portal (src/ui/portal.c) over a private session bus: a second
 * connection plays the app, stubs stand in for the shell (hw_emit, where the sheet would open) and the
 * compositor. Run by tests/run_suites.sh under dbus-run-session, with a temporary home folder. */
#include "../src/ui/portal.h"
#include "../src/ui/gshort.h"
#include "../src/ui/webask.h"
#include "../src/wl/keygrab.h"
#include "../src/vault/dbus.h"
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static char last_kind[64], last_json[8192];
void hw_emit(const char *kind, const char *json) {
    snprintf(last_kind, sizeof last_kind, "%s", kind);
    snprintf(last_json, sizeof last_json, "%s", json);
}
int wl_focus_pid(void) { return 0; }
int wl_foreign_surface(const char *h) { (void)h; return 0; }
static const char *fake_app = "desktop:com.example.Talk";
int appid_of_pid(unsigned pid, char *out, size_t n) { (void)pid; if (!fake_app) return 1; snprintf(out, n, "%s", fake_app); return 0; }
/* the ScreenCast side of the same portal (no helper here: only its sessions, which share the session paths) */
int appid_running(int (*match)(const char *app, void *u), void *u) { (void)match; (void)u; return 0; }
int restrict_blocks(const char *fwid, uint32_t flag) { (void)fwid; (void)flag; return 0; }
int cast_available(void) { return 0; }
int cast_begin(int id, const CastSource *src, int n, int cursor_embedded) { (void)id; (void)src; (void)n; (void)cursor_embedded; return -1; }
void cast_end(int id) { (void)id; }
int cast_remote(int id, uint32_t req) { (void)id; (void)req; return -1; }
void cast_shown(int id) { (void)id; }
int cast_thumb(const CastSource *src, int max) { (void)src; (void)max; return 0; }
/* web apps' questions share the connection (src/ui/webask.c, its own tests): none here */
void webask_attach(DConn *c) { (void)c; }
void webask_call(DConn *c, const DMsg *m) { (void)c; (void)m; }
int webask_reply(DConn *c, const DMsg *m) { (void)c; (void)m; return 0; }
void webask_gone(const char *name) { (void)name; }
void webask_appeared(const char *name) { (void)name; }
void webask_lost(void) {}

static DConn app;
typedef struct { char member[64], sig[64], path[400], s1[400], s2[128]; uint32_t code; uint64_t t; int type; char err[128]; int nlist; char list[8][128]; } Got;
static Got got[64];
static int ngot;

static void decode_list(DRd *r, Got *g) {
    size_t end = dr_arr(r, 8);
    while (!r->err && dr_more(r, end)) {
        dr_struct(r);
        const char *id = dr_str(r, NULL);
        char desc[64] = "", trig[64] = "";
        size_t de = dr_arr(r, 8);
        while (!r->err && dr_more(r, de)) {
            dr_struct(r);
            const char *k = dr_str(r, NULL), *sig = dr_variant(r);
            if (!strcmp(sig, "s")) { const char *v = dr_str(r, NULL); if (!strcmp(k, "description")) snprintf(desc, sizeof desc, "%s", v); else snprintf(trig, sizeof trig, "%s", v); }
            else dr_skip(r, sig);
        }
        if (g->nlist < 8) snprintf(g->list[g->nlist++], 128, "%s=%s(%s)", id, trig, desc);
    }
}

static void pump(int ms) {
    for (int left = ms; left > 0; left -= 10) {
        struct pollfd p[2] = { { portal_fd(), POLLIN | (portal_wants_write() ? POLLOUT : 0), 0 }, { app.fd, POLLIN, 0 } };
        poll(p, 2, 10);
        portal_dispatch();
        DMsg m;
        while (dbus_next(&app, &m) == 1) {
            if (ngot < 64 && (m.type == DBUS_SIGNAL || m.type == DBUS_ERROR) && !(m.sender && !strcmp(m.sender, "org.freedesktop.DBus"))) {
                Got *g = &got[ngot++];
                memset(g, 0, sizeof *g);
                g->type = m.type;
                snprintf(g->member, sizeof g->member, "%s", m.member ? m.member : "");
                snprintf(g->sig, sizeof g->sig, "%s", m.sig ? m.sig : "");
                snprintf(g->path, sizeof g->path, "%s", m.path ? m.path : "");
                if (m.error) snprintf(g->err, sizeof g->err, "%s", m.error);
                DRd r;
                dr_init(&r, &m);
                if (m.sig && !strcmp(m.sig, "ua{sv}")) {
                    g->code = dr_u32(&r);
                    size_t end = dr_arr(&r, 8);
                    while (!r.err && dr_more(&r, end)) {
                        dr_struct(&r);
                        const char *k = dr_str(&r, NULL), *sig = dr_variant(&r);
                        if (!strcmp(k, "session_handle") && !strcmp(sig, "s")) snprintf(g->s1, sizeof g->s1, "%s", dr_str(&r, NULL));
                        else if (!strcmp(k, "shortcuts")) decode_list(&r, g);
                        else dr_skip(&r, sig);
                    }
                } else if (m.sig && !strcmp(m.sig, "osta{sv}")) {
                    snprintf(g->s1, sizeof g->s1, "%s", dr_str(&r, NULL));
                    snprintf(g->s2, sizeof g->s2, "%s", dr_str(&r, NULL));
                    g->t = dr_u64(&r);
                } else if (m.sig && !strcmp(m.sig, "oa(sa{sv})")) {
                    snprintf(g->s1, sizeof g->s1, "%s", dr_str(&r, NULL));
                    decode_list(&r, g);
                }
            }
            dbus_msg_free(&m);
        }
        dbus_flush(&app);
    }
}

static Got *find(const char *member) {
    for (int i = ngot; i-- > 0;) if (!strcmp(got[i].member, member)) return &got[i];
    return NULL;
}

static uint32_t call(const char *path, const char *iface, const char *member, const char *sig, DBuf *b, char *reply_path, char *err) {
    DHdr h = { .type = DBUS_CALL, .path = path, .iface = iface, .member = member, .dest = "org.freedesktop.portal.Desktop", .sig = sig };
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
                pump(100);
                return s;
            }
            dbus_msg_free(&m);
        }
    }
    return 0;
}

static void opt_s(DBuf *b, const char *k, const char *v) { dw_struct(b); dw_str(b, k); dw_variant(b, "s"); dw_str(b, v); }

static void add_match(void) {
    DBuf b; dw_init(&b);
    dw_str(&b, "type='signal',interface='org.freedesktop.portal.GlobalShortcuts'");
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus", .member = "AddMatch", .dest = "org.freedesktop.DBus", .sig = "s" };
    dbus_send(&app, &h, &b); dw_free(&b);
    dw_init(&b);
    dw_str(&b, "type='signal',interface='org.freedesktop.portal.Request'");
    dbus_send(&app, &h, &b); dw_free(&b);
    dbus_flush(&app);
}

static void create(char *session, const char *stoken) {
    DBuf b; dw_init(&b);
    size_t a = dw_arr_begin(&b, 8);
    opt_s(&b, "handle_token", "c1");
    opt_s(&b, "session_handle_token", stoken);
    dw_arr_end(&b, a);
    char req[400], err[128];
    call("/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "CreateSession", "a{sv}", &b, req, err);
    dw_free(&b);
    pump(200);
    Got *g = find("Response");
    snprintf(session, 400, "%s", g ? g->s1 : "");
}

/* CreateSession on iface with that session token: the session's path from the Response, "" and err when refused */
static void create_on(const char *iface, char *session, const char *stoken, char *err) {
    DBuf b; dw_init(&b);
    size_t a = dw_arr_begin(&b, 8);
    opt_s(&b, "handle_token", "c9");
    opt_s(&b, "session_handle_token", stoken);
    dw_arr_end(&b, a);
    char req[400];
    ngot = 0;
    call("/org/freedesktop/portal/desktop", iface, "CreateSession", "a{sv}", &b, req, err);
    dw_free(&b);
    pump(150);
    Got *g = err[0] ? NULL : find("Response");
    snprintf(session, 400, "%s", g ? g->s1 : "");
}

static void bind(const char *session, char *err, int n, const char *const *ids, const char *const *trig) {
    DBuf b; dw_init(&b);
    dw_str(&b, session);
    size_t a = dw_arr_begin(&b, 8);
    for (int i = 0; i < n; i++) {
        dw_struct(&b);
        dw_str(&b, ids[i]);
        size_t o = dw_arr_begin(&b, 8);
        opt_s(&b, "description", ids[i]);
        if (trig[i]) opt_s(&b, "preferred_trigger", trig[i]);
        dw_arr_end(&b, o);
    }
    dw_arr_end(&b, a);
    dw_str(&b, "");
    a = dw_arr_begin(&b, 8);
    opt_s(&b, "handle_token", "b1");
    dw_arr_end(&b, a);
    char req[400];
    call("/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "BindShortcuts", "oa(sa{sv})sa{sv}", &b, req, err);
    dw_free(&b);
}

int main(void) {
    char home[] = "/tmp/portal-gs-XXXXXX";
    if (!mkdtemp(home)) return 2;
    setenv("HOME", home, 1);
    CHECK(portal_start() == 0);
    int fd = dbus_connect_session();
    CHECK(fd >= 0 && dbus_open(&app, fd, 2000) == 0);
    add_match();

    char session[400], err[128];
    create(session, "s1");
    CHECK(!strncmp(session, "/org/freedesktop/portal/desktop/session/", 40) && strstr(session, "/s1"));

    const char *ids[] = { "ptt", "mute" }, *trig[] = { "CTRL+SHIFT+t", "LOGO+m" };
    ngot = 0;
    bind(session, err, 2, ids, trig);
    CHECK(!err[0]);
    CHECK(!strcmp(last_kind, "portal-shortcuts") && strstr(last_json, "\"proposed\":\"Ctrl+Shift+T\"") && strstr(last_json, "\"kept\":1"));
    CHECK(!find("Response"));                                   /* nothing bound before the person answers */
    CHECK(portal_shortcut_key(KEY_T, 1, 't', KG_CTRL | KG_SHIFT) == 0);
    char again[128];
    bind(session, again, 2, ids, trig);
    CHECK(again[0]);                                            /* asking: one sheet at a time */

    /* the person answers: ptt with its key, mute with another (the kept one would be refused anyway) */
    int id = 0;
    sscanf(strstr(last_json, "\"id\":") + 5, "%d", &id);
    const char *ans_ids[] = { "ptt", "mute" }, *ans[] = { "Ctrl+Shift+T", "Meta+M" };
    portal_shortcuts_chosen(id, ans_ids, ans, 2);
    pump(200);
    Got *g = find("Response");
    CHECK(g && g->code == 0 && g->nlist == 1 && strstr(g->list[0], "ptt=Ctrl+Shift+T(ptt)"));

    /* push-to-talk: press, repeats, release */
    ngot = 0;
    CHECK(portal_shortcut_key(KEY_T, 1, 't', KG_CTRL | KG_SHIFT) == 1);
    CHECK(portal_shortcut_key(KEY_T, 2, 't', KG_CTRL | KG_SHIFT) == 1);
    pump(150);
    CHECK((g = find("Activated")) && !strcmp(g->s1, session) && !strcmp(g->s2, "ptt") && g->t > 0);
    CHECK(portal_shortcut_key(KEY_T, 0, 't', KG_CTRL) == 1);   /* released after Shift: still its release */
    pump(150);
    CHECK((g = find("Deactivated")) && !strcmp(g->s2, "ptt"));
    CHECK(ngot == 2);
    CHECK(portal_shortcut_key(KEY_T, 1, 't', KG_CTRL) == 0);   /* other modifiers: not it */
    CHECK(portal_shortcut_key(KEY_T, 0, 't', KG_CTRL) == 0);
    portal_shortcuts_pause(1);
    CHECK(portal_shortcut_key(KEY_T, 1, 't', KG_CTRL | KG_SHIFT) == 0);
    portal_shortcuts_pause(0);
    CHECK(portal_shortcut_key(KEY_T, 0, 't', KG_CTRL | KG_SHIFT) == 0);

    /* ListShortcuts */
    ngot = 0;
    {
        DBuf b; dw_init(&b);
        dw_str(&b, session);
        size_t a = dw_arr_begin(&b, 8); opt_s(&b, "handle_token", "l1"); dw_arr_end(&b, a);
        char req[400];
        call("/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "ListShortcuts", "oa{sv}", &b, req, err);
        dw_free(&b);
        pump(150);
        CHECK(!err[0] && (g = find("Response")) && g->nlist == 1 && strstr(req, "/l1"));
    }
    /* a bound session can't bind again */
    bind(session, err, 2, ids, trig);
    CHECK(err[0]);

    /* the file was written, and lord.fs can't write it */
    char path[700];
    snprintf(path, sizeof path, "%s/.config/lordos/global-shortcuts.json", home);
    CHECK(access(path, R_OK) == 0);
    FILE *f = fopen(path, "r");
    char text[2048] = "";
    if (f) { text[fread(text, 1, sizeof text - 1, f)] = 0; fclose(f); }
    CHECK(strstr(text, "\"desktop:com.example.Talk\"") && strstr(text, "\"trigger\":\"Ctrl+Shift+T\"") && strstr(text, "\"mute\""));

    /* Settings changes it: ShortcutsChanged */
    ngot = 0;
    CHECK(portal_shortcut_set("desktop:com.example.Talk", "ptt", "Alt+F9") == 0);
    CHECK(portal_shortcut_set("desktop:com.example.Talk", "ptt", "Meta+F9") == -1);
    CHECK(portal_shortcut_set("desktop:com.example.Talk", "ptt", "F") == -1);
    CHECK(portal_shortcut_set("desktop:com.example.Talk", "nope", "Alt+F8") == -1);
    pump(150);
    CHECK((g = find("ShortcutsChanged")) && g->nlist == 1 && strstr(g->list[0], "ptt=Alt+F9"));
    CHECK(portal_shortcut_key(KEY_F9, 1, 0, KG_ALT) == 1);
    CHECK(portal_shortcut_key(KEY_F9, 0, 0, KG_ALT) == 1);
    CHECK(portal_shortcut_key(KEY_T, 1, 't', KG_CTRL | KG_SHIFT) == 0);

    /* check for another app: it's taken */
    char out[64], owner[256]; int kept = -1;
    CHECK(portal_shortcut_check("ALT+F9", 0, "desktop:other", "x", out, sizeof out, &kept, owner, sizeof owner) == 0 && !strcmp(owner, "desktop:com.example.Talk") && kept == 0);
    CHECK(portal_shortcut_check("LOGO+space", 0, "desktop:other", "x", out, sizeof out, &kept, owner, sizeof owner) == 0 && kept == GS_LOGO && !strcmp(out, "Meta+Space"));
    CHECK(portal_shortcut_check("nope+x", 0, "", "", out, sizeof out, &kept, owner, sizeof owner) == -1);

    /* Session.Close: nothing more */
    {
        DBuf b; dw_init(&b);
        call(session, "org.freedesktop.portal.Session", "Close", NULL, &b, NULL, err);
        dw_free(&b);
        CHECK(!err[0]);
    }
    CHECK(portal_shortcut_key(KEY_F9, 1, 0, KG_ALT) == 0);

    /* a new session of the same app: decided before, no sheet */
    create(session, "s2");
    last_kind[0] = 0;
    ngot = 0;
    bind(session, err, 2, ids, trig);
    pump(150);
    CHECK(!err[0] && strcmp(last_kind, "portal-shortcuts") && (g = find("Response")) && g->nlist == 1 && strstr(g->list[0], "ptt=Alt+F9"));
    /* held while the app leaves the bus: the release goes nowhere */
    CHECK(portal_shortcut_key(KEY_F9, 1, 0, KG_ALT) == 1);
    /* a new proposal id → the sheet again; Request.Close cancels it */
    char s3[400];
    create(s3, "s3");
    const char *ids3[] = { "ptt", "new" }, *trig3[] = { "CTRL+SHIFT+t", "CTRL+ALT+n" };
    last_kind[0] = 0;
    bind(s3, err, 2, ids3, trig3);
    CHECK(!strcmp(last_kind, "portal-shortcuts") && strstr(last_json, "\"current\":\"Alt+F9\""));
    {
        DBuf b; dw_init(&b);
        char rp[400];
        snprintf(rp, sizeof rp, "%s", s3);
        char *p = strstr(rp, "/session/");
        memcpy(p, "/request/", 9);
        char *tok = strrchr(rp, '/');
        strcpy(tok + 1, "b1");
        call(rp, "org.freedesktop.portal.Request", "Close", NULL, &b, NULL, err);
        dw_free(&b);
        CHECK(!err[0] && !strcmp(last_kind, "portal-shortcuts-gone"));
    }
    /* Forget in Settings */
    ngot = 0;
    CHECK(portal_shortcuts_forget("desktop:com.example.Talk") == 2);
    pump(150);
    CHECK((g = find("ShortcutsChanged")) && g->nlist == 0);
    CHECK(portal_shortcut_key(KEY_F9, 0, 0, KG_ALT) == 1);      /* the held release is still taken */
    CHECK(portal_shortcut_key(KEY_F9, 1, 0, KG_ALT) == 0);

    /* an app in no scope: asked, nothing kept */
    fake_app = NULL;
    char s4[400];
    create(s4, "s4");
    last_kind[0] = 0;
    bind(s4, err, 2, ids3, trig3);
    CHECK(!strcmp(last_kind, "portal-shortcuts") && strstr(last_json, "\"app\":\"\""));
    sscanf(strstr(last_json, "\"id\":") + 5, "%d", &id);
    const char *a4[] = { "Ctrl+Alt+N", "Ctrl+Alt+N" };
    portal_shortcuts_chosen(id, ids3, a4, 2);
    pump(150);
    char *list = portal_shortcuts_list();
    CHECK(list && !strstr(list, "Ctrl+Alt+N"));
    free(list);
    CHECK(portal_shortcut_key(KEY_N, 1, 'n', KG_CTRL | KG_ALT) == 1);
    CHECK(portal_shortcut_key(KEY_N, 0, 'n', KG_CTRL | KG_ALT) == 1);

    /* a ScreenCast session has the same path form: one object is never both, and Session.Close reaches the right one */
    {
        char sc[400], gs[400], e2[128];
        create_on("org.freedesktop.portal.ScreenCast", sc, "s5", err);
        CHECK(!err[0] && strstr(sc, "/session/") && strstr(sc, "/s5"));
        create_on("org.freedesktop.portal.GlobalShortcuts", gs, "s5", err);
        CHECK(err[0] && !gs[0]);
        DBuf b; dw_init(&b);
        call(sc, "org.freedesktop.portal.Session", "Close", NULL, &b, NULL, err);
        CHECK(!err[0]);
        call(sc, "org.freedesktop.portal.Session", "Close", NULL, &b, NULL, err);
        CHECK(err[0]);                                          /* gone */
        create_on("org.freedesktop.portal.GlobalShortcuts", gs, "s6", err);
        CHECK(!err[0] && strstr(gs, "/s6"));
        create_on("org.freedesktop.portal.ScreenCast", sc, "s6", e2);
        CHECK(e2[0] && !sc[0]);
        call(gs, "org.freedesktop.portal.Session", "Close", NULL, &b, NULL, err);
        CHECK(!err[0]);
        dw_free(&b);
    }

    /* the app leaves the bus: nothing more for it */
    dbus_close(&app);
    for (int i = 0; i < 30; i++) { portal_dispatch(); usleep(10000); }
    CHECK(portal_shortcut_key(KEY_N, 1, 'n', KG_CTRL | KG_ALT) == 0);

    unlink(path);
    snprintf(path, sizeof path, "%s/.config/lordos", home);
    rmdir(path);
    snprintf(path, sizeof path, "%s/.config", home);
    rmdir(path);
    rmdir(home);
    if (fails) { printf("portal-gs-test: %d failures\n", fails); return 1; }
    printf("portal-gs-test: ok\n");
    return 0;
}
