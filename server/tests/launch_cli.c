/* launch_cli.c — the Linux app launcher (src/apps.c) outside the desktop, for the end-to-end
 * tests of the firewall's "no access to your files" (tests/fwlinux/files.sh).
 *
 *   build/launch-cli <desktop id> [file]
 *
 * Reads ~/.config/axia/firewall.json into the same store lord.fw uses (sec/restrict.h) and
 * calls apps_launch / apps_launch_with: the real launcher code, Landlock plan included. Outside
 * a session the app stays in the caller's cgroup (the cgroup service only places axia-server's
 * own binaries). A test tool: never installed. */
#include "apps.h"
#include "fsops.h"
#include "fw/rules.h"
#include "sec/broker.h"
#include "sec/perms.h"
#include "sec/restrict.h"

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>

/* restrict.c's other users (the broker, the permission store) aren't part of this tool */
void brokersrv_revoke(const char *app, const char *perm) { (void)app; (void)perm; }
PermState perm_get(const char *app_id, Perm p) { (void)app_id; (void)p; return PERM_ASK; }

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) { fprintf(stderr, "usage: launch-cli <desktop id> [file]\n"); return 2; }
    const char *home = getenv("HOME");
    char path[4096], err[200] = "";
    snprintf(path, sizeof path, "%s/.config/axia/firewall.json", home ? home : "/nonexistent");
    FwStore *s = fw_store_new();
    if (!s || fw_load(s, path, err, sizeof err) < 0) fprintf(stderr, "launch-cli: %s: %s (no restrictions)\n", path, err);
    restrict_set_store(s);
    char real[4096];
    const char *user = getenv("USER");
    if (home && realpath(home, real)) fsops_init(real, user ? user : "");   /* the roots "Open with" accepts */
    const DesktopApp *list;
    apps_list("en", &list);
    int r = argc == 3 ? apps_launch_with(argv[1], argv[2]) : apps_launch(argv[1]);
    printf("launch-cli: %s → %s\n", argv[1], r ? "not launched" : "launched");
    return r ? 1 : 0;
}
