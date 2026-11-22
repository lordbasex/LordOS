/* vmgpu_admin.c — Machines' graphics cards in the admin helper (vm_admin.h: gpu-role, vm-gvt-create, vm-gvt-remove,
 * gpu-min-freq; AGENTS.md "Machines' graphics cards", docs/MACHINES-GPU.md Mode A). Every argument is checked here
 * against the GPUs root reads itself from sysfs (vmgpu.c); the GPU that drives the desktop (the cards the session's
 * lord-server, this helper's parent, holds open) is always refused. Writes only: the two fixed configuration files
 * (and removes the two hand-written ones only when they say exactly what LordOS writes), an mdev's create/remove, the
 * iGPU's minimum frequency, and its own notes under /run/lordos/vm-gvt. Runs one program: update-initramfs -u.
 *
 * For the tests every path is under vmgpu_admin_root ("" here) and the desktop's descriptors are read from
 * vmgpu_admin_fd_dir (NULL: /proc/<parent>/fd); vmgpu_admin_initramfs runs update-initramfs. */
#include "vm_admin.h"
#include "vmgpu.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

static int run_initramfs(void) {
    const char *const argv[] = { "/usr/sbin/update-initramfs", "-u", NULL };
    return admin_run(argv);
}

static int real_prlimit(pid_t pid, const struct rlimit *rl) { return prlimit(pid, RLIMIT_MEMLOCK, rl, NULL); }

const char *vmgpu_admin_root = "";
int (*vmgpu_admin_prlimit)(pid_t pid, const struct rlimit *rl) = real_prlimit;
/* the one lord-vm whose QEMUs vm-memlock serves: the installed one (a test sets its fake root's own) */
const char *vmgpu_admin_lordvm = "/usr/lib/lordos/bin/lord-vm";
const char *vmgpu_admin_fd_dir = NULL;
int (*vmgpu_admin_initramfs)(void) = run_initramfs;

#define R vmgpu_admin_root

static void rpath(char *out, size_t n, const char *fmt, const char *a, const char *b) {
    char rel[600];
    int k = snprintf(rel, sizeof rel, fmt, a, b);
    if (k < 0 || (size_t)k >= sizeof rel) { out[0] = 0; return; }
    k = snprintf(out, n, "%s%s", R, rel);
    if (k < 0 || (size_t)k >= n) out[0] = 0;
}

static int read_line(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r < 0) return -1;
    out[r] = 0;
    out[strcspn(out, "\n")] = 0;
    return 0;
}

/* a sysfs attribute written as the kernel wants it: one write, no O_CREAT (it must already be there; a test's folder
 * has the file) */
static int write_attr(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t w = write(fd, text, strlen(text));
    int e = errno;
    close(fd);
    errno = e;
    return w == (ssize_t)strlen(text) ? 0 : -1;
}

/* a configuration file replaced whole: a new file beside it, then rename; never through a link */
static int write_file(const char *path, const char *text) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.lordos-new", path);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    size_t n = strlen(text);
    int ok = write(fd, text, n) == (ssize_t)n && !fsync(fd);
    close(fd);
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

static int file_says_ours(const char *path, int which) {
    char text[2048];
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    ssize_t r = read(fd, text, sizeof text - 1);
    close(fd);
    if (r < 0) return 0;
    text[r] = 0;
    return vmgpu_conf_ours(text, which);
}

/* the GPUs, with the desktop's marked */
static int gpus(VmGpu *g) {
    int n = vmgpu_list(R, g, VMGPU_MAX);
    char fdd[64];
    if (!vmgpu_admin_fd_dir) snprintf(fdd, sizeof fdd, "/proc/%d/fd", (int)getppid());
    vmgpu_mark_desktop(R, vmgpu_admin_fd_dir ? vmgpu_admin_fd_dir : fdd, g, n);
    return n;
}

static const VmGpu *find(const VmGpu *g, int n, const char *addr) {
    for (int i = 0; i < n; i++) if (!strcmp(g[i].addr, addr)) return &g[i];
    return NULL;
}

/* ---- gpu-role ---- */

