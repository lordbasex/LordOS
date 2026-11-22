/* fpportal_probe.c — a fake Flatpak app's side of the portals, run inside a real Flatpak sandbox for the end-to-end
 * checks of docs/FLATPAK.md (lending the chosen file, Screenshot, Spawn's usr-fd): it calls the portal on the session bus
 * the sandbox has (Flatpak's filtering proxy), waits for the answer and checks what it got. A test tool, built static so
 * it runs on any Flatpak runtime; never installed.
 *
 *   fpportal-probe open SIBLING      FileChooser.OpenFile; the file it gets must be readable, SIBLING (a real path next
 *                                    to the chosen file) must not be
 *   fpportal-probe save              FileChooser.SaveFile; writes "saved by the probe" to what it gets
 *   fpportal-probe shot              Screenshot; the picture it gets must be a PNG it can read
 *   fpportal-probe color             PickColor
 *   fpportal-probe spawn-usr DIR     Flatpak.Spawn with usr-fd DIR: refused unless the app is Steam
 * Prints "PROBE ok ..." or "PROBE fail ..." lines; exit 0 when every check held. */
#include "vault/dbus.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static DConn c;
static int bad;
#define SAY(ok, ...) do { printf("PROBE %s ", (ok) ? "ok" : "fail"); printf(__VA_ARGS__); printf("\n"); fflush(stdout); if (!(ok)) bad = 1; } while (0)

static void opt_s(DBuf *b, const char *k, const char *v) { dw_struct(b); dw_str(b, k); dw_variant(b, "s"); dw_str(b, v); }

static int unhex(char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }
static void uri_path(const char *uri, char *out, size_t n) {
    size_t o = 0;
    if (!strncmp(uri, "file://", 7)) uri += 7;
    for (const char *s = uri; *s && o + 1 < n; s++) {
        if (*s == '%' && unhex(s[1]) >= 0 && unhex(s[2]) >= 0) { out[o++] = (char)(unhex(s[1]) * 16 + unhex(s[2])); s += 2; }
        else out[o++] = *s;
    }
    out[o] = 0;
}

