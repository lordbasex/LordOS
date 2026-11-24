/* js_vm.c — lord.vm: the Machines app (docs/VIRTUALIZATION.md 3.1 and "Phase 1 implementation"). lord-server plays
 * the part server/tests/vm_dev.c plays in the tests: it starts lord-vm on a control socketpair (fd 3), asks it for
 * the display connection, starts lord-vmdisplay with it (fd 3 its channel, fd 4 the D-Bus descriptor), gives the
 * helper a sealed picture memfd that the engine shows as a media node, acknowledges frames once they were painted
 * and forwards input. Nothing here blocks: each poll takes at most VD_POLL_MAX messages per channel.
 *
 *   host() → { arch, cpus, cores (physical), ramMb, kvmDevice, kvm, vcpusRecommended, memMaxMb, missing: { amd64, arm64 }, ready,
 *             cards: [{ name, kind: 'wired'|'wifi', up }] }   (cards: what a bridged machine may join, 6.5)
 *   list() → [{ dir, folder, name, id, arch, accel, vcpus, memoryMb, diskGb, firmware, tpm, secureBoot, network, iso, state, reason,
 *              code, detail, since, error }]   the machines in ~/Machines (<name>.lordvm)
 *   create({ name, arch, vcpus, memoryMb, diskGb, import | iso }) → job id (0: refused);  job(id) → { done, ok, out, dir }
 *   start(dir) → session id · attach(dir) → session id (a machine already running, through its runtime socket)
 *   poll(id) → { state, reason, code, detail, vcpus, memoryMb, sandbox, cgroup, display, w, h, media, ended, exit,
 *               lastError, frames }
 *   cmd(id, 'shutdown'|'poweroff'|'pause'|'resume'|'reset'|'display') · stop(dir, how) for a machine with no session
 *   key(id, evdev, down) · motion(id, x, y) · button(id, b, down) · wheel(id, dy, dx) · release(id)
 *   resize(id, w, h) · rate(id, hz) · close(id)
 *   log(dir) → the end of the machine's lord-vm.log (what failed)
 *   cdrom(id, path | null) → a disc into (or out of) a running machine's drive; poll's cd / cdMessage say how it went
 *   sendFile(id, path[, { open }]) → request id: a dropped file into the guest's Downloads (guest agent); with open, the
 *             guest tools then open it there (Files' "Open with", 7.3); files(id) → how they went */
#include "js_vm.h"
#include "js_secret.h"
#include "../media/media_int.h"
#include "../media/aring.h"
#include "../vm/vmproto.h"
#include "../vm/vmpace.h"
#include "../vm/machine.h"
#include "../vm/vmgroup.h"
#include "../vm/vmlink.h"
#include "../vm/vm_admin.h"
#include "../vm/vmthumb.h"
#include "../vm/vmops.h"
#include "../vm/logclean.h"
#include "../vm/vmagent.h"
#include "../vm/vmnet.h"
#include "../vm/vmgpu.h"
#include "../vm/vmsig.h"
#include "../wl/wl.h"
#include "../gpu/gpu.h"
#include "../gpu/dmaread.h"
#include "../gpu/gband.h"
#include <drm/drm_fourcc.h>
#include "../sec/json.h"
#include "../sec/perms.h"
#include "../sec/restrict.h"
#include "../sec/cgroup.h"
#include "../vm/vmwatch.h"
#include "../sound/snd.h"
#include "ui.h"
#include "../term/term.h"
#include "../apps.h"
#include "../fsops.h"
#include "../pointerlock.h"

#include <crypt.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <linux/fs.h>
#include <linux/fiemap.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../vm/vmpin.h"

extern char **environ;

static const char *const STATES[VS_COUNT] = { "stopped", "starting", "running", "paused", "stopping", "crashed" };
static const char *const REASONS[VR_COUNT] = {
    "none", "guest-shutdown", "host-shutdown", "forced", "poweroff", "qemu-failed", "qemu-signal", "out-of-memory",
    "start-failed", "guest-panic", "supervisor-lost", "reset", "user", "suspended", "resume-failed", "installed",
    "qemu-3d-crash", "qemu-3d-stop", "session-full",
};

static void reap_orphans(void);

/* ---- paths ---- */

static int bin_path(const char *name, char *out, size_t n) {
    ssize_t l = readlink("/proc/self/exe", out, n - 32);
    if (l <= 0) return -1;
    out[l] = 0;
    char *s = strrchr(out, '/');
    if (!s) return -1;
    snprintf(s + 1, n - (size_t)(s + 1 - out), "%s", name);
    struct stat st;
    return stat(out, &st) || !S_ISREG(st.st_mode) || (st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 022) ? -1 : 0;
}

/* ~/Machines, made on first use; its real path */
static int library(char *out, size_t n, int make) {
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') return -1;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/Machines", home);
    if (make && mkdir(p, 0700) && errno != EEXIST) return -1;
    char r[PATH_MAX];
    if (!realpath(p, r) || strlen(r) >= n) return -1;
    memcpy(out, r, strlen(r) + 1);
    return 0;
}

/* a machine folder the app may act on: a real folder <name>.lordvm directly in ~/Machines, or a machine group's member,
 * <group>.lordgroup/<name>.lordvm in ~/Machines (docs/VIRTUALIZATION.md 19.2), one level and no more */
static int machine_dir(JSContext *c, JSValueConst v, char *out, size_t n) {
    const char *s = JS_ToCString(c, v);
    if (!s) return -1;
    char lib[PATH_MAX], real[PATH_MAX];
    struct stat st;
    int ok = !library(lib, sizeof lib, 0) && !lstat(s, &st) && S_ISDIR(st.st_mode) && realpath(s, real);
    JS_FreeCString(c, s);
    if (!ok) return -1;
    size_t ll = strlen(lib), rl = strlen(real);
    if (strncmp(real, lib, ll) || real[ll] != '/' || rl < ll + 8 || strcmp(real + rl - 7, ".lordvm") || rl >= n) return -1;
    const char *sl = strchr(real + ll + 1, '/');
    if (sl) {
        /* a member: its parent is a .lordgroup folder of the library, and nothing deeper */
        char g[PATH_MAX];
        if (strchr(sl + 1, '/') || !vm_group_parent(real, g, sizeof g) || strlen(g) != (size_t)(sl - real)) return -1;
    }
    memcpy(out, real, rl + 1);
    return 0;
}

/* a machine group's folder the app may act on: a real folder <name>.lordgroup directly in ~/Machines */
static int group_dir(JSContext *c, JSValueConst v, char *out, size_t n) {
    const char *s = JS_ToCString(c, v);
    if (!s) return -1;
    char lib[PATH_MAX], real[PATH_MAX];
    struct stat st;
    int ok = !library(lib, sizeof lib, 0) && !lstat(s, &st) && S_ISDIR(st.st_mode) && realpath(s, real);
    JS_FreeCString(c, s);
    if (!ok) return -1;
    size_t ll = strlen(lib), rl = strlen(real);
    if (strncmp(real, lib, ll) || real[ll] != '/' || strchr(real + ll + 1, '/') || !vm_group_dir_ok(real) || rl >= n) return -1;
    memcpy(out, real, rl + 1);
    return 0;
}

static int host_arch(void) {
    struct utsname u;
    if (uname(&u)) return -1;
    return !strcmp(u.machine, "x86_64") ? VM_ARCH_AMD64 : !strcmp(u.machine, "aarch64") ? VM_ARCH_ARM64 : -1;
}

static int kvm_usable(void) {
    int fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static void set_str(JSContext *c, JSValue o, const char *k, const char *v) { JS_SetPropertyStr(c, o, k, JS_NewString(c, v ? v : "")); }
static void set_int(JSContext *c, JSValue o, const char *k, long v) { JS_SetPropertyStr(c, o, k, JS_NewInt64(c, v)); }
static int ival(JSContext *c, JSValueConst v) { int i = 0; JS_ToInt32(c, &i, v); return i; }

/* ---- host ---- */

static JSValue f_host(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    VmHost h;
    vm_host_probe(&h);
    JSValue o = JS_NewObject(c);
    int a = host_arch();
    set_str(c, o, "arch", a == VM_ARCH_AMD64 ? "amd64" : a == VM_ARCH_ARM64 ? "arm64" : "");
    set_int(c, o, "cpus", h.cpus);
    set_int(c, o, "cores", vm_host_cores(h.cpus));
    set_int(c, o, "ramMb", h.ram_mb);
    struct stat st;
    JS_SetPropertyStr(c, o, "kvmDevice", JS_NewBool(c, !stat("/dev/kvm", &st) && S_ISCHR(st.st_mode)));
    JS_SetPropertyStr(c, o, "kvm", JS_NewBool(c, kvm_usable()));
    set_int(c, o, "vcpusRecommended", vm_vcpus_recommended(&h));
    set_int(c, o, "memMaxMb", vm_mem_max_mb(&h));
    char pk[512], bin[PATH_MAX];
    JSValue miss = JS_NewObject(c);
    vm_missing_packages(2, pk, sizeof pk); set_str(c, miss, "amd64", pk);
    vm_missing_packages(1, pk, sizeof pk); set_str(c, miss, "arm64", pk);
    JS_SetPropertyStr(c, o, "missing", miss);
    JS_SetPropertyStr(c, o, "ready", JS_NewBool(c, !bin_path("lord-vm", bin, sizeof bin)));
    JS_SetPropertyStr(c, o, "display", JS_NewBool(c, !bin_path("lord-vmdisplay", bin, sizeof bin)));
    /* 3D graphics (6.10): "" when a Linux machine can have them here, else why not: "arch" (amd64 only for now),
     * "gpu" (this desktop draws without a GPU, so it couldn't read the pictures back), "modules" (QEMU's OpenGL
     * modules missing: vm-enable installs qemu-system-modules-opengl), "node" (no render node this user can open) */
    {
        GpuInfo gi;
        gpu_info(&gi);
        const char *why3d = "";
        struct stat rs;
        if (a != VM_ARCH_AMD64) why3d = "arch";
        else if (!gpu_active() || !gi.dmabuf || !gi.render_node || strncmp(gi.render_node, "/dev/dri/renderD", 16)) why3d = "gpu";
        else if (access("/usr/lib/x86_64-linux-gnu/qemu/hw-display-virtio-vga-gl.so", F_OK) || access("/usr/lib/x86_64-linux-gnu/qemu/ui-opengl.so", F_OK)) why3d = "modules";
        else if (stat(gi.render_node, &rs) || !S_ISCHR(rs.st_mode) || access(gi.render_node, R_OK | W_OK)) why3d = "node";
        set_str(c, o, "no3d", why3d);
    }
    char lib[PATH_MAX];
    set_str(c, o, "library", library(lib, sizeof lib, 0) ? "" : lib);
    /* where machines live: free space and what kind of disk (a microSD card is slow for machines) */
    char where[PATH_MAX];
    const char *home = getenv("HOME");
    snprintf(where, sizeof where, "%s", lib[0] && !library(lib, sizeof lib, 0) ? lib : home ? home : "/");
    struct statvfs vf;
    if (!statvfs(where, &vf)) {
        JS_SetPropertyStr(c, o, "libFreeBytes", JS_NewFloat64(c, (double)vf.f_bavail * vf.f_frsize));
        JS_SetPropertyStr(c, o, "libTotalBytes", JS_NewFloat64(c, (double)vf.f_blocks * vf.f_frsize));
    }
    struct stat ls;
    char sys[96], link[PATH_MAX];
    const char *kind = "";
    if (!stat(where, &ls)) {
        snprintf(sys, sizeof sys, "/sys/dev/block/%u:%u", major(ls.st_dev), minor(ls.st_dev));
        ssize_t k = readlink(sys, link, sizeof link - 1);
        if (k > 0) { link[k] = 0; kind = strstr(link, "/mmcblk") ? "sd" : strstr(link, "/nvme") ? "nvme" : strstr(link, "/usb") ? "usb" : "disk"; }
    }
    set_str(c, o, "libDevice", kind);
    char model[128] = "";
    int mf = open("/proc/device-tree/model", O_RDONLY | O_CLOEXEC);
    if (mf >= 0) { ssize_t k = read(mf, model, sizeof model - 1); close(mf); if (k > 0) model[k] = 0; }
    for (char *q = model; *q; q++) if ((unsigned char)*q < 0x20) *q = 0;
    set_str(c, o, "board", model);
    char *cards = vmnet_cards_json("/sys/class/net");
    JS_SetPropertyStr(c, o, "cards", cards ? JS_ParseJSON(c, cards, strlen(cards), "<cards>") : JS_NewArray(c));
    free(cards);
    return o;
}

/* a bridged machine's macvtap (6.5) that nothing runs on any more goes: the admin helper removes only the caller's and
 * never one a process holds. Not within 30 s of a start (Machines makes it just before), and once a minute per machine */
static time_t last_start;
static void unbridge_idle(const VmMachine *m, const char *state) {
    static struct { char id[37]; time_t at; } sent[8];
    static int next;
    char name[VM_IFNAME_MAX + 1];
    if (vm_bridge_ifname(m->id, name) || !if_nametoindex(name)) return;
    if (strcmp(state, "stopped") && strcmp(state, "crashed")) return;
    time_t now = time(NULL);
    if (now - last_start < 30) return;
    for (int i = 0; i < 8; i++) if (!strcmp(sent[i].id, m->id) && now - sent[i].at < 60) return;
    if (admin_fd() < 0 || admin_send("vm-net-unbridge", m->id, strlen(m->id)) < 0) return;
    snprintf(sent[next].id, sizeof sent[next].id, "%s", m->id);
    sent[next].at = now;
    next = (next + 1) % 8;
}

/* a machine's Intel vGPU (docs/MACHINES-GPU.md, Mode A) that nothing runs on any more goes, the same way: the admin
 * helper's vm-gvt-remove removes only the caller's and never one a process holds */
static void gvt_idle(const VmMachine *m, const char *state) {
    static struct { char id[37]; time_t at; } sent[8];
    static int next;
    char p[96];
    snprintf(p, sizeof p, "/sys/bus/mdev/devices/%s", m->id);
    if (access(p, F_OK)) return;
    if (strcmp(state, "stopped") && strcmp(state, "crashed")) return;
    time_t now = time(NULL);
    if (now - last_start < 30) return;
    for (int i = 0; i < 8; i++) if (!strcmp(sent[i].id, m->id) && now - sent[i].at < 60) return;
    if (admin_fd() < 0 || admin_send("vm-gvt-remove", m->id, strlen(m->id)) < 0) return;
    snprintf(sent[next].id, sizeof sent[next].id, "%s", m->id);
    sent[next].at = now;
    next = (next + 1) % 8;
}

/* While a machine with a vGPU runs, the iGPU's minimum frequency is raised (gpu-min-freq): i915's own governor kept a
 * GVT-g guest at 350 MHz and 11–16 fps where 800–1150 MHz gives 75 (docs/MACHINES-GPU.md). Two thirds of RP0, to a
 * multiple of 50 (800 MHz on a UHD 630); "default" once none runs. Asked again at most once a minute */
static void gvt_freq(int running) {
    static int raised;
    static time_t at;
    time_t now = time(NULL);
    if (running == raised || now - at < (running ? 5 : 60)) return;
    VmGpu g[VMGPU_MAX];
    int n = vmgpu_list("", g, VMGPU_MAX);
    vmgpu_mark_desktop("", "/proc/self/fd", g, n);
    const VmGpu *x = vmgpu_gvt(g, n);
    if (!x || x->card < 0) { raised = running; return; }
    char arg[48];
    if (running) {
        char p[160], b[32] = "";
        snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/drm/card%d/gt/gt0/rps_RP0_freq_mhz", x->addr, x->card);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        ssize_t r = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
        if (fd >= 0) close(fd);
        int rp0 = r > 0 ? (b[r] = 0, atoi(b)) : 0;
        if (rp0 <= 0) { raised = running; return; }
        snprintf(arg, sizeof arg, "%s %d", x->addr, rp0 * 2 / 3 / 50 * 50);
    } else snprintf(arg, sizeof arg, "%s default", x->addr);
    if (admin_fd() < 0 || admin_send("gpu-min-freq", arg, strlen(arg)) < 0) return;
    raised = running;
    at = now;
}

/* A machine with a vGPU starts with its vCPUs held until its QEMU may lock the machine's memory (lordvm.c,
 * vgpu_memlock_tick): only root raises that limit, so the admin helper's vm-memlock does it for that one QEMU, which it
 * checks itself (the caller's, under the caller's lord-vm, with that machine's vGPU) and caps (maintainer's decision
 * 2026-11-21). Asked once per QEMU, from run.json's qemu_pid while the machine starts */
static void gvt_memlock(const char *dir) {
    static long sent[16];
    static int next;
    char p[PATH_MAX + 16], buf[2048];
    snprintf(p, sizeof p, "%s/run.json", dir);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0) return;
    long qpid = 0;
    JDoc d;
    if (!json_parse(buf, (size_t)n, &d, NULL, 0)) {
        const char *st = json_str(d.root, "state");
        if (st && (!strcmp(st, "starting") || !strcmp(st, "running") || !strcmp(st, "paused"))) qpid = (long)json_num(d.root, "qemu_pid", 0);
        json_free(&d);
    }
    if (qpid <= 1 || qpid > 999999999) return;
    for (int i = 0; i < 16; i++) if (sent[i] == qpid) return;
    char a[16];
    snprintf(a, sizeof a, "%ld", qpid);
    if (admin_fd() < 0 || admin_send("vm-memlock", a, strlen(a)) < 0) return;
    sent[next] = qpid;
    next = (next + 1) % 16;
}

/* gpus() → { gpus: [...], configured, restart } (vmgpu.h's vmgpu_json): Settings → Graphics cards and Machines'
 * Configure. The desktop's GPU is the card this process holds */
static JSValue f_gpus(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    VmGpu g[VMGPU_MAX];
    int n = vmgpu_list("", g, VMGPU_MAX);
    vmgpu_mark_desktop("", "/proc/self/fd", g, n);
    char *j = vmgpu_json_full("", g, n, vmgpu_configured(""));
    if (!j) return JS_NULL;
    JSValue v = JS_ParseJSON(c, j, strlen(j), "<gpus>");
    free(j);
    return v;
}

/* ---- the library ---- */

/* run.json, read as lord-vm writes it; a state that says "active" with no lord-vm behind it is a machine whose
 * supervisor died: lord-vm recovers it at the next start, the list says so now */
static void run_state(const char *dir, JSContext *c, JSValue o) {
    char p[PATH_MAX + 16], buf[2048];
    snprintf(p, sizeof p, "%s/run.json", dir);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    const char *state = "stopped", *reason = "none";
    char st[16] = "stopped", rs[24] = "none", since[24] = "", detail[200] = "";
    int code = 0;
    if (n > 0) {
        JDoc d;
        if (!json_parse(buf, (size_t)n, &d, NULL, 0)) {
            const char *s = json_str(d.root, "state"), *r = json_str(d.root, "reason"), *si = json_str(d.root, "since"), *de = json_str(d.root, "detail");
            for (int i = 0; s && i < VS_COUNT; i++) if (!strcmp(s, STATES[i])) snprintf(st, sizeof st, "%s", s);
            for (int i = 0; r && i < VR_COUNT; i++) if (!strcmp(r, REASONS[i])) snprintf(rs, sizeof rs, "%s", r);
            if (si) snprintf(since, sizeof since, "%s", si);
            if (de) snprintf(detail, sizeof detail, "%s", de);
            {
                const char *ag = json_str(d.root, "agent");
                set_str(c, o, "agentState", ag && (!strcmp(ag, "connected") || !strcmp(ag, "waiting")) ? ag : "off");
            }
            code = (int)json_num(d.root, "code", 0);
            long pid = (long)json_num(d.root, "pid", 0);
            int active = !strcmp(st, "starting") || !strcmp(st, "running") || !strcmp(st, "paused") || !strcmp(st, "stopping");
            if (active) {
                char cp[64], comm[32] = "";
                snprintf(cp, sizeof cp, "/proc/%ld/comm", pid);
                int cf = pid > 0 ? open(cp, O_RDONLY | O_CLOEXEC) : -1;
                ssize_t k = cf >= 0 ? read(cf, comm, sizeof comm - 1) : -1;
                if (cf >= 0) close(cf);
                if (k > 0) comm[k] = 0;
                if (k <= 0 || strncmp(comm, "lord-vm", 7)) { snprintf(st, sizeof st, "stopped"); snprintf(rs, sizeof rs, "supervisor-lost"); }
                else set_int(c, o, "vmPid", pid);     /* its lord-vm: a group's link is made again when it changes (19.3) */
            }
            json_free(&d);
        }
    }
    state = st; reason = rs;
    set_str(c, o, "state", state);
    set_str(c, o, "reason", reason);
    set_int(c, o, "code", code);
    set_str(c, o, "since", since);
    set_str(c, o, "detail", detail);
}

/* vCPU pinning (docs/VIRTUALIZATION.md 3.7.1): what a machine without a choice of its own gets here, as lord-vm decides it
 * at start (vmpin_default_on over this computer's topology, read once) */
static const char *pin_effective(const VmMachine *m) {
    static VmTopo topo;
    static cpu_set_t allowed;
    static int read;
    if (m->cpu_pin) return VM_PIN_NAMES[m->cpu_pin];
    if (!read) read = !vmpin_topo_read("/sys", &topo) && !sched_getaffinity(0, sizeof allowed, &allowed) ? 1 : -1;
    return read > 0 && vmpin_default_on(&topo, &allowed, m->vcpus) ? "auto" : "off";
}

/* one machine of the library, or a group's member (gdir: its group folder, g its group.json, mi its index there) */
static JSValue list_one(JSContext *c, const char *dir, const char *folder, const char *gdir, const VmGroup *g, int mi, int *gvt_running) {
    JSValue o = JS_NewObject(c);
    set_str(c, o, "dir", dir);
    set_str(c, o, "folder", folder);
    VmMachine m;
    char err[256] = "";
    int r = vm_machine_load(dir, &m, err, sizeof err);
    if (r == VM_OK) {
        set_str(c, o, "name", m.name);
        set_str(c, o, "id", m.id);
        set_str(c, o, "arch", vm_arch_name(m.arch));
        set_str(c, o, "accel", m.accel == VM_ACCEL_KVM ? "kvm" : "tcg");
        set_int(c, o, "vcpus", m.vcpus);
        set_int(c, o, "memoryMb", m.memory_mb);
        set_int(c, o, "diskGb", m.disk_gb);
        set_str(c, o, "firmware", m.firmware == VM_FW_UEFI ? "uefi" : "bios");
        JS_SetPropertyStr(c, o, "tpm", JS_NewBool(c, m.tpm));
        JS_SetPropertyStr(c, o, "secureBoot", JS_NewBool(c, m.secure_boot));
        {
            /* where it starts from: "auto" or "network,disk" (vm_boot_spec), and the firmware's boot menu */
            char bo[32];
            vm_boot_spec(&m, bo, sizeof bo);
            set_str(c, o, "bootOrder", bo);
            JS_SetPropertyStr(c, o, "bootMenu", JS_NewBool(c, m.boot_menu));
        }
        set_str(c, o, "network", VM_NET_NAMES[m.network]);
        set_str(c, o, "bridge", m.bridge);
        {
            /* forwarded ports (6.5): [{ proto, host, guest, open }] */
            JSValue pa = JS_NewArray(c);
            for (int i = 0; i < m.n_ports; i++) {
                JSValue e = JS_NewObject(c);
                set_str(c, e, "proto", m.ports[i].proto == VM_PROTO_UDP ? "udp" : "tcp");
                set_int(c, e, "host", m.ports[i].host);
                set_int(c, e, "guest", m.ports[i].guest);
                set_str(c, e, "open", m.ports[i].open == VM_OPEN_LAN ? "lan" : "this-computer");
                JS_SetPropertyUint32(c, pa, (uint32_t)i, e);
            }
            JS_SetPropertyStr(c, o, "ports", pa);
        }
        set_str(c, o, "display", m.display == VM_DISPLAY_WINDOW ? "window" : "none");
        set_str(c, o, "iso", m.iso);
        set_str(c, o, "base", m.base);
        JS_SetPropertyStr(c, o, "agent", JS_NewBool(c, m.agent));
        JS_SetPropertyStr(c, o, "clipboardAsk", JS_NewBool(c, m.clipboard_ask));
        set_str(c, o, "pointer", m.pointer_guest ? "guest" : "lordos");   /* who draws a Windows guest's pointer (7.1.2) */
        JS_SetPropertyStr(c, o, "captureFullscreen", JS_NewBool(c, m.capture_fullscreen));   /* "Capture mouse" (6.25) */
        JS_SetPropertyStr(c, o, "perfProfile", JS_NewBool(c, m.perf_profile));   /* the guest's performance profile (7.1.4) */
        JS_SetPropertyStr(c, o, "guestLinks", JS_NewBool(c, m.guest_links));     /* links and notifications from the guest (6.12) */
        JS_SetPropertyStr(c, o, "guestNotify", JS_NewBool(c, m.guest_notify));
        set_str(c, o, "cpuPin", pin_effective(&m));                                           /* "off" | "auto" (3.7.1) */
        JS_SetPropertyStr(c, o, "cpuPinChosen", JS_NewBool(c, m.cpu_pin != VM_PIN_DEFAULT));
        set_str(c, o, "clipboard", m.clipboard == VM_CLIP_BOTH ? "both" : m.clipboard == VM_CLIP_TO_GUEST ? "to-guest" : m.clipboard == VM_CLIP_FROM_GUEST ? "from-guest" : "off");
        set_str(c, o, "mac", m.mac);
        /* sound (6.9): host-side choices */
        JS_SetPropertyStr(c, o, "sound", JS_NewBool(c, m.sound));
        set_int(c, o, "volume", m.volume);
        JS_SetPropertyStr(c, o, "microphone", JS_NewBool(c, m.microphone));
        set_str(c, o, "soundOut", m.sound_out);
        set_str(c, o, "soundIn", m.sound_in);
        {
            /* a Clone whose guest is still to get its own hostname and machine-id (6.3) */
            char idh[VM_HOSTNAME_MAX + 1] = "";
            set_str(c, o, "identityPending", vm_identity_read(dir, idh, sizeof idh) ? "" : idh);
        }
        set_str(c, o, "devices", m.devices == VM_DEV_COMPAT ? "compat" : "virtio");
        /* the windows device set (4.3), and the graphics (6.10): 3D for Linux machines only */
        JS_SetPropertyStr(c, o, "windowsDevices", JS_NewBool(c, m.devices == VM_DEV_WINDOWS));
        set_str(c, o, "graphics", m.graphics == VM_GFX_3D ? "3d" : "2d");
        set_str(c, o, "gpu", VM_VGPU_NAMES[m.vgpu]);        /* an Intel vGPU (schema 11): "", "intel-whole", "intel-shared" */
        {
            char rs[24] = "follow";
            if (m.res_w && m.res_h) snprintf(rs, sizeof rs, "%dx%d", m.res_w, m.res_h);
            set_str(c, o, "resolution", rs);
        }
        set_str(c, o, "nicModel", VM_NIC_MODELS[m.nic_model]);
        set_int(c, o, "dataDisks", m.n_data);
        {
            /* shared folders (6.7): [{ tag, path, writable }] */
            JSValue sh = JS_NewArray(c);
            for (int i = 0; i < m.n_shares; i++) {
                JSValue e = JS_NewObject(c);
                set_str(c, e, "tag", m.share_tag[i]);
                set_str(c, e, "path", m.share_path[i]);
                JS_SetPropertyStr(c, e, "writable", JS_NewBool(c, m.share_rw[i]));
                JS_SetPropertyUint32(c, sh, (uint32_t)i, e);
            }
            JS_SetPropertyStr(c, o, "shares", sh);
            /* the transfer folder (6.7.1): on, and where it is (made at the machine's start, or by transferFolder) */
            char xp[PATH_MAX];
            JS_SetPropertyStr(c, o, "transfer", JS_NewBool(c, m.transfer));
            set_str(c, o, "transferPath", vm_transfer_path(dir, xp, sizeof xp) ? "" : xp);
        }
        {
            char ip[PATH_MAX + 320];
            snprintf(ip, sizeof ip, "%s/imported.json", dir);
            JS_SetPropertyStr(c, o, "imported", JS_NewBool(c, !access(ip, F_OK)));
        }
        set_str(c, o, "created", m.created);
        set_str(c, o, "error", "");
    } else {
        set_str(c, o, "name", folder);
        set_str(c, o, "error", r == VM_E_NEWER ? "newer" : "invalid");
    }
    run_state(dir, c, o);
    if (r == VM_OK) {
        JSValue sv = JS_GetPropertyStr(c, o, "state");
        const char *ss = JS_ToCString(c, sv);
        if (ss) {
            unbridge_idle(&m, ss);
            if (m.vgpu) {
                gvt_idle(&m, ss);
                gvt_memlock(dir);
                if (!strcmp(ss, "running") || !strcmp(ss, "paused") || !strcmp(ss, "starting")) gvt_running++;
            }
            JS_FreeCString(c, ss);
        }
        JS_FreeValue(c, sv);
    }
    /* an operation that was interrupted and waits for the next start (3.6) */
    {
        VmOp op;
        set_str(c, o, "operation", vm_op_read_peek(dir, &op) ? op.op : "");
    }
    /* a saved session (6.1): the next start resumes from it */
    char sp[PATH_MAX + 300];
    snprintf(sp, sizeof sp, "%s/suspend.state", dir);
    struct stat ss;
    JS_SetPropertyStr(c, o, "suspended", JS_NewBool(c, !lstat(sp, &ss) && S_ISREG(ss.st_mode)));
    JS_SetPropertyStr(c, o, "stateBytes", JS_NewFloat64(c, !lstat(sp, &ss) && S_ISREG(ss.st_mode) ? (double)ss.st_blocks * 512 : 0));
    if (gdir) {
        /* a machine group's member (docs/VIRTUALIZATION.md 19.2): the group's folder and id, its role, whether it is the
         * card's, and whether its machine.json names this group (else the group is damaged: 19.9) */
        set_str(c, o, "groupDir", gdir);
        set_str(c, o, "groupId", g->id);
        set_str(c, o, "groupName", g->name);
        set_str(c, o, "role", VMG_ROLE_NAMES[g->m[mi].role]);
        JS_SetPropertyStr(c, o, "primary", JS_NewBool(c, g->m[mi].primary));
        JSValue gv = JS_GetPropertyStr(c, o, "error");
        const char *ge = JS_ToCString(c, gv);
        int ok = ge && !ge[0];
        if (ge) JS_FreeCString(c, ge);
        JS_FreeValue(c, gv);
        VmMachine mm;
        int loaded = !vm_machine_load(dir, &mm, NULL, 0);
        JS_SetPropertyStr(c, o, "groupOk", JS_NewBool(c, ok && loaded && !strcmp(mm.group, g->id) && mm.role == g->m[mi].role));
        /* the group's kind and the member's lock (19.5): its windows open protected (model.js machineProtected) */
        set_str(c, o, "groupKind", g->kind == VMG_KIND_WHONIX ? "whonix" : "");
        set_str(c, o, "profileLock", loaded ? VM_LOCK_NAMES[mm.profile_lock] : "");
        JS_SetPropertyStr(c, o, "disposable", JS_NewBool(c, g->disposable && g->m[mi].role == VM_ROLE_WORKSTATION));
    }
    return o;
}

static JSValue f_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    reap_orphans();
    JSValue arr = JS_NewArray(c);
    char lib[PATH_MAX];
    if (library(lib, sizeof lib, 0)) return arr;
    DIR *d = opendir(lib);
    if (!d) return arr;
    uint32_t n = 0;
    int gvt_running = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256) {
        size_t l = strlen(e->d_name);
        struct stat st;
        char dir[PATH_MAX + 520];
        if (e->d_name[0] != '.' && l > 10 && !strcmp(e->d_name + l - 10, VMG_SUFFIX)) {
            /* a machine group (19.2): its members, as group.json names them, one level down; at most VMG_MEMBERS_MAX */
            char gdir[PATH_MAX + 260];
            VmGroup g;
            snprintf(gdir, sizeof gdir, "%s/%s", lib, e->d_name);
            if (lstat(gdir, &st) || !S_ISDIR(st.st_mode) || !vm_group_dir_ok(gdir) || vm_group_load(gdir, &g, NULL, 0)) continue;
            for (int i = 0; i < g.n && n < 256; i++) {
                char mj[PATH_MAX + 540];
                snprintf(dir, sizeof dir, "%s/%s", gdir, g.m[i].dir);
                snprintf(mj, sizeof mj, "%s/machine.json", dir);
                if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || access(mj, F_OK)) continue;
                char folder[VMG_DIR_MAX + 1];
                snprintf(folder, sizeof folder, "%.*s", (int)(strlen(g.m[i].dir) - 7), g.m[i].dir);
                JSValue o = list_one(c, dir, folder, gdir, &g, i, &gvt_running);
                JS_SetPropertyUint32(c, arr, n++, o);
            }
            continue;
        }
        if (e->d_name[0] == '.' || l < 8 || strcmp(e->d_name + l - 7, ".lordvm")) continue;
        snprintf(dir, sizeof dir, "%s/%s", lib, e->d_name);
        if (lstat(dir, &st) || !S_ISDIR(st.st_mode)) continue;
        char mj[PATH_MAX + 540];
        snprintf(mj, sizeof mj, "%s/machine.json", dir);
        if (access(mj, F_OK)) continue;          /* being created (lord-vm --create writes it last) */
        char folder[260];
        snprintf(folder, sizeof folder, "%.*s", (int)(l - 7), e->d_name);
        JSValue o = list_one(c, dir, folder, NULL, NULL, -1, &gvt_running);
        JS_SetPropertyUint32(c, arr, n++, o);
    }
    closedir(d);
    gvt_freq(gvt_running > 0);
    return arr;
}

/* groups() → [{ dir, id, name, kind, members: [{ role, dir, start, stop, primary }], disposable, freshGateway, release,
 * error }]: the library's machine groups (docs/VIRTUALIZATION.md 19.2), read with vm_group_load; error "newer" or
 * "invalid" for a group.json that can't be used (its members aren't listed then) */
static JSValue f_groups(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue arr = JS_NewArray(c);
    char lib[PATH_MAX];
    if (library(lib, sizeof lib, 0)) return arr;
    DIR *d = opendir(lib);
    if (!d) return arr;
    uint32_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 64) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l <= 10 || strcmp(e->d_name + l - 10, VMG_SUFFIX)) continue;
        char gdir[PATH_MAX + 260];
        struct stat st;
        snprintf(gdir, sizeof gdir, "%s/%s", lib, e->d_name);
        if (lstat(gdir, &st) || !S_ISDIR(st.st_mode) || !vm_group_dir_ok(gdir)) continue;
        VmGroup g;
        int r = vm_group_load(gdir, &g, NULL, 0);
        JSValue o = JS_NewObject(c);
        set_str(c, o, "dir", gdir);
        set_str(c, o, "error", r == VM_OK ? "" : r == VM_E_NEWER ? "newer" : "invalid");
        if (r == VM_OK) {
            set_str(c, o, "id", g.id);
            set_str(c, o, "name", g.name);
            set_str(c, o, "kind", g.kind == VMG_KIND_WHONIX ? "whonix" : "");
            set_str(c, o, "release", g.release);
            JS_SetPropertyStr(c, o, "disposable", JS_NewBool(c, g.disposable));
            JS_SetPropertyStr(c, o, "freshGateway", JS_NewBool(c, g.fresh_gateway));
            /* the verified archive the members came from, and whether both bases are there (Storage: the archive may
             * go once they are; Start Fresh needs only the bases, 19.6) */
            set_str(c, o, "archiveSha512", g.archive_sha512);
            struct stat bs;
            int both = 1;
            for (int r = 1; r < VM_ROLE_COUNT; r++) if (!g.base[r][0] || lstat(g.base[r], &bs) || !S_ISREG(bs.st_mode)) both = 0;
            JS_SetPropertyStr(c, o, "basesThere", JS_NewBool(c, both));
            JSValue ms = JS_NewArray(c);
            for (int i = 0; i < g.n; i++) {
                JSValue m = JS_NewObject(c);
                char md[PATH_MAX + 520];
                snprintf(md, sizeof md, "%s/%s", gdir, g.m[i].dir);
                set_str(c, m, "role", VMG_ROLE_NAMES[g.m[i].role]);
                set_str(c, m, "dir", md);
                set_int(c, m, "start", g.m[i].start);
                set_int(c, m, "stop", g.m[i].stop);
                JS_SetPropertyStr(c, m, "primary", JS_NewBool(c, g.m[i].primary));
                JS_SetPropertyUint32(c, ms, (uint32_t)i, m);
            }
            JS_SetPropertyStr(c, o, "members", ms);
        } else set_str(c, o, "name", e->d_name);
        JS_SetPropertyUint32(c, arr, n++, o);
    }
    closedir(d);
    return arr;
}

