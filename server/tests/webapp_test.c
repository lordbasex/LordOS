/* webapp_test.c — lord-web's pure parts (src/web, docs/WEBAPPS.md) without WebKit: the addresses (weburl.c: what is
 * an app address, origins, the scope prefix), the registry file (webreg.c: ids, URLs, scope, size, unknown keys, the
 * argv/registry match, reading it from a throwaway home), argv and the probe's request line (webargs.c), the probe's
 * answer and the icon check (webmanifest.c), and the download names (webdl.c).
 *
 *   make -C server build/webapp-test        (also in `make test`; needs no WPE) */
#include "../src/web/webargs.h"
#include "../src/web/webdl.h"
#include "../src/web/webmanifest.h"
#include "../src/web/webreg.h"
#include "../src/web/weburl.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_url(void) {
    WebUrl u;
    CHECK(!weburl_parse("https://Web.Example.COM/a?b#c", &u) && u.https && !strcmp(u.host, "web.example.com") && u.port == 0, "parse");
    CHECK(u.path_len == 4 && !strncmp(u.path, "/a?b", 4), "path without the fragment");
    CHECK(!weburl_parse("https://example.com:443/", &u) && u.port == 0, "default port");
    CHECK(!weburl_parse("https://example.com:8443", &u) && u.port == 8443 && u.path_len == 0, "port");
    CHECK(!weburl_parse("http://[::1]:8080/x", &u) && !strcmp(u.host, "[::1]") && weburl_is_local(&u), "ipv6");
    const char *bad[] = { "ftp://example.com/", "https:/example.com", "https://", "https://user@example.com/",
                          "https://exa mple.com/", "https://example.com/a b", "https://example.com:0/", "https://example.com:65536/",
                          "https://example.com:/", "https://exa_mple.com/", "https://.example.com/", "https://a..b/",
                          "https://example.com\\@evil/", "https://ex\xc3\xa1mple.com/", "https://[::1/", "https://[zz]/",
                          "javascript:alert(1)", "https://example.com:12a/", NULL };
    for (int i = 0; bad[i]; i++) CHECK(weburl_parse(bad[i], &u), "refused: %s", bad[i]);
    char longurl[WEBURL_MAX + 32] = "https://example.com/";
    memset(longurl + 20, 'a', WEBURL_MAX);
    longurl[WEBURL_MAX + 1] = 0;
    CHECK(weburl_parse(longurl, &u), "too long");

    CHECK(weburl_app_ok("https://example.com/"), "https");
    CHECK(!weburl_app_ok("http://example.com/"), "plain http elsewhere");
    CHECK(weburl_app_ok("http://localhost:8000/") && weburl_app_ok("http://127.0.0.1/"), "http on this computer");

    char o[WEBURL_ORIGIN_MAX + 1];
    CHECK(!weburl_origin("HTTPS://Example.com:443/x", o, sizeof o) && !strcmp(o, "https://example.com"), "origin %s", o);
    CHECK(!weburl_origin("http://localhost:8000/x", o, sizeof o) && !strcmp(o, "http://localhost:8000"), "origin port %s", o);
    CHECK(weburl_same_origin("https://a.example/x", "https://A.example:443/y"), "same origin");
    CHECK(!weburl_same_origin("https://a.example/", "http://a.example/"), "scheme differs");
    CHECK(!weburl_same_origin("https://a.example/", "https://a.example:8443/"), "port differs");

    CHECK(weburl_in_scope("https://web.example.com/chat?x#y", "https://web.example.com/"), "in scope");
    CHECK(weburl_in_scope("https://web.example.com", "https://web.example.com/"), "no path is /");
    CHECK(!weburl_in_scope("https://web.example.com.evil.net/", "https://web.example.com"), "a longer host is not in scope");
    CHECK(!weburl_in_scope("https://evil.net/https://web.example.com/", "https://web.example.com/"), "another origin");
    CHECK(weburl_in_scope("https://a.example/app/x", "https://a.example/app/"), "folder scope");
    CHECK(!weburl_in_scope("https://a.example/other", "https://a.example/app/"), "outside the folder");
    CHECK(!weburl_in_scope("https://a.example/app#/x", "https://a.example/app/"), "a fragment is not path");
    CHECK(!weburl_in_scope("not a url", "https://a.example/"), "garbage");

    CHECK(!weburl_dir("https://a.example/b/c?d/e", o, sizeof o) && !strcmp(o, "https://a.example/b/"), "dir %s", o);
    CHECK(!weburl_dir("https://a.example", o, sizeof o) && !strcmp(o, "https://a.example/"), "dir of root %s", o);
}

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

