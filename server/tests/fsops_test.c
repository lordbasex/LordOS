/* fsops_test.c — test of fsops.c on a temporary home folder: roots and protected
 * folders, copy (recursive, links, permissions), move, Trash (put, list, restore,
 * delete, empty), size, search and recents. */
#include "fsops.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char H[256];

static void put(const char *rel, const char *text) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", H, rel);
    FILE *f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}
static int exists(const char *rel) { char p[512]; struct stat st; snprintf(p, sizeof p, "%s/%s", H, rel); return lstat(p, &st) == 0; }
static void path(char *out, const char *rel) { sprintf(out, "%s/%s", H, rel); }

static void wait_job(int id) {
    for (int k = 0; k < 400; k++) {
        for (int i = 0; i < FSJOB_MAX; i++) {
            const FsJob *j = fsjob_get(i);
            if (j && j->id == id && __atomic_load_n(&j->done, __ATOMIC_ACQUIRE)) {
                if (j->failed) fprintf(stderr, "tarea %d: %s\n", id, j->error);
                CHECK(!j->failed);
                fsjob_forget(id);
                return;
            }
        }
        usleep(5000);
    }
    CHECK(!"the task did not finish");
}

static int trash_n;
static char trash_id[1024];
static void count_cb(void *ud, const char *id, const char *name, const char *orig, const char *date, int dir, double size) {
    (void)ud; (void)name; (void)date; (void)dir; (void)size;
    trash_n++;
    snprintf(trash_id, sizeof trash_id, "%s", id);
    CHECK(orig[0] == '/');
}
static int hits;
static void hit_cb(void *ud, const char *p, int dir, double size, double mtime) { (void)ud; (void)p; (void)dir; (void)size; (void)mtime; hits++; }