/* link(groupDir, on) → "" or why not (English, for the log): the group's internal link (19.3, vm/vmlink.h). The two
 * machines paired are the ones the group's own group.json and machine.json name; JS only names the group folder, which
 * must be one of this library's. A new pair each time: Machines calls it once both members run, and again after either
 * one started anew */
static int group_dir(JSContext *c, JSValueConst v, char *out, size_t n);
static JSValue f_link(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char gdir[PATH_MAX], why[256];
    if (argc < 2 || group_dir(c, argv[0], gdir, sizeof gdir)) return JS_NewString(c, "not a group of this library");
    int on = JS_ToBool(c, argv[1]);
    if (vm_link_group(gdir, on, 3000, why, sizeof why)) {
        fprintf(stderr, "vm: group link %s: %s\n", on ? "on" : "off", why);
        return JS_NewString(c, why[0] ? why : "failed");
    }
    fprintf(stderr, "vm: group link %s (%s)\n", on ? "connected" : "pulled", gdir);
    return JS_NewString(c, "");
}

/* ---- creating (lord-vm --create, in the background) ---- */

#define MAX_JOBS 4
typedef struct {
    int used, fd, done, ok, exit; pid_t pid; char out[16384]; size_t n; char dir[PATH_MAX + 80]; char hash[160]; char creds[PATH_MAX];
    /* a lord-pgpv job (f_sig_check): the fingerprint asked for, the image's SHA-256 and the checksum file's bytes the
     * helper was given, to look for that SHA-256 in once it says valid */
    int sigcheck; char sig_fpr[41]; char sig_sha[65]; char *sums; size_t sums_n;
} Job;
static Job jobs[MAX_JOBS];

/* the folder's name from the machine's: letters, digits, space, '-', '_', '.'; others become '-' */
/* a new machine's folder: as machine.c names it (a rename gives the same) */
static void folder_name(const char *name, char *out, size_t n) { vm_folder_name(name, out, n); }

static int user_file(JSContext *c, JSValueConst v, char *out, size_t n) {
    const char *s = JS_ToCString(c, v);
    if (!s) return -1;
    char r[PATH_MAX];
    struct stat st;
    int ok = s[0] == '/' && realpath(s, r) && !stat(r, &st) && S_ISREG(st.st_mode) && !access(r, R_OK) && strlen(r) < n;
    JS_FreeCString(c, s);
    if (!ok) return -1;
    for (const unsigned char *p = (const unsigned char *)r; *p; p++) if (*p < 0x20 || *p == 0x7f) return -1;
    memcpy(out, r, strlen(r) + 1);
    return 0;
}

static int prop_int(JSContext *c, JSValueConst o, const char *k) { JSValue v = JS_GetPropertyStr(c, o, k); int i = 0; JS_ToInt32(c, &i, v); JS_FreeValue(c, v); return i; }

static void host_defaults(char *lang, size_t ln, char *tz, size_t tn, char *km, size_t kn);

/* an xkb layout's description ("English (US)") from the system's rules, for an unattended Debian install: "" if not
 * found. Only the "! layout" section, a line "  <code>  <description>" */
static void xkb_desc(const char *code, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen("/usr/share/X11/xkb/rules/base.lst", "re");
    if (!f) return;
    char line[256];
    int in = 0;
    size_t cl = strlen(code);
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '!') { in = !strncmp(line, "! layout", 8); continue; }
        if (!in) continue;
        const char *p = line;
        while (*p == ' ') p++;
        if (strncmp(p, code, cl) || p[cl] != ' ') continue;
        p += cl;
        while (*p == ' ') p++;
        size_t l = strcspn(p, "\n");
        if (l < n) { memcpy(out, p, l); out[l] = 0; }
        break;
    }
    fclose(f);
}

/* an unattended install's spec for lord-vm --unattended (vm/vmunattend.h): the object's words as JSON, the password
 * replaced by its SHA-512 crypt hash, in a 0600 file of the runtime folder that lord-vm reads and removes. 0, or -1 */
/* UTF-8 text → Windows Setup's own encoding of a password in an answer file: base64 of UTF-16LE(text + "Password"). Not a
 * hash (Setup has none): the seed disk that holds it is wiped when the install is done (lord-vm). 0, or -1 */
static int win_password(const char *pw, char *out, size_t on) {
    char t[300];
    if (snprintf(t, sizeof t, "%sPassword", pw) >= (int)sizeof t) return -1;
    uint8_t u16[600];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)t; *p;) {
        uint32_t cp;
        int k = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 0;
        if (!k) return -1;
        cp = k == 1 ? *p : k == 2 ? (*p & 0x1Fu) : k == 3 ? (*p & 0x0Fu) : (*p & 0x07u);
        for (int i = 1; i < k; i++) { if ((p[i] & 0xC0) != 0x80) return -1; cp = (cp << 6) | (p[i] & 0x3Fu); }
        p += k;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            uint32_t hi = 0xD800 + (cp >> 10), lo = 0xDC00 + (cp & 0x3FF);
            if (n + 4 > sizeof u16) return -1;
            u16[n++] = (uint8_t)hi; u16[n++] = (uint8_t)(hi >> 8); u16[n++] = (uint8_t)lo; u16[n++] = (uint8_t)(lo >> 8);
        } else {
            if (n + 2 > sizeof u16) return -1;
            u16[n++] = (uint8_t)cp; u16[n++] = (uint8_t)(cp >> 8);
        }
    }
    explicit_bzero(t, sizeof t);
    static const char B[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)u16[i] << 16 | (i + 1 < n ? (uint32_t)u16[i + 1] << 8 : 0) | (i + 2 < n ? u16[i + 2] : 0);
        if (o + 5 > on) { explicit_bzero(u16, sizeof u16); return -1; }
        out[o++] = B[v >> 18 & 63]; out[o++] = B[v >> 12 & 63];
        out[o++] = i + 1 < n ? B[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? B[v & 63] : '=';
    }
    out[o] = 0;
    explicit_bzero(u16, sizeof u16);
    return 0;
}

static int unattended_spec(JSContext *c, JSValueConst o, const char *arch, char *path, size_t pn) {
    static const char *const keys[] = { "kind", "user", "fullname", "hostname", "timezone", "keymap", "variant", "locale", "release",
                                        "kernel", "initrd", "append", "lang", NULL };
    char buf[8192];
    size_t n = (size_t)snprintf(buf, sizeof buf, "{\"arch\":\"%s\"", arch);
    int ok = 1;
    for (int i = 0; keys[i] && ok; i++) {
        JSValue v = JS_GetPropertyStr(c, o, keys[i]);
        if (JS_IsString(v)) {
            const char *sv = JS_ToCString(c, v);
            char q[1200];
            if (!sv || strlen(sv) > 512 || !json_quote(q, sizeof q, sv)) ok = 0;
            else n += (size_t)snprintf(buf + n, n < sizeof buf ? sizeof buf - n : 0, ",\"%s\":%s", keys[i], q);
            if (sv) JS_FreeCString(c, sv);
        }
        JS_FreeValue(c, v);
    }
    {
        /* the keyboard layout's description, for installers that ask for it by name (Debian's desktop) */
        JSValue vk = JS_GetPropertyStr(c, o, "keymap");
        const char *km = JS_IsString(vk) ? JS_ToCString(c, vk) : NULL;
        char desc[96], q[200];
        if (km && strlen(km) < 32) { xkb_desc(km, desc, sizeof desc); if (desc[0] && json_quote(q, sizeof q, desc)) n += (size_t)snprintf(buf + n, n < sizeof buf ? sizeof buf - n : 0, ",\"keydesc\":%s", q); }
        if (km) JS_FreeCString(c, km);
        JS_FreeValue(c, vk);
    }
    /* Windows (vmunattend.h): its own words; the password in Setup's encoding, and none is allowed */
    int win = 0;
    {
        JSValue vk = JS_GetPropertyStr(c, o, "kind");
        const char *k = JS_IsString(vk) ? JS_ToCString(c, vk) : NULL;
        win = k && !strcmp(k, "windows");
        if (k) JS_FreeCString(c, k);
        JS_FreeValue(c, vk);
    }
    if (win) {
        static const char *const wk[] = { "edition", "key", "uilang", "inputloc", "wtz", "userloc", NULL };
        for (int i = 0; wk[i] && ok; i++) {
            JSValue v = JS_GetPropertyStr(c, o, wk[i]);
            if (JS_IsString(v)) {
                const char *sv = JS_ToCString(c, v);
                char q[200];
                if (!sv || strlen(sv) > 64 || !json_quote(q, sizeof q, sv)) ok = 0;
                else n += (size_t)snprintf(buf + n, n < sizeof buf ? sizeof buf - n : 0, ",\"%s\":%s", wk[i], q);
                if (sv) JS_FreeCString(c, sv);
            }
            JS_FreeValue(c, v);
        }
        JSValue vg = JS_GetPropertyStr(c, o, "guided");
        if (JS_IsBool(vg)) n += (size_t)snprintf(buf + n, n < sizeof buf ? sizeof buf - n : 0, ",\"guided\":%s", JS_ToBool(c, vg) ? "true" : "false");
        JS_FreeValue(c, vg);
        JSValue vp = JS_GetPropertyStr(c, o, "password");
        size_t pwn = 0;
        char *pw = JS_IsString(vp) ? js_secret_dup(c, vp, &pwn) : NULL;
        char wp[260] = "";
        if (pw && pw[0] && (strlen(pw) > 127 || win_password(pw, wp, sizeof wp))) ok = 0;
        js_secret_free(pw, pwn);
        JS_FreeValue(c, vp);
        n += (size_t)snprintf(buf + n, n < sizeof buf ? sizeof buf - n : 0, ",\"wpass\":\"%s\"", wp);
        explicit_bzero(wp, sizeof wp);
    }
    JSValue vd = JS_GetPropertyStr(c, o, "desktop");
    if (JS_IsBool(vd)) n += (size_t)snprintf(buf + n, n < sizeof buf ? sizeof buf - n : 0, ",\"desktop\":%s", JS_ToBool(c, vd) ? "true" : "false");
    JS_FreeValue(c, vd);
    /* the password: only its hash leaves here */
    JSValue vp = win ? JS_UNDEFINED : JS_GetPropertyStr(c, o, "password");
    size_t pwn = 0;
    char *pw = JS_IsString(vp) ? js_secret_dup(c, vp, &pwn) : NULL;
    char hash[160] = "";
    if (pw && pw[0] && strlen(pw) < 256) {
        char salt[64];
        struct crypt_data cd;
        memset(&cd, 0, sizeof cd);
        const char *h = crypt_gensalt_rn("$6$", 0, NULL, 0, salt, sizeof salt) ? crypt_r(pw, salt, &cd) : NULL;
        if (h && h[0] == '$' && strlen(h) < sizeof hash) snprintf(hash, sizeof hash, "%s", h);
        explicit_bzero(&cd, sizeof cd);
    }
    js_secret_free(pw, pwn);
    JS_FreeValue(c, vp);
    if (!win && !hash[0]) ok = 0;
    if (!win) n += (size_t)snprintf(buf + n, n < sizeof buf ? sizeof buf - n : 0, ",\"hash\":\"%s\"}", hash);
    else n += (size_t)snprintf(buf + n, n < sizeof buf ? sizeof buf - n : 0, "}");
    if (!ok || n >= sizeof buf) return -1;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    static unsigned k;
    if (rt && rt[0] == '/' && strlen(rt) < 200) snprintf(path, pn, "%s/.lordos-vm-unattended-%d-%u", rt, (int)getpid(), ++k);
    else snprintf(path, pn, "/run/user/%u/.lordos-vm-unattended-%d-%u", (unsigned)getuid(), (int)getpid(), ++k);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    int good = fd >= 0 && write(fd, buf, n) == (ssize_t)n;
    if (fd >= 0) close(fd);
    explicit_bzero(buf, sizeof buf);
    if (!good) { if (fd >= 0) unlink(path); path[0] = 0; return -1; }
    return 0;
}

static JSValue f_create(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_NewInt32(c, 0);
    int slot = -1;
    for (int i = 0; i < MAX_JOBS; i++) if (!jobs[i].used) { slot = i; break; }
    char bin[PATH_MAX], lib[PATH_MAX];
    if (slot < 0 || bin_path("lord-vm", bin, sizeof bin) || library(lib, sizeof lib, 1)) return JS_NewInt32(c, 0);
    JSValue vn = JS_GetPropertyStr(c, argv[0], "name"), va = JS_GetPropertyStr(c, argv[0], "arch");
    JSValue vi = JS_GetPropertyStr(c, argv[0], "import"), vs = JS_GetPropertyStr(c, argv[0], "iso");
    const char *name = JS_ToCString(c, vn), *arch = JS_ToCString(c, va);
    int vcpus = prop_int(c, argv[0], "vcpus"), mem = prop_int(c, argv[0], "memoryMb"), disk = prop_int(c, argv[0], "diskGb");
    char imp[PATH_MAX] = "", iso[PATH_MAX] = "";
    int isolated = 0, share = 0;
    {
        JSValue vsh = JS_GetPropertyStr(c, argv[0], "shareClipboard");
        share = JS_ToBool(c, vsh) > 0;
        JS_FreeValue(c, vsh);
    }
    {
        JSValue vnet = JS_GetPropertyStr(c, argv[0], "network");
        const char *ns = JS_IsString(vnet) ? JS_ToCString(c, vnet) : NULL;
        isolated = ns && !strcmp(ns, "isolated");
        if (ns) JS_FreeCString(c, ns);
        JS_FreeValue(c, vnet);
    }
    int has_imp = JS_IsString(vi), has_iso = JS_IsString(vs);
    int ok = name && arch && vm_name_valid(name) && (!strcmp(arch, "amd64") || !strcmp(arch, "arm64")) &&
             vcpus >= 1 && vcpus <= VM_VCPUS_MAX && mem >= VM_MEM_MIN_MB && mem <= VM_MEM_MAX_MB &&
             disk >= VM_DISK_MIN_GB && disk <= VM_DISK_MAX_GB && has_imp != has_iso &&
             !(has_imp ? user_file(c, vi, imp, sizeof imp) : user_file(c, vs, iso, sizeof iso));
    /* a root password for systems that ask for one on their first start: only its hash is kept, in the machine's
     * folder (firstboot.creds, 0600), until the machine's first clean shutdown */
    char hash[160] = "";
    JSValue vp = JS_GetPropertyStr(c, argv[0], "rootPassword");
    if (ok && JS_IsString(vp)) {
        size_t pwn = 0;
        char *pw = js_secret_dup(c, vp, &pwn);
        if (pw && pw[0]) {
            char salt[64];
            struct crypt_data cd;
            memset(&cd, 0, sizeof cd);
            const char *h = strlen(pw) < 256 && crypt_gensalt_rn("$6$", 0, NULL, 0, salt, sizeof salt) ? crypt_r(pw, salt, &cd) : NULL;
            if (h && h[0] == '$' && strlen(h) < sizeof hash) snprintf(hash, sizeof hash, "%s", h); else ok = 0;
            explicit_bzero(&cd, sizeof cd);
        }
        js_secret_free(pw, pwn);
    }
    JS_FreeValue(c, vp);
    /* an unattended install (only from an ISO) */
    char spec[PATH_MAX] = "";
    JSValue vu = JS_GetPropertyStr(c, argv[0], "unattended");
    if (ok && JS_IsObject(vu)) { if (!has_iso || unattended_spec(c, vu, arch, spec, sizeof spec)) ok = 0; }
    JS_FreeValue(c, vu);
    /* a Windows 11 machine (4.3): secure boot, a TPM, Hyper-V, local time, its devices and the drivers' disc */
    char drv[PATH_MAX] = "";
    int windows = 0;
    JSValue vw = JS_GetPropertyStr(c, argv[0], "windows");
    if (ok && JS_IsObject(vw)) {
        JSValue vdr = JS_GetPropertyStr(c, vw, "drivers");
        windows = 1;
        if (!JS_IsString(vdr) || user_file(c, vdr, drv, sizeof drv) || strcmp(arch, "amd64")) ok = 0;
        JS_FreeValue(c, vdr);
    }
    JS_FreeValue(c, vw);
    JSValue vg = JS_GetPropertyStr(c, argv[0], "graphics");
    int gfx3d = 0;
    if (JS_IsString(vg)) {
        const char *gs = JS_ToCString(c, vg);
        if (gs && !strcmp(gs, "3d")) gfx3d = 1;
        else if (!gs || strcmp(gs, "2d")) ok = 0;
        if (gs) JS_FreeCString(c, gs);
    }
    JS_FreeValue(c, vg);
    int id = 0;
    if (ok) {
        char folder[64], dir[PATH_MAX + 80];
        folder_name(name, folder, sizeof folder);
        snprintf(dir, sizeof dir, "%s/%s.lordvm", lib, folder);
        for (int k = 2; !access(dir, F_OK) && k < 100; k++) snprintf(dir, sizeof dir, "%s/%s %d.lordvm", lib, folder, k);
        int native = (!strcmp(arch, "amd64") ? VM_ARCH_AMD64 : VM_ARCH_ARM64) == host_arch();
        char sv[16], sm[16], sd[16];
        snprintf(sv, sizeof sv, "%d", vcpus); snprintf(sm, sizeof sm, "%d", mem); snprintf(sd, sizeof sd, "%d", disk);
        char *args[48] = { "lord-vm", "--create", dir, "--name", (char *)name, "--arch", (char *)arch, "--accel", native ? "kvm" : "tcg",
                         "--vcpus", sv, "--memory", sm, "--disk", sd, "--network", isolated ? "isolated" : "shared", has_imp ? "--import" : "--iso", has_imp ? imp : iso, NULL };
        int na = 0;
        while (args[na]) na++;
        if (spec[0]) { args[na++] = "--unattended"; args[na++] = spec; }
        if (share) { args[na++] = "--agent"; args[na++] = "on"; args[na++] = "--clipboard"; args[na++] = "both"; }
        /* the Design profile's 3D graphics (6.10): Linux machines only; lord-vm refuses them for the windows devices */
        if (gfx3d && !windows) { args[na++] = "--graphics"; args[na++] = "3d"; }
        if (windows) {
            static char *const W[] = { "--secure-boot", "on", "--tpm", "on", "--hyperv", "on", "--rtc", "local", "--devices", "windows", "--drivers" };
            for (size_t i = 0; i < sizeof W / sizeof W[0]; i++) args[na++] = W[i];
            args[na++] = drv;
        }
        args[na] = NULL;
        int p[2];
        if (!access(dir, F_OK) || pipe2(p, O_CLOEXEC)) ok = 0;
        else {
            pid_t pid = fork();
            if (!pid) {
                int null = open("/dev/null", O_RDONLY);
                if (null >= 0) dup2(null, 0);
                dup2(p[1], 1);
                dup2(p[1], 2);
                syscall(SYS_close_range, 3, ~0u, 0);
                sigset_t none;
                sigemptyset(&none);
                sigprocmask(SIG_SETMASK, &none, NULL);
                execve(bin, args, environ);
                _exit(127);
            }
            close(p[1]);
            if (pid < 0) { close(p[0]); ok = 0; }
            else {
                fcntl(p[0], F_SETFL, O_NONBLOCK);
                jobs[slot] = (Job){ .used = 1, .fd = p[0], .pid = pid };
                snprintf(jobs[slot].dir, sizeof jobs[slot].dir, "%s", dir);
                snprintf(jobs[slot].hash, sizeof jobs[slot].hash, "%s", hash);
                id = slot + 1;
            }
        }
    }
    if (!id && spec[0]) unlink(spec);
    if (name) JS_FreeCString(c, name);
    if (arch) JS_FreeCString(c, arch);
    JS_FreeValue(c, vn); JS_FreeValue(c, va); JS_FreeValue(c, vi); JS_FreeValue(c, vs);
    return JS_NewInt32(c, id);
}

/* lord-vm with args in the background, as a job (f_job reads it); 0 when it couldn't start */
static int spawn_job(char *const args[], const char *dir) {
    int slot = -1;
    for (int i = 0; i < MAX_JOBS; i++) if (!jobs[i].used) { slot = i; break; }
    char bin[PATH_MAX];
    if (slot < 0 || bin_path("lord-vm", bin, sizeof bin)) return 0;
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return 0;
    pid_t pid = fork();
    if (!pid) {
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) dup2(null, 0);
        dup2(p[1], 1);
        dup2(p[1], 2);
        syscall(SYS_close_range, 3, ~0u, 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execve(bin, args, environ);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) { close(p[0]); return 0; }
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    jobs[slot] = (Job){ .used = 1, .fd = p[0], .pid = pid };
    snprintf(jobs[slot].dir, sizeof jobs[slot].dir, "%s", dir);
    return slot + 1;
}

/* hash(path) → job id: lord-vm --sha256 on a file the person chose or downloaded ({"progress":P} lines, then
 * {"sha256":…,"size":…}), to check a Windows ISO against the catalog's list (4.3) */
static JSValue f_hash(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char path[PATH_MAX];
    if (argc < 1 || !JS_IsString(argv[0]) || user_file(c, argv[0], path, sizeof path)) return JS_NewInt32(c, 0);
    char *args[] = { "lord-vm", "--sha256", path, NULL };
    return JS_NewInt32(c, spawn_job(args, ""));
}

/* ---- machine groups' jobs (docs/VIRTUALIZATION.md 19.6, 19.7) ----
 * groupCreate({ catalogId, archive, sha512, name }) → job id: lord-vm --group-create <library>/<name>.lordgroup. The
 * archive must be one the admin helper verified, in the image store (images/<sha512>.tar.xz, root's): lord-vm hashes it
 * again and takes the members' names, caps and settings from the signed catalog, never from here.
 * groupFresh(groupDir, gateway) → job id: lord-vm --group-fresh (Start Fresh, the group off).
 * groupSet(groupDir, { disposable, freshGateway }) → job id: lord-vm --group-set. 0 when refused. */
static JSValue f_group_create(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_NewInt32(c, 0);
    char lib[PATH_MAX], gdir[PATH_MAX + 64], arch[PATH_MAX], cid[40] = "", sha[136] = "", name[VM_NAME_MAX + 1] = "";
    if (library(lib, sizeof lib, 1)) return JS_NewInt32(c, 0);
    JSValue vc = JS_GetPropertyStr(c, argv[0], "catalogId"), va = JS_GetPropertyStr(c, argv[0], "archive");
    JSValue vs = JS_GetPropertyStr(c, argv[0], "sha512"), vn = JS_GetPropertyStr(c, argv[0], "name");
    const char *cs = JS_ToCString(c, vc), *ss = JS_ToCString(c, vs), *ns = JS_ToCString(c, vn);
    int ok = cs && ss && ns && strlen(cs) < sizeof cid && strlen(ss) == 128 && strlen(ns) < sizeof name && vm_name_valid(ns) &&
             JS_IsString(va) && !user_file(c, va, arch, sizeof arch);
    if (ok) { snprintf(cid, sizeof cid, "%s", cs); snprintf(sha, sizeof sha, "%s", ss); snprintf(name, sizeof name, "%s", ns); }
    if (cs) JS_FreeCString(c, cs);
    if (ss) JS_FreeCString(c, ss);
    if (ns) JS_FreeCString(c, ns);
    JS_FreeValue(c, vc); JS_FreeValue(c, va); JS_FreeValue(c, vs); JS_FreeValue(c, vn);
    if (!ok) return JS_NewInt32(c, 0);
    for (const char *p = cid; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return JS_NewInt32(c, 0);
    for (const char *p = sha; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return JS_NewInt32(c, 0);
    /* the verified download: <image store>/<sha512>.tar.xz and nothing else */
    char want[PATH_MAX];
    snprintf(want, sizeof want, "/var/lib/lordos/machines/images/%s.tar.xz", sha);
    if (strcmp(arch, want)) return JS_NewInt32(c, 0);
    struct stat st;
    if (snprintf(gdir, sizeof gdir, "%s/%s%s", lib, name, VMG_SUFFIX) >= (int)sizeof gdir || !vm_group_dir_ok(gdir) || !lstat(gdir, &st)) return JS_NewInt32(c, 0);
    char *args[] = { "lord-vm", "--group-create", gdir, "--catalog-id", cid, "--archive", arch, "--sha512", sha, "--name", name, NULL };
    return JS_NewInt32(c, spawn_job(args, ""));
}
static JSValue f_group_fresh(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char gdir[PATH_MAX];
    if (argc < 1 || group_dir(c, argv[0], gdir, sizeof gdir)) return JS_NewInt32(c, 0);
    int gw = argc > 1 && JS_ToBool(c, argv[1]) > 0;
    char *args[] = { "lord-vm", "--group-fresh", gdir, gw ? "--gateway" : NULL, NULL };
    return JS_NewInt32(c, spawn_job(args, gdir));
}
static JSValue f_group_set(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char gdir[PATH_MAX];
    if (argc < 2 || !JS_IsObject(argv[1]) || group_dir(c, argv[0], gdir, sizeof gdir)) return JS_NewInt32(c, 0);
    char *args[8] = { "lord-vm", "--group-set", gdir };
    int n = 3;
    JSValue vd = JS_GetPropertyStr(c, argv[1], "disposable"), vf = JS_GetPropertyStr(c, argv[1], "freshGateway");
    if (JS_IsBool(vd)) { args[n++] = "--disposable"; args[n++] = JS_ToBool(c, vd) ? "on" : "off"; }
    if (JS_IsBool(vf)) { args[n++] = "--fresh-gateway"; args[n++] = JS_ToBool(c, vf) ? "on" : "off"; }
    JS_FreeValue(c, vd); JS_FreeValue(c, vf);
    args[n] = NULL;
    return JS_NewInt32(c, n > 3 ? spawn_job(args, gdir) : 0);
}

/* (security scan F5) A guest writes to a writable shared folder: never one that is, sits under or holds a path lord.fs
 * protects (settings.json, the Terminal's configuration wherever the user put it, launchers…), or the guest could rewrite
 * what the host runs. Checked when the shares are set and again before every start: the Terminal's file can move into a
 * shared folder after the share was made. The reason a call returned 0 is kept for refusal(). */
static char refusal[48];
static int share_writable_ok(const char *path) {
    char real[PATH_MAX];
    if (!realpath(path, real)) return 1;            /* not there: lord-vm refuses the share itself */
    return !fsops_protected_resolved(real);
}
static int machine_shares_ok(const char *dir) {
    VmMachine m;
    char err[256];
    if (vm_machine_load(dir, &m, err, sizeof err) != VM_OK) return 1;   /* a bad machine.json is lord-vm's to report */
    for (int i = 0; i < m.n_shares; i++)
        if (m.share_rw[i] && !share_writable_ok(m.share_path[i])) { snprintf(refusal, sizeof refusal, "vm.shareProtected"); return 0; }
    char xp[PATH_MAX];
    if (m.transfer && !vm_transfer_path(dir, xp, sizeof xp) && !share_writable_ok(xp)) { snprintf(refusal, sizeof refusal, "vm.shareProtected"); return 0; }
    return 1;
}
static JSValue f_refusal(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue r = refusal[0] ? JS_NewString(c, refusal) : JS_NULL;
    refusal[0] = 0;
    return r;
}

/* set(dir, { name, vcpus, memoryMb, diskGb, network, bridge, ports: [{ proto, host, guest, open }], iso,
 *            shares: [{ tag, path, writable }], bootOrder: "auto"|"network,disk"…, bootMenu, pointer: "lordos"|"guest", captureFullscreen, perfProfile,
 *            cpuPin: "off"|"auto" }) → job id: lord-vm --set on a
 * machine that is off (Configure). shares replaces the list ([] empties it); lord-vm decides which folders may be shared */
static JSValue f_set(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char dir[PATH_MAX];
    if (argc < 2 || !JS_IsObject(argv[1]) || machine_dir(c, argv[0], dir, sizeof dir)) return JS_NewInt32(c, 0);
    char *args[66 + 2 * VM_SHARES + 2 * VM_PORTS], shv[VM_SHARES][VM_TAG_MAX + VM_PATH_MAX + 8], pv[VM_PORTS][48], br[VM_IFNAME_MAX + 1] = "", sv[16], sm[16], sd[16], name[VM_NAME_MAX + 8] = "", net[16] = "", iso[PATH_MAX] = "";
    int n = 0, ok = 1;
    args[n++] = "lord-vm"; args[n++] = "--set"; args[n++] = dir;
    JSValue v = JS_GetPropertyStr(c, argv[1], "name");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        if (s && vm_name_valid(s)) { snprintf(name, sizeof name, "%s", s); args[n++] = "--name"; args[n++] = name; } else ok = 0;
        if (s) JS_FreeCString(c, s);
    }
    JS_FreeValue(c, v);
    int vc = prop_int(c, argv[1], "vcpus"), mem = prop_int(c, argv[1], "memoryMb"), disk = prop_int(c, argv[1], "diskGb");
    if (vc) { if (vc < 1 || vc > VM_VCPUS_MAX) ok = 0; snprintf(sv, sizeof sv, "%d", vc); args[n++] = "--vcpus"; args[n++] = sv; }
    if (mem) { if (mem < VM_MEM_MIN_MB || mem > VM_MEM_MAX_MB) ok = 0; snprintf(sm, sizeof sm, "%d", mem); args[n++] = "--memory"; args[n++] = sm; }
    if (disk) { if (disk < VM_DISK_MIN_GB || disk > VM_DISK_MAX_GB) ok = 0; snprintf(sd, sizeof sd, "%d", disk); args[n++] = "--disk"; args[n++] = sd; }
    v = JS_GetPropertyStr(c, argv[1], "network");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        int known = 0;
        for (int k = 0; s && VM_NET_NAMES[k]; k++) if (!strcmp(s, VM_NET_NAMES[k])) known = 1;
        if (known) { snprintf(net, sizeof net, "%s", s); args[n++] = "--network"; args[n++] = net; } else ok = 0;
        if (s) JS_FreeCString(c, s);
    }
    JS_FreeValue(c, v);
    /* the network's details (6.5): the wired card a bridged machine joins, the forwarded ports (replacing the list; []
     * empties it), each checked here and again by lord-vm */
    v = JS_GetPropertyStr(c, argv[1], "bridge");
    if (JS_IsString(v)) {
        const char *bs = JS_ToCString(c, v);
        if (bs && vm_ifname_valid(bs)) { snprintf(br, sizeof br, "%s", bs); args[n++] = "--bridge"; args[n++] = br; } else ok = 0;
        if (bs) JS_FreeCString(c, bs);
    } else if (!JS_IsUndefined(v)) ok = 0;
    JS_FreeValue(c, v);
    v = JS_GetPropertyStr(c, argv[1], "ports");
    if (JS_IsArray(c, v)) {
        uint32_t len = 0;
        JSValue l = JS_GetPropertyStr(c, v, "length");
        JS_ToUint32(c, &len, l);
        JS_FreeValue(c, l);
        VmPort ps[VM_PORTS];
        if (len > VM_PORTS) ok = 0;
        else if (!len) { args[n++] = "--ports"; args[n++] = "none"; }
        for (uint32_t i = 0; ok && i < len; i++) {
            JSValue e = JS_GetPropertyUint32(c, v, i), pr = JS_GetPropertyStr(c, e, "proto"), op = JS_GetPropertyStr(c, e, "open");
            const char *prs = JS_IsString(pr) ? JS_ToCString(c, pr) : NULL, *ops = JS_IsString(op) ? JS_ToCString(c, op) : NULL;
            int h = prop_int(c, e, "host"), g = prop_int(c, e, "guest");
            ps[i] = (VmPort){ prs && !strcmp(prs, "udp") ? VM_PROTO_UDP : VM_PROTO_TCP, h, g, ops && !strcmp(ops, "lan") ? VM_OPEN_LAN : VM_OPEN_LOCAL };
            if (!prs || (strcmp(prs, "tcp") && strcmp(prs, "udp")) || (ops && strcmp(ops, "lan") && strcmp(ops, "this-computer")) || vm_ports_check(ps, (int)i + 1, VM_NET_SHARED, NULL, 0)) ok = 0;
            else { vm_port_spec(&ps[i], pv[i], sizeof pv[i]); args[n++] = "--port"; args[n++] = pv[i]; }
            if (prs) JS_FreeCString(c, prs);
            if (ops) JS_FreeCString(c, ops);
            JS_FreeValue(c, pr); JS_FreeValue(c, op); JS_FreeValue(c, e);
        }
    } else if (!JS_IsUndefined(v)) ok = 0;
    JS_FreeValue(c, v);
    char clip[16] = "";
    v = JS_GetPropertyStr(c, argv[1], "agent");
    if (JS_IsBool(v)) { args[n++] = "--agent"; args[n++] = JS_ToBool(c, v) ? "on" : "off"; }
    JS_FreeValue(c, v);
    v = JS_GetPropertyStr(c, argv[1], "clipboard");
    if (JS_IsString(v)) {
        const char *cs = JS_ToCString(c, v);
        if (cs && (!strcmp(cs, "off") || !strcmp(cs, "to-guest") || !strcmp(cs, "from-guest") || !strcmp(cs, "both"))) { snprintf(clip, sizeof clip, "%s", cs); args[n++] = "--clipboard"; args[n++] = clip; } else ok = 0;
        if (cs) JS_FreeCString(c, cs);
    }
    JS_FreeValue(c, v);
    char gfx[8] = "";
    v = JS_GetPropertyStr(c, argv[1], "graphics");
    if (JS_IsString(v)) {
        const char *gs = JS_ToCString(c, v);
        if (gs && (!strcmp(gs, "2d") || !strcmp(gs, "3d"))) { snprintf(gfx, sizeof gfx, "%s", gs); args[n++] = "--graphics"; args[n++] = gfx; } else ok = 0;
        if (gs) JS_FreeCString(c, gs);
    }
    JS_FreeValue(c, v);
    char gpu[16] = "";
    v = JS_GetPropertyStr(c, argv[1], "gpu");
    if (JS_IsString(v)) {
        const char *gs = JS_ToCString(c, v);
        if (gs && (!gs[0] || !strcmp(gs, "intel-whole") || !strcmp(gs, "intel-shared"))) { snprintf(gpu, sizeof gpu, "%s", gs[0] ? gs : "none"); args[n++] = "--gpu"; args[n++] = gpu; } else ok = 0;
        if (gs) JS_FreeCString(c, gs);
    } else if (!JS_IsUndefined(v)) ok = 0;
    JS_FreeValue(c, v);
    char res[24] = "";
    v = JS_GetPropertyStr(c, argv[1], "resolution");
    if (JS_IsString(v)) {
        const char *rs = JS_ToCString(c, v);
        int rw, rh;
        if (rs && (!strcmp(rs, "follow") || !vm_resolution_parse(rs, &rw, &rh))) { snprintf(res, sizeof res, "%s", rs); args[n++] = "--resolution"; args[n++] = res; } else ok = 0;
        if (rs) JS_FreeCString(c, rs);
    }
    JS_FreeValue(c, v);
    char devs[16] = "", nicm[24] = "";
    v = JS_GetPropertyStr(c, argv[1], "devices");
    if (JS_IsString(v)) {
        const char *ds = JS_ToCString(c, v);
        if (ds && (!strcmp(ds, "virtio") || !strcmp(ds, "compat"))) { snprintf(devs, sizeof devs, "%s", ds); args[n++] = "--devices"; args[n++] = devs; } else ok = 0;
        if (ds) JS_FreeCString(c, ds);
    }
    JS_FreeValue(c, v);
    v = JS_GetPropertyStr(c, argv[1], "nicModel");
    if (JS_IsString(v)) {
        const char *ns = JS_ToCString(c, v);
        int known = 0;
        for (int k = 0; ns && VM_NIC_MODELS[k]; k++) if (!strcmp(ns, VM_NIC_MODELS[k])) known = 1;
        if (known) { snprintf(nicm, sizeof nicm, "%s", ns); args[n++] = "--nic-model"; args[n++] = nicm; } else ok = 0;
        if (ns) JS_FreeCString(c, ns);
    }
    JS_FreeValue(c, v);
    v = JS_GetPropertyStr(c, argv[1], "transfer");
    if (JS_IsBool(v)) { args[n++] = "--transfer"; args[n++] = JS_ToBool(c, v) ? "on" : "off"; }
    else if (!JS_IsUndefined(v)) ok = 0;
    JS_FreeValue(c, v);
    v = JS_GetPropertyStr(c, argv[1], "clipboardAsk");
    if (JS_IsBool(v)) { args[n++] = "--clipboard-ask"; args[n++] = JS_ToBool(c, v) ? "on" : "off"; }
    else if (!JS_IsUndefined(v)) ok = 0;
    JS_FreeValue(c, v);
    v = JS_GetPropertyStr(c, argv[1], "captureFullscreen");
    if (JS_IsBool(v)) { args[n++] = "--capture-fullscreen"; args[n++] = JS_ToBool(c, v) ? "on" : "off"; }
    else if (!JS_IsUndefined(v)) ok = 0;
    JS_FreeValue(c, v);
    v = JS_GetPropertyStr(c, argv[1], "perfProfile");
    if (JS_IsBool(v)) { args[n++] = "--perf-profile"; args[n++] = JS_ToBool(c, v) ? "on" : "off"; }
    v = JS_GetPropertyStr(c, argv[1], "guestLinks");
    if (JS_IsBool(v)) { args[n++] = "--guest-links"; args[n++] = JS_ToBool(c, v) ? "on" : "off"; }
    v = JS_GetPropertyStr(c, argv[1], "guestNotify");
    if (JS_IsBool(v)) { args[n++] = "--guest-notify"; args[n++] = JS_ToBool(c, v) ? "on" : "off"; }
    else if (!JS_IsUndefined(v)) ok = 0;
    v = JS_GetPropertyStr(c, argv[1], "cpuPin");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        if (s && !strcmp(s, "off")) { args[n++] = "--cpu-pin"; args[n++] = "off"; }
        else if (s && !strcmp(s, "auto")) { args[n++] = "--cpu-pin"; args[n++] = "auto"; }
        else ok = 0;
        JS_FreeCString(c, s);
    } else if (!JS_IsUndefined(v)) ok = 0;
    JS_FreeValue(c, v);
    v = JS_GetPropertyStr(c, argv[1], "pointer");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        if (s && (!strcmp(s, "lordos") || !strcmp(s, "guest"))) { args[n++] = "--pointer"; args[n++] = !strcmp(s, "guest") ? "guest" : "lordos"; } else ok = 0;
        if (s) JS_FreeCString(c, s);
    } else if (!JS_IsUndefined(v)) ok = 0;
    JS_FreeValue(c, v);
    v = JS_GetPropertyStr(c, argv[1], "shares");
    if (JS_IsArray(c, v)) {
        uint32_t len = 0;
        JSValue l = JS_GetPropertyStr(c, v, "length");
        JS_ToUint32(c, &len, l);
        JS_FreeValue(c, l);
        if (len > VM_SHARES) ok = 0;
        else if (!len) { args[n++] = "--shares"; args[n++] = "none"; }
        for (uint32_t i = 0; ok && i < len; i++) {
            JSValue e = JS_GetPropertyUint32(c, v, i), tv = JS_GetPropertyStr(c, e, "tag"), pv = JS_GetPropertyStr(c, e, "path"), wv = JS_GetPropertyStr(c, e, "writable");
            const char *ts = JS_IsString(tv) ? JS_ToCString(c, tv) : NULL, *ps = JS_IsString(pv) ? JS_ToCString(c, pv) : NULL;
            if (ts && ps && JS_ToBool(c, wv) && !share_writable_ok(ps)) { snprintf(refusal, sizeof refusal, "vm.shareProtected"); ok = 0; }
            else if (ts && ps && vm_tag_valid(ts) && ps[0] == '/' && strlen(ps) < VM_PATH_MAX && (JS_IsBool(wv) || JS_IsUndefined(wv))) {
                snprintf(shv[i], sizeof shv[i], "%s:%s:%s", JS_ToBool(c, wv) ? "rw" : "ro", ts, ps);
                args[n++] = "--share"; args[n++] = shv[i];
            } else ok = 0;
            if (ts) JS_FreeCString(c, ts);
            if (ps) JS_FreeCString(c, ps);
            JS_FreeValue(c, tv); JS_FreeValue(c, pv); JS_FreeValue(c, wv); JS_FreeValue(c, e);
        }
    } else if (!JS_IsUndefined(v)) ok = 0;
    JS_FreeValue(c, v);
    /* the boot order, checked here as lord-vm checks it (vm_boot_parse) */
    char boot[32] = "";
    v = JS_GetPropertyStr(c, argv[1], "bootOrder");
    if (JS_IsString(v)) {
        const char *bs = JS_ToCString(c, v);
        int ord[VM_BOOT_DEVS];
        if (bs && strlen(bs) < sizeof boot && vm_boot_parse(bs, ord) >= 0) { snprintf(boot, sizeof boot, "%s", bs); args[n++] = "--boot-order"; args[n++] = boot; } else ok = 0;
        if (bs) JS_FreeCString(c, bs);
    } else if (!JS_IsUndefined(v)) ok = 0;
    JS_FreeValue(c, v);
    v = JS_GetPropertyStr(c, argv[1], "bootMenu");
    if (JS_IsBool(v)) { args[n++] = "--boot-menu"; args[n++] = JS_ToBool(c, v) ? "on" : "off"; }
    else if (!JS_IsUndefined(v)) ok = 0;
    JS_FreeValue(c, v);
    v = JS_GetPropertyStr(c, argv[1], "iso");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        if (s && !strcmp(s, "none")) snprintf(iso, sizeof iso, "none");
        else if (user_file(c, v, iso, sizeof iso)) ok = 0;
        if (s) JS_FreeCString(c, s);
        args[n++] = "--iso"; args[n++] = iso;
    }
    JS_FreeValue(c, v);
    args[n] = NULL;
    return JS_NewInt32(c, ok && n > 3 ? spawn_job(args, dir) : 0);
}

