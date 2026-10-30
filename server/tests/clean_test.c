/* clean_test.c — Cleaner's lord-server side and the real lord-clean helper (build/lord-clean, next to
 * this test), in throwaway folders under the system's temporary folder only, never a real home
 * folder: the rule table, the walk (links, `..`, mounts, limits, names, protected paths), what is
 * accepted from the helper, plans, execution against changed files, hard links and cancel. */
#include "clean/clean.h"
#include "clean/cleanfs.h"
#include "clean/cleanproto.h"
#include "clean/rules.h"
#include "fsops.h"
#include "sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char H[PATH_MAX];

static void path_of(char *out, size_t n, const char *rel) { if ((size_t)snprintf(out, n, "%s/%s", H, rel) >= n) abort(); }

static void mkparents(const char *rel) {
    char p[PATH_MAX * 2];
    path_of(p, sizeof p, rel);
    for (char *s = p + strlen(H) + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0700); *s = '/'; }
}

static void put(const char *rel, const char *text) {
    char p[PATH_MAX * 2];
    mkparents(rel);
    path_of(p, sizeof p, rel);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, text, strlen(text)) < 0) abort();
    close(fd);
}

static int exists(const char *rel) {
    char p[PATH_MAX * 2];
    struct stat st;
    path_of(p, sizeof p, rel);
    return !lstat(p, &st);
}

static void link_to(const char *target, const char *rel) {
    char p[PATH_MAX * 2];
    mkparents(rel);
    path_of(p, sizeof p, rel);
    if (symlink(target, p)) abort();
}

static void hardlink(const char *a, const char *b) {
    char p[PATH_MAX * 2], q[PATH_MAX * 2];
    mkparents(b);
    path_of(p, sizeof p, a);
    path_of(q, sizeof q, b);
    if (link(p, q)) abort();
}

static void wipe(void) {
    char cmd[PATH_MAX * 5];
    /* H always comes from mkdtemp under the temporary folder */
    if (strncmp(H, "/tmp/clean-test.", 16)) abort();
    snprintf(cmd, sizeof cmd, "chmod -R u+rwx '%s'/* '%s'/.[a-z]* 2>/dev/null; rm -rf '%s'/* '%s'/.[a-z]*", H, H, H, H);
    if (system(cmd) < 0) abort();
}

/* ---- the job, from the outside ---- */

typedef struct { JDoc d; const JVal *scan, *plan; const char *state, *error; } Poll;

static void poll_free(Poll *p) { json_free(&p->d); memset(p, 0, sizeof *p); }

static void poll_read(Poll *p) {
    char *j = clean_poll();
    memset(p, 0, sizeof *p);
    if (!j || json_parse(j, strlen(j), &p->d, NULL, 0)) abort();
    free(j);
    p->scan = json_get(p->d.root, "scan");
    p->plan = json_get(p->d.root, "plan");
    p->state = json_str(p->d.root, "state");
    p->error = json_str(p->d.root, "error");
}

/* until the job ends; the caller frees p */
static void wait_job(Poll *p) {
    for (int i = 0; i < 4000; i++) {
        poll_read(p);
        if (strcmp(p->state, "running")) return;
        poll_free(p);
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "FAIL: the job did not end\n");
    exit(1);
}

static double num(const JVal *o, const char *k) { return json_num(o, k, -1); }
static double skipped(const JVal *scan, const char *k) { return json_num(json_get(scan, "skipped"), k, -1); }
static double count(const JVal *plan, const char *k) { return json_num(json_get(plan, "counts"), k, -1); }

static int run_scan(Poll *p) {
    int id = clean_start();
    CHECK(id > 0);
    wait_job(p);
    return id;
}

/* every candidate id; returns how many */
static int all_ids(uint32_t *ids, int cap) {
    char *j = clean_results(0, 0, cap);
    JDoc d = { 0 };
    int n = 0;
    if (!j || json_parse(j, strlen(j), &d, NULL, 0)) abort();
    for (const JVal *v = json_get(d.root, "items")->kid; v && n < cap; v = v->next) ids[n++] = (uint32_t)json_num(v, "id", 0);
    json_free(&d);
    free(j);
    return n;
}

