/* vmusb_test.c — src/vm/vmusb.c on a made-up sysfs (docs/VIRTUALIZATION.md 6.6): listing, the refusals (a hub, the
 * session's keyboard held open, storage for someone who isn't an administrator, a mounted or held disk, swap), the
 * request, rule and line parsers (and random input into each), and the descriptor check. Built with ASan and UBSan. */
#include "../src/vm/vmusb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char root[256];

static void mk(const char *rel) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    for (char *q = p + strlen(root) + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
    mkdir(p, 0755);
}
static void put(const char *rel, const char *text) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    char d[1024];
    snprintf(d, sizeof d, "%s", rel);
    char *sl = strrchr(d, '/');
    if (sl) { *sl = 0; mk(d); }
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(1); }
    fputs(text, f);
    fclose(f);
}
static void lnk(const char *rel, const char *to) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    unlink(p);
    if (symlink(to, p)) { perror(p); exit(1); }
}

#define DEV "sys/bus/usb/devices"

static void device(const char *name, int bus, int dev, const char *vid, const char *pid, const char *cls, const char *product) {
    char b[256], v[32];
    snprintf(b, sizeof b, DEV "/%s/busnum", name); snprintf(v, sizeof v, "%d\n", bus); put(b, v);
    snprintf(b, sizeof b, DEV "/%s/devnum", name); snprintf(v, sizeof v, "%d\n", dev); put(b, v);
    snprintf(b, sizeof b, DEV "/%s/idVendor", name); snprintf(v, sizeof v, "%s\n", vid); put(b, v);
    snprintf(b, sizeof b, DEV "/%s/idProduct", name); snprintf(v, sizeof v, "%s\n", pid); put(b, v);
    snprintf(b, sizeof b, DEV "/%s/dev", name); snprintf(v, sizeof v, "189:%d\n", (bus - 1) * 128 + dev - 1); put(b, v);
    snprintf(b, sizeof b, DEV "/%s/bDeviceClass", name); snprintf(v, sizeof v, "%s\n", cls); put(b, v);
    snprintf(b, sizeof b, DEV "/%s/speed", name); put(b, "480\n");
    snprintf(b, sizeof b, DEV "/%s/product", name); put(b, product);
}
static void iface(const char *name, const char *ifn, const char *cls) {
    char b[256];
    snprintf(b, sizeof b, DEV "/%s/%s:%s/bInterfaceClass", name, name, ifn);
    char v[8];
    snprintf(v, sizeof v, "%s\n", cls);
    put(b, v);
}