/* stats(dir) → { cpuUsec, memBytes, ioBytes, at } from the running QEMU's cgroup (counters: the app takes the
 * differences), diskBytes (what the machine's disks use on this computer), or only diskBytes when it's off.
 * While it runs, also what Sentinela's machine rules check (docs/SENTINELA.md, "Machines"), read-only and never from
 * run.json (the person's file, a hint only): verified (false: run.json says it runs but its lord-vm didn't answer as the
 * installed lord-vm, of this user, with that QEMU as its child), and when true qemuPid, sandbox and cgMode (the VMS_*
 * bits and VMC_* mode the machine's lord-vm reports over its socket, as `lord-vm --status`), seccomp and noNewPrivs (the
 * kernel's /proc/<qemu>/status), scope ("in": QEMU and lord-vm exactly in the session's app-lordos-lordos-vm-<id>.scope
 * or below it, "out" otherwise), passt (its passt process alive) */
static long long cg_num(const char *cg, const char *file, const char *key) {
    char p[PATH_MAX + 64], buf[4096];
    snprintf(p, sizeof p, "/sys/fs/cgroup%s/%s", cg, file);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t k = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (k <= 0) return -1;
    buf[k] = 0;
    if (!key) return atoll(buf);
    long long sum = 0;
    int found = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strcmp(key, "io")) {                      /* io.stat: "maj:min rbytes=N wbytes=N …" per device */
            char *r = strstr(line, "rbytes="), *w = strstr(line, "wbytes=");
            if (r) sum += atoll(r + 7);
            if (w) sum += atoll(w + 7);
            found = 1;
        } else if (!strncmp(line, key, strlen(key)) && line[strlen(key)] == ' ') { sum = atoll(line + strlen(key) + 1); found = 1; break; }
    }
    return found ? sum : -1;
}
/* ---- what Sentinela checks of a running machine (docs/SENTINELA.md, "Machines") ---- */

/* /proc/<pid>/status: PPid, Seccomp (2: a filter), NoNewPrivs */
static int proc_status(long pid, long *ppid, int *sec, int *nnp) {
    char p[64], st[4096];
    snprintf(p, sizeof p, "/proc/%ld/status", pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    ssize_t k = fd >= 0 ? read(fd, st, sizeof st - 1) : -1;
    if (fd >= 0) close(fd);
    if (k <= 0) return -1;
    st[k] = 0;
    *ppid = -1; *sec = -1; *nnp = -1;
    for (char *line = st; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (!strncmp(line, "PPid:", 5)) *ppid = atol(line + 5);
        else if (!strncmp(line, "Seccomp:", 8)) *sec = atoi(line + 8);
        else if (!strncmp(line, "NoNewPrivs:", 11)) *nnp = atoi(line + 11);
    }
    return *ppid < 0 ? -1 : 0;
}
static long proc_uid(long pid) {
    char p[64], st[4096];
    snprintf(p, sizeof p, "/proc/%ld/status", pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    ssize_t k = fd >= 0 ? read(fd, st, sizeof st - 1) : -1;
    if (fd >= 0) close(fd);
    if (k <= 0) return -1;
    st[k] = 0;
    const char *u = strstr(st, "\nUid:");
    return u ? atol(u + 5) : -1;
}

/* the process runs the installed program `name` (the one next to lord-server, bin_path): an upgraded one shows as
 * "(deleted)" and still counts */
static int vm_exe_is(long pid, const char *name) {
    char want[PATH_MAX], got[PATH_MAX], p[64];
    if (pid <= 1 || bin_path(name, want, sizeof want)) return 0;
    snprintf(p, sizeof p, "/proc/%ld/exe", pid);
    ssize_t k = readlink(p, got, sizeof got - 1);
    if (k <= 0) return 0;
    got[k] = 0;
    char rw[PATH_MAX];
    return realpath(want, rw) && vmwatch_exe_same(got, rw);
}

/* where lord-sessiond puts a machine's scope: beside lord-server's own leaf (".../lordos-session-<uid>/server"), the
 * exact cgroup path; "" outside a LordOS session */
static void vm_scope_path(const char *id, int tor, char *out, size_t n) { vmwatch_scope_path(cgroup_self(), id, tor, out, n); }

/* the process's cgroup (v2) is exactly `scope` or below it */
static int vm_in_cgroup(long pid, const char *scope) {
    char p[64], buf[PATH_MAX + 8];
    snprintf(p, sizeof p, "/proc/%ld/cgroup", pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    ssize_t k = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    if (k <= 3) return 0;
    buf[k] = 0;
    return vmwatch_cg_under(buf, scope);
}

/* the machine's own lord-vm, asked over its socket (vmwatch_live) */
static int vm_live(const char *id, VmLive *lv) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char base[256], path[sizeof ((struct sockaddr_un *)0)->sun_path];
    if (rt && rt[0] == '/') snprintf(base, sizeof base, "%s", rt); else snprintf(base, sizeof base, "/run/user/%u", (unsigned)getuid());
    if ((size_t)snprintf(path, sizeof path, "%s/lordos-vm/%s.sock", base, id) >= sizeof path) return -1;
    return vmwatch_live(path, lv, 150);
}
static JSValue f_stats(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    char dir[PATH_MAX];
    if (machine_dir(c, argv[0], dir, sizeof dir)) return JS_NULL;
    JSValue o = JS_NewObject(c);
    double disk = 0;
    char p[PATH_MAX + 64];
    const char *files[] = { "system.qcow2", "nvram.qcow2" };
    for (int i = 0; i < 2; i++) {
        struct stat st;
        snprintf(p, sizeof p, "%s/disks/%s", dir, files[i]);
        if (!stat(p, &st)) disk += (double)st.st_blocks * 512;
    }
    JS_SetPropertyStr(c, o, "diskBytes", JS_NewFloat64(c, disk));
    {
        /* a linked clone's frozen base (<library>/.shared/, machine.json "base"): shared with its siblings, so shown
         * beside the machine's own size, never added to it */
        static VmMachine bm;
        char berr[128];
        struct stat st;
        if (!vm_machine_load(dir, &bm, berr, sizeof berr) && bm.base[0] && vm_base_valid(bm.base) && !stat(bm.base, &st) && S_ISREG(st.st_mode))
            JS_SetPropertyStr(c, o, "baseBytes", JS_NewFloat64(c, (double)st.st_blocks * 512));
    }
    /* run.json is the person's own file: only a hint (which QEMU the Machines cards count, whether to ask lord-vm) */
    snprintf(p, sizeof p, "%s/run.json", dir);
    char buf[2048];
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    long qpid = 0, hint_passt = 0;
    if (n > 0) {
        JDoc d;
        if (!json_parse(buf, (size_t)n, &d, NULL, 0)) {
            const char *s = json_str(d.root, "state");
            if (s && (!strcmp(s, "running") || !strcmp(s, "paused"))) {
                qpid = (long)json_num(d.root, "qemu_pid", 0);
                hint_passt = (long)json_num(d.root, "passt_pid", 0);
            }
            json_free(&d);
        }
    }
    /* Sentinela's facts (docs/SENTINELA.md, "Machines") come from the machine's lord-vm itself, never from run.json */
    static VmMachine sm;
    char serr[128], scope[PATH_MAX] = "";
    int have_m = !vm_machine_load(dir, &sm, serr, sizeof serr) && sm.id[0];
    if (have_m) vm_scope_path(sm.id, sm.uplink_tor, scope, sizeof scope);
    if (qpid > 0 && have_m) {
        VmLive lv;
        int ok = !vm_live(sm.id, &lv);
        if (ok && (lv.state == VS_RUNNING || lv.state == VS_PAUSED)) {
            /* the peer of the machine's socket is the installed lord-vm, the person's, and the QEMU it names is its child */
            long pp = -1;
            int sec = -1, nnp = -1;
            ok = lv.uid == getuid() && vm_exe_is(lv.pid, "lord-vm") && lv.qemu > 1 && !proc_status(lv.qemu, &pp, &sec, &nnp) && pp == lv.pid;
            JS_SetPropertyStr(c, o, "verified", JS_NewBool(c, ok));
            if (ok) {
                qpid = lv.qemu;
                JS_SetPropertyStr(c, o, "qemuPid", JS_NewInt32(c, (int32_t)lv.qemu));
                set_int(c, o, "seccomp", sec);
                set_int(c, o, "noNewPrivs", nnp);
                set_int(c, o, "sandbox", lv.sandbox);
                set_str(c, o, "cgMode", lv.cg & VMC_NESTED ? "nested" : lv.cg & VMC_SCOPE ? "scope" : "none");
                /* in its own machine's scope (exactly that cgroup or below it), lord-vm and QEMU both; "" when this
                 * isn't a LordOS session and the scope's place isn't known */
                if (scope[0]) set_str(c, o, "scope", vm_in_cgroup(lv.qemu, scope) && vm_in_cgroup(lv.pid, scope) ? "in" : "out");
                /* its passt (Shared network): a passt of the person's, lord-vm's child or in the machine's scope */
                long pt = hint_passt, ppp = -1;
                int alive = 0, d1, d2;
                if (pt > 1 && pt <= 999999999 && !proc_status(pt, &ppp, &d1, &d2)) {
                    char comm[32] = "";
                    snprintf(p, sizeof p, "/proc/%ld/comm", pt);
                    int cf = open(p, O_RDONLY | O_CLOEXEC);
                    ssize_t k = cf >= 0 ? read(cf, comm, sizeof comm - 1) : -1;
                    if (cf >= 0) close(cf);
                    if (k > 0) comm[k] = 0;
                    alive = k > 0 && !strncmp(comm, "passt", 5) && proc_uid(pt) == getuid() && (ppp == lv.pid || (scope[0] && vm_in_cgroup(pt, scope)));
                }
                JS_SetPropertyStr(c, o, "passt", JS_NewBool(c, alive));
            }
        } else if (!ok) {
            /* no answer: unverified unless the machine stopped meanwhile */
            int still = 0;
            fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
            if (fd >= 0) close(fd);
            if (n > 0) { buf[n] = 0; still = strstr(buf, "\"state\":\"running\"") || strstr(buf, "\"state\":\"paused\""); }
            if (still) JS_SetPropertyStr(c, o, "verified", JS_FALSE);
        }
    }
    char cg[PATH_MAX] = "";
    if (qpid > 0) {
        snprintf(p, sizeof p, "/proc/%ld/cgroup", qpid);
        int cf = open(p, O_RDONLY | O_CLOEXEC);
        ssize_t k = cf >= 0 ? read(cf, buf, sizeof buf - 1) : -1;
        if (cf >= 0) close(cf);
        if (k > 3 && !strncmp(buf, "0::", 3)) {
            buf[k] = 0;
            char *nl = strchr(buf, '\n');
            if (nl) *nl = 0;
            /* QEMU's own leaf, or the machine's scope when lord-vm couldn't divide it; never an unrelated cgroup */
            if (strstr(buf + 3, "lordos-vm-") && strlen(buf + 3) < sizeof cg) snprintf(cg, sizeof cg, "%s", buf + 3);
            /* with the scope's place known: its counters only when QEMU is really under it */
            if (scope[0] && !vm_in_cgroup(qpid, scope)) cg[0] = 0;
        }
    }
    if (cg[0]) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        JS_SetPropertyStr(c, o, "at", JS_NewFloat64(c, ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6));
        JS_SetPropertyStr(c, o, "cpuUsec", JS_NewFloat64(c, (double)cg_num(cg, "cpu.stat", "usage_usec")));
        JS_SetPropertyStr(c, o, "memBytes", JS_NewFloat64(c, (double)cg_num(cg, "memory.current", NULL)));
        JS_SetPropertyStr(c, o, "ioBytes", JS_NewFloat64(c, (double)cg_num(cg, "io.stat", "io")));
    }
    return o;
}

/* snapshot(session) → bool: the machine's picture now as its card picture (<machine>/thumb.png) */
static JSValue f_snapshot(JSContext *c, JSValueConst t, int argc, JSValueConst *argv);

/* isoInfo(path) → { label } : the ISO 9660 volume label (32 bytes at 0x8028), to recognize the system in it */
static JSValue f_isoinfo(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    char path[PATH_MAX];
    if (user_file(c, argv[0], path, sizeof path)) return JS_NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return JS_NULL;
    unsigned char pvd[72];
    ssize_t k = pread(fd, pvd, sizeof pvd, 0x8000);
    close(fd);
    JSValue o = JS_NewObject(c);
    char label[33] = "";
    if (k == (ssize_t)sizeof pvd && pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5)) {
        memcpy(label, pvd + 40, 32);
        for (int i = 31; i >= 0 && (label[i] == ' ' || !label[i]); i--) label[i] = 0;
        for (int i = 0; label[i]; i++) if ((unsigned char)label[i] < 0x20 || (unsigned char)label[i] > 0x7e) label[i] = '?';
    }
    set_str(c, o, "label", label);
    return o;
}

/* ---- a person's system: its OpenPGP signature (lord-pgpv; docs/VIRTUALIZATION.md 5.6.1, AGENTS.md "lord-pgpv") ---- */

#define VM_USER_LISTS "/var/lib/lordos/machines/lists/"

/* a root-owned regular file of the image store, read whole (at most max bytes) */
static char *store_file(const char *path, size_t max, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    char *b = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022) && st.st_size > 0 &&
        (size_t)st.st_size <= max && (b = malloc((size_t)st.st_size + 1))) {
        size_t n = 0;
        while (n < (size_t)st.st_size) {
            ssize_t r = read(fd, b + n, (size_t)st.st_size - n);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            n += (size_t)r;
        }
        if (n != (size_t)st.st_size) { free(b); b = NULL; } else { b[n] = 0; *len = n; }
    }
    close(fd);
    return b;
}

/* bytes into a sealed memfd at offset 0 (-1 on failure) */
static int sealed_fd(const char *name, const void *b, size_t n) {
    int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    size_t o = 0;
    while (o < n) {
        ssize_t w = write(fd, (const char *)b + o, n - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { close(fd); return -1; }
        o += (size_t)w;
    }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) || lseek(fd, 0, SEEK_SET)) { close(fd); return -1; }
    return fd;
}

/* sigCheck(sha256, key, fingerprint) → job id (0 when it can't start): checks the detached signature that
 * vm-image-get-user stored for that image (lists/u-<sha256>.sig, over lists/u-<sha256>.sums) with the armored public
 * key of the system's definition, of which only the primary key with that v4 fingerprint (or its subkeys) counts.
 * lord-pgpv does the OpenPGP part in its sandbox on sealed memfds; then, only when it answered valid with that very
 * fingerprint, the image's SHA-256 must be a line of the checksum file it checked. lord.vm.job(id).out is then
 * {"valid":true,"fingerprint":"<40 hex>"} or {"valid":false,"reason":"<word>"} (VMSIG_REASONS, notListed, noFiles). */
static JSValue f_sig_check(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 3 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]) || !JS_IsString(argv[2])) return JS_NewInt32(c, 0);
    int slot = -1;
    for (int i = 0; i < MAX_JOBS; i++) if (!jobs[i].used) { slot = i; break; }
    char bin[PATH_MAX];
    if (slot < 0 || bin_path("lord-pgpv", bin, sizeof bin)) return JS_NewInt32(c, 0);
    size_t kl = 0;
    const char *sha = JS_ToCString(c, argv[0]), *fpr = JS_ToCString(c, argv[2]), *key = JS_ToCStringLen(c, &kl, argv[1]);
    char s[65] = "", f[41] = "";
    int ok = sha && fpr && key && strlen(sha) == 64 && vmsig_fpr_ok(fpr) && kl > 0 && kl <= VMSIG_KEY_MAX;
    for (int i = 0; ok && i < 64; i++) {
        char ch = sha[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) ok = 0;
        s[i] = ch;
    }
    for (int i = 0; ok && i < 40; i++) f[i] = (char)(fpr[i] >= 'a' && fpr[i] <= 'f' ? fpr[i] - 32 : fpr[i]);
    int kfd = ok ? sealed_fd("lordos-pgpv-key", key, kl) : -1;
    if (sha) JS_FreeCString(c, sha);
    if (fpr) JS_FreeCString(c, fpr);
    if (key) JS_FreeCString(c, key);
    if (kfd < 0) return JS_NewInt32(c, 0);
    char p[160];
    size_t sl = 0, gl = 0;
    snprintf(p, sizeof p, VM_USER_LISTS "u-%s.sums", s);
    char *sums = store_file(p, VMSIG_SUMS_MAX, &sl);
    snprintf(p, sizeof p, VM_USER_LISTS "u-%s.sig", s);
    char *sig = store_file(p, VMSIG_SIG_MAX, &gl);
    int sfd = sums ? sealed_fd("lordos-pgpv-sums", sums, sl) : -1, gfd = sig ? sealed_fd("lordos-pgpv-sig", sig, gl) : -1;
    int have_files = sums && sig;
    free(sig);
    int in[2] = { -1, -1 }, out[2] = { -1, -1 };
    pid_t pid = -1;
    if (sfd >= 0 && gfd >= 0 && !pipe2(in, O_CLOEXEC) && !pipe2(out, O_CLOEXEC)) {
        pid = fork();
        if (!pid) {
            /* fds 0 (the request), 1 (the answer), 2 (/dev/null), 3 key, 4 checksum file, 5 signature; nothing else */
            int a = fcntl(in[0], F_DUPFD, 10), b = fcntl(out[1], F_DUPFD, 10), k = fcntl(kfd, F_DUPFD, 10),
                m = fcntl(sfd, F_DUPFD, 10), g = fcntl(gfd, F_DUPFD, 10), null = open("/dev/null", O_WRONLY);
            if (a < 0 || b < 0 || k < 0 || m < 0 || g < 0 || null < 0 || dup2(a, 0) != 0 || dup2(b, 1) != 1 || dup2(null, 2) != 2 ||
                dup2(k, 3) != 3 || dup2(m, 4) != 4 || dup2(g, 5) != 5) _exit(127);
            syscall(SYS_close_range, 6, ~0u, 0);
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, NULL);
            char *const args[] = { "lord-pgpv", NULL }, *const envp[] = { NULL };
            execve(bin, args, envp);
            _exit(127);
        }
    }
    close(kfd);
    if (sfd >= 0) close(sfd);
    if (gfd >= 0) close(gfd);
    if (in[0] >= 0) close(in[0]);
    if (out[1] >= 0) close(out[1]);
    if (pid <= 0) {
        if (in[1] >= 0) close(in[1]);
        if (out[0] >= 0) close(out[0]);
        free(sums);
        /* no signature files stored for that image (downloaded before, or a failed fetch): a finished job that says so */
        if (!have_files) {
            jobs[slot] = (Job){ .used = 1, .fd = -1, .done = 1, .ok = 0, .exit = 1, .pid = 0 };
            jobs[slot].n = (size_t)snprintf(jobs[slot].out, sizeof jobs[slot].out, "{\"valid\":false,\"reason\":\"noFiles\"}");
            return JS_NewInt32(c, slot + 1);
        }
        return JS_NewInt32(c, 0);
    }
    char req[64];
    int rl = snprintf(req, sizeof req, "%s %lld\n", f, (long long)time(NULL));
    if (write(in[1], req, (size_t)rl) != rl) { /* the helper answers malformed */ }
    close(in[1]);
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    jobs[slot] = (Job){ .used = 1, .fd = out[0], .pid = pid, .sigcheck = 1, .sums = sums, .sums_n = sl };
    memcpy(jobs[slot].sig_fpr, f, 41);
    memcpy(jobs[slot].sig_sha, s, 65);
    return JS_NewInt32(c, slot + 1);
}

/* the verdict of a finished lord-pgpv job, built here: never the helper's text */
static void sig_verdict(Job *j) {
    char why[32] = "";
    int rep = vmsig_reply(j->out, j->n, j->sig_fpr, why, sizeof why);
    int listed = rep == 1 && j->sums && vmsig_sums_has_hash(j->sums, j->sums_n, j->sig_sha);
    char fpr[41];
    memcpy(fpr, j->sig_fpr, sizeof fpr);
    if (rep == 1 && listed)
        j->n = (size_t)snprintf(j->out, sizeof j->out, "{\"valid\":true,\"fingerprint\":\"%s\"}", fpr);
    else
        j->n = (size_t)snprintf(j->out, sizeof j->out, "{\"valid\":false,\"reason\":\"%s\"}", rep == 1 ? "notListed" : why[0] ? why : "badSignature");
    j->ok = rep == 1 && listed;
    free(j->sums);
    j->sums = NULL;
}

static JSValue f_job(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    int i = ival(c, argv[0]);
    if (i < 1 || i > MAX_JOBS || !jobs[i - 1].used) return JS_NULL;
    Job *j = &jobs[i - 1];
    if (!j->done) {
        char tmp[1024];
        ssize_t r;
        while ((r = read(j->fd, tmp, sizeof tmp)) > 0) {
            /* a long job (an import's progress lines) keeps its last part: the newest lines and the final answer */
            if (j->n + (size_t)r > sizeof j->out - 1) {
                size_t drop = j->n + (size_t)r - (sizeof j->out - 1);
                if (drop > j->n) drop = j->n;
                memmove(j->out, j->out + drop, j->n - drop);
                j->n -= drop;
            }
            size_t k = (size_t)r < sizeof j->out - 1 - j->n ? (size_t)r : sizeof j->out - 1 - j->n;
            memcpy(j->out + j->n, tmp, k);
            j->n += k;
        }
        j->out[j->n] = 0;
        int st;
        if (r == 0 && waitpid(j->pid, &st, WNOHANG) == j->pid) {
            j->done = 1;
            j->ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
            j->exit = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            close(j->fd);
            if (j->ok && j->hash[0]) {
                char p[PATH_MAX + 120];
                snprintf(p, sizeof p, "%s/firstboot.creds", j->dir);
                int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
                if (fd >= 0) { dprintf(fd, "passwd.hashed-password.root=%s\n", j->hash); close(fd); }
            }
            explicit_bzero(j->hash, sizeof j->hash);
            if (j->creds[0]) unlink(j->creds);
            if (j->sigcheck) sig_verdict(j);
        }
    }
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "done", JS_NewBool(c, j->done));
    JS_SetPropertyStr(c, o, "ok", JS_NewBool(c, j->ok));
    set_int(c, o, "exit", j->exit);
    set_str(c, o, "out", j->out);
    set_str(c, o, "dir", j->dir);
    if (j->done) memset(j, 0, sizeof *j);
    return o;
}

/* ---- sessions: one running machine and its window ---- */

#define MAX_SESS 4
/* one dmabuf of a 3D machine's picture (VD_DMABUF) and its import: a texture of the desktop's GPU (tex) or of the reader
 * on the buffer's own render node (ri, gpu/dmaread.h); gfail/rfail: that one refused it (not tried again for this dmabuf) */
typedef struct { int fd; ino_t ino; int w, h, stride, flip; uint32_t fourcc; uint64_t mod; GpuTex *tex; DmaImg *ri; int gfail, rfail; unsigned used, gen; } VmDmabuf;
typedef struct {
    int used, vm, vd, err;              /* channels; lord-vm's stderr (until its log is open) */
    pid_t vm_pid, vd_pid;
    char dir[PATH_MAX], id[37];
    int state, reason, code, vcpus, mem, sandbox, cg, have_info;
    char detail[VM_TEXT_MAX];
    uint32_t seq, display_seq;
    int display_asked, display_failed, started_paused, resumed;
    int vd_state, vd_up;                /* VDS_*; the helper said it's connected */
    int media, w, h;
    int ack_pending, ack_frame;         /* a frame to acknowledge once painted */
    unsigned ack_seq;
    long frames;
    int ended, exit_code;
    char last_error[VM_TEXT_MAX];
    char errtail[400];
    char creds[PATH_MAX];               /* the first-boot settings file, removed once lord-vm read it */
    uint8_t *cur;                       /* the guest's pointer (VD_CURSOR) until there's a picture to give it to */
    int vgpu;                           /* the machine has an Intel vGPU (gvt_memlock while it starts) */
    int windows;                        /* a Windows guest: its display driver draws the pointer into the picture until
                                         * it hands one over (viogpudo's HWCursor, 7.1.2). Not with a vGPU: Intel's
                                         * driver puts it on the vGPU's cursor plane, which QEMU hands over as
                                         * CursorDefine; until one comes, the host's arrow */
    int ptr_guest;                      /* machine.json "pointer": "guest": a shape QEMU still holds from before (a saved
                                         * state's) isn't drawn over the guest's own */
    int cur_w, cur_h, cur_hx, cur_hy;
    int job_kind, job_status, job_pct;  /* the last VM_JOB (suspend, resume, snapshots) */
    char job_text[VM_TEXT_MAX];
    uint32_t cd_seq;                    /* VM_CDROM: the request, and how it went (0 none, 1 waiting, 2 done, 3 failed) */
    int cd_state;
    char cd_msg[VM_TEXT_MAX];
    int ag_state, ag_caps;              /* the guest agent (VM_AGENT_STATE), and what it said about itself */
    int ag_seen;                        /* it was connected in this run of the guest (a restart forgets it): "lost" when
                                         * it stops answering while the guest runs (the light's red state, 6.17) */
    char ag_text[VM_TEXT_MAX];
    uint32_t ag_seq;                    /* an Install Agent request, and how it went (0 none, 1 waiting, 2 done, 3 failed) */
    uint32_t xf_seq;                    /* a Set Up the Transfer Folder request (VAG_TRANSFER), the same way */
    int xf_result;
    char xf_msg[VM_TEXT_MAX];
    int ag_result;
    char ag_msg[VM_TEXT_MAX];
    char *clip_in;                      /* the guest's clipboard text (VM_CLIP_DATA), and a counter of them */
    int clip_seq;
    char *clip_html;                    /* its rich text (sanitized by lord-vm) and picture (header checked by lord-vm,
                                         * re-encoded by lord-media before it's used), when it came as VCF_CLIPX */
    uint8_t *clip_png;
    size_t clip_html_n, clip_png_n;
    uint64_t clip_last;                 /* what went either way last (its main form's hash): not sent back */
    unsigned clip_skip_img;             /* the clipboard picture made from the guest's (wl_clip_image_gen): not sent back */
    /* the clipboard's gate (6.8): the machine is in use while its window (or one of its apps mode windows) is in front,
     * and CLIP_GRACE_MS after; only then does the guest get the host's clipboard when it asks, or set the host's.
     * The app says which (clipboardGate); this side holds it, so a guest can't reach either way in the background */
    int gate_on, clip_ask;              /* in use now; "ask before each paste" (the app asks the person) */
    long long gate_until;               /* in use until then (the grace after it went to the background) */
    uint32_t offer_gen;                 /* the last offer (VM_CLIP_OFFER): generation, forms, the host clipboard's hash */
    int offer_forms;
    uint64_t offer_hash;
    uint32_t ask_gen[4];                /* ask mode: requests waiting for the person, per AGF_* form (0 none) */
    long long ask_at[4];
    int ask_seq;                        /* a counter of requests put to the person */
    int blk_read, blk_write;            /* requests refused, and guest clipboards ignored, while not in use */
    int console_term;                   /* the Shell (6.18): a terminal session on the serial console's stream */
    uint32_t console_seq;
    /* the agent's shell (6.18, VM_SHELL): a terminal session on its stream, and the size the guest was last told */
    int shell_term;
    uint32_t shell_seq;
    int shell_cols, shell_rows;
    /* links and notifications from the guest (6.12, VM_GUEST_NOTE, already checked by lord-vm and again here). One link
     * waits for the person at a time (another one meanwhile is dropped: never swapped in under the pointer); it is
     * offered only while the machine is in use, and opens only through guestLinkAnswer, no sooner than LINK_MIN_MS after
     * the app first saw it. Notifications wait for the app in a small queue (a full one drops) */
    char link[AG_LINK_MAX + 1];
    uint32_t link_seq;
    long long link_seen;                /* when the app first saw it (0: not yet) */
    int link_dropped;
    struct { char title[AG_NOTE_TITLE_MAX + 1], body[AG_NOTE_BODY_MAX + 1]; } notes[8];
    int note_n, note_seq, note_dropped;
    int rec;                            /* a recording of the picture (6.20): its media source, fed as frames come */
    int protect;                        /* a protected machine's (lord.vm.protect): its picture never leaves as a picture */
    double rec_fed;
    int rec_sound;                      /* lord-vmdisplay writes the guest's sound into the recording's ring */
    int notice, notice_code, notice_seq; /* VM_NOTICE: VMN_*, its number, a counter of them, and its words */
    char notice_text[VM_TEXT_MAX];
    /* 3D (VD_DMABUF): the dmabufs QEMU draws into, imported once each (QEMU flips between a few, and names each
     * again on every ScanoutDMABUF: known by inode), the one shown now, and the rows still to read back into the
     * picture (their frame waits until the dmabuf's fence has signalled: never a wait) */
    VmDmabuf db[4];
    int db_cur;                         /* index in db, -1: the picture isn't a dmabuf */
    unsigned db_tick;
    int db_y0, db_y1, db_frame, db_pending;
    long db_reads, db_fails;
    int db_warned;
    /* With the GPU composing, the picture is shown as a texture, never read back (docs/MACHINES-GPU.md, "A vGPU's
     * picture without a readback"): the desktop GPU's own import of QEMU's dmabuf (VmDmabuf.tex, a Linux 3D machine),
     * or for an Intel vGPU's picture two linear copies made on the iGPU (gout), imported by the desktop's GPU: one is
     * shown (gshow) while the next frame is copied into the other (gpend, until its fence on the copy's dmabuf
     * signals). gstale: the rows each copy lacks (gpu/gband.h: the frames copied into the other one since; all of them
     * for a copy just made or imported, and for both when the picture's source changes). The copy also tests whether
     * anything changed (QEMU's vfio display sends the whole picture every refresh, changed or not): a frame that
     * didn't is acknowledged at once and nothing is presented */
    struct { DmaOut *o; GpuTex *tex; int w, h; } gout[2];
    GBand gstale[2];
    unsigned gsrc;                      /* the dmabuf (VmDmabuf.gen) the copies were last made from; 0 none */
    unsigned db_gen;
    int gshow, gpend, gfail;
    int gcopy_y0, gcopy_y1;             /* the frame's rows in the copy in flight */
    int db_more;                        /* frames came while a copy was in flight: another copy after it */
    long db_same;                       /* frames that changed nothing (not shown again) */
    /* pacing (vm/vmpace.h, VD_PACE): what lord-vmdisplay was told, whether any window shows the picture (the app says,
     * lord.vm.shown), frames not read while none did (the next one is read whole), and counters for Logs and tests */
    VmPace pace;
    int pic_hidden, db_skipped;
    long pic_skipped;
    unsigned rkey;                      /* names the picture in map for dmaread_rows_new's change test */
    /* sound (6.9): machine.json's choices, what the guest does now (VD_GUEST_AUDIO), and what this side has open. The
     * speakers' stream and the microphone's belong to the picture's media source (m->audio, m->mic), so lord-server's
     * loop serves them; their rings go to lord-vmdisplay (VD_AUDIO_OUT, VD_MIC) */
    int snd_on, snd_vol, mic_allowed;
    char snd_out[VM_SOUND_DEV_MAX + 1], snd_in[VM_SOUND_DEV_MAX + 1];   /* the output and input chosen ("" the default) */
    int guest_out, guest_in;
    int spk_sent, mic_sent;             /* the current helper has the speakers' / microphone's ring */
    int mic_wait;                       /* the guest records and the machine allows it, LordOS's permission not (yet) */
    int spk_vol, spk_play;              /* what the speakers' stream was last given */
    long long snd_retry_at;             /* a stream without PipeWire (not there yet, or gone) tries again then */
    char name[VM_NAME_MAX + 1];
    /* files dropped on the window, on their way into the guest (VM_FILE): 1 copying, 2 done, 3 failed; msg is the
     * guest's path (done) or why not, for the app to show; files() hands finished ones over once */
    struct { uint32_t seq; int state, pct, via, open, how; long long size; char name[VM_TEXT_MAX], msg[VM_TEXT_MAX]; } fx[16];
    int fx_changes;
    /* apps mode (7.2): the guest's windows, their icons and its apps, as lord-vm passed them on (VM_WINDATA, parsed
     * again here); seqs count changes for the app; apps_mode: the picture may have no node that shows all of it */
    LagWinList *wins;
    struct { uint32_t id; int w, h; uint8_t *rgba; } icons[LAG_WIN_MAX];
    LagAppList *gapps;
    int wins_seq, icons_seq, gapps_seq, gapps_gen, apps_mode;
    /* closes the guest refused (VM_WINREFUSED with LAGA_CLOSE): the last per window, newest last; closeRefusedSeq */
    struct { uint32_t gid; int reason; double at; } refused[8];
    int layout[6];                      /* VM_GUEST_LAYOUT, handed to lord-vmdisplay (VD_LAYOUT); 0s none */
    int refused_n, refused_seq;
    /* "Capture mouse" (6.25): the pointer lock is this session's (pointerlock.h); how many captures ended and why the last one
     * did (PTRLOCK_END_*), for the window */
    int captured, cap_ends, cap_why;
} Sess;
static Sess sess[MAX_SESS];

