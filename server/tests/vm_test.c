/* vm_test.c — Machines without QEMU (docs/VIRTUALIZATION.md, "Phase 1 implementation"):
 *   - machine.json: a good file and its round trip, every field broken in turn, unknown keys, a newer schema,
 *     limits against a Pi 5 (4 CPUs, 16 GB), a 4 GB board and an amd64 host, the resource budget;
 *   - the fixed messages: what each side accepts, wrong lengths, versions and extra descriptors on a real
 *     SOCK_SEQPACKET pair, damage rectangles, the cursor memfd check;
 *   - QMP lines: split reads, garbage, an over-long line, kinds;
 *   - the evdev to QEMU key map. */
#include "vm/machine.h"
#include "vm/qmp.h"
#include "vm/vmkeys.h"
#include "vm/vmproto.h"
#include "vm/logclean.h"
#include "vm/vmops.h"
#include "vm/vmagent.h"
#include "vm/vmnet.h"
#include "vm/vmgvtwin.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); } } while (0)

static const char GOOD[] =
    "{\"schema\":1,\"id\":\"0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b\",\"name\":\"Debian 13 \xc3\xa9t\xc3\xa9\",\"arch\":\"arm64\",\"accel\":\"kvm\","
    "\"vcpus\":3,\"memory_mb\":2048,\"disk_gb\":20,\"firmware\":\"uefi\",\"machine_type\":\"virt-10.0\","
    "\"firmware_code\":\"/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd\",\"firmware_sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
    "\"display\":\"window\",\"network\":\"shared\",\"mac\":\"52:54:00:12:ab:cd\",\"created\":\"2026-11-04T20:00:00Z\",\"updated\":\"2026-11-04T20:00:00Z\"}";

/* GOOD with one "key":value replaced (or removed when repl is NULL) */
static int parse_with(const char *key, const char *repl, VmMachine *m) {
    char buf[4096], pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *at = strstr(GOOD, pat);
    if (!at) return -100;
    const char *v = at + strlen(pat), *end = v;
    if (*end == '"') { end++; while (*end != '"') end++; end++; } else while (*end != ',' && *end != '}') end++;
    size_t pre = (size_t)(at - GOOD);
    if (repl) snprintf(buf, sizeof buf, "%.*s%s%s%s", (int)pre, GOOD, pat, repl, end);
    else {
        if (*end == ',') end++;
        else if (pre && GOOD[pre - 1] == ',') pre--;
        snprintf(buf, sizeof buf, "%.*s%s", (int)pre, GOOD, end);
    }
    char err[256];
    return vm_machine_parse(buf, strlen(buf), m, err, sizeof err);
}

