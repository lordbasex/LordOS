/* vmlistcache_test.c — lord.vm.list's cache (src/vm/vmlistcache.h) over fixture machine folders under a temporary HOME:
 * a kept list is given while nothing changed and dropped by a machine.json written, a run.json renamed in place (lord-vm's
 * way), a machine folder added (and then a file in it), a group's member changed, a folder removed, the time limit, or an
 * active machine's lord-vm gone; thumb.png, logs and temporary files never drop it; the generation moves with each
 * change and not without one. The firmware check hashes each file version once, whatever the machines' ids (the old
 * id[0] % 16 slots made ids starting with '1' and 'a' hash the file again at every list).
 *
 *   make -C server build/vmlistcache-test        (also in `make test`) */
#include "../src/vm/vmlistcache.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char T[256], LIB[512];

static void put(const char *rel, const char *text) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", LIB, rel);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(1); }
    fputs(text, f);
    fclose(f);
}
static void dir(const char *rel) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", LIB, rel);
    if (mkdir(p, 0700)) { perror(p); exit(1); }
}

static int alive_ok(long pid) { (void)pid; return 1; }
static int alive_none(long pid) { (void)pid; return 0; }

/* the list as f_list keeps it: watches first (vmlc_begin), then the store */
static void relist(VmListCache *c, double now, const char *json) {
    vmlc_begin(c, LIB);
    vmlc_store(c, json, strlen(json), now);
}

static int kept(VmListCache *c, double now) { return vmlc_get(c, LIB, now, alive_ok, NULL) != NULL; }

static void test_cache(void) {
    VmListCache *c = calloc(1, sizeof *c);
    vmlc_init(c);
    CHECK(vmlc_name_matters("machine.json") && vmlc_name_matters("run.json") && vmlc_name_matters("A.lordvm"), "names that matter");
    CHECK(!vmlc_name_matters("thumb.png") && !vmlc_name_matters(".run.json.tmp") && !vmlc_name_matters("x.tmp") &&
          !vmlc_name_matters("machine.log") && !vmlc_name_matters("a~"), "names that don't");
    double now = 1000;
    CHECK(!kept(c, now), "nothing kept at first");
    relist(c, now, "[1]");
    CHECK(vmlc_fd(c) >= 0 && !c->broken, "watched: fd %d", vmlc_fd(c));
    size_t len = 0;
    const char *j = vmlc_get(c, LIB, now + 1, alive_ok, &len);
    CHECK(j && len == 3 && !strcmp(j, "[1]"), "kept: %s", j ? j : "(none)");
    unsigned g = c->gen;
    CHECK(kept(c, now + 2) && c->gen == g, "kept again, the generation stays");

    /* noise that never changes the list */
    put("A.lordvm/thumb.png", "png");
    put("A.lordvm/qemu.log", "line\n");
    put("A.lordvm/.run.json.tmp", "{}");
    CHECK(kept(c, now + 3) && c->gen == g, "thumb.png, a log, a temporary file: still kept");

    /* lord-vm's run.json: a temporary file renamed in place */
    char a[1024], b[1024];
    snprintf(a, sizeof a, "%s/A.lordvm/.run.json.tmp", LIB);
    snprintf(b, sizeof b, "%s/A.lordvm/run.json", LIB);
    CHECK(!rename(a, b), "rename");
    CHECK(!kept(c, now + 4) && c->gen != g, "run.json renamed in: dropped, the generation moved");
    relist(c, now + 5, "[2]");
    CHECK(kept(c, now + 6), "kept again");

    /* machine.json written in place */
    put("B.lordvm/machine.json", "{\"name\":\"B2\"}");
    CHECK(!kept(c, now + 7), "machine.json written: dropped");
    relist(c, now + 8, "[3]");

    /* a new machine folder, then its machine.json (lord-vm --create writes it last) */
    dir("C.lordvm");
    CHECK(!kept(c, now + 9), "a folder added: dropped");
    relist(c, now + 10, "[3]");
    CHECK(kept(c, now + 11), "kept (no machine.json yet)");
    put("C.lordvm/machine.json", "{}");
    CHECK(!kept(c, now + 12), "the new folder is watched: its machine.json drops it");
    relist(c, now + 13, "[4]");

    /* a disks/ file (direct kernel boot) */
    put("A.lordvm/disks/vmlinuz", "k");
    CHECK(!kept(c, now + 14), "disks/ is watched too");
    relist(c, now + 15, "[4]");

    /* a group's member */
    put("G.lordgroup/W.lordvm/run.json", "{}");
    CHECK(!kept(c, now + 16), "a group member's run.json: dropped");
    relist(c, now + 17, "[4]");

    /* a folder removed */
    char rm[1100];
    snprintf(rm, sizeof rm, "rm -rf '%s/C.lordvm'", LIB);
    if (system(rm)) {}
    CHECK(!kept(c, now + 18), "a machine removed: dropped");
    relist(c, now + 19, "[5]");

    /* the time limit */
    CHECK(kept(c, now + 19 + VMLC_TTL_MS - 1), "young enough");
    CHECK(!kept(c, now + 19 + VMLC_TTL_MS), "too old: read again");
    relist(c, now + 20, "[5]");

    /* an active machine's lord-vm gone (it writes nothing when it dies) */
    vmlc_begin(c, LIB);
    vmlc_pid(c, 4242);
    vmlc_store(c, "[6]", 3, now + 21);
    CHECK(vmlc_get(c, LIB, now + 22, alive_ok, NULL), "its lord-vm is there: kept");
    g = c->gen;
    CHECK(!vmlc_get(c, LIB, now + 23, alive_none, NULL) && c->gen != g, "its lord-vm gone: dropped, the generation moved");

    /* the generation: a list read again that is the same doesn't move it, a different one does */
    relist(c, now + 24, "[6]");
    g = c->gen;
    vmlc_begin(c, LIB);
    vmlc_store(c, "[6]", 3, now + 25);
    CHECK(c->gen == g, "the same list: the generation stays");
    vmlc_begin(c, LIB);
    vmlc_store(c, "[7]", 3, now + 26);
    CHECK(c->gen != g, "a different list: it moves");

    /* a change while it was reading the folders: kept, but not given */
    vmlc_begin(c, LIB);
    put("B.lordvm/machine.json", "{\"name\":\"B3\"}");
    vmlc_drain(c);
    vmlc_store(c, "[8]", 3, now + 27);
    CHECK(!kept(c, now + 28), "changed while read: read again next time");

    /* another library (HOME changed) */
    CHECK(!vmlc_get(c, "/nonexistent", now + 29, alive_ok, NULL), "another library: nothing kept for it");
    vmlc_free(c);
    free(c);
}

