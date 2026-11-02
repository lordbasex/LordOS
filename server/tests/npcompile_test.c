/* npcompile_test.c — from rules to BPF maps (src/netperm/npcompile.c), without a kernel.
 *
 * Builds random stores (domains, wildcards, nested v4/v6 networks, lan/internet/any, ports,
 * protocols, directions, trusted, restricted, modes) and names learned from the resolver,
 * compiles each app and decides each connection with the same code that runs in the kernel (np_decide from
 * np_shared.h, with an LPM trie simulated by linear search). Compares it against fw_decide, the
 * step-1 reference: verdict, reason and rule must always match.
 * Also: readable fixed cases, trie keys, the list cap and a measurement. */
#ifndef _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "netperm/npcompile.h"
#include "netperm/npdns.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define NAMES_APP "lordos"      /* where lord-netperm keeps what it learns from the resolver */

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static int pick(int n) { return (int)(rnd() % (uint32_t)n); }

static int bit_at(const uint8_t *a, int i) { return a[i >> 3] >> (7 - (i & 7)) & 1; }

/* The kernel's LPM trie, simulated: the longest-prefix entry that contains the address. */
static const NpList *lpm(const NpCompiled *c, int fam, const uint8_t *addr) {
    const NpNode *best = NULL;
    for (int i = 0; i < c->nnodes; i++) {
        const NpNode *n = &c->nodes[i];
        if (n->fam != fam) continue;
        int ok = 1;
        for (int b = 0; b < n->prefix && ok; b++) ok = bit_at(n->addr, b) == bit_at(addr, b);
        if (ok && (!best || n->prefix > best->prefix)) best = n;
    }
    return best ? &best->list : NULL;
}

/* 1: the destination is in a subscribed blocklist (np_blk4/np_blk6 hit; FwQuery.listed) */
static int g_listed;

/* What the BPF program does with a connection (np.bpf.c), with the app already compiled. */
static NpRes kernel(const NpCompiled *c, int dir, int proto, int fam, const uint8_t *in, int port) {
    uint8_t addr[16] = { 0 };
    static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    if (fam == 6 && !memcmp(in, mapped, 12)) { fam = 4; memcpy(addr, in + 12, 4); }
    else if (fam) memcpy(addr, in, fam == 4 ? 4 : 16);
    int lan = fam == 4 ? np_is_lan4(addr) : fam == 6 ? np_is_lan6(addr) : -1;
    const NpList *l = NULL;
    if (fam == 4 && c->app.has4) l = lpm(c, 4, addr);
    if (fam == 6 && c->app.has6) l = lpm(c, 6, addr);
    NpRes r;
    np_decide(&c->app, l, dir, proto, port, lan, g_listed, np_is_loop(fam, addr), &r);
    return r;
}

static FwRule mk(const char *app, FwDir dir, const char *dst, FwAction act) {
    FwRule r;
    memset(&r, 0, sizeof r);
    snprintf(r.app, sizeof r.app, "%s", app);
    r.dir = dir;
    r.action = act;
    if (fw_parse_dest(dst, &r)) { fprintf(stderr, "invalid destination in the test: %s\n", dst); exit(2); }
    return r;
}

static uint32_t add(FwStore *s, FwRule r) {
    uint32_t id = 0;
    if (fw_rule_add(s, &r, &id)) { fprintf(stderr, "could not add %s\n", r.app); exit(2); }
    return id;
}

static int ip(const char *s, uint8_t *a) {
    memset(a, 0, 16);
    if (inet_pton(AF_INET, s, a) == 1) return 4;
    if (inet_pton(AF_INET6, s, a) == 1) return 6;
    fprintf(stderr, "invalid IP in the test: %s\n", s);
    exit(2);
}

