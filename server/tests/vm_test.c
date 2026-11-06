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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
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
        { "schema", "4", VM_E_NEWER }, { "schema", "0", VM_E_SCHEMA }, { "schema", "\"1\"", VM_E_SCHEMA }, { "schema", NULL, VM_E_SCHEMA },
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
        { "display", "\"spice\"", VM_E_RANGE }, { "network", "\"bridged\"", VM_E_RANGE }, { "network", "\"isolated\"", VM_OK },
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
    const char newer[] = "{\"schema\":9,\"whatever\":true}";
    CHECK(vm_machine_parse(newer, strlen(newer), &m, err, sizeof err) == VM_E_NEWER);
    VmMachine n9;
    vm_machine_parse(GOOD, strlen(GOOD), &n9, err, sizeof err);
    n9.schema = 4;
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
}

int main(void) {
    test_clipx();
    test_log_clean();
    test_images();
    test_schema();
    test_ops();
    test_agent();
    test_host();
    test_messages();
    test_qmp();
    test_keys();
    printf("vm-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
