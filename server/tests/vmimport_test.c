/* vmimport_test.c — the importer's parsers (docs/VIRTUALIZATION.md 6.22), no QEMU needed. Fixture files are invented
 * (tests/vmimport/: no real machine's data). Per format: what is carried (name, CPUs and topology, memory, firmware, the
 * SMBIOS UUID as the guest read it, MACs and card models, disks and their order, serials) and what stops or is lost.
 * Then the hostile cases: XML declarations (XXE, entity expansion), bad entities, deep nesting, path names that leave the
 * bundle, tar members with folders, links or "..", and every fixture truncated at every length and mutated at random
 * (run under ASan and UBSan by make test). Run from server/. */
#include "vm/vmimport.h"
#include "vm/vmxml.h"
#include "vm/machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); } } while (0)

static char *slurp(const char *name, size_t *len) {
    char p[256];
    snprintf(p, sizeof p, "tests/vmimport/%s", name);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "missing fixture %s\n", p); exit(1); }
    char *b = malloc(1 << 20);
    *len = fread(b, 1, (1 << 20) - 1, f);
    b[*len] = 0;
    fclose(f);
    return b;
}

static VmiMachine M;
static char err[256];

static void test_vmx(void) {
    size_t n;
    char *t = slurp("win11.vmx", &n);
    CHECK(vmi_parse_vmx(t, n, &M, err, sizeof err) == 0);
    CHECK(!strcmp(M.name, "Win 11 \"Work\""));
    CHECK(M.windows && M.firmware == VM_FW_UEFI && M.secure_boot && M.tpm && M.rtc_local);
    CHECK(M.vcpus == 4 && M.sockets == 2 && M.cores == 2 && M.threads == 1 && M.memory_mb == 8192);
    /* virtualHW 19: SMBIOS 2.7, and a Windows guest reads uuid.bios little-endian */
    CHECK(!strcmp(M.uuid, "34124d56-7856-bc9a-def0-112233445566"));
    CHECK(M.nnic == 2 && !strcmp(M.nic[0].mac, "00:0c:29:ab:cd:ef") && !strcmp(M.nic[0].model, "e1000e"));
    CHECK(!strcmp(M.nic[1].mac, "00:50:56:01:02:03") && !strcmp(M.nic[1].model, "vmxnet3"));
    /* the boot disk (bios.hddOrder) first, then the others; the ISO isn't a disk */
    CHECK(M.ndisk == 2 && !strcmp(M.disk[0].chain[0].path, "Win 11-000002.vmdk") && !strcmp(M.disk[0].bus, "nvme") && M.disk[0].boot);
    CHECK(!strcmp(M.disk[1].chain[0].path, "data.vmdk") && !strcmp(M.disk[1].bus, "sata") && !M.disk[1].boot);
    CHECK(M.cdrom_files == 1 && M.shared_folders == 1 && !M.saved_state && !M.encrypted);
    free(t);

    t = slurp("linux-old.vmx", &n);
    CHECK(vmi_parse_vmx(t, n, &M, err, sizeof err) == 0);
    /* virtualHW 10: SMBIOS 2.4, a Linux guest reads the bytes as stored */
    CHECK(!strcmp(M.uuid, "564d1234-5678-9abc-def0-112233445566") && !M.windows && !M.rtc_local);
    CHECK(M.firmware == VM_FW_BIOS && M.vcpus == 2 && M.sockets == 2 && M.cores == 1);
    CHECK(M.nnic == 1 && !strcmp(M.nic[0].mac, "00:0c:29:11:22:33") && !strcmp(M.nic[0].model, "pcnet"));
    CHECK(M.saved_state == 1 && M.ndisk == 1 && !strcmp(M.disk[0].serial, "ddb.uuid"));
    free(t);

    /* acpi.smbiosVersion2.7 overrides the hardware version */
    const char *x = "config.version = \"8\"\nvirtualHW.version = \"10\"\nacpi.smbiosVersion2.7 = \"TRUE\"\nmemsize = \"1024\"\nguestOS = \"other\"\n"
                    "uuid.bios = \"56 4d 12 34 56 78 9a bc-de f0 11 22 33 44 55 66\"\n";
    CHECK(vmi_parse_vmx(x, strlen(x), &M, err, sizeof err) == 0 && !strcmp(M.uuid, "34124d56-7856-bc9a-def0-112233445566"));
    CHECK(vmi_parse_vmx("hello\n", 6, &M, err, sizeof err) < 0);
    x = "config.version = \"8\"\nmemsize = \"1024\"\nencryption.keySafe = \"vmware:key/list/(pair/...)\"\n";
    CHECK(vmi_parse_vmx(x, strlen(x), &M, err, sizeof err) == 0 && M.encrypted);
    x = "config.version = \"8\"\nmemsize = \"1024\"\nguestOS = \"arm-windows11-64\"\n";
    CHECK(vmi_parse_vmx(x, strlen(x), &M, err, sizeof err) == 0 && M.arch == VM_ARCH_ARM64 && M.windows);
}

