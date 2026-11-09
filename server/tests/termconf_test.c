/* termconf_test.c — the Terminal's configuration file (src/termconf.c): the default and a chosen file, paths that are
 * refused, a missing file and a place that isn't there (nothing created in it), atomic 0600 writes, the backups, the
 * migration out of settings.json and its backup, and the protection from lord.fs (fsops). A temporary home.
 * `make build/termconf-test` */
#include "../src/termconf.h"
#include "../src/fsops.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails, total;
static void check(int ok, const char *what) { total++; printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static char H[PATH_MAX];
static int exists(const char *rel) { char p[PATH_MAX + 64]; struct stat st; snprintf(p, sizeof p, "%s%s", H, rel); return !lstat(p, &st); }
static int mode_of(const char *rel) { char p[PATH_MAX + 64]; struct stat st; snprintf(p, sizeof p, "%s%s", H, rel); return lstat(p, &st) ? -1 : (int)(st.st_mode & 0777); }
static void put(const char *rel, const char *txt) { char p[PATH_MAX + 64]; snprintf(p, sizeof p, "%s%s", H, rel); FILE *f = fopen(p, "w"); if (f) { fputs(txt, f); fclose(f); } }
static void mk(const char *rel) { char p[PATH_MAX + 64]; snprintf(p, sizeof p, "%s%s", H, rel); mkdir(p, 0700); }
static void age(const char *rel, int secs) {
    char p[PATH_MAX + 64]; snprintf(p, sizeof p, "%s%s", H, rel);
    struct timespec t[2]; clock_gettime(CLOCK_REALTIME, &t[0]); t[0].tv_sec -= secs; t[1] = t[0];
    utimensat(AT_FDCWD, p, t, 0);
}
static int has_code(const char *err, const char *code) { return !strncmp(err, code, strlen(code)); }

int main(void) {
    char tmpl[] = "/tmp/termconf-test-XXXXXX";
    if (!mkdtemp(tmpl) || !realpath(tmpl, H)) return 1;
    fsops_init(H, "nobody");
    termconf_init(H);
    char err[256], p[PATH_MAX + 64];
    size_t len;
    double mt;

    /* the default file */
    check(!termconf_is_custom() && strstr(termconf_path(), "/.config/lordos/terminal.json"), "default: ~/.config/lordos/terminal.json");
    check(!termconf_read(termconf_path(), &len, &mt, err, sizeof err) && has_code(err, "termconf.unavailable"), "no ~/.config/lordos yet: unavailable");
    check(!termconf_write(termconf_path(), "{\"schema\":1}", 12, &mt, err, sizeof err), "default: written (its folders are made)");
    check(mode_of("/.config/lordos/terminal.json") == 0600, "0600");
    char *t = termconf_read(termconf_path(), &len, &mt, err, sizeof err);
    check(t && len == 12 && !strcmp(t, "{\"schema\":1}") && mt > 0, "read back");
    free(t);

    /* protection from lord.fs: the file and its family, settings.json's family */
    snprintf(p, sizeof p, "%s/.config/lordos/terminal.json", H);
    check(fsops_protected(p), "lord.fs: terminal.json protected");
    snprintf(p, sizeof p, "%s/.config/lordos/terminal.json.3", H);
    check(fsops_protected(p), "lord.fs: its backups too");
    snprintf(p, sizeof p, "%s/.config/lordos/terminal.jsonx", H);
    check(!fsops_protected(p), "lord.fs: a name that only starts the same isn't");
    snprintf(p, sizeof p, "%s/.config/lordos/settings.json.pre-terminal-migration.bak", H);
    check(fsops_protected(p), "lord.fs: settings.json's migration backup protected");

    /* backups: the first write after one exists keeps a .1; within 10 minutes no new one */
    check(!termconf_write(termconf_path(), "{\"schema\":1,\"v\":2}", 18, &mt, err, sizeof err) && exists("/.config/lordos/terminal.json.1"), "backup .1 made");
    check(mode_of("/.config/lordos/terminal.json.1") == 0600, "backup 0600");
    termconf_write(termconf_path(), "{\"v\":3}", 7, &mt, err, sizeof err);
    check(!exists("/.config/lordos/terminal.json.2"), "no new backup within 10 minutes");
    for (int i = 0; i < 7; i++) { age("/.config/lordos/terminal.json.1", 700); termconf_write(termconf_path(), "{\"v\":4}", 7, &mt, err, sizeof err); }
    check(exists("/.config/lordos/terminal.json.5") && !exists("/.config/lordos/terminal.json.6"), "five backups at most");

    /* a chosen file */
    check(termconf_set_custom("relative/terminal.json", err, sizeof err) && has_code(err, "termconf.badPath"), "relative path refused");
    snprintf(p, sizeof p, "%s/Backups/../terminal.json", H);
    check(termconf_set_custom(p, err, sizeof err) && has_code(err, "termconf.badPath"), "'..' refused");
    snprintf(p, sizeof p, "%s/Backups/terminal.txt", H);
    check(termconf_set_custom(p, err, sizeof err) && has_code(err, "termconf.badPath"), "not .json refused");
    check(termconf_set_custom("/etc/lordos-terminal.json", err, sizeof err) && has_code(err, "fs.outsideHome"), "outside the home refused");
    snprintf(p, sizeof p, "%s/.config/lordos/settings.json", H);
    check(termconf_set_custom(p, err, sizeof err) && has_code(err, "fs.protected"), "settings.json refused");
    mk("/.config/lordos/mail");
    snprintf(p, sizeof p, "%s/.config/lordos/mail/x.json", H);
    check(termconf_set_custom(p, err, sizeof err) && has_code(err, "fs.protected"), "a protected folder refused");

    mk("/Backups"); mk("/Backups/LordOS-Terminal");
    snprintf(p, sizeof p, "%s/Backups/LordOS-Terminal/terminal.json", H);
    check(!termconf_set_custom(p, err, sizeof err) && termconf_is_custom() && !strcmp(termconf_path(), p), "chosen file set");
    check(fsops_protected(p), "lord.fs: the chosen file protected");
    snprintf(p, sizeof p, "%s/Backups/LordOS-Terminal", H);
    check(fsops_protected_below(p), "lord.fs: its folder can't be moved by a generic operation");
    check(!termconf_read(termconf_path(), &len, &mt, err, sizeof err) && has_code(err, "termconf.missing"), "chosen, not written yet: missing");
    check(!termconf_write(termconf_path(), "{\"chosen\":1}", 12, &mt, err, sizeof err), "chosen file written");
    check(mode_of("/Backups/LordOS-Terminal/terminal.json") == 0600, "chosen file 0600");
    snprintf(p, sizeof p, "%s/Backups/LordOS-Terminal/terminal.json", H);
    check(!termconf_set_custom(p, err, sizeof err), "choosing the same file again is fine");

    /* the place goes away (renamed folder, disk not mounted): unavailable, and nothing is created there */
    char a[PATH_MAX + 64], b[PATH_MAX + 64];
    snprintf(a, sizeof a, "%s/Backups/LordOS-Terminal", H);
    snprintf(b, sizeof b, "%s/Backups/Renamed", H);
    rename(a, b);
    check(!termconf_read(termconf_path(), &len, &mt, err, sizeof err) && has_code(err, "termconf.unavailable"), "folder gone: unavailable");
    check(termconf_write(termconf_path(), "{}", 2, &mt, err, sizeof err) && has_code(err, "termconf.unavailable"), "folder gone: not written");
    check(!exists("/Backups/LordOS-Terminal"), "folder gone: not created");
    t = termconf_read(termconf_cache(), &len, &mt, err, sizeof err);
    check(t != NULL, "the cache is still there");
    free(t);
    snprintf(p, sizeof p, "%s/Media-not-mounted/terminal.json", H);
    check(!termconf_set_custom(p, err, sizeof err), "a file in a folder that isn't there yet can be chosen (a disk mounted later)");

    /* a link or a folder in its place */
    termconf_set_custom("", err, sizeof err);
    check(!termconf_is_custom(), "back to the default");
    mk("/Links");
    snprintf(a, sizeof a, "%s/Links/terminal.json", H);
    snprintf(b, sizeof b, "%s/.config/lordos/terminal.json", H);
    if (symlink(b, a)) perror("symlink");
    termconf_set_custom(a, err, sizeof err);
    check(!termconf_read(termconf_path(), &len, &mt, err, sizeof err) && has_code(err, "termconf.notRegular"), "a link is not followed");
    check(termconf_write(termconf_path(), "{}", 2, &mt, err, sizeof err) && has_code(err, "termconf.notRegular"), "a link is not replaced");
    termconf_set_custom("", err, sizeof err);

    /* an export: 0600 anywhere of the user's, never over the active file */
    snprintf(p, sizeof p, "%s/profiles-export.json", H);
    check(!termconf_export(p, "{}", 2, err, sizeof err) && mode_of("/profiles-export.json") == 0600, "export written 0600");
    check(termconf_export(termconf_path(), "{}", 2, err, sizeof err) && has_code(err, "fs.protected"), "export over the active file refused");
    check(termconf_export("/etc/x.json", "{}", 2, err, sizeof err) && has_code(err, "fs.outsideHome"), "export outside the home refused");

    /* the migration */
    mk("/.config");
    put("/.config/lordos/settings.json", "{\"terminal\":{\"profiles\":[{\"id\":\"x\",\"itermGuid\":\"G\"}]}}");
    check(termconf_migrate("{}", 2, err, sizeof err) && has_code(err, "termconf.exists"), "migration refused: terminal.json already there");
    snprintf(p, sizeof p, "%s/.config/lordos", H);
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof cmd, "rm -f %s/terminal.json*", p);
    if (system(cmd)) perror("rm");
    const char *mig = "{\"schema\":1,\"profiles\":[{\"id\":\"x\",\"itermGuid\":\"G\"}]}";
    check(!termconf_migrate(mig, strlen(mig), err, sizeof err), "migrated");
    check(exists("/.config/lordos/settings.json.pre-terminal-migration.bak"), "settings.json backed up first");
    check(mode_of("/.config/lordos/settings.json.pre-terminal-migration.bak") == 0600, "backup 0600");
    t = termconf_read(termconf_cache(), &len, &mt, err, sizeof err);
    check(t && !strcmp(t, mig), "terminal.json has every field (itermGuid too)");
    free(t);
    put("/.config/lordos/settings.json", "{\"later\":1}");
    if (system(cmd)) perror("rm");
    termconf_migrate(mig, strlen(mig), err, sizeof err);
    snprintf(p, sizeof p, "%s/.config/lordos/settings.json.pre-terminal-migration.bak", H);
    FILE *f = fopen(p, "r");
    char back[256] = "";
    if (f) { if (!fgets(back, sizeof back, f)) back[0] = 0; fclose(f); }
    check(strstr(back, "itermGuid") != NULL, "an existing migration backup is kept, not overwritten");

    snprintf(cmd, sizeof cmd, "rm -rf %s", H);
    if (system(cmd)) perror("rm");
    printf("termconf-test: %d/%d\n", total - fails, total);
    return fails != 0;
}
