/* js_vm.c — lord.vm: the Machines app (docs/VIRTUALIZATION.md 3.1 and "Phase 1 implementation"). lord-server plays
 * the part server/tests/vm_dev.c plays in the tests: it starts lord-vm on a control socketpair (fd 3), asks it for
 * the display connection, starts lord-vmdisplay with it (fd 3 its channel, fd 4 the D-Bus descriptor), gives the
 * helper a sealed picture memfd that the engine shows as a media node, acknowledges frames once they were painted
 * and forwards input. Nothing here blocks: each poll takes at most VD_POLL_MAX messages per channel.
 *
 *   host() → { arch, cpus, ramMb, kvmDevice, kvm, vcpusRecommended, memMaxMb, missing: { amd64, arm64 }, ready }
 *   list() → [{ dir, folder, name, id, arch, accel, vcpus, memoryMb, diskGb, firmware, network, iso, state, reason,
 *              code, detail, since, error }]   the machines in ~/Machines (<name>.lordvm)
 *   create({ name, arch, vcpus, memoryMb, diskGb, import | iso }) → job id (0: refused);  job(id) → { done, ok, out, dir }
 *   start(dir) → session id · attach(dir) → session id (a machine already running, through its runtime socket)
 *   poll(id) → { state, reason, code, detail, vcpus, memoryMb, sandbox, cgroup, display, w, h, media, ended, exit,
 *               lastError, frames }
 *   cmd(id, 'shutdown'|'poweroff'|'pause'|'resume'|'reset'|'display') · stop(dir, how) for a machine with no session
 *   key(id, evdev, down) · motion(id, x, y) · button(id, b, down) · wheel(id, dy, dx) · release(id)
 *   resize(id, w, h) · rate(id, hz) · close(id)
 *   log(dir) → the end of the machine's lord-vm.log (what failed)
 *   cdrom(id, path | null) → a disc into (or out of) a running machine's drive; poll's cd / cdMessage say how it went */
#include "js_vm.h"
#include "../media/media_int.h"
#include "../vm/vmproto.h"
#include "../vm/machine.h"
#include "../vm/vm_admin.h"
#include "../vm/vmthumb.h"
#include "../vm/vmops.h"
#include "../vm/logclean.h"
#include "../sec/json.h"
#include "ui.h"

#include <crypt.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

extern char **environ;

static const char *const STATES[VS_COUNT] = { "stopped", "starting", "running", "paused", "stopping", "crashed" };
static const char *const REASONS[VR_COUNT] = {
    "none", "guest-shutdown", "host-shutdown", "forced", "poweroff", "qemu-failed", "qemu-signal", "out-of-memory",
    "start-failed", "guest-panic", "supervisor-lost", "reset", "user", "suspended", "resume-failed",
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

/* a machine folder the app may act on: a real folder <name>.lordvm directly in ~/Machines */
static int machine_dir(JSContext *c, JSValueConst v, char *out, size_t n) {
    const char *s = JS_ToCString(c, v);
    if (!s) return -1;
    char lib[PATH_MAX], real[PATH_MAX];
    struct stat st;
    int ok = !library(lib, sizeof lib, 0) && !lstat(s, &st) && S_ISDIR(st.st_mode) && realpath(s, real);
    JS_FreeCString(c, s);
    if (!ok) return -1;
    size_t ll = strlen(lib), rl = strlen(real);
    if (strncmp(real, lib, ll) || real[ll] != '/' || strchr(real + ll + 1, '/') || rl < ll + 8 || strcmp(real + rl - 7, ".lordvm") || rl >= n) return -1;
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
    return o;
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

static JSValue f_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    reap_orphans();
    JSValue arr = JS_NewArray(c);
    char lib[PATH_MAX];
    if (library(lib, sizeof lib, 0)) return arr;
    DIR *d = opendir(lib);
    if (!d) return arr;
    uint32_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 8 || strcmp(e->d_name + l - 7, ".lordvm")) continue;
        char dir[PATH_MAX + 260];
        snprintf(dir, sizeof dir, "%s/%s", lib, e->d_name);
        struct stat st;
        if (lstat(dir, &st) || !S_ISDIR(st.st_mode)) continue;
        char mj[PATH_MAX + 300];
        snprintf(mj, sizeof mj, "%s/machine.json", dir);
        if (access(mj, F_OK)) continue;          /* being created (lord-vm --create writes it last) */
        JSValue o = JS_NewObject(c);
        set_str(c, o, "dir", dir);
        char folder[260];
        snprintf(folder, sizeof folder, "%.*s", (int)(l - 7), e->d_name);
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
            set_str(c, o, "network", m.network == VM_NET_SHARED ? "shared" : "isolated");
            set_str(c, o, "display", m.display == VM_DISPLAY_WINDOW ? "window" : "none");
            set_str(c, o, "iso", m.iso);
            set_str(c, o, "base", m.base);
            set_str(c, o, "mac", m.mac);
            set_str(c, o, "created", m.created);
            set_str(c, o, "error", "");
        } else {
            set_str(c, o, "name", folder);
            set_str(c, o, "error", r == VM_E_NEWER ? "newer" : "invalid");
        }
        run_state(dir, c, o);
        /* a saved session (6.1): the next start resumes from it */
        char sp[PATH_MAX + 300];
        snprintf(sp, sizeof sp, "%s/suspend.state", dir);
        struct stat ss;
        JS_SetPropertyStr(c, o, "suspended", JS_NewBool(c, !lstat(sp, &ss) && S_ISREG(ss.st_mode)));
        JS_SetPropertyStr(c, o, "stateBytes", JS_NewFloat64(c, !lstat(sp, &ss) && S_ISREG(ss.st_mode) ? (double)ss.st_blocks * 512 : 0));
        JS_SetPropertyUint32(c, arr, n++, o);
    }
    closedir(d);
    return arr;
}