/* the status of the plan item whose display path ends in tail */
static void item_status(const char *tail, char *out, size_t n) {
    char *j = clean_results(1, 0, CLEAN_PAGE_MAX);
    JDoc d = { 0 };
    out[0] = 0;
    if (!j || json_parse(j, strlen(j), &d, NULL, 0)) abort();
    for (const JVal *v = json_get(d.root, "items")->kid; v; v = v->next) {
        const char *p = json_str(v, "path");
        size_t a = strlen(p), b = strlen(tail);
        if (a >= b && !strcmp(p + a - b, tail)) snprintf(out, n, "%s", json_str(v, "status"));
    }
    json_free(&d);
    free(j);
}

static int has_candidate(const char *tail) {
    char *j = clean_results(0, 0, CLEAN_PAGE_MAX);
    int r = j && strstr(j, tail) != NULL;
    free(j);
    return r;
}

/* builds the plan of every candidate and waits for it */
static int plan_all(int scan, Poll *p) {
    uint32_t ids[CLEAN_PAGE_MAX];
    int n = all_ids(ids, CLEAN_PAGE_MAX);
    int plan = clean_plan(scan, ids, (size_t)n);
    CHECK(plan > 0);
    wait_job(p);
    return plan;
}

#define T ".cache/thumbnails"

/* ---- rule table ---- */

static CleanRule good(void) {
    return (CleanRule){ .id = "good", .version = 1, .category = "cache", .root = ".cache/good", .pattern = "*.png",
                        .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_TRASH, .text_key = "cleaner.rule.good" };
}

