/* webmem_test.c — lord.webapps.memory's sum (src/web/webmem.c) on a made-up /proc tree in a throwaway folder: the
 * Pss of a lord-web and of every process under it (children and grandchildren), never of its parent, its siblings or
 * an unrelated process; a process name with spaces and parentheses; a descendant whose smaps_rollup can't be read is
 * counted apart; a missing root; the WEBMEM_MAX_PROCS bound; and the "Pss:" line parser.
 *
 *   make -C server build/webmem-test        (also in `make test`) */
#include "../src/web/webmem.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char root[PATH_MAX];

static void put(const char *rel, const char *text) {
    char p[PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    fputs(text, f);
    fclose(f);
}

/* a process: its stat (name, then its parent) and, when pss >= 0, a smaps_rollup with that Pss */
static void proc(int pid, const char *name, int ppid, long pss) {
    char d[PATH_MAX + 32], t[512];
    snprintf(d, sizeof d, "%s/%d", root, pid);
    mkdir(d, 0755);
    snprintf(d, sizeof d, "%d/stat", pid);
    snprintf(t, sizeof t, "%d (%s) S %d %d %d 0 -1 4194560 100 0 0 0\n", pid, name, ppid, pid, pid);
    put(d, t);
    if (pss < 0) return;
    snprintf(d, sizeof d, "%d/smaps_rollup", pid);
    snprintf(t, sizeof t, "55d0c0000000-7ffd00000000 ---p 00000000 00:00 0  [rollup]\nRss:  %ld kB\nPss:  %ld kB\nPss_Anon:  1 kB\n", pss * 2, pss);
    put(d, t);
}

int main(void) {
    snprintf(root, sizeof root, "/tmp/webmem-test-XXXXXX");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }

    proc(50, "lord-server", 1, 90000);                 /* the parent: never counted */
    proc(100, "lord-web", 50, 61000);                  /* the app's process */
    proc(101, "bwrap", 100, 500);
    proc(102, "WebKitWebProcess", 101, 542000);        /* a grandchild, under bubblewrap */
    proc(103, "WebKit Network (x) )", 100, 67000);     /* spaces and parentheses in its name */
    proc(104, "WebKitWebProcess", 100, -1);            /* unreadable: counted apart */
    proc(200, "lord-web", 50, 70000);                  /* a sibling: another app */
    proc(201, "WebKitWebProcess", 200, 300000);
    proc(300, "bash", 1, 4000);
    put("self", "not a process\n");
    char d[PATH_MAX + 16];
    snprintf(d, sizeof d, "%s/sys", root);
    mkdir(d, 0755);

    WebMem m;
    CHECK(webmem_tree(root, 100, &m) == 0, "the tree of 100");
    CHECK(m.pss_kb == 61000 + 500 + 542000 + 67000, "sum %lld", m.pss_kb);
    CHECK(m.procs == 4, "procs %d", m.procs);
    CHECK(m.unreadable == 1, "unreadable %d", m.unreadable);

    CHECK(webmem_tree(root, 200, &m) == 0 && m.pss_kb == 370000 && m.procs == 2 && m.unreadable == 0, "the sibling alone: %lld %d", m.pss_kb, m.procs);
    CHECK(webmem_tree(root, 102, &m) == 0 && m.pss_kb == 542000 && m.procs == 1, "a leaf: %lld", m.pss_kb);
    CHECK(webmem_tree(root, 104, &m) == -1, "a root without smaps_rollup");
    CHECK(webmem_tree(root, 999, &m) == -1, "a root that is not there");
    CHECK(webmem_tree(root, 1, &m) == -1 && webmem_tree(root, 0, &m) == -1, "init and 0 are never a web app");
    snprintf(d, sizeof d, "%s/nothing", root);
    CHECK(webmem_tree(d, 100, &m) == -1, "no /proc at all");

    /* a fork bomb under it: bounded */
    proc(400, "lord-web", 50, 1000);
    for (int i = 0; i < WEBMEM_MAX_PROCS + 20; i++) proc(401 + i, "child", i ? 400 + i : 400, 10);
    CHECK(webmem_tree(root, 400, &m) == 0, "the long chain");
    CHECK(m.procs + m.unreadable <= WEBMEM_MAX_PROCS && m.procs == WEBMEM_MAX_PROCS, "bounded: %d", m.procs);
    CHECK(m.pss_kb == 1000 + 10LL * (WEBMEM_MAX_PROCS - 1), "bounded sum %lld", m.pss_kb);

    /* the line parser */
    CHECK(webmem_pss_line("Rss: 10 kB\nPss: 7 kB\n") == 7, "Pss");
    CHECK(webmem_pss_line("Pss_Anon: 3 kB\nPss: 9 kB\n") == 9, "Pss_Anon is not the Pss: line");
    CHECK(webmem_pss_line("Rss: 10 kB\n") == -1, "no Pss");
    CHECK(webmem_pss_line("Pss: kB\n") == -1, "no number");
    CHECK(webmem_pss_line("") == -1, "empty");

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd)) printf("could not remove %s\n", root);
    printf("webmem-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
