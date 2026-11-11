/* webapp_test.c — lord-web's pure parts (src/web, docs/WEBAPPS.md) without WebKit: the addresses (weburl.c: what is
 * an app address, origins, the scope prefix), argv and the probe's request line (webargs.c), the probe's
 * answer and the icon check (webmanifest.c), and the download names (webdl.c). The registry file itself (webreg.c,
 * shared with lord-server) is tests/webreg_test.c's.
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

static int cls(const char *url) {
    WebUrl u;
    return weburl_parse(url, &u) ? -1 : weburl_host_class(&u);
}

/* the probe never goes from a public address to this computer or a local network, nor from https to http */
static void test_probe_hops(void) {
    const char *priv[] = { "http://localhost/", "https://LOCALHOST./", "https://a.localhost/", "https://printer.local/", "https://127.0.0.1/",
                           "https://127.8.9.1/", "https://127.1/", "https://0x7f.1/", "https://017700000001/", "https://2130706433/", "https://0.0.0.0/",
                           "https://10.1.2.3/", "https://172.16.0.1/", "https://172.31.255.255/", "https://192.168.1.1/", "https://169.254.169.254/",
                           "https://100.64.0.1/", "https://[::1]/", "https://[::]/", "https://[fc00::1]/", "https://[fd12:3456::1]/", "https://[fe80::1]/",
                           "https://[::ffff:127.0.0.1]/", "https://[::ffff:192.168.0.1]/", "https://[::ffff:7f00:1]/", "https://1.2.3.999/" };
    for (size_t i = 0; i < sizeof priv / sizeof priv[0]; i++) CHECK(cls(priv[i]) == WEBURL_PRIVATE, "private: %s", priv[i]);
    const char *pub[] = { "https://example.com/", "https://8.8.8.8/", "https://172.32.0.1/", "https://172.15.0.1/", "https://192.169.0.1/",
                          "https://100.128.0.1/", "https://[2001:db8::1]/", "https://[::ffff:8.8.8.8]/", "https://local.example/", "https://localhost.example/",
                          "https://10.example/" };
    for (size_t i = 0; i < sizeof pub / sizeof pub[0]; i++) CHECK(cls(pub[i]) == WEBURL_PUBLIC, "public: %s", pub[i]);

    /* names of a local network, and single labels */
    const char *lan[] = { "https://router/", "https://nas./", "https://printer.lan/", "https://box.home/", "https://x.home.arpa/", "https://db.internal/",
                          "https://wiki.intranet/", "https://h.localdomain/", "https://mail.corp/", "https://lan/", "https://internal/" };
    for (size_t i = 0; i < sizeof lan / sizeof lan[0]; i++) CHECK(cls(lan[i]) == WEBURL_PRIVATE, "a local network's name: %s", lan[i]);
    const char *names[] = { "https://lan.example.com/", "https://home.example/", "https://corporate.example/", "https://intranet.example.org/" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) CHECK(cls(names[i]) == WEBURL_PUBLIC, "a public name: %s", names[i]);
    WebUrl lu;
    CHECK(!weburl_parse("https://127.1./", &lu) && weburl_host_is_literal(&lu) && !weburl_parse("https://[::1]/", &lu) && weburl_host_is_literal(&lu) &&
          !weburl_parse("https://a.example/", &lu) && !weburl_host_is_literal(&lu), "addresses and names");

    /* every address a name resolved to (the probe feeds what getaddrinfo said) */
    WebAddr pub4 = { 4, { 93, 184, 216, 34 } }, lo4 = { 4, { 127, 0, 0, 1 } }, ten = { 4, { 10, 0, 0, 7 } }, cgnat = { 4, { 100, 100, 1, 1 } },
            link4 = { 4, { 169, 254, 169, 254 } }, pub6 = { 6, { 0x20, 0x01, 0x0d, 0xb8, [15] = 1 } }, lo6 = { 6, { [15] = 1 } },
            ula = { 6, { 0xfd, 0x00, [15] = 1 } }, ll6 = { 6, { 0xfe, 0x80, [15] = 1 } }, map = { 6, { [10] = 0xff, [11] = 0xff, [12] = 192, [13] = 168, [14] = 0, [15] = 1 } },
            odd = { 9, { 0 } };
    WebAddr all_pub[] = { pub4, pub6 };
    CHECK(weburl_addrs_class(all_pub, 2) == WEBURL_PUBLIC, "only public addresses");
    WebAddr one[2];
    const WebAddr bad[] = { lo4, ten, cgnat, link4, lo6, ula, ll6, map, odd };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        one[0] = pub4;
        one[1] = bad[i];
        CHECK(weburl_addrs_class(one, 2) == WEBURL_PRIVATE, "one private address among public ones (%zu)", i);
    }
    CHECK(weburl_addrs_class(NULL, 0) == WEBURL_PRIVATE && weburl_addrs_class(all_pub, 0) == WEBURL_PRIVATE, "nothing resolved: refused");

    /* every hop, by what it resolved to: once a hop was public, every next one must be public */
    struct { int start; int hops[4]; int n, want; const char *what; } seq[] = {
        { 1, { WEBURL_PUBLIC, WEBURL_PUBLIC }, 2, 1, "public, public" },
        { 1, { WEBURL_PUBLIC, WEBURL_PRIVATE }, 2, 0, "public, then private" },
        { 1, { -1 }, 1, 0, "public, then a name that doesn't resolve" },
        { 0, { WEBURL_PRIVATE, WEBURL_PRIVATE }, 2, 1, "local, local" },
        { 0, { WEBURL_PRIVATE, WEBURL_PUBLIC, WEBURL_PRIVATE }, 3, 0, "local, public, then private" },
        { 0, { WEBURL_PUBLIC, WEBURL_PUBLIC }, 2, 1, "local, then public ones" },
        { 0, { -1, WEBURL_PRIVATE }, 2, 0, "local, an unknown name, then private" },
    };
    for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        int seen = seq[i].start, ok = 1;
        for (int k = 0; k < seq[i].n && ok; k++) ok = weburl_probe_step(&seen, seq[i].hops[k]);
        CHECK(ok == seq[i].want, "hops: %s", seq[i].what);
    }

    CHECK(weburl_probe_hop_ok("https://a.example/", "https://b.example/x"), "public to public");
    CHECK(!weburl_probe_hop_ok("https://a.example/", "http://a.example/"), "https to http");
    CHECK(!weburl_probe_hop_ok("https://a.example/", "http://localhost:8080/"), "https to http on this computer");
    CHECK(!weburl_probe_hop_ok("https://a.example/", "https://127.0.0.1:8443/"), "public to this computer");
    CHECK(!weburl_probe_hop_ok("https://a.example/", "https://192.168.1.1/admin"), "public to the local network");
    CHECK(!weburl_probe_hop_ok("https://a.example/", "https://[::1]/"), "public to ::1");
    CHECK(weburl_probe_hop_ok("http://localhost:8000/", "http://127.0.0.1:8000/x"), "this computer to itself (tests)");
    CHECK(weburl_probe_hop_ok("https://10.0.0.5/", "https://example.com/"), "private to public");
    CHECK(!weburl_probe_hop_ok("https://a.example/", "ftp://a.example/") && !weburl_probe_hop_ok("bad", "https://a.example/"), "not addresses");

    CHECK(weburl_probe_final_ok("https://a.example/", "https://b.example/"), "ended public");
    CHECK(!weburl_probe_final_ok("https://a.example/", "http://localhost/"), "ended on http://localhost from a public request");
    CHECK(!weburl_probe_final_ok("https://a.example/", "https://localhost/"), "ended on this computer from a public request");
    CHECK(!weburl_probe_final_ok("https://a.example/", "http://b.example/"), "ended on plain http");
    CHECK(weburl_probe_final_ok("http://localhost:8000/", "http://localhost:8000/app/"), "asked for this computer, ended there");
    CHECK(!weburl_probe_final_ok("https://localhost:8000/", "http://localhost:8000/"), "even here, not from https to http");
}

