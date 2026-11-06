/* brframe_test.c — Settings → Appearance → "Browsers: tabs in the title bar" (src/hw/brframe.c):
 * Chromium's Preferences edit (malformed JSON, missing keys, theme extensions, idempotence, the
 * rest of the file kept byte for byte), Firefox's user.js line and its tidy-up, running browsers,
 * stale locks, missing and hostile profile folders, over fake homes in a temporary folder.
 *
 *   build/brframe-test                          (make test)
 *   build/brframe-test edit <Preferences> on|off   prints the edited file (or why not) to stdout */
#include "hw/brframe.h"
#include "sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int n_ok, n_bad;
#define CHECK(cond, ...) do { if (cond) n_ok++; else { n_bad++; printf("✗ %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char root[256];

static void mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0700); *s = '/'; }
    mkdir(p, 0700);
}

static void put(const char *rel, const char *text) {
    char p[1024], d[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    snprintf(d, sizeof d, "%s", p);
    char *sl = strrchr(d, '/');
    if (sl) { *sl = 0; mkdirs(d); }
    FILE *f = fopen(p, "wb");
    if (!f) { printf("cannot write %s: %s\n", p, strerror(errno)); exit(2); }
    fputs(text, f);
    fclose(f);
}

static char *get(const char *rel) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    static char buf[8][65536];
    static int k;
    char *b = buf[k++ % 8];
    size_t n = fread(b, 1, sizeof buf[0] - 1, f);
    b[n] = 0;
    fclose(f);
    return b;
}

static int exists(const char *rel) {
    char p[1024];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", root, rel);
    return lstat(p, &st) == 0;
}

static void path(char *out, size_t n, const char *rel) { snprintf(out, n, "%s/%s", root, rel); }

/* the parsed value of browser.custom_chrome_frame (-1 missing) and extensions.theme.system_theme */
static void chrome_values(const char *text, int *frame, int *theme) {
    JDoc d;
    *frame = *theme = -2;
    if (!text || json_parse(text, strlen(text), &d, NULL, 0)) return;
    const JVal *b = json_get(d.root, "browser"), *f = b ? json_get(b, "custom_chrome_frame") : NULL;
    const JVal *e = json_get(d.root, "extensions"), *t = e ? json_get(e, "theme") : NULL, *s = t ? json_get(t, "system_theme") : NULL;
    *frame = f && f->type == J_BOOL ? (int)f->num : -1;
    *theme = s && s->type == J_NUM ? (int)s->num : -1;
    json_free(&d);
}

static int edit(const char *in, int on, char **out) {
    size_t n;
    return brf_chrome_edit(in, strlen(in), on, out, &n);
}

