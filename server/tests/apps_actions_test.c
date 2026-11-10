/* apps_actions_test.c — desktop actions of the Linux app launcher (src/apps.c): which
 * "[Desktop Action …]" groups are listed, their translated names, and that apps_launch_action only
 * runs an action the .desktop lists (with its own Exec line, through the normal launcher).
 *
 * Uses a temporary HOME with one .desktop in ~/.local/share/applications whose actions touch
 * marker files. Outside a session the launched process stays in the caller's cgroup. */
#include "apps.h"
#include "fsops.h"
#include "fw/rules.h"
#include "sec/broker.h"
#include "sec/perms.h"
#include "sec/restrict.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void brokersrv_revoke(const char *app, const char *perm) { (void)app; (void)perm; }
PermState perm_get(const char *app_id, Perm p) { (void)app_id; (void)p; return PERM_ASK; }

static int n, bad;
#define CHECK(name, cond) do { n++; if (!(cond)) { bad++; printf("✗ %s\n", name); } } while (0)

static int appeared(const char *path) {
    for (int i = 0; i < 100; i++) {
        if (!access(path, F_OK)) return 1;
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}

int main(void) {
    char home[] = "/tmp/lord-actions-XXXXXX";
    if (!mkdtemp(home)) { perror("mkdtemp"); return 1; }
    setenv("HOME", home, 1);
    unsetenv("LORDOS_SESSIOND");
    char path[512];
    const char *dirs[] = { "%s/.local", "%s/.local/share", "%s/.local/share/applications", NULL };
    for (int i = 0; dirs[i]; i++) { snprintf(path, sizeof path, dirs[i], home); mkdir(path, 0700); }
    snprintf(path, sizeof path, "%s/.local/share/applications/lord-demo.desktop", home);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 1; }
    fprintf(f,
            "[Desktop Entry]\nType=Application\nName=Demo\nExec=/usr/bin/touch %s/main\n"
            "Actions=new-window;private;bad_name;noexec;nolabel;new-window;\n\n"
            "[Desktop Action new-window]\nName=New Window\nName[es]=Nueva ventana\nExec=/usr/bin/touch %s/act-new\n\n"
            "[Desktop Action private]\nName=Private\\sWindow\nExec=/usr/bin/touch %s/act-private\n\n"
            "[Desktop Action bad_name]\nName=Bad\nExec=/usr/bin/touch %s/act-bad\n\n"
            "[Desktop Action noexec]\nName=No exec\n\n"
            "[Desktop Action nolabel]\nExec=/usr/bin/touch %s/act-nolabel\n\n"
            "[Desktop Action unlisted]\nName=Unlisted\nExec=/usr/bin/touch %s/act-unlisted\n",
            home, home, home, home, home, home);
    fclose(f);
    restrict_set_store(fw_store_new());

    AppAction acts[APPS_MAX_ACTIONS];
    int k = apps_actions("lord-demo", "en", acts, APPS_MAX_ACTIONS);
    CHECK("two valid listed actions", k == 2);
    CHECK("first is new-window", k > 0 && !strcmp(acts[0].name, "new-window") && !strcmp(acts[0].label, "New Window"));
    CHECK("second is private, unescaped", k > 1 && !strcmp(acts[1].name, "private") && !strcmp(acts[1].label, "Private Window"));
    k = apps_actions("lord-demo", "es-AR", acts, APPS_MAX_ACTIONS);
    CHECK("translated label", k > 0 && !strcmp(acts[0].label, "Nueva ventana"));
    CHECK("max is honoured", apps_actions("lord-demo", "en", acts, 1) == 1);
    CHECK("unknown app: none", apps_actions("lord-nope", "en", acts, APPS_MAX_ACTIONS) == 0);
    CHECK("invalid id: none", apps_actions("../lord-demo", "en", acts, APPS_MAX_ACTIONS) == 0);

    CHECK("unlisted action refused", apps_launch_action("lord-demo", "unlisted") != 0);
    CHECK("invalid action name refused", apps_launch_action("lord-demo", "bad_name") != 0);
    CHECK("action without Exec refused", apps_launch_action("lord-demo", "noexec") != 0);
    CHECK("action without Name refused", apps_launch_action("lord-demo", "nolabel") != 0);
    CHECK("path-like action refused", apps_launch_action("lord-demo", "../new-window") != 0);
    CHECK("empty action refused", apps_launch_action("lord-demo", "") != 0);
    CHECK("NULL action refused", apps_launch_action("lord-demo", NULL) != 0);
    CHECK("unknown app refused", apps_launch_action("lord-nope", "private") != 0);
    CHECK("action on a path id refused", apps_launch_action("../x", "private") != 0);

    CHECK("listed action launches", apps_launch_action("lord-demo", "private") == 0);
    snprintf(path, sizeof path, "%s/act-private", home);
    CHECK("it ran that action's Exec", appeared(path));
    snprintf(path, sizeof path, "%s/main", home);
    CHECK("not the main Exec", access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/act-unlisted", home);
    CHECK("the unlisted action never ran", access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/act-bad", home);
    CHECK("the badly named action never ran", access(path, F_OK) != 0);
    CHECK("plain launch still works", apps_launch("lord-demo") == 0);
    snprintf(path, sizeof path, "%s/main", home);
    CHECK("main Exec ran", appeared(path));

    /* a package that names its file freely ("Lord Spaced App.desktop"): listed as Lord-Spaced-App,
     * launched from its real file (0.8.5) */
    snprintf(path, sizeof path, "%s/.local/share/applications/Lord Spaced App.desktop", home);
    f = fopen(path, "w");
    if (f) { fprintf(f, "[Desktop Entry]\nType=Application\nName=Spaced\nExec=/usr/bin/touch %s/spaced\n", home); fclose(f); }
    const DesktopApp *list;
    int na = apps_list("en", &list), found = 0;
    for (int i = 0; i < na; i++) if (!strcmp(list[i].id, "Lord-Spaced-App")) found = 1;
    CHECK("a file name with spaces is listed, its id sanitized", found);
    CHECK("it launches", apps_launch("Lord-Spaced-App") == 0);
    snprintf(path, sizeof path, "%s/spaced", home);
    CHECK("its Exec ran", appeared(path));
    CHECK("the raw name with spaces is not an id", apps_launch("Lord Spaced App") != 0);

    /* a web address for the browser (lord.apps.launchUri): Cloud's GitHub issue form with its report is ~1.5 KB (#79) */
    snprintf(path, sizeof path, "%s/.local/share/applications/lord-web.desktop", home);
    f = fopen(path, "w");
    if (f) { fprintf(f, "[Desktop Entry]\nType=Application\nName=Web\nMimeType=x-scheme-handler/https;\nExec=/usr/bin/touch %s/web %%u\n", home); fclose(f); }
    char url[APPS_URI_WEB_MAX + 64];
    int ul = snprintf(url, sizeof url, "https://github.com/o/r/issues/new?template=x.yml&report=");
    while (ul < 2000) url[ul++] = 'a';
    url[ul] = 0;
    CHECK("a 2000-character web address launches", apps_launch_uri("lord-web", url) == 0);
    snprintf(path, sizeof path, "%s/web", home);
    CHECK("the browser's Exec ran", appeared(path));
    while (ul < APPS_URI_WEB_MAX + 1) url[ul++] = 'a';
    url[ul] = 0;
    CHECK("a web address over the limit is refused", apps_launch_uri("lord-web", url) != 0);
    CHECK("parentheses are refused (encode them)", apps_launch_uri("lord-web", "https://example.org/a(b)") != 0);
    CHECK("quotes and spaces are refused", apps_launch_uri("lord-web", "https://example.org/a'b") != 0 && apps_launch_uri("lord-web", "https://example.org/a b") != 0);
    char vnc[400];
    snprintf(vnc, sizeof vnc, "vnc://");
    for (int i = 6; i < 320; i++) vnc[i] = 'h';
    vnc[320] = 0;
    CHECK("other schemes keep 300 characters", apps_launch_uri("lord-web", vnc) != 0);
    CHECK("an app without the scheme is refused", apps_launch_uri("lord-demo", "https://example.org/") != 0);

    /* the name the keyboard shortcuts prompt shows (apps_name_of_program): only an entry whose Exec runs that very
     * program, or that Flatpak app; the window's app_id only says where to look first */
    char bin[600], real[4096], tru[4096], name[160];
    snprintf(path, sizeof path, "%s/bin", home);
    mkdir(path, 0700);
    snprintf(bin, sizeof bin, "%s/bin/viewer", home);
    f = fopen(bin, "w");
    if (f) { fputs("#!/bin/sh\n", f); fclose(f); }
    chmod(bin, 0755);
    snprintf(path, sizeof path, "%s/.local/bin", home);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.local/bin/flatpak", home);
    f = fopen(path, "w");
    if (f) { fputs("#!/bin/sh\n", f); fclose(f); }
    chmod(path, 0755);
    const char *entries[][2] = {
        { "org.example.Viewer", "Name=Viewer\nExec=env FOO=1 %s --x %%u\n" },
        { "liar", "Name=Lies\nStartupWMClass=viewer\nExec=/usr/bin/true\n" },
        { "org.example.Flat", "Name=Flat App\nExec=flatpak run --branch=stable --command=x org.example.Flat %%U\n" },
        { "org.example.Other", "Name=Other\nExec=flatpak run org.example.Flat.Extra\n" },
    };
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof path, "%s/.local/share/applications/%s.desktop", home, entries[i][0]);
        f = fopen(path, "w");
        if (!f) continue;
        fprintf(f, "[Desktop Entry]\nType=Application\n");
        fprintf(f, entries[i][1], bin);
        fclose(f);
    }
    CHECK("the program's realpath", realpath(bin, real) != NULL && realpath("/usr/bin/true", tru) != NULL);
    apps_name_of_program(real, NULL, "viewer", name, sizeof name);
    CHECK("the entry that runs it (past env), by the app_id's hint", !strcmp(name, "Viewer"));
    apps_name_of_program(real, NULL, NULL, name, sizeof name);
    CHECK("the entry that runs it, without a hint", !strcmp(name, "Viewer"));
    apps_name_of_program(real, NULL, "liar", name, sizeof name);
    CHECK("a hint that points elsewhere: still the entry that runs it", !strcmp(name, "Viewer"));
    apps_name_of_program(tru, NULL, "org.example.Viewer", name, sizeof name);
    CHECK("another program calling itself Viewer isn't Viewer", strcmp(name, "Viewer") != 0);
    apps_name_of_program("/nonexistent/viewer", NULL, "org.example.Viewer", name, sizeof name);
    CHECK("a program no entry runs: no name", !name[0]);
    apps_name_of_program(NULL, "org.example.Flat", NULL, name, sizeof name);
    CHECK("a Flatpak app: the entry that runs it", !strcmp(name, "Flat App"));
    apps_name_of_program(NULL, "org.example.Flat", "liar", name, sizeof name);
    CHECK("a Flatpak app, whatever the hint", !strcmp(name, "Flat App"));
    apps_name_of_program(NULL, "org.example.Fla", NULL, name, sizeof name);
    CHECK("a Flatpak id is matched whole", !name[0]);
    apps_name_of_program(NULL, "org.example.Viewer", NULL, name, sizeof name);
    CHECK("an entry named like the Flatpak id that doesn't run it: no name", !name[0]);
    apps_name_of_program("relative/x", NULL, "viewer", name, sizeof name);
    CHECK("a relative program: no name", !name[0]);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    if (system(cmd)) {}
    printf(bad ? "apps-actions: %d of %d failed\n" : "apps-actions: %d/%d ok\n", bad ? bad : n, n);
    return bad ? 1 : 0;
}
