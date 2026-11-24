/* vmgroup_test.c — group.json (vm/vmgroup.c, docs/VIRTUALIZATION.md 19.4) and machine.json schema 14's member keys
 * (vm/machine.c): every rule, the JSON writer read back, folder names and paths, orders, and a fuzz pass of mutated
 * files. Built under ASan and UBSan (mk/vm.mk). */
#include "../src/vm/vmgroup.h"
#include "../src/vm/machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what); fails++; } } while (0)

static const char *GOOD =
    "{\n  \"schema\": 1,\n  \"id\": \"6f1c2b3a-1111-4222-8333-444455556666\",\n  \"name\": \"Whonix\",\n  \"kind\": \"whonix\",\n"
    "  \"members\": [\n"
    "    { \"role\": \"gateway\", \"dir\": \"Gateway.lordvm\", \"start\": 1, \"stop\": 2 },\n"
    "    { \"role\": \"workstation\", \"dir\": \"Workstation.lordvm\", \"start\": 2, \"stop\": 1, \"primary\": true }\n  ],\n"
    "  \"image\": { \"catalog\": \"whonix\", \"release\": \"18.2.4.3\", \"edition\": \"LXQt\",\n"
    "             \"archive_sha512\": \"e57c79cb137f941a4f5341d841c1da8866182ebc9115acb17e21aed883c72c1566e3ef4e1078a07b018f41f60182db050cbcd931235051f0d7e90c0005fa5354\",\n"
    "             \"bases\": { \"gateway\": \"/home/u/Machines/.shared/whonix-18.2.4.3-gateway-0123456789ab.qcow2\",\n"
    "                        \"workstation\": \"/home/u/Machines/.shared/whonix-18.2.4.3-workstation-0123456789ab.qcow2\" },\n"
    "             \"base_sha256\": { \"gateway\": \"0000000000000000000000000000000000000000000000000000000000000001\",\n"
    "                              \"workstation\": \"0000000000000000000000000000000000000000000000000000000000000002\" } },\n"
    "  \"disposable\": false,\n  \"fresh_gateway\": false\n}\n";

/* GOOD with the first occurrence of `from` replaced by `to`; parse result */
static int variant(const char *from, const char *to, char *err, size_t en) {
    const char *p = strstr(GOOD, from);
    if (!p) { fprintf(stderr, "test bug: '%s' not in GOOD\n", from); fails++; return -99; }
    char buf[8192];
    snprintf(buf, sizeof buf, "%.*s%s%s", (int)(p - GOOD), GOOD, to, p + strlen(from));
    VmGroup g;
    return vm_group_parse(buf, strlen(buf), &g, err, en);
}

static void bad(const char *from, const char *to, const char *why) {
    char err[200] = "";
    int r = variant(from, to, err, sizeof err);
    if (r == VM_OK) { fprintf(stderr, "FAIL: accepted (%s)\n", why); fails++; }
}

