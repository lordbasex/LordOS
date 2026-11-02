/* fwdest_test.c — the destinations of server/tests/fw_dest_cases.txt through the store's parser
 * (fw_parse_dest, then fw_format_dest): each one must be refused ("!") or come back as the canonical
 * text. The same file is checked against the Settings sheet's parser (sdk/fwrule.js) by
 * tests/fwrule_input_test.mjs, so what the sheet accepts is what lord.fw.addRule stores.
 *
 * It also runs tests/fw_domain_cases.txt (0.8.2): one destination's verdict through fw_decide, as the
 * Monitor's per-domain switch shows it with sdk/fwrule.js decide(), once from the store and once after
 * a round trip through the firewall.json text (fw_to_json → fw_from_json), the file axia-netperm reads.
 *
 *   build/fwdest-test [cases file]      (default: tests/fw_dest_cases.txt, run from server/) */
#include "../src/fw/rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

static char *trim(char *s) {
    while (*s == ' ') s++;
    char *e = s + strlen(s);
    while (e > s && e[-1] == ' ') *--e = 0;
    return s;
}

static const char *verdict(FwStore *st, const char *app, char *names, const char *addr, const char *proto, int port) {
    const char *hosts[8];
    int nh = 0;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", names);
    if (strcmp(buf, "-")) for (char *t = strtok(buf, ","); t && nh < 8; t = strtok(NULL, ",")) hosts[nh++] = t;
    FwQuery q;
    memset(&q, 0, sizeof q);
    q.app = app;
    q.dir = FW_OUT;
    q.proto = !strcmp(proto, "udp") ? FW_PROTO_UDP : FW_PROTO_TCP;
    q.port = (uint16_t)port;
    if (strcmp(addr, "-")) {
        if (inet_pton(AF_INET, addr, q.addr) == 1) q.fam = 4;
        else if (inet_pton(AF_INET6, addr, q.addr) == 1) q.fam = 6;
    }
    q.hosts = hosts;
    q.nhosts = nh;
    FwDecision d = fw_decide(st, &q);
    return d.reason == FW_BY_RESTRICTED ? "restricted" : d.reason == FW_BY_RULE ? (d.verdict == FW_V_DENY ? "deny" : "allow") : "none";
}

static int domain_cases(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 1; }
    char line[1024];
    int n = 0, bad = 0;
    const char *app = "desktop:t";
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *col[6];
        int k = 0;
        for (char *p = line, *bar; k < 6; p = bar + 1) {
            bar = strchr(p, '|');
            if (bar) *bar = 0;
            col[k++] = trim(p);
            if (!bar) break;
        }
        if (k != 6) { fprintf(stderr, "malformed line\n"); bad++; continue; }
        FwStore *st = fw_store_new();
        if (strcmp(col[0], "-")) {
            for (char *r = strtok(col[0], ";"); r; r = strtok(NULL, ";")) {
                char act[16] = "", dst[300] = "", port[32] = "", proto[8] = "";
                sscanf(r, "%15s %299s %31s %7s", act, dst, port, proto);
                if (!strcmp(port, "tcp") || !strcmp(port, "udp")) { snprintf(proto, sizeof proto, "%s", port); port[0] = 0; }
                FwRule rule;
                memset(&rule, 0, sizeof rule);
                snprintf(rule.app, sizeof rule.app, "%s", app);
                rule.dir = FW_OUT;
                rule.action = !strcmp(act, "deny") ? FW_DENY : FW_ALLOW;
                rule.life = FW_LIFE_FOREVER;
                rule.port_lo = 0;
                rule.port_hi = 65535;
                if (port[0]) { unsigned lo = 0, hi = 0; if (sscanf(port, "%u-%u", &lo, &hi) < 2) hi = lo; rule.port_lo = (uint16_t)lo; rule.port_hi = (uint16_t)hi; }
                rule.proto = !strcmp(proto, "tcp") ? FW_PROTO_TCP : !strcmp(proto, "udp") ? FW_PROTO_UDP : FW_PROTO_ANY;
                if (fw_parse_dest(dst, &rule) || fw_rule_add(st, &rule, NULL)) { fprintf(stderr, "bad rule %s\n", r); bad++; }
            }
        }
        if (strcmp(col[4], "-")) {
            uint32_t fl = 0;
            for (char *r = strtok(col[4], ","); r; r = strtok(NULL, ",")) fl |= fw_flag_from_name(trim(r));
            fw_set_restricted(st, app, fl);
        }
        char proto[8] = "tcp";
        int port = 0;
        sscanf(col[3], "%7s %d", proto, &port);
        const char *got = verdict(st, app, col[1], col[2], proto, port);
        /* the same rules after the firewall.json text */
        size_t len = 0;
        char *j = fw_to_json(st, &len), err[120];
        FwStore *back = fw_store_new();
        const char *again = j && !fw_from_json(back, j, len, err, sizeof err) ? verdict(back, app, col[1], col[2], proto, port) : "(json failed)";
        n++;
        if (strcmp(got, col[5]) || strcmp(again, col[5])) { fprintf(stderr, "✗ domain case %d (%s | %s): want %s, got %s, after JSON %s\n", n, col[1], col[2], col[5], got, again); bad++; }
        free(j);
        fw_store_free(back);
        fw_store_free(st);
    }
    fclose(f);
    printf("fwdest-test: %d domain cases, %d failed\n", n, bad);
    return bad != 0 || n == 0;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tests/fw_dest_cases.txt";
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 1; }
    char line[600];
    int n = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (!tab) { fprintf(stderr, "malformed line: %s\n", line); bad++; continue; }
        *tab = 0;
        const char *in = line, *want = tab + 1;
        FwRule r;
        memset(&r, 0, sizeof r);
        char got[FW_NAME_MAX + 8] = "!";
        if (!fw_parse_dest(in, &r) && fw_format_dest(&r, got, sizeof got)) snprintf(got, sizeof got, "(format failed)");
        n++;
        if (strcmp(got, want)) { fprintf(stderr, "✗ %-40s want %-24s got %s\n", in, want, got); bad++; }
    }
    fclose(f);
    printf("fwdest-test: %d cases, %d failed\n", n, bad);
    int dom = argc > 1 ? 0 : domain_cases("tests/fw_domain_cases.txt");
    return bad != 0 || n == 0 || dom;
}
