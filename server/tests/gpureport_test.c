/* gpureport_test.c — the graphics compatibility report (src/sysreport/gpureport.c): a fake machine with an Intel iGPU
 * and two NVIDIA cards (one on the proprietary driver drawing the desktop, one no driver took), full of things that must
 * never reach a report (serial numbers, a MAC, a disk's UUID, an address, the user's and the computer's names, a path in
 * the home); NVIDIA's version file in both kinds; the redaction on its own, its cut, and random bytes through it.
 *
 *   build/gpureport-test             runs the checks (built with ASan and UBSan in mk/sysreport.mk) */
#include "sysreport/gpureport.h"

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
static void parent(const char *p) {
    char d[1024];
    snprintf(d, sizeof d, "%s", p);
    char *sl = strrchr(d, '/');
    if (sl) { *sl = 0; mkdirs(d); }
}
static void putn(const char *root, const char *rel, const void *data, size_t len) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    parent(p);
    FILE *f = fopen(p, "wb");
    if (!f) { printf("cannot write %s: %s\n", p, strerror(errno)); exit(2); }
    fwrite(data, 1, len, f);
    fclose(f);
}
static void put(const char *root, const char *rel, const char *text) { putn(root, rel, text, strlen(text)); }
static void lnk(const char *root, const char *rel, const char *target) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    parent(p);
    unlink(p);
    if (symlink(target, p)) printf("symlink %s: %s\n", p, strerror(errno));
}
static void rmtree(const char *dir) {
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) {}
}
static int has(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }

static const char *PCI_IDS =
    "10de  NVIDIA Corporation\n"
    "\t2504  GA106 [GeForce RTX 3060 Lite Hash Rate]\n"
    "\t1c82  GP107 [GeForce GTX 1050 Ti]\n"
    "8086  Intel Corporation\n"
    "\t3e92  CoffeeLake-S GT2 [UHD Graphics 630]\n";

/* a PCI GPU: ids, class, driver (NULL none), a serial that must not come out */
static void pci(const char *r, const char *slot, const char *ven, const char *dev, const char *drv, int boot) {
    char d[256], f[400];
    snprintf(d, sizeof d, "sys/devices/pci0000:00/%s", slot);
    snprintf(f, sizeof f, "%s/vendor", d); put(r, f, ven);
    snprintf(f, sizeof f, "%s/device", d); put(r, f, dev);
    snprintf(f, sizeof f, "%s/class", d); put(r, f, "0x030000\n");
    snprintf(f, sizeof f, "%s/serial", d); put(r, f, "SECRET-SERIAL-1234\n");
    if (boot) { snprintf(f, sizeof f, "%s/boot_vga", d); put(r, f, "1\n"); }
    if (drv) {
        char t[256];
        snprintf(f, sizeof f, "%s/driver", d);
        snprintf(t, sizeof t, "../../../bus/pci/drivers/%s", drv);
        lnk(r, f, t);
    }
    char rel[256], t[256];
    snprintf(rel, sizeof rel, "sys/bus/pci/devices/%s", slot);
    snprintf(t, sizeof t, "../../../devices/pci0000:00/%s", slot);
    lnk(r, rel, t);
}
static void drm(const char *r, int n, const char *slot, int render) {
    char rel[256], t[256];
    snprintf(rel, sizeof rel, "sys/class/drm/card%d", n);
    snprintf(t, sizeof t, "../../devices/pci0000:00/%s/drm/card%d", slot, n);
    lnk(r, rel, t);
    snprintf(rel, sizeof rel, "sys/devices/pci0000:00/%s/drm/card%d/device", slot, n);
    { char t2[256]; snprintf(t2, sizeof t2, "../../../%s", slot); lnk(r, rel, t2); }
    snprintf(rel, sizeof rel, "sys/devices/pci0000:00/%s/drm/card%d/dev", slot, n);
    put(r, rel, "226:0\n");
    if (render) { snprintf(rel, sizeof rel, "sys/devices/pci0000:00/%s/drm/renderD%d/dev", slot, 128 + n); put(r, rel, "226:128\n"); }
}