static void test_group(void) {
    VmGroup g;
    char err[200];
    int r = vm_group_parse(GOOD, strlen(GOOD), &g, err, sizeof err);
    CHECK(r == VM_OK, err);
    CHECK(g.n == 2 && g.kind == VMG_KIND_WHONIX && !strcmp(g.name, "Whonix"), "fields");
    CHECK(vm_group_primary(&g) == 1 && vm_group_role(&g, VM_ROLE_GATEWAY) == 0, "primary, role");
    int idx[VMG_MEMBERS_MAX];
    CHECK(vm_group_order(&g, 0, idx) == 2 && idx[0] == 0 && idx[1] == 1, "start order: gateway first");
    CHECK(vm_group_order(&g, 1, idx) == 2 && idx[0] == 1 && idx[1] == 0, "stop order: workstation first");
    CHECK(!strcmp(g.release, "18.2.4.3") && g.has_image && g.base[VM_ROLE_WORKSTATION][0], "image");
    /* written and read back the same */
    char *j = vm_group_json(&g);
    VmGroup h;
    CHECK(j && vm_group_parse(j, strlen(j), &h, err, sizeof err) == VM_OK, err);
    CHECK(j && !memcmp(&g, &h, sizeof g), "round trip");
    free(j);
    /* save and load */
    char tmpl[] = "/tmp/vmgroup-test.XXXXXX";
    char *d = mkdtemp(tmpl);
    CHECK(d && vm_group_save(d, &g) == VM_OK, "save");
    CHECK(d && vm_group_load(d, &h, err, sizeof err) == VM_OK && !memcmp(&g, &h, sizeof g), "load");
    if (d) { char p[256]; snprintf(p, sizeof p, "%s/group.json", d); unlink(p); rmdir(d); }

    /* every rule of 19.4 */
    bad("\"schema\": 1", "\"schema\": 0", "schema 0");
    bad("\"schema\": 1", "\"schema\": \"1\"", "schema as text");
    CHECK(variant("\"schema\": 1", "\"schema\": 2", err, sizeof err) == VM_E_NEWER, "a newer schema is read-only");
    bad("\"kind\": \"whonix\"", "\"kind\": \"generic\"", "unknown kind");
    bad("\"name\": \"Whonix\"", "\"name\": \"\"", "empty name");
    bad("\"name\": \"Whonix\"", "\"name\": \"a/b\"", "slash in name");
    bad("\"id\": \"6f1c2b3a-1111-4222-8333-444455556666\"", "\"id\": \"6F1C2B3A-1111-4222-8333-444455556666\"", "upper-case id");
    bad("\"disposable\": false", "\"disposable\": false, \"extra\": 1", "unknown key");
    bad("\"stop\": 2 }", "\"stop\": 2, \"x\": 1 }", "unknown member key");
    bad("\"Gateway.lordvm\"", "\"../Gateway.lordvm\"", "path traversal");
    bad("\"Gateway.lordvm\"", "\"a/Gateway.lordvm\"", "slash in dir");
    bad("\"Gateway.lordvm\"", "\".Gateway.lordvm\"", "hidden dir");
    bad("\"Gateway.lordvm\"", "\"Gateway\"", "no .lordvm");
    bad("\"Gateway.lordvm\"", "\"Workstation.lordvm\"", "the same dir twice");
    bad("\"Gateway.lordvm\"", "\"workstation.LORDVM\"", "not .lordvm (case)");
    bad("\"start\": 1, \"stop\": 2 }", "\"start\": 2, \"stop\": 2 }", "start not a permutation");
    bad("\"start\": 1, \"stop\": 2 }", "\"start\": 1, \"stop\": 1 }", "stop not a permutation");
    bad("\"start\": 1, \"stop\": 2 }", "\"start\": 1, \"stop\": 2, \"primary\": true }", "two primaries");
    bad(", \"primary\": true }", " }", "no primary");
    bad("\"start\": 1, \"stop\": 2 }", "\"start\": 1.5, \"stop\": 2 }", "a fraction");
    bad("\"role\": \"gateway\"", "\"role\": \"workstation\"", "a role twice");
    bad("\"role\": \"gateway\"", "\"role\": \"router\"", "unknown role");
    bad("\"start\": 1, \"stop\": 2 }", "\"start\": 1, \"stop\": 2 },\n { \"role\": \"gateway\", \"dir\": \"C.lordvm\", \"start\": 3, \"stop\": 3 }", "a third member");
    bad("\"members\": [", "\"members\": [ { \"role\": \"gateway\", \"dir\": \"A.lordvm\", \"start\": 1, \"stop\": 1 }, { \"role\": \"gateway\", \"dir\": \"B.lordvm\", \"start\": 1, \"stop\": 1 }, { \"role\": \"gateway\", \"dir\": \"C.lordvm\", \"start\": 1, \"stop\": 1 }, ", "more than 4 members");
    /* whonix: the gateway first, the workstation the card */
    {
        char buf[8192];
        snprintf(buf, sizeof buf, "%s", GOOD);
        char *a = strstr(buf, "\"start\": 1, \"stop\": 2 }"), *b = strstr(buf, "\"start\": 2, \"stop\": 1, \"primary\": true }");
        memcpy(a, "\"start\": 2, \"stop\": 1 }", 23);
        memcpy(b, "\"start\": 1, \"stop\": 2,", 22);
        CHECK(vm_group_parse(buf, strlen(buf), &g, err, sizeof err) != VM_OK, "a whonix workstation started first");
    }
    bad("\"stop\": 2 }", "\"stop\": 2, \"primary\": true }", "gateway primary");
    bad("\"release\": \"18.2.4.3\"", "\"release\": \"18.2.4.3.\"", "release ends with a dot");
    bad("\"release\": \"18.2.4.3\"", "\"release\": \"18;2\"", "release with ;");
    bad("\"edition\": \"LXQt\"", "\"edition\": \"LX Qt\"", "edition with a space");
    bad("\"archive_sha512\": \"e57c", "\"archive_sha512\": \"E57c", "upper-case sha512");
    bad("\"archive_sha512\": \"e57c79cb", "\"archive_sha512\": \"e57c79c", "short sha512");
    bad("/home/u/Machines/.shared/whonix-18.2.4.3-gateway", "/home/u/Machines/other/whonix-18.2.4.3-gateway", "base outside .shared");
    bad("-gateway-0123456789ab.qcow2", "-gateway-0123456789ab.img", "base not qcow2");
    bad("\"gateway\": \"0000", "\"router\": \"0000", "a hash for an unknown role");
    bad("\"workstation\": \"0000000000000000000000000000000000000000000000000000000000000002\"", "\"workstation\": \"02\"", "short sha256");
    bad("\"image\":", "\"imagex\":", "unknown top key");
    {
        /* whonix needs its image */
        const char *p = strstr(GOOD, ",\n  \"image\"");
        const char *q = strstr(GOOD, ",\n  \"disposable\"");
        char buf[8192];
        snprintf(buf, sizeof buf, "%.*s%s", (int)(p - GOOD), GOOD, q);
        CHECK(vm_group_parse(buf, strlen(buf), &g, err, sizeof err) != VM_OK, "whonix without an image");
    }
    /* names and paths */
    CHECK(vm_group_member_name_ok("Gateway.lordvm") && !vm_group_member_name_ok(".lordvm") && !vm_group_member_name_ok("a,b.lordvm"), "member names");
    CHECK(vm_group_dir_ok("/home/u/Machines/Whonix.lordgroup"), "a group folder");
    CHECK(!vm_group_dir_ok("/home/u/Machines/../Whonix.lordgroup") && !vm_group_dir_ok("Whonix.lordgroup") && !vm_group_dir_ok("/home/u//W.lordgroup") &&
          !vm_group_dir_ok("/home/u/Machines/.lordgroup") && !vm_group_dir_ok("/home/u/Machines/W.lordgroup/") && !vm_group_dir_ok("/home/u/W.lordvm"),
          "bad group folders");
    char par[512];
    CHECK(vm_group_parent("/home/u/Machines/W.lordgroup/Gateway.lordvm", par, sizeof par) && !strcmp(par, "/home/u/Machines/W.lordgroup"), "parent");
    CHECK(!vm_group_parent("/home/u/Machines/Gateway.lordvm", par, sizeof par), "not in a group");
}

