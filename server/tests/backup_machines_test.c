/* backup_machines_test.c — Backups and virtual machines (src/backup/engine.c, vmmeta.h;
 * docs/VIRTUALIZATION.md 6.14): a "*.lordvm" folder with a machine.json is a machine.
 *
 *   - a stopped machine: everything copied but its temporary files (lock, run.json, operation.json,
 *     log/, "*.part", dot files in disks/); the engine holds a shared flock on its lock meanwhile
 *     (lord-vm's exclusive one fails then) and lets it go afterwards;
 *   - a running machine (this process holds LOCK_EX on its lock through another open file
 *     description, as lord-vm does): its disks are the previous snapshot's, hard-linked (same
 *     inode), never the changed live ones; a disk added meanwhile is not mixed in; its other files
 *     are copied; its name is reported cleaned (no control or bidi override characters);
 *   - "backup": false: the whole folder is left out and counted;
 *   - a lock that is a symbolic link (never followed: the machine counts as busy) or a fifo (no hang);
 *   - a machine.json that is a symbolic link (never followed: backed up, the link stored as a link);
 *   - an unparseable machine.json (backed up); a "*.lordvm" without machine.json (ordinary folder);
 *   - the library's .shared/ base disks are backed up;
 *   - no descriptor is left open; bk_vm_meta is strict.
 * Built and run under ASan and UBSan by mk/backupvm.mk. */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "backup/engine.h"
#include "backup/vmmeta.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#define ROOT "/var/tmp/lordos-backupvm-test"
static const char *H = ROOT "/home", *M = ROOT "/dest/LordOS-Backups/test-user";

static void put(const char *rel, const char *text) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", H, rel);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    fputs(text, f);
    fclose(f);
}

static void mk(const char *rel) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", H, rel);
    if (mkdir(p, 0755) && errno != EEXIST) { perror(p); exit(2); }
}

static void lnk(const char *target, const char *rel) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", H, rel);
    if (symlink(target, p)) { perror(p); exit(2); }
}

static int rm_cb(const char *p, const struct stat *st, int flag, struct FTW *f) {
    (void)st; (void)f;
    if (flag == FTW_DP || flag == FTW_D) { chmod(p, 0700); return rmdir(p) && errno != ENOENT ? -1 : 0; }
    return unlink(p) && errno != ENOENT ? -1 : 0;
}
static void rm_rf(const char *p) { nftw(p, rm_cb, 32, FTW_DEPTH | FTW_PHYS); }

static void sp(const char *id, const char *rel, char *out, size_t n) { snprintf(out, n, "%s/%s/%s", M, id, rel); }
static int in_snap(const char *id, const char *rel) { char p[PATH_MAX]; struct stat st; sp(id, rel, p, sizeof p); return lstat(p, &st) == 0; }
static ino_t snap_ino(const char *id, const char *rel) { char p[PATH_MAX]; struct stat st; sp(id, rel, p, sizeof p); return lstat(p, &st) ? 0 : st.st_ino; }
static int snap_is_link(const char *id, const char *rel) { char p[PATH_MAX]; struct stat st; sp(id, rel, p, sizeof p); return !lstat(p, &st) && S_ISLNK(st.st_mode); }
static int snap_text(const char *id, const char *rel, const char *want) {
    char p[PATH_MAX], buf[256];
    sp(id, rel, p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    return !strcmp(buf, want);
}

static int open_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) if (e->d_name[0] != '.') n++;
    if (d) closedir(d);
    return n;
}

/* lord-vm's way: open (another open file description) and take it exclusively */
static int lordvm_lock(const char *machine) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/Machines/%s/lock", H, machine);
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return -2; }
    return fd;
}

/* while a Stopped.lordvm file is copied, lord-vm cannot take the lock; afterwards it can */
static int seen_stopped, lock_held_inside, lock_free_after;
static void progress(BkRun *r, const char *rel) {
    (void)r;
    int inside = !strncmp(rel, "Machines/Stopped.lordvm/", 24);
    if (!inside && !seen_stopped) return;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/Machines/Stopped.lordvm/lock", H);
    int fd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    int got = flock(fd, LOCK_EX | LOCK_NB) == 0;
    close(fd);                                       /* drops it if taken */
    if (inside) { seen_stopped = 1; if (!got) lock_held_inside++; }
    else if (got) lock_free_after++;
}

static void init_run(BkRun *r) {
    memset(r, 0, sizeof *r);
    r->home = H;
    r->machine = M;
    r->excl[r->nexcl++] = ".cache";
    r->min_free = 1;
    r->vm_meta = bk_vm_meta;
    r->progress = progress;
}