static void test_rules(void) {
    int n;
    char why[128];
    const CleanRule *t = clean_rules(&n);
    CHECK(n == 1);
    CHECK(!clean_rules_check(t, n, why, sizeof why));
    CHECK(!strcmp(t[0].id, "thumbnails") && !strcmp(t[0].root, T) && t[0].risk == CLEAN_RISK_REGENERABLE && t[0].action == CLEAN_ACT_TRASH);
    CleanRule r = good();
    CHECK(!clean_rule_check(&r, why, sizeof why));
#define BAD(field, value) do { r = good(); r.field = value; CHECK(clean_rule_check(&r, why, sizeof why)); } while (0)
    BAD(id, ""); BAD(id, "Bad_Id"); BAD(id, NULL); BAD(version, 0); BAD(category, "a b");
    BAD(root, ""); BAD(root, "/etc"); BAD(root, "../x"); BAD(root, ".cache/../x"); BAD(root, ".cache//x"); BAD(root, ".cache/x/");
    BAD(root, ".cache");                            /* the whole cache folder is not a rule */
    BAD(root, ".config"); BAD(root, "Documents"); BAD(root, ".local/share");
    BAD(root, ".cache/lordos");                     /* holds a protected path */
    BAD(root, ".config/lordos");
    BAD(root, ".cache/lordos/fwlists");             /* is one */
    BAD(root, ".local/share/lordos/vault/sub");     /* is inside one */
    BAD(pattern, ""); BAD(pattern, "a*b*"); BAD(pattern, "../x"); BAD(pattern, "a/b"); BAD(pattern, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    BAD(min_age_days, -1); BAD(text_key, "cleaner.rule.other"); BAD(text_key, NULL);
    BAD(risk, (CleanRisk)9); BAD(action, (CleanAction)9);
    r = good(); r.risk = CLEAN_RISK_PROTECTED;
    CHECK(clean_rule_check(&r, why, sizeof why));   /* a protected rule offers nothing */
    r.action = CLEAN_ACT_NONE;
    CHECK(!clean_rule_check(&r, why, sizeof why));
    static const char *const badex[] = { "a/b", NULL };
    r = good(); r.exclude = badex;
    CHECK(clean_rule_check(&r, why, sizeof why));
    CleanRule two[2] = { good(), good() };
    CHECK(clean_rules_check(two, 2, why, sizeof why));    /* the same id twice */
    two[1].id = "other"; two[1].text_key = "cleaner.rule.other";
    CHECK(clean_rules_check(two, 2, why, sizeof why));    /* the same root twice */
    two[1].root = ".cache/other";
    CHECK(!clean_rules_check(two, 2, why, sizeof why));

    CHECK(clean_glob("*.png", "a.png") && clean_glob("*.png", ".png") && !clean_glob("*.png", "a.pngx") && !clean_glob("*.png", "png"));
    CHECK(clean_glob("a*", "abc") && clean_glob("abc", "abc") && !clean_glob("abc", "abcd") && !clean_glob("ab*bc", "abc"));
    CHECK(clean_name_ok("a b.png") && clean_name_ok("ñandú.png") && clean_name_ok("日本.png"));
    CHECK(!clean_name_ok("") && !clean_name_ok(".") && !clean_name_ok("..") && !clean_name_ok("a/b") && !clean_name_ok("a\nb") && !clean_name_ok("\x7f"));
    CHECK(!clean_name_ok("\xff") && !clean_name_ok("\xc0\xaf") && !clean_name_ok("\xed\xa0\x80") && !clean_name_ok("\xe2\x82") && !clean_name_ok("\xc2\x85"));
    CHECK(clean_relpath_ok("a/b/c.png", 4) && !clean_relpath_ok("a/b/c.png", 1) && !clean_relpath_ok("/a", 4) && !clean_relpath_ok("a/../b", 4) && !clean_relpath_ok("a//b", 4));
}

/* ---- the walk ---- */

static void test_scan(void) {
    wipe();
    put(T "/normal/a.png", "aaaa");
    put(T "/normal/b.png", "bbbbbbbb");
    put(T "/large/c.png", "c");
    put(T "/normal/note.txt", "not a thumbnail");
    put("Documents/secret.png", "secret");
    Poll p;
    int scan = run_scan(&p);
    CHECK(!strcmp(p.state, "done"));
    CHECK(json_bool(p.scan, "complete", 0) == 1);
    CHECK(num(p.scan, "id") == scan);
    CHECK(num(p.scan, "candidates") == 3);
    CHECK(num(p.scan, "logical") == 13);
    CHECK(num(p.scan, "seen") == 6);                /* three thumbnails, one note, two folders */
    CHECK(num(p.scan, "dirs") == 3);
    CHECK(skipped(p.scan, "nomatch") == 1);
    CHECK(has_candidate("~/" T "/normal/a.png"));
    CHECK(!has_candidate("note.txt") && !has_candidate("secret"));
    poll_free(&p);
    /* a scan changes nothing */
    CHECK(exists(T "/normal/a.png") && exists(T "/normal/b.png") && exists(T "/large/c.png") && exists(T "/normal/note.txt"));
    CHECK(!exists(".local/share/Trash"));
}

static void test_absent(void) {
    wipe();
    Poll p;
    run_scan(&p);
    CHECK(!strcmp(p.state, "done") && json_bool(p.scan, "complete", 0) == 1 && num(p.scan, "candidates") == 0);
    poll_free(&p);
}

static void test_symlinks(void) {
    wipe();
    put("Documents/secret.png", "secret");
    put("Documents/inner/deep.png", "deep");
    put(T "/normal/real.png", "real");
    link_to("../../../Documents/secret.png", T "/normal/evil.png");       /* a link named like a thumbnail */
    link_to("../../Documents/inner", T "/linked");                        /* a link to a folder */
    link_to("/etc", T "/etc");
    Poll p;
    int scan = run_scan(&p);
    CHECK(!strcmp(p.state, "done"));
    CHECK(num(p.scan, "candidates") == 1);
    CHECK(skipped(p.scan, "symlinks") == 3);
    CHECK(!has_candidate("evil") && !has_candidate("deep") && !has_candidate("secret"));
    poll_free(&p);
    plan_all(scan, &p);
    poll_free(&p);

    /* the root itself is a link: nothing is read through it */
    wipe();
    put("Documents/x.png", "x");
    mkparents(T);
    link_to("../Documents", T);
    run_scan(&p);
    CHECK(!strcmp(p.state, "done"));
    CHECK(num(p.scan, "candidates") == 0 && json_bool(p.scan, "complete", 1) == 0);
    CHECK(!strcmp(json_str(json_get(p.scan, "rules")->kid, "root"), "changed"));
    poll_free(&p);
}

/* what lord-server takes from the helper: it trusts nothing */
static void test_records(void) {
    wipe();
    struct stat st;
    if (stat(H, &st)) abort();
    char rec[2048];
    unsigned M = (unsigned)(st.st_dev >> 8) & 0xfff, m = (unsigned)(st.st_dev & 0xff) | ((unsigned)(st.st_dev >> 12) & 0xfff00);
#define REC(path, ino, size, dev) (snprintf(rec, sizeof rec, "{\"r\":0,\"p\":\"%s\",\"M\":%u,\"m\":%u,\"i\":\"%s\",\"l\":1,\"s\":%s,\"a\":4096,\"t\":1700000000,\"n\":5,\"u\":%u}", \
                                            path, (dev), m, ino, size, (unsigned)getuid()), rec)
    CHECK(clean_test_record(REC("normal/a.png", "12", "4", M), 1) == 0);
    CHECK(clean_test_record(REC("../../Documents/a.png", "12", "4", M), 1) == -1);
    CHECK(clean_test_record(REC("normal/../../a.png", "12", "4", M), 1) == -1);
    CHECK(clean_test_record(REC("/etc/a.png", "12", "4", M), 1) == -1);
    CHECK(clean_test_record(REC("normal//a.png", "12", "4", M), 1) == -1);
    CHECK(clean_test_record(REC("a\\nb.png", "12", "4", M), 1) == -1);
    CHECK(clean_test_record(REC("", "12", "4", M), 1) == -1);
    CHECK(clean_test_record(REC("normal/a.png", "12x", "4", M), 1) == -1);
    CHECK(clean_test_record(REC("normal/a.png", "99999999999999999999999", "4", M), 1) == -1);
    CHECK(clean_test_record(REC("normal/a.png", "12", "-4", M), 1) == -1);
    CHECK(clean_test_record(REC("normal/a.png", "12", "1.5", M), 1) == -1);
    CHECK(clean_test_record(REC("normal/a.png", "12", "1e300", M), 1) == -1);
    CHECK(clean_test_record(REC("normal/a.png", "12", "4", M + 1), 1) == -1);    /* another device */
    CHECK(clean_test_record("{\"r\":0}", 1) == -1);
    CHECK(clean_test_record("[1]", 1) == -1);
    snprintf(rec, sizeof rec, "{\"r\":7,\"p\":\"a.png\",\"M\":%u,\"m\":%u,\"i\":\"1\",\"l\":1,\"s\":1,\"a\":1,\"t\":1,\"n\":0,\"u\":%u}", M, m, (unsigned)getuid());
    CHECK(clean_test_record(rec, 1) == -1);         /* no such rule */
    CHECK(clean_test_record(REC("normal/note.txt", "12", "4", M), 1) == 1);      /* not what the rule covers */
    snprintf(rec, sizeof rec, "{\"r\":0,\"p\":\"a.png\",\"M\":%u,\"m\":%u,\"i\":\"1\",\"l\":1,\"s\":1,\"a\":1,\"t\":1,\"n\":0,\"u\":%u}", M, m, (unsigned)getuid() + 1);
    CHECK(clean_test_record(rec, 1) == 1);          /* another user's file */
}