static void win_free(Sess *s) {
    free(s->wins); s->wins = NULL;
    free(s->gapps); s->gapps = NULL;
    for (int i = 0; i < LAG_WIN_MAX; i++) { free(s->icons[i].rgba); s->icons[i].rgba = NULL; s->icons[i].id = 0; }
    s->refused_n = 0;
}

/* a close the guest refused, kept for closeRefusals(): the same window's earlier one goes, the oldest when full */
static void refused_add(Sess *s, uint32_t gid, int reason) {
    int n = 0;
    for (int i = 0; i < s->refused_n; i++) if (s->refused[i].gid != gid) s->refused[n++] = s->refused[i];
    if (n == (int)(sizeof s->refused / sizeof *s->refused)) { memmove(s->refused, s->refused + 1, (size_t)(n - 1) * sizeof *s->refused); n--; }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    s->refused[n].gid = gid;
    s->refused[n].reason = reason;
    s->refused[n].at = (double)ts.tv_sec * 1000.0 + (double)(ts.tv_nsec / 1000000);
    s->refused_n = n + 1;
    s->refused_seq++;
}

/* lord-vm processes whose window closed before they exited */
static pid_t orphans[16];
static void later(pid_t p) {
    for (int i = 0; i < 16; i++) if (!orphans[i]) { orphans[i] = p; return; }
}
static void reap_orphans(void) {
    for (int i = 0; i < 16; i++) if (orphans[i] && waitpid(orphans[i], NULL, WNOHANG) != 0) orphans[i] = 0;
}

static Sess *get(JSContext *c, JSValueConst v) {
    int i = ival(c, v);
    return i >= 1 && i <= MAX_SESS && sess[i - 1].used ? &sess[i - 1] : NULL;
}

static int new_slot(void) { for (int i = 0; i < MAX_SESS; i++) if (!sess[i].used) return i; return -1; }

/* May the session's picture leave as a picture of its own (the card's thumb.png, a screenshot, a recording, a snapshot's
 * picture)? Not a protected machine's (lord.vm.protect), nor one shown inside a protected window: media_src_exportable,
 * the one gate for a media source's pixels (media/capture.c; docs/PRIVACY.md "Protected windows"). */
static int pic_out_ok(Sess *s, MediaSrc *m) {
    if (s && s->protect) { media_src_protect(m); return 0; }
    return m && media_src_exportable(m);
}

static int ask(Sess *s, uint16_t type, int a) {
    if (s->vm < 0) return -1;
    VmMsg m = vm_msg(type, ++s->seq, a, 0, 0, 0, NULL);
    return vm_send(s->vm, &m, -1) ? -1 : (int)s->seq;
}
static void vdsend(Sess *s, int type, int a, int b) {
    if (s->vd < 0) return;
    VdMsg m = vd_msg((uint16_t)type, a, b, 0, 0, 0, 0);
    vd_send(s->vd, &m, -1);
    /* input ends an idle pace (the helper ends it itself: vmpace.h) */
    if (type == VD_KEY || type == VD_MOTION || type == VD_BUTTON || type == VD_WHEEL || type == VD_REL) vmpace_input(&s->pace);
}

/* the picture's recording ends now (finished properly: the file keeps what was recorded before), its sound with it */
static void rec_end(Sess *s, MediaSrc *r) {
    if (r) media_src_close(r);
    s->rec = 0;
    if (s->rec_sound) { vdsend(s, VD_AUDIO_REC, 0, 0); s->rec_sound = 0; }
}

/* VD_PACE when what the picture needs changed (vm/vmpace.h): a picture no window shows (and nothing records) is taken
 * VMPACE_HIDDEN_MS apart, a vGPU's that keeps not changing VMPACE_IDLE_MS apart; a Linux 3D (virgl) one never in the
 * helper */
static void pace_tell(Sess *s) {
    if (s->vd < 0 || !s->vd_up) return;
    int ms = 0, kind = 0;
    int virgl = s->db_cur >= 0 && !s->vgpu;
    if (!vmpace_step(&s->pace, s->pic_hidden && !s->rec, virgl, &ms, &kind)) return;
    VdMsg m = vd_msg(VD_PACE, ms, kind, 0, 0, 0, 0);
    vd_send(s->vd, &m, -1);
}

static void init_sess(Sess *s, const char *dir) {
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->vm = s->vd = s->err = -1;
    s->state = VS_STARTING;
    s->vd_state = -1;
    s->db_cur = -1;
    for (int i = 0; i < 4; i++) s->db[i].fd = -1;
    s->gshow = s->gpend = -1;
    snprintf(s->dir, sizeof s->dir, "%s", dir);
    VmMachine m;
    if (!vm_machine_load(dir, &m, NULL, 0)) {
        s->snd_on = m.sound; s->snd_vol = m.volume; s->mic_allowed = m.microphone; snprintf(s->name, sizeof s->name, "%s", m.name);
        s->windows = m.devices == VM_DEV_WINDOWS;
        s->vgpu = m.vgpu != VM_VGPU_NONE;
        s->ptr_guest = s->windows && !s->vgpu && m.pointer_guest;   /* HWCursor is viogpudo's: a vGPU's plane is always drawn here */
        memcpy(s->snd_out, m.sound_out, sizeof s->snd_out);
        memcpy(s->snd_in, m.sound_in, sizeof s->snd_in);
    }
    else { s->snd_on = 1; s->snd_vol = 100; }
}

/* ---- sound (6.9) ----
 * The guest's sound plays from lord-server's own PipeWire stream (media/maudio.c), fed from a ring lord-vmdisplay writes;
 * the guest's microphone reads a ring lord-server fills from the person's microphone. lord-vmdisplay never gets the
 * PipeWire socket. The microphone opens only while all three hold: the machine's "microphone" is on, LordOS's
 * microphone permission allows Machines (asked with the system prompt, revocable in Settings → Privacy; checked here,
 * not only in the app), and the guest records now. Otherwise the guest reads silence (lord-vmdisplay). */
#define BUILTIN_FWID "lordos"           /* built-in apps' firewall id (docs/FIREWALL.md), as js_media.c */
static int mic_permitted(void) {
    uint32_t rf = restrict_flag_of(PERM_MICROPHONE);
    if (rf && restrict_blocks(BUILTIN_FWID, rf)) return 0;
    return perm_get("machines", PERM_MICROPHONE) == PERM_ALLOW;
}
/* the guest's layout to lord-vmdisplay (its tablet points at the shown display) */
static void layout_send(Sess *s) {
    if (s->vd < 0) return;
    VdMsg m = vd_msg(VD_LAYOUT, s->layout[0], s->layout[1], s->layout[2], s->layout[3], s->layout[4], s->layout[5]);
    vd_send(s->vd, &m, -1);
}
static void vdsend_fd(Sess *s, int type, int a, int fd) { if (s->vd >= 0) { VdMsg m = vd_msg((uint16_t)type, a, 0, 0, 0, 0, 0); vd_send(s->vd, &m, fd); } }

static long long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* PipeWire wasn't reachable when a stream opened (a window opened early in the session) or went away: the stream
 * connects again every 2 s while the guest plays or records, with the same ring, so the helper needs nothing new */
static void snd_retry(Sess *s, MediaSrc *ms) {
    int spk = ms->audio && s->guest_out && maudio_failed(ms->audio), mic = ms->mic && maudio_failed(ms->mic);
    if (!spk && !mic) { s->snd_retry_at = 0; return; }
    long long now = mono_ms();
    if (s->snd_retry_at && now < s->snd_retry_at) return;
    s->snd_retry_at = now + 2000;
    if (spk && !maudio_retry(ms->audio)) { s->spk_vol = -1; s->spk_play = -1; }
    if (mic) maudio_retry(ms->mic);
}

/* the device a stream uses: the one chosen while Settings → Sound lists it (or while that isn't known), else the
 * default; checked at every poll, so a device unplugged sends the stream to the default and one plugged back in takes
 * it again (maudio_target moves a live stream only when this changes) */
/* the chosen device while it is listed now, else "" (the default); fails closed: an unknown sound state means the
 * default, and an input must be a microphone, never an output's monitor (the microphone permission doesn't cover
 * what this computer plays) */
static const char *snd_target(int kind, const char *chosen) {
    if (!chosen[0] || snd_device_present(kind, chosen) != 1) return "";
    if (kind == SND_SOURCE && !snd_source_is_input(chosen)) return "";
    return chosen;
}

static void snd_apply(Sess *s) {
    MediaSrc *ms = s->media ? media_src_get(s->media) : NULL;
    int up = s->vd >= 0 && s->vd_up && ms;
    if (up) snd_retry(s, ms);
    /* the speakers: a stream while the machine's sound is on (60 ms of buffer; the helper keeps at most 80 more) */
    if (up && s->snd_on) {
        if (!ms->audio) {
            int ring = -1;
            ms->audio = maudio_open(0, "Machines", s->name[0] ? s->name : "Machine", "video", &ring);
            if (ms->audio) maudio_latency(ms->audio, 60);
            s->spk_sent = 0;
            s->spk_vol = -1;
            s->spk_play = -1;
        }
        if (ms->audio) {
            maudio_target(ms->audio, snd_target(SND_SINK, s->snd_out));     /* chosen in Configure or the bar: moved now */
            if (!s->spk_sent) { vdsend_fd(s, VD_AUDIO_OUT, 1, maudio_ring_fd(ms->audio)); s->spk_sent = 1; }
            if (s->spk_vol != s->snd_vol) { s->spk_vol = s->snd_vol; maudio_volume(ms->audio, s->snd_vol / 100.0); }
            if (s->spk_play != s->guest_out) { s->spk_play = s->guest_out; maudio_playing(ms->audio, s->guest_out); }
        }
    } else if (ms && ms->audio) {
        if (s->spk_sent) vdsend(s, VD_AUDIO_OUT, 0, 0);
        maudio_close(ms->audio);
        ms->audio = NULL;
        s->spk_sent = 0;
    }
    /* the microphone: not for a paused machine either (its input stays enabled while nothing runs) */
    int want = up && s->mic_allowed && s->guest_in && s->state == VS_RUNNING, perm = want && mic_permitted();
    s->mic_wait = want && !perm;
    if (perm) {
        if (!ms->mic) {
            int ring = -1;
            ms->mic = maudio_open(1, "Machines", s->name[0] ? s->name : "Machine", snd_target(SND_SOURCE, s->snd_in), &ring);
            s->mic_sent = 0;
        } else maudio_target(ms->mic, snd_target(SND_SOURCE, s->snd_in));
        if (ms->mic && !s->mic_sent) { vdsend_fd(s, VD_MIC, 1, maudio_ring_fd(ms->mic)); s->mic_sent = 1; }
    } else if (ms && ms->mic) {
        if (s->mic_sent) vdsend(s, VD_MIC, 0, 0);
        maudio_close(ms->mic);
        ms->mic = NULL;
        s->mic_sent = 0;
    }
}

/* First-boot settings for a guest that asks for them (systemd's credentials: a Debian "nocloud" image otherwise stops
 * at a prompt on its serial line, where nobody sees it): this computer's language, keyboard and time zone (5.2,
 * "copied from the host"). Only words of a closed alphabet; no password. A file of the person's, 0600, in the runtime
 * folder; lord-vm reads it at start and lord-server removes it at the first message. */
static int safe_word(const char *s, const char *extra) {
    if (!*s || strlen(s) > 64) return 0;
    for (const char *p = s; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr(extra, *p))) return 0;
    return 1;
}
static void host_keymap(char *out, size_t n) {
    snprintf(out, n, "us");
    FILE *f = fopen("/etc/default/keyboard", "re");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "XKBLAYOUT=", 10)) continue;
        char *v = line + 10, w[64];
        size_t k = 0;
        if (*v == '"') v++;
        while (*v && *v != '"' && *v != ',' && *v != '\n' && k + 1 < sizeof w) w[k++] = *v++;
        w[k] = 0;
        if (safe_word(w, "-_")) snprintf(out, n, "%s", w);
    }
    fclose(f);
}
static void host_defaults(char *lang, size_t ln, char *tz, size_t tn, char *km, size_t kn) {
    char lt[256];
    snprintf(lang, ln, "C.UTF-8");
    snprintf(tz, tn, "UTC");
    const char *l = getenv("LANG");
    if (l && safe_word(l, "._@-")) snprintf(lang, ln, "%s", l);
    ssize_t r = readlink("/etc/localtime", lt, sizeof lt - 1);
    if (r > 0) { lt[r] = 0; char *z = strstr(lt, "zoneinfo/"); if (z && safe_word(z + 9, "/_-+")) snprintf(tz, tn, "%s", z + 9); }
    host_keymap(km, kn);
}

/* lord.vm.hostDefaults() → { locale, timezone, keymap }: this computer's, what an unattended install starts from */
static JSValue f_host_defaults(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char lang[80], tz[96], km[64];
    host_defaults(lang, sizeof lang, tz, sizeof tz, km, sizeof km);
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "locale", JS_NewString(c, lang));
    JS_SetPropertyStr(c, o, "timezone", JS_NewString(c, tz));
    JS_SetPropertyStr(c, o, "keymap", JS_NewString(c, km));
    return o;
}

static int write_creds(char *path, size_t n, const char *dir) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char base[256];
    if (rt && rt[0] == '/' && strlen(rt) < 200) snprintf(base, sizeof base, "%s", rt); else snprintf(base, sizeof base, "/run/user/%u", (unsigned)getuid());
    static unsigned k;
    snprintf(path, n, "%s/.lordos-vm-firstboot-%d-%u", base, (int)getpid(), ++k);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { path[0] = 0; return -1; }
    char lang[80] = "C.UTF-8", tz[96] = "UTC", km[64], buf[512], lt[256];
    const char *l = getenv("LANG");
    if (l && safe_word(l, "._@-")) snprintf(lang, sizeof lang, "%s", l);
    ssize_t r = readlink("/etc/localtime", lt, sizeof lt - 1);
    if (r > 0) { lt[r] = 0; char *z = strstr(lt, "zoneinfo/"); if (z && safe_word(z + 9, "/_-+")) snprintf(tz, sizeof tz, "%s", z + 9); }
    host_keymap(km, sizeof km);
    int len = snprintf(buf, sizeof buf, "firstboot.locale=%s\nfirstboot.keymap=%s\nfirstboot.timezone=%s\n", lang, km, tz);
    int ok = len > 0 && write(fd, buf, (size_t)len) == len;
    /* the machine's own first-start settings (a root password's hash), checked line by line */
    char fp[PATH_MAX + 32], line[256];
    snprintf(fp, sizeof fp, "%s/firstboot.creds", dir);
    int ff = open(fp, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    FILE *f = ff >= 0 ? fdopen(ff, "r") : NULL;
    if (!f && ff >= 0) close(ff);
    while (ok && f && fgets(line, sizeof line, f)) {
        static const char key[] = "passwd.hashed-password.root=";
        size_t ln = strlen(line), kl = sizeof key - 1;
        if (ln && line[ln - 1] == '\n') line[--ln] = 0;
        int good = !strncmp(line, key, kl) && line[kl] == '$' && ln - kl < 160;
        for (const char *q = line + kl; good && *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '$' || *q == '.' || *q == '/')) good = 0;
        if (!good) continue;
        ok = dprintf(fd, "%s\n", line) > 0;
    }
    if (f) fclose(f);
    explicit_bzero(line, sizeof line);
    close(fd);
    if (!ok) { unlink(path); path[0] = 0; return -1; }
    return 0;
}

/* start(dir[, { resumeAnyway, discardState, loadvm }]) → session id */
static int opt_bool(JSContext *c, int argc, JSValueConst *argv, const char *k) {
    if (argc < 2 || !JS_IsObject(argv[1])) return 0;
    JSValue v = JS_GetPropertyStr(c, argv[1], k);
    int b = JS_ToBool(c, v) > 0;
    JS_FreeValue(c, v);
    return b;
}
static JSValue f_start(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char dir[PATH_MAX], bin[PATH_MAX];
    int anyway = opt_bool(c, argc, argv, "resumeAnyway"), discard = opt_bool(c, argc, argv, "discardState");
    char loadvm[40] = "";
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(c, argv[1], "loadvm");
        const char *t2 = JS_IsString(v) ? JS_ToCString(c, v) : NULL;
        if (t2 && vm_snap_tag_valid(t2)) snprintf(loadvm, sizeof loadvm, "%s", t2);
        if (t2) JS_FreeCString(c, t2);
        JS_FreeValue(c, v);
    }
    int slot = new_slot();
    if (slot < 0 || machine_dir(c, argv[0], dir, sizeof dir) || bin_path("lord-vm", bin, sizeof bin)) return JS_NewInt32(c, 0);
    last_start = time(NULL);
    if (!machine_shares_ok(dir)) return JS_NewInt32(c, 0);
    int sv[2], ep[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return JS_NewInt32(c, 0);
    if (pipe2(ep, O_CLOEXEC)) { close(sv[0]); close(sv[1]); return JS_NewInt32(c, 0); }
    /* lord-vm writes to stderr with nobody waiting: never let a full pipe stop the supervisor */
    fcntl(ep[1], F_SETFL, O_NONBLOCK);
    fcntl(ep[0], F_SETFL, O_NONBLOCK);
    char creds[PATH_MAX];
    int have_creds = !write_creds(creds, sizeof creds, dir);
    char rnode[64] = "none";
    {
        GpuInfo gi;
        gpu_info(&gi);
        if (gpu_active() && gi.dmabuf && gi.render_node && !strncmp(gi.render_node, "/dev/dri/renderD", 16)) snprintf(rnode, sizeof rnode, "%s", gi.render_node);
    }
    pid_t p = fork();
    if (!p) {
        int ch = fcntl(sv[1], F_DUPFD, 10), e = fcntl(ep[1], F_DUPFD, 11);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); }
        dup2(e, 2);
        dup2(ch, 3);
        syscall(SYS_close_range, 4, ~0u, 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        /* no cgroup_enter here: lord-vm places itself in the machine's scope through lord-sessiond */
        char *args[20];
        int k = 0;
        args[k++] = "lord-vm"; args[k++] = "--start"; args[k++] = dir; args[k++] = "--control-fd"; args[k++] = "3"; args[k++] = "--paused";
        /* 3D graphics (6.10): QEMU draws on the GPU this desktop composes with, whose dmabufs it can read back; without
         * a GPU path a 3D machine starts with 2D graphics */
        args[k++] = "--render-node"; args[k++] = rnode;
        if (anyway) args[k++] = "--resume-anyway";
        if (discard) args[k++] = "--discard-state";
        if (loadvm[0]) { args[k++] = "--loadvm"; args[k++] = loadvm; }
        if (have_creds) { args[k++] = "--credentials-file"; args[k++] = creds; }
        args[k] = NULL;
        execve(bin, args, environ);
        _exit(127);
    }
    close(sv[1]);
    close(ep[1]);
    if (p < 0) { close(sv[0]); close(ep[0]); if (have_creds) unlink(creds); return JS_NewInt32(c, 0); }
    Sess *s = &sess[slot];
    init_sess(s, dir);
    if (have_creds) snprintf(s->creds, sizeof s->creds, "%s", creds);
    s->vm = sv[0];
    s->err = ep[0];
    s->vm_pid = p;
    s->started_paused = 1;
    return JS_NewInt32(c, slot + 1);
}

/* a machine already running: its runtime socket ($XDG_RUNTIME_DIR/lordos-vm/<id>.sock) */
static JSValue f_attach(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    char dir[PATH_MAX];
    int slot = new_slot();
    VmMachine m;
    if (slot < 0 || machine_dir(c, argv[0], dir, sizeof dir) || vm_machine_load(dir, &m, NULL, 0)) return JS_NewInt32(c, 0);
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char base[256], path[sizeof ((struct sockaddr_un *)0)->sun_path];
    if (rt && rt[0] == '/') snprintf(base, sizeof base, "%s", rt); else snprintf(base, sizeof base, "/run/user/%u", (unsigned)getuid());
    if ((size_t)snprintf(path, sizeof path, "%s/lordos-vm/%s.sock", base, m.id) >= sizeof path) return JS_NewInt32(c, 0);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, path, strlen(path) + 1);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) { if (fd >= 0) close(fd); return JS_NewInt32(c, 0); }
    Sess *s = &sess[slot];
    init_sess(s, dir);
    s->vm = fd;
    snprintf(s->id, sizeof s->id, "%s", m.id);
    ask(s, VM_STATUS, 0);
    return JS_NewInt32(c, slot + 1);
}

static void spawn_vmdisplay(Sess *s, int dbus_fd) {
    char bin[PATH_MAX];
    int sv[2];
    if (bin_path("lord-vmdisplay", bin, sizeof bin) || socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { close(dbus_fd); s->display_failed = 1; return; }
    pid_t p = fork();
    if (!p) {
        int a = fcntl(sv[1], F_DUPFD, 20), b = fcntl(dbus_fd, F_DUPFD, 21);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); }          /* its notes go to the session's log (stderr) */
        dup2(a, 3);
        dup2(b, 4);
        syscall(SYS_close_range, 5, ~0u, 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        char *args[] = { "lord-vmdisplay", s->id, NULL };
        execve(bin, args, environ);
        _exit(127);
    }
    close(sv[1]);
    close(dbus_fd);
    if (p < 0) { close(sv[0]); s->display_failed = 1; return; }
    s->vd = sv[0];
    s->vd_pid = p;
    s->vd_up = 0;
}

static void resume_once(Sess *s) {
    /* started paused so the first picture is seen; then it runs, also when there's no picture to wait for */
    if (s->started_paused && !s->resumed && s->state == VS_PAUSED) { s->resumed = 1; ask(s, VM_RESUME, 0); }
}

static void maybe_display(Sess *s) {
    if (s->display_asked || !s->have_info || (s->state != VS_RUNNING && s->state != VS_PAUSED)) return;
    s->display_asked = 1;
    int q = ask(s, VM_DISPLAY, 0);
    if (q < 0) { s->display_failed = 1; resume_once(s); }
    else s->display_seq = (uint32_t)q;
}

/* ---- 3D pictures (VD_DMABUF, vmproto.h) ---- */

static MediaSrc *sess_media(Sess *s) { return s->media ? media_src_get(s->media) : NULL; }

/* a texture going away is no longer the picture's */
static void gtex_drop(Sess *s, GpuTex *t) {
    MediaSrc *ms = sess_media(s);
    if (ms && t && ms->gtex == t) { ms->gtex = NULL; ms->gpu_drawn = 0; }
}

static void gout_free(Sess *s) {
    for (int k = 0; k < 2; k++) {
        if (s->gout[k].tex) { gtex_drop(s, s->gout[k].tex); gpu_tex_free(s->gout[k].tex); }
        dmaread_out_free(s->gout[k].o);
        memset(&s->gout[k], 0, sizeof s->gout[k]);
    }
    s->gshow = s->gpend = -1;
    s->gsrc = 0;
}

static void db_free(Sess *s, int i) {
    if (s->db[i].tex) { gtex_drop(s, s->db[i].tex); gpu_tex_free(s->db[i].tex); }
    if (s->db[i].ri) dmaread_free(s->db[i].ri);
    if (s->db[i].fd >= 0) close(s->db[i].fd);
    memset(&s->db[i], 0, sizeof s->db[i]);
    s->db[i].fd = -1;
    if (s->db_cur == i) s->db_cur = -1;
}
static void db_drop_all(Sess *s) {
    gout_free(s);
    s->gfail = 0;
    for (int i = 0; i < 4; i++) db_free(s, i);
    s->db_cur = -1;
    s->db_pending = s->db_more = 0;
}

/* VD_DMABUF: the dmabuf the picture is now (fd), or none (fd < 0, m->a == 0) */
static void db_take(Sess *s, const VdMsg *m, int fd) {
    if (!m->a || fd < 0) { if (fd >= 0) close(fd); s->db_cur = -1; s->db_pending = 0; return; }
    struct stat st;
    if (fstat(fd, &st)) { close(fd); return; }
    int st_stride = m->c & ~VD_DMABUF_FLIP, flip = (m->c & VD_DMABUF_FLIP) != 0;
    uint64_t mod = (uint64_t)(uint32_t)m->e | (uint64_t)(uint32_t)m->f << 32;
    int k = -1;
    for (int i = 0; i < 4; i++)
        if (s->db[i].fd >= 0 && s->db[i].ino == st.st_ino && s->db[i].w == m->a && s->db[i].h == m->b && s->db[i].stride == st_stride &&
            s->db[i].fourcc == (uint32_t)m->d && s->db[i].mod == mod) k = i;
    if (k >= 0) close(fd);
    else {
        /* a new one: in a free place, or the one shown longest ago */
        for (int i = 0; i < 4 && k < 0; i++) if (s->db[i].fd < 0) k = i;
        if (k < 0) { k = 0; for (int i = 1; i < 4; i++) if (s->db[i].used < s->db[k].used) k = i; }
        db_free(s, k);
        s->db[k].fd = fd; s->db[k].ino = st.st_ino; s->db[k].w = m->a; s->db[k].h = m->b; s->db[k].stride = st_stride;
        s->db[k].fourcc = (uint32_t)m->d; s->db[k].mod = mod;
        s->db[k].gen = ++s->db_gen ? s->db_gen : ++s->db_gen;
    }
    s->db[k].flip = flip;
    s->db[k].used = ++s->db_tick;
    s->db_cur = k;
}

/* The picture may be shown as a GPU texture now: the GPU composes, and nothing needs the CPU's pixels every frame (a
 * recording of the machine, a recording of the screen). LORD_VM_GPUTEX=0 turns it off (the readback of before). */
static int gpu_show_ok(Sess *s) {
    static int env = -1;
    if (env < 0) { const char *e = getenv("LORD_VM_GPUTEX"); env = !(e && !strcmp(e, "0")); }
    return env && gpu_active() && !s->rec && !media_needs_cpu();
}

/* the two linear copies for this dmabuf's import, made and imported by the desktop's GPU once (and again for another
 * size or a new GPU context); 0 ready */
static int gout_ready(Sess *s, const VmDmabuf *d) {
    if (s->gout[0].o && (s->gout[0].w != d->w || s->gout[0].h != d->h)) gout_free(s);   /* another size */
    for (int k = 0; k < 2; k++)
        if (s->gout[k].tex && !gpu_tex_current(s->gout[k].tex)) { gtex_drop(s, s->gout[k].tex); gpu_tex_free(s->gout[k].tex); s->gout[k].tex = NULL; }
    for (int k = 0; k < 2; k++) {
        if (!s->gout[k].o) {
            s->gout[k].o = dmaread_out_new(d->ri);
            s->gout[k].w = d->w; s->gout[k].h = d->h;
            if (!s->gout[k].o) return -1;
            s->gout[k].tex = NULL;
            gband_all(&s->gstale[k], d->h);             /* nothing in it yet */
            if (s->gshow == k) s->gshow = -1;
        }
        if (!s->gout[k].tex) {
            int stride = 0, fd = dmaread_out_fd(s->gout[k].o, &stride);
            GpuDmabuf b = { .width = d->w, .height = d->h, .nplanes = 1, .fourcc = DRM_FORMAT_XRGB8888, .modifier = DRM_FORMAT_MOD_LINEAR,
                            .fd = { fd, -1, -1, -1 }, .offset = { 0 }, .stride = { (uint32_t)stride } };
            s->gout[k].tex = fd >= 0 ? gpu_tex_dmabuf(&b) : NULL;
            if (!s->gout[k].tex) return -1;
            gband_all(&s->gstale[k], d->h);             /* a new import: copied whole next time */
            if (s->gshow == k) s->gshow = -1;
        }
    }
    return 0;
}

/* map brought up to date with the frame the texture shows (media_src_need_cpu: a capture, a recording, a CPU paint) */
static int db_cpu_read(int id) {
    for (int i = 0; i < MAX_SESS; i++) {
        Sess *s = &sess[i];
        if (!s->used || s->media != id || s->db_cur < 0) continue;
        MediaSrc *ms = sess_media(s);
        VmDmabuf *d = &s->db[s->db_cur];
        if (!ms || !ms->map || d->w != s->w || d->h != s->h) return -1;
        if (d->tex) return gpu_tex_read_rows(d->tex, (uint8_t *)ms->map, 0, d->h, d->flip);
        if (d->ri) return dmaread_rows_new(d->ri, (uint8_t *)ms->map, 0, d->h, d->flip, (const void *)(uintptr_t)s->rkey) < 0 ? -1 : 0;
        return -1;
    }
    return -1;
}

/* The pending rows: 1 a new frame (in the picture's memory, or in the texture it is shown from), 2 a frame that changed
 * nothing (acknowledged at once, nothing presented), 0 not yet (the dmabuf's fence, or the copy's, hasn't signalled:
 * tried again when it does, and lord-vmdisplay goes on by itself after 250 ms), -1 can't (no GPU path, an import
 * refused: the picture stays as it was, and the frame is acknowledged so the guest isn't held). *ry0..*ry1: its rows */
static int db_read(Sess *s, MediaSrc *ms, int *ry0, int *ry1) {
    if (s->db_cur < 0 || !ms || !ms->map) return -1;
    VmDmabuf *d = &s->db[s->db_cur];
    if (d->w != s->w || d->h != s->h) return -1;
    if (!ms->cpu_read) ms->cpu_read = db_cpu_read;
    /* a copy in flight: done when its fence signals; the copy that was in it becomes the one shown */
    if (s->gpend >= 0) {
        int k = s->gpend;
        struct pollfd pf = { dmaread_out_fd(s->gout[k].o, NULL), POLLIN, 0 };
        if (pf.fd >= 0 && poll(&pf, 1, 0) == 0) return 0;
        int c = dmaread_changed(s->gout[k].o);
        s->gpend = -1;
        s->gshow = k;
        *ry0 = s->gcopy_y0; *ry1 = s->gcopy_y1;
        if (s->gout[k].tex && gpu_tex_current(s->gout[k].tex)) {
            ms->gtex = s->gout[k].tex; ms->gtex_flip = 0; ms->cpu_stale = 1;
        }
        if (c == 0 && ms->gtex) { s->db_same++; return 2; }
        return 1;
    }
    struct pollfd pf = { d->fd, POLLIN, 0 };
    if (poll(&pf, 1, 0) == 0) return 0;
    if (d->tex && !gpu_tex_current(d->tex)) { gtex_drop(s, d->tex); gpu_tex_free(d->tex); d->tex = NULL; }
    if (!d->tex && !d->ri) {
        /* Which GPU reads it: the desktop's (a Linux 3D machine's virgl picture is made on it), or the reader on the
         * Intel render node (gpu/dmaread.h) for an Intel GVT-g machine's picture: its Intel-tiled modifier can't be
         * imported by another vendor's GPU, and on the CPU path (LORD_GPU=0, no usable GPU) there is no desktop GPU;
         * also when the desktop's GPU refused it. */
        const char *in = dmaread_intel_node();
        GpuInfo gi;
        gpu_info(&gi);
        int desk_intel = in && gi.render_node && !strcmp(gi.render_node, in);
        if (gpu_active() && !d->gfail && !(in && dmaread_mod_intel(d->mod) && !desk_intel)) {
            GpuDmabuf b = { .width = d->w, .height = d->h, .nplanes = 1, .fourcc = d->fourcc, .modifier = d->mod, .fd = { d->fd, -1, -1, -1 },
                            .offset = { 0 }, .stride = { (uint32_t)d->stride } };
            d->tex = gpu_tex_dmabuf(&b);
            if (!d->tex) d->gfail = 1;
        }
        if (!d->tex && in && !d->rfail) {
            d->ri = dmaread_import(in, d->fd, d->w, d->h, d->stride, d->fourcc, d->mod);
            if (!d->ri) d->rfail = 1;
        }
        if (!d->tex && !d->ri) {
            if (!s->db_warned++)
                fprintf(stderr, "machines: a 3D picture can't be shown: %s (modifier %llx)\n",
                        gpu_active() ? "no GPU took its dmabuf" : in ? "the desktop draws without a GPU and the Intel GPU refused its dmabuf" : "the desktop draws without a GPU",
                        (unsigned long long)d->mod);
            s->db_fails++;
            return -1;
        }
    }
    int y0 = s->db_y0 < 0 ? 0 : s->db_y0, y1 = s->db_y1 > s->h ? s->h : s->db_y1;
    if (y1 <= y0) return 2;
    *ry0 = y0; *ry1 = y1;
    int show = gpu_show_ok(s);
    /* the desktop's GPU imported it: shown straight from it */
    if (show && d->tex) {
        if (ms->gtex != d->tex && s->gshow >= 0) gout_free(s);  /* the copies are an Intel picture's */
        ms->gtex = d->tex; ms->gtex_flip = d->flip; ms->cpu_stale = 1;
        s->db_reads++;
        return 1;
    }
    /* an Intel vGPU's picture: copied on the iGPU into the copy not shown, imported by the desktop's GPU */
    if (show && d->ri && !s->gfail) {
        if (!gout_ready(s, d)) {
            int k = s->gshow == 0 ? 1 : 0;
            /* another dmabuf than the copies were made from (a new attach, the guest flipped to another buffer): what
             * either copy holds says nothing about it, so every row is copied (both copies, in turn) */
            if (s->gsrc != d->gen) { gband_all(&s->gstale[0], d->h); gband_all(&s->gstale[1], d->h); s->gsrc = d->gen; }
            GBand c = gband_copy(&s->gstale[k], y0, y1, d->h);
            const DmaOut *prev = s->gshow >= 0 ? s->gout[s->gshow].o : NULL;
            if (!dmaread_copy(d->ri, s->gout[k].o, c.y0, c.y1 - c.y0, d->flip, prev)) {
                gband_copied(s->gstale, k, y0, y1);     /* the other copy lacks this frame's rows now */
                s->gpend = k;
                s->gcopy_y0 = c.y0; s->gcopy_y1 = c.y1;   /* the rows copied: all of them for a new source */
                /* frames that come while it's in flight start a new band */
                s->db_y0 = INT_MAX; s->db_y1 = INT_MIN; s->db_more = 0;
                s->db_reads++;
                return 0;
            }
        }
        fprintf(stderr, "machines: the vGPU's picture can't be shown as a texture (no linear copy the desktop's GPU takes): read back instead\n");
        gout_free(s);
        s->gfail = 1;
    }
    /* read back into the picture's memory: all of it if a texture showed it until now */
    if (ms->gtex) { ms->gtex = NULL; ms->gpu_drawn = 0; }
    if (s->gshow >= 0 || s->gpend >= 0) gout_free(s);
    if (ms->cpu_stale) { y0 = 0; y1 = s->h; *ry0 = 0; *ry1 = s->h; }
    int r = d->tex ? (gpu_tex_read_rows(d->tex, (uint8_t *)ms->map, y0, y1 - y0, d->flip) ? -1 : 1)
                   : dmaread_rows_new(d->ri, (uint8_t *)ms->map, y0, y1 - y0, d->flip, (const void *)(uintptr_t)s->rkey);
    if (r < 0) { s->db_fails++; return -1; }
    ms->cpu_stale = 0;
    s->db_reads++;
    if (!r) { s->db_same++; return 2; }
    return 1;
}