void vm_act_gpu_role(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char addr[16];
    int role;
    if (vmgpu_role_arg(arg, len, addr, &role)) { admin_err("admin.invalidArgument", NULL); return; }
    VmGpu g[VMGPU_MAX];
    int n = gpus(g);
    const VmGpu *x = find(g, n, addr);
    if (!x) { admin_err("gpu.notFound", "card", addr, NULL); return; }
    if (x->desktop) { admin_err("gpu.desktop", "card", addr, NULL); return; }
    if (role == VMGPU_ROLE_DEDICATED || role == VMGPU_ROLE_AI) { admin_err("gpu.roleNotYet", "card", addr, NULL); return; }
    char probe[1024], load[1024], lprobe[1024], lload[1024];
    snprintf(probe, sizeof probe, "%s%s", R, VMGPU_MODPROBE);
    snprintf(load, sizeof load, "%s%s", R, VMGPU_MODLOAD);
    snprintf(lprobe, sizeof lprobe, "%s%s", R, VMGPU_LEGACY_MODPROBE);
    snprintf(lload, sizeof lload, "%s%s", R, VMGPU_LEGACY_MODLOAD);
    int changed = 0;
    if (role == VMGPU_ROLE_SHARED) {
        if (!x->gvt_capable) { admin_err("gpu.notGvt", "card", addr, NULL); return; }
        if (!file_says_ours(probe, 0)) {
            if (write_file(probe, VMGPU_MODPROBE_TEXT)) { admin_err("gpu.writeFailed", "name", VMGPU_MODPROBE, NULL); return; }
            changed = 1;
        }
        if (!file_says_ours(load, 1)) {
            if (write_file(load, VMGPU_MODLOAD_TEXT)) { unlink(probe); admin_err("gpu.writeFailed", "name", VMGPU_MODLOAD, NULL); return; }
            changed = 1;
        }
        /* the same lines written by hand before (the feasibility test): adopted, so nothing says it twice */
        if (file_says_ours(lprobe, 0) && !unlink(lprobe)) changed = 1;
        if (file_says_ours(lload, 1) && !unlink(lload)) changed = 1;
    } else {
        /* unused: only an iGPU GVT-g can share has anything written for it */
        if (x->gvt_capable) {
            if (!unlink(probe)) changed = 1;
            if (!unlink(load)) changed = 1;
            if (file_says_ours(lprobe, 0) && !unlink(lprobe)) changed = 1;
            if (file_says_ours(lload, 1) && !unlink(lload)) changed = 1;
        }
    }
    if (changed && vmgpu_admin_initramfs && vmgpu_admin_initramfs()) { admin_err("gpu.initramfsFailed", NULL); return; }
    admin_ok("gpu.roleSet", "card", addr, NULL);
}

/* ---- vm-gvt-create, vm-gvt-remove ---- */

/* the note of who asked for a vGPU: its uid, or -1 */
static long note_uid(const char *id) {
    char p[1024], b[32];
    rpath(p, sizeof p, VMGPU_RUN "/%s%s", id, "");
    if (read_line(p, b, sizeof b) || !b[0]) return -1;
    char *e;
    long u = strtol(b, &e, 10);
    return (*e && *e != ' ') || e == b || u < 0 ? -1 : u;
}

static int note_write(const char *id, uid_t uid) {
    char d[1024], p[1024], b[32];
    rpath(d, sizeof d, "/run/lordos%s%s", "", "");
    mkdir(d, 0755);
    rpath(d, sizeof d, VMGPU_RUN "%s%s", "", "");
    if (mkdir(d, 0700) && errno != EEXIST) return -1;
    rpath(p, sizeof p, VMGPU_RUN "/%s%s", id, "");
    snprintf(b, sizeof b, "%u\n", (unsigned)uid);
    return write_file(p, b);
}

static void grant_release(const char *id);
static void note_drop(const char *id) {
    grant_release(id);
    char p[1024];
    rpath(p, sizeof p, VMGPU_RUN "/%s%s", id, "");
    unlink(p);
}

/* the mdev's IOMMU group, or -1 when it isn't there */
static int mdev_group(const char *id) {
    char p[1024], t[256];
    rpath(p, sizeof p, "/sys/bus/mdev/devices/%s/iommu_group", id, "");
    ssize_t l = readlink(p, t, sizeof t - 1);
    if (l <= 0) return -1;
    t[l] = 0;
    const char *b = strrchr(t, '/');
    b = b ? b + 1 : t;
    if (!*b || strspn(b, "0123456789") != strlen(b) || strlen(b) > 6) return -1;
    return atoi(b);
}