/* who may use the app's permissions (webengine.c on_permission and on_query) */
static void test_perm_context(void) {
    const char *sc = "https://web.example.com/app/", *or = "https://web.example.com";
    CHECK(weburl_perm_allowed("https://web.example.com/app/inbox", NULL, sc, or), "the app's page, a request (no origin given)");
    CHECK(weburl_perm_allowed("https://web.example.com/app/", "https://web.example.com", sc, or), "the app's page, a query from its origin");
    CHECK(weburl_perm_allowed("https://Web.Example.com:443/app/x", "https://web.example.com:443", sc, or), "case and the default port");
    CHECK(!weburl_perm_allowed("https://evil.example/app/", NULL, sc, or), "another site in the window");
    CHECK(!weburl_perm_allowed("https://accounts.example.org/signin", NULL, sc, or), "a sign-in page after a redirect");
    CHECK(!weburl_perm_allowed("https://web.example.com/other/", NULL, sc, or), "the app's origin, outside its scope");
    CHECK(!weburl_perm_allowed("https://web.example.com/app/", "https://ads.example.net", sc, or), "a frame of another origin");
    CHECK(!weburl_perm_allowed("https://web.example.com/app/", "null", sc, or) && !weburl_perm_allowed("https://web.example.com/app/", "", sc, or), "an opaque origin");
    CHECK(!weburl_perm_allowed("https://web.example.com/app/", "http://web.example.com", sc, or), "the same host over http");
    CHECK(!weburl_perm_allowed("https://web.example.com/app/", "https://web.example.com:8443", sc, or), "another port");
    CHECK(!weburl_perm_allowed(NULL, NULL, sc, or) && !weburl_perm_allowed("about:blank", NULL, sc, or) && !weburl_perm_allowed("data:text/html,x", NULL, sc, or),
          "no page yet, about:, data:");
    CHECK(!weburl_perm_allowed("http://web.example.com/app/", NULL, "http://web.example.com/app/", "http://web.example.com"), "plain http is never an app's");
    CHECK(weburl_perm_allowed("http://localhost:8000/x", "http://localhost:8000", "http://localhost:8000/", "http://localhost:8000"), "http on this computer");
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

    unsigned char png[33] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 16, 0, 0, 0, 16, 8, 6 };
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
    test_perm_context();
    test_probe_hops();
    test_args();
    test_manifest();
    test_downloads();
    printf("webapp-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