static void test_names(void) {
    wipe();
    put(T "/normal/ok.png", "ok");
    put(T "/normal/bad\xff.png", "bad");
    put(T "/normal/new\nline.png", "bad");
    Poll p;
    run_scan(&p);
    CHECK(!strcmp(p.state, "done"));
    CHECK(num(p.scan, "candidates") == 1 && skipped(p.scan, "names") == 2);
    CHECK(json_bool(p.scan, "complete", 1) == 0);   /* shown as uncertain, not offered */
    poll_free(&p);
}

static void test_limits(void) {
    wipe();
    char name[64];
    for (int i = 0; i < 10; i++) { snprintf(name, sizeof name, T "/n/%d.png", i); put(name, "x"); }
    clean_test_max_files = 4;
    Poll p;
    run_scan(&p);
    clean_test_max_files = CLEAN_MAX_FILES;
    CHECK(!strcmp(p.state, "done") && !strcmp(json_str(p.scan, "limit"), "files"));
    CHECK(json_bool(p.scan, "complete", 1) == 0 && num(p.scan, "seen") == 4);
    CHECK(num(p.scan, "candidates") <= 4);
    poll_free(&p);

    wipe();
    char rel[1024] = T;
    for (int i = 0; i < CLEAN_MAX_DEPTH + 6; i++) strncat(rel, "/d", sizeof rel - strlen(rel) - 1);
    char file[1100];
    snprintf(file, sizeof file, "%s/deep.png", rel);
    put(file, "deep");
    put(T "/top.png", "top");
    run_scan(&p);
    CHECK(!strcmp(p.state, "done") && !strcmp(json_str(p.scan, "limit"), "depth"));
    CHECK(json_bool(p.scan, "complete", 1) == 0 && num(p.scan, "candidates") == 1);
    CHECK(!has_candidate("deep.png"));
    poll_free(&p);
}

