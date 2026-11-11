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

/* #85: a cloud or network mount inside a listed folder is listed as a folder without a stat of its root (which waits for
 * the server). Needs such a mount on this machine (the first fuse.*, cifs or nfs one whose parent can be read). */
typedef struct { const char *want; int seen, dir; double mtime; } MountSeen;
static void mount_cb(void *ud, const FsEntry *e) { MountSeen *m = ud; if (!strcmp(e->name, m->want)) { m->seen = 1; m->dir = e->dir; m->mtime = e->mtime; } }
static void test_remote_child(void) {
    FILE *f = fopen("/proc/self/mounts", "re");
    char line[4096], dev[1024], mnt[2048], ty[64], found[2048] = "";
    while (f && !found[0] && fgets(line, sizeof line, f))
        if (sscanf(line, "%1023s %2047s %63s", dev, mnt, ty) == 3 && (!strncmp(ty, "fuse.", 5) || !strcmp(ty, "cifs") || !strncmp(ty, "nfs", 3)) &&
            !strchr(mnt, '\\') && strrchr(mnt, '/') != mnt) snprintf(found, sizeof found, "%s", mnt);
    if (f) fclose(f);
    if (!found[0]) { printf("fsops-test: remote mount listing skipped: needs a fuse, cifs or nfs mount\n"); return; }
    char *slash = strrchr(found, '/');
    *slash = 0;
    MountSeen m = { slash + 1, 0, 0, -1 };
    CHECK(fsops_list(found, 100000, mount_cb, &m) >= 0);
    CHECK(m.seen && m.dir && m.mtime == 0);        /* a folder, not stat'ed: no time */
}

int main(void) {
    test_remote_child();
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
    /* web apps: the registry lord-web starts from, and each profile's data (docs/WEBAPPS.md) */
    path(a, ".config/lordos/web/apps/chat.json");
    CHECK(fsops_protected(a));
    path(a, ".local/share/lordos/web/chat/personal/cookies.sqlite");
    CHECK(fsops_protected(a));
    path(a, ".config/lordos/webcam.json");
    CHECK(!fsops_protected(a));
    path(a, "Documentos/x.txt");
    CHECK(!fsops_protected(a));
    path(a, ".config/lordos/global-shortcuts.json");     /* the global shortcuts portal's decisions */
    CHECK(fsops_protected(a));
    path(a, ".config/lordos/global-shortcuts.json.tmp");
    CHECK(fsops_protected(a));

    /* a parent of a protected path can't be trashed, renamed, moved or restored over: that would take
     * the protected path with it, or bring a crafted one in */
    static const char *const dirs[] = { ".config", ".config/lordos", ".config/plain", ".local", ".local/share", ".local/share/lordos",
                                         ".local/share/lordos/vault", "box", NULL };
    for (int i = 0; dirs[i]; i++) { path(a, dirs[i]); mkdir(a, 0700); }
    put(".config/lordos/settings.json", "{}");
    put(".config/lordos/other.json", "{}");
    put(".local/share/lordos/vault/v", "x");
    put(".config/plain/a.txt", "a");
    put("box/settings.json", "{\"evil\":1}");
    static const char *const parents[] = { ".config/lordos", ".config", ".local/share/lordos", ".local/share", ".local", NULL };
    for (int i = 0; parents[i]; i++) {
        path(a, parents[i]);
        CHECK(fsops_protected_below(a));
        CHECK(fstrash_put(a, err, sizeof err) == -1 && !strncmp(err, "fs.protected", 12));
        path(b, "moved-away");
        CHECK(fsops_rename(a, b, err, sizeof err) == -1 && !strncmp(err, "fs.protected", 12));
        CHECK(fsjob_start(a, b, 1, err, sizeof err) == -1 && !strncmp(err, "fs.protected", 12));
        CHECK(exists(parents[i]) && !exists("moved-away"));
    }
    CHECK(exists(".config/lordos/settings.json"));
    /* nor can a folder take such a name */
    path(a, ".config/lordos");
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
    path(a, ".config/lordos/other.json");
    CHECK(!fsops_protected_below(a));
    path(b, ".config/lordos/renamed.json");
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
            fprintf(f, "[Trash Info]\nPath=%s/.config/lordos\nDeletionDate=2026-01-01T00:00:00\n", H);
            fclose(f);
        }
        path(a, ".config/lordos");
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

    /* a protected path reached through a link (~/.config → another folder): the real folder holds settings.json, which
     * a lexical check misses and the resolved one sees (a machine's writable share) */
    {
        char e[512], l[512], f[600], d[512];
        snprintf(e, sizeof e, "%s/elsewhere", H); mkdir(e, 0755);
        snprintf(d, sizeof d, "%s/elsewhere/lordos", H); mkdir(d, 0755);
        snprintf(f, sizeof f, "%s/settings.json", d);
        FILE *sf = fopen(f, "w"); if (sf) { fputs("{}", sf); fclose(sf); }
        char keep[520];
        snprintf(l, sizeof l, "%s/.config", H);
        snprintf(keep, sizeof keep, "%s/.config-real", H);
        rename(l, keep);                                /* the earlier tests' real folder, put back after */
        if (symlink(e, l)) perror("symlink");
        CHECK(!fsops_protected(e) && !fsops_protected_below(e));   /* lexically: nothing protected here */
        CHECK(fsops_protected_resolved(e));             /* resolved: it holds settings.json */
        CHECK(fsops_protected_resolved(d));
        /* a protected file not written yet, in a folder reached through the link: still seen where it would be */
        fsops_protect_file(FSOPS_PF_TERMINAL, "");
        char tj[600];
        snprintf(tj, sizeof tj, "%s/.config/lordos/terminal.json", H);
        fsops_protect_file(FSOPS_PF_TERMINAL, tj);
        unlink(f);                                      /* settings.json gone too: only the missing file is left */
        CHECK(fsops_protected_resolved(d));
        CHECK(fsops_protected_resolved(e));
        fsops_protect_file(FSOPS_PF_TERMINAL, "");
        snprintf(d, sizeof d, "%s/Plain", H); mkdir(d, 0755);
        CHECK(!fsops_protected_resolved(d));
        unlink(l);
        rename(keep, l);
    }

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", H);
    if (system(cmd)) {}
    printf("fsops-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