/* /dev/vfio/<group> is open in some process (root reads every process's descriptors) */
static int group_in_use(int grp) {
    char want[48], proc[1024];
    snprintf(want, sizeof want, "/dev/vfio/%d", grp);
    rpath(proc, sizeof proc, "/proc%s%s", "", "");
    DIR *d = opendir(proc);
    if (!d) return 1;
    int used = 0;
    for (struct dirent *e; !used && (e = readdir(d));) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char fdp[1100];
        int k = snprintf(fdp, sizeof fdp, "%s/%s/fd", proc, e->d_name);
        DIR *f = k > 0 && (size_t)k < sizeof fdp ? opendir(fdp) : NULL;
        if (!f) continue;
        for (struct dirent *x; !used && (x = readdir(f));) {
            if (x->d_name[0] == '.') continue;
            char lp[1400], t[64];
            snprintf(lp, sizeof lp, "%s/%s", fdp, x->d_name);
            ssize_t l = readlink(lp, t, sizeof t - 1);
            if (l > 0) { t[l] = 0; if (!strcmp(t, want)) used = 1; }
        }
        closedir(f);
    }
    closedir(d);
    return used;
}

/* whether process pid holds /dev/vfio/<group> open */
static int pid_holds_group(int pid, int grp) {
    char want[48], fdp[700];
    snprintf(want, sizeof want, "/dev/vfio/%d", grp);
    snprintf(fdp, sizeof fdp, "%s/proc/%d/fd", R, pid);
    DIR *f = opendir(fdp);
    if (!f) return 0;
    int held = 0;
    for (struct dirent *x; !held && (x = readdir(f));) {
        if (x->d_name[0] == '.') continue;
        char lp[1400], t[64];
        snprintf(lp, sizeof lp, "%s/%s", fdp, x->d_name);
        ssize_t l = readlink(lp, t, sizeof t - 1);
        if (l > 0) { t[l] = 0; if (!strcmp(t, want)) held = 1; }
    }
    closedir(f);
    return held;
}

/* /dev/vfio/<group> to the caller, 0600: checked by what it is (that group's character device), never followed */
static int give_group(int grp, uid_t uid, gid_t gid) {
    char p[1024], dev[32], g[16];
    snprintf(g, sizeof g, "%d", grp);
    rpath(p, sizeof p, "/sys/class/vfio/%s/dev", g, "");
    unsigned ma, mi;
    if (read_line(p, dev, sizeof dev) || sscanf(dev, "%u:%u", &ma, &mi) != 2) return -1;
    rpath(p, sizeof p, "/dev/vfio/%s", g, "");
    int fd = -1;
    for (int i = 0; i < 50 && (fd = open(p, O_PATH | O_NOFOLLOW | O_CLOEXEC)) < 0; i++) { struct timespec t = { 0, 20000000 }; nanosleep(&t, NULL); }
    if (fd < 0) return -1;
    struct stat st;
    int ok = !fstat(fd, &st) && (S_ISCHR(st.st_mode) || (R[0] && S_ISREG(st.st_mode))) &&
             (R[0] || (major(st.st_rdev) == ma && minor(st.st_rdev) == mi)) && !fchownat(fd, "", uid, gid, AT_EMPTY_PATH);
    if (ok) {
        char fp[64];
        snprintf(fp, sizeof fp, "/proc/self/fd/%d", fd);
        ok = !chmod(fp, 0600);
    }
    close(fd);
    return ok ? 0 : -1;
}

