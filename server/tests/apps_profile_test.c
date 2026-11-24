/* apps_profile_test.c — lord.apps.launchProfile (src/apps.c apps_launch_profile; approved by the maintainer 2026-11-24):
 * a Chromium-family browser opened in one of its own profiles. A fake system tree (defaults_set_root) has
 * usr/share/applications/google-chrome.desktop whose Exec is a recorder script; a temporary HOME has its Local State.
 * Checked: the argv is the Exec line's plus exactly one "--profile-directory=<profile>"; a profile name not of the
 * browser's form, or not in its Local State, is refused; a hostile Local State (not JSON, a link, a FIFO, oversize, the
 * wrong shape, a key that is not an object) is refused; a browser outside the table, or one whose .desktop is the user's
 * own (~/.local/share/applications) is refused; nothing else is ever run. */
#include "apps.h"
#include "fsops.h"
#include "fw/rules.h"
#include "hw/defaults.h"
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
static char home[64], root[64], out[256];

static int appeared(const char *path) {
    for (int i = 0; i < 100; i++) {
        if (!access(path, F_OK)) return 1;
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}
static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}
static void local_state(const char *text) {
    char p[512];
    snprintf(p, sizeof p, "%s/.config/google-chrome/Local State", home);
    unlink(p);
    if (text) write_file(p, text);
}
/* launches and returns what the recorder got ("" if it never ran), one argument per line */
static const char *run(const char *id, const char *profile, char *got, size_t cap) {
    unlink(out);
    got[0] = 0;
    if (apps_launch_profile(id, profile) != 0) return got;
    if (!appeared(out)) return got;
    struct timespec ts = { 0, 60 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    FILE *f = fopen(out, "r");
    if (f) { size_t r = fread(got, 1, cap - 1, f); got[r] = 0; fclose(f); }
    return got;
}

int main(void) {
    snprintf(home, sizeof home, "/tmp/lord-profile-home-XXXXXX");
    snprintf(root, sizeof root, "/tmp/lord-profile-root-XXXXXX");
    if (!mkdtemp(home) || !mkdtemp(root)) { perror("mkdtemp"); return 1; }
    setenv("HOME", home, 1);
    unsetenv("LORDOS_SESSIOND");
    char p[512], got[1024];
    const char *hd[] = { "%s/.config", "%s/.config/google-chrome", "%s/.local", "%s/.local/share", "%s/.local/share/applications", NULL };
    for (int i = 0; hd[i]; i++) { snprintf(p, sizeof p, hd[i], home); mkdir(p, 0700); }
    const char *rd[] = { "%s/usr", "%s/usr/share", "%s/usr/share/applications", NULL };
    for (int i = 0; rd[i]; i++) { snprintf(p, sizeof p, rd[i], root); mkdir(p, 0755); }
    snprintf(out, sizeof out, "%s/argv", home);
    /* the recorder: writes its arguments, one per line */
    char rec[512], text[2048];
    snprintf(rec, sizeof rec, "%s/recorder", root);
    snprintf(text, sizeof text, "#!/bin/sh\nfor a in \"$@\"; do printf '%%s\\n' \"$a\"; done > '%s'\n", out);
    write_file(rec, text);
    chmod(rec, 0755);
    snprintf(text, sizeof text, "[Desktop Entry]\nType=Application\nName=Google Chrome\nExec=%s --first %%U\n", rec);
    snprintf(p, sizeof p, "%s/usr/share/applications/google-chrome.desktop", root);
    write_file(p, text);
    snprintf(p, sizeof p, "%s/usr/share/applications/gedit.desktop", root);
    write_file(p, "[Desktop Entry]\nType=Application\nName=Gedit\nExec=/bin/true\n");
    /* a user .desktop for another browser of the table: never used */
    snprintf(text, sizeof text, "[Desktop Entry]\nType=Application\nName=Chromium\nExec=%s --user-entry\n", rec);
    snprintf(p, sizeof p, "%s/.local/share/applications/chromium.desktop", home);
    write_file(p, text);
    defaults_set_root(root);
    restrict_set_store(fw_store_new());

    local_state("{\"profile\":{\"info_cache\":{\"Default\":{\"name\":\"Personal\"},\"Profile 2\":{\"name\":\"Dev\"}},\"last_used\":\"Default\"}}");
    CHECK("names: the browser's own form only", apps_profile_name_ok("Default") && apps_profile_name_ok("Profile 1") && apps_profile_name_ok("Profile 9999")
          && !apps_profile_name_ok("Profile 0") && !apps_profile_name_ok("Profile 01") && !apps_profile_name_ok("Profile 10000")
          && !apps_profile_name_ok("profile 1") && !apps_profile_name_ok("Profile 1 ") && !apps_profile_name_ok("../Default")
          && !apps_profile_name_ok("Default --incognito") && !apps_profile_name_ok("") && !apps_profile_name_ok(NULL));
    run("google-chrome", "Profile 2", got, sizeof got);
    CHECK("argv: the Exec line's, then one --profile-directory", !strcmp(got, "--first\n--profile-directory=Profile 2\n"));
    if (strcmp(got, "--first\n--profile-directory=Profile 2\n")) printf("  got: %s\n", got);
    run("google-chrome", "Default", got, sizeof got);
    CHECK("Default", !strcmp(got, "--first\n--profile-directory=Default\n"));
    CHECK("a folder the Local State doesn't list", !*run("google-chrome", "Profile 3", got, sizeof got));
    CHECK("a name not of the browser's form", !*run("google-chrome", "Profile 2 --incognito", got, sizeof got));
    CHECK("not a browser of the table", !*run("gedit", "Default", got, sizeof got));
    CHECK("a user .desktop of a table browser is refused", !*run("chromium", "Default", got, sizeof got));
    CHECK("an unknown id", !*run("../google-chrome", "Default", got, sizeof got));
    /* hostile Local State */
    local_state("{\"profile\":{\"info_cache\":{\"Profile 2\":");
    CHECK("Local State not JSON", !*run("google-chrome", "Profile 2", got, sizeof got));
    local_state("{\"profile\":{\"info_cache\":[\"Profile 2\"]}}");
    CHECK("Local State of the wrong shape", !*run("google-chrome", "Profile 2", got, sizeof got));
    local_state("{\"profile\":{\"info_cache\":{\"Profile 2\":\"x\"}}}");
    CHECK("a listed key that isn't an object", !*run("google-chrome", "Profile 2", got, sizeof got));
    local_state("{\"profile\":{\"info_cache\":{\"Profile 2\":{},\"Profile 2\":{}}}}");
    CHECK("duplicate keys refused by the parser", !*run("google-chrome", "Profile 2", got, sizeof got));
    {
        char *big = malloc(1024 * 1024 + 64);
        if (big) {
            strcpy(big, "{\"profile\":{\"info_cache\":{\"Profile 2\":{}}},\"pad\":\"");
            size_t l = strlen(big);
            memset(big + l, 'x', 1024 * 1024);
            strcpy(big + l + 1024 * 1024, "\"}");
            local_state(big);
            free(big);
            CHECK("Local State over 1 MiB", !*run("google-chrome", "Profile 2", got, sizeof got));
        }
    }
    local_state(NULL);
    char real[512], link[512];
    snprintf(real, sizeof real, "%s/elsewhere.json", home);
    write_file(real, "{\"profile\":{\"info_cache\":{\"Profile 2\":{}}}}");
    snprintf(link, sizeof link, "%s/.config/google-chrome/Local State", home);
    if (symlink(real, link)) perror("symlink");
    CHECK("Local State that is a link", !*run("google-chrome", "Profile 2", got, sizeof got));
    unlink(link);
    if (mkfifo(link, 0600)) perror("mkfifo");
    CHECK("Local State that is a FIFO (no hang)", !*run("google-chrome", "Profile 2", got, sizeof got));
    unlink(link);
    CHECK("no Local State", !*run("google-chrome", "Default", got, sizeof got));

    snprintf(p, sizeof p, "rm -rf '%s' '%s'", home, root);
    if (system(p)) {}
    printf("apps-profile-test: %s (%d checks)\n", bad ? "FAILED" : "ok", n);
    return bad != 0;
}