static void test_schema(void) {
    VmMachine m;
    char err[256];
    CHECK(vm_machine_parse(GOOD, strlen(GOOD), &m, err, sizeof err) == VM_OK);
    CHECK(m.arch == VM_ARCH_ARM64 && m.accel == VM_ACCEL_KVM && m.vcpus == 3 && m.memory_mb == 2048 && m.firmware == VM_FW_UEFI);
    CHECK(!strcmp(m.machine_type, "virt-10.0") && m.network == VM_NET_SHARED && !m.iso[0]);
    /* round trip */
    char *j = vm_machine_json(&m);
    VmMachine m2;
    CHECK(j && vm_machine_parse(j, strlen(j), &m2, err, sizeof err) == VM_OK && !memcmp(&m, &m2, sizeof m));
    free(j);

    struct { const char *key, *val; int want; } bad[] = {
        { "schema", "13", VM_E_NEWER }, { "schema", "0", VM_E_SCHEMA }, { "schema", "\"1\"", VM_E_SCHEMA }, { "schema", NULL, VM_E_SCHEMA },
        { "id", "\"0F8E2C1A-3B4D-4E5F-8A6B-7C8D9E0F1A2B\"", VM_E_RANGE }, { "id", "\"x\"", VM_E_RANGE }, { "id", NULL, VM_E_FIELD },
        { "name", "\"\"", VM_E_RANGE }, { "name", "\"a/b\"", VM_E_RANGE }, { "name", "\"   \"", VM_E_RANGE }, { "name", "\"tab\\there\"", VM_E_RANGE },
        { "name", "\"\\u202e\"", VM_OK }, { "name", "\"x\\u2028y\"", VM_E_RANGE },
        { "name", "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"", VM_E_RANGE },
        { "arch", "\"riscv64\"", VM_E_RANGE }, { "arch", "7", VM_E_FIELD },
        { "accel", "\"hvf\"", VM_E_RANGE },
        { "vcpus", "0", VM_E_RANGE }, { "vcpus", "65", VM_E_RANGE }, { "vcpus", "2.5", VM_E_FIELD }, { "vcpus", "\"3\"", VM_E_FIELD }, { "vcpus", "1e10", VM_E_FIELD },
        { "memory_mb", "255", VM_E_RANGE }, { "memory_mb", "-1", VM_E_RANGE },
        { "disk_gb", "0", VM_E_RANGE }, { "disk_gb", "16385", VM_E_RANGE },
        { "firmware", "\"bios\"", VM_E_RANGE },          /* arm64 boots UEFI only */
        { "machine_type", "\"virt\"", VM_E_RANGE }, { "machine_type", "\"pc-q35-10.0\"", VM_E_RANGE }, { "machine_type", "\"virt-10.0.1\"", VM_E_RANGE },
        { "firmware_code", "\"/home/x/CODE.fd\"", VM_E_RANGE }, { "firmware_code", "\"/usr/share/../etc/shadow\"", VM_E_RANGE },
        { "firmware_code", "\"/usr/share/a,b.fd\"", VM_E_RANGE },
        { "firmware_sha256", "\"00\"", VM_E_RANGE },
        { "display", "\"spice\"", VM_E_RANGE }, { "network", "\"bridged\"", VM_E_RANGE }, { "network", "\"hostonly\"", VM_E_FIELD }, { "network", "\"isolated\"", VM_OK },
        { "mac", "\"52:54:00:12:AB:CD\"", VM_E_RANGE }, { "mac", "\"01:00:5e:00:00:01\"", VM_E_RANGE }, { "mac", "\"52:54:00:12:ab\"", VM_E_RANGE },
        { "created", "\"yesterday\"", VM_E_RANGE }, { "updated", "\"2026-13-01T00:00:00Z\"", VM_E_RANGE }, { "created", NULL, VM_E_FIELD },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int r = parse_with(bad[i].key, bad[i].val, &m);
        if (r != bad[i].want) fprintf(stderr, "  %s = %s: got %d, want %d\n", bad[i].key, bad[i].val ? bad[i].val : "(missing)", r, bad[i].want);
        CHECK(r == bad[i].want);
    }
    /* iso: absolute, no .., no comma */
    char buf[4096];
    snprintf(buf, sizeof buf, "%.*s,\"iso\":\"/home/u/d.iso\"}", (int)strlen(GOOD) - 1, GOOD);
    CHECK(vm_machine_parse(buf, strlen(buf), &m, err, sizeof err) == VM_OK && !strcmp(m.iso, "/home/u/d.iso"));
    snprintf(buf, sizeof buf, "%.*s,\"iso\":\"d.iso\"}", (int)strlen(GOOD) - 1, GOOD);
    CHECK(vm_machine_parse(buf, strlen(buf), &m, err, sizeof err) == VM_E_RANGE);
    snprintf(buf, sizeof buf, "%.*s,\"iso\":\"/a/../b.iso\"}", (int)strlen(GOOD) - 1, GOOD);
    CHECK(vm_machine_parse(buf, strlen(buf), &m, err, sizeof err) == VM_E_RANGE);
    snprintf(buf, sizeof buf, "%.*s,\"iso\":\"/a/b.iso,readonly=off\"}", (int)strlen(GOOD) - 1, GOOD);
    CHECK(vm_machine_parse(buf, strlen(buf), &m, err, sizeof err) == VM_E_RANGE);
    /* unknown keys, duplicates, not JSON, not an object, too big */
    snprintf(buf, sizeof buf, "%.*s,\"password\":\"x\"}", (int)strlen(GOOD) - 1, GOOD);
    CHECK(vm_machine_parse(buf, strlen(buf), &m, err, sizeof err) == VM_E_FIELD);
    snprintf(buf, sizeof buf, "%.*s,\"vcpus\":4}", (int)strlen(GOOD) - 1, GOOD);
    CHECK(vm_machine_parse(buf, strlen(buf), &m, err, sizeof err) == VM_E_JSON);
    CHECK(vm_machine_parse("[]", 2, &m, err, sizeof err) == VM_E_JSON);
    CHECK(vm_machine_parse("{", 1, &m, err, sizeof err) == VM_E_JSON);
    CHECK(vm_machine_parse(GOOD, 70 * 1024, &m, err, sizeof err) == VM_E_JSON);
    /* a newer file is refused before its other keys are judged, and isn't saved */
    const char newer[] = "{\"schema\":13,\"whatever\":true}";
    CHECK(vm_machine_parse(newer, strlen(newer), &m, err, sizeof err) == VM_E_NEWER);
    VmMachine n9;
    vm_machine_parse(GOOD, strlen(GOOD), &n9, err, sizeof err);
    n9.schema = VM_SCHEMA + 1;
    CHECK(vm_machine_save("/nonexistent-dir", &n9) != VM_OK);
    /* schema 2: base (a .qcow2 in a .shared folder), agent, clipboard; not in a schema 1 file */
    struct { const char *add; int want; } s2[] = {
        { "\"base\":\"/home/u/Machines/.shared/a.qcow2\"", VM_OK }, { "\"base\":\"/home/u/Machines/a.qcow2\"", VM_E_RANGE },
        { "\"base\":\"/home/u/.shared/x/a.qcow2\"", VM_E_RANGE }, { "\"base\":\"/home/u/.shared/a.img\"", VM_E_RANGE },
        { "\"base\":\"/home/u/.shared/../a.qcow2\"", VM_E_RANGE },
        { "\"agent\":true", VM_OK }, { "\"agent\":1", VM_E_FIELD }, { "\"clipboard\":\"both\"", VM_OK }, { "\"clipboard\":\"all\"", VM_E_RANGE },
    };
    for (size_t i = 0; i < sizeof s2 / sizeof s2[0]; i++) {
        char t[4096];
        const char *at = strstr(GOOD, "\"schema\":1");
        snprintf(t, sizeof t, "%.*s\"schema\":2%.*s,%s}", (int)(at - GOOD), GOOD, (int)(strlen(at) - 10 - 1), at + 10, s2[i].add);
        int r = vm_machine_parse(t, strlen(t), &m, err, sizeof err);
        if (r != s2[i].want) fprintf(stderr, "  schema 2 %s: got %d (%s)\n", s2[i].add, r, err);
        CHECK(r == s2[i].want);
        snprintf(t, sizeof t, "%.*s,%s}", (int)strlen(GOOD) - 1, GOOD, s2[i].add);
        CHECK(vm_machine_parse(t, strlen(t), &m, err, sizeof err) != VM_OK || s2[i].want != VM_OK);    /* never in schema 1 */
    }
    /* schema 3 (import, 6.22): an imported machine's hardware; never in schema 1 or 2 */
    struct { const char *add; int want; } s3[] = {
        { "\"devices\":\"compat\"", VM_OK }, { "\"devices\":\"fast\"", VM_E_RANGE }, { "\"nic_model\":\"e1000e\"", VM_OK },
        { "\"nic_model\":\"vmxnet3\"", VM_OK }, { "\"nic_model\":\"ne2k\"", VM_E_RANGE },
        { "\"extra_nics\":[{\"mac\":\"00:0c:29:aa:bb:cc\",\"model\":\"e1000e\"},{\"mac\":\"00:50:56:01:02:03\",\"model\":\"vmxnet3\"}]", VM_OK },
        { "\"extra_nics\":[{\"mac\":\"52:54:00:12:ab:cd\",\"model\":\"e1000e\"}]", VM_E_RANGE },
        { "\"extra_nics\":[{\"mac\":\"01:0c:29:aa:bb:cc\",\"model\":\"e1000e\"}]", VM_E_RANGE }, { "\"extra_nics\":[\"00:0c:29:aa:bb:cc\"]", VM_E_FIELD },
        { "\"extra_nics\":[{\"mac\":\"00:0c:29:aa:bb:cc\",\"model\":\"ne2k\"}]", VM_E_RANGE }, { "\"extra_nics\":[{\"mac\":\"00:0c:29:aa:bb:cc\"}]", VM_E_FIELD },
        { "\"extra_nics\":[{\"mac\":\"00:0c:29:aa:bb:cc\",\"model\":\"e1000\",\"x\":1}]", VM_E_FIELD },
        { "\"cpu_sockets\":1,\"cpu_cores\":3,\"cpu_threads\":1", VM_OK }, { "\"cpu_sockets\":2,\"cpu_cores\":2,\"cpu_threads\":1", VM_E_RANGE },
        { "\"cpu_sockets\":3", VM_E_RANGE },
        { "\"smbios\":{\"manufacturer\":\"Example, Inc.\",\"serial\":\"ABC-123\"}", VM_OK }, { "\"smbios\":{\"vendor\":\"x\"}", VM_E_FIELD },
        { "\"smbios\":{\"serial\":\"tab\\there\"}", VM_E_RANGE }, { "\"smbios\":{\"serial\":\"\u00e9\"}", VM_E_RANGE },
        { "\"disk_serial\":\"VBaaaaaaaa-1100ffff\"", VM_OK }, { "\"disk_serial\":\"a,b\"", VM_E_RANGE }, { "\"disk_serial\":\"012345678901234567890\"", VM_E_RANGE },
        { "\"data_disks\":[{\"gb\":10,\"serial\":\"D1\"},{\"gb\":1}]", VM_OK }, { "\"data_disks\":[{\"gb\":0}]", VM_E_RANGE },
        { "\"data_disks\":[{\"gb\":1,\"file\":\"../../x\"}]", VM_E_FIELD }, { "\"data_disks\":[{\"gb\":1},{\"gb\":1},{\"gb\":1},{\"gb\":1},{\"gb\":1}]", VM_E_RANGE },
        { "\"rtc\":\"local\"", VM_OK }, { "\"rtc\":\"gmt\"", VM_E_RANGE },
        { "\"data_disks\":[{\"gb\":1}],\"base\":\"/home/u/Machines/.shared/a.qcow2\"", VM_E_RANGE },
    };
    for (size_t i = 0; i < sizeof s3 / sizeof s3[0]; i++) {
        char t[4096];
        const char *at = strstr(GOOD, "\"schema\":1");
        snprintf(t, sizeof t, "%.*s\"schema\":3%.*s,%s}", (int)(at - GOOD), GOOD, (int)(strlen(at) - 10 - 1), at + 10, s3[i].add);
        int r = vm_machine_parse(t, strlen(t), &m, err, sizeof err);
        if (r != s3[i].want) fprintf(stderr, "  schema 3 %s: got %d (%s)\n", s3[i].add, r, err);
        CHECK(r == s3[i].want);
        if (r == VM_OK) {
            /* the round trip keeps every field */
            char *jj = vm_machine_json(&m);
            CHECK(jj && vm_machine_parse(jj, strlen(jj), &m2, err, sizeof err) == VM_OK && !memcmp(&m, &m2, sizeof m));
            free(jj);
        }
        snprintf(t, sizeof t, "%.*s\"schema\":2%.*s,%s}", (int)(at - GOOD), GOOD, (int)(strlen(at) - 10 - 1), at + 10, s3[i].add);
        CHECK(vm_machine_parse(t, strlen(t), &m, err, sizeof err) != VM_OK || s3[i].want != VM_OK);    /* never in schema 2 */
    }
    /* schema 4 (0.8.8.7): shared folders; never in schema 3 */
    struct { const char *add; int want; } s4[] = {
        { "\"shares\":[{\"tag\":\"work\",\"path\":\"/home/u/Work\"}]", VM_OK },
        { "\"shares\":[{\"tag\":\"work\",\"path\":\"/home/u/Work\",\"writable\":true},{\"tag\":\"m-2_x\",\"path\":\"/media/u/Disk\"}]", VM_OK },
        { "\"shares\":[{\"tag\":\"a b\",\"path\":\"/home/u/Work\"}]", VM_E_RANGE }, { "\"shares\":[{\"tag\":\"\",\"path\":\"/home/u/Work\"}]", VM_E_RANGE },
        { "\"shares\":[{\"tag\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"path\":\"/home/u/Work\"}]", VM_E_RANGE },
        { "\"shares\":[{\"tag\":\"w\",\"path\":\"Work\"}]", VM_E_RANGE }, { "\"shares\":[{\"tag\":\"w\",\"path\":\"/home/u/../etc\"}]", VM_E_RANGE },
        { "\"shares\":[{\"tag\":\"w\",\"path\":\"/home/u/a,b\"}]", VM_E_RANGE }, { "\"shares\":[{\"tag\":\"w\"}]", VM_E_FIELD },
        { "\"shares\":[{\"tag\":\"w\",\"path\":\"/home/u/W\",\"writable\":1}]", VM_E_FIELD },
        { "\"shares\":[{\"tag\":\"w\",\"path\":\"/home/u/W\",\"mode\":\"rw\"}]", VM_E_FIELD },
        { "\"shares\":[{\"tag\":\"w\",\"path\":\"/home/u/W\"},{\"tag\":\"w\",\"path\":\"/home/u/X\"}]", VM_E_RANGE },
        { "\"shares\":[{\"tag\":\"w\",\"path\":\"/home/u/W\"},{\"tag\":\"x\",\"path\":\"/home/u/W\"}]", VM_E_RANGE },
        { "\"shares\":[{\"tag\":\"a\",\"path\":\"/a\"},{\"tag\":\"b\",\"path\":\"/b\"},{\"tag\":\"c\",\"path\":\"/c\"},{\"tag\":\"d\",\"path\":\"/d\"},{\"tag\":\"e\",\"path\":\"/e\"}]", VM_E_RANGE },
        { "\"shares\":{}", VM_E_FIELD },
    };
    for (size_t i = 0; i < sizeof s4 / sizeof s4[0]; i++) {
        char t[4096];
        const char *at = strstr(GOOD, "\"schema\":1");
        snprintf(t, sizeof t, "%.*s\"schema\":4%.*s,%s}", (int)(at - GOOD), GOOD, (int)(strlen(at) - 10 - 1), at + 10, s4[i].add);
        int r = vm_machine_parse(t, strlen(t), &m, err, sizeof err);
        if (r != s4[i].want) fprintf(stderr, "  schema 4 %s: got %d (%s)\n", s4[i].add, r, err);
        CHECK(r == s4[i].want);
        if (r == VM_OK) {
            char *jj = vm_machine_json(&m);
            CHECK(jj && vm_machine_parse(jj, strlen(jj), &m2, err, sizeof err) == VM_OK && !memcmp(&m, &m2, sizeof m));
            free(jj);
        }
        snprintf(t, sizeof t, "%.*s\"schema\":3%.*s,%s}", (int)(at - GOOD), GOOD, (int)(strlen(at) - 10 - 1), at + 10, s4[i].add);
        CHECK(vm_machine_parse(t, strlen(t), &m, err, sizeof err) != VM_OK || s4[i].want != VM_OK);    /* never in schema 3 */
    }
    /* schema 5 (Windows 11): secure boot, TPM 2.0, Hyper-V enlightenments; amd64 only, secure boot with UEFI only, never in
     * schema 4 */
    {
        char amd[2048];
        const char *a = "\"arch\":\"arm64\"", *mt = "\"machine_type\":\"virt-10.0\"", *fc = "/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd";
        const char *pa = strstr(GOOD, a), *pm = strstr(GOOD, mt), *pf = strstr(GOOD, fc);
        CHECK(pa && pm && pf && pa < pm && pm < pf);
        snprintf(amd, sizeof amd, "%.*s\"arch\":\"amd64\"%.*s\"machine_type\":\"pc-q35-10.0\"%.*s/usr/share/OVMF/OVMF_CODE_4M.secboot.fd%s",
                 (int)(pa - GOOD), GOOD, (int)(pm - pa - strlen(a)), pa + strlen(a), (int)(pf - pm - strlen(mt)), pm + strlen(mt), pf + strlen(fc));
        struct { const char *add; int schema, want; } s5[] = {
            { "\"secure_boot\":true,\"tpm\":true,\"hyperv\":true", 5, VM_OK },
            { "\"tpm\":true", 5, VM_OK }, { "\"secure_boot\":false", 5, VM_OK },
            { "\"tpm\":1", 5, VM_E_FIELD }, { "\"hyperv\":\"yes\"", 5, VM_E_FIELD },
            { "\"tpm\":true", 4, VM_E_FIELD }, { "\"secure_boot\":true", 4, VM_E_FIELD },
            /* graphics: "2d" or "3d", schema 5, never with the windows devices */
            { "\"graphics\":\"3d\"", 5, VM_OK }, { "\"graphics\":\"2d\"", 5, VM_OK },
            { "\"graphics\":\"4d\"", 5, VM_E_RANGE }, { "\"graphics\":3", 5, VM_E_FIELD },
            { "\"graphics\":\"3d\"", 4, VM_E_FIELD },
            { "\"graphics\":\"3d\",\"devices\":\"windows\"", 5, VM_E_RANGE },
            { "\"graphics\":\"2d\",\"devices\":\"windows\"", 5, VM_OK },
        };
        for (size_t i = 0; i < sizeof s5 / sizeof s5[0]; i++) {
            char t[4096];
            const char *at = strstr(amd, "\"schema\":1");
            snprintf(t, sizeof t, "%.*s\"schema\":%d%.*s,%s}", (int)(at - amd), amd, s5[i].schema, (int)(strlen(at) - 10 - 1), at + 10, s5[i].add);
            int r = vm_machine_parse(t, strlen(t), &m, err, sizeof err);
            if (r != s5[i].want) fprintf(stderr, "  schema 5 %s: got %d (%s)\n", s5[i].add, r, err);
            CHECK(r == s5[i].want);
            if (r == VM_OK) {
                char *jj = vm_machine_json(&m);
                CHECK(jj && vm_machine_parse(jj, strlen(jj), &m2, err, sizeof err) == VM_OK && !memcmp(&m, &m2, sizeof m));
                free(jj);
            }
        }
        /* arm64 (GOOD) and a bios machine refuse them */
        {
            char t[4096];
            const char *at = strstr(GOOD, "\"schema\":1");
            snprintf(t, sizeof t, "%.*s\"schema\":5%.*s,\"tpm\":true}", (int)(at - GOOD), GOOD, (int)(strlen(at) - 10 - 1), at + 10);
            CHECK(vm_machine_parse(t, strlen(t), &m, err, sizeof err) == VM_E_RANGE);
            CHECK(vm_machine_parse(amd, strlen(amd), &m, err, sizeof err) == VM_OK);
            m.schema = 5; m.firmware = VM_FW_BIOS; m.firmware_code[0] = m.firmware_sha256[0] = 0; m.secure_boot = 1;
            CHECK(vm_machine_check(&m, err, sizeof err) == VM_E_RANGE);
            m.secure_boot = 0; m.tpm = 1;
            CHECK(vm_machine_check(&m, err, sizeof err) == VM_OK);
        }
        /* a 3D machine's file names it; a 2D one's doesn't (older builds read it unchanged) */
        {
            CHECK(vm_machine_parse(amd, strlen(amd), &m, err, sizeof err) == VM_OK);
            m.schema = 5; m.graphics = VM_GFX_3D;
            char *jj = vm_machine_json(&m);
            CHECK(jj && strstr(jj, "\"graphics\": \"3d\"") && vm_machine_parse(jj, strlen(jj), &m2, err, sizeof err) == VM_OK && m2.graphics == VM_GFX_3D);
            free(jj);
            m.graphics = VM_GFX_2D;
            jj = vm_machine_json(&m);
            CHECK(jj && !strstr(jj, "graphics"));
            free(jj);
        }
    }
    /* schema 6 (sound, 6.9): sound and microphone true or false, volume 0..100; a file without them reads as sound on
     * at full volume and the microphone off; never in schema 5 */
    {
        struct { const char *add; int schema, want, sound, volume, mic; } s6[] = {
            { "\"sound\":false,\"volume\":40,\"microphone\":true", 6, VM_OK, 0, 40, 1 },
            { "\"volume\":0", 6, VM_OK, 1, 0, 0 }, { "\"volume\":100", 6, VM_OK, 1, 100, 0 },
            { "\"microphone\":false", 6, VM_OK, 1, 100, 0 },
            { "\"volume\":101", 6, VM_E_RANGE, 0, 0, 0 }, { "\"volume\":-1", 6, VM_E_RANGE, 0, 0, 0 },
            { "\"volume\":50.5", 6, VM_E_FIELD, 0, 0, 0 }, { "\"volume\":\"50\"", 6, VM_E_FIELD, 0, 0, 0 },
            { "\"sound\":1", 6, VM_E_FIELD, 0, 0, 0 }, { "\"microphone\":\"on\"", 6, VM_E_FIELD, 0, 0, 0 },
            { "\"microphone\":true", 5, VM_E_FIELD, 0, 0, 0 }, { "\"sound\":false", 5, VM_E_FIELD, 0, 0, 0 },
            { "\"volume\":100", 5, VM_OK, 1, 100, 0 },     /* the default it means anyway */
        };
        for (size_t i = 0; i < sizeof s6 / sizeof s6[0]; i++) {
            char t[4096];
            const char *at = strstr(GOOD, "\"schema\":1");
            snprintf(t, sizeof t, "%.*s\"schema\":%d%.*s,%s}", (int)(at - GOOD), GOOD, s6[i].schema, (int)(strlen(at) - 10 - 1), at + 10, s6[i].add);
            int r = vm_machine_parse(t, strlen(t), &m, err, sizeof err);
            if (r != s6[i].want) fprintf(stderr, "  schema 6 %s: got %d (%s)\n", s6[i].add, r, err);
            CHECK(r == s6[i].want);
            if (r == VM_OK) {
                CHECK(m.sound == s6[i].sound && m.volume == s6[i].volume && m.microphone == s6[i].mic);
                m.schema = VM_SCHEMA;
                char *jj = vm_machine_json(&m);
                CHECK(jj && strstr(jj, "\"volume\"") && vm_machine_parse(jj, strlen(jj), &m2, err, sizeof err) == VM_OK && !memcmp(&m, &m2, sizeof m));
                free(jj);
            }
        }
        /* an old file: the defaults */
        CHECK(vm_machine_parse(GOOD, strlen(GOOD), &m, err, sizeof err) == VM_OK && m.sound == 1 && m.volume == 100 && m.microphone == 0);
        VmHost hh = { .arch = VM_ARCH_ARM64, .cpus = 4, .ram_mb = 8192, .kvm = 1 };
        vm_machine_defaults(&m, &hh, VM_ARCH_ARM64);
        CHECK(m.sound == 1 && m.volume == 100 && m.microphone == 0);
    }
    /* schema 7 (the transfer folder, 6.7.1): true or false; a file without it reads as off (a machine made before keeps
     * its hardware), new machines have it; never below schema 7; the tag LordOS is the transfer folder's then */
    {
        struct { const char *add; int schema, want, on; } s7[] = {
            { "\"transfer\":true", 7, VM_OK, 1 }, { "\"transfer\":false", 7, VM_OK, 0 },
            { "\"transfer\":1", 7, VM_E_FIELD, 0 }, { "\"transfer\":\"on\"", 7, VM_E_FIELD, 0 },
            { "\"transfer\":true", 6, VM_E_FIELD, 0 }, { "\"transfer\":false", 6, VM_OK, 0 },
            { "\"transfer\":true,\"shares\":[{\"tag\":\"LordOS\",\"path\":\"/home/u/x\",\"writable\":true}]", 7, VM_E_RANGE, 0 },
            { "\"transfer\":false,\"shares\":[{\"tag\":\"LordOS\",\"path\":\"/home/u/x\",\"writable\":true}]", 7, VM_OK, 0 },
            { "\"transfer\":true,\"shares\":[{\"tag\":\"Work\",\"path\":\"/home/u/x\",\"writable\":true}]", 7, VM_OK, 1 },
        };
        for (size_t i = 0; i < sizeof s7 / sizeof s7[0]; i++) {
            char t[4096];
            const char *at = strstr(GOOD, "\"schema\":1");
            snprintf(t, sizeof t, "%.*s\"schema\":%d%.*s,%s}", (int)(at - GOOD), GOOD, s7[i].schema, (int)(strlen(at) - 10 - 1), at + 10, s7[i].add);
            int r = vm_machine_parse(t, strlen(t), &m, err, sizeof err);
            if (r != s7[i].want) fprintf(stderr, "  schema 7 %s (%d): got %d (%s)\n", s7[i].add, s7[i].schema, r, err);
            CHECK(r == s7[i].want);
            if (r == VM_OK) {
                CHECK(m.transfer == s7[i].on);
                m.schema = VM_SCHEMA;
                char *jj = vm_machine_json(&m);
                CHECK(jj && strstr(jj, s7[i].on ? "\"transfer\": true" : "\"transfer\": false") && vm_machine_parse(jj, strlen(jj), &m2, err, sizeof err) == VM_OK && !memcmp(&m, &m2, sizeof m));
                free(jj);
            }
        }
        CHECK(vm_machine_parse(GOOD, strlen(GOOD), &m, err, sizeof err) == VM_OK && m.transfer == 0);
        VmHost hh = { .arch = VM_ARCH_AMD64, .cpus = 4, .ram_mb = 8192, .kvm = 1 };
        vm_machine_defaults(&m, &hh, VM_ARCH_AMD64);
        CHECK(m.transfer == 1 && m.schema == VM_SCHEMA && VM_SCHEMA >= 7);
        /* where it is: <library>/Transfer/<folder name without .lordvm>; nothing for a folder that isn't <name>.lordvm */
        char xp[PATH_MAX];
        CHECK(vm_transfer_path("/home/u/Machines/Ubuntu 26.04.lordvm", xp, sizeof xp) == 0 && !strcmp(xp, "/home/u/Machines/Transfer/Ubuntu 26.04"));
        CHECK(vm_transfer_path("/home/u/Machines/Win.lordvm/", xp, sizeof xp) == 0 && !strcmp(xp, "/home/u/Machines/Transfer/Win"));
        CHECK(vm_transfer_path("/home/u/Machines/x.img", xp, sizeof xp) < 0);
        CHECK(vm_transfer_path("/home/u/Machines/.lordvm", xp, sizeof xp) < 0);
        CHECK(vm_transfer_path("/home/u/Machines/.hidden.lordvm", xp, sizeof xp) < 0);
        CHECK(vm_transfer_path("x.lordvm", xp, sizeof xp) < 0);
        CHECK(vm_transfer_path("/a.lordvm", xp, sizeof xp) < 0);
        CHECK(vm_transfer_path("/home/u/Mach,ines/a.lordvm", xp, sizeof xp) < 0);       /* a path QEMU can't take */
        CHECK(vm_transfer_path("/home/u/Machines/a.lordvm", xp, 20) < 0);                /* doesn't fit */
        CHECK(vm_transfer_path(NULL, xp, sizeof xp) < 0);
        /* a person's share can't be the transfer folder: it's inside the library */
    }
    /* schema 8 (the sound's output and input, 6.9): a node.name each, printable without spaces, at most VM_SOUND_DEV_MAX;
     * "" or no key is the default; never below schema 8; written only when chosen */
    {
        char longn[VM_SOUND_DEV_MAX + 2], add[400];
        memset(longn, 'a', sizeof longn - 1); longn[sizeof longn - 1] = 0;          /* one too many */
        struct { const char *add; int schema, want; const char *out, *in; } s8[] = {
            { "\"sound_out\":\"alsa_output.pci-0000_00_1f.3.analog-stereo\"", 8, VM_OK, "alsa_output.pci-0000_00_1f.3.analog-stereo", "" },
            { "\"sound_in\":\"bluez_input.AA_BB_CC_DD_EE_FF.0\"", 8, VM_OK, "", "bluez_input.AA_BB_CC_DD_EE_FF.0" },
            { "\"sound_out\":\"\",\"sound_in\":\"\"", 8, VM_OK, "", "" },
            { "\"sound_out\":\"a b\"", 8, VM_E_RANGE, "", "" }, { "\"sound_in\":\"a\\tb\"", 8, VM_E_RANGE, "", "" },
            { "\"sound_out\":\"a\\u0001\"", 8, VM_E_RANGE, "", "" }, { "\"sound_out\":\"caf\u00e9\"", 8, VM_E_RANGE, "", "" },
            { "\"sound_out\":1", 8, VM_E_FIELD, "", "" }, { "\"sound_in\":true", 8, VM_E_FIELD, "", "" },
            { "\"sound_out\":\"x\"", 7, VM_E_FIELD, "", "" }, { "\"sound_in\":\"x\"", 7, VM_E_FIELD, "", "" },
            { "\"sound_out\":\"\"", 7, VM_OK, "", "" },      /* the default it means anyway */
            { add, 8, VM_E_RANGE, "", "" },
        };
        snprintf(add, sizeof add, "\"sound_out\":\"%s\"", longn);
        for (size_t i = 0; i < sizeof s8 / sizeof s8[0]; i++) {
            char t[4096];
            const char *at = strstr(GOOD, "\"schema\":1");
            snprintf(t, sizeof t, "%.*s\"schema\":%d%.*s,%s}", (int)(at - GOOD), GOOD, s8[i].schema, (int)(strlen(at) - 10 - 1), at + 10, s8[i].add);
            int r = vm_machine_parse(t, strlen(t), &m, err, sizeof err);
            if (r != s8[i].want) fprintf(stderr, "  schema 8 %s (%d): got %d (%s)\n", s8[i].add, s8[i].schema, r, err);
            CHECK(r == s8[i].want);
            if (r == VM_OK) {
                CHECK(!strcmp(m.sound_out, s8[i].out) && !strcmp(m.sound_in, s8[i].in));
                m.schema = VM_SCHEMA;
                char *jj = vm_machine_json(&m);
                CHECK(jj && !strstr(jj, "\"sound_out\": \"\"") && !strstr(jj, "\"sound_in\": \"\"") && (!*s8[i].out || strstr(jj, s8[i].out)) && (!*s8[i].in || strstr(jj, s8[i].in)));
                CHECK(jj && vm_machine_parse(jj, strlen(jj), &m2, err, sizeof err) == VM_OK && !memcmp(&m, &m2, sizeof m));
                free(jj);
            }
        }
        /* schema 9: clipboard_ask, true or false, never below schema 9, written only when true */
        {
            struct { const char *add; int schema, want, ask; } s9[] = {
                { "\"clipboard_ask\":true", 9, VM_OK, 1 }, { "\"clipboard_ask\":false", 9, VM_OK, 0 }, { "\"agent\":true", 9, VM_OK, 0 },
                { "\"clipboard_ask\":1", 9, VM_E_FIELD, 0 }, { "\"clipboard_ask\":\"yes\"", 9, VM_E_FIELD, 0 },
                { "\"clipboard_ask\":true", 8, VM_E_FIELD, 0 }, { "\"clipboard_ask\":false", 8, VM_OK, 0 },
            };
            for (size_t i = 0; i < sizeof s9 / sizeof s9[0]; i++) {
                char t[4096];
                const char *at = strstr(GOOD, "\"schema\":1");
                snprintf(t, sizeof t, "%.*s\"schema\":%d%.*s,%s}", (int)(at - GOOD), GOOD, s9[i].schema, (int)(strlen(at) - 10 - 1), at + 10, s9[i].add);
                int r = vm_machine_parse(t, strlen(t), &m, err, sizeof err);
                if (r != s9[i].want) fprintf(stderr, "  schema 9 %s (%d): got %d (%s)\n", s9[i].add, s9[i].schema, r, err);
                CHECK(r == s9[i].want);
                if (r != VM_OK) continue;
                CHECK(m.clipboard_ask == s9[i].ask);
                m.schema = VM_SCHEMA;
                char *jj = vm_machine_json(&m);
                CHECK(jj && !!strstr(jj, "\"clipboard_ask\": true") == s9[i].ask && !strstr(jj, "\"clipboard_ask\": false"));
                CHECK(jj && vm_machine_parse(jj, strlen(jj), &m2, err, sizeof err) == VM_OK && !memcmp(&m, &m2, sizeof m));
                free(jj);
            }
            CHECK(VM_SCHEMA >= 9);
        }
        /* schema 10: pointer, "lordos" (the default, never written) or "guest", never below schema 10 */
        {
            struct { const char *add; int schema, want, guest; } s10[] = {
                { "\"pointer\":\"guest\"", 10, VM_OK, 1 }, { "\"pointer\":\"lordos\"", 10, VM_OK, 0 }, { "\"agent\":true", 10, VM_OK, 0 },
                { "\"pointer\":\"host\"", 10, VM_E_RANGE, 0 }, { "\"pointer\":1", 10, VM_E_FIELD, 0 }, { "\"pointer\":true", 10, VM_E_FIELD, 0 },
                { "\"pointer\":\"guest\"", 9, VM_E_FIELD, 0 }, { "\"pointer\":\"lordos\"", 9, VM_OK, 0 },
            };
            for (size_t i = 0; i < sizeof s10 / sizeof s10[0]; i++) {
                char t[4096];
                const char *at = strstr(GOOD, "\"schema\":1");
                snprintf(t, sizeof t, "%.*s\"schema\":%d%.*s,%s}", (int)(at - GOOD), GOOD, s10[i].schema, (int)(strlen(at) - 10 - 1), at + 10, s10[i].add);
                int r = vm_machine_parse(t, strlen(t), &m, err, sizeof err);
                if (r != s10[i].want) fprintf(stderr, "  schema 10 %s (%d): got %d (%s)\n", s10[i].add, s10[i].schema, r, err);
                CHECK(r == s10[i].want);
                if (r != VM_OK) continue;
                CHECK(m.pointer_guest == s10[i].guest);
                m.schema = VM_SCHEMA;
                char *jj = vm_machine_json(&m);
                CHECK(jj && !!strstr(jj, "\"pointer\": \"guest\"") == s10[i].guest && !strstr(jj, "\"lordos\""));
                CHECK(jj && vm_machine_parse(jj, strlen(jj), &m2, err, sizeof err) == VM_OK && !memcmp(&m, &m2, sizeof m));
                free(jj);
            }
            CHECK(VM_SCHEMA >= 10);
        }
        /* schema 11: gpu, an Intel vGPU ("intel-whole" | "intel-shared"; none is never written), amd64 UEFI only, never
         * with 3D graphics, never below schema 11 */
        {
            char amd[2048];
            const char *a = "\"arch\":\"arm64\"", *mt = "\"machine_type\":\"virt-10.0\"", *fc = "/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd";
            const char *pa = strstr(GOOD, a), *pm = strstr(GOOD, mt), *pf = strstr(GOOD, fc);
            snprintf(amd, sizeof amd, "%.*s\"arch\":\"amd64\"%.*s\"machine_type\":\"pc-q35-10.0\"%.*s/usr/share/OVMF/OVMF_CODE_4M.secboot.fd%s",
                     (int)(pa - GOOD), GOOD, (int)(pm - pa - strlen(a)), pa + strlen(a), (int)(pf - pm - strlen(mt)), pm + strlen(mt), pf + strlen(fc));
            struct { const char *add; int schema, arm, want, vgpu; } s11[] = {
                { "\"gpu\":\"intel-whole\"", 11, 0, VM_OK, VM_VGPU_WHOLE }, { "\"gpu\":\"intel-shared\"", 11, 0, VM_OK, VM_VGPU_SHARED },
                { "\"gpu\":\"\"", 11, 0, VM_OK, 0 }, { "\"agent\":true", 11, 0, VM_OK, 0 },
                { "\"gpu\":\"amd\"", 11, 0, VM_E_RANGE, 0 }, { "\"gpu\":1", 11, 0, VM_E_FIELD, 0 }, { "\"gpu\":\"intel-whole\"", 10, 0, VM_E_FIELD, 0 },
                { "\"gpu\":\"intel-whole\"", 11, 1, VM_E_RANGE, 0 }, { "\"gpu\":\"intel-whole\",\"graphics\":\"3d\"", 11, 0, VM_E_RANGE, 0 },
                { "\"gpu\":\"intel-whole\",\"firmware\":\"bios\"", 11, 0, VM_E_JSON, 0 },
            };
            for (size_t i = 0; i < sizeof s11 / sizeof s11[0]; i++) {
                if (s11[i].want == VM_E_JSON) continue;          /* a key twice isn't this test's business */
                const char *base = s11[i].arm ? GOOD : amd;
                char t[4096];
                const char *at = strstr(base, "\"schema\":1");
                snprintf(t, sizeof t, "%.*s\"schema\":%d%.*s,%s}", (int)(at - base), base, s11[i].schema, (int)(strlen(at) - 10 - 1), at + 10, s11[i].add);
                int r = vm_machine_parse(t, strlen(t), &m, err, sizeof err);
                if (r != s11[i].want) fprintf(stderr, "  schema 11 %s (%d): got %d (%s)\n", s11[i].add, s11[i].schema, r, err);
                CHECK(r == s11[i].want);
                if (r != VM_OK) continue;
                CHECK(m.vgpu == s11[i].vgpu);
                m.schema = VM_SCHEMA;
                char *jj = vm_machine_json(&m);
                CHECK(jj && !!strstr(jj, "\"gpu\": ") == !!s11[i].vgpu);
                CHECK(jj && vm_machine_parse(jj, strlen(jj), &m2, err, sizeof err) == VM_OK && !memcmp(&m, &m2, sizeof m));
                free(jj);
            }
            CHECK(VM_SCHEMA >= 11);
        }
        /* schema 12: capture_fullscreen ("Capture mouse", 6.25), true or false, written only when true, never below 12 */
        {
            struct { const char *add; int schema, want, on; } s12[] = {
                { "\"capture_fullscreen\":true", 12, VM_OK, 1 }, { "\"capture_fullscreen\":false", 12, VM_OK, 0 }, { "\"agent\":true", 12, VM_OK, 0 },
                { "\"capture_fullscreen\":1", 12, VM_E_FIELD, 0 }, { "\"capture_fullscreen\":\"yes\"", 12, VM_E_FIELD, 0 },
                { "\"capture_fullscreen\":true", 11, VM_E_FIELD, 0 }, { "\"capture_fullscreen\":false", 11, VM_OK, 0 },
            };
            for (size_t i = 0; i < sizeof s12 / sizeof s12[0]; i++) {
                char t[4096];
                const char *at = strstr(GOOD, "\"schema\":1");
                snprintf(t, sizeof t, "%.*s\"schema\":%d%.*s,%s}", (int)(at - GOOD), GOOD, s12[i].schema, (int)(strlen(at) - 10 - 1), at + 10, s12[i].add);
                int r = vm_machine_parse(t, strlen(t), &m, err, sizeof err);
                if (r != s12[i].want) fprintf(stderr, "  schema 12 %s (%d): got %d (%s)\n", s12[i].add, s12[i].schema, r, err);
                CHECK(r == s12[i].want);
                if (r != VM_OK) continue;
                CHECK(m.capture_fullscreen == s12[i].on);
                m.schema = VM_SCHEMA;
                char *jj = vm_machine_json(&m);
                CHECK(jj && !!strstr(jj, "\"capture_fullscreen\": true") == s12[i].on && !strstr(jj, "\"capture_fullscreen\": false"));
                CHECK(jj && vm_machine_parse(jj, strlen(jj), &m2, err, sizeof err) == VM_OK && !memcmp(&m, &m2, sizeof m));
                free(jj);
            }
            /* written by an older lord-vm's rules: refused below schema 12 even when set in memory */
            CHECK(vm_machine_parse(GOOD, strlen(GOOD), &m, err, sizeof err) == VM_OK && !m.capture_fullscreen);
            m.capture_fullscreen = 1;
            CHECK(vm_machine_check(&m, err, sizeof err) == VM_E_FIELD);
            m.capture_fullscreen = 2; m.schema = VM_SCHEMA;
            CHECK(vm_machine_check(&m, err, sizeof err) == VM_E_RANGE);
            CHECK(VM_SCHEMA >= 12);
        }
        /* which of QEMU's mice "Capture mouse" makes the active one (query-mice's answer, untrusted) */
        {
            struct { const char *json; int rel, want, cur, added; } mp[] = {
                /* amd64: the PS/2 mouse and the tablet */
                { "[{\"name\":\"QEMU PS/2 Mouse\",\"index\":0,\"current\":false,\"absolute\":false},"
                  "{\"name\":\"QEMU USB Tablet\",\"index\":2,\"current\":true,\"absolute\":true}]", 1, 0, 0, 0 },
                { "[{\"name\":\"QEMU PS/2 Mouse\",\"index\":0,\"current\":false,\"absolute\":false},"
                  "{\"name\":\"QEMU USB Tablet\",\"index\":2,\"current\":true,\"absolute\":true}]", 0, 2, 1, 0 },
                { "[{\"name\":\"QEMU PS/2 Mouse\",\"index\":0,\"current\":true,\"absolute\":false},"
                  "{\"name\":\"QEMU Virtio Tablet\",\"index\":1,\"current\":false,\"absolute\":true}]", 0, 1, 0, 0 },
                /* arm64: an added mouse took over by itself: the tablet again, and it says so */
                { "[{\"name\":\"QEMU USB Tablet\",\"index\":1,\"current\":false,\"absolute\":true},"
                  "{\"name\":\"QEMU USB Mouse\",\"index\":2,\"current\":true,\"absolute\":false}]", 0, 1, 0, 1 },
                /* relative: the USB or virtio mouse before PS/2 */
                { "[{\"name\":\"QEMU PS/2 Mouse\",\"index\":0,\"current\":false,\"absolute\":false},"
                  "{\"name\":\"QEMU Virtio Mouse\",\"index\":3,\"current\":true,\"absolute\":false}]", 1, 3, 1, 1 },
                /* none of the kind wanted; malformed entries skipped */
                { "[{\"name\":\"QEMU USB Tablet\",\"index\":1,\"current\":true,\"absolute\":true}]", 1, -1, 0, 0 },
                { "[{\"name\":\"x\",\"index\":-1,\"absolute\":false},{\"name\":\"y\",\"index\":1.5,\"absolute\":false},"
                  "{\"name\":\"z\",\"index\":\"3\",\"absolute\":false},{\"index\":4,\"absolute\":\"no\"},7]", 1, -1, 0, 0 },
                { "{\"name\":\"QEMU PS/2 Mouse\"}", 1, -1, 0, 0 },
                { "[]", 0, -1, 0, 0 },
            };
            for (size_t i = 0; i < sizeof mp / sizeof mp[0]; i++) {
                JDoc d;
                CHECK(json_parse(mp[i].json, strlen(mp[i].json), &d, err, sizeof err) == 0);
                int cur = -1, added = -1, got = vm_mouse_pick(d.root, mp[i].rel, &cur, &added);
                if (got != mp[i].want || cur != mp[i].cur || added != mp[i].added) fprintf(stderr, "  mouse pick %zu: got %d %d %d\n", i, got, cur, added);
                CHECK(got == mp[i].want && cur == mp[i].cur && added == mp[i].added);
                json_free(&d);
            }
            CHECK(vm_mouse_pick(NULL, 1, &(int){ 0 }, &(int){ 0 }) == -1);
        }
        /* exactly VM_SOUND_DEV_MAX fits */
        longn[VM_SOUND_DEV_MAX] = 0;
        CHECK(vm_sound_dev_valid(longn) && !vm_sound_dev_valid("") && !vm_sound_dev_valid(NULL) && !vm_sound_dev_valid("a\nb"));
        CHECK(vm_machine_parse(GOOD, strlen(GOOD), &m, err, sizeof err) == VM_OK && !m.sound_out[0] && !m.sound_in[0]);
        /* VM_SOUND's text "OUT\tIN" */
        char o[VM_SOUND_DEV_MAX + 1] = "keep", in[VM_SOUND_DEV_MAX + 1] = "keep";
        CHECK(vm_sound_devs_parse("alsa_output.x\talsa_input.y", o, sizeof o, in, sizeof in) == 0 && !strcmp(o, "alsa_output.x") && !strcmp(in, "alsa_input.y"));
        CHECK(vm_sound_devs_parse("\t", o, sizeof o, in, sizeof in) == 0 && !o[0] && !in[0]);
        strcpy(o, "keep"); strcpy(in, "keep");
        CHECK(vm_sound_devs_parse("", o, sizeof o, in, sizeof in) < 0 && vm_sound_devs_parse("a\tb\tc", o, sizeof o, in, sizeof in) < 0);
        CHECK(vm_sound_devs_parse("a b\tc", o, sizeof o, in, sizeof in) < 0 && vm_sound_devs_parse(NULL, o, sizeof o, in, sizeof in) < 0);
        CHECK(vm_sound_devs_parse("a\tb", o, 1, in, sizeof in) < 0 && !strcmp(o, "keep") && !strcmp(in, "keep"));
        char t2[2 * VM_SOUND_DEV_MAX + 4];
        snprintf(t2, sizeof t2, "x\t%sa", longn);                                  /* the input one too long */
        CHECK(vm_sound_devs_parse(t2, o, sizeof o, in, sizeof in) < 0 && !strcmp(in, "keep"));
        CHECK(strlen(t2) < VM_TEXT_MAX + VM_SOUND_DEV_MAX && 2 * VM_SOUND_DEV_MAX + 1 < VM_TEXT_MAX);   /* both fit one message */
    }
    /* the transfer folder is tied to its machine (vm_transfer_claim): an owner record beside it with the machine's id; a
     * folder of that name that's another machine's, or nobody's and not empty, moves aside, never deleted; the Trash moves
     * a machine's own folder aside (vm_transfer_aside) */
    {
        char lib[] = "/tmp/vm-xfer.XXXXXX";
        if (mkdtemp(lib)) {
            const char *A = "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b", *B = "1f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2c";
            char dir[PATH_MAX], xp[PATH_MAX], xp2[PATH_MAX], mv[NAME_MAX + 1], p2[PATH_MAX + 64], own[PATH_MAX + 128];
            snprintf(dir, sizeof dir, "%s/Ubuntu.lordvm", lib);
            struct stat st;
            CHECK(vm_transfer_claim(dir, A, xp, sizeof xp, mv, sizeof mv) == 0 && !mv[0] && !lstat(xp, &st) && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0777);
            snprintf(own, sizeof own, "%s/Transfer/.Ubuntu.machine", lib);
            CHECK(!lstat(own, &st) && S_ISREG(st.st_mode));
            snprintf(p2, sizeof p2, "%s/Transfer", lib);
            CHECK(!lstat(p2, &st) && (st.st_mode & 0777) == 0700);
            snprintf(p2, sizeof p2, "%s/a.txt", xp);
            FILE *f = fopen(p2, "w"); if (f) { fputs("A's\n", f); fclose(f); }
            /* the same machine again: the same folder, its files there */
            CHECK(vm_transfer_claim(dir, A, xp2, sizeof xp2, mv, sizeof mv) == 0 && !mv[0] && !strcmp(xp, xp2) && !access(p2, F_OK));
            /* another machine of the same name: A's folder moves aside with its files, B gets an empty one */
            CHECK(vm_transfer_claim(dir, B, xp2, sizeof xp2, mv, sizeof mv) == 0 && !strncmp(mv, "Ubuntu (old ", 12) && access(p2, F_OK));
            char aside[PATH_MAX + 64];
            snprintf(aside, sizeof aside, "%s/Transfer/%s/a.txt", lib, mv);
            CHECK(!access(aside, F_OK));
            snprintf(aside, sizeof aside, "%s/Transfer/.%s.machine", lib, mv);
            CHECK(!access(aside, F_OK));                       /* its owner record went with it */
            /* once more for A: B's (empty) folder moves aside too, under another name */
            char mv2[NAME_MAX + 1];
            CHECK(vm_transfer_claim(dir, A, xp2, sizeof xp2, mv2, sizeof mv2) == 0 && mv2[0] && strcmp(mv, mv2));
            /* a folder nobody claimed: taken when empty, moved aside when not */
            snprintf(dir, sizeof dir, "%s/Debian.lordvm", lib);
            snprintf(p2, sizeof p2, "%s/Transfer/Debian", lib); mkdir(p2, 0700);
            CHECK(vm_transfer_claim(dir, B, xp, sizeof xp, mv, sizeof mv) == 0 && !mv[0]);
            snprintf(dir, sizeof dir, "%s/Fedora.lordvm", lib);
            snprintf(p2, sizeof p2, "%s/Transfer/Fedora", lib); mkdir(p2, 0700);
            snprintf(own, sizeof own, "%s/x", p2); f = fopen(own, "w"); if (f) fclose(f);
            CHECK(vm_transfer_claim(dir, B, xp, sizeof xp, mv, sizeof mv) == 0 && !strncmp(mv, "Fedora (old ", 12));
            /* a link where the folder should be: moved aside, never followed */
            snprintf(dir, sizeof dir, "%s/Arch.lordvm", lib);
            snprintf(p2, sizeof p2, "%s/Transfer/Arch", lib);
            CHECK(!symlink("/etc", p2));
            CHECK(vm_transfer_claim(dir, A, xp, sizeof xp, mv, sizeof mv) == 0 && mv[0] && !lstat(xp, &st) && S_ISDIR(st.st_mode));
            /* the Trash: only the machine's own folder moves */
            snprintf(dir, sizeof dir, "%s/Ubuntu.lordvm", lib);
            CHECK(vm_transfer_aside(dir, B, mv, sizeof mv) == 1);              /* A's now: B's id doesn't move it */
            CHECK(vm_transfer_aside(dir, A, mv, sizeof mv) == 0 && mv[0]);
            CHECK(vm_transfer_path(dir, xp, sizeof xp) == 0 && lstat(xp, &st));
            CHECK(vm_transfer_aside(dir, A, mv, sizeof mv) == 1);              /* nothing left to move */
            CHECK(vm_transfer_claim(dir, "not-an-id", xp, sizeof xp, mv, sizeof mv) < 0);
            char cmd[PATH_MAX + 16];
            snprintf(cmd, sizeof cmd, "rm -rf '%s'", lib);
            if (system(cmd)) {}
        }
    }
    /* schema 6's boot order: a list of 1 to 3 different devices (disk, cd, network), and boot_menu; neither in the file is
     * auto (the system disk, then the CD/DVD), which is how every older file reads and what a new machine writes */
    {
        struct { const char *add; int schema, want, n, o0, o1, o2, menu; } bo[] = {
            { "\"boot_order\":[\"network\",\"disk\"]", 6, VM_OK, 2, VM_BOOT_NET, VM_BOOT_DISK, -1, 0 },
            { "\"boot_order\":[\"cd\",\"disk\",\"network\"],\"boot_menu\":true", 6, VM_OK, 3, VM_BOOT_CD, VM_BOOT_DISK, VM_BOOT_NET, 1 },
            { "\"boot_order\":[\"disk\"]", 6, VM_OK, 1, VM_BOOT_DISK, -1, -1, 0 },
            { "\"boot_menu\":false", 6, VM_OK, 0, -1, -1, -1, 0 },
            { "\"boot_order\":[]", 6, VM_E_RANGE, 0, 0, 0, 0, 0 },
            { "\"boot_order\":[\"disk\",\"disk\"]", 6, VM_E_RANGE, 0, 0, 0, 0, 0 },
            { "\"boot_order\":[\"disk\",\"cd\",\"network\",\"disk\"]", 6, VM_E_RANGE, 0, 0, 0, 0, 0 },
            { "\"boot_order\":[\"floppy\"]", 6, VM_E_RANGE, 0, 0, 0, 0, 0 },
            { "\"boot_order\":[1]", 6, VM_E_RANGE, 0, 0, 0, 0, 0 },
            { "\"boot_order\":\"auto\"", 6, VM_E_FIELD, 0, 0, 0, 0, 0 },
            { "\"boot_menu\":\"on\"", 6, VM_E_FIELD, 0, 0, 0, 0, 0 },
            { "\"boot_order\":[\"network\"]", 5, VM_E_FIELD, 0, 0, 0, 0, 0 },
            { "\"boot_menu\":true", 5, VM_E_FIELD, 0, 0, 0, 0, 0 },
        };
        for (size_t i = 0; i < sizeof bo / sizeof bo[0]; i++) {
            char t[4096];
            const char *at = strstr(GOOD, "\"schema\":1");
            snprintf(t, sizeof t, "%.*s\"schema\":%d%.*s,%s}", (int)(at - GOOD), GOOD, bo[i].schema, (int)(strlen(at) - 10 - 1), at + 10, bo[i].add);
            int r = vm_machine_parse(t, strlen(t), &m, err, sizeof err);
            if (r != bo[i].want) fprintf(stderr, "  boot %s: got %d (%s)\n", bo[i].add, r, err);
            CHECK(r == bo[i].want);
            if (r != VM_OK) continue;
            const int want[3] = { bo[i].o0, bo[i].o1, bo[i].o2 };
            CHECK(m.n_boot == bo[i].n && m.boot_menu == bo[i].menu);
            for (int k = 0; k < m.n_boot; k++) CHECK(m.boot_order[k] == want[k]);
            char *jj = vm_machine_json(&m);
            CHECK(jj && vm_machine_parse(jj, strlen(jj), &m2, err, sizeof err) == VM_OK && !memcmp(&m, &m2, sizeof m));
            CHECK(jj && !strstr(jj, "boot_order") == !m.n_boot && !strstr(jj, "boot_menu") == !m.boot_menu);
            free(jj);
        }
        /* an old file and a new machine: auto, written as nothing */
        CHECK(vm_machine_parse(GOOD, strlen(GOOD), &m, err, sizeof err) == VM_OK && m.n_boot == 0 && m.boot_menu == 0);
        char *jj = vm_machine_json(&m);
        CHECK(jj && !strstr(jj, "boot_"));
        free(jj);
        /* the command line's text */
        int o[VM_BOOT_DEVS];
        CHECK(vm_boot_parse("auto", o) == 0);
        CHECK(vm_boot_parse("network,disk", o) == 2 && o[0] == VM_BOOT_NET && o[1] == VM_BOOT_DISK);
        CHECK(vm_boot_parse("disk,cd,network", o) == 3 && o[0] == VM_BOOT_DISK && o[1] == VM_BOOT_CD && o[2] == VM_BOOT_NET);
        const char *bad[] = { "", ",", "disk,", ",disk", "disk,disk", "disk,cd,network,disk", "Disk", "auto,disk", "dis", "disks", "network disk", NULL };
        for (int k = 0; bad[k]; k++) CHECK(vm_boot_parse(bad[k], o) == -1);
        CHECK(vm_boot_parse(NULL, o) == -1);
        char sp[32];
        vm_boot_spec(&m, sp, sizeof sp); CHECK(!strcmp(sp, "auto"));
        m.n_boot = vm_boot_parse("cd,network", m.boot_order);
        vm_boot_spec(&m, sp, sizeof sp); CHECK(!strcmp(sp, "cd,network"));
        /* QEMU's bootindex: auto is the disk then the CD (after an installer's kernel at 0: 1 and 2); a chosen order numbers
         * the listed devices and leaves the others out */
        int ix[VM_BOOT_DEVS];
        m.n_boot = 0;
        vm_boot_indexes(&m, 0, ix); CHECK(ix[VM_BOOT_DISK] == 0 && ix[VM_BOOT_CD] == 1 && ix[VM_BOOT_NET] == -1);
        vm_boot_indexes(&m, 1, ix); CHECK(ix[VM_BOOT_DISK] == 1 && ix[VM_BOOT_CD] == 2 && ix[VM_BOOT_NET] == -1);
        m.n_boot = vm_boot_parse("network,disk", m.boot_order);
        vm_boot_indexes(&m, 0, ix); CHECK(ix[VM_BOOT_NET] == 0 && ix[VM_BOOT_DISK] == 1 && ix[VM_BOOT_CD] == -1);
        m.n_boot = vm_boot_parse("cd,network,disk", m.boot_order);
        vm_boot_indexes(&m, 1, ix); CHECK(ix[VM_BOOT_CD] == 1 && ix[VM_BOOT_NET] == 2 && ix[VM_BOOT_DISK] == 3);
        /* checked after a change too (lord-vm --set) */
        m.schema = 6; m.n_boot = 2; m.boot_order[0] = VM_BOOT_CD; m.boot_order[1] = VM_BOOT_CD;
        CHECK(vm_machine_check(&m, err, sizeof err) == VM_E_RANGE);
        m.boot_order[1] = 7;
        CHECK(vm_machine_check(&m, err, sizeof err) == VM_E_RANGE);
        m.n_boot = 0; m.boot_menu = 2;
        CHECK(vm_machine_check(&m, err, sizeof err) == VM_E_RANGE);
    }
    /* which folders may be shared (vm_share_allowed): a real folder in home (not home), under /media/<user> or /mnt, nothing
     * hidden, never the library or what holds it */
    {
        char hm[] = "/tmp/vm-share.XXXXXX";
        if (mkdtemp(hm)) {
            char real[PATH_MAX], d[PATH_MAX + 64], lib[PATH_MAX + 64], lnk[PATH_MAX + 64];
            CHECK(realpath(hm, real) != NULL);
            snprintf(d, sizeof d, "%s/Work", real); mkdir(d, 0700);
            snprintf(lib, sizeof lib, "%s/Machines", real); mkdir(lib, 0700);
            snprintf(lnk, sizeof lnk, "%s/.config", real); mkdir(lnk, 0700);
            snprintf(lnk, sizeof lnk, "%s/Machines/A.lordvm", real); mkdir(lnk, 0700);
            CHECK(vm_share_allowed(d, real, "u", lib, err, sizeof err) == 0);
            CHECK(vm_share_allowed(real, real, "u", lib, err, sizeof err) < 0);            /* all of home */
            CHECK(vm_share_allowed(lib, real, "u", lib, err, sizeof err) < 0);             /* the library */
            CHECK(vm_share_allowed(lnk, real, "u", lib, err, sizeof err) < 0);             /* a machine inside it */
            snprintf(lnk, sizeof lnk, "%s/.config", real);
            CHECK(vm_share_allowed(lnk, real, "u", lib, err, sizeof err) < 0);             /* hidden */
            CHECK(vm_share_allowed(d, real, "u", real, err, sizeof err) < 0);
            snprintf(lnk, sizeof lnk, "%s/Link", real);
            CHECK(!symlink(d, lnk) && vm_share_allowed(lnk, real, "u", lib, err, sizeof err) < 0);   /* a symbolic link */
            snprintf(lnk, sizeof lnk, "%s/Missing", real);
            CHECK(vm_share_allowed(lnk, real, "u", lib, err, sizeof err) < 0);
            CHECK(vm_share_allowed("/etc", real, "u", lib, err, sizeof err) < 0);
            CHECK(vm_share_allowed("/", real, "u", lib, err, sizeof err) < 0);
            CHECK(vm_share_allowed("/mnt", real, "u", lib, err, sizeof err) < 0);
            snprintf(lnk, sizeof lnk, "%s/Link", real); unlink(lnk);
            snprintf(lnk, sizeof lnk, "%s/Machines/A.lordvm", real); rmdir(lnk);
            rmdir(lib); rmdir(d);
            snprintf(lnk, sizeof lnk, "%s/.config", real); rmdir(lnk);
            rmdir(real);
        }
    }
    /* amd64 with bios: no firmware_code */
    vm_machine_parse(GOOD, strlen(GOOD), &m, err, sizeof err);
    m.arch = VM_ARCH_AMD64;
    snprintf(m.machine_type, sizeof m.machine_type, "pc-q35-10.0");
    m.firmware = VM_FW_BIOS;
    CHECK(vm_machine_check(&m, err, sizeof err) == VM_E_RANGE);
    m.firmware_code[0] = m.firmware_sha256[0] = 0;
    CHECK(vm_machine_check(&m, err, sizeof err) == VM_OK);
    j = vm_machine_json(&m);
    CHECK(j && !strstr(j, "firmware_code") && vm_machine_parse(j, strlen(j), &m2, err, sizeof err) == VM_OK && m2.firmware == VM_FW_BIOS);
    free(j);
    /* save and load */
    char tmpl[] = "/tmp/vm-test.XXXXXX";
    if (mkdtemp(tmpl)) {
        /* a schema 1 machine: written as schema 2, its old file kept beside it once */
        char p0[256];
        snprintf(p0, sizeof p0, "%s/machine.json", tmpl);
        char *old = vm_machine_json(&m);
        int fd0 = open(p0, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd0 >= 0 && old) { (void)!write(fd0, old, strlen(old)); close(fd0); }
        free(old);
        CHECK(m.schema == 1 && vm_machine_save(tmpl, &m) == VM_OK);
        VmMachine m3 = m;
        m3.schema = VM_SCHEMA;
        char *j1 = vm_machine_json(&m3), *j2 = NULL;
        CHECK(vm_machine_load(tmpl, &m2, err, sizeof err) == VM_OK && m2.schema == VM_SCHEMA && (j2 = vm_machine_json(&m2)) && !strcmp(j1, j2));
        free(j1); free(j2);
        char p[256];
        snprintf(p, sizeof p, "%s/machine.json.schema1", tmpl);
        CHECK(!access(p, F_OK));
        unlink(p);
        snprintf(p, sizeof p, "%s/machine.json", tmpl); unlink(p);
        rmdir(tmpl);
    }
}

static void test_host(void) {
    VmMachine m;
    char err[256], warn[256];
    vm_machine_parse(GOOD, strlen(GOOD), &m, err, sizeof err);
    VmHost pi = { VM_ARCH_ARM64, 4, 16208, 1 }, small = { VM_ARCH_ARM64, 4, 3884, 1 }, pc = { VM_ARCH_AMD64, 8, 16000, 1 };
    CHECK(vm_vcpus_recommended(&pi) == 3 && vm_mem_max_mb(&pi) == 16208 - 2048 && vm_mem_max_mb(&small) == 3884 - 1024);
    CHECK(vm_machine_check_host(&m, &pi, err, sizeof err, warn, sizeof warn) == VM_OK && !warn[0]);
    m.vcpus = 4;
    CHECK(vm_machine_check_host(&m, &pi, err, sizeof err, warn, sizeof warn) == VM_OK && warn[0]);   /* allowed, advised against */
    m.vcpus = 5;
    CHECK(vm_machine_check_host(&m, &pi, err, sizeof err, warn, sizeof warn) == VM_E_HOST);
    m.vcpus = 3;
    m.memory_mb = 14160;
    CHECK(vm_machine_check_host(&m, &pi, err, sizeof err, warn, sizeof warn) == VM_OK);
    m.memory_mb = 14161;
    CHECK(vm_machine_check_host(&m, &pi, err, sizeof err, warn, sizeof warn) == VM_E_HOST);
    m.memory_mb = 3000;
    CHECK(vm_machine_check_host(&m, &small, err, sizeof err, warn, sizeof warn) == VM_E_HOST);
    m.memory_mb = 2048;
    /* KVM: same architecture and /dev/kvm only */
    CHECK(vm_machine_check_host(&m, &pc, err, sizeof err, warn, sizeof warn) == VM_E_HOST);
    m.accel = VM_ACCEL_TCG;
    CHECK(vm_machine_check_host(&m, &pc, err, sizeof err, warn, sizeof warn) == VM_OK);
    m.accel = VM_ACCEL_KVM;
    pi.kvm = 0;
    CHECK(vm_machine_check_host(&m, &pi, err, sizeof err, warn, sizeof warn) == VM_E_HOST);
    pi.kvm = 1;
    /* limits and budget */
    CHECK(vm_mem_limit_mb(&m) == 2048 + VM_OVERHEAD_KVM_MB);
    m.accel = VM_ACCEL_TCG;
    CHECK(vm_mem_limit_mb(&m) == 2048 + VM_OVERHEAD_TCG_MB);
    CHECK(vm_budget_fits(&pi, 0, 14160) && !vm_budget_fits(&pi, 0, 14161));
    CHECK(vm_budget_fits(&pi, 8000, 6000) && !vm_budget_fits(&pi, 8000, 7000) && !vm_budget_fits(&pi, -1, 10));
    /* defaults */
    VmMachine d;
    vm_machine_defaults(&d, &pi, VM_ARCH_ARM64);
    CHECK(d.vcpus == 3 && d.memory_mb == 2048 && d.accel == VM_ACCEL_KVM && vm_uuid_valid(d.id) && vm_mac_valid(d.mac));
    vm_machine_defaults(&d, &pc, VM_ARCH_ARM64);
    CHECK(d.vcpus == 4 && d.accel == VM_ACCEL_TCG);
    VmMachine e;
    vm_machine_defaults(&e, &pi, VM_ARCH_ARM64);
    CHECK(strcmp(d.id, e.id) && d.id[14] == '4');
}

static void test_messages(void) {
    VmMsg q = vm_msg(VM_SHUTDOWN, 1, 30, 0, 0, 0, NULL);
    CHECK(vm_msg_valid(&q, 1));
    q.a = 601; CHECK(!vm_msg_valid(&q, 1));
    q.a = -1; CHECK(!vm_msg_valid(&q, 1));
    q = vm_msg(VM_STATE, 0, VS_RUNNING, VR_NONE, 0, 0, "x");
    CHECK(vm_msg_valid(&q, 0) && !vm_msg_valid(&q, 1));
    q.a = VS_COUNT; CHECK(!vm_msg_valid(&q, 0));
    q = vm_msg(VM_REPLY, 3, 99, 0, 0, 0, NULL); CHECK(!vm_msg_valid(&q, 0));
    q = vm_msg(200, 0, 0, 0, 0, 0, NULL); CHECK(!vm_msg_valid(&q, 0) && !vm_msg_valid(&q, 1));
    q = vm_msg(VM_STATUS, 0, 0, 0, 0, 0, NULL); q.version = 2; CHECK(!vm_msg_valid(&q, 1));
    /* Beta: the CD/DVD drive while running is a request; a notice comes only from lord-vm, with a known kind */
    q = vm_msg(VM_CDROM, 4, 0, 0, 0, 0, "/x.iso"); CHECK(vm_msg_valid(&q, 1) && !vm_msg_valid(&q, 0));
    q = vm_msg(VM_NOTICE, 0, VMN_NET_LOST, 11, 0, 0, "passt"); CHECK(vm_msg_valid(&q, 0) && !vm_msg_valid(&q, 1));
    q.a = VMN_COUNT; CHECK(!vm_msg_valid(&q, 0));
    q.a = VMN_NONE; CHECK(!vm_msg_valid(&q, 0));
    /* a dropped file (VM_FILE) is a request with a name; its progress comes only from lord-vm, 0..100 */
    q = vm_msg(VM_FILE, 6, 0, 0, 0, 0, "a.txt"); CHECK(vm_msg_valid(&q, 1) && !vm_msg_valid(&q, 0));
    q = vm_msg(VM_FILE, 6, 0, 0, 0, 0, ""); CHECK(!vm_msg_valid(&q, 1));
    q = vm_msg(VM_FILE_PROGRESS, 6, 42, 0, 0, 0, "a.txt"); CHECK(vm_msg_valid(&q, 0) && !vm_msg_valid(&q, 1));
    q = vm_msg(VM_FILE_PROGRESS, 6, 42, 1, 0, 0, "a.txt"); CHECK(vm_msg_valid(&q, 0));       /* b: through the transfer folder */
    /* the transfer folder (6.7.1): VAG_TRANSFER is an agent request (WinFsp's hash as text, or none); the agent's state
     * says whether the guest has it (VAC_TRANSFER) or can once WinFsp is in (VAC_XFER_SETUP) */
    q = vm_msg(VM_AGENT, 7, VAG_TRANSFER, 0, 0, 0, ""); CHECK(vm_msg_valid(&q, 1));
    q = vm_msg(VM_AGENT, 7, VAG_EXTRA_ON + 1, 0, 0, 0, ""); CHECK(!vm_msg_valid(&q, 1));
    q = vm_msg(VM_AGENT_STATE, 0, VAS_CONNECTED, VAC_EXEC | VAC_FILES | VAC_TRANSFER, 0, 0, "1\tx"); CHECK(vm_msg_valid(&q, 0));
    q = vm_msg(VM_AGENT_STATE, 0, VAS_CONNECTED, VAC_EXEC | VAC_XFER_SETUP, 0, 0, "1\tx"); CHECK(vm_msg_valid(&q, 0));
    q = vm_msg(VM_AGENT_STATE, 0, VAS_CONNECTED, VAC_DISPLAY | (int)LAG_CAP_WINDOWS | VAC_TRANSFER | VAC_XFER_SETUP, 0, 0, "1\tx"); CHECK(vm_msg_valid(&q, 0));
    q = vm_msg(VM_AGENT_STATE, 0, VAS_CONNECTED, VAC_CLIP_NOTOOL, 0, 0, "1\tx"); CHECK(vm_msg_valid(&q, 0));       /* "needs xclip" reaches the app */
    q = vm_msg(VM_AGENT_STATE, 0, VAS_CONNECTED, VAC_CLIPBOARD | VAC_CLIP_LAZY, 0, 0, "1\tx"); CHECK(vm_msg_valid(&q, 0));
    q = vm_msg(VM_AGENT_STATE, 0, VAS_CONNECTED, VAC_ALL + 1, 0, 0, "1\tx"); CHECK(!vm_msg_valid(&q, 0));
    q.a = 101; CHECK(!vm_msg_valid(&q, 0));
    q.a = -1; CHECK(!vm_msg_valid(&q, 0));
    char longtext[400];
    memset(longtext, 'a', sizeof longtext - 1); longtext[sizeof longtext - 1] = 0;
    q = vm_msg(VM_QMP, 0, 0, 0, 0, 0, longtext);
    CHECK(strlen(q.text) == VM_TEXT_MAX - 1);

    VdMsg d = vd_msg(VD_KEY, 30, 1, 0, 0, 0, 0); CHECK(vd_msg_valid(&d, 1));
    d.a = 0; CHECK(!vd_msg_valid(&d, 1));
    d.a = 768; CHECK(!vd_msg_valid(&d, 1));
    d = vd_msg(VD_KEY, 30, 2, 0, 0, 0, 0); CHECK(!vd_msg_valid(&d, 1));
    d = vd_msg(VD_BUTTON, 3, 1, 0, 0, 0, 0); CHECK(!vd_msg_valid(&d, 1));     /* wheel buttons only through VD_WHEEL */
    d = vd_msg(VD_BUTTON, VDB_RIGHT, 1, 0, 0, 0, 0); CHECK(vd_msg_valid(&d, 1));
    d = vd_msg(VD_MOTION, -1, 0, 0, 0, 0, 0); CHECK(!vd_msg_valid(&d, 1));
    d = vd_msg(VD_RESIZE, 100, 100, 0, 0, 0, 0); CHECK(!vd_msg_valid(&d, 1));
    d = vd_msg(VD_RESIZE, 1920, 1080, 0, 0, 0, 0); CHECK(vd_msg_valid(&d, 1));
    d = vd_msg(VD_RATE, 0, 0, 0, 0, 0, 0); CHECK(!vd_msg_valid(&d, 1));
    d = vd_msg(VD_WHEEL, 40, 0, 0, 0, 0, 0); CHECK(!vd_msg_valid(&d, 1));
    d = vd_msg(VD_SIZE, 8192, 8192, 0, 0, 0, 0); CHECK(vd_msg_valid(&d, 0));
    d = vd_msg(VD_SIZE, 8193, 10, 0, 0, 0, 0); CHECK(!vd_msg_valid(&d, 0));
    d = vd_msg(VD_SIZE, 0, 10, 0, 0, 0, 0); CHECK(!vd_msg_valid(&d, 0));
    d = vd_msg(VD_SIZE, 10, 10, 0, 0, 0, 0); CHECK(!vd_msg_valid(&d, 1));      /* direction matters */
    d = vd_msg(VD_CURSOR, 64, 64, 63, 0, 0, 0); CHECK(vd_msg_valid(&d, 0));
    d = vd_msg(VD_CURSOR, 64, 64, 64, 0, 0, 0); CHECK(!vd_msg_valid(&d, 0));
    d = vd_msg(VD_CURSOR, 257, 64, 0, 0, 0, 0); CHECK(!vd_msg_valid(&d, 0));
    d = vd_msg(VD_FRAME, 0, 0, 640, 480, 1, 0); CHECK(vd_msg_valid(&d, 0) && vd_rect_inside(&d, 640, 480) && !vd_rect_inside(&d, 639, 480));
    d = vd_msg(VD_FRAME, 2147483000, 0, 2000, 1, 1, 0); CHECK(!vd_rect_inside(&d, 640, 480));
    /* sound (6.9): the rings go to the helper; what the guest does comes from it */
    d = vd_msg(VD_AUDIO_OUT, 1, 0, 0, 0, 0, 0); CHECK(vd_msg_valid(&d, 1) && !vd_msg_valid(&d, 0));
    d = vd_msg(VD_AUDIO_OUT, 2, 0, 0, 0, 0, 0); CHECK(!vd_msg_valid(&d, 1));
    d = vd_msg(VD_MIC, 0, 0, 0, 0, 0, 0); CHECK(vd_msg_valid(&d, 1) && !vd_msg_valid(&d, 0));
    d = vd_msg(VD_MIC, -1, 0, 0, 0, 0, 0); CHECK(!vd_msg_valid(&d, 1));
    d = vd_msg(VD_GUEST_AUDIO, VDG_IN, 1, 0, 0, 0, 0); CHECK(vd_msg_valid(&d, 0) && !vd_msg_valid(&d, 1));
    d = vd_msg(VD_GUEST_AUDIO, VDG_OUT, 2, 0, 0, 0, 0); CHECK(!vd_msg_valid(&d, 0));
    d = vd_msg(VD_GUEST_AUDIO, 2, 1, 0, 0, 0, 0); CHECK(!vd_msg_valid(&d, 0));
    q = vm_msg(VM_SOUND, 1, 1, 80, 0, 0, NULL); CHECK(vm_msg_valid(&q, 1) && !vm_msg_valid(&q, 0));
    q.b = 101; CHECK(!vm_msg_valid(&q, 1));
    q.b = -1; CHECK(!vm_msg_valid(&q, 1));
    q.b = 0; q.a = 2; CHECK(!vm_msg_valid(&q, 1));
    q.a = 0; q.c = 2; CHECK(!vm_msg_valid(&q, 1));
    q.c = 0; q.d = 1; CHECK(vm_msg_valid(&q, 1));
    q.d = 2; CHECK(!vm_msg_valid(&q, 1));
    /* the speakers' ring keeps at most VD_AUDIO_AHEAD_MS ahead of the reader (15 360 bytes at 80 ms), and a broken reader
     * cursor counts as full */
    CHECK(vd_audio_fits(0, 0, 1u << 18, 15360) && !vd_audio_fits(0, 0, 1u << 18, 15364));
    CHECK(vd_audio_fits(1000, 1000 + 15000, 1u << 18, 360) && !vd_audio_fits(1000, 1000 + 15000, 1u << 18, 364));
    CHECK(!vd_audio_fits(5000, 4000, 1u << 18, 4));                   /* reader ahead of the writer */
    CHECK(!vd_audio_fits(0, (1u << 18) + 4, 1u << 18, 4));           /* further back than the ring */
    CHECK(!vd_audio_fits(0, 0, 4096, 8192) && vd_audio_fits(0, 0, 4096, 4096));    /* a small ring caps itself */
    /* the guest's microphone at the real rate: 48 frames a millisecond, at most what fits and 100 ms, no burst after a
     * pause */
    {
        uint64_t given = 0;
        CHECK(vd_mic_owed(1000, 1000, &given, 4800) == 0);
        CHECK(vd_mic_owed(1000, 1010, &given, 4800) == 480);
        given += 480;
        CHECK(vd_mic_owed(1000, 1010, &given, 4800) == 0);
        CHECK(vd_mic_owed(1000, 1030, &given, 100) == 100);
        given += 100;
        CHECK(vd_mic_owed(1000, 1030, &given, 4800) == 860);
        given = 960;
        CHECK(vd_mic_owed(1000, 6000, &given, 100000) == 4800 && given == 240000 - 4800);   /* a 5 s pause: 100 ms only */
        given = 0;
        CHECK(vd_mic_owed(1000, 900, &given, 4800) == 0);              /* a clock before the start */
    }

    /* real sockets: lengths, versions, descriptors */
    int sv[2];
    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
    VmMsg in, out = vm_msg(VM_STATUS, 5, 0, 0, 0, 0, "hi");
    int fd;
    CHECK(vm_recv(sv[1], &in, &fd) == 0);                              /* nothing yet */
    CHECK(!vm_send(sv[0], &out, -1) && vm_recv(sv[1], &in, &fd) == 1 && fd == -1 && in.seq == 5 && !strcmp(in.text, "hi"));
    memset(out.text, 'z', sizeof out.text);                            /* no NUL from the peer: the reader ends it */
    CHECK(!vm_send(sv[0], &out, -1) && vm_recv(sv[1], &in, &fd) == 1 && in.text[VM_TEXT_MAX - 1] == 0);
    char shortpkt[10] = { 0 };
    CHECK(send(sv[0], shortpkt, sizeof shortpkt, 0) == sizeof shortpkt && vm_recv(sv[1], &in, &fd) == -1);
    VmMsg bad = vm_msg(VM_STATUS, 0, 0, 0, 0, 0, NULL);
    bad.version = 9;
    CHECK(!vm_send(sv[0], &bad, -1) && vm_recv(sv[1], &in, &fd) == -1);
    int mfd = memfd_create("t", MFD_CLOEXEC);
    CHECK(!vm_send(sv[0], &out, mfd) && vm_recv(sv[1], &in, &fd) == 1 && fd >= 0);
    if (fd >= 0) close(fd);
    CHECK(!vm_send(sv[0], &out, mfd) && vm_recv(sv[1], &in, NULL) == 1);     /* a descriptor nobody wants is closed */
    /* two descriptors on one message: refused, both closed */
    {
        struct iovec io = { &out, sizeof out };
        union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int) * 2)]; } cm;
        memset(&cm, 0, sizeof cm);
        struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cm.b, .msg_controllen = sizeof cm.b };
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int) * 2);
        int two[2] = { mfd, mfd };
        memcpy(CMSG_DATA(c), two, sizeof two);
        CHECK(sendmsg(sv[0], &mh, 0) == sizeof out && vm_recv(sv[1], &in, &fd) == -1 && fd == -1);
    }
    close(sv[0]);
    CHECK(vm_recv(sv[1], &in, &fd) == -1);                             /* hung up */
    close(sv[1]);
    close(mfd);
    /* picture and cursor memfds */
    int pic = vm_picture_memfd(640 * 480 * 4);
    CHECK(pic >= 0 && ftruncate(pic, 10) != 0);                        /* lord-server's picture can't shrink */
    if (pic >= 0) close(pic);
    int cur = memfd_create("c", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(cur >= 0 && !ftruncate(cur, 64 * 64 * 4));
    CHECK(vm_cursor_check(cur, 64, 64) == -1);                         /* not sealed yet */
    CHECK(!fcntl(cur, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE));
    CHECK(vm_cursor_check(cur, 64, 64) == 0 && vm_cursor_check(cur, 32, 32) == -1);
    close(cur);
}

