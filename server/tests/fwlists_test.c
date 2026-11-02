/* fwlists_test.c — subscribed blocklists (src/fw/fwlists.c, docs/FIREWALL.md "Blocklists"):
 * the raw parsers (hosts, domains, addresses) and their strictness, the set's lookups, the
 * compiled file's reader, the shared-address rule, mutation fuzzing and a measurement. */
#define _POSIX_C_SOURCE 200809L
#include "fw/fwlists.h"
#include "fw/rules.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t rng = 0x243f6a8885a308d3ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

static int has_line(const char *text, const char *line) {
    size_t n = strlen(line);
    for (const char *p = text; (p = strstr(p, line)); p += n)
        if ((p == text || p[-1] == '\n') && (p[n] == '\n' || !p[n])) return 1;
    return 0;
}

static int parse(const char *raw, FwlFormat f, int sub, char **out, FwlStats *st, char *err) {
    size_t ol;
    return fwl_parse_raw(raw, strlen(raw), f, sub, out, &ol, st, err, 32);
}

static uint8_t *ip(const char *s, int *fam) {
    static uint8_t a[16];
    memset(a, 0, 16);
    *fam = strchr(s, ':') ? 6 : 4;
    inet_pton(*fam == 4 ? AF_INET : AF_INET6, s, a);
    return a;
}

static void test_hosts(void) {
    const char *raw =
        "# a hosts file\n"
        "127.0.0.1 localhost\n"
        "127.0.0.1 localhost.localdomain\n"
        "::1 ip6-localhost ip6-loopback\n"
        "0.0.0.0 0.0.0.0\n"
        "\n"
        "0.0.0.0 Tracker.Example   # inline comment\n"
        "0.0.0.0 ads.example.net metrics.example.net\r\n"
        "127.0.0.1\tpixel.example.org\n"
        "0.0.0.0 tracker.example\n"            /* repeated */
        "0.0.0.0 intranet\n"                    /* no dot: skipped */
        "192.168.1.10 printer.example.com\n";   /* not a sink-hole: a bad line */
    char *out = NULL, err[32] = "";
    FwlStats st;
    CHECK(!parse(raw, FWL_FMT_HOSTS, 0, &out, &st, err));
    CHECK(out && has_line(out, "d tracker.example") && has_line(out, "d ads.example.net") && has_line(out, "d metrics.example.net") &&
          has_line(out, "d pixel.example.org"));
    CHECK(out && !strstr(out, "localhost") && !strstr(out, "printer") && !strstr(out, "intranet"));
    CHECK(st.domains == 4 && st.dups == 1 && st.bad == 1 && st.skipped >= 4);
    free(out);
    /* with sub = 1 every name covers its subdomains */
    CHECK(!parse("0.0.0.0 ads.example.net\n", FWL_FMT_HOSTS, 1, &out, &st, err) && has_line(out, "s ads.example.net"));
    free(out);
}

static void test_domains(void) {
    char *out = NULL, err[32] = "";
    FwlStats st;
    CHECK(!parse("! comment\nexample.com\n*.cdn-ads.example\n||adserver.example^\nWWW.Example.ORG.\n", FWL_FMT_DOMAINS, 0, &out, &st, err));
    CHECK(has_line(out, "d example.com") && has_line(out, "s cdn-ads.example") && has_line(out, "s adserver.example") && has_line(out, "d www.example.org"));
    free(out);
    /* two names on a line, an address or odd bytes are bad lines; enough of them reject the list */
    char big[8000] = "";
    for (int i = 0; i < 30; i++) strcat(big, "this is not a domain list\n");
    for (int i = 0; i < 10; i++) strcat(big, "ok.example\n");
    CHECK(parse(big, FWL_FMT_DOMAINS, 0, &out, &st, err) == -1 && !strcmp(err, "format"));
    CHECK(parse("1.2.3.4\n", FWL_FMT_DOMAINS, 0, &out, &st, err) == -1 && !strcmp(err, "empty"));
    CHECK(parse("# nothing\n\n", FWL_FMT_DOMAINS, 0, &out, &st, err) == -1 && !strcmp(err, "empty"));
    /* a NUL byte or a very long line reject the whole list */
    char nul[] = "a.example\n\0b.example\n";
    size_t ol;
    CHECK(fwl_parse_raw(nul, sizeof nul - 1, FWL_FMT_DOMAINS, 0, &out, &ol, &st, err, sizeof err) == -1 && !strcmp(err, "nul"));
    char *lng = malloc(FWL_LINE_MAX + 40);
    memset(lng, 'a', FWL_LINE_MAX + 30);
    memcpy(lng, "x.example\n", 10);
    lng[FWL_LINE_MAX + 30] = 0;
    CHECK(parse(lng, FWL_FMT_DOMAINS, 0, &out, &st, err) == -1 && !strcmp(err, "line"));
    free(lng);
    CHECK(fwl_parse_raw("a.example\n", FWL_RAW_MAX + 1, FWL_FMT_DOMAINS, 0, &out, &ol, &st, err, sizeof err) == -1 && !strcmp(err, "big"));
    CHECK(fwl_parse_raw("a.example\n", 10, (FwlFormat)9, 0, &out, &ol, &st, err, sizeof err) == -1);
    CHECK(fwl_format_from_name("hosts") == FWL_FMT_HOSTS && fwl_format_from_name("ips") == FWL_FMT_IPS && !fwl_format_from_name("csv"));
}