static void test_vbox(void) {
    size_t n;
    char *t = slurp("debian.vbox", &n);
    CHECK(vmi_parse_vbox(t, n, &M, err, sizeof err) == 0);
    CHECK(!strcmp(M.name, "Debian & tests") && !M.windows && M.firmware == VM_FW_BIOS && !M.rtc_local);
    CHECK(M.vcpus == 2 && M.memory_mb == 1024 && M.arch == -1);
    /* no hardware uuid: the machine's, in network order, SMBIOS 2.5: Linux reads it as written */
    CHECK(!strcmp(M.uuid, "11111111-2222-3333-4444-555566667777"));
    CHECK(!strcmp(M.sm_manufacturer, "Example Systems") && !strcmp(M.sm_sku, "SKU-42"));
    CHECK(M.nnic == 2 && !strcmp(M.nic[0].mac, "08:00:27:a1:b2:c3") && !strcmp(M.nic[0].model, "e1000") && M.nic[0].connected);
    CHECK(!strcmp(M.nic[1].mac, "08:00:27:d4:e5:f6") && !strcmp(M.nic[1].model, "virtio"));
    /* the SATA disk is a snapshot over its base: the chain is base first */
    CHECK(M.ndisk == 1 && M.disk[0].nchain == 2 && !strcmp(M.disk[0].chain[0].path, "debian.vdi"));
    CHECK(!strcmp(M.disk[0].chain[1].path, "Snapshots/{bbbbbbbb-0000-0000-0000-000000000002}.vdi") && M.disk[0].chain[1].fmt == VMF_VDI);
    CHECK(!strcmp(M.disk[0].chain[0].uuid, "aaaaaaaa-bbbb-cccc-dddd-eeeeffff0011"));
    /* VirtualBox's default SATA serial from the base image's uuid */
    CHECK(!strcmp(M.disk[0].serial, "VBaaaaaaaa-1100ffff"));
    CHECK(M.snapshots >= 1 && M.cdrom_files == 1 && !M.saved_state);
    free(t);

    t = slurp("win.vbox", &n);
    CHECK(vmi_parse_vbox(t, n, &M, err, sizeof err) == 0);
    /* a Windows guest reads network-order bytes little-endian: the hardware uuid comes out swapped */
    CHECK(M.windows && M.rtc_local && !strcmp(M.uuid, "67452301-ab89-efcd-0123-456789abcdef"));
    CHECK(M.firmware == VM_FW_UEFI && M.tpm && M.saved_state && !strcmp(M.nic[0].model, "e1000-82545em") && !strcmp(M.nic[0].mac, "08:00:27:12:34:56"));
    free(t);

    t = slurp("winle.vbox", &n);
    CHECK(vmi_parse_vbox(t, n, &M, err, sizeof err) == 0);
    CHECK(!strcmp(M.uuid, "01234567-89ab-cdef-0123-456789abcdef") && M.arch == VM_ARCH_AMD64);
    CHECK(!strcmp(M.sm_serial, "VirtualBox-01234567-89ab-cdef-0123-456789abcdef"));
    free(t);

    t = slurp("evil.vbox", &n);
    CHECK(vmi_parse_vbox(t, n, &M, err, sizeof err) < 0 && strstr(err, "DOCTYPE"));
    free(t);
}