static void fixture(const char *r) {
    put(r, "usr/share/misc/pci.ids", PCI_IDS);
    pci(r, "0000:00:02.0", "0x8086\n", "0x3e92\n", "i915", 0);
    pci(r, "0000:01:00.0", "0x10de\n", "0x2504\n", "nvidia", 1);
    pci(r, "0000:02:00.0", "0x10de\n", "0x1c82\n", NULL, 0);
    drm(r, 0, "0000:00:02.0", 1);
    drm(r, 1, "0000:01:00.0", 1);
    put(r, "proc/sys/kernel/osrelease", "6.12.57+deb13-amd64\n");
    put(r, "proc/cmdline", "BOOT_IMAGE=/boot/vmlinuz-6.12 root=UUID=0b1e6c1a-1111-2222-3333-444455556666 ro quiet nvidia-drm.modeset=1 "
                           "nouveau.modeset=0 ip=192.168.1.20 hostname=secrethost nvidia.NVreg_TemporaryFilePath=/home/secretuser/tmp nomodeset2 nomodeset\n");
    put(r, "proc/driver/nvidia/version", "NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  560.35.03  Release Build  (dvs-builder@U16-I3-B03-4-3)  Fri Aug 16 21:42:42 UTC 2024\n"
                                         "GCC version:  gcc version 12.2.0 (Debian 12.2.0-14)\n");
    mkdirs(r); { char p[1024]; snprintf(p, sizeof p, "%s/sys/module/nvidia", r); mkdirs(p); }
    put(r, "sys/module/nvidia_drm/parameters/modeset", "Y\n");
    put(r, "sys/module/nvidia_drm/parameters/fbdev", "N\n");
    put(r, "usr/bin/nvidia-smi", "");
    put(r, "dev/nvidia-uvm", "");
    put(r, "usr/lib/x86_64-linux-gnu/nvidia/current/libcuda.so.1", "");
    put(r, "usr/lib/x86_64-linux-gnu/libgallium-25.0.7-2.so", "");
    put(r, "usr/lib/x86_64-linux-gnu/dri/iHD_drv_video.so", "");
    put(r, "usr/lib/x86_64-linux-gnu/dri/nvidia_drv_video.so", "");
    put(r, "usr/lib/x86_64-linux-gnu/dri/evil;rm_drv_video.so", "");
    put(r, "usr/lib/x86_64-linux-gnu/vdpau/libvdpau_nvidia.so.1", "");
    put(r, "usr/share/vulkan/icd.d/nvidia_icd.json", "{\"file_format_version\":\"1.0.1\",\"ICD\":{\"library_path\":\"libGLX_nvidia.so.0\",\"api_version\":\"1.4.303\"}}");
    put(r, "usr/share/vulkan/icd.d/intel_icd.x86_64.json", "{\"ICD\":{\"library_path\":\"/home/secretuser/libvulkan_intel.so\",\"api_version\":\"1.4.305\"}}");
    put(r, "etc/OpenCL/vendors/nvidia.icd", "libnvidia-opencl.so.1\n");
    putn(r, "sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c", "\x06\x00\x00\x00\x01", 5);
    put(r, "sys/kernel/security/lockdown", "none [integrity] confidentiality\n");
    /* the user's lord-server holds card1; another process named otherwise holds card0 */
    put(r, "proc/4242/comm", "lord-server\n");
    lnk(r, "proc/4242/fd/7", "/dev/dri/card1");
    lnk(r, "proc/4242/fd/8", "/dev/dri/renderD129");
    put(r, "proc/5000/comm", "firefox-esr\n");
    lnk(r, "proc/5000/fd/3", "/dev/dri/card0");
}

static const char *const LEAKS[] = { "SECRET", "secretuser", "secrethost", "0b1e6c1a", "192.168", "/home/", "dvs-builder", "evil" };