/* ---- protected paths ---- */

static void test_protected(void) {
    /* a table no real build can have: a root that holds a protected path, to prove the walk refuses it */
    static CleanRule wide;
    wide = good();
    wide.root = ".cache/lordos";
    wide.pattern = "*";
    wipe();
    put(".cache/lordos/fwlists/list.png", "blocklist");
    put(".cache/lordos/fwlists/sub/deep.png", "blocklist");
    put(".cache/lordos/plain.png", "fine");
    clean_rules_override(&wide, 1);
    Poll p;
    run_scan(&p);
    CHECK(!strcmp(p.state, "done"));
    CHECK(num(p.scan, "candidates") == 1 && skipped(p.scan, "protected") == 1);
    CHECK(!has_candidate("list.png") && !has_candidate("deep.png"));
    poll_free(&p);

    /* the same record forced past the helper: lord-server drops it by itself */
    struct stat st;
    if (stat(H, &st)) abort();
    char rec[512];
    unsigned M = (unsigned)(st.st_dev >> 8) & 0xfff, m = (unsigned)(st.st_dev & 0xff) | ((unsigned)(st.st_dev >> 12) & 0xfff00);
    snprintf(rec, sizeof rec, "{\"r\":0,\"p\":\"fwlists/list.png\",\"M\":%u,\"m\":%u,\"i\":\"1\",\"l\":1,\"s\":1,\"a\":1,\"t\":1,\"n\":0,\"u\":%u}", M, m, (unsigned)getuid());
    CHECK(clean_test_record(rec, 1) == 1);
    clean_rules_override(NULL, 0);

    /* a candidate that became protected: the plan and the execution both refuse it */
    wipe();
    put(T "/a.png", "a");
    put(".cache/lordos/fwlists/a.png", "must stay");
    Poll q;
    int scan = run_scan(&q);
    poll_free(&q);
    static CleanRule moved;
    moved = good();
    moved.root = ".cache/lordos/fwlists";
    clean_rules_override(&moved, 1);
    plan_all(scan, &q);
    CHECK(count(q.plan, "denied") == 1 && count(q.plan, "ready") == 0);
    int plan = (int)num(q.plan, "id");
    poll_free(&q);
    CHECK(clean_execute(plan) == plan);
    wait_job(&q);
    CHECK(count(q.plan, "denied") == 1 && count(q.plan, "done") == 0);
    poll_free(&q);
    clean_rules_override(NULL, 0);
    CHECK(exists(".cache/lordos/fwlists/a.png") && exists(T "/a.png"));

    /* a rule that stopped offering the action, and one the file is too new for */
    static CleanRule noact, young;
    noact = good(); noact.root = T; noact.action = CLEAN_ACT_NONE;
    young = good(); young.root = T; young.min_age_days = 3000;
    wipe();
    put(T "/a.png", "a");
    scan = run_scan(&q);
    poll_free(&q);
    clean_rules_override(&noact, 1);
    plan_all(scan, &q);
    CHECK(count(q.plan, "unsupported") == 1);
    poll_free(&q);
    clean_rules_override(&young, 1);
    plan_all(scan, &q);
    CHECK(count(q.plan, "skipped") == 1);
    poll_free(&q);
    clean_rules_override(NULL, 0);
    CHECK(exists(T "/a.png"));
}