static void test_qmp(void) {
    int sv[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    Qmp q;
    CHECK(!qmp_open(&q, sv[0]));
    JDoc d;
    const char a[] = "{\"QMP\":{\"version\":{}}}\n{\"return\":{}}\n{\"event\":\"STOP\",\"data\":{}}\n{\"err";
    CHECK(write(sv[1], a, sizeof a - 1) == (ssize_t)sizeof a - 1);
    CHECK(qmp_fill(&q) == 0);
    CHECK(qmp_next(&q, &d) == 1 && qmp_kind(d.root) == QMP_MSG_GREETING); json_free(&d);
    CHECK(qmp_next(&q, &d) == 1 && qmp_kind(d.root) == QMP_MSG_RETURN); json_free(&d);
    CHECK(qmp_next(&q, &d) == 1 && qmp_kind(d.root) == QMP_MSG_EVENT && !strcmp(json_str(d.root, "event"), "STOP")); json_free(&d);
    CHECK(qmp_next(&q, &d) == 0);                                       /* half a line waits */
    const char b[] = "or\":{\"class\":\"GenericError\",\"desc\":\"no \\\"such\\\" thing\"}}\nnot json\n[1,2]\n\n";
    CHECK(write(sv[1], b, sizeof b - 1) == (ssize_t)sizeof b - 1);
    CHECK(qmp_fill(&q) == 0);
    CHECK(qmp_next(&q, &d) == 1 && qmp_kind(d.root) == QMP_MSG_ERROR && !strcmp(qmp_error_desc(d.root), "no \"such\" thing")); json_free(&d);
    CHECK(qmp_next(&q, &d) == 0 && q.refused == 2);                    /* garbage and a non-object, skipped */
    /* a line longer than the buffer is dropped, the next one survives */
    char *big = malloc(QMP_LINE_MAX + 100);
    memset(big, 'x', QMP_LINE_MAX + 99);
    big[QMP_LINE_MAX + 99] = '\n';
    size_t off = 0;
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    while (off < QMP_LINE_MAX + 100) {
        ssize_t w = write(sv[1], big + off, QMP_LINE_MAX + 100 - off);
        if (w > 0) off += (size_t)w;
        qmp_fill(&q);
        JDoc x;
        while (qmp_next(&q, &x) > 0) { json_free(&x); CHECK(0); }     /* nothing may come out of the long line */
    }
    fcntl(sv[1], F_SETFL, 0);
    CHECK(write(sv[1], "{\"return\":{}}\n", 14) == 14);
    qmp_fill(&q);
    CHECK(qmp_next(&q, &d) == 1 && qmp_kind(d.root) == QMP_MSG_RETURN); json_free(&d);
    CHECK(q.refused >= 3);
    free(big);
    /* commands are one line with escapes */
    CHECK(!qmp_command(&q, "human-monitor-command", "{\"command-line\":\"info\"}", -1));
    char got[256] = { 0 };
    CHECK(read(sv[1], got, sizeof got - 1) > 0 && !strcmp(got, "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"info\"}}\n"));
    CHECK(qmp_raw(&q, "{\"execute\":\"x\"}\n{\"execute\":\"quit\"}") == -1);   /* one command only */
    close(sv[1]);
    CHECK(qmp_fill(&q) == -1);
    qmp_close(&q);
}

static void test_keys(void) {
    CHECK(vm_evdev_to_qnum(1) == 0x01 && vm_evdev_to_qnum(28) == 0x1c && vm_evdev_to_qnum(83) == 0x53);
    CHECK(vm_evdev_to_qnum(97) == 0x9d && vm_evdev_to_qnum(103) == 0xc8 && vm_evdev_to_qnum(125) == 0xdb && vm_evdev_to_qnum(111) == 0xd3);
    CHECK(vm_evdev_to_qnum(0) == 0 && vm_evdev_to_qnum(84) == 0 && vm_evdev_to_qnum(700) == 0 && vm_evdev_to_qnum(-5) == 0);
    /* no two keys on one number */
    unsigned char seen[256] = { 0 };
    int dup = 0;
    for (int k = 0; k < 768; k++) { uint32_t q = vm_evdev_to_qnum(k); if (q) { if (q > 255 || seen[q]) dup++; else seen[q] = 1; } }
    CHECK(dup == 0);
}

/* import: the format comes from our check of the first bytes, never QEMU's probing */
static void put_be(uint8_t *p, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }
static void test_images(void) {
    uint8_t h[VM_IMG_HEAD];
    char err[128];
    memset(h, 0, sizeof h);
    memcpy(h, "QFI\xfb", 4); put_be(h + 4, 3, 4); put_be(h + 100, 112, 4);
    CHECK(vm_image_kind(h, sizeof h, "/x/debian.qcow2", err, sizeof err) == VM_IMG_QCOW2);
    CHECK(vm_image_kind(h, sizeof h, "/x/debian.img", err, sizeof err) == VM_IMG_QCOW2);      /* the magic wins */
    CHECK(vm_qcow2_plain(h, sizeof h, err, sizeof err) == 0);
    put_be(h + 8, 0x200, 8); put_be(h + 16, 11, 4);                                              /* backing file */
    CHECK(vm_image_kind(h, sizeof h, "/x/a.qcow2", err, sizeof err) == -1 && strstr(err, "backing"));
    CHECK(vm_image_kind(h, sizeof h, "/x/a.img", err, sizeof err) == -1);
    put_be(h + 8, 0, 8); CHECK(vm_qcow2_plain(h, sizeof h, err, sizeof err) == -1);              /* size alone */
    put_be(h + 16, 0, 4);
    put_be(h + 72, 4, 8); CHECK(vm_image_kind(h, sizeof h, "/x/a.qcow2", err, sizeof err) == -1 && strstr(err, "another file"));
    put_be(h + 72, 2, 8); CHECK(vm_qcow2_plain(h, sizeof h, err, sizeof err) == -1);              /* corrupt */
    put_be(h + 72, 1 << 6, 8); CHECK(vm_qcow2_plain(h, sizeof h, err, sizeof err) == -1);         /* unknown feature */
    put_be(h + 72, 1 | 8 | 16, 8); CHECK(vm_qcow2_plain(h, sizeof h, err, sizeof err) == 0);      /* dirty, zstd, extended L2 */
    put_be(h + 72, 0, 8);
    put_be(h + 100, 50, 4); CHECK(vm_qcow2_plain(h, sizeof h, err, sizeof err) == -1);
    put_be(h + 100, 104, 4);
    CHECK(vm_qcow2_plain(h, 80, err, sizeof err) == -1);                                          /* v3 cut short */
    put_be(h + 4, 2, 4); CHECK(vm_qcow2_plain(h, 80, err, sizeof err) == 0);                      /* v2 has no more */
    put_be(h + 4, 4, 4); CHECK(vm_qcow2_plain(h, sizeof h, err, sizeof err) == -1);
    memset(h, 0, sizeof h);
    CHECK(vm_image_kind(h, sizeof h, "/x/disk.img", err, sizeof err) == VM_IMG_RAW);
    CHECK(vm_image_kind(h, sizeof h, "/x/DISK.RAW", err, sizeof err) == VM_IMG_RAW);
    CHECK(vm_image_kind(h, sizeof h, "/x/disk.qcow2", err, sizeof err) == -1);                    /* raw pretending */
    CHECK(vm_image_kind(h, sizeof h, "/x/disk.vmdk", err, sizeof err) == -1);
    CHECK(vm_image_kind(h, 100, "/x/disk.img", err, sizeof err) == -1);
    CHECK(vm_image_kind(h, 0, NULL, err, sizeof err) == -1);
}

/* the log cleaner never reads past the end: a bare ESC, "ESC [" and a CSI cut off at the end of the log (each in a
 * buffer of exactly its size, so ASan sees one byte too far) */
static void test_log_clean(void) {
    static const char *const in[] = { "boot ok\x1b", "boot ok\x1b[", "boot ok\x1b[0;3", "a\x1b[32mOK\x1b[0m b\r\n", "x\x1b" "cy" };
    static const char *const out[] = { "boot ok", "boot ok", "boot ok", "aOK b\n", "xy" };
    for (size_t i = 0; i < sizeof in / sizeof in[0]; i++) {
        size_t n = strlen(in[i]) + 1;
        char *b = malloc(n);
        memcpy(b, in[i], n);
        vm_log_clean(b);
        CHECK(!strcmp(b, out[i]));
        free(b);
    }
}

/* Beta: operation.json (3.6) and snapshots.json (6.2) */
static void test_ops(void) {
    char tpl[] = "/tmp/vm-test-ops-XXXXXX";
    char *dir = mkdtemp(tpl);
    CHECK(dir != NULL);
    if (!dir) return;
    VmOp o;
    CHECK(vm_op_read(dir, &o) == 0);
    CHECK(!vm_op_begin(dir, "snapshot-take", "s20261105-120000", "/a", NULL));
    CHECK(vm_op_read(dir, &o) == 1 && !strcmp(o.op, "snapshot-take") && !strcmp(o.tag, "s20261105-120000") && !strcmp(o.a, "/a") && o.started[0]);
    vm_op_end(dir);
    CHECK(vm_op_read(dir, &o) == 0);
    char p[256];
    snprintf(p, sizeof p, "%s/operation.json", dir);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) { (void)!write(fd, "{not json", 9); close(fd); }
    CHECK(vm_op_read(dir, &o) == -1 && access(p, F_OK) != 0);       /* unreadable: dropped */
    char tag[24];
    vm_snap_tag(tag);
    CHECK(vm_snap_tag_valid(tag));
    CHECK(!vm_snap_tag_valid("x2026") && !vm_snap_tag_valid("s;rm") && !vm_snap_tag_valid("S1") && !vm_snap_tag_valid("s") && !vm_snap_tag_valid(NULL));
    VmSnap a[3], b[VM_SNAPS_MAX];
    memset(a, 0, sizeof a);
    snprintf(a[0].tag, sizeof a[0].tag, "s1"); snprintf(a[0].name, sizeof a[0].name, "Fresh install"); snprintf(a[0].created, sizeof a[0].created, "2026-11-05T12:00:00Z");
    snprintf(a[1].tag, sizeof a[1].tag, "s2"); snprintf(a[1].name, sizeof a[1].name, "Antes \"del\" cambio \xc3\xa9"); a[1].memory = 1; a[1].vmstate = 301989888;
    snprintf(a[2].tag, sizeof a[2].tag, "bad tag"); snprintf(a[2].name, sizeof a[2].name, "x");
    CHECK(!vm_snaps_save(dir, a, 3));
    int n = vm_snaps_load(dir, b, VM_SNAPS_MAX);
    CHECK(n == 2);                                                   /* a bad tag is never loaded */
    CHECK(n == 2 && !strcmp(b[1].name, a[1].name) && b[1].memory && b[1].vmstate == 301989888 && !b[0].memory);
    CHECK(vm_snaps_find(b, n, "s2") == 1 && vm_snaps_find(b, n, "s9") == -1);
    snprintf(p, sizeof p, "rm -rf %s", dir);
    (void)!system(p);
}

