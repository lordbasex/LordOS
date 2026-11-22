/* fplend_test.c — lord-server's checks before a chosen file is lent to a Flatpak app (fplend_check in src/fplend.c;
 * docs/FLATPAK.md "Lending a chosen file"). Claude Security F1/F2 (2026-11-22): the policy compares path strings, so a
 * path the app spelt "/home/u/./.config" or "/home/u//.ssh" (through FileChooser's current_folder) must never reach it:
 *   1. pathcanon.h: what a canonical path is;
 *   2. "/./", "//", "..", a trailing '/' or '.' and a link on the way are refused, for a file, a folder and a save,
 *      even when the stubbed grant rule below (a plain string compare, as apps.c's) would let them through;
 *   3. the plain spelling of a protected folder is refused too, and an ordinary file, folder and save are accepted with
 *      the right dev/ino; a save makes its target empty and names the file itself, never its folder (F5). */
#include "../src/fplend.h"
#include "../src/fsops.h"
#include "../src/sec/pathcanon.h"
#include "../src/sec/sandbox.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static char home[1024];

/* the launcher's grant rule, as a plain string compare like apps.c's grant_ok (no canonical check here on purpose:
 * fplend_check must refuse a non-canonical path before it asks) */
int apps_grant_ok(const char *path) {
    static const char *const deny[] = { "/.ssh", "/.config/autostart", "/.aws", NULL };
    char full[1200];
    if (!strcmp(path, home)) return 0;
    for (int i = 0; deny[i]; i++) {
        snprintf(full, sizeof full, "%s%s", home, deny[i]);
        size_t n = strlen(full), m = strlen(path);
        if ((!strncmp(path, full, n) && (path[n] == '/' || !path[n])) || (!strncmp(full, path, m) && (full[m] == '/' || !full[m]))) return 0;
    }
    return 1;
}
/* not reached by fplend_check */
int appid_of_pid(unsigned pid, char *out, size_t n) { (void)pid; (void)n; out[0] = 0; return -1; }

static void put(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { perror(path); exit(1); }
    if (write(fd, text, strlen(text)) < 0) {}
    close(fd);
}

static int check(const char *rel, int kind, SbLendMsg *q) {
    char p[2048];
    snprintf(p, sizeof p, "%s%s", home, rel);
    return fplend_check("org.example.App", p, kind, q);
}