/* Decide with both and compare. */
static int same(FwStore *s, DnsMap *names, const NpCompiled *c, const char *app, FwDir dir, FwProto proto,
                int fam, const uint8_t *addr, uint16_t port, long long now, FwDecision *ref, NpRes *got) {
    char hn[DNSMAP_PER_IP][FW_NAME_MAX];
    const char *hp[DNSMAP_PER_IP];
    int nh = fam ? dnsmap_lookup(names, NAMES_APP, fam, addr, now, hn, DNSMAP_PER_IP) : 0;
    for (int i = 0; i < nh; i++) hp[i] = hn[i];
    FwQuery q;
    memset(&q, 0, sizeof q);
    q.app = app; q.dir = dir; q.proto = proto; q.fam = fam; q.port = port;
    if (fam) memcpy(q.addr, addr, 16);
    q.hosts = hp; q.nhosts = nh;
    q.listed = g_listed;
    *ref = fw_decide(s, &q);
    *got = kernel(c, dir == FW_IN ? NP_D_IN : NP_D_OUT, proto == FW_PROTO_TCP ? NP_P_TCP : NP_P_UDP, fam, addr, port);
    int rv = ref->verdict == FW_V_DENY ? NP_V_DENY : NP_V_ALLOW;   /* "ask" compiles as silent */
    int rr = ref->reason == FW_BY_RESTRICTED ? NP_BY_RESTRICTED : ref->reason == FW_BY_RULE ? NP_BY_RULE :
             ref->reason == FW_BY_TRUSTED ? NP_BY_TRUSTED : ref->reason == FW_BY_LIST ? NP_BY_LIST : NP_BY_MODE;
    if (got->verdict != rv || got->reason != rr) return 0;
    if (rr == NP_BY_RULE && got->rule_id != ref->rule_id) return 0;
    if (rr == NP_BY_RESTRICTED && got->flag != ref->flag) return 0;
    return 1;
}

static NpCompiled compile(FwStore *s, const char *app, DnsMap *names, long long now) {
    NpCompiled c;
    if (np_compile_app(s, app, 7, names, NAMES_APP, now, &c)) { fprintf(stderr, "compile failed\n"); exit(2); }
    return c;
}

