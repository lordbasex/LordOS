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
    char o[WEBREG_URL_MAX + 1];
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
    char sc[WEBREG_URL_MAX + 1];
    CHECK(!webreg_default_scope("https://a.example/x/y?q=/z", sc, sizeof sc) && !strcmp(sc, "https://a.example/x/"), "default scope: %s", sc);
    CHECK(!webreg_default_scope("https://a.example", sc, sizeof sc) && !strcmp(sc, "https://a.example/"), "default scope of an origin: %s", sc);

    CHECK(webreg_perm_kind("camera") == WEBREG_PERM_CAMERA && webreg_perm_kind("contacts") < 0, "permission kinds");
    CHECK(webreg_perm_value("ask") == WEBREG_ASK && webreg_perm_value("once") < 0 && webreg_perm_value("Allow") < 0, "permission values");
}

static void test_file(void) {
    WebApp a;
    char why[96] = "";
    CHECK(!webreg_parse(SAMPLE, strlen(SAMPLE), &a, why, sizeof why), "the sample: %s", why);
    CHECK(!strcmp(a.id, "chat") && a.nprofiles == 2 && !strcmp(a.profiles[1].id, "work") && a.background && a.source == WEBREG_SRC_CATALOG &&
          a.perms[WEBREG_PERM_LOCATION] == WEBREG_DENY && a.perms[WEBREG_PERM_CAMERA] == WEBREG_ASK, "its fields");
    char out[WEBREG_FILE_MAX + 1];
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
    char *big = malloc(WEBREG_FILE_MAX + 2);
    memset(big, ' ', WEBREG_FILE_MAX + 1);
    memcpy(big, SAMPLE, strlen(SAMPLE));
    CHECK(webreg_parse(big, WEBREG_FILE_MAX + 1, &a, NULL, 0) < 0, "more than 64 KB");
    free(big);

    /* a struct that breaks a rule is never written */
    webreg_parse(SAMPLE, strlen(SAMPLE), &a, NULL, 0);
    snprintf(a.start_url, sizeof a.start_url, "https://evil.example/");
    CHECK(webreg_write(&a, out, sizeof out) == 0, "a bad struct is not written");
}

