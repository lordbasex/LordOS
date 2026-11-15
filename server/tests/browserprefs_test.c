/* browserprefs_test.c — LordOS's initial preferences for the Chromium-based browsers from the Store
 * (src/pkg/browserprefs.c): the merge rule over vendor files and the file handling in fixture roots.
 * With --browser-prefs it is the same command as `lord-server --browser-prefs`, for
 * tests/migration_chromium_family_test.sh. */
#include "../src/pkg/browserprefs.h"
#include "../src/sec/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, n;
#define CHECK(c, ...) do { n++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the value at a dotted path of a JSON text, as a number (bool: 0/1), or -99 if absent / not JSON */
static double at(const char *text, const char *a, const char *b, const char *c) {
    JDoc d;
    if (json_parse(text, strlen(text), &d, NULL, 0)) return -99;
    const JVal *v = json_get(d.root, a);
    if (v && b) v = json_get(v, b);
    if (v && c) v = json_get(v, c);
    double r = !v ? -99 : v->type == J_BOOL || v->type == J_NUM ? v->num : -98;
    json_free(&d);
    return r;
}

static const char *strof(const char *text, const char *a, const char *b) {
    static char buf[256];
    JDoc d;
    buf[0] = 0;
    if (json_parse(text, strlen(text), &d, NULL, 0)) return buf;
    const JVal *v = json_get(d.root, a);
    if (v && b) v = json_get(v, b);
    if (v && v->type == J_STR) snprintf(buf, sizeof buf, "%s", v->str);
    json_free(&d);
    return buf;
}

/* merges in, checks the result code, and that a second merge of the result changes nothing */
static char *merged(const char *in, int want) {
    char *out = NULL;
    size_t ol;
    int r = browser_prefs_merge(in, in ? strlen(in) : 0, &out, &ol);
    CHECK(r == want, "merge of %s: %d, expected %d", in ? in : "(none)", r, want);
    if (r != 1) { free(out); return in ? strdup(in) : NULL; }
    CHECK(strlen(out) == ol, "length");
    char *again = NULL;
    size_t al;
    CHECK(browser_prefs_merge(out, ol, &again, &al) == 0, "second merge of %s changed it again", out);
    free(again);
    return out;
}