static void test_chrome_edit(void) {
    char *o, *o2;
    int f, t;
    CHECK(edit("{}", 1, &o) == BRF_CHANGED, "empty object, on");
    chrome_values(o, &f, &t);
    CHECK(f == 1 && t == 1, "empty on → frame %d theme %d", f, t);
    CHECK(edit(o, 1, &o2) == BRF_SAME && !o2, "second time is the same");
    free(o);

    CHECK(edit("{}", 0, &o) == BRF_CHANGED, "empty off");
    chrome_values(o, &f, &t);
    CHECK(f == 0 && t == -1, "off writes only the frame (%d %d)", f, t);
    free(o);

    /* the rest of the file is kept byte for byte */
    const char *real = "{\"a\":\"}\\\"{\",\"browser\":{\"has_seen_welcome_page\":true,\"custom_chrome_frame\":false,\"window_placement\":{\"x\":1}},"
                       "\"extensions\":{\"theme\":{\"system_theme\":0}},\"z\":[1,2,{\"q\":null}]}";
    CHECK(edit(real, 1, &o) == BRF_CHANGED, "real-looking prefs");
    CHECK(o && !strcmp(o, "{\"a\":\"}\\\"{\",\"browser\":{\"has_seen_welcome_page\":true,\"custom_chrome_frame\":true,\"window_placement\":{\"x\":1}},"
                         "\"extensions\":{\"theme\":{\"system_theme\":1}},\"z\":[1,2,{\"q\":null}]}"), "surgical edit: %s", o ? o : "(null)");
    CHECK(edit(o, 1, &o2) == BRF_SAME, "idempotent");
    CHECK(edit(o, 0, &o2) == BRF_CHANGED, "off again");
    chrome_values(o2, &f, &t);
    CHECK(f == 0 && t == 1, "off keeps the GTK theme (%d %d)", f, t);
    free(o2);
    free(o);

    /* pretty-printed, members missing in an existing object */
    CHECK(edit("{\n  \"browser\": {\n    \"x\": 1\n  },\n  \"extensions\": { }\n}\n", 1, &o) == BRF_CHANGED, "pretty");
    chrome_values(o, &f, &t);
    CHECK(f == 1 && t == 1, "pretty → %d %d", f, t);
    free(o);

    /* a theme extension the user installed: the frame changes, the theme doesn't */
    CHECK(edit("{\"extensions\":{\"theme\":{\"id\":\"abcdefghijklmnopabcdefghijklmnop\",\"system_theme\":0}}}", 1, &o) == BRF_CHANGED, "theme ext");
    chrome_values(o, &f, &t);
    CHECK(f == 1 && t == 0, "theme extension kept (%d %d)", f, t);
    free(o);
    CHECK(edit("{\"browser\":{\"custom_chrome_frame\":true},\"extensions\":{\"theme\":{\"id\":\"x\"}}}", 1, &o) == BRF_SAME, "theme ext, already on");

    /* malformed or unexpected: refused, never guessed */
    const char *bad[] = {
        "", "{", "{\"browser\":{\"custom_chrome_frame\":tru}}", "{} x", "[1,2]", "\"str\"", "null",
        "{\"browser\":5}", "{\"browser\":{\"custom_chrome_frame\":true},\"extensions\":{\"theme\":7}}",
        "{\"browser\":{},\"browser\":{}}",                      /* duplicate keys */
        "{\"\\u0062rowser\":{\"x\":1}}",                         /* the same key, escaped: inserting would duplicate it */
        "{\"a\":\"\\u0000\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        o = (char *)1;
        int r = edit(bad[i], 1, &o);
        CHECK(r == BRF_BAD && o == NULL, "bad[%zu] %s → %d", i, bad[i], r);
    }
}

static int uedit(const char *in, int v, char **out, int *own) {
    size_t n;
    return brf_userjs_edit(in, strlen(in), v, out, &n, own);
}

static void test_userjs(void) {
    char *o, *o2;
    int own;
    CHECK(uedit("", 0, &o, &own) == BRF_CHANGED && !own, "empty user.js");
    CHECK(o && brf_userjs_ours(o, strlen(o)) == 0 && brf_prefs_value(o, strlen(o)) == 0, "line with 0: %s", o);
    CHECK(uedit(o, 0, &o2, &own) == BRF_SAME, "same value again");
    CHECK(uedit(o, 1, &o2, &own) == BRF_CHANGED && brf_userjs_ours(o2, strlen(o2)) == 1, "replaced by 1");
    CHECK(!strstr(strstr(o2, "inTitlebar") + 1, "inTitlebar"), "only one line: %s", o2);
    free(o);
    CHECK(uedit(o2, -1, &o, &own) == BRF_CHANGED && o && !o[0], "removed: empty");
    free(o);
    free(o2);

    /* other lines are kept, with or without a final newline */
    CHECK(uedit("user_pref(\"a.b\", true);\n// note", 1, &o, &own) == BRF_CHANGED, "keeps others");
    CHECK(o && !strncmp(o, "user_pref(\"a.b\", true);\n// note\nuser_pref(\"browser.tabs.inTitlebar\", 1);", 61), "appended: %s", o);
    CHECK(uedit(o, -1, &o2, &own) == BRF_CHANGED && !strcmp(o2, "user_pref(\"a.b\", true);\n// note\n"), "removed again: [%s]", o2);
    free(o);
    free(o2);
    CHECK(uedit("user_pref(\"a.b\", true);\n", -1, &o, &own) == BRF_SAME, "nothing of ours to remove");

    /* the user's own line (either quotes): theirs, untouched */
    CHECK(uedit("user_pref(\"browser.tabs.inTitlebar\", 0);\n", 1, &o, &own) == BRF_SAME && own && !o, "own line kept");
    CHECK(uedit("  user_pref('browser.tabs.inTitlebar', 1);\n", 0, &o, &own) == BRF_SAME && own, "own line, single quotes");
    CHECK(uedit("// user_pref(\"browser.tabs.inTitlebar\", 0);\n", 1, &o, &own) == BRF_CHANGED && !own, "a comment isn't a line");
    free(o);
    CHECK(uedit("user_pref(\"browser.tabs.inTitlebarX\", 0);\n", 1, &o, &own) == BRF_CHANGED && !own, "another pref name");
    free(o);
    size_t n;
    CHECK(brf_userjs_edit("a\0b", 3, 1, &o, &n, &own) == BRF_BAD, "NUL bytes refused");

    /* prefs.js values */
    const char *pj = "// Mozilla User Preferences\nuser_pref(\"browser.startup.page\", 3);\nuser_pref(\"browser.tabs.inTitlebar\", 0);\n";
    CHECK(brf_prefs_value(pj, strlen(pj)) == 0, "prefs.js 0");
    CHECK(brf_prefs_value("", 0) == -1, "prefs.js unset");
    CHECK(brf_prefs_value("user_pref(\"browser.tabs.inTitlebar\", true);", 44) == 1, "true");
}

