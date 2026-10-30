/* clean_test.c — Cleaner's lord-server side and the real lord-clean helper (build/lord-clean, next to
 * this test), in throwaway folders under the system's temporary folder only, never a real home
 * folder: the rule table, the walk (links, `..`, mounts, limits, names, protected paths), what is
 * accepted from the helper, plans, execution against changed files, hard links and cancel. */
#include "clean/clean.h"
#include "clean/clean_admin.h"
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
#include <sched.h>
#include <sys/mount.h>
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
    char *j = clean_poll(-1);
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

static CleanRule base;                              /* one rule to the Trash: most tests below use it */

/* ---- rule table ---- */

static CleanRule good(void) {
    return (CleanRule){ .id = "good", .version = 1, .category = "cache", .root = ".cache/good", .pattern = "*.png",
                        .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_TRASH, .text_key = "cleaner.rule.good" };
}

static void test_rules(void) {
    int n;
    char why[128];
    const CleanRule *t = clean_rules(&n);
    CHECK(n >= 30);
    CHECK(!clean_rules_check(t, n, why, sizeof why));
    CHECK(!strcmp(t[0].id, "thumbnails") && !strcmp(t[0].root, T) && t[0].risk == CLEAN_RISK_REGENERABLE && t[0].action == CLEAN_ACT_DELETE);
    for (int i = 0; i < n; i++) {                   /* a real delete only for regenerable rules, the Trash folder never cleaned item by item */
        CHECK(t[i].action != CLEAN_ACT_DELETE || t[i].risk == CLEAN_RISK_REGENERABLE);
        CHECK(strcmp(t[i].root, ".cache"));
        /* browsers: cache folders only, never what holds a person's data */
        for (const char *const *w = (const char *const[]){ "Cookies", "Login", "History", "sessionstore", "places", "Local Storage", NULL }; *w; w++) CHECK(!strstr(t[i].root, *w));
        if (!strcmp(t[i].category, "browser")) CHECK(strstr(t[i].root, "ache") && t[i].lock);
        if (!strcmp(t[i].category, "ai")) CHECK(t[i].explicit_only || t[i].action == CLEAN_ACT_NONE);
        if (!strcmp(t[i].category, "info")) CHECK(t[i].action == CLEAN_ACT_NONE);
    }
    {
        char star[64];
        CHECK(clean_root_fits(".cache/x/*/cache2", ".cache/x/a.default/cache2", star, sizeof star) && !strcmp(star, "a.default"));
        CHECK(!clean_root_fits(".cache/x/*/cache2", ".cache/x/a/b/cache2", star, sizeof star));
        CHECK(!clean_root_fits(".cache/x/*/cache2", ".cache/y/a/cache2", star, sizeof star));
    }
    CleanRule del = good();
    del.risk = CLEAN_RISK_REVIEW; del.action = CLEAN_ACT_DELETE;
    CHECK(clean_rule_check(&del, why, sizeof why));
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
    clean_rules_override(&base, 1);

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
    clean_rules_override(&base, 1);
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
    clean_rules_override(&base, 1);
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

/* ---- the real delete: real table, throwaway folder ---- */

/* a fingerprint of everything outside the rule's root */
static void outside_sum(char *out, size_t n) {
    char cmd[PATH_MAX * 3];
    snprintf(cmd, sizeof cmd, "cd '%s' && find . -path ./.cache/thumbnails -prune -o -printf '%%p %%y %%s %%T@ %%l\\n' | LC_ALL=C sort | sha256sum", H);
    FILE *f = popen(cmd, "r");
    if (!f || !fgets(out, (int)n, f)) abort();
    pclose(f);
}

static char deepdir[64];
static void swap_folder(const char *real) {         /* the folder becomes a link at the last moment */
    (void)real;
    char a[PATH_MAX * 2], b[PATH_MAX * 2 + 8];
    path_of(a, sizeof a, deepdir);
    snprintf(b, sizeof b, "%s.old", a);
    if (rename(a, b) || symlink("../../Documents", a)) abort();
    clean_test_before_trash = NULL;
}

static void test_delete(void) {
    clean_rules_override(NULL, 0);
    wipe();
    put(T "/n/a.png", "aaaa");
    put(T "/n/b.png", "bbbb");
    put(T "/n/grown.png", "grown");
    put(T "/m/c.png", "cccc");
    put(T "/l/d.png", "dddd");
    put("Documents/c.png", "personal");
    put("Documents/d.png", "personal");
    put("Documents/swapped.png", "other");
    char before[128], after[128];
    outside_sum(before, sizeof before);
    Poll p;
    int scan = run_scan(&p);
    CHECK(num(p.scan, "candidates") == 5);
    poll_free(&p);
    const char *const names[] = { "thumbnails" };
    int plan = clean_plan_rules(scan, names, 1);
    CHECK(plan > 0);
    wait_job(&p);
    CHECK(num(p.plan, "deleteItems") == 5 && num(p.plan, "trashItems") == 0);
    int final = 0;
    for (const JVal *w = json_get(p.plan, "warnings")->kid; w; w = w->next) final |= !strcmp(w->str, "deleteIsFinal");
    CHECK(final);
    poll_free(&p);
    /* after the plan: a changed file, and a folder swapped for a link to personal files of the same name */
    put(T "/n/grown.png", "grown a lot");
    char a[PATH_MAX * 2], b[PATH_MAX * 2];
    path_of(a, sizeof a, T "/m");
    path_of(b, sizeof b, T "/m.old");
    CHECK(!rename(a, b));
    link_to("../../Documents", T "/m");
    /* and one swapped in the instant after its item was checked */
    snprintf(deepdir, sizeof deepdir, T "/l");
    clean_test_before_trash = NULL;
    CHECK(clean_execute(plan) == plan);
    wait_job(&p);
    CHECK(!strcmp(p.state, "done"));
    CHECK(count(p.plan, "changed") == 2 && count(p.plan, "done") == 3);
    poll_free(&p);
    CHECK(!exists(T "/n/a.png") && !exists(T "/n/b.png") && !exists(T "/l/d.png"));
    CHECK(exists(T "/n/grown.png") && exists(T "/m.old/c.png"));
    CHECK(!exists(".local/share/Trash"));           /* deleted, not moved */
    outside_sum(after, sizeof after);
    CHECK(!strcmp(before, after));                  /* nothing outside the root moved */

    /* the folder swapped after its item was checked: only the original folder's file can go */
    wipe();
    put(T "/l/d.png", "dddd");
    put("Documents/d.png", "personal");
    outside_sum(before, sizeof before);
    scan = run_scan(&p);
    poll_free(&p);
    plan = clean_plan_rules(scan, names, 1);
    wait_job(&p);
    poll_free(&p);
    clean_test_before_trash = swap_folder;
    CHECK(clean_execute(plan) == plan);
    wait_job(&p);
    poll_free(&p);
    outside_sum(after, sizeof after);
    CHECK(!strcmp(before, after) && exists("Documents/d.png"));
    CHECK(!exists(T "/l.old/d.png"));

    /* the name pointed at another file at the last moment: that file survives, under its name */
    wipe();
    put(T "/a.png", "a");
    put("Documents/swapped.png", "b");
    scan = run_scan(&p);
    poll_free(&p);
    plan = clean_plan_rules(scan, names, 1);
    wait_job(&p);
    poll_free(&p);
    snprintf(swap_rel, sizeof swap_rel, T "/a.png");
    clean_test_before_trash = swap_file;
    CHECK(clean_execute(plan) == plan);
    wait_job(&p);
    clean_test_before_trash = NULL;
    CHECK(count(p.plan, "changed") == 1 && count(p.plan, "done") == 0);
    poll_free(&p);
    CHECK(exists(T "/a.png"));
    char list[PATH_MAX * 3];
    snprintf(list, sizeof list, "ls -A '%s/" T "' | grep -c lordos-clean", H);
    FILE *f = popen(list, "r");
    int leftovers = -1;
    if (f) { if (fscanf(f, "%d", &leftovers) != 1) leftovers = -1; pclose(f); }
    CHECK(leftovers == 0);                          /* no private name left behind */

    /* a rule's name can't be asked for if it isn't in the table */
    const char *const nope[] = { "../etc" };
    CHECK(clean_plan_rules(scan, nope, 1) == CLEAN_E_BADID);
    /* the Trash rule shows its size and offers no action item by item */
    wipe();
    put(".local/share/Trash/files/old.bin", "old");
    scan = run_scan(&p);
    CHECK(num(p.scan, "candidates") == 1);
    poll_free(&p);
    const char *const tr[] = { "trash" };
    plan = clean_plan_rules(scan, tr, 1);
    wait_job(&p);
    CHECK(count(p.plan, "unsupported") == 1);
    poll_free(&p);
    /* a review rule never gets the real delete, even from a table that asks for it */
    static CleanRule forced[CLEAN_MAX_RULES];
    int nreal;
    const CleanRule *real = clean_rules(&nreal);
    memcpy(forced, real, (size_t)nreal * sizeof *forced);
    for (int i = 0; i < nreal; i++) if (!strcmp(forced[i].id, "trash")) forced[i].action = CLEAN_ACT_DELETE;
    clean_rules_override(forced, nreal);
    plan = plan_all(scan, &p);
    CHECK(count(p.plan, "unsupported") == 1);
    poll_free(&p);
    clean_rules_override(NULL, 0);
    CHECK(exists(".local/share/Trash/files/old.bin"));
    /* the temporary rule's age and the real emptyTrash */
    CHECK(clean_empty_trash() == 0);
    wait_job(&p);
    CHECK(num(json_get(p.d.root, "emptied"), "items") == 1);
    poll_free(&p);
    CHECK(!exists(".local/share/Trash/files/old.bin"));
    wipe();
    put(".cache/tmp/new.bin", "new");
    put(".cache/tmp/old.bin", "old");
    char o[PATH_MAX * 2];
    path_of(o, sizeof o, ".cache/tmp/old.bin");
    struct timespec tm[2] = { { 1000000000, 0 }, { 1000000000, 0 } };
    CHECK(!utimensat(AT_FDCWD, o, tm, 0));
    run_scan(&p);
    CHECK(num(p.scan, "candidates") == 1 && has_candidate("old.bin") && !has_candidate("new.bin"));
    poll_free(&p);
    clean_cancel(1);
    clean_rules_override(&base, 1);
}

/* a mount inside the root, in a user and mount namespace of a child: returns fails, or -1 if the kernel refuses */
static int mount_child(void) {
    clean_rules_override(NULL, 0);
    uid_t uid = getuid();
    gid_t gid = getgid();
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS)) return -1;
    char m[64];
    int fd = open("/proc/self/setgroups", O_WRONLY);
    if (fd >= 0) { if (write(fd, "deny", 4) < 0) return -1; close(fd); }
    fd = open("/proc/self/uid_map", O_WRONLY);
    int n = snprintf(m, sizeof m, "%u %u 1", (unsigned)uid, (unsigned)uid);
    if (fd < 0 || write(fd, m, (size_t)n) < 0) return -1;
    close(fd);
    fd = open("/proc/self/gid_map", O_WRONLY);
    n = snprintf(m, sizeof m, "%u %u 1", (unsigned)gid, (unsigned)gid);
    if (fd < 0 || write(fd, m, (size_t)n) < 0) return -1;
    close(fd);
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)) return -1;
    wipe();
    put(T "/plain.png", "p");
    put(T "/over/under.png", "under");
    put(T "/mnt/.keep", "");
    char a[PATH_MAX * 2];
    path_of(a, sizeof a, T "/mnt");
    if (mount("tmpfs", a, "tmpfs", 0, NULL)) return -1;
    put(T "/mnt/inside.png", "inside");
    Poll p;
    int scan = run_scan(&p);
    CHECK(num(p.scan, "candidates") == 2 && skipped(p.scan, "mounts") == 1);
    CHECK(!has_candidate("inside.png"));
    poll_free(&p);
    /* a mount placed over a folder after the scan: its same-named file isn't touched, nor what it hides */
    const char *const names[] = { "thumbnails" };
    int plan = clean_plan_rules(scan, names, 1);
    wait_job(&p);
    poll_free(&p);
    path_of(a, sizeof a, T "/over");
    if (mount("tmpfs", a, "tmpfs", 0, NULL)) return -1;
    put(T "/over/under.png", "under");
    CHECK(clean_execute(plan) == plan);
    wait_job(&p);
    CHECK(count(p.plan, "changed") == 1 && count(p.plan, "done") == 1);
    poll_free(&p);
    CHECK(exists(T "/over/under.png") && exists(T "/mnt/inside.png") && !exists(T "/plain.png"));
    umount(a);
    CHECK(exists(T "/over/under.png"));
    return fails;
}