static void test_pvs(void) {
    size_t n;
    char *t = slurp("guest.pvs", &n);
    CHECK(vmi_parse_pvs(t, n, &M, err, sizeof err) == 0);
    CHECK(!strcmp(M.name, "Office VM") && M.windows && M.firmware == VM_FW_UEFI && M.tpm);
    CHECK(!strcmp(M.uuid, "feedface-1111-4222-8333-444455556666"));
    CHECK(M.vcpus == 2 && M.memory_mb == 4096 && M.nnic == 1 && !strcmp(M.nic[0].mac, "00:1c:42:ab:cd:ef"));
    /* the disk folder by its last name only: the absolute path was another computer's */
    CHECK(M.ndisk == 1 && !strcmp(M.disk[0].chain[0].path, "harddisk.hdd") && M.disk[0].chain[0].fmt == VMF_PARALLELS);
    CHECK(!strcmp(M.disk[0].bus, "sata") && !strcmp(M.disk[0].serial, "PRL0123456789"));
    free(t);

    VmiHdd h;
    t = slurp("DiskDescriptor.xml", &n);
    CHECK(vmi_parse_hdd(t, n, &h, err, sizeof err) == 0);
    CHECK(h.nlayers == 2 && h.snapshots == 1 && h.sectors == 2097152 && !h.encrypted);
    CHECK(!strcmp(h.file[0], "harddisk.hdd.0.{5fbaabe3-6958-40ff-92a7-860e329aab41}.hds"));
    CHECK(!strcmp(h.file[1], "harddisk.hdd.0.{6abcdef0-1111-2222-3333-444455556666}.hds"));
    /* a loop in the snapshot tree */
    char *loop = strdup(t);
    char *pg = strstr(loop, "<ParentGUID>{00000000");
    memcpy(pg + 12, "{6abcdef0-1111-2222-3333-444455556666}", 38);
    CHECK(vmi_parse_hdd(loop, strlen(loop), &h, err, sizeof err) < 0);
    free(loop);
    free(t);
}

static void test_ovf(void) {
    size_t n;
    char *t = slurp("server.ovf", &n);
    CHECK(vmi_parse_ovf(t, n, &M, err, sizeof err) == 0);
    CHECK(!strcmp(M.name, "server-2024") && M.firmware == VM_FW_UEFI && !M.windows);
    CHECK(M.vcpus == 2 && M.cores == 2 && M.sockets == 1 && M.memory_mb == 4096);
    CHECK(M.nnic == 1 && !strcmp(M.nic[0].mac, "00:50:56:aa:bb:cc") && !strcmp(M.nic[0].model, "vmxnet3"));
    CHECK(M.ndisk == 1 && !strcmp(M.disk[0].chain[0].path, "server-disk1.vmdk") && M.disk[0].chain[0].fmt == VMF_VMDK);
    CHECK(M.disk[0].vsize == 20LL << 30 && !M.uuid[0]);
    free(t);
    const char *gz = "<Envelope><References><File id=\"f\" href=\"d.vmdk.gz\" compression=\"gzip\"/></References><DiskSection><Disk diskId=\"d\" fileRef=\"f\"/></DiskSection>"
                     "<VirtualSystem id=\"x\"><VirtualHardwareSection><Item><ResourceType>4</ResourceType><VirtualQuantity>512</VirtualQuantity></Item>"
                     "<Item><ResourceType>17</ResourceType><HostResource>ovf:/disk/d</HostResource></Item></VirtualHardwareSection></VirtualSystem></Envelope>";
    CHECK(vmi_parse_ovf(gz, strlen(gz), &M, err, sizeof err) == 0 && strstr(M.problem, "gzip"));
}