static void tree(void) {
    /* the root hub */
    device("usb1", 1, 1, "1d6b", "0002", "09", "xHCI Host Controller\n");
    iface("usb1", "1.0", "09");
    /* a webcam with a microphone; its product name has control bytes and UTF-8 that must not come through */
    device("1-1", 1, 2, "046d", "0843", "ef", "Logitech\x1b[31m Webcam \xc3\xa9 C930e\n");
    put(DEV "/1-1/serial", "FA11927E\n");
    put(DEV "/1-1/manufacturer", "Logitech\n");
    iface("1-1", "1.0", "0e"); iface("1-1", "1.1", "0e"); iface("1-1", "1.2", "01"); iface("1-1", "1.3", "01");
    /* a keyboard: its event node is 1:3 (the numbers of /dev/null, which a fake process below holds) */
    device("1-2", 1, 3, "046d", "c52b", "00", "USB Receiver\n");
    iface("1-2", "1.0", "03");
    put(DEV "/1-2/1-2:1.0/0003:046D:C52B.0001/input/input5/event7/dev", "1:3\n");
    put(DEV "/1-2/1-2:1.0/0003:046D:C52B.0001/input/input5/event7/uevent", "\n");
    put("class/input/.k", "");
    lnk(DEV "/1-2/1-2:1.0/0003:046D:C52B.0001/input/input5/event7/subsystem", "../../../../../../../../class/input");
    /* a USB disk with one partition */
    device("1-3", 1, 4, "0781", "5581", "00", "Ultra\n");
    put(DEV "/1-3/serial", "4C530001\n");
    iface("1-3", "1.0", "08");
    put(DEV "/1-3/1-3:1.0/host6/target6:0:0/6:0:0:0/block/sdz/dev", "250:0\n");
    put(DEV "/1-3/1-3:1.0/host6/target6:0:0/6:0:0:0/block/sdz/sdz1/dev", "250:1\n");
    mk(DEV "/1-3/1-3:1.0/host6/target6:0:0/6:0:0:0/block/sdz/holders");
    mk(DEV "/1-3/1-3:1.0/host6/target6:0:0/6:0:0:0/block/sdz/sdz1/holders");
    put("class/block/.k", "");
    lnk(DEV "/1-3/1-3:1.0/host6/target6:0:0/6:0:0:0/block/sdz/subsystem", "../../../../../../../../class/block");
    lnk(DEV "/1-3/1-3:1.0/host6/target6:0:0/6:0:0:0/block/sdz/sdz1/subsystem", "../../../../../../../../../class/block");
    /* a hub with a device below it (its folder inside the hub's, as sysfs has it) */
    device("1-4", 1, 5, "05e3", "0608", "09", "USB2.0 Hub\n");
    iface("1-4", "1.0", "09");
    device("1-4.1", 1, 6, "0a12", "0001", "e0", "\n");
    /* bus 2 */
    device("usb2", 2, 1, "1d6b", "0003", "09", "xHCI Host Controller\n");
    /* a device whose attributes are garbage: left out */
    device("1-9", 1, 9, "zz12", "0001", "00", "Bad\n");
    /* empty /proc and one with a process holding the keyboard's node (1:3 through /dev/null) */
    mk("proc-empty");
    mk("proc/123/fd");
    lnk("proc/123/fd/4", "/dev/null");
    put("proc/123/fd/5", "");               /* a regular file: not a device */
    put("mountinfo-none", "22 1 0:21 / /proc rw - proc proc rw\n");
    put("mountinfo-sdz1", "22 1 0:21 / /proc rw - proc proc rw\n36 35 250:1 / /media/x rw - vfat /dev/sdz1 rw\n");
    put("swaps-none", "Filename\tType\tSize\tUsed\tPriority\n");
}

static void p(char *out, const char *rel) { snprintf(out, 512, "%s/%s", root, rel); }

static void test_list(void) {
    char sys[512];
    p(sys, DEV);
    VmUsbDev d[VMUSB_MAX];
    int n = vmusb_list(sys, d, VMUSB_MAX);
    CHECK(n == 5, "five devices (no root hub, not the garbage one): %d", n);
    int cam = -1, kb = -1, disk = -1, hub = -1;
    for (int i = 0; i < n; i++) {
        if (d[i].vendor == 0x046d && d[i].product == 0x0843) cam = i;
        if (d[i].vendor == 0x046d && d[i].product == 0xc52b) kb = i;
        if (d[i].vendor == 0x0781) disk = i;
        if (d[i].vendor == 0x05e3) hub = i;
        CHECK(strncmp(d[i].name, "usb", 3), "root hub listed");
    }
    CHECK(cam >= 0 && kb >= 0 && disk >= 0 && hub >= 0, "each one found");
    if (cam < 0 || kb < 0 || disk < 0 || hub < 0) return;
    CHECK(d[cam].bus == 1 && d[cam].dev == 2 && d[cam].major == 189 && d[cam].minor == 1, "camera's numbers");
    CHECK((d[cam].flags & VMUSB_F_VIDEO) && (d[cam].flags & VMUSB_F_AUDIO) && !(d[cam].flags & VMUSB_F_HUB), "camera's flags %x", d[cam].flags);
    CHECK(!strcmp(d[cam].serial, "FA11927E"), "serial %s", d[cam].serial);
    for (const char *q = d[cam].model; *q; q++) CHECK(*q >= 0x20 && *q < 0x7f, "a control or non-ASCII byte in the name");
    CHECK(strstr(d[cam].model, "Webcam") != NULL, "name kept: %s", d[cam].model);
    CHECK((d[kb].flags & VMUSB_F_INPUT) && (d[kb].flags & VMUSB_F_HID), "keyboard flags %x", d[kb].flags);
    CHECK((d[disk].flags & VMUSB_F_STORAGE) && (d[disk].flags & VMUSB_F_BLOCK), "disk flags %x", d[disk].flags);
    CHECK(d[hub].flags & VMUSB_F_HUB, "hub flag");
    CHECK(!(d[hub].flags & (VMUSB_F_INPUT | VMUSB_F_BLOCK)), "the hub's child isn't walked into");
    for (int i = 1; i < n; i++) CHECK(d[i - 1].bus < d[i].bus || (d[i - 1].bus == d[i].bus && d[i - 1].dev < d[i].dev), "sorted");
    VmUsbDev one;
    CHECK(!vmusb_find(sys, 1, 4, &one) && one.vendor == 0x0781, "find by bus and address");
    CHECK(vmusb_find(sys, 1, 99, &one), "a missing address");
    CHECK(!vmusb_find(sys, 1, 1, &one) && (one.flags & VMUSB_F_ROOTHUB), "the root hub found by address is one");
    VmUsbNode nodes[8];
    CHECK(vmusb_nodes(sys, &d[disk], nodes, 8) == 2, "disk and its partition");
    CHECK(vmusb_nodes(sys, &d[cam], nodes, 8) == 0, "no nodes under the camera");
}