static void test_ips(void) {
    char *out = NULL, err[32] = "";
    FwlStats st;
    const char *raw = "# feed\n203.0.113.7\n198.51.100.0/24 ; a comment\n203.0.113.9\t7\n"
                      "10.0.0.0/8\n192.168.1.1\n127.0.0.1\n0.0.0.0/0\n2.0.0.0/7\n"     /* LAN and too broad: skipped */
                      "2001:db8::/32\n2001:db8:5::1\n::/0\nfe80::1\n"
                      "198.51.100.1/24\n"                                              /* host bits set: bad */
                      "1.2.3.4-1.2.3.9\n";                                              /* a range: bad */
    CHECK(!parse(raw, FWL_FMT_IPS, 0, &out, &st, err));
    CHECK(has_line(out, "n 203.0.113.7") && has_line(out, "n 198.51.100.0/24") && has_line(out, "n 203.0.113.9") &&
          has_line(out, "n 2001:db8::/32") && has_line(out, "n 2001:db8:5::1"));
    CHECK(!strstr(out, "10.0.0.0") && !strstr(out, "192.168") && !strstr(out, "127.") && !strstr(out, "0.0.0.0/0") && !strstr(out, "fe80"));
    CHECK(st.nets == 5 && st.skipped == 7 && st.bad == 2);
    free(out);
}

static void test_set(void) {
    FwLists *l = fwl_new();
    CHECK(fwl_add_line(l, "d tracker.example", 17, 1) == 0);
    CHECK(fwl_add_line(l, "d tracker.example", 17, 2) == 0);           /* repeated: the first tag stays */
    CHECK(fwl_add_line(l, "s ads.example", 13, 2) == 0);
    CHECK(fwl_add_line(l, "n 203.0.113.0/24", 16, 3) == 0);
    CHECK(fwl_add_line(l, "n 2001:db8::/32", 15, 4) == 0);
    CHECK(fwl_add_line(l, "d Upper.example", 15, 1) == -1);           /* not normalized */
    CHECK(fwl_add_line(l, "n 10.0.0.0/8", 12, 1) == -1);              /* LAN */
    CHECK(fwl_add_line(l, "x y", 3, 1) == -1 && fwl_add_line(l, "d a.b", 5, 0) == -1);
    CHECK(fwl_domains(l) == 2 && fwl_nets(l) == 2);
    CHECK(fwl_match_name(l, "tracker.example") == 1 && fwl_match_name(l, "TRACKER.example.") == 1);
    CHECK(fwl_match_name(l, "www.tracker.example") == 0);              /* exact only */
    CHECK(fwl_match_name(l, "ads.example") == 2 && fwl_match_name(l, "a.b.ads.example") == 2);
    CHECK(fwl_match_name(l, "notads.example") == 0 && fwl_match_name(l, "example") == 0);
    int fam;
    uint8_t *a = ip("203.0.113.99", &fam);
    CHECK(fwl_match_addr(l, fam, a) == 3);
    a = ip("203.0.114.1", &fam);
    CHECK(fwl_match_addr(l, fam, a) == 0);
    a = ip("::ffff:203.0.113.5", &fam);
    CHECK(fwl_match_addr(l, fam, a) == 3);                              /* an IPv4-mapped address is IPv4 */
    a = ip("2001:db8:ffff::1", &fam);
    CHECK(fwl_match_addr(l, fam, a) == 4);
    /* the shared-address rule */
    const char *both[] = { "x.ads.example", "tracker.example" }, *mixed[] = { "tracker.example", "site.example" };
    CHECK(fwl_block_names(l, both, 2) == 2);
    CHECK(fwl_block_names(l, mixed, 2) == 0);
    CHECK(fwl_block_names(l, NULL, 0) == 0);
    CHECK(fwl_check_line("d tracker.example", 17) == 0 && fwl_check_line("d Tracker.example", 17) == -1 && fwl_check_line("n 10.0.0.0/8", 12) == -1);
    /* the compiled file: round trip (repeated lines are fine), and any bad line rejects it */
    const char *t = "lordos-fwlists 1\nd tracker.example\ns ads.example\nn 203.0.113.0/24\nd tracker.example\nn 2001:db8::/32\n";
    size_t n = strlen(t);
    char err[64];
    FwLists *b = fwl_load_text(t, n, err, sizeof err);
    CHECK(b && fwl_domains(b) == 2 && fwl_nets(b) == 2 && fwl_match_name(b, "q.ads.example") && fwl_match_addr(b, 4, ip("203.0.113.1", &fam)));
    fwl_free(b);
    const char *bad[] = { "", "lordos-fwlists 2\nd a.example\n", "lordos-fwlists 1\nd a.example\nn 10.0.0.0/8\n", "lordos-fwlists 1\nd ../etc\n",
                          "lordos-fwlists 1\nq a.example\n", "lordos-fwlists 1x\n", NULL };
    for (int i = 0; bad[i]; i++) CHECK(!fwl_load_text(bad[i], strlen(bad[i]), err, sizeof err));
    char withnul[] = "lordos-fwlists 1\nd a.example\0\n";
    CHECK(!fwl_load_text(withnul, sizeof withnul - 1, err, sizeof err));
    b = fwl_load_text("lordos-fwlists 1", 16, err, sizeof err);
    CHECK(b && !fwl_domains(b));
    fwl_free(b);
    fwl_free(l);
}