/* ---- plans and execution ---- */

static void test_execute(void) {
    wipe();
    put(T "/n/keep.png", "keep");
    put(T "/n/grown.png", "grown");
    put(T "/n/touched.png", "touched");
    put(T "/n/replaced.png", "repl");
    put(T "/n/gone.png", "gone");
    put(T "/m/deep.png", "deep");
    put("Documents/deep.png", "personal");
    Poll p;
    int scan = run_scan(&p);
    CHECK(num(p.scan, "candidates") == 6);
    poll_free(&p);
    int plan = plan_all(scan, &p);
    CHECK(!strcmp(json_str(p.plan, "state"), "ready"));
    CHECK(count(p.plan, "ready") == 6 && num(p.plan, "logical") == 28);
    CHECK(num(p.plan, "expiresInS") > 0);
    const JVal *w = json_get(p.plan, "warnings");
    CHECK(w && w->len >= 1 && w->kid->str && !strcmp(w->kid->str, "trashKeepsSpace"));
    poll_free(&p);

    /* between the plan and the execution */
    put(T "/n/grown.png", "grown a lot");
    char f[PATH_MAX * 2];
    path_of(f, sizeof f, T "/n/touched.png");
    struct timespec tm[2] = { { 1000000000, 0 }, { 1000000000, 0 } };
    CHECK(!utimensat(AT_FDCWD, f, tm, 0));
    path_of(f, sizeof f, T "/n/replaced.png");
    CHECK(!unlink(f));
    put(T "/n/replaced.png", "repl");               /* same name and size, another inode */
    path_of(f, sizeof f, T "/n/gone.png");
    CHECK(!unlink(f));
    /* a folder swapped for a link to personal files with the same name inside */
    char a[PATH_MAX * 2], b[PATH_MAX * 2];
    path_of(a, sizeof a, T "/m");
    path_of(b, sizeof b, T "/m.old");
    CHECK(!rename(a, b));
    link_to("../../Documents", T "/m");

    CHECK(clean_execute(plan) == plan);
    wait_job(&p);
    CHECK(!strcmp(p.state, "done"));
    CHECK(!strcmp(json_str(p.plan, "state"), "finished"));
    CHECK(count(p.plan, "done") == 1 && count(p.plan, "changed") == 5 && count(p.plan, "ready") == 0);
    CHECK(num(p.plan, "processed") == 6);
    CHECK(num(p.plan, "freeBefore") > 0 && num(p.plan, "freeAfter") > 0);
    poll_free(&p);
    char st[16];
    item_status("/n/keep.png", st, sizeof st);
    CHECK(!strcmp(st, "done"));
    item_status("/n/grown.png", st, sizeof st);
    CHECK(!strcmp(st, "changed"));
    item_status("/m/deep.png", st, sizeof st);
    CHECK(!strcmp(st, "changed"));
    CHECK(!exists(T "/n/keep.png"));
    CHECK(exists(".local/share/Trash/files/keep.png") && exists(".local/share/Trash/info/keep.png.trashinfo"));
    CHECK(exists(T "/n/grown.png") && exists(T "/n/touched.png") && exists(T "/n/replaced.png"));
    CHECK(exists("Documents/deep.png") && exists(T "/m.old/deep.png"));
    CHECK(!exists(".local/share/Trash/files/deep.png"));

    /* the Trash holds it: it can be restored, nothing was deleted */
    char id[PATH_MAX * 2], err[256];
    path_of(id, sizeof id, ".local/share/Trash/files/keep.png");
    CHECK(!fstrash_restore(id, err, sizeof err));
    CHECK(exists(T "/n/keep.png"));

    /* a plan runs once */
    CHECK(clean_execute(plan) == CLEAN_E_NOPLAN);
}