static void test_mounts(void) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) { fails = 0; int r = mount_child(); _exit(r < 0 ? 77 : r ? 1 : 0); }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 77) { printf("clean-test: mount cases skipped: needs user namespaces\n"); return; }
    checks++;
    if (!WIFEXITED(st) || WEXITSTATUS(st)) { fails++; fprintf(stderr, "FAIL: mount cases\n"); }
}

/* ---- the tree: large and old files, the storage map ---- */

static void put_size(const char *rel, size_t n, char fill) {
    char p[PATH_MAX * 2];
    mkparents(rel);
    path_of(p, sizeof p, rel);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    char buf[4096];
    memset(buf, fill, sizeof buf);
    for (size_t off = 0; off < n; off += sizeof buf) if (write(fd, buf, n - off < sizeof buf ? n - off : sizeof buf) < 0) abort();
    close(fd);
}

static int page_total(int scan, const CleanFilter *f) {
    char *j = clean_results_of(scan, 0, CLEAN_PAGE_MAX, f);
    JDoc d = { 0 };
    if (!j || json_parse(j, strlen(j), &d, NULL, 0)) abort();
    int n = (int)json_num(d.root, "total", -1);
    json_free(&d);
    free(j);
    return n;
}

static void set_lock(const char *rel, const char *target) {
    char p[PATH_MAX * 2];
    mkparents(rel);
    path_of(p, sizeof p, rel);
    unlink(p);
    if (symlink(target, p)) abort();
}