static void fixed_cases(void) {
    FwStore *s = fw_store_new();
    DnsMap *names = dnsmap_new(0);
    long long now = 1000;
    uint8_t a[16];
    FwDecision ref;
    NpRes got;
    const char *app = "desktop:curltest";

    /* "deny all, except api.ejemplo.com" */
    add(s, mk(app, FW_OUT, "any", FW_DENY));
    add(s, mk(app, FW_OUT, "*.ejemplo.com", FW_DENY));
    uint32_t allow = add(s, mk(app, FW_OUT, "api.ejemplo.com", FW_ALLOW));
    int fam = ip("203.0.113.10", a);
    dnsmap_add(names, NAMES_APP, "api.ejemplo.com", fam, a, 300, now);
    uint8_t b[16];
    ip("203.0.113.20", b);
    dnsmap_add(names, NAMES_APP, "www.ejemplo.com", 4, b, 300, now);
    NpCompiled c = compile(s, app, names, now);
    CHECK(c.has_domains && c.app.has4 && c.nnodes == 2 && !c.truncated);
    CHECK(same(s, names, &c, app, FW_OUT, FW_PROTO_TCP, 4, a, 443, now, &ref, &got));
    CHECK(got.verdict == NP_V_ALLOW && got.rule_id == allow);
    CHECK(same(s, names, &c, app, FW_OUT, FW_PROTO_TCP, 4, b, 443, now, &ref, &got));
    CHECK(got.verdict == NP_V_DENY);
    uint8_t other[16];
    ip("198.51.100.1", other);
    CHECK(same(s, names, &c, app, FW_OUT, FW_PROTO_TCP, 4, other, 443, now, &ref, &got));
    CHECK(got.verdict == NP_V_DENY && got.reason == NP_BY_RULE);
    /* an app with its own DNS claiming api.ejemplo.com is another IP: that IP is not in the
     * maps (only the system resolver is learned from), so it does not get through */
    CHECK(kernel(&c, NP_D_OUT, NP_P_TCP, 4, other, 443).verdict == NP_V_DENY);
    /* another app does not inherit the rule */
    NpCompiled c2 = compile(s, "desktop:otra", names, now);
    CHECK(c2.nnodes == 0 && c2.app.ngen == 0);
    CHECK(kernel(&c2, NP_D_OUT, NP_P_TCP, 4, other, 443).verdict == NP_V_ALLOW);
    CHECK(kernel(&c2, NP_D_OUT, NP_P_TCP, 4, other, 443).record == 1);    /* silent: gets recorded */
    np_compiled_free(&c);
    np_compiled_free(&c2);

    /* "LAN only": reaches the gateway, not the internet */
    fw_set_restricted(s, "desktop:lan", FW_R_NO_INTERNET);
    c = compile(s, "desktop:lan", names, now);
    ip("172.17.0.1", a);
    CHECK(kernel(&c, NP_D_OUT, NP_P_TCP, 4, a, 80).verdict == NP_V_ALLOW);
    CHECK(kernel(&c, NP_D_OUT, NP_P_TCP, 4, other, 80).verdict == NP_V_DENY);
    CHECK(kernel(&c, NP_D_OUT, NP_P_TCP, 4, other, 80).flag == NP_R_NO_INTERNET);
    /* bind to an address that is not local: unknown address = internet, like fw_decide */
    CHECK(same(s, names, &c, "desktop:lan", FW_IN, FW_PROTO_TCP, 0, a, 8080, now, &ref, &got) && got.verdict == NP_V_DENY);
    np_compiled_free(&c);

    /* "no incoming connections" */
    fw_set_restricted(s, "desktop:noin", FW_R_NO_INCOMING);
    c = compile(s, "desktop:noin", names, now);
    CHECK(kernel(&c, NP_D_IN, NP_P_TCP, 0, a, 8080).verdict == NP_V_DENY);
    CHECK(kernel(&c, NP_D_OUT, NP_P_TCP, 4, other, 443).verdict == NP_V_ALLOW);
    np_compiled_free(&c);

    /* confiable y apagado */
    fw_set_trusted(s, "desktop:conf", 1);
    c = compile(s, "desktop:conf", names, now);
    got = kernel(&c, NP_D_OUT, NP_P_UDP, 4, other, 53);
    CHECK(got.verdict == NP_V_ALLOW && got.reason == NP_BY_TRUSTED && !got.record);
    np_compiled_free(&c);
    fw_set_mode(s, FW_MODE_OFF);
    c = compile(s, "desktop:otra", names, now);
    got = kernel(&c, NP_D_OUT, NP_P_UDP, 4, other, 53);
    CHECK(got.verdict == NP_V_ALLOW && got.reason == NP_BY_MODE && !got.record);
    np_compiled_free(&c);

    /* nested networks and IPv4-mapped IPv6 */
    add(s, mk("desktop:redes", FW_OUT, "10.0.0.0/8", FW_DENY));
    uint32_t mid = add(s, mk("desktop:redes", FW_OUT, "10.1.0.0/16", FW_ALLOW));
    add(s, mk("desktop:redes", FW_OUT, "10.1.2.0/24", FW_DENY));
    c = compile(s, "desktop:redes", names, now);
    CHECK(c.nnodes == 3 && c.app.has4 && !c.app.has6);
    ip("10.1.9.9", a);
    got = kernel(&c, NP_D_OUT, NP_P_TCP, 4, a, 22);
    CHECK(got.verdict == NP_V_ALLOW && got.rule_id == mid);
    ip("::ffff:10.1.2.3", a);
    CHECK(same(s, names, &c, "desktop:redes", FW_OUT, FW_PROTO_TCP, 6, a, 22, now, &ref, &got) && got.verdict == NP_V_DENY);
    np_compiled_free(&c);

    /* list cap: more than NP_LIST_MAX rules for the same network are reported */
    for (int i = 0; i < NP_LIST_MAX + 4; i++) {
        FwRule r = mk("desktop:muchas", FW_OUT, "192.0.2.0/24", i & 1 ? FW_DENY : FW_ALLOW);
        r.port_lo = r.port_hi = (uint16_t)(1000 + i);
        add(s, r);
    }
    c = compile(s, "desktop:muchas", names, now);
    CHECK(c.truncated == 4 && c.nnodes == 1 && c.nodes[0].list.n == NP_LIST_MAX);
    np_compiled_free(&c);

    /* trie keys: big-endian index followed by the address; prefix = 32 + n */
    NpNode n;
    memset(&n, 0, sizeof n);
    n.fam = 4; n.prefix = 24;
    ip("192.0.2.0", n.addr);
    NpLpm4Key k4;
    np_key4(0x01020304, &n, &k4);
    CHECK(k4.prefixlen == 56 && k4.data[0] == 1 && k4.data[3] == 4 && k4.data[4] == 192 && k4.data[6] == 2);
    n.fam = 6; n.prefix = 32;
    ip("2001:db8::", n.addr);
    NpLpm6Key k6;
    np_key6(5, &n, &k6);
    CHECK(k6.prefixlen == 64 && k6.data[3] == 5 && k6.data[4] == 0x20 && k6.data[7] == 0xb8);
    CHECK(sizeof(NpLpm4Key) == 12 && sizeof(NpLpm6Key) == 24 && sizeof(NpEnt) == 12);

    /* np_compile_host (a new DNS answer) yields the same node as the full compilation */
    FwStore *s2 = fw_store_new();
    add(s2, mk("lpk:juego", FW_OUT, "*.cdn.ejemplo.net", FW_ALLOW));
    add(s2, mk("lpk:juego", FW_OUT, "any", FW_DENY));
    add(s2, mk("lpk:juego", FW_OUT, "198.51.100.0/24", FW_DENY));
    char nm[2][FW_NAME_MAX] = { "www.juego.com", "a1.cdn.ejemplo.net" };
    ip("198.51.100.7", a);
    NpNode hn;
    int tr = 0;
    CHECK(np_compile_host(s2, "lpk:juego", 4, a, (const char (*)[FW_NAME_MAX])nm, 2, &hn, &tr) == 1);
    CHECK(hn.list.n == 2 && hn.list.e[0].verdict == NP_V_ALLOW && hn.list.e[1].verdict == NP_V_DENY && !tr);
    CHECK(np_compile_host(s2, "lpk:juego", 4, a, (const char (*)[FW_NAME_MAX])nm, 1, &hn, &tr) == 0);
    fw_store_free(s2);

    np_compiled_free(&c);
    dnsmap_free(names);
    fw_store_free(s);
}

