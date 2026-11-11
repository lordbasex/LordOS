/* webreg_test.c — web apps without WebKit (docs/WEBAPPS.md): the registry's rules (ids, names, URLs and their
 * origin, unknown keys, permissions, profiles, the file's size), writing and reading back, profile ids, the probe's
 * answers made safe, the icon check; and lord-server's side (src/web/webmgr.c) in a throwaway home against a
 * stand-in lord-web (a shell script): the probe protocol on fd 3, installing from a probe and from the catalog,
 * profiles, permissions, uninstalling with the data, and starting it (argv, environment, one process per app and
 * profile, whose process it is); and LordOS's own catalog (assets/webapps/catalog.json): every entry installs, with
 * its icon.
 *
 *   make -C server build/webreg-test        (also in `make test`) */
#include "../src/web/webreg.h"
#include "../src/web/webmgr.h"
#include "../src/sec/json.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char SAMPLE[] =
    "{\n"
    "  \"schema\": 1,\n"
    "  \"id\": \"chat\",\n"
    "  \"name\": \"Chat\",\n"
    "  \"startUrl\": \"https://web.chat.example/\",\n"
    "  \"scope\": \"https://web.chat.example/\",\n"
    "  \"origin\": \"https://web.chat.example\",\n"
    "  \"icon\": \"icons/chat.png\",\n"
    "  \"themeColor\": \"#25d366\",\n"
    "  \"source\": \"catalog\",\n"
    "  \"background\": true,\n"
    "  \"profiles\": [ { \"id\": \"personal\", \"name\": \"Personal\" }, { \"id\": \"work\", \"name\": \"Work\" } ],\n"
    "  \"permissions\": { \"notifications\": \"ask\", \"camera\": \"ask\", \"microphone\": \"ask\", \"screen\": \"ask\", \"location\": \"deny\" }\n"
    "}\n";

/* SAMPLE with `from` replaced by `to` (once), then parsed */
static int parses_with(const char *from, const char *to) {
    char buf[8192];
    const char *at = strstr(SAMPLE, from);
    if (!at) { printf("FAIL: %s not in the sample\n", from); fails++; return -1; }
    snprintf(buf, sizeof buf, "%.*s%s%s", (int)(at - SAMPLE), SAMPLE, to, at + strlen(from));
    WebApp a;
    return webreg_parse(buf, strlen(buf), &a, NULL, 0);
}

static int origin_is(const char *url, const char *want) {
    char o[WEB_URL_MAX + 1];
    if (webreg_url_origin(url, o, sizeof o)) return want == NULL;
    return want && !strcmp(o, want);
}