static void test_collect(const char *root) {
    GpuReport *r = malloc(sizeof *r);
    gpureport_collect(r, root, NULL);
    CHECK(r->ncard == 3, "three GPUs (got %d)", r->ncard);
    if (r->ncard == 3) {
        CHECK(!strcmp(r->card[0].card, "card0") && !strcmp(r->card[0].driver, "i915") && !strcmp(r->card[0].slot, "0000:00:02.0"), "card0 is the iGPU on i915 (%s %s %s)", r->card[0].card, r->card[0].driver, r->card[0].slot);
        CHECK(!strcmp(r->card[0].device, "CoffeeLake-S GT2 [UHD Graphics 630]") && r->card[0].render_node, "card0's name and render node");
        CHECK(!strcmp(r->card[1].card, "card1") && !strcmp(r->card[1].driver, "nvidia") && r->card[1].boot_vga && !strcmp(gpureport_kind(&r->card[1]), "nvidia"), "card1 is NVIDIA's");
        CHECK(!r->card[2].card[0] && !r->card[2].driver[0] && !strcmp(r->card[2].slot, "0000:02:00.0") && !strcmp(r->card[2].device_id, "1c82"), "the card no driver took is listed from PCI");
        CHECK(r->card[0].desktop == -1, "desktop unknown before marking");
    }
    CHECK(!strcmp(r->kernel, "6.12.57+deb13-amd64"), "kernel (%s)", r->kernel);
    CHECK(!strcmp(r->firmware, "uefi") && r->secure_boot == 1 && !strcmp(r->lockdown, "integrity"), "firmware, Secure Boot, lockdown");
    CHECK(!strcmp(r->cmdline, "nvidia-drm.modeset=1 nouveau.modeset=0 nomodeset"), "only the graphics options of the command line (%s)", r->cmdline);
    CHECK(r->nv_loaded && !strcmp(r->nv_version, "560.35.03") && r->nv_open == 1, "NVIDIA's open modules 560.35.03");
    CHECK(r->nv_modeset == 1 && r->nv_fbdev == 0 && !r->nouveau_loaded, "modeset on, fbdev off, no nouveau");
    CHECK(r->cuda && r->nv_smi && r->nv_uvm, "CUDA's userspace in nvidia/current, nvidia-smi, nvidia-uvm");
    CHECK(!strcmp(r->mesa, "25.0.7-2"), "Mesa's version (%s)", r->mesa);
    int vk_nv = 0, vk_in = 0;
    for (int i = 0; i < r->nvulkan; i++) { vk_nv |= !strcmp(r->vulkan[i], "nvidia 1.4.303"); vk_in |= !strcmp(r->vulkan[i], "intel 1.4.305"); }
    CHECK(r->nvulkan == 2 && vk_nv && vk_in, "Vulkan ICDs by driver and version");
    CHECK(r->nva == 2 && r->nvdpau == 1 && !strcmp(r->vdpau[0], "nvidia") && r->opencl == 1, "VA-API (the bad name left out), VDPAU, OpenCL");

    CHECK(gpureport_mark_desktop(r, root, NULL) == 1, "the user's lord-server holds one card");
    CHECK(r->ncard == 3 && r->card[1].desktop == 1 && r->card[0].desktop == 0 && r->card[2].desktop == 0, "card1 draws the desktop, firefox's card0 doesn't count");

    r->session = 1;
    snprintf(r->composition, sizeof r->composition, "cpu");
    snprintf(r->gpu_init, sizeof r->gpu_init, "no-gbm");
    snprintf(r->lordos, sizeof r->lordos, "0.8.10-beta");
    char *text = gpureport_text(r), *json = gpureport_json(r);
    char *red = gpureport_redacted(text, "secretuser", "secrethost", "/home/secretuser");
    CHECK(text && json && red, "text and JSON made");
    CHECK(has(text, "GPU 2: NVIDIA Corporation GA106 [GeForce RTX 3060 Lite Hash Rate] [10de:2504]"), "the NVIDIA card's line");
    CHECK(has(text, "driver: nvidia 560.35.03 (open kernel modules)"), "its driver and version");
    CHECK(has(text, "draws the desktop: yes; boot display: yes; DRM node: card1"), "it draws the desktop");
    CHECK(has(text, "driver: i915 (Mesa 25.0.7-2)"), "the iGPU's Mesa");
    CHECK(has(text, "GPU 3: NVIDIA Corporation GP107 [GeForce GTX 1050 Ti] [10de:1c82]\n  driver: none"), "the card without a driver");
    CHECK(has(text, "nvidia-drm modeset: on; fbdev: off") && has(text, "CUDA: libcuda installed; nvidia-smi installed; /dev/nvidia-uvm present"), "NVIDIA's part");
    CHECK(has(text, "Secure Boot: on; lockdown: integrity"), "Secure Boot");
    CHECK(has(text, "composition: cpu; GPU start: no-gbm"), "the session's composition");
    CHECK(has(json, "\"kind\":\"nvidia\"") && has(json, "\"modeset\":1") && has(json, "\"desktop\":1") && has(json, "\"session\":{\"composition\":\"cpu\""), "JSON facts");
    for (size_t i = 0; i < sizeof LEAKS / sizeof *LEAKS; i++) {
        CHECK(!has(red, LEAKS[i]), "the report holds %s", LEAKS[i]);
        CHECK(!has(json, LEAKS[i]), "the JSON holds %s", LEAKS[i]);
    }
    CHECK(red && strlen(red) < GR_TEXT_MAX, "the report is short (%zu)", red ? strlen(red) : 0);
    free(text); free(json); free(red);

    /* outside a session */
    r->session = 0;
    text = gpureport_text(r);
    CHECK(has(text, "not known from the command line"), "the command line says what it can't know");
    free(text);
    /* the display engine (drmprobe.h): absent until the CLI probes it, then two lines per card and an object in the JSON */
    json = gpureport_json(r);
    CHECK(has(json, "\"desktop\":0,\"drm\":null}"), "not probed: drm null");
    free(json);
    DrmFacts *d = &r->card[1].drm;
    drmprobe_init(d);
    d->probed = d->card_node = d->render_node = 1;
    d->atomic = 1; d->planes = 4; d->primary = 1; d->overlay = 2; d->cursor = 1; d->scanout_formats = 20; d->scanout_modifiers = 3;
    d->syncobj = d->syncobj_timeline = 1; d->non_desktop = 1; d->connectors = 3;
    text = gpureport_text(r); json = gpureport_json(r);
    CHECK(has(text, "DRM node: card1; render node: yes\n  display engine: atomic yes; planes 4 (1 primary, 2 overlay, 1 cursor); scanout formats 20, modifiers 3"), "the engine's line");
    CHECK(has(text, "  explicit sync: syncobj yes, timeline yes; PRIME import unknown, export unknown; drm-lease: yes (1 non-desktop connector)\n"), "explicit sync and drm-lease");
    CHECK(has(json, "\"drm\":{\"cardNode\":true,\"renderNode\":true,\"atomic\":1,\"planes\":4,") && has(json, "\"nonDesktop\":1}"), "the engine in the JSON");
    CHECK(!has(text, "card0; render node: yes\n  display engine"), "an unprobed card has no engine line");
    free(text); free(json);
    free(r);
}

