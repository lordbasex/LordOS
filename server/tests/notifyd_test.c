/* notifyd_test.c — the notification server's "no notifications" check (src/hw/notifyd.c) against a
 * fake session bus, so the cases a real bus never produces can be forced:
 *   - an unrestricted app's notification is shown, a restricted one's is dropped with a valid id;
 *   - a reply to the PID question forged by another client (sender not org.freedesktop.DBus) is
 *     ignored; the bus's own answer decides;
 *   - an error reply, a PID that can't be resolved, and no reply at all (3 s) drop it.
 * The test is the bus (a unix socket in /tmp); notifyd runs in a child with the PID → app lookup
 * replaced (100 = desktop:bad, restricted; 200 = desktop:good; anything else unknown).
 * Also what Chromium checks before it uses the service instead of drawing its own popups
 * (GetCapabilities with "body" and "actions", GetServerInformation with spec 1.2), a Notify the way
 * Chrome sends it (actions, image-data, image-path, desktop-entry, suppress-sound hints), and
 * dbus_session_export, which gives the session's apps the bus address (docs/WAYLAND.md). */
#include "hw/notifyd.h"
#include "fw/rules.h"
#include "sec/broker.h"
#include "sec/perms.h"
#include "sec/restrict.h"
#include "vault/dbus.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void brokersrv_revoke(const char *app, const char *perm) { (void)app; (void)perm; }
PermState perm_get(const char *app_id, Perm p) { (void)app_id; (void)p; return PERM_ASK; }

static int fails, total;
static void check(int ok, const char *what) { total++; if (!ok) { fails++; printf("FAIL: %s\n", what); } }

/* ---- the notifyd side (child) ---- */
static int shown_fd;
static void on_event(const char *kind, const char *json) {
    if (strcmp(kind, "notify")) return;
    const char *t = strstr(json, "\"title\":\"");
    if (!t) return;
    t += 9;
    const char *e = strchr(t, '"');
    if (e && write(shown_fd, t, (size_t)(e - t)) < 0) {}
    const char *d = strstr(json, "\"desktop\":\"google-chrome\"");
    const char *a = strstr(json, "\"actions\":[[\"default\",\"Activate\"],[\"settings\",\"Settings\"]]");
    if (d && write(shown_fd, "|desktop", 8) < 0) {}
    if (a && write(shown_fd, "|actions", 8) < 0) {}
    if (strstr(json, "\"suppressSound\":true") && write(shown_fd, "|quiet", 6) < 0) {}
    if (write(shown_fd, "\n", 1) < 0) {}
}
static int fake_app_of_pid(uint32_t pid, char *out, size_t n) {
    if (pid == 100) { snprintf(out, n, "desktop:bad"); return 0; }
    if (pid == 200) { snprintf(out, n, "desktop:good"); return 0; }
    return -1;
}
static void run_notifyd(int secs) {
    FwStore *s = fw_store_new();
    fw_set_restricted(s, "desktop:bad", FW_R_NO_NOTIFICATIONS);
    restrict_set_store(s);
    notifyd_set_app_of_pid(fake_app_of_pid);
    if (notifyd_start(on_event)) _exit(3);
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) {
        struct pollfd p = { .fd = notifyd_fd(), .events = POLLIN | (notifyd_wants_write() ? POLLOUT : 0) };
        if (p.fd < 0) _exit(4);
        if (poll(&p, 1, 100) > 0) notifyd_dispatch();
        notifyd_tick();
    }
    _exit(0);
}

/* ---- the bus side (this process) ---- */
static DConn bus;
static uint32_t bserial = 100;
static void bus_send(DHdr *h, DBuf *b) { h->serial = ++bserial; dbus_send(&bus, h, b); dbus_flush(&bus); }

static int read_line(int fd, char *out, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 5000) <= 0 || read(fd, out + n, 1) != 1) return -1;
        n++;
        if (n >= 2 && out[n - 2] == '\r' && out[n - 1] == '\n') { out[n - 2] = 0; return 0; }
    }
    return -1;
}

/* next message from notifyd within ms (1), or 0 */
static int next(DMsg *m, int ms) {
    for (int i = 0; i < ms / 20; i++) {
        dbus_flush(&bus);
        int r = dbus_next(&bus, m);
        if (r < 0) return 0;
        if (r == 1) return 1;
        struct pollfd p = { bus.fd, POLLIN, 0 };
        poll(&p, 1, 20);
    }
    return 0;
}