static void drop_display(Sess *s, int close_helper) {
    ptrlock_end_owner(s);               /* no picture, no capture (it tells lord-vm and the helper first) */
    if (s->vd >= 0) { if (close_helper) vdsend(s, VD_CLOSE, 0, 0); close(s->vd); s->vd = -1; }
    if (s->vd_pid > 0) {
        if (waitpid(s->vd_pid, NULL, WNOHANG) == 0) { kill(s->vd_pid, SIGKILL); waitpid(s->vd_pid, NULL, 0); }
        s->vd_pid = 0;
    }
    s->vd_up = 0;
    s->ack_pending = 0;
    memset(&s->pace, 0, sizeof s->pace);      /* the next helper starts unpaced */
    db_drop_all(s);
    /* the next helper gets the rings again; until then nothing plays and the microphone is closed */
    s->guest_out = s->guest_in = 0;
    snd_apply(s);
    /* the next helper gets the guest's pointer again from QEMU; until then, the host's */
    free(s->cur); s->cur = NULL;
    if (s->media) { media_src_cursor(s->media, NULL, 0, 0, 0, 0); if (s->windows && !s->vgpu) media_src_cursor_guest_draws(s->media, 1); }
}

static void drop_creds(Sess *s) { if (s->creds[0]) { unlink(s->creds); s->creds[0] = 0; } }

/* the end of a VM_FILE: 1 if this reply was one */
static int fx_reply(Sess *s, const VmMsg *m) {
    for (int i = 0; i < 16; i++) if (s->fx[i].state == 1 && s->fx[i].seq == m->seq) {
        s->fx[i].state = m->a == VME_OK ? 2 : 3;
        s->fx[i].how = m->a == VME_OK && m->b >= 0 && m->b <= AGP_LAST ? m->b : 0;
        if (m->a == VME_OK) s->fx[i].pct = 100;
        snprintf(s->fx[i].msg, sizeof s->fx[i].msg, "%s", m->text);
        s->fx_changes++;
        return 1;
    }
    return 0;
}

/* VM_WINDATA: the guest's windows, an icon or its apps; the memfd is checked sealed and of its size, and what it holds
 * is parsed again with vmagent.c's parsers (lord-vm wrote it, but this side trusts no length) */
static void windata(Sess *s, const VmMsg *m, int fd) {
    int seals = fcntl(fd, F_GET_SEALS);
    struct stat st;
    if (seals < 0 || (seals & (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW)) != (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW) || fstat(fd, &st) || st.st_size != m->a) return;
    uint8_t *b = malloc((size_t)m->a);
    if (!b || pread(fd, b, (size_t)m->a, 0) != m->a) { free(b); return; }
    if (m->b == LAG_WINDOWS) {
        LagWinList *l = malloc(sizeof *l);
        if (l && !lag_winlist_read(b, (size_t)m->a, l)) {
            free(s->wins); s->wins = l; l = NULL; s->wins_seq++;
            /* icons of windows that are gone go too */
            for (int i = 0; i < LAG_WIN_MAX; i++) {
                if (!s->icons[i].id) continue;
                int there = 0;
                for (int k = 0; k < s->wins->n && !there; k++) there = s->wins->w[k].id == s->icons[i].id;
                if (!there) { free(s->icons[i].rgba); s->icons[i].rgba = NULL; s->icons[i].id = 0; }
            }
        }
        free(l);
    } else if (m->b == LAG_WINICON) {
        uint32_t id; uint16_t w, h; const uint8_t *px;
        if (!lag_winicon_parse(b, (size_t)m->a, &id, &w, &h, &px)) {
            int k = -1;
            for (int i = 0; i < LAG_WIN_MAX && k < 0; i++) if (s->icons[i].id == id) k = i;
            for (int i = 0; i < LAG_WIN_MAX && k < 0; i++) if (!s->icons[i].id) k = i;
            uint8_t *copy = k >= 0 ? malloc((size_t)w * h * 4) : NULL;
            if (copy) {
                memcpy(copy, px, (size_t)w * h * 4);
                free(s->icons[k].rgba);
                s->icons[k].id = id; s->icons[k].w = w; s->icons[k].h = h; s->icons[k].rgba = copy;
                s->icons_seq++;
            }
        }
    } else if (m->b == LAG_APPLIST) {
        LagAppList *l = malloc(sizeof *l);
        if (l && !lag_applist_read(b, (size_t)m->a, l)) { free(s->gapps); s->gapps = l; l = NULL; s->gapps_seq++; s->gapps_gen = m->c; }
        free(l);
    }
    free(b);
}

#define CLIP_GRACE_MS 3000
#define CLIP_ASK_MS 30000
static int clip_in_use(Sess *s) { return s->gate_on || mono_ms() < s->gate_until; }
static uint64_t clip_hash(const void *p, size_t n);
/* this computer's clipboard as the guest may have it: a picture, or text with its rich text when an app gave some;
 * never a secret (Passwords, password fields). 0 there is one (cx's parts point into the clipboard's own copies; *h its
 * main form's hash), -1 nothing to give */
static int host_clip(AgClip *cx, uint64_t *h) {
    memset(cx, 0, sizeof *cx);
    if (wl_clip_is_image()) {
        size_t n = 0;
        const unsigned char *png = wl_clip_png(&n);
        if (!png || !n || n > AG_PNG_MAX || wl_clip_image_sensitive()) return -1;
        cx->png = png; cx->png_n = (uint32_t)n;
        *h = clip_hash(png, n);
        return 0;
    }
    const char *txt = term_clipboard();
    size_t tn = txt ? strlen(txt) : 0, hn = 0;
    if (!tn || tn > AG_TEXT_MAX || term_clipboard_sensitive() || !ag_utf8_ok((const uint8_t *)txt, tn)) return -1;
    const char *html = wl_clip_html(&hn);
    cx->text = (const uint8_t *)txt; cx->text_n = (uint32_t)tn;
    if (html && hn && hn <= AG_TEXT_MAX && ag_utf8_ok((const uint8_t *)html, hn)) { cx->html = (const uint8_t *)html; cx->html_n = (uint32_t)hn; }
    *h = cx->html ? clip_hash(html, hn) : clip_hash(txt, tn);
    return 0;
}
static int clip_forms(const AgClip *c) { return (c->text ? AGM_TEXT : 0) | (c->html ? AGM_HTML : 0) | (c->png ? AGM_PNG : 0); }

static void clip_deny(Sess *s, uint32_t gen, int form, int why) {
    VmMsg m = vm_msg(VM_CLIP_DENY, ++s->seq, (int)gen, form, why, 0, NULL);
    if (s->vm >= 0) vm_send(s->vm, &m, -1);
}

/* the guest's request for one form of an offer: the data when the offer is still this computer's clipboard; 0 sent */
static int clip_give(Sess *s, uint32_t gen, int form) {
    AgClip cx, one;
    uint64_t h;
    if (gen != s->offer_gen || host_clip(&cx, &h) || h != s->offer_hash) { clip_deny(s, gen, form, VCD_STALE); return -1; }
    memset(&one, 0, sizeof one);
    if (form == AGF_TEXT) { one.text = cx.text; one.text_n = cx.text_n; }
    else if (form == AGF_HTML) { one.html = cx.html; one.html_n = cx.html_n; }
    else { one.png = cx.png; one.png_n = cx.png_n; }
    size_t n = ag_clipx_size(&one);
    if (!n) { clip_deny(s, gen, form, VCD_NONE); return -1; }
    uint8_t *buf = malloc(n);
    int ok = 0;
    if (buf) {
        ag_clipx_write(&one, buf);
        int fd = memfd_create("lordos-vm-clipboard", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd >= 0 && write(fd, buf, n) == (ssize_t)n && !fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)) {
            VmMsg m = vm_msg(VM_CLIP, ++s->seq, (int)n, VCF_CLIPX, (int)gen, form, NULL);
            ok = !vm_send(s->vm, &m, fd);
        }
        if (fd >= 0) close(fd);
        free(buf);
    }
    return ok ? 0 : -1;
}

/* ask mode: requests the person didn't answer in CLIP_ASK_MS are refused */
static void clip_ask_expire(Sess *s) {
    long long now = mono_ms();
    for (int f = AGF_TEXT; f <= AGF_PNG; f++)
        if (s->ask_gen[f] && now - s->ask_at[f] > CLIP_ASK_MS) { clip_deny(s, s->ask_gen[f], f, VCD_DECLINED); s->ask_gen[f] = 0; }
}

/* a link or a notification from the guest (VM_GUEST_NOTE): a sealed memfd of the size said, parsed again */
#define LINK_MIN_MS 700
static void guest_note(Sess *s, const VmMsg *m, int fd) {
    int seals = fcntl(fd, F_GET_SEALS);
    struct stat st;
    uint8_t b[VM_GUEST_NOTE_MAX];
    if (seals < 0 || (seals & (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW)) != (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW) ||
        fstat(fd, &st) || st.st_size != m->a || m->a > (int)sizeof b || pread(fd, b, (size_t)m->a, 0) != m->a) return;
    size_t n = (size_t)m->a;
    if (m->b == AGK_LINK) {
        /* only while the machine is in use (its window in front): a link in the background is the guest's, not the person's */
        if (!ag_link_ok(b, n) || n > AG_LINK_MAX) return;
        if (!clip_in_use(s) || s->link[0]) { s->link_dropped++; return; }
        memcpy(s->link, b, n);
        s->link[n] = 0;
        s->link_seq++;
        s->link_seen = 0;
    } else {
        const uint8_t *t, *bo;
        uint16_t tn, bn;
        if (ag_note_parse(b, n, &t, &tn, &bo, &bn)) return;
        if (s->note_n >= (int)(sizeof s->notes / sizeof *s->notes)) { s->note_dropped++; return; }
        memcpy(s->notes[s->note_n].title, t, tn); s->notes[s->note_n].title[tn] = 0;
        memcpy(s->notes[s->note_n].body, bo, bn); s->notes[s->note_n].body[bn] = 0;
        s->note_n++;
        s->note_seq++;
    }
}

static void from_vm(Sess *s) {
    for (int k = 0; k < VD_POLL_MAX && s->vm >= 0; k++) {
        VmMsg m;
        int fd = -1;
        int r = vm_recv(s->vm, &m, &fd);
        if (r) drop_creds(s);           /* lord-vm read its arguments long before it says anything */
        if (r < 0) { close(s->vm); s->vm = -1; break; }
        if (!r) break;
        if (!vm_msg_valid(&m, 0)) { if (fd >= 0) close(fd); continue; }
        switch (m.type) {
        case VM_INFO:
            s->vcpus = m.a; s->mem = m.b; s->sandbox = m.c; s->cg = m.d; s->have_info = 1;
            if (vm_uuid_valid(m.text)) snprintf(s->id, sizeof s->id, "%.36s", m.text);
            maybe_display(s);
            break;
        case VM_STATE:
            s->state = m.a; s->reason = m.b; s->code = m.c;
            if (m.b == VR_GUEST_RESET || m.a != VS_RUNNING) s->ag_seen = 0;     /* a restarting guest's agent isn't lost */
            if (m.a == VS_STOPPED && (m.b == VR_GUEST_SHUTDOWN || m.b == VR_HOST_SHUTDOWN)) {
                /* the system shut down by itself once: its first start is behind it */
                char fp[PATH_MAX + 32];
                snprintf(fp, sizeof fp, "%s/firstboot.creds", s->dir);
                unlink(fp);
            }
            snprintf(s->detail, sizeof s->detail, "%s", m.text);
            maybe_display(s);
            if (s->display_failed) resume_once(s);
            break;
        case VM_AGENT_STATE:
            s->ag_state = m.a; s->ag_caps = m.b;
            if (m.a == VAS_CONNECTED) s->ag_seen = 1;
            if (m.a == VAS_OFF) s->ag_seen = 0;
            snprintf(s->ag_text, sizeof s->ag_text, "%s", m.text);
            break;
        case VM_CLIP_REQ:
            /* a program in the guest pastes: the host's clipboard only while the machine is in use, and in ask mode
             * only once the person says yes (clipboardAnswer) */
            if (!clip_in_use(s)) { s->blk_read++; clip_deny(s, (uint32_t)m.a, m.b, VCD_BACKGROUND); break; }
            if (s->clip_ask) {
                if ((uint32_t)m.a != s->offer_gen) { clip_deny(s, (uint32_t)m.a, m.b, VCD_STALE); break; }
                if (!s->ask_gen[m.b]) s->ask_at[m.b] = mono_ms();
                s->ask_gen[m.b] = (uint32_t)m.a;
                s->ask_seq++;
                break;
            }
            clip_give(s, (uint32_t)m.a, m.b);
            break;
        case VM_CLIP_DATA:
            /* the guest's clipboard, only while the machine is in use: in the background it's ignored (and lord-vm
             * logs it), so a guest can't change this computer's clipboard behind the person's back */
            if (fd >= 0 && !clip_in_use(s)) {
                s->blk_write++;
                s->clip_last = 0;           /* the guest's clipboard isn't ours any more: offered again at the next turn */
                clip_deny(s, 0, 0, VCD_BACKGROUND);
                break;
            }
            /* the guest's text: checked sealed and of the size it says, then kept for the app to take */
            if (fd >= 0) {
                int seals = fcntl(fd, F_GET_SEALS);
                struct stat cst;
                if (seals >= 0 && (seals & (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW)) == (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW) &&
                    !fstat(fd, &cst) && cst.st_size == m.a) {
                    char *t = malloc((size_t)m.a + 1);
                    if (t && pread(fd, t, (size_t)m.a, 0) == m.a) {
                        t[m.a] = 0;
                        free(s->clip_in); free(s->clip_html); free(s->clip_png);
                        s->clip_in = s->clip_html = NULL; s->clip_png = NULL; s->clip_html_n = s->clip_png_n = 0;
                        AgClip cx;
                        if (m.b == VCF_TEXT) { s->clip_in = t; t = NULL; s->clip_seq++; }
                        else if (!ag_clipx_parse((const uint8_t *)t, (size_t)m.a, &cx)) {
                            /* lord-vm checked every part; kept as parts, the picture still to be re-encoded */
                            if (cx.text && (s->clip_in = malloc(cx.text_n + 1))) { memcpy(s->clip_in, cx.text, cx.text_n); s->clip_in[cx.text_n] = 0; }
                            if (cx.html && (s->clip_html = malloc(cx.html_n + 1))) { memcpy(s->clip_html, cx.html, cx.html_n); s->clip_html[cx.html_n] = 0; s->clip_html_n = cx.html_n; }
                            if (cx.png && (s->clip_png = malloc(cx.png_n))) { memcpy(s->clip_png, cx.png, cx.png_n); s->clip_png_n = cx.png_n; }
                            s->clip_seq++;
                        }
                    }
                    free(t);
                }
            }
            break;
        case VM_WINDATA:
            if (fd >= 0) windata(s, &m, fd);
            break;
        case VM_GUEST_LAYOUT:
            s->layout[0] = m.a; s->layout[1] = m.b; s->layout[2] = (m.c >> 16) & 0xffff; s->layout[3] = m.c & 0xffff;
            s->layout[4] = (m.d >> 16) & 0xffff; s->layout[5] = m.d & 0xffff;
            layout_send(s);
            break;
        case VM_WINREFUSED:
            if (m.b == LAGA_CLOSE) refused_add(s, (uint32_t)m.a, m.c);
            break;
        case VM_SHELL_FD:
            if (fd >= 0 && m.seq == s->shell_seq) {
                if (s->shell_term) { Session *old = term_get(s->shell_term); if (old) term_close(old); }
                Session *ts = term_create_fd(fd, 10000);
                if (ts) { s->shell_term = ts->id; fd = -1; }
            }
            break;
        case VM_GUEST_NOTE:
            if (fd >= 0) guest_note(s, &m, fd);
            break;
        case VM_CONSOLE_FD:
            if (fd >= 0 && m.seq == s->console_seq) {
                if (s->console_term) { Session *old = term_get(s->console_term); if (old) term_close(old); }
                Session *ts = term_create_fd(fd, 5000);
                if (ts) { s->console_term = ts->id; fd = -1; }
            }
            break;
        case VM_JOB:
            s->job_kind = m.a; s->job_status = m.b; s->job_pct = m.c;
            snprintf(s->job_text, sizeof s->job_text, "%s", m.text);
            break;
        case VM_NOTICE:
            s->notice = m.a;
            s->notice_code = m.b;
            s->notice_seq++;
            snprintf(s->notice_text, sizeof s->notice_text, "%s", m.text);
            break;
        case VM_FILE_PROGRESS:
            for (int i = 0; i < 16; i++) if (s->fx[i].state == 1 && s->fx[i].seq == m.seq) { s->fx[i].pct = m.a; s->fx[i].via = m.b == 1; s->fx_changes++; }
            break;
        case VM_REPLY:
            if (fx_reply(s, &m)) break;
            if (s->ag_seq && m.seq == s->ag_seq) { s->ag_result = m.a == VME_OK ? 2 : 3; snprintf(s->ag_msg, sizeof s->ag_msg, "%s", m.text); s->ag_seq = 0; }
            if (s->xf_seq && m.seq == s->xf_seq) { s->xf_result = m.a == VME_OK ? 2 : 3; snprintf(s->xf_msg, sizeof s->xf_msg, "%s", m.text); s->xf_seq = 0; }
            if (s->cd_seq && m.seq == s->cd_seq) { s->cd_state = m.a == VME_OK ? 2 : 3; snprintf(s->cd_msg, sizeof s->cd_msg, "%s", m.text); s->cd_seq = 0; }
            if (m.a != VME_OK) {
                snprintf(s->last_error, sizeof s->last_error, "%s", m.text);
                if (m.seq == s->display_seq) { s->display_failed = 1; resume_once(s); }
            }
            break;
        case VM_DISPLAY_FD:
            if (fd < 0 || !vm_uuid_valid(s->id)) { s->display_failed = 1; resume_once(s); break; }
            drop_display(s, 1);
            spawn_vmdisplay(s, fd);
            fd = -1;
            resume_once(s);
            break;
        }
        if (fd >= 0) close(fd);
    }
}

static void set_picture(Sess *s, int w, int h) {
    size_t len = (size_t)w * h * 4;
    int fd = vm_picture_memfd(len);
    if (fd < 0) return;
    /* writable: a 3D machine's frames are read back into it here (db_read) */
    uint8_t *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return; }
    MediaSrc *m = s->media ? media_src_get(s->media) : NULL;
    /* a new picture (an attach, another size): no texture of the one before is shown over it, and the copies of a
     * vGPU's picture start again, whole */
    gout_free(s);
    if (m && m->gtex) { m->gtex = NULL; m->gpu_drawn = 0; }
    if (!m) m = media_src_new(SK_IMAGE, "machines");
    if (!m) { munmap(map, len); close(fd); return; }
    if (s->protect) media_src_protect(m);
    if (m->map) munmap(m->map, m->map_len);
    free(m->cache); m->cache = NULL; m->cache_w = m->cache_h = 0;
    m->map = map; m->map_len = len; m->px = (uint32_t *)map; m->w = w; m->h = h; m->opaque = 1; m->xrgb = 1; m->frame_seq++;
    /* a new memory: what the change test compares against (dmaread_rows_new) isn't in it; nor is a texture's frame */
    static unsigned rkeys;
    s->rkey = ++rkeys ? rkeys : ++rkeys;
    m->cpu_stale = 0;
    if (!s->media && s->cur) media_src_cursor(m->id, s->cur, s->cur_w, s->cur_h, s->cur_hx, s->cur_hy);
    /* Windows' display driver draws its pointer into the picture unless it was told to hand it over (HWCursor 1,
     * 7.1.2): until a shape comes (VD_CURSOR), LordOS's arrow over it would be a second one (2026-11-20) */
    if (!s->media && s->windows && !s->vgpu && !s->cur) media_src_cursor_guest_draws(m->id, 1);
    s->media = m->id; s->w = w; s->h = h;
    s->ack_pending = 0;
    VdMsg r = vd_msg(VD_SHM, 0, 0, 0, 0, 0, 0);
    vd_send(s->vd, &r, fd);
    close(fd);
}

/* VD_CURSOR's memfd (checked: sealed, w × h × 4 bytes): kept for the picture, or given to it */
static void guest_cursor(Sess *s, int fd, int w, int h, int hx, int hy) {
    size_t len = (size_t)w * h * 4;
    uint8_t *p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return;
    uint8_t *copy = malloc(len);
    if (copy) memcpy(copy, p, len);
    munmap(p, len);
    if (!copy) return;
    if (hx < 0 || hy < 0 || hx >= w || hy >= h) { free(copy); return; }
    /* an Intel vGPU's cursor plane read back with nothing visible in it (the guest's driver hasn't drawn one yet, or the
     * plane went off): the host's arrow, never no pointer at all (2026-11-22). Without a vGPU a blank image is an app
     * hiding the pointer on purpose */
    if (s->vgpu) {
        size_t k = 3;
        while (k < len && !copy[k]) k += 4;
        if (k >= len) {
            free(copy);
            free(s->cur); s->cur = NULL;
            if (s->media) media_src_cursor(s->media, NULL, 0, 0, 0, 0);
            return;
        }
    }
    free(s->cur);
    s->cur = copy; s->cur_w = w; s->cur_h = h; s->cur_hx = hx; s->cur_hy = hy;
    if (s->media) media_src_cursor(s->media, copy, w, h, hx, hy);
    /* the guest hands its pointer over (a Windows guest with viogpudo's HWCursor 1): LordOS draws it from now on,
     * where the host's pointer is, with no round trip through the guest */
    if (s->media) media_src_cursor_guest_draws(s->media, 0);
}

static void from_vd(Sess *s) {
    int frames = 0, last = 0;
    struct { int x0, y0, x1, y1, c; } du = { 0, 0, 0, 0, 0 };     /* the frames' changed area, together (2D) */
    for (int k = 0; k < VD_POLL_MAX && s->vd >= 0; k++) {
        VdMsg m;
        int fd = -1;
        int r = vd_recv(s->vd, &m, &fd);
        if (r < 0) { drop_display(s, 0); s->vd_state = VDS_LOST; break; }
        if (!r) break;
        if (!vd_msg_valid(&m, 0)) { if (fd >= 0) close(fd); continue; }
        switch (m.type) {
        case VD_READY: s->vd_up = 1; snd_apply(s); layout_send(s); break;
        case VD_SIZE: set_picture(s, m.a, m.b); snd_apply(s); break;
        case VD_GUEST_AUDIO:
            if (m.a == VDG_OUT) s->guest_out = m.b; else s->guest_in = m.b;
            snd_apply(s);
            break;
        case VD_FRAME:
            if (!s->media || !vd_rect_inside(&m, s->w, s->h)) break;
            if (s->db_cur >= 0) {
                /* a 3D picture: its rows are read back (or copied) below, once the GPU is done with them */
                if (!s->db_pending) { s->db_y0 = m.b; s->db_y1 = m.b + m.d; }
                else { if (m.b < s->db_y0) s->db_y0 = m.b; if (m.b + m.d > s->db_y1) s->db_y1 = m.b + m.d; }
                if (s->gpend >= 0) s->db_more = 1;  /* after the copy in flight */
                s->db_pending = 1; s->db_frame = m.e;
                break;
            }
            frames++; last = m.e;
            if (!du.c) { du.x0 = m.a; du.y0 = m.b; du.x1 = m.a + m.c; du.y1 = m.b + m.d; du.c = 1; }
            else { du.x0 = m.a < du.x0 ? m.a : du.x0; du.y0 = m.b < du.y0 ? m.b : du.y0; du.x1 = m.a + m.c > du.x1 ? m.a + m.c : du.x1; du.y1 = m.b + m.d > du.y1 ? m.b + m.d : du.y1; }
            break;
        case VD_DMABUF: db_take(s, &m, fd); fd = -1; break;
        case VD_CURSOR:
            /* the guest's pointer: shown instead of the host's over the picture (main.c, media_node_cursor) */
            if (fd >= 0 && !s->ptr_guest && !vm_cursor_check(fd, m.a, m.b)) guest_cursor(s, fd, m.a, m.b, m.c, m.d);
            break;
        case VD_POINTER:
            /* where the guest puts it doesn't matter (the host's pointer drives an absolute tablet); whether it shows does.
             * Captured, the guest's pointer image is drawn where the guest has it, inside the picture; the real pointer stays
             * at the picture's centre and clicks go straight to the guest (pointerlock.h), so this steers nothing */
            if (s->media) media_src_cursor_visible(s->media, m.c != 0);
            if (s->captured && s->media) {
                float x, y;
                if (m.c && m.a >= 0 && m.b >= 0 && m.a < s->w && m.b < s->h && !media_src_to_screen(s->media, m.a + 0.5f, m.b + 0.5f, &x, &y)) ptrlock_show(1, x, y);
                else ptrlock_show(0, 0, 0);
            }
            break;
        case VD_STATE: s->vd_state = m.a; break;
        default: break;
        }
        if (fd >= 0) close(fd);
    }
    MediaSrc *ms = s->media ? media_src_get(s->media) : NULL;
    int gpu_frame = 0;
    if (s->db_pending && s->pic_hidden && !s->rec && s->gpend < 0) {
        /* no window shows it: not read at all, acknowledged at once; the first frame shown again is read whole */
        s->db_pending = 0;
        s->db_skipped = 1;
        s->pic_skipped++;
        VdMsg a = vd_msg(VD_FRAME_DONE, 0, 0, 0, 0, s->db_frame, 0);
        vd_send(s->vd, &a, -1);
    } else if (s->db_pending) {
        int ry0 = 0, ry1 = s->h;
        int was_more = s->gpend >= 0 && s->db_more;
        int r = db_read(s, ms, &ry0, &ry1);
        if (r == 1 || r == 2) vmpace_frame(&s->pace, r == 1);
        if (r == 2) {
            /* nothing changed (QEMU's vfio display repeats the picture every refresh): acknowledged at once, so the
             * next one comes without the 250 ms wait, and nothing is painted or presented */
            s->db_pending = was_more;
            VdMsg a = vd_msg(VD_FRAME_DONE, 0, 0, 0, 0, s->db_frame, 0);
            vd_send(s->vd, &a, -1);
        } else if (r) {
            s->db_pending = was_more;
            frames++; last = s->db_frame; du.x0 = 0; du.x1 = s->w; du.y0 = ry0; du.y1 = ry1; du.c = 1;
            gpu_frame = r == 1 && ms && ms->gtex;
        }
    }
    if (frames && ms) {
        ms->frame_seq++;
        int shown;
        /* a frame in the texture the last paint drew: presenting again is enough (no UI damage, no paint) */
        if (gpu_frame && ms->gpu_drawn) { media_src_present(ms); shown = 1; }
        else shown = du.c ? media_src_damage_rect(ms, du.x0, du.y0, du.x1 - du.x0, du.y1 - du.y0) : (media_src_damage(ms), 1);
        /* what changed isn't in the part of the picture shown (zoomed in), or in apps mode in any of the guest's
         * windows shown here (or none is): nothing to paint, so the frame counts as read at once (the helper isn't
         * held 250 ms) */
        if (!shown && (s->apps_mode || ms->node)) ms->painted_seq = ms->frame_seq;
        /* the node it was drawn in went away (the window's tree was made again): one repaint of everything finds
         * the new node, and that paint binds it */
        else if (!ms->node && !media_src_crop_count(ms)) damage_add((Box){ 0, 0, 16384, 16384 });
        s->frames += frames;
        s->ack_pending = 1; s->ack_frame = last; s->ack_seq = ms->frame_seq;
    }
    pace_tell(s);
    /* the recording takes the picture as it changes, and at least once a second while it doesn't; with sound, every
     * frame's time (30 a second), so the picture's timeline runs as long as the sound's */
    if (s->rec && ms && ms->px) {
        MediaSrc *r = media_src_get(s->rec);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
        if (!r || r->kind != SK_SCREENREC) { s->rec = 0; if (s->rec_sound) { vdsend(s, VD_AUDIO_REC, 0, 0); s->rec_sound = 0; } }
        else if (!pic_out_ok(s, ms)) rec_end(s, r);         /* the picture became a protected window's: it ends here */
        else if ((frames || now - s->rec_fed > (s->rec_sound ? 1000.0 / 30 : 1000)) && media_framerec_feed(r, ms->px, ms->w, ms->h)) s->rec_fed = now;
    }
}

/* VD_FRAME_DONE once the engine read the picture (the media node painted that frame); a hidden window never
 * paints, and the helper goes on after 250 ms by itself */
static void ack(Sess *s) {
    if (!s->ack_pending || s->vd < 0) return;
    MediaSrc *ms = s->media ? media_src_get(s->media) : NULL;
    if (!ms || ms->painted_seq != s->ack_seq) return;
    VdMsg a = vd_msg(VD_FRAME_DONE, 0, 0, 0, 0, s->ack_frame, 0);
    vd_send(s->vd, &a, -1);
    s->ack_pending = 0;
}

/* ---- lord-server's main loop (main.c): the pictures are read there, not at the app's poll ----
 * Each display's socket is in the loop's poll(): a frame is taken (from_vd) as soon as lord-vmdisplay sends it and
 * acknowledged right after the paint that showed it (js_vm_painted, after render()). Before 2026-11-21 both waited
 * for the Machines app's poll every 16 ms, and the acknowledgement came at the poll after the next one: a guest
 * drawing 60 frames a second was shown at 31 (docs/MACHINES-GPU.md, Part 1). A 3D picture's dmabuf waiting for its
 * fence is polled too (it becomes readable when the GPU is done with it). */
static int vm_poll_sess[2 * MAX_SESS];

int js_vm_pollfds(struct pollfd *p, int max) {
    int n = 0;
    for (int i = 0; i < MAX_SESS && n < max; i++) {
        Sess *s = &sess[i];
        if (!s->used || s->vd < 0) continue;
        vm_poll_sess[n] = i;
        p[n++] = (struct pollfd){ s->vd, POLLIN, 0 };
        /* a copy in flight (a vGPU's picture): its fence; else a frame waiting for QEMU's dmabuf */
        int gfd = s->gpend >= 0 ? dmaread_out_fd(s->gout[s->gpend].o, NULL) : -1;
        if (gfd >= 0 && n < max) {
            vm_poll_sess[n] = i;
            p[n++] = (struct pollfd){ gfd, POLLIN, 0 };
        } else if (s->db_pending && s->db_cur >= 0 && s->db[s->db_cur].fd >= 0 && n < max) {
            vm_poll_sess[n] = i;
            p[n++] = (struct pollfd){ s->db[s->db_cur].fd, POLLIN, 0 };
        }
    }
    return n;
}

void js_vm_dispatch(const struct pollfd *p, int n) {
    int done[MAX_SESS] = { 0 };
    for (int k = 0; k < n && k < 2 * MAX_SESS; k++) {
        int i = vm_poll_sess[k];
        if (!p[k].revents || done[i]) continue;
        Sess *s = &sess[i];
        if (!s->used || s->vd < 0) continue;
        done[i] = 1;
        from_vd(s);
    }
}

/* after a paint: every frame it showed is acknowledged now, so lord-vmdisplay hands the guest's next one over at once */
void js_vm_painted(void) {
    for (int i = 0; i < MAX_SESS; i++) if (sess[i].used && sess[i].ack_pending) ack(&sess[i]);
}

static void read_err(Sess *s) {
    if (s->err < 0) return;
    char tmp[512];
    ssize_t r;
    while ((r = read(s->err, tmp, sizeof tmp - 1)) > 0) {
        tmp[r] = 0;
        size_t have = strlen(s->errtail), add = (size_t)r;
        if (add >= sizeof s->errtail) { memcpy(s->errtail, tmp + add - (sizeof s->errtail - 1), sizeof s->errtail - 1); s->errtail[sizeof s->errtail - 1] = 0; continue; }
        if (have + add >= sizeof s->errtail) { size_t drop = have + add - (sizeof s->errtail - 1); memmove(s->errtail, s->errtail + drop, have - drop + 1); have -= drop; }
        memcpy(s->errtail + have, tmp, add + 1);
    }
    if (r == 0) { close(s->err); s->err = -1; }
}

