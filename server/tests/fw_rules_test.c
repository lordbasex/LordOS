/* fw_rules_test.c — per-app network rules (src/fw/rules.c, docs/FIREWALL.md): precedence, v4/v6
 * networks, LAN, wildcards, durations, restrictions, JSON (round trip and rejection) and a random
 * test against an obvious linear version of the decider. */
#ifndef _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "fw/rules.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static FwRule mk(const char *app, FwDir dir, const char *dst, FwAction act) {
    FwRule r;
    memset(&r, 0, sizeof r);
    snprintf(r.app, sizeof r.app, "%s", app);
    r.dir = dir;
    r.action = act;
    if (fw_parse_dest(dst, &r)) { fprintf(stderr, "invalid destination in the test: %s\n", dst); exit(2); }
    return r;
}

static uint32_t add(FwStore *s, const char *app, FwDir dir, const char *dst, FwAction act) {
    FwRule r = mk(app, dir, dst, act);
    uint32_t id = 0;
    if (fw_rule_add(s, &r, &id)) { fprintf(stderr, "could not add %s %s\n", app, dst); exit(2); }
    return id;
}

static FwQuery qry(const char *app, FwDir dir, const char *ip, uint16_t port, const char *host) {
    static const char *hosts[1];
    FwQuery q;
    memset(&q, 0, sizeof q);
    q.app = app;
    q.dir = dir;
    q.proto = FW_PROTO_TCP;
    q.port = port;
    if (ip) {
        if (strchr(ip, ':')) { q.fam = 6; inet_pton(AF_INET6, ip, q.addr); }
        else { q.fam = 4; inet_pton(AF_INET, ip, q.addr); }
    }
    if (host) { hosts[0] = host; q.hosts = hosts; q.nhosts = 1; }
    return q;
}

static FwDecision dec(FwStore *s, const char *app, const char *ip, uint16_t port, const char *host) {
    FwQuery q = qry(app, FW_OUT, ip, port, host);
    return fw_decide(s, &q);
}

/* ---- ids, destinos, LAN ------------------------------------------------------------------------ */

