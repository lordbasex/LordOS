/* hwscan_test.c — the Store suggestions' hardware facts (src/hw/hwscan.c) over fake trees: DMI,
 * device tree, PCI, USB (interfaces, drivers, udev's libsane flag), virtio ports and the firmware
 * misses of a kernel log (kmsg and dmesg formats, names already present, unsafe names).
 *
 *   build/hwscan-test                          the tests (make test)
 *   build/hwscan-test dump [root]              the facts JSON of a tree, or of this machine
 *   build/hwscan-test fixture <dir> <kind>     writes a fake tree (vmware, virtualbox, nvidia,
 *                                              printer, scanner, rpi5, qemu, firmware, demo) for
 *                                              LORD_HWSCAN_ROOT (screenshots and manual tests) */
#include "hw/hwscan.h"
#include "sec/json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int n_ok, n_bad;
#define CHECK(cond, ...) do { if (cond) n_ok++; else { n_bad++; printf("✗ " __VA_ARGS__); printf("\n"); } } while (0)

static void mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

/* <root>/<rel> with text (parent folders created). len < 0: strlen. */
static void put(const char *root, const char *rel, const char *text, int len) {
    char p[1024], d[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    snprintf(d, sizeof d, "%s", p);
    char *sl = strrchr(d, '/');
    if (sl) { *sl = 0; mkdirs(d); }
    FILE *f = fopen(p, "wb");
    if (!f) { printf("cannot write %s: %s\n", p, strerror(errno)); exit(2); }
    fwrite(text, 1, len < 0 ? strlen(text) : (size_t)len, f);
    fclose(f);
}

static void link_driver(const char *root, const char *dir, const char *driver) {
    char p[1024], target[256];
    snprintf(p, sizeof p, "%s/%s", root, dir);
    mkdirs(p);
    snprintf(p, sizeof p, "%s/%s/driver", root, dir);
    snprintf(target, sizeof target, "../../../bus/drivers/%s", driver);
    unlink(p);
    if (symlink(target, p)) printf("symlink %s: %s\n", p, strerror(errno));
}

static void pci(const char *root, const char *slot, const char *vendor, const char *device, const char *cls, const char *driver) {
    char d[256], f[300];
    snprintf(d, sizeof d, "sys/bus/pci/devices/%s", slot);
    snprintf(f, sizeof f, "%s/vendor", d); put(root, f, vendor, -1);
    snprintf(f, sizeof f, "%s/device", d); put(root, f, device, -1);
    snprintf(f, sizeof f, "%s/class", d); put(root, f, cls, -1);
    if (driver) link_driver(root, d, driver);
}

static void usb_dev(const char *root, const char *name, const char *vendor, const char *product, const char *dev) {
    char d[256], f[300];
    snprintf(d, sizeof d, "sys/bus/usb/devices/%s", name);
    snprintf(f, sizeof f, "%s/idVendor", d); put(root, f, vendor, -1);
    snprintf(f, sizeof f, "%s/idProduct", d); put(root, f, product, -1);
    snprintf(f, sizeof f, "%s/bDeviceClass", d); put(root, f, "00\n", -1);
    if (dev) { snprintf(f, sizeof f, "%s/dev", d); put(root, f, dev, -1); }
}

static void usb_iface(const char *root, const char *name, const char *cls, const char *sub, const char *proto, const char *driver) {
    char d[256], f[300];
    snprintf(d, sizeof d, "sys/bus/usb/devices/%s", name);
    snprintf(f, sizeof f, "%s/bInterfaceClass", d); put(root, f, cls, -1);
    snprintf(f, sizeof f, "%s/bInterfaceSubClass", d); put(root, f, sub, -1);
    snprintf(f, sizeof f, "%s/bInterfaceProtocol", d); put(root, f, proto, -1);
    if (driver) link_driver(root, d, driver);
}

static void dmi(const char *root, const char *vendor, const char *product) {
    put(root, "sys/class/dmi/id/sys_vendor", vendor, -1);
    put(root, "sys/class/dmi/id/product_name", product, -1);
    put(root, "sys/class/dmi/id/chassis_type", "1\n", -1);
}

/* the kinds of fake tree */
static void tree(const char *root, const char *kind) {
    int demo = !strcmp(kind, "demo");
    put(root, "arch", "x86_64\n", -1);
    /* every PC has a host bridge and a USB root hub, which never suggest anything */
    pci(root, "0000:00:00.0", "0x8086\n", "0x29c0\n", "0x060000\n", NULL);
    usb_dev(root, "usb1", "1d6b", "0002", "189:0\n");
    if (!strcmp(kind, "vmware")) {
        dmi(root, "VMware, Inc.\n", "VMware Virtual Platform\n");
        pci(root, "0000:00:0f.0", "0x15ad\n", "0x0405\n", "0x030000\n", "vmwgfx");
    } else if (!strcmp(kind, "virtualbox")) {
        dmi(root, "innotek GmbH\n", "VirtualBox\n");
        pci(root, "0000:00:04.0", "0x80ee\n", "0xcafe\n", "0x088000\n", "vboxguest");
    } else if (!strcmp(kind, "qemu")) {
        dmi(root, "QEMU\n", "Standard PC (Q35 + ICH9, 2009)\n");
        put(root, "sys/class/virtio-ports/vport2p1/name", "org.qemu.guest_agent.0\n", -1);
    } else if (!strcmp(kind, "rpi5")) {
        put(root, "arch", "aarch64\n", -1);
        put(root, "proc/device-tree/model", "Raspberry Pi 5 Model B Rev 1.1", 31);
        put(root, "proc/device-tree/compatible", "raspberrypi,5-model-b\0brcm,bcm2712\0", 35);
    } else {
        dmi(root, "Example Computer Co.\n", "Desktop 3000\n");
    }
    if (demo) put(root, "sys/class/virtio-ports/vport2p1/name", "org.qemu.guest_agent.0\n", -1);
    if (!strcmp(kind, "nvidia") || demo)
        pci(root, "0000:01:00.0", "0x10de\n", "0x2484\n", "0x030000\n", "nouveau");
    if (!strcmp(kind, "printer") || demo) {
        usb_dev(root, "1-2", "03f0", "2b17", "189:2\n");
        usb_iface(root, "1-2:1.0", "07\n", "01\n", "02\n", "usblp");
        usb_iface(root, "1-2:1.1", "07\n", "01\n", "04\n", NULL);
    }
    if (!strcmp(kind, "scanner")) {
        usb_dev(root, "1-3", "04b8", "0142", "189:3\n");
        usb_iface(root, "1-3:1.0", "ff\n", "ff\n", "ff\n", NULL);
        put(root, "run/udev/data/c189:3", "I:123\nE:ID_VENDOR=EPSON\nE:libsane_matched=yes\nG:scanner\n", -1);
    }
    if (!strcmp(kind, "firmware") || demo) {
        pci(root, "0000:02:00.0", "0x14e4\n", "0x43a0\n", "0x028000\n", NULL);
        put(root, "kmsg",
            "6,512,4100000,-;brcmfmac 0000:02:00.0: Direct firmware load for brcm/brcmfmac4360-pcie.bin failed with error -2\n"
            " SUBSYSTEM=pci\n DEVICE=+pci:0000:02:00.0\n"
            "3,513,4100100,-;brcmfmac 0000:02:00.0: brcmf_pcie_setup: Dongle setup failed\n"
            "6,514,4200000,-;bluetooth hci0: Direct firmware load for rtl_bt/rtl8761bu_fw.bin failed with error -2\n", -1);
        if (!demo) put(root, "lib/firmware/rtl_nic/rtl8168h-2.fw", "x", 1);
    }
}

static char *facts_of(const char *root) { return hwscan_json(root); }

static void test_tree(const char *kind, void (*check)(const JVal *)) {
    char root[] = "/tmp/hwscan-test-XXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); exit(2); }
    tree(root, kind);
    char *j = facts_of(root);
    JDoc d;
    char err[128];
    int bad = !j || json_parse(j, strlen(j), &d, err, sizeof err);
    CHECK(!bad, "%s: facts are valid JSON (%s)", kind, j ? err : "NULL");
    if (!bad) {
        CHECK(json_bool(d.root, "fixture", 0) == 1, "%s: fixture flag", kind);
        check(d.root);
        json_free(&d);
    }
    free(j);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd)) printf("cleanup failed\n");
}