/* Beta: org.lordos.agent frames, UTF-8, base64 (the guest is untrusted) */
static void test_agent(void) {
    uint8_t b[64];
    ag_frame_head(b, AGK_CLIP, 0, 3);
    memcpy(b + AG_HEAD, "abc", 3);
    AgFrame f;
    CHECK(ag_frame_next(b, AG_HEAD + 2, &f) == 0);                       /* not complete */
    CHECK(ag_frame_next(b, AG_HEAD + 3, &f) == AG_HEAD + 3 && f.kind == AGK_CLIP && f.len == 3 && !memcmp(f.data, "abc", 3));
    ag_frame_head(b, AGK_CLIP, 0, AG_FRAME_MAX + 1);
    CHECK(ag_frame_next(b, AG_HEAD, &f) == -1);                          /* too long: dropped */
    ag_frame_head(b, 999, 7, 0);
    CHECK(ag_frame_next(b, AG_HEAD, &f) == AG_HEAD && f.kind == 999 && f.chan == 7 && !f.len);   /* unknown kinds pass, to be skipped */
    CHECK(ag_utf8_ok((const uint8_t *)"h\xc3\xa9llo \xe2\x82\xac \xf0\x9f\x98\x80", 15));
    CHECK(!ag_utf8_ok((const uint8_t *)"a\0b", 3) && !ag_utf8_ok((const uint8_t *)"\xc3", 1) && !ag_utf8_ok((const uint8_t *)"\xc0\x80", 2));
    CHECK(!ag_utf8_ok((const uint8_t *)"\xed\xa0\x80", 3) && !ag_utf8_ok((const uint8_t *)"\xf5\x80\x80\x80", 4) && !ag_utf8_ok((const uint8_t *)"\xe2\x82", 2));
    char e[32];
    uint8_t d[32];
    CHECK(ag_b64_encode((const uint8_t *)"hello!!", 7, e, sizeof e) == 12 && !strcmp(e, "aGVsbG8hIQ=="));
    CHECK(ag_b64_decode(e, strlen(e), d, sizeof d) == 7 && !memcmp(d, "hello!!", 7));
    CHECK(ag_b64_decode("a$==", 4, d, sizeof d) == -1 && ag_b64_decode("aGVsbG8=", 8, d, 2) == -1 && ag_b64_encode(d, 7, e, 8) == -1);
}

/* a dropped file's name as the guest gets it (VM_FILE), and what a guest-exec printed */
static const char *dn(const char *in) { static char o[AG_NAME_MAX + 1]; return ag_drop_name(in, o, sizeof o) ? "(refused)" : o; }
static void test_drop_names(void) {
    CHECK(!strcmp(dn("notes.txt"), "notes.txt"));
    CHECK(!strcmp(dn("/home/a/b/Informe final.pdf"), "Informe final.pdf"));
    CHECK(!strcmp(dn("Prueba \xc3\xb1" "and\xc3\xba: copia?.bin"), "Prueba \xc3\xb1" "and\xc3\xba_ copia_.bin"));
    CHECK(!strcmp(dn("a\\b|c<d>e\"f*g"), "a_b_c_d_e_f_g"));
    CHECK(!strcmp(dn("tab\there\x7f"), "tab_here_"));
    CHECK(!strcmp(dn("  lead and trail. . "), "lead and trail"));
    CHECK(!strcmp(dn(".bashrc"), ".bashrc"));
    CHECK(!strcmp(dn("CON"), "_CON") && !strcmp(dn("nul.txt"), "_nul.txt") && !strcmp(dn("Com7.log"), "_Com7.log") && !strcmp(dn("LPT1 .x"), "_LPT1 .x"));
    CHECK(!strcmp(dn("console.txt"), "console.txt") && !strcmp(dn("com10"), "com10") && !strcmp(dn("lpt0"), "lpt0"));
    CHECK(!strcmp(dn("."), "(refused)") && !strcmp(dn(".."), "(refused)") && !strcmp(dn("   "), "(refused)") && !strcmp(dn(""), "(refused)") && !strcmp(dn("dir/"), "(refused)"));
    CHECK(!strcmp(dn("bad \xc3"), "(refused)") && !strcmp(dn("\xc0\x80"), "(refused)"));
    /* too long: cut on a character, the extension kept */
    char lng[400];
    size_t k = 0;
    for (int i = 0; i < 120; i++) { lng[k++] = '\xc3'; lng[k++] = '\xa9'; }       /* 240 bytes of é */
    memcpy(lng + k, ".tar.gz", 8);
    const char *o = dn(lng);
    CHECK(strlen(o) <= AG_NAME_MAX && strlen(o) > 100 && ag_utf8_ok((const uint8_t *)o, strlen(o)) && !strcmp(o + strlen(o) - 3, ".gz"));
    char small[8];
    CHECK(!ag_drop_name("abcdefghij.k", small, sizeof small) && strlen(small) == 7 && !strcmp(small + 5, ".k"));
    CHECK(ag_drop_name("abc", small, 1) == -1);
    char t[64];
    char e[64];
    ag_b64_encode((const uint8_t *)"  /home/a/Downloads/x (2).txt\n", 31, e, sizeof e);
    ag_exec_text(e, t, sizeof t); CHECK(!strcmp(t, "/home/a/Downloads/x (2).txt"));
    ag_b64_encode((const uint8_t *)"a\x01" "b\xff" "c", 5, e, sizeof e);
    ag_exec_text(e, t, sizeof t); CHECK(!strcmp(t, "a b?c"));
    ag_b64_encode((const uint8_t *)"\xc3\xa9\xc3\xa9\xc3\xa9", 6, e, sizeof e);
    ag_exec_text(e, t, 4); CHECK(!strcmp(t, "\xc3\xa9"));                           /* cut on a character */
    ag_exec_text(NULL, t, sizeof t); CHECK(!t[0]);
    /* the private folders a drop is written into: only the ones the fixed scripts make */
    CHECK(ag_drop_staging_ok("/var/tmp/lordos-drop.Ab3dE9xZ", 0) && !ag_drop_staging_ok("/var/tmp/lordos-drop.Ab3d", 0));
    CHECK(!ag_drop_staging_ok("/home/a/lordos-drop.Ab3dE9xZ", 0) && !ag_drop_staging_ok("/var/tmp/lordos-drop.Ab3dE9x/", 0) && !ag_drop_staging_ok(NULL, 0));
    CHECK(ag_drop_staging_ok("C:\\Windows\\Temp\\lordos-drop-0123456789abcdef0123456789abcdef", 1));
    CHECK(ag_drop_staging_ok("D:\\WINDOWS\\TEMP\\lordos-drop-0123456789abcdef0123456789abcdef", 1));
    CHECK(!ag_drop_staging_ok("C:\\Users\\a\\Temp\\..\\lordos-drop-0123456789abcdef0123456789abcdef", 1));
    CHECK(!ag_drop_staging_ok("C:\\Windows\\Temp\\lordos-drop-0123456789ABCDEF0123456789abcdef", 1));
    CHECK(!ag_drop_staging_ok("C:\\Windows\\Temp\\lordos-drop-0123456789abcdef0123456789abcde", 1));
    CHECK(!ag_drop_staging_ok("C:/Windows/Temp/lordos-drop-0123456789abcdef0123456789abcdef", 1));
    CHECK(!ag_drop_staging_ok("\\\\srv\\Temp\\lordos-drop-0123456789abcdef0123456789abcdef", 1));
    CHECK(!ag_drop_staging_ok("/var/tmp/lordos-drop.Ab3dE9xZ", 1) && !ag_drop_staging_ok("C:\\Windows\\Temp\\lordos-drop-0123456789abcdef0123456789abcdef", 0));
    ag_exec_text("!!", t, sizeof t); CHECK(!t[0]);
}

