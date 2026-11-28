/* bgsave_test.c — settings.json and the clipboard's stores saved off the interface's thread (bgsave.c, GitHub #250).
 *
 * The #250 bug: on a laptop's spinning disk, lord.settings.save (after every window move) and
 * lord.clipboard.saveStore (after every copy) froze the desktop 50–650 ms each, the rename and the fsync waiting for
 * the disk on lord-server's only thread. A test hook stands in for that disk (it holds the worker's write until the
 * test lets it go) and the test checks:
 *   - a save returns at once while the disk is slow;
 *   - saves that come while one is held collapse into the newest (the file never goes back to older content);
 *   - bgsave_flush waits until the newest text is on the disk (lord.settings.load, a theme switch, the session's end);
 *   - a durable save after a queued one wins (flush, then written now);
 *   - private files are 0600 from a mkstemp name, and no temporary file is left behind, also when the write fails;
 *   - with every slot taken, one more file is written on the caller's thread instead of being lost. */
#include "bgsave.h"

#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* ---- the slow disk: a write whose path holds "/slow" waits until the gate opens ---- */
static pthread_mutex_t gmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gcv = PTHREAD_COND_INITIALIZER;
static int gate_open, held;
static void hook(const char *dst) {
    if (!strstr(dst, "/slow")) return;
    pthread_mutex_lock(&gmu);
    held++;
    pthread_cond_broadcast(&gcv);
    while (!gate_open) pthread_cond_wait(&gcv, &gmu);
    held--;
    pthread_mutex_unlock(&gmu);
}
static void set_gate(int open) { pthread_mutex_lock(&gmu); gate_open = open; pthread_cond_broadcast(&gcv); pthread_mutex_unlock(&gmu); }
static void wait_held(void) { pthread_mutex_lock(&gmu); while (!held) pthread_cond_wait(&gcv, &gmu); pthread_mutex_unlock(&gmu); }
static int writes(void) { bgsave_flush(NULL); return bgsave_writes; }

static char D[256];

/* bgsave_edit's edit: the old text, then the data */
static char *append_edit(const char *dst, const char *old, size_t oldlen, const char *data, size_t len, size_t *outlen) {
    (void)dst;
    char *out = malloc(oldlen + len + 1);
    if (!out) return NULL;
    memcpy(out, old, oldlen);
    memcpy(out + oldlen, data, len);
    out[oldlen + len] = 0;
    *outlen = oldlen + len;
    return out;
}
static char *keep_edit(const char *dst, const char *old, size_t oldlen, const char *data, size_t len, size_t *outlen) {
    (void)dst; (void)old; (void)oldlen; (void)data; (void)len; (void)outlen;
    return NULL;
}

static char *slurp(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    static char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}
static int same(const char *p, const char *want) { const char *got = slurp(p); return got && !strcmp(got, want); }
static int entries(const char *dir) {
    DIR *d = opendir(dir);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    if (d) closedir(d);
    return n;
}