static void test_canon(void) {
    char o[FW_APP_MAX];
    CHECK(!fw_app_canon("lordos", o, sizeof o) && !strcmp(o, "lordos"));
    CHECK(!fw_app_canon("desktop:org.chromium.Chromium", o, sizeof o) && !strcmp(o, "desktop:org.chromium.Chromium"));
    CHECK(!fw_app_canon("app-lordos-lpk-sudoku.scope", o, sizeof o) && !strcmp(o, "lpk:sudoku"));
    CHECK(!fw_app_canon("app-lordos-org.mozilla.firefox.scope", o, sizeof o) && !strcmp(o, "desktop:org.mozilla.firefox"));
    CHECK(!fw_app_canon("/usr/bin/curl", o, sizeof o) && !strcmp(o, "/usr/bin/curl"));
    const char *bad[] = { "", "desktop:", ":x", "a b", "a\"b", "/usr/../bin/sh", "/usr//bin", "/usr/bin/", "rel/path",
                          "app-lordos-lpk-a:b.scope", "x\n", "\xc3\xa1" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(fw_app_canon(bad[i], o, sizeof o) == -1);
    /* (0.8.5) the components' scopes */
    CHECK(!fw_component_scope("lordos-net-mail.scope", o, sizeof o) && !strcmp(o, "lordos:mail"));
    CHECK(!fw_component_scope("lordos-net-terminal.scope", o, sizeof o) && !strcmp(o, "lordos:terminal"));
    CHECK(!fw_app_canon("lordos:store", o, sizeof o) && !strcmp(o, "lordos:store"));
    const char *badsc[] = { "lordos-net-evil.scope", "lordos-net-.scope", "lordos-net-mail", "lordos-net-mail.scope.scope",
                            "lordos-maild.scope", "app-lordos-mail.scope", "lordos-net-Mail.scope", NULL };
    for (int i = 0; badsc[i]; i++) CHECK(fw_component_scope(badsc[i], o, sizeof o) == -1);
    CHECK(fw_component_ok("lists") && !fw_component_ok("") && !fw_component_ok(NULL) && !fw_component_ok("vpn"));
    char longid[200];
    memset(longid, 'a', sizeof longid - 1);
    longid[sizeof longid - 1] = 0;
    CHECK(fw_app_canon(longid, o, sizeof o) == -1);
    CHECK(fw_app_canon("lordos", o, 3) == -1);
}

static void test_dest(void) {
    const char *ok[][2] = {
        { "any", "any" }, { "lan", "lan" }, { "internet", "internet" },
        { "Example.COM.", "example.com" }, { "*.example.com", "*.example.com" }, { "_dmarc.x.org", "_dmarc.x.org" },
        { "10.0.0.0/8", "10.0.0.0/8" }, { "192.0.2.7", "192.0.2.7" }, { "192.0.2.7/32", "192.0.2.7" },
        { "0.0.0.0/0", "0.0.0.0/0" }, { "2001:db8::/32", "2001:db8::/32" }, { "2001:DB8::1", "2001:db8::1" },
        { "::ffff:10.1.2.0/120", "10.1.2.0/24" }, { "::/0", "::/0" },
    };
    for (size_t i = 0; i < sizeof ok / sizeof *ok; i++) {
        FwRule r;
        memset(&r, 0, sizeof r);
        char out[300];
        int p = fw_parse_dest(ok[i][0], &r);
        CHECK(p == 0);
        CHECK(!p && !fw_format_dest(&r, out, sizeof out) && !strcmp(out, ok[i][1]));
    }
    const char *bad[] = { "", "*", "*.", "*example.com", "a..b", ".a", "a.b.123", "1.2.3.400", "1.2.3", "10.0.0.1/8",
                          "10.0.0.0/33", "10.0.0.0/08", "10.0.0.0/", "2001:db8::1/129", "2001:db8::1/64", "ex ample.com",
                          "a.-b.c\x01", "*.*.com", "ñ.com", "1.2.3.4/-1", "::1::2" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        FwRule r;
        memset(&r, 0, sizeof r);
        int p = fw_parse_dest(bad[i], &r);
        if (p != -1) fprintf(stderr, "  accepted invalid destination: \"%s\"\n", bad[i]);
        CHECK(p == -1);
    }
    char lab[80], name[400];
    memset(lab, 'a', 64); lab[64] = 0;
    snprintf(name, sizeof name, "%s.com", lab);
    FwRule r;
    CHECK(fw_parse_dest(name, &r) == -1);      /* 64-byte label */
    lab[63] = 0;
    snprintf(name, sizeof name, "%s.%s.%s.%s", lab, lab, lab, lab);   /* 255 > 253 */
    CHECK(fw_parse_dest(name, &r) == -1);
}

static int lan_of(const char *ip) {
    uint8_t a[16] = { 0 };
    int fam = strchr(ip, ':') ? 6 : 4;
    inet_pton(fam == 4 ? AF_INET : AF_INET6, ip, a);
    return fw_addr_is_lan(fam, a);
}

static void test_lan(void) {
    const char *lan[] = { "10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.1.1", "127.0.0.1", "169.254.3.4",
                          "100.64.0.1", "224.0.0.251", "255.255.255.255", "0.0.0.0", "::1", "::", "fe80::1",
                          "fd12:3456::1", "fc00::1", "ff02::fb", "::ffff:192.168.0.1" };
    const char *net[] = { "8.8.8.8", "172.15.255.255", "172.32.0.0", "192.169.0.1", "100.128.0.1", "1.1.1.1",
                          "2001:4860:4860::8888", "fec0::1", "::ffff:8.8.8.8", "11.0.0.1", "223.255.255.255" };
    for (size_t i = 0; i < sizeof lan / sizeof *lan; i++) CHECK(lan_of(lan[i]) == 1);
    for (size_t i = 0; i < sizeof net / sizeof *net; i++) CHECK(lan_of(net[i]) == 0);
}

/* ---- precedencia ------------------------------------------------------------------------------- */

static void test_mode_and_trusted(void) {
    FwStore *s = fw_store_new();
    CHECK(fw_mode(s) == FW_MODE_SILENT);
    FwDecision d = dec(s, "desktop:a", "8.8.8.8", 443, NULL);
    CHECK(d.verdict == FW_V_ALLOW && d.reason == FW_BY_MODE && d.record == 1);
    fw_set_mode(s, FW_MODE_ASK);
    d = dec(s, "desktop:a", "8.8.8.8", 443, NULL);
    CHECK(d.verdict == FW_V_ASK && d.reason == FW_BY_MODE && !d.record);
    fw_set_mode(s, FW_MODE_OFF);
    d = dec(s, "desktop:a", "8.8.8.8", 443, NULL);
    CHECK(d.verdict == FW_V_ALLOW && d.reason == FW_BY_MODE && !d.record);
    fw_set_mode(s, (FwMode)7);                 /* invalid: no change */
    CHECK(fw_mode(s) == FW_MODE_OFF);

    fw_set_mode(s, FW_MODE_ASK);
    CHECK(!fw_set_trusted(s, "desktop:a", 1));
    d = dec(s, "desktop:a", "8.8.8.8", 443, NULL);
    CHECK(d.verdict == FW_V_ALLOW && d.reason == FW_BY_TRUSTED && !d.record);
    /* an explicit rule still applies to a trusted app */
    add(s, "desktop:a", FW_OUT, "8.8.8.8", FW_DENY);
    d = dec(s, "desktop:a", "8.8.8.8", 443, NULL);
    CHECK(d.verdict == FW_V_DENY && d.reason == FW_BY_RULE);
    /* trusting and restricting are mutually exclusive */
    CHECK(!fw_set_restricted(s, "desktop:a", FW_R_NO_CAMERA));
    CHECK(!fw_is_trusted(s, "desktop:a") && fw_restricted(s, "desktop:a") == FW_R_NO_CAMERA);
    CHECK(!fw_set_trusted(s, "desktop:a", 1));
    CHECK(fw_is_trusted(s, "desktop:a") && fw_restricted(s, "desktop:a") == 0);
    CHECK(fw_set_restricted(s, "desktop:a", 1u << 20) == -1);
    CHECK(fw_set_trusted(s, "bad id", 1) == -1);
    /* without a valid app: denied */
    d = dec(s, "", "8.8.8.8", 443, NULL);
    CHECK(d.verdict == FW_V_DENY);
    /* the cgroup scope is the same app */
    d = dec(s, "app-lordos-a.scope", "8.8.8.8", 443, NULL);
    CHECK(d.verdict == FW_V_DENY && d.reason == FW_BY_RULE);
    fw_store_free(s);
}

/* 0.8.5: the deny mode ("deny silently") and subscribed blocklists (FwQuery.listed) */
static FwDecision decl(FwStore *s, const char *app, const char *ip, const char *host, int listed) {
    FwQuery q = qry(app, FW_OUT, ip, 443, host);
    q.listed = listed;
    return fw_decide(s, &q);
}

static void test_deny_mode_and_lists(void) {
    FwStore *s = fw_store_new();
    fw_set_mode(s, FW_MODE_DENY);
    CHECK(fw_mode(s) == FW_MODE_DENY);
    FwDecision d = dec(s, "desktop:a", "8.8.8.8", 443, NULL);
    CHECK(d.verdict == FW_V_DENY && d.reason == FW_BY_MODE && d.record == 1);
    d = dec(s, "desktop:a", "192.168.1.1", 443, NULL);
    CHECK(d.verdict == FW_V_DENY && d.reason == FW_BY_MODE);
    d = dec(s, "desktop:a", "127.0.0.53", 53, NULL);               /* the resolver's stub: loopback stays */
    CHECK(d.verdict == FW_V_ALLOW && d.reason == FW_BY_MODE);
    d = dec(s, "desktop:a", "::1", 631, NULL);
    CHECK(d.verdict == FW_V_ALLOW);
    d = dec(s, "desktop:a", "::ffff:127.0.0.1", 631, NULL);
    CHECK(d.verdict == FW_V_ALLOW);
    add(s, "desktop:a", FW_OUT, "example.org", FW_ALLOW);           /* a rule still allows */
    d = dec(s, "desktop:a", "203.0.113.5", 443, "example.org");
    CHECK(d.verdict == FW_V_ALLOW && d.reason == FW_BY_RULE);
    CHECK(!fw_set_trusted(s, "desktop:t", 1));                    /* trusted apps keep the network */
    d = dec(s, "desktop:t", "8.8.8.8", 443, NULL);
    CHECK(d.verdict == FW_V_ALLOW && d.reason == FW_BY_TRUSTED);
    /* the mode survives the file */
    size_t n;
    char *j = fw_to_json(s, &n);
    CHECK(j && strstr(j, "\"mode\":\"deny\""));
    FwStore *t = fw_store_new();
    char err[128];
    CHECK(j && !fw_from_json(t, j, n, err, sizeof err) && fw_mode(t) == FW_MODE_DENY);
    free(j);
    fw_store_free(t);
    fw_store_free(s);

    /* lists: deny before lan/internet/any rules, never before a rule naming the destination */
    s = fw_store_new();
    const char *A = "desktop:b";
    d = decl(s, A, "203.0.113.9", "tracker.example", 1);
    CHECK(d.verdict == FW_V_DENY && d.reason == FW_BY_LIST && d.record == 1);
    d = decl(s, A, "203.0.113.9", "tracker.example", 0);
    CHECK(d.verdict == FW_V_ALLOW && d.reason == FW_BY_MODE);
    add(s, A, FW_OUT, "any", FW_ALLOW);
    add(s, A, FW_OUT, "internet", FW_ALLOW);
    d = decl(s, A, "203.0.113.9", "tracker.example", 1);
    CHECK(d.verdict == FW_V_DENY && d.reason == FW_BY_LIST);       /* broad rules don't override a list */
    uint32_t mine = add(s, A, FW_OUT, "tracker.example", FW_ALLOW);
    d = decl(s, A, "203.0.113.9", "tracker.example", 1);
    CHECK(d.verdict == FW_V_ALLOW && d.reason == FW_BY_RULE && d.rule_id == mine);   /* the owner's allow wins */
    uint32_t net = add(s, A, FW_OUT, "198.51.100.0/24", FW_ALLOW);
    d = decl(s, A, "198.51.100.7", NULL, 1);
    CHECK(d.verdict == FW_V_ALLOW && d.rule_id == net);
    add(s, A, FW_OUT, "*.ads.example", FW_DENY);
    d = decl(s, A, "203.0.113.10", "x.ads.example", 1);
    CHECK(d.verdict == FW_V_DENY && d.reason == FW_BY_RULE);       /* the owner's own deny still says so */
    CHECK(!fw_set_trusted(s, A, 1));
    d = decl(s, "desktop:c", "203.0.113.9", NULL, 1);
    CHECK(d.verdict == FW_V_DENY && d.reason == FW_BY_LIST);
    CHECK(!fw_set_trusted(s, "desktop:c", 1));                    /* lists apply to trusted apps too */
    d = decl(s, "desktop:c", "203.0.113.9", NULL, 1);
    CHECK(d.verdict == FW_V_DENY && d.reason == FW_BY_LIST);
    CHECK(!fw_set_restricted(s, "desktop:d", FW_R_NO_INTERNET));  /* a restriction comes first */
    d = decl(s, "desktop:d", "203.0.113.9", NULL, 1);
    CHECK(d.verdict == FW_V_DENY && d.reason == FW_BY_RESTRICTED);
    fw_store_free(s);
}

static void test_precedence(void) {
    FwStore *s = fw_store_new();
    fw_set_mode(s, FW_MODE_ASK);
    const char *A = "desktop:org.chromium.Chromium";

    /* allow and deny for the same destination: the last decision replaces it (same id) */
    uint32_t i1 = add(s, A, FW_OUT, "any", FW_ALLOW);
    uint32_t i2 = add(s, A, FW_OUT, "any", FW_DENY);
    CHECK(i1 == i2 && fw_rule_count(s) == 1);
    CHECK(dec(s, A, "1.1.1.1", 80, NULL).verdict == FW_V_DENY);

    /* the most specific wins: allowed domain over "deny all" */
    uint32_t dom = add(s, A, FW_OUT, "api.example.com", FW_ALLOW);
    FwDecision d = dec(s, A, "93.184.216.34", 443, "api.example.com");
    CHECK(d.verdict == FW_V_ALLOW && d.rule_id == dom);
    CHECK(dec(s, A, "93.184.216.34", 443, "www.example.com").verdict == FW_V_DENY);

    /* an exact domain beats the wildcard, and a longer suffix beats a shorter one */
    uint32_t w1 = add(s, A, FW_OUT, "*.example.com", FW_DENY);
    uint32_t w2 = add(s, A, FW_OUT, "*.cdn.example.com", FW_ALLOW);
    CHECK(dec(s, A, "93.184.216.34", 443, "api.example.com").rule_id == dom);
    CHECK(dec(s, A, "93.184.216.34", 443, "x.example.com").rule_id == w1);
    CHECK(dec(s, A, "93.184.216.34", 443, "example.com").rule_id == w1);    /* the wildcard covers the suffix */
    CHECK(dec(s, A, "93.184.216.34", 443, "img.cdn.example.com").rule_id == w2);
    CHECK(dec(s, A, "93.184.216.34", 443, "API.Example.COM.").rule_id == dom);
    CHECK(dec(s, A, "93.184.216.34", 443, "notexample.com").verdict == FW_V_DENY);  /* "any" */
    CHECK(dec(s, A, "93.184.216.34", 443, "notexample.com").rule_id == i1);

    /* networks: the smallest wins; with a domain and a network at once, the domain wins */
    uint32_t c8 = add(s, A, FW_OUT, "10.0.0.0/8", FW_ALLOW);
    uint32_t c24 = add(s, A, FW_OUT, "10.1.2.0/24", FW_DENY);
    uint32_t c32 = add(s, A, FW_OUT, "10.1.2.3", FW_ALLOW);
    CHECK(dec(s, A, "10.9.9.9", 80, NULL).rule_id == c8);
    CHECK(dec(s, A, "10.1.2.9", 80, NULL).rule_id == c24);
    CHECK(dec(s, A, "10.1.2.3", 80, NULL).rule_id == c32);
    CHECK(dec(s, A, "::ffff:10.1.2.9", 80, NULL).rule_id == c24);           /* IPv4 mapeada */
    CHECK(dec(s, A, "10.1.2.9", 80, "api.example.com").rule_id == dom);
    uint32_t v6a = add(s, A, FW_OUT, "2001:db8::/32", FW_DENY);
    uint32_t v6b = add(s, A, FW_OUT, "2001:db8:1::/48", FW_ALLOW);
    uint32_t v6c = add(s, A, FW_OUT, "::/0", FW_ALLOW);
    CHECK(dec(s, A, "2001:db8:2::1", 80, NULL).rule_id == v6a);
    CHECK(dec(s, A, "2001:db8:1::1", 80, NULL).rule_id == v6b);
    CHECK(dec(s, A, "2a00::1", 80, NULL).rule_id == v6c);
    CHECK(dec(s, A, "2a00::1", 80, NULL).verdict == FW_V_ALLOW);

    /* LAN / internet sit between the network and "any" */
    uint32_t lan = add(s, A, FW_OUT, "lan", FW_ALLOW);
    uint32_t inet = add(s, A, FW_OUT, "internet", FW_DENY);
    CHECK(dec(s, A, "192.168.1.5", 80, NULL).rule_id == lan);
    CHECK(dec(s, A, "8.8.8.8", 80, NULL).rule_id == inet);
    CHECK(dec(s, A, "10.9.9.9", 80, NULL).rule_id == c8);                   /* the network is more specific */
    CHECK(dec(s, A, NULL, 80, NULL).rule_id == i1);                         /* no address: only "any" */

    /* at equal specificity, deny beats allow: two names for the same IP */
    FwStore *t = fw_store_new();
    uint32_t ok = add(t, A, FW_OUT, "good.example", FW_ALLOW);
    uint32_t no = add(t, A, FW_OUT, "evil.example", FW_DENY);
    const char *both[] = { "good.example", "evil.example" };
    FwQuery q = qry(A, FW_OUT, "203.0.113.5", 443, NULL);
    q.hosts = both; q.nhosts = 2;
    d = fw_decide(t, &q);
    CHECK(d.verdict == FW_V_DENY && d.rule_id == no);
    (void)ok;
    /* a specific port beats any port, and a specific protocol beats "any" */
    FwRule r = mk(A, FW_OUT, "203.0.113.5", FW_ALLOW);
    r.port_lo = r.port_hi = 443;
    uint32_t p443;
    CHECK(!fw_rule_add(t, &r, &p443));
    r.port_lo = r.port_hi = 0; r.action = FW_DENY;
    uint32_t pany;
    CHECK(!fw_rule_add(t, &r, &pany));
    CHECK(p443 != pany);
    q.hosts = NULL; q.nhosts = 0;
    q.port = 443; CHECK(fw_decide(t, &q).rule_id == p443);
    q.port = 80;  CHECK(fw_decide(t, &q).rule_id == pany);
    r.port_lo = 1; r.port_hi = 1024; r.action = FW_ALLOW; r.proto = FW_PROTO_UDP;
    uint32_t pudp;
    CHECK(!fw_rule_add(t, &r, &pudp));
    q.proto = FW_PROTO_UDP; q.port = 53; CHECK(fw_decide(t, &q).rule_id == pudp);
    q.proto = FW_PROTO_TCP; q.port = 53; CHECK(fw_decide(t, &q).rule_id == pany);
    /* direction (outbound/inbound) separates the rules */
    FwQuery qi = qry(A, FW_IN, "203.0.113.5", 443, NULL);
    CHECK(fw_decide(t, &qi).reason == FW_BY_MODE);
    fw_store_free(t);

    /* another app does not see these rules */
    CHECK(dec(s, "desktop:other", "10.1.2.3", 80, NULL).reason == FW_BY_MODE);

    /* sacar reglas */
    CHECK(!fw_rule_remove(s, c32));
    CHECK(dec(s, A, "10.1.2.3", 80, NULL).rule_id == c24);
    CHECK(fw_rule_remove(s, c32) == -1);
    CHECK(fw_rule_get(s, c24) && fw_rule_get(s, c24)->prefix == 24);
    CHECK(!fw_rule_remove(s, c24) && !fw_rule_remove(s, c8));
    CHECK(dec(s, A, "10.1.2.3", 80, NULL).rule_id == lan);
    FwRule list[64];
    int n = fw_rule_list(s, A, list, 64);
    CHECK(n == (int)fw_rule_count(s));
    for (int i = 1; i < n; i++) CHECK(list[i - 1].id < list[i].id);
    CHECK(fw_rule_list(s, "desktop:nobody", list, 64) == 0);
    fw_app_forget(s, A);
    CHECK(fw_rule_count(s) == 0);
    CHECK(dec(s, A, "8.8.8.8", 80, NULL).reason == FW_BY_MODE);
    fw_store_free(s);
}

static void test_restricted(void) {
    FwStore *s = fw_store_new();
    const char *A = "lpk:notes";
    add(s, A, FW_OUT, "any", FW_ALLOW);
    add(s, A, FW_IN, "any", FW_ALLOW);
    CHECK(dec(s, A, "8.8.8.8", 443, NULL).verdict == FW_V_ALLOW);

    CHECK(!fw_set_restricted(s, A, FW_R_NO_INTERNET));
    FwDecision d = dec(s, A, "8.8.8.8", 443, NULL);
    CHECK(d.verdict == FW_V_DENY && d.reason == FW_BY_RESTRICTED && d.flag == FW_R_NO_INTERNET);
    CHECK(dec(s, A, "192.168.0.10", 443, NULL).verdict == FW_V_ALLOW);
    CHECK(dec(s, A, NULL, 443, "example.com").verdict == FW_V_DENY);       /* no address = internet */

    CHECK(!fw_set_restricted(s, A, FW_R_NO_LAN));
    CHECK(dec(s, A, "192.168.0.10", 443, NULL).flag == FW_R_NO_LAN);
    CHECK(dec(s, A, "127.0.0.1", 443, NULL).flag == FW_R_NO_LAN);
    CHECK(dec(s, A, "fe80::1", 443, NULL).flag == FW_R_NO_LAN);
    CHECK(dec(s, A, "8.8.8.8", 443, NULL).verdict == FW_V_ALLOW);

    CHECK(!fw_set_restricted(s, A, FW_R_NO_INCOMING));
    FwQuery qi = qry(A, FW_IN, "192.168.0.10", 8080, NULL);
    CHECK(fw_decide(s, &qi).flag == FW_R_NO_INCOMING);
    CHECK(dec(s, A, "192.168.0.10", 443, NULL).verdict == FW_V_ALLOW);

    CHECK(!fw_set_restricted(s, A, FW_R_NO_NETWORK | FW_R_NO_MIC));
    CHECK(dec(s, A, "192.168.0.10", 443, NULL).flag == FW_R_NO_NETWORK);
    CHECK(fw_decide(s, &qi).flag == FW_R_NO_NETWORK);
    CHECK(fw_blocks(s, A, FW_R_NO_MIC) && !fw_blocks(s, A, FW_R_NO_CAMERA));

    uint32_t others[] = { FW_R_NO_CAMERA, FW_R_NO_MIC, FW_R_NO_SCREEN, FW_R_NO_USB, FW_R_NO_BLUETOOTH, FW_R_NO_FILES, FW_R_NO_NOTIFICATIONS };
    for (size_t i = 0; i < sizeof others / sizeof *others; i++) {
        CHECK(!fw_set_restricted(s, A, others[i]));
        CHECK(fw_blocks(s, A, others[i]));
        CHECK(dec(s, A, "8.8.8.8", 443, NULL).verdict == FW_V_ALLOW);       /* they don't touch the network */
        CHECK(fw_flag_from_name(fw_flag_name(others[i])) == others[i]);
    }
    CHECK(!fw_set_restricted(s, A, 0));
    CHECK(!fw_restricted(s, A));
    CHECK(fw_flag_from_name("no-such") == 0 && fw_flag_name(3) == NULL);
    fw_store_free(s);
}

static void test_lifetimes(void) {
    FwStore *s = fw_store_new();
    fw_set_mode(s, FW_MODE_ASK);
    const char *A = "desktop:x";
    FwRule r = mk(A, FW_OUT, "example.org", FW_ALLOW);
    r.life = FW_LIFE_ONCE;
    uint32_t once;
    CHECK(!fw_rule_add(s, &r, &once));
    FwDecision d = dec(s, A, "198.51.100.1", 443, "example.org");
    CHECK(d.verdict == FW_V_ALLOW && d.rule_id == once);
    CHECK(!fw_rule_get(s, once) && fw_rule_count(s) == 0);
    CHECK(dec(s, A, "198.51.100.1", 443, "example.org").verdict == FW_V_ASK);

    r = mk(A, FW_OUT, "198.51.100.0/24", FW_DENY);
    r.life = FW_LIFE_UNTIL_QUIT;
    uint32_t q1;
    CHECK(!fw_rule_add(s, &r, &q1));
    uint32_t keep = add(s, A, FW_OUT, "lan", FW_ALLOW);
    r = mk("desktop:y", FW_OUT, "any", FW_DENY);
    r.life = FW_LIFE_UNTIL_QUIT;
    uint32_t qy;
    CHECK(!fw_rule_add(s, &r, &qy));
    CHECK(dec(s, A, "198.51.100.1", 443, NULL).rule_id == q1);
    CHECK(dec(s, A, "198.51.100.1", 443, NULL).rule_id == q1);      /* not used up */
    char *json = fw_to_json(s, NULL);
    CHECK(json && !strstr(json, "198.51.100") && strstr(json, "\"lan\""));
    free(json);
    fw_app_quit(s, A);
    CHECK(!fw_rule_get(s, q1) && fw_rule_get(s, keep) && fw_rule_get(s, qy));
    CHECK(dec(s, A, "198.51.100.1", 443, NULL).verdict == FW_V_ASK);
    fw_app_quit(s, "desktop:y");
    CHECK(fw_rule_count(s) == 1);

    r = mk(A, FW_OUT, "any", FW_ALLOW);
    r.life = (FwLife)9;
    CHECK(fw_rule_add(s, &r, NULL) == -1);
    r.life = FW_LIFE_FOREVER; r.dir = (FwDir)5;
    CHECK(fw_rule_add(s, &r, NULL) == -1);
    r.dir = FW_OUT; r.port_lo = 10; r.port_hi = 5;
    CHECK(fw_rule_add(s, &r, NULL) == -1);
    r.port_lo = 0; r.port_hi = 0; r.kind = FW_DST_CIDR; r.fam = 4; r.prefix = 8; r.addr[0] = 10; r.addr[3] = 1;
    CHECK(fw_rule_add(s, &r, NULL) == -1);       /* host bits */
    fw_store_free(s);
}

/* ---- JSON -------------------------------------------------------------------------------------- */

static FwStore *sample(void) {
    FwStore *s = fw_store_new();
    fw_set_mode(s, FW_MODE_ASK);
    fw_set_trusted(s, "lordos", 1);
    fw_set_trusted(s, "desktop:org.gnome.Maps", 1);
    fw_set_restricted(s, "lpk:game", FW_R_NO_INTERNET | FW_R_NO_CAMERA | FW_R_NO_NOTIFICATIONS);
    add(s, "desktop:org.chromium.Chromium", FW_OUT, "any", FW_ALLOW);
    add(s, "desktop:org.chromium.Chromium", FW_OUT, "*.doubleclick.net", FW_DENY);
    FwRule r = mk("/usr/bin/curl", FW_OUT, "2001:db8::/32", FW_DENY);
    r.port_lo = 8000; r.port_hi = 8080; r.proto = FW_PROTO_TCP; r.when = 1760000000;
    fw_rule_add(s, &r, NULL);
    r = mk("lpk:game", FW_IN, "lan", FW_ALLOW);
    r.port_lo = r.port_hi = 27015; r.proto = FW_PROTO_UDP;
    fw_rule_add(s, &r, NULL);
    add(s, "app-lordos-lpk-game.scope", FW_OUT, "10.0.0.0/8", FW_ALLOW);
    return s;
}

static void test_json(void) {
    FwStore *s = sample();
    size_t n1;
    char *j1 = fw_to_json(s, &n1);
    CHECK(j1 && n1 == strlen(j1));
    FwStore *t = fw_store_new();
    char err[200];
    CHECK(!fw_from_json(t, j1, n1, err, sizeof err));
    char *j2 = fw_to_json(t, NULL);
    CHECK(j2 && !strcmp(j1, j2));
    CHECK(fw_mode(t) == FW_MODE_ASK && fw_is_trusted(t, "lordos") && fw_blocks(t, "lpk:game", FW_R_NO_CAMERA));
    CHECK(dec(t, "desktop:org.chromium.Chromium", "1.2.3.4", 443, "ad.doubleclick.net").verdict == FW_V_DENY);
    CHECK(dec(t, "desktop:org.chromium.Chromium", "1.2.3.4", 443, "example.com").verdict == FW_V_ALLOW);
    CHECK(dec(t, "lpk:game", "10.2.3.4", 443, NULL).verdict == FW_V_ALLOW);
    free(j2);

    /* file */
    char path[256];
    const char *tmpdir = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/fw_rules_test.%d.json", tmpdir && *tmpdir ? tmpdir : "/tmp", (int)getpid());
    CHECK(!fw_save(s, path));
    FwStore *u = fw_store_new();
    CHECK(!fw_load(u, path, err, sizeof err));
    char *j3 = fw_to_json(u, NULL);
    CHECK(j3 && !strcmp(j1, j3));
    free(j3);
    unlink(path);
    CHECK(fw_load(u, path, err, sizeof err) == 1);         /* no file: stays the same */
    CHECK(fw_rule_count(u) == fw_rule_count(s));
    fw_store_free(u);

    /* bad input: rejected, and the store does not change */
    const char *bad[] = {
        "", "[]", "null", "{", "{\"v\":2}", "{}", "{\"v\":\"1\"}", "{\"v\":1,\"x\":1}", "{\"v\":1}{}",
        "{\"v\":1,\"mode\":\"loud\"}", "{\"v\":1,\"mode\":1}",
        "{\"v\":1,\"trusted\":\"lordos\"}", "{\"v\":1,\"trusted\":[1]}", "{\"v\":1,\"trusted\":[\"a b\"]}",
        "{\"v\":1,\"trusted\":[\"lordos\",\"lordos\"]}", "{\"v\":1,\"trusted\":[\"app-lordos-x.scope\"]}",
        "{\"v\":1,\"restricted\":[]}", "{\"v\":1,\"restricted\":{\"lpk:a\":[]}}",
        "{\"v\":1,\"restricted\":{\"lpk:a\":[\"no-wifi\"]}}", "{\"v\":1,\"restricted\":{\"lpk:a\":[\"no-mic\",\"no-mic\"]}}",
        "{\"v\":1,\"restricted\":{\"lpk:a\":\"no-mic\"}}", "{\"v\":1,\"trusted\":[\"lpk:a\"],\"restricted\":{\"lpk:a\":[\"no-mic\"]}}",
        "{\"v\":1,\"rules\":{}}", "{\"v\":1,\"rules\":[1]}", "{\"v\":1,\"rules\":[{}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"any\"}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"any\",\"action\":\"maybe\"}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"10.0.0.1/8\",\"action\":\"allow\"}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"any\",\"action\":\"allow\",\"dir\":\"up\"}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"any\",\"action\":\"allow\",\"port\":\"0\"}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"any\",\"action\":\"allow\",\"port\":\"9-1\"}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"any\",\"action\":\"allow\",\"port\":\"65536\"}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"any\",\"action\":\"allow\",\"port\":1.5}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"any\",\"action\":\"allow\",\"proto\":\"icmp\"}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"any\",\"action\":\"allow\",\"when\":-1}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"any\",\"action\":\"allow\",\"life\":\"once\"}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"any\",\"action\":\"allow\",\"app\":\"lordos\"}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"*.\",\"action\":\"allow\"}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lord\\u0000os\",\"dst\":\"any\",\"action\":\"allow\"}]}",
        "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"any\",\"action\":\"allow\"},]}",
    };
    size_t before = fw_rule_count(t);
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        int r = fw_from_json(t, bad[i], strlen(bad[i]), err, sizeof err);
        if (r != -1) fprintf(stderr, "  accepted invalid JSON %zu: %s\n", i, bad[i]);
        CHECK(r == -1 && err[0]);
    }
    CHECK(fw_from_json(t, "{\"v\":1}\0", 8, err, sizeof err) == -1);
    CHECK(fw_rule_count(t) == before && fw_mode(t) == FW_MODE_ASK);
    /* the minimum is valid and leaves everything empty */
    CHECK(!fw_from_json(t, "{\"v\":1}", 7, err, sizeof err) && fw_rule_count(t) == 0 && fw_mode(t) == FW_MODE_SILENT);
    const char *num = "{\"v\":1,\"rules\":[{\"app\":\"lordos\",\"dst\":\"any\",\"action\":\"deny\",\"port\":443}]}";
    CHECK(!fw_from_json(t, num, strlen(num), err, sizeof err) && fw_rule_count(t) == 1);
    CHECK(dec(t, "lordos", "1.2.3.4", 443, NULL).verdict == FW_V_DENY && dec(t, "lordos", "1.2.3.4", 80, NULL).reason == FW_BY_MODE);

    /* random mutations of the good JSON: never crashes; if accepted, it is written and read back */
    srand(1234);
    char *m = malloc(n1 + 1);
    int accepted = 0;
    for (int it = 0; it < 20000; it++) {
        memcpy(m, j1, n1 + 1);
        size_t len = n1;
        int k = 1 + rand() % 4;
        for (int x = 0; x < k && len; x++) {
            size_t p = (size_t)rand() % len;
            switch (rand() % 3) {
            case 0: m[p] = (char)(rand() % 256); break;
            case 1: m[p] = "{}[]\",:0a-*./\\"[rand() % 14]; break;
            case 2: len = p; break;
            }
        }
        FwStore *f = fw_store_new();
        if (!fw_from_json(f, m, len, err, sizeof err)) {
            accepted++;
            char *again = fw_to_json(f, NULL);
            FwStore *g = fw_store_new();
            CHECK(again && !fw_from_json(g, again, strlen(again), err, sizeof err));
            char *again2 = fw_to_json(g, NULL);
            CHECK(again2 && !strcmp(again, again2));
            free(again); free(again2);
            fw_store_free(g);
        }
        fw_store_free(f);
    }
    printf("  JSON: 20000 mutations, %d accepted and consistent\n", accepted);
    free(m);
    free(j1);
    fw_store_free(t);
    fw_store_free(s);
}