/* the guest's clipboard beyond text (6.8): AGK_CLIPX, PNG headers, HTML made harmless, Clone hostnames (6.3) */
static int san(const char *in, char *out, size_t cap) { return (int)ag_html_sanitize(in, strlen(in), out, cap); }
static void test_clipx(void) {
    uint8_t buf[256];
    AgClip c = { (const uint8_t *)"hi", (const uint8_t *)"<b>hi</b>", NULL, 2, 9, 0 }, r;
    size_t n = ag_clipx_size(&c);
    CHECK(n == 5 + 2 + 5 + 9);
    ag_clipx_write(&c, buf);
    CHECK(!ag_clipx_parse(buf, n, &r) && r.text_n == 2 && !memcmp(r.text, "hi", 2) && r.html_n == 9 && !r.png);
    CHECK(ag_clipx_parse(buf, n - 1, &r) == -1);                                     /* cut short */
    CHECK(ag_clipx_parse(buf, 0, &r) == -1);                                         /* nothing */
    uint8_t twice[14] = { AGF_TEXT, 2, 0, 0, 0, 'a', 'b', AGF_TEXT, 2, 0, 0, 0, 'c', 'd' };
    CHECK(ag_clipx_parse(twice, sizeof twice, &r) == -1);                            /* a form twice */
    uint8_t odd[7] = { 9, 2, 0, 0, 0, 'a', 'b' };
    CHECK(ag_clipx_parse(odd, sizeof odd, &r) == -1);                                /* unknown form */
    uint8_t huge[5] = { AGF_HTML, 0xff, 0xff, 0xff, 0x7f };
    CHECK(ag_clipx_parse(huge, sizeof huge, &r) == -1);
    /* PNG header */
    uint8_t png[40] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 1, 0, 0, 0, 0, 200, 8, 6, 0, 0, 0 };
    uint32_t w = 0, h = 0;
    CHECK(!ag_png_check(png, sizeof png, &w, &h) && w == 256 && h == 200);
    png[16] = 0x7f;                                                                  /* 2 billion wide */
    CHECK(ag_png_check(png, sizeof png, &w, &h) == -1);
    png[16] = 0; png[24] = 7;                                                        /* bit depth 7 */
    CHECK(ag_png_check(png, sizeof png, &w, &h) == -1);
    png[24] = 8; png[1] = 'X';
    CHECK(ag_png_check(png, sizeof png, &w, &h) == -1);
    /* HTML */
    char o[512];
    CHECK(san("<p>Hello <b>world</b></p>", o, sizeof o) > 0 && !strcmp(o, "<p>Hello <b>world</b></p>"));
    CHECK(san("a<script>alert(1)</script>b", o, sizeof o) > 0 && !strcmp(o, "ab"));
    CHECK(san("a<SCRIPT type=x>alert(1)</ScRiPt >b", o, sizeof o) > 0 && !strcmp(o, "ab"));
    CHECK(san("<style>body{}</style><i>x</i>", o, sizeof o) > 0 && !strcmp(o, "<i>x</i>"));
    CHECK(san("<img src=x onerror=alert(1)>t", o, sizeof o) > 0 && !strcmp(o, "t"));
    CHECK(san("<a href=\"javascript:alert(1)\" onclick=\"x()\">l</a>", o, sizeof o) > 0 && !strcmp(o, "<a>l</a>"));
    CHECK(san("<a href='https://example.test/?a=1&b=\"2'>l</a>", o, sizeof o) > 0 && !strcmp(o, "<a href=\"https://example.test/?a=1&b=&quot;2\">l</a>"));
    CHECK(san("<a href=\" javascript:x\">l</a>", o, sizeof o) > 0 && !strcmp(o, "<a>l</a>"));
    CHECK(san("<span style=\"color: red; background: url(http://x); font-weight:bold\">s</span>", o, sizeof o) > 0 &&
          !strcmp(o, "<span style=\"color: red; font-weight: bold\">s</span>"));
    CHECK(san("<span style=\"color: expression(alert(1))\">s</span>", o, sizeof o) > 0 && !strcmp(o, "<span>s</span>"));
    CHECK(san("<!-- c --><html><body><!--StartFragment--><u>u</u><!--EndFragment--></body></html>", o, sizeof o) > 0 && !strcmp(o, "<u>u</u>"));
    CHECK(san("1 < 2 > 0 <3", o, sizeof o) > 0 && !strcmp(o, "1 &lt; 2 &gt; 0 &lt;3"));
    CHECK(san("<iframe src=x>inner<b>no</b></iframe>after", o, sizeof o) > 0 && !strcmp(o, "after"));
    CHECK(san("<td colspan=2 rowspan=\"x\">c</td>", o, sizeof o) > 0 && !strcmp(o, "<td colspan=\"2\">c</td>"));
    CHECK(san("<svg><script>x</script></svg>ok", o, sizeof o) > 0 && !strcmp(o, "ok"));
    CHECK(san("<b title=\"a\"<script>\">x</b>", o, sizeof o) > 0 && !strstr(o, "<script"));
    CHECK(san("<div>unclosed <i", o, sizeof o) > 0 && !strcmp(o, "<div>unclosed &lt;i"));
    CHECK(san("<p>abcdefgh</p>", o, 8) == -1);                                       /* doesn't fit */
    /* hostnames */
    char hn[64];
    CHECK(vm_hostname_valid("debian-13-clone") && !vm_hostname_valid("-a") && !vm_hostname_valid("a-") && !vm_hostname_valid("A") &&
          !vm_hostname_valid("a.b") && !vm_hostname_valid("") && !vm_hostname_valid("a;rm -rf"));
    vm_hostname_from_name("Debian 13 (test) ", hn, sizeof hn); CHECK(!strcmp(hn, "debian-13-test"));
    vm_hostname_from_name("Ñandú", hn, sizeof hn); CHECK(!strcmp(hn, "and"));
    vm_hostname_from_name("--", hn, sizeof hn); CHECK(!strcmp(hn, "machine"));
    /* Windows computer names (NetBIOS: at most 15, not only digits) */
    char wn[VM_WINNAME_MAX + 1];
    CHECK(vm_winname_valid("WIN-BASE-CLONE") && vm_winname_valid("a1") && !vm_winname_valid("123") && !vm_winname_valid("-a") &&
          !vm_winname_valid("a.b") && !vm_winname_valid("") && !vm_winname_valid("abcdefghijklmnop") && !vm_winname_valid("a b"));
    CHECK(!vm_winname_from_host("win-base-clone", "908312a6-6bbe", wn) && !strcmp(wn, "win-base-clone"));
    CHECK(!vm_winname_from_host("windows-11-pro-clone", "908312a6-6bbe", wn) && !strcmp(wn, "windows-11-9083"));
    CHECK(!vm_winname_from_host("my-long----name-clone", "c0ffee00", wn) && !strcmp(wn, "my-long-c0ff"));
    CHECK(vm_winname_from_host("1234", "x", wn) && !wn[0]);                         /* only digits */
    CHECK(vm_winname_from_host("Bad;name", "x", wn) && !wn[0]);                     /* not a hostname */
    CHECK(vm_winname_from_host("12345678901234567", "", wn) && !wn[0]);               /* long digits, no id: none */
}


/* schema 6 (6.5): the bridged and host-only networks, bridge, ports; the admin actions' arguments and the cards */
static void mkfile(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { (void)!write(fd, text, strlen(text)); close(fd); }
}
static void test_network(void) {
    VmMachine m;
    char err[256], t[4096];
    const char *at = strstr(GOOD, "\"schema\":1");
    struct { const char *net, *add; int want; } s6[] = {
        { "shared", "\"ports\":[{\"proto\":\"tcp\",\"host\":2222,\"guest\":22,\"open\":\"this-computer\"}]", VM_OK },
        { "shared", "\"ports\":[{\"proto\":\"udp\",\"host\":5353,\"guest\":53}]", VM_OK },                   /* open: this computer */
        { "shared", "\"ports\":[{\"proto\":\"tcp\",\"host\":8080,\"guest\":80,\"open\":\"lan\"}]", VM_OK },
        { "shared", "\"ports\":[{\"proto\":\"tcp\",\"host\":80,\"guest\":80}]", VM_E_RANGE },                 /* < 1024 */
        { "shared", "\"ports\":[{\"proto\":\"tcp\",\"host\":1023,\"guest\":80}]", VM_E_RANGE },
        { "shared", "\"ports\":[{\"proto\":\"tcp\",\"host\":65536,\"guest\":80}]", VM_E_RANGE },
        { "shared", "\"ports\":[{\"proto\":\"tcp\",\"host\":2222,\"guest\":0}]", VM_E_RANGE },
        { "shared", "\"ports\":[{\"proto\":\"tcp\",\"host\":2222,\"guest\":65536}]", VM_E_RANGE },
        { "shared", "\"ports\":[{\"proto\":\"sctp\",\"host\":2222,\"guest\":22}]", VM_E_RANGE },
        { "shared", "\"ports\":[{\"proto\":\"tcp\",\"host\":2222,\"guest\":22,\"open\":\"world\"}]", VM_E_RANGE },
        { "shared", "\"ports\":[{\"proto\":\"tcp\",\"host\":2222,\"guest\":22,\"via\":\"x\"}]", VM_E_FIELD },
        { "shared", "\"ports\":[{\"proto\":\"tcp\",\"host\":\"2222\",\"guest\":22}]", VM_E_FIELD },
        { "shared", "\"ports\":[{\"proto\":\"tcp\",\"host\":2222.5,\"guest\":22}]", VM_E_FIELD },
        { "shared", "\"ports\":{}", VM_E_FIELD },
        { "shared", "\"ports\":[{\"proto\":\"tcp\",\"host\":2222,\"guest\":22},{\"proto\":\"tcp\",\"host\":2222,\"guest\":23}]", VM_E_RANGE },
        { "shared", "\"ports\":[{\"proto\":\"tcp\",\"host\":2222,\"guest\":22},{\"proto\":\"udp\",\"host\":2222,\"guest\":22}]", VM_OK },
        { "hostonly", "\"ports\":[{\"proto\":\"tcp\",\"host\":2222,\"guest\":22}]", VM_OK },
        { "hostonly", "\"ports\":[{\"proto\":\"tcp\",\"host\":2222,\"guest\":22,\"open\":\"lan\"}]", VM_E_RANGE },
        { "bridged", "\"bridge\":\"enp3s0\"", VM_OK },
        { "bridged", "\"bridge\":\"\"", VM_E_RANGE },
        { "bridged", "\"bridge\":\"en p3\"", VM_E_RANGE },
        { "bridged", "\"bridge\":\"..\"", VM_E_RANGE },
        { "bridged", "\"bridge\":\"a-very-long-name0\"", VM_E_RANGE },
        { "bridged", "\"bridge\":\"eth0/x\"", VM_E_RANGE },
        { "isolated", "\"bridge\":\"eth0\"", VM_OK },                                                        /* kept */
        { "nat", "\"bridge\":\"eth0\"", VM_E_RANGE },
    };
    for (size_t i = 0; i < sizeof s6 / sizeof s6[0]; i++) {
        char g[4096];
        snprintf(g, sizeof g, "%s", GOOD);
        char *nv = strstr(g, "\"network\":\"shared\"");
        char rest[4096];
        snprintf(rest, sizeof rest, "%s", nv + strlen("\"network\":\"shared\""));
        snprintf(nv, sizeof g - (size_t)(nv - g), "\"network\":\"%s\"%s", s6[i].net, rest);
        const char *a6 = strstr(g, "\"schema\":1");
        snprintf(t, sizeof t, "%.*s\"schema\":6%.*s,%s}", (int)(a6 - g), g, (int)(strlen(a6) - 10 - 1), a6 + 10, s6[i].add);
        int r = vm_machine_parse(t, strlen(t), &m, err, sizeof err);
        if (r != s6[i].want) fprintf(stderr, "  schema 6 %s %s: got %d (%s)\n", s6[i].net, s6[i].add, r, err);
        CHECK(r == s6[i].want);
        if (r == VM_OK) {
            /* written and read back the same */
            char *j = vm_machine_json(&m);
            VmMachine m2;
            CHECK(j && vm_machine_parse(j, strlen(j), &m2, err, sizeof err) == VM_OK && !memcmp(&m, &m2, sizeof m));
            free(j);
        }
        /* never below schema 6 */
        snprintf(t, sizeof t, "%.*s\"schema\":5%.*s,%s}", (int)(a6 - g), g, (int)(strlen(a6) - 10 - 1), a6 + 10, s6[i].add);
        CHECK(vm_machine_parse(t, strlen(t), &m, err, sizeof err) != VM_OK);
    }
    /* the old modes still load at every schema, and a schema 5 file isn't refused for its network */
    snprintf(t, sizeof t, "%.*s\"schema\":5%s", (int)(at - GOOD), GOOD, at + 10);
    CHECK(vm_machine_parse(t, strlen(t), &m, err, sizeof err) == VM_OK && m.network == VM_NET_SHARED && !m.n_ports && !m.bridge[0]);
    /* at most VM_PORTS rules */
    vm_machine_parse(GOOD, strlen(GOOD), &m, err, sizeof err);
    m.schema = VM_SCHEMA;
    for (int i = 0; i < VM_PORTS; i++) m.ports[i] = (VmPort){ i & 1, 2000 + i, 22, VM_OPEN_LOCAL };
    m.n_ports = VM_PORTS;
    CHECK(vm_machine_check(&m, err, sizeof err) == VM_OK);
    m.n_ports = VM_PORTS + 1;
    CHECK(vm_machine_check(&m, err, sizeof err) == VM_E_RANGE);
    /* passt's options: 127.0.0.1 unless "lan", and never "lan" for a host-only machine; "none" without rules */
    m.n_ports = 3;
    m.ports[0] = (VmPort){ VM_PROTO_TCP, 2222, 22, VM_OPEN_LOCAL };
    m.ports[1] = (VmPort){ VM_PROTO_TCP, 8080, 80, VM_OPEN_LAN };
    m.ports[2] = (VmPort){ VM_PROTO_UDP, 5353, 53, VM_OPEN_LOCAL };
    char pv[VM_PORTS][48];
    m.network = VM_NET_SHARED;
    CHECK(vm_passt_ports(&m, VM_PROTO_TCP, pv, VM_PORTS) == 2 && !strcmp(pv[0], "127.0.0.1/2222:22") && !strcmp(pv[1], "0.0.0.0/8080:80"));
    CHECK(vm_passt_ports(&m, VM_PROTO_UDP, pv, VM_PORTS) == 1 && !strcmp(pv[0], "127.0.0.1/5353:53"));
    m.network = VM_NET_HOSTONLY;
    CHECK(vm_passt_ports(&m, VM_PROTO_TCP, pv, VM_PORTS) == 2 && !strcmp(pv[1], "127.0.0.1/8080:80"));
    m.n_ports = 0;
    CHECK(vm_passt_ports(&m, VM_PROTO_TCP, pv, VM_PORTS) == 1 && !strcmp(pv[0], "none"));
    /* the command line's rules */
    VmPort p;
    char sp[48];
    CHECK(!vm_port_parse("tcp:2222:22", &p) && p.proto == VM_PROTO_TCP && p.host == 2222 && p.guest == 22 && p.open == VM_OPEN_LOCAL);
    CHECK(!vm_port_parse("udp:5353:53:lan", &p) && p.proto == VM_PROTO_UDP && p.open == VM_OPEN_LAN);
    vm_port_spec(&p, sp, sizeof sp);
    CHECK(!strcmp(sp, "udp:5353:53:lan"));
    CHECK(!vm_port_parse("tcp:2222:22:this-computer", &p));
    const char *badp[] = { "tcp:80:80", "tcp:2222", "tcp:2222:22:lan:x", "tcp:+2222:22", "tcp: 2222:22", "tcp:2222:-1", "icmp:2222:22",
                           "tcp:2222:22:world", "tcp:70000:22", "tcp:2222:0", "", "tcp:2222:22x", "TCP:2222:22", NULL };
    for (int i = 0; badp[i]; i++) { if (!vm_port_parse(badp[i], &p)) fprintf(stderr, "  port %s accepted\n", badp[i]); CHECK(vm_port_parse(badp[i], &p)); }
    /* the macvtap's name */
    char nm[16];
    CHECK(!vm_bridge_ifname("0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b", nm) && !strcmp(nm, "lvm0f8e2c1a3b4d") && strlen(nm) == 15);
    CHECK(vm_bridge_ifname("0F8E2C1A-3b4d-4e5f-8a6b-7c8d9e0f1a2b", nm));
    CHECK(vm_bridge_ifname("../x", nm));
    /* the admin actions' arguments, checked on the root side */
    char id[37], card[16], mac[18];
    const char *ok1 = "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b enp3s0 52:54:00:12:ab:cd";
    CHECK(!vmnet_bridge_arg(ok1, strlen(ok1), id, card, mac) && !strcmp(card, "enp3s0") && !strcmp(mac, "52:54:00:12:ab:cd") && !strcmp(id, "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b"));
    const char *badb[] = {
        "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b enp3s0  52:54:00:12:ab:cd",        /* two spaces */
        "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b enp3s0 52:54:00:12:AB:CD",         /* uppercase MAC */
        "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b enp3s0 53:54:00:12:ab:cd",         /* multicast */
        "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b enp3s0 00:00:00:00:00:00",
        "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b ../x 52:54:00:12:ab:cd",
        "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b a;b 52:54:00:12:ab:cd",
        "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b averyveryverylong0 52:54:00:12:ab:cd",
        "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2 enp3s0 52:54:00:12:ab:cd",
        "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b enp3s0 52:54:00:12:ab:cd ",
        "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b enp3s0",
        "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b enp3s0 52:54:00:12:ab:cd\nx",
        "", NULL };
    for (int i = 0; badb[i]; i++) CHECK(vmnet_bridge_arg(badb[i], strlen(badb[i]), id, card, mac));
    {
        /* a NUL inside the argument: its length says more than the text */
        char nul[] = "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b enp3s0 52:54:00:12:ab:cd";
        nul[40] = 0;
        CHECK(vmnet_bridge_arg(nul, sizeof nul - 1, id, card, mac));
    }
    CHECK(!vmnet_unbridge_arg("0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b", 36, id));
    CHECK(vmnet_unbridge_arg("0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b ", 37, id) && vmnet_unbridge_arg("lvm0f8e2c1a3b4d", 15, id) && vmnet_unbridge_arg("", 0, id));
    /* the alias */
    char al[96], aid[37];
    uid_t u = 0;
    vmnet_alias(al, sizeof al, 1000, "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b");
    CHECK(!strcmp(al, "lordos-vm 1000 0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b") && vmnet_alias_parse(al, &u, aid) && u == 1000 && !strcmp(aid, "0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b"));
    CHECK(vmnet_alias_parse("lordos-vm 1000 0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b\n", &u, NULL));
    CHECK(!vmnet_alias_parse("lordos-vm -1 0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b", &u, NULL) && !vmnet_alias_parse("lordos-vm 1000 x", &u, NULL) &&
          !vmnet_alias_parse("my card", &u, NULL) && !vmnet_alias_parse("lordos-vm 1000 0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2bx", &u, NULL) && !vmnet_alias_parse("", &u, NULL));
    /* what a card is, over an invented sysfs */
    char sys[] = "/tmp/vm-net-test.XXXXXX", q[512];
    if (mkdtemp(sys)) {
        const char *dirs[] = { "eth0", "eth0/device", "wlan0", "wlan0/device", "wlan0/wireless", "br0", "br0/bridge", "eth1", "eth1/device",
                               "eth1/brport", "ib0", "ib0/device", "lvm0f8e2c1a3b4d", NULL };
        for (int i = 0; dirs[i]; i++) { snprintf(q, sizeof q, "%s/%s", sys, dirs[i]); mkdir(q, 0755); }
        snprintf(q, sizeof q, "%s/eth0/type", sys); mkfile(q, "1\n");
        snprintf(q, sizeof q, "%s/eth0/operstate", sys); mkfile(q, "up\n");
        snprintf(q, sizeof q, "%s/wlan0/type", sys); mkfile(q, "1\n");
        snprintf(q, sizeof q, "%s/wlan0/operstate", sys); mkfile(q, "down\n");
        snprintf(q, sizeof q, "%s/eth1/type", sys); mkfile(q, "1\n");
        snprintf(q, sizeof q, "%s/ib0/type", sys); mkfile(q, "32\n");
        snprintf(q, sizeof q, "%s/br0/type", sys); mkfile(q, "1\n");
        CHECK(vmnet_card(sys, "eth0") == VMNET_WIRED);
        CHECK(vmnet_card(sys, "wlan0") == VMNET_WIFI);
        CHECK(vmnet_card(sys, "br0") == VMNET_VIRTUAL);
        CHECK(vmnet_card(sys, "lvm0f8e2c1a3b4d") == VMNET_VIRTUAL);
        CHECK(vmnet_card(sys, "eth1") == VMNET_ENSLAVED);
        CHECK(vmnet_card(sys, "ib0") == VMNET_NOT_ETHER);
        CHECK(vmnet_card(sys, "eth9") == VMNET_MISSING && vmnet_card(sys, "..") == VMNET_MISSING && vmnet_card(sys, "eth0/device") == VMNET_MISSING);
        CHECK(!strcmp(vmnet_card_error(VMNET_WIFI), "vm.bridgeWifi") && !vmnet_card_error(VMNET_WIRED)[0]);
        char *js = vmnet_cards_json(sys);
        CHECK(js && strstr(js, "{\"name\":\"eth0\",\"kind\":\"wired\",\"up\":true}") && strstr(js, "{\"name\":\"wlan0\",\"kind\":\"wifi\",\"up\":false}") &&
              !strstr(js, "br0") && !strstr(js, "eth1") && !strstr(js, "lvm"));
        free(js);
        char cmd[600];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", sys);
        (void)!system(cmd);
    }
}


