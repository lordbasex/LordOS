/* webalive_fake.c — a stand-in lord-web for tests/webalive_test.c: `--app <app> --profile <profile>` like the real one, it
 * takes the app's bus name (webreg_bus_name) on the session bus and answers org.lordos.WebApp1.Ping with "ok" and Show
 * with nothing, until $HOME/hang-<its pid> appears: then it stops reading the bus (a main loop that hangs). With
 * $HOME/old it answers Ping as an older lord-web would (UnknownMethod). It starts one child in a session of its own (a
 * WebKit process that left the group: the restart must end it too) and writes its own pid to $HOME/start-<app>-<pid>
 * and the child's to $HOME/child-<pid>. Built by mk/webapps.mk as build/webalive-fake. */
#include "../src/vault/dbus.h"
#include "../src/web/webreg.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void put_pid(const char *path, pid_t pid) {
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)pid);
    fclose(f);
    rename(tmp, path);
}

int main(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[1], "--app") || strcmp(argv[3], "--profile")) return 2;
    const char *home = getenv("HOME");
    if (!home) return 2;
    char path[PATH_MAX], name[160];
    if (webreg_bus_name(argv[2], argv[4], name, sizeof name)) return 2;

    pid_t child = fork();
    if (child == 0) {
        setsid();
        for (;;) pause();
    }
    snprintf(path, sizeof path, "%s/child-%d", home, (int)getpid());
    put_pid(path, child);

    int fd = dbus_connect_session();
    DConn c;
    if (fd < 0 || dbus_open(&c, fd, 5000)) return 3;
    DBuf b;
    dw_init(&b);
    dw_str(&b, name);
    dw_u32(&b, 4);                                  /* DO_NOT_QUEUE */
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus", .member = "RequestName",
               .dest = "org.freedesktop.DBus", .sig = "su" };
    uint32_t serial = dbus_send(&c, &h, &b);
    dw_free(&b);
    DMsg m;
    if (!serial || dbus_wait_reply(&c, serial, &m, 5000) != 1) return 4;
    dbus_msg_free(&m);
    snprintf(path, sizeof path, "%s/start-%s-%d", home, argv[2], (int)getpid());
    put_pid(path, getpid());

    char hang[PATH_MAX], old[PATH_MAX];
    snprintf(hang, sizeof hang, "%s/hang-%d", home, (int)getpid());
    snprintf(old, sizeof old, "%s/old", home);
    for (;;) {
        struct stat st;
        if (!stat(hang, &st)) for (;;) pause();     /* hung: nothing read, nothing answered, SIGTERM still ends it */
        struct pollfd p = { c.fd, POLLIN, 0 };
        poll(&p, 1, 50);
        int r;
        while ((r = dbus_next(&c, &m)) == 1) {
            if (m.type == DBUS_CALL && m.member && m.iface && !strcmp(m.iface, WEBREG_SHOW_IFACE) && !strcmp(m.member, "Ping")) {
                if (!stat(old, &st)) dbus_error(&c, &m, "org.freedesktop.DBus.Error.UnknownMethod", "no such method");
                else {
                    DBuf a;
                    dw_init(&a);
                    dw_str(&a, "ok");
                    dbus_reply(&c, &m, "s", &a);
                    dw_free(&a);
                }
            } else if (m.type == DBUS_CALL && !(m.flags & DBUS_NO_REPLY)) dbus_reply(&c, &m, NULL, NULL);
            dbus_msg_free(&m);
        }
        if (r < 0 || dbus_flush(&c) < 0) return 0;
    }
}