static void test_vmdk(void) {
    size_t n;
    char *t = slurp("split.vmdk", &n);
    VmiVmdk d;
    CHECK(vmi_parse_vmdk(t, n, &d, err, sizeof err) == 0);
    CHECK(!strcmp(d.create_type, "twoGbMaxExtentSparse") && !strcmp(d.parent, "base.vmdk") && d.nextents == 3);
    CHECK(!strcmp(d.ext[0].file, "split-s001.vmdk") && d.ext[0].sectors == 4192256 && !strcmp(d.ext[1].kind, "SPARSE"));
    CHECK(!strcmp(d.ext[2].kind, "ZERO") && !d.ext[2].file[0] && !d.encrypted);
    CHECK(!strncmp(d.uuid, "60 00 C2 9a", 11));
    free(t);
    CHECK(vmi_parse_vmdk("just text\n", 10, &d, err, sizeof err) < 0);
    const char *neg = "# Disk DescriptorFile\ncreateType=\"monolithicFlat\"\nRW 2048 FLAT \"a-flat.vmdk\" -512\n";
    CHECK(vmi_parse_vmdk(neg, strlen(neg), &d, err, sizeof err) < 0);
    char *lng = malloc(4000);
    snprintf(lng, 4000, "# Disk DescriptorFile\ncreateType=\"monolithicFlat\"\nRW 2048 FLAT \"%01500d.vmdk\" 0\n", 7);
    CHECK(vmi_parse_vmdk(lng, strlen(lng), &d, err, sizeof err) < 0);
    free(lng);
}