/* apps mode (vmagent.h LAG_*): every guest → host payload is parsed strictly; the writers round-trip */
static size_t put32(uint8_t *b, size_t o, uint32_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); b[o + 2] = (uint8_t)(v >> 16); b[o + 3] = (uint8_t)(v >> 24); return o + 4; }
static size_t put16(uint8_t *b, size_t o, uint32_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); return o + 2; }
static size_t putstr(uint8_t *b, size_t o, const char *s) { size_t l = strlen(s); o = put16(b, o, (uint32_t)l); memcpy(b + o, s, l); return o + l; }
static size_t win1(uint8_t *b, size_t o, uint32_t id, int x, int y, int w, int h, uint32_t flags, uint32_t owner, const char *title, const char *app) {
    o = put32(b, o, id); o = put32(b, o, (uint32_t)x); o = put32(b, o, (uint32_t)y); o = put32(b, o, (uint32_t)w); o = put32(b, o, (uint32_t)h);
    o = put32(b, o, flags);
    if (flags & LAGW_OWNED) o = put32(b, o, owner);
    o = put32(b, o, 1234);
    o = putstr(b, o, title);
    return putstr(b, o, app);
}
static void test_apps_mode(void) {
    static uint8_t b[LAG_WINDOWS_BYTES], c[LAG_WINDOWS_BYTES];
    static LagWinList l, l2;
    CHECK(ag_hello_caps((const uint8_t *)"2 clipboard clipx windows", 25) == (AG_HELLO_CLIPBOARD | AG_HELLO_CLIPX | AG_HELLO_WINDOWS));
    CHECK(ag_hello_caps((const uint8_t *)"2 clipboard windowsx", 20) == AG_HELLO_CLIPBOARD && ag_hello_caps((const uint8_t *)"", 0) == 0);
    CHECK(ag_hello_caps((const uint8_t *)"4 noclip windows", 16) == (AG_HELLO_NOCLIP | AG_HELLO_WINDOWS) && ag_hello_caps((const uint8_t *)"4 noclipx", 9) == 0);
    size_t o = put32(b, 0, 7);
    o = put16(b, o, 2);
    o = win1(b, o, 10, 100, 50, 640, 480, LAGW_FOCUSED, 0, "Notes \xe2\x80\x94 untitled", "notepad.exe");
    o = win1(b, o, 11, 120, 80, 300, 200, LAGW_OWNED | LAGW_TOPMOST, 10, "Save as", "notepad.exe");
    CHECK(!lag_winlist_read(b, o, &l) && l.serial == 7 && l.n == 2 && l.w[0].id == 10 && l.w[0].w == 640 && l.w[1].owner == 10 && !strcmp(l.w[1].title, "Save as") && l.w[0].pid == 1234);
    size_t n = lag_windows_write(&l, c, sizeof c);
    CHECK(n == o && !memcmp(b, c, n) && !lag_winlist_read(c, n, &l2) && l2.n == 2);
    CHECK(lag_winlist_read(b, o - 1, &l) == -1);                         /* cut short */
    b[o] = 0;
    CHECK(lag_winlist_read(b, o + 1, &l) == -1);                         /* a byte left over */
    /* each field out of bounds */
    size_t k;
    k = put16(b, put32(b, 0, 1), LAG_WIN_MAX + 1); CHECK(lag_winlist_read(b, k, &l) == -1);                                      /* too many */
    k = win1(b, put16(b, put32(b, 0, 1), 1), 0, 0, 0, 10, 10, 0, 0, "a", "b"); CHECK(lag_winlist_read(b, k, &l) == -1);          /* id 0 */
    k = win1(b, put16(b, put32(b, 0, 1), 1), 5, 0, 0, 0, 10, 0, 0, "a", "b"); CHECK(lag_winlist_read(b, k, &l) == -1);           /* w 0 */
    k = win1(b, put16(b, put32(b, 0, 1), 1), 5, 0, 0, LAG_DIM_MAX + 1, 10, 0, 0, "a", "b"); CHECK(lag_winlist_read(b, k, &l) == -1);        /* w too big */
    k = win1(b, put16(b, put32(b, 0, 1), 1), 5, LAG_DIM_MAX + 1, 0, 10, 10, 0, 0, "a", "b"); CHECK(lag_winlist_read(b, k, &l) == -1);      /* x too far */
    k = win1(b, put16(b, put32(b, 0, 1), 1), 5, -32000, -32000, 160, 28, LAGW_MINIMIZED, 0, "a", "b");
    CHECK(lag_winlist_read(b, k, &l) == -1);                                                                                     /* minimized: still a place */
    k = win1(b, put16(b, put32(b, 0, 1), 1), 5, 0, 0, 10, 10, 32, 0, "a", "b"); CHECK(lag_winlist_read(b, k, &l) == -1);          /* unknown flag */
    k = win1(b, put16(b, put32(b, 0, 1), 1), 5, 0, 0, 10, 10, LAGW_OWNED, 5, "a", "b"); CHECK(lag_winlist_read(b, k, &l) == -1);  /* owns itself */
    k = win1(b, put16(b, put32(b, 0, 1), 1), 5, 0, 0, 10, 10, LAGW_OWNED, 0, "a", "b"); CHECK(lag_winlist_read(b, k, &l) == -1);  /* owner 0 */
    k = win1(b, put16(b, put32(b, 0, 1), 1), 5, 0, 0, 10, 10, 0, 0, "a\x01", "b"); CHECK(lag_winlist_read(b, k, &l) == -1);        /* control character */
    k = win1(b, put16(b, put32(b, 0, 1), 1), 5, 0, 0, 10, 10, 0, 0, "\xc2\x85", "b"); CHECK(lag_winlist_read(b, k, &l) == -1);    /* C1 control */
    k = win1(b, put16(b, put32(b, 0, 1), 1), 5, 0, 0, 10, 10, 0, 0, "\xc3", "b"); CHECK(lag_winlist_read(b, k, &l) == -1);        /* not UTF-8 */
    k = win1(b, put16(b, put32(b, 0, 1), 2), 5, 0, 0, 10, 10, 0, 0, "a", "b"); k = win1(b, k, 5, 0, 0, 10, 10, 0, 0, "a", "b");
    CHECK(lag_winlist_read(b, k, &l) == -1);                                                                                     /* same id twice */
    {   /* a title longer than LAG_TITLE_MAX */
        static char big[LAG_TITLE_MAX + 2];
        memset(big, 'x', LAG_TITLE_MAX + 1);
        k = win1(b, put16(b, put32(b, 0, 1), 1), 5, 0, 0, 10, 10, 0, 0, big, "b"); CHECK(lag_winlist_read(b, k, &l) == -1);
        big[LAG_TITLE_MAX] = 0;
        k = win1(b, put16(b, put32(b, 0, 1), 1), 5, 0, 0, 10, 10, 0, 0, big, "b"); CHECK(!lag_winlist_read(b, k, &l));
    }
    /* a length that points past the end */
    k = win1(b, put16(b, put32(b, 0, 1), 1), 5, 0, 0, 10, 10, 0, 0, "abc", "b");
    put16(b, 6 + 28, 60000);
    CHECK(lag_winlist_read(b, k, &l) == -1);
    /* icons */
    uint8_t rgba[16 * 16 * 4];
    memset(rgba, 0x80, sizeof rgba);
    n = lag_icon_write(10, 16, 16, rgba, c, sizeof c);
    uint32_t id; uint16_t iw, ih; const uint8_t *px;
    CHECK(n == 8 + sizeof rgba && !lag_winicon_parse(c, n, &id, &iw, &ih, &px) && id == 10 && iw == 16 && ih == 16 && px == c + 8);
    CHECK(lag_winicon_parse(c, n - 1, &id, &iw, &ih, &px) == -1 && lag_winicon_parse(c, 7, &id, &iw, &ih, &px) == -1);
    put16(c, 4, 65); CHECK(lag_winicon_parse(c, n, &id, &iw, &ih, &px) == -1);
    put16(c, 4, 0); CHECK(lag_winicon_parse(c, n, &id, &iw, &ih, &px) == -1);
    CHECK(!lag_icon_write(1, 65, 1, rgba, c, sizeof c) && !lag_icon_write(0, 1, 1, rgba, c, sizeof c));
    /* actions (the guest checks them too) */
    LagAct a = { 10, LAGA_MOVE, -5, 20, 800, 600 }, a2;
    n = lag_act_write(&a, c, sizeof c);
    CHECK(n == 21 && !lag_winact_parse(c, n, &a2) && a2.id == 10 && a2.x == -5 && a2.h == 600);
    CHECK(lag_winact_parse(c, 20, &a2) == -1 && lag_winact_parse(c, 5, &a2) == -1);
    a.action = LAGA_CLOSE;
    n = lag_act_write(&a, c, sizeof c);
    CHECK(n == 5 && !lag_winact_parse(c, 5, &a2) && a2.action == LAGA_CLOSE && lag_winact_parse(c, 6, &a2) == -1);
    c[4] = 6; CHECK(lag_winact_parse(c, 5, &a2) == -1);
    c[4] = 0; CHECK(lag_winact_parse(c, 5, &a2) == -1);
    a.action = LAGA_MOVE; a.w = 0; CHECK(!lag_act_write(&a, c, sizeof c));
    /* launch and the list of apps */
    char tok[LAG_TOKEN_MAX + 1];
    n = lag_launch_write("org.lordos.notes", c, sizeof c);
    CHECK(n == 18 && !lag_launch_token(c, n, tok, sizeof tok) && !strcmp(tok, "org.lordos.notes"));
    CHECK(!lag_launch_write("", c, sizeof c) && lag_launch_token(c, n + 1, tok, sizeof tok) == -1 && lag_launch_token(c, n, tok, 8) == -1);
    static LagAppList al, al2;
    al.n = 2;
    snprintf(al.a[0].name, sizeof al.a[0].name, "Notes"); snprintf(al.a[0].token, sizeof al.a[0].token, "notes");
    snprintf(al.a[1].name, sizeof al.a[1].name, "Calculadora"); snprintf(al.a[1].token, sizeof al.a[1].token, "Microsoft.WindowsCalculator_8wekyb3d8bbwe!App");
    static uint8_t big[LAG_APPLIST_BYTES];
    n = lag_applist_write(&al, big, sizeof big);
    CHECK(n && !lag_applist_read(big, n, &al2) && al2.n == 2 && lag_applist_has(&al2, "notes") && !lag_applist_has(&al2, "note") && !lag_applist_has(&al2, ""));
    CHECK(lag_applist_read(big, n - 1, &al2) == -1);
    put16(big, 0, LAG_APPS_MAX + 1); CHECK(lag_applist_read(big, n, &al2) == -1);
    k = putstr(b, put16(b, 0, 1), "Name"); k = putstr(b, k, ""); CHECK(lag_applist_read(b, k, &al2) == -1);       /* empty token */
    k = putstr(b, put16(b, 0, 1), ""); k = putstr(b, k, "t"); CHECK(lag_applist_read(b, k, &al2) == -1);          /* empty name */
    CHECK(lag_rect_ok(0, 0, 1, 1) && !lag_rect_ok(0, 0, 1, 0) && !lag_rect_ok(LAG_DIM_MAX + 1, 0, 1, 1) && lag_rect_ok(-LAG_DIM_MAX, 0, LAG_DIM_MAX, 1));
    /* the control messages around it */
    VmMsg m = vm_msg(VM_APPS, 1, 1, 0, 0, 0, NULL);
    CHECK(vm_msg_valid(&m, 1));
    m.a = 2; CHECK(!vm_msg_valid(&m, 1));
    m = vm_msg(VM_WINACT, 1, 10, LAGA_MOVE, 0, 0, "1 2 3 4"); CHECK(vm_msg_valid(&m, 1));
    m.b = 6; CHECK(!vm_msg_valid(&m, 1));
    m = vm_msg(VM_LAUNCH, 1, 0, 0, 0, 0, ""); CHECK(!vm_msg_valid(&m, 1));
    m = vm_msg(VM_LAUNCH, 1, 3, 2, 0, 0, ""); CHECK(vm_msg_valid(&m, 1));
    m.a = 256; CHECK(!vm_msg_valid(&m, 1));
    m = vm_msg(VM_WINDATA, 0, 100, LAG_WINDOWS, 0, 0, NULL); CHECK(vm_msg_valid(&m, 0));
    m.b = LAG_WINACT; CHECK(!vm_msg_valid(&m, 0));
    m.b = LAG_APPLIST; m.a = (int)VM_WINDATA_MAX + 1; CHECK(!vm_msg_valid(&m, 0));
    m = vm_msg(VM_AGENT_STATE, 0, VAS_CONNECTED, VAC_CLIPBOARD | (int)LAG_CAP_WINDOWS, 0, 0, NULL); CHECK(vm_msg_valid(&m, 0));
    /* random bytes never crash a parser and never pass with a byte out of place */
    uint32_t seed = 12345;
    for (int it = 0; it < 20000; it++) {
        size_t len = (seed = seed * 1103515245 + 12345) % 300;
        for (size_t i = 0; i < len; i++) b[i] = (uint8_t)((seed = seed * 1103515245 + 12345) >> 16);
        lag_winlist_read(b, len, &l); lag_winicon_parse(b, len, &id, &iw, &ih, &px); lag_winact_parse(b, len, &a2);
        lag_launch_token(b, len, tok, sizeof tok); lag_applist_read(b, len, &al2);
    }
}

/* the resolution a machine starts with (Configure → Display) */
static void test_resolution(void) {
    int w = 0, h = 0;
    CHECK(!vm_resolution_parse("1920x1080", &w, &h) && w == 1920 && h == 1080);
    CHECK(!vm_resolution_parse("640x480", &w, &h));
    CHECK(vm_resolution_parse("1921x1080", &w, &h));          /* odd */
    CHECK(vm_resolution_parse("639x480", &w, &h));
    CHECK(vm_resolution_parse("7682x4320", &w, &h));
    CHECK(vm_resolution_parse("1920X1080", &w, &h));
    CHECK(vm_resolution_parse("1920x1080 ", &w, &h));
    CHECK(vm_resolution_parse("x1080", &w, &h));
    CHECK(vm_resolution_parse("123456x480", &w, &h));
    CHECK(vm_resolution_parse("", &w, &h));
}

/* the Windows guest tools (vmagent.h): AGK_DISPLAY and apps mode, every frame parsed to its last byte */
typedef struct { uint8_t b[4096]; size_t n; } TB;
static void tb8(TB *t, uint8_t v) { t->b[t->n++] = v; }
static void tb16(TB *t, uint16_t v) { tb8(t, (uint8_t)v); tb8(t, (uint8_t)(v >> 8)); }
static void tb32(TB *t, uint32_t v) { tb16(t, (uint16_t)v); tb16(t, (uint16_t)(v >> 16)); }
static void tbs(TB *t, const char *s) { tb16(t, (uint16_t)strlen(s)); memcpy(t->b + t->n, s, strlen(s)); t->n += strlen(s); }
static void tbwin(TB *t, uint32_t id, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t flags, uint32_t owner, const char *title, const char *app) {
    tb32(t, id); tb32(t, (uint32_t)x); tb32(t, (uint32_t)y); tb32(t, (uint32_t)w); tb32(t, (uint32_t)h); tb32(t, flags);
    if (flags & LAGW_OWNED) tb32(t, owner);
    tb32(t, 4242); tbs(t, title); tbs(t, app);
}
/* AGK_DISPLAYS: the guest's displays, for lord-vm's log (lordos-guest-win w7) */
static void test_displays(void) {
    AgDisplay in[AG_DISPLAYS_MAX], out[AG_DISPLAYS_MAX];
    memset(in, 0, sizeof in);
    in[0] = (AgDisplay){ 0, 0, 1022, 724, AGDS_PRIMARY | AGDS_HW, "\\\\.\\DISPLAY1" };
    in[1] = (AgDisplay){ 1022, 0, 1920, 1080, AGDS_HW, "\\\\.\\DISPLAY2" };
    uint8_t b[AG_DISPLAYS_BYTES + 4];
    size_t n = ag_displays_write(in, 2, 1, b, sizeof b);
    int shown = 7;
    CHECK(n == 2 + 2 * 18 + 24 && ag_displays_parse(b, n, out, &shown) == 2 && shown == 1);
    CHECK(out[1].x == 1022 && out[1].w == 1920 && out[1].h == 1080 && out[1].flags == AGDS_HW && !strcmp(out[1].name, "\\\\.\\DISPLAY2"));
    char t[512];
    ag_displays_text(out, 2, shown, t, sizeof t);
    CHECK(!strcmp(t, "\\\\.\\DISPLAY1 1022x724 at 0,0 primary; \\\\.\\DISPLAY2 1920x1080 at 1022,0 (shown)"));
    ag_displays_text(out, 2, shown, t, 10);
    CHECK(strlen(t) == 9);
    /* exact length: one byte short or one too many */
    CHECK(ag_displays_parse(b, n - 1, out, &shown) == -1);
    b[n] = 0; CHECK(ag_displays_parse(b, n + 1, out, &shown) == -1);
    CHECK(ag_displays_parse(NULL, n, out, &shown) == -1 && ag_displays_parse(b, 1, out, &shown) == -1);
    /* no shown display */
    CHECK(ag_displays_write(in, 2, -1, b, sizeof b) == n && b[1] == AG_DISPLAYS_NONE && ag_displays_parse(b, n, out, &shown) == 2 && shown == -1);
    /* count 0 or above AG_DISPLAYS_MAX, shown out of range */
    uint8_t c[AG_DISPLAYS_BYTES + 64];
    memcpy(c, b, n); c[0] = 0; CHECK(ag_displays_parse(c, n, out, &shown) == -1);
    memcpy(c, b, n); c[0] = AG_DISPLAYS_MAX + 1; CHECK(ag_displays_parse(c, n, out, &shown) == -1);
    memcpy(c, b, n); c[1] = 2; CHECK(ag_displays_parse(c, n, out, &shown) == -1);
    memcpy(c, b, n); c[1] = 1; CHECK(ag_displays_parse(c, n, out, &shown) == 2);
    /* the name: empty, too long, not printable ASCII; its length past the end */
    memcpy(c, b, n); c[2 + 17] = 0; CHECK(ag_displays_parse(c, n, out, &shown) == -1);
    memcpy(c, b, n); c[2 + 17] = AG_DISPLAY_NAME_MAX + 1; CHECK(ag_displays_parse(c, n, out, &shown) == -1);
    memcpy(c, b, n); c[2 + 18] = 0x1b; CHECK(ag_displays_parse(c, n, out, &shown) == -1);
    memcpy(c, b, n); c[2 + 18] = 0xc3; CHECK(ag_displays_parse(c, n, out, &shown) == -1);
    memcpy(c, b, n); c[2 + 18 + 12 + 17] = 200; CHECK(ag_displays_parse(c, n, out, &shown) == -1);
    /* flags: an unknown bit, two primaries */
    memcpy(c, b, n); c[2 + 16] |= 4; CHECK(ag_displays_parse(c, n, out, &shown) == -1);
    memcpy(c, b, n); c[2 + 18 + 12 + 16] |= AGDS_PRIMARY; CHECK(ag_displays_parse(c, n, out, &shown) == -1);
    /* sizes and places */
    AgDisplay bad[1] = { { 0, 0, 0, 600, 0, "X" } };
    CHECK(ag_displays_write(bad, 1, 0, b, sizeof b) == 0);
    bad[0].w = AG_DISPLAY_MAX + 1; CHECK(ag_displays_write(bad, 1, 0, b, sizeof b) == 0);
    bad[0].w = 800; bad[0].x = LAG_DIM_MAX + 1; CHECK(ag_displays_write(bad, 1, 0, b, sizeof b) == 0);
    bad[0].x = -LAG_DIM_MAX; CHECK(ag_displays_write(bad, 1, 0, b, sizeof b) == 2 + 18 + 1);
    memcpy(c, b, 21); memset(c + 2 + 8, 0, 4); CHECK(ag_displays_parse(c, 21, out, &shown) == -1);               /* w 0 */
    memcpy(c, b, 21); c[2 + 11] = 0x80; CHECK(ag_displays_parse(c, 21, out, &shown) == -1);           /* w huge */
    memcpy(c, b, 21); c[2 + 3] = 0x7f; CHECK(ag_displays_parse(c, 21, out, &shown) == -1);            /* x huge */
    memcpy(c, b, 21); c[2 + 4] = 0xff; c[2 + 7] = 0x80; CHECK(ag_displays_parse(c, 21, out, &shown) == -1);   /* y negative huge */
    bad[0].name[0] = 0; CHECK(ag_displays_write(bad, 1, 0, b, sizeof b) == 0);
    /* the writer: a shown index out of range, too many, a buffer too small */
    CHECK(ag_displays_write(in, 2, 2, b, sizeof b) == 0 && ag_displays_write(in, 0, -1, b, sizeof b) == 0);
    CHECK(ag_displays_write(in, AG_DISPLAYS_MAX + 1, -1, b, sizeof b) == 0 && ag_displays_write(in, 2, 0, b, 30) == 0);
    /* eight displays fit AG_DISPLAYS_BYTES with the longest names */
    for (int i = 0; i < AG_DISPLAYS_MAX; i++) { in[i] = (AgDisplay){ i * 100, 0, 640, 480, i ? 0 : AGDS_PRIMARY, "" }; memset(in[i].name, 'A' + i, AG_DISPLAY_NAME_MAX); }
    n = ag_displays_write(in, AG_DISPLAYS_MAX, 3, b, sizeof b);
    CHECK(n == AG_DISPLAYS_BYTES && ag_displays_parse(b, n, out, &shown) == AG_DISPLAYS_MAX && shown == 3 && strlen(out[7].name) == AG_DISPLAY_NAME_MAX);
    /* random bytes never crash it */
    uint32_t r = 12345;
    for (int k = 0; k < 20000; k++) {
        size_t m = 2 + (r % 120);
        for (size_t i = 0; i < m; i++) { r = r * 1103515245u + 12345u; c[i] = (uint8_t)(r >> 16); }
        c[0] = (uint8_t)(1 + c[0] % 3);
        int s2 = 0;
        (void)ag_displays_parse(c, m, out, &s2);
    }
}

/* which display the picture shows (lordos-guest-win w8): the maintainer's GVT-g guest as w7 reported it */
static void test_shown_pick(void) {
    AgDisplay d[3];
    memset(d, 0, sizeof d);
    /* \\.\DISPLAY2: the Intel vGPU (PCI), follows the host's size; \\.\DISPLAY1: primary, not PCI, 1022x724 */
    d[0] = (AgDisplay){ 1022, 0, 1024, 768, AGDS_HW, "\\\\.\\DISPLAY2" };
    d[1] = (AgDisplay){ 0, 0, 1022, 724, AGDS_PRIMARY, "\\\\.\\DISPLAY1" };
    int lock = 9;
    /* at hello the host asked 1022x724: the non-PCI display has that size, but never wins over a PCI one */
    CHECK(ag_shown_pick(d, 2, 1022, 724, -1, &lock) == 0 && lock == -1);
    /* nothing asked yet */
    CHECK(ag_shown_pick(d, 2, 0, 0, -1, &lock) == 0 && lock == -1);
    /* w7's lock on DISPLAY1 isn't kept while a PCI display is there */
    CHECK(ag_shown_pick(d, 2, 1022, 724, 1, &lock) == 0 && lock == -1);
    /* apps mode: the host asks 1920x1080 and the vGPU's output follows */
    d[0].w = 1920; d[0].h = 1080;
    CHECK(ag_shown_pick(d, 2, 1920, 1080, -1, &lock) == 0 && lock == 0);
    CHECK(ag_shown_pick(d, 2, 1600, 900, 0, &lock) == 0 && lock == 0);          /* kept while it stays */
    /* two PCI displays: the one with the size asked for, then the primary */
    d[1].flags = AGDS_PRIMARY | AGDS_HW;
    CHECK(ag_shown_pick(d, 2, 1920, 1080, -1, &lock) == 0 && lock == 0);
    CHECK(ag_shown_pick(d, 2, 1022, 724, -1, &lock) == 1 && lock == 1);
    CHECK(ag_shown_pick(d, 2, 800, 600, -1, &lock) == 1 && lock == -1);         /* no size match: the primary */
    CHECK(ag_shown_pick(d, 2, 1925, 1085, -1, &lock) == 0 && lock == 0);        /* under by less than 8 */
    /* no PCI display at all: the size, then the primary */
    d[0].flags = 0; d[1].flags = AGDS_PRIMARY;
    CHECK(ag_shown_pick(d, 2, 1920, 1080, -1, &lock) == 0 && lock == 0);
    CHECK(ag_shown_pick(d, 2, 0, 0, -1, &lock) == 1 && lock == -1);
    /* one display; none; a lock out of range */
    CHECK(ag_shown_pick(d, 1, 0, 0, -1, &lock) == 0);
    CHECK(ag_shown_pick(d, 0, 0, 0, -1, &lock) == -1 && ag_shown_pick(NULL, 2, 0, 0, -1, &lock) == -1);
    CHECK(ag_shown_pick(d, 2, 0, 0, 5, NULL) == 1);
    /* a third, PCI display that isn't primary beats two that aren't PCI */
    d[2] = (AgDisplay){ 3000, 0, 1280, 800, AGDS_HW, "\\\\.\\DISPLAY3" };
    CHECK(ag_shown_pick(d, 3, 1920, 1080, 1, &lock) == 2 && lock == -1);
}