static void build_home(void) {
    rm_rf(ROOT);
    if (mkdir(ROOT, 0700) || mkdir(ROOT "/home", 0700) || mkdir(ROOT "/dest", 0700)) { perror(ROOT); exit(2); }
    mk("Machines");
    put("note.txt", "not a machine\n");
    /* the library's shared bases (frozen; linked clones read them) */
    mk("Machines/.shared");
    put("Machines/.shared/base-debian.qcow2", "frozen base\n");

    /* stopped: everything but the temporaries */
    mk("Machines/Stopped.lordvm");
    put("Machines/Stopped.lordvm/machine.json", "{\"schema\":13,\"name\":\"Stopped\",\"backup\":true}");
    put("Machines/Stopped.lordvm/lock", "");
    put("Machines/Stopped.lordvm/run.json", "{}");
    put("Machines/Stopped.lordvm/operation.json", "{}");
    put("Machines/Stopped.lordvm/snapshots.json", "[]");
    put("Machines/Stopped.lordvm/cpu-usage.json", "{}");
    put("Machines/Stopped.lordvm/thumb.png", "png");
    put("Machines/Stopped.lordvm/firmware-code.fd", "fw");
    put("Machines/Stopped.lordvm/export.part", "half");
    mk("Machines/Stopped.lordvm/log");
    put("Machines/Stopped.lordvm/log/lord-vm.log", "log");
    mk("Machines/Stopped.lordvm/disks");
    put("Machines/Stopped.lordvm/disks/system.qcow2", "stopped disk\n");
    put("Machines/Stopped.lordvm/disks/nvram.qcow2", "nvram");
    put("Machines/Stopped.lordvm/disks/.session-overlay.qcow2", "overlay");
    put("Machines/Stopped.lordvm/disks/.system.qcow2.fresh", "fresh");
    put("Machines/Stopped.lordvm/disks/data-2.qcow2.part", "half");
    mk("Machines/Stopped.lordvm/disks/tpm");
    put("Machines/Stopped.lordvm/disks/tpm/tpm2-00.permall", "tpm");

    /* runs in the second backup; no lock file yet (the engine makes one, as lord-vm would) */
    mk("Machines/Running.lordvm");
    put("Machines/Running.lordvm/machine.json", "{\"name\":\"Run\\u202ening\\u0007 VM\"}");
    put("Machines/Running.lordvm/snapshots.json", "[1]");
    mk("Machines/Running.lordvm/disks");
    put("Machines/Running.lordvm/disks/system.qcow2", "consistent disk\n");
    mk("Machines/Running.lordvm/disks/tpm");
    put("Machines/Running.lordvm/disks/tpm/tpm2-00.permall", "tpm1");

    /* "backup": false */
    mk("Machines/Off.lordvm");
    put("Machines/Off.lordvm/machine.json", "{\"name\":\"Off\",\"backup\":false}");
    mk("Machines/Off.lordvm/disks");
    put("Machines/Off.lordvm/disks/system.qcow2", "off");

    /* its lock is a link to a free file: following it would copy the disks */
    put("free-lock", "");
    mk("Machines/SymLock.lordvm");
    put("Machines/SymLock.lordvm/machine.json", "{\"name\":\"SymLock\"}");
    lnk(ROOT "/home/free-lock", "Machines/SymLock.lordvm/lock");
    mk("Machines/SymLock.lordvm/disks");
    put("Machines/SymLock.lordvm/disks/system.qcow2", "symlock");

    /* a fifo as the lock: no hang */
    mk("Machines/Fifo.lordvm");
    put("Machines/Fifo.lordvm/machine.json", "{\"name\":\"Fifo\"}");
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/Machines/Fifo.lordvm/lock", H);
    if (mkfifo(p, 0600)) { perror(p); exit(2); }
    mk("Machines/Fifo.lordvm/disks");
    put("Machines/Fifo.lordvm/disks/system.qcow2", "fifo");

    /* machine.json is a link to a file saying "backup": false: not followed, so backed up */
    put("other.json", "{\"name\":\"Elsewhere\",\"backup\":false}");
    mk("Machines/SymJson.lordvm");
    lnk(ROOT "/home/other.json", "Machines/SymJson.lordvm/machine.json");
    mk("Machines/SymJson.lordvm/disks");
    put("Machines/SymJson.lordvm/disks/system.qcow2", "symjson");
    put("Machines/SymJson.lordvm/run.json", "{}");

    /* unparseable machine.json: backed up */
    mk("Machines/Bad.lordvm");
    put("Machines/Bad.lordvm/machine.json", "{\"backup\": false,");
    mk("Machines/Bad.lordvm/disks");
    put("Machines/Bad.lordvm/disks/system.qcow2", "bad");

    /* named like a machine, no machine.json: an ordinary folder */
    mk("Machines/Plain.lordvm");
    put("Machines/Plain.lordvm/lock", "");
    put("Machines/Plain.lordvm/run.json", "{}");
}