int main(void) {
    /* 1 */
    CHECK(path_canonical("/") && path_canonical("/a") && path_canonical("/a/b.c") && path_canonical("/a/.b") &&
          path_canonical("/a/..b") && path_canonical("/a/b.."), "1: plain paths are canonical");
    CHECK(!path_canonical(NULL) && !path_canonical("") && !path_canonical("a/b") && !path_canonical("//") &&
          !path_canonical("/a//b") && !path_canonical("/a/./b") && !path_canonical("/a/../b") && !path_canonical("/a/") &&
          !path_canonical("/a/.") && !path_canonical("/a/..") && !path_canonical("/.") && !path_canonical("//a"),
          "1: empty, '.' and '..' elements and a '/' at the end are not");

    char tmpl[] = "/tmp/fplend-test.XXXXXX";
    if (!mkdtemp(tmpl) || !realpath(tmpl, home)) { perror("mkdtemp"); return 1; }
    fsops_init(home, "tester");
    char p[2048];
    const char *dirs[] = { "/.config", "/.config/autostart", "/.ssh", "/.aws", "/Docs", "/Docs/Sub", NULL };
    for (int i = 0; dirs[i]; i++) { snprintf(p, sizeof p, "%s%s", home, dirs[i]); mkdir(p, 0700); }
    snprintf(p, sizeof p, "%s/.ssh/config", home); put(p, "Host *");
    snprintf(p, sizeof p, "%s/.aws/credentials", home); put(p, "secret");
    snprintf(p, sizeof p, "%s/Docs/a.txt", home); put(p, "a");
    snprintf(p, sizeof p, "%s/link", home); if (symlink(".ssh", p)) {}
    SbLendMsg *q = calloc(1, sizeof *q);

    /* 2: the bypass spellings (F1/F2), each a folder lent writable before */
    const char *odd[] = { "/./.config", "//.ssh", "/./.config/autostart", "/.config//autostart", "/.config/autostart/.",
                          "/.ssh/", "/Docs/../.ssh", "/./Docs", "//Docs", NULL };
    for (int i = 0; odd[i]; i++) {
        char what[256];
        snprintf(what, sizeof what, "2: folder \"~%s\" refused", odd[i]);
        CHECK(check(odd[i], FPLEND_DIR, q) == -1, what);
    }
    CHECK(check("/./.ssh/config", FPLEND_FILE, q) == -1, "2: a file through \"/./\" refused");
    CHECK(check("//Docs/a.txt", FPLEND_FILE, q) == -1, "2: a file through \"//\" refused");
    CHECK(check("/./.ssh/config", FPLEND_SAVE, q) == -1, "2: a save through \"/./\" refused");
    CHECK(check("//.ssh/config", FPLEND_SAVE, q) == -1, "2: a save through \"//\" refused");
    CHECK(check("/.ssh//config", FPLEND_SAVE, q) == -1, "2: a save with \"//\" before the name refused");
    CHECK(check("/./.aws/notes.txt", FPLEND_SAVE, q) == -1, "2: a save into ~/./.aws refused");
    snprintf(p, sizeof p, "%s/./.aws/notes.txt", home);
    CHECK(access(p, F_OK) != 0, "2: and no file was made there");
    CHECK(check("/link/config", FPLEND_FILE, q) == -1, "2: a link on the way refused");
    CHECK(check("/link", FPLEND_DIR, q) == -1, "2: a link as the folder refused");

    /* 3: the plain spellings: protected and sensitive still refused, ordinary ones lent */
    CHECK(check("/.config", FPLEND_DIR, q) == -1, "3: ~/.config (holds autostart) refused");
    CHECK(check("/.config/autostart", FPLEND_DIR, q) == -1, "3: ~/.config/autostart refused");
    CHECK(check("/.ssh", FPLEND_DIR, q) == -1, "3: ~/.ssh refused");
    CHECK(check("/.ssh/config", FPLEND_SAVE, q) == -1, "3: a save over ~/.ssh/config refused");
    CHECK(check("/.aws/notes.txt", FPLEND_SAVE, q) == -1, "3: a save into ~/.aws refused");
    CHECK(check("/.config/autostart/x.desktop", FPLEND_SAVE, q) == -1, "3: a save into autostart refused");
    snprintf(p, sizeof p, "%s/.config/autostart/x.desktop", home);
    CHECK(access(p, F_OK) != 0, "3: and nothing was made there");
    CHECK(check("", FPLEND_DIR, q) == -1, "3: the home folder itself refused");
    struct stat st;
    snprintf(p, sizeof p, "%s/Docs/a.txt", home); stat(p, &st);
    CHECK(check("/Docs/a.txt", FPLEND_FILE, q) == 0 && q->rw == 1 && q->dev == (uint64_t)st.st_dev &&
          q->ino == (uint64_t)st.st_ino && !strcmp(q->path, p) && !strcmp(q->name, "a.txt"), "3: an ordinary file is lent writable");
    CHECK(check("/.ssh/config", FPLEND_FILE, q) == 0 && q->rw == 0, "3: a file the person chose in ~/.ssh is lent read-only");
    snprintf(p, sizeof p, "%s/Docs/Sub", home); stat(p, &st);
    CHECK(check("/Docs/Sub", FPLEND_DIR, q) == 0 && q->ino == (uint64_t)st.st_ino, "3: an ordinary folder is lent");
    CHECK(check("/Docs/new.txt", FPLEND_SAVE, q) == 0, "3: a save in an ordinary folder is lent");
    snprintf(p, sizeof p, "%s/Docs/new.txt", home);
    CHECK(!stat(p, &st) && S_ISREG(st.st_mode) && st.st_size == 0, "3: its target was made empty");
    CHECK(!strcmp(q->path, p) && q->ino == (uint64_t)st.st_ino && q->tino == q->ino && q->rw == 1 && q->kind == FPLEND_SAVE,
          "3: a save names the file itself, never its folder (F5)");
    CHECK(check("/saved.txt", FPLEND_SAVE, q) == 0, "3: a save right in the home folder is lent (only the file goes)");
    CHECK(check("/Docs/a.txt", 9, q) == -1, "3: an unknown kind refused");

    free(q);
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    if (system(cmd)) {}
    printf("fplend-test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