/* ---- random against a linear version --------------------------------------------------------- */

static int labels_of(const char *d) { int n = 1; for (; *d; d++) n += *d == '.'; return n; }

static int ref_dst(const FwRule *r, const FwQuery *q, int lan) {
    switch (r->kind) {
    case FW_DST_ANY: return 0;
    case FW_DST_LAN: return lan == 1 ? 500 : -1;
    case FW_DST_INTERNET: return lan == 0 ? 500 : -1;
    case FW_DST_CIDR: {
        if (r->fam != q->fam) return -1;
        for (int i = 0; i < r->prefix; i++)
            if ((r->addr[i >> 3] ^ q->addr[i >> 3]) >> (7 - (i & 7)) & 1) return -1;
        return 1000 + r->prefix;
    }
    case FW_DST_DOMAIN: {
        int best = -1;
        for (int h = 0; h < q->nhosts; h++) {
            const char *n = q->hosts[h];
            size_t nl = strlen(n), dl = strlen(r->domain);
            if (!r->wildcard) { if (!strcmp(n, r->domain) && 3000 + labels_of(n) > best) best = 3000 + labels_of(n); }
            else if (!strcmp(n, r->domain) || (nl > dl && n[nl - dl - 1] == '.' && !strcmp(n + nl - dl, r->domain))) {
                if (2000 + labels_of(r->domain) > best) best = 2000 + labels_of(r->domain);
            }
        }
        return best;
    }
    }
    return -1;
}