/* ---- on disk ---- */

static pid_t child_named(const char *name) {
    pid_t p = fork();
    if (p == 0) {
        prctl(PR_SET_NAME, name, 0, 0, 0);
        for (;;) pause();
    }
    usleep(50000);
    return p;
}

static void stop(pid_t p) { kill(p, SIGKILL); waitpid(p, NULL, 0); }

static void lock_link(const char *rel, const char *target) {
    char p[1024];
    path(p, sizeof p, rel);
    unlink(p);
    if (symlink(target, p)) printf("symlink %s: %s\n", p, strerror(errno));
}

static void test_chromium_disk(void) {
    char cfg[512], host[256], t[512];
    BrfResult r;
    path(cfg, sizeof cfg, "h1/.config");
    mkdirs(cfg);
    brf_chromium_apply(cfg, 1, &r);
    CHECK(!r.found && !r.profiles && !r.changed, "no chromium folder");
    CHECK(!brf_chromium_running(cfg), "no folder: not running");

    const char *orig = "{\"browser\":{\"custom_chrome_frame\":false},\"zz\":1}";
    put("h1/.config/chromium/Default/Preferences", orig);
    put("h1/.config/chromium/Profile 1/Preferences", "{}");
    put("h1/.config/chromium/Profile 2/Preferences", "{\"browser\":");             /* truncated */
    put("h1/.config/chromium/System Profile/Preferences", "{}");                   /* not a user profile */
    put("h1/.config/chromium/Profile x/Preferences", "{}");
    mkdirs(root);
    put("h1/.config/chromium/Profile 3/.keep", "");                                 /* never started */
    put("h1/target.json", "{}");
    mkdirs(cfg);
    {
        char p[1024];
        path(p, sizeof p, "h1/.config/chromium/Profile 4");
        mkdirs(p);
        lock_link("h1/.config/chromium/Profile 4/Preferences", "../../../target.json");   /* a link: refused */
        path(p, sizeof p, "h1/.config/chromium/Default/Preferences");
        chmod(p, 0600);
    }
    char *before2 = strdup(get("h1/.config/chromium/Profile 2/Preferences"));
    int son0, soff0;
    brf_chromium_state(cfg, &son0, &soff0);
    CHECK(son0 == 1 && soff0 == 2, "state before: %d on, %d off (an unset frame counts for both, a malformed profile for none)", son0, soff0);

    /* running: a live "chromium" process of ours holds the lock → nothing is written */
    gethostname(host, sizeof host);
    pid_t ch = child_named("chromium");
    snprintf(t, sizeof t, "%s-%d", host, (int)ch);
    lock_link("h1/.config/chromium/SingletonLock", t);
    CHECK(brf_chromium_running(cfg), "running");
    brf_chromium_apply(cfg, 1, &r);
    CHECK(r.found && r.running && !r.changed && !strcmp(get("h1/.config/chromium/Default/Preferences"), orig), "running: untouched");
    stop(ch);

    /* a stale lock (dead pid, or a pid that isn't chromium) doesn't stop it; another host does */
    snprintf(t, sizeof t, "%s-%d", host, (int)getpid());
    lock_link("h1/.config/chromium/SingletonLock", t);
    CHECK(!brf_chromium_running(cfg), "our own pid isn't chromium");
    lock_link("h1/.config/chromium/SingletonLock", "some-other-host-12");
    CHECK(brf_chromium_running(cfg), "another computer counts as running");
    snprintf(t, sizeof t, "%s-%d", host, (int)ch);        /* dead now */
    lock_link("h1/.config/chromium/SingletonLock", t);
    CHECK(!brf_chromium_running(cfg), "dead pid: stale");

    brf_chromium_apply(cfg, 1, &r);
    CHECK(r.found && !r.running && r.profiles == 4 && r.changed == 2 && r.failed == 2 && !r.unchanged,
          "apply: profiles %d changed %d failed %d unchanged %d", r.profiles, r.changed, r.failed, r.unchanged);
    int f, th;
    chrome_values(get("h1/.config/chromium/Default/Preferences"), &f, &th);
    CHECK(f == 1 && th == 1, "Default on (%d %d)", f, th);
    CHECK(strstr(get("h1/.config/chromium/Default/Preferences"), "\"zz\":1") != NULL, "other keys kept");
    CHECK(!strcmp(get("h1/.config/chromium/Default/Preferences.lordos-backup"), orig), "backup is the original");
    chrome_values(get("h1/.config/chromium/Profile 1/Preferences"), &f, &th);
    CHECK(f == 1 && th == 1, "Profile 1 on");
    CHECK(!strcmp(get("h1/.config/chromium/Profile 2/Preferences"), before2) && !exists("h1/.config/chromium/Profile 2/Preferences.lordos-backup"),
          "malformed profile untouched, no backup");
    CHECK(!strcmp(get("h1/target.json"), "{}"), "a linked Preferences is never followed");
    CHECK(!strcmp(get("h1/.config/chromium/System Profile/Preferences"), "{}") && !strcmp(get("h1/.config/chromium/Profile x/Preferences"), "{}"),
          "other folders untouched");
    CHECK(!exists("h1/.config/chromium/Default/Preferences.lordos-tmp"), "no temporary file left");
    {
        char p[1024];
        struct stat st;
        path(p, sizeof p, "h1/.config/chromium/Default/Preferences");
        CHECK(!stat(p, &st) && (st.st_mode & 0777) == 0600, "mode kept: %o", st.st_mode & 0777);
    }

    int son, soff;
    brf_chromium_state(cfg, &son, &soff);
    CHECK(son == 2 && soff == 0, "state after on: %d on, %d off", son, soff);
    brf_chromium_apply(cfg, 1, &r);
    CHECK(r.changed == 0 && r.unchanged == 2 && r.failed == 2, "idempotent: changed %d unchanged %d", r.changed, r.unchanged);
    brf_chromium_apply(cfg, 0, &r);
    CHECK(r.changed == 2, "off: changed %d", r.changed);
    chrome_values(get("h1/.config/chromium/Default/Preferences"), &f, &th);
    CHECK(f == 0 && th == 1, "Default off (%d %d)", f, th);
    brf_chromium_state(cfg, &son, &soff);
    CHECK(son == 0 && soff == 2, "state after off: %d on, %d off", son, soff);
    free(before2);
}