static JSValue f_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    if (!s) return JS_NULL;
    from_vm(s);
    if (s->vgpu && (s->state == VS_STARTING || s->state == VS_PAUSED)) gvt_memlock(s->dir);
    /* the picture's frames are read by lord-server's loop (js_vm_dispatch) and acknowledged after each paint
     * (js_vm_painted); this poll only reports the state */
    read_err(s);
    snd_apply(s);                       /* a permission given or taken since */
    MediaSrc *sm = s->media ? media_src_get(s->media) : NULL;
    if (sm && sm->audio) maudio_service(sm->audio);      /* what the helper wrote since, without waiting for PipeWire to ask */
    if (sm && sm->mic) maudio_service(sm->mic);
    if (s->vm < 0 && !s->ended) {
        int st;
        if (s->vm_pid > 0) {
            if (waitpid(s->vm_pid, &st, WNOHANG) == s->vm_pid) { s->exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st); s->vm_pid = 0; s->ended = 1; }
        } else s->ended = 1;
        if (s->ended) {
            read_err(s);
            drop_creds(s);
            /* lord-vm gone without a last state: it never started the machine, or it died */
            if (s->state != VS_STOPPED && s->state != VS_CRASHED) {
                /* it never ran (exit status from lord-vm's own checks), or it died while the machine ran */
                int ran = s->have_info || s->state == VS_RUNNING || s->state == VS_PAUSED || s->state == VS_STOPPING;
                s->state = ran ? VS_STOPPED : VS_CRASHED;
                s->reason = ran || s->exit_code >= 128 ? VR_SUPERVISOR_LOST : VR_START_FAILED;
            }
            drop_display(s, 1);
        }
    }
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "captured", JS_NewBool(c, s->captured));
    set_int(c, o, "captureEnds", s->cap_ends);
    set_int(c, o, "captureWhy", s->cap_why);
    set_str(c, o, "state", STATES[s->state]);
    set_str(c, o, "reason", REASONS[s->reason]);
    set_int(c, o, "code", s->code);
    set_str(c, o, "detail", s->detail);
    set_int(c, o, "vcpus", s->vcpus);
    set_int(c, o, "memoryMb", s->mem);
    set_int(c, o, "sandbox", s->sandbox);
    set_int(c, o, "cgroup", s->cg);
    const char *disp = s->vd >= 0 ? (s->media ? "up" : "connecting") : s->display_failed ? "none" : s->vd_state == VDS_LOST ? "lost" : s->display_asked ? "connecting" : "waiting";
    set_str(c, o, "display", disp);
    set_int(c, o, "w", s->w);
    set_int(c, o, "h", s->h);
    set_int(c, o, "media", s->vd >= 0 ? s->media : 0);
    JS_SetPropertyStr(c, o, "ended", JS_NewBool(c, s->ended));
    set_int(c, o, "exit", s->exit_code);
    set_str(c, o, "lastError", s->last_error);
    set_str(c, o, "stderr", s->errtail);
    set_int(c, o, "frames", s->frames);
    /* the picture's pacing (vm/vmpace.h): reads made, reads that changed nothing, frames not read while hidden, the pace now */
    set_int(c, o, "picReads", s->db_reads);
    set_int(c, o, "picSame", s->db_same);
    set_int(c, o, "picSkipped", s->pic_skipped);
    set_int(c, o, "pace", s->pace.kind);
    set_int(c, o, "cd", s->cd_state);
    if (s->console_term && !term_get(s->console_term)) s->console_term = 0;
    set_int(c, o, "console", s->console_term);
    /* the agent's shell: its terminal, and its size passed on to the guest when the terminal changed it */
    {
        Session *ts = s->shell_term ? term_get(s->shell_term) : NULL;
        if (s->shell_term && !ts) s->shell_term = 0;
        if (ts && ts->alive && (ts->t.cols != s->shell_cols || ts->t.rows != s->shell_rows) && ts->t.cols >= 1 && ts->t.rows >= 1 &&
            ts->t.cols <= AG_SHELL_COLS_MAX && ts->t.rows <= AG_SHELL_ROWS_MAX && s->vm >= 0) {
            VmMsg sm = vm_msg(VM_SHELL, ++s->seq, VSH_SIZE, ts->t.cols, ts->t.rows, 0, NULL);
            if (!vm_send(s->vm, &sm, -1)) { s->shell_cols = ts->t.cols; s->shell_rows = ts->t.rows; }
        }
        set_int(c, o, "shell", s->shell_term);
    }
    /* a link from the guest waiting for the person (6.12): shown from now on; and how many notifications wait */
    if (s->link[0]) {
        JSValue l = JS_NewObject(c);
        set_int(c, l, "seq", s->link_seq);
        set_str(c, l, "url", s->link);
        JS_SetPropertyStr(c, o, "guestLink", l);
        if (!s->link_seen) s->link_seen = mono_ms();
    }
    set_int(c, o, "guestLinkSeq", s->link[0] ? (int)s->link_seq : 0);
    set_int(c, o, "guestLinkDropped", s->link_dropped);
    set_int(c, o, "guestNotesSeq", s->note_seq);
    {
        JSValue ag = JS_NewObject(c);
        set_str(c, ag, "state", s->ag_state == VAS_CONNECTED ? "connected" : s->ag_state == VAS_WAITING ? "waiting" : "off");
        set_int(c, ag, "caps", s->ag_caps);
        const char *tab = strchr(s->ag_text, '\t');
        char ver[64] = "";
        snprintf(ver, sizeof ver, "%.*s", tab ? (int)(tab - s->ag_text) : 0, s->ag_text);
        set_str(c, ag, "version", ver);
        /* "version\tsystem\ttools" (vmproto.h): the tools' version is the LordOS tools' hello, "" before it */
        const char *tab2 = tab ? strchr(tab + 1, '\t') : NULL;
        char sys[VM_TEXT_MAX] = "";
        if (tab) snprintf(sys, sizeof sys, "%.*s", tab2 ? (int)(tab2 - tab - 1) : (int)strlen(tab + 1), tab + 1);
        set_str(c, ag, "system", sys);
        set_str(c, ag, "tools", tab2 ? tab2 + 1 : "");
        set_int(c, ag, "install", s->ag_result);
        JS_SetPropertyStr(c, ag, "lost", JS_NewBool(c, s->ag_seen && s->ag_state == VAS_WAITING && s->state == VS_RUNNING));
        set_str(c, ag, "message", s->ag_msg);
        set_int(c, ag, "transfer", s->xf_result);
        set_str(c, ag, "transferMessage", s->xf_msg);
        JS_SetPropertyStr(c, o, "agent", ag);
    }
    set_int(c, o, "clipIn", s->clip_seq);
    clip_ask_expire(s);
    set_int(c, o, "clipBlockedRead", s->blk_read);
    set_int(c, o, "clipBlockedWrite", s->blk_write);
    set_int(c, o, "clipAskSeq", s->ask_seq);
    {
        uint32_t g = 0;
        for (int f = AGF_TEXT; f <= AGF_PNG; f++) if (s->ask_gen[f]) g = s->ask_gen[f];
        set_int(c, o, "clipAsk", (int64_t)g);
    }
    set_str(c, o, "cdMessage", s->cd_msg);
    set_str(c, o, "notice", s->notice == VMN_NET_LOST ? "network-lost" : s->notice == VMN_RECOVERED ? "recovered" : s->notice == VMN_NET_BACK ? "network-back"
                          : s->notice == VMN_IDENTITY ? "identity" : "");
    set_int(c, o, "noticeCode", s->notice_code);
    set_int(c, o, "noticeSeq", s->notice_seq);
    if (s->job_kind) {
        static const char *const JK[VJ_COUNT] = { "", "suspend", "resume", "snapshot", "revert", "delete" };
        JSValue j = JS_NewObject(c);
        set_str(c, j, "kind", JK[s->job_kind]);
        set_str(c, j, "status", s->job_status == VJS_RUNNING ? "running" : s->job_status == VJS_DONE ? "done" : "failed");
        set_int(c, j, "pct", s->job_pct);
        set_str(c, j, "text", s->job_text);
        JS_SetPropertyStr(c, o, "job", j);
    }
    set_str(c, o, "noticeText", s->notice_text);
    {
        /* sound (6.9): mic "off" (not allowed, or the guest doesn't record), "wait" (the guest records and the machine
         * allows it: the app asks for LordOS's permission), "live" (the guest hears the microphone) */
        JSValue so = JS_NewObject(c);
        JS_SetPropertyStr(c, so, "on", JS_NewBool(c, s->snd_on));
        set_int(c, so, "volume", s->snd_vol);
        JS_SetPropertyStr(c, so, "microphone", JS_NewBool(c, s->mic_allowed));
        set_str(c, so, "output", s->snd_out);
        set_str(c, so, "input", s->snd_in);
        JS_SetPropertyStr(c, so, "playing", JS_NewBool(c, s->guest_out));
        JS_SetPropertyStr(c, so, "recording", JS_NewBool(c, s->guest_in));
        set_str(c, so, "mic", sm && sm->mic && s->mic_sent ? "live" : s->mic_wait ? "wait" : "off");
        JS_SetPropertyStr(c, so, "bytes", JS_NewFloat64(c, sm && sm->audio ? (double)maudio_bytes(sm->audio) : 0));
        JS_SetPropertyStr(c, so, "micBytes", JS_NewFloat64(c, sm && sm->mic ? (double)maudio_bytes(sm->mic) : 0));
        JS_SetPropertyStr(c, o, "sound", so);
    }
    set_int(c, o, "filesSeq", s->fx_changes);
    set_int(c, o, "windowsSeq", s->wins_seq);
    set_int(c, o, "iconsSeq", s->icons_seq);
    set_int(c, o, "guestAppsSeq", s->gapps_seq);
    set_int(c, o, "closeRefusedSeq", s->refused_seq);
    return o;
}

/* sendFile(session, path[, { open: true } | { place: { win, x, y, batch, index, count } }]) → request id (0: refused): a file dropped on the machine's window, copied
 * into the guest's Downloads by lord-vm through the guest agent. The file is opened here, for the person, read-only, and
 * only a regular file; lord-vm gets the descriptor and the name, never a path. With open (Files' "Open with" a machine,
 * docs/VIRTUALIZATION.md 7.3) the guest tools then open that copy in the guest: the path must resolve inside the
 * person's home folder or their mounts (fsops_allowed), is opened without following a last link, and lord-vm refuses
 * it unless the guest's tools said "open". With place (files dropped on an apps mode window, 7.2.1) the same checks,
 * then the guest tools hand the copy to that window. This exists only in the session's JS (never in an --app process
 * or a web view: they have no lord.vm), for Files' menu and Machines.
 * files(session) → [{ id, name, size, pct, state: 'copying'|'done'|'failed', message, via, open, place, placed }]: the guest's path or why not; finished ones are listed once (poll's filesSeq
 * counts changes) */
static JSValue f_send_file(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc >= 2 ? get(c, argv[0]) : NULL;
    const char *p = s ? JS_ToCString(c, argv[1]) : NULL;
    int slot = -1, fd = -1, want_open = 0, mode = 0;
    int32_t pw = 0, pc = 0, pd = 0;
    uint32_t q = 0;
    if (s && argc >= 3 && JS_IsObject(argv[2])) {
        JSValue v = JS_GetPropertyStr(c, argv[2], "open");
        want_open = JS_ToBool(c, v) == 1;
        JS_FreeValue(c, v);
        mode = want_open ? 1 : 0;
        /* place: { win, x, y, batch, index, count }: files dropped on an apps mode window (7.2.1), checked again by lord-vm */
        JSValue pl = JS_GetPropertyStr(c, argv[2], "place");
        if (JS_IsObject(pl)) {
            int32_t f[6] = { 0 };
            static const char *const K[6] = { "win", "x", "y", "batch", "index", "count" };
            int bad = 0;
            for (int i = 0; i < 6; i++) { JSValue x = JS_GetPropertyStr(c, pl, K[i]); if (JS_ToInt32(c, &f[i], x)) bad = 1; JS_FreeValue(c, x); }
            if (!bad && f[0] && f[1] >= 0 && f[1] <= LAG_DIM_MAX && f[2] >= 0 && f[2] <= LAG_DIM_MAX && f[3] >= 1 && f[3] <= 32767 && f[5] >= 1 && f[5] <= AG_PLACE_MAX && f[4] >= 0 && f[4] < f[5]) {
                mode = 2; want_open = 1;
                pw = f[0]; pc = f[1] | f[2] << 16; pd = f[3] << 16 | f[4] << 8 | f[5];
            } else want_open = -1;          /* asked and malformed: refused */
        }
        JS_FreeValue(c, pl);
    }
    if (s) for (int i = 0; i < 16; i++) if (!s->fx[i].state) { slot = i; break; }
    if (want_open < 0) { /* refused below: no descriptor */ }
    else if (want_open && p) {
        /* opened in the guest: only the person's own files (home, their mounts), the resolved path, no last link */
        char real[PATH_MAX];
        if (p[0] == '/' && slot >= 0 && s->vm >= 0 && realpath(p, real) && fsops_allowed(real))
            fd = open(real, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK | O_NOFOLLOW);
    } else if (p && p[0] == '/' && slot >= 0 && s->vm >= 0) fd = open(p, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    struct stat st;
    if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode)) {
        /* the name, at most VM_TEXT_MAX - 1 bytes: a long one keeps its start (on a character) and its extension;
         * lord-vm makes it safe for the guest */
        const char *base = strrchr(p, '/') + 1;
        char name[VM_TEXT_MAX];
        size_t bl = strlen(base);
        if (bl < sizeof name) memcpy(name, base, bl + 1);
        else {
            const char *dot = strrchr(base, '.');
            size_t el = dot && strlen(dot) <= 16 ? strlen(dot) : 0, hl = sizeof name - 1 - el;
            while (hl > 0 && ((unsigned char)base[hl] & 0xc0) == 0x80) hl--;
            memcpy(name, base, hl);
            memcpy(name + hl, base + bl - el, el);
            name[hl + el] = 0;
        }
        if (bl && name[0]) {
            VmMsg m = vm_msg(VM_FILE, ++s->seq, mode, pw, pc, pd, name);
            if (!vm_send(s->vm, &m, fd)) {
                q = m.seq;
                s->fx[slot].seq = q; s->fx[slot].state = 1; s->fx[slot].pct = 0; s->fx[slot].via = 0; s->fx[slot].open = mode; s->fx[slot].how = 0; s->fx[slot].size = (long long)st.st_size;
                snprintf(s->fx[slot].name, sizeof s->fx[slot].name, "%s", name);
                s->fx[slot].msg[0] = 0;
                s->fx_changes++;
            }
        }
    }
    if (fd >= 0) close(fd);
    if (p) JS_FreeCString(c, p);
    return JS_NewUint32(c, q);
}
static JSValue f_files(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    JSValue a = JS_NewArray(c);
    if (!s) return a;
    uint32_t k = 0;
    for (int i = 0; i < 16; i++) {
        if (!s->fx[i].state) continue;
        /* lord-vm is gone: what it had is lost */
        if (s->fx[i].state == 1 && s->vm < 0) { s->fx[i].state = 3; snprintf(s->fx[i].msg, sizeof s->fx[i].msg, "the machine stopped"); }
        JSValue o = JS_NewObject(c);
        set_int(c, o, "id", (int)s->fx[i].seq);
        set_str(c, o, "name", s->fx[i].name);
        JS_SetPropertyStr(c, o, "size", JS_NewInt64(c, s->fx[i].size));
        set_int(c, o, "pct", s->fx[i].pct);
        set_str(c, o, "state", s->fx[i].state == 1 ? "copying" : s->fx[i].state == 2 ? "done" : "failed");
        set_str(c, o, "message", s->fx[i].msg);
        set_str(c, o, "via", s->fx[i].via ? "transfer" : "agent");
        JS_SetPropertyStr(c, o, "open", JS_NewBool(c, s->fx[i].open == 1));
        /* a drop on an apps mode window: how the guest took it */
        static const char *const HOW[] = { "", "folder", "window", "clipboard" };
        if (s->fx[i].open == 2) { JS_SetPropertyStr(c, o, "place", JS_TRUE); set_str(c, o, "placed", HOW[s->fx[i].how]); }
        JS_SetPropertyUint32(c, a, k++, o);
        if (s->fx[i].state != 1) s->fx[i].state = 0;
    }
    return a;
}

/* transferFolder(dir) → the machine's transfer folder (6.7.1), made and claimed for it as lord-vm does at its start
 * (vm_transfer_claim: a folder of that name that's another machine's moves aside), or null; for Open Transfer Folder
 * before the first start. transferAside(dir) → bool: before the machine goes to the Trash, its own transfer folder moves
 * aside to "<name> (old YYYY-MM-DD)" (kept, never deleted), so a new machine of that name doesn't get its files */
static int machine_id_of(const char *dir, char id[37]) {
    VmMachine m;
    char err[256];
    if (vm_machine_load(dir, &m, err, sizeof err) != VM_OK) return -1;
    memcpy(id, m.id, 37);
    return 0;
}
static JSValue f_transfer_folder(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char dir[PATH_MAX], xp[PATH_MAX], id[37], moved[NAME_MAX + 1];
    if (argc < 1 || machine_dir(c, argv[0], dir, sizeof dir) || machine_id_of(dir, id)) return JS_NULL;
    if (vm_transfer_claim(dir, id, xp, sizeof xp, moved, sizeof moved)) return JS_NULL;
    struct stat st;
    if (lstat(xp, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) return JS_NULL;
    return JS_NewString(c, xp);
}
static JSValue f_transfer_aside(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char dir[PATH_MAX], id[37], moved[NAME_MAX + 1];
    if (argc < 1 || machine_dir(c, argv[0], dir, sizeof dir) || machine_id_of(dir, id)) return JS_FALSE;
    return JS_NewBool(c, vm_transfer_aside(dir, id, moved, sizeof moved) >= 0);
}

/* renamePlan(dir, name) → { problem, moves, folder, other }: what renaming the machine to name does (6.26; vm_rename_plan,
 * read-only): problem null or 'badName', 'nameTaken' (other: that machine's name), 'folderTaken', 'transferTaken';
 * moves: its folder and transfer folder get the name folder. lord-vm checks it all again under the machine's lock */
static JSValue f_rename_plan(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char dir[PATH_MAX];
    if (argc < 2 || !JS_IsString(argv[1]) || machine_dir(c, argv[0], dir, sizeof dir)) return JS_NULL;
    const char *name = JS_ToCString(c, argv[1]);
    if (!name) return JS_NULL;
    VmRenamePlan rp;
    int pr = vm_rename_plan(dir, name, &rp);
    JS_FreeCString(c, name);
    static const char *const P[] = { NULL, "badName", "nameTaken", "folderTaken", "transferTaken", "badName" };
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "problem", pr > 0 && pr < (int)(sizeof P / sizeof P[0]) ? JS_NewString(c, P[pr]) : JS_NULL);
    JS_SetPropertyStr(c, o, "moves", JS_NewBool(c, rp.moves));
    set_str(c, o, "folder", rp.folder);
    set_str(c, o, "other", rp.other);
    return o;
}
/* rename(dir, name) → job id: lord-vm --rename. Off: the name, then its folder and transfer folder (the job's out:
 * { dir, folder: 'moved'|'same'|'kept', why }); running: the name only (folder 'later' when it would move); exit 5 and
 * { problem } when the name or a folder's name is taken */
static JSValue f_rename(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char dir[PATH_MAX], name[VM_NAME_MAX + 1];
    if (argc < 2 || !JS_IsString(argv[1]) || machine_dir(c, argv[0], dir, sizeof dir)) return JS_NewInt32(c, 0);
    const char *s = JS_ToCString(c, argv[1]);
    int ok = s && vm_rename_name_ok(s);
    if (ok) snprintf(name, sizeof name, "%s", s);
    if (s) JS_FreeCString(c, s);
    if (!ok) return JS_NewInt32(c, 0);
    char *args[] = { "lord-vm", "--rename", dir, name, NULL };
    return JS_NewInt32(c, spawn_job(args, dir));
}

/* cdrom(id, path | null) → bool: a disc into the running machine's drive (the file is opened here, for the person, and
 * handed to lord-vm), or out with null. poll's cd says how it went (1 waiting, 2 done, 3 failed: cdMessage). */
static JSValue f_cdrom(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc >= 1 ? get(c, argv[0]) : NULL;
    if (!s || s->vm < 0 || s->cd_state == 1) return JS_FALSE;
    char path[PATH_MAX] = "";
    int fd = -1;
    if (argc >= 2 && JS_IsString(argv[1])) {
        if (user_file(c, argv[1], path, sizeof path)) return JS_FALSE;
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
        if (fd < 0) return JS_FALSE;
    }
    VmMsg m = vm_msg(VM_CDROM, ++s->seq, 0, 0, 0, 0, strlen(path) < VM_TEXT_MAX ? path : "");
    int ok = !vm_send(s->vm, &m, fd);
    if (fd >= 0) close(fd);
    if (ok) { s->cd_seq = s->seq; s->cd_state = 1; s->cd_msg[0] = 0; }
    return JS_NewBool(c, ok);
}

static JSValue f_cmd(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc >= 2 ? get(c, argv[0]) : NULL;
    const char *w = argc >= 2 ? JS_ToCString(c, argv[1]) : NULL;
    int ok = 0;
    if (s && w) {
        if (!strcmp(w, "shutdown")) ok = ask(s, VM_SHUTDOWN, 0) > 0;
        else if (!strcmp(w, "poweroff")) ok = ask(s, VM_POWEROFF, 0) > 0;
        else if (!strcmp(w, "pause")) ok = ask(s, VM_PAUSE, 0) > 0;
        else if (!strcmp(w, "resume")) ok = ask(s, VM_RESUME, 0) > 0;
        else if (!strcmp(w, "reset")) ok = ask(s, VM_RESET, 0) > 0;
        else if (!strcmp(w, "suspend")) ok = ask(s, VM_SUSPEND, 0) > 0;
        else if (!strcmp(w, "keep")) ok = ask(s, VM_KEEP, 1) > 0;
        else if (!strcmp(w, "nokeep")) ok = ask(s, VM_KEEP, 0) > 0;
        else if (!strcmp(w, "display")) {
            /* the picture again after lord-vmdisplay went away */
            if (s->vd < 0 && (s->state == VS_RUNNING || s->state == VS_PAUSED)) { s->display_asked = 0; s->display_failed = 0; s->vd_state = -1; maybe_display(s); ok = 1; }
        }
    }
    if (w) JS_FreeCString(c, w);
    return JS_NewBool(c, ok);
}

static JSValue f_key(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; Sess *s = argc >= 3 ? get(c, argv[0]) : NULL;
    int k = s ? ival(c, argv[1]) : 0;
    if (s && k > 0 && k < 768) vdsend(s, VD_KEY, k, ival(c, argv[2]) ? 1 : 0);
    return JS_UNDEFINED;
}
static JSValue f_motion(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; Sess *s = argc >= 3 ? get(c, argv[0]) : NULL;
    if (s && s->w > 0) {
        int x = ival(c, argv[1]), y = ival(c, argv[2]);
        x = x < 0 ? 0 : x >= s->w ? s->w - 1 : x;
        y = y < 0 ? 0 : y >= s->h ? s->h - 1 : y;
        vdsend(s, VD_MOTION, x, y);
    }
    return JS_UNDEFINED;
}
static JSValue f_button(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; Sess *s = argc >= 3 ? get(c, argv[0]) : NULL;
    if (s) {
        int b = ival(c, argv[1]);
        int q = b == 0 ? VDB_LEFT : b == 1 ? VDB_MIDDLE : b == 2 ? VDB_RIGHT : b == 3 ? VDB_SIDE : b == 4 ? VDB_EXTRA : -1;
        if (q >= 0) vdsend(s, VD_BUTTON, q, ival(c, argv[2]) ? 1 : 0);
    }
    return JS_UNDEFINED;
}
static JSValue f_wheel(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; Sess *s = argc >= 3 ? get(c, argv[0]) : NULL;
    if (s) {
        int dy = ival(c, argv[1]), dx = ival(c, argv[2]);
        dy = dy < -32 ? -32 : dy > 32 ? 32 : dy;
        dx = dx < -32 ? -32 : dx > 32 ? 32 : dx;
        if (dy || dx) vdsend(s, VD_WHEEL, dy, dx);
    }
    return JS_UNDEFINED;
}
/* "Capture mouse" (docs/VIRTUALIZATION.md 6.25): the mouse's motion straight to lord-vmdisplay, no JavaScript in between */
static void cap_motion(void *ud, int dx, int dy) {
    Sess *s = ud;
    while (dx || dy) {
        int a = dx > VD_REL_MAX ? VD_REL_MAX : dx < -VD_REL_MAX ? -VD_REL_MAX : dx, b = dy > VD_REL_MAX ? VD_REL_MAX : dy < -VD_REL_MAX ? -VD_REL_MAX : dy;
        vdsend(s, VD_REL, a, b);
        dx -= a; dy -= b;
    }
}
/* buttons and the wheel straight to the guest, with no hit test: whatever the guest says about its pointer, a click
 * never lands on anything of this computer's (pointerlock.h) */
static void cap_button(void *ud, int b, int down) {
    Sess *s = ud;
    int q = b == 0 ? VDB_LEFT : b == 1 ? VDB_MIDDLE : b == 2 ? VDB_RIGHT : b == 3 ? VDB_SIDE : b == 4 ? VDB_EXTRA : -1;
    if (q >= 0) vdsend(s, VD_BUTTON, q, down ? 1 : 0);
}
static void cap_wheel(void *ud, int dy, int dx) {
    Sess *s = ud;
    dy = dy < -32 ? -32 : dy > 32 ? 32 : dy;
    dx = dx < -32 ? -32 : dx > 32 ? 32 : dx;
    if (dy || dx) vdsend(s, VD_WHEEL, dy, dx);
}
/* ended by the window, ❖Esc or Ctrl+Alt, the picture's node going away or the input leaving: the tablet again, and
 * nothing left held in the guest (a button pressed while captured is released here, out of the window's sight) */
static void cap_ended(void *ud, int why) {
    Sess *s = ud;
    vdsend(s, VD_RELEASE_ALL, 0, 0);
    s->captured = 0;
    s->cap_ends++;
    s->cap_why = why;
    vdsend(s, VD_CAPTURE, 0, 0);
    ask(s, VM_CAPTURE, 0);
}
/* capture(sid, on, node) → true when captured (on) or released: node is the picture's box (the pointer stays inside it).
 * Only a running machine with its picture; apps mode never asks (vmwindow.jsx) */
static JSValue f_capture(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc >= 2 ? get(c, argv[0]) : NULL;
    if (!s) return JS_FALSE;
    if (!JS_ToBool(c, argv[1])) { ptrlock_end_owner(s); return JS_TRUE; }
    int node = argc >= 3 ? ival(c, argv[2]) : 0;
    if (s->captured) return JS_NewBool(c, !ptrlock_retarget(s, node));     /* the window was rebuilt (full screen) */
    if (s->vd < 0 || !s->media || s->vm < 0 || (s->state != VS_RUNNING && s->state != VS_PAUSED) || node <= 0) return JS_FALSE;
    PtrLockSink k = { .motion = cap_motion, .ended = cap_ended, .ud = s, .button = cap_button, .wheel = cap_wheel };
    /* lord-vmdisplay first: from now on it takes only relative motion */
    vdsend(s, VD_CAPTURE, 1, 0);
    if (ptrlock_begin(node, &k)) { vdsend(s, VD_CAPTURE, 0, 0); return JS_FALSE; }
    s->captured = 1;
    ask(s, VM_CAPTURE, 1);
    return JS_TRUE;
}
static JSValue f_release(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; Sess *s = get(c, argv[0]); if (s) vdsend(s, VD_RELEASE_ALL, 0, 0); return JS_UNDEFINED; }
static JSValue f_resize(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; Sess *s = argc >= 3 ? get(c, argv[0]) : NULL;
    int w = s ? ival(c, argv[1]) : 0, h = s ? ival(c, argv[2]) : 0;
    if (s && w >= 320 && h >= 200 && w <= (int)VD_MAX_DIM && h <= (int)VD_MAX_DIM) {
        vdsend(s, VD_RESIZE, w, h);
        /* and to lord-vm: a guest whose helper sets its own display's size (Windows) gets it from there */
        if (s->vm >= 0) { VmMsg m = vm_msg(VM_DISPLAY_SIZE, ++s->seq, w, h, 0, 0, NULL); vm_send(s->vm, &m, -1); }
    }
    return JS_UNDEFINED;
}
static JSValue f_rate(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; Sess *s = argc >= 2 ? get(c, argv[0]) : NULL;
    int hz = s ? ival(c, argv[1]) : 0;
    if (s && hz >= 1 && hz <= 240) vdsend(s, VD_RATE, hz, 0);
    return JS_UNDEFINED;
}

/* lord.vm.shown(sid, on): whether any window shows the picture (the machine's, or one of its apps mode windows: the app
 * decides with the SDK's windowsOutOfSight). Hidden, its frames aren't read and lord-vmdisplay takes them less often
 * (vm/vmpace.h); shown again, the next frame is read whole, and a 3D picture's current one at once */
static JSValue f_shown(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; Sess *s = argc >= 2 ? get(c, argv[0]) : NULL;
    if (!s) return JS_UNDEFINED;
    int hidden = !JS_ToBool(c, argv[1]);
    if (hidden == s->pic_hidden) return JS_UNDEFINED;
    s->pic_hidden = hidden;
    if (!hidden && s->db_skipped) {
        s->db_skipped = 0;
        s->gsrc = 0;                            /* both copies of a vGPU's picture lack the frames not read */
        if (s->db_cur >= 0) { s->db_y0 = 0; s->db_y1 = s->h; s->db_pending = 1; }
    }
    pace_tell(s);
    return JS_UNDEFINED;
}

/* the window let go of the machine: a machine this session started is shut down by lord-vm when its control
 * channel closes (ACPI, then off after 60 s); an attached one keeps running */
static JSValue f_close(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    if (!s) return JS_UNDEFINED;
    if (s->rec) { MediaSrc *r = media_src_get(s->rec); if (r) media_src_close(r); s->rec = 0; }     /* finished properly, "done" follows */
    if (s->rec_sound) { vdsend(s, VD_AUDIO_REC, 0, 0); s->rec_sound = 0; }
    if (s->console_term) { Session *ts = term_get(s->console_term); if (ts) term_close(ts); s->console_term = 0; }
    if (s->shell_term) { Session *ts = term_get(s->shell_term); if (ts) term_close(ts); s->shell_term = 0; }
    s->link[0] = 0; s->note_n = 0;
    free(s->clip_in); s->clip_in = NULL;
    free(s->clip_html); s->clip_html = NULL; free(s->clip_png); s->clip_png = NULL;
    drop_display(s, 1);
    drop_creds(s);
    if (s->vm >= 0) close(s->vm);
    if (s->err >= 0) close(s->err);
    /* lord-vm finishes on its own (up to a minute for an ACPI shutdown): reaped later */
    if (s->vm_pid > 0 && waitpid(s->vm_pid, NULL, WNOHANG) == 0) later(s->vm_pid);
    MediaSrc *m = s->media ? media_src_get(s->media) : NULL;
    if (m) media_src_close(m);
    free(s->cur);
    win_free(s);
    memset(s, 0, sizeof *s);
    return JS_UNDEFINED;
}

/* one of sound()'s device names: "" (the default) or a node.name as machine.json keeps it; 0 ok (out set or kept), -1 refused */
static int snd_dev_arg(JSContext *c, JSValueConst o, const char *key, char *out, size_t n) {
    JSValue v = JS_GetPropertyStr(c, o, key);
    int r = 0;
    if (JS_IsString(v)) {
        const char *x = JS_ToCString(c, v);
        if (x && (!*x || vm_sound_dev_valid(x)) && strlen(x) < n) snprintf(out, n, "%s", x); else r = -1;
        if (x) JS_FreeCString(c, x);
    } else if (!JS_IsUndefined(v)) r = -1;
    JS_FreeValue(c, v);
    return r;
}

/* sound(dir, { sound, volume, microphone, output, input }) → { live, job }: the machine's sound choices (6.9); output and
 * input are a sound device's node.name (as lord.sound's state lists them) or "" for the system's default. A machine whose window is
 * here gets them now (this side plays and records) and its lord-vm writes machine.json (VM_SOUND); one running without
 * a window gets VM_SOUND through its runtime socket; one that is off gets lord-vm --set (job). null when refused. */
static JSValue f_sound(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char dir[PATH_MAX];
    VmMachine m;
    if (argc < 2 || !JS_IsObject(argv[1]) || machine_dir(c, argv[0], dir, sizeof dir) || vm_machine_load(dir, &m, NULL, 0)) return JS_NULL;
    int so = m.sound, vo = m.volume, mi = m.microphone, ok = 1;
    char out[VM_SOUND_DEV_MAX + 1], in[VM_SOUND_DEV_MAX + 1], devs[2 * VM_SOUND_DEV_MAX + 2];
    memcpy(out, m.sound_out, sizeof out);
    memcpy(in, m.sound_in, sizeof in);
    if (snd_dev_arg(c, argv[1], "output", out, sizeof out) || snd_dev_arg(c, argv[1], "input", in, sizeof in)) return JS_NULL;
    snprintf(devs, sizeof devs, "%s\t%s", out, in);
    JSValue v = JS_GetPropertyStr(c, argv[1], "sound");
    if (JS_IsBool(v)) so = JS_ToBool(c, v); else if (!JS_IsUndefined(v)) ok = 0;
    JS_FreeValue(c, v);
    v = JS_GetPropertyStr(c, argv[1], "microphone");
    if (JS_IsBool(v)) mi = JS_ToBool(c, v); else if (!JS_IsUndefined(v)) ok = 0;
    JS_FreeValue(c, v);
    v = JS_GetPropertyStr(c, argv[1], "volume");
    if (JS_IsNumber(v)) { double d = 0; JS_ToFloat64(c, &d, v); if (d >= 0 && d <= 100) vo = (int)(d + 0.5); else ok = 0; } else if (!JS_IsUndefined(v)) ok = 0;
    JS_FreeValue(c, v);
    if (!ok) return JS_NULL;
    int live = 0, job = 0;
    for (int i = 0; i < MAX_SESS; i++) {
        Sess *s = &sess[i];
        if (!s->used || s->vm < 0 || s->ended || strcmp(s->dir, dir)) continue;
        VmMsg q = vm_msg(VM_SOUND, ++s->seq, so, vo, mi, 1, devs);
        if (vm_send(s->vm, &q, -1)) continue;
        s->snd_on = so; s->snd_vol = vo; s->mic_allowed = mi;
        memcpy(s->snd_out, out, sizeof s->snd_out);
        memcpy(s->snd_in, in, sizeof s->snd_in);
        snd_apply(s);
        live = 1;
    }
    if (!live) {
        /* running without a window here: its runtime socket, one request, its answer awaited briefly */
        const char *rt = getenv("XDG_RUNTIME_DIR");
        char base[256], path[sizeof ((struct sockaddr_un *)0)->sun_path];
        if (rt && rt[0] == '/') snprintf(base, sizeof base, "%s", rt); else snprintf(base, sizeof base, "/run/user/%u", (unsigned)getuid());
        int fd = (size_t)snprintf(path, sizeof path, "%s/lordos-vm/%s.sock", base, m.id) < sizeof path ? socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0) : -1;
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        if (fd >= 0) memcpy(a.sun_path, path, strlen(path) + 1);
        if (fd >= 0 && !connect(fd, (struct sockaddr *)&a, sizeof a)) {
            VmMsg q = vm_msg(VM_SOUND, 1, so, vo, mi, 1, devs);
            if (!vm_send(fd, &q, -1)) {
                for (int k = 0; k < 8; k++) {
                    struct pollfd pf = { fd, POLLIN, 0 };
                    if (poll(&pf, 1, 60) <= 0) continue;
                    VmMsg r;
                    int rfd = -1, got = vm_recv(fd, &r, &rfd);
                    if (rfd >= 0) close(rfd);
                    if (got <= 0) break;
                    if (r.type == VM_REPLY && r.seq == 1) { live = r.a == VME_OK; break; }
                }
            }
        }
        if (fd >= 0) close(fd);
    }
    if (!live) {
        char sv[8];
        snprintf(sv, sizeof sv, "%d", vo);
        char *args[] = { "lord-vm", "--set", dir, "--sound", so ? "on" : "off", "--volume", sv, "--microphone", mi ? "on" : "off",
                         "--sound-out", out[0] ? out : "default", "--sound-in", in[0] ? in : "default", NULL };
        job = spawn_job(args, dir);
        if (!job) return JS_NULL;
    }
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "live", JS_NewBool(c, live));
    set_int(c, o, "job", job);
    return o;
}

/* a machine with no window here (started elsewhere, or kept running): one request through its runtime socket */
static JSValue f_stop(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_FALSE;
    JSValue v = f_attach(c, t, 1, argv);
    int slot = ival(c, v);
    JS_FreeValue(c, v);
    Sess *s = slot >= 1 && slot <= MAX_SESS && sess[slot - 1].used ? &sess[slot - 1] : NULL;
    if (!s) return JS_FALSE;
    const char *w = JS_ToCString(c, argv[1]);
    /* shutdown, poweroff, pause, resume, reset (the library's menu) */
    int type = !w ? -1 : !strcmp(w, "poweroff") ? VM_POWEROFF : !strcmp(w, "shutdown") ? VM_SHUTDOWN : !strcmp(w, "pause") ? VM_PAUSE
             : !strcmp(w, "resume") ? VM_RESUME : !strcmp(w, "reset") ? VM_RESET : !strcmp(w, "suspend") ? VM_SUSPEND : -1;
    int seq = type >= 0 ? ask(s, (uint16_t)type, 0) : -1;
    int ok = seq > 0;
    if (w) JS_FreeCString(c, w);
    /* lord-vm answers each request: wait for that answer (half a second at most), so it isn't sent to a closed socket */
    for (int k = 0; ok && k < 8; k++) {
        struct pollfd pf = { s->vm, POLLIN, 0 };
        if (poll(&pf, 1, 60) <= 0) continue;
        VmMsg r;
        int fd = -1;
        int got = vm_recv(s->vm, &r, &fd);
        if (fd >= 0) close(fd);
        if (got <= 0) break;
        if (r.type == VM_REPLY && r.seq == (uint32_t)seq) break;
    }
    close(s->vm);
    win_free(s);
    memset(s, 0, sizeof *s);
    return JS_NewBool(c, ok);
}