static const JVal *nth(const JVal *arr, size_t i) {
    if (!arr || arr->type != J_ARR) return NULL;
    const JVal *v = arr->kid;
    while (v && i--) v = v->next;
    return v;
}
static const char *dmi_s(const JVal *r, const char *k) { return json_str(json_get(r, "dmi"), k); }
static size_t len_of(const JVal *r, const char *k) { const JVal *v = json_get(r, k); return v && v->type == J_ARR ? v->len : 999; }
static const JVal *pci_by(const JVal *r, const char *vendor) {
    for (const JVal *v = json_get(r, "pci") ? json_get(r, "pci")->kid : NULL; v; v = v->next)
        if (json_str(v, "vendor") && !strcmp(json_str(v, "vendor"), vendor)) return v;
    return NULL;
}
#define STREQ(a, b) ((a) && !strcmp((a), (b)))

static void c_vmware(const JVal *r) {
    CHECK(STREQ(dmi_s(r, "sysVendor"), "VMware, Inc."), "vmware: sysVendor");
    const JVal *g = pci_by(r, "15ad");
    CHECK(g && STREQ(json_str(g, "device"), "0405") && STREQ(json_str(g, "class"), "030000") && STREQ(json_str(g, "driver"), "vmwgfx"), "vmware: SVGA device");
    CHECK(len_of(r, "usb") == 0, "vmware: the root hub is not listed");
    const JVal *fw = json_get(r, "firmware");
    CHECK(fw && json_bool(fw, "readable", 1) == 0, "vmware: no kmsg file → not readable");
}
static void c_vbox(const JVal *r) {
    CHECK(STREQ(dmi_s(r, "productName"), "VirtualBox"), "virtualbox: productName");
    CHECK(pci_by(r, "80ee") != NULL, "virtualbox: guest device");
}
static void c_qemu(const JVal *r) {
    const JVal *v = nth(json_get(r, "virtio"), 0);
    CHECK(v && v->type == J_STR && STREQ(v->str, "org.qemu.guest_agent.0"), "qemu: guest agent port");
}
static void c_rpi(const JVal *r) {
    CHECK(STREQ(json_str(r, "model"), "Raspberry Pi 5 Model B Rev 1.1"), "rpi5: model");
    const JVal *c0 = nth(json_get(r, "compatible"), 0), *c1 = nth(json_get(r, "compatible"), 1);
    CHECK(c0 && STREQ(c0->str, "raspberrypi,5-model-b") && c1 && STREQ(c1->str, "brcm,bcm2712"), "rpi5: compatible");
    CHECK(STREQ(json_str(r, "arch"), "aarch64"), "rpi5: arch");
    CHECK(STREQ(dmi_s(r, "sysVendor"), ""), "rpi5: no DMI");
}
static void c_nvidia(const JVal *r) {
    const JVal *g = pci_by(r, "10de");
    CHECK(g && STREQ(json_str(g, "class"), "030000") && STREQ(json_str(g, "driver"), "nouveau"), "nvidia: VGA device on nouveau");
}
static void c_printer(const JVal *r) {
    const JVal *u = nth(json_get(r, "usb"), 0);
    CHECK(u && STREQ(json_str(u, "vendor"), "03f0") && STREQ(json_str(u, "product"), "2b17"), "printer: ids");
    const JVal *i0 = u ? nth(json_get(u, "ifaces"), 0) : NULL, *i1 = u ? nth(json_get(u, "ifaces"), 1) : NULL;
    CHECK(i0 && STREQ(i0->str, "070102") && i1 && STREQ(i1->str, "070104"), "printer: interfaces 07/01/02 and 07/01/04 (IPP over USB)");
    CHECK(u && STREQ(json_str(u, "driver"), "usblp"), "printer: driver");
    CHECK(u && json_bool(u, "sane", 1) == 0, "printer: not a scanner");
}
static void c_scanner(const JVal *r) {
    const JVal *u = nth(json_get(r, "usb"), 0);
    CHECK(u && STREQ(json_str(u, "vendor"), "04b8") && json_bool(u, "sane", 0) == 1, "scanner: udev libsane flag");
}
static void c_firmware(const JVal *r) {
    const JVal *fw = json_get(r, "firmware");
    CHECK(fw && json_bool(fw, "readable", 0) == 1, "firmware: log readable");
    const JVal *m = json_get(fw, "missing");
    const JVal *a = nth(m, 0), *b = nth(m, 1);
    CHECK(m && m->len == 2, "firmware: two misses (%zu)", m ? m->len : 0);
    CHECK(a && STREQ(json_str(a, "file"), "brcm/brcmfmac4360-pcie.bin") && STREQ(json_str(a, "driver"), "brcmfmac"), "firmware: brcm miss");
    CHECK(b && STREQ(json_str(b, "file"), "rtl_bt/rtl8761bu_fw.bin") && STREQ(json_str(b, "driver"), "bluetooth"), "firmware: bluetooth miss");
}