/* ---- creating (lord-vm --create, in the background) ---- */

#define MAX_JOBS 4
typedef struct { int used, fd, done, ok, exit; pid_t pid; char out[2048]; size_t n; char dir[PATH_MAX + 80]; char hash[160]; char creds[PATH_MAX]; } Job;
static Job jobs[MAX_JOBS];

/* the folder's name from the machine's: letters, digits, space, '-', '_', '.'; others become '-' */
static void folder_name(const char *name, char *out, size_t n) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && o + 1 < n && o < 48; p++) {
        int ok = (*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'z') || *p == ' ' || *p == '-' || *p == '_' || (*p == '.' && o);
        if (*p >= 0x80) { while ((p[1] & 0xc0) == 0x80) p++; ok = 0; }
        char ch = ok ? (char)*p : '-';
        if (ch == '-' && o && out[o - 1] == '-') continue;
        out[o++] = ch;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '-' || out[o - 1] == '.')) o--;
    out[o] = 0;
    if (!o) snprintf(out, n, "Machine");
}

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
    int isolated = 0;
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
        const char *pw = JS_ToCString(c, vp);
        if (pw && pw[0]) {
            char salt[64];
            struct crypt_data cd;
            memset(&cd, 0, sizeof cd);
            const char *h = strlen(pw) < 256 && crypt_gensalt_rn("$6$", 0, NULL, 0, salt, sizeof salt) ? crypt_r(pw, salt, &cd) : NULL;
            if (h && h[0] == '$' && strlen(h) < sizeof hash) snprintf(hash, sizeof hash, "%s", h); else ok = 0;
            explicit_bzero(&cd, sizeof cd);
        }
        if (pw) { explicit_bzero((char *)pw, strlen(pw)); JS_FreeCString(c, pw); }
    }
    JS_FreeValue(c, vp);
    int id = 0;
    if (ok) {
        char folder[64], dir[PATH_MAX + 80];
        folder_name(name, folder, sizeof folder);
        snprintf(dir, sizeof dir, "%s/%s.lordvm", lib, folder);
        for (int k = 2; !access(dir, F_OK) && k < 100; k++) snprintf(dir, sizeof dir, "%s/%s %d.lordvm", lib, folder, k);
        int native = (!strcmp(arch, "amd64") ? VM_ARCH_AMD64 : VM_ARCH_ARM64) == host_arch();
        char sv[16], sm[16], sd[16];
        snprintf(sv, sizeof sv, "%d", vcpus); snprintf(sm, sizeof sm, "%d", mem); snprintf(sd, sizeof sd, "%d", disk);
        char *args[] = { "lord-vm", "--create", dir, "--name", (char *)name, "--arch", (char *)arch, "--accel", native ? "kvm" : "tcg",
                         "--vcpus", sv, "--memory", sm, "--disk", sd, "--network", isolated ? "isolated" : "shared", has_imp ? "--import" : "--iso", has_imp ? imp : iso, NULL };
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

/* set(dir, { name, vcpus, memoryMb, diskGb, network, iso }) → job id: lord-vm --set on a machine that is off (Configure) */
static JSValue f_set(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char dir[PATH_MAX];
    if (argc < 2 || !JS_IsObject(argv[1]) || machine_dir(c, argv[0], dir, sizeof dir)) return JS_NewInt32(c, 0);
    char *args[24], sv[16], sm[16], sd[16], name[VM_NAME_MAX + 8] = "", net[16] = "", iso[PATH_MAX] = "";
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
        if (s && (!strcmp(s, "shared") || !strcmp(s, "isolated"))) { snprintf(net, sizeof net, "%s", s); args[n++] = "--network"; args[n++] = net; } else ok = 0;
        if (s) JS_FreeCString(c, s);
    }
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
 * differences), diskBytes (what the machine's disks use on this computer), or only diskBytes when it's off */
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
    snprintf(p, sizeof p, "%s/run.json", dir);
    char buf[2048];
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    long qpid = 0;
    if (n > 0) {
        JDoc d;
        if (!json_parse(buf, (size_t)n, &d, NULL, 0)) {
            const char *s = json_str(d.root, "state");
            if (s && (!strcmp(s, "running") || !strcmp(s, "paused"))) qpid = (long)json_num(d.root, "qemu_pid", 0);
            json_free(&d);
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

static JSValue f_job(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    int i = ival(c, argv[0]);
    if (i < 1 || i > MAX_JOBS || !jobs[i - 1].used) return JS_NULL;
    Job *j = &jobs[i - 1];
    if (!j->done) {
        char tmp[1024];
        ssize_t r;
        while ((r = read(j->fd, tmp, sizeof tmp)) > 0) {
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
    int cur_w, cur_h, cur_hx, cur_hy;
    int job_kind, job_status, job_pct;  /* the last VM_JOB (suspend, resume, snapshots) */
    char job_text[VM_TEXT_MAX];
    uint32_t cd_seq;                    /* VM_CDROM: the request, and how it went (0 none, 1 waiting, 2 done, 3 failed) */
    int cd_state;
    char cd_msg[VM_TEXT_MAX];
    int rec;                            /* a recording of the picture (6.20): its media source, fed as frames come */
    double rec_fed;
    int notice;                         /* VM_NOTICE: VMN_*, and its words */
    char notice_text[VM_TEXT_MAX];
} Sess;
static Sess sess[MAX_SESS];

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

static int ask(Sess *s, uint16_t type, int a) {
    if (s->vm < 0) return -1;
    VmMsg m = vm_msg(type, ++s->seq, a, 0, 0, 0, NULL);
    return vm_send(s->vm, &m, -1) ? -1 : (int)s->seq;
}
static void vdsend(Sess *s, int type, int a, int b) { if (s->vd >= 0) { VdMsg m = vd_msg((uint16_t)type, a, b, 0, 0, 0, 0); vd_send(s->vd, &m, -1); } }

static void init_sess(Sess *s, const char *dir) {
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->vm = s->vd = s->err = -1;
    s->state = VS_STARTING;
    s->vd_state = -1;
    snprintf(s->dir, sizeof s->dir, "%s", dir);
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
    int sv[2], ep[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return JS_NewInt32(c, 0);
    if (pipe2(ep, O_CLOEXEC)) { close(sv[0]); close(sv[1]); return JS_NewInt32(c, 0); }
    /* lord-vm writes to stderr with nobody waiting: never let a full pipe stop the supervisor */
    fcntl(ep[1], F_SETFL, O_NONBLOCK);
    fcntl(ep[0], F_SETFL, O_NONBLOCK);
    char creds[PATH_MAX];
    int have_creds = !write_creds(creds, sizeof creds, dir);
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
        char *args[16];
        int k = 0;
        args[k++] = "lord-vm"; args[k++] = "--start"; args[k++] = dir; args[k++] = "--control-fd"; args[k++] = "3"; args[k++] = "--paused";
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
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
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

static void drop_display(Sess *s, int close_helper) {
    if (s->vd >= 0) { if (close_helper) vdsend(s, VD_CLOSE, 0, 0); close(s->vd); s->vd = -1; }
    if (s->vd_pid > 0) {
        if (waitpid(s->vd_pid, NULL, WNOHANG) == 0) { kill(s->vd_pid, SIGKILL); waitpid(s->vd_pid, NULL, 0); }
        s->vd_pid = 0;
    }
    s->vd_up = 0;
    s->ack_pending = 0;
    /* the next helper gets the guest's pointer again from QEMU; until then, the host's */
    free(s->cur); s->cur = NULL;
    if (s->media) media_src_cursor(s->media, NULL, 0, 0, 0, 0);
}

static void drop_creds(Sess *s) { if (s->creds[0]) { unlink(s->creds); s->creds[0] = 0; } }

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
        case VM_JOB:
            s->job_kind = m.a; s->job_status = m.b; s->job_pct = m.c;
            snprintf(s->job_text, sizeof s->job_text, "%s", m.text);
            break;
        case VM_NOTICE:
            s->notice = m.a;
            snprintf(s->notice_text, sizeof s->notice_text, "%s", m.text);
            break;
        case VM_REPLY:
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
    uint8_t *map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return; }
    MediaSrc *m = s->media ? media_src_get(s->media) : NULL;
    if (!m) m = media_src_new(SK_IMAGE, "machines");
    if (!m) { munmap(map, len); close(fd); return; }
    if (m->map) munmap(m->map, m->map_len);
    free(m->cache); m->cache = NULL; m->cache_w = m->cache_h = 0;
    m->map = map; m->map_len = len; m->px = (uint32_t *)map; m->w = w; m->h = h; m->opaque = 1; m->xrgb = 1; m->frame_seq++;
    if (!s->media && s->cur) media_src_cursor(m->id, s->cur, s->cur_w, s->cur_h, s->cur_hx, s->cur_hy);
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
    free(s->cur);
    s->cur = copy; s->cur_w = w; s->cur_h = h; s->cur_hx = hx; s->cur_hy = hy;
    if (s->media) media_src_cursor(s->media, copy, w, h, hx, hy);
}

static void from_vd(Sess *s) {
    int frames = 0, last = 0;
    for (int k = 0; k < VD_POLL_MAX && s->vd >= 0; k++) {
        VdMsg m;
        int fd = -1;
        int r = vd_recv(s->vd, &m, &fd);
        if (r < 0) { drop_display(s, 0); s->vd_state = VDS_LOST; break; }
        if (!r) break;
        if (!vd_msg_valid(&m, 0)) { if (fd >= 0) close(fd); continue; }
        switch (m.type) {
        case VD_READY: s->vd_up = 1; break;
        case VD_SIZE: set_picture(s, m.a, m.b); break;
        case VD_FRAME:
            if (!s->media || !vd_rect_inside(&m, s->w, s->h)) break;
            frames++; last = m.e;
            break;
        case VD_CURSOR:
            /* the guest's pointer: shown instead of the host's over the picture (main.c, media_node_cursor) */
            if (fd >= 0 && !vm_cursor_check(fd, m.a, m.b)) guest_cursor(s, fd, m.a, m.b, m.c, m.d);
            break;
        case VD_POINTER:
            /* where the guest puts it doesn't matter (the host's pointer drives an absolute tablet); whether it shows does */
            if (s->media) media_src_cursor_visible(s->media, m.c != 0);
            break;
        case VD_STATE: s->vd_state = m.a; break;
        default: break;
        }
        if (fd >= 0) close(fd);
    }
    MediaSrc *ms = s->media ? media_src_get(s->media) : NULL;
    if (frames && ms) {
        ms->frame_seq++;
        media_src_damage(ms);
        /* the node it was drawn in went away (the window's tree was made again): one repaint of everything finds
         * the new node, and that paint binds it */
        if (!ms->node) damage_add((Box){ 0, 0, 16384, 16384 });
        s->frames += frames;
        s->ack_pending = 1; s->ack_frame = last; s->ack_seq = ms->frame_seq;
    }
    /* the recording takes the picture as it changes, and at least once a second while it doesn't */
    if (s->rec && ms && ms->px) {
        MediaSrc *r = media_src_get(s->rec);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
        if (!r || r->kind != SK_SCREENREC) s->rec = 0;
        else if ((frames || now - s->rec_fed > 1000) && media_framerec_feed(r, ms->px, ms->w, ms->h)) s->rec_fed = now;
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
    ack(s);
    from_vm(s);
    if (s->vd >= 0) from_vd(s);
    ack(s);
    read_err(s);
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
    set_int(c, o, "cd", s->cd_state);
    set_str(c, o, "cdMessage", s->cd_msg);
    set_str(c, o, "notice", s->notice == VMN_NET_LOST ? "network-lost" : s->notice == VMN_RECOVERED ? "recovered" : "");
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
    return o;
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
static JSValue f_release(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; Sess *s = get(c, argv[0]); if (s) vdsend(s, VD_RELEASE_ALL, 0, 0); return JS_UNDEFINED; }
static JSValue f_resize(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; Sess *s = argc >= 3 ? get(c, argv[0]) : NULL;
    int w = s ? ival(c, argv[1]) : 0, h = s ? ival(c, argv[2]) : 0;
    if (s && w >= 320 && h >= 200 && w <= (int)VD_MAX_DIM && h <= (int)VD_MAX_DIM) vdsend(s, VD_RESIZE, w, h);
    return JS_UNDEFINED;
}
static JSValue f_rate(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; Sess *s = argc >= 2 ? get(c, argv[0]) : NULL;
    int hz = s ? ival(c, argv[1]) : 0;
    if (s && hz >= 1 && hz <= 240) vdsend(s, VD_RATE, hz, 0);
    return JS_UNDEFINED;
}

/* the window let go of the machine: a machine this session started is shut down by lord-vm when its control
 * channel closes (ACPI, then off after 60 s); an attached one keeps running */
static JSValue f_close(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    if (!s) return JS_UNDEFINED;
    if (s->rec) { MediaSrc *r = media_src_get(s->rec); if (r) media_src_close(r); s->rec = 0; }     /* finished properly, "done" follows */
    drop_display(s, 1);
    drop_creds(s);
    if (s->vm >= 0) close(s->vm);
    if (s->err >= 0) close(s->err);
    /* lord-vm finishes on its own (up to a minute for an ACPI shutdown): reaped later */
    if (s->vm_pid > 0 && waitpid(s->vm_pid, NULL, WNOHANG) == 0) later(s->vm_pid);
    MediaSrc *m = s->media ? media_src_get(s->media) : NULL;
    if (m) media_src_close(m);
    free(s->cur);
    memset(s, 0, sizeof *s);
    return JS_UNDEFINED;
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
    int serial = which && !strcmp(which, "serial");
    if (which) JS_FreeCString(c, which);
    char p[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/log/%s", dir, serial ? "serial.log" : "lord-vm.log");
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
    if (!m || !m->px || m->w < 1 || m->h < 1) return JS_FALSE;
    return JS_NewBool(c, !vm_thumb_save(s->dir, m->px, m->w, m->h, VM_THUMB_W));
}

/* screenshot(session) → the file's path, or "": the guest's picture at its size, in ~/Pictures/Screenshots as
 * "<machine> <date> <time>.png" */
static JSValue f_screenshot(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    MediaSrc *m = s && s->media ? media_src_get(s->media) : NULL;
    const char *home = getenv("HOME");
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

/* record(session) → a media id (0: couldn't): the guest's picture at its size into ~/Pictures/Screenshots as
 * "<machine> <date> <time>.mp4", the way the Screenshots app records (lord-media, hardware encoding where there is
 * some). No screen-capture permission: it's the machine's own picture, never the screen, so protected windows can't be
 * in it. Stopped with lord.media.stop(id) (its "done" event names the file), or when the window closes. */
static JSValue f_record(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    Sess *s = get(c, argv[0]);
    MediaSrc *m = s && s->media ? media_src_get(s->media) : NULL;
    const char *home = getenv("HOME");
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
    int fds[3] = { fd, -1, render };
    r->sock = media_spawn(MK_SCREENREC, params, fds, 3, &r->pid);
    close(fd);
    if (render >= 0) close(render);
    if (r->sock < 0 || media_framerec_start(r, W, H, 30)) { media_src_close(r); return JS_NewInt32(c, 0); }
    s->rec = r->id;
    s->rec_fed = 0;
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
    if (machine_dir(c, argv[0], dir, sizeof dir)) return JS_NewInt32(c, 0);
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
    if (m && m->px && m->w > 0 && m->h > 0) vm_png_save(sd, file, m->px, m->w, m->h, 640);
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

/* clone(dir, { name, duplicate, linked, fromSnapshot }) → job id (lord-vm --clone; its out is the new machine's JSON) */
static JSValue f_clone(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char dir[PATH_MAX], lib[PATH_MAX];
    if (argc < 2 || !JS_IsObject(argv[1]) || machine_dir(c, argv[0], dir, sizeof dir) || library(lib, sizeof lib, 0)) return JS_NewInt32(c, 0);
    JSValue vn = JS_GetPropertyStr(c, argv[1], "name"), vs = JS_GetPropertyStr(c, argv[1], "fromSnapshot");
    const char *name = JS_ToCString(c, vn), *snap = JS_IsString(vs) ? JS_ToCString(c, vs) : NULL;
    int dup = opt_bool(c, argc, argv, "duplicate"), linked = opt_bool(c, argc, argv, "linked");
    int id = 0;
    char nm[VM_NAME_MAX + 1] = "", tg[40] = "", folder[64], dst[PATH_MAX + 80];
    if (name && vm_name_valid(name) && (!snap || vm_snap_tag_valid(snap))) {
        snprintf(nm, sizeof nm, "%s", name);
        if (snap) snprintf(tg, sizeof tg, "%s", snap);
        folder_name(nm, folder, sizeof folder);
        snprintf(dst, sizeof dst, "%s/%s.lordvm", lib, folder);
        for (int k = 2; !access(dst, F_OK) && k < 100; k++) snprintf(dst, sizeof dst, "%s/%s %d.lordvm", lib, folder, k);
        char *args[12];
        int k = 0;
        args[k++] = "lord-vm"; args[k++] = "--clone"; args[k++] = dir; args[k++] = dst; args[k++] = "--name"; args[k++] = nm;
        if (dup) args[k++] = "--duplicate";
        if (linked) args[k++] = "--linked";
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

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("host", 0, f_host), JS_CFUNC_DEF("list", 0, f_list), JS_CFUNC_DEF("create", 1, f_create), JS_CFUNC_DEF("job", 1, f_job),
    JS_CFUNC_DEF("start", 1, f_start), JS_CFUNC_DEF("attach", 1, f_attach), JS_CFUNC_DEF("poll", 1, f_poll), JS_CFUNC_DEF("cmd", 2, f_cmd),
    JS_CFUNC_DEF("stop", 2, f_stop), JS_CFUNC_DEF("key", 3, f_key), JS_CFUNC_DEF("motion", 3, f_motion), JS_CFUNC_DEF("button", 3, f_button),
    JS_CFUNC_DEF("wheel", 3, f_wheel), JS_CFUNC_DEF("release", 1, f_release), JS_CFUNC_DEF("resize", 3, f_resize), JS_CFUNC_DEF("rate", 2, f_rate),
    JS_CFUNC_DEF("close", 1, f_close), JS_CFUNC_DEF("log", 3, f_log), JS_CFUNC_DEF("eject", 1, f_eject),
    JS_CFUNC_DEF("set", 2, f_set), JS_CFUNC_DEF("stats", 1, f_stats), JS_CFUNC_DEF("snapshot", 1, f_snapshot),
    JS_CFUNC_DEF("isoInfo", 1, f_isoinfo), JS_CFUNC_DEF("screenshot", 1, f_screenshot), JS_CFUNC_DEF("cdrom", 2, f_cdrom),
    JS_CFUNC_DEF("snaps", 1, f_snaps), JS_CFUNC_DEF("snapTake", 3, f_snap_take), JS_CFUNC_DEF("snapRevert", 2, f_snap_revert),
    JS_CFUNC_DEF("snapDelete", 2, f_snap_delete),
    JS_CFUNC_DEF("cloneInfo", 1, f_clone_info), JS_CFUNC_DEF("clone", 2, f_clone), JS_CFUNC_DEF("compact", 2, f_compact),
    JS_CFUNC_DEF("storage", 0, f_storage), JS_CFUNC_DEF("record", 1, f_record), JS_CFUNC_DEF("removeBase", 1, f_remove_base),
    JS_CFUNC_DEF("startDetached", 1, f_start_detached), JS_CFUNC_DEF("discardState", 1, f_discard), JS_CFUNC_DEF("awake", 0, f_awake),
};

void js_vm_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "vm", o);
}
