/* fsasync_test.c — lord.fs's Async calls (fsasync.c) on a temporary home folder.
 *
 * The 0.8.8.7 bug: Files called lord.fs.list / stat / trash on the interface's thread, so a Cloud folder (rclone FUSE)
 * that took seconds to answer froze the whole desktop. A test hook stands in for the slow file system (it holds a request
 * on its worker until the test lets it go, as rclone holds a FUSE call while it asks the provider), and the test checks:
 *   - submitting never waits for the file system (a "slow" request is held, the call returns at once);
 *   - other requests finish while slow ones are held (one stuck mount doesn't hold up the others);
 *   - the answers: list (entries with the columns' fields), stat, trash, free;
 *   - the checks are the synchronous calls' own: outside the roots, /proc, the protected folders, the Trash only;
 *   - a full queue answers fs.busy at once instead of waiting. */
#include "fsasync.h"
#include "fsops.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char H[256];

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* ---- the slow file system: a request whose path holds "/slow" waits until the gate opens ---- */
static pthread_mutex_t gmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gcv = PTHREAD_COND_INITIALIZER;
static int gate_open, held;
static void hook(int op, const char *path) {
    (void)op;
    if (!strstr(path, "/slow")) return;
    pthread_mutex_lock(&gmu);
    held++;
    pthread_cond_broadcast(&gcv);
    while (!gate_open) pthread_cond_wait(&gcv, &gmu);
    held--;
    pthread_mutex_unlock(&gmu);
}
static void open_gate(void) { pthread_mutex_lock(&gmu); gate_open = 1; pthread_cond_broadcast(&gcv); pthread_mutex_unlock(&gmu); }
static int held_now(void) { pthread_mutex_lock(&gmu); int n = held; pthread_mutex_unlock(&gmu); return n; }

/* ---- answers, by request id ---- */
#define MAXR 512
static char *answer[MAXR + 1];
static void reply(void *ud, int req, const char *json) {
    (void)ud;
    if (req > 0 && req <= MAXR) { free(answer[req]); answer[req] = strdup(json); }
}
/* waits (as the main loop does: poll on the eventfd) until request req is answered, at most ms */
static const char *wait_for(int req, int ms) {
    double end = now_ms() + ms;
    while (req > 0 && req <= MAXR && !answer[req] && now_ms() < end) {
        struct pollfd p = { fsasync_fd(), POLLIN, 0 };
        if (poll(&p, 1, 20) > 0) fsasync_dispatch(reply, NULL);
    }
    return req > 0 && req <= MAXR ? answer[req] : NULL;
}

static void put(const char *rel, const char *text) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", H, rel);
    FILE *f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}
static int exists(const char *rel) { char p[512]; struct stat st; snprintf(p, sizeof p, "%s/%s", H, rel); return lstat(p, &st) == 0; }
static int has(const char *json, const char *s) { return json && strstr(json, s) != NULL; }

static int submit(int op, const char *rel) {
    char p[512], err[256] = "";
    if (rel[0] == '/') snprintf(p, sizeof p, "%s", rel); else snprintf(p, sizeof p, "%s/%s", H, rel);
    int r = fsasync_submit(op, p, err, sizeof err);
    if (r < 0) fprintf(stderr, "submit %s: %s\n", p, err);
    return r;
}