static void test_refusals(void) {
    char sys[512], pe[512], ph[512], mn[512], ms[512], sw[512];
    p(sys, DEV); p(pe, "proc-empty"); p(ph, "proc"); p(mn, "mountinfo-none"); p(ms, "mountinfo-sdz1"); p(sw, "swaps-none");
    VmUsbDev d;
    vmusb_find(sys, 1, 1, &d);
    CHECK(vmusb_refusal(sys, pe, mn, sw, &d, 1) && !strcmp(vmusb_refusal(sys, pe, mn, sw, &d, 1), "vm.usbHub"), "root hub refused");
    vmusb_find(sys, 1, 5, &d);
    CHECK(!strcmp(vmusb_refusal(sys, pe, mn, sw, &d, 1), "vm.usbHub"), "hub refused");
    vmusb_find(sys, 1, 2, &d);
    CHECK(vmusb_refusal(sys, pe, mn, sw, &d, 0) == NULL, "camera allowed, even for someone who isn't an administrator");
    vmusb_find(sys, 1, 3, &d);
    CHECK(vmusb_refusal(sys, pe, mn, sw, &d, 1) == NULL, "a keyboard nobody holds may go");
    const char *r = vmusb_refusal(sys, ph, mn, sw, &d, 1);
    CHECK(r && !strcmp(r, "vm.usbInputInUse"), "the keyboard a process holds is refused: %s", r ? r : "(none)");
    vmusb_find(sys, 1, 4, &d);
    r = vmusb_refusal(sys, pe, mn, sw, &d, 0);
    CHECK(r && !strcmp(r, "vm.usbStorageAdmin"), "storage for someone who isn't an administrator: %s", r ? r : "(none)");
    CHECK(vmusb_refusal(sys, pe, mn, sw, &d, 1) == NULL, "an unmounted disk for an administrator");
    r = vmusb_refusal(sys, pe, ms, sw, &d, 1);
    CHECK(r && !strcmp(r, "vm.usbMounted"), "a mounted partition: %s", r ? r : "(none)");
    char missing[600];
    snprintf(missing, sizeof missing, "%s/no-such-mountinfo", root);
    r = vmusb_refusal(sys, pe, missing, sw, &d, 1);
    CHECK(r && !strcmp(r, "vm.usbMounted"), "can't read mountinfo: as if mounted");
    put(DEV "/1-3/1-3:1.0/host6/target6:0:0/6:0:0:0/block/sdz/sdz1/holders/dm-3", "");
    r = vmusb_refusal(sys, pe, mn, sw, &d, 1);
    CHECK(r && !strcmp(r, "vm.usbMounted"), "a partition another block device holds: %s", r ? r : "(none)");
    char h[700];
    snprintf(h, sizeof h, "%s/" DEV "/1-3/1-3:1.0/host6/target6:0:0/6:0:0:0/block/sdz/sdz1/holders/dm-3", root);
    unlink(h);
    /* swap on a block device: /dev/loop0's numbers given to the disk, when this computer has it */
    struct stat st;
    if (!stat("/dev/loop0", &st) && S_ISBLK(st.st_mode)) {
        char v[32];
        snprintf(v, sizeof v, "%u:%u\n", major(st.st_rdev), minor(st.st_rdev));
        put(DEV "/1-3/1-3:1.0/host6/target6:0:0/6:0:0:0/block/sdz/dev", v);
        put("swaps-loop", "Filename\tType\tSize\tUsed\tPriority\n/dev/loop0  partition\t1\t0\t-2\n");
        char sl[512];
        p(sl, "swaps-loop");
        r = vmusb_refusal(sys, pe, mn, sl, &d, 1);
        CHECK(r && !strcmp(r, "vm.usbMounted"), "swap on it: %s", r ? r : "(none)");
        put(DEV "/1-3/1-3:1.0/host6/target6:0:0/6:0:0:0/block/sdz/dev", "250:0\n");
    }
    /* a node that isn't usb_device's */
    VmUsbDev fake = d;
    fake.major = 8;
    r = vmusb_refusal(sys, pe, mn, sw, &fake, 1);
    CHECK(r && !strcmp(r, "vm.usbGone"), "not a usb_device node");
}