static void test_merge(void) {
    char *s = merged(NULL, 1);
    CHECK(s && at(s, "browser", "custom_chrome_frame", NULL) == 1, "fresh: custom frame");
    CHECK(s && at(s, "extensions", "theme", "system_theme") == 1, "fresh: GTK theme");
    free(s);

    s = merged("{}", 1);
    CHECK(at(s, "browser", "custom_chrome_frame", NULL) == 1 && at(s, "extensions", "theme", "system_theme") == 1, "empty object: %s", s);
    free(s);

    /* the vendor's keys stay, with their values */
    s = merged("{\n  \"distribution\": {\"skip_first_run_ui\": true, \"import_bookmarks\": false},\n  \"homepage\": \"https://example.org/\"\n}\n", 1);
    CHECK(at(s, "distribution", "skip_first_run_ui", NULL) == 1 && at(s, "distribution", "import_bookmarks", NULL) == 0, "vendor distribution kept: %s", s);
    CHECK(!strcmp(strof(s, "homepage", NULL), "https://example.org/"), "vendor homepage kept: %s", s);
    CHECK(at(s, "browser", "custom_chrome_frame", NULL) == 1 && at(s, "extensions", "theme", "system_theme") == 1, "added: %s", s);
    free(s);

    /* inside the vendor's own "browser" and "extensions" objects */
    s = merged("{\"browser\": {\"show_home_button\": true}, \"extensions\": {\"settings\": {}}}", 1);
    CHECK(at(s, "browser", "show_home_button", NULL) == 1 && at(s, "browser", "custom_chrome_frame", NULL) == 1, "into browser: %s", s);
    CHECK(at(s, "extensions", "theme", "system_theme") == 1, "into extensions: %s", s);
    free(s);
    s = merged("{\"extensions\": {\"theme\": {}}}", 1);
    CHECK(at(s, "extensions", "theme", "system_theme") == 1 && at(s, "browser", "custom_chrome_frame", NULL) == 1, "into empty theme: %s", s);
    free(s);

    /* a vendor's own value is never replaced */
    s = merged("{\"browser\": {\"custom_chrome_frame\": false}}", 1);
    CHECK(at(s, "browser", "custom_chrome_frame", NULL) == 0, "vendor false kept: %s", s);
    CHECK(at(s, "extensions", "theme", "system_theme") == 1, "theme still added: %s", s);
    free(s);
    s = merged("{\"browser\": {\"custom_chrome_frame\": false}, \"extensions\": {\"theme\": {\"system_theme\": 0}}}", 0);
    free(s);
    /* a key of another shape is the vendor's: left as it is */
    s = merged("{\"extensions\": [1, 2], \"browser\": \"x\"}", 0);
    free(s);
    s = merged("{\"extensions\": {\"theme\": 3}}", 1);
    CHECK(at(s, "extensions", "theme", NULL) == 3 && at(s, "browser", "custom_chrome_frame", NULL) == 1, "theme scalar kept: %s", s);
    free(s);

    /* strings with braces, brackets and escaped quotes don't confuse the positions */
    s = merged("{\"a\": \"}{\\\"[\", \"b\": [1, {\"c\": \"]}\"}], \"browser\": {\"x\": \"{\\\\\"}}", 1);
    CHECK(!strcmp(strof(s, "a", NULL), "}{\"["), "string a kept: %s", s);
    CHECK(!strcmp(strof(s, "browser", "x"), "{\\"), "string x kept: %s", s);
    CHECK(at(s, "browser", "custom_chrome_frame", NULL) == 1, "into browser after tricky strings: %s", s);
    free(s);

    /* left alone: not JSON, not an object, duplicate keys, a key written with escapes */
    const char *bad[] = { "", "not json", "[]", "\"x\"", "{\"a\": 1,}", "{\"a\": 1, \"a\": 2}", "{\"br\\u006fwser\": {}}", "{} {}" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) { char *o = NULL; size_t l; CHECK(browser_prefs_merge(bad[i], strlen(bad[i]), &o, &l) == -1, "left alone: %s", bad[i]); free(o); }
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = calloc(1, 1 << 16);
    size_t r = b ? fread(b, 1, (1 << 16) - 1, f) : 0;
    (void)r;
    fclose(f);
    return b;
}

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static void test_files(void) {
    char root[] = "/tmp/lord-browserprefs.XXXXXX", p[512], q[512];
    if (!mkdtemp(root)) { fails++; printf("FAIL mkdtemp\n"); return; }
    CHECK(browser_prefs_dir("google-chrome-stable") && !strcmp(browser_prefs_dir("google-chrome-stable"), "/opt/google/chrome"), "chrome dir");
    CHECK(!browser_prefs_dir("chromium") && !browser_prefs_dir("firefox-esr") && !browser_prefs_dir("") && !browser_prefs_dir(NULL), "not in the list");
    CHECK(browser_prefs_apply_pkg(root, "chromium") == 0 && browser_prefs_apply_pkg(root, "../etc") == 0, "unknown package: nothing");

    /* not installed: nothing created */
    CHECK(browser_prefs_apply_pkg(root, "google-chrome-stable") == 0, "not installed");
    snprintf(p, sizeof p, "%s/opt", root);
    CHECK(access(p, F_OK) != 0, "no directory created");

    /* installed, no file: a new initial_preferences, 0644, the fixed text; then nothing more */
    snprintf(p, sizeof p, "mkdir -p %s/opt/google/chrome %s/opt/brave.com/brave %s/opt/microsoft/msedge", root, root, root);
    CHECK(system(p) == 0, "mkdir");
    CHECK(browser_prefs_apply_pkg(root, "google-chrome-stable") == 1, "chrome written");
    snprintf(p, sizeof p, "%s/opt/google/chrome/initial_preferences", root);
    struct stat st;
    CHECK(!stat(p, &st) && (st.st_mode & 0777) == 0644, "mode 0644");
    char *a = slurp(p);
    CHECK(a && at(a, "browser", "custom_chrome_frame", NULL) == 1 && at(a, "extensions", "theme", "system_theme") == 1, "chrome content");
    CHECK(browser_prefs_apply_pkg(root, "google-chrome-stable") == 0, "second run: nothing");
    char *b = slurp(p);
    CHECK(a && b && !strcmp(a, b), "second run: same text");
    free(a); free(b);

    /* the vendor ships master_preferences only: merged there, no initial_preferences that would hide it */
    snprintf(p, sizeof p, "%s/opt/microsoft/msedge/master_preferences", root);
    put(p, "{\"distribution\": {\"msedge\": 1}}\n");
    CHECK(browser_prefs_apply_pkg(root, "microsoft-edge-stable") == 1, "edge merged");
    a = slurp(p);
    CHECK(a && at(a, "distribution", "msedge", NULL) == 1 && at(a, "extensions", "theme", "system_theme") == 1, "edge content: %s", a ? a : "");
    free(a);
    snprintf(q, sizeof q, "%s/opt/microsoft/msedge/initial_preferences", root);
    CHECK(access(q, F_OK) != 0, "edge: no initial_preferences");

    /* initial_preferences wins over master_preferences (it's the one the browser reads) */
    snprintf(p, sizeof p, "%s/opt/brave.com/brave/master_preferences", root);
    put(p, "{\"m\": 1}");
    snprintf(q, sizeof q, "%s/opt/brave.com/brave/initial_preferences", root);
    put(q, "{\"i\": 1}");
    CHECK(browser_prefs_apply_pkg(root, "brave-browser") == 1, "brave merged");
    a = slurp(p); b = slurp(q);
    CHECK(a && !strcmp(a, "{\"m\": 1}"), "master untouched");
    CHECK(b && at(b, "i", NULL, NULL) == 1 && at(b, "browser", "custom_chrome_frame", NULL) == 1, "initial merged");
    free(a); free(b);

    /* a vendor file that isn't JSON, or a symlink: left alone */
    put(q, "garbage");
    CHECK(browser_prefs_apply_pkg(root, "brave-browser") == -1, "garbage left alone");
    a = slurp(q);
    CHECK(a && !strcmp(a, "garbage"), "garbage untouched");
    free(a);
    unlink(q);
    snprintf(p, sizeof p, "%s/target", root);
    put(p, "{}");
    CHECK(symlink(p, q) == 0, "symlink");
    CHECK(browser_prefs_apply_pkg(root, "brave-browser") == -1, "symlink left alone");
    a = slurp(p);
    CHECK(a && !strcmp(a, "{}"), "symlink target untouched");
    free(a);
    /* the browser's directory itself a symlink: not followed */
    snprintf(p, sizeof p, "rm -rf %s/opt/google/chrome && mkdir -p %s/elsewhere && ln -s %s/elsewhere %s/opt/google/chrome", root, root, root, root);
    CHECK(system(p) == 0, "dir symlink");
    CHECK(browser_prefs_apply_pkg(root, "google-chrome-stable") == -1, "directory symlink not followed");
    snprintf(p, sizeof p, "%s/elsewhere/initial_preferences", root);
    CHECK(access(p, F_OK) != 0, "nothing written through the symlink");

    /* no temporary files left */
    snprintf(p, sizeof p, "find %s -name '.*lordos-tmp' | grep -q .", root);
    CHECK(system(p) != 0, "no temporary files");
    snprintf(p, sizeof p, "rm -rf %s", root);
    if (system(p)) {}
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--browser-prefs")) return browser_prefs_main(argc, argv);
    test_merge();
    test_files();
    printf("browserprefs-test: %d checks, %d failed\n", n, fails);
    return fails != 0;
}