static void test_rules(void) {
    CHECK(webreg_id_ok("chat") && webreg_id_ok("0chat") && webreg_id_ok("a-b-c"), "ids");
    CHECK(!webreg_id_ok("") && !webreg_id_ok("-a") && !webreg_id_ok("Chat") && !webreg_id_ok("a_b") && !webreg_id_ok("a.b") && !webreg_id_ok("../x"), "bad ids");
    CHECK(webreg_id_ok("a234567890123456789012345678901234567890") && !webreg_id_ok("a2345678901234567890123456789012345678901"), "40 characters at most");

    CHECK(webreg_name_ok("Chat") && webreg_name_ok("Mi trabajo") && webreg_name_ok("Caf\xc3\xa9") && webreg_name_ok("\xe6\x97\xa5\xe6\x9c\xac"), "names");
    CHECK(!webreg_name_ok("") && !webreg_name_ok(" Chat") && !webreg_name_ok("Chat ") && !webreg_name_ok("Ch\tat"), "empty, padded or a control character");
    CHECK(!webreg_name_ok("Chat\xe2\x80\xae" "gpj.exe"), "a bidi override");
    CHECK(!webreg_name_ok("Ch\xe2\x80\x8b" "at"), "a zero-width space");
    CHECK(!webreg_name_ok("\xc0\xaf") && !webreg_name_ok("\xed\xa0\x80") && !webreg_name_ok("ab\xc3"), "overlong, surrogate and cut UTF-8");
    CHECK(!webreg_name_ok("12345678901234567890123456789012345678901234567890123456789012345"), "65 bytes");

    CHECK(origin_is("https://web.chat.example/", "https://web.chat.example"), "an origin");
    CHECK(origin_is("https://a.example:8443/x?y#z", "https://a.example:8443"), "with its port");
    CHECK(origin_is("https://a.example:443/", "https://a.example"), "the default port is not part of it");
    CHECK(origin_is("https://a.example", "https://a.example") && origin_is("https://a.example?q", "https://a.example"), "no path");
    CHECK(origin_is("http://localhost:3000/", "http://localhost:3000") && origin_is("http://127.0.0.1/", "http://127.0.0.1") &&
          origin_is("http://[::1]:8080/", "http://[::1]:8080"), "http: on this computer");
    CHECK(origin_is("http://a.example/", NULL) && origin_is("http://localhost.a.example/", NULL), "http: anywhere else");
    CHECK(origin_is("https://user@a.example/", NULL) && origin_is("https://u:p@a.example/", NULL), "a user or password");
    CHECK(origin_is("ftp://a.example/", NULL) && origin_is("javascript:alert(1)", NULL) && origin_is("file:///etc/passwd", NULL), "other schemes");
    CHECK(origin_is("https://A.example/", NULL) && origin_is("https://a.example/a b", NULL) && origin_is("https://a.ex\xc3\xa1mple/", NULL), "capitals, spaces, not punycode");
    CHECK(origin_is("https:///x", NULL) && origin_is("https://a..b/", NULL) && origin_is("https://-a.b/", NULL) && origin_is("https://a.b:0/", NULL) &&
          origin_is("https://a.b:99999/", NULL) && origin_is("https://a.b:/", NULL) && origin_is("https://a\\b/", NULL), "broken hosts and ports");

    CHECK(webreg_urls_ok("https://a.example/app/start", "https://a.example/app/", "https://a.example"), "scope over start");
    CHECK(!webreg_urls_ok("https://a.example/other", "https://a.example/app/", "https://a.example"), "start outside its scope");
    CHECK(!webreg_urls_ok("https://a.example/", "https://a.example/", "https://b.example"), "another origin");
    CHECK(!webreg_urls_ok("https://a.example.evil/", "https://a.example", "https://a.example"), "a scope with no path is no prefix");
    CHECK(!webreg_urls_ok("https://a.example:443/", "https://a.example:443/", "https://a.example"), "a scope with the default port written");
    char sc[WEB_URL_MAX + 1];
    CHECK(!webreg_default_scope("https://a.example/x/y?q=/z", sc, sizeof sc) && !strcmp(sc, "https://a.example/x/"), "default scope: %s", sc);
    CHECK(!webreg_default_scope("https://a.example", sc, sizeof sc) && !strcmp(sc, "https://a.example/"), "default scope of an origin: %s", sc);

    CHECK(webreg_perm_kind("camera") == WEB_PERM_CAMERA && webreg_perm_kind("contacts") < 0, "permission kinds");
    CHECK(webreg_perm_value("ask") == WEB_ASK && webreg_perm_value("once") < 0 && webreg_perm_value("Allow") < 0, "permission values");
}