static void test_profiles(void) {
    WebApp a;
    webreg_parse(SAMPLE, strlen(SAMPLE), &a, NULL, 0);
    char id[WEBREG_ID_MAX + 1];
    CHECK(!webreg_profile_new_id(&a, "Work", id, sizeof id) && !strcmp(id, "work-2"), "a name taken: %s", id);
    CHECK(!webreg_profile_new_id(&a, "  Mi  Trabajo! ", id, sizeof id) && !strcmp(id, "mi-trabajo"), "spaces and signs: %s", id);
    CHECK(!webreg_profile_new_id(&a, "\xe6\x97\xa5\xe6\x9c\xac", id, sizeof id) && !strcmp(id, "profile"), "no ASCII letters: %s", id);
    CHECK(!webreg_profile_new_id(&a, "A very long profile name that goes on and on and on", id, sizeof id) && webreg_id_ok(id), "long: %s", id);
    a.nprofiles = WEBREG_PROFILES_MAX;
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
    CHECK(webreg_probe_parse("{\"ok\":false}", 12, &p) < 0 && !p.error[0], "not ok");
    const char *codes[] = { "web.probe.request", "web.probe.load", "web.probe.tls", "web.probe.notHttps", "web.probe.page", "web.probe.timeout",
                            "web.probe.iconType", "web.probe.iconTooBig" };
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        char l[96];
        snprintf(l, sizeof l, "{\"ok\":false,\"error\":\"%s\"}", codes[i]);
        CHECK(webreg_probe_parse(l, strlen(l), &p) < 0 && !strcmp(p.error, codes[i]) && webreg_probe_code_ok(codes[i]), "the runtime's code %s", codes[i]);
    }
    CHECK(webreg_probe_parse("{\"ok\":false,\"error\":\"web.probe.other\"}", 39, &p) < 0 && !p.error[0] && !webreg_probe_code_ok("webapps.x"), "an unknown code");
    const char *li = "{\"ok\":true,\"name\":\"C\",\"startUrl\":\"https://c.example/\",\"icons\":[{\"src\":\"https://c.example/a.svg\",\"type\":\"image/svg+xml\"},"
                     "{\"src\":\"https://c.example/48.png\",\"sizes\":\"48x48\",\"type\":\"image/png\"},{\"src\":\"https://cdn.example/512.png\",\"sizes\":\"512x512\",\"type\":\"image/png\"},"
                     "{\"src\":\"https://c.example/1024.png\",\"sizes\":\"1024x1024\",\"type\":\"image/png\"},{\"src\":\"http://c.example/256.png\",\"sizes\":\"256x256\",\"type\":\"image/png\"}]}";
    CHECK(!webreg_probe_parse(li, strlen(li), &p) && !strcmp(p.icon_url, "https://cdn.example/512.png"), "the icon: the largest PNG up to 512, https only (%s)", p.icon_url);
    const char *lb = "{\"ok\":true,\"startUrl\":\"https://c.example/\",\"icons\":[{\"src\":\"https://c.example/2048.png\",\"sizes\":\"2048x2048\"},"
                     "{\"src\":\"https://c.example/1024.png\",\"sizes\":\"1024x1024\"},{\"src\":\"https://c.example/f.ico\"}]}";
    CHECK(!webreg_probe_parse(lb, strlen(lb), &p) && !strcmp(p.icon_url, "https://c.example/1024.png"), "only big ones: the smallest (%s)", p.icon_url);
    const char *ls = "{\"ok\":true,\"startUrl\":\"https://c.example/\",\"icons\":[{\"src\":\"https://c.example/f.ico\"},{\"src\":\"https://c.example/i.svg\"}]}";
    CHECK(!webreg_probe_parse(ls, strlen(ls), &p) && !strcmp(p.icon_url, "https://c.example/i.svg"), "no PNG: the SVG (%s)", p.icon_url);
    const char *ln = "{\"ok\":true,\"startUrl\":\"https://c.example/\",\"icons\":[{\"src\":\"https://c.example/f.ico\"}]}";
    CHECK(!webreg_probe_parse(ln, strlen(ln), &p) && !p.icon_url[0], "no usable icon");
    const char *l4 = "{\"ok\":true,\"startUrl\":\"http://b.example/\"}";
    CHECK(webreg_probe_parse(l4, strlen(l4), &p) < 0, "an http: page");
    CHECK(webreg_probe_parse("not json", 8, &p) < 0, "not JSON");

    size_t size = 0;
    int svg = -1;
    char code[48];
    const char *h1 = "{\"ok\":true,\"type\":\"image/png\",\"size\":33}";
    CHECK(webreg_icon_header(h1, strlen(h1), &size, &svg, code, sizeof code) == 1 && size == 33 && svg == 0, "an icon header (lord-web --probe-icon)");
    const char *h0 = "{\"ok\":true,\"type\":\"image/svg+xml\",\"size\":90}";
    CHECK(webreg_icon_header(h0, strlen(h0), &size, &svg, code, sizeof code) == 1 && size == 90 && svg == 1, "an SVG header");
    const char *he = "{\"ok\":false,\"error\":\"web.probe.iconTooBig\"}";
    CHECK(webreg_icon_header(he, strlen(he), &size, &svg, code, sizeof code) == 0 && !strcmp(code, "web.probe.iconTooBig"), "the runtime refused: %s", code);
    const char *hx = "{\"ok\":false,\"error\":\"<b>x</b>\"}";
    CHECK(webreg_icon_header(hx, strlen(hx), &size, &svg, code, sizeof code) == 0 && !code[0], "an unknown code is not passed on");
    const char *h2 = "{\"ok\":true,\"type\":\"image/png\",\"size\":2000000}";
    CHECK(webreg_icon_header(h2, strlen(h2), &size, &svg, code, sizeof code) < 0, "an icon too big");
    const char *h3 = "{\"ok\":true,\"type\":\"text/html\",\"size\":10}";
    CHECK(webreg_icon_header(h3, strlen(h3), &size, &svg, code, sizeof code) < 0, "not an image");
    CHECK(webreg_icon_header("{\"icon\":null}", 13, &size, &svg, code, sizeof code) < 0, "not the runtime's header");

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
    const char *s4 = "\xef\xbb\xbf<?xml version=\"1.0\"?>\n<!-- a > b -->\n<!DOCTYPE svg>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
    CHECK(webreg_icon_kind((const unsigned char *)s4, strlen(s4)) == 2, "an SVG after a BOM, a declaration, a comment and a doctype");
    const char *s5 = "<!doctype html><html><svg></svg></html>";
    CHECK(webreg_icon_kind((const unsigned char *)s5, strlen(s5)) == 0, "a page with an svg inside");
    const char *s6 = "<svg onload=\"x\"><foreignObject/></svg>";
    CHECK(webreg_icon_kind((const unsigned char *)s6, strlen(s6)) == 0, "an SVG with a foreignObject");
    memcpy(bad, png, 33);
    bad[12] = 'X';
    CHECK(webreg_icon_kind(bad, sizeof bad) == 0, "a PNG without IHDR");
    CHECK(webreg_icon_kind((const unsigned char *)"GIF89a", 6) == 0 && webreg_icon_kind(png, 0) == 0, "others");
}