/* machine.json schema 14 */
static const char *M14 =
    "{\"schema\": 14, \"id\": \"0b6c1a2e-1111-4222-8333-444455556666\", \"name\": \"Gateway\", \"arch\": \"amd64\", \"accel\": \"kvm\", \"vcpus\": 1,"
    " \"memory_mb\": 2048, \"disk_gb\": 100, \"firmware\": \"bios\", \"machine_type\": \"pc-q35-10.0\", \"display\": \"window\", \"network\": \"shared\","
    " \"mac\": \"52:54:00:12:34:56\", \"base\": \"/home/u/Machines/.shared/g.qcow2\", \"agent\": false, \"clipboard\": \"off\", \"sound\": true, \"volume\": 100,"
    " \"microphone\": false, \"transfer\": false, \"group\": \"6f1c2b3a-1111-4222-8333-444455556666\", \"role\": \"gateway\", \"uplink\": \"tor\","
    " \"link_mac\": \"52:54:00:65:43:21\", \"profile_lock\": \"anonymity\", \"timers\": \"whonix-gateway\","
    " \"created\": \"2026-11-23T20:00:00Z\", \"updated\": \"2026-11-23T20:00:00Z\"}";

static int mvariant(const char *from, const char *to) {
    const char *p = strstr(M14, from);
    if (!p) { fprintf(stderr, "test bug: '%s' not in M14\n", from); fails++; return -99; }
    char buf[4096], err[200];
    snprintf(buf, sizeof buf, "%.*s%s%s", (int)(p - M14), M14, to, p + strlen(from));
    VmMachine m;
    return vm_machine_parse(buf, strlen(buf), &m, err, sizeof err);
}