static void test_file(void) {
    WebApp a;
    char why[96] = "";
    CHECK(!webreg_parse(SAMPLE, strlen(SAMPLE), &a, why, sizeof why), "the sample: %s", why);
    CHECK(!strcmp(a.id, "chat") && a.nprofiles == 2 && !strcmp(a.profiles[1].id, "work") && a.background && a.source == WEB_SRC_CATALOG &&
          a.perms[WEB_PERM_LOCATION] == WEB_DENY && a.perms[WEB_PERM_CAMERA] == WEB_ASK, "its fields");
    char out[WEB_FILE_MAX + 1];
    size_t n = webreg_write(&a, out, sizeof out);
    CHECK(n > 0 && !strcmp(out, SAMPLE), "written back the same:\n%s", out);
    WebApp b;
    CHECK(!webreg_parse(out, n, &b, NULL, 0) && !memcmp(&a, &b, sizeof a), "and read again");

    CHECK(parses_with("\"background\": true,", "\"background\": true, \"exec\": \"/bin/sh\",") < 0, "an unknown key");
    CHECK(parses_with("\"schema\": 1", "\"schema\": 2") < 0, "another schema");
    CHECK(parses_with("  \"origin\": \"https://web.chat.example\",\n", "") < 0, "a missing key");
    CHECK(parses_with("\"origin\": \"https://web.chat.example\"", "\"origin\": \"https://evil.example\"") < 0, "an origin that is not the URLs'");
    CHECK(parses_with("\"scope\": \"https://web.chat.example/\"", "\"scope\": \"https://web.chat.example/inbox/\"") < 0, "start outside the scope");
    CHECK(parses_with("\"startUrl\": \"https://web.chat.example/\"", "\"startUrl\": \"http://web.chat.example/\"") < 0, "http:");
    CHECK(parses_with("\"location\": \"deny\"", "\"location\": \"maybe\"") < 0, "a permission value");
    CHECK(parses_with("\"location\": \"deny\"", "\"location\": \"deny\", \"contacts\": \"allow\"") < 0, "an unknown permission");
    CHECK(parses_with("{ \"id\": \"work\", \"name\": \"Work\" }", "{ \"id\": \"personal\", \"name\": \"Work\" }") < 0, "a profile id twice");
    CHECK(parses_with("{ \"id\": \"work\", \"name\": \"Work\" }", "{ \"id\": \"work\", \"name\": \"Work\", \"dir\": \"/\" }") < 0, "a profile's unknown key");
    CHECK(parses_with("{ \"id\": \"work\", \"name\": \"Work\" }", "{ \"id\": \"../x\", \"name\": \"Work\" }") < 0, "a profile id with a path");
    CHECK(parses_with("\"icon\": \"icons/chat.png\"", "\"icon\": \"../../.bashrc\"") < 0, "an icon elsewhere");
    CHECK(parses_with("\"icon\": \"icons/chat.png\"", "\"icon\": \"icons/other.png\"") < 0, "another app's icon");
    CHECK(parses_with("\"icon\": \"icons/chat.png\"", "\"icon\": \"icons/chat.svg\"") == 0, "an SVG icon");
    CHECK(parses_with("\"themeColor\": \"#25d366\"", "\"themeColor\": \"red\"") < 0, "a color");
    CHECK(parses_with("\"source\": \"catalog\"", "\"source\": \"store\"") < 0, "a source");
    CHECK(parses_with("\"background\": true", "\"background\": 1") < 0, "background is a boolean");
    CHECK(parses_with("\"name\": \"Chat\"", "\"name\": \"\"") < 0, "an empty name");
    CHECK(parses_with("\"profiles\": [ { \"id\": \"personal\", \"name\": \"Personal\" }, { \"id\": \"work\", \"name\": \"Work\" } ]", "\"profiles\": []") < 0, "no profiles");
    CHECK(parses_with("\"id\": \"chat\"", "\"id\": \"chat\", \"id\": \"chat\"") < 0, "a key twice");
    char many[4096] = "\"profiles\": [";
    for (int i = 0; i < 17; i++) snprintf(many + strlen(many), sizeof many - strlen(many), "%s{ \"id\": \"p%d\", \"name\": \"P%d\" }", i ? ", " : " ", i, i);
    strcat(many, " ]");
    CHECK(parses_with("\"profiles\": [ { \"id\": \"personal\", \"name\": \"Personal\" }, { \"id\": \"work\", \"name\": \"Work\" } ]", many) < 0, "17 profiles");
    char *big = malloc(WEB_FILE_MAX + 2);
    memset(big, ' ', WEB_FILE_MAX + 1);
    memcpy(big, SAMPLE, strlen(SAMPLE));
    CHECK(webreg_parse(big, WEB_FILE_MAX + 1, &a, NULL, 0) < 0, "more than 64 KB");
    free(big);

    /* a struct that breaks a rule is never written */
    webreg_parse(SAMPLE, strlen(SAMPLE), &a, NULL, 0);
    snprintf(a.start_url, sizeof a.start_url, "https://evil.example/");
    CHECK(webreg_write(&a, out, sizeof out) == 0, "a bad struct is not written");
}

