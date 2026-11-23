/* perms_default_test.c — the one built-in permission that starts allowed (src/sec/perms.c, GitHub #219):
 *   - Screenshots' "screen" is allowed with no saved decision, and nothing else is (not its microphone, not another
 *     app's screen, not a package's);
 *   - perm_get, what the broker and every isolated app go by, never sees the default;
 *   - a "deny" saved in Settings → Privacy wins and sticks after a reload; "allow" saved explicitly stays allowed; taking
 *     the decision away (ask) brings the default back;
 *   - permisos.json gets no entry for the default itself. */
#include "sec/perms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, total;
static void check(int ok, const char *what) { total++; if (!ok) { fails++; printf("FAIL: %s\n", what); } }

static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--read")) {       /* a fresh process: what a new session reads */
        perm_init(argv[2]);
        return perm_get_builtin("screenshots", PERM_SCREEN) == PERM_DENY && perm_get("screenshots", PERM_SCREEN) == PERM_DENY ? 0 : 1;
    }
    const char *self = argv[0];
    char home[] = "/tmp/perms-default-XXXXXX";
    if (!mkdtemp(home)) { perror("mkdtemp"); return 1; }
    char p[512];
    snprintf(p, sizeof p, "%s/.config", home);
    mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/.config/lordos", home);
    mkdir(p, 0700);
    perm_init(home);

    check(perm_builtin_default("screenshots", PERM_SCREEN) == PERM_ALLOW, "Screenshots' screen starts allowed");
    check(perm_builtin_default("screenshots", PERM_MICROPHONE) == PERM_ASK, "not its microphone");
    check(perm_builtin_default("screenshots", PERM_CAMERA) == PERM_ASK, "not its camera");
    check(perm_builtin_default("photos", PERM_SCREEN) == PERM_ASK, "not another built-in app's screen");
    check(perm_builtin_default("com.example.shots", PERM_SCREEN) == PERM_ASK, "not a package's screen");
    check(perm_builtin_default("screenshotsx", PERM_SCREEN) == PERM_ASK && perm_builtin_default("screenshot", PERM_SCREEN) == PERM_ASK,
          "the id exactly, not a prefix");
    check(perm_builtin_default(NULL, PERM_SCREEN) == PERM_ASK, "no app: no default");

    check(perm_get_builtin("screenshots", PERM_SCREEN) == PERM_ALLOW, "with nothing saved the built-in check allows it");
    check(perm_get("screenshots", PERM_SCREEN) == PERM_ASK, "perm_get (the broker's) never sees the default");
    check(perm_get_builtin("photos", PERM_SCREEN) == PERM_ASK, "another app still asks");

    snprintf(p, sizeof p, "%s/.config/lordos/permisos.json", home);
    check(access(p, F_OK) != 0, "the default writes nothing");

    /* revoked in Settings → Privacy: Don't Allow */
    check(perm_set("screenshots", PERM_SCREEN, PERM_DENY) == 0, "deny saved");
    check(perm_get_builtin("screenshots", PERM_SCREEN) == PERM_DENY, "a saved deny wins over the default");
    char *j = slurp(p);
    check(j && strstr(j, "\"screenshots\"") && strstr(j, "\"deny\""), "and it is in permisos.json");

    /* a new session (this program again, with an empty table) reads it back: the revocation sticks */
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "'%s' --read '%s'", self, home);
    check(system(cmd) == 0, "the deny sticks in a new session");

    check(perm_set("screenshots", PERM_SCREEN, PERM_ALLOW) == 0 && perm_get_builtin("screenshots", PERM_SCREEN) == PERM_ALLOW,
          "allowed again");
    check(perm_set("screenshots", PERM_SCREEN, PERM_ASK) == 0 && perm_get_builtin("screenshots", PERM_SCREEN) == PERM_ALLOW,
          "no decision: the default again");

    unlink(p);
    snprintf(p, sizeof p, "%s/.config/lordos", home);
    rmdir(p);
    snprintf(p, sizeof p, "%s/.config", home);
    rmdir(p);
    rmdir(home);
    printf("perms-default-test: %d/%d\n", total - fails, total);
    return fails ? 1 : 0;
}