static void reply_to(const DMsg *call, const char *sig, DBuf *b, const char *sender) {
    DHdr h = { .type = DBUS_RETURN, .reply_serial = call->serial, .dest = ":1.1", .sig = sig, .sender = sender };
    bus_send(&h, b);
}

/* sends Notify(title) from :1.9 and returns its serial */
static uint32_t notify(const char *title) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, "test"); dw_u32(&b, 0); dw_str(&b, ""); dw_str(&b, title); dw_str(&b, "body");
    size_t a = dw_arr_begin(&b, 4); dw_arr_end(&b, a);
    a = dw_arr_begin(&b, 8); dw_arr_end(&b, a);
    dw_i32(&b, -1);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/Notifications", .iface = "org.freedesktop.Notifications",
               .member = "Notify", .dest = ":1.1", .sig = "susssasa{sv}i", .sender = ":1.9" };
    bus_send(&h, &b);
    dw_free(&b);
    return bserial;
}

/* a Notify as Google Chrome sends it (notification_platform_bridge_linux.cc): two actions and the hints
 * it sets, an image as image-data (iiibiiay) and its icon as a file */
static uint32_t notify_chrome(const char *title) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, "Google Chrome"); dw_u32(&b, 0); dw_str(&b, ""); dw_str(&b, title); dw_str(&b, "web.example.org<br>hi &amp; bye");
    size_t a = dw_arr_begin(&b, 4);
    dw_str(&b, "default"); dw_str(&b, "Activate"); dw_str(&b, "settings"); dw_str(&b, "Settings");
    dw_arr_end(&b, a);
    a = dw_arr_begin(&b, 8);
    dw_struct(&b); dw_str(&b, "image-data"); dw_variant(&b, "(iiibiiay)");
    { static const uint8_t px[2 * 2 * 4] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
      dw_struct(&b); dw_i32(&b, 2); dw_i32(&b, 2); dw_i32(&b, 8); dw_bool(&b, 1); dw_i32(&b, 8); dw_i32(&b, 4); dw_bytes(&b, px, sizeof px); }
    dw_struct(&b); dw_str(&b, "image-path"); dw_variant(&b, "s"); dw_str(&b, "/tmp/.org.chromium.Chromium.XXXX");
    dw_struct(&b); dw_str(&b, "desktop-entry"); dw_variant(&b, "s"); dw_str(&b, "google-chrome");
    dw_struct(&b); dw_str(&b, "suppress-sound"); dw_variant(&b, "b"); dw_bool(&b, 1);
    dw_struct(&b); dw_str(&b, "x-kde-origin-name"); dw_variant(&b, "s"); dw_str(&b, "web.example.org");
    dw_struct(&b); dw_str(&b, "urgency"); dw_variant(&b, "y"); dw_u8(&b, 1);
    dw_arr_end(&b, a);
    dw_i32(&b, -1);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/Notifications", .iface = "org.freedesktop.Notifications",
               .member = "Notify", .dest = ":1.1", .sig = "susssasa{sv}i", .sender = ":1.9" };
    bus_send(&h, &b);
    dw_free(&b);
    return bserial;
}

/* calls a method without arguments and waits for its answer (1 with it in *out) */
static int call_and_wait(const char *member, DMsg *out) {
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/Notifications", .iface = "org.freedesktop.Notifications",
               .member = member, .dest = ":1.1", .sender = ":1.9" };
    bus_send(&h, NULL);
    uint32_t serial = bserial;
    while (next(out, 2000)) {
        if ((out->type == DBUS_RETURN || out->type == DBUS_ERROR) && out->reply_serial == serial) return 1;
        dbus_msg_free(out);
    }
    return 0;
}