static void parse_tests(void) {
    HwFwMiss m[8];
    const char *log =
        "[    3.1] iwlwifi 0000:00:14.3: Direct firmware load for iwlwifi-so-a0-gf-a0-89.ucode failed with error -2\n"
        "[    3.2] iwlwifi 0000:00:14.3: Direct firmware load for iwlwifi-so-a0-gf-a0-89.ucode failed with error -2\n"
        "[    3.3] r8169 0000:03:00.0: firmware: failed to load rtl_nic/rtl8168h-2.fw (-2)\n"
        "[    3.4] evil 0000:00:01.0: Direct firmware load for ../../etc/shadow failed with error -2\n"
        "[    3.5] evil 0000:00:01.0: Direct firmware load for /etc/shadow failed with error -2\n"
        "[    3.6] evil 0000:00:01.0: Direct firmware load for a b failed with error -2\n"
        "[    3.7] amdgpu: Direct firmware load for amdgpu/navi10_pfp.bin failed with error -2\n"
        "[    3.8] some unrelated line about firmware\n";
    int n = hwscan_fw_parse(log, strlen(log), m, 8);
    CHECK(n == 3, "parse: 3 unique valid misses (%d)", n);
    CHECK(n > 0 && !strcmp(m[0].file, "iwlwifi-so-a0-gf-a0-89.ucode") && !strcmp(m[0].driver, "iwlwifi"), "parse: iwlwifi");
    CHECK(n > 1 && !strcmp(m[1].file, "rtl_nic/rtl8168h-2.fw") && !strcmp(m[1].driver, "r8169"), "parse: Debian's 'failed to load' line");
    CHECK(n > 2 && !strcmp(m[2].file, "amdgpu/navi10_pfp.bin") && !m[2].driver[0], "parse: no device → no driver name");
    CHECK(hwscan_fw_parse(log, strlen(log), m, 1) == 1, "parse: max respected");
    static const char *const bads[] = { "", "/x", "../x", "a/../b", ".hidden", "a//b", "a b", "a;b", "a\\b", "a\n" };
    for (size_t i = 0; i < sizeof bads / sizeof bads[0]; i++) CHECK(!hwscan_fw_name_valid(bads[i]), "name rejected: %s", bads[i]);
    static const char *const goods[] = { "iwlwifi-cc-a0-77.ucode", "rtl_bt/rtl8761bu_fw.bin", "brcm/brcmfmac43455-sdio.raspberrypi,5-model-b.bin", "intel/ibt-0040-0041.sfi", "mediatek/WIFI_RAM_CODE_MT7922_1.bin" };
    for (size_t i = 0; i < sizeof goods / sizeof goods[0]; i++) CHECK(hwscan_fw_name_valid(goods[i]), "name accepted: %s", goods[i]);
    /* a long line without the end of the pattern, and an empty log */
    char big[600];
    memset(big, 'x', sizeof big);
    memcpy(big, "Direct firmware load for ", 25);
    CHECK(hwscan_fw_parse(big, sizeof big, m, 8) == 0, "parse: truncated line");
    CHECK(hwscan_fw_parse("", 0, m, 8) == 0, "parse: empty");
}