static void test_changed_before_plan(void) {
    wipe();
    put(T "/a.png", "a");
    put(T "/b.png", "b");
    Poll p;
    int scan = run_scan(&p);
    poll_free(&p);
    put(T "/a.png", "longer");
    plan_all(scan, &p);
    CHECK(count(p.plan, "ready") == 1 && count(p.plan, "changed") == 1 && num(p.plan, "logical") == 1);
    poll_free(&p);
}

/* the name is pointed at another file in the instant between the check and the move */
static char swap_rel[128];
static void swap_file(const char *real) {
    (void)real;
    char p[PATH_MAX * 2], q[PATH_MAX * 2];
    path_of(p, sizeof p, swap_rel);
    path_of(q, sizeof q, "Documents/swapped.png");
    if (unlink(p) || rename(q, p)) abort();
}
static void test_last_moment(void) {
    wipe();
    put(T "/a.png", "a");
    put("Documents/swapped.png", "b");
    Poll p;
    int scan = run_scan(&p);
    poll_free(&p);
    int plan = plan_all(scan, &p);
    poll_free(&p);
    snprintf(swap_rel, sizeof swap_rel, T "/a.png");
    clean_test_before_trash = swap_file;
    CHECK(clean_execute(plan) == plan);
    wait_job(&p);
    clean_test_before_trash = NULL;
    CHECK(count(p.plan, "changed") == 1 && count(p.plan, "done") == 0);
    poll_free(&p);
    CHECK(exists(T "/a.png"));                      /* the other file is back where it was */
    CHECK(!exists(".local/share/Trash/files/a.png") && !exists(".local/share/Trash/info/a.png.trashinfo"));
}

static void cancel_now(const char *real) { (void)real; clean_cancel(0); }
static void test_cancel_execute(void) {
    wipe();
    put(T "/a.png", "a");
    put(T "/b.png", "b");
    put(T "/c.png", "c");
    Poll p;
    int scan = run_scan(&p);
    poll_free(&p);
    int plan = plan_all(scan, &p);
    poll_free(&p);
    clean_test_before_trash = cancel_now;
    CHECK(clean_execute(plan) == plan);
    wait_job(&p);
    clean_test_before_trash = NULL;
    CHECK(!strcmp(p.state, "cancelled"));
    CHECK(count(p.plan, "done") == 1 && count(p.plan, "cancelled") == 2);   /* the one in flight finished */
    poll_free(&p);
}

static void test_hardlinks(void) {
    wipe();
    put(T "/a.png", "0123456789");
    hardlink(T "/a.png", T "/b.png");               /* both links inside the root */
    put(T "/c.png", "0123456789");
    hardlink(T "/c.png", "Documents/kept.png");     /* one link outside */
    Poll p;
    int scan = run_scan(&p);
    CHECK(num(p.scan, "candidates") == 3);
    CHECK(num(p.scan, "logical") == 20);            /* a and b count once */
    double alloc = num(p.scan, "allocated"), est = num(p.scan, "estimated");
    CHECK(alloc >= 0 && est == alloc / 2);          /* c's blocks stay with Documents/kept.png */
    poll_free(&p);
    plan_all(scan, &p);
    CHECK(num(p.plan, "logical") == 20);
    CHECK(num(p.plan, "estimated") == est);
    int warned = 0;
    for (const JVal *w = json_get(p.plan, "warnings")->kid; w; w = w->next) warned |= !strcmp(w->str, "linksElsewhere");
    CHECK(warned);
    poll_free(&p);
}