/* dbus_session_export: dir has a socket "bus" of ours */
static void test_export(const char *dir, const char *expect) {
    char got[256];
    unsetenv("DBUS_SESSION_BUS_ADDRESS");
    setenv("XDG_RUNTIME_DIR", dir, 1);
    check(dbus_session_export() == 1, "export: set when unset and $XDG_RUNTIME_DIR/bus is our socket");
    snprintf(got, sizeof got, "%s", getenv("DBUS_SESSION_BUS_ADDRESS") ? getenv("DBUS_SESSION_BUS_ADDRESS") : "");
    check(!strcmp(got, expect), "export: unix:path= with the address escaping");
    if (strcmp(got, expect)) printf("  got %s, want %s\n", got, expect);
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/elsewhere", 1);
    check(dbus_session_export() == 0 && !strcmp(getenv("DBUS_SESSION_BUS_ADDRESS"), "unix:path=/elsewhere"), "export: an address already set is kept");
    unsetenv("DBUS_SESSION_BUS_ADDRESS");
    setenv("XDG_RUNTIME_DIR", "/nonexistent-lordos-test", 1);
    check(dbus_session_export() == 0 && !getenv("DBUS_SESSION_BUS_ADDRESS"), "export: no socket there, nothing set");
    setenv("XDG_RUNTIME_DIR", "relative/dir", 1);
    check(dbus_session_export() == 0 && !getenv("DBUS_SESSION_BUS_ADDRESS"), "export: a relative runtime dir is ignored");
    unsetenv("XDG_RUNTIME_DIR");
    check(dbus_session_export() == 0 && !getenv("DBUS_SESSION_BUS_ADDRESS"), "export: no runtime dir, nothing set");
}

/* waits for notifyd's GetConnectionUnixProcessID; returns its serial (0 if none) */
static uint32_t pid_question(void) {
    DMsg m;
    while (next(&m, 2000)) {
        uint32_t s = m.type == DBUS_CALL && m.member && !strcmp(m.member, "GetConnectionUnixProcessID") ? m.serial : 0;
        dbus_msg_free(&m);
        if (s) return s;
    }
    return 0;
}

static void pid_answer(uint32_t q, uint32_t pid, const char *sender) {
    DMsg fake = { 0 };
    fake.serial = q;
    DBuf b;
    dw_init(&b);
    dw_u32(&b, pid);
    reply_to(&fake, "u", &b, sender);
    dw_free(&b);
}