/* Brave (#39): its profiles under ~/.config/BraveSoftware/Brave-Browser, its own lock and process name */
static void test_brave_disk(void) {
    char cfg[512], host[256], t[512];
    BrfResult r;
    path(cfg, sizeof cfg, "h3/.config");
    mkdirs(cfg);
    /* a new Brave profile: no frame key, no theme key (Brave's own frame with the Classic theme) */
    const char *fresh = "{\"brave\":{\"rewards\":{}},\"extensions\":{\"settings\":{}}}";
    put("h3/.config/BraveSoftware/Brave-Browser/Default/Preferences", fresh);
    put("h3/.config/BraveSoftware/Brave-Browser/Profile 2/Preferences", "{\"browser\":{\"custom_chrome_frame\":true},\"extensions\":{\"theme\":{\"system_theme\":1}}}");
    put("h3/.config/chromium/Default/Preferences", "{}");
    int son, soff;
    brf_chromium_state(cfg, &son, &soff);
    CHECK(soff == 2 && son == 3, "state with Brave: %d on, %d off", son, soff);

    /* Brave open (a live "brave" holds its lock): Chromium is written, Brave waits */
    gethostname(host, sizeof host);
    pid_t b = child_named("brave");
    snprintf(t, sizeof t, "%s-%d", host, (int)b);
    lock_link("h3/.config/BraveSoftware/Brave-Browser/SingletonLock", t);
    CHECK(brf_chromium_running(cfg), "Brave open counts as running");
    brf_chromium_apply(cfg, 1, &r);
    CHECK(r.found && r.running && r.changed == 1 && !strcmp(get("h3/.config/BraveSoftware/Brave-Browser/Default/Preferences"), fresh),
          "Brave open: Chromium changed %d, Brave untouched", r.changed);
    stop(b);
    /* a "chromium" process holding Brave's lock isn't Brave: stale */
    pid_t c = child_named("chromium");
    snprintf(t, sizeof t, "%s-%d", host, (int)c);
    lock_link("h3/.config/BraveSoftware/Brave-Browser/SingletonLock", t);
    CHECK(!brf_chromium_running(cfg), "another browser's pid in Brave's lock is stale");
    stop(c);
    brf_chromium_apply(cfg, 1, &r);
    CHECK(r.found && !r.running && r.changed == 1 && r.unchanged == 2, "Brave closed: changed %d unchanged %d", r.changed, r.unchanged);
    int f, th;
    chrome_values(get("h3/.config/BraveSoftware/Brave-Browser/Default/Preferences"), &f, &th);
    CHECK(f == 1 && th == 1, "Brave Default: own bar and the GTK theme (%d %d)", f, th);
    CHECK(strstr(get("h3/.config/BraveSoftware/Brave-Browser/Default/Preferences"), "\"rewards\":{}") != NULL, "Brave's own keys kept");
    CHECK(!strcmp(get("h3/.config/BraveSoftware/Brave-Browser/Default/Preferences.lordos-backup"), fresh), "Brave backup");
    brf_chromium_state(cfg, &son, &soff);
    CHECK(soff == 0 && son == 3, "state after on: %d on, %d off", son, soff);
    brf_chromium_apply(cfg, 0, &r);
    CHECK(r.changed == 3, "off: changed %d", r.changed);
    chrome_values(get("h3/.config/BraveSoftware/Brave-Browser/Profile 2/Preferences"), &f, &th);
    CHECK(f == 0 && th == 1, "Brave Profile 2 off (%d %d)", f, th);
}