static void test_meta(void) {
    int b;
    char name[BK_VM_NAME];
    CHECK(bk_vm_meta("{\"name\":\"A\",\"backup\":false}", strlen("{\"name\":\"A\",\"backup\":false}"), &b, name, sizeof name) == 0 && b == 0 && !strcmp(name, "A"), "meta false");
    CHECK(bk_vm_meta("{\"name\":\"A\"}", strlen("{\"name\":\"A\"}"), &b, name, sizeof name) == 0 && b == 1, "meta default on");
    CHECK(bk_vm_meta("{\"backup\":\"no\"}", strlen("{\"backup\":\"no\"}"), &b, name, sizeof name) == -1 && b == 1, "meta: backup must be a boolean");
    CHECK(bk_vm_meta("{\"name\":3,\"backup\":false}", strlen("{\"name\":3,\"backup\":false}"), &b, name, sizeof name) == -1 && b == 1, "meta: name must be text");
    CHECK(bk_vm_meta("[false]", strlen("[false]"), &b, name, sizeof name) == -1 && b == 1, "meta: not an object");
    CHECK(bk_vm_meta("{\"backup\":false,\"backup\":false}", strlen("{\"backup\":false,\"backup\":false}"), &b, name, sizeof name) == -1 && b == 1, "meta: duplicate key");
}