static void test_profiles(void) {
    WebApp a;
    webreg_parse(SAMPLE, strlen(SAMPLE), &a, NULL, 0);
    char id[WEB_ID_MAX + 1];
    CHECK(!webreg_profile_new_id(&a, "Work", id, sizeof id) && !strcmp(id, "work-2"), "a name taken: %s", id);
    CHECK(!webreg_profile_new_id(&a, "  Mi  Trabajo! ", id, sizeof id) && !strcmp(id, "mi-trabajo"), "spaces and signs: %s", id);
    CHECK(!webreg_profile_new_id(&a, "\xe6\x97\xa5\xe6\x9c\xac", id, sizeof id) && !strcmp(id, "profile"), "no ASCII letters: %s", id);
    CHECK(!webreg_profile_new_id(&a, "A very long profile name that goes on and on and on", id, sizeof id) && webreg_id_ok(id), "long: %s", id);
    a.nprofiles = WEB_PROFILES_MAX;
    CHECK(webreg_profile_new_id(&a, "More", id, sizeof id) < 0, "full");
    CHECK(!webreg_slug("Notes Online", id, sizeof id, NULL, NULL) && !strcmp(id, "notes-online"), "an app id: %s", id);
}

static void test_probe(void) {
    WebProbe p;
    const char *l1 = "{\"ok\":true,\"name\":\"Chat\",\"startUrl\":\"https://web.chat.example/\",\"scope\":\"https://web.chat.example/\","
                     "\"origin\":\"https://web.chat.example\",\"themeColor\":\"#25d366\",\"icons\":[{\"src\":\"/i.png\",\"sizes\":\"192x192\",\"type\":\"image/png\"}],\"manifest\":true}";
    CHECK(!webreg_probe_parse(l1, strlen(l1), &p) && p.ok && p.manifest && !strcmp(p.name, "Chat") && !strcmp(p.origin, "https://web.chat.example") &&
          !strcmp(p.theme_color, "#25d366"), "a probe's answer");
    const char *l2 = "{\"ok\":true,\"name\":\"\\u202eChat\\n\\tWeb  \",\"startUrl\":\"https://b.example/app/x\",\"scope\":\"https://evil.example/\",\"origin\":\"https://evil.example\"}";
    CHECK(!webreg_probe_parse(l2, strlen(l2), &p) && !strcmp(p.name, "Chat Web") && !strcmp(p.origin, "https://b.example") && !strcmp(p.scope, "https://b.example/app/"),
          "made safe: '%s' %s %s", p.name, p.origin, p.scope);
    const char *l3 = "{\"ok\":true,\"name\":\"\\u200b\",\"startUrl\":\"https://b.example/\"}";
    CHECK(!webreg_probe_parse(l3, strlen(l3), &p) && !strcmp(p.name, "b.example") && !p.manifest, "no name: the host (%s)", p.name);
    CHECK(webreg_probe_parse("{\"ok\":false}", 12, &p) < 0, "not ok");
    const char *l4 = "{\"ok\":true,\"startUrl\":\"http://b.example/\"}";
    CHECK(webreg_probe_parse(l4, strlen(l4), &p) < 0, "an http: page");
    CHECK(webreg_probe_parse("not json", 8, &p) < 0, "not JSON");

    size_t size = 0;
    int svg = -1;
    const char *h1 = "{\"icon\":{\"type\":\"image/png\",\"size\":33}}";
    CHECK(webreg_icon_header(h1, strlen(h1), &size, &svg) == 1 && size == 33 && svg == 0, "an icon header");
    CHECK(webreg_icon_header("{\"icon\":null}", 13, &size, &svg) == 0, "no icon");
    const char *h2 = "{\"icon\":{\"type\":\"image/png\",\"size\":2000000}}";
    CHECK(webreg_icon_header(h2, strlen(h2), &size, &svg) < 0, "an icon too big");
    const char *h3 = "{\"icon\":{\"type\":\"text/html\",\"size\":10}}";
    CHECK(webreg_icon_header(h3, strlen(h3), &size, &svg) < 0, "not an image");

    static const unsigned char png[33] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 16, 0, 0, 0, 16, 8, 6, 0, 0, 0, 0x1f, 0xf3, 0xff, 0x61 };
    CHECK(webreg_icon_kind(png, sizeof png) == 1, "a PNG");
    unsigned char bad[33];
    memcpy(bad, png, 33);
    bad[19] = 0; bad[18] = 0; bad[17] = 0; bad[16] = 0;
    CHECK(webreg_icon_kind(bad, sizeof bad) == 0, "a PNG of width 0");
    const char *s1 = "<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1 1\"/>";
    CHECK(webreg_icon_kind((const unsigned char *)s1, strlen(s1)) == 2, "an SVG");
    const char *s2 = "<svg><script>alert(1)</script></svg>";
    CHECK(webreg_icon_kind((const unsigned char *)s2, strlen(s2)) == 0, "an SVG with a script");
    const char *s3 = "<html><body>hi</body></html>";
    CHECK(webreg_icon_kind((const unsigned char *)s3, strlen(s3)) == 0, "a page");
}