/* the absolute tablet on a guest with two displays (w8 on the GVT-g guest: clicks landed across both) */
static void test_tablet_layout(void) {
    AgDisplay d[2];
    memset(d, 0, sizeof d);
    /* after w8 made the vGPU's output primary: \\.\DISPLAY2 1920x1080 at 0,0 (shown), \\.\DISPLAY1 1022x724 at 1920,0 */
    d[0] = (AgDisplay){ 0, 0, 1920, 1080, AGDS_PRIMARY | AGDS_HW, "\\\\.\\DISPLAY2" };
    d[1] = (AgDisplay){ 1920, 0, 1022, 724, 0, "\\\\.\\DISPLAY1" };
    int L[6];
    CHECK(ag_displays_layout(d, 2, 0, L) == 0 && L[0] == 0 && L[1] == 0 && L[2] == 1920 && L[3] == 1080 && L[4] == 2942 && L[5] == 1080);
    /* as w7 left it: DISPLAY1 primary at 0,0, DISPLAY2 to its right */
    d[0].x = 1022; d[1].x = 0;
    CHECK(ag_displays_layout(d, 2, 0, L) == 0 && L[0] == 1022 && L[1] == 0 && L[4] == 2942 && L[5] == 1080);
    /* a display left of and above the primary: the box starts there */
    d[1].x = -1022; d[1].y = -100; d[0].x = 0;
    CHECK(ag_displays_layout(d, 2, 0, L) == 0 && L[0] == 1022 && L[1] == 100 && L[4] == 2942 && L[5] == 1180);
    /* one display, none shown, the shown one the whole box (a smaller one inside it): no layout */
    CHECK(ag_displays_layout(d, 1, 0, L) == 1 && !L[4]);
    CHECK(ag_displays_layout(d, 2, -1, L) == 1 && !L[0] && !L[4]);
    d[1] = (AgDisplay){ 0, 0, 800, 600, 0, "X" };
    CHECK(ag_displays_layout(d, 2, 0, L) == 1);
    /* too wide for the message */
    d[1] = (AgDisplay){ 30000, 0, 8192, 600, 0, "X" };
    CHECK(ag_displays_layout(d, 2, 0, L) == 1);

    /* the mapping: every picture pixel of the shown display lands on its own guest pixel (within 1), as QEMU scales by
     * the picture's width (0x7fff over it) and the guest spreads that range over its whole desktop */
    struct { int off, shown, desk; } cases[] = { { 0, 1920, 2942 }, { 1022, 1920, 2942 }, { 0, 1080, 1080 }, { 100, 1080, 1180 } };
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        int worst = 0, pic = cases[c].shown;
        for (int p = 0; p < pic; p++) {
            int q = vd_abs_map(p, pic, cases[c].off, cases[c].shown, cases[c].desk);
            CHECK(q >= 0 && q < pic);
            long long abs = (long long)q * 0x7fff / pic;                              /* qemu_input_scale_axis */
            long long g = abs * cases[c].desk / 0x7fff;                               /* the guest's pixel */
            int err = (int)(g - (cases[c].off + p));
            if (err < 0) err = -err;
            if (err > worst) worst = err;
        }
        CHECK(worst <= 2);
    }
    /* no layout: as it was (clamped) */
    CHECK(vd_abs_map(500, 1920, 0, 0, 0) == 500 && vd_abs_map(5000, 1920, 0, 0, 0) == 1919 && vd_abs_map(-3, 1920, 0, 0, 0) == 0);
    CHECK(vd_abs_map(10, 0, 0, 1920, 2942) == 0);
    CHECK(vd_abs_map(10, 1920, 2000, 1920, 2942) == 10);                    /* a layout that doesn't fit: ignored */
    /* the right edge of the shown display stays left of the other display */
    CHECK(vd_abs_map(1919, 1920, 0, 1920, 2942) * 2942LL / 1920 < 1920);

    /* the messages */
    VmMsg m = vm_msg(VM_GUEST_LAYOUT, 0, 0, 0, 1920 << 16 | 1080, 2942 << 16 | 1080, NULL);
    CHECK(vm_msg_valid(&m, 0) && !vm_msg_valid(&m, 1));
    m = vm_msg(VM_GUEST_LAYOUT, 0, 0, 0, 0, 0, NULL); CHECK(vm_msg_valid(&m, 0));                  /* none */
    m = vm_msg(VM_GUEST_LAYOUT, 0, 1023, 0, 1920 << 16 | 1080, 2942 << 16 | 1080, NULL); CHECK(!vm_msg_valid(&m, 0));   /* past the box */
    m = vm_msg(VM_GUEST_LAYOUT, 0, -1, 0, 1920 << 16 | 1080, 2942 << 16 | 1080, NULL); CHECK(!vm_msg_valid(&m, 0));
    m = vm_msg(VM_GUEST_LAYOUT, 0, 0, 0, 0, 2942 << 16 | 1080, NULL); CHECK(!vm_msg_valid(&m, 0));                      /* no size */
    m = vm_msg(VM_GUEST_LAYOUT, 0, 0, 0, 1920 << 16 | 1080, (int)(0x8000u << 16 | 1080), NULL); CHECK(!vm_msg_valid(&m, 0));
    VdMsg v = vd_msg(VD_LAYOUT, 0, 0, 1920, 1080, 2942, 1080); CHECK(vd_msg_valid(&v, 1) && !vd_msg_valid(&v, 0));
    v = vd_msg(VD_LAYOUT, 0, 0, 0, 0, 0, 0); CHECK(vd_msg_valid(&v, 1));
    v = vd_msg(VD_LAYOUT, 1100, 0, 1920, 1080, 2942, 1080); CHECK(!vd_msg_valid(&v, 1));
    v = vd_msg(VD_LAYOUT, 0, 0, 1920, 1080, 40000, 1080); CHECK(!vd_msg_valid(&v, 1));

    /* the extra display (w9): offered only while the shown display is on a PCI adapter and another isn't */
    d[0] = (AgDisplay){ 0, 0, 1920, 1080, AGDS_PRIMARY | AGDS_HW, "\\\\.\\DISPLAY2" };
    d[1] = (AgDisplay){ 1920, 0, 1022, 724, 0, "\\\\.\\DISPLAY1" };
    CHECK(ag_displays_extra(d, 2, 0) == 1);
    CHECK(ag_displays_extra(d, 2, 1) == 0);                                 /* the shown one isn't PCI */
    CHECK(ag_displays_extra(d, 1, 0) == 0 && ag_displays_extra(d, 2, -1) == 0 && ag_displays_extra(NULL, 2, 0) == 0);
    d[1].flags = AGDS_HW; CHECK(ag_displays_extra(d, 2, 0) == 0);           /* two PCI displays: nothing extra */
    uint8_t one = 1, zero = 0, two = 2, pair[2] = { 1, 0 };
    int off = -1;
    CHECK(!ag_extra_parse(&one, 1, &off) && off == 1 && !ag_extra_parse(&zero, 1, &off) && off == 0);
    CHECK(ag_extra_parse(&two, 1, &off) == -1 && ag_extra_parse(pair, 2, &off) == -1 && ag_extra_parse(NULL, 1, &off) == -1 && ag_extra_parse(&one, 0, &off) == -1);
    CHECK(ag_hello_caps((const uint8_t *)"w9 gvt extra", 12) == (AG_HELLO_GVT | AG_HELLO_EXTRA));
    CHECK(ag_hello_caps((const uint8_t *)"w9 extra extraoff", 17) == (AG_HELLO_EXTRA | AG_HELLO_EXTRAOFF));
    CHECK(ag_hello_caps((const uint8_t *)"w9 extras extraof", 17) == 0);
    m = vm_msg(VM_AGENT, 1, VAG_EXTRA_OFF, 0, 0, 0, ""); CHECK(vm_msg_valid(&m, 1));
    m.a = VAG_EXTRA_ON; CHECK(vm_msg_valid(&m, 1));
    m.a = VAG_EXTRA_ON + 1; CHECK(!vm_msg_valid(&m, 1));
    m = vm_msg(VM_AGENT_STATE, 0, VAS_CONNECTED, VAC_EXTRA | VAC_EXTRA_OFF, 0, 0, "1\tx"); CHECK(vm_msg_valid(&m, 0));
}

/* LAG_WINREFUSED and LAGW_ELEVATED (w8: a window that runs as administrator in Windows) */
static void test_winrefused(void) {
    uint8_t b[LAG_WINREFUSED_BYTES + 1];
    uint32_t id = 0;
    uint8_t a = 0, r = 0;
    CHECK(!lag_winrefused_write(b, 0x10a2c, LAGA_CLOSE, LAGR_ELEVATED));
    CHECK(!lag_winrefused_parse(b, LAG_WINREFUSED_BYTES, &id, &a, &r) && id == 0x10a2c && a == LAGA_CLOSE && r == LAGR_ELEVATED);
    CHECK(lag_winrefused_parse(b, LAG_WINREFUSED_BYTES - 1, &id, &a, &r) == -1 && lag_winrefused_parse(b, LAG_WINREFUSED_BYTES + 1, &id, &a, &r) == -1);
    CHECK(lag_winrefused_parse(NULL, LAG_WINREFUSED_BYTES, &id, &a, &r) == -1);
    b[4] = 0; CHECK(lag_winrefused_parse(b, LAG_WINREFUSED_BYTES, NULL, NULL, NULL) == -1);
    b[4] = LAGA_MOVE + 1; CHECK(lag_winrefused_parse(b, LAG_WINREFUSED_BYTES, NULL, NULL, NULL) == -1);
    b[4] = LAGA_MOVE; b[5] = 0; CHECK(lag_winrefused_parse(b, LAG_WINREFUSED_BYTES, NULL, NULL, NULL) == -1);
    b[5] = LAGR_LAST + 1; CHECK(lag_winrefused_parse(b, LAG_WINREFUSED_BYTES, NULL, NULL, NULL) == -1);
    b[5] = LAGR_DENIED; CHECK(!lag_winrefused_parse(b, LAG_WINREFUSED_BYTES, NULL, NULL, &r) && r == LAGR_DENIED);
    memset(b, 0, 4); CHECK(lag_winrefused_parse(b, LAG_WINREFUSED_BYTES, NULL, NULL, NULL) == -1);       /* id 0 */
    CHECK(lag_winrefused_write(b, 0, LAGA_CLOSE, LAGR_ELEVATED) == -1 && lag_winrefused_write(b, 1, 9, LAGR_ELEVATED) == -1 &&
          lag_winrefused_write(b, 1, LAGA_CLOSE, 7) == -1);
    /* a window flagged elevated survives the list's parse and copy; an unknown flag still doesn't */
    LagWinList l;
    memset(&l, 0, sizeof l);
    l.serial = 3; l.n = 1;
    l.w[0] = (LagWinEntry){ .id = 77, .flags = LAGW_ELEVATED | LAGW_FOCUSED, .pid = 9, .x = 10, .y = 20, .w = 300, .h = 200 };
    strcpy(l.w[0].title, "Computer Management"); strcpy(l.w[0].app, "mmc.exe");
    static uint8_t buf[LAG_WINDOWS_BYTES];
    size_t n = lag_windows_write(&l, buf, sizeof buf);
    static LagWinList back;
    CHECK(n > 0 && !lag_winlist_read(buf, n, &back) && back.n == 1 && back.w[0].flags == (LAGW_ELEVATED | LAGW_FOCUSED));
    buf[6 + 20] = 32;               /* flags: a bit nobody knows */
    CHECK(lag_winlist_read(buf, n, &back) == -1);
}

static void test_lag(void) {
    uint8_t d[8];
    uint32_t w = 0, h = 0;
    ag_display_write(d, 1784, 830);
    CHECK(!ag_display_parse(d, 8, &w, &h) && w == 1784 && h == 830);
    CHECK(ag_display_parse(d, 7, &w, &h) == -1 && ag_display_parse(NULL, 8, &w, &h) == -1);
    ag_display_write(d, 319, 600); CHECK(ag_display_parse(d, 8, NULL, NULL) == -1);
    ag_display_write(d, 800, 199); CHECK(ag_display_parse(d, 8, NULL, NULL) == -1);
    ag_display_write(d, 8193, 600); CHECK(ag_display_parse(d, 8, NULL, NULL) == -1);
    ag_display_write(d, 0xffffffffu, 600); CHECK(ag_display_parse(d, 8, NULL, NULL) == -1);
    ag_display_write(d, 8192, 8192); CHECK(!ag_display_parse(d, 8, NULL, NULL));

    int on = -1;
    uint8_t one = 1, two = 2;
    CHECK(!lag_apps_parse(&one, 1, &on) && on == 1 && lag_apps_parse(&two, 1, &on) == -1 && lag_apps_parse(&one, 0, &on) == -1);

    /* LAG_WINDOWS */
    TB t = { .n = 0 };
    tb32(&t, 7); tb16(&t, 2);
    tbwin(&t, 0x10, 0, 0, 800, 600, LAGW_FOCUSED, 0, "Notepad \xe2\x80\x94 a.txt", "notepad.exe");
    tbwin(&t, 0x20, -8, -8, 300, 200, LAGW_OWNED | LAGW_TOPMOST, 0x10, "", "notepad.exe");
    LagWin ws[LAG_WIN_MAX];
    uint32_t serial = 0;
    CHECK(lag_windows_parse(t.b, t.n, &serial, ws, LAG_WIN_MAX) == 2 && serial == 7);
    CHECK(ws[0].id == 0x10 && ws[0].w == 800 && ws[0].flags == LAGW_FOCUSED && ws[0].pid == 4242 && ws[0].title_n == 17 && ws[0].app_n == 11);
    CHECK(ws[1].owner == 0x10 && ws[1].x == -8 && !ws[1].title_n);
    CHECK(lag_windows_parse(t.b, t.n, NULL, ws, 1) == 1);                                       /* fewer kept, still checked */
    CHECK(lag_windows_parse(t.b, t.n - 1, NULL, ws, LAG_WIN_MAX) == -1);                       /* short */
    t.b[t.n++] = 0;
    CHECK(lag_windows_parse(t.b, t.n, NULL, ws, LAG_WIN_MAX) == -1);                           /* a byte too many */
    TB e = { .n = 0 }; tb32(&e, 1); tb16(&e, 0);
    CHECK(lag_windows_parse(e.b, e.n, NULL, ws, LAG_WIN_MAX) == 0);                             /* no windows */
    struct { uint32_t id; int32_t x, y, w, h; uint32_t f, o; const char *ti, *ap; } bad[] = {
        { 0, 0, 0, 10, 10, 0, 0, "a", "b" },                        /* id 0 */
        { 1, 0, 0, 0, 10, 0, 0, "a", "b" },                         /* empty */
        { 1, 0, 0, 10, LAG_DIM_MAX + 1, 0, 0, "a", "b" },
        { 1, -LAG_DIM_MAX - 1, 0, 10, 10, 0, 0, "a", "b" },
        { 1, 0, 0, 10, 10, 32, 0, "a", "b" },                       /* unknown flag */
        { 1, 0, 0, 10, 10, LAGW_OWNED, 0, "a", "b" },               /* owner 0 */
        { 1, 0, 0, 10, 10, LAGW_OWNED, 1, "a", "b" },               /* owns itself */
        { 1, 0, 0, 10, 10, 0, 0, "a\xff", "b" },                    /* not UTF-8 */
        { 1, 0, 0, 10, 10, 0, 0, "a", "..\\x.exe" },                /* a path */
        { 1, 0, 0, 10, 10, 0, 0, "a", "C:x.exe" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        TB b = { .n = 0 };
        tb32(&b, 1); tb16(&b, 1);
        tbwin(&b, bad[i].id, bad[i].x, bad[i].y, bad[i].w, bad[i].h, bad[i].f, bad[i].o, bad[i].ti, bad[i].ap);
        CHECK(lag_windows_parse(b.b, b.n, NULL, ws, LAG_WIN_MAX) == -1);
    }
    TB dup = { .n = 0 }; tb32(&dup, 1); tb16(&dup, 2);
    tbwin(&dup, 5, 0, 0, 10, 10, 0, 0, "a", "b"); tbwin(&dup, 5, 0, 0, 10, 10, 0, 0, "a", "b");
    CHECK(lag_windows_parse(dup.b, dup.n, NULL, ws, LAG_WIN_MAX) == -1);                     /* one window twice */
    TB many = { .n = 0 }; tb32(&many, 1); tb16(&many, LAG_WIN_MAX + 1);
    CHECK(lag_windows_parse(many.b, many.n, NULL, ws, LAG_WIN_MAX) == -1);
    TB *bt = (TB *)malloc(sizeof(TB)); bt->n = 0; tb32(bt, 1); tb16(bt, 1);
    tb32(bt, 9); tb32(bt, 0); tb32(bt, 0); tb32(bt, 10); tb32(bt, 10); tb32(bt, 0); tb32(bt, 1);
    tb16(bt, LAG_TITLE_MAX + 1); memset(bt->b + bt->n, 'a', LAG_TITLE_MAX + 1); bt->n += LAG_TITLE_MAX + 1; tbs(bt, "x");
    CHECK(lag_windows_parse(bt->b, bt->n, NULL, ws, LAG_WIN_MAX) == -1);                     /* title too long */
    free(bt);
    TB lie = { .n = 0 }; tb32(&lie, 1); tb16(&lie, 1);
    tb32(&lie, 9); tb32(&lie, 0); tb32(&lie, 0); tb32(&lie, 10); tb32(&lie, 10); tb32(&lie, 0); tb32(&lie, 1); tb16(&lie, 400); tbs(&lie, "x");
    CHECK(lag_windows_parse(lie.b, lie.n, NULL, ws, LAG_WIN_MAX) == -1);                     /* a length past the end */

    /* LAG_WINICON */
    TB ic = { .n = 0 }; tb32(&ic, 0x10); tb16(&ic, 2); tb16(&ic, 2); for (int i = 0; i < 16; i++) tb8(&ic, (uint8_t)i);
    uint32_t iid = 0; uint16_t iw = 0, ih = 0; const uint8_t *px = NULL;
    CHECK(!lag_winicon_parse(ic.b, ic.n, &iid, &iw, &ih, &px) && iid == 0x10 && iw == 2 && ih == 2 && px == ic.b + 8);
    CHECK(lag_winicon_parse(ic.b, ic.n - 1, &iid, &iw, &ih, &px) == -1);
    ic.b[4] = LAG_ICON_MAX + 1; CHECK(lag_winicon_parse(ic.b, ic.n, &iid, &iw, &ih, &px) == -1);
    ic.b[4] = 0; CHECK(lag_winicon_parse(ic.b, ic.n, &iid, &iw, &ih, &px) == -1);

    /* LAG_WINACT */
    LagAct a;
    TB ac = { .n = 0 }; tb32(&ac, 0x10); tb8(&ac, LAGA_CLOSE);
    CHECK(!lag_winact_parse(ac.b, ac.n, &a) && a.id == 0x10 && a.action == LAGA_CLOSE);
    tb8(&ac, 0); CHECK(lag_winact_parse(ac.b, ac.n, &a) == -1);
    ac.n = 4; tb8(&ac, 6); CHECK(lag_winact_parse(ac.b, ac.n, &a) == -1);
    ac.n = 4; tb8(&ac, 0); CHECK(lag_winact_parse(ac.b, ac.n, &a) == -1);
    TB mv = { .n = 0 }; tb32(&mv, 0x10); tb8(&mv, LAGA_MOVE); tb32(&mv, (uint32_t)-5); tb32(&mv, 10); tb32(&mv, 640); tb32(&mv, 480);
    CHECK(!lag_winact_parse(mv.b, mv.n, &a) && a.x == -5 && a.w == 640 && a.h == 480);
    CHECK(lag_winact_parse(mv.b, 5, &a) == -1 && lag_winact_parse(mv.b, mv.n - 1, &a) == -1);
    mv.n = 17; tb32(&mv, 0); CHECK(lag_winact_parse(mv.b, mv.n, &a) == -1);                  /* height 0 */
    TB z = { .n = 0 }; tb32(&z, 0); tb8(&z, LAGA_FOCUS); CHECK(lag_winact_parse(z.b, z.n, &a) == -1);

    /* LAG_APPLIST, LAG_LAUNCH */
    TB al = { .n = 0 }; tb16(&al, 2);
    tbs(&al, "Notepad"); tbs(&al, "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Notepad.lnk");
    tbs(&al, "Paint"); tbs(&al, "C:\\Users\\a\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Paint.lnk");
    LagApp ap[4];
    CHECK(lag_applist_parse(al.b, al.n, ap, 4) == 2 && ap[1].name_n == 5 && !memcmp(ap[1].name, "Paint", 5));
    CHECK(lag_applist_parse(al.b, al.n - 1, ap, 4) == -1);
    TB ae = { .n = 0 }; tb16(&ae, 1); tbs(&ae, ""); tbs(&ae, "x"); CHECK(lag_applist_parse(ae.b, ae.n, ap, 4) == -1);    /* no name */
    TB am = { .n = 0 }; tb16(&am, LAG_APPS_MAX + 1); CHECK(lag_applist_parse(am.b, am.n, ap, 4) == -1);
    const uint8_t *tok = NULL; uint16_t tn = 0;
    TB la = { .n = 0 }; tbs(&la, "C:\\x.lnk");
    CHECK(!lag_launch_parse(la.b, la.n, &tok, &tn) && tn == 8 && !memcmp(tok, "C:\\x.lnk", 8));
    CHECK(lag_launch_parse(la.b, la.n - 1, &tok, &tn) == -1);
    TB l0 = { .n = 0 }; tbs(&l0, ""); CHECK(lag_launch_parse(l0.b, l0.n, &tok, &tn) == -1);
    TB lb = { .n = 0 }; tbs(&lb, "\xc3"); CHECK(lag_launch_parse(lb.b, lb.n, &tok, &tn) == -1);
}

/* the host's clipboard on demand (6.8): AGK_CLIPOFFER, AGK_CLIPREQ, AGK_CLIPDATA and the control messages around them */
static void test_clip_on_demand(void) {
    uint8_t o[6], r[5];
    uint32_t gen = 0;
    uint8_t forms = 0, flags = 0, form = 0, status = 9;
    ag_offer_write(o, 7, AGM_TEXT | AGM_PNG, AGO_ASK);
    CHECK(!ag_offer_parse(o, 6, &gen, &forms, &flags) && gen == 7 && forms == (AGM_TEXT | AGM_PNG) && flags == AGO_ASK);
    CHECK(ag_offer_parse(o, 5, NULL, NULL, NULL) == -1 && ag_offer_parse(NULL, 6, NULL, NULL, NULL) == -1);
    uint8_t big[7]; memcpy(big, o, 6); big[6] = 0; CHECK(ag_offer_parse(big, 7, NULL, NULL, NULL) == -1);      /* a byte too many */
    ag_offer_write(o, 0, AGM_TEXT, 0); CHECK(ag_offer_parse(o, 6, NULL, NULL, NULL) == -1);                       /* generation 0 */
    ag_offer_write(o, 1, 0, 0); CHECK(ag_offer_parse(o, 6, NULL, NULL, NULL) == -1);                              /* no form */
    ag_offer_write(o, 1, 8, 0); CHECK(ag_offer_parse(o, 6, NULL, NULL, NULL) == -1);                              /* unknown form */
    ag_offer_write(o, 1, AGM_ALL, 2); CHECK(ag_offer_parse(o, 6, NULL, NULL, NULL) == -1);                        /* unknown flag */
    ag_offer_write(o, 0xffffffffu, AGM_ALL, 0); CHECK(!ag_offer_parse(o, 6, &gen, NULL, NULL) && gen == 0xffffffffu);

    ag_clipreq_write(r, 9, AGF_HTML);
    CHECK(!ag_clipreq_parse(r, 5, &gen, &form) && gen == 9 && form == AGF_HTML);
    CHECK(ag_clipreq_parse(r, 4, NULL, NULL) == -1 && ag_clipreq_parse(r, 6, NULL, NULL) == -1);
    ag_clipreq_write(r, 9, 0); CHECK(ag_clipreq_parse(r, 5, NULL, NULL) == -1);
    ag_clipreq_write(r, 9, 4); CHECK(ag_clipreq_parse(r, 5, NULL, NULL) == -1);
    ag_clipreq_write(r, 0, AGF_TEXT); CHECK(ag_clipreq_parse(r, 5, NULL, NULL) == -1);

    uint8_t *d = calloc(1, 6 + AG_PNG_MAX + 1);
    const uint8_t *data = NULL;
    uint32_t dn = 0;
    ag_clipdata_head(d, 3, AGF_TEXT, AGD_OK); memcpy(d + 6, "h\xc3\xa9", 3);
    CHECK(!ag_clipdata_parse(d, 9, &gen, &form, &status, &data, &dn) && gen == 3 && form == AGF_TEXT && status == AGD_OK && dn == 3 && data == d + 6);
    CHECK(ag_clipdata_parse(d, 6, NULL, NULL, NULL, NULL, NULL) == -1);                                          /* OK with nothing */
    CHECK(ag_clipdata_parse(d, 8, NULL, NULL, NULL, NULL, NULL) == -1);                                          /* cut UTF-8 */
    d[7] = 0; CHECK(ag_clipdata_parse(d, 8, NULL, NULL, NULL, NULL, NULL) == -1);                                /* a NUL */
    ag_clipdata_head(d, 3, AGF_TEXT, AGD_REFUSED);
    CHECK(!ag_clipdata_parse(d, 6, NULL, NULL, &status, &data, &dn) && status == AGD_REFUSED && !data && !dn);
    CHECK(ag_clipdata_parse(d, 7, NULL, NULL, NULL, NULL, NULL) == -1);                                          /* a refusal with data */
    ag_clipdata_head(d, 3, AGF_TEXT, AGD_NONE + 1); CHECK(ag_clipdata_parse(d, 6, NULL, NULL, NULL, NULL, NULL) == -1);
    ag_clipdata_head(d, 0, AGF_TEXT, AGD_STALE); CHECK(ag_clipdata_parse(d, 6, NULL, NULL, NULL, NULL, NULL) == -1);
    ag_clipdata_head(d, 3, 4, AGD_STALE); CHECK(ag_clipdata_parse(d, 6, NULL, NULL, NULL, NULL, NULL) == -1);
    memset(d + 6, 'a', AG_TEXT_MAX + 1);
    ag_clipdata_head(d, 3, AGF_HTML, AGD_OK);
    CHECK(!ag_clipdata_parse(d, 6 + AG_TEXT_MAX, NULL, NULL, NULL, NULL, NULL) && ag_clipdata_parse(d, 7 + AG_TEXT_MAX, NULL, NULL, NULL, NULL, NULL) == -1);
    static const uint8_t png[33] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 2, 0, 0, 0, 3, 8, 6, 0, 0, 0 };
    ag_clipdata_head(d, 3, AGF_PNG, AGD_OK); memcpy(d + 6, png, sizeof png);
    CHECK(!ag_clipdata_parse(d, 6 + sizeof png, NULL, &form, NULL, NULL, &dn) && form == AGF_PNG && dn == sizeof png);
    d[7] = 'X'; CHECK(ag_clipdata_parse(d, 6 + sizeof png, NULL, NULL, NULL, NULL, NULL) == -1);             /* not a PNG */
    d[7] = 0x89; memcpy(d + 6, png, sizeof png);
    CHECK(ag_clipdata_parse(d, 6 + AG_PNG_MAX + 1, NULL, NULL, NULL, NULL, NULL) == -1);                       /* too large */
    free(d);
    CHECK(ag_hello_caps((const uint8_t *)"w2 clipboard clipx clipreq display windows", 42) == (AG_HELLO_CLIPBOARD | AG_HELLO_CLIPX | AG_HELLO_CLIPREQ | AG_HELLO_WINDOWS));
    CHECK(!(ag_hello_caps((const uint8_t *)"5 clipboard clipreqx", 20) & AG_HELLO_CLIPREQ));

    /* the control messages */
    VmMsg m = vm_msg(VM_CLIP_OFFER, 1, 5, AGM_TEXT | AGM_HTML, AGO_ASK, 0, NULL); CHECK(vm_msg_valid(&m, 1) && !vm_msg_valid(&m, 0));
    m.a = 0; CHECK(!vm_msg_valid(&m, 1));
    m.a = 5; m.b = 0; CHECK(!vm_msg_valid(&m, 1));
    m.b = 8; CHECK(!vm_msg_valid(&m, 1));
    m.b = 1; m.c = 2; CHECK(!vm_msg_valid(&m, 1));
    m = vm_msg(VM_CLIP_DENY, 1, 5, AGF_PNG, VCD_BACKGROUND, 0, NULL); CHECK(vm_msg_valid(&m, 1));
    m.c = 0; CHECK(!vm_msg_valid(&m, 1));
    m.c = VCD_LAST + 1; CHECK(!vm_msg_valid(&m, 1));
    m.c = VCD_STALE; m.b = 0; CHECK(!vm_msg_valid(&m, 1));                       /* a request without its form */
    m = vm_msg(VM_CLIP_DENY, 1, 0, 0, VCD_BACKGROUND, 0, NULL); CHECK(vm_msg_valid(&m, 1));      /* the guest's change ignored */
    m.b = AGF_TEXT; CHECK(!vm_msg_valid(&m, 1));
    m = vm_msg(VM_CLIP, 1, 10, VCF_CLIPX, 5, AGF_TEXT, NULL); CHECK(vm_msg_valid(&m, 1));          /* an answer */
    m.d = 0; CHECK(!vm_msg_valid(&m, 1));
    m.d = AGF_TEXT; m.b = VCF_TEXT; CHECK(!vm_msg_valid(&m, 1));                  /* answers are in AGK_CLIPX's layout */
    m = vm_msg(VM_CLIP, 1, 10, VCF_TEXT, 0, 0, NULL); CHECK(vm_msg_valid(&m, 1)); /* a push, as before */
    m.c = -1; CHECK(!vm_msg_valid(&m, 1));
    /* VM_WINREFUSED (w8): a window id, an action, a reason; nothing else */
    m = vm_msg(VM_WINREFUSED, 0, 0x10a2c, LAGA_CLOSE, LAGR_ELEVATED, 0, NULL); CHECK(vm_msg_valid(&m, 0) && !vm_msg_valid(&m, 1));
    m.c = LAGR_DENIED; CHECK(vm_msg_valid(&m, 0));
    m.c = 0; CHECK(!vm_msg_valid(&m, 0));
    m.c = 3; CHECK(!vm_msg_valid(&m, 0));
    m.c = 1; m.b = 0; CHECK(!vm_msg_valid(&m, 0));
    m.b = 6; CHECK(!vm_msg_valid(&m, 0));
    m.b = LAGA_MOVE; m.a = 0; CHECK(!vm_msg_valid(&m, 0));
    m.a = -5; CHECK(vm_msg_valid(&m, 0));                              /* an id above 2^31, as an int */
    m.d = 1; CHECK(!vm_msg_valid(&m, 0));
    m = vm_msg(VM_CLIP_REQ, 0, 5, AGF_PNG, 0, 0, NULL); CHECK(vm_msg_valid(&m, 0) && !vm_msg_valid(&m, 1));
    m.a = 0; CHECK(!vm_msg_valid(&m, 0));
    m.a = 5; m.b = 4; CHECK(!vm_msg_valid(&m, 0));
    /* random bytes never crash the parsers */
    uint8_t b[64];
    uint32_t seed = 777;
    for (int it = 0; it < 50000; it++) {
        size_t len = (seed = seed * 1103515245 + 12345) % 48;
        for (size_t i = 0; i < len; i++) b[i] = (uint8_t)((seed = seed * 1103515245 + 12345) >> 16);
        if (it & 1 && len >= 6) b[5] = (uint8_t)(it % 5);
        if (!ag_offer_parse(b, len, &gen, &forms, &flags)) CHECK(len == 6 && gen && forms && !(forms & ~AGM_ALL) && !(flags & ~AGO_ALL));
        if (!ag_clipreq_parse(b, len, &gen, &form)) CHECK(len == 5 && gen && form >= AGF_TEXT && form <= AGF_PNG);
        if (!ag_clipdata_parse(b, len, &gen, &form, &status, &data, &dn)) CHECK(gen && dn == len - 6 && (status == AGD_OK ? data && dn && ag_utf8_ok(data, dn) : !dn));
    }
}