int main(void) {
    snprintf(D, sizeof D, "%s/bgsave-test.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(D)) { perror("mkdtemp"); return 1; }
    bgsave_test_hook = hook;
    char dst[600], tmp[700];

    /* 1. a save returns at once while the disk holds the write, and the newest of several wins */
    snprintf(dst, sizeof dst, "%s/slow-settings.json", D);
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    set_gate(0);
    int w0 = writes();
    CHECK(bgsave_put(dst, tmp, "v1", 2, 0) == 0);
    wait_held();                                     /* v1 is on the "disk" now, held */
    double t = now_ms();
    CHECK(bgsave_put(dst, tmp, "v2", 2, 0) == 0);
    CHECK(bgsave_put(dst, tmp, "v3", 2, 0) == 0);
    CHECK(now_ms() - t < 50);                        /* did not wait for the held write */
    usleep(100 * 1000);
    set_gate(1);
    bgsave_flush(dst);
    CHECK(same(dst, "v3"));
    CHECK(writes() - w0 == 2);                       /* v1, then v3; v2 was never written */
    CHECK(access(tmp, F_OK) != 0);

    /* 2. a durable save after a queued one: flush, then written now; the queued text never lands after it */
    set_gate(0);
    CHECK(bgsave_put(dst, tmp, "old", 3, 0) == 0);
    wait_held();
    CHECK(bgsave_put(dst, tmp, "older-queued", 12, 0) == 0);
    set_gate(1);
    bgsave_flush(dst);
    CHECK(bgsave_write_now(dst, tmp, "durable", 7, BGS_FSYNC) == 0);
    bgsave_flush(NULL);
    CHECK(same(dst, "durable"));

    /* 2b. (QA 0.8.9.6.2, BUG-I) a durable save queued on the worker (session restore's autosave) stays durable when a
     * plain save of the same file replaces it before it is written, and its folder is synced too */
    set_gate(0);
    CHECK(bgsave_put(dst, tmp, "busy", 4, 0) == 0);
    wait_held();
    CHECK(bgsave_put(dst, tmp, "marker", 6, BGS_FSYNC | BGS_SYNCDIR) == 0);
    CHECK(bgsave_put(dst, tmp, "plain-after", 11, 0) == 0);
    set_gate(1);
    bgsave_flush(dst);
    CHECK(same(dst, "plain-after"));
    CHECK((bgsave_last_flags & (BGS_FSYNC | BGS_SYNCDIR)) == (BGS_FSYNC | BGS_SYNCDIR));
    /* once written, the next plain save is plain again */
    CHECK(bgsave_put(dst, tmp, "next", 4, 0) == 0);
    bgsave_flush(dst);
    CHECK(same(dst, "next") && bgsave_last_flags == 0);

    /* 2c. (QA 0.8.9.6.2, PI-1) lord.fs.write's sync later: queued once per path, done by the worker, flush waits for it */
    CHECK(bgsave_sync_later(dst) == 0 && bgsave_sync_later(dst) == 0);
    CHECK(bgsave_sync_later("") <= 1 && bgsave_sync_later(NULL) == -1);
    bgsave_flush(NULL);
    CHECK(same(dst, "next"));

    /* 3. a private store: 0600 from a mkstemp name next to it, nothing else left in the folder */
    char sub[300];
    snprintf(sub, sizeof sub, "%s/clip", D);
    mkdir(sub, 0700);
    snprintf(dst, sizeof dst, "%s/history.json", sub);
    snprintf(tmp, sizeof tmp, "%s/.history.json.lordos-XXXXXX", sub);
    CHECK(bgsave_put(dst, tmp, "{\"items\":[]}", 12, BGS_FSYNC | BGS_PRIVATE | BGS_UNIQUE) == 0);
    bgsave_flush(dst);
    struct stat st;
    CHECK(!stat(dst, &st) && (st.st_mode & 0777) == 0600);
    CHECK(same(dst, "{\"items\":[]}"));
    CHECK(entries(sub) == 1);

    /* 4. a plain file is 0666 less the umask, as fopen made it */
    mode_t um = umask(022);
    snprintf(dst, sizeof dst, "%s/plain.json", D);
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    CHECK(bgsave_put(dst, tmp, "x", 1, 0) == 0);
    bgsave_flush(NULL);
    CHECK(!stat(dst, &st) && (st.st_mode & 0777) == 0644);
    umask(um);

    /* 5. a write that fails on the worker: logged, nothing left, flush still returns */
    snprintf(dst, sizeof dst, "%s/missing/settings.json", D);
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    CHECK(bgsave_put(dst, tmp, "x", 1, 0) == 0);
    bgsave_flush(dst);
    CHECK(access(dst, F_OK) != 0);

    /* 6. every slot taken by a held write: one more file is written now, on this thread */
    char many[BGS_SLOTS][600];
    set_gate(0);
    for (int i = 0; i < BGS_SLOTS; i++) {
        snprintf(many[i], sizeof many[i], "%s/slow-%d.json", D, i);
        snprintf(tmp, sizeof tmp, "%s/slow-%d.json.tmp", D, i);
        CHECK(bgsave_put(many[i], tmp, "s", 1, 0) == 0);
    }
    wait_held();
    snprintf(dst, sizeof dst, "%s/extra.json", D);
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    CHECK(bgsave_put(dst, tmp, "now", 3, 0) == 0);
    CHECK(same(dst, "now"));                         /* already there: written on the caller's thread */
    set_gate(1);
    bgsave_flush(NULL);
    for (int i = 0; i < BGS_SLOTS; i++) CHECK(same(many[i], "s"));

    /* 7. an edit (the portal's GSettings keyfile, QA ws174): made on the worker from the file's current text; one
     * queued while an older one is held replaces it; NULL leaves the file */
    snprintf(dst, sizeof dst, "%s/slow-keyfile", D);
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    CHECK(bgsave_put(dst, tmp, "a", 1, 0) == 0);
    bgsave_flush(dst);
    set_gate(0);
    double e0 = now_ms();
    CHECK(bgsave_edit(dst, tmp, append_edit, "b", 1, 0) == 0);
    wait_held();
    CHECK(bgsave_edit(dst, tmp, append_edit, "c", 1, 0) == 0);
    CHECK(bgsave_edit(dst, tmp, append_edit, "d", 1, 0) == 0);
    CHECK(now_ms() - e0 < 500);                       /* returned while the disk was held */
    set_gate(1);
    bgsave_flush(dst);
    CHECK(same(dst, "abd"));                         /* b on "a", then only the newest of c and d, on "ab" */
    CHECK(bgsave_edit(dst, tmp, keep_edit, "x", 1, 0) == 0);
    bgsave_flush(dst);
    CHECK(same(dst, "abd"));
    CHECK(bgsave_edit(dst, tmp, NULL, "x", 1, 0) == -1);
    snprintf(dst, sizeof dst, "%s/new-keyfile", D);
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    CHECK(bgsave_edit(dst, tmp, append_edit, "n", 1, 0) == 0);
    bgsave_flush(dst);
    CHECK(same(dst, "n"));                           /* no file yet: the edit starts from "" */

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", D);
    if (system(cmd)) { /* left in TMPDIR */ }
    printf("bgsave-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