int main(void) {
    snprintf(H, sizeof H, "/tmp/fsops-test-%d", (int)getpid());
    mkdir(H, 0755);
    fsops_init(H, "nadie");
    char a[512], b[512], err[256];

    /* roots and protected folders */
    CHECK(fsops_allowed(H));
    CHECK(!fsops_allowed("/etc/passwd"));
    CHECK(!fsops_allowed("/tmp"));
    /* reading only: the whole tree but the pseudo-filesystems (Files → This computer) */
    CHECK(fsops_readable("/"));
    CHECK(fsops_readable("/etc/passwd"));
    CHECK(fsops_readable("/usr/share"));
    CHECK(fsops_readable(H));
    CHECK(fsops_readable("/run/media/nadie/USB"));
    CHECK(fsops_readable("/procs"));
    CHECK(!fsops_readable("/proc"));
    CHECK(!fsops_readable("/proc/self/mem"));
    CHECK(!fsops_readable("/sys/class"));
    CHECK(!fsops_readable("/dev/sda"));
    CHECK(!fsops_readable("/run/user/1000/bus"));
    CHECK(!fsops_readable("/run/media/otro/USB"));
    CHECK(!fsops_readable("relative/path"));
    path(a, ".local/share/applications/x.desktop");
    CHECK(fsops_protected(a));
    path(a, "Documentos/x.txt");
    CHECK(!fsops_protected(a));

    /* a parent of a protected path can't be trashed, renamed, moved or restored over: that would take
     * the protected path with it, or bring a crafted one in */
    static const char *const dirs[] = { ".config", ".config/axia", ".config/plain", ".local", ".local/share", ".local/share/axia",
                                         ".local/share/axia/vault", "box", NULL };
    for (int i = 0; dirs[i]; i++) { path(a, dirs[i]); mkdir(a, 0700); }
    put(".config/axia/settings.json", "{}");
    put(".config/axia/other.json", "{}");
    put(".local/share/axia/vault/v", "x");
    put(".config/plain/a.txt", "a");
    put("box/settings.json", "{\"evil\":1}");
    static const char *const parents[] = { ".config/axia", ".config", ".local/share/axia", ".local/share", ".local", NULL };
    for (int i = 0; parents[i]; i++) {
        path(a, parents[i]);
        CHECK(fsops_protected_below(a));
        CHECK(fstrash_put(a, err, sizeof err) == -1 && !strncmp(err, "fs.protected", 12));
        path(b, "moved-away");
        CHECK(fsops_rename(a, b, err, sizeof err) == -1 && !strncmp(err, "fs.protected", 12));
        CHECK(fsjob_start(a, b, 1, err, sizeof err) == -1 && !strncmp(err, "fs.protected", 12));
        CHECK(exists(parents[i]) && !exists("moved-away"));
    }
    CHECK(exists(".config/axia/settings.json"));
    /* nor can a folder take such a name */
    path(a, ".config/axia");
    path(b, ".config/lordos-old");
    char c2[512];
    path(c2, "box");
    CHECK(!rename(a, b));
    CHECK(fsops_rename(c2, a, err, sizeof err) == -1 && !strncmp(err, "fs.protected", 12));
    CHECK(fsjob_start(c2, a, 1, err, sizeof err) == -1 && !strncmp(err, "fs.protected", 12));
    CHECK(fsjob_start(c2, a, 0, err, sizeof err) == -1 && !strncmp(err, "fs.protected", 12));
    CHECK(!rename(b, a));
    /* normal folders work as always, the protected path's siblings too */
    path(a, ".config/plain");
    CHECK(!fsops_protected_below(a));
    path(b, ".config/plain2");
    CHECK(!fsops_rename(a, b, err, sizeof err));
    CHECK(exists(".config/plain2/a.txt"));
    CHECK(!fstrash_put(b, err, sizeof err));
    path(a, ".config/axia/other.json");
    CHECK(!fsops_protected_below(a));
    path(b, ".config/axia/renamed.json");
    CHECK(!fsops_rename(a, b, err, sizeof err));
    path(a, "box");
    path(b, "box2");
    CHECK(fsops_rename(a, a, err, sizeof err) == -1);       /* it exists */
    CHECK(!fsops_rename(a, b, err, sizeof err));
    /* rename checks both sides itself, whatever its caller resolved, and never replaces a target */
    path(a, "box2/settings.json");
    path(b, ".config/autostart/x.desktop");
    CHECK(fsops_rename(a, b, err, sizeof err) == -1 && !strncmp(err, "fs.protected", 12));
    path(c2, "box2/y.json");
    CHECK(fsops_rename(b, c2, err, sizeof err) == -1 && !strncmp(err, "fs.protected", 12));
    path(b, ".config");
    CHECK(fsops_rename(a, b, err, sizeof err) == -1 && !strncmp(err, "fs.protected", 12));
    CHECK(fsops_rename(b, c2, err, sizeof err) == -1 && !strncmp(err, "fs.protected", 12));
    put("box2/y.json", "target");
    CHECK(fsops_rename(a, c2, err, sizeof err) == -1 && !strncmp(err, "fs.exists", 9));
    {
        char got[16] = "";
        FILE *f = fopen(c2, "r");
        if (f) { if (!fgets(got, sizeof got, f)) got[0] = 0; fclose(f); }
        CHECK(!strcmp(got, "target") && exists("box2/settings.json"));
    }
    /* a trashed parent from before this check can't come back over the protected path */
    path(a, ".config/plain3");
    mkdir(a, 0700);
    CHECK(!fstrash_put(a, err, sizeof err));
    {
        char t[512], info[512];
        path(t, ".local/share/Trash/files/plain3");
        path(info, ".local/share/Trash/info/plain3.trashinfo");
        FILE *f = fopen(info, "w");
        if (f) {
            fprintf(f, "[Trash Info]\nPath=%s/.config/axia\nDeletionDate=2026-01-01T00:00:00\n", H);
            fclose(f);
        }
        path(a, ".config/axia");
        path(b, ".config/lordos-kept");
        CHECK(!rename(a, b));
        CHECK(fstrash_restore(t, err, sizeof err) == -1 && !strncmp(err, "fs.protected", 12));
        CHECK(!rename(b, a));
    }
    fstrash_empty();

    /* copy a tree with a link and permissions */
    mkdir(strcat(strcpy(a, H), "/src"), 0750);
    mkdir(strcat(strcpy(a, H), "/src/sub"), 0700);
    put("src/a.txt", "hola");
    put("src/sub/b.txt", "chau");
    path(a, "src/a.txt");
    chmod(a, 0640);
    path(b, "src/link");
    symlink("a.txt", b);
    path(a, "src");
    path(b, "copia");
    int id = fsjob_start(a, b, 0, err, sizeof err);
    CHECK(id > 0);
    wait_job(id);
    CHECK(exists("copia/a.txt") && exists("copia/sub/b.txt"));
    struct stat st;
    path(a, "copia/link");
    CHECK(!lstat(a, &st) && S_ISLNK(st.st_mode));
    path(a, "copia/a.txt");
    CHECK(!stat(a, &st) && (st.st_mode & 0777) == 0640);
    /* does not overwrite */
    path(a, "src");
    path(b, "copia");
    CHECK(fsjob_start(a, b, 0, err, sizeof err) < 0);
    /* not into itself */
    path(b, "src/sub/otra");
    CHECK(fsjob_start(a, b, 0, err, sizeof err) < 0);

    /* move (same disk: instant rename) */
    path(a, "copia");
    path(b, "movida");
    id = fsjob_start(a, b, 1, err, sizeof err);
    CHECK(id > 0);
    wait_job(id);
    CHECK(!exists("copia") && exists("movida/sub/b.txt"));

    /* Papelera */
    path(a, "movida");
    CHECK(fstrash_put(a, err, sizeof err) == 0);
    CHECK(!exists("movida") && exists(".local/share/Trash/files/movida") && exists(".local/share/Trash/info/movida.trashinfo"));
    put("doble.txt", "1");
    path(a, "doble.txt");
    CHECK(fstrash_put(a, err, sizeof err) == 0);
    put("doble.txt", "2");
    CHECK(fstrash_put(a, err, sizeof err) == 0);
    CHECK(exists(".local/share/Trash/files/doble.txt.2"));
    trash_n = 0;
    fstrash_list(count_cb, NULL);
    CHECK(trash_n == 3);
    path(a, ".local/share/Trash/files/movida");
    CHECK(fstrash_restore(a, err, sizeof err) == 0);
    CHECK(exists("movida/a.txt") && !exists(".local/share/Trash/info/movida.trashinfo"));
    /* restoring does not overwrite */
    put("doble.txt", "3");
    path(a, ".local/share/Trash/files/doble.txt");
    CHECK(fstrash_restore(a, err, sizeof err) != 0);
    CHECK(fstrash_remove(a, err, sizeof err) == 0);
    CHECK(!exists(".local/share/Trash/files/doble.txt"));
    /* delete only inside the Trash */
    path(a, "movida");
    CHECK(fstrash_remove(a, err, sizeof err) != 0 && exists("movida"));
    path(a, ".local/share/Trash/files/../../../../movida");
    CHECK(fstrash_remove(a, err, sizeof err) != 0 && exists("movida"));
    CHECK(fstrash_empty() == 1);
    trash_n = 0;
    fstrash_list(count_cb, NULL);
    CHECK(trash_n == 0);

    /* size, search, recents */
    FsDu d;
    path(a, "src");
    fs_du(a, 500, &d);
    CHECK(d.complete && d.files >= 2 && d.size >= 8);
    hits = 0;
    fs_search(H, "B.TXT", 50, 8, hit_cb, NULL);
    CHECK(hits == 2);                       /* src/sub/b.txt and movida/sub/b.txt */
    hits = 0;
    fs_recent(10, 1, hit_cb, NULL);
    CHECK(hits >= 3);

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", H);
    if (system(cmd)) {}
    printf("fsops-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