void vm_act_gvt_create(const AdminReq *r, const char *arg, size_t len) {
    char id[37], type[VMGPU_TYPE_MAX + 1];
    if (vmgpu_create_arg(arg, len, id, type)) { admin_err("admin.invalidArgument", NULL); return; }
    VmGpu g[VMGPU_MAX];
    int n = gpus(g);
    const VmGpu *x = vmgpu_gvt(g, n);
    if (!x) { admin_err("vm.gvtNone", NULL); return; }
    int grp = mdev_group(id);
    if (grp >= 0) {
        /* already there: this caller's (by root's note) is given again, anyone else's is refused */
        if (note_uid(id) != (long)r->uid) { admin_err("vm.gvtBusy", NULL); return; }
    } else {
        int k = -1;
        for (int i = 0; i < x->n_types; i++) if (!strcmp(x->types[i].name, type)) k = i;
        if (k < 0 || x->types[k].available <= 0) { admin_err("vm.gvtNoType", "type", type, NULL); return; }
        char p[1024];
        rpath(p, sizeof p, "/sys/bus/pci/devices/%s/mdev_supported_types/%s/create", x->addr, type);
        if (note_write(id, r->uid)) { admin_err("vm.gvtFailed", NULL); return; }
        if (write_attr(p, id)) {
            fprintf(stderr, "admin: vm-gvt-create: %s on %s: %s\n", type, x->addr, strerror(errno));
            note_drop(id);
            admin_err("vm.gvtFailed", NULL);
            return;
        }
        for (int i = 0; i < 100 && (grp = mdev_group(id)) < 0; i++) { struct timespec t = { 0, 20000000 }; nanosleep(&t, NULL); }
        if (grp < 0) { note_drop(id); admin_err("vm.gvtFailed", NULL); return; }
    }
    if (give_group(grp, r->uid, r->gid)) {
        char p[1024];
        rpath(p, sizeof p, "/sys/bus/mdev/devices/%s/remove", id, "");
        if (!group_in_use(grp) && !write_attr(p, "1")) note_drop(id);
        admin_err("vm.gvtFailed", NULL);
        return;
    }
    char gs[16];
    snprintf(gs, sizeof gs, "%d", grp);
    admin_ok("vm.gvtReady", "name", gs, NULL);
}

void vm_act_gvt_remove(const AdminReq *r, const char *arg, size_t len) {
    char id[37];
    if (vmgpu_remove_arg(arg, len, id)) { admin_err("admin.invalidArgument", NULL); return; }
    int grp = mdev_group(id);
    long owner = note_uid(id);
    if (grp < 0) { if (owner == (long)r->uid) note_drop(id); admin_ok("vm.gvtGone", NULL); return; }
    if (owner != (long)r->uid) { admin_err("vm.gvtBusy", NULL); return; }
    if (group_in_use(grp)) { admin_err("vm.gvtInUse", NULL); return; }
    char p[1024];
    rpath(p, sizeof p, "/sys/bus/mdev/devices/%s/remove", id, "");
    if (write_attr(p, "1")) { admin_err("vm.gvtFailed", NULL); return; }
    note_drop(id);
    admin_ok("vm.gvtGone", NULL);
}

/* ---- gpu-min-freq ---- */

/* the iGPU's rps file (gt/gt0/rps_<what>_freq_mhz, or the older card-level gt_<what>_freq_mhz) */
static int freq_path(const VmGpu *x, const char *what, char *out, size_t n) {
    char card[16];
    snprintf(card, sizeof card, "card%d", x->card);
    char rel[200];
    snprintf(rel, sizeof rel, "/sys/bus/pci/devices/%s/drm/%s/gt/gt0/rps_", x->addr, card);
    int k = snprintf(out, n, "%s%s%s_freq_mhz", R, rel, what);
    if (k > 0 && (size_t)k < n && !access(out, F_OK)) return 0;
    snprintf(rel, sizeof rel, "/sys/bus/pci/devices/%s/drm/%s/gt_", x->addr, card);
    k = snprintf(out, n, "%s%s%s_freq_mhz", R, rel, what);
    return k < 0 || (size_t)k >= n || access(out, F_OK) ? -1 : 0;
}

static int read_mhz(const VmGpu *x, const char *what) {
    char p[1100], b[32];
    if (freq_path(x, what, p, sizeof p) || read_line(p, b, sizeof b)) return -1;
    return atoi(b);
}

/* any vGPU root made (a note) that is still there; for uid only when uid >= 0 */
static int any_vgpu(long uid) {
    char d[1024];
    rpath(d, sizeof d, VMGPU_RUN "%s%s", "", "");
    DIR *dir = opendir(d);
    if (!dir) return 0;
    int found = 0;
    for (struct dirent *e; !found && (e = readdir(dir));) {
        char id[37];
        if (strlen(e->d_name) != 36 || vmgpu_remove_arg(e->d_name, 36, id)) continue;
        if (mdev_group(id) < 0) continue;
        if (uid < 0 || note_uid(id) == uid) found = 1;
    }
    closedir(dir);
    return found;
}

