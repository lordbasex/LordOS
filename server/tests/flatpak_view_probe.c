/* flatpak_view_probe.c — does `flatpak run` (bubblewrap) start inside the launcher's view? (docs/FLATPAK.md, Problem 1:
 * the first thing to test; "Why scoping-only Landlock"). Not a suite: run by hand on a test machine with Flatpak.
 *
 *   build/flatpak-view-probe [--no-bus] -- /usr/bin/flatpak run ...
 *
 * Builds the same view the launcher builds for a keep_home app (sandbox_files_isolate: user, mount, PID and IPC
 * namespaces, a runtime folder with only the Wayland socket, pipewire-0, pulse/native and, unless --no-bus, the session
 * bus; never systemd/), applies Landlock's scoping only (sandbox_apply_scope), says whether the user manager's socket is
 * reachable from inside, then execs the command. It doesn't enter an app scope (that needs lord-sessiond). */
#include "../src/sec/sandbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int bus = 1, i = 1;
    if (i < argc && !strcmp(argv[i], "--no-bus")) { bus = 0; i++; }
    if (i < argc && !strcmp(argv[i], "--")) i++;
    if (i >= argc) { fprintf(stderr, "usage: flatpak-view-probe [--no-bus] -- command ...\n"); return 2; }
    static char home[4096], rt[4096];
    const char *h = getenv("HOME"), *r = getenv("XDG_RUNTIME_DIR"), *wd = getenv("WAYLAND_DISPLAY");
    if (!h || !realpath(h, home) || !r || !realpath(r, rt)) { fprintf(stderr, "probe: HOME or XDG_RUNTIME_DIR missing\n"); return 2; }
    SandboxFilesCfg cfg = { .home = home, .runtime_dir = rt, .keep_home = 1 };
    int n = 0;
    cfg.sockets[n++] = wd && wd[0] && !strchr(wd, '/') ? wd : "wayland-0";
    cfg.sockets[n++] = "pipewire-0";
    cfg.sockets[n++] = "pulse/native";
    if (bus) cfg.sockets[n++] = "bus";
    cfg.sockets[n] = NULL;
    if (sandbox_files_isolate(&cfg)) { fprintf(stderr, "probe: the view could not be built\n"); return 3; }
    if (sandbox_apply_scope()) { fprintf(stderr, "probe: Landlock scoping failed\n"); return 3; }
    char sd[4200];
    snprintf(sd, sizeof sd, "/run/user/%u/systemd/private", (unsigned)getuid());
    fprintf(stderr, "probe: in the view; %s %s; session bus %s\n", sd, access(sd, F_OK) ? "hidden" : "REACHABLE",
            bus ? "kept" : "removed");
    execv(argv[i], argv + i);
    perror("probe: exec");
    return 127;
}