static void test_browser(void) {
    clean_rules_override(NULL, 0);
    wipe();
    put(".cache/mozilla/firefox/abc.default/cache2/entries/one", "cached");
    put(".cache/mozilla/firefox/abc.default/cookies.sqlite", "person");
    put(".mozilla/firefox/abc.default/cookies.sqlite", "person");
    put(".mozilla/firefox/abc.default/places.sqlite", "person");
    put(".cache/chromium/Default/Cache/data_1", "cached");
    put(".config/chromium/Default/Cookies", "person");
    put(".config/chromium/Default/GPUCache/data_0", "cached");
    char live[64];
    snprintf(live, sizeof live, "127.0.1.1:+%d", (int)getpid());
    set_lock(".mozilla/firefox/abc.default/lock", live);
    snprintf(live, sizeof live, "host-%d", (int)getpid());
    set_lock(".config/chromium/SingletonLock", live);
    Poll p;
    int scan = run_scan(&p);
    CHECK(num(p.scan, "candidates") == 0);          /* the browsers run: not read */
    int busy = 0;
    for (const JVal *r = json_get(p.scan, "rules")->kid; r; r = r->next) busy += !strcmp(json_str(r, "root"), "busy");
    CHECK(busy >= 3);
    poll_free(&p);
    set_lock(".mozilla/firefox/abc.default/lock", "127.0.1.1:+999999");   /* stale: that process is gone */
    unlink(strcat(strcpy((char[PATH_MAX * 2]){ 0 }, H), "/.config/chromium/SingletonLock"));
    scan = run_scan(&p);
    CHECK(num(p.scan, "candidates") == 3);
    CHECK(!has_candidate("cookies") && !has_candidate("Cookies") && !has_candidate("places"));
    poll_free(&p);
    const char *const names[] = { "firefox-cache", "chromium-cache", "chromium-gpu" };
    int plan = clean_plan_rules(scan, names, 3);
    CHECK(plan > 0);
    wait_job(&p);
    CHECK(count(p.plan, "ready") == 3);
    poll_free(&p);
    /* the browser starts between the plan and the cleanup */
    snprintf(live, sizeof live, "host-%d", (int)getpid());
    set_lock(".config/chromium/SingletonLock", live);
    CHECK(clean_execute(plan) == plan);
    wait_job(&p);
    CHECK(count(p.plan, "done") == 1 && count(p.plan, "skipped") == 2);
    poll_free(&p);
    CHECK(!exists(".cache/mozilla/firefox/abc.default/cache2/entries/one") && exists(".cache/chromium/Default/Cache/data_1"));
    CHECK(exists(".mozilla/firefox/abc.default/cookies.sqlite") && exists(".config/chromium/Default/Cookies"));
    clean_cancel(1);
    clean_rules_override(&base, 1);
}