void vm_act_gpu_min_freq(const AdminReq *r, const char *arg, size_t len) {
    char addr[16];
    int mhz;
    if (vmgpu_freq_arg(arg, len, addr, &mhz)) { admin_err("admin.invalidArgument", NULL); return; }
    VmGpu g[VMGPU_MAX];
    int n = gpus(g);
    const VmGpu *x = find(g, n, addr);
    if (!x) { admin_err("gpu.notFound", "card", addr, NULL); return; }
    if (x->desktop) { admin_err("gpu.desktop", "card", addr, NULL); return; }
    if (!x->gvt_on || x->card < 0) { admin_err("gpu.notGvt", "card", addr, NULL); return; }
    char minp[1100], saved[1024], b[32];
    if (freq_path(x, "min", minp, sizeof minp)) { admin_err("gpu.writeFailed", "name", addr, NULL); return; }
    rpath(saved, sizeof saved, VMGPU_RUN "/minfreq-%s%s", addr, "");
    if (!mhz) {
        /* back to the value read before the first change, once no machine has a vGPU any more */
        if (read_line(saved, b, sizeof b)) { admin_ok("gpu.freqSet", NULL); return; }
        if (any_vgpu(-1)) { admin_ok("gpu.freqSet", NULL); return; }
        if (write_attr(minp, b)) { admin_err("gpu.writeFailed", "name", addr, NULL); return; }
        unlink(saved);
        admin_ok("gpu.freqSet", NULL);
        return;
    }
    if (!any_vgpu((long)r->uid)) { admin_err("vm.gvtNone", NULL); return; }
    int lo = read_mhz(x, "RPn"), hi = read_mhz(x, "RP0");
    if (lo <= 0 || hi <= 0 || mhz < lo || mhz > hi) { admin_err("gpu.freqRange", NULL); return; }
    if (access(saved, F_OK)) {
        int cur = read_mhz(x, "min");
        if (cur <= 0) { admin_err("gpu.writeFailed", "name", addr, NULL); return; }
        snprintf(b, sizeof b, "%d\n", cur);
        char d[1024];
        rpath(d, sizeof d, "/run/lordos%s%s", "", "");
        mkdir(d, 0755);
        rpath(d, sizeof d, VMGPU_RUN "%s%s", "", "");
        mkdir(d, 0700);
        if (write_file(saved, b)) { admin_err("gpu.writeFailed", "name", addr, NULL); return; }
    }
    snprintf(b, sizeof b, "%d", mhz);
    if (write_attr(minp, b)) { admin_err("gpu.writeFailed", "name", addr, NULL); return; }
    admin_ok("gpu.freqSet", NULL);
}

/* at the helper's start and end: the caller's vGPUs no process holds go, and the minimum frequency comes back when
 * none is left */
void vm_gvt_sweep(uid_t uid) {
    char d[1024];
    rpath(d, sizeof d, VMGPU_RUN "%s%s", "", "");
    DIR *dir = opendir(d);
    if (!dir) return;
    char ids[16][37];
    int k = 0;
    for (struct dirent *e; k < 16 && (e = readdir(dir));) {
        char id[37];
        if (strlen(e->d_name) != 36 || vmgpu_remove_arg(e->d_name, 36, id) || note_uid(id) != (long)uid) continue;
        snprintf(ids[k++], 37, "%s", id);
    }
    closedir(dir);
    for (int i = 0; i < k; i++) {
        int grp = mdev_group(ids[i]);
        char p[1024];
        rpath(p, sizeof p, "/sys/bus/mdev/devices/%s/remove", ids[i], "");
        if (grp < 0) note_drop(ids[i]);
        else if (!group_in_use(grp) && !write_attr(p, "1")) { note_drop(ids[i]); fprintf(stderr, "admin: removed a machine's vGPU nobody uses (%s)\n", ids[i]); }
    }
    if (any_vgpu(-1)) return;
    VmGpu g[VMGPU_MAX];
    int n = vmgpu_list(R, g, VMGPU_MAX);
    for (int i = 0; i < n; i++) {
        char saved[1024], minp[1100], b[32];
        rpath(saved, sizeof saved, VMGPU_RUN "/minfreq-%s%s", g[i].addr, "");
        if (read_line(saved, b, sizeof b)) continue;
        if (!freq_path(&g[i], "min", minp, sizeof minp) && !write_attr(minp, b)) unlink(saved);
    }
}

/* ---- vm-memlock, gpu-memlock-cap (maintainer's decision 2026-11-21) ---- */

