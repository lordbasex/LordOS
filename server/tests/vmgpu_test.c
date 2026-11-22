/* vmgpu_test.c — Machines' graphics cards (src/vm/vmgpu.c, vmgpu_admin.c): the argument and file parsers, the GPU
 * list read from an invented sysfs, and the four admin actions (gpu-role, vm-gvt-create, vm-gvt-remove, gpu-min-freq)
 * run like the admin helper runs them but under a fake root folder (vmgpu_admin_root), never on this computer's /etc
 * or /sys. A child plays the kernel: when an mdev's create file gets a UUID it makes the device's folder. */
#include "admin.h"
#include "vm/vm_admin.h"
#include "vm/vmgpu.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern const char *vmgpu_admin_root, *vmgpu_admin_fd_dir;
extern int (*vmgpu_admin_initramfs)(void);
#include <sys/resource.h>
extern int (*vmgpu_admin_prlimit)(pid_t pid, const struct rlimit *rl);
static pid_t pl_pid;
static rlim_t pl_cur, pl_max;
static int fake_prlimit(pid_t pid, const struct rlimit *rl) { pl_pid = pid; pl_cur = rl->rlim_cur; pl_max = rl->rlim_max; return 0; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static char last[2048];
void admin_say(const char *kind, const char *fmt, ...) {
    char m[1800];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    snprintf(last, sizeof last, "%s %s", kind, m);
}
int admin_run(const char *const argv[]) { (void)argv; return 0; }

static int initramfs_runs;
static int fake_initramfs(void) { initramfs_runs++; return 0; }

static char root[512];

static void mk(const char *rel) {
    char p[1024], *s;
    snprintf(p, sizeof p, "%s/%s", root, rel);
    for (s = p + strlen(root) + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(p, 0755); *s = '/'; }
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
static void lnk(const char *rel, const char *target) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    unlink(p);
    if (symlink(target, p)) { perror(p); exit(1); }
}
static int exists(const char *rel) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    return !access(p, F_OK);
}
static void get(const char *rel, char *out, size_t n) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    out[0] = 0;
    FILE *f = fopen(p, "r");
    if (!f) return;
    size_t r = fread(out, 1, n - 1, f);
    out[r] = 0;
    fclose(f);
}

#define IGPU "sys/bus/pci/devices/0000:00:02.0"
#define AMD  "sys/bus/pci/devices/0000:01:00.0"
#define ID1 "908312a6-6bbe-4eaf-85f6-0c9531621bad"
#define ID2 "11111111-2222-4333-8444-555555555555"

static void fake_sys(void) {
    put(IGPU "/class", "0x038000\n"); put(IGPU "/vendor", "0x8086\n"); put(IGPU "/device", "0x9bc5\n");
    mk("sys/bus/pci/drivers/i915"); lnk(IGPU "/driver", "../../../bus/pci/drivers/i915");
    mk("sys/kernel/iommu_groups/0"); lnk(IGPU "/iommu_group", "../../../kernel/iommu_groups/0");
    mk(IGPU "/drm/card0"); mk(IGPU "/drm/renderD128");
    put(IGPU "/drm/card0/gt/gt0/rps_RPn_freq_mhz", "350\n"); put(IGPU "/drm/card0/gt/gt0/rps_RP0_freq_mhz", "1200\n");
    put(IGPU "/drm/card0/gt/gt0/rps_min_freq_mhz", "350\n");
    put(IGPU "/mdev_supported_types/i915-GVTg_V5_4/description", "low_gm_size: 128MB\nhigh_gm_size: 512MB\nfence: 4\nresolution: 1920x1200\nweight: 4\n");
    put(IGPU "/mdev_supported_types/i915-GVTg_V5_4/available_instances", "1\n");
    put(IGPU "/mdev_supported_types/i915-GVTg_V5_4/create", "");
    put(IGPU "/mdev_supported_types/i915-GVTg_V5_8/description", "low_gm_size: 64MB\nhigh_gm_size: 384MB\nfence: 4\nresolution: 1024x768\nweight: 2\n");
    put(IGPU "/mdev_supported_types/i915-GVTg_V5_8/available_instances", "2\n");
    put(IGPU "/mdev_supported_types/i915-GVTg_V5_8/create", "");
    put(AMD "/class", "0x030000\n"); put(AMD "/vendor", "0x1002\n"); put(AMD "/device", "0x67df\n"); put(AMD "/boot_vga", "1\n");
    mk("sys/bus/pci/drivers/amdgpu"); lnk(AMD "/driver", "../../../bus/pci/drivers/amdgpu");
    mk(AMD "/drm/card1"); mk(AMD "/drm/renderD129");
    put("sys/bus/pci/devices/0000:00:1f.3/class", "0x040300\n");      /* sound: not a GPU */
    mk("etc/modprobe.d"); mk("etc/modules-load.d"); mk("proc"); mk("sys/bus/mdev/devices"); mk("dev/vfio");
    mk("fd"); mk("run");
    lnk("fd/5", "/dev/dri/card1");                                      /* lord-server holds the AMD card */
}