static void test_rules_on_disk(void) {
    clean_rules_override(NULL, 0);
    wipe();
    /* every rule's kind of place, with what must not be touched next to it */
    put(".cache/pip/http/a", "x");
    put(".npm/_cacache/content/a", "x");
    put(".cache/yarn/v6/a", "x");
    put(".cache/go-build/00/a", "x");
    put(".local/share/pnpm/store/v3/a", "x");
    put(".cache/lordos/thumbs/a.png", "x");
    put(".cache/lordos/fwlists/list", "must stay");
    put(".npm/other", "must stay");
    put(".cargo/registry/cache/a", "x");
    put(".gradle/caches/a", "x");
    put("Downloads/film.part", "x");
    put("Downloads/old.iso", "x");
    put("Downloads/new.iso", "x");
    char f[PATH_MAX * 2];
    struct timespec tm[2] = { { 1000000000, 0 }, { 1000000000, 0 } };
    path_of(f, sizeof f, "Downloads/film.part");
    CHECK(!utimensat(AT_FDCWD, f, tm, 0));
    path_of(f, sizeof f, "Downloads/old.iso");
    CHECK(!utimensat(AT_FDCWD, f, tm, 0));
    Poll p;
    int scan = run_scan(&p);
    CHECK(!strcmp(p.state, "done"));
    CHECK(has_candidate(".cache/pip/http/a") && has_candidate(".npm/_cacache/content/a") && has_candidate(".cache/go-build/00/a"));
    CHECK(has_candidate("thumbs/a.png") && !has_candidate("fwlists") && !has_candidate(".npm/other") && !has_candidate("new.iso"));
    poll_free(&p);
    /* a category-wide cleanup never takes a shown-only rule's files */
    const char *const names[] = { "pip", "npm", "yarn", "go-build", "lordos-thumbnails", "pnpm-store", "partial-downloads" };
    int plan = clean_plan_rules(scan, names, 7);
    CHECK(plan > 0);
    wait_job(&p);
    CHECK(num(p.plan, "deleteItems") == 5 && num(p.plan, "trashItems") == 2);
    poll_free(&p);
    CHECK(clean_execute(plan) == plan);
    wait_job(&p);
    CHECK(count(p.plan, "done") == 7);
    poll_free(&p);
    CHECK(!exists(".cache/pip/http/a") && exists(".local/share/Trash/files/a") && exists(".cargo/registry/cache/a") && exists("Downloads/old.iso"));
    CHECK(exists(".npm/other") && exists(".cache/lordos/fwlists/list"));
    clean_cancel(1);
    clean_rules_override(&base, 1);
}