static void touch_later(const char *rel, int secs) {
    char p[1024];
    path(p, sizeof p, rel);
    struct timespec ts[2];
    clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec += secs;
    ts[1] = ts[0];
    utimensat(AT_FDCWD, p, ts, AT_SYMLINK_NOFOLLOW);
}

static void test_firefox_disk(void) {
    char cfg[512], home[512];
    BrfResult r;
    path(home, sizeof home, "h2");
    path(cfg, sizeof cfg, "h2/.config");
    mkdirs(cfg);
    brf_firefox_apply(cfg, home, 1, &r);
    CHECK(!r.found && !r.profiles, "no Firefox folders");

    put("h2/.config/mozilla/firefox/profiles.ini",
        "[Profile1]\nName=default\nIsRelative=1\nPath=aaaa.default\nDefault=1\n\n"
        "[Profile0]\r\nName=default-esr\r\nIsRelative=1\r\nPath=bbbb.default-esr\r\n\r\n"
        "[Profile2]\nName=abs\nIsRelative=0\nPath=/tmp\n\n"
        "[Profile3]\nName=evil\nIsRelative=1\nPath=../../etc\n\n"
        "[Profile4]\nName=missing\nIsRelative=1\nPath=nothere\n\n"
        "[Install3B6073811A6ABF12]\nDefault=bbbb.default-esr\nLocked=1\n\n"
        "[General]\nStartWithLastProfile=1\nVersion=2");                  /* no final newline */
    put("h2/.config/mozilla/firefox/aaaa.default/prefs.js", "user_pref(\"browser.tabs.inTitlebar\", 1);\n");
    put("h2/.config/mozilla/firefox/bbbb.default-esr/prefs.js", "user_pref(\"x\", 1);\n");
    put("h2/.config/mozilla/firefox/bbbb.default-esr/user.js", "user_pref(\"x.y\", 2);");
    /* legacy folder with a profile whose user wrote the line themselves */
    put("h2/.mozilla/firefox/profiles.ini", "[Profile0]\nIsRelative=1\nPath=cccc.mine\n");
    put("h2/.mozilla/firefox/cccc.mine/user.js", "user_pref(\"browser.tabs.inTitlebar\", 0);\n");

    brf_firefox_apply(cfg, home, 1, &r);
    CHECK(r.found && r.profiles == 3 && r.changed == 1 && r.unchanged == 1 && r.kept == 1 && !r.failed,
          "apply on: profiles %d changed %d unchanged %d kept %d failed %d", r.profiles, r.changed, r.unchanged, r.kept, r.failed);
    CHECK(!exists("h2/.config/mozilla/firefox/aaaa.default/user.js"), "prefs.js already 1: no line needed");
    char *u = get("h2/.config/mozilla/firefox/bbbb.default-esr/user.js");
    CHECK(u && !strncmp(u, "user_pref(\"x.y\", 2);\nuser_pref(\"browser.tabs.inTitlebar\", 1);", 61), "line appended: %s", u ? u : "");
    CHECK(!strcmp(get("h2/.config/mozilla/firefox/bbbb.default-esr/user.js.lordos-backup"), "user_pref(\"x.y\", 2);"), "user.js backup");
    CHECK(!strcmp(get("h2/.mozilla/firefox/cccc.mine/user.js"), "user_pref(\"browser.tabs.inTitlebar\", 0);\n"), "own line untouched");

    brf_firefox_apply(cfg, home, 1, &r);
    CHECK(r.changed == 0 && r.unchanged == 2 && r.kept == 1, "idempotent: changed %d unchanged %d", r.changed, r.unchanged);

    /* off: both profiles get the line (aaaa's prefs.js says 1) */
    brf_firefox_apply(cfg, home, 0, &r);
    CHECK(r.changed == 2, "off: changed %d", r.changed);
    CHECK(brf_userjs_ours(get("h2/.config/mozilla/firefox/aaaa.default/user.js"), strlen(get("h2/.config/mozilla/firefox/aaaa.default/user.js"))) == 0, "aaaa 0");
    {
        char p[1024];
        struct stat st;
        path(p, sizeof p, "h2/.config/mozilla/firefox/aaaa.default/user.js");
        CHECK(!stat(p, &st) && (st.st_mode & 0777) == 0600, "new user.js is 0600");
    }

    /* tidy: not before Firefox has started with the line. prefs.js written after it is no proof
     * (Firefox writes it when it quits, also after a line written while it ran) */
    touch_later("h2/.config/mozilla/firefox/aaaa.default/prefs.js", 100);
    touch_later("h2/.config/mozilla/firefox/bbbb.default-esr/prefs.js", 100);
    CHECK(brf_firefox_tidy(cfg, home) == 0, "no start since the line: nothing to tidy");
    put("h2/.config/mozilla/firefox/aaaa.default/.parentlock", "");
    touch_later("h2/.config/mozilla/firefox/aaaa.default/.parentlock", -100);
    lock_link("h2/.config/mozilla/firefox/bbbb.default-esr/lock", "127.0.1.1:+1");
    touch_later("h2/.config/mozilla/firefox/bbbb.default-esr/lock", -100);
    CHECK(brf_firefox_tidy(cfg, home) == 0, "last start before the line: nothing to tidy");

    /* running: a process holds .parentlock → the line is written anyway (read at the next start),
     * counted as running, and tidy waits */
    put("h2/.config/mozilla/firefox/bbbb.default-esr/.parentlock", "");
    char p[1024];
    path(p, sizeof p, "h2/.config/mozilla/firefox/bbbb.default-esr/.parentlock");
    int pipefd[2];
    if (pipe(pipefd)) { perror("pipe"); exit(2); }
    pid_t ff = fork();
    if (ff == 0) {
        int fd = open(p, O_RDWR);
        struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
        fcntl(fd, F_SETLK, &fl);
        if (write(pipefd[1], "k", 1) < 0) _exit(1);
        for (;;) pause();
    }
    char k;
    if (read(pipefd[0], &k, 1) != 1) { perror("read"); exit(2); }
    brf_firefox_apply(cfg, home, 1, &r);
    CHECK(r.changed == 2 && r.running == 1, "running profile: changed %d running %d", r.changed, r.running);
    /* both profiles "started" after the line; only the closed one is tidied */
    touch_later("h2/.config/mozilla/firefox/aaaa.default/.parentlock", 100);
    touch_later("h2/.config/mozilla/firefox/bbbb.default-esr/.parentlock", 100);
    CHECK(brf_firefox_tidy(cfg, home) == 1, "tidy only the closed profile");
    CHECK(!exists("h2/.config/mozilla/firefox/aaaa.default/user.js"), "a user.js that held only our line is removed");
    stop(ff);
    close(pipefd[0]);
    close(pipefd[1]);

    /* the "lock" link of a live firefox counts too; a stale one doesn't */
    char t[64];
    pid_t fx = child_named("firefox-esr");
    snprintf(t, sizeof t, "127.0.1.1:+%d", (int)fx);
    lock_link("h2/.config/mozilla/firefox/bbbb.default-esr/lock", t);
    touch_later("h2/.config/mozilla/firefox/bbbb.default-esr/lock", 100);
    CHECK(brf_firefox_tidy(cfg, home) == 0, "lock link of a live firefox: wait");
    stop(fx);
    CHECK(brf_firefox_tidy(cfg, home) == 1, "stale lock link: tidied");
    CHECK(!strcmp(get("h2/.config/mozilla/firefox/bbbb.default-esr/user.js"), "user_pref(\"x.y\", 2);\n"), "other lines kept: %s",
          get("h2/.config/mozilla/firefox/bbbb.default-esr/user.js"));
    CHECK(brf_firefox_tidy(cfg, home) == 0, "tidy is idempotent");
    CHECK(!strcmp(get("h2/.mozilla/firefox/cccc.mine/user.js"), "user_pref(\"browser.tabs.inTitlebar\", 0);\n"), "own line still untouched");

    /* a user.js that is a link or too big is refused */
    lock_link("h2/.config/mozilla/firefox/aaaa.default/user.js", "../../../../target");
    put("h2/target", "keep");
    brf_firefox_apply(cfg, home, 0, &r);
    CHECK(r.failed == 1 && !strcmp(get("h2/target"), "keep"), "linked user.js refused (failed %d)", r.failed);
}