static void test_parsers(void) {
    char id[37];
    int b, d;
    unsigned v, pr;
    const char *good = "b94f7394-9765-41da-8183-75a06c0d6f03 1 11 046d:0843";
    CHECK(!vmusb_req_parse(good, strlen(good), id, &b, &d, &v, &pr) && b == 1 && d == 11 && v == 0x046d && pr == 0x0843 && !strcmp(id, "b94f7394-9765-41da-8183-75a06c0d6f03"), "a good request");
    const char *bad[] = {
        "", " b94f7394-9765-41da-8183-75a06c0d6f03 1 11 046d:0843", "b94f7394-9765-41da-8183-75a06c0d6f03 1 11 046d:0843 ",
        "b94f7394-9765-41da-8183-75a06c0d6f03  1 11 046d:0843", "b94f7394-9765-41da-8183-75a06c0d6f03 0 11 046d:0843",
        "b94f7394-9765-41da-8183-75a06c0d6f03 1 0 046d:0843", "b94f7394-9765-41da-8183-75a06c0d6f03 1 128 046d:0843",
        "b94f7394-9765-41da-8183-75a06c0d6f03 256 1 046d:0843", "b94f7394-9765-41da-8183-75a06c0d6f03 01 11 046d:0843",
        "b94f7394-9765-41da-8183-75a06c0d6f03 -1 11 046d:0843", "b94f7394-9765-41da-8183-75a06c0d6f03 1 11 046D:0843",
        "b94f7394-9765-41da-8183-75a06c0d6f03 1 11 046d0843", "b94f7394-9765-41da-8183-75a06c0d6f03 1 11 046d:08430",
        "B94F7394-9765-41da-8183-75a06c0d6f03 1 11 046d:0843", "b94f7394-9765-41da-8183-75a06c0d6f0 1 11 046d:0843",
        "../../etc 1 11 046d:0843", "b94f7394-9765-41da-8183-75a06c0d6f03 1 11", "b94f7394-9765-41da-8183-75a06c0d6f03 1 11 046d:0843 x",
        "b94f7394-9765-41da-8183-75a06c0d6f03\t1 11 046d:0843", "b94f7394-9765-41da-8183-75a06c0d6f03 1 11 046d:0843\n",
        "b94f7394-9765-41da-8183-75a06c0d6f03 99999999999 11 046d:0843",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(vmusb_req_parse(bad[i], strlen(bad[i]), id, &b, &d, &v, &pr), "refused: [%s]", bad[i]);
    CHECK(vmusb_req_parse("b94f7394-9765-41da-8183-75a06c0d6f03 1 11 046d:0843\0x", 53, id, &b, &d, &v, &pr), "a NUL inside");
    VmUsbRule r;
    CHECK(!vmusb_rule_parse("046d:0843", &r) && r.vendor == 0x46d && r.product == 0x843 && !r.serial[0], "rule");
    CHECK(!vmusb_rule_parse("0781:5581:4C530001", &r) && !strcmp(r.serial, "4C530001"), "rule with serial");
    const char *badr[] = { "", "046d", "046d:084", "046d:0843:", "046d:0843:a b", "046d:0843:a/b", "046D:0843", "046d-0843", "046d:0843:" "0123456789012345678901234567890123456789012345678901234567890123" };
    for (size_t i = 0; i < sizeof badr / sizeof *badr; i++) CHECK(vmusb_rule_parse(badr[i], &r), "rule refused: [%s]", badr[i]);
    char sys[512];
    p(sys, DEV);
    VmUsbDev cam, disk;
    vmusb_find(sys, 1, 2, &cam);
    vmusb_find(sys, 1, 4, &disk);
    vmusb_rule_parse("046d:0843", &r);
    CHECK(vmusb_rule_match(&r, &cam) && !vmusb_rule_match(&r, &disk), "match by ids");
    vmusb_rule_parse("046d:0843:FA11927E", &r);
    CHECK(vmusb_rule_match(&r, &cam), "match with the serial");
    vmusb_rule_parse("046d:0843:OTHER", &r);
    CHECK(!vmusb_rule_match(&r, &cam), "another serial doesn't match");
    char s[80];
    vmusb_rule_parse("0781:5581:4C530001", &r);
    vmusb_rule_str(&r, s, sizeof s);
    CHECK(!strcmp(s, "0781:5581:4C530001"), "rule back to text: %s", s);
    CHECK(!vmusb_line_parse("1 11 046d:0843", &b, &d, &v, &pr) && b == 1 && d == 11 && v == 0x46d, "line");
    const char *badl[] = { "", "1 11", "1  11 046d:0843", "0 11 046d:0843", "1 11 046d:0843 ", "1 11 046d:0843\t2 3 1111:2222", "x 1 046d:0843" };
    for (size_t i = 0; i < sizeof badl / sizeof *badl; i++) CHECK(vmusb_line_parse(badl[i], &b, &d, &v, &pr), "line refused: [%s]", badl[i]);
    /* random input into each parser: no crash, no read past the end (ASan) */
    srand(7);
    char buf[96];
    for (int k = 0; k < 200000; k++) {
        size_t n = (size_t)(rand() % 90);
        for (size_t i = 0; i < n; i++) {
            int c = rand() % 6;
            buf[i] = c == 0 ? ' ' : c == 1 ? ':' : c == 2 ? (char)('0' + rand() % 10) : c == 3 ? (char)('a' + rand() % 6) : c == 4 ? '-' : (char)(rand() % 256);
        }
        char *heap = malloc(n ? n : 1);
        memcpy(heap, buf, n);
        if (!vmusb_req_parse(heap, n, id, &b, &d, &v, &pr)) CHECK(b >= 1 && b <= 255 && d >= 1 && d <= 127 && strlen(id) == 36, "random request accepted with bad values");
        free(heap);
        buf[n] = 0;
        if (!vmusb_rule_parse(buf, &r)) CHECK(strlen(buf) <= 73, "random rule");
        if (!vmusb_line_parse(buf, &b, &d, &v, &pr)) CHECK(b >= 1 && d >= 1 && d <= 127, "random line");
    }
}

static void test_fd(void) {
    int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    CHECK(vmusb_fd_ok(fd, 1, 4), "/dev/null isn't a USB device");
    close(fd);
    int pp[2];
    if (!pipe(pp)) { CHECK(vmusb_fd_ok(pp[0], 1, 4), "a pipe isn't one"); close(pp[0]); close(pp[1]); }
    CHECK(vmusb_fd_ok(-1, 1, 4), "no descriptor");
    /* this computer's own: a node opened read-only is refused (needs read and write), the wrong address too */
    char sysp[] = VMUSB_SYS;
    VmUsbDev d[VMUSB_MAX];
    int n = vmusb_list(sysp, d, VMUSB_MAX);
    for (int i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/bus/usb/%03d/%03d", d[i].bus, d[i].dev);
        int f = open(path, O_RDONLY | O_CLOEXEC);
        if (f < 0) continue;
        CHECK(vmusb_fd_ok(f, d[i].bus, d[i].dev), "read-only node refused");
        close(f);
        break;
    }
}

int main(void) {
    snprintf(root, sizeof root, "%s/vmusb-test.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    tree();
    test_list();
    test_refusals();
    test_parsers();
    test_fd();
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd)) {}
    if (fails) { fprintf(stderr, "vmusb-test: %d failed\n", fails); return 1; }
    printf("vmusb-test: ok\n");
    return 0;
}