static void test_models(void) {
    clean_rules_override(NULL, 0);
    wipe();
    put_size(".cache/huggingface/hub/models--org--small/blobs/aaa", 40000, 'a');
    put_size(".cache/huggingface/hub/models--org--small/snapshots/r1/x.bin", 20000, 'b');
    put_size(".cache/huggingface/hub/models--org--big/blobs/ccc.incomplete", 50000, 'c');
    put(".cache/huggingface/hub/version.txt", "1");
    put(".cache/huggingface/hub/datasets--x/blob", "not a model");
    put_size(".lmstudio/models/maker/tiny/m.gguf", 30000, 'd');
    put(".ollama/models/blobs/sha256-a", "shared");
    Poll p;
    int scan = run_scan(&p);
    CHECK(!strcmp(p.state, "done"));
    CHECK(has_candidate("models--org--small") && !has_candidate("/snapshots/") && !has_candidate("datasets--x"));
    CHECK(has_candidate(".lmstudio/models/maker/tiny") && !has_candidate("m.gguf"));
    poll_free(&p);
    /* never by category */
    const char *const hf[] = { "ai-huggingface" }, *const part[] = { "ai-huggingface-partial" }, *const ol[] = { "ai-ollama" };
    CHECK(clean_plan_rules(scan, hf, 1) == CLEAN_E_BADID && clean_plan_rules(scan, part, 1) == CLEAN_E_BADID);
    /* one model, by its id, and its size is the folder's */
    char *j = clean_results(0, 0, CLEAN_PAGE_MAX);
    JDoc d = { 0 };
    CHECK(j && !json_parse(j, strlen(j), &d, NULL, 0));
    uint32_t model = 0, ollama = 0;
    for (const JVal *v = json_get(d.root, "items")->kid; v; v = v->next) {
        if (strstr(json_str(v, "path"), "models--org--small")) { model = (uint32_t)json_num(v, "id", 0); CHECK(json_num(v, "size", 0) == 60000 && json_bool(v, "folder", 0) == 1); }
        if (strstr(json_str(v, "path"), ".ollama")) ollama = (uint32_t)json_num(v, "id", 0);
    }
    json_free(&d);
    free(j);
    CHECK(model && ollama);
    int plan = clean_plan(scan, &model, 1);
    CHECK(plan > 0);
    wait_job(&p);
    CHECK(json_bool(p.plan, "named", 0) == 1 && num(p.plan, "trashItems") == 1);
    poll_free(&p);
    CHECK(clean_execute(plan) == plan);
    wait_job(&p);
    CHECK(count(p.plan, "done") == 1);
    poll_free(&p);
    CHECK(!exists(".cache/huggingface/hub/models--org--small") && exists(".local/share/Trash/files/models--org--small/blobs/aaa"));
    CHECK(clean_restore(plan, model) == 0);
    CHECK(exists(".cache/huggingface/hub/models--org--small/blobs/aaa"));
    /* Ollama shares blobs: measured, nothing offered */
    plan = clean_plan(scan, &ollama, 1);
    wait_job(&p);
    CHECK(count(p.plan, "unsupported") == 1);
    poll_free(&p);
    (void)ol;
    clean_cancel(1);
    clean_rules_override(&base, 1);
}

/* the system actions' folder rules, on throwaway folders (the actions fix the real ones) */
static void test_system_rules(void) {
    wipe();
    put("apt/a_1.0_amd64.deb", "deb");
    put("apt/lock", "");
    put("apt/keep.txt", "");
    put("apt/sub/inner.deb", "deb");
    link_to("../secret.deb", "apt/evil.deb");
    put("secret.deb", "must stay");
    put("apt/partial/half.deb", "half");
    char p[PATH_MAX * 2];
    path_of(p, sizeof p, "apt");
    int fd = open(p, O_RDONLY | O_DIRECTORY);
    double freed = 0;
    CHECK(clean_folder_bytes(fd, 1) > 0);
    CHECK(clean_purge_debs(fd, &freed) == 1);
    close(fd);
    CHECK(!exists("apt/a_1.0_amd64.deb") && exists("apt/lock") && exists("apt/keep.txt") && exists("apt/sub/inner.deb"));
    CHECK(exists("apt/evil.deb") && exists("secret.deb"));
    path_of(p, sizeof p, "apt/partial");
    fd = open(p, O_RDONLY | O_DIRECTORY);
    CHECK(clean_purge_partial(fd, &freed) == 1);
    close(fd);

    put("tmp/old", "o");
    put("tmp/recent", "n");
    put("tmp/inuse", "u");
    put("tmp/linked", "l");
    put("tmp/sub/deep", "d");
    hardlink("tmp/linked", "tmp/sub/other");
    link_to("../secret.deb", "tmp/oldlink");
    time_t future = time(NULL) + 40 * 86400;
    path_of(p, sizeof p, "tmp/inuse");
    int held = open(p, O_RDONLY);
    path_of(p, sizeof p, "tmp");
    fd = open(p, O_RDONLY | O_DIRECTORY);
    /* "now" a little over 30 days ahead makes the files old, the recent one is touched past it */
    char q[PATH_MAX * 2];
    path_of(q, sizeof q, "tmp/recent");
    struct timespec tm[2] = { { future, 0 }, { future, 0 } };
    CHECK(!utimensat(AT_FDCWD, q, tm, 0));
    CHECK(clean_purge_old(fd, getuid() + 1, future + 1, &freed) == 0);        /* another owner's: not root's rule */
    CHECK(clean_purge_old(fd, getuid(), future - 40 * 86400, &freed) == 0);   /* nothing old yet */
    CHECK(clean_purge_old(fd, getuid(), future, &freed) == 1);
    close(fd);
    close(held);
    CHECK(!exists("tmp/old") && exists("tmp/recent") && exists("tmp/inuse") && exists("tmp/linked") && exists("tmp/oldlink") && exists("secret.deb"));
}