static int edit_file(const char *file, int on) {
    FILE *f = fopen(file, "rb");
    if (!f) { perror(file); return 2; }
    static char buf[32 << 20];
    size_t n = fread(buf, 1, sizeof buf, f), outn;
    fclose(f);
    char *out;
    int r = brf_chrome_edit(buf, n, on, &out, &outn);
    if (r == BRF_CHANGED) fwrite(out, 1, outn, stdout);
    fprintf(stderr, "%s\n", r == BRF_CHANGED ? "changed" : r == BRF_SAME ? "same" : "bad");
    free(out);
    return r == BRF_BAD ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "edit")) return edit_file(argv[2], !strcmp(argv[3], "on"));
    snprintf(root, sizeof root, "%s/brframe-test.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    test_chrome_edit();
    test_userjs();
    test_chromium_disk();
    test_brave_disk();
    test_firefox_disk();
    BrfResult a = { 1, 2, 1, 1, 0, 0, 0 }, b = { 0 };
    char *j = brf_result_json(&a, &b);
    JDoc d;
    CHECK(j && !json_parse(j, strlen(j), &d, NULL, 0), "result JSON: %s", j ? j : "");
    if (j) json_free(&d);
    free(j);
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd)) printf("could not remove %s\n", root);
    printf("brframe-test: %d ok, %d failed\n", n_ok, n_bad);
    return n_bad ? 1 : 0;
}