static JSValue f_log(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    char dir[PATH_MAX];
    if (machine_dir(c, argv[0], dir, sizeof dir)) return JS_NewString(c, "");
    /* log(dir[, which, bytes]): the end of lord-vm.log ("vm", the default) or of the guest's serial console ("serial"),
     * 4 KB by default and 256 KB at most */
    int max = argc > 2 ? ival(c, argv[2]) : 4096;
    if (max < 1024) max = 1024;
    if (max > 262144) max = 262144;
    const char *which = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(c, argv[1]) : NULL;
    int serial = which && !strcmp(which, "serial"), journal = which && !strcmp(which, "journal");
    if (which) JS_FreeCString(c, which);
    char p[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/log/%s", dir, serial ? "serial.log" : journal ? "journal.log" : "lord-vm.log");
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return JS_NewString(c, "");
    char *buf = malloc((size_t)max + 1);
    if (!buf) { close(fd); return JS_NewString(c, ""); }
    off_t end = lseek(fd, 0, SEEK_END);
    off_t from = end > max ? end - max : 0;
    ssize_t n = pread(fd, buf, (size_t)max, from);
    close(fd);
    if (n <= 0) { free(buf); return JS_NewString(c, ""); }
    buf[n] = 0;
    char *s = buf;
    if (from) { char *nl = strchr(s, '\n'); if (nl) s = nl + 1; }
    /* the serial console's color codes and control characters go */
    vm_log_clean(s);
    JSValue r = JS_NewString(c, s);
    free(buf);
    return r;
}

static JSValue f_snapshot(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    MediaSrc *m = s && s->media ? media_src_get(s->media) : NULL;
    if (m && !pic_out_ok(s, m)) return JS_FALSE;        /* a protected machine has no card picture (thumb.png) */
    media_src_need_cpu(m);              /* a picture shown as a texture: read back now */
    if (!m || !m->px || m->w < 1 || m->h < 1) return JS_FALSE;
    /* a group's member under the anonymity profile (docs/VIRTUALIZATION.md 19.5): no picture of it stays on disk */
    VmMachine mm;
    if (vm_machine_load(s->dir, &mm, NULL, 0) || mm.profile_lock) return JS_FALSE;
    return JS_NewBool(c, !vm_thumb_save_async(s->dir, m->px, m->w, m->h, VM_THUMB_W));
}

/* protect(session) → bool: the machine is a protected one (Machines: the anonymity profile, docs/VIRTUALIZATION.md 19.5):
 * from now on, for the session's life, its picture never leaves as a picture of its own (no card picture, screenshot,
 * recording or snapshot picture; pic_out_ok), a recording running stops, and the card picture it had (thumb.png) is
 * removed. One way: nothing turns it off. Its windows are protected by the app (wm.open's protected), so the screen's
 * captures show them as the protected fill. */
static JSValue f_protect(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    if (!s) return JS_FALSE;
    s->protect = 1;
    MediaSrc *m = s->media ? media_src_get(s->media) : NULL;
    media_src_protect(m);
    if (s->rec) rec_end(s, media_src_get(s->rec));
    char p[PATH_MAX + 16];
    if (s->dir[0] == '/' && snprintf(p, sizeof p, "%s/thumb.png", s->dir) < (int)sizeof p) unlink(p);
    return JS_TRUE;
}

/* screenshot(session) → the file's path, or "": the guest's picture at its size, in ~/Pictures/Screenshots as
 * "<machine> <date> <time>.png" */
static JSValue f_screenshot(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    MediaSrc *m = s && s->media ? media_src_get(s->media) : NULL;
    const char *home = getenv("HOME");
    if (m && !pic_out_ok(s, m)) return JS_NewString(c, "");     /* a protected machine: no screenshot of it */
    media_src_need_cpu(m);
    if (!m || !m->px || !home || home[0] != '/') return JS_NewString(c, "");
    char dir[PATH_MAX], name[256], path[PATH_MAX + 260];
    snprintf(dir, sizeof dir, "%s/Pictures", home); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/Pictures/Screenshots", home); mkdir(dir, 0755);
    VmMachine vm;
    char err[128];
    const char *base = !vm_machine_load(s->dir, &vm, err, sizeof err) ? vm.name : "Machine";
    char clean[VM_NAME_MAX + 1];
    size_t k = 0;
    for (const char *q = base; *q && k + 1 < sizeof clean; q++) clean[k++] = (*q == '/' || (unsigned char)*q < 0x20) ? '-' : *q;
    clean[k] = 0;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char when[40];
    strftime(when, sizeof when, "%Y-%m-%d %H.%M.%S", &tmv);
    snprintf(name, sizeof name, "%s %s.png", clean, when);
    if (vm_png_save(dir, name, m->px, m->w, m->h, 8192)) return JS_NewString(c, "");
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return JS_NewString(c, path);
}

/* agent(session, 'install'|'update'|'timesync'|'shutdown') → bool: a guest agent request (poll's agent.install says how
 * Install went). 'update' is Install asked by Machines itself for older LordOS tools: the same install, marked so
 * lord-vm's log says it */
/* WinFsp's installer for agent(id, 'transfer', path): only a file the admin helper keeps in the machines' image store,
 * root's, named by its SHA-256 (images/<sha256>.iso), whose record there says it's the catalog's "winfsp" entry. The
 * descriptor, and the hash as text, go to lord-vm, which hashes the file again; the guest checks its signature too */
#define VM_IMAGE_STORE "/var/lib/lordos/machines/images/"
static int winfsp_file(JSContext *c, JSValueConst v, char sha[65]) {
    const char *p = JS_IsString(v) ? JS_ToCString(c, v) : NULL;
    size_t sl = strlen(VM_IMAGE_STORE);
    int ok = p && !strncmp(p, VM_IMAGE_STORE, sl) && strlen(p) == sl + 68 && !strcmp(p + sl + 64, ".iso") && strspn(p + sl, "0123456789abcdef") == 64;
    if (ok) { memcpy(sha, p + sl, 64); sha[64] = 0; }
    if (p) JS_FreeCString(c, p);
    if (!ok) return -1;
    char info[sizeof VM_IMAGE_STORE + 80], buf[512];
    snprintf(info, sizeof info, VM_IMAGE_STORE "%s.json", sha);
    int fd = open(info, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    ssize_t n = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    JDoc d;
    if (n <= 0 || json_parse(buf, (size_t)n, &d, NULL, 0)) return -1;
    const char *id = json_str(d.root, "id");
    ok = id && !strcmp(id, "winfsp");
    json_free(&d);
    if (!ok) return -1;
    snprintf(info, sizeof info, VM_IMAGE_STORE "%s.iso", sha);
    fd = open(info, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY);
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022)) { if (fd >= 0) close(fd); return -1; }
    return fd;
}

static JSValue f_agent(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc >= 2 ? get(c, argv[0]) : NULL;
    const char *w = argc >= 2 ? JS_ToCString(c, argv[1]) : NULL;
    int upd = w && !strcmp(w, "update");
    int a = !w ? 0 : !strcmp(w, "install") || upd ? VAG_INSTALL : !strcmp(w, "timesync") ? VAG_TIMESYNC : !strcmp(w, "shutdown") ? VAG_SHUTDOWN :
            !strcmp(w, "transfer") ? VAG_TRANSFER : !strcmp(w, "extraOff") ? VAG_EXTRA_OFF : !strcmp(w, "extraOn") ? VAG_EXTRA_ON : 0;
    if (w) JS_FreeCString(c, w);
    if (!s || !a || s->vm < 0) return JS_FALSE;
    if (a == VAG_TRANSFER) {
        /* the transfer folder (6.7.1): looked at again in the guest, or (a path) WinFsp installed in a Windows guest */
        char sha[65] = "";
        int fd = -1;
        if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2]) && (fd = winfsp_file(c, argv[2], sha)) < 0) return JS_FALSE;
        VmMsg m = vm_msg(VM_AGENT, ++s->seq, VAG_TRANSFER, 0, 0, 0, sha);
        int bad = vm_send(s->vm, &m, fd);
        if (fd >= 0) close(fd);
        if (bad) return JS_FALSE;
        s->xf_seq = m.seq; s->xf_result = 1; s->xf_msg[0] = 0;
        return JS_TRUE;
    }
    /* one install at a time: a click while one is on its way (lord-vm answers when the guest's script ended) is the
     * same request, not another run of the script */
    if (a == VAG_INSTALL && s->ag_seq) return JS_TRUE;
    int q = -1;
    if (upd) { VmMsg m = vm_msg(VM_AGENT, ++s->seq, VAG_INSTALL, 1, 0, 0, NULL); q = vm_send(s->vm, &m, -1) ? -1 : (int)s->seq; }
    else q = ask(s, VM_AGENT, a);
    if (q < 0) return JS_FALSE;
    if (a == VAG_INSTALL) { s->ag_seq = (uint32_t)q; s->ag_result = 1; s->ag_msg[0] = 0; }
    return JS_TRUE;
}

/* ---- apps mode (docs/VIRTUALIZATION.md 7.2) ----
 * apps(session, on) → bool: asks the guest's helper to hide its desktop and list its windows (lord-vm refuses unless the
 * helper said "windows"). windows(session) → [{ id, x, y, w, h, focused, minimized, topmost, owner, pid, title, app }]
 * top to bottom, in the guest's pixels (poll's windowsSeq counts changes). winIcon(session, id) → { width, height, data }
 * (RGBA, at most 64 × 64) or null (iconsSeq). winAct(session, id, 'focus'|'close'|'minimize'|'restore'|'move'[, x, y, w,
 * h]) → bool. guestApps(session) → [{ name, token }] (guestAppsSeq); launch(session, token) → bool: lord-vm sends only a
 * token the guest listed. closeRefusals(session) → [{ gid, reason: 'elevated'|'other', at }] (closeRefusedSeq): the guest
 * couldn't close that window (it runs as administrator in Windows, or the system refused), at (ms since 1970) when it
 * said so; the last per window. Everything the guest said is shown as text only. */
static JSValue f_apps(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc >= 2 ? get(c, argv[0]) : NULL;
    if (!s || s->vm < 0) return JS_FALSE;
    int on = JS_ToBool(c, argv[1]) > 0;
    s->apps_mode = on;
    return JS_NewBool(c, ask(s, VM_APPS, on) >= 0);
}

static JSValue f_close_refusals(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc >= 1 ? get(c, argv[0]) : NULL;
    JSValue a = JS_NewArray(c);
    for (int i = 0; s && i < s->refused_n; i++) {
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "gid", JS_NewFloat64(c, s->refused[i].gid));
        set_str(c, o, "reason", s->refused[i].reason == LAGR_ELEVATED ? "elevated" : "other");
        JS_SetPropertyStr(c, o, "at", JS_NewFloat64(c, s->refused[i].at));
        JS_SetPropertyUint32(c, a, (uint32_t)i, o);
    }
    return a;
}

static JSValue f_windows(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    JSValue a = JS_NewArray(c);
    if (!s || !s->wins) return a;
    for (int i = 0; i < s->wins->n; i++) {
        const LagWinEntry *w = &s->wins->w[i];
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "id", JS_NewFloat64(c, w->id));
        set_int(c, o, "x", w->x); set_int(c, o, "y", w->y); set_int(c, o, "w", w->w); set_int(c, o, "h", w->h);
        JS_SetPropertyStr(c, o, "focused", JS_NewBool(c, w->flags & LAGW_FOCUSED));
        JS_SetPropertyStr(c, o, "minimized", JS_NewBool(c, w->flags & LAGW_MINIMIZED));
        JS_SetPropertyStr(c, o, "topmost", JS_NewBool(c, w->flags & LAGW_TOPMOST));
        JS_SetPropertyStr(c, o, "elevated", JS_NewBool(c, w->flags & LAGW_ELEVATED));   /* runs as administrator in the guest */
        JS_SetPropertyStr(c, o, "owner", JS_NewFloat64(c, w->owner));
        JS_SetPropertyStr(c, o, "pid", JS_NewFloat64(c, w->pid));
        set_str(c, o, "title", w->title);
        set_str(c, o, "app", w->app);
        JS_SetPropertyUint32(c, a, (uint32_t)i, o);
    }
    return a;
}

static JSValue f_win_icon(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc >= 2 ? get(c, argv[0]) : NULL;
    double idd = 0;
    if (!s || JS_ToFloat64(c, &idd, argv[1]) || idd < 1 || idd > 4294967295.0) return JS_NULL;
    for (int i = 0; i < LAG_WIN_MAX; i++) {
        if (!s->icons[i].rgba || s->icons[i].id != (uint32_t)idd) continue;
        JSValue o = JS_NewObject(c);
        set_int(c, o, "width", s->icons[i].w);
        set_int(c, o, "height", s->icons[i].h);
        JS_SetPropertyStr(c, o, "data", JS_NewArrayBufferCopy(c, s->icons[i].rgba, (size_t)s->icons[i].w * s->icons[i].h * 4));
        return o;
    }
    return JS_NULL;
}

static JSValue f_win_act(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc >= 3 ? get(c, argv[0]) : NULL;
    double idd = 0;
    if (!s || s->vm < 0 || JS_ToFloat64(c, &idd, argv[1]) || idd < 1 || idd > 4294967295.0) return JS_FALSE;
    const char *w = JS_ToCString(c, argv[2]);
    int a = !w ? 0 : !strcmp(w, "focus") ? LAGA_FOCUS : !strcmp(w, "close") ? LAGA_CLOSE : !strcmp(w, "minimize") ? LAGA_MINIMIZE
          : !strcmp(w, "restore") ? LAGA_RESTORE : !strcmp(w, "move") ? LAGA_MOVE : 0;
    if (w) JS_FreeCString(c, w);
    if (!a) return JS_FALSE;
    char text[64] = "";
    if (a == LAGA_MOVE) {
        if (argc < 7) return JS_FALSE;
        int x = ival(c, argv[3]), y = ival(c, argv[4]), ww = ival(c, argv[5]), hh = ival(c, argv[6]);
        if (!lag_rect_ok(x, y, ww, hh)) return JS_FALSE;
        snprintf(text, sizeof text, "%d %d %d %d", x, y, ww, hh);
    }
    VmMsg m = vm_msg(VM_WINACT, ++s->seq, (int32_t)(uint32_t)idd, a, 0, 0, text);
    return JS_NewBool(c, !vm_send(s->vm, &m, -1));
}

static JSValue f_guest_apps(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    JSValue a = JS_NewArray(c);
    if (!s || !s->gapps) return a;
    for (int i = 0; i < s->gapps->n; i++) {
        JSValue o = JS_NewObject(c);
        set_str(c, o, "name", s->gapps->a[i].name);
        set_str(c, o, "token", s->gapps->a[i].token);
        JS_SetPropertyUint32(c, a, (uint32_t)i, o);
    }
    return a;
}

static JSValue f_launch(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc >= 2 ? get(c, argv[0]) : NULL;
    if (!s || s->vm < 0 || !s->gapps) return JS_FALSE;
    const char *tok = JS_ToCString(c, argv[1]);
    /* the app's place in the list lord-vm passed on, with that list's generation: lord-vm sends the guest its own token */
    int at = -1;
    for (int i = 0; tok && i < s->gapps->n && at < 0; i++) if (!strcmp(s->gapps->a[i].token, tok)) at = i;
    if (tok) JS_FreeCString(c, tok);
    if (at < 0 || s->gapps_gen <= 0) return JS_FALSE;
    VmMsg m = vm_msg(VM_LAUNCH, ++s->seq, at, s->gapps_gen, 0, 0, NULL);
    return JS_NewBool(c, !vm_send(s->vm, &m, -1));
}

/* clipboardSend(session, text) → bool: the host's clipboard text for the guest, in a sealed memfd (lord-vm passes it
 * on only in the directions machine.json allows). clipboardTake(session) → the guest's last text (poll's clipIn counts) */
static JSValue f_clip_send(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc >= 2 ? get(c, argv[0]) : NULL;
    size_t n = 0;
    const char *txt = s ? JS_ToCStringLen(c, &n, argv[1]) : NULL;
    int ok = 0;
    if (txt && n > 0 && n <= VM_CLIP_MAX && s->vm >= 0) {
        int fd = memfd_create("lordos-vm-clipboard", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd >= 0 && write(fd, txt, n) == (ssize_t)n && !fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)) {
            VmMsg m = vm_msg(VM_CLIP, ++s->seq, (int)n, 0, 0, 0, NULL);
            ok = !vm_send(s->vm, &m, fd);
        }
        if (fd >= 0) close(fd);
    }
    if (txt) JS_FreeCString(c, txt);
    return JS_NewBool(c, ok);
}
/* an answer bound to what was shown: clipboardApply/clipboardTake(session, seq) refuse when the guest's clipboard has
 * changed since the copy numbered seq (ask mode: the person said yes to that preview, not to whatever came after) */
static int clip_seq_ok(JSContext *c, Sess *s, int argc, JSValueConst *argv) {
    if (argc < 2 || JS_IsUndefined(argv[1])) return 1;
    int32_t seq = 0;
    return !JS_ToInt32(c, &seq, argv[1]) && seq == s->clip_seq;
}

static JSValue f_clip_take(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = get(c, argv[0]);
    return s && s->clip_in && clip_seq_ok(c, s, argc, argv) ? JS_NewString(c, s->clip_in) : JS_NewString(c, "");
}

static uint64_t clip_hash(const void *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t *)p)[i]; h *= 1099511628211ULL; }
    return h ^ n;
}

/* the text of some HTML, for a guest that offered rich text without its plain text: tags out, a few entities back */
static char *html_text(const char *h, size_t n) {
    char *o = malloc(n + 1);
    size_t k = 0;
    int in_tag = 0;
    for (size_t i = 0; o && i < n; i++) {
        char ch = h[i];
        if (in_tag) { if (ch == '>') in_tag = 0; continue; }
        if (ch == '<') { in_tag = 1; if (i + 3 < n && (!strncmp(h + i, "<br", 3) || !strncmp(h + i, "<p", 2) || !strncmp(h + i, "</p", 3) || !strncmp(h + i, "</li", 4)) && k && o[k - 1] != '\n') o[k++] = '\n'; continue; }
        if (ch == '&') {
            static const struct { const char *e; char c; } E[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&#39;", '\'' }, { "&nbsp;", ' ' } };
            int done = 0;
            for (size_t e = 0; e < sizeof E / sizeof E[0] && !done; e++) { size_t l = strlen(E[e].e); if (i + l <= n && !strncmp(h + i, E[e].e, l)) { o[k++] = E[e].c; i += l - 1; done = 1; } }
            if (done) continue;
        }
        o[k++] = ch;
    }
    if (o) o[k] = 0;
    return o;
}

/* clipboardApply(session) → "image" | "html" | "text" | "": the guest's last clipboard on this computer's. A picture is
 * re-encoded in lord-media's sandbox first (decoded and written again as a PNG: nothing the guest crafted reaches an
 * app) and arrives a moment later; rich text goes with its text (wl_clip_set_html); plain text is left to the app
 * (clipboard.set, which keeps the history's ways). */
static JSValue f_clip_apply(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = get(c, argv[0]);
    if (!s || !clip_seq_ok(c, s, argc, argv)) return JS_NewString(c, "");
    if (s->clip_png) {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        char dir[PATH_MAX], dst[PATH_MAX + 64], tmp[PATH_MAX + 64];
        static unsigned seq;
        int in = memfd_create("lordos-vm-clipboard", MFD_CLOEXEC | MFD_ALLOW_SEALING), o = -1;
        if (rt && in >= 0 && write(in, s->clip_png, s->clip_png_n) == (ssize_t)s->clip_png_n && !fcntl(in, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)) {
            snprintf(dir, sizeof dir, "%s/lordos", rt);
            mkdir(dir, 0700);
            snprintf(dst, sizeof dst, "%s/clip-vm%d-%u.png", dir, (int)getpid(), ++seq);
            snprintf(tmp, sizeof tmp, "%s/.vm%d-%u.part", dir, (int)getpid(), seq);
            o = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        }
        int fds[2] = { in, o }, id = 0;
        if (in >= 0 && o >= 0) { lseek(in, 0, SEEK_SET); id = media_job(0, MW_EDIT, JP_CLIP, fds, 2, 0, 0, "format=png\n", tmp, dst, 0); }
        else { if (in >= 0) close(in); if (o >= 0) close(o); }
        s->clip_last = clip_hash(s->clip_png, s->clip_png_n);
        s->clip_skip_img = wl_clip_image_gen() + 1;
        free(s->clip_png); s->clip_png = NULL; s->clip_png_n = 0;
        return JS_NewString(c, id > 0 ? "image" : "");
    }
    if (s->clip_html) {
        char *txt = s->clip_in ? strdup(s->clip_in) : html_text(s->clip_html, s->clip_html_n);
        if (!txt) return JS_NewString(c, "");
        term_set_clipboard(txt, strlen(txt));
        term_clipboard_mark("machines", 0);
        wl_clip_set_html(s->clip_html, s->clip_html_n);
        s->clip_last = clip_hash(s->clip_html, s->clip_html_n);
        free(txt);
        free(s->clip_html); s->clip_html = NULL; s->clip_html_n = 0;
        return JS_NewString(c, "html");
    }
    if (s->clip_in) { s->clip_last = clip_hash(s->clip_in, strlen(s->clip_in)); return JS_NewString(c, "text"); }
    return JS_NewString(c, "");
}

/* clipboardSendHost(session) → bool: this computer's clipboard for the guest, as it is now (host_clip: never a secret).
 * Not again if it's what went either way last. A guest helper that takes it on demand (VAC_CLIP_LAZY) gets only an
 * offer, which forms there are, and asks for one when a program there pastes (VM_CLIP_REQ, answered while the machine
 * is in use); an older one gets it all at once. lord-vm checks it all again and sends it on only the ways machine.json
 * allows. The app calls it only when the person turns to the machine (its window to the front, a click on it). */
static JSValue f_clip_send_host(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = get(c, argv[0]);
    if (!s || s->vm < 0) return JS_FALSE;
    AgClip cx;
    uint64_t h;
    if (host_clip(&cx, &h)) return JS_FALSE;
    if (cx.png && wl_clip_image_gen() == s->clip_skip_img) { s->clip_skip_img = 0; s->clip_last = h; return JS_FALSE; }
    if (h == s->clip_last) return JS_FALSE;
    /* ask mode is enforced here too: a full push (a helper that can't ask for the data) needs the hash the person
     * approved (clipboardSendHost(session, approvedHash)), and it must still be this computer's clipboard. A guest that
     * flips its on-demand capability can't turn an offer into a push the person never saw */
    if (s->clip_ask && !(s->ag_caps & VAC_CLIP_LAZY)) {
        char hx[24];
        snprintf(hx, sizeof hx, "%016llx", (unsigned long long)h);
        const char *ap = argc >= 2 && JS_IsString(argv[1]) ? JS_ToCString(c, argv[1]) : NULL;
        int approved = ap && !strcmp(ap, hx);
        if (ap) JS_FreeCString(c, ap);
        if (!approved) return JS_FALSE;
    }
    if (s->ag_caps & VAC_CLIP_LAZY) {
        /* a new generation per offer: a request for an older one is refused as stale */
        s->offer_gen = s->offer_gen >= 0x7ffffff0u ? 1 : s->offer_gen + 1;
        s->offer_forms = clip_forms(&cx);
        s->offer_hash = h;
        for (int f = AGF_TEXT; f <= AGF_PNG; f++) if (s->ask_gen[f]) { clip_deny(s, s->ask_gen[f], f, VCD_STALE); s->ask_gen[f] = 0; }
        VmMsg m = vm_msg(VM_CLIP_OFFER, ++s->seq, (int)s->offer_gen, s->offer_forms, s->clip_ask ? AGO_ASK : 0, 0, NULL);
        int ok = !vm_send(s->vm, &m, -1);
        if (ok) s->clip_last = h;
        return JS_NewBool(c, ok);
    }
    size_t n = ag_clipx_size(&cx);
    uint8_t *buf = malloc(n);
    int ok = 0;
    if (buf) {
        ag_clipx_write(&cx, buf);
        int fd = memfd_create("lordos-vm-clipboard", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd >= 0 && write(fd, buf, n) == (ssize_t)n && !fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)) {
            VmMsg m = vm_msg(VM_CLIP, ++s->seq, (int)n, VCF_CLIPX, 0, 0, NULL);
            ok = !vm_send(s->vm, &m, fd);
        }
        if (fd >= 0) close(fd);
        free(buf);
    }
    if (ok) s->clip_last = h;
    return JS_NewBool(c, ok);
}

/* clipboardGate(session, inUse, ask): the app says whether the person is using the machine now (its window, or one of
 * its apps mode windows, in front; a click on it). Going out of use keeps it open CLIP_GRACE_MS more, so a paste just
 * after switching works. ask: "ask before each paste" (machine.json clipboard_ask): requests wait for clipboardAnswer */
static JSValue f_clip_gate(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc >= 2 ? get(c, argv[0]) : NULL;
    if (!s) return JS_FALSE;
    int on = JS_ToBool(c, argv[1]);
    if (s->gate_on && !on) s->gate_until = mono_ms() + CLIP_GRACE_MS;
    if (on) s->gate_until = 0;
    s->gate_on = on;
    if (argc >= 3) s->clip_ask = JS_ToBool(c, argv[2]);
    return JS_TRUE;
}

/* clipboardAnswer(session, gen, yes) → bool: the person's answer (ask mode) to the requests waiting for that offer:
 * every form asked for it at once (a program that asks for several forms of one copy is asked about once) */
static JSValue f_clip_answer(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc >= 3 ? get(c, argv[0]) : NULL;
    uint32_t gen = 0;
    if (!s || JS_ToUint32(c, &gen, argv[1]) || !gen) return JS_FALSE;
    int yes = JS_ToBool(c, argv[2]), any = 0;
    for (int f = AGF_TEXT; f <= AGF_PNG; f++) {
        if (s->ask_gen[f] != gen) continue;
        s->ask_gen[f] = 0;
        any = 1;
        if (yes) clip_give(s, gen, f); else clip_deny(s, gen, f, VCD_DECLINED);
    }
    return JS_NewBool(c, any);
}

/* a line of text to show the person: the first max characters, control characters and line ends as spaces */
static JSValue preview_text(JSContext *c, const char *p, size_t n, size_t max) {
    char out[400];
    size_t k = 0, chars = 0;
    for (size_t i = 0; i < n && k + 8 < sizeof out && chars < max; ) {
        unsigned char ch = (unsigned char)p[i];
        size_t l = ch < 0x80 ? 1 : (ch >> 5) == 6 ? 2 : (ch >> 4) == 14 ? 3 : 4;
        if (i + l > n) break;
        if (ch < 0x20 || ch == 0x7f) { if (k && out[k - 1] != ' ') out[k++] = ' '; i++; continue; }
        memcpy(out + k, p + i, l);
        k += l; i += l; chars++;
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
    JSValue v = JS_NewString(c, out);
    return v;
}

/* clipboardPreview(session, 'host' | 'guest') → { kind: 'text' | 'html' | 'image', text, w, h, fresh } or null: what a
 * paste into the machine would give it (this computer's clipboard; never a secret), or what the guest copied last
 * (waiting for clipboardApply), for the ask mode's bar. fresh: not what went either way last */
static JSValue f_clip_preview(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = argc >= 1 ? get(c, argv[0]) : NULL;
    if (!s) return JS_NULL;
    const char *which = argc >= 2 ? JS_ToCString(c, argv[1]) : NULL;
    int guest = which && !strcmp(which, "guest");
    if (which) JS_FreeCString(c, which);
    AgClip cx;
    uint64_t h = 0;
    char *tmp = NULL;
    memset(&cx, 0, sizeof cx);
    if (guest) {
        if (s->clip_png) { cx.png = s->clip_png; cx.png_n = (uint32_t)s->clip_png_n; }
        else if (s->clip_html) { cx.html = (const uint8_t *)s->clip_html; cx.html_n = (uint32_t)s->clip_html_n; }
        if (s->clip_in) { cx.text = (const uint8_t *)s->clip_in; cx.text_n = (uint32_t)strlen(s->clip_in); }
        if (!cx.png && !cx.html && !cx.text) return JS_NULL;
    } else if (host_clip(&cx, &h)) return JS_NULL;
    JSValue o = JS_NewObject(c);
    uint32_t w = 0, hh = 0;
    if (cx.png && !ag_png_check(cx.png, cx.png_n, &w, &hh)) {
        set_str(c, o, "kind", "image");
        set_int(c, o, "w", w);
        set_int(c, o, "h", hh);
    } else {
        set_str(c, o, "kind", cx.html ? "html" : "text");
        if (!cx.text && cx.html) { tmp = html_text((const char *)cx.html, cx.html_n); if (tmp) { cx.text = (const uint8_t *)tmp; cx.text_n = (uint32_t)strlen(tmp); } }
        JS_SetPropertyStr(c, o, "text", cx.text ? preview_text(c, (const char *)cx.text, cx.text_n, 80) : JS_NewString(c, ""));
        free(tmp);
    }
    JS_SetPropertyStr(c, o, "fresh", JS_NewBool(c, guest || h != s->clip_last));
    char hx[24];
    snprintf(hx, sizeof hx, "%016llx", (unsigned long long)h);
    set_str(c, o, "hash", hx);
    return o;
}

/* console(session) → bool: asks lord-vm for the serial console's stream; poll's console is then the terminal session to
 * show in a <Terminal> (lord.term.key writes to it). Asking again replaces it. */
static JSValue f_console(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    if (!s || s->vm < 0) return JS_FALSE;
    int q = ask(s, VM_CONSOLE, 0);
    if (q < 0) return JS_FALSE;
    s->console_seq = (uint32_t)q;
    return JS_TRUE;
}

/* shell(session, cols, rows) → bool: asks lord-vm for the agent's shell (6.18: the guest's tools start the signed-in
 * person's own shell over the agent); poll's shell is then the terminal session to show in a <Terminal>. Its size
 * follows the terminal by itself. shell(session, 0) ends it. Asking again replaces it */
static JSValue f_shell(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = get(c, argv[0]);
    int32_t cols = 0, rows = 0;
    if (argc > 1) JS_ToInt32(c, &cols, argv[1]);
    if (argc > 2) JS_ToInt32(c, &rows, argv[2]);
    if (!s || s->vm < 0) return JS_FALSE;
    if (!cols) {
        if (s->shell_term) { Session *ts = term_get(s->shell_term); if (ts) term_close(ts); s->shell_term = 0; }
        VmMsg m = vm_msg(VM_SHELL, ++s->seq, VSH_CLOSE, 0, 0, 0, NULL);
        return JS_NewBool(c, !vm_send(s->vm, &m, -1));
    }
    if (cols < 1 || cols > AG_SHELL_COLS_MAX || rows < 1 || rows > AG_SHELL_ROWS_MAX) return JS_FALSE;
    VmMsg m = vm_msg(VM_SHELL, ++s->seq, VSH_OPEN, cols, rows, 0, NULL);
    if (vm_send(s->vm, &m, -1)) return JS_FALSE;
    s->shell_seq = s->seq;
    s->shell_cols = cols;
    s->shell_rows = rows;
    return JS_TRUE;
}

/* guestNotes(session) → [{ title, body }]: the guest's notifications that came since (6.12), as text only, for the app
 * to show as from the machine; taken once */
static JSValue f_guest_notes(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    JSValue a = JS_NewArray(c);
    if (!s) return a;
    for (int i = 0; i < s->note_n; i++) {
        JSValue o = JS_NewObject(c);
        set_str(c, o, "title", s->notes[i].title);
        set_str(c, o, "body", s->notes[i].body);
        JS_SetPropertyUint32(c, a, (uint32_t)i, o);
    }
    s->note_n = 0;
    return a;
}

/* guestLinkAnswer(session, seq, open) → bool: the person's answer to the link the guest asked to open (poll's guestLink).
 * Only that link (never a URL from JS), only the one still waiting with that seq, only LINK_MIN_MS after the app first
 * saw it (a click meant for something else doesn't open it), only while the machine is still in use; then in the
 * default browser, as one argument (apps_launch_uri's own checks). Either answer lets the link go */
static JSValue f_guest_link_answer(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    Sess *s = get(c, argv[0]);
    int32_t seq = 0;
    if (argc > 1) JS_ToInt32(c, &seq, argv[1]);
    int open = argc > 2 && JS_ToBool(c, argv[2]);
    if (!s || !s->link[0] || (uint32_t)seq != s->link_seq) return JS_FALSE;
    if (open && (!s->link_seen || mono_ms() - s->link_seen < LINK_MIN_MS)) return JS_FALSE;     /* too soon: the bar stays */
    char url[AG_LINK_MAX + 1];
    snprintf(url, sizeof url, "%s", s->link);
    s->link[0] = 0;
    if (!open) return JS_TRUE;
    if (!clip_in_use(s) || !ag_link_ok((const uint8_t *)url, strlen(url))) return JS_FALSE;
    char def[192];
    if (!apps_default_for("x-scheme-handler/https", def, sizeof def) && !apps_default_for("x-scheme-handler/http", def, sizeof def)) return JS_FALSE;
    size_t l = strlen(def);
    if (l > 8 && !strcmp(def + l - 8, ".desktop")) def[l - 8] = 0;
    return JS_NewBool(c, !apps_launch_uri(def, url));
}

/* the recording's sound ring (aring.h): a sealed memfd, 48 kHz, 16-bit, one channel, about 5 s */
static int sound_ring(void) {
    uint32_t size = 1u << 19;
    int fd = memfd_create("lordos-vm-sound", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)ar_bytes(size)) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) { close(fd); return -1; }
    ARingHdr *h = mmap(NULL, AR_HDR, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (h == MAP_FAILED) { close(fd); return -1; }
    memset(h, 0, AR_HDR);
    h->rate = AR_RATE;
    h->channels = 1;
    h->size = size;
    h->magic = AR_MAGIC;
    munmap(h, AR_HDR);
    return fd;
}

/* record(session) → a media id (0: couldn't): the guest's picture at its size into ~/Pictures/Screenshots as
 * "<machine> <date> <time>.mp4", the way the Screenshots app records (lord-media, hardware encoding where there is
 * some). No screen-capture permission: it's the machine's own picture, never the screen, so protected windows can't be
 * in it. Stopped with lord.media.stop(id) (its "done" event names the file), or when the window closes. */
static JSValue f_record(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    MediaSrc *m = s && s->media ? media_src_get(s->media) : NULL;
    const char *home = getenv("HOME");
    if (m && !pic_out_ok(s, m)) return JS_NewInt32(c, 0);       /* a protected machine: never recorded */
    media_src_need_cpu(m);              /* the recording reads the picture's memory from now on (gpu_show_ok) */
    if (!m || !m->px || m->w < 16 || m->h < 16 || !home || home[0] != '/' || s->rec) return JS_NewInt32(c, 0);
    char dir[PATH_MAX], name[256], path[PATH_MAX + 260];
    snprintf(dir, sizeof dir, "%s/Pictures", home); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/Pictures/Screenshots", home); mkdir(dir, 0755);
    VmMachine vm;
    const char *base = !vm_machine_load(s->dir, &vm, NULL, 0) ? vm.name : "Machine";
    char clean[VM_NAME_MAX + 1];
    size_t k = 0;
    for (const char *q = base; *q && k + 1 < sizeof clean; q++) clean[k++] = (*q == '/' || (unsigned char)*q < 0x20) ? '-' : *q;
    clean[k] = 0;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char when[40];
    strftime(when, sizeof when, "%Y-%m-%d %H.%M.%S", &tmv);
    snprintf(name, sizeof name, "%s %s.mp4", clean, when);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    MediaSrc *r = media_src_new(SK_SCREENREC, "machines");
    if (!r) return JS_NewInt32(c, 0);
    int fd = media_open_output(path, r->tmp, sizeof r->tmp, r->dst, sizeof r->dst);
    if (fd < 0) { media_src_close(r); return JS_NewInt32(c, 0); }
    int render = media_render_fd(), W = m->w & ~1, H = m->h & ~1;
    char params[160];
    snprintf(params, sizeof params, "width=%d\nheight=%d\nfps=30\nhw=%d\nhwaccel=%s\n", W, H, render >= 0, media_hwaccel());
    /* the guest's sound (6.9): a ring lord-vmdisplay writes and lord-media reads, where a screen recording takes the
     * microphone (48 kHz, one channel); muxed as the recording's audio track */
    int ring = s->vd >= 0 ? sound_ring() : -1;
    int fds[3] = { fd, ring, render };
    r->sock = media_spawn(MK_SCREENREC, params, fds, 3, &r->pid);
    close(fd);
    if (render >= 0) close(render);
    if (r->sock < 0 || media_framerec_start(r, W, H, 30)) { if (ring >= 0) close(ring); media_src_close(r); return JS_NewInt32(c, 0); }
    s->rec = r->id;
    s->rec_fed = 0;
    s->rec_sound = 0;
    if (ring >= 0) {
        VdMsg am = vd_msg(VD_AUDIO_REC, 1, 0, 0, 0, 0, 0);
        s->rec_sound = !vd_send(s->vd, &am, ring);
        close(ring);
    }
    media_framerec_feed(r, m->px, m->w, m->h);
    return JS_NewInt32(c, r->id);
}

/* takes the ISO out of a machine that is off (after an install): machine.json without "iso". Refused while lord-vm
 * holds the machine's lock. true when done */
static JSValue f_eject(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    char dir[PATH_MAX], p[PATH_MAX + 16], err[256];
    if (machine_dir(c, argv[0], dir, sizeof dir)) return JS_FALSE;
    snprintf(p, sizeof p, "%s/lock", dir);
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return JS_FALSE;
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return JS_FALSE; }        /* running */
    VmMachine m;
    int ok = !vm_machine_load(dir, &m, err, sizeof err);
    if (ok && m.iso[0]) {
        m.iso[0] = 0;
        vm_now_iso(m.updated);
        ok = !vm_machine_save(dir, &m);
    }
    flock(fd, LOCK_UN);
    close(fd);
    return JS_NewBool(c, ok);
}