int main(void) {
    test_meta();
    build_home();
    int fds0 = open_fds();
    static char ids[8][BK_ID_LEN];

    /* ---- first run: nothing running (SymLock and Fifo can't be locked) ---- */
    BkRun r;
    init_run(&r);
    time_t now = time(NULL);
    int rc = bk_snapshot(&r, now);
    CHECK(rc == 0, "run 1: %d %s", rc, r.error);
    CHECK(bk_list(M, ids, 8) == 1, "one snapshot");
    const char *s1 = ids[0];
    CHECK(r.vm_left_out == 1, "run 1 left out: %llu", (unsigned long long)r.vm_left_out);
    CHECK(r.vm_busy == 2 && r.vm_nnames == 2, "run 1 busy: %llu", (unsigned long long)r.vm_busy);
    CHECK(r.vm_nnames == 2 && !strcmp(r.vm_busy_names[0], "Fifo") && !strcmp(r.vm_busy_names[1], "SymLock"), "run 1 names");
    CHECK(open_fds() == fds0, "no descriptor left open (%d, was %d)", open_fds(), fds0);
    CHECK(seen_stopped && lock_held_inside > 0, "the shared lock is held while Stopped is copied (%d)", lock_held_inside);
    CHECK(lock_free_after > 0, "and released after it (%d)", lock_free_after);

    static const char *const kept[] = { "machine.json", "snapshots.json", "cpu-usage.json", "thumb.png", "firmware-code.fd",
                                        "disks/system.qcow2", "disks/nvram.qcow2", "disks/tpm/tpm2-00.permall", NULL };
    static const char *const left[] = { "lock", "run.json", "operation.json", "export.part", "log", "disks/.session-overlay.qcow2",
                                        "disks/.system.qcow2.fresh", "disks/data-2.qcow2.part", NULL };
    char rel[PATH_MAX];
    for (int i = 0; kept[i]; i++) { snprintf(rel, sizeof rel, "Machines/Stopped.lordvm/%s", kept[i]); CHECK(in_snap(s1, rel), "kept: %s", rel); }
    for (int i = 0; left[i]; i++) { snprintf(rel, sizeof rel, "Machines/Stopped.lordvm/%s", left[i]); CHECK(!in_snap(s1, rel), "left out: %s", rel); }
    CHECK(snap_text(s1, "Machines/Running.lordvm/disks/system.qcow2", "consistent disk\n"), "stopped Running: disk copied");
    CHECK(!in_snap(s1, "Machines/Running.lordvm/lock"), "the lock the engine made is not backed up");
    CHECK(!in_snap(s1, "Machines/Off.lordvm"), "backup:false left out");
    CHECK(in_snap(s1, "Machines/.shared/base-debian.qcow2"), ".shared backed up");
    CHECK(in_snap(s1, "Machines/SymLock.lordvm/machine.json") && !in_snap(s1, "Machines/SymLock.lordvm/disks/system.qcow2"),
          "symlinked lock not followed: busy, its disks not copied");
    CHECK(!in_snap(s1, "Machines/SymLock.lordvm/lock"), "symlinked lock not stored");
    CHECK(!in_snap(s1, "Machines/Fifo.lordvm/disks/system.qcow2"), "fifo lock: busy");
    CHECK(snap_is_link(s1, "Machines/SymJson.lordvm/machine.json"), "symlinked machine.json stored as a link");
    CHECK(snap_text(s1, "Machines/SymJson.lordvm/disks/system.qcow2", "symjson") && !in_snap(s1, "Machines/SymJson.lordvm/run.json"),
          "symlinked machine.json: a machine, backed up");
    CHECK(snap_text(s1, "Machines/Bad.lordvm/disks/system.qcow2", "bad"), "unparseable machine.json: backed up");
    CHECK(in_snap(s1, "Machines/Plain.lordvm/lock") && in_snap(s1, "Machines/Plain.lordvm/run.json"), "no machine.json: ordinary folder");

    /* ---- second run: Running runs (this process holds its lock as lord-vm does) ---- */
    int vmfd = lordvm_lock("Running.lordvm");
    CHECK(vmfd >= 0, "lord-vm's lock: %d", vmfd);
    put("Machines/Running.lordvm/disks/system.qcow2", "half-written disk, longer\n");
    put("Machines/Running.lordvm/disks/data-1.qcow2", "added while running\n");
    put("Machines/Running.lordvm/disks/tpm/tpm2-00.permall", "tpm2-changed");
    put("Machines/Running.lordvm/snapshots.json", "[1,2]");
    put("Machines/Running.lordvm/run.json", "{\"pid\":1}");
    seen_stopped = lock_held_inside = lock_free_after = 0;
    init_run(&r);
    rc = bk_snapshot(&r, now + 5);
    CHECK(rc == 0, "run 2: %d %s", rc, r.error);
    CHECK(bk_list(M, ids, 8) == 2, "two snapshots");
    const char *s2 = ids[1];
    CHECK(r.vm_busy == 3 && r.vm_left_out == 1, "run 2: busy %llu, left out %llu", (unsigned long long)r.vm_busy, (unsigned long long)r.vm_left_out);
    int found = 0;
    for (int i = 0; i < r.vm_nnames; i++) if (!strcmp(r.vm_busy_names[i], "Running VM")) found = 1;
    CHECK(found, "the running machine's name, cleaned: %s", r.vm_nnames > 2 ? r.vm_busy_names[2] : "-");
    const char *d = "Machines/Running.lordvm/disks/system.qcow2", *t = "Machines/Running.lordvm/disks/tpm/tpm2-00.permall";
    CHECK(snap_ino(s1, d) && snap_ino(s1, d) == snap_ino(s2, d), "busy disk carried by hard link (same inode)");
    CHECK(snap_text(s2, d, "consistent disk\n"), "busy disk: the previous copy, not the live one");
    CHECK(snap_ino(s1, t) && snap_ino(s1, t) == snap_ino(s2, t) && snap_text(s2, t, "tpm1"), "TPM state carried too");
    CHECK(!in_snap(s2, "Machines/Running.lordvm/disks/data-1.qcow2"), "a disk added while running is not mixed in");
    CHECK(snap_text(s2, "Machines/Running.lordvm/snapshots.json", "[1,2]"), "the busy machine's other files are copied");
    CHECK(!in_snap(s2, "Machines/Running.lordvm/run.json") && !in_snap(s2, "Machines/Running.lordvm/lock"), "busy: temporaries left out");
    CHECK(snap_text(s2, "Machines/Stopped.lordvm/disks/system.qcow2", "stopped disk\n"), "the stopped machine still copied");
    CHECK(seen_stopped && lock_held_inside > 0 && lock_free_after > 0, "run 2: lock held for Stopped only while it is copied");
    CHECK(open_fds() == fds0 + 1, "no descriptor left open (only the test's lock): %d, was %d", open_fds(), fds0);
    /* still exclusively ours: the engine never took it */
    CHECK(flock(vmfd, LOCK_EX | LOCK_NB) == 0, "lord-vm's lock untouched");
    close(vmfd);

    /* ---- third run: stopped again, its live disk is copied ---- */
    init_run(&r);
    rc = bk_snapshot(&r, now + 10);
    CHECK(rc == 0 && r.vm_busy == 2, "run 3: %d busy %llu", rc, (unsigned long long)r.vm_busy);
    CHECK(bk_list(M, ids, 8) == 3, "three snapshots");
    CHECK(snap_text(ids[2], d, "half-written disk, longer\n") && in_snap(ids[2], "Machines/Running.lordvm/disks/data-1.qcow2"),
          "stopped again: the current disks are copied");

    rm_rf(ROOT);
    printf("backup-machines-test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
