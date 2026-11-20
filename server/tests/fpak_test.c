/* fpak_test.c — Flatpak apps for the launcher (src/fpak.c): ids, branches, reading an installation without following
 * links out of it, the [Context] home grant and the `flatpak run` command line. */
#include "../src/fpak.h"
#include "../src/fw/rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int pass, failc;
#define CHECK(x) do { if (x) pass++; else { failc++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

static char root[256];
static void mk(const char *rel) { char p[512]; snprintf(p, sizeof p, "%s/%s", root, rel); mkdir(p, 0755); }
static void put(const char *rel, const char *text) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "w"); if (f) { fputs(text, f); fclose(f); }
}
static void ln(const char *target, const char *rel) { char p[512]; snprintf(p, sizeof p, "%s/%s", root, rel); unlink(p); if (symlink(target, p)) {} }

static int has(char **argv, const char *s) { for (int i = 0; argv[i]; i++) if (!strcmp(argv[i], s)) return 1; return 0; }
static int idx(char **argv, const char *s) { for (int i = 0; argv[i]; i++) if (!strcmp(argv[i], s)) return i; return -1; }

#define C64 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

int main(void) {
    /* ids */
    CHECK(fpak_id_ok("org.gnome.Calculator"));
    CHECK(fpak_id_ok("com.github.tchx84.Flatseal"));
    CHECK(fpak_id_ok("io.github.some_one.App-Name"));
    CHECK(!fpak_id_ok("org.gnome"));                              /* two elements */
    CHECK(!fpak_id_ok("org..gnome.X"));
    CHECK(!fpak_id_ok(".org.gnome.X"));
    CHECK(!fpak_id_ok("org.gnome.X."));
    CHECK(!fpak_id_ok("org.9gnome.X"));                           /* an element starting with a digit */
    CHECK(!fpak_id_ok("org.my-co.App"));                          /* '-' outside the last element */
    CHECK(!fpak_id_ok("org.gnome.Calc/../x"));
    CHECK(!fpak_id_ok("org.gnome.Calc ulator"));
    CHECK(!fpak_id_ok("org.gnome.Calc;rm"));
    CHECK(!fpak_id_ok(""));
    char longid[80];
    memset(longid, 'a', sizeof longid); memcpy(longid, "org.x.", 6); longid[60] = 0;   /* 60 bytes: over the scope limit */
    CHECK(!fpak_id_ok(longid));
    longid[59] = 0;
    CHECK(fpak_id_ok(longid));
    /* branches */
    CHECK(fpak_branch_ok("stable") && fpak_branch_ok("25.08") && fpak_branch_ok("beta"));
    CHECK(!fpak_branch_ok("") && !fpak_branch_ok("../x") && !fpak_branch_ok("a/b") && !fpak_branch_ok("-x") && !fpak_branch_ok("a b"));

    /* an installation */
    snprintf(root, sizeof root, "/tmp/fpak-test.XXXXXX");
    if (!mkdtemp(root)) { printf("no temp dir\n"); return 1; }
    mk("app"); mk("app/org.gnome.Calculator"); mk("app/org.gnome.Calculator/x86_64"); mk("app/org.gnome.Calculator/x86_64/stable");
    mk("app/org.gnome.Calculator/x86_64/stable/" C64);
    put("app/org.gnome.Calculator/x86_64/stable/" C64 "/metadata", "[Application]\nname=org.gnome.Calculator\n\n[Context]\nshared=network;ipc;\nfilesystems=xdg-download;home:ro;\n");
    ln(C64, "app/org.gnome.Calculator/x86_64/stable/active");
    ln("x86_64/stable", "app/org.gnome.Calculator/current");
    char arch[32], branch[64], md[4096];
    CHECK(fpak_installed(root, "org.gnome.Calculator", arch, sizeof arch, branch, sizeof branch, md, sizeof md) == 0);
    CHECK(!strcmp(arch, "x86_64") && !strcmp(branch, "stable") && strstr(md, "[Context]"));
    CHECK(fpak_installed(root, "org.gnome.Nothing", arch, sizeof arch, branch, sizeof branch, NULL, 0) < 0);
    /* links that leave the app's folder are refused */
    ln("../../../etc", "app/org.gnome.Calculator/current");
    CHECK(fpak_installed(root, "org.gnome.Calculator", arch, sizeof arch, branch, sizeof branch, NULL, 0) < 0);
    ln("x86_64/stable", "app/org.gnome.Calculator/current");
    ln("../../../../x", "app/org.gnome.Calculator/x86_64/stable/active");
    CHECK(fpak_installed(root, "org.gnome.Calculator", arch, sizeof arch, branch, sizeof branch, NULL, 0) < 0);
    ln(C64, "app/org.gnome.Calculator/x86_64/stable/active");
    /* the app's folder itself a link: refused */
    char p1[512], p2[512];
    snprintf(p1, sizeof p1, "%s/app/org.gnome.Calculator", root);
    snprintf(p2, sizeof p2, "%s/app/org.gnome.Other", root);
    if (symlink(p1, p2)) {}
    CHECK(fpak_installed(root, "org.gnome.Other", arch, sizeof arch, branch, sizeof branch, NULL, 0) < 0);
    /* metadata a link: refused */
    snprintf(p1, sizeof p1, "%s/app/org.gnome.Calculator/x86_64/stable/" C64 "/metadata", root);
    unlink(p1);
    if (symlink("/etc/passwd", p1)) {}
    CHECK(fpak_installed(root, "org.gnome.Calculator", arch, sizeof arch, branch, sizeof branch, NULL, 0) < 0);

    /* the command line */
    char *argv[FPAK_ARGV_MAX], buf[8192];
    FpakRun r = { .id = "org.gnome.Calculator", .arch = "x86_64", .branch = "stable", .home = root, .x11 = 1 };
    int n = fpak_argv(&r, argv, buf, sizeof buf);
    CHECK(n > 5 && !strcmp(argv[0], "/usr/bin/flatpak") && !strcmp(argv[1], "run") && !strcmp(argv[n - 1], "org.gnome.Calculator"));
    CHECK(has(argv, "--arch=x86_64") && has(argv, "--branch=stable") && has(argv, "--nosocket=session-bus"));
    CHECK(has(argv, "--no-talk-name=org.freedesktop.Flatpak") && has(argv, "--no-talk-name=org.freedesktop.systemd1"));
    CHECK(has(argv, "--no-talk-name=org.freedesktop.secrets") && has(argv, "--talk-name=org.freedesktop.Notifications"));
    CHECK(has(argv, "--nofilesystem=~/.config/lordos") && has(argv, "--nofilesystem=~/.config/autostart"));
    CHECK(!has(argv, "--unshare=network") && !has(argv, "--nosocket=x11"));
    /* restrictions */
    r.restricted = FW_R_NO_NETWORK | FW_R_NO_USB | FW_R_NO_BLUETOOTH | FW_R_NO_FILES;
    n = fpak_argv(&r, argv, buf, sizeof buf);
    CHECK(has(argv, "--unshare=network") && has(argv, "--nodevice=usb") && has(argv, "--disallow=bluetooth"));
    CHECK(has(argv, "--nofilesystem=home") && has(argv, "--nofilesystem=host") && has(argv, "--nosocket=x11"));
    /* LordOS's options come before the id: nothing after it but the opened path */
    r.restricted = 0;
    r.opened = "/home/a/Documents/x.txt";
    n = fpak_argv(&r, argv, buf, sizeof buf);
    CHECK(n > 2 && !strcmp(argv[n - 1], "/home/a/Documents/x.txt") && !strcmp(argv[n - 2], "org.gnome.Calculator"));
    CHECK(idx(argv, "--filesystem=/home/a/Documents/x.txt") >= 0 && idx(argv, "--filesystem=/home/a/Documents/x.txt") < n - 2);
    r.opened_ro = 1;                                              /* a path the launcher may only lend read-only */
    n = fpak_argv(&r, argv, buf, sizeof buf);
    CHECK(has(argv, "--filesystem=/home/a/Documents/x.txt:ro") && !has(argv, "--filesystem=/home/a/Documents/x.txt"));
    r.opened_ro = 0;
    r.opened = "/home/a/x:rw";                                    /* a mode suffix would change the grant */
    CHECK(fpak_argv(&r, argv, buf, sizeof buf) < 0);
    r.opened = "relative/x";
    CHECK(fpak_argv(&r, argv, buf, sizeof buf) < 0);
    r.opened = NULL;
    r.id = "org.gnome";
    CHECK(fpak_argv(&r, argv, buf, sizeof buf) < 0);
    r.id = "org.gnome.Calculator";
    r.branch = "../x";
    CHECK(fpak_argv(&r, argv, buf, sizeof buf) < 0);
    r.branch = "stable";
    CHECK(fpak_argv(&r, argv, buf, 64) < 0);                      /* no room: refused, never cut short */

    /* org.freedesktop.portal.Flatpak is talked to (lord-server serves it, E9), never owned */
    CHECK(has(argv, "--talk-name=org.freedesktop.portal.Flatpak") && !has(argv, "--no-talk-name=org.freedesktop.portal.Flatpak"));

    /* a Spawn (E9): what the app asks for first, LordOS's options after it, the app's arguments after the id */
    {
        static char *sargv[FPAK_SPAWN_ARGV_MAX], sbuf[65536];
        const char *args[] = { "/usr/libexec/glycin-loaders/2+/glycin-svg", "--dbus-fd", "3", "--filesystem=host", NULL };
        const char *env[] = { "RUST_LOG=debug", NULL };
        FpakSpawn sp = { .flags = FPAK_SPAWN_SANDBOX | FPAK_SPAWN_WATCH_BUS, .args = args, .env = env, .instance_fd = -1 };
        r.restricted = FW_R_NO_NETWORK;
        int m = fpak_spawn_argv(&r, &sp, sargv, sbuf, sizeof sbuf);
        CHECK(m > 8 && has(sargv, "--sandbox") && has(sargv, "--no-session-bus") && has(sargv, "--no-a11y-bus"));
        CHECK(has(sargv, "--command=/usr/libexec/glycin-loaders/2+/glycin-svg") && has(sargv, "--env=RUST_LOG=debug"));
        int id_at = idx(sargv, "org.gnome.Calculator");
        CHECK(id_at > 0 && idx(sargv, "--filesystem=host") > id_at && !strcmp(sargv[m - 1], "--filesystem=host"));
        CHECK(idx(sargv, "--dbus-fd") == id_at + 1);
        /* LordOS's restrictions and never-granted options come after what the app asked for */
        CHECK(idx(sargv, "--unshare=network") > idx(sargv, "--sandbox") && idx(sargv, "--nosocket=session-bus") > idx(sargv, "--command=/usr/libexec/glycin-loaders/2+/glycin-svg"));
        sp.flags = FPAK_SPAWN_SANDBOX;
        sp.sandbox_flags = FPAK_SANDBOX_SHARE_DISPLAY | FPAK_SANDBOX_SHARE_GPU | FPAK_SANDBOX_ALLOW_DBUS;
        r.x11 = 0;
        m = fpak_spawn_argv(&r, &sp, sargv, sbuf, sizeof sbuf);
        CHECK(has(sargv, "--socket=wayland") && has(sargv, "--device=dri") && has(sargv, "--session-bus"));
        CHECK(idx(sargv, "--nosocket=fallback-x11") > idx(sargv, "--socket=fallback-x11"));   /* no X: the later one wins */
        r.x11 = 1;
        sp.flags = FPAK_SPAWN_EXPOSE_PIDS; sp.sandbox_flags = 0; sp.parent_pid = 7;
        m = fpak_spawn_argv(&r, &sp, sargv, sbuf, sizeof sbuf);
        CHECK(has(sargv, "--parent-pid=7") && has(sargv, "--parent-expose-pids") && !has(sargv, "--sandbox"));
        sp.parent_pid = 0;
        CHECK(fpak_spawn_argv(&r, &sp, sargv, sbuf, sizeof sbuf) < 0);                         /* no parent to expose to */
        sp.flags = 0;
        const char *expose[] = { "glycin", NULL }, *bad_expose[] = { "../.ssh", NULL };
        sp.expose = expose;
        m = fpak_spawn_argv(&r, &sp, sargv, sbuf, sizeof sbuf);
        { char want[600]; snprintf(want, sizeof want, "--filesystem=%s/.var/app/org.gnome.Calculator/sandbox/glycin", root); CHECK(has(sargv, want)); }
        sp.expose = bad_expose;
        CHECK(fpak_spawn_argv(&r, &sp, sargv, sbuf, sizeof sbuf) < 0);
        sp.expose = NULL;
        sp.flags = 1u << 12;                                                                  /* an unknown flag */
        CHECK(fpak_spawn_argv(&r, &sp, sargv, sbuf, sizeof sbuf) < 0);
        sp.flags = 0;
        const char *bad_env[] = { "1X=y", NULL }, *nl_env[] = { "X=a\n--filesystem=host", NULL };
        sp.env = bad_env;
        CHECK(fpak_spawn_argv(&r, &sp, sargv, sbuf, sizeof sbuf) < 0);
        sp.env = nl_env;
        CHECK(fpak_spawn_argv(&r, &sp, sargv, sbuf, sizeof sbuf) < 0);
        sp.env = env;
        sp.cwd = "/app/../home";
        CHECK(fpak_spawn_argv(&r, &sp, sargv, sbuf, sizeof sbuf) < 0);
        sp.cwd = "relative";
        CHECK(fpak_spawn_argv(&r, &sp, sargv, sbuf, sizeof sbuf) < 0);
        sp.cwd = "/app/..data";                                                              /* a name, not ".." */
        CHECK(fpak_spawn_argv(&r, &sp, sargv, sbuf, sizeof sbuf) > 0);
        sp.cwd = NULL;
        const char *empty[] = { "", NULL };
        sp.args = empty;
        CHECK(fpak_spawn_argv(&r, &sp, sargv, sbuf, sizeof sbuf) < 0);
        sp.args = args;
        r.opened = "/home/a/x";                                                               /* never both */
        CHECK(fpak_spawn_argv(&r, &sp, sargv, sbuf, sizeof sbuf) < 0);
        r.opened = NULL;
        r.restricted = 0;
    }

    /* refs and the closed list of management commands (E3) */
    CHECK(fpak_ref_ok("app/org.gnome.Calculator/x86_64/stable", "x86_64"));
    CHECK(!fpak_ref_ok("runtime/org.gnome.Platform/x86_64/48", "x86_64"));           /* never a runtime */
    CHECK(!fpak_ref_ok("app/org.gnome.Calculator/aarch64/stable", "x86_64"));
    CHECK(!fpak_ref_ok("app/org.gnome.Calculator/x86_64/stable/x", "x86_64"));
    CHECK(!fpak_ref_ok("app/org.gnome.Calculator/x86_64", "x86_64"));
    CHECK(!fpak_ref_ok("app/../x86_64/stable", "x86_64"));
    CHECK(!fpak_ref_ok("app/org.gnome.Calculator/x86_64/--no-gpg-verify", "x86_64"));
    CHECK(!fpak_ref_ok("app/org.gnome.Calculator/x86_64/", "x86_64"));
    n = fpak_cmd_argv(FPAK_CMD_INSTALL, "app/org.gnome.Calculator/x86_64/stable", "x86_64", argv, buf, sizeof buf);
    CHECK(n == 7 && !strcmp(argv[0], "/usr/bin/flatpak") && !strcmp(argv[1], "install") && !strcmp(argv[5], "flathub") &&
          !strcmp(argv[6], "app/org.gnome.Calculator/x86_64/stable") && has(argv, "--user") && has(argv, "--noninteractive"));
    CHECK(fpak_cmd_argv(FPAK_CMD_INSTALL, NULL, "x86_64", argv, buf, sizeof buf) < 0);               /* a ref is needed */
    CHECK(fpak_cmd_argv(FPAK_CMD_INSTALL, "flathub", "x86_64", argv, buf, sizeof buf) < 0);
    CHECK(fpak_cmd_argv(FPAK_CMD_APPSTREAM, "app/org.gnome.Calculator/x86_64/stable", "x86_64", argv, buf, sizeof buf) < 0);
    n = fpak_cmd_argv(FPAK_CMD_REMOTE_ADD, NULL, "x86_64", argv, buf, sizeof buf);
    CHECK(n > 0 && !strcmp(argv[n - 1], FPAK_REPO_FILE) && has(argv, "--from") && !has(argv, "--no-gpg-verify"));
    n = fpak_cmd_argv(FPAK_CMD_UPDATE, NULL, "x86_64", argv, buf, sizeof buf);
    CHECK(n > 0 && !strcmp(argv[n - 1], "-y"));
    n = fpak_cmd_argv(FPAK_CMD_UPDATE, "app/org.gnome.Calculator/x86_64/stable", "x86_64", argv, buf, sizeof buf);
    CHECK(n > 0 && !strcmp(argv[n - 1], "app/org.gnome.Calculator/x86_64/stable"));
    CHECK(fpak_cmd_argv(FPAK_CMD_COUNT, NULL, "x86_64", argv, buf, sizeof buf) < 0 && fpak_cmd_argv(-1, NULL, "x86_64", argv, buf, sizeof buf) < 0);
    for (int c = 0; c < FPAK_CMD_COUNT; c++) {                                       /* never a relaxed check */
        int m = fpak_cmd_argv(c, c == FPAK_CMD_INFO || c == FPAK_CMD_INSTALL || c == FPAK_CMD_UNINSTALL ? "app/a.b.C/x86_64/stable" : NULL,
                              "x86_64", argv, buf, sizeof buf);
        CHECK(m > 0 && !has(argv, "--no-gpg-verify") && !has(argv, "--system") && has(argv, "--user"));
    }

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd)) {}
    printf("fpak-test: %d passed, %d failed\n", pass, failc);
    return failc != 0;
}