/* the kernel's part: an mdev's folder appears when its create file gets a UUID */
static pid_t kernel_pid;
static void kernel_start(void) {
    kernel_pid = fork();
    if (kernel_pid) return;
    for (int i = 0; i < 1000; i++) {
        const char *types[] = { IGPU "/mdev_supported_types/i915-GVTg_V5_4/create", IGPU "/mdev_supported_types/i915-GVTg_V5_8/create" };
        for (int t = 0; t < 2; t++) {
            char id[64];
            get(types[t], id, sizeof id);
            id[strcspn(id, "\n")] = 0;
            if (strlen(id) != 36) continue;
            char rel[256];
            snprintf(rel, sizeof rel, "sys/bus/mdev/devices/%s", id);
            if (exists(rel)) continue;
            mk("sys/kernel/iommu_groups/16");
            char p[300];
            snprintf(p, sizeof p, "%s/remove", rel);
            put(p, "");
            put("sys/class/vfio/16/dev", "237:0\n");
            put("dev/vfio/16", "");
            snprintf(p, sizeof p, "%s/iommu_group", rel);
            lnk(p, "../../../../kernel/iommu_groups/16");
        }
        struct timespec ts = { 0, 5000000 };
        nanosleep(&ts, NULL);
    }
    _exit(0);
}

static void test_parsers(void) {
    CHECK(vmgpu_addr_valid("0000:00:02.0"), "addr");
    CHECK(!vmgpu_addr_valid("0000:00:02.8") && !vmgpu_addr_valid("0000:00:02") && !vmgpu_addr_valid("0000:0G:02.0") &&
          !vmgpu_addr_valid("0000:00:02.0 ") && !vmgpu_addr_valid("../00:02.0x") && !vmgpu_addr_valid("0000:00:02.0/"), "bad addr");
    CHECK(vmgpu_type_valid("i915-GVTg_V5_4") && !vmgpu_type_valid("..") && !vmgpu_type_valid("a/b") && !vmgpu_type_valid("") &&
          !vmgpu_type_valid("a b") && !vmgpu_type_valid("01234567890123456789012345678901234567890"), "type names");
    CHECK(vmgpu_gvt_device(0x8086, 0x9bc5) && vmgpu_gvt_device(0x8086, 0x3e92) && vmgpu_gvt_device(0x8086, 0x5912) &&
          vmgpu_gvt_device(0x8086, 0x1912) && vmgpu_gvt_device(0x8086, 0x1616) && vmgpu_gvt_device(0x8086, 0x87ca), "gvt families");
    CHECK(!vmgpu_gvt_device(0x8086, 0x9a49) && !vmgpu_gvt_device(0x8086, 0x8a52) && !vmgpu_gvt_device(0x1002, 0x9bc5) &&
          !vmgpu_gvt_device(0x8086, 0x46a6) && !vmgpu_gvt_device(0x8086, 0x3184), "not gvt (Tiger Lake, Ice Lake, AMD, Alder Lake, Gemini Lake)");
    char a[16], id[37], ty[VMGPU_TYPE_MAX + 1];
    int role = -1, mhz = -1;
    CHECK(!vmgpu_role_arg("0000:00:02.0 machines-shared", 28, a, &role) && role == VMGPU_ROLE_SHARED && !strcmp(a, "0000:00:02.0"), "role shared");
    CHECK(!vmgpu_role_arg("0000:00:02.0 unused", 19, a, &role) && role == VMGPU_ROLE_UNUSED, "role unused");
    CHECK(!vmgpu_role_arg("0000:00:02.0 ai", 15, a, &role) && role == VMGPU_ROLE_AI, "role ai");
    const char *bad_roles[] = { "0000:00:02.0  unused", "0000:00:02.0 unused ", "0000:00:02.0 Unused", "0000:00:02.0", "0000:00:02.0 unused x",
                                "0000:00:02.0\tunused", "../../x unused", "0000:00:02.0 options i915 x=1", "" };
    for (size_t i = 0; i < sizeof bad_roles / sizeof *bad_roles; i++) CHECK(vmgpu_role_arg(bad_roles[i], strlen(bad_roles[i]), a, &role), "bad role %zu", i);
    CHECK(vmgpu_role_arg("0000:00:02.0 unused\0x", 21, a, &role), "nul in role");
    CHECK(!vmgpu_create_arg(ID1 " i915-GVTg_V5_4", 51, id, ty) && !strcmp(id, ID1) && !strcmp(ty, "i915-GVTg_V5_4"), "create arg");
    const char *bad_create[] = { ID1, ID1 " ../x", ID1 " a/b", "908312A6-6bbe-4eaf-85f6-0c9531621bad i915-GVTg_V5_4", ID1 "  i915-GVTg_V5_4",
                                 "908312a6-6bbe-4eaf-85f6-0c9531621ba i915-GVTg_V5_4", ID1 " i915-GVTg_V5_4 x" };
    for (size_t i = 0; i < sizeof bad_create / sizeof *bad_create; i++) CHECK(vmgpu_create_arg(bad_create[i], strlen(bad_create[i]), id, ty), "bad create %zu", i);
    CHECK(!vmgpu_remove_arg(ID1, 36, id) && vmgpu_remove_arg(ID1 " x", 38, id) && vmgpu_remove_arg("x", 1, id), "remove arg");
    CHECK(!vmgpu_freq_arg("0000:00:02.0 800", 16, a, &mhz) && mhz == 800, "freq 800");
    CHECK(!vmgpu_freq_arg("0000:00:02.0 default", 20, a, &mhz) && mhz == 0, "freq default");
    const char *bad_freq[] = { "0000:00:02.0 0", "0000:00:02.0 -5", "0000:00:02.0 0800", "0000:00:02.0 10001", "0000:00:02.0 800MHz", "0000:00:02.0 123456", "0000:00:02.0" };
    for (size_t i = 0; i < sizeof bad_freq / sizeof *bad_freq; i++) CHECK(vmgpu_freq_arg(bad_freq[i], strlen(bad_freq[i]), a, &mhz), "bad freq %zu", i);

    VmGpuType t = { 0 };
    CHECK(!vmgpu_type_desc("low_gm_size: 128MB\nhigh_gm_size: 512MB\nfence: 4\nresolution: 1920x1200\nweight: 4\n", &t) &&
          t.low_mb == 128 && t.high_mb == 512 && t.fence == 4 && t.max_w == 1920 && t.max_h == 1200 && t.weight == 4, "type desc");
    VmGpuType u = { 0 };
    CHECK(vmgpu_type_desc("low_gm_size: 128MB\n", &u), "no resolution");
    CHECK(vmgpu_type_desc("resolution: -1x5\n", &u) && vmgpu_type_desc("resolution: 99999x5\n", &u), "bad resolution");

    CHECK(vmgpu_conf_ours("options i915 enable_gvt=1 enable_guc=0\n", 0), "hand-written modprobe");
    CHECK(vmgpu_conf_ours("# x\n\noptions  i915 enable_guc=0   enable_gvt=1\n", 0), "options in another order, comments");
    CHECK(vmgpu_conf_ours(VMGPU_MODPROBE_TEXT, 0) && vmgpu_conf_ours(VMGPU_MODLOAD_TEXT, 1) && vmgpu_conf_ours("kvmgt\n", 1), "ours");
    CHECK(!vmgpu_conf_ours("options i915 enable_gvt=1\n", 0) && !vmgpu_conf_ours("options i915 enable_gvt=1 enable_guc=0\nblacklist nouveau\n", 0) &&
          !vmgpu_conf_ours("kvmgt\nvfio\n", 1) && !vmgpu_conf_ours("", 1) && !vmgpu_conf_ours("options i915 enable_gvt=1 enable_guc=0 enable_fbc=0\n", 0), "not ours");
}