static void test_machine(void) {
    VmMachine m;
    char err[200];
    CHECK(vm_machine_parse(M14, strlen(M14), &m, err, sizeof err) == VM_OK, err);
    CHECK(m.role == VM_ROLE_GATEWAY && m.uplink_tor && m.profile_lock == VM_LOCK_ANONYMITY && m.timers == VM_TIMERS_WHONIX_GATEWAY, "member keys");
    char *j = vm_machine_json(&m);
    VmMachine b;
    CHECK(j && vm_machine_parse(j, strlen(j), &b, err, sizeof err) == VM_OK && !memcmp(&m, &b, sizeof m), "member round trip");
    free(j);
    CHECK(mvariant("\"agent\": false", "\"agent\": true") != VM_OK, "locked: agent");
    CHECK(mvariant("\"clipboard\": \"off\"", "\"clipboard\": \"both\"") != VM_OK, "locked: clipboard");
    CHECK(mvariant("\"transfer\": false", "\"transfer\": true") != VM_OK, "locked: transfer");
    CHECK(mvariant("\"microphone\": false", "\"microphone\": true") != VM_OK, "locked: microphone");
    CHECK(mvariant("\"agent\": false", "\"agent\": false, \"graphics\": \"3d\"") != VM_OK, "locked: 3d");
    CHECK(mvariant("\"agent\": false", "\"agent\": false, \"perf_profile\": true") != VM_OK, "locked: perf profile");
    CHECK(mvariant("\"agent\": false", "\"agent\": false, \"disk_serial\": \"X1\"") != VM_OK, "locked: disk serial");
    CHECK(mvariant("\"agent\": false", "\"agent\": false, \"ports\": [{\"proto\": \"tcp\", \"host\": 2222, \"guest\": 22, \"open\": \"this-computer\"}]") != VM_OK, "tor: ports");
    CHECK(mvariant("\"agent\": false", "\"agent\": false, \"shares\": [{\"tag\": \"a\", \"path\": \"/home/u/a\", \"writable\": false}]") != VM_OK, "locked: shares");
    CHECK(mvariant("\"profile_lock\": \"anonymity\", ", "") == VM_OK, "unlocked member (not made by LordOS, but valid)");
    CHECK(mvariant("\"network\": \"shared\"", "\"network\": \"link\"") != VM_OK, "a gateway on the link only");
    CHECK(mvariant("\"link_mac\": \"52:54:00:65:43:21\"", "\"link_mac\": \"52:54:00:12:34:56\"") != VM_OK, "link_mac = mac");
    CHECK(mvariant("\"uplink\": \"tor\"", "\"uplink\": \"vpn\"") != VM_OK, "unknown uplink");
    CHECK(mvariant("\"timers\": \"whonix-gateway\"", "\"timers\": \"whonix-workstation\"") != VM_OK, "workstation timers on a gateway");
    CHECK(mvariant("\"role\": \"gateway\"", "\"role\": \"router\"") != VM_OK, "unknown role");
    CHECK(mvariant("\"schema\": 14", "\"schema\": 13") != VM_OK, "member keys need 14");
    CHECK(mvariant("\"accel\": \"kvm\"", "\"accel\": \"tcg\"") != VM_OK, "members use KVM");
    CHECK(mvariant("\"group\": \"6f1c2b3a-1111-4222-8333-444455556666\", ", "") != VM_OK, "member keys without a group");
    /* the workstation: link only */
    CHECK(mvariant("\"network\": \"shared\"", "\"network\": \"link\"") != VM_OK, "x");
    {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s", M14);
        const char *swaps[][2] = { { "\"network\": \"shared\"", "\"network\": \"link\"" }, { "\"role\": \"gateway\"", "\"role\": \"workstation\"" },
                                   { "\"uplink\": \"tor\", ", "" }, { "\"link_mac\": \"52:54:00:65:43:21\", ", "" },
                                   { "\"timers\": \"whonix-gateway\"", "\"timers\": \"whonix-workstation\"" } };
        for (size_t i = 0; i < sizeof swaps / sizeof swaps[0]; i++) {
            char *p = strstr(buf, swaps[i][0]);
            if (!p) { fails++; continue; }
            char t[4096];
            snprintf(t, sizeof t, "%.*s%s%s", (int)(p - buf), buf, swaps[i][1], p + strlen(swaps[i][0]));
            snprintf(buf, sizeof buf, "%s", t);
        }
        CHECK(vm_machine_parse(buf, strlen(buf), &m, err, sizeof err) == VM_OK && m.network == VM_NET_LINK, err);
    }
    /* a machine that isn't a member can't be on the link */
    {
        VmMachine x = m;
        x.group[0] = 0; x.role = 0; x.profile_lock = 0; x.timers = 0;
        CHECK(vm_machine_check(&x, err, sizeof err) != VM_OK, "the link network outside a group");
    }
}

