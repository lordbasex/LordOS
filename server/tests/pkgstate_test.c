/* pkgstate_test.c — the signing "business" flow in C: install, detect "Modified by
 * you", restore, share (signed derivative) and the update-by-key rule. Works
 * in $HOME (a VM), with a real .lpk passed as an argument (made with tools/lpk.mjs).
 *
 *   build/pkgstate-test <app.lpk> <id>
 */
#include "sec/pkgstate.h"
#include "sec/sign.h"
#include "lpk.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define OK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); putchar('\n'); } else { printf("ok: "); printf(__VA_ARGS__); putchar('\n'); } } while (0)

static int install(const char *path, const char *id) {
    Lpk pk;
    char err[160];
    if (lpk_open(path, &pk, err, sizeof err)) { printf("does not open: %s\n", err); return -1; }
    int r = lpk_install(&pk, id, err, sizeof err);
    if (!r) r = pkg_keep_original(id, path);
    lpk_free(&pk);
    return r;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <app.lpk> <id>\n", argv[0]); return 2; }
    const char *path = argv[1], *id = argv[2];
    const char *home = getenv("HOME");

    OK(install(path, id) == 0, "install %s", id);
    PkgStatus st;
    OK(!pkg_status(id, &st) && st.installed && st.has_original && !st.modified, "just installed: no changes (trust=%s, signature=%d)", sig_trust_name(st.sig.trust), st.sig.has_signature);

    /* modify an installed file */
    char f[512];
    snprintf(f, sizeof f, "%s/.local/share/axia/apps/%s/app.js", home, id);
    int fd = open(f, O_WRONLY | O_APPEND);
    if (fd >= 0) { if (write(fd, "\n/*tocado*/\n", 12)) {} close(fd); }
    OK(!pkg_status(id, &st) && st.modified && st.nchanged >= 1, "after editing app.js: \"Modified by you\" (%d changes)", st.nchanged);
    int sawAppjs = 0;
    for (int i = 0; i < st.nchanged; i++) if (!strcmp(st.changed[i], "app.js") && st.change_kind[i] == 'm') sawAppjs = 1;
    OK(sawAppjs, "the detected change is app.js (modified)");

    /* add a new file */
    snprintf(f, sizeof f, "%s/.local/share/axia/apps/%s/extra.txt", home, id);
    fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { if (write(fd, "x", 1)) {} close(fd); }
    pkg_status(id, &st);
    int sawExtra = 0;
    for (int i = 0; i < st.nchanged; i++) if (!strcmp(st.changed[i], "extra.txt") && st.change_kind[i] == 'a') sawExtra = 1;
    OK(sawExtra, "detects the added file extra.txt");

    /* share my version: an .lpk signed with the user's key, with basedOn */
    char out[512], manifest[512], err[160];
    snprintf(out, sizeof out, "%s/mia.lpk", home);
    unlink(out);
    int ml = snprintf(manifest, sizeof manifest,
        "{\"id\":\"%s\",\"version\":\"1.0.0+mia\",\"name\":\"Mia\",\"icon\":\"icon.png\",\"basedOn\":\"%s@1.0.0 by LordOS\"}", id, id);
    OK(pkg_share(id, manifest, (size_t)ml, out, err, sizeof err) == 0, "share my version: %s", err[0] ? err : "ok");
    /* the derivative verifies with the user's key */
    Lpk dv;
    if (!lpk_open(out, &dv, err, sizeof err)) {
        LpkSigInfo si;
        int r = sig_verify_package(&dv, id, "1.0.0+mia", &si, err, sizeof err);
        OK(!r && si.has_signature && si.trust == LPK_SIGNED, "the derivative is signed by the user (%s): %s", si.key, r ? err : "ok");
        lpk_free(&dv);
    } else OK(0, "the derivative does not open: %s", err);

    /* restore the original: back exactly, no changes */
    OK(pkg_restore(id, err, sizeof err) == 0, "restore: %s", err[0] ? err : "ok");
    OK(!pkg_status(id, &st) && !st.modified && st.nchanged == 0, "after restoring: no changes");

    /* update rule: another key is rejected if the installed one is signed */
    LpkSigInfo next;
    memset(&next, 0, sizeof next);
    if (st.sig.has_signature) {
        next.has_signature = 1;
        snprintf(next.key, sizeof next.key, "%063d1", 0);     /* another key */
        OK(pkg_check_update(id, &next, err, sizeof err) != 0, "update with another key rejected: %s", err);
        snprintf(next.key, sizeof next.key, "%s", st.sig.key);
        OK(pkg_check_update(id, &next, err, sizeof err) == 0, "update with the same key accepted");
    }

    pkg_forget(id);
    lpk_remove(id);
    printf("pkgstate-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