static void present_tests(void) {
    char root[] = "/tmp/hwscan-fw-XXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); exit(2); }
    put(root, "lib/firmware/rtl_nic/rtl8168h-2.fw.zst", "x", 1);
    put(root, "lib/firmware/updates/iwlwifi-cc-a0-77.ucode", "x", 1);
    CHECK(hwscan_fw_present(root, "rtl_nic/rtl8168h-2.fw"), "present: .zst");
    CHECK(hwscan_fw_present(root, "iwlwifi-cc-a0-77.ucode"), "present: updates/");
    CHECK(!hwscan_fw_present(root, "rtl_bt/rtl8761bu_fw.bin"), "present: missing");
    CHECK(!hwscan_fw_present(root, "../firmware/rtl_nic/rtl8168h-2.fw.zst"), "present: unsafe name");
    const char *log = "4,1,1,-;r8169 0000:03:00.0: Direct firmware load for rtl_nic/rtl8168h-2.fw failed with error -2\n"
                      "4,2,2,-;bluetooth hci0: Direct firmware load for rtl_bt/rtl8761bu_fw.bin failed with error -2\n";
    HwFwMiss m[4];
    int n = hwscan_fw_missing(root, log, strlen(log), m, 4);
    CHECK(n == 1 && !strcmp(m[0].file, "rtl_bt/rtl8761bu_fw.bin"), "missing: installed since → dropped (%d)", n);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd)) printf("cleanup failed\n");
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "dump")) {
        char *j = hwscan_json(argc > 2 ? argv[2] : "");
        puts(j ? j : "null");
        free(j);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "fixture")) {
        mkdirs(argv[2]);
        tree(argv[2], argv[3]);
        printf("fixture %s in %s (run axia-server with LORD_HWSCAN_ROOT=%s)\n", argv[3], argv[2], argv[2]);
        return 0;
    }
    parse_tests();
    present_tests();
    test_tree("vmware", c_vmware);
    test_tree("virtualbox", c_vbox);
    test_tree("qemu", c_qemu);
    test_tree("rpi5", c_rpi);
    test_tree("nvidia", c_nvidia);
    test_tree("printer", c_printer);
    test_tree("scanner", c_scanner);
    test_tree("firmware", c_firmware);
    /* the real machine: valid JSON, not a fixture */
    char *j = hwscan_json("");
    JDoc d;
    int ok = j && !json_parse(j, strlen(j), &d, NULL, 0);
    CHECK(ok, "this machine: valid JSON");
    if (ok) { CHECK(json_bool(d.root, "fixture", 1) == 0, "this machine: not a fixture"); json_free(&d); }
    free(j);
    printf("hwscan-test: %d ok, %d failed\n", n_ok, n_bad);
    return n_bad ? 1 : 0;
}