static void test_tree(void) {
    wipe();
    put_size("Pictures/a/big.png", 3 << 20, 'a');
    put_size("Pictures/small.png", 1000, 'b');
    put_size("Videos/film.mkv", 2 << 20, 'c');
    hardlink("Videos/film.mkv", "Videos/same-film.mkv");
    put_size("Documents/old.pdf", 2 << 20, 'd');
    char f[PATH_MAX * 2];
    path_of(f, sizeof f, "Documents/old.pdf");
    struct timespec tm[2] = { { 1000000000, 0 }, { 1000000000, 0 } };
    CHECK(!utimensat(AT_FDCWD, f, tm, 0));
    path_of(f, sizeof f, "Documents/sparse.bin");
    int fd = open(f, O_WRONLY | O_CREAT, 0600);
    CHECK(fd >= 0 && !ftruncate(fd, 8 << 20));
    close(fd);
    put(".local/share/lordos/vault/secret.png", "must not be listed");
    put("Skip/hidden.bin", "x");
    put_size("Skip/huge.bin", 2 << 20, 'e');
    CleanOpts o = { .min_size = 1 << 20, .exclude = { "Skip" }, .nexclude = 1 };
    int scan = clean_start_kind(CLEAN_KIND_TREE, &o);
    CHECK(scan > 0);
    Poll p;
    wait_job(&p);
    CHECK(!strcmp(p.state, "done"));
    CHECK(json_bool(p.scan, "complete", 0) == 1);
    CHECK(num(p.scan, "candidates") == 4);          /* big.png, film (one link), old.pdf, sparse.bin */
    CHECK(skipped(p.scan, "protected") >= 1 && skipped(p.scan, "excluded") >= 1);
    const JVal *ty = json_get(p.scan, "types");
    double video_files = -1, image_s = -1;
    for (const JVal *t = ty->kid; t; t = t->next) {
        if (!strcmp(json_str(t, "type"), "video")) video_files = json_num(t, "files", -1);
        if (!strcmp(json_str(t, "type"), "image")) image_s = json_num(t, "logical", -1);
    }
    CHECK(video_files == 1);                        /* a hard link counts once */
    CHECK(image_s == (3 << 20) + 1000);
    poll_free(&p);
    CHECK(page_total(scan, &(CleanFilter){ .type = CLEAN_TYPE_VIDEO }) == 1);
    CHECK(page_total(scan, &(CleanFilter){ .type = -1, .under = "Documents" }) == 2);
    CHECK(page_total(scan, &(CleanFilter){ .type = -1, .min_size = 3 << 20 }) == 2);
    CHECK(page_total(scan, &(CleanFilter){ .type = -1, .older_than = 1200000000 }) == 1);
    /* largest first */
    char *j = clean_results_of(scan, 0, 1, &(CleanFilter){ .type = -1 });
    CHECK(j && strstr(j, "sparse.bin"));
    free(j);
    /* the tree: allocated order, logical and allocated apart */
    j = clean_tree(scan, 0, 50);
    JDoc d = { 0 };
    CHECK(j && !json_parse(j, strlen(j), &d, NULL, 0));
    const JVal *kids = json_get(d.root, "children");
    CHECK(kids && kids->len >= 3);
    CHECK(!strcmp(json_str(kids->kid, "name"), "Pictures") || !strcmp(json_str(kids->kid, "name"), "Videos") || !strcmp(json_str(kids->kid, "name"), "Documents"));
    double last = 1e30, doc_l = 0, doc_a = 1e30;
    int doc = -1;
    for (const JVal *k = kids->kid; k; k = k->next) {
        CHECK(json_num(k, "allocated", 0) <= last);
        last = json_num(k, "allocated", 0);
        CHECK(strcmp(json_str(k, "name"), "Skip"));
        if (!strcmp(json_str(k, "name"), "Documents")) { doc_l = json_num(k, "logical", 0); doc_a = last; doc = (int)json_num(k, "id", -1); }
    }
    CHECK(doc_l == (10 << 20) && doc_a < (3 << 20));/* sparse: 8 MB logical, nothing allocated */
    CHECK(doc > 0);
    json_free(&d);
    free(j);
    j = clean_tree(scan, doc, 10);
    CHECK(j && strstr(j, "\"trail\":[{"));
    free(j);
    CHECK(clean_tree(scan, 99999, 10) == NULL);
    CHECK(clean_tree(scan + 1000, 0, 10) == NULL);
    /* large files go to the Trash, and can come back */
    uint32_t id = 0;
    j = clean_results_of(scan, 0, 1, &(CleanFilter){ .type = CLEAN_TYPE_IMAGE });
    CHECK(!json_parse(j, strlen(j), &d, NULL, 0));
    id = (uint32_t)json_num(json_get(d.root, "items")->kid, "id", 0);
    CHECK(!strcmp(json_str(json_get(d.root, "items")->kid, "action"), "trash"));
    json_free(&d);
    free(j);
    int plan = clean_plan(scan, &id, 1);
    CHECK(plan > 0);
    wait_job(&p);
    poll_free(&p);
    CHECK(clean_execute(plan) == plan);
    wait_job(&p);
    CHECK(count(p.plan, "done") == 1);
    poll_free(&p);
    CHECK(!exists("Pictures/a/big.png") && exists(".local/share/Trash/files/big.png"));
    CHECK(clean_restore(plan, id + 1) == CLEAN_E_BADID);
    CHECK(clean_restore(plan, id) == 0);
    CHECK(exists("Pictures/a/big.png") && !exists(".local/share/Trash/files/big.png"));
    CHECK(clean_restore(plan, id) == CLEAN_E_BADID);/* once */
    poll_read(&p);
    CHECK(count(p.plan, "restored") == 1);
    poll_free(&p);
    /* the history holds counts, never a path */
    j = clean_history();
    CHECK(j && strstr(j, "\"action\":\"clean\"") && !strstr(j, "Pictures") && !strstr(j, H) && !strstr(j, "big.png"));
    free(j);
    char hist[PATH_MAX * 2];
    path_of(hist, sizeof hist, ".local/state/lordos/cleaner-history.json");
    CHECK(exists(".local/state/lordos/cleaner-history.json"));
    clean_history_clear();
    CHECK(!exists(".local/state/lordos/cleaner-history.json"));
    /* options: a volume and a folder C doesn't know */
    CHECK(clean_start_kind(CLEAN_KIND_TREE, &(CleanOpts){ .volume = "mount:9" }) == CLEAN_E_BADFOLDER);
    CHECK(clean_start_kind(CLEAN_KIND_TREE, &(CleanOpts){ .volume = "/etc" }) == CLEAN_E_BADFOLDER);
    CHECK(clean_start_kind(CLEAN_KIND_TREE, &(CleanOpts){ .exclude = { "../x" }, .nexclude = 1 }) == CLEAN_E_BADFOLDER);
    j = clean_volumes();
    CHECK(j && strstr(j, "\"id\":\"root\"") && strstr(j, "\"id\":\"home\""));
    free(j);
}