static void test_nv_version(void) {
    char v[24];
    int open_ = 7;
    CHECK(!gpureport_nv_version("NVRM version: NVIDIA UNIX x86_64 Kernel Module  535.247.01  Thu Apr 24 18:00:00 UTC 2025\n", v, sizeof v, &open_) && !strcmp(v, "535.247.01") && open_ == 0, "proprietary 535");
    CHECK(!gpureport_nv_version("NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  570.153.02  Release Build\n", v, sizeof v, &open_) && !strcmp(v, "570.153.02") && open_ == 1, "open 570");
    CHECK(gpureport_nv_version("NVRM version: something else\nKernel Module 1.2\n", v, sizeof v, &open_) && !v[0], "a Kernel Module on another line isn't read");
    CHECK(gpureport_nv_version("NVRM version: NVIDIA UNIX Kernel Module  abc  x\n", v, sizeof v, &open_) && !v[0], "no number, no version");
    CHECK(gpureport_nv_version("NVRM version: NVIDIA UNIX Kernel Module  1234567890123456789012345678.1\n", v, sizeof v, &open_) && !v[0], "too long a number");
    CHECK(gpureport_nv_version("", v, sizeof v, &open_) && gpureport_nv_version(NULL, v, sizeof v, &open_), "empty");
}