/* "Uid:" (all four the same as uid) and "PPid:" of a process's status; -1 when it isn't uid's */
static int proc_owned(int pid, uid_t uid, int *ppid) {
    char rel[64], b[4096], path[700];
    snprintf(rel, sizeof rel, "/proc/%d/status", pid);
    snprintf(path, sizeof path, "%s%s", R, rel);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t r = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) close(fd);
    if (r <= 0) return -1;
    b[r] = 0;
    const char *u = strstr(b, "\nUid:"), *pp = strstr(b, "\nPPid:");
    unsigned a[4];
    if (!u || !pp || sscanf(u + 5, "%u %u %u %u", &a[0], &a[1], &a[2], &a[3]) != 4) return -1;
    for (int i = 0; i < 4; i++) if (a[i] != (unsigned)uid) return -1;
    if (ppid) *ppid = atoi(pp + 6);
    return 0;
}

static int proc_exe(int pid, char *out, size_t n) {
    char path[700];
    snprintf(path, sizeof path, "%s/proc/%d/exe", R, pid);
    ssize_t l = readlink(path, out, n - 1);
    if (l <= 0) return -1;
    out[l] = 0;
    return 0;
}

/* a process's start time (/proc/<pid>/stat, field 22, after the command's closing parenthesis); 0 unknown. With the pid
 * it names one process: a pid used again has another start time */