/* random bytes and mutations of a real-looking list: never a crash, and whatever is accepted
 * loads back as a compiled file */
static void test_fuzz(void) {
    const char *seed = "0.0.0.0 a.example\n0.0.0.0 b.example c.example\n# x\n203.0.113.1\n*.d.example\n";
    int accepted = 0;
    for (int i = 0; i < 20000; i++) {
        char buf[256];
        size_t n = strlen(seed);
        memcpy(buf, seed, n);
        int muts = 1 + (int)(rnd() % 8);
        for (int m = 0; m < muts; m++) buf[rnd() % n] = (char)(rnd() & 0xff);
        if (i % 4 == 0) for (size_t k = 0; k < n; k++) buf[k] = (char)(rnd() & 0xff);
        char *out = NULL, err[32];
        size_t ol;
        FwlStats st;
        FwlFormat f = (FwlFormat)(1 + rnd() % 3);
        if (!fwl_parse_raw(buf, n, f, (int)(rnd() & 1), &out, &ol, &st, err, sizeof err)) {
            accepted++;
            FwLists *l = fwl_new();
            CHECK(fwl_add_text(l, out, ol, 1) >= 1);
            fwl_free(l);
            free(out);
        }
    }
    printf("fwlists-test: 20000 mutated lists, %d accepted\n", accepted);
}

static double secs(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static void bench(void) {
    size_t cap = 200000 * 40, n = 0;
    char *raw = malloc(cap);
    for (int i = 0; i < 200000; i++) n += (size_t)snprintf(raw + n, cap - n, "0.0.0.0 host%d.tracker%d.example\n", i, i % 977);
    double t0 = secs();
    char *out, err[32];
    size_t ol;
    FwlStats st;
    int rc = fwl_parse_raw(raw, n, FWL_FMT_HOSTS, 0, &out, &ol, &st, err, sizeof err);
    double t1 = secs();
    CHECK(!rc && st.domains == 200000);
    FwLists *l = fwl_new();
    CHECK(!rc && fwl_add_text(l, out, ol, 1) == 200000);
    double t2 = secs();
    int hits = 0;
    char name[64];
    for (int i = 0; i < 1000000; i++) {
        snprintf(name, sizeof name, "host%d.tracker%d.example", i % 400000, (i % 400000) % 977);
        hits += fwl_match_name(l, name) != 0;
    }
    double t3 = secs();
    CHECK(hits == 600000);
    printf("fwlists-test: 200000-name hosts list parsed in %.0f ms, loaded in %.0f ms, 1e6 lookups in %.0f ms (the set: %zu entries)\n",
           (t1 - t0) * 1e3, (t2 - t1) * 1e3, (t3 - t2) * 1e3, fwl_domains(l));
    fwl_free(l);
    free(out);
    free(raw);
}

int main(void) {
    test_hosts();
    test_domains();
    test_ips();
    test_set();
    test_fuzz();
    bench();
    printf("fwlists-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
