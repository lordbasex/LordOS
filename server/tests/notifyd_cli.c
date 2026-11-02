/* notifyd_cli.c — the real org.freedesktop.Notifications server (src/hw/notifyd.c) outside the
 * desktop, for tests/fwlinux/notify.sh.
 *
 *   DBUS_SESSION_BUS_ADDRESS=<a private test bus> build/notifyd-cli <seconds>
 *
 * Loads ~/.config/axia/firewall.json into the store lord.fw uses (sec/restrict.h), owns the
 * name on that bus and prints one line per notification it would show ("notify {...}") or close.
 * A test tool: never installed. */
#include "hw/notifyd.h"
#include "fw/rules.h"
#include "sec/broker.h"
#include "sec/perms.h"
#include "sec/restrict.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void brokersrv_revoke(const char *app, const char *perm) { (void)app; (void)perm; }
PermState perm_get(const char *app_id, Perm p) { (void)app_id; (void)p; return PERM_ASK; }

static void on_event(const char *kind, const char *json) { printf("%s %s\n", kind, json); fflush(stdout); }

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    const char *home = getenv("HOME");
    char path[4096], err[200] = "";
    snprintf(path, sizeof path, "%s/.config/axia/firewall.json", home ? home : "/nonexistent");
    FwStore *s = fw_store_new();
    if (!s || fw_load(s, path, err, sizeof err) < 0) fprintf(stderr, "notifyd-cli: %s: %s (no restrictions)\n", path, err);
    restrict_set_store(s);
    if (notifyd_start(on_event)) { fprintf(stderr, "notifyd-cli: could not own the name\n"); return 1; }
    printf("ready\n");
    fflush(stdout);
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) {
        struct pollfd p = { .fd = notifyd_fd(), .events = POLLIN | (notifyd_wants_write() ? POLLOUT : 0) };
        if (p.fd < 0) return 1;
        if (poll(&p, 1, 200) > 0) notifyd_dispatch();
    }
    return 0;
}