/* a scan limited to one source: lord-server reports the root as read-only with no action */
static void test_tree_limits(void) {
    wipe();
    for (int i = 0; i < 300; i++) { char n[64]; snprintf(n, sizeof n, "many/d%d/f.bin", i); put(n, "x"); }
    int scan = clean_start_kind(CLEAN_KIND_TREE, &(CleanOpts){ .min_size = 1 });
    CHECK(scan > 0);
    Poll p;
    wait_job(&p);
    CHECK(num(p.scan, "candidates") == 300 && num(p.scan, "dirs") == 302);
    poll_free(&p);
    char *j = clean_tree(scan, 1, 500);
    JDoc d = { 0 };
    CHECK(j && !json_parse(j, strlen(j), &d, NULL, 0) && json_get(d.root, "children")->len == 300);
    json_free(&d);
    free(j);
    j = clean_tree(scan, 1, 10);
    CHECK(j && !json_parse(j, strlen(j), &d, NULL, 0) && json_num(d.root, "more", 0) == 290);
    json_free(&d);
    free(j);
}

/* ---- exact duplicates ---- */

static int member_id(int scan, const char *tail) {
    char *j = clean_groups(scan, 0, 100);
    JDoc d = { 0 };
    int id = 0;
    if (!j || json_parse(j, strlen(j), &d, NULL, 0)) abort();
    for (const JVal *g = json_get(d.root, "groups")->kid; g; g = g->next)
        for (const JVal *m = json_get(g, "members")->kid; m; m = m->next) {
            const char *p = json_str(m, "path");
            size_t a = strlen(p), b = strlen(tail);
            if (a >= b && !strcmp(p + a - b, tail)) id = (int)json_num(m, "id", 0);
        }
    json_free(&d);
    free(j);
    return id;
}

static void make_dups(void) {
    wipe();
    put_size("D/a/one.bin", 100000, 'x');
    put_size("D/b/two.bin", 100000, 'x');
    put_size("D/c/three.bin", 100000, 'x');
    put_size("D/same-size.bin", 100000, 'y');       /* same size, other content */
    /* the first and last 16 KB equal, the middle not: the partial hash matches, the full one must not */
    put_size("D/mid1.bin", 200000, 'm');
    put_size("D/mid2.bin", 200000, 'm');
    char f[PATH_MAX * 2];
    path_of(f, sizeof f, "D/mid2.bin");
    int fd = open(f, O_WRONLY);
    if (fd < 0 || pwrite(fd, "Z", 1, 100000) != 1) abort();
    close(fd);
    put("D/empty1.bin", "");
    put("D/empty2.bin", "");
    put_size("D/link1.bin", 50000, 'l');            /* only hard links: one file, no duplicate */
    hardlink("D/link1.bin", "D/link2.bin");
    put_size("D/h1.bin", 60000, 'h');               /* a copy and a pair of links */
    hardlink("D/h1.bin", "D/h2.bin");
    put_size("D/h3.bin", 60000, 'h');
}