/* ---- where the names come from (npdns.c) ----------------------------------------------------- */

static DnsMap *learn_into;
static int learned;
static void learn_cb(void *u, const uint8_t *pkt, size_t len) {
    (void)u;
    if (dnsmap_learn(learn_into, NAMES_APP, pkt, len, 100) > 0) learned++;
}

static void dns_sources(void) {
    learn_into = dnsmap_new(0);
    /* a real answer from the systemd-resolved 257 monitor (VM 122): www.wikipedia.org →
     * CNAME dyna.wikimedia.org → A/AAAA */
    static const char vl[] =
        "{\"parameters\":{\"state\":\"success\",\"question\":[{\"class\":1,\"type\":1,\"name\":\"www.wikipedia.org\"},"
        "{\"class\":1,\"type\":28,\"name\":\"www.wikipedia.org\"}],\"answer\":["
        "{\"rr\":{\"key\":{\"class\":1,\"type\":5,\"name\":\"www.wikipedia.org\"},\"name\":\"dyna.wikimedia.org\"},"
        "\"raw\":\"A3d3dwl3aWtpcGVkaWEDb3JnAAAFAAEAAEU0ABQEZHluYQl3aWtpbWVkaWEDb3JnAA==\",\"ifindex\":2},"
        "{\"rr\":{\"key\":{\"class\":1,\"type\":28,\"name\":\"dyna.wikimedia.org\"}},"
        "\"raw\":\"BGR5bmEJd2lraW1lZGlhA29yZwAAHAABAAAAtAAQKgLsgAcA7RoAAAAAAAAAAQ==\",\"ifindex\":2}]},\"continues\":true}";
    CHECK(npdns_from_varlink(vl, sizeof vl - 1, learn_cb, NULL) == 1 && learned == 1);
    uint8_t a[16];
    int fam = ip("2a02:ec80:700:ed1a::1", a);
    char nm[DNSMAP_PER_IP][FW_NAME_MAX];
    int n = dnsmap_lookup(learn_into, NAMES_APP, fam, a, 100, nm, DNSMAP_PER_IP);
    CHECK(n == 2);
    CHECK(n == 2 && ((!strcmp(nm[0], "www.wikipedia.org") && !strcmp(nm[1], "dyna.wikimedia.org")) ||
                     (!strcmp(nm[1], "www.wikipedia.org") && !strcmp(nm[0], "dyna.wikimedia.org"))));
    /* anything that isn't a successful answer, or is malformed, builds nothing */
    static const char *const bad[] = {
        "{\"parameters\":{\"ready\":true},\"continues\":true}",
        "{\"parameters\":{\"state\":\"errno\",\"question\":[],\"answer\":[]}}",
        "{\"parameters\":{\"state\":\"success\",\"question\":[{\"name\":\"a..b\"}],\"answer\":[{\"raw\":\"AAAA\"}]}}",
        "{\"parameters\":{\"state\":\"success\",\"question\":[{\"name\":\"x.com\"}],\"answer\":[{\"raw\":\"!!!!\"}]}}",
        "{\"parameters\":{\"state\":\"success\",\"question\":[{\"name\":\"x.com\"}],\"answer\":[{\"raw\":\"A3d3dw=\"}]}}",
        "{\"parameters\"", "", "[1,2]",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(npdns_from_varlink(bad[i], strlen(bad[i]), learn_cb, NULL) <= 0);
    CHECK(learned == 1);
    /* a record that doesn't follow from the question (a lying server would add it) is not learned */
    static const char lie[] =
        "{\"parameters\":{\"state\":\"success\",\"question\":[{\"name\":\"evil.example\"}],\"answer\":["
        "{\"raw\":\"BGR5bmEJd2lraW1lZGlhA29yZwAAHAABAAAAtAAQKgLsgAcA7RoAAAAAAAAAAg==\"}]}}";
    npdns_from_varlink(lie, sizeof lie - 1, learn_cb, NULL);
    fam = ip("2a02:ec80:700:ed1a::2", a);
    CHECK(dnsmap_lookup(learn_into, NAMES_APP, fam, a, 100, nm, DNSMAP_PER_IP) == 0);

    /* packets (without resolved): only UDP from port 53 of a resolv.conf server */
    uint8_t p[64] = { 0x45, 0, 0, 40, 0, 0, 0x40, 0, 64, 17, 0, 0, 8, 8, 8, 8, 192, 168, 1, 5,
                      0, 53, 0x9c, 0x40, 0, 20, 0, 0 };
    for (int i = 28; i < 40; i++) p[i] = (uint8_t)i;
    NpAddr srv = { 4, { 8, 8, 8, 8 } };
    const uint8_t *dns;
    size_t dl;
    CHECK(npdns_from_packet(p, 40, &srv, 1, &dns, &dl) == 0 && dl == 12 && dns == p + 28);
    NpAddr other = { 4, { 1, 1, 1, 1 } };
    CHECK(npdns_from_packet(p, 40, &other, 1, &dns, &dl) == -1);          /* another server */
    p[21] = 54;
    CHECK(npdns_from_packet(p, 40, &srv, 1, &dns, &dl) == -1);            /* another port */
    p[21] = 53; p[6] = 0x20;
    CHECK(npdns_from_packet(p, 40, &srv, 1, &dns, &dl) == -1);            /* fragmento */
    p[6] = 0x40; p[25] = 200;
    CHECK(npdns_from_packet(p, 40, &srv, 1, &dns, &dl) == -1);            /* largo UDP mentiroso */
    p[25] = 20;
    for (size_t cut = 0; cut < 40; cut++) npdns_from_packet(p, cut, &srv, 1, &dns, &dl);   /* truncated: no crash */
    dnsmap_free(learn_into);
}

/* ---- random ---------------------------------------------------------------------------------- */

static const char *const DOMS[] = { "ejemplo.com", "api.ejemplo.com", "www.ejemplo.com", "cdn.ejemplo.net",
                                    "a1.cdn.ejemplo.net", "x.y.ejemplo.com", "otro.org", "doubleclick.net",
                                    "ads.doubleclick.net" };
#define NDOMS (int)(sizeof DOMS / sizeof DOMS[0])
static const char *const NETS[] = { "10.0.0.0/8", "10.1.0.0/16", "10.1.2.0/24", "10.1.2.3", "192.168.0.0/16",
                                    "203.0.113.0/24", "203.0.113.10", "198.51.100.0/24", "0.0.0.0/0",
                                    "2001:db8::/32", "2001:db8:1::/48", "2001:db8:1::5", "fe80::/10", "::/0" };
#define NNETS (int)(sizeof NETS / sizeof NETS[0])
static const char *const IPS[] = { "10.1.2.3", "10.1.2.4", "10.1.9.9", "10.9.9.9", "192.168.1.1", "203.0.113.10",
                                   "203.0.113.20", "198.51.100.7", "8.8.8.8", "127.0.0.1", "172.17.0.1",
                                   "2001:db8:1::5", "2001:db8:1::6", "2001:db8:2::1", "fe80::1", "2a00::1",
                                   "::ffff:10.1.2.3", "::ffff:8.8.8.8", "::1" };
#define NIPS (int)(sizeof IPS / sizeof IPS[0])
static const char *const APPS[] = { "lordos", "desktop:a", "desktop:b", "lpk:c" };
#define NAPPS (int)(sizeof APPS / sizeof APPS[0])

static void random_rule(FwStore *s, const char *app) {
    char dst[80];
    switch (pick(6)) {
    case 0: snprintf(dst, sizeof dst, "%s", DOMS[pick(NDOMS)]); break;
    case 1: snprintf(dst, sizeof dst, "*.%s", DOMS[pick(NDOMS)]); break;
    case 2: case 3: snprintf(dst, sizeof dst, "%s", NETS[pick(NNETS)]); break;
    case 4: snprintf(dst, sizeof dst, "%s", pick(2) ? "lan" : "internet"); break;
    default: snprintf(dst, sizeof dst, "any"); break;
    }
    FwRule r = mk(app, pick(5) ? FW_OUT : FW_IN, dst, pick(2) ? FW_DENY : FW_ALLOW);
    int p = pick(4);
    if (p == 1) r.port_lo = r.port_hi = (uint16_t)(pick(2) ? 443 : 53);
    else if (p == 2) { r.port_lo = 400; r.port_hi = 500; }
    r.proto = (FwProto)pick(3);
    r.life = pick(8) ? FW_LIFE_FOREVER : FW_LIFE_UNTIL_QUIT;
    uint32_t id;
    fw_rule_add(s, &r, &id);
}

static void random_round(int *diffs, int *queries) {
    FwStore *s = fw_store_new();
    DnsMap *names = dnsmap_new(0);
    long long now = 5000;
    int m = pick(8);
    fw_set_mode(s, m < 2 ? FW_MODE_OFF : m == 2 ? FW_MODE_ASK : m == 3 ? FW_MODE_DENY : FW_MODE_SILENT);
    for (int i = 0; i < NAPPS; i++) {
        int nr = pick(9);
        for (int k = 0; k < nr; k++) random_rule(s, APPS[i]);
        int x = pick(6);
        if (x == 0) fw_set_trusted(s, APPS[i], 1);
        else if (x == 1) fw_set_restricted(s, APPS[i], 1u << pick(4));
    }
    for (int i = 0; i < 12; i++) {
        uint8_t a[16];
        int fam = ip(IPS[pick(NIPS)], a);
        dnsmap_add(names, NAMES_APP, DOMS[pick(NDOMS)], fam, a, 120, now);
    }
    for (int i = 0; i < NAPPS; i++) {
        NpCompiled c = compile(s, APPS[i], names, now);
        if (c.truncated) { np_compiled_free(&c); continue; }
        for (int k = 0; k < 40; k++) {
            uint8_t a[16];
            int fam = ip(IPS[pick(NIPS)], a);
            FwDir dir = pick(4) ? FW_OUT : FW_IN;
            if (dir == FW_IN && pick(2)) fam = 0;   /* bind to any address */
            int port = pick(3) == 0 ? 443 : pick(2) ? 53 : 400 + pick(150);
            FwDecision ref;
            NpRes got;
            (*queries)++;
            g_listed = pick(5) == 0;
            if (!same(s, names, &c, APPS[i], dir, pick(2) ? FW_PROTO_TCP : FW_PROTO_UDP, fam, a, (uint16_t)port, now, &ref, &got)) {
                if (*diffs < 5) fprintf(stderr, "diferencia: app %s ip %s port %d dir %d: ref %d/%d/%u, kernel %d/%d/%u\n", APPS[i],
                                        fam ? IPS[0] : "?", port, dir, ref.verdict, ref.reason, ref.rule_id, got.verdict, got.reason, got.rule_id);
                (*diffs)++;
            }
        }
        np_compiled_free(&c);
    }
    g_listed = 0;
    dnsmap_free(names);
    fw_store_free(s);
}

static double secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void bench(void) {
    FwStore *s = fw_store_new();
    DnsMap *names = dnsmap_new(8192);
    char dst[64];
    for (int i = 0; i < 200; i++) {
        snprintf(dst, sizeof dst, "10.%d.%d.0/24", i / 256, i % 256);
        add(s, mk("desktop:grande", FW_OUT, dst, i & 1 ? FW_DENY : FW_ALLOW));
        snprintf(dst, sizeof dst, "*.sitio%d.com", i);
        add(s, mk("desktop:grande", FW_OUT, dst, FW_ALLOW));
    }
    add(s, mk("desktop:grande", FW_OUT, "any", FW_DENY));
    for (int i = 0; i < 4000; i++) {
        uint8_t a[16] = { 198, 18, (uint8_t)(i >> 8), (uint8_t)i };
        char nm[64];
        snprintf(nm, sizeof nm, "www.sitio%d.com", i % 400);
        dnsmap_add(names, NAMES_APP, nm, 4, a, 600, 0);
    }
    double t0 = secs();
    NpCompiled c;
    np_compile_app(s, "desktop:grande", 1, names, NAMES_APP, 1, &c);
    double t1 = secs();
    printf("npcompile-test: 401 rules and 4000 learned addresses → %d nodes in %.1f ms\n", c.nnodes, (t1 - t0) * 1e3);
    CHECK(c.nnodes == 200 + 2000 && !c.truncated);
    np_compiled_free(&c);
    dnsmap_free(names);
    fw_store_free(s);
}

/* The device and Bluetooth restrictions (cgroup/dev, cgroup/lsm_socket_create): which majors each
 * one covers, that they reach the compiled policy of Linux apps but never lord-server's own
 * ("lordos"), and that they don't change any network decision. */
static void device_cases(void) {
    const uint32_t HID = 242;
    CHECK(np_dev_restriction(NP_DEV_CHAR, 81, HID) == NP_R_NO_CAMERA);          /* /dev/video* */
    CHECK(np_dev_restriction(NP_DEV_CHAR, 29, HID) == NP_R_NO_SCREEN);          /* /dev/fb* */
    CHECK(np_dev_restriction(NP_DEV_CHAR, 189, HID) == NP_R_NO_USB);            /* /dev/bus/usb */
    CHECK(np_dev_restriction(NP_DEV_CHAR, 166, HID) == NP_R_NO_USB);            /* ttyACM */
    CHECK(np_dev_restriction(NP_DEV_CHAR, 188, HID) == NP_R_NO_USB);            /* ttyUSB */
    CHECK(np_dev_restriction(NP_DEV_CHAR, HID, HID) == NP_R_NO_USB);            /* hidraw */
    CHECK(np_dev_restriction(NP_DEV_CHAR, HID, 0) == 0);                        /* no hidraw in the kernel */
    CHECK(np_dev_restriction(1, 81, HID) == 0);                                 /* a block device 81: not a camera */
    static const uint32_t free_majors[] = { 1, 4, 5, 10, 13, 116, 226, 180, 246, 0 };   /* mem, tty, misc, input, sound, dri... */
    for (int i = 0; free_majors[i]; i++) CHECK(np_dev_restriction(NP_DEV_CHAR, free_majors[i], HID) == 0);
    /* the FW_R_* bits are the same */
    CHECK(NP_R_NO_CAMERA == FW_R_NO_CAMERA && NP_R_NO_MIC == FW_R_NO_MIC && NP_R_NO_SCREEN == FW_R_NO_SCREEN && NP_R_NO_USB == FW_R_NO_USB && NP_R_NO_BLUETOOTH == FW_R_NO_BLUETOOTH);

    FwStore *s = fw_store_new();
    DnsMap *names = dnsmap_new(0);
    fw_set_restricted(s, "desktop:cam", FW_R_NO_CAMERA | FW_R_NO_USB | FW_R_NO_BLUETOOTH | FW_R_NO_FILES | FW_R_NO_MIC);
    fw_set_restricted(s, "lordos", FW_R_NO_CAMERA | FW_R_NO_USB | FW_R_NO_BLUETOOTH | FW_R_NO_INTERNET);
    NpCompiled c = compile(s, "desktop:cam", names, 1000);
    CHECK(c.app.flags == (NP_R_NO_CAMERA | NP_R_NO_USB | NP_R_NO_BLUETOOTH | NP_R_NO_MIC));  /* files aren't the kernel's */
    uint8_t a[16];
    FwDecision ref;
    NpRes got;
    int fam = ip("203.0.113.10", a);
    CHECK(same(s, names, &c, "desktop:cam", FW_OUT, FW_PROTO_TCP, fam, a, 443, 1000, &ref, &got));
    CHECK(got.verdict == NP_V_ALLOW);                                           /* the network is untouched */
    np_compiled_free(&c);
    c = compile(s, "lordos", names, 1000);
    CHECK(c.app.flags == NP_R_NO_INTERNET);                                     /* lord-server keeps its devices */
    np_compiled_free(&c);
    fw_store_free(s);
    dnsmap_free(names);
}

int main(void) {
    device_cases();
    fixed_cases();
    dns_sources();
    int diffs = 0, queries = 0;
    for (int r = 0; r < 3000; r++) random_round(&diffs, &queries);
    CHECK(diffs == 0);
    printf("npcompile-test: %d random connections against fw_decide, %d differences\n", queries, diffs);
    bench();
    printf("npcompile-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