/* ---- the cases lord-web's side brought (the same module: what lord-web reads at start) ---- */

static const char GOOD[] =
    "{\n  \"schema\": 1,\n  \"id\": \"chat\",\n  \"name\": \"Chat\",\n"
    "  \"startUrl\": \"https://web.example.com/\",\n  \"scope\": \"https://web.example.com/\",\n"
    "  \"origin\": \"https://web.example.com\",\n  \"icon\": \"icons/chat.png\",\n  \"themeColor\": \"#25d366\",\n"
    "  \"source\": \"catalog\",\n  \"background\": true,\n"
    "  \"profiles\": [ { \"id\": \"personal\", \"name\": \"Personal\" }, { \"id\": \"work\", \"name\": \"Trabajo \xc3\xa9\" } ],\n"
    "  \"permissions\": { \"notifications\": \"ask\", \"camera\": \"allow\", \"microphone\": \"ask\", \"screen\": \"ask\", \"location\": \"deny\" }\n}\n";

/* GOOD with the first occurrence of from replaced by to */
static int parse_with(const char *from, const char *to, char *err, size_t n) {
    char buf[8192];
    const char *at = strstr(GOOD, from);
    if (!at) { printf("FAIL: %s not in the sample\n", from); fails++; return 0; }
    snprintf(buf, sizeof buf, "%.*s%s%s", (int)(at - GOOD), GOOD, to, at + strlen(from));
    WebApp a;
    return webreg_parse(buf, strlen(buf), &a, err, n);
}