static void test_firmware(void) {
    char f1[600], f2[600];
    snprintf(f1, sizeof f1, "%s/CODE_A.fd", T);
    snprintf(f2, sizeof f2, "%s/CODE_B.fd", T);
    FILE *f = fopen(f1, "w");
    for (int i = 0; i < 4096; i++) fputs("0123456789abcdef", f);
    fclose(f);
    f = fopen(f2, "w");
    fputs("abc", f);
    fclose(f);
    /* sha256("abc") */
    const char *abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const char *zero = "0000000000000000000000000000000000000000000000000000000000000000";
    unsigned h0 = vmlc_fw_hashes;
    /* sixteen machines on each file, ids or not: each file hashed once */
    for (int i = 0; i < 16; i++) {
        int fd = open(f2, O_RDONLY);
        CHECK(vmlc_firmware_changed(fd, abc) == 0, "abc: not changed");
        close(fd);
        fd = open(f1, O_RDONLY);
        CHECK(vmlc_firmware_changed(fd, zero) == 1, "A: changed");
        close(fd);
    }
    CHECK(vmlc_fw_hashes - h0 == 2, "two files, two hashes: %u", vmlc_fw_hashes - h0);
    /* the file rewritten (a package update renames a new one in: a new inode; here the same one, a tick later) */
    usleep(50000);
    f = fopen(f2, "w");
    fputs("abd", f);
    fclose(f);
    int fd = open(f2, O_RDONLY);
    CHECK(vmlc_firmware_changed(fd, abc) == 1, "abd: changed");
    close(fd);
    CHECK(vmlc_fw_hashes - h0 == 3, "a new version is hashed again: %u", vmlc_fw_hashes - h0);
    CHECK(vmlc_firmware_changed(-1, abc) == -1, "no file: -1");
    fd = open(T, O_RDONLY);
    CHECK(vmlc_firmware_changed(fd, abc) == -1, "a folder: -1");
    close(fd);
}

int main(void) {
    snprintf(T, sizeof T, "%s/vmlistcache-test.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(T)) { perror("mkdtemp"); return 1; }
    char home[400];
    snprintf(home, sizeof home, "%s/home", T);
    mkdir(home, 0700);
    setenv("HOME", home, 1);
    snprintf(LIB, sizeof LIB, "%s/Machines", home);
    mkdir(LIB, 0700);
    dir("A.lordvm");
    dir("A.lordvm/disks");
    dir("B.lordvm");
    dir("G.lordgroup");
    dir("G.lordgroup/W.lordvm");
    put("A.lordvm/machine.json", "{}");
    put("B.lordvm/machine.json", "{}");
    put("G.lordgroup/group.json", "{}");
    put("G.lordgroup/W.lordvm/machine.json", "{}");
    test_cache();
    test_firmware();
    char rm[400];
    snprintf(rm, sizeof rm, "rm -rf '%s'", T);
    if (system(rm)) {}
    printf("vmlistcache-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