static void test_list(void) {
    VmGpu g[VMGPU_MAX];
    int n = vmgpu_list(root, g, VMGPU_MAX);
    CHECK(n == 2, "two GPUs, got %d", n);
    if (n != 2) return;
    CHECK(!strcmp(g[0].addr, "0000:00:02.0") && g[0].vendor == 0x8086 && g[0].device == 0x9bc5 && !strcmp(g[0].driver, "i915") &&
          g[0].iommu_group == 0 && g[0].card == 0 && g[0].render == 128 && g[0].gvt_capable && g[0].gvt_on && g[0].n_types == 2, "iGPU");
    CHECK(!strcmp(g[0].types[0].name, "i915-GVTg_V5_4") && g[0].types[0].available == 1 && !strcmp(g[0].types[1].name, "i915-GVTg_V5_8"), "types largest first");
    CHECK(!strcmp(g[1].driver, "amdgpu") && g[1].boot_vga && !g[1].gvt_capable && g[1].card == 1, "AMD");
    char fd[600];
    snprintf(fd, sizeof fd, "%s/fd", root);
    CHECK(vmgpu_mark_desktop(root, fd, g, n) == 1 && g[1].desktop && !g[0].desktop, "the desktop's is the held card");
    CHECK(vmgpu_gvt(g, n) == &g[0], "the GVT iGPU");
    CHECK(vmgpu_pick_type(&g[0], 1, 0, 0, 0) == 0, "whole: V5_4");
    CHECK(vmgpu_pick_type(&g[0], 0, 1024, 768, 0) == 1, "shared 1024x768: V5_8");
    CHECK(vmgpu_pick_type(&g[0], 0, 1920, 1080, 0) == 0, "shared 1080p: V5_4");
    CHECK(vmgpu_pick_type(&g[0], 0, 2560, 1440, 0) == -1, "nothing shows 1440p");
    /* nobody holds a card: the firmware's primary display */
    CHECK(vmgpu_mark_desktop(root, "/nonexistent", g, n) == 1 && g[1].desktop, "boot_vga fallback");
    /* the iGPU as the desktop's: no GVT GPU to offer */
    lnk("fd/5", "/dev/dri/card0");
    vmgpu_mark_desktop(root, fd, g, n);
    CHECK(g[0].desktop && !vmgpu_gvt(g, n), "the desktop's iGPU isn't offered");
    lnk("fd/5", "/dev/dri/card1");
    vmgpu_mark_desktop(root, fd, g, n);
    char *j = vmgpu_json(g, n, VMGPU_ROLE_UNUSED);
    CHECK(j && strstr(j, "\"address\":\"0000:00:02.0\"") && strstr(j, "\"render\":\"/dev/dri/renderD128\"") && strstr(j, "\"maxW\":1920") &&
          strstr(j, "\"configured\":\"unused\"") && strstr(j, "\"restart\":true"), "json: %s", j ? j : "(null)");
    free(j);
}