/* ---- lord-server's side, against a stand-in lord-web ---- */

static char root[512];

static void write_text(const char *path, const char *text, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text)) printf("FAIL: could not write %s\n", path);
    if (fd >= 0) close(fd);
    chmod(path, mode);
}

static char *read_text(const char *path) {
    size_t n;
    return webreg_read_file(path, 1 << 20, &n);
}

static const char FAKE[] =
    "#!/bin/sh\n"
    "# a stand-in lord-web: the probe protocol, or a process that stays\n"
    "if [ \"$1\" = --probe ]; then\n"
    "  read -r url <&3\n"
    "  echo \"$url\" > \"$HOME/probed\"\n"
    "  printf '{\"ok\":true,\"name\":\"Notes Online\",\"startUrl\":\"https://notes.example/app/\",\"scope\":\"https://notes.example/app/\","
    "\"origin\":\"https://notes.example\",\"manifest\":true}\\n' >&3\n"
    "  printf '{\"icon\":{\"type\":\"image/png\",\"size\":33}}\\n' >&3\n"
    "  printf '\\211PNG\\r\\n\\032\\n\\000\\000\\000\\rIHDR\\000\\000\\000\\020\\000\\000\\000\\020\\010\\006\\000\\000\\000\\037\\363\\377a' >&3\n"
    "  exit 0\n"
    "fi\n"
    "echo \"$@\" > \"$HOME/args-$2-$4\"\n"
    "/usr/bin/env > \"$HOME/env-$2-$4\"\n"
    "exec /bin/sleep 30\n";