static void test_plan_input(void) {
    wipe();
    put(T "/a.png", "a");
    put(T "/b.png", "b");
    Poll p;
    int scan = run_scan(&p);
    poll_free(&p);
    uint32_t bad[] = { 1, 99 }, zero[] = { 0 }, dup[] = { 1, 1, 1 }, one[] = { 1 };
    CHECK(clean_plan(scan, bad, 2) == CLEAN_E_BADID);
    CHECK(clean_plan(scan, zero, 1) == CLEAN_E_BADID);
    CHECK(clean_plan(scan, one, 0) == CLEAN_E_BADID);
    CHECK(clean_plan(scan + 100, one, 1) == CLEAN_E_NOSCAN);
    CHECK(clean_execute(12345) == CLEAN_E_NOPLAN);
    CHECK(clean_plan(scan, dup, 3) > 0);
    wait_job(&p);
    CHECK(num(p.plan, "items") == 1);
    poll_free(&p);

    clean_test_plan_ttl = 0;
    int plan = clean_plan(scan, one, 1);
    wait_job(&p);
    poll_free(&p);
    clean_test_plan_ttl = CLEAN_PLAN_TTL_S;
    struct timespec ts = { 0, 20 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    CHECK(clean_execute(plan) == CLEAN_E_EXPIRED);  /* an expired plan does nothing */
    CHECK(clean_execute(plan) == CLEAN_E_NOPLAN);
    CHECK(exists(T "/a.png"));
}

static void test_cancel_scan(void) {
    wipe();
    put(T "/a.png", "a");
    clean_test_stop_helper = 1;                     /* a helper that can't finish before the cancel */
    int id = clean_start();
    CHECK(id > 0);
    int pid = 0;
    for (int i = 0; i < 2000 && pid <= 0; i++) {
        pid = clean_test_helper_pid();
        struct timespec ts = { 0, 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    clean_test_stop_helper = 0;
    CHECK(pid > 0);
    CHECK(clean_start() == CLEAN_E_BUSY);
    clean_cancel(0);
    Poll p;
    wait_job(&p);
    CHECK(!strcmp(p.state, "cancelled"));
    CHECK(num(p.scan, "candidates") == 0 && json_bool(p.scan, "complete", 1) == 0);
    poll_free(&p);
    CHECK(clean_test_helper_pid() == -1);
    CHECK(kill(pid, 0) == -1 && errno == ESRCH);    /* the helper is gone, and reaped */
    CHECK(exists(T "/a.png"));

    /* cancel with forget drops the results */
    run_scan(&p);
    poll_free(&p);
    clean_cancel(1);
    poll_read(&p);
    CHECK(p.scan && p.scan->type == J_NULL && !strcmp(p.state, "idle"));
    poll_free(&p);
}

int main(void) {
    if (syscall(SYS_landlock_create_ruleset, NULL, 0U, 1U) < 1) { printf("clean-test: skipped: needs Landlock\n"); return 0; }
    char tmpl[] = "/tmp/clean-test.XXXXXX";
    if (!mkdtemp(tmpl) || !realpath(tmpl, H)) return 1;
    fsops_init(H, "clean-test");
    CHECK(clean_available());
    test_rules();
    test_scan();
    test_absent();
    test_symlinks();
    test_records();
    test_names();
    test_limits();
    test_protected();
    test_execute();
    test_changed_before_plan();
    test_last_moment();
    test_cancel_execute();
    test_hardlinks();
    test_plan_input();
    test_cancel_scan();
    wipe();
    rmdir(H);
    printf("clean-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