static void test_registry(void) {
    CHECK(webreg_id_ok("chat") && webreg_id_ok("0a-b") && webreg_id_ok("a234567890123456789012345678901234567890" + 1), "ids");
    CHECK(!webreg_id_ok("") && !webreg_id_ok("-a") && !webreg_id_ok("Chat") && !webreg_id_ok("a_b") && !webreg_id_ok("a.b") && !webreg_id_ok("a/b"), "bad ids");
    CHECK(!webreg_id_ok("a2345678901234567890123456789012345678901"), "41 bytes");
    CHECK(webreg_text_ok("Caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80", 128), "utf-8");
    CHECK(!webreg_text_ok("a\nb", 128) && !webreg_text_ok("\xc3", 128) && !webreg_text_ok("\xc0\xaf", 128) && !webreg_text_ok("\xed\xa0\x80", 128) && !webreg_text_ok("\xc2\x85", 128), "bad text");

    WebApp a;
    char err[160];
    CHECK(!webreg_parse(GOOD, strlen(GOOD), &a, err, sizeof err), "good: %s", err);
    CHECK(!strcmp(a.id, "chat") && !strcmp(a.name, "Chat") && a.catalog && a.background && a.nprofiles == 2, "fields");
    CHECK(!strcmp(a.icon, "icons/chat.png") && !strcmp(a.theme_color, "#25d366"), "icon and color");
    CHECK(a.perms[WK_NOTIFICATIONS] == WP_ASK && a.perms[WK_CAMERA] == WP_ALLOW && a.perms[WK_LOCATION] == WP_DENY, "permissions");
    CHECK(webreg_profile(&a, "work") && !strcmp(webreg_profile(&a, "work")->name, "Trabajo \xc3\xa9") && !webreg_profile(&a, "other"), "profiles");
    CHECK(!webreg_check_launch(&a, "chat", "work", err, sizeof err), "launch: %s", err);
    CHECK(webreg_check_launch(&a, "mail", "work", err, sizeof err), "another app's file");
    CHECK(webreg_check_launch(&a, "chat", "home", err, sizeof err), "a profile it doesn't have");
    CHECK(webreg_check_launch(&a, "chat", "../x", err, sizeof err), "a bad profile id");

    /* the argv/registry mismatches and every malformed field */
    struct { const char *from, *to, *why; } cases[] = {
        { "\"schema\": 1", "\"schema\": 2", "schema" },
        { "\"schema\": 1,\n", "", "no schema" },
        { "\"id\": \"chat\"", "\"id\": \"Chat\"", "id" },
        { "\"name\": \"Chat\"", "\"name\": \"\"", "empty name" },
        { "\"name\": \"Chat\"", "\"name\": 5", "name type" },
        { "\"startUrl\": \"https://web.example.com/\"", "\"startUrl\": \"http://web.example.com/\"", "plain http" },
        { "\"startUrl\": \"https://web.example.com/\"", "\"startUrl\": \"https://other.example.com/\"", "startUrl on another origin" },
        { "\"scope\": \"https://web.example.com/\"", "\"scope\": \"https://web.example.com/app/\"", "scope not a prefix" },
        { "\"scope\": \"https://web.example.com/\"", "\"scope\": \"https://web.example.com/?a\"", "scope with a query" },
        { "\"origin\": \"https://web.example.com\"", "\"origin\": \"https://web.example.com/\"", "origin with a path" },
        { "\"origin\": \"https://web.example.com\"", "\"origin\": \"https://Web.example.com\"", "origin not lower case" },
        { "\"icon\": \"icons/chat.png\"", "\"icon\": \"../chat.png\"", "icon climbs" },
        { "\"icon\": \"icons/chat.png\"", "\"icon\": \"icons/a/chat.png\"", "icon in a subfolder" },
        { "\"icon\": \"icons/chat.png\"", "\"icon\": \"icons/chat.exe\"", "icon type" },
        { "\"icon\": \"icons/chat.png\"", "\"icon\": \"/etc/passwd.png\"", "absolute icon" },
        { "\"themeColor\": \"#25d366\"", "\"themeColor\": \"green\"", "color" },
        { "\"source\": \"catalog\"", "\"source\": \"store\"", "source" },
        { "\"background\": true", "\"background\": 1", "background type" },
        { "\"profiles\": [ { \"id\": \"personal\", \"name\": \"Personal\" }, ", "\"profiles\": [ ", "still good (one profile)" },
        { "\"camera\": \"allow\"", "\"camera\": \"yes\"", "permission word" },
        { "\"camera\": \"allow\"", "\"clipboard\": \"allow\"", "unknown permission" },
        { "\"background\": true", "\"background\": true, \"inject\": \"x\"", "unknown key" },
        { "\"background\": true", "\"background\": true, \"background\": false", "duplicate key" },
        { "{ \"id\": \"personal\", \"name\": \"Personal\" }", "{ \"id\": \"work\", \"name\": \"Personal\" }", "profile twice" },
        { "{ \"id\": \"personal\", \"name\": \"Personal\" }", "{ \"id\": \"personal\" }", "profile without a name" },
        { "{ \"id\": \"personal\", \"name\": \"Personal\" }", "{ \"id\": \"personal\", \"name\": \"P\", \"x\": 1 }", "profile unknown key" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int r = parse_with(cases[i].from, cases[i].to, err, sizeof err);
        if (!strncmp(cases[i].why, "still good", 10)) CHECK(!r, "%s: %s", cases[i].why, err);
        else CHECK(r && err[0], "refused: %s", cases[i].why);
    }
    char none[] = "{\"schema\":1,\"id\":\"a\",\"name\":\"A\",\"startUrl\":\"https://a.example/\",\"scope\":\"https://a.example/\",\"origin\":\"https://a.example\",\"profiles\":[]}";
    CHECK(webreg_parse(none, strlen(none), &a, err, sizeof err), "no profiles");
    char loc[] = "{\"schema\":1,\"id\":\"a\",\"name\":\"A\",\"startUrl\":\"http://localhost:8000/x\",\"scope\":\"http://localhost:8000/\",\"origin\":\"http://localhost:8000\",\"profiles\":[{\"id\":\"p\",\"name\":\"P\"}]}";
    CHECK(!webreg_parse(loc, strlen(loc), &a, err, sizeof err) && a.perms[WK_CAMERA] == WP_ASK, "local http, permissions default to ask: %s", err);
    CHECK(webreg_parse("[]", 2, &a, err, sizeof err) && webreg_parse("{", 1, &a, err, sizeof err), "not an object");

    /* the file: a throwaway home */
    char home[] = "/tmp/webapp-test.XXXXXX";
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

static void test_args(void) {
    WebArgs a;
    char *ok[] = { "lord-web", "--app", "chat", "--profile", "work", NULL };
    CHECK(webargs_parse(5, ok, &a) == WEBARGS_APP && !strcmp(a.app, "chat") && !strcmp(a.profile, "work"), "app");
    char *probe[] = { "lord-web", "--probe", NULL };
    CHECK(webargs_parse(2, probe, &a) == WEBARGS_PROBE, "probe");
    char *icon[] = { "lord-web", "--probe-icon", NULL };
    CHECK(webargs_parse(2, icon, &a) == WEBARGS_PROBE_ICON, "probe-icon");
    char *swapped[] = { "lord-web", "--profile", "work", "--app", "chat", NULL };
    char *extra[] = { "lord-web", "--app", "chat", "--profile", "work", "--url", NULL };
    char *badid[] = { "lord-web", "--app", "../chat", "--profile", "work", NULL };
    char *url[] = { "lord-web", "https://evil.example/", NULL };
    char *none[] = { "lord-web", NULL };
    CHECK(webargs_parse(5, swapped, &a) == WEBARGS_BAD && webargs_parse(6, extra, &a) == WEBARGS_BAD && webargs_parse(5, badid, &a) == WEBARGS_BAD
          && webargs_parse(2, url, &a) == WEBARGS_BAD && webargs_parse(1, none, &a) == WEBARGS_BAD, "anything else");

    char u[WEBARGS_LINE_MAX + 1];
    CHECK(!webargs_probe_line("https://a.example/\n", 19, u, sizeof u) && !strcmp(u, "https://a.example/"), "line");
    CHECK(webargs_probe_line("https://a.example/", 18, u, sizeof u), "no newline");
    CHECK(webargs_probe_line("https://a.example/\nx\n", 21, u, sizeof u), "two lines");
    CHECK(webargs_probe_line("http://a.example/\n", 18, u, sizeof u), "http");
    CHECK(webargs_probe_line("file:///etc/passwd\n", 19, u, sizeof u), "file:");
    CHECK(!webargs_probe_line("http://127.0.0.1:8000/\n", 23, u, sizeof u), "http on this computer");
}

static void test_manifest(void) {
    char out[WEBMANIFEST_LINE_MAX], c[8];
    CHECK(!webmanifest_color("#ABC", c) && !strcmp(c, "#aabbcc") && !webmanifest_color(" #25D366 ", c) && !strcmp(c, "#25d366"), "colors");
    CHECK(webmanifest_color("red", c) && webmanifest_color("#12345", c) && webmanifest_color("#25d366 x", c), "bad colors");

    const char *page =
        "{\"title\":\"Page\",\"metaThemeColor\":\"#111\",\"manifest\":{\"name\":\"  Chat\\n App \",\"short_name\":\"C\","
        "\"start_url\":\"https://web.example.com/app/?src=pwa\",\"scope\":\"https://web.example.com/app/\",\"theme_color\":\"#25D366\","
        "\"icons\":[{\"src\":\"https://web.example.com/i/192.png\",\"sizes\":\"192x192\",\"type\":\"image/png\"},"
        "{\"src\":\"data:image/png;base64,AAAA\",\"sizes\":\"1x1\"},{\"src\":\"https://cdn.example.net/512.png\",\"sizes\":\"512x512\",\"type\":\"image/png\"}]},"
        "\"linkIcons\":[{\"src\":\"https://web.example.com/favicon.ico\"}]}";
    CHECK(!webmanifest_answer("https://web.example.com/app/#main", page, strlen(page), out, sizeof out), "answer");
    CHECK(strstr(out, "\"ok\":true") && strstr(out, "\"name\":\"Chat App\"") && strstr(out, "\"startUrl\":\"https://web.example.com/app/?src=pwa\"")
          && strstr(out, "\"scope\":\"https://web.example.com/app/\"") && strstr(out, "\"origin\":\"https://web.example.com\"")
          && strstr(out, "\"themeColor\":\"#25d366\"") && strstr(out, "\"manifest\":true"), "fields: %s", out);
    CHECK(strstr(out, "192.png") && strstr(out, "cdn.example.net/512.png") && !strstr(out, "data:") && !strstr(out, "favicon"), "icons: %s", out);

    /* a manifest pointing elsewhere: the page's own address and folder instead */
    const char *evil =
        "{\"title\":\"T\",\"manifest\":{\"name\":\"X\",\"start_url\":\"https://evil.example/\",\"scope\":\"https://evil.example/\",\"icons\":[]},"
        "\"linkIcons\":[{\"src\":\"https://web.example.com/apple.png\",\"sizes\":\"180x180\",\"type\":\"image/png\"}]}";
    CHECK(!webmanifest_answer("https://web.example.com/a/b", evil, strlen(evil), out, sizeof out), "evil answer");
    CHECK(strstr(out, "\"startUrl\":\"https://web.example.com/a/b\"") && strstr(out, "\"scope\":\"https://web.example.com/a/\"")
          && strstr(out, "\"origin\":\"https://web.example.com\"") && strstr(out, "apple.png") && strstr(out, "\"themeColor\":null"), "evil: %s", out);
    /* a scope that doesn't hold the start address */
    const char *narrow = "{\"title\":\"T\",\"manifest\":{\"start_url\":\"https://w.example/x\",\"scope\":\"https://w.example/y/\"}}";
    CHECK(!webmanifest_answer("https://w.example/", narrow, strlen(narrow), out, sizeof out) && strstr(out, "\"scope\":\"https://w.example/\"") && strstr(out, "\"name\":\"T\""), "narrow: %s", out);
    /* no manifest, no title: the host's name */
    const char *bare = "{\"title\":\"\",\"manifest\":null,\"linkIcons\":[]}";
    CHECK(!webmanifest_answer("https://plain.example/", bare, strlen(bare), out, sizeof out) && strstr(out, "\"name\":\"plain.example\"") && strstr(out, "\"manifest\":false") && strstr(out, "\"icons\":[]"), "bare: %s", out);
    CHECK(webmanifest_answer("http://plain.example/", bare, strlen(bare), out, sizeof out), "a plain-http page");
    CHECK(webmanifest_answer("https://plain.example/", "[1]", 3, out, sizeof out), "not an object");
    CHECK(!webmanifest_error("web.probe.load", out, sizeof out) && !strcmp(out, "{\"ok\":false,\"error\":\"web.probe.load\"}"), "error: %s", out);

    unsigned char png[32] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
    CHECK(webicon_kind(png, sizeof png) == WEBICON_PNG && !strcmp(webicon_type(WEBICON_PNG), "image/png"), "png");
    png[12] = 'X';
    CHECK(webicon_kind(png, sizeof png) == WEBICON_NONE, "png without IHDR");
    const char *svg = "\xef\xbb\xbf<?xml version=\"1.0\"?>\n<!-- a > b -->\n<!DOCTYPE svg>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
    CHECK(webicon_kind((const unsigned char *)svg, strlen(svg)) == WEBICON_SVG, "svg");
    const char *html = "<!doctype html><html><svg></svg></html>";
    CHECK(webicon_kind((const unsigned char *)html, strlen(html)) == WEBICON_NONE, "html with an svg inside");
    CHECK(webicon_kind((const unsigned char *)"GIF89a", 6) == WEBICON_NONE && webicon_kind(png, 0) == WEBICON_NONE, "others");
    CHECK(webicon_kind(png, WEBICON_MAX + 1) == WEBICON_NONE, "too big");
}

static void test_downloads(void) {
    char n[256];
    struct { const char *in, *want; } cases[] = {
        { "report.pdf", "report.pdf" }, { "../../.bashrc", "bashrc" }, { "a/b\\c.txt", "c.txt" }, { "...", "download" },
        { "", "download" }, { NULL, "download" }, { "inv\x01oice\x7f.pdf", "invoice.pdf" }, { "a:b*c?.txt", "a_b_c_.txt" },
        { " name . ", "name" }, { "x\xc2\x85y", "xy" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        CHECK(!webdl_name(cases[i].in, n, sizeof n) && !strcmp(n, cases[i].want), "%s -> %s (want %s)", cases[i].in ? cases[i].in : "(null)", n, cases[i].want);
    char longname[600];
    memset(longname, 'a', 300);
    strcpy(longname + 300, "\xc3\xa9\xc3\xa9.tar.gz");
    CHECK(!webdl_name(longname, n, sizeof n) && strlen(n) <= WEBDL_NAME_MAX && !strcmp(n + strlen(n) - 3, ".gz"), "long: %zu", strlen(n));
    memset(longname, 'a', 199);
    strcpy(longname + 199, "\xc3\xa9zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz");
    CHECK(!webdl_name(longname, n, sizeof n) && strlen(n) == 199, "cut on a character boundary: %zu", strlen(n));

    char dir[] = "/tmp/webdl-test.XXXXXX";
    if (!mkdtemp(dir)) { CHECK(0, "mkdtemp"); return; }
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    CHECK(!webdl_unique(fd, "a.pdf", n, sizeof n) && !strcmp(n, "a.pdf"), "free");
    close(openat(fd, "a.pdf", O_CREAT | O_WRONLY, 0600));
    CHECK(!webdl_unique(fd, "a.pdf", n, sizeof n) && !strcmp(n, "a (2).pdf"), "taken: %s", n);
    close(openat(fd, "a (2).pdf", O_CREAT | O_WRONLY, 0600));
    CHECK(!webdl_unique(fd, "a.pdf", n, sizeof n) && !strcmp(n, "a (3).pdf"), "taken twice: %s", n);
    CHECK(!symlinkat("/nonexistent", fd, "b"), "symlink");
    CHECK(!webdl_unique(fd, "b", n, sizeof n) && !strcmp(n, "b (2)"), "a dangling link counts as taken: %s", n);
    unlinkat(fd, "a.pdf", 0);
    unlinkat(fd, "a (2).pdf", 0);
    unlinkat(fd, "b", 0);
    close(fd);
    rmdir(dir);
}

int main(void) {
    test_url();
    test_registry();
    test_args();
    test_manifest();
    test_downloads();
    printf("webapp-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