static unsigned long long proc_start(int pid) {
    char path[700], b[1024];
    snprintf(path, sizeof path, "%s/proc/%d/stat", R, pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    char *q = strrchr(b, ')');
    if (!q) return 0;
    q++;
    for (int field = 3; field <= 22 && q; field++) {       /* after ')': field 3 (state) */
        while (*q == ' ') q++;
        if (field == 22) return strtoull(q, NULL, 10);
        q = strchr(q, ' ');
    }
    return 0;
}

/* The grant made on a vGPU, its own record beside the vGPU's note (/run/lordos/vm-gvt/grant-<id>, "<uid> <pid> <start>
 * <MB>"): it counts until the granted process ends, whatever happens to the vGPU; when the vGPU goes while that process
 * lives (vm-gvt-remove, the sweep), its limit goes back to the default first (grant_release). Inheritance can't carry a
 * raised limit past it: QEMU runs with its own -sandbox spawn=deny (fork, vfork, execve and clone without CLONE_THREAD
 * refused, lordvm.c) and our filter refuses clone3, so the granted process never has a child. -1 when there is none */
static int note_grant(const char *id, long *pid, unsigned long long *start, long *mb) {
    char p[1024], b[96];
    rpath(p, sizeof p, VMGPU_RUN "/grant-%s%s", id, "");
    unsigned long u;
    if (read_line(p, b, sizeof b) || sscanf(b, "%lu %ld %llu %ld", &u, pid, start, mb) != 4) return -1;
    return 0;
}

static int note_grant_write(const char *id, uid_t uid, int pid, unsigned long long start, long mb) {
    char p[1024], b[96];
    rpath(p, sizeof p, VMGPU_RUN "/grant-%s%s", id, "");
    snprintf(b, sizeof b, "%u %d %llu %ld\n", (unsigned)uid, pid, start, mb);
    return write_file(p, b);
}

/* the granted process still runs (the same pid with the same start time) */
static int grant_live(long pid, unsigned long long start) { return pid > 1 && pid < 1000000000 && start && proc_start((int)pid) == start; }

static void grant_unlink(const char *id) {
    char p[1024];
    rpath(p, sizeof p, VMGPU_RUN "/grant-%s%s", id, "");
    unlink(p);
}

/* the default limit a released grant goes back to (systemd's and Debian's: 8 MB) */
#define VMGPU_MEMLOCK_DEFAULT ((rlim_t)8 << 20)

/* a vGPU's grant ends: a granted process that still lives gets the default limit back, then the record goes */
static void grant_release(const char *id) {
    long pid, mb;
    unsigned long long st;
    if (note_grant(id, &pid, &st, &mb)) return;
    if (grant_live(pid, st)) {
        struct rlimit rl = { VMGPU_MEMLOCK_DEFAULT, VMGPU_MEMLOCK_DEFAULT };
        if (vmgpu_admin_prlimit((pid_t)pid, &rl)) { fprintf(stderr, "admin: a vGPU's grant: can't put %ld's locked memory back\n", pid); return; }
    }
    grant_unlink(id);
}

/* MB granted to the live QEMUs of every vGPU but `except`; the records of processes that ended go */
static long grants_sum(const char *except) {
    char d[1024];
    rpath(d, sizeof d, VMGPU_RUN "%s%s", "", "");
    DIR *dir = opendir(d);
    if (!dir) return 0;
    long sum = 0;
    char dead[16][37];
    int nd = 0;
    for (struct dirent *e; (e = readdir(dir));) {
        char id[37];
        long pid, mb;
        unsigned long long st;
        if (strncmp(e->d_name, "grant-", 6) || strlen(e->d_name) != 42 || vmgpu_remove_arg(e->d_name + 6, 36, id) || !strcmp(id, except)) continue;
        if (note_grant(id, &pid, &st, &mb)) continue;
        if (grant_live(pid, st)) { if (mb > 0) sum += mb; }
        else if (nd < 16) snprintf(dead[nd++], 37, "%s", id);
    }
    closedir(dir);
    for (int i = 0; i < nd; i++) grant_unlink(dead[i]);
    return sum;
}

/* lord-vm's QEMU gets its locked-memory limit only through this. The pid must be a live process of the caller whose exe
 * is the system's QEMU and whose parent is the installed lord-vm (vmgpu_admin_lordvm, never a program of that name
 * elsewhere), whose -uuid and vfio-pci-nohotplug sysfsdev name a vGPU root's note gives the caller, and which holds that
 * vGPU's /dev/vfio/<group> open. One grant per vGPU: the granted pid and its start time go into root's note, and another
 * process is refused while that one lives. The cap is for all the live grants together (every user's): refused when they
 * and this one would pass it. A pidfd taken first keeps the checks on that process. The limit (soft and hard) is the
 * process's own -m + 512 MB */
void vm_act_memlock(const AdminReq *r, const char *arg, size_t len) {
    int pid, ppid = 0;
    if (vmgpu_pid_arg(arg, len, &pid)) { admin_err("admin.invalidArgument", NULL); return; }
    int pfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pfd < 0) { admin_err("vm.memlockRefused", NULL); return; }
    char exe[256], pexe[256];
    int ok = !proc_owned(pid, r->uid, &ppid) && ppid > 1 && !proc_exe(pid, exe, sizeof exe) &&
             (!strcmp(exe, "/usr/bin/qemu-system-x86_64") || !strcmp(exe, "/usr/bin/qemu-system-aarch64")) &&
             !proc_owned(ppid, r->uid, NULL) && !proc_exe(ppid, pexe, sizeof pexe);
    /* the parent: the installed lord-vm itself, not any program of that name */
    ok = ok && !strcmp(pexe, vmgpu_admin_lordvm);
    /* the command line: -uuid <id>, -m <MB>, and the vGPU of that same id */
    char cl[16384], id[37] = "";
    long mem = 0;
    int vgpu = 0;
    if (ok) {
        char path[700];
        snprintf(path, sizeof path, "%s/proc/%d/cmdline", R, pid);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        ssize_t n = fd >= 0 ? read(fd, cl, sizeof cl - 1) : -1;
        if (fd >= 0) close(fd);
        ok = n > 0 && n < (ssize_t)sizeof cl - 1;
        if (ok) {
            cl[n] = 0;
            const char *a[1024];
            int na = 0;
            for (ssize_t o = 0; o < n && na < 1024; o += (ssize_t)strlen(cl + o) + 1) a[na++] = cl + o;
            for (int i = 0; i + 1 < na; i++) {
                if (!strcmp(a[i], "-uuid") && !vmgpu_remove_arg(a[i + 1], strlen(a[i + 1]), id)) {}
                if (!strcmp(a[i], "-m")) {
                    const char *v = a[i + 1];
                    char *e;
                    long x = strtol(v, &e, 10);
                    mem = !*e && v[0] >= '1' && v[0] <= '9' && x >= 64 && x <= 16 * 1024 * 1024 ? x : -1;
                }
            }
            char want[96];
            snprintf(want, sizeof want, "sysfsdev=/sys/bus/mdev/devices/%s,", id);
            for (int i = 0; id[0] && i < na; i++) if (!strncmp(a[i], "vfio-pci-nohotplug,", 19) && strstr(a[i], want)) vgpu = 1;
        }
    }
    int grp = id[0] ? mdev_group(id) : -1;
    ok = ok && id[0] && mem > 0 && vgpu && note_uid(id) == (long)r->uid && grp >= 0;
    /* a command line naming the vGPU proves nothing by itself: the process must hold that vGPU's group open */
    ok = ok && pid_holds_group(pid, grp);
    unsigned long long start = ok ? proc_start(pid) : 0;
    if (!ok || !start) { close(pfd); admin_err("vm.memlockRefused", NULL); return; }
    /* the cap counts every grant: one check-and-grant at a time, held until the grant is written and applied */
    char lp[1024];
    rpath(lp, sizeof lp, VMGPU_RUN "/.lock%s%s", "", "");
    int lk = open(lp, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lk < 0 || flock(lk, LOCK_EX)) { if (lk >= 0) close(lk); close(pfd); admin_err("vm.memlockRefused", NULL); return; }
    /* one grant per vGPU: another while the one granted lives is refused (the same process again changes nothing) */
    long gpid, gmb;
    unsigned long long gstart;
    if (!note_grant(id, &gpid, &gstart, &gmb) && grant_live(gpid, gstart)) {
        close(pfd);
        close(lk);
        if (gpid == pid && gstart == start) { char ns[24]; snprintf(ns, sizeof ns, "%ld", gmb); admin_ok("vm.memlockSet", "need", ns, NULL); }
        else admin_err("vm.memlockRefused", NULL);
        return;
    }
    long total = vmgpu_total_mb(R), cap = vmgpu_memlock_cap_mb(R, total, NULL), need = mem + VMGPU_MEMLOCK_EXTRA_MB;
    /* the cap is for every machine's QEMU together, every user's: the live grants of the other vGPUs count */
    long others = grants_sum(id);
    if (others + need > cap) {
        char ns[24], cs[24];
        snprintf(ns, sizeof ns, "%ld", need);
        snprintf(cs, sizeof cs, "%ld", cap - others > 0 ? cap - others : 0);
        close(pfd);
        close(lk);
        admin_err("vm.memlockCap", "need", ns, "cap", cs, NULL);
        return;
    }
    /* still the process checked */
    if (syscall(SYS_pidfd_send_signal, pfd, 0, NULL, 0)) { close(pfd); close(lk); admin_err("vm.memlockRefused", NULL); return; }
    /* the record first, so the grant is counted before it exists; taken back when the limit can't be set */
    if (note_grant_write(id, r->uid, pid, start, need)) { close(pfd); close(lk); admin_err("vm.memlockRefused", NULL); return; }
    struct rlimit rl = { (rlim_t)need << 20, (rlim_t)need << 20 };
    int e = vmgpu_admin_prlimit((pid_t)pid, &rl);
    close(pfd);
    if (e) { grant_unlink(id); close(lk); admin_err("vm.memlockRefused", NULL); return; }
    close(lk);
    char ns[24];
    snprintf(ns, sizeof ns, "%ld", need);
    admin_ok("vm.memlockSet", "need", ns, NULL);
}