static void test_redact(void) {
    const char *in =
        "home /home/secretuser/.local/share/x and ~ ok\n"
        "other /home/bob/file and /home/ alone\n"
        "mac de:ad:be:ef:00:11 and DE-AD-BE-EF-00-11, not a mac 0000:01:00.0\n"
        "uuid 0b1e6c1a-1111-2222-3333-444455556666 end\n"
        "serial: ABC123 next, SerialNumber=XYZ9, Serial Number unknown\n"
        "mail secretuser@example.org and a.b@c.d\n"
        "user secretuser, host secrethost, secretuser2 stays, nosecretuser stays\n"
        "versions 560.35.03 and 10de:2504 stay\n";
    char *o = gpureport_redacted(in, "secretuser", "secrethost", "/home/secretuser/");
    CHECK(o != NULL, "redacted");
    if (!o) return;
    CHECK(has(o, "home ~/.local/share/x"), "the home becomes ~");
    CHECK(has(o, "other /home/<user>/file and /home/ alone"), "another home's name goes");
    CHECK(has(o, "mac <mac> and <mac>, not a mac 0000:01:00.0"), "MACs go, a PCI slot stays");
    CHECK(has(o, "uuid <uuid> end"), "a UUID goes");
    CHECK(has(o, "serial: <hidden> next, SerialNumber=<hidden>"), "serials go (%s)", o);
    CHECK(has(o, "mail <email> and <email>"), "e-mail addresses go");
    CHECK(has(o, "user <user>, host <host>, secretuser2 stays, nosecretuser stays"), "names only as whole words");
    CHECK(has(o, "versions 560.35.03 and 10de:2504 stay"), "versions and ids stay");
    CHECK(!has(o, "ABC123") && !has(o, "XYZ9") && !has(o, "example.org") && !has(o, "secrethost"), "nothing left");
    free(o);

    /* names that would wipe the report aren't used */
    o = gpureport_redacted("driver: none\nroot on localhost\nx\n", "root", "localhost", "/");
    CHECK(o && !strcmp(o, "driver: none\nroot on localhost\nx\n"), "root, localhost and / are left alone");
    free(o);
    o = gpureport_redacted("a b\n", "a", NULL, NULL);
    CHECK(o && !strcmp(o, "a b\n"), "a one-letter user is left alone");
    free(o);
    o = gpureport_redacted("", "u1", "h1", "/home/u1");
    CHECK(o && !*o, "empty text");
    free(o);

    /* the cut: on a line's end, marked */
    size_t big = GR_TEXT_MAX * 2;
    char *t = malloc(big + 1);
    for (size_t i = 0; i < big; i++) t[i] = i % 50 == 49 ? '\n' : 'x';
    t[big] = 0;
    o = gpureport_redacted(t, NULL, NULL, NULL);
    size_t ol = o ? strlen(o) : 0;
    CHECK(o && ol <= GR_TEXT_MAX && ol > GR_TEXT_MAX - 60 && !strcmp(o + ol - 6, "[cut]\n") && o[ol - 7] == '\n', "cut at GR_TEXT_MAX on a line's end (%zu)", ol);
    free(o);
    /* one long line */
    memset(t, 'y', big);
    o = gpureport_redacted(t, NULL, NULL, NULL);
    CHECK(o && strlen(o) <= GR_TEXT_MAX, "a single line is cut too");
    free(o);
    free(t);

    /* random bytes: no crash, no read past the end (ASan) */
    srand(1234);
    char buf[512];
    for (int k = 0; k < 20000; k++) {
        size_t n = (size_t)(rand() % 511);
        static const char alpha[] = "de:ad:BE-ef0123456789abcdef@./home/serialSerial= \n-xyzuser";
        for (size_t i = 0; i < n; i++) buf[i] = k % 3 ? alpha[rand() % (int)(sizeof alpha - 1)] : (char)(rand() % 255 + 1);
        buf[n] = 0;
        o = gpureport_redacted(buf, "user", "xyz", "/home/user");
        if (!o) { CHECK(0, "random input %d", k); break; }
        free(o);
    }
    CHECK(1, "random input");
}

int main(void) {
    char root[] = "/tmp/gpureport-test-XXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    fixture(root);
    test_collect(root);
    rmtree(root);
    test_nv_version();
    test_redact();
    printf("gpureport-test: %d ok, %d failed\n", n_ok, n_bad);
    return n_bad ? 1 : 0;
}