/* waits for the answer to Notify serial: the id (0 = none, or an error) */
static uint32_t notify_answer(uint32_t serial, int ms) {
    DMsg m;
    while (next(&m, ms)) {
        uint32_t id = 0;
        int mine = (m.type == DBUS_RETURN || m.type == DBUS_ERROR) && m.reply_serial == serial;
        if (mine && m.type == DBUS_RETURN && m.sig && !strcmp(m.sig, "u")) { DRd r; dr_init(&r, &m); id = dr_u32(&r); }
        dbus_msg_free(&m);
        if (mine) return id;
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char path[] = "/tmp/notifyd test,=-XXXXXX";       /* a space, a comma and '=' get escaped in the address */
    if (!mkdtemp(path)) return 2;
    char sock[100], addr[200];
    snprintf(sock, sizeof sock, "%s/bus", path);
    int l = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
    if (l < 0 || bind(l, (struct sockaddr *)&sa, sizeof sa) || listen(l, 1)) return 2;
    {
        char want[200];
        snprintf(want, sizeof want, "unix:path=/tmp/notifyd%%20test%%2c%%3d-%s/bus", path + strlen("/tmp/notifyd test,=-"));
        test_export(path, want);
        snprintf(addr, sizeof addr, "%s", want);   /* notifyd reads the escaped form back */
    }
    int pipefd[2];
    if (pipe(pipefd)) return 2;
    setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
    pid_t child = fork();
    if (child == 0) { close(pipefd[0]); shown_fd = pipefd[1]; close(l); run_notifyd(15); }
    close(pipefd[1]);
    int fd = accept(l, NULL, NULL);
    char line[256], nul;
    check(fd >= 0 && read(fd, &nul, 1) == 1 && !read_line(fd, line, sizeof line), "auth line");
    check(write(fd, "OK 0123456789abcdef0123456789abcdef\r\n", 37) == 37, "auth ok");
    check(!read_line(fd, line, sizeof line) && !strcmp(line, "BEGIN"), "BEGIN");
    memset(&bus, 0, sizeof bus);
    bus.fd = fd;
    for (int step = 0; step < 2; step++) {          /* Hello, RequestName */
        DMsg m;
        if (!next(&m, 3000)) { check(0, "Hello/RequestName"); break; }
        DBuf b;
        dw_init(&b);
        if (!strcmp(m.member, "Hello")) { dw_str(&b, ":1.1"); reply_to(&m, "s", &b, "org.freedesktop.DBus"); }
        else { dw_u32(&b, 1); reply_to(&m, "u", &b, "org.freedesktop.DBus"); }
        dw_free(&b);
        dbus_msg_free(&m);
    }

    usleep(300 * 1000);                             /* notifyd_start's wait for RequestName is over */

    /* 0. what Chromium checks before using the service: "body" and "actions" (else it draws its own
     *    popups), and a specification version of at least 1.1 */
    {
        DMsg m;
        int body = 0, actions = 0, markup = 0;
        if (call_and_wait("GetCapabilities", &m)) {
            check(m.type == DBUS_RETURN && m.sig && !strcmp(m.sig, "as"), "0: GetCapabilities answers as");
            DRd r;
            dr_init(&r, &m);
            size_t end = dr_arr(&r, 4);
            while (!r.err && dr_more(&r, end)) {
                const char *c = dr_str(&r, NULL);
                if (!c) break;
                body |= !strcmp(c, "body"); actions |= !strcmp(c, "actions"); markup |= !strcmp(c, "body-markup");
            }
            dbus_msg_free(&m);
        }
        check(body && actions && markup, "0: capabilities body, actions and body-markup");
        if (call_and_wait("GetServerInformation", &m)) {
            check(m.type == DBUS_RETURN && m.sig && !strcmp(m.sig, "ssss"), "0: GetServerInformation answers ssss");
            DRd r;
            dr_init(&r, &m);
            const char *name = dr_str(&r, NULL), *vendor = dr_str(&r, NULL), *version = dr_str(&r, NULL), *spec = dr_str(&r, NULL);
            check(!r.err && name && name[0] && vendor && version && spec && !strcmp(spec, "1.2"), "0: name set and spec_version 1.2");
            dbus_msg_free(&m);
        } else check(0, "0: GetServerInformation answered");
    }

    /* 1. unrestricted: shown */
    uint32_t n = notify("good"), q = pid_question();
    check(q != 0, "1: notifyd asks the bus for the sender's PID");
    pid_answer(q, 200, "org.freedesktop.DBus");
    check(notify_answer(n, 2000) > 0, "1: Notify answered with an id");

    /* 1b. Chrome's Notify (actions and every hint it sets, an image among them): shown, with its
     *     .desktop entry and both actions */
    n = notify_chrome("chrome"); q = pid_question();
    check(q != 0, "1b: Chrome's Notify parsed and the PID asked");
    pid_answer(q, 200, "org.freedesktop.DBus");
    check(notify_answer(n, 2000) > 0, "1b: Chrome's Notify answered with an id");

    /* 2. restricted: answered, not shown */
    n = notify("bad"); q = pid_question();
    pid_answer(q, 100, "org.freedesktop.DBus");
    check(notify_answer(n, 2000) > 0, "2: restricted Notify still gets an id");

    /* 3. a forged reply from another client with an unrestricted PID is ignored */
    n = notify("spoof"); q = pid_question();
    pid_answer(q, 200, ":1.9");
    pid_answer(q, 100, "org.freedesktop.DBus");
    check(notify_answer(n, 2000) > 0, "3: answered after the bus's own reply");

    /* 4. an error reply from the bus: dropped, still an id */
    n = notify("error"); q = pid_question();
    { DHdr h = { .type = DBUS_ERROR, .reply_serial = q, .dest = ":1.1", .error = "org.freedesktop.DBus.Error.NameHasNoOwner", .sender = "org.freedesktop.DBus" };
      bus_send(&h, NULL); }
    check(notify_answer(n, 2000) > 0, "4: error reply: answered with an id");

    /* 5. a PID that can't be resolved (process gone): dropped */
    n = notify("gone"); q = pid_question();
    pid_answer(q, 999, "org.freedesktop.DBus");
    check(notify_answer(n, 2000) > 0, "5: unknown PID: answered with an id");

    /* 6. no reply at all: after 3 s, dropped with an id */
    n = notify("silent"); q = pid_question();
    check(q != 0, "6: asked");
    check(notify_answer(n, 5000) > 0, "6: no reply: answered with an id after the timeout");

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    char shown[512] = "";
    ssize_t r = read(pipefd[0], shown, sizeof shown - 1);
    shown[r > 0 ? r : 0] = 0;
    check(!strcmp(shown, "good\nchrome|desktop|actions|quiet\n"), "only the unrestricted notifications were shown, Chrome's with its entry, actions and suppress-sound");
    if (strcmp(shown, "good\nchrome|desktop|actions\n")) printf("  shown: %s\n", shown);
    unlink(sock);
    rmdir(path);
    printf("notifyd-test: %d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