static int ok(const char *code) { return !strncmp(last, "OK ", 3) && strstr(last, code); }
static int err(const char *code) { return !strncmp(last, "ERR ", 4) && strstr(last, code); }
#define ACT(fn, s) do { last[0] = 0; fn(&req, s, strlen(s)); } while (0)

static void test_actions(void) {
    char fd[600];
    snprintf(fd, sizeof fd, "%s/fd", root);
    vmgpu_admin_root = root;
    vmgpu_admin_fd_dir = fd;
    vmgpu_admin_initramfs = fake_initramfs;
    AdminReq req = { .id = 1, .user = "t", .home = "/tmp", .uid = getuid(), .gid = getgid(), .admin = 1 };
    AdminReq other = req;
    other.uid = req.uid + 1;
    char t[1024];

    /* gpu-role */
    ACT(vm_act_gpu_role, "0000:01:00.0 machines-shared");
    CHECK(err("gpu.desktop") && !exists("etc/modprobe.d/lordos-gpu.conf"), "desktop refused: %s", last);
    ACT(vm_act_gpu_role, "0000:00:09.0 machines-shared");
    CHECK(err("gpu.notFound"), "unknown address: %s", last);
    ACT(vm_act_gpu_role, "0000:00:02.0 machines-dedicated");
    CHECK(err("gpu.roleNotYet"), "dedicated not yet: %s", last);
    ACT(vm_act_gpu_role, "0000:00:02.0 options");
    CHECK(err("admin.invalidArgument"), "bad role: %s", last);
    /* the maintainer's hand-written files are adopted */
    put("etc/modprobe.d/i915-gvt.conf", "options i915 enable_gvt=1 enable_guc=0\n");
    put("etc/modules-load.d/kvmgt.conf", "kvmgt\n");
    put("etc/modprobe.d/other.conf", "options snd_hda_intel power_save=0\n");
    initramfs_runs = 0;
    ACT(vm_act_gpu_role, "0000:00:02.0 machines-shared");
    get("etc/modprobe.d/lordos-gpu.conf", t, sizeof t);
    CHECK(ok("gpu.roleSet") && !strcmp(t, VMGPU_MODPROBE_TEXT) && exists("etc/modules-load.d/lordos-gpu.conf") && initramfs_runs == 1, "shared written: %s", last);
    CHECK(!exists("etc/modprobe.d/i915-gvt.conf") && !exists("etc/modules-load.d/kvmgt.conf") && exists("etc/modprobe.d/other.conf"), "hand-written adopted, others kept");
    CHECK(vmgpu_configured(root) == VMGPU_ROLE_SHARED, "configured");
    ACT(vm_act_gpu_role, "0000:00:02.0 machines-shared");
    CHECK(ok("gpu.roleSet") && initramfs_runs == 1, "again: nothing changed, no initramfs");
    ACT(vm_act_gpu_role, "0000:00:02.0 unused");
    CHECK(ok("gpu.roleSet") && !exists("etc/modprobe.d/lordos-gpu.conf") && !exists("etc/modules-load.d/lordos-gpu.conf") && initramfs_runs == 2, "unused removes");
    CHECK(vmgpu_configured(root) == VMGPU_ROLE_UNUSED && exists("etc/modprobe.d/other.conf"), "unused");
    /* hand-written files that say more than ours are left alone */
    put("etc/modprobe.d/i915-gvt.conf", "options i915 enable_gvt=1 enable_guc=0 enable_fbc=1\n");
    put("etc/modules-load.d/kvmgt.conf", "kvmgt\n");
    ACT(vm_act_gpu_role, "0000:00:02.0 unused");
    CHECK(ok("gpu.roleSet") && exists("etc/modprobe.d/i915-gvt.conf") && !exists("etc/modules-load.d/kvmgt.conf"), "unused removes only what matches");
    unlink((snprintf(t, sizeof t, "%s/etc/modprobe.d/i915-gvt.conf", root), t));
    /* a link where our file goes is replaced, never followed */
    char victim[600], lp[600];
    snprintf(victim, sizeof victim, "%s/victim", root);
    put("victim", "keep\n");
    snprintf(lp, sizeof lp, "%s/etc/modprobe.d/lordos-gpu.conf", root);
    if (symlink(victim, lp)) perror("symlink");
    ACT(vm_act_gpu_role, "0000:00:02.0 machines-shared");
    get("victim", t, sizeof t);
    CHECK(ok("gpu.roleSet") && !strcmp(t, "keep\n"), "symlink not followed");
    ACT(vm_act_gpu_role, "0000:00:02.0 unused");

    /* vm-gvt-create */
    kernel_start();
    ACT(vm_act_gvt_create, ID1 " i915-GVTg_V5_9");
    CHECK(err("vm.gvtNoType"), "unknown type: %s", last);
    ACT(vm_act_gvt_create, ID1 " ../../x");
    CHECK(err("admin.invalidArgument"), "bad type: %s", last);
    put(IGPU "/mdev_supported_types/i915-GVTg_V5_4/available_instances", "0\n");
    ACT(vm_act_gvt_create, ID1 " i915-GVTg_V5_4");
    CHECK(err("vm.gvtNoType"), "none available: %s", last);
    put(IGPU "/mdev_supported_types/i915-GVTg_V5_4/available_instances", "1\n");
    ACT(vm_act_gvt_create, ID1 " i915-GVTg_V5_4");
    get(IGPU "/mdev_supported_types/i915-GVTg_V5_4/create", t, sizeof t);
    CHECK(ok("vm.gvtReady") && strstr(last, "name=16") && !strcmp(t, ID1), "created: %s", last);
    struct stat st;
    snprintf(t, sizeof t, "%s/dev/vfio/16", root);
    CHECK(!stat(t, &st) && (st.st_mode & 0777) == 0600 && st.st_uid == req.uid, "group given 0600");
    get("run/lordos/vm-gvt/" ID1, t, sizeof t);
    CHECK((unsigned)atoi(t) == (unsigned)req.uid, "noted: %s", t);
    ACT(vm_act_gvt_create, ID1 " i915-GVTg_V5_4");
    CHECK(ok("vm.gvtReady"), "same caller again: %s", last);
    last[0] = 0; vm_act_gvt_create(&other, ID1 " i915-GVTg_V5_4", strlen(ID1 " i915-GVTg_V5_4"));
    CHECK(err("vm.gvtBusy"), "another caller: %s", last);
    /* the desktop on the iGPU: refused */
    lnk("fd/5", "/dev/dri/card0");
    ACT(vm_act_gvt_create, ID2 " i915-GVTg_V5_8");
    CHECK(err("vm.gvtNone"), "desktop's iGPU: %s", last);
    ACT(vm_act_gpu_min_freq, "0000:00:02.0 800");
    CHECK(err("gpu.desktop"), "freq on the desktop's: %s", last);
    lnk("fd/5", "/dev/dri/card1");

    /* gpu-min-freq */
    ACT(vm_act_gpu_min_freq, "0000:00:02.0 2000");
    CHECK(err("gpu.freqRange"), "above RP0: %s", last);
    ACT(vm_act_gpu_min_freq, "0000:00:02.0 100");
    CHECK(err("gpu.freqRange"), "below RPn: %s", last);
    last[0] = 0; vm_act_gpu_min_freq(&other, "0000:00:02.0 800", 16);
    CHECK(err("vm.gvtNone"), "someone with no vGPU: %s", last);
    ACT(vm_act_gpu_min_freq, "0000:01:00.0 800");
    CHECK(err("gpu.desktop"), "AMD: %s", last);
    ACT(vm_act_gpu_min_freq, "0000:00:02.0 800");
    get(IGPU "/drm/card0/gt/gt0/rps_min_freq_mhz", t, sizeof t);
    CHECK(ok("gpu.freqSet") && atoi(t) == 800, "set 800: %s %s", last, t);
    ACT(vm_act_gpu_min_freq, "0000:00:02.0 900");
    get("run/lordos/vm-gvt/minfreq-0000:00:02.0", t, sizeof t);
    CHECK(ok("gpu.freqSet") && atoi(t) == 350, "the first value kept: %s", t);
    ACT(vm_act_gpu_min_freq, "0000:00:02.0 default");
    get(IGPU "/drm/card0/gt/gt0/rps_min_freq_mhz", t, sizeof t);
    CHECK(ok("gpu.freqSet") && atoi(t) == 900, "default waits while a vGPU is there: %s", t);

    /* vm-gvt-remove */
    put("proc/4242/fd/.keep", "");
    lnk("proc/4242/fd/9", "/dev/vfio/16");
    ACT(vm_act_gvt_remove, ID1);
    CHECK(err("vm.gvtInUse"), "held: %s", last);
    snprintf(t, sizeof t, "%s/proc/4242/fd/9", root);
    unlink(t);
    last[0] = 0; vm_act_gvt_remove(&other, ID1, 36);
    CHECK(err("vm.gvtBusy"), "another's: %s", last);
    ACT(vm_act_gvt_remove, ID1);
    get("sys/bus/mdev/devices/" ID1 "/remove", t, sizeof t);
    CHECK(ok("vm.gvtGone") && !strcmp(t, "1") && !exists("run/lordos/vm-gvt/" ID1), "removed: %s", last);
    /* the kernel removes the folder */
    snprintf(t, sizeof t, "%s/sys/bus/mdev/devices/" ID1 "/iommu_group", root);
    unlink(t);
    ACT(vm_act_gvt_remove, ID1);
    CHECK(ok("vm.gvtGone"), "gone already: %s", last);
    ACT(vm_act_gpu_min_freq, "0000:00:02.0 default");
    get(IGPU "/drm/card0/gt/gt0/rps_min_freq_mhz", t, sizeof t);
    CHECK(ok("gpu.freqSet") && atoi(t) == 350 && !exists("run/lordos/vm-gvt/minfreq-0000:00:02.0"), "default restores: %s", t);

    /* the sweep: a leftover vGPU nobody holds goes, and the frequency comes back */
    snprintf(t, sizeof t, "%s/sys/bus/mdev/devices/" ID1, root);
    char rm[1100];
    snprintf(rm, sizeof rm, "%s/remove", t);
    unlink(rm);
    rmdir(t);
    put(IGPU "/mdev_supported_types/i915-GVTg_V5_4/create", "");
    ACT(vm_act_gvt_create, ID2 " i915-GVTg_V5_4");
    CHECK(ok("vm.gvtReady"), "second: %s", last);
    ACT(vm_act_gpu_min_freq, "0000:00:02.0 700");
    vm_gvt_sweep(req.uid);
    get("sys/bus/mdev/devices/" ID2 "/remove", t, sizeof t);
    CHECK(!strcmp(t, "1") && !exists("run/lordos/vm-gvt/" ID2), "swept");
    kill(kernel_pid, SIGKILL);
    waitpid(kernel_pid, NULL, 0);
}