static void test_launch_file(void) {
    CHECK(webreg_id_ok("0a-b") && !webreg_id_ok("a/b"), "ids");
    CHECK(webreg_name_ok("Caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"), "utf-8");
    CHECK(!webreg_name_ok("a\nb") && !webreg_name_ok("\xc3") && !webreg_name_ok("\xc2\x85"), "a line end, a cut sequence, a C1 control");

    WebApp a;
    char err[160];
    CHECK(!webreg_parse(GOOD, strlen(GOOD), &a, err, sizeof err), "good: %s", err);
    CHECK(!strcmp(a.id, "chat") && !strcmp(a.name, "Chat") && a.source == WEBREG_SRC_CATALOG && a.background && a.nprofiles == 2, "fields");
    CHECK(!strcmp(a.icon, "icons/chat.png") && !strcmp(a.theme_color, "#25d366"), "icon and color");
    CHECK(a.perms[WEBREG_PERM_NOTIFICATIONS] == WEBREG_ASK && a.perms[WEBREG_PERM_CAMERA] == WEBREG_ALLOW && a.perms[WEBREG_PERM_LOCATION] == WEBREG_DENY, "permissions");
    CHECK(webreg_profile(&a, "work") && !strcmp(webreg_profile(&a, "work")->name, "Trabajo \xc3\xa9") && !webreg_profile(&a, "other"), "profiles");
    CHECK(!webreg_check_launch(&a, "chat", "work", err, sizeof err), "launch: %s", err);
    CHECK(webreg_check_launch(&a, "mail", "work", err, sizeof err), "another app's file");
    CHECK(webreg_check_launch(&a, "chat", "home", err, sizeof err), "a profile it doesn't have");
    CHECK(webreg_check_launch(&a, "chat", "../x", err, sizeof err), "a bad profile id");

    /* the argv/registry mismatches and every malformed field */
    struct { const char *from, *to, *why; } cases[] = {
        { "\"schema\": 1,\n", "", "no schema" },
        { "  \"source\": \"catalog\",\n", "", "no source" },
        { "\"id\": \"chat\"", "\"id\": \"Chat\"", "id" },
        { "\"name\": \"Chat\"", "\"name\": \"\"", "empty name" },
        { "\"name\": \"Chat\"", "\"name\": 5", "name type" },
        { "\"startUrl\": \"https://web.example.com/\"", "\"startUrl\": \"https://other.example.com/\"", "startUrl on another origin" },
        { "\"scope\": \"https://web.example.com/\"", "\"scope\": \"https://web.example.com/?a\"", "scope with a query" },
        { "\"origin\": \"https://web.example.com\"", "\"origin\": \"https://web.example.com/\"", "origin with a path" },
        { "\"origin\": \"https://web.example.com\"", "\"origin\": \"https://Web.example.com\"", "origin not lower case" },
        { "\"icon\": \"icons/chat.png\"", "\"icon\": \"icons/a/chat.png\"", "icon in a subfolder" },
        { "\"icon\": \"icons/chat.png\"", "\"icon\": \"icons/chat.exe\"", "icon type" },
        { "\"icon\": \"icons/chat.png\"", "\"icon\": \"/etc/passwd.png\"", "absolute icon" },
        { "\"profiles\": [ { \"id\": \"personal\", \"name\": \"Personal\" }, ", "\"profiles\": [ ", "still good (one profile)" },
        { "{ \"id\": \"personal\", \"name\": \"Personal\" }", "{ \"id\": \"personal\" }", "profile without a name" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int r = parse_with(cases[i].from, cases[i].to, err, sizeof err);
        if (!strncmp(cases[i].why, "still good", 10)) CHECK(!r, "%s: %s", cases[i].why, err);
        else CHECK(r && err[0], "refused: %s", cases[i].why);
    }
    char none[] = "{\"schema\":1,\"id\":\"a\",\"name\":\"A\",\"startUrl\":\"https://a.example/\",\"scope\":\"https://a.example/\",\"origin\":\"https://a.example\",\"profiles\":[]}";
    CHECK(webreg_parse(none, strlen(none), &a, err, sizeof err), "no profiles");
    char loc[] = "{\"schema\":1,\"id\":\"a\",\"name\":\"A\",\"startUrl\":\"http://localhost:8000/x\",\"scope\":\"http://localhost:8000/\",\"origin\":\"http://localhost:8000\",\"source\":\"url\",\"profiles\":[{\"id\":\"p\",\"name\":\"P\"}]}";
    CHECK(!webreg_parse(loc, strlen(loc), &a, err, sizeof err) && a.perms[WEBREG_PERM_CAMERA] == WEBREG_ASK && a.perms[WEBREG_PERM_LOCATION] == WEBREG_DENY, "local http, permissions default to ask (location to deny): %s", err);
    CHECK(webreg_parse("[]", 2, &a, err, sizeof err) && webreg_parse("{", 1, &a, err, sizeof err), "not an object");

    /* the file: a throwaway home */
    char home[] = "/tmp/webreg-launch.XXXXXX";
    if (!mkdtemp(home)) { CHECK(0, "mkdtemp"); return; }
    char p[512], q[512];
    snprintf(p, sizeof p, "%s/.config/lordos/web/apps", home);
    char mk[600];
    snprintf(mk, sizeof mk, "mkdir -p '%s'", p);
    CHECK(!system(mk), "mkdir");
    snprintf(p, sizeof p, "%s/.config/lordos/web/apps/chat.json", home);
    FILE *f = fopen(p, "w");
    if (f) { fputs(GOOD, f); fclose(f); }
    CHECK(!webreg_load(home, "chat", &a, err, sizeof err) && !strcmp(a.id, "chat"), "load: %s", err);
    CHECK(webreg_load(home, "mail", &a, err, sizeof err), "a missing file");
    CHECK(webreg_load(home, "../chat", &a, err, sizeof err), "a bad id never reaches the folder");
    /* the file of another app, under this app's name: refused at launch */
    snprintf(q, sizeof q, "%s/.config/lordos/web/apps/mail.json", home);
    CHECK(!symlink(p, q), "symlink");
    CHECK(webreg_load(home, "mail", &a, err, sizeof err), "a link is not followed");
    unlink(q);
    f = fopen(q, "w");
    if (f) { fputs(GOOD, f); fclose(f); }
    CHECK(!webreg_load(home, "mail", &a, err, sizeof err) && webreg_check_launch(&a, "mail", "work", err, sizeof err), "the file's id must be the app");
    /* over 64 KB */
    f = fopen(p, "w");
    if (f) { fputs("{\"schema\":1,\"name\":\"", f); for (int i = 0; i < WEBREG_FILE_MAX; i++) fputc('a', f); fputs("\"}", f); fclose(f); }
    CHECK(webreg_load(home, "chat", &a, err, sizeof err) && strstr(err, "over"), "too big: %s", err);
    snprintf(mk, sizeof mk, "rm -rf '%s'", home);
    CHECK(!system(mk), "cleanup");
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
    "# a stand-in lord-web: the runtime's two probe steps (docs/WEBAPPS.md), or a process that stays\n"
    "if [ \"$1\" = --probe ]; then\n"
    "  read -r url <&3\n"
    "  echo \"$url\" > \"$HOME/probed\"\n"
    "  printf '{\"ok\":true,\"name\":\"Notes Online\",\"startUrl\":\"https://notes.example/app/\",\"scope\":\"https://notes.example/app/\","
    "\"origin\":\"https://notes.example\",\"themeColor\":null,\"icons\":[{\"src\":\"https://notes.example/i/64.png\",\"sizes\":\"64x64\",\"type\":\"image/png\"},"
    "{\"src\":\"https://cdn.example/i/192.png\",\"sizes\":\"192x192\",\"type\":\"image/png\"}],\"manifest\":true}\\n' >&3\n"
    "  exit 0\n"
    "fi\n"
    "if [ \"$1\" = --probe-icon ]; then\n"
    "  read -r url <&3\n"
    "  echo \"$url\" > \"$HOME/probed-icon\"\n"
    "  printf '{\"ok\":true,\"type\":\"image/png\",\"size\":33}\\n' >&3\n"
    "  printf '\\211PNG\\r\\n\\032\\n\\000\\000\\000\\rIHDR\\000\\000\\000\\020\\000\\000\\000\\020\\010\\006\\000\\000\\000\\037\\363\\377a' >&3\n"
    "  exit 0\n"
    "fi\n"
    "echo \"$@\" > \"$HOME/args-$2-$4\"\n"
    "/usr/bin/env > \"$HOME/env-$2-$4\"\n"
    "exec /bin/sleep 30\n";

/* a stand-in whose page step fails the way the runtime says it does */
static const char FAKE_TLS[] =
    "#!/bin/sh\n"
    "read -r url <&3\n"
    "printf '{\"ok\":false,\"error\":\"web.probe.tls\"}\\n' >&3\n";

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
    snprintf(p, sizeof p, "%s/probed-icon", root);
    probed = read_text(p);
    CHECK(probed && !strcmp(probed, "https://cdn.example/i/192.png\n"), "the chosen icon went to --probe-icon on fd 3: %s", probed ? probed : "");
    free(probed);

    /* the runtime's own error code reaches JS (errors.json has its text) */
    snprintf(p, sizeof p, "%s/lord-web-tls", root);
    write_text(p, FAKE_TLS, 0755);
    webmgr_set_helper(p);
    int tid = webmgr_probe("https://tls.example/", &err);
    char *tj = tid > 0 ? wait_probe(tid) : NULL;
    CHECK(tj && strstr(tj, "\"ok\":false") && strstr(tj, "\"error\":\"web.probe.tls\""), "a TLS failure: %s", tj ? tj : "(none)");
    free(tj);
    snprintf(p, sizeof p, "%s/lord-web", root);
    webmgr_set_helper(p);

    /* installing from it */
    char app[WEBREG_ID_MAX + 1] = "";
    CHECK(webmgr_install("{\"source\":\"url\",\"probe\":999,\"profile\":\"Personal\"}", 49, app, sizeof app) != NULL, "an unknown probe");
    char entry[256];
    snprintf(entry, sizeof entry, "{\"source\":\"url\",\"probe\":%d,\"profile\":\"Personal\",\"startUrl\":\"https://evil.example/\"}", id);
    CHECK(webmgr_install(entry, strlen(entry), app, sizeof app) != NULL, "an entry that names a URL");
    snprintf(entry, sizeof entry, "{\"source\":\"url\",\"probe\":%d,\"profile\":\"Personal\"}", id);
    err = webmgr_install(entry, strlen(entry), app, sizeof app);
    CHECK(!err && !strcmp(app, "notes-online"), "installed from the probe: %s %s", err ? err : "", app);
    WebApp a;
    CHECK(!webmgr_load("notes-online", &a) && !strcmp(a.start_url, "https://notes.example/app/") && a.source == WEBREG_SRC_URL &&
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
    CHECK(!webmgr_load("chat", &a) && a.background && a.source == WEBREG_SRC_CATALOG && !a.icon[0], "its file, without the broken icon");
    CHECK(webmgr_install(cat, strlen(cat), app, sizeof app) != NULL, "already installed");

    /* profiles and permissions */
    char prof[WEBREG_ID_MAX + 1];
    CHECK(!webmgr_add_profile("chat", "Work", prof, sizeof prof) && !strcmp(prof, "work"), "a profile added");
    CHECK(webmgr_add_profile("chat", "Bad\xe2\x80\xae", prof, sizeof prof) != NULL, "a profile name with a bidi override");
    CHECK(!webmgr_rename_profile("chat", "work", "Office"), "renamed");
    CHECK(webmgr_rename_profile("chat", "nope", "X") != NULL, "renaming a missing profile");
    CHECK(!webmgr_set_permission("chat", "camera", "allow") && webmgr_set_permission("chat", "camera", "once") && webmgr_set_permission("chat", "files", "allow"),
          "permissions");
    CHECK(!webmgr_set_background("chat", 0), "background off");
    CHECK(!webmgr_load("chat", &a) && a.nprofiles == 2 && !strcmp(a.profiles[1].name, "Office") && a.perms[WEBREG_PERM_CAMERA] == WEBREG_ALLOW && !a.background, "all kept");

    /* starting it: argv and environment exactly, one process per app and profile */
    int raised = -1;
    CHECK(webmgr_launch("chat", "nope", &raised) != NULL, "a profile that is not in the registry");
    CHECK(webmgr_launch("chat", "../x", &raised) != NULL, "a profile id with a path");
    CHECK(!webmgr_launch("chat", "work", &raised) && raised == 0, "started");
    CHECK(!webmgr_launch("chat", "work", &raised) && raised == 1, "the second launch only raises it");
    WebRunning r[WEB_MAX_RUNNING];
    int n = webmgr_running(r, WEB_MAX_RUNNING);
    CHECK(n == 1 && !strcmp(r[0].app, "chat") && !strcmp(r[0].profile, "work"), "running: %d", n);
    char oa[WEBREG_ID_MAX + 1], op[WEBREG_ID_MAX + 1];
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
        char entry[256], app[WEBREG_ID_MAX + 1] = "";
        snprintf(entry, sizeof entry, "{\"source\":\"catalog\",\"id\":\"%s\",\"profile\":\"Personal\"}", id ? id : "");
        const char *err = webmgr_install(entry, strlen(entry), app, sizeof app);
        WebApp a;
        CHECK(id && !err && !strcmp(app, id), "catalog entry %s installs: %s", id ? id : "(no id)", err ? err : "");
        CHECK(id && !webmgr_load(id, &a) && a.source == WEBREG_SRC_CATALOG && strstr(a.icon, ".png"), "catalog entry %s has its icon", id ? id : "");
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
    test_launch_file();
    test_profiles();
    test_probe();
    test_manager();
    test_repo_catalog();
    printf("webreg-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