static uint32_t ref_decide(const FwRule *rules, int n, const FwQuery *q) {
    int lan = q->fam ? fw_addr_is_lan(q->fam, q->addr) : -1;
    const FwRule *best = NULL;
    long bs[5] = { 0 };
    for (int i = 0; i < n; i++) {
        const FwRule *r = &rules[i];
        if (strcmp(r->app, q->app) || r->dir != q->dir) continue;
        if (r->proto != FW_PROTO_ANY && r->proto != q->proto) continue;
        if (q->port < r->port_lo || q->port > r->port_hi) continue;
        int d = ref_dst(r, q, lan);
        if (d < 0) continue;
        long sc[5] = { d, 65535 - (r->port_hi - r->port_lo), r->proto != FW_PROTO_ANY, r->action == FW_DENY, (long)r->id };
        int better = !best;
        for (int k = 0; !better && k < 5; k++) { if (sc[k] != bs[k]) { better = sc[k] > bs[k]; break; } }
        if (better) { best = r; memcpy(bs, sc, sizeof bs); }
    }
    return best ? best->id : 0;
}

static void test_random(void) {
    static const char *apps[] = { "lordos", "desktop:a", "lpk:b" };
    static const char *names[] = { "a.com", "b.a.com", "c.b.a.com", "x.org", "y.x.org", "com", "org", "z.net" };
    FwStore *s = fw_store_new();
    fw_set_mode(s, FW_MODE_ASK);
    srand(42);
    FwRule *all = malloc(sizeof *all * 4096);
    long mism = 0, total = 0;
    for (int it = 0; it < 30000; it++) {
        int op = rand() % 10;
        if (op < 4) {
            FwRule r;
            memset(&r, 0, sizeof r);
            snprintf(r.app, sizeof r.app, "%s", apps[rand() % 3]);
            r.dir = (FwDir)(rand() % 2);
            r.action = (FwAction)(rand() % 2);
            r.proto = (FwProto)(rand() % 3);
            if (rand() % 2) { r.port_lo = (uint16_t)(rand() % 4 * 100); r.port_hi = (uint16_t)(r.port_lo + rand() % 3 * 100); }
            char dst[80];
            switch (rand() % 6) {
            case 0: snprintf(dst, sizeof dst, "any"); break;
            case 1: snprintf(dst, sizeof dst, rand() % 2 ? "lan" : "internet"); break;
            case 2: snprintf(dst, sizeof dst, "%s", names[rand() % 8]); break;
            case 3: snprintf(dst, sizeof dst, "*.%s", names[rand() % 8]); break;
            case 4: {
                int p = rand() % 33;
                uint32_t a = (uint32_t)(rand() % 4 == 0 ? 0x0a000000u : 0xc0a80000u) | (uint32_t)(rand() % 65536);
                if (p < 32) a &= p ? ~0u << (32 - p) : 0;
                snprintf(dst, sizeof dst, "%u.%u.%u.%u/%d", a >> 24, a >> 16 & 255, a >> 8 & 255, a & 255, p);
                break;
            }
            default: {
                int p = rand() % 129;
                uint8_t a[16] = { 0x20, 0x01, 0x0d, 0xb8 };
                a[4] = (uint8_t)(rand() % 4); a[15] = (uint8_t)(rand() % 4);
                for (int i = p; i < 128; i++) a[i >> 3] &= (uint8_t)~(0x80 >> (i & 7));
                char ip[64];
                inet_ntop(AF_INET6, a, ip, sizeof ip);
                snprintf(dst, sizeof dst, "%s/%d", ip, p);
            }
            }
            if (fw_parse_dest(dst, &r)) { fprintf(stderr, "dst %s\n", dst); fails++; continue; }
            r.life = rand() % 8 == 0 ? FW_LIFE_ONCE : FW_LIFE_FOREVER;
            CHECK(!fw_rule_add(s, &r, NULL));
        } else if (op < 5) {
            int n = fw_rule_list(s, NULL, all, 4096);
            if (n > 0) CHECK(!fw_rule_remove(s, all[rand() % (n < 4096 ? n : 4096)].id));
        } else {
            int n = fw_rule_list(s, NULL, all, 4096);
            if (n > 4096) n = 4096;
            FwQuery q;
            memset(&q, 0, sizeof q);
            q.app = apps[rand() % 3];
            q.dir = (FwDir)(rand() % 2);
            q.proto = rand() % 2 ? FW_PROTO_TCP : FW_PROTO_UDP;
            q.port = (uint16_t)(rand() % 700);
            const char *hs[2] = { names[rand() % 8], names[rand() % 8] };
            q.hosts = hs;
            q.nhosts = rand() % 3;
            int k = rand() % 3;
            if (k == 1) {
                q.fam = 4;
                uint32_t a = (uint32_t)(rand() % 3 == 0 ? 0x0a000000u : rand() % 2 ? 0xc0a80000u : 0x08080000u) | (uint32_t)(rand() % 65536);
                q.addr[0] = (uint8_t)(a >> 24); q.addr[1] = (uint8_t)(a >> 16); q.addr[2] = (uint8_t)(a >> 8); q.addr[3] = (uint8_t)a;
            } else if (k == 2) {
                q.fam = 6;
                uint8_t a[16] = { 0x20, 0x01, 0x0d, 0xb8 };
                a[4] = (uint8_t)(rand() % 4); a[15] = (uint8_t)(rand() % 4);
                memcpy(q.addr, a, 16);
            }
            uint32_t want = ref_decide(all, n, &q);
            FwDecision d = fw_decide(s, &q);
            uint32_t got = d.reason == FW_BY_RULE ? d.rule_id : 0;
            total++;
            if (want != got) { mism++; if (mism < 5) fprintf(stderr, "  random: expected rule %u, got %u\n", want, got); }
        }
    }
    CHECK(mism == 0);
    printf("  random: %ld decisions against the linear version, %ld different, %zu rules at the end\n", total, mism, fw_rule_count(s));
    free(all);
    fw_store_free(s);
}