void vm_act_gpu_memlock_cap(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    long mb;
    if (vmgpu_memlock_arg(arg, len, &mb)) { admin_err("admin.invalidArgument", NULL); return; }
    long total = vmgpu_total_mb(R);
    char path[1024];
    snprintf(path, sizeof path, "%s" VMGPU_MEMLOCK_CONF, R);
    if (!mb) {
        if (unlink(path) && errno != ENOENT) { admin_err("gpu.writeFailed", "name", VMGPU_MEMLOCK_CONF, NULL); return; }
        admin_ok("gpu.memlockSet", NULL);
        return;
    }
    if (total <= 0 || mb < VMGPU_MEMLOCK_MIN_MB || mb > total - VMGPU_MEMLOCK_MIN_MB) { admin_err("gpu.memlockRange", NULL); return; }
    char d[1024], b[48];
    snprintf(d, sizeof d, "%s/etc/lordos", R);
    if (mkdir(d, 0755) && errno != EEXIST) { admin_err("gpu.writeFailed", "name", VMGPU_MEMLOCK_CONF, NULL); return; }
    snprintf(b, sizeof b, "cap_mb=%ld\n", mb);
    if (write_file(path, b)) { admin_err("gpu.writeFailed", "name", VMGPU_MEMLOCK_CONF, NULL); return; }
    admin_ok("gpu.memlockSet", NULL);
}