/* startDetached(dir) → job id: the machine starts with no window (lord-vm --detach --keep-running); a window can
 * attach to it later, and it keeps running when that window closes */
static JSValue f_start_detached(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    char dir[PATH_MAX], creds[PATH_MAX];
    if (machine_dir(c, argv[0], dir, sizeof dir) || !machine_shares_ok(dir)) return JS_NewInt32(c, 0);
    last_start = time(NULL);
    int have = !write_creds(creds, sizeof creds, dir);
    char *args[] = { "lord-vm", "--start", dir, "--detach", "--keep-running", have ? "--credentials-file" : NULL, creds, NULL };
    int id = spawn_job(args, dir);
    if (!id) { if (have) unlink(creds); return JS_NewInt32(c, 0); }
    if (have) snprintf(jobs[id - 1].creds, sizeof jobs[id - 1].creds, "%s", creds);
    return JS_NewInt32(c, id);
}

/* discardState(dir) → job id: the saved session goes; the next start boots from the disks */
static JSValue f_discard(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    char dir[PATH_MAX];
    if (machine_dir(c, argv[0], dir, sizeof dir)) return JS_NewInt32(c, 0);
    char *args[] = { "lord-vm", "--discard-state", dir, NULL };
    return JS_NewInt32(c, spawn_job(args, dir));
}

/* awake() → how many running machines keep this computer from sleeping (5.3, "Keep this computer awake"; app.json
 * keepAwake, on unless turned off). The session's idle sleep asks before it suspends. */
static JSValue f_awake(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char lib[PATH_MAX];
    if (library(lib, sizeof lib, 0)) return JS_NewInt32(c, 0);
    DIR *d = opendir(lib);
    if (!d) return JS_NewInt32(c, 0);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 8 || strcmp(e->d_name + l - 7, ".lordvm")) continue;
        char p[PATH_MAX + 300], buf[2048];
        snprintf(p, sizeof p, "%s/%s/run.json", lib, e->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        ssize_t k = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
        if (fd >= 0) close(fd);
        if (k <= 0) continue;
        JDoc j;
        if (json_parse(buf, (size_t)k, &j, NULL, 0)) continue;
        const char *st = json_str(j.root, "state");
        long pid = (long)json_num(j.root, "pid", 0);
        int on = st && (!strcmp(st, "running") || !strcmp(st, "starting") || !strcmp(st, "stopping")) && pid > 0 && !kill((pid_t)pid, 0);
        json_free(&j);
        if (!on) continue;
        snprintf(p, sizeof p, "%s/%s/app.json", lib, e->d_name);
        fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        k = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
        if (fd >= 0) close(fd);
        int keep = 1;
        if (k > 0 && !json_parse(buf, (size_t)k, &j, NULL, 0)) {
            keep = json_bool(j.root, "keepAwake", 1);
            json_free(&j);
        }
        n += keep;
    }
    closedir(d);
    return JS_NewInt32(c, n);
}

/* ---- snapshots (6.2): lord-vm --snapshot as a job (through the running lord-vm when the machine is on) ---- */

/* snaps(dir) → { list: [{ tag, name, created, memory, vmstate, picture }], max, warn } */
static JSValue f_snaps(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    char dir[PATH_MAX];
    if (machine_dir(c, argv[0], dir, sizeof dir)) return JS_NULL;
    static VmSnap list[VM_SNAPS_MAX];
    int n = vm_snaps_load(dir, list, VM_SNAPS_MAX);
    JSValue o = JS_NewObject(c), arr = JS_NewArray(c);
    for (int i = 0; i < n; i++) {
        JSValue e = JS_NewObject(c);
        set_str(c, e, "tag", list[i].tag);
        set_str(c, e, "name", list[i].name);
        set_str(c, e, "created", list[i].created);
        JS_SetPropertyStr(c, e, "memory", JS_NewBool(c, list[i].memory));
        JS_SetPropertyStr(c, e, "vmstate", JS_NewFloat64(c, list[i].vmstate));
        char p[PATH_MAX + 64];
        struct stat st;
        snprintf(p, sizeof p, "%s/snapshots/%s.png", dir, list[i].tag);
        set_str(c, e, "picture", !stat(p, &st) ? p : "");
        JS_SetPropertyUint32(c, arr, (uint32_t)i, e);
    }
    JS_SetPropertyStr(c, o, "list", arr);
    set_int(c, o, "max", VM_SNAPS_MAX);
    set_int(c, o, "warn", VM_SNAPS_WARN);
    return o;
}

/* snapTake(dir, name[, session]) → { job, tag }: the picture first (the session's live one, or the card's), then
 * lord-vm --snapshot DIR take NAME TAG */
static JSValue f_snap_take(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char dir[PATH_MAX];
    if (argc < 2 || machine_dir(c, argv[0], dir, sizeof dir)) return JS_NULL;
    const char *name = JS_ToCString(c, argv[1]);
    if (!name) return JS_NULL;
    char nm[VM_NAME_MAX + 1];
    int ok = vm_name_valid(name);
    snprintf(nm, sizeof nm, "%s", name);
    JS_FreeCString(c, name);
    if (!ok) return JS_NULL;
    char tag[33], sd[PATH_MAX + 32], file[64];
    vm_snap_tag(tag);
    snprintf(sd, sizeof sd, "%s/snapshots", dir);
    mkdir(sd, 0700);
    snprintf(file, sizeof file, "%s.png", tag);
    Sess *s = argc >= 3 ? get(c, argv[2]) : NULL;
    MediaSrc *m = s && s->media ? media_src_get(s->media) : NULL;
    int shut = (s && s->protect) || (m && !pic_out_ok(s, m));    /* a protected machine's snapshot keeps no picture */
    if (!shut) media_src_need_cpu(m);
    if (shut) { /* nothing: the list shows the snapshot without a picture */ }
    else if (m && m->px && m->w > 0 && m->h > 0) vm_png_save(sd, file, m->px, m->w, m->h, 640);
    else {
        char from[PATH_MAX + 32], to[PATH_MAX + 96];
        snprintf(from, sizeof from, "%s/thumb.png", dir);
        snprintf(to, sizeof to, "%s/%s", sd, file);
        int a = open(from, O_RDONLY | O_CLOEXEC | O_NOFOLLOW), b = a >= 0 ? open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600) : -1;
        char buf[65536];
        ssize_t r;
        while (b >= 0 && (r = read(a, buf, sizeof buf)) > 0) if (write(b, buf, (size_t)r) != r) break;
        if (a >= 0) close(a);
        if (b >= 0) close(b);
    }
    char *args[] = { "lord-vm", "--snapshot", dir, "take", nm, tag, NULL };
    int id = spawn_job(args, dir);
    if (!id) return JS_NULL;
    JSValue o = JS_NewObject(c);
    set_int(c, o, "job", id);
    set_str(c, o, "tag", tag);
    return o;
}

/* snapRevert(dir, tag) / snapDelete(dir, tag) → job id. A revert of a snapshot with memory on a machine that's off
 * ends with exit 4: the app starts the machine with { loadvm: tag } instead. */
static JSValue snap_job(JSContext *c, int argc, JSValueConst *argv, const char *what) {
    char dir[PATH_MAX];
    if (argc < 2 || machine_dir(c, argv[0], dir, sizeof dir)) return JS_NewInt32(c, 0);
    const char *tag = JS_ToCString(c, argv[1]);
    char tg[40] = "";
    if (tag && vm_snap_tag_valid(tag)) snprintf(tg, sizeof tg, "%s", tag);
    if (tag) JS_FreeCString(c, tag);
    if (!tg[0]) return JS_NewInt32(c, 0);
    char *args[] = { "lord-vm", "--snapshot", dir, (char *)what, tg, NULL };
    return JS_NewInt32(c, spawn_job(args, dir));
}
static JSValue f_snap_revert(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return snap_job(c, argc, argv, "revert"); }
static JSValue f_snap_delete(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return snap_job(c, argc, argv, "delete"); }

/* ---- clones, compaction, storage (6.3, 6.4, 6.16) ---- */

/* the file system's name, and whether it can share file data (reflink) */
static const char *fs_name(long t) {
    switch ((unsigned long)t) {
    case 0xEF53: return "ext4";
    case 0x9123683E: return "btrfs";
    case 0x58465342: return "xfs";
    case 0x01021994: return "tmpfs";
    case 0x2FC12FC1: return "zfs";
    case 0xF2F52010: return "f2fs";
    default: return "other";
    }
}
/* can this folder's file system make reflink copies? (a real FICLONE of a small file, removed after) */
static int reflink_ok(const char *dir) {
    char a[PATH_MAX + 64], b[PATH_MAX + 64];
    snprintf(a, sizeof a, "%s/.reflink-test-%d-a", dir, (int)getpid());
    snprintf(b, sizeof b, "%s/.reflink-test-%d-b", dir, (int)getpid());
    int fa = open(a, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600), ok = 0;
    if (fa >= 0) {
        char buf[4096];
        memset(buf, 7, sizeof buf);
        if (write(fa, buf, sizeof buf) == (ssize_t)sizeof buf) {
            int fb = open(b, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fb >= 0) { ok = !ioctl(fb, FICLONE, fa); close(fb); }
        }
        close(fa);
    }
    unlink(a); unlink(b);
    return ok;
}

/* cloneInfo(dir) → { fs, reflink, snapshots, suspended, linked, running }: what the Clone sheet says it will do */
static JSValue f_clone_info(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    char dir[PATH_MAX], lib[PATH_MAX];
    VmMachine m;
    if (machine_dir(c, argv[0], dir, sizeof dir) || library(lib, sizeof lib, 0) || vm_machine_load(dir, &m, NULL, 0)) return JS_NULL;
    struct statfs sf;
    JSValue o = JS_NewObject(c);
    set_str(c, o, "fs", !statfs(lib, &sf) ? fs_name((long)sf.f_type) : "other");
    JS_SetPropertyStr(c, o, "reflink", JS_NewBool(c, reflink_ok(lib)));
    static VmSnap sn[VM_SNAPS_MAX];
    set_int(c, o, "snapshots", vm_snaps_load(dir, sn, VM_SNAPS_MAX));
    char p[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/suspend.state", dir);
    JS_SetPropertyStr(c, o, "suspended", JS_NewBool(c, !access(p, F_OK)));
    JS_SetPropertyStr(c, o, "linked", JS_NewBool(c, m.base[0] != 0));
    return o;
}

/* clone(dir, { name, duplicate, linked, fromSnapshot, hostname, keepIdentity }) → job id (lord-vm --clone; its out is the new
 * machine's JSON). A Clone's guest gets `hostname` (else "<name>-clone"), a new machine-id and SSH host keys at its first
 * start, through the agent, unless keepIdentity */
static JSValue f_clone(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char dir[PATH_MAX], lib[PATH_MAX];
    if (argc < 2 || !JS_IsObject(argv[1]) || machine_dir(c, argv[0], dir, sizeof dir) || library(lib, sizeof lib, 0)) return JS_NewInt32(c, 0);
    JSValue vn = JS_GetPropertyStr(c, argv[1], "name"), vs = JS_GetPropertyStr(c, argv[1], "fromSnapshot");
    const char *name = JS_ToCString(c, vn), *snap = JS_IsString(vs) ? JS_ToCString(c, vs) : NULL;
    int dup = opt_bool(c, argc, argv, "duplicate"), linked = opt_bool(c, argc, argv, "linked"), keep = opt_bool(c, argc, argv, "keepIdentity");
    JSValue vh = JS_GetPropertyStr(c, argv[1], "hostname");
    const char *hn = JS_IsString(vh) ? JS_ToCString(c, vh) : NULL;
    char host[VM_HOSTNAME_MAX + 1] = "";
    int host_ok = !hn || !hn[0] || vm_hostname_valid(hn);
    if (hn && hn[0] && host_ok) snprintf(host, sizeof host, "%s", hn);
    if (hn) JS_FreeCString(c, hn);
    JS_FreeValue(c, vh);
    int id = 0;
    char nm[VM_NAME_MAX + 1] = "", tg[40] = "", folder[64], dst[PATH_MAX + 80];
    if (name && vm_name_valid(name) && (!snap || vm_snap_tag_valid(snap)) && host_ok) {
        snprintf(nm, sizeof nm, "%s", name);
        if (snap) snprintf(tg, sizeof tg, "%s", snap);
        folder_name(nm, folder, sizeof folder);
        snprintf(dst, sizeof dst, "%s/%s.lordvm", lib, folder);
        for (int k = 2; !access(dst, F_OK) && k < 100; k++) snprintf(dst, sizeof dst, "%s/%s %d.lordvm", lib, folder, k);
        char *args[14];
        int k = 0;
        args[k++] = "lord-vm"; args[k++] = "--clone"; args[k++] = dir; args[k++] = dst; args[k++] = "--name"; args[k++] = nm;
        if (dup) args[k++] = "--duplicate";
        if (linked) args[k++] = "--linked";
        if (!dup && keep) args[k++] = "--keep-identity";
        else if (!dup && host[0]) { args[k++] = "--hostname"; args[k++] = host; }
        if (tg[0]) { args[k++] = "--from-snapshot"; args[k++] = tg; }
        args[k] = NULL;
        if (access(dst, F_OK)) id = spawn_job(args, dst);
    }
    if (name) JS_FreeCString(c, name);
    if (snap) JS_FreeCString(c, snap);
    JS_FreeValue(c, vn); JS_FreeValue(c, vs);
    return JS_NewInt32(c, id);
}

/* compact(dir, estimate) → job id: lord-vm --compact [--estimate] (the estimate's out is JSON) */
static JSValue f_compact(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char dir[PATH_MAX];
    if (machine_dir(c, argv[0], dir, sizeof dir)) return JS_NewInt32(c, 0);
    int est = argc > 1 && JS_ToBool(c, argv[1]) > 0;
    char *args[] = { "lord-vm", "--compact", dir, est ? "--estimate" : NULL, NULL };
    return JS_NewInt32(c, spawn_job(args, dir));
}

/* what a file takes on disk, and how much of it is shared with another file (reflink: FIEMAP_EXTENT_SHARED) */
static double blocks_of(const char *p) { struct stat st; return !lstat(p, &st) && S_ISREG(st.st_mode) ? (double)st.st_blocks * 512 : 0; }
static double shared_of(const char *p) {
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    union { struct fiemap f; char b[sizeof(struct fiemap) + 256 * sizeof(struct fiemap_extent)]; } u;
    double shared = 0;
    unsigned long long start = 0;
    for (int rounds = 0; rounds < 4096; rounds++) {
        memset(&u, 0, sizeof u);
        u.f.fm_start = start;
        u.f.fm_length = ~0ULL;
        u.f.fm_extent_count = 256;
        if (ioctl(fd, FS_IOC_FIEMAP, &u.f) || !u.f.fm_mapped_extents) break;
        int last = 0;
        for (unsigned i = 0; i < u.f.fm_mapped_extents; i++) {
            struct fiemap_extent *e = &u.f.fm_extents[i];
            if (e->fe_flags & FIEMAP_EXTENT_SHARED) shared += (double)e->fe_length;
            if (e->fe_flags & FIEMAP_EXTENT_LAST) last = 1;
            start = e->fe_logical + e->fe_length;
        }
        if (last) break;
    }
    close(fd);
    return shared;
}
static double tree_bytes(const char *dir, int depth) {
    double n = 0;
    DIR *d = depth < 4 ? opendir(dir) : NULL;
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_MAX + 260];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) n += tree_bytes(p, depth + 1); else if (S_ISREG(st.st_mode)) n += (double)st.st_blocks * 512;
    }
    closedir(d);
    return n;
}

/* the bases machines name, in the library and in the Trash (a machine there still holds its base) */
typedef struct { char path[VM_PATH_MAX]; int users, trash; } BaseUse;
static int base_note(BaseUse *b, int n, int max, const char *path, int trash) {
    for (int i = 0; i < n; i++) if (!strcmp(b[i].path, path)) { if (trash) b[i].trash++; else b[i].users++; return n; }
    if (n >= max) return n;
    snprintf(b[n].path, sizeof b[n].path, "%s", path);
    b[n].users = !trash; b[n].trash = !!trash;
    return n + 1;
}
static int trash_dir(char *out, size_t n) {
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') return -1;
    snprintf(out, n, "%s/.local/share/Trash/files", home);
    return 0;
}

/* storage() → the Storage window's figures (6.16): { total, free, machines: [{ dir, name, id, base, disks, snapshots,
 * state, other, shared, total, frees }], bases: [{ path, bytes, users, trash }], trash, isos } in bytes. "frees" is what
 * deleting the machine would really give back: its own files less what it shares by reflink, plus its base when it's
 * the last machine on it (the Trash counted). */
static JSValue f_storage(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char lib[PATH_MAX], tr[PATH_MAX];
    JSValue o = JS_NewObject(c), arr = JS_NewArray(c), barr = JS_NewArray(c);
    if (library(lib, sizeof lib, 0)) { JS_SetPropertyStr(c, o, "machines", arr); JS_SetPropertyStr(c, o, "bases", barr); return o; }
    struct statvfs vf;
    if (!statvfs(lib, &vf)) {
        JS_SetPropertyStr(c, o, "total", JS_NewFloat64(c, (double)vf.f_blocks * vf.f_frsize));
        JS_SetPropertyStr(c, o, "free", JS_NewFloat64(c, (double)vf.f_bavail * vf.f_frsize));
    }
    static BaseUse bases[64];
    int nb = 0;
    double trash = 0, isos = 0;
    static char seen_iso[16][VM_PATH_MAX];
    int nis = 0;
    if (!trash_dir(tr, sizeof tr)) {
        DIR *d = opendir(tr);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l < 8 || strcmp(e->d_name + l - 7, ".lordvm")) continue;
            char p[PATH_MAX + 260];
            snprintf(p, sizeof p, "%s/%s", tr, e->d_name);
            trash += tree_bytes(p, 0);
            VmMachine m;
            if (!vm_machine_load(p, &m, NULL, 0) && m.base[0]) nb = base_note(bases, nb, 64, m.base, 1);
        }
        if (d) closedir(d);
    }
    /* the machines: a first pass for the bases' users */
    DIR *d = opendir(lib);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        /* a machine group (19.6): its members' bases, and the ones its group.json names, are in use (never offered
         * for removal: Start Fresh makes the members again from them) */
        if (e->d_name[0] != '.' && l > 10 && !strcmp(e->d_name + l - 10, VMG_SUFFIX)) {
            char gd[PATH_MAX + 260];
            snprintf(gd, sizeof gd, "%s/%s", lib, e->d_name);
            VmGroup g;
            if (vm_group_load(gd, &g, NULL, 0)) continue;
            for (int r = 1; r < VM_ROLE_COUNT; r++) if (g.base[r][0]) nb = base_note(bases, nb, 64, g.base[r], 0);
            for (int i = 0; i < g.n; i++) {
                char mp[PATH_MAX + 520];
                snprintf(mp, sizeof mp, "%s/%s", gd, g.m[i].dir);
                VmMachine m;
                if (!vm_machine_load(mp, &m, NULL, 0) && m.base[0] && (!g.base[m.role][0] || strcmp(m.base, g.base[m.role]))) nb = base_note(bases, nb, 64, m.base, 0);
            }
            continue;
        }
        if (e->d_name[0] == '.' || l < 8 || strcmp(e->d_name + l - 7, ".lordvm")) continue;
        char p[PATH_MAX + 260];
        snprintf(p, sizeof p, "%s/%s", lib, e->d_name);
        VmMachine m;
        if (!vm_machine_load(p, &m, NULL, 0) && m.base[0]) nb = base_note(bases, nb, 64, m.base, 0);
    }
    if (d) rewinddir(d);
    uint32_t k = 0;
    while (d && (e = readdir(d)) && k < 256) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 8 || strcmp(e->d_name + l - 7, ".lordvm")) continue;
        char p[PATH_MAX + 260], f[PATH_MAX + 300];
        snprintf(p, sizeof p, "%s/%s", lib, e->d_name);
        VmMachine m;
        if (vm_machine_load(p, &m, NULL, 0)) continue;
        double disks = 0, shared = 0;
        const char *files[] = { "system.qcow2", "nvram.qcow2" };
        for (int i = 0; i < 2; i++) { snprintf(f, sizeof f, "%s/disks/%s", p, files[i]); disks += blocks_of(f); shared += shared_of(f); }
        snprintf(f, sizeof f, "%s/suspend.state", p);
        double state = blocks_of(f);
        shared += shared_of(f);
        static VmSnap sn[VM_SNAPS_MAX];
        int ns = vm_snaps_load(p, sn, VM_SNAPS_MAX);
        double snaps = 0;
        for (int i = 0; i < ns; i++) snaps += sn[i].vmstate;
        if (snaps > disks) snaps = disks;
        double all = tree_bytes(p, 0), other = all - disks - state;
        if (other < 0) other = 0;
        double frees = all - shared;
        if (m.base[0]) {
            for (int i = 0; i < nb; i++) if (!strcmp(bases[i].path, m.base) && bases[i].users + bases[i].trash == 1) frees += blocks_of(m.base);
        }
        if (m.iso[0]) {
            int dupi = 0;
            for (int i = 0; i < nis; i++) if (!strcmp(seen_iso[i], m.iso)) dupi = 1;
            if (!dupi && nis < 16) { snprintf(seen_iso[nis++], VM_PATH_MAX, "%s", m.iso); isos += blocks_of(m.iso); }
        }
        JSValue x = JS_NewObject(c);
        set_str(c, x, "dir", p);
        set_str(c, x, "name", m.name);
        set_str(c, x, "id", m.id);
        set_str(c, x, "base", m.base);
        JS_SetPropertyStr(c, x, "disks", JS_NewFloat64(c, disks - snaps));
        JS_SetPropertyStr(c, x, "snapshots", JS_NewFloat64(c, snaps));
        set_int(c, x, "snapshotCount", ns);
        JS_SetPropertyStr(c, x, "state", JS_NewFloat64(c, state));
        JS_SetPropertyStr(c, x, "other", JS_NewFloat64(c, other));
        JS_SetPropertyStr(c, x, "shared", JS_NewFloat64(c, shared));
        JS_SetPropertyStr(c, x, "total", JS_NewFloat64(c, all));
        JS_SetPropertyStr(c, x, "frees", JS_NewFloat64(c, frees < 0 ? 0 : frees));
        JS_SetPropertyUint32(c, arr, k++, x);
    }
    if (d) closedir(d);
    for (int i = 0; i < nb; i++) {
        JSValue x = JS_NewObject(c);
        set_str(c, x, "path", bases[i].path);
        JS_SetPropertyStr(c, x, "bytes", JS_NewFloat64(c, blocks_of(bases[i].path)));
        set_int(c, x, "users", bases[i].users);
        set_int(c, x, "trash", bases[i].trash);
        JS_SetPropertyUint32(c, barr, (uint32_t)i, x);
    }
    /* bases nobody names any more (all their machines deleted for good) */
    char sh[PATH_MAX + 16];
    snprintf(sh, sizeof sh, "%s/.shared", lib);
    DIR *sd = opendir(sh);
    uint32_t bi = (uint32_t)nb;
    while (sd && (e = readdir(sd))) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 7 || strcmp(e->d_name + l - 6, ".qcow2")) continue;
        char p[PATH_MAX + 300];
        snprintf(p, sizeof p, "%s/%s", sh, e->d_name);
        int known = 0;
        for (int i = 0; i < nb; i++) if (!strcmp(bases[i].path, p)) known = 1;
        if (known) continue;
        JSValue x = JS_NewObject(c);
        set_str(c, x, "path", p);
        JS_SetPropertyStr(c, x, "bytes", JS_NewFloat64(c, blocks_of(p)));
        set_int(c, x, "users", 0);
        set_int(c, x, "trash", 0);
        JS_SetPropertyUint32(c, barr, bi++, x);
    }
    if (sd) closedir(sd);
    JS_SetPropertyStr(c, o, "machines", arr);
    JS_SetPropertyStr(c, o, "bases", barr);
    JS_SetPropertyStr(c, o, "trash", JS_NewFloat64(c, trash));
    JS_SetPropertyStr(c, o, "isos", JS_NewFloat64(c, isos));
    return o;
}

/* removeBase(path) → bool: a shared base that no machine names, in the library or the Trash, goes */
static JSValue f_remove_base(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    char lib[PATH_MAX], want[PATH_MAX + 16];
    const char *p = JS_ToCString(c, argv[0]);
    if (!p) return JS_FALSE;
    int ok = !library(lib, sizeof lib, 0) && vm_base_valid(p);
    snprintf(want, sizeof want, "%s/.shared/", lib);
    ok = ok && !strncmp(p, want, strlen(want));
    char path[VM_PATH_MAX];
    snprintf(path, sizeof path, "%s", p);
    JS_FreeCString(c, p);
    if (!ok) return JS_FALSE;
    JSValue s = f_storage(c, JS_UNDEFINED, 0, NULL), b = JS_GetPropertyStr(c, s, "bases");
    int used = 1;
    uint32_t n = 0;
    JSValue len = JS_GetPropertyStr(c, b, "length");
    JS_ToUint32(c, &n, len);
    JS_FreeValue(c, len);
    for (uint32_t i = 0; i < n; i++) {
        JSValue x = JS_GetPropertyUint32(c, b, i), vp = JS_GetPropertyStr(c, x, "path");
        const char *xp = JS_ToCString(c, vp);
        if (xp && !strcmp(xp, path)) used = prop_int(c, x, "users") + prop_int(c, x, "trash");
        if (xp) JS_FreeCString(c, xp);
        JS_FreeValue(c, vp); JS_FreeValue(c, x);
    }
    JS_FreeValue(c, b); JS_FreeValue(c, s);
    return JS_NewBool(c, !used && !unlink(path));
}

/* ---- importing a machine of another virtualization app (6.22): lord-vm --import-plan and --import-machine ---- */

/* a file or a folder the person chose: absolute, real, readable, no control characters */
static int user_path(JSContext *c, JSValueConst v, char *out, size_t n) {
    const char *s = JS_ToCString(c, v);
    if (!s) return -1;
    char r[PATH_MAX];
    struct stat st;
    int ok = s[0] == '/' && realpath(s, r) && !stat(r, &st) && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) && !access(r, R_OK) && strlen(r) < n;
    JS_FreeCString(c, s);
    if (!ok) return -1;
    for (const unsigned char *p = (const unsigned char *)r; *p; p++) if (*p < 0x20 || *p == 0x7f) return -1;
    memcpy(out, r, strlen(r) + 1);
    return 0;
}

static int opt_str(JSContext *c, JSValueConst o, const char *k, char *out, size_t n) {
    out[0] = 0;
    if (!JS_IsObject(o)) return 0;
    JSValue v = JS_GetPropertyStr(c, o, k);
    int r = 0;
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        if (s && strlen(s) < n) snprintf(out, n, "%s", s); else r = -1;
        if (s) JS_FreeCString(c, s);
    }
    JS_FreeValue(c, v);
    return r;
}

/* importPlan(path, { arch }) → job id; the job's out is the plan's JSON (lord-vm --import-plan) */
static JSValue f_import_plan(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char src[PATH_MAX], arch[16];
    if (argc < 1 || user_path(c, argv[0], src, sizeof src) || opt_str(c, argc > 1 ? argv[1] : JS_UNDEFINED, "arch", arch, sizeof arch)) return JS_NewInt32(c, 0);
    if (arch[0] && strcmp(arch, "amd64") && strcmp(arch, "arm64")) return JS_NewInt32(c, 0);
    char *args[] = { "lord-vm", "--import-plan", src, arch[0] ? "--arch" : NULL, arch, NULL };
    return JS_NewInt32(c, spawn_job(args, ""));
}

/* importMachine({ path, name, arch, devices, network, firmware, vcpus, memoryMb }) → job id; progress lines, then
 * {"imported":true,"dir":...}. The new folder is ~/Machines/<name>.lordvm, as for create */
static JSValue f_import(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_NewInt32(c, 0);
    char lib[PATH_MAX], src[PATH_MAX], name[VM_NAME_MAX + 8], arch[16], devs[16], net[16], fw[16];
    JSValue vp = JS_GetPropertyStr(c, argv[0], "path");
    int bad = library(lib, sizeof lib, 1) || !JS_IsString(vp) || user_path(c, vp, src, sizeof src);
    JS_FreeValue(c, vp);
    if (bad || opt_str(c, argv[0], "name", name, sizeof name) || opt_str(c, argv[0], "arch", arch, sizeof arch) ||
        opt_str(c, argv[0], "devices", devs, sizeof devs) || opt_str(c, argv[0], "network", net, sizeof net) || opt_str(c, argv[0], "firmware", fw, sizeof fw))
        return JS_NewInt32(c, 0);
    if (!vm_name_valid(name) || (arch[0] && strcmp(arch, "amd64") && strcmp(arch, "arm64")) || (devs[0] && strcmp(devs, "virtio") && strcmp(devs, "compat")) ||
        (net[0] && strcmp(net, "shared") && strcmp(net, "isolated")) || (fw[0] && strcmp(fw, "uefi") && strcmp(fw, "bios")))
        return JS_NewInt32(c, 0);
    int vcpus = prop_int(c, argv[0], "vcpus"), mem = prop_int(c, argv[0], "memoryMb");
    if (vcpus < 0 || vcpus > VM_VCPUS_MAX || mem < 0 || mem > VM_MEM_MAX_MB || (mem && mem < VM_MEM_MIN_MB)) return JS_NewInt32(c, 0);
    char folder[64], dir[PATH_MAX + 80], sv[16], sm[16];
    folder_name(name, folder, sizeof folder);
    snprintf(dir, sizeof dir, "%s/%s.lordvm", lib, folder);
    for (int k = 2; !access(dir, F_OK) && k < 100; k++) snprintf(dir, sizeof dir, "%s/%s %d.lordvm", lib, folder, k);
    if (!access(dir, F_OK)) return JS_NewInt32(c, 0);
    snprintf(sv, sizeof sv, "%d", vcpus);
    snprintf(sm, sizeof sm, "%d", mem);
    char *args[24];
    int n = 0;
    args[n++] = "lord-vm"; args[n++] = "--import-machine"; args[n++] = src; args[n++] = dir; args[n++] = "--name"; args[n++] = name;
    if (arch[0]) { args[n++] = "--arch"; args[n++] = arch; }
    if (devs[0]) { args[n++] = "--devices"; args[n++] = devs; }
    if (net[0]) { args[n++] = "--network"; args[n++] = net; }
    if (fw[0]) { args[n++] = "--firmware"; args[n++] = fw; }
    if (vcpus) { args[n++] = "--vcpus"; args[n++] = sv; }
    if (mem) { args[n++] = "--memory"; args[n++] = sm; }
    args[n] = NULL;
    return JS_NewInt32(c, spawn_job(args, dir));
}

/* importInfo(dir) → imported.json's text (where it came from, what changed), or "" */
static JSValue f_import_info(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char dir[PATH_MAX];
    if (argc < 1 || machine_dir(c, argv[0], dir, sizeof dir)) return JS_NewString(c, "");
    char p[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/imported.json", dir);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return JS_NewString(c, "");
    char buf[8192];
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    buf[r > 0 ? r : 0] = 0;
    return JS_NewString(c, buf);
}

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("host", 0, f_host), JS_CFUNC_DEF("list", 0, f_list), JS_CFUNC_DEF("groups", 0, f_groups), JS_CFUNC_DEF("link", 2, f_link), JS_CFUNC_DEF("groupCreate", 1, f_group_create), JS_CFUNC_DEF("groupFresh", 2, f_group_fresh), JS_CFUNC_DEF("groupSet", 2, f_group_set), JS_CFUNC_DEF("create", 1, f_create), JS_CFUNC_DEF("hostDefaults", 0, f_host_defaults), JS_CFUNC_DEF("job", 1, f_job),
    JS_CFUNC_DEF("start", 1, f_start), JS_CFUNC_DEF("attach", 1, f_attach), JS_CFUNC_DEF("poll", 1, f_poll), JS_CFUNC_DEF("cmd", 2, f_cmd),
    JS_CFUNC_DEF("stop", 2, f_stop), JS_CFUNC_DEF("key", 3, f_key), JS_CFUNC_DEF("motion", 3, f_motion), JS_CFUNC_DEF("button", 3, f_button),
    JS_CFUNC_DEF("wheel", 3, f_wheel), JS_CFUNC_DEF("release", 1, f_release), JS_CFUNC_DEF("capture", 3, f_capture), JS_CFUNC_DEF("resize", 3, f_resize), JS_CFUNC_DEF("rate", 2, f_rate), JS_CFUNC_DEF("shown", 2, f_shown),
    JS_CFUNC_DEF("close", 1, f_close), JS_CFUNC_DEF("log", 3, f_log), JS_CFUNC_DEF("eject", 1, f_eject),
    JS_CFUNC_DEF("set", 2, f_set), JS_CFUNC_DEF("stats", 1, f_stats), JS_CFUNC_DEF("snapshot", 1, f_snapshot), JS_CFUNC_DEF("protect", 1, f_protect),
    JS_CFUNC_DEF("isoInfo", 1, f_isoinfo), JS_CFUNC_DEF("hash", 1, f_hash), JS_CFUNC_DEF("sigCheck", 3, f_sig_check), JS_CFUNC_DEF("screenshot", 1, f_screenshot), JS_CFUNC_DEF("cdrom", 2, f_cdrom),
    JS_CFUNC_DEF("snaps", 1, f_snaps), JS_CFUNC_DEF("snapTake", 3, f_snap_take), JS_CFUNC_DEF("snapRevert", 2, f_snap_revert),
    JS_CFUNC_DEF("snapDelete", 2, f_snap_delete),
    JS_CFUNC_DEF("cloneInfo", 1, f_clone_info), JS_CFUNC_DEF("clone", 2, f_clone), JS_CFUNC_DEF("compact", 2, f_compact),
    JS_CFUNC_DEF("storage", 0, f_storage), JS_CFUNC_DEF("record", 1, f_record), JS_CFUNC_DEF("console", 1, f_console),
    JS_CFUNC_DEF("shell", 3, f_shell), JS_CFUNC_DEF("guestNotes", 1, f_guest_notes), JS_CFUNC_DEF("guestLinkAnswer", 3, f_guest_link_answer),
    JS_CFUNC_DEF("agent", 3, f_agent), JS_CFUNC_DEF("transferFolder", 1, f_transfer_folder), JS_CFUNC_DEF("transferAside", 1, f_transfer_aside), JS_CFUNC_DEF("renamePlan", 2, f_rename_plan), JS_CFUNC_DEF("rename", 2, f_rename), JS_CFUNC_DEF("clipboardSend", 2, f_clip_send), JS_CFUNC_DEF("clipboardTake", 2, f_clip_take),
    JS_CFUNC_DEF("clipboardApply", 2, f_clip_apply), JS_CFUNC_DEF("clipboardSendHost", 2, f_clip_send_host),
    JS_CFUNC_DEF("clipboardGate", 3, f_clip_gate), JS_CFUNC_DEF("clipboardAnswer", 3, f_clip_answer), JS_CFUNC_DEF("clipboardPreview", 2, f_clip_preview), JS_CFUNC_DEF("sendFile", 3, f_send_file), JS_CFUNC_DEF("files", 1, f_files), JS_CFUNC_DEF("removeBase", 1, f_remove_base),
    JS_CFUNC_DEF("importPlan", 2, f_import_plan), JS_CFUNC_DEF("importMachine", 1, f_import), JS_CFUNC_DEF("importInfo", 1, f_import_info),
    JS_CFUNC_DEF("startDetached", 1, f_start_detached), JS_CFUNC_DEF("sound", 2, f_sound), JS_CFUNC_DEF("refusal", 0, f_refusal), JS_CFUNC_DEF("discardState", 1, f_discard), JS_CFUNC_DEF("awake", 0, f_awake),
    JS_CFUNC_DEF("apps", 2, f_apps), JS_CFUNC_DEF("windows", 1, f_windows), JS_CFUNC_DEF("winIcon", 2, f_win_icon), JS_CFUNC_DEF("winAct", 3, f_win_act), JS_CFUNC_DEF("closeRefusals", 1, f_close_refusals),
    JS_CFUNC_DEF("guestApps", 1, f_guest_apps), JS_CFUNC_DEF("gpus", 0, f_gpus), JS_CFUNC_DEF("launch", 2, f_launch),
};

void js_vm_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "vm", o);
}