static void test_helpers(void) {
    /* names a bundle may use */
    CHECK(vmi_rel_ok("disk.vmdk") && vmi_rel_ok("Snapshots/{a}.vdi") && vmi_rel_ok("a b/c.hds"));
    const char *bad[] = { "", "/etc/passwd", "../x.vmdk", "a/../../x", "a/./b", "a//b", "./a", "..", "a\\b", "c:/x", "nbd://host/x", "a\n", "~/x", "a/" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(!vmi_rel_ok(bad[i]));
    char mac[18];
    CHECK(!vmi_mac("00:0C:29:AA:BB:CC", mac) && !strcmp(mac, "00:0c:29:aa:bb:cc"));
    CHECK(!vmi_mac("080027A1B2C3", mac) && !strcmp(mac, "08:00:27:a1:b2:c3"));
    CHECK(!vmi_mac("00-1c-42-01-02-03", mac) && !strcmp(mac, "00:1c:42:01:02:03"));
    CHECK(vmi_mac("01:00:5e:00:00:01", mac) < 0 && vmi_mac("00:00:00:00:00:00", mac) < 0 && vmi_mac("00:0c:29:aa:bb", mac) < 0 && vmi_mac("00:0c:29:aa:bb:cc:dd", mac) < 0 && vmi_mac("0:0c:29:aa:bb:cc", mac) < 0);
    CHECK(vm_mac_valid("00:0c:29:aa:bb:cc") && vm_mac_valid("52:54:00:12:34:56") && !vm_mac_valid("01:0c:29:aa:bb:cc") && !vm_mac_valid("00:00:00:00:00:00") && !vm_mac_valid("00:0C:29:aa:bb:cc"));
    uint8_t u[16];
    char s[37];
    CHECK(!vmi_uuid_bytes("{01234567-89AB-cdef-0123-456789abcdef}", u));
    vmi_uuid_str(u, s);
    CHECK(!strcmp(s, "01234567-89ab-cdef-0123-456789abcdef"));
    vmi_uuid_swap(u);
    vmi_uuid_str(u, s);
    CHECK(!strcmp(s, "67452301-ab89-efcd-0123-456789abcdef"));
    CHECK(vmi_uuid_bytes("{0123", u) < 0 && vmi_uuid_bytes("01234567-89ab-cdef-0123-456789abcdef00", u) < 0 && vmi_uuid_bytes("{01234567-89ab-cdef-0123-456789abcdef", u) < 0);
    /* formats from bytes */
    uint8_t h[512] = { 0 };
    memcpy(h, "KDMV", 4); CHECK(vmi_disk_format(h, 512, "x.vmdk") == VMF_VMDK);
    memset(h, 0, 4); h[64] = 0x7f; h[65] = 0x10; h[66] = 0xda; h[67] = 0xbe; CHECK(vmi_disk_format(h, 512, "x") == VMF_VDI);
    memset(h, 0, 512); memcpy(h, "WithouFreSpacExt", 16); CHECK(vmi_disk_format(h, 512, "x.hds") == VMF_PARALLELS);
    memset(h, 0, 512); CHECK(vmi_disk_format(h, 512, "x.qcow2") == VMF_UNKNOWN && vmi_disk_format(h, 512, "x.img") == VMF_RAW);
    memcpy(h, "vhdxfile", 8); CHECK(vmi_disk_format(h, 512, "x.img") == VMF_VHDX);   /* the bytes win over the name */
    /* a VDI header */
    uint8_t v[512] = { 0 };
    v[64] = 0x7f; v[65] = 0x10; v[66] = 0xda; v[67] = 0xbe; v[70] = 1; v[76] = 4; v[368 + 3] = 1;
    VmiVdi vd;
    CHECK(!vmi_vdi_header(v, 512, &vd) && vd.type == 4 && vd.size == 1 << 24);
    v[70] = 2;
    CHECK(vmi_vdi_header(v, 512, &vd) < 0);
    /* schema 3's checks */
    CHECK(vm_serial_valid("VBaaaaaaaa-1100ffff") && !vm_serial_valid("a,b") && !vm_serial_valid("012345678901234567890") && vm_smbios_valid("Example, Inc.") && !vm_smbios_valid("\x01"));
}

/* ---- XML: hostile input ---- */

static void test_xml(void) {
    const char *refused[] = {
        "<!DOCTYPE a [<!ENTITY x SYSTEM \"file:///etc/passwd\">]><a>&x;</a>",
        "<!DOCTYPE a [<!ENTITY a \"aaaa\"><!ENTITY b \"&a;&a;&a;\">]><a>&b;</a>",
        "<a><!ENTITY x \"y\"></a>",
        "<a>&unknown;</a>", "<a>&#0;</a>", "<a>&#xD800;</a>", "<a>&#x110000;</a>", "<a>&#;</a>", "<a>&amp</a>",
        "<a b=\"1\" b=\"2\"/>", "<a b=1/>", "<a><b></a></b>", "<a>", "</a>", "<a/><b/>", "text<a/>", "<a b=\"<\"/>", "<1a/>", "<a b=\"x\"c=\"y\"/>",
        "<a><![CDATA[x</a>", "<a><!-- x</a>", "<?xml version=\"1.0\"", "",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        VxNode *r = vx_parse(refused[i], strlen(refused[i]), NULL);
        CHECK(r == NULL);
        if (r) { fprintf(stderr, "  accepted: %s\n", refused[i]); vx_free(r); }
    }
    const char *ok = "<?xml version=\"1.0\"?><!-- c --><p:a xmlns:p=\"u\" p:k=\"v &amp; &#x41;&#66;\"> <b>x &lt; y</b><b>2</b><c><![CDATA[<raw>]]></c></p:a>";
    VxNode *r = vx_parse(ok, strlen(ok), NULL);
    CHECK(r && !strcmp(r->name, "a") && !strcmp(vx_attr(r, "k"), "v & AB"));
    CHECK(r && !strcmp(vx_child_text(r, "b"), "x < y") && !strcmp(vx_text(vx_next(vx_child(r, "b"), "b")), "2") && !strcmp(vx_child_text(r, "c"), "<raw>"));
    vx_free(r);
    /* depth and size limits */
    char *deep = malloc(70 * 3 + 70 * 4 + 1);
    size_t o = 0;
    for (int i = 0; i < 70; i++) { memcpy(deep + o, "<a>", 3); o += 3; }
    for (int i = 0; i < 70; i++) { memcpy(deep + o, "</a>", 4); o += 4; }
    CHECK(vx_parse(deep, o, NULL) == NULL);
    o = 0;
    for (int i = 0; i < 60; i++) { memcpy(deep + o, "<a>", 3); o += 3; }
    for (int i = 0; i < 60; i++) { memcpy(deep + o, "</a>", 4); o += 4; }
    r = vx_parse(deep, o, NULL);
    CHECK(r != NULL);
    vx_free(r);
    free(deep);
    size_t big = VX_MAX_INPUT + 1;
    char *b = malloc(big);
    memset(b, ' ', big);
    CHECK(vx_parse(b, big, NULL) == NULL);
    free(b);
    char *many = malloc(8 + (VX_MAX_NODES + 2) * 4 + 8);
    o = 0;
    memcpy(many, "<a>", 3); o = 3;
    for (int i = 0; i < VX_MAX_NODES + 1; i++) { memcpy(many + o, "<b/>", 4); o += 4; }
    memcpy(many + o, "</a>", 4); o += 4;
    CHECK(vx_parse(many, o, NULL) == NULL);
    free(many);
    CHECK(vx_parse("<a>\0</a>", 8, NULL) == NULL);
    /* text in many small pieces (between comments) costs linear time, not its square */
    {
        size_t np = 200000, o2 = 0;
        char *pc = malloc(np * 8 + 16);
        memcpy(pc, "<a>", 3); o2 = 3;
        for (size_t i = 0; i < np; i++) { memcpy(pc + o2, "x<!---->", 8); o2 += 8; }
        memcpy(pc + o2, "</a>", 4); o2 += 4;
        clock_t t0 = clock();
        VxNode *rr = vx_parse(pc, o2, NULL);
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        CHECK(rr && strlen(vx_text(rr)) == np && secs < 1.0);
        if (secs >= 1.0) fprintf(stderr, "  %zu pieces took %.2f s\n", np, secs);
        vx_free(rr);
        free(pc);
    }
}

/* ---- tar ---- */

typedef struct { const uint8_t *p; size_t n; } Mem;
static long mread(void *ctx, void *buf, size_t n, long long off) {
    Mem *m = ctx;
    if (off < 0 || (size_t)off > m->n) return -1;
    size_t k = m->n - (size_t)off < n ? m->n - (size_t)off : n;
    memcpy(buf, m->p + off, k);
    return (long)k;
}
static void tar_hdr(uint8_t *h, const char *name, long size, char type) {
    memset(h, 0, 512);
    snprintf((char *)h, 100, "%s", name);
    snprintf((char *)h + 100, 8, "0000644");
    snprintf((char *)h + 124, 12, "%011lo", size);
    h[156] = (uint8_t)type;
    memcpy(h + 257, "ustar", 5);
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
}
static void test_tar(void) {
    static uint8_t a[512 * 8];
    VmiTarEntry e[8];
    memset(a, 0, sizeof a);
    tar_hdr(a, "server.ovf", 10, '0');
    memcpy(a + 512, "<Envelope>", 10);
    tar_hdr(a + 1024, "server-disk1.vmdk", 600, '0');
    Mem m = { a, sizeof a };
    int n = vmi_tar_list(mread, &m, sizeof a, e, 8, err, sizeof err);
    CHECK(n == 2 && !strcmp(e[0].name, "server.ovf") && e[0].offset == 512 && e[0].size == 10 && e[1].offset == 1536 && e[1].size == 600);
    const char *badnames[] = { "../x.vmdk", "a/b.vmdk", "/etc/passwd" };
    for (int i = 0; i < 3; i++) { memset(a, 0, sizeof a); tar_hdr(a, badnames[i], 1, '0'); CHECK(vmi_tar_list(mread, &m, sizeof a, e, 8, err, sizeof err) < 0); }
    const char types[] = { '1', '2', '3', '4', '6', 'x', 'L' };
    for (size_t i = 0; i < sizeof types; i++) { memset(a, 0, sizeof a); tar_hdr(a, "x.vmdk", 0, types[i]); CHECK(vmi_tar_list(mread, &m, sizeof a, e, 8, err, sizeof err) < 0); }
    memset(a, 0, sizeof a); tar_hdr(a, "x.vmdk", 99999, '0'); CHECK(vmi_tar_list(mread, &m, sizeof a, e, 8, err, sizeof err) < 0);   /* cut short */
    memset(a, 0, sizeof a); tar_hdr(a, "x.vmdk", 1, '0'); a[148] ^= 1; CHECK(vmi_tar_list(mread, &m, sizeof a, e, 8, err, sizeof err) < 0);   /* checksum */
    memset(a, 0, sizeof a); tar_hdr(a, "x.vmdk", 1, '0'); tar_hdr(a + 1024, "x.vmdk", 1, '0'); CHECK(vmi_tar_list(mread, &m, sizeof a, e, 8, err, sizeof err) < 0);  /* twice */
}

/* ---- fuzz-style: every fixture truncated at every length and mutated at random ---- */

static unsigned rnd = 12345;
static unsigned next(void) { rnd = rnd * 1103515245u + 12345u; return rnd >> 8; }

static int parse_any(int kind, const char *t, size_t n) {
    VmiVmdk d;
    VmiHdd h;
    switch (kind) {
    case 0: return vmi_parse_vmx(t, n, &M, err, sizeof err);
    case 1: return vmi_parse_vbox(t, n, &M, err, sizeof err);
    case 2: return vmi_parse_pvs(t, n, &M, err, sizeof err);
    case 3: return vmi_parse_ovf(t, n, &M, err, sizeof err);
    case 4: return vmi_parse_vmdk(t, n, &d, err, sizeof err);
    default: return vmi_parse_hdd(t, n, &h, err, sizeof err);
    }
}

#define SANE(c) do { if (!(c)) { fails++; fprintf(stderr, "%s:%d: fuzz: %s\n", __FILE__, __LINE__, #c); } } while (0)
static void check_sane(void) {
    /* whatever came out stays within its bounds and its strings end (ASan catches the rest) */
    SANE(M.nnic >= 0 && M.nnic <= VMI_MAX_NICS && M.ndisk >= 0 && M.ndisk <= VMI_MAX_DISKS);
    SANE(strlen(M.name) < VMI_STR && strlen(M.uuid) < 37);
    for (int i = 0; i < M.ndisk; i++) SANE(M.disk[i].nchain >= 0 && M.disk[i].nchain <= VMI_MAX_CHAIN);
}

static void test_fuzz(void) {
    static const struct { const char *f; int kind; } F[] = {
        { "win11.vmx", 0 }, { "linux-old.vmx", 0 }, { "debian.vbox", 1 }, { "win.vbox", 1 }, { "winle.vbox", 1 }, { "guest.pvs", 2 },
        { "server.ovf", 3 }, { "split.vmdk", 4 }, { "DiskDescriptor.xml", 5 },
    };
    long runs = 0;
    for (size_t f = 0; f < sizeof F / sizeof F[0]; f++) {
        size_t n;
        char *t = slurp(F[f].f, &n);
        char *c = malloc(n + 1);
        for (size_t l = 0; l <= n; l++) { memcpy(c, t, l); parse_any(F[f].kind, c, l); if (F[f].kind < 4) check_sane(); runs++; }
        static const char *tokens[] = { "<", ">", "/", "\"", "&", "&#", ";", "=", "\n", "{", "}", "..", "<!DOCTYPE", "<![CDATA[", "-->", "\0" };
        for (int it = 0; it < 3000; it++) {
            memcpy(c, t, n);
            size_t len = n;
            int muts = 1 + (int)(next() % 8);
            for (int k = 0; k < muts && len; k++) {
                size_t at = next() % len;
                switch (next() % 4) {
                case 0: c[at] = (char)(next() & 0xff); break;
                case 1: { const char *tk = tokens[next() % (sizeof tokens / sizeof tokens[0])]; size_t tl = strlen(tk) ? strlen(tk) : 1; if (at + tl <= len) memcpy(c + at, tk, tl); break; }
                case 2: if (at + 1 < len) { memmove(c + at, c + at + 1, len - at - 1); len--; } break;
                default: { size_t from = next() % len, l2 = next() % 64; if (from + l2 <= len && at + l2 <= len) memmove(c + at, c + from, l2); } break;
                }
            }
            parse_any(F[f].kind, c, len);
            if (F[f].kind < 4) check_sane();
            runs++;
        }
        free(c);
        free(t);
    }
    printf("vmimport_test: %ld parser runs on truncated and mutated fixtures\n", runs);
}

int main(void) {
    test_helpers();
    test_xml();
    test_vmx();
    test_vbox();
    test_pvs();
    test_ovf();
    test_vmdk();
    test_tar();
    test_fuzz();
    printf("vmimport_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