/* mutations of the good file: never a crash, and whatever parses checks */
static void fuzz(void) {
    srand(1234);
    size_t n = strlen(GOOD);
    char buf[8192];
    int accepted = 0;
    for (int it = 0; it < 30000; it++) {
        memcpy(buf, GOOD, n + 1);
        size_t len = n;
        int k = 1 + rand() % 4;
        for (int i = 0; i < k; i++) {
            size_t at = (size_t)rand() % len;
            switch (rand() % 4) {
            case 0: buf[at] = (char)(rand() % 256); break;
            case 1: buf[at] = "{}[]\",:0123456789aefnrstu "[rand() % 26]; break;
            case 2: memmove(buf + at, buf + at + 1, len - at); len--; break;
            default: if (len + 1 < sizeof buf) { memmove(buf + at + 1, buf + at, len - at + 1); buf[at] = buf[(size_t)rand() % len]; len++; } break;
            }
        }
        VmGroup g;
        char err[200];
        if (vm_group_parse(buf, len, &g, err, sizeof err) == VM_OK) {
            accepted++;
            if (vm_group_check(&g, NULL, 0) != VM_OK) { fprintf(stderr, "FAIL: fuzz accepted a group that doesn't check\n"); fails++; }
        }
    }
    printf("fuzz: 30000 mutations, %d still valid\n", accepted);
}

int main(void) {
    test_group();
    test_machine();
    fuzz();
    if (fails) { fprintf(stderr, "vmgroup-test: %d failures\n", fails); return 1; }
    printf("vmgroup-test: ok\n");
    return 0;
}