static void bench(void) {
    FwStore *s = fw_store_new();
    char app[64], dst[64];
    for (int i = 0; i < 50000; i++) {
        snprintf(app, sizeof app, "desktop:app%d", i % 500);
        if (i % 3 == 0) snprintf(dst, sizeof dst, "h%d.example%d.com", i, i % 7);
        else if (i % 3 == 1) snprintf(dst, sizeof dst, "*.s%d.net", i);
        else snprintf(dst, sizeof dst, "10.%d.%d.0/24", i / 256 % 256, i % 256);
        add(s, app, FW_OUT, dst, i % 2 ? FW_ALLOW : FW_DENY);
    }
    const char *h[] = { "a.b.c.s4.net" };
    FwQuery q = qry("desktop:app4", FW_OUT, "10.0.4.9", 443, NULL);
    q.hosts = h; q.nhosts = 1;
    clock_t t0 = clock();
    unsigned long sum = 0;
    for (int i = 0; i < 1000000; i++) sum += fw_decide(s, &q).rule_id;
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    printf("  measurement: 50000 rules, 1e6 decisions in %.0f ms (%.3f us each)\n", ms, ms / 1000.0);
    CHECK(sum == 1000000ul * fw_decide(s, &q).rule_id && sum);
    fw_store_free(s);
}

int main(void) {
    test_canon();
    test_dest();
    test_lan();
    test_mode_and_trusted();
    test_deny_mode_and_lists();
    test_precedence();
    test_restricted();
    test_lifetimes();
    test_json();
    test_random();
    bench();
    printf("fw-rules-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