/* Files' "Open with" a machine (docs/VIRTUALIZATION.md 7.3): AGK_OPEN, AGK_OPENED, the hello word, VM_FILE a = 1 */
/* Intel's driver for a GVT-g vGPU (vmgvtwin.h): the fixed script's constants, and the one line it prints */
static void test_gvtwin(void) {
    /* the script carries the constants and nothing to fill in */
    CHECK(strstr(GVTWIN_PS, "$U='" GVTWIN_DRV_URL "'") && strstr(GVTWIN_PS, "$H='" GVTWIN_DRV_SHA256 "'"));
    CHECK(strstr(GVTWIN_PS, "$V='" GVTWIN_DRV_VERSION "'") && strstr(GVTWIN_PS, "$I='" GVTWIN_DRV_INF "'"));
    CHECK(!strchr(GVTWIN_PS, '%') && !strstr(GVTWIN_PS, "@VER@") && !strstr(GVTWIN_PS, "@URL@"));
    static const char host[] = "https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/";
    CHECK(!strncmp(GVTWIN_DRV_URL, host, sizeof host - 1) && !strchr(GVTWIN_DRV_URL, '\'') && !strchr(GVTWIN_DRV_URL, ' '));
    CHECK(strlen(GVTWIN_DRV_SHA256) == 64);
    for (const char *h = GVTWIN_DRV_SHA256; *h; h++) CHECK((*h >= '0' && *h <= '9') || (*h >= 'A' && *h <= 'F'));
    /* the device's ID goes into the block only after the driver is there, and the block is lifted only for our install */
    CHECK(strstr(GVTWIN_PS, "DenyDeviceIDsRetroactive -Value 0") && strstr(GVTWIN_PS, "if($dr -in 'already','installed','restart'){"));
    CHECK(strstr(GVTWIN_PS, "Microsoft Windows Hardware Compatibility Publisher") && strstr(GVTWIN_PS, "O=Intel Corporation"));
    /* PowerShell's variable names ignore case: two names that differ only in case are one variable ($K the registry key
     * and $k a list of SIDs overwrote each other, seen on the dev VM 2026-11-21: "the block: path 'S-1-5-18' not found") */
    {
        char names[64][24];
        int nn = 0, clash = 0;
        for (const char *q = GVTWIN_PS; (q = strchr(q, '$'));) {
            q++;
            size_t l = 0;
            while ((q[l] >= 'a' && q[l] <= 'z') || (q[l] >= 'A' && q[l] <= 'Z') || (q[l] >= '0' && q[l] <= '9') || q[l] == '_') l++;
            if (!l || l >= sizeof names[0]) continue;
            int seen = 0;
            for (int i = 0; i < nn && !seen; i++) {
                if (strlen(names[i]) != l || strncasecmp(names[i], q, l)) continue;
                seen = 1;
                if (strncmp(names[i], q, l)) { clash = 1; fprintf(stderr, "  GVTWIN_PS: $%s and $%.*s are one variable\n", names[i], (int)l, q); }
            }
            if (!seen && nn < 64) { memcpy(names[nn], q, l); names[nn][l] = 0; nn++; }
        }
        CHECK(!clash && nn > 10);
    }
    /* every line ends with a newline (lines aren't run together) and the braces balance outside quotes */
    {
        int depth = 0, q = 0, neg = 0;
        for (const char *c = GVTWIN_PS; *c; c++) {
            if (q) { if (*c == '\'') q = 0; continue; }
            if (*c == '\'') q = 1;
            else if (*c == '{') depth++;
            else if (*c == '}' && --depth < 0) neg = 1;
        }
        CHECK(depth == 0 && !q && !neg);
        CHECK(GVTWIN_PS[strlen(GVTWIN_PS) - 1] == '\n');
    }

    GvtWinResult g;
    char line[400];
    struct { const char *said; int ok, d, b, p; const char *id, *why; } c[] = {
        { "already kept off PCI\\VEN_8086&DEV_9BC5 ", 0, GW_D_ALREADY, GW_B_KEPT, GW_P_OFF, "PCI\\VEN_8086&DEV_9BC5", "" },
        { "already kept off PCI\\VEN_8086&DEV_9BC5", 0, GW_D_ALREADY, GW_B_KEPT, GW_P_OFF, "PCI\\VEN_8086&DEV_9BC5", "" },
        { "installed added turned PCI\\VEN_8086&DEV_3E92 ", 0, GW_D_INSTALLED, GW_B_ADDED, GW_P_TURNED, "PCI\\VEN_8086&DEV_3E92", "" },
        { "restart added off PCI\\VEN_8086&DEV_9BC5 ", 0, GW_D_RESTART, GW_B_ADDED, GW_P_OFF, "PCI\\VEN_8086&DEV_9BC5", "" },
        { "refused none off PCI\\VEN_8086&DEV_9BC5 the download is not the driver LordOS checked (SHA-256)", 0, GW_D_REFUSED, GW_B_NONE, GW_P_OFF, "PCI\\VEN_8086&DEV_9BC5", "the download is not the driver LordOS checked (SHA-256)" },
        { "failed none failed - hibernation: access denied", 0, GW_D_FAILED, GW_B_NONE, GW_P_FAILED, "", "hibernation: access denied" },
        { "nodevice none off - ", 0, GW_D_NODEVICE, GW_B_NONE, GW_P_OFF, "", "" },
        { "busy none failed - another run is at work", 0, GW_D_BUSY, GW_B_NONE, GW_P_FAILED, "", "another run is at work" },
        { "installed failed off PCI\\VEN_8086&DEV_9BC5 the block: denied", 0, GW_D_INSTALLED, GW_B_FAILED, GW_P_OFF, "PCI\\VEN_8086&DEV_9BC5", "the block: denied" },
        /* malformed */
        { "", -1, 0, 0, 0, "", "" },
        { "already", -1, 0, 0, 0, "", "" },
        { "already kept off", -1, 0, 0, 0, "", "" },
        { "Already kept off PCI\\VEN_8086&DEV_9BC5 ", -1, 0, 0, 0, "", "" },
        { "already kept on PCI\\VEN_8086&DEV_9BC5 ", -1, 0, 0, 0, "", "" },
        { "already kept off - ", -1, 0, 0, 0, "", "" },                         /* a driver on no device */
        { "refused added off PCI\\VEN_8086&DEV_9BC5 ", -1, 0, 0, 0, "", "" },  /* a block without our driver */
        { "already kept off PCI\\VEN_10DE&DEV_9BC5 ", -1, 0, 0, 0, "", "" },
        { "already kept off PCI\\VEN_8086&DEV_9bc5 ", -1, 0, 0, 0, "", "" },
        { "already kept off PCI\\VEN_8086&DEV_9BC ", -1, 0, 0, 0, "", "" },
        { "already kept off PCI\\VEN_8086&DEV_9BC55 ", -1, 0, 0, 0, "", "" },
        { "already  kept off PCI\\VEN_8086&DEV_9BC5 ", -1, 0, 0, 0, "", "" },
        { "failed none off - caf\xc3\xa9", -1, 0, 0, 0, "", "" },
        { "failed none off - a\tb", -1, 0, 0, 0, "", "" },
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        int r = gvtwin_parse(c[i].said, &g);
        if (r != c[i].ok) fprintf(stderr, "  gvtwin_parse \"%s\": got %d\n", c[i].said, r);
        CHECK(r == c[i].ok);
        if (r) continue;
        CHECK(g.driver == c[i].d && g.block == c[i].b && g.power == c[i].p && !strcmp(g.id, c[i].id) && !strcmp(g.why, c[i].why));
        gvtwin_describe(&g, line, sizeof line);
        CHECK(!strncmp(line, "intel driver: ", 14) && (!g.why[0] || strstr(line, g.why)));
    }
    /* a reason of exactly GVTWIN_WHY_MAX fits, one more doesn't */
    {
        char t[256];
        int o = snprintf(t, sizeof t, "failed none off - ");
        memset(t + o, 'x', GVTWIN_WHY_MAX); t[o + GVTWIN_WHY_MAX] = 0;
        CHECK(gvtwin_parse(t, &g) == 0 && strlen(g.why) == GVTWIN_WHY_MAX);
        t[o + GVTWIN_WHY_MAX] = 'x'; t[o + GVTWIN_WHY_MAX + 1] = 0;
        CHECK(gvtwin_parse(t, &g) == -1);
    }
    CHECK(gvtwin_parse(NULL, &g) == -1);
    gvtwin_parse("installed added turned PCI\\VEN_8086&DEV_9BC5 ", &g);
    gvtwin_describe(&g, line, sizeof line);
    CHECK(!strcmp(line, "intel driver: " GVTWIN_DRV_VERSION " installed (PCI\\VEN_8086&DEV_9BC5); its updates are blocked now; hibernation and Fast Startup turned off"));
    char tiny[8];
    gvtwin_describe(&g, tiny, sizeof tiny);
    CHECK(!strcmp(tiny, "intel d"));
}

static void test_open_with(void) {
    static const char tok[] = "0123456789abcdef0123456789abcdef";
    uint8_t sha[32], o[AG_OPEN_BYTES], back[32];
    for (int i = 0; i < 32; i++) sha[i] = (uint8_t)(i * 7 + 1);
    char t[AG_OPEN_TOKEN + 1];
    uint64_t size = 0;
    CHECK(!ag_open_write(o, tok, 0x123456789aull, sha));
    CHECK(!ag_open_parse(o, AG_OPEN_BYTES, t, &size, back) && !strcmp(t, tok) && size == 0x123456789aull && !memcmp(back, sha, 32));
    CHECK(ag_open_parse(o, AG_OPEN_BYTES - 1, NULL, NULL, NULL) == -1 && ag_open_parse(NULL, AG_OPEN_BYTES, NULL, NULL, NULL) == -1);
    uint8_t big[AG_OPEN_BYTES + 1]; memcpy(big, o, AG_OPEN_BYTES); big[AG_OPEN_BYTES] = 0;
    CHECK(ag_open_parse(big, sizeof big, NULL, NULL, NULL) == -1);                                     /* a byte too many */
    o[3] = 'A'; CHECK(ag_open_parse(o, AG_OPEN_BYTES, NULL, NULL, NULL) == -1);                        /* upper case */
    o[3] = '/'; CHECK(ag_open_parse(o, AG_OPEN_BYTES, NULL, NULL, NULL) == -1);                        /* a path character */
    o[3] = 0; CHECK(ag_open_parse(o, AG_OPEN_BYTES, NULL, NULL, NULL) == -1);                          /* a NUL */
    CHECK(ag_open_write(o, "0123", 1, sha) == -1 && ag_open_write(o, "..\\x\\456789abcdef0123456789abcdef", 1, sha) == -1);
    CHECK(ag_open_write(o, tok, 1, NULL) == -1);
    CHECK(ag_open_token_ok(tok, 32) && !ag_open_token_ok(tok, 31) && !ag_open_token_ok("0123456789abcdef0123456789abcdeg", 32));
    uint8_t r[AG_OPENED_BYTES], st = 9;
    uint32_t code = 9;
    CHECK(!ag_opened_write(r, tok, AGOS_OK, 0) && !ag_opened_parse(r, sizeof r, t, &st, &code) && !strcmp(t, tok) && st == AGOS_OK && code == 0);
    CHECK(!ag_opened_write(r, tok, AGOS_FAILED, 1155) && !ag_opened_parse(r, sizeof r, NULL, &st, &code) && st == AGOS_FAILED && code == 1155);
    CHECK(!ag_opened_write(r, tok, AGOS_OK, 5) && !ag_opened_parse(r, sizeof r, NULL, NULL, &code) && code == 0);   /* OK carries no code */
    r[AG_OPEN_TOKEN + 1] = 5; CHECK(ag_opened_parse(r, sizeof r, NULL, NULL, NULL) == -1);            /* OK with a code */
    CHECK(ag_opened_write(r, tok, AGOS_LAST + 1, 0) == -1);
    ag_opened_write(r, tok, AGOS_CHANGED, 0); r[AG_OPEN_TOKEN] = AGOS_LAST + 1; CHECK(ag_opened_parse(r, sizeof r, NULL, NULL, NULL) == -1);
    ag_opened_write(r, tok, AGOS_CHANGED, 0); CHECK(ag_opened_parse(r, sizeof r - 1, NULL, NULL, NULL) == -1);
    r[0] = 'z'; CHECK(ag_opened_parse(r, sizeof r, NULL, NULL, NULL) == -1);
    /* AGK_PLACE, AGK_PLACED: files dropped on an apps mode window */
    AgPlace a, b2;
    memset(&a, 0, sizeof a);
    memcpy(a.token, tok, sizeof a.token);
    a.size = 77; memcpy(a.sha, sha, 32); a.win = 0x30140; a.x = 1919; a.y = 0; a.batch = 9; a.index = 2; a.count = 3;
    uint8_t pl[AG_PLACE_BYTES + 1];
    CHECK(!ag_place_write(pl, &a) && !ag_place_parse(pl, AG_PLACE_BYTES, &b2) && b2.win == a.win && b2.x == 1919 && b2.y == 0 && b2.batch == 9 &&
          b2.index == 2 && b2.count == 3 && b2.size == 77 && !strcmp(b2.token, tok) && !memcmp(b2.sha, sha, 32));
    CHECK(ag_place_parse(pl, AG_PLACE_BYTES - 1, NULL) == -1 && ag_place_parse(pl, AG_PLACE_BYTES + 1, NULL) == -1);
    AgPlace bad = a; bad.win = 0; CHECK(ag_place_write(pl, &bad) == -1);
    bad = a; bad.x = -1; CHECK(ag_place_write(pl, &bad) == -1);
    bad = a; bad.y = LAG_DIM_MAX + 1; CHECK(ag_place_write(pl, &bad) == -1);
    bad = a; bad.index = 3; CHECK(ag_place_write(pl, &bad) == -1);
    bad = a; bad.count = AG_PLACE_MAX + 1; bad.index = 0; CHECK(ag_place_write(pl, &bad) == -1);
    bad = a; bad.batch = 0; CHECK(ag_place_write(pl, &bad) == -1);
    ag_place_write(pl, &a); pl[AG_OPEN_BYTES + 4 + 3] = 0x80; CHECK(ag_place_parse(pl, AG_PLACE_BYTES, NULL) == -1);    /* x negative */
    ag_place_write(pl, &a); pl[AG_OPEN_BYTES + 16] = 5; CHECK(ag_place_parse(pl, AG_PLACE_BYTES, NULL) == -1);           /* index ≥ count */
    uint8_t pd[AG_PLACED_BYTES], how = 9;
    CHECK(!ag_placed_write(pd, tok, AGOS_OK, AGP_CLIPBOARD, 0) && !ag_placed_parse(pd, sizeof pd, t, &st, &how, &code) && st == AGOS_OK && how == AGP_CLIPBOARD && !code);
    CHECK(ag_placed_write(pd, tok, AGOS_OK, AGP_NONE, 0) == -1 && ag_placed_write(pd, tok, AGOS_CHANGED, AGP_FOLDER, 0) == -1);
    CHECK(!ag_placed_write(pd, tok, AGOS_FAILED, AGP_NONE, 5) && !ag_placed_parse(pd, sizeof pd, NULL, &st, &how, &code) && st == AGOS_FAILED && code == 5);
    pd[AG_OPEN_TOKEN + 1] = AGP_LAST + 1; CHECK(ag_placed_parse(pd, sizeof pd, NULL, NULL, NULL, NULL) == -1);
    CHECK(ag_placed_parse(pd, sizeof pd - 1, NULL, NULL, NULL, NULL) == -1);
    VmMsg pm = vm_msg(VM_FILE, 1, 2, 0x30140, 100 | 200 << 16, 9 << 16 | 1 << 8 | 3, "a.txt"); CHECK(vm_msg_valid(&pm, 1));
    pm.b = 0; CHECK(!vm_msg_valid(&pm, 1));
    pm.b = 5; pm.d = 9 << 16 | 3 << 8 | 3; CHECK(!vm_msg_valid(&pm, 1));          /* index ≥ count */
    pm.d = 9 << 16 | 0 << 8 | 9; CHECK(!vm_msg_valid(&pm, 1));                   /* more than 8 */
    pm.d = 0 << 16 | 0 << 8 | 1; CHECK(!vm_msg_valid(&pm, 1));                   /* no batch */
    pm.d = 1 << 16 | 1; pm.c = 16385; CHECK(!vm_msg_valid(&pm, 1));               /* x too far */
    pm.c = -1; CHECK(!vm_msg_valid(&pm, 1));
    pm.c = 0; pm.a = 3; CHECK(!vm_msg_valid(&pm, 1));
    CHECK(ag_hello_caps((const uint8_t *)"w4 open place", 13) == (AG_HELLO_OPEN | AG_HELLO_PLACE));
    CHECK(ag_hello_caps((const uint8_t *)"w5 open place pointer", 21) == (AG_HELLO_OPEN | AG_HELLO_PLACE | AG_HELLO_POINTER));
    CHECK(ag_hello_caps((const uint8_t *)"w5 pointers", 11) == 0);
    CHECK(ag_hello_caps((const uint8_t *)"w6 pointer gvt", 14) == (AG_HELLO_POINTER | AG_HELLO_GVT));
    CHECK(ag_hello_caps((const uint8_t *)"w6 gvtx gv", 10) == 0);
    /* the hello word */
    CHECK(ag_hello_caps((const uint8_t *)"w3 clipboard open windows", 25) == (AG_HELLO_CLIPBOARD | AG_HELLO_OPEN | AG_HELLO_WINDOWS));
    CHECK(!(ag_hello_caps((const uint8_t *)"w3 opens openx", 14) & AG_HELLO_OPEN));
    /* VM_FILE: a is 0 (a drop) or 1 (open it there) */
    VmMsg m = vm_msg(VM_FILE, 1, 1, 0, 0, 0, "winbox.exe"); CHECK(vm_msg_valid(&m, 1));
    m.a = 0; CHECK(vm_msg_valid(&m, 1));
    m.a = 2; CHECK(!vm_msg_valid(&m, 1));
    m.a = -1; CHECK(!vm_msg_valid(&m, 1));
    m = vm_msg(VM_AGENT_STATE, 0, VAS_CONNECTED, VAC_OPEN | VAC_FILES, 0, 0, "w3\twindows"); CHECK(vm_msg_valid(&m, 0));
    m.b = VAC_ALL + 1; CHECK(!vm_msg_valid(&m, 0));
    /* random bytes never crash the parsers, and what they take is exactly what they promise */
    uint8_t b[128];
    uint32_t seed = 4242;
    for (int it = 0; it < 60000; it++) {
        size_t len = (seed = seed * 1103515245 + 12345) % 120;
        for (size_t i = 0; i < len; i++) b[i] = (uint8_t)((seed = seed * 1103515245 + 12345) >> 16);
        /* often a valid token in front, so the rest of the parser is reached */
        if (it % 3 && len >= AG_OPEN_TOKEN) for (int i = 0; i < AG_OPEN_TOKEN; i++) b[i] = (uint8_t)"0123456789abcdef"[b[i] & 15];
        if (it % 5 == 0 && len == AG_OPEN_BYTES + 1) len--;
        if (it % 7 == 0 && len >= AG_OPENED_BYTES) { len = AG_OPENED_BYTES; b[AG_OPEN_TOKEN] &= 3; }
        if (it % 11 == 0 && len >= AG_PLACED_BYTES) { len = AG_PLACED_BYTES; b[AG_OPEN_TOKEN] &= 3; b[AG_OPEN_TOKEN + 1] &= 3; }
        if (it % 13 == 0 && len >= AG_PLACE_BYTES) { len = AG_PLACE_BYTES; b[AG_OPEN_BYTES + 7] = b[AG_OPEN_BYTES + 11] = 0; b[AG_OPEN_BYTES + 17] = (b[AG_OPEN_BYTES + 17] & 7) + 1; b[AG_OPEN_BYTES + 16] &= 7; }
        if (!ag_open_parse(b, len, t, &size, back)) CHECK(len == AG_OPEN_BYTES && ag_open_token_ok(t, strlen(t)) && strlen(t) == AG_OPEN_TOKEN);
        if (!ag_opened_parse(b, len, t, &st, &code)) CHECK(len == AG_OPENED_BYTES && st <= AGOS_LAST && (st != AGOS_OK || !code) && strlen(t) == AG_OPEN_TOKEN);
        AgPlace r;
        if (!ag_place_parse(b, len, &r)) CHECK(len == AG_PLACE_BYTES && r.win && r.x >= 0 && r.y >= 0 && r.x <= LAG_DIM_MAX && r.y <= LAG_DIM_MAX && r.batch && r.index < r.count && r.count <= AG_PLACE_MAX);
        if (!ag_placed_parse(b, len, t, &st, &how, &code)) CHECK(len == AG_PLACED_BYTES && st <= AGOS_LAST && how <= AGP_LAST && (st == AGOS_OK ? how && !code : !how));
    }
}

int main(void) {
    test_gvtwin();
    test_open_with();
    test_clip_on_demand();
    test_resolution();
    test_lag();
    test_displays();
    test_shown_pick();
    test_winrefused();
    test_tablet_layout();
    test_apps_mode();
    test_clipx();
    test_log_clean();
    test_images();
    test_schema();
    test_network();
    test_ops();
    test_agent();
    test_drop_names();
    test_host();
    test_messages();
    test_qmp();
    test_keys();
    printf("vm-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