int main(void) {
    snprintf(H, sizeof H, "/tmp/fsasync-test-%d", (int)getpid());
    mkdir(H, 0755);
    char real[256];
    if (!realpath(H, real)) return 1;
    snprintf(H, sizeof H, "%s", real);
    fsops_init(H, "nobody");
    fsasync_test_hook = hook;
    char p[512];
    snprintf(p, sizeof p, "%s/slow", H); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/fast", H); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/fast/sub", H); mkdir(p, 0755);
    put("fast/a.txt", "hello");
    put("slow/b.txt", "world");
    snprintf(p, sizeof p, "%s/slow2", H); mkdir(p, 0755);
    put("slow2/c.txt", "!");
    put("fast/del.txt", "bye");
    snprintf(p, sizeof p, "%s/.config", H); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/.config/lordos", H); mkdir(p, 0755);
    put(".config/lordos/settings.json", "{}");
    CHECK(fsasync_fd() == -1);                      /* nothing is created before the first request */

    /* 1. a slow folder: the call returns at once, and the worker is held */
    double t0 = now_ms();
    int slow_list = submit(FSA_LIST, "slow");
    int slow_trash = submit(FSA_TRASH, "slow2/c.txt");
    double took = now_ms() - t0;
    CHECK(slow_list > 0 && slow_trash > 0);
    CHECK(took < 50);                                /* the interface's thread didn't wait for the file system */
    for (int k = 0; k < 200 && held_now() < 2; k++) usleep(5000);
    CHECK(held_now() == 2);
    CHECK(fsasync_fd() >= 0);

    /* 2. meanwhile, other folders answer (a new worker each, the slow ones keep theirs) */
    int fast = submit(FSA_LIST, "fast");
    const char *j = wait_for(fast, 3000);
    CHECK(j != NULL);
    CHECK(has(j, "\"entries\":["));
    CHECK(has(j, "\"name\":\"a.txt\"") && has(j, "\"size\":5"));
    CHECK(has(j, "\"name\":\"sub\",\"link\":false,\"dir\":true"));
    CHECK(has(j, "\"mode\":\"") && has(j, "\"user\":\"") && has(j, "\"atime\":"));
    CHECK(!answer[slow_list] && !answer[slow_trash]);      /* still held */
    CHECK(held_now() == 2);

    int st = submit(FSA_STAT, "fast/a.txt");
    j = wait_for(st, 3000);
    CHECK(has(j, "\"stat\":{\"name\":\"a.txt\""));
    CHECK(has(j, "\"writable\":true"));
    st = submit(FSA_STAT, "/etc/hostname");                    /* readable, never writable */
    j = wait_for(st, 3000);
    CHECK(has(j, "\"writable\":false") || has(j, "\"error\":\"sys.errno"));

    int fr = submit(FSA_FREE, "fast");
    j = wait_for(fr, 3000);
    CHECK(has(j, "\"total\":") && has(j, "\"free\":"));

    /* 3. the checks are lord.fs's own */
    int r = submit(FSA_LIST, "/proc/self");
    CHECK(has(wait_for(r, 3000), "\"error\":\"fs.systemFolder\""));
    r = submit(FSA_LIST, "missing-folder");
    CHECK(has(wait_for(r, 3000), "\"error\":\"fs.outsideHome\""));
    r = submit(FSA_TRASH, "/etc/hostname");                    /* outside the writable roots */
    CHECK(has(wait_for(r, 3000), "\"error\":\"fs.outsideHome\""));
    r = submit(FSA_TRASH, ".config/lordos/settings.json");     /* the deny list (fsops_protected) */
    CHECK(has(wait_for(r, 3000), "\"error\":\"fs.protected\""));
    CHECK(exists(".config/lordos/settings.json"));
    r = submit(FSA_TRASH, ".config/lordos");                   /* a parent of a protected path */
    CHECK(has(wait_for(r, 3000), "\"error\":\"fs.protected\""));
    CHECK(exists(".config/lordos/settings.json"));
    char err[256] = "";
    CHECK(fsasync_submit(99, H, err, sizeof err) < 0 && !strncmp(err, "sys.badArgument", 15));
    CHECK(fsasync_submit(FSA_LIST, "", err, sizeof err) < 0);

    /* 4. the Trash, off the thread */
    r = submit(FSA_TRASH, "fast/del.txt");
    CHECK(has(wait_for(r, 3000), "\"ok\":true"));
    CHECK(!exists("fast/del.txt"));
    CHECK(exists(".local/share/Trash/files/del.txt"));
    CHECK(exists(".local/share/Trash/info/del.txt.trashinfo"));

    /* new folder and rename, off the thread, with lord.fs.mkdir / rename's checks */
    r = submit(FSA_MKDIR, "fast/New folder");
    CHECK(has(wait_for(r, 3000), "\"ok\":true"));
    CHECK(exists("fast/New folder"));
    r = submit(FSA_MKDIR, ".config/lordos/settings.json");
    CHECK(has(wait_for(r, 3000), "\"error\":\"fs.protected\""));
    char from[512], to[512];
    snprintf(from, sizeof from, "%s/fast/New folder", H);
    snprintf(to, sizeof to, "%s/fast/Photos", H);
    r = fsasync_submit2(FSA_RENAME, from, to, err, sizeof err);
    CHECK(has(wait_for(r, 3000), "\"ok\":true"));
    CHECK(exists("fast/Photos") && !exists("fast/New folder"));
    snprintf(from, sizeof from, "%s/fast/a.txt", H);
    snprintf(to, sizeof to, "%s/fast/Photos", H);
    r = fsasync_submit2(FSA_RENAME, from, to, err, sizeof err);   /* never over an existing name */
    CHECK(has(wait_for(r, 3000), "\"error\":\"fs.exists"));
    snprintf(to, sizeof to, "%s/.config/lordos/settings.json", H);
    r = fsasync_submit2(FSA_RENAME, from, to, err, sizeof err);   /* nor onto a protected one */
    CHECK(has(wait_for(r, 3000), "\"error\":\"fs.protected"));
    snprintf(to, sizeof to, "/tmp/fsasync-escape-%d", (int)getpid());
    r = fsasync_submit2(FSA_RENAME, from, to, err, sizeof err);   /* nor out of the roots */
    CHECK(has(wait_for(r, 3000), "\"error\":\"fs.outsideHome"));
    CHECK(exists("fast/a.txt"));
    CHECK(fsasync_submit2(FSA_RENAME, from, NULL, err, sizeof err) < 0);
    CHECK(fsasync_submit2(FSA_LIST, from, to, err, sizeof err) < 0);

    /* 5. a full queue is refused at once (fs.busy), never waited for */
    int ids[FSA_QUEUE + 4], n = 0, busy = 0;
    t0 = now_ms();
    for (int i = 0; i < FSA_QUEUE + 4; i++) {
        char e2[256] = "";
        snprintf(p, sizeof p, "%s/slow", H);
        int id = fsasync_submit(FSA_STAT, p, e2, sizeof e2);
        if (id > 0) ids[n++] = id; else if (!strncmp(e2, "fs.busy", 7)) busy++;
    }
    CHECK(now_ms() - t0 < 200);
    CHECK(busy >= 4);
    CHECK(n + 2 <= FSA_QUEUE);                        /* the two held at the start count */

    /* 6. the slow file system answers: everything held comes back */
    open_gate();
    j = wait_for(slow_list, 5000);
    CHECK(has(j, "\"name\":\"b.txt\""));
    CHECK(has(wait_for(slow_trash, 5000), "\"ok\":true"));
    CHECK(!exists("slow2/c.txt") && exists("slow/b.txt"));
    int back = 0;
    for (int i = 0; i < n; i++) if (ids[i] <= MAXR && wait_for(ids[i], 5000)) back++;
    int expect = 0;
    for (int i = 0; i < n; i++) if (ids[i] <= MAXR) expect++;
    CHECK(back == expect);
    r = submit(FSA_LIST, "fast");                     /* the queue has room again */
    CHECK(r > 0);
    CHECK(wait_for(r, 3000) != NULL);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", H);
    if (system(cmd)) fprintf(stderr, "could not remove %s\n", H);
    for (int i = 0; i <= MAXR; i++) free(answer[i]);
    printf("fsasync-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
