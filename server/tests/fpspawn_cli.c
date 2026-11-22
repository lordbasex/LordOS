/* fpspawn_cli.c — a Flatpak app launched by the real launcher (src/apps.c) with lord-server's Spawn portal
 * (src/fpspawn.c) and the app's Spawn helper (src/fpspawner.c), outside the desktop: for the sub-sandbox tests of
 * tests/fwflatpak/spawn.sh (Chromium's and zypak's EXPOSE_PIDS / SHARE_PIDS). A test tool: never installed.
 *
 *   build/fpspawn-cli <Flatpak app id> [seconds]
 *
 * Takes org.freedesktop.portal.Flatpak on the session bus of DBUS_SESSION_BUS_ADDRESS (a private bus in the test, so a
 * running lord-server keeps its own), launches the app and serves its Spawn calls for that many seconds (default 60). */
#include "apps.h"
#include "fpspawn.h"
#include "fsops.h"
#include "fw/rules.h"
#include "sec/broker.h"
#include "sec/perms.h"
#include "sec/restrict.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void brokersrv_revoke(const char *app, const char *perm) { (void)app; (void)perm; }
PermState perm_get(const char *app_id, Perm p) { (void)app_id; (void)p; return PERM_ASK; }

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--launch-view")) return apps_launch_view_main();
    if (argc < 2 || argc > 3) { fprintf(stderr, "usage: fpspawn-cli <Flatpak app id> [seconds]\n"); return 2; }
    const char *home = getenv("HOME");
    char path[4096], err[200] = "";
    snprintf(path, sizeof path, "%s/.config/lordos/firewall.json", home ? home : "/nonexistent");
    FwStore *s = fw_store_new();
    if (!s || fw_load(s, path, err, sizeof err) < 0) fprintf(stderr, "fpspawn-cli: %s: %s (no restrictions)\n", path, err);
    restrict_set_store(s);
    if (fpspawn_start()) { fprintf(stderr, "fpspawn-cli: can't serve org.freedesktop.portal.Flatpak on this bus\n"); return 1; }
    apps_set_spawn_hook(fpspawn_add_view);
    const DesktopApp *list;
    apps_list("en", &list);
    if (apps_launch_flatpak(argv[1])) { fprintf(stderr, "fpspawn-cli: %s not launched %s\n", argv[1], apps_last_error()); return 1; }
    printf("fpspawn-cli: %s launched\n", argv[1]);
    fflush(stdout);
    int secs = argc == 3 ? atoi(argv[2]) : 60;
    time_t end = time(NULL) + (secs > 0 ? secs : 60);
    while (time(NULL) < end) {
        struct pollfd p[40];
        int n = 0;
        if (fpspawn_fd() >= 0) p[n++] = (struct pollfd){ fpspawn_fd(), (short)(POLLIN | (fpspawn_wants_write() ? POLLOUT : 0)), 0 };
        n += fpspawn_pollfds(p + n, 39);
        poll(p, (nfds_t)n, 50);
        if (n && p[0].revents) fpspawn_dispatch();
        fpspawn_dispatch_views();
        fpspawn_tick();
    }
    fpspawn_stop();
    return 0;
}