static void test_memlock(void) {
    AdminReq req = { .id = 1, .user = "t", .home = "/tmp", .uid = getuid(), .gid = getgid(), .admin = 1 };
    AdminReq other = req;
    other.uid = req.uid + 1;
    vmgpu_admin_prlimit = fake_prlimit;
    char t[1024];
    /* the default cap: total - max(4 GB, 25 %), down to a whole GB */
    CHECK(vmgpu_memlock_default_mb(32768) == 24576 && vmgpu_memlock_default_mb(16384) == 12288 && vmgpu_memlock_default_mb(8192) == 4096 &&
          vmgpu_memlock_default_mb(6000) == 2048 && vmgpu_memlock_default_mb(65536 + 700) == 49152, "default caps");
    long mb;
    int pid;
    CHECK(!vmgpu_memlock_arg("8192", 4, &mb) && mb == 8192 && !vmgpu_memlock_arg("default", 7, &mb) && mb == 0, "cap args");
    CHECK(vmgpu_memlock_arg("08192", 5, &mb) && vmgpu_memlock_arg("-1", 2, &mb) && vmgpu_memlock_arg("8 GB", 4, &mb) && vmgpu_memlock_arg("", 0, &mb), "bad cap args");
    CHECK(!vmgpu_pid_arg("4242", 4, &pid) && pid == 4242 && vmgpu_pid_arg("1", 1, &pid) && vmgpu_pid_arg("0", 1, &pid) && vmgpu_pid_arg("12 3", 4, &pid) &&
          vmgpu_pid_arg("-5", 2, &pid) && vmgpu_pid_arg("1234567890", 10, &pid), "pid args");
    /* gpu-memlock-cap, 32 GB of memory */
    put("proc/meminfo", "MemTotal:       32768000 kB\nMemFree: 1 kB\n");
    long total = vmgpu_total_mb(root);
    CHECK(total == 32000, "total %ld", total);
    int custom = 1;
    CHECK(vmgpu_memlock_cap_mb(root, total, &custom) == vmgpu_memlock_default_mb(total) && !custom, "no file: default");
    ACT(vm_act_gpu_memlock_cap, "1024");
    CHECK(err("gpu.memlockRange"), "below 2 GB: %s", last);
    ACT(vm_act_gpu_memlock_cap, "31000");
    CHECK(err("gpu.memlockRange"), "above total - 2 GB: %s", last);
    ACT(vm_act_gpu_memlock_cap, "8192");
    get("etc/lordos/gpu-memlock.conf", t, sizeof t);
    CHECK(ok("gpu.memlockSet") && !strcmp(t, "cap_mb=8192\n") && vmgpu_memlock_cap_mb(root, total, &custom) == 8192 && custom, "set 8 GB: %s", t);
    char *j = vmgpu_json_full(root, NULL, 0, VMGPU_ROLE_UNUSED);
    CHECK(j && strstr(j, "\"memlock\":{\"capMb\":8192,") && strstr(j, "\"custom\":true}}") && strstr(j, "\"totalMb\":32000"), "json: %s", j ? j : "");
    free(j);
    put("etc/lordos/gpu-memlock.conf", "cap_mb=99999\n");
    CHECK(vmgpu_memlock_cap_mb(root, total, &custom) == vmgpu_memlock_default_mb(total) && !custom, "a value out of range counts as the default");
    ACT(vm_act_gpu_memlock_cap, "default");
    CHECK(ok("gpu.memlockSet") && !exists("etc/lordos/gpu-memlock.conf"), "default removes");
    ACT(vm_act_gpu_memlock_cap, "8192");
    /* vm-memlock: real children stand for QEMUs (their pidfds), their /proc under the fake root */
    char st[256], rel[160];
    snprintf(st, sizeof st, "Name:\tlord-vm\nPPid:\t1\nUid:\t%u\t%u\t%u\t%u\n", (unsigned)req.uid, (unsigned)req.uid, (unsigned)req.uid, (unsigned)req.uid);
    put("proc/5151/status", st);
    lnk("proc/5151/exe", "/usr/lib/lordos/bin/lord-vm");
    mk("sys/kernel/iommu_groups/16");
    mk("sys/kernel/iommu_groups/17");
    mk("sys/bus/mdev/devices/" ID1);
    lnk("sys/bus/mdev/devices/" ID1 "/iommu_group", "../../../../kernel/iommu_groups/16");
    mk("sys/bus/mdev/devices/" ID2);
    lnk("sys/bus/mdev/devices/" ID2 "/iommu_group", "../../../../kernel/iommu_groups/17");
    snprintf(st, sizeof st, "%u\n", (unsigned)req.uid);
    put("run/lordos/vm-gvt/" ID1, st);
    put("run/lordos/vm-gvt/" ID2, st);
    pid_t q = fork();
    if (!q) { pause(); _exit(0); }
    pid_t q2 = fork();
    if (!q2) { pause(); _exit(0); }
    /* a fake QEMU: status, exe, stat (start time), cmdline (machine id, memory) and the fds it holds */
    #define FAKE_QEMU(P, ID, MEM, GRP, START) do { \
        char s_[256], r_[160]; \
        snprintf(s_, sizeof s_, "Name:\tqemu\nPPid:\t5151\nUid:\t%u\t%u\t%u\t%u\n", (unsigned)req.uid, (unsigned)req.uid, (unsigned)req.uid, (unsigned)req.uid); \
        snprintf(r_, sizeof r_, "proc/%d/status", (int)(P)); put(r_, s_); \
        snprintf(r_, sizeof r_, "proc/%d/exe", (int)(P)); lnk(r_, "/usr/bin/qemu-system-x86_64"); \
        snprintf(s_, sizeof s_, "%d (qemu-system-x86) S 5151 1 1 0 -1 0 0 0 0 0 0 0 0 0 20 0 9 0 %llu 0 0\n", (int)(P), (unsigned long long)(START)); \
        snprintf(r_, sizeof r_, "proc/%d/stat", (int)(P)); put(r_, s_); \
        char c_[400]; \
        int l_ = snprintf(c_, sizeof c_, "/usr/bin/qemu-system-x86_64%c-uuid%c%s%c-m%c%d%c-device%cvfio-pci-nohotplug,id=video0,sysfsdev=/sys/bus/mdev/devices/%s,display=on%c", \
                          0, 0, ID, 0, 0, MEM, 0, 0, ID, 0); \
        char cp_[1100]; snprintf(cp_, sizeof cp_, "%s/proc/%d/cmdline", root, (int)(P)); \
        FILE *f_ = fopen(cp_, "w"); if (f_) { fwrite(c_, 1, (size_t)l_, f_); fclose(f_); } \
        snprintf(r_, sizeof r_, "proc/%d/fd/.keep", (int)(P)); put(r_, ""); \
        snprintf(r_, sizeof r_, "proc/%d/fd/9", (int)(P)); \
        if (GRP >= 0) { char g_[32]; snprintf(g_, sizeof g_, "/dev/vfio/%d", GRP); lnk(r_, g_); } else { char u_[1100]; snprintf(u_, sizeof u_, "%s/%s", root, r_); unlink(u_); } \
    } while (0)
    FAKE_QEMU(q, ID1, 4096, 16, 1000);
    char a[16], a2[16];
    snprintf(a, sizeof a, "%d", (int)q);
    snprintf(a2, sizeof a2, "%d", (int)q2);
    pl_pid = 0;
    ACT(vm_act_memlock, a);
    CHECK(ok("vm.memlockSet") && pl_pid == q && pl_cur == (rlim_t)4608 << 20 && pl_max == pl_cur, "set 4096 + 512 MB: %s", last);
    char t2[128];
    snprintf(t2, sizeof t2, "%u %d 1000 4608", (unsigned)req.uid, (int)q);
    get("run/lordos/vm-gvt/" ID1, t, sizeof t);
    CHECK(!strncmp(t, t2, strlen(t2)), "the grant noted: %s", t);
    /* the same process again: nothing new */
    pl_pid = 0;
    ACT(vm_act_memlock, a);
    CHECK(ok("vm.memlockSet") && !pl_pid, "again: %s", last);
    /* a second QEMU for the same vGPU while the first lives: refused */
    FAKE_QEMU(q2, ID1, 2048, 16, 2000);
    ACT(vm_act_memlock, a2);
    CHECK(err("vm.memlockRefused") && !pl_pid, "second grant on one vGPU: %s", last);
    /* the sum over every vGPU: 4608 granted, cap 8192, another 4096 + 512 doesn't fit */
    FAKE_QEMU(q2, ID2, 4096, 17, 2000);
    ACT(vm_act_memlock, a2);
    CHECK(err("vm.memlockCap") && strstr(last, "need=4608") && strstr(last, "cap=3584") && !pl_pid, "sum over the cap: %s", last);
    FAKE_QEMU(q2, ID2, 2048, 17, 2000);
    ACT(vm_act_memlock, a2);
    CHECK(ok("vm.memlockSet") && pl_pid == q2 && pl_cur == (rlim_t)2560 << 20, "fits beside the first: %s", last);
    /* the first QEMU ended (its pid now another process's start time): its share is free, and its vGPU can be granted
     * to a new QEMU */
    FAKE_QEMU(q, ID1, 4096, 16, 3000);
    pl_pid = 0;
    ACT(vm_act_memlock, a);
    CHECK(ok("vm.memlockSet") && pl_pid == q, "a dead grant frees its share: %s", last);
    /* the cap alone */
    ACT(vm_act_gpu_memlock_cap, "4096");
    FAKE_QEMU(q, ID1, 4096, 16, 4000);
    pl_pid = 0;
    ACT(vm_act_memlock, a);
    CHECK(err("vm.memlockCap") && strstr(last, "need=4608") && !pl_pid, "cap: %s", last);
    ACT(vm_act_gpu_memlock_cap, "16384");
    /* refused, nothing set: another user's, another binary, a lord-vm elsewhere than the installed one, no vfio fd,
     * another's vGPU, another machine's vGPU, a bad argument, an ended process */
    FAKE_QEMU(q, ID1, 4096, 16, 5000);
    pl_pid = 0;
    last[0] = 0; vm_act_memlock(&other, a, strlen(a));
    CHECK(err("vm.memlockRefused") && !pl_pid, "another user's: %s", last);
    snprintf(rel, sizeof rel, "proc/%d/exe", (int)q); lnk(rel, "/home/x/qemu-system-x86_64");
    ACT(vm_act_memlock, a);
    CHECK(err("vm.memlockRefused") && !pl_pid, "not the system's QEMU: %s", last);
    lnk(rel, "/usr/bin/qemu-system-x86_64");
    lnk("proc/5151/exe", "/home/x/lord-vm");
    ACT(vm_act_memlock, a);
    CHECK(err("vm.memlockRefused") && !pl_pid, "a lord-vm that isn't the installed one: %s", last);
    lnk("proc/5151/exe", "/usr/bin/bash");
    ACT(vm_act_memlock, a);
    CHECK(err("vm.memlockRefused") && !pl_pid, "parent not lord-vm: %s", last);
    lnk("proc/5151/exe", "/usr/lib/lordos/bin/lord-vm");
    FAKE_QEMU(q, ID1, 4096, -1, 5000);
    ACT(vm_act_memlock, a);
    CHECK(err("vm.memlockRefused") && !pl_pid, "doesn't hold the vGPU's group: %s", last);
    FAKE_QEMU(q, ID1, 4096, 17, 5000);
    ACT(vm_act_memlock, a);
    CHECK(err("vm.memlockRefused") && !pl_pid, "holds another group: %s", last);
    FAKE_QEMU(q, ID1, 4096, 16, 5000);
    put("run/lordos/vm-gvt/" ID1, "99999\n");
    ACT(vm_act_memlock, a);
    CHECK(err("vm.memlockRefused") && !pl_pid, "another's vGPU: %s", last);
    snprintf(st, sizeof st, "%u\n", (unsigned)req.uid);
    put("run/lordos/vm-gvt/" ID1, st);
    ACT(vm_act_memlock, "4242 x");
    CHECK(err("admin.invalidArgument"), "bad arg: %s", last);
    ACT(vm_act_memlock, a);
    CHECK(ok("vm.memlockSet") && pl_pid == q, "all checks pass again: %s", last);
    kill(q, SIGKILL); kill(q2, SIGKILL);
    waitpid(q, NULL, 0); waitpid(q2, NULL, 0);
    pl_pid = 0;
    ACT(vm_act_memlock, a);
    CHECK(err("vm.memlockRefused") && !pl_pid, "ended: %s", last);
    #undef FAKE_QEMU
}

int main(void) {
    snprintf(root, sizeof root, "/tmp/vmgpu-test-XXXXXX");
    const char *tmp = getenv("TMPDIR");
    if (tmp && strlen(tmp) < 400) snprintf(root, sizeof root, "%s/vmgpu-test-XXXXXX", tmp);
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    fake_sys();
    test_parsers();
    test_list();
    test_actions();
    test_memlock();
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd)) {}
    printf("vmgpu-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
