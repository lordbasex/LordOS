/* backup_test.c — tests for the backup engine (src/backup/engine.c and crypt.c): snapshots,
 * hard links between snapshots, symbolic links (never followed), exclusions, interrupted and
 * resumed runs, restore (keep both, replace, protected folders, escapes through links), retention
 * (a pure function, with made-up dates) and encryption. */
/* the test paths are short: GCC cannot know that and warns on every path snprintf */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "backup/engine.h"
#include "backup/crypt.h"
#include "fsops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char T[PATH_MAX], H[PATH_MAX], M[PATH_MAX];

static void put(const char *rel, const char *text) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", H, rel);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    fputs(text, f);
    fclose(f);
}

static void put_big(const char *rel, size_t mb) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", H, rel);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    char *buf = malloc(1 << 20);
    for (size_t i = 0; i < mb; i++) { memset(buf, (int)('a' + i % 26), 1 << 20); fwrite(buf, 1, 1 << 20, f); }
    free(buf);
    fclose(f);
}

static void mk(const char *rel) { char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", H, rel); mkdir(p, 0755); }

static int slurp(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t k = fread(out, 1, n - 1, f);
    out[k] = 0;
    fclose(f);
    return 0;
}

static ino_t ino(const char *path) { struct stat st; return lstat(path, &st) ? 0 : st.st_ino; }
static int exists(const char *path) { struct stat st; return lstat(path, &st) == 0; }

static void snap_path(const char *id, const char *rel, char *out, size_t n) { snprintf(out, n, "%s/%s/%s", M, id, rel); }

static void init_run(BkRun *r) {
    memset(r, 0, sizeof *r);
    r->home = H;
    r->machine = M;
    r->excl[r->nexcl++] = ".cache";
    r->excl[r->nexcl++] = "Excluded";
    r->min_free = 1;
}

/* checkpoint that stops after ~N bytes */
static uint64_t cut_after;
static int cut_cb(BkRun *r) { return cut_after && r->bytes >= cut_after; }

static void test_retention(void) {
    /* one per hour for 90 days, ending now */
    enum { N = 90 * 24 };
    static time_t t[N];
    static int keep[N];
    time_t now = 1792000000;                /* 2026 */
    for (int i = 0; i < N; i++) t[i] = now - (time_t)(N - 1 - i) * 3600;
    bk_retention(t, N, now, 0, keep);
    int recent = 0, total = 0;
    for (int i = 0; i < N; i++) { total += keep[i]; if (now - t[i] < 86400) { recent++; CHECK(keep[i], "everything from the last 24 h stays"); } }
    CHECK(recent == 24, "24 hourly (%d)", recent);
    CHECK(keep[N - 1], "the newest stays");
    /* between 1 and 30 days: one per calendar day */
    int days[40] = { 0 };
    for (int i = 0; i < N; i++) {
        long age = (long)(now - t[i]);
        if (keep[i] && age >= 86400 && age < 30L * 86400) days[(t[i] / 86400) % 40]++;
    }
    for (int d = 0; d < 40; d++) CHECK(days[d] <= 1, "at most one per day (%d)", days[d]);
    /* after 30 days: one per week */
    long weeks[20];
    int nw = 0;
    for (int i = 0; i < N; i++) {
        if (!keep[i] || now - t[i] < 30L * 86400) continue;
        long w = (t[i] - 4L * 86400) / (7L * 86400);
        for (int k = 0; k < nw; k++) CHECK(weeks[k] != w, "two from the same week");
        if (nw < 20) weeks[nw++] = w;
    }
    CHECK(nw >= 8 && nw <= 10, "weekly between 30 and 90 days (%d)", nw);
    CHECK(total < 24 + 31 + 11, "reasonable total (%d)", total);
    /* small cases */
    time_t one = now;
    int k1 = 0;
    bk_retention(&one, 1, now, 0, &k1);
    CHECK(k1 == 1, "a single one stays");
    time_t old[3] = { now - 100L * 86400, now - 99L * 86400, now - 98L * 86400 };   /* same week or not, the newest stays */
    int k3[3];
    bk_retention(old, 3, now, 0, k3);
    CHECK(k3[2] == 1, "the newest stays even if old");
    /* time zone: two on the same local day but on different UTC days */
    time_t tz[2] = { 1791900000 - 3600 * 2, 1791900000 + 3600 * 2 };
    int kz[2];
    long off = -((1791900000L % 86400));     /* the local day boundary falls right between them */
    bk_retention(tz, 2, tz[1] + 5 * 86400, off + 86400 / 2, kz);
    CHECK(kz[1] == 1, "time zone: the newest stays");
}

static void test_rel(void) {
    CHECK(bk_rel_valid(""), "empty is valid");
    CHECK(bk_rel_valid("a/b c/d.txt"), "normal is valid");
    CHECK(!bk_rel_valid("/etc"), "absolute rejected");
    CHECK(!bk_rel_valid("a/../b"), ".. rejected");
    CHECK(!bk_rel_valid(".."), "bare .. rejected");
    CHECK(!bk_rel_valid("a//b"), "// rejected");
    CHECK(!bk_rel_valid("a/./b"), ". rejected");
    CHECK(!bk_rel_valid("a/"), "trailing slash rejected");
    CHECK(!bk_rel_valid("a\nb"), "control character rejected");
    CHECK(bk_id_valid("2026-10-17-235959"), "id");
    CHECK(!bk_id_valid("2026-10-17-23595"), "short id");
    CHECK(!bk_id_valid("../10-17-235959x"), "odd id");
    char id[BK_ID_LEN];
    time_t now = time(NULL);
    bk_id_make(now, id);
    CHECK(bk_id_time(id) == now, "id round trip");
}

/* encryption: sizes, round trips, tampering, wrong passphrase and an encrypted snapshot */
static void test_crypt(void) {
    static const uint64_t sizes[] = { 0, 1, 15, 16, 65535, 65536, 65537, 131072, 200000, 1u << 20 };
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        uint64_t p = 99;
        CHECK(!bkc_plain_size(bkc_enc_size(sizes[i]), &p) && p == sizes[i], "size %llu round trip", (unsigned long long)sizes[i]);
    }
    uint64_t junk;
    CHECK(bkc_plain_size(BKC_HDR + 5, &junk) != 0, "impossible size rejected");
    char M2[PATH_MAX], p[PATH_MAX], q[PATH_MAX], buf[256];
    snprintf(M2, sizeof M2, "%s/encrypted/" BK_DIRNAME "/host-user", T);
    snprintf(p, sizeof p, "%s/encrypted", T); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/encrypted/" BK_DIRNAME, T); mkdir(p, 0700);
    mkdir(M2, 0700);
    uint8_t key[BKC_KEY], k2[BKC_KEY];
    CHECK(bkc_setup(M2, "a long test passphrase", key) == 0, "set up encryption");
    CHECK(bkc_unlock(M2, "a long test passphrase", k2) == 0 && !memcmp(key, k2, sizeof key), "unlock with the passphrase");
    CHECK(bkc_unlock(M2, "another passphrase", k2) == -1, "wrong passphrase");
    /* round trip with several file sizes */
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        snprintf(p, sizeof p, "%s/plain.bin", T); snprintf(q, sizeof q, "%s/encrypted.bin", T);
        FILE *f = fopen(p, "w");
        for (uint64_t k = 0; k < sizes[i]; k++) fputc((int)((k * 7 + i) & 0xff), f);
        fclose(f);
        int in = open(p, O_RDONLY), out = open(q, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int r = bkc_encrypt_fd(in, out, key, NULL, NULL);
        close(in); close(out);
        struct stat st;
        CHECK(r == 0 && !stat(q, &st) && (uint64_t)st.st_size == bkc_enc_size(sizes[i]), "encrypt %llu", (unsigned long long)sizes[i]);
        char back[PATH_MAX];
        snprintf(back, sizeof back, "%s/back.bin", T);
        in = open(q, O_RDONLY); out = open(back, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        r = bkc_decrypt_fd(in, out, key, NULL, NULL);
        close(in); close(out);
        char cmd[3 * PATH_MAX];
        snprintf(cmd, sizeof cmd, "cmp -s '%s' '%s'", p, back);
        CHECK(r == 0 && system(cmd) == 0, "decrypt %llu identical", (unsigned long long)sizes[i]);
        if (sizes[i] == 200000) {
            /* one changed byte, a missing chunk and the wrong key: fails without returning bad data */
            int fd = open(q, O_RDWR);
            unsigned char b;
            pread(fd, &b, 1, 70000); b ^= 1; pwrite(fd, &b, 1, 70000);
            close(fd);
            in = open(q, O_RDONLY); out = open(back, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            CHECK(bkc_decrypt_fd(in, out, key, NULL, NULL) == -3, "tampered byte detected");
            close(in); close(out);
            pread(fd = open(q, O_RDWR), &b, 1, 70000); b ^= 1; pwrite(fd, &b, 1, 70000);
            CHECK(ftruncate(fd, (off_t)bkc_enc_size(2 * 65536)) == 0, "truncate");
            close(fd);
            in = open(q, O_RDONLY); out = open(back, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            CHECK(bkc_decrypt_fd(in, out, key, NULL, NULL) == -3, "missing chunks detected");
            close(in); close(out);
            uint8_t bad[BKC_KEY];
            memcpy(bad, key, sizeof bad); bad[0] ^= 1;
            in = open(q, O_RDONLY); out = open(back, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            CHECK(bkc_decrypt_fd(in, out, bad, NULL, NULL) == -3, "wrong key detected");
            close(in); close(out);
        }
    }
    /* encrypted snapshot: nothing in clear text, hard links still work, restore decrypts */
    BkRun r;
    memset(&r, 0, sizeof r);
    r.home = H; r.machine = M2; r.key = key; r.min_free = 1;
    r.excl[r.nexcl++] = "dest";
    put("secret.txt", "very secret text 1234\n");
    int rc = bk_snapshot(&r, time(NULL) - 600);
    CHECK(rc == 0, "encrypted snapshot (%s)", r.error);
    char id1[BK_ID_LEN];
    snprintf(id1, sizeof id1, "%s", r.id);
    snprintf(p, sizeof p, "%s/%s/secret.txt", M2, id1);
    CHECK(!slurp(p, buf, sizeof buf) && !strstr(buf, "secret text 1234") && !memcmp(buf, "LBK1", 4), "the backup file is encrypted");
    memset(&r, 0, sizeof r);
    r.home = H; r.machine = M2; r.key = key; r.min_free = 1;
    r.excl[r.nexcl++] = "dest";
    rc = bk_snapshot(&r, time(NULL) - 300);
    CHECK(rc == 0 && r.copied == 0 && r.linked > 3, "encrypted: unchanged files are linked (%llu copied)", (unsigned long long)r.copied);
    snprintf(q, sizeof q, "%s/%s/secret.txt", M2, r.id);
    CHECK(ino(p) == ino(q), "encrypted: same inode");
    snprintf(q, sizeof q, "%s/secret.txt", H);
    unlink(q);
    BkRestore o = { .key = key };
    rc = bk_restore(M2, id1, "secret.txt", H, 0, " (restored)", &o);
    CHECK(rc == 0 && o.files == 1 && !slurp(q, buf, sizeof buf) && !strcmp(buf, "very secret text 1234\n"), "restore decrypts");
    rc = bk_restore(M2, id1, "secret.txt", H, 0, " (restored)", &o);
    CHECK(rc == 0 && o.kept_both == 0 && o.skipped == 1, "encrypted: identical to the current file, not duplicated");
    /* tampered backup: nothing is written */
    int fd = open(p, O_RDWR);
    unsigned char b;
    pread(fd, &b, 1, BKC_HDR + 2); b ^= 0x40; pwrite(fd, &b, 1, BKC_HDR + 2);
    close(fd);
    unlink(q);
    BkRestore o2 = { .key = key };
    bk_restore(M2, id1, "secret.txt", H, 0, " (restored)", &o2);
    CHECK(o2.errors == 1 && !exists(q), "tampered backup: error and nothing written (%s)", o2.error);
    bkc_wipe(key, sizeof key);
}

int main(void) {
    test_retention();
    test_rel();

    snprintf(T, sizeof T, "/tmp/lordos-backup-test-%d", (int)getpid());
    mkdir(T, 0700);
    char h0[PATH_MAX];
    snprintf(h0, sizeof h0, "%s/home", T);
    mkdir(h0, 0755);
    if (!realpath(h0, H)) return 2;
    snprintf(M, sizeof M, "%s/dest/" BK_DIRNAME "/host-user", H);   /* destination INSIDE the home folder */
    fsops_init(H, "test-user");

    put("a.txt", "version 1\n");
    mk("dir"); put_big("dir/b.bin", 1);
    mk("sub"); mk("sub/deep"); put("sub/deep/c.txt", "c\n");
    mk(".cache"); put(".cache/junk", "x");
    mk("Excluded"); put("Excluded/x", "x");
    mk(".config"); mk(".config/axia"); put(".config/axia/settings.json", "{\"language\":\"es\"}\n");
    char p[PATH_MAX], q[PATH_MAX], buf[256];
    snprintf(p, sizeof p, "%s/escape", H); symlink("/etc/passwd", p);
    snprintf(p, sizeof p, "%s/relative", H); symlink("a.txt", p);
    snprintf(p, sizeof p, "%s/fifo", H); mkfifo(p, 0600);

    time_t t0 = time(NULL) - 7200;
    BkRun r;
    /* the destination inside home: skipped by dev/ino */
    char skip[PATH_MAX], d0[PATH_MAX];
    snprintf(d0, sizeof d0, "%s/dest/" BK_DIRNAME, H);
    init_run(&r);
    r.skip_real = d0;
    {
        char mkp[PATH_MAX];
        snprintf(mkp, sizeof mkp, "%s/dest", H); mkdir(mkp, 0700); mkdir(d0, 0700);
        if (realpath(d0, skip)) r.skip_real = skip;
    }
    int rc = bk_snapshot(&r, t0);
    CHECK(rc == 0, "first snapshot (%d: %s)", rc, r.error);
    char id1[BK_ID_LEN];
    snprintf(id1, sizeof id1, "%s", r.id);
    snap_path(id1, "a.txt", p, sizeof p);
    CHECK(!slurp(p, buf, sizeof buf) && !strcmp(buf, "version 1\n"), "a.txt copied");
    snap_path(id1, "sub/deep/c.txt", p, sizeof p);
    CHECK(exists(p), "deep folders");
    snap_path(id1, ".cache", p, sizeof p);
    CHECK(!exists(p), ".cache excluded");
    snap_path(id1, "Excluded", p, sizeof p);
    CHECK(!exists(p), "user exclusion");
    snap_path(id1, "fifo", p, sizeof p);
    CHECK(!exists(p), "fifo skipped");
    snap_path(id1, "dest", p, sizeof p);
    snprintf(q, sizeof q, "%s/" BK_DIRNAME, p);
    CHECK(!exists(q), "the destination is not copied into itself");
    snap_path(id1, "escape", p, sizeof p);
    struct stat st;
    CHECK(!lstat(p, &st) && S_ISLNK(st.st_mode), "symlink stored as a symlink");
    ssize_t k = readlink(p, buf, sizeof buf - 1);
    CHECK(k > 0 && (buf[k] = 0, !strcmp(buf, "/etc/passwd")), "with the same target text");
    CHECK(r.skipped >= 1, "special files counted (%llu)", (unsigned long long)r.skipped);

    /* second: a.txt changes, the rest is linked */
    put("a.txt", "version 2, a bit longer\n");
    init_run(&r);
    r.skip_real = skip;
    rc = bk_snapshot(&r, t0 + 3600);
    CHECK(rc == 0, "second snapshot (%s)", r.error);
    char id2[BK_ID_LEN];
    snprintf(id2, sizeof id2, "%s", r.id);
    CHECK(strcmp(id1, id2) < 0, "ids in order");
    snap_path(id1, "dir/b.bin", p, sizeof p);
    snap_path(id2, "dir/b.bin", q, sizeof q);
    CHECK(ino(p) && ino(p) == ino(q), "dedup: same inode");
    snap_path(id1, "a.txt", p, sizeof p);
    snap_path(id2, "a.txt", q, sizeof q);
    CHECK(ino(p) != ino(q), "changed: new copy");
    CHECK(r.copied == 1, "only one was copied (%llu)", (unsigned long long)r.copied);
    CHECK(r.linked >= 3, "linked (%llu)", (unsigned long long)r.linked);

    /* interrupted and resumed run */
    put("aaa-new.txt", "new before the interruption\n");
    put_big("zz-big.bin", 40);
    init_run(&r);
    r.skip_real = skip;
    r.checkpoint = cut_cb;
    cut_after = 12u << 20;
    rc = bk_snapshot(&r, t0 + 5400);
    CHECK(rc == 1, "interrupted (%d)", rc);
    char inprog[PATH_MAX];
    snprintf(inprog, sizeof inprog, "%s/.inprogress-%s", M, r.id);
    CHECK(exists(inprog), ".inprogress is left");
    char ids[16][BK_ID_LEN];
    CHECK(bk_list(M, ids, 16) == 2, "the interrupted run is not listed");
    snprintf(p, sizeof p, "%s/zz-big.bin", inprog);
    CHECK(!exists(p), "the half-copied file has no final name");
    snprintf(p, sizeof p, "%s/aaa-new.txt", inprog);
    ino_t copied_before = ino(p);
    CHECK(copied_before != 0, "what was copied before the interruption is there");
    cut_after = 0;
    init_run(&r);
    r.skip_real = skip;
    rc = bk_snapshot(&r, t0 + 5500);
    CHECK(rc == 0, "resumed (%s)", r.error);
    char id3[BK_ID_LEN];
    snprintf(id3, sizeof id3, "%s", r.id);
    snap_path(id3, "aaa-new.txt", p, sizeof p);
    CHECK(ino(p) == copied_before, "resumed: already copied files are linked (not copied again)");
    CHECK(r.copied == 1, "resumed: only the big one (%llu)", (unsigned long long)r.copied);
    snap_path(id3, "zz-big.bin", p, sizeof p);
    CHECK(!stat(p, &st) && st.st_size == 40 << 20, "the big file is complete");
    CHECK(!exists(inprog), "no .inprogress");
    DIR *dd = opendir(M);
    int junk = 0;
    for (struct dirent *e; dd && (e = readdir(dd));) if (e->d_name[0] == '.' && strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) junk++;
    if (dd) closedir(dd);
    CHECK(junk == 0, "no leftovers of the interrupted run (%d)", junk);

    /* restore */
    BkRestore o = { 0 };
    snprintf(p, sizeof p, "%s/a.txt", H);
    unlink(p);
    rc = bk_restore(M, id1, "a.txt", H, 0, " (restored)", &o);
    CHECK(rc == 0 && o.files == 1, "restore a deleted file (%d %s)", rc, o.error);
    CHECK(!slurp(p, buf, sizeof buf) && !strcmp(buf, "version 1\n"), "with the content it had then");
    put("a.txt", "version 3 current\n");
    rc = bk_restore(M, id1, "a.txt", H, 0, " (restored)", &o);
    snprintf(q, sizeof q, "%s/a (restored).txt", H);
    CHECK(rc == 0 && o.kept_both == 1 && !slurp(q, buf, sizeof buf) && !strcmp(buf, "version 1\n"), "keep both");
    CHECK(!slurp(p, buf, sizeof buf) && !strcmp(buf, "version 3 current\n"), "the current file is not overwritten");
    rc = bk_restore(M, id1, "a.txt", H, 0, " (restored)", &o);
    snprintf(q, sizeof q, "%s/a (restored) 2.txt", H);
    CHECK(rc == 0 && exists(q), "second duplicate is numbered");
    rc = bk_restore(M, id1, "a.txt", H, 1, " (restored)", &o);
    CHECK(rc == 0 && o.replaced == 1 && !slurp(p, buf, sizeof buf) && !strcmp(buf, "version 1\n"), "replace");
    /* a folder that is gone */
    snprintf(p, sizeof p, "%s/sub/deep/c.txt", H); unlink(p);
    snprintf(p, sizeof p, "%s/sub/deep", H); rmdir(p);
    rc = bk_restore(M, id2, "sub/deep/c.txt", H, 0, " (restored)", &o);
    snprintf(p, sizeof p, "%s/sub/deep/c.txt", H);
    CHECK(rc == 0 && exists(p), "restore creates missing folders");
    /* protected: a .desktop planted in the backup and the settings are not written */
    char plant[PATH_MAX];
    snap_path(id3, ".local", plant, sizeof plant); chmod(plant, 0700); mkdir(plant, 0700);
    snap_path(id3, ".local/share", plant, sizeof plant); mkdir(plant, 0700);
    snap_path(id3, ".local/share/applications", plant, sizeof plant); mkdir(plant, 0700);
    snap_path(id3, ".local/share/applications/evil.desktop", plant, sizeof plant);
    FILE *f = fopen(plant, "w"); if (f) { fputs("[Desktop Entry]\nExec=rm -rf ~\n", f); fclose(f); }
    put(".config/axia/settings.json", "{\"language\":\"pt\"}\n");
    rc = bk_restore(M, id3, "", H, 1, " (restored)", &o);
    CHECK(rc == 0, "restore everything (%s)", o.error);
    snprintf(p, sizeof p, "%s/.local/share/applications/evil.desktop", H);
    CHECK(!exists(p), "does not write .desktop");
    snprintf(p, sizeof p, "%s/.config/axia/settings.json", H);
    CHECK(!slurp(p, buf, sizeof buf) && strstr(buf, "pt"), "does not overwrite settings.json");
    CHECK(o.skipped >= 2, "protected paths skipped (%llu)", (unsigned long long)o.skipped);
    /* links: the link is restored, not its target; never descend through a link in the backup */
    snprintf(p, sizeof p, "%s/escape", H); unlink(p);
    rc = bk_restore(M, id1, "escape", H, 0, " (restored)", &o);
    CHECK(rc == 0 && !lstat(p, &st) && S_ISLNK(st.st_mode), "symlink restored as a symlink");
    snap_path(id1, "outside", plant, sizeof plant);
    symlink("/etc", plant);
    rc = bk_restore(M, id1, "outside/passwd", H, 0, " (restored)", &o);
    snprintf(p, sizeof p, "%s/outside", H);
    CHECK(rc != 0 && !exists(p), "no escape through a link in the backup");
    rc = bk_restore(M, id1, "../../../etc/passwd", H, 0, " (restored)", &o);
    CHECK(rc != 0, ".. rejected");
    rc = bk_restore(M, id1, "a.txt", "/etc", 0, " (restored)", &o);
    CHECK(rc != 0, "destination outside the roots rejected");
    rc = bk_restore(M, id1, "a.txt", H, 0, "/x", &o);
    CHECK(rc != 0, "suffix with a slash rejected");
    /* a link in the home folder pointing to a protected folder */
    snprintf(p, sizeof p, "%s/shortcut", H);
    snprintf(q, sizeof q, "%s/.local/share/applications", H);
    mkdir(q, 0700);
    symlink(q, p);
    snap_path(id1, "shortcut", plant, sizeof plant); unlink(plant); mkdir(plant, 0700);
    snap_path(id1, "shortcut/x.desktop", plant, sizeof plant);
    f = fopen(plant, "w"); if (f) { fputs("x", f); fclose(f); }
    rc = bk_restore(M, id1, "shortcut/x.desktop", H, 0, " (restored)", &o);
    snprintf(p, sizeof p, "%s/x.desktop", q);
    CHECK(!exists(p), "does not write into a protected folder through a link (%d)", rc);

    /* pruning: out of space, only the newest stays */
    int before = bk_list(M, ids, 16);
    bk_prune(M, time(NULL), UINT64_MAX);
    int after = bk_list(M, ids, 16);
    CHECK(before == 3 && after == 1 && !strcmp(ids[0], id3), "pruning for space: the newest stays (%d → %d)", before, after);
    CHECK(bk_delete(M, "../../x") != 0, "delete with an invalid name");

    test_crypt();

    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof cmd, "chmod -R u+w '%s' && rm -rf '%s'", T, T);
    if (system(cmd)) { /* left in /tmp */ }
    printf("backup-test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