static char *wait_probe(int id) {
    for (int i = 0; i < 500; i++) {
        int gone = 0;
        char *j = webmgr_probe_result(id, &gone);
        if (j || gone) return j;
        struct timespec ts = { 0, 20 * 1000000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void test_manager(void) {
    char tmpl[] = "/tmp/webreg-test.XXXXXX";
    if (!mkdtemp(tmpl)) { printf("FAIL: mkdtemp\n"); fails++; return; }
    snprintf(root, sizeof root, "%s", tmpl);
    char p[1024], q[4096];
    setenv("HOME", root, 1);
    snprintf(p, sizeof p, "%s/run", root);
    mkdir(p, 0700);
    setenv("XDG_RUNTIME_DIR", p, 1);
    setenv("WAYLAND_DISPLAY", "wayland-test", 1);
    setenv("LANG", "C.UTF-8", 1);
    setenv("SECRET_TOKEN", "must-not-pass", 1);
    snprintf(p, sizeof p, "%s/lord-web", root);
    write_text(p, FAKE, 0755);
    webmgr_set_helper(p);
    /* the catalog, as LordOS installs it */
    snprintf(p, sizeof p, "%s/assets/webapps/icons", root);
    snprintf(q, sizeof q, "mkdir -p '%s'", p);
    CHECK(system(q) == 0, "assets folder");
    snprintf(p, sizeof p, "%s/assets/webapps/catalog.json", root);
    write_text(p, "{\"schema\":1,\"apps\":[{\"id\":\"chat\",\"name\":\"Chat\",\"startUrl\":\"https://web.chat.example/\",\"scope\":\"https://web.chat.example/\","
                  "\"origin\":\"https://web.chat.example\",\"icon\":\"chat.png\",\"themeColor\":\"#25d366\",\"background\":true}]}", 0644);
    snprintf(p, sizeof p, "%s/assets/webapps/icons/chat.png", root);
    write_text(p, "not a png", 0644);
    snprintf(p, sizeof p, "%s/assets", root);
    webmgr_init(p);
    CHECK(webmgr_available(), "the stand-in is usable");

    /* the probe */
    const char *err = NULL;
    CHECK(webmgr_probe("http://notes.example/", &err) == 0 && err && !strcmp(err, "webapps.badAddress"), "a probe of an http: address");
    CHECK(webmgr_probe("https://user@notes.example/", &err) == 0, "a probe of an address with a user");
    int id = webmgr_probe("https://notes.example/", &err);
    CHECK(id > 0, "a probe: %s", err ? err : "");
    char *j = wait_probe(id);
    CHECK(j && strstr(j, "\"ok\":true") && strstr(j, "\"name\":\"Notes Online\"") && strstr(j, "\"origin\":\"https://notes.example\"") &&
          strstr(j, "\"iconKind\":\"png\"") && strstr(j, "\"iconPath\":\"/"), "its answer: %s", j ? j : "(none)");
    free(j);
    snprintf(p, sizeof p, "%s/probed", root);
    char *probed = read_text(p);
    CHECK(probed && !strcmp(probed, "https://notes.example/\n"), "the address went on fd 3: %s", probed ? probed : "");
    free(probed);

    /* installing from it */
    char app[WEB_ID_MAX + 1] = "";
    CHECK(webmgr_install("{\"source\":\"url\",\"probe\":999,\"profile\":\"Personal\"}", 49, app, sizeof app) != NULL, "an unknown probe");
    char entry[256];
    snprintf(entry, sizeof entry, "{\"source\":\"url\",\"probe\":%d,\"profile\":\"Personal\",\"startUrl\":\"https://evil.example/\"}", id);
    CHECK(webmgr_install(entry, strlen(entry), app, sizeof app) != NULL, "an entry that names a URL");
    snprintf(entry, sizeof entry, "{\"source\":\"url\",\"probe\":%d,\"profile\":\"Personal\"}", id);
    err = webmgr_install(entry, strlen(entry), app, sizeof app);
    CHECK(!err && !strcmp(app, "notes-online"), "installed from the probe: %s %s", err ? err : "", app);
    WebApp a;
    CHECK(!webmgr_load("notes-online", &a) && !strcmp(a.start_url, "https://notes.example/app/") && a.source == WEB_SRC_URL &&
          !strcmp(a.icon, "icons/notes-online.png") && a.nprofiles == 1 && !strcmp(a.profiles[0].id, "personal"), "its registry file");
    snprintf(p, sizeof p, "%s/.config/lordos/web/apps/notes-online.json", root);
    struct stat st;
    CHECK(!stat(p, &st) && (st.st_mode & 0777) == 0600, "written 0600");
    CHECK(webmgr_install(entry, strlen(entry), app, sizeof app) != NULL, "a probe is used once");

    /* the catalog: its data, never JS's; a broken icon only means no icon */
    CHECK(webmgr_install("{\"source\":\"catalog\",\"id\":\"nope\",\"profile\":\"Personal\"}", 51, app, sizeof app) != NULL, "not in the catalog");
    const char *cat = "{\"source\":\"catalog\",\"id\":\"chat\",\"name\":\"Other\",\"profile\":\"Personal\"}";
    CHECK(webmgr_install(cat, strlen(cat), app, sizeof app) != NULL, "a catalog entry with a name from JS");
    cat = "{\"source\":\"catalog\",\"id\":\"chat\",\"profile\":\"Personal\"}";
    err = webmgr_install(cat, strlen(cat), app, sizeof app);
    CHECK(!err && !strcmp(app, "chat"), "installed from the catalog: %s", err ? err : "");
    CHECK(!webmgr_load("chat", &a) && a.background && a.source == WEB_SRC_CATALOG && !a.icon[0], "its file, without the broken icon");
    CHECK(webmgr_install(cat, strlen(cat), app, sizeof app) != NULL, "already installed");

    /* profiles and permissions */
    char prof[WEB_ID_MAX + 1];
    CHECK(!webmgr_add_profile("chat", "Work", prof, sizeof prof) && !strcmp(prof, "work"), "a profile added");
    CHECK(webmgr_add_profile("chat", "Bad\xe2\x80\xae", prof, sizeof prof) != NULL, "a profile name with a bidi override");
    CHECK(!webmgr_rename_profile("chat", "work", "Office"), "renamed");
    CHECK(webmgr_rename_profile("chat", "nope", "X") != NULL, "renaming a missing profile");
    CHECK(!webmgr_set_permission("chat", "camera", "allow") && webmgr_set_permission("chat", "camera", "once") && webmgr_set_permission("chat", "files", "allow"),
          "permissions");
    CHECK(!webmgr_set_background("chat", 0), "background off");
    CHECK(!webmgr_load("chat", &a) && a.nprofiles == 2 && !strcmp(a.profiles[1].name, "Office") && a.perms[WEB_PERM_CAMERA] == WEB_ALLOW && !a.background, "all kept");

    /* starting it: argv and environment exactly, one process per app and profile */
    int raised = -1;
    CHECK(webmgr_launch("chat", "nope", &raised) != NULL, "a profile that is not in the registry");
    CHECK(webmgr_launch("chat", "../x", &raised) != NULL, "a profile id with a path");
    CHECK(!webmgr_launch("chat", "work", &raised) && raised == 0, "started");
    CHECK(!webmgr_launch("chat", "work", &raised) && raised == 1, "the second launch only raises it");
    WebRunning r[WEB_MAX_RUNNING];
    int n = webmgr_running(r, WEB_MAX_RUNNING);
    CHECK(n == 1 && !strcmp(r[0].app, "chat") && !strcmp(r[0].profile, "work"), "running: %d", n);
    char oa[WEB_ID_MAX + 1], op[WEB_ID_MAX + 1];
    CHECK(n == 1 && !webmgr_owner(r[0].pid, oa, op) && !strcmp(oa, "chat") && !strcmp(op, "work"), "its process");
    CHECK(webmgr_owner(getpid(), oa, op) < 0, "not one of ours");
    snprintf(p, sizeof p, "%s/args-chat-work", root);
    char *args = NULL;
    for (int i = 0; i < 200 && !(args = read_text(p)); i++) { struct timespec ts = { 0, 10 * 1000000 }; nanosleep(&ts, NULL); }
    CHECK(args && !strcmp(args, "--app chat --profile work\n"), "argv: %s", args ? args : "(none)");
    free(args);
    snprintf(p, sizeof p, "%s/env-chat-work", root);
    char *env = NULL;
    for (int i = 0; i < 200 && !(env = read_text(p)); i++) { struct timespec ts = { 0, 10 * 1000000 }; nanosleep(&ts, NULL); }
    CHECK(env && strstr(env, "LORDOS_WEB=1") && strstr(env, "WAYLAND_DISPLAY=wayland-test") && strstr(env, "LANG=C.UTF-8") && !strstr(env, "SECRET_TOKEN"),
          "environment: %s", env ? env : "(none)");
    free(env);
    snprintf(p, sizeof p, "%s/.local/share/lordos/web/chat/work", root);
    CHECK(!stat(p, &st) && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0700, "its data folder, 0700");
    snprintf(q, sizeof q, "%s/cookies.sqlite", p);
    write_text(q, "x", 0600);

    /* removing a profile ends its process and, when asked, its data */
    CHECK(webmgr_remove_profile("chat", "work", 1) == NULL, "the profile removed");
    CHECK(webmgr_running(r, WEB_MAX_RUNNING) == 0, "its process ended");
    CHECK(stat(p, &st) != 0, "its data gone");
    CHECK(webmgr_remove_profile("chat", "personal", 0) != NULL, "the last profile stays");

    /* uninstalling */
    snprintf(p, sizeof p, "%s/.local/share/lordos/web/notes-online/personal", root);
    snprintf(q, sizeof q, "mkdir -p '%s' && touch '%s/data.db' && ln -s /etc '%s/link'", p, p, p);
    CHECK(system(q) == 0, "a profile with data");
    CHECK(!webmgr_uninstall("notes-online", 1), "uninstalled with its data");
    CHECK(webmgr_load("notes-online", &a) != NULL, "no registry file");
    snprintf(p, sizeof p, "%s/.local/share/lordos/web/notes-online", root);
    CHECK(stat(p, &st) != 0 && !stat("/etc/hostname", &st), "its data gone, nothing through the link");
    snprintf(p, sizeof p, "%s/.config/lordos/web/icons/notes-online.png", root);
    CHECK(stat(p, &st) != 0, "its icon gone");
    CHECK(!webmgr_uninstall("chat", 0) && webmgr_uninstall("chat", 0) != NULL, "uninstalled once");

    snprintf(q, sizeof q, "rm -rf '%s'", root);
    if (system(q)) printf("could not remove %s\n", root);
}

/* LordOS's catalog as shipped (../assets from server/): every entry passes the registry's rules and installs with its
 * icon (a real PNG of at most 1 MB), so the Store never offers an app lord-server would refuse. */
static void test_repo_catalog(void) {
    char tmpl[] = "/tmp/webreg-catalog.XXXXXX", q[4096];
    if (!mkdtemp(tmpl)) { printf("FAIL: mkdtemp\n"); fails++; return; }
    setenv("HOME", tmpl, 1);
    webmgr_init("../assets");
    char *t = read_text("../assets/webapps/catalog.json");
    JDoc d;
    int parsed = t && !json_parse(t, strlen(t), &d, NULL, 0);
    CHECK(parsed, "the catalog is JSON");
    int n = 0;
    const JVal *apps = parsed && d.root->type == J_OBJ ? json_get(d.root, "apps") : NULL;
    for (const JVal *e = apps && apps->type == J_ARR ? apps->kid : NULL; e; e = e->next, n++) {
        const char *id = json_str(e, "id");
        char entry[256], app[WEB_ID_MAX + 1] = "";
        snprintf(entry, sizeof entry, "{\"source\":\"catalog\",\"id\":\"%s\",\"profile\":\"Personal\"}", id ? id : "");
        const char *err = webmgr_install(entry, strlen(entry), app, sizeof app);
        WebApp a;
        CHECK(id && !err && !strcmp(app, id), "catalog entry %s installs: %s", id ? id : "(no id)", err ? err : "");
        CHECK(id && !webmgr_load(id, &a) && a.source == WEB_SRC_CATALOG && strstr(a.icon, ".png"), "catalog entry %s has its icon", id ? id : "");
    }
    CHECK(n >= 4, "the catalog has its apps: %d", n);
    if (parsed) json_free(&d);
    free(t);
    snprintf(q, sizeof q, "rm -rf '%s'", tmpl);
    if (system(q)) printf("could not remove %s\n", tmpl);
}

int main(void) {
    test_rules();
    test_file();
    test_profiles();
    test_probe();
    test_manager();
    test_repo_catalog();
    printf("webreg-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