static void test_dups(void) {
    make_dups();
    const CleanOpts o = { .folders = { "D" }, .nfolders = 1 };
    int scan = clean_start_kind(CLEAN_KIND_DUPS, &o);
    CHECK(scan > 0);
    Poll p;
    wait_job(&p);
    CHECK(!strcmp(p.state, "done"));
    CHECK(num(p.scan, "groups") == 2);
    CHECK(num(p.scan, "candidates") == 6);          /* one, two, three; h1, h2, h3 */
    CHECK(num(p.scan, "hashed") >= 5);
    poll_free(&p);
    CHECK(member_id(scan, "same-size.bin") == 0 && member_id(scan, "mid1.bin") == 0 && member_id(scan, "empty1.bin") == 0 && member_id(scan, "link1.bin") == 0);

    /* nothing is selected by itself; choosing the whole group is refused: a copy stays */
    uint32_t all[3] = { (uint32_t)member_id(scan, "one.bin"), (uint32_t)member_id(scan, "two.bin"), (uint32_t)member_id(scan, "three.bin") };
    int plan = clean_plan(scan, all, 3);
    CHECK(plan > 0);
    wait_job(&p);
    CHECK(count(p.plan, "skipped") == 3 && count(p.plan, "ready") == 0);
    int warned = 0;
    for (const JVal *w = json_get(p.plan, "warnings")->kid; w; w = w->next) warned |= !strcmp(w->str, "keepOneCopy");
    CHECK(warned);
    poll_free(&p);
    /* a hard link is not a copy: h1 and h2 are one file, so taking both still keeps h3, taking h3 and h1 keeps nothing else */
    uint32_t links[2] = { (uint32_t)member_id(scan, "h1.bin"), (uint32_t)member_id(scan, "h3.bin") };
    plan = clean_plan(scan, links, 2);
    wait_job(&p);
    CHECK(count(p.plan, "ready") == 1 && count(p.plan, "skipped") == 1);   /* h3 has no other inode left unselected */
    poll_free(&p);

    /* two of three: both go, the third stays */
    plan = clean_plan(scan, all, 2);
    wait_job(&p);
    CHECK(count(p.plan, "ready") == 2);
    poll_free(&p);
    CHECK(clean_execute(plan) == plan);
    wait_job(&p);
    CHECK(count(p.plan, "done") == 2);
    poll_free(&p);
    CHECK(!exists("D/a/one.bin") && !exists("D/b/two.bin") && exists("D/c/three.bin"));

    /* the copy that stays changed, its size, inode and date the same: the byte comparison stops it */
    make_dups();
    scan = clean_start_kind(CLEAN_KIND_DUPS, &o);
    wait_job(&p);
    poll_free(&p);
    uint32_t one = (uint32_t)member_id(scan, "one.bin");
    uint32_t pair[2] = { one, (uint32_t)member_id(scan, "two.bin") };
    plan = clean_plan(scan, pair, 2);
    wait_job(&p);
    CHECK(count(p.plan, "ready") == 2);
    poll_free(&p);
    char f[PATH_MAX * 2];
    struct stat st;
    path_of(f, sizeof f, "D/c/three.bin");
    CHECK(!stat(f, &st));
    int fd = open(f, O_WRONLY);
    CHECK(fd >= 0 && pwrite(fd, "!", 1, 50000) == 1);
    close(fd);
    struct timespec tm[2] = { st.st_atim, st.st_mtim };
    CHECK(!utimensat(AT_FDCWD, f, tm, 0));
    CHECK(clean_execute(plan) == plan);
    wait_job(&p);
    CHECK(count(p.plan, "done") == 0 && count(p.plan, "changed") == 2);
    poll_free(&p);
    CHECK(exists("D/a/one.bin") && exists("D/b/two.bin"));

    /* the copy that stays is gone */
    make_dups();
    scan = clean_start_kind(CLEAN_KIND_DUPS, &o);
    wait_job(&p);
    poll_free(&p);
    uint32_t single = (uint32_t)member_id(scan, "one.bin"), other[1] = { single };
    uint32_t dropped[2] = { other[0], (uint32_t)member_id(scan, "two.bin") };
    plan = clean_plan(scan, dropped, 2);
    wait_job(&p);
    poll_free(&p);
    path_of(f, sizeof f, "D/c/three.bin");
    CHECK(!unlink(f));
    CHECK(clean_execute(plan) == plan);
    wait_job(&p);
    CHECK(count(p.plan, "changed") == 2 && count(p.plan, "done") == 0);
    poll_free(&p);

    /* folders: only under the home folder, no overlap */
    CHECK(clean_start_kind(CLEAN_KIND_DUPS, &(CleanOpts){ .folders = { "../etc" }, .nfolders = 1 }) == CLEAN_E_BADFOLDER);
    CHECK(clean_start_kind(CLEAN_KIND_DUPS, &(CleanOpts){ .folders = { "/etc" }, .nfolders = 1 }) == CLEAN_E_BADFOLDER);
    CHECK(clean_start_kind(CLEAN_KIND_DUPS, &(CleanOpts){ .folders = { "D", "D/a" }, .nfolders = 2 }) == CLEAN_E_BADFOLDER);
    CHECK(clean_start_kind(CLEAN_KIND_DUPS, &(CleanOpts){ .nfolders = 0 }) == CLEAN_E_BADFOLDER);
    /* two folders are compared with each other */
    wipe();
    put_size("X/a.bin", 30000, 'q');
    put_size("Y/b.bin", 30000, 'q');
    scan = clean_start_kind(CLEAN_KIND_DUPS, &(CleanOpts){ .folders = { "X", "Y" }, .nfolders = 2 });
    wait_job(&p);
    CHECK(num(p.scan, "groups") == 1);
    poll_free(&p);
    clean_cancel(1);
}

int main(void) {
    if (syscall(SYS_landlock_create_ruleset, NULL, 0U, 1U) < 1) { printf("clean-test: skipped: needs Landlock\n"); return 0; }
    char tmpl[] = "/tmp/clean-test.XXXXXX";
    if (!mkdtemp(tmpl) || !realpath(tmpl, H)) return 1;
    fsops_init(H, "clean-test");
    CHECK(clean_available());
    test_rules();
    base = good();
    base.root = T;
    base.pattern = "*.png";
    base.text_key = "cleaner.rule.good";
    clean_rules_override(&base, 1);
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
    test_delete();
    test_mounts();
    test_browser();
    test_rules_on_disk();
    test_models();
    test_system_rules();
    test_tree();
    test_tree_limits();
    test_dups();
    wipe();
    rmdir(H);
    printf("clean-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