/* calls iface.member on the portal; waits up to `secs` for the Response on the handle it returns */
static int request(const char *iface, const char *member, const char *sig, DBuf *b, int secs, uint32_t *code, char *uri, size_t un, double *rgb) {
    DBuf m; dw_init(&m);
    dw_str(&m, "type='signal',interface='org.freedesktop.portal.Request'");
    DHdr am = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus", .member = "AddMatch", .dest = "org.freedesktop.DBus", .sig = "s" };
    dbus_send(&c, &am, &m);
    dw_free(&m);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/portal/desktop", .iface = iface, .member = member,
               .dest = "org.freedesktop.portal.Desktop", .sig = sig };
    uint32_t s = dbus_send(&c, &h, b);
    dbus_flush(&c);
    DMsg r;
    if (dbus_wait_reply(&c, s, &r, 5000) != 1) { printf("PROBE fail %s.%s: %s\n", iface, member, r.error ? r.error : "no reply"); return -1; }
    dbus_msg_free(&r);
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) {
        struct pollfd p = { c.fd, POLLIN, 0 };
        poll(&p, 1, 500);
        DMsg m2;
        while (dbus_next(&c, &m2) == 1) {
            if (m2.type == DBUS_SIGNAL && m2.member && !strcmp(m2.member, "Response") && m2.sig && !strcmp(m2.sig, "ua{sv}")) {
                DRd rd;
                dr_init(&rd, &m2);
                *code = dr_u32(&rd);
                size_t e = dr_arr(&rd, 8);
                while (!rd.err && dr_more(&rd, e)) {
                    dr_struct(&rd);
                    const char *k = dr_str(&rd, NULL), *vs = dr_variant(&rd);
                    if ((!strcmp(k, "uri") && !strcmp(vs, "s"))) snprintf(uri, un, "%s", dr_str(&rd, NULL));
                    else if (!strcmp(k, "uris") && !strcmp(vs, "as")) {
                        size_t e2 = dr_arr(&rd, 4);
                        while (!rd.err && dr_more(&rd, e2)) { const char *u = dr_str(&rd, NULL); if (!uri[0]) snprintf(uri, un, "%s", u); }
                    } else if (!strcmp(k, "color") && !strcmp(vs, "(ddd)") && rgb) {
                        dr_struct(&rd);
                        for (int i = 0; i < 3; i++) { uint64_t u = dr_u64(&rd); memcpy(&rgb[i], &u, 8); }
                    } else dr_skip(&rd, vs);
                }
                dbus_msg_free(&m2);
                return 0;
            }
            dbus_msg_free(&m2);
        }
        dbus_flush(&c);
    }
    printf("PROBE fail %s.%s: no answer in %d s\n", iface, member, secs);
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: fpportal-probe open SIBLING | save | shot | color | spawn-usr DIR\n"); return 2; }
    int fd = dbus_connect_session();
    if (fd < 0 || dbus_open_fds(&c, fd, 3000)) { printf("PROBE fail no session bus\n"); return 1; }
    const char *what = argv[1];
    uint32_t code = 99;
    char uri[4096] = "", path[4096];
    DBuf b; dw_init(&b);
    if (!strcmp(what, "open") || !strcmp(what, "save")) {
        int save = what[0] == 's';
        dw_str(&b, "");
        dw_str(&b, save ? "Save" : "Open");
        size_t a = dw_arr_begin(&b, 8);
        opt_s(&b, "handle_token", "probe1");
        if (save) opt_s(&b, "current_name", "probe-saved.txt");
        dw_arr_end(&b, a);
        if (request("org.freedesktop.portal.FileChooser", save ? "SaveFile" : "OpenFile", "ssa{sv}", &b, 180, &code, uri, sizeof uri, NULL)) return 1;
        uri_path(uri, path, sizeof path);
        SAY(code == 0 && !strncmp(path, "/run/flatpak/app/", 17) && strstr(path, "/doc/"), "%s: got %s (code %u)", what, path, code);
        if (!save) {
            int f = open(path, O_RDONLY);
            char buf[256] = "";
            ssize_t n = f >= 0 ? read(f, buf, sizeof buf - 1) : -1;
            if (f >= 0) close(f);
            buf[n > 0 ? n : 0] = 0;
            SAY(n >= 0, "the chosen file is readable: \"%.60s\"", buf);
            if (argc > 2) {
                int s = open(argv[2], O_RDONLY);
                SAY(s < 0, "the file next to it (%s) is not reachable", argv[2]);
                if (s >= 0) close(s);
                char sib[4200];
                const char *base = strrchr(argv[2], '/');
                snprintf(sib, sizeof sib, "%.*s/%s", (int)(strrchr(path, '/') - path), path, base ? base + 1 : argv[2]);
                s = open(sib, O_RDONLY);
                SAY(s < 0, "nor next to the lent one (%s)", sib);
                if (s >= 0) close(s);
            }
        } else {
            int f = open(path, O_WRONLY | O_TRUNC);
            SAY(f >= 0 && write(f, "saved by the probe\n", 19) == 19, "the save target is writable");
            if (f >= 0) close(f);
            char other[4200];
            snprintf(other, sizeof other, "%.*s/probe-other.txt", (int)(strrchr(path, '/') - path), path);
            f = open(other, O_WRONLY | O_CREAT, 0600);
            SAY(f < 0, "nothing else can be made next to it");
            if (f >= 0) close(f);
        }
    } else if (!strcmp(what, "shot") || !strcmp(what, "color")) {
        int color = what[0] == 'c';
        double rgb[3] = { -1, -1, -1 };
        dw_str(&b, "");
        size_t a = dw_arr_begin(&b, 8);
        opt_s(&b, "handle_token", "probe2");
        dw_arr_end(&b, a);
        if (request("org.freedesktop.portal.Screenshot", color ? "PickColor" : "Screenshot", "sa{sv}", &b, 180, &code, uri, sizeof uri, rgb)) return 1;
        if (color) SAY(code == 0 && rgb[0] >= 0 && rgb[0] <= 1, "color: %.3f %.3f %.3f (code %u)", rgb[0], rgb[1], rgb[2], code);
        else {
            uri_path(uri, path, sizeof path);
            int f = open(path, O_RDONLY);
            unsigned char sig[8] = { 0 };
            ssize_t n = f >= 0 ? read(f, sig, 8) : -1;
            if (f >= 0) close(f);
            SAY(code == 0 && n == 8 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G', "shot: %s is a PNG it can read (code %u)", path, code);
        }
    } else if (!strcmp(what, "spawn-usr") && argc > 2) {
        int d = open(argv[2], O_RDONLY | O_DIRECTORY);
        dw_bytes(&b, (const uint8_t *)"/", 2);
        size_t a = dw_arr_begin(&b, 4);
        dw_bytes(&b, (const uint8_t *)"/bin/true", 10);
        dw_arr_end(&b, a);
        a = dw_arr_begin(&b, 8);
        dw_arr_end(&b, a);
        a = dw_arr_begin(&b, 8);
        dw_arr_end(&b, a);
        dw_u32(&b, 0);
        a = dw_arr_begin(&b, 8);
        dw_struct(&b); dw_str(&b, "usr-fd"); dw_variant(&b, "h"); dw_u32(&b, 0);
        dw_arr_end(&b, a);
        DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/portal/Flatpak", .iface = "org.freedesktop.portal.Flatpak", .member = "Spawn",
                   .dest = "org.freedesktop.portal.Flatpak", .sig = "ayaaya{uh}a{ss}ua{sv}" };
        uint32_t s = d >= 0 ? dbus_send_fd(&c, &h, &b, d) : 0;
        dbus_flush(&c);
        DMsg r;
        int got = s ? dbus_wait_reply(&c, s, &r, 5000) : -1;
        const char *err = got == 0 && r.error ? r.error : "";
        printf("PROBE info spawn with usr-fd: %s\n", got == 1 ? "started" : got == 0 ? err : "no reply");
        if (argc > 3 && !strcmp(argv[3], "expect-refused")) SAY(got == 0 && strstr(err, "NotSupported"), "usr-fd refused for this app");
        if (got >= 0) dbus_msg_free(&r);
    } else { fprintf(stderr, "unknown\n"); return 2; }
    dw_free(&b);
    return bad;
}
