/* pwguard_cli.c — the PipeWire guard (src/sec/pwguard.c) outside the desktop, for
 * tests/fwlinux/mic.sh.
 *
 *   XDG_RUNTIME_DIR=/run/user/<uid> build/pwguard-cli <seconds>
 *
 * Rereads ~/.config/axia/firewall.json every second into the store lord.fw uses
 * (sec/restrict.h), the way Settings changes it in a session, and runs the guard against the
 * session's PipeWire. A test tool: never installed. */
#include "sec/pwguard.h"
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

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    const char *home = getenv("HOME");
    char path[4096];
    snprintf(path, sizeof path, "%s/.config/axia/firewall.json", home ? home : "/nonexistent");
    FwStore *cur = NULL;
    if (pwguard_start()) { fprintf(stderr, "pwguard-cli: no PipeWire manager socket\n"); return 1; }
    printf("ready\n");
    fflush(stdout);
    time_t end = time(NULL) + secs, last = 0;
    while (time(NULL) < end) {
        if (time(NULL) != last) {
            last = time(NULL);
            FwStore *s = fw_store_new();
            char err[200];
            if (s && fw_load(s, path, err, sizeof err) >= 0) { restrict_set_store(s); fw_store_free(cur); cur = s; }
            else fw_store_free(s);
        }
        struct pollfd p = { .fd = pwguard_fd(), .events = POLLIN | (pwguard_wants_write() ? POLLOUT : 0) };
        if (p.fd >= 0 && poll(&p, 1, 100) > 0) pwguard_dispatch();
        else if (p.fd < 0) { struct timespec t = { 0, 100000000 }; nanosleep(&t, NULL); }
        pwguard_tick();
    }
    return 0;
}
