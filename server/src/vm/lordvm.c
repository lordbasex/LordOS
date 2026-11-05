/* lordvm.c — lord-vm: the supervisor of one machine (docs/VIRTUALIZATION.md 3.1, 3.3, 6.1 and "Phase 1
 * implementation"). One process per running machine. It reads machine.json, places itself in the machine's
 * cgroup scope, opens /dev/kvm, starts passt and QEMU in their sandboxes, holds QEMU's control socket (QMP),
 * reports state, and exits when QEMU has exited. It draws nothing: the picture is lord-vmdisplay's.
 *
 *   lord-vm --create DIR [--name N] [--arch arm64|amd64] [--accel kvm|tcg] [--vcpus N] [--memory MB]
 *                        [--disk GB] [--firmware uefi|bios] [--display window|none] [--network shared|isolated]
 *                        [--import IMAGE] [--iso FILE]
 *   lord-vm --start DIR [--paused] [--detach] [--control-fd N] [--keep-running] [--dev-qmp]
 *                       [--credentials-file FILE]
 *     FILE: systemd credentials for the guest's first boot, one per line, NAME=VALUE or binary:NAME=BASE64.
 *     They may be secrets (a root password), so they never go on a command line or into the log: lord-vm reads
 *     the file, puts each credential in a memfd and QEMU reads it as -smbios type=11,path=/proc/self/fd/N.
 *     The log names them only.
 *   lord-vm --status DIR [--json]      --check DIR
 *   lord-vm --stop DIR [--timeout S]   --poweroff DIR   --pause DIR   --resume DIR   --reset DIR
 *   lord-vm --qmp DIR 'JSON'           (only a machine started with --dev-qmp)
 *
 * Exit status: 0 ok (or the machine stopped cleanly), 1 failed or crashed, 2 usage, 3 already running / not
 * running. */
#include "machine.h"
#include "qmp.h"
#include "vmproto.h"
#include "vmsandbox.h"
#include "vmops.h"
#include "vmagent.h"
#include "../sec/cgroup.h"
#include "../sec/json.h"
#include "../doc/wire.h"
#include "../pkg/crypto.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef VM_LOGIND
#include <systemd/sd-bus.h>
#endif

#define QEMU_IMG "/usr/bin/qemu-img"
#define PASST    "/usr/bin/passt"
#define VM_DNS_ADDR "169.254.0.53"     /* the guest's nameserver in Shared mode (passt answers for it) */
static const char *const QEMU_BIN[2] = { "/usr/bin/qemu-system-aarch64", "/usr/bin/qemu-system-x86_64" };

/* UEFI code and variable templates, by architecture, first found wins */
static const char *const FW_ARM64[][2] = {
    { "/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd", "/usr/share/AAVMF/AAVMF_VARS.fd" },
    { "/usr/share/AAVMF/AAVMF_CODE.fd", "/usr/share/AAVMF/AAVMF_VARS.fd" },
};
static const char *const FW_AMD64[][2] = {
    { "/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd" },
};

#define LOG_CAP      (4 << 20)
#define SERIAL_CAP   (1 << 20)
#define MAX_CLIENTS  8
#define MAX_PENDING  64
#define MAX_CREDS    16

static const char *const STATE_NAME[VS_COUNT] = { "stopped", "starting", "running", "paused", "stopping", "crashed" };
static const char *const REASON_NAME[VR_COUNT] = {
    "none", "guest-shutdown", "host-shutdown", "forced", "poweroff", "qemu-failed", "qemu-signal", "out-of-memory",
    "start-failed", "guest-panic", "supervisor-lost", "reset", "user", "suspended", "resume-failed",
};

/* ---- log ---- */

static int log_fd = -1, log_stderr = 1;
static off_t log_bytes;

static void vlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void vlog(const char *fmt, ...) {
    char line[1024];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    int n = (int)strftime(line, sizeof line, "%Y-%m-%dT%H:%M:%S", &tm);
    n += snprintf(line + n, sizeof line - (size_t)n, ".%03ldZ ", ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    va_end(ap);
    n += k < 0 ? 0 : k >= (int)(sizeof line - (size_t)n - 1) ? (int)(sizeof line - (size_t)n - 2) : k;
    line[n++] = '\n';
    if (log_fd >= 0 && log_bytes < LOG_CAP) { if (write(log_fd, line, (size_t)n) > 0) log_bytes += n; }
    if (log_stderr) { ssize_t w = write(2, line, (size_t)n); (void)w; }
}

/* raw bytes from a child's stderr into the log, line by line */
static void log_child(const char *who, const char *buf, size_t n) {
    while (n) {
        const char *nl = memchr(buf, '\n', n);
        size_t l = nl ? (size_t)(nl - buf) : n;
        if (l) vlog("%s: %.*s", who, (int)(l > 900 ? 900 : l), buf);
        if (!nl) break;
        buf = nl + 1;
        n -= l + 1;
    }
}

/* ---- small utilities ---- */

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int write_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        c += w; n -= (size_t)w;
    }
    return 0;
}

static int file_exists(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }

static int sha256_file(const char *path, char out[65]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    Sha256 s;
    sha256_init(&s);
    static uint8_t buf[1 << 16];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        if (!r) break;
        sha256_update(&s, buf, (size_t)r);
    }
    close(fd);
    uint8_t d[32];
    sha256_final(&s, d);
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
    return 0;
}

/* runs a program (no shell) and waits; its output goes to the log. 0 when it exited with 0. With `jail_read`, the
 * program runs inside vm_jail_tool (it may read only that file and write only in `jail_rw`) or not at all. */
static int run_tool_in(char *const argv[], char *out, size_t outcap, const char *jail_read, const char *jail_rw);
static int run_tool(char *const argv[], char *out, size_t outcap) { return run_tool_in(argv, out, outcap, NULL, NULL); }
static const char *jail_read2;          /* a second file the next jailed tool may read (a linked clone's base) */
static int run_tool_in(char *const argv[], char *out, size_t outcap, const char *jail_read, const char *jail_rw) {
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (!pid) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);        /* a qemu-img never outlives the lord-vm that runs it (a killed clone) */
        dup2(p[1], 1);
        dup2(p[1], 2);
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) dup2(null, 0);
        syscall(SYS_close_range, 3, ~0u, 0);
        if (jail_read) {
            int mask = vm_jail_tool2(jail_read, jail_read2, jail_rw);
            if (!(mask & VMS_NO_NEW_PRIVS) || !(mask & VMS_SECCOMP_OURS) || (vm_landlock_abi() >= 1 && !(mask & VMS_LANDLOCK_FS))) {
                static const char e[] = "lord-vm: the image tool's sandbox couldn't be set up\n";
                write_all(2, e, sizeof e - 1);
                _exit(126);
            }
        }
        char *env[] = { "LC_ALL=C", NULL };
        execve(argv[0], argv, env);
        _exit(127);
    }
    close(p[1]);
    size_t n = 0;
    char tmp[4096];
    for (;;) {
        ssize_t r = read(p[0], tmp, sizeof tmp);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        if (out && n + 1 < outcap) { size_t k = (size_t)r < outcap - 1 - n ? (size_t)r : outcap - 1 - n; memcpy(out + n, tmp, k); n += k; }
    }
    if (out && outcap) out[n] = 0;
    close(p[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* a folder lord-vm accepts: absolute, real, with no comma or control character (QEMU's options) */
static int dir_ok(const char *d) {
    if (d[0] != '/' || strlen(d) > VM_PATH_MAX - 64) return 0;
    for (const unsigned char *p = (const unsigned char *)d; *p; p++) if (*p < 0x20 || *p == 0x7f || *p == ',') return 0;
    return 1;
}

static const char *runtime_base(char *buf, size_t n) {
    const char *x = getenv("XDG_RUNTIME_DIR");
    struct stat st;
    if (x && x[0] == '/' && !stat(x, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid()) snprintf(buf, n, "%s/lordos-vm", x);
    else {
        char d[64];
        snprintf(d, sizeof d, "/run/user/%u", (unsigned)getuid());
        if (stat(d, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) return NULL;
        snprintf(buf, n, "%s/lordos-vm", d);
    }
    return buf;
}

static int socket_path(const VmMachine *m, char *out, size_t n, char *rundir, size_t rn) {
    char base[PATH_MAX];
    if (!runtime_base(base, sizeof base)) return -1;
    if (rundir) snprintf(rundir, rn, "%s", base);
    if ((size_t)snprintf(out, n, "%s/%s.sock", base, m->id) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return -1;
    return 0;
}

/* ---- run.json ---- */

typedef struct { int state, reason, code; long qemu_pid, passt_pid, pid; char since[21], detail[160]; int agent; } RunInfo;

static void runinfo_write(const char *dir, const RunInfo *r) {
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"state\":\"%s\",\"reason\":\"%s\",\"code\":%d,\"pid\":%ld,\"qemu_pid\":%ld,\"passt_pid\":%ld,\"since\":",
              STATE_NAME[r->state], REASON_NAME[r->reason], r->code, r->pid, r->qemu_pid, r->passt_pid);
    jb_str(&b, r->since);
    jb_raw(&b, ",\"detail\":"); jb_str(&b, r->detail);
    jb_printf(&b, ",\"agent\":\"%s\"", r->agent == VAS_CONNECTED ? "connected" : r->agent == VAS_WAITING ? "waiting" : "off");
    jb_raw(&b, "}\n");
    if (b.oom) { jb_free(&b); return; }
    char tmp[PATH_MAX + 32], path[PATH_MAX + 32];
    snprintf(tmp, sizeof tmp, "%s/.run.json.tmp", dir);
    snprintf(path, sizeof path, "%s/run.json", dir);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        int ok = !write_all(fd, b.p, b.n);
        close(fd);
        if (!ok || rename(tmp, path)) unlink(tmp);
    }
    jb_free(&b);
}

static int name_index(const char *const *names, int n, const char *s) {
    for (int i = 0; s && i < n; i++) if (!strcmp(names[i], s)) return i;
    return -1;
}

static int runinfo_read(const char *dir, RunInfo *r) {
    memset(r, 0, sizeof *r);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/run.json", dir);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    JDoc d;
    if (json_parse(buf, (size_t)n, &d, NULL, 0)) return -1;
    int s = name_index(STATE_NAME, VS_COUNT, json_str(d.root, "state")), w = name_index(REASON_NAME, VR_COUNT, json_str(d.root, "reason"));
    r->state = s < 0 ? VS_STOPPED : s;
    r->reason = w < 0 ? VR_NONE : w;
    r->code = (int)json_num(d.root, "code", 0);
    r->pid = (long)json_num(d.root, "pid", 0);
    r->qemu_pid = (long)json_num(d.root, "qemu_pid", 0);
    r->passt_pid = (long)json_num(d.root, "passt_pid", 0);
    const char *since = json_str(d.root, "since"), *det = json_str(d.root, "detail");
    if (since) snprintf(r->since, sizeof r->since, "%s", since);
    if (det) snprintf(r->detail, sizeof r->detail, "%s", det);
    json_free(&d);
    return 0;
}

static int state_active(int s) { return s == VS_STARTING || s == VS_RUNNING || s == VS_PAUSED || s == VS_STOPPING; }

/* ---- create ---- */

static int machine_type_for(int arch, char *out, size_t n) {
    char *argv[] = { (char *)QEMU_BIN[arch], "-machine", "help", NULL };
    static char buf[65536];
    if (run_tool(argv, buf, sizeof buf)) return -1;
    const char *alias = arch == VM_ARCH_ARM64 ? "virt " : "q35 ";
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!strncmp(line, alias, strlen(alias))) {
            char *a = strstr(line, "(alias of ");
            if (a) {
                a += 10;
                char *e = strchr(a, ')');
                if (e && (size_t)(e - a) < n) { memcpy(out, a, (size_t)(e - a)); out[e - a] = 0; return 0; }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    return -1;
}

static int firmware_for(int arch, const char **code, const char **vars) {
    if (arch == VM_ARCH_ARM64) {
        for (size_t i = 0; i < sizeof FW_ARM64 / sizeof FW_ARM64[0]; i++)
            if (file_exists(FW_ARM64[i][0]) && file_exists(FW_ARM64[i][1])) { *code = FW_ARM64[i][0]; *vars = FW_ARM64[i][1]; return 0; }
    } else {
        for (size_t i = 0; i < sizeof FW_AMD64 / sizeof FW_AMD64[0]; i++)
            if (file_exists(FW_AMD64[i][0]) && file_exists(FW_AMD64[i][1])) { *code = FW_AMD64[i][0]; *vars = FW_AMD64[i][1]; return 0; }
    }
    return -1;
}

static int parse_enum(const char *s, const char *a, const char *b) { return !strcmp(s, a) ? 0 : !strcmp(s, b) ? 1 : -1; }
static int parse_clip(const char *s) {
    return !strcmp(s, "off") ? VM_CLIP_OFF : !strcmp(s, "to-guest") ? VM_CLIP_TO_GUEST : !strcmp(s, "from-guest") ? VM_CLIP_FROM_GUEST : !strcmp(s, "both") ? VM_CLIP_BOTH : -1;
}

/* The image to import, as QEMU must read it: a json: filename that names the format (never probed) and, for qcow2,
 * no backing file. QEMU's option parser isn't involved, so no character of the path needs escaping. */
static char *image_spec(const char *real, int kind) {
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "json:{\"driver\":\"%s\",\"read-only\":true,\"file\":{\"driver\":\"file\",\"filename\":", kind == VM_IMG_QCOW2 ? "qcow2" : "raw");
    jb_str(&b, real);
    jb_raw(&b, kind == VM_IMG_QCOW2 ? "},\"backing\":null}" : "}}");
    return b.oom ? NULL : b.p;
}

/* Reads the first bytes of the image and decides its format (vm_image_kind), then asks qemu-img, jailed, for its
 * size with that format; refuses an image QEMU says has a backing file or an external data file anyway. */
static int image_check(const char *real, int *kind, int *gb, char *err, size_t errlen) {
    uint8_t head[VM_IMG_HEAD];
    int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) { if (fd >= 0) close(fd); snprintf(err, errlen, "isn't a file"); return -1; }
    ssize_t n = read(fd, head, sizeof head);
    close(fd);
    *kind = vm_image_kind(head, n > 0 ? (size_t)n : 0, real, err, errlen);
    if (*kind < 0) return -1;
    char *spec = image_spec(real, *kind);
    if (!spec) { snprintf(err, errlen, "out of memory"); return -1; }
    char *argv[] = { QEMU_IMG, "info", "--output=json", spec, NULL };
    static char buf[65536];
    int r = run_tool_in(argv, buf, sizeof buf, real, NULL);
    free(spec);
    if (r) { snprintf(err, errlen, "QEMU can't read it: %.180s", buf); return -1; }
    JDoc d;
    if (json_parse(buf, strlen(buf), &d, NULL, 0)) { snprintf(err, errlen, "qemu-img's answer isn't JSON"); return -1; }
    const char *fmt = json_str(d.root, "format");
    const JVal *fs = json_get(json_get(d.root, "format-specific"), "data");
    double vs = json_num(d.root, "virtual-size", 0);
    int bad = !fmt || strcmp(fmt, *kind == VM_IMG_QCOW2 ? "qcow2" : "raw") || json_get(d.root, "backing-filename") ||
              json_get(d.root, "full-backing-filename") || json_get(d.root, "backing-filename-format") || (fs && json_get(fs, "data-file"));
    json_free(&d);
    if (bad) { snprintf(err, errlen, "the image depends on another file: refused"); return -1; }
    if (vs <= 0) { snprintf(err, errlen, "the image has no size"); return -1; }
    *gb = (int)((vs + (1 << 30) - 1) / (1 << 30));
    return 0;
}

/* the system disk at each start: a qcow2 with no backing file and no external data file (vm_qcow2_plain) */
static int disk_plain(const char *path, char *err, size_t errlen) {
    uint8_t head[VM_IMG_HEAD];
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { snprintf(err, errlen, "can't open it: %s", strerror(errno)); return -1; }
    ssize_t n = read(fd, head, sizeof head);
    close(fd);
    return vm_qcow2_plain(head, n > 0 ? (size_t)n : 0, err, errlen);
}

/* ---- snapshots, offline (6.2): qemu-img snapshot on every writable disk, jailed like an import ---- */

/* the machine's writable disks, in snapshot order: the system disk, then the UEFI variables */
static int disk_files(const char *dir, const VmMachine *m, char out[2][PATH_MAX + 64]) {
    int n = 0;
    snprintf(out[n++], PATH_MAX + 64, "%s/disks/system.qcow2", dir);
    if (m->firmware == VM_FW_UEFI) snprintf(out[n++], PATH_MAX + 64, "%s/disks/nvram.qcow2", dir);
    return n;
}

/* qemu-img snapshot -c|-a|-d TAG on one disk, as json: with no backing file (a linked clone's base isn't read) */
static int qimg_snap(const char *dir, const char *disk, const char *flag, const char *tag, char *err, size_t errlen) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "json:{\"driver\":\"qcow2\",\"backing\":null,\"file\":{\"driver\":\"file\",\"filename\":");
    jb_str(&b, disk);
    jb_raw(&b, "}}");
    if (b.oom) { jb_free(&b); snprintf(err, errlen, "out of memory"); return -1; }
    char dd[PATH_MAX + 64];
    snprintf(dd, sizeof dd, "%s/disks", dir);
    char *argv[] = { QEMU_IMG, "snapshot", (char *)flag, (char *)tag, b.p, NULL };
    int r = run_tool_in(argv, err, errlen, disk, dd);
    jb_free(&b);
    return r;
}

/* the tags a disk holds (qemu-img info, jailed, read only): how many, into tags */
static int qimg_tags(const char *disk, char tags[][33], int max) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "json:{\"driver\":\"qcow2\",\"backing\":null,\"read-only\":true,\"file\":{\"driver\":\"file\",\"filename\":");
    jb_str(&b, disk);
    jb_raw(&b, "}}");
    if (b.oom) { jb_free(&b); return -1; }
    char *argv[] = { QEMU_IMG, "info", "--output=json", "-U", b.p, NULL };
    static char out[262144];
    int r = run_tool_in(argv, out, sizeof out, disk, NULL);
    jb_free(&b);
    if (r) return -1;
    JDoc d;
    if (json_parse(out, strlen(out), &d, NULL, 0)) return -1;
    int n = 0;
    const JVal *arr = json_get(d.root, "snapshots");
    for (const JVal *e = arr && arr->type == J_ARR ? arr->kid : NULL; e && n < max; e = e->next) {
        const char *t = json_str(e, "name");
        if (t && strlen(t) < 33) snprintf(tags[n++], 33, "%s", t);
    }
    json_free(&d);
    return n;
}

/* after an operation that stopped in the middle: clusters a killed QEMU or qemu-img allocated but never referenced
 * are given back (qemu-img check -r leaks: only leaks, never a guess at corruption). 0 clean or repaired. */
static int qimg_repair_leaks(const char *dir, const char *disk) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "json:{\"driver\":\"qcow2\",\"backing\":null,\"file\":{\"driver\":\"file\",\"filename\":");
    jb_str(&b, disk);
    jb_raw(&b, "}}");
    if (b.oom) { jb_free(&b); return -1; }
    char dd[PATH_MAX + 64], out[2048];
    snprintf(dd, sizeof dd, "%s/disks", dir);
    char *argv[] = { QEMU_IMG, "check", "-q", "-r", "leaks", b.p, NULL };
    int r = run_tool_in(argv, out, sizeof out, disk, dd);
    jb_free(&b);
    if (r) vlog("recovery: qemu-img check on %s: %.200s", strrchr(disk, '/') + 1, out);
    return r;
}

static void snap_picture_rm(const char *dir, const char *tag) {
    char p[PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/snapshots/%s.png", dir, tag);
    unlink(p);
}

/* a tag on the disks that snapshots.json doesn't list is what's left of a snapshot that didn't finish: removed at the
 * next open (6.2, "Failure midway"). Only tags of our own form are touched. */
static void snaps_sweep(const char *dir, const VmMachine *m) {
    char disks[2][PATH_MAX + 64];
    int nd = disk_files(dir, m, disks);
    VmSnap list[VM_SNAPS_MAX];
    int n = vm_snaps_load(dir, list, VM_SNAPS_MAX);
    for (int i = 0; i < nd; i++) {
        char tags[VM_SNAPS_MAX + 8][33];
        int k = qimg_tags(disks[i], tags, VM_SNAPS_MAX + 8);
        for (int t = 0; t < k; t++) {
            if (!vm_snap_tag_valid(tags[t]) || vm_snaps_find(list, n, tags[t]) >= 0) continue;
            char err[256];
            int r = qimg_snap(dir, disks[i], "-d", tags[t], err, sizeof err);
            vlog("snapshots: a leftover tag %s on %s %s", tags[t], strrchr(disks[i], '/') + 1, r ? "couldn't be removed" : "was removed");
        }
    }
}

static int snap_take_off(const char *dir, const VmMachine *m, const char *tag, const char *name, char *err, size_t errlen) {
    VmSnap list[VM_SNAPS_MAX];
    int n = vm_snaps_load(dir, list, VM_SNAPS_MAX);
    if (n >= VM_SNAPS_MAX) { snprintf(err, errlen, "a machine keeps at most %d snapshots", VM_SNAPS_MAX); return -1; }
    if (vm_snaps_find(list, n, tag) >= 0) { snprintf(err, errlen, "there is a snapshot %s already", tag); return -1; }
    char disks[2][PATH_MAX + 64];
    int nd = disk_files(dir, m, disks);
    if (vm_op_begin(dir, "snapshot-take", tag, NULL, NULL)) { snprintf(err, errlen, "can't write operation.json"); return -1; }
    for (int i = 0; i < nd; i++) {
        if (qimg_snap(dir, disks[i], "-c", tag, err, errlen)) {
            char e2[256];
            for (int k = 0; k < i; k++) qimg_snap(dir, disks[k], "-d", tag, e2, sizeof e2);
            vm_op_end(dir);
            return -1;
        }
    }
    VmSnap sn;
    memset(&sn, 0, sizeof sn);
    snprintf(sn.tag, sizeof sn.tag, "%s", tag);
    snprintf(sn.name, sizeof sn.name, "%s", name);
    vm_now_iso(sn.created);
    list[n++] = sn;
    if (vm_snaps_save(dir, list, n)) { snprintf(err, errlen, "can't write snapshots.json"); return -1; }   /* the sweep removes the tags */
    vm_op_end(dir);
    return 0;
}

static int snap_delete_off(const char *dir, const VmMachine *m, const char *tag, char *err, size_t errlen) {
    VmSnap list[VM_SNAPS_MAX];
    int n = vm_snaps_load(dir, list, VM_SNAPS_MAX), i = vm_snaps_find(list, n, tag);
    if (i < 0) { snprintf(err, errlen, "there's no snapshot %s", tag); return -1; }
    if (vm_op_begin(dir, "snapshot-delete", tag, NULL, NULL)) { snprintf(err, errlen, "can't write operation.json"); return -1; }
    memmove(&list[i], &list[i + 1], sizeof list[0] * (size_t)(n - i - 1));
    if (vm_snaps_save(dir, list, n - 1)) { vm_op_end(dir); snprintf(err, errlen, "can't write snapshots.json"); return -1; }
    char disks[2][PATH_MAX + 64], e2[256];
    int nd = disk_files(dir, m, disks);
    for (int k = 0; k < nd; k++) if (qimg_snap(dir, disks[k], "-d", tag, e2, sizeof e2)) vlog("snapshots: deleting %s on %s: %s", tag, disks[k], e2);
    snap_picture_rm(dir, tag);
    vm_op_end(dir);
    return 0;
}

static void discard_state(const char *dir);
/* back to a snapshot with the machine off: every disk goes back; a saved session no longer fits the disks and goes */
static int snap_revert_off(const char *dir, const VmMachine *m, const char *tag, char *err, size_t errlen) {
    VmSnap list[VM_SNAPS_MAX];
    int n = vm_snaps_load(dir, list, VM_SNAPS_MAX);
    if (vm_snaps_find(list, n, tag) < 0) { snprintf(err, errlen, "there's no snapshot %s", tag); return -1; }
    char disks[2][PATH_MAX + 64];
    int nd = disk_files(dir, m, disks);
    if (vm_op_begin(dir, "snapshot-revert", tag, NULL, NULL)) { snprintf(err, errlen, "can't write operation.json"); return -1; }
    for (int i = 0; i < nd; i++) if (qimg_snap(dir, disks[i], "-a", tag, err, errlen)) return -1;     /* operation.json stays: the next open goes on */
    discard_state(dir);
    vm_op_end(dir);
    return 0;
}

static int cmd_create(const char *dir, int argc, char **argv) {
    VmHost h;
    vm_host_probe(&h);
    int arch = h.arch >= 0 ? h.arch : VM_ARCH_AMD64;
    const char *name = NULL, *import = NULL, *iso = NULL;
    int accel = -1, vcpus = 0, mem = 0, disk = 0, fw = -1, disp = -1, net = -1, agent = -1, clip = -1;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) { fprintf(stderr, "lord-vm: %s needs a value\n", a); return 2; }
        i++;
        if (!strcmp(a, "--name")) name = v;
        else if (!strcmp(a, "--arch")) { arch = parse_enum(v, "arm64", "amd64"); if (arch < 0) { fprintf(stderr, "lord-vm: --arch arm64|amd64\n"); return 2; } }
        else if (!strcmp(a, "--accel")) accel = parse_enum(v, "kvm", "tcg");
        else if (!strcmp(a, "--vcpus")) vcpus = atoi(v);
        else if (!strcmp(a, "--memory")) mem = atoi(v);
        else if (!strcmp(a, "--disk")) disk = atoi(v);
        else if (!strcmp(a, "--firmware")) fw = parse_enum(v, "uefi", "bios");
        else if (!strcmp(a, "--display")) disp = parse_enum(v, "window", "none");
        else if (!strcmp(a, "--network")) net = parse_enum(v, "shared", "isolated");
        else if (!strcmp(a, "--import")) import = v;
        else if (!strcmp(a, "--iso")) iso = v;
        else if (!strcmp(a, "--agent")) { agent = parse_enum(v, "off", "on"); if (agent < 0) { fprintf(stderr, "lord-vm: --agent on|off\n"); return 2; } }
        else if (!strcmp(a, "--clipboard")) { clip = parse_clip(v); if (clip < 0) { fprintf(stderr, "lord-vm: --clipboard off|to-guest|from-guest|both\n"); return 2; } }
        else { fprintf(stderr, "lord-vm: unknown option %s\n", a); return 2; }
    }
    VmMachine m;
    vm_machine_defaults(&m, &h, arch);
    if (agent >= 0) m.agent = agent;
    if (clip >= 0) m.clipboard = clip;
    if (name) snprintf(m.name, sizeof m.name, "%s", name);
    if (accel >= 0) m.accel = accel;
    if (vcpus) m.vcpus = vcpus;
    if (mem) m.memory_mb = mem;
    if (disk) m.disk_gb = disk;
    if (disp >= 0) m.display = disp;
    if (net >= 0) m.network = net;
    const char *code = NULL, *vars = NULL;
    if (fw < 0) fw = firmware_for(arch, &code, &vars) ? VM_FW_BIOS : VM_FW_UEFI;
    m.firmware = fw;
    if (fw == VM_FW_UEFI) {
        if (firmware_for(arch, &code, &vars)) { fprintf(stderr, "lord-vm: no UEFI firmware for %s is installed (%s)\n", vm_arch_name(arch), arch == VM_ARCH_ARM64 ? "qemu-efi-aarch64" : "ovmf"); return 1; }
        snprintf(m.firmware_code, sizeof m.firmware_code, "%s", code);
        if (sha256_file(code, m.firmware_sha256)) { fprintf(stderr, "lord-vm: can't read %s\n", code); return 1; }
    }
    if (!file_exists(QEMU_BIN[arch])) { fprintf(stderr, "lord-vm: %s isn't installed\n", QEMU_BIN[arch]); return 1; }
    if (machine_type_for(arch, m.machine_type, sizeof m.machine_type)) { fprintf(stderr, "lord-vm: can't read QEMU's machine types\n"); return 1; }
    char isoreal[PATH_MAX];
    if (iso) {
        if (!realpath(iso, isoreal) || !file_exists(isoreal) || strlen(isoreal) >= sizeof m.iso) { fprintf(stderr, "lord-vm: %s isn't a file (or its path is too long)\n", iso); return 1; }
        memcpy(m.iso, isoreal, strlen(isoreal) + 1);
    }
    char impreal[PATH_MAX];
    int impkind = -1;
    if (import) {
        int g = 0;
        char why[256];
        if (!realpath(import, impreal)) { fprintf(stderr, "lord-vm: %s: %s\n", import, strerror(errno)); return 1; }
        if (image_check(impreal, &impkind, &g, why, sizeof why)) { fprintf(stderr, "lord-vm: %s: %s\n", import, why); return 1; }
        if (m.disk_gb < g) m.disk_gb = g;
    }
    char err[256], warn[256];
    if (vm_machine_check(&m, err, sizeof err)) { fprintf(stderr, "lord-vm: %s\n", err); return 1; }
    if (vm_machine_check_host(&m, &h, err, sizeof err, warn, sizeof warn)) { fprintf(stderr, "lord-vm: %s\n", err); return 1; }
    if (warn[0]) fprintf(stderr, "lord-vm: note: %s\n", warn);

    if (mkdir(dir, 0700) && errno != EEXIST) { fprintf(stderr, "lord-vm: can't make %s: %s\n", dir, strerror(errno)); return 1; }
    char real[PATH_MAX];
    if (!realpath(dir, real) || !dir_ok(real)) { fprintf(stderr, "lord-vm: %s: the folder's path can't contain ',' or control characters\n", dir); return 1; }
    char p[PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/machine.json", real);
    if (!access(p, F_OK)) { fprintf(stderr, "lord-vm: %s already holds a machine\n", real); return 3; }
    snprintf(p, sizeof p, "%s/disks", real); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/log", real); mkdir(p, 0700);
    char sys[PATH_MAX + 64], size[32];
    snprintf(sys, sizeof sys, "%s/disks/system.qcow2", real);
    snprintf(size, sizeof size, "%dG", m.disk_gb);
    if (import) {
        /* jailed: reads the image only, writes only in disks/; the format is named, backing is off */
        char *spec = image_spec(impreal, impkind), dd[PATH_MAX + 64];
        snprintf(dd, sizeof dd, "%s/disks", real);
        char *cv[] = { QEMU_IMG, "convert", "-q", "-O", "qcow2", spec, sys, NULL };
        char *rs[] = { QEMU_IMG, "resize", "-q", "-f", "qcow2", sys, size, NULL };
        int bad = !spec || run_tool_in(cv, err, sizeof err, impreal, dd) || run_tool_in(rs, err, sizeof err, sys, dd);
        free(spec);
        char why[256];
        if (!bad && disk_plain(sys, why, sizeof why)) { snprintf(err, sizeof err, "%s", why); bad = 1; }
        if (bad) { fprintf(stderr, "lord-vm: importing %s failed: %s\n", import, err); unlink(sys); return 1; }
    } else {
        char *cr[] = { QEMU_IMG, "create", "-q", "-f", "qcow2", sys, size, NULL };
        if (run_tool(cr, err, sizeof err)) { fprintf(stderr, "lord-vm: qemu-img create failed: %s\n", err); return 1; }
    }
    if (m.firmware == VM_FW_UEFI) {
        char nv[PATH_MAX + 64];
        snprintf(nv, sizeof nv, "%s/disks/nvram.qcow2", real);
        char *cv[] = { QEMU_IMG, "convert", "-q", "-f", "raw", "-O", "qcow2", (char *)vars, nv, NULL };
        if (run_tool(cv, err, sizeof err)) { fprintf(stderr, "lord-vm: making the UEFI variables failed: %s\n", err); unlink(sys); return 1; }
    }
    if (vm_machine_save(real, &m)) { fprintf(stderr, "lord-vm: can't write machine.json\n"); return 1; }
    printf("{\"created\":true,\"dir\":\""); for (const char *c = real; *c; c++) { if (*c == '"' || *c == '\\') putchar('\\'); putchar(*c); }
    printf("\",\"id\":\"%s\",\"arch\":\"%s\",\"accel\":\"%s\",\"firmware\":\"%s\",\"machine_type\":\"%s\",\"vcpus\":%d,\"memory_mb\":%d,\"disk_gb\":%d}\n",
           m.id, vm_arch_name(m.arch), m.accel ? "tcg" : "kvm", m.firmware ? "bios" : "uefi", m.machine_type, m.vcpus, m.memory_mb, m.disk_gb);
    return 0;
}

/* ---- the supervisor ---- */

typedef struct { int fd, control; } Client;
enum { P_IGNORE = 0, P_REPLY, P_GETFD, P_ADDCLIENT, P_RAW, P_CONT_START, P_CD, P_CD_LAST, P_VERSION, P_SUSPEND, P_MIGQ, P_RESUME,
       P_SNAPJOB, P_JOBQ, P_SNAPSIZE, P_NET, P_NET_ADD, P_NET_LAST };
typedef struct { int kind, client; uint32_t seq; int fd; } Pending;

static struct {
    char dir[PATH_MAX], disks[PATH_MAX + 16], rundir[PATH_MAX], sock_path[PATH_MAX];
    VmMachine m;
    int state, reason, code;
    char detail[160], since[21];
    pid_t qemu, passt;
    int qemu_status;
    int qemu_done;
    Qmp qmp;
    int qmp_up;
    int listen_fd;
    Client cl[MAX_CLIENTS];
    int ncl;
    int serial_fd, serial_log, qemu_err, passt_err, sig_fd;
    off_t serial_bytes;
    int lock_fd;
    int sandbox, cg_mode;
    int memev_fd;
    long oom0;
    int host_shutdown, poweroff, forced, guest_shutdown, panic, killed;
    long long shut_deadline, kill_deadline;
    Pending pq[MAX_PENDING];
    int pq_head, pq_n;
    int dev_qmp, paused_start, keep_running;
    int notify_fd;
    int display_n;
    int cred_fd[MAX_CREDS];
    char cred_name[MAX_CREDS][130];
    int ncreds;
    /* the CD/DVD drive (scsi-cd "cd0"): the disc's block node ("" empty), its fdset (0: none, the start's ISO is a path) */
    char cd_node[16], cd_new_node[16];
    int cd_fdset, cd_new_fdset, cd_busy, cd_client;
    uint32_t cd_seq;
    char cd_err[160], cd_path[VM_PATH_MAX];
    /* the last VM_NOTICE, sent again to clients that connect later */
    int notice, notice_code;
    char notice_text[160];
    /* suspend to disk (6.1): the state file lord-vm opened, who asked, whether the vCPUs ran before */
    char qemu_version[64], cpu_id[17];
    int susp_fd, susp_client, susp_was_running, suspending, suspended;
    uint32_t susp_seq;
    long long susp_t0, migq_next;
    /* resuming from suspend.state at this start */
    int resuming, resume_anyway, resumed;
    char resume_err[160];
    /* the Shell (VM_CONSOLE): one stream to the serial console, and the last bytes the guest wrote, for a new one */
    int console_fd;
    char ring[8192];
    size_t ring_n, ring_at;
    /* the guest agent (6.17): qemu-guest-agent on one port (JSON lines, Qmp's reader), org.lordos.agent on another */
    int qga_fd, lag_fd, ag_state, ag_caps, qga_up, qga_open;
    Qmp qga;
    int agq[32], agq_head, agq_n;           /* what each qemu-guest-agent reply answers (AQ_*) */
    long long ag_next, sync_at, ping_at, journal_at, shutdown_at;
    long sync_id;
    char ag_version[48], ag_os[96], jcursor[256];
    long jpid;                              /* a journalctl running in the guest (guest-exec), 0 none */
    int keep_awake, inhibit_done;
    uint8_t *lag_buf;
    size_t lag_n;
    int lag_hello, lag_clipx;               /* lordos-guest said hello; it takes AGK_CLIPX (pictures, rich text) */
    char lag_version[48];
    uint8_t *helper;                        /* lordos-guest for VAG_INSTALL, read before lord-vm's own Landlock */
    size_t helper_n, helper_off;
    long helper_handle;
    int install_client;
    uint32_t install_seq;
    /* a snapshot job (VM_SNAPSHOT): which, its tag and name, who asked; --loadvm TAG at start */
    int snap_kind, snap_client;
    uint32_t snap_seq;
    char snap_tag[33], snap_name[VM_NAME_MAX + 1], snap_err[160], loadvm[33];
    /* the network's self-healing (6.5): passt's keeper (started before lord-vm's own Landlock, which leaves lord-vm no
     * way to start a program), the passt it started last (followed by pidfd: not our child), QEMU's backend number
     * (pst<gen> on hub port hp<gen>), when the network went and when to try again, restarts in the last minute */
    int keeper_fd, passt_pidfd, net_gen, net_busy, net_restarts, net_gave_up;
    pid_t keeper, passt_ext;
    long long net_lost_at, net_retry_at, net_window_at;
    char net_err[160];
    /* the computer stops or the session ends (6.1): the machine is saved to disk, else shut down, else turned off */
    int host_stopping;
    long long host_stop_deadline;
    char host_stop_why[96];
    /* a Clone's first start (6.3): the guest gets its own hostname and machine-id through the agent (identity.json) */
    char id_hostname[64];
    int id_state;                           /* 0 none, 1 pending, 2 running in the guest, 3 done */
    long id_pid;
    long long id_poll_at;
} S;

static void awake_update(void);
static void send_to(int ci, const VmMsg *m, int fd) {
    if (ci < 0 || ci >= MAX_CLIENTS || S.cl[ci].fd < 0) return;
    if (vm_send(S.cl[ci].fd, m, fd)) vlog("client %d: message %d not delivered (%s)", ci, m->type, strerror(errno));
}

static void reply(int ci, uint32_t seq, int code, const char *text) {
    VmMsg r = vm_msg(VM_REPLY, seq, code, 0, 0, 0, text);
    send_to(ci, &r, -1);
}

static VmMsg state_msg(void) { return vm_msg(VM_STATE, 0, S.state, S.reason, S.code, (int)S.qemu, S.detail); }
static VmMsg info_msg(void) { return vm_msg(VM_INFO, 0, S.m.vcpus, S.m.memory_mb, S.sandbox, S.cg_mode, S.m.id); }

static void notify_parent(int ok) {
    if (S.notify_fd < 0) return;
    char c = ok ? 'o' : 'f';
    write_all(S.notify_fd, &c, 1);
    close(S.notify_fd);
    S.notify_fd = -1;
}

static void set_state(int st, int why, int code, const char *detail) {
    int changed = st != S.state || why != S.reason;
    S.state = st;
    S.reason = why;
    S.code = code;
    if (detail) snprintf(S.detail, sizeof S.detail, "%s", detail);
    vm_now_iso(S.since);
    RunInfo r = { st, why, code, S.qemu, S.passt > 0 ? S.passt : S.passt_ext, getpid(), "", "", st == VS_RUNNING || st == VS_PAUSED ? S.ag_state : VAS_OFF };
    memcpy(r.since, S.since, sizeof r.since);
    memcpy(r.detail, S.detail, sizeof r.detail);
    runinfo_write(S.dir, &r);
    if (changed || why == VR_GUEST_RESET) vlog("state %s (%s%s%s)", STATE_NAME[st], REASON_NAME[why], S.detail[0] ? ": " : "", S.detail);
    VmMsg m = state_msg();
    for (int i = 0; i < MAX_CLIENTS; i++) send_to(i, &m, -1);
    if (st == VS_RUNNING || st == VS_PAUSED) notify_parent(1);
    else if (st == VS_STOPPED || st == VS_CRASHED) notify_parent(0);
    awake_update();
}

static int pq_push(int kind, int client, uint32_t seq, int fd) {
    if (S.pq_n >= MAX_PENDING) return -1;
    S.pq[(S.pq_head + S.pq_n) % MAX_PENDING] = (Pending){ kind, client, seq, fd };
    S.pq_n++;
    return 0;
}

static int qmp_do(const char *cmd, const char *args, int kind, int client, uint32_t seq, int pass_fd, int keep_fd) {
    if (!S.qmp_up || S.pq_n >= MAX_PENDING) return -1;
    if (qmp_command(&S.qmp, cmd, args, pass_fd)) { vlog("QMP %s: can't send", cmd); return -1; }
    pq_push(kind, client, seq, keep_fd);
    return 0;
}

/* ---- cgroups ---- */

static int read_self_cgroup(char *out, size_t n) {
    FILE *f = fopen("/proc/self/cgroup", "re");
    if (!f) return -1;
    char line[PATH_MAX];
    int r = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "0::", 3)) continue;
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if ((size_t)snprintf(out, n, "/sys/fs/cgroup%s", line + 3) < n) r = 0;
        break;
    }
    fclose(f);
    return r;
}

static int cg_write(const char *dir, const char *file, const char *val) {
    char p[PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/%s", dir, file);
    int fd = open(p, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int r = write_all(fd, val, strlen(val));
    close(fd);
    return r;
}

static int qemu_procs_fd = -1, passt_procs_fd = -1;

/* the machine's scope, then leaves inside it when the scope is ours to divide (3.7) */
static void setup_cgroups(void) {
    char scope[96];
    snprintf(scope, sizeof scope, "app-lordos-lordos-vm-%s.scope", S.m.id);
    long limit = vm_mem_limit_mb(&S.m);
    int cpu = S.m.vcpus * 100 + 50;
    if (cpu > 6400) cpu = 6400;
    CgroupLimits lim = { .mem_high_mb = 0, .mem_max_mb = limit + 128, .cpu_percent = cpu };
    S.cg_mode = VMC_NONE;
    if (!cgroup_enter(scope, &lim)) { S.cg_mode = VMC_SCOPE; vlog("cgroup: in %s (memory.max %ld MB, cpu %d%%)", scope, limit + 128, cpu); }
    char self[PATH_MAX];
    if (read_self_cgroup(self, sizeof self)) { vlog("cgroup: can't read /proc/self/cgroup"); return; }
    char procs[PATH_MAX + 32];
    snprintf(procs, sizeof procs, "%s/memory.events", self);
    S.memev_fd = open(procs, O_RDONLY | O_CLOEXEC);
    snprintf(procs, sizeof procs, "%s/cgroup.procs", self);
    if (access(procs, W_OK) || access(self, W_OK)) {
        vlog("cgroup: %s isn't ours to divide: QEMU and passt share %s", self, S.cg_mode ? "the machine's scope" : "lord-vm's cgroup, with no limits");
        return;
    }
    /* cgroup v2: a cgroup with leaves holds no processes itself, so lord-vm moves to its own leaf first */
    char leaf[PATH_MAX + 32];
    snprintf(leaf, sizeof leaf, "%s/supervisor", self);
    if ((mkdir(leaf, 0755) && errno != EEXIST) || cg_write(leaf, "cgroup.procs", "0")) { vlog("cgroup: can't make leaves in %s: %s", self, strerror(errno)); return; }
    if (cg_write(self, "cgroup.subtree_control", "+memory +cpu +pids")) vlog("cgroup: controllers in %s: %s (limits may not apply)", self, strerror(errno));
    char q[PATH_MAX + 32], p[PATH_MAX + 32], val[64];
    snprintf(q, sizeof q, "%s/qemu", self);
    snprintf(p, sizeof p, "%s/passt", self);
    if ((mkdir(q, 0755) && errno != EEXIST) || (mkdir(p, 0755) && errno != EEXIST)) { vlog("cgroup: can't make leaves: %s", strerror(errno)); return; }
    snprintf(val, sizeof val, "%ld", limit * 1024L * 1024L);
    int lq = cg_write(q, "memory.max", val);
    snprintf(val, sizeof val, "%d 100000", cpu * 1000);
    cg_write(q, "cpu.max", val);
    cg_write(q, "memory.swap.max", "0");
    cg_write(p, "memory.max", "134217728");
    cg_write(p, "cpu.max", "200000 100000");
    char f[PATH_MAX + 64];
    snprintf(f, sizeof f, "%s/cgroup.procs", q);
    qemu_procs_fd = open(f, O_WRONLY | O_CLOEXEC);
    snprintf(f, sizeof f, "%s/cgroup.procs", p);
    passt_procs_fd = open(f, O_WRONLY | O_CLOEXEC);
    if (qemu_procs_fd < 0 || passt_procs_fd < 0) { vlog("cgroup: leaves not writable"); return; }
    snprintf(f, sizeof f, "%s/memory.events", q);
    int me = open(f, O_RDONLY | O_CLOEXEC);
    if (me >= 0) { if (S.memev_fd >= 0) close(S.memev_fd); S.memev_fd = me; }
    S.cg_mode |= VMC_NESTED;
    vlog("cgroup: nested leaves in %s: qemu (memory.max %ld MB%s, cpu %d%%), passt (128 MB, 200%%), supervisor", self, limit, lq ? " NOT SET" : "", cpu);
}

static long read_oom_kills(void) {
    if (S.memev_fd < 0) return 0;
    char buf[512];
    ssize_t n = pread(S.memev_fd, buf, sizeof buf - 1, 0);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *p = strstr(buf, "oom_kill ");
    return p ? atol(p + 9) : 0;
}

/* ---- starting ---- */

static void child_common(int procs_fd) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    setpgid(0, 0);
    if (procs_fd >= 0 && write(procs_fd, "0", 1) != 1) { const char e[] = "lord-vm: can't enter the cgroup leaf\n"; ssize_t w = write(2, e, sizeof e - 1); (void)w; }
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    for (int s = 1; s < 32; s++) if (s != SIGKILL && s != SIGSTOP) signal(s, SIG_DFL);
    struct rlimit no = { 0, 0 };
    setrlimit(RLIMIT_CORE, &no);
}

/* moves the fds in want[] to 3, 4, ... in order, without one overwriting another */
static void place_fds(const int *want, int n) {
    int tmp[32];
    for (int i = 0; i < n; i++) tmp[i] = fcntl(want[i], F_DUPFD_CLOEXEC, 32);
    for (int i = 0; i < n; i++) { dup2(tmp[i], 3 + i); }
    syscall(SYS_close_range, 3 + n, ~0u, 0);
}

static pid_t spawn_passt(int net_fd, int err_fd) {
    pid_t pid = fork();
    if (pid) return pid;
    child_common(passt_procs_fd);
    int null = open("/dev/null", O_RDWR);
    dup2(null, 0);
    dup2(err_fd, 1);
    dup2(err_fd, 2);
    int fds[1] = { net_fd };
    place_fds(fds, 1);
    /* Shared (6.5): out only. No port forwarded in, and the host's loopback isn't the gateway (--no-map-gw). DNS:
     * queries to one fixed address go to the host's first nameserver, loopback resolvers included (with --dns as
     * well, passt advertised the address but didn't answer on it: measured 2026-11-05) */
    char *argv[] = { PASST, "-f", "-q", "-1", "--fd", "3", "--no-map-gw", "-t", "none", "-u", "none",
                     "--dns-forward", VM_DNS_ADDR, NULL };
    char *env[] = { NULL };
    execve(PASST, argv, env);
    _exit(127);
}

/* passt's keeper (6.5): a child forked before lord-vm applies its own Landlock, which leaves lord-vm unable to start a
 * program. For each byte lord-vm writes it starts one passt (its child, which dies with it) and sends back, on one
 * SOCK_SEQPACKET message, passt's pid and three descriptors: the network socket for QEMU, passt's stderr and a pidfd.
 * It does nothing else and reads nothing else. */
static void keeper_main(int sock) {
    signal(SIGCHLD, SIG_IGN);           /* its passts are reaped by the kernel; lord-vm follows them by pidfd */
    for (;;) {
        char c;
        ssize_t r = read(sock, &c, 1);
        if (r == 0 || (r < 0 && errno != EINTR)) _exit(0);
        if (r < 0) continue;
        int net[2] = { -1, -1 }, perr[2] = { -1, -1 }, fds[3] = { -1, -1, -1 };
        int32_t pid = -1;
        if (!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, net) && !pipe2(perr, O_CLOEXEC)) {
            pid_t p = spawn_passt(net[1], perr[1]);
            int pfd = p > 0 ? (int)syscall(SYS_pidfd_open, p, 0) : -1;
            if (pfd >= 0) { pid = p; fds[0] = net[0]; fds[1] = perr[0]; fds[2] = pfd; }
            else if (p > 0) kill(p, SIGKILL);
        }
        if (net[1] >= 0) close(net[1]);
        if (perr[1] >= 0) close(perr[1]);
        union { struct cmsghdr h; char b[CMSG_SPACE(sizeof fds)]; } cm;
        struct iovec io = { &pid, sizeof pid };
        struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1 };
        if (pid > 0) {
            memset(&cm, 0, sizeof cm);
            mh.msg_control = cm.b;
            mh.msg_controllen = sizeof cm.b;
            struct cmsghdr *ch = CMSG_FIRSTHDR(&mh);
            ch->cmsg_level = SOL_SOCKET;
            ch->cmsg_type = SCM_RIGHTS;
            ch->cmsg_len = CMSG_LEN(sizeof fds);
            memcpy(CMSG_DATA(ch), fds, sizeof fds);
        }
        if (sendmsg(sock, &mh, MSG_NOSIGNAL) < 0 && pid > 0) kill(pid, SIGKILL);
        for (int i = 0; i < 3; i++) if (fds[i] >= 0) close(fds[i]);
        if (pid < 0) { if (net[0] >= 0) close(net[0]); if (perr[0] >= 0) close(perr[0]); }
    }
}

/* forks the keeper: it keeps fd 0-2, its socket and the passt leaf of the machine's cgroup, nothing else */
static pid_t start_keeper(int *out) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(sv[0]); close(sv[1]); return -1; }
    if (!pid) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL);
        int s = fcntl(sv[1], F_DUPFD_CLOEXEC, 64), pf = passt_procs_fd >= 0 ? fcntl(passt_procs_fd, F_DUPFD_CLOEXEC, 65) : -1;
        if (s < 0) _exit(1);
        int k3 = dup3(s, 3, O_CLOEXEC), k4 = pf >= 0 ? dup3(pf, 4, O_CLOEXEC) : -1;
        syscall(SYS_close_range, pf >= 0 ? 5 : 4, ~0u, 0);
        passt_procs_fd = k4;
        keeper_main(k3);
        _exit(0);
    }
    close(sv[1]);
    *out = sv[0];
    return pid;
}

/* QEMU's option values: a comma is written twice */
static void opt_escape(char *out, size_t n, const char *s) {
    size_t o = 0;
    for (; *s && o + 3 < n; s++) { if (*s == ',') out[o++] = ','; out[o++] = *s; }
    out[o] = 0;
}

static char *blockdev_file(const char *node, const char *path, int ro) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"driver\":\"file\",\"node-name\":"); jb_str(&b, node);
    jb_raw(&b, ",\"filename\":"); jb_str(&b, path);
    jb_printf(&b, ",\"read-only\":%s%s}", ro ? "true" : "false", ro ? "" : ",\"discard\":\"unmap\"");
    return b.oom ? NULL : b.p;
}
static char *blockdev_fmt(const char *drv, const char *node, const char *file, int ro) {
    JBuf b;
    jb_init(&b);
    /* a qcow2 never follows a backing file, whatever its header says (start checks the header too) */
    jb_printf(&b, "{\"driver\":\"%s\",\"node-name\":\"%s\",\"file\":\"%s\",\"read-only\":%s%s%s}", drv, node, file, ro ? "true" : "false",
              ro ? "" : ",\"discard\":\"unmap\"", !strcmp(drv, "qcow2") ? ",\"backing\":null" : "");
    return b.oom ? NULL : b.p;
}

/* a linked clone's system disk: its backing is the shared base node, named here, never the header's file name */
static char *blockdev_overlay(const char *node, const char *file, const char *backing) {
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"driver\":\"qcow2\",\"node-name\":\"%s\",\"file\":\"%s\",\"read-only\":false,\"discard\":\"unmap\",\"backing\":\"%s\"}", node, file, backing);
    return b.oom ? NULL : b.p;
}

#define MAXARGS 128
static char *qargv[MAXARGS];
static int qargc;
static void arg(const char *s) { if (qargc < MAXARGS - 1) qargv[qargc++] = strdup(s); qargv[qargc] = NULL; }
static void argf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void argf(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    arg(buf);
}
static void argj(char *json) { if (json) { arg(json); free(json); } }

/* fds inside QEMU: 3 /dev/kvm (or /dev/null), 4 QMP, 5 serial, 6 network (or /dev/null) */
static void build_qemu_args(void) {
    const VmMachine *m = &S.m;
    int kvm = m->accel == VM_ACCEL_KVM, arm = m->arch == VM_ARCH_ARM64, uefi = m->firmware == VM_FW_UEFI;
    char esc[2 * VM_NAME_MAX + 8], p[PATH_MAX + 64];
    qargc = 0;
    arg(QEMU_BIN[m->arch]);
    arg("-nodefaults"); arg("-no-user-config");
    opt_escape(esc, sizeof esc, m->name);
    arg("-name"); argf("guest=%s,debug-threads=on", esc);
    arg("-uuid"); arg(m->id);
    arg("-machine"); argf("%s,dump-guest-core=off%s", m->machine_type, uefi ? ",pflash0=fw-code,pflash1=fw-vars" : "");
    if (kvm) { arg("-accel"); arg("kvm,device=/dev/fdset/1"); arg("-add-fd"); arg("fd=3,set=1,opaque=kvm"); }
    else { arg("-accel"); arg("tcg,thread=multi,tb-size=256"); }
    arg("-cpu"); arg(kvm ? "host" : "max");
    arg("-smp"); argf("%d", m->vcpus);
    arg("-m"); argf("%d", m->memory_mb);
    if (uefi) {
        arg("-blockdev"); argj(blockdev_file("fw-code-file", m->firmware_code, 1));
        arg("-blockdev"); argj(blockdev_fmt("raw", "fw-code", "fw-code-file", 1));
        snprintf(p, sizeof p, "%s/nvram.qcow2", S.disks);
        arg("-blockdev"); argj(blockdev_file("fw-vars-file", p, 0));
        arg("-blockdev"); argj(blockdev_fmt("qcow2", "fw-vars", "fw-vars-file", 0));
    }
    snprintf(p, sizeof p, "%s/system.qcow2", S.disks);
    if (m->base[0]) {
        arg("-blockdev"); argj(blockdev_file("base-file", m->base, 1));
        arg("-blockdev"); argj(blockdev_fmt("qcow2", "base", "base-file", 1));
        arg("-blockdev"); argj(blockdev_file("sys-file", p, 0));
        arg("-blockdev"); argj(blockdev_overlay("sys", "sys-file", "base"));
    } else {
        arg("-blockdev"); argj(blockdev_file("sys-file", p, 0));
        arg("-blockdev"); argj(blockdev_fmt("qcow2", "sys", "sys-file", 0));
    }
    /* Every PCI device has a fixed slot: with the slots QEMU picks, taking an ISO out after an install moved the
     * network card (enp0s3 became enp0s2) and the installed system's network didn't come up. Slots 1 to 6 are the
     * ones a machine without an ISO always had; the ISO's controller goes after them. */
    arg("-device"); arg("virtio-blk-pci,id=sysdisk,drive=sys,bootindex=0,addr=0x1");
    /* the CD/DVD drive is always there (slot 8, after the ones a machine always had), so a disc can go in and out while
     * the machine runs (VM_CDROM); the ISO of machine.json is in it at start */
    arg("-device"); arg("virtio-scsi-pci,id=scsi0,addr=0x8");
    if (m->iso[0]) {
        arg("-blockdev"); argj(blockdev_file("cd-file", m->iso, 1));
        arg("-blockdev"); argj(blockdev_fmt("raw", "cd", "cd-file", 1));
        arg("-device"); arg("scsi-cd,id=cd0,bus=scsi0.0,drive=cd,bootindex=1");
        snprintf(S.cd_node, sizeof S.cd_node, "cd");
    } else { arg("-device"); arg("scsi-cd,id=cd0,bus=scsi0.0,bootindex=1"); S.cd_node[0] = 0; }
    if (m->network == VM_NET_SHARED) {
        /* The card sits on a hub, and passt on another port of it: when passt ends, a new one is put on a new port
         * (QMP netdev_del, getfd, netdev_add socket + hubport) while the card stays as it was, so the guest keeps its
         * network without a restart (6.5). A card attached straight to its backend can't be given another one. */
        arg("-netdev"); arg("hubport,id=nic0,hubid=0");
        arg("-netdev"); arg("stream,id=pst0,addr.type=fd,addr.str=6");
        arg("-netdev"); arg("hubport,id=hp0,hubid=0,netdev=pst0");
        arg("-device"); argf("virtio-net-pci,netdev=nic0,mac=%s,addr=0x2", m->mac);
    }
    arg("-device"); arg("virtio-rng-pci,addr=0x3");
    /* amd64 with -nodefaults needs a display device or its kernel never starts (3.3); arm64's virt has no keyboard */
    if (!arm) { arg("-device"); arg("virtio-vga,id=video0,addr=0x4"); }
    if (m->display == VM_DISPLAY_WINDOW) {
        if (arm) { arg("-device"); arg("virtio-gpu-pci,id=video0,addr=0x4"); arg("-device"); arg("virtio-keyboard-pci,addr=0x5"); }
        arg("-device"); arg(arm ? "virtio-tablet-pci,addr=0x6" : "virtio-tablet-pci,addr=0x5");
        /* the guest's sound goes out through the same peer-to-peer D-Bus as the picture (lord-vmdisplay listens): 48 kHz,
         * 16-bit stereo, what lord-server's recordings and speakers take without converting */
        arg("-audiodev"); arg("dbus,id=snd0,out.frequency=48000,out.channels=2,out.format=s16,in.frequency=48000,in.channels=2,in.format=s16");
        arg("-display"); arg("dbus,p2p=yes,audiodev=snd0");
    } else { arg("-audiodev"); arg("none,id=snd0"); arg("-display"); arg("none"); }
    /* a sound card (HDA, which every Linux and Windows guest has a driver for), on its own slot after the CD/DVD
     * drive's: a machine made before it had none, and a state saved without it still loads (a device the saved
     * stream doesn't name keeps its reset state) */
    arg("-device"); arg("intel-hda,id=sound0,addr=0x9");
    arg("-device"); arg("hda-output,id=sound0-codec0,bus=sound0.0,cad=0,audiodev=snd0");
    arg("-chardev"); arg("socket,id=qmp,fd=4"); arg("-mon"); arg("chardev=qmp,mode=control");
    arg("-chardev"); arg("socket,id=ser0,fd=5"); arg("-serial"); arg("chardev:ser0");
    if (m->agent) {
        /* the guest agent (6.17): virtio-serial at slot 7, after every slot a machine had; qemu-guest-agent's port and
         * ours, each a socket lord-vm holds the other end of (fds 7 and 8) */
        arg("-device"); arg("virtio-serial-pci,id=vser0,addr=0x7");
        arg("-chardev"); arg("socket,id=qga0,fd=7");
        arg("-device"); arg("virtserialport,bus=vser0.0,nr=1,chardev=qga0,id=qga-port,name=org.qemu.guest_agent.0");
        arg("-chardev"); arg("socket,id=lag0,fd=8");
        arg("-device"); arg("virtserialport,bus=vser0.0,nr=2,chardev=lag0,id=lag-port,name=org.lordos.agent");
    }
    arg("-sandbox"); arg("on,obsolete=deny,elevateprivileges=deny,spawn=deny,resourcecontrol=deny");
    arg("-msg"); arg("timestamp=on");
    arg("-S");
    if (S.resuming) { arg("-incoming"); arg("defer"); }
    /* credentials: memfds at fd 7, 8, ... (never their values on the command line) */
    for (int i = 0; i < S.ncreds; i++) { arg("-smbios"); argf("type=11,path=/proc/self/fd/%d", (m->agent ? 9 : 7) + i); }
}

static void fail_start(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail_start(const char *fmt, ...) {
    char d[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d, sizeof d, fmt, ap);
    va_end(ap);
    set_state(VS_CRASHED, VR_START_FAILED, 0, d);
}

static int suspend_usable(char *err, size_t n);
static const char *snap_devices(void);
static void send_job(int kind, int status, int pct, const char *text);
static int start_machine(void) {
    VmHost h;
    vm_host_probe(&h);
    char err[256], warn[256];
    if (vm_machine_check_host(&S.m, &h, err, sizeof err, warn, sizeof warn)) { fail_start("%s", err); return -1; }
    if (warn[0]) vlog("note: %s", warn);
    char p[PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/system.qcow2", S.disks);
    if (!file_exists(p)) { fail_start("the system disk is missing: %s", p); return -1; }
    if (S.m.base[0]) {
        /* a linked clone (6.3): its base is checked like a system disk, and the disk itself may name a backing file,
         * which is never followed: QEMU gets the base by node */
        if (!file_exists(S.m.base)) { fail_start("the shared disk this machine was cloned from is missing: %s", S.m.base); return -1; }
        if (disk_plain(S.m.base, err, sizeof err)) { fail_start("the shared disk can't be used: %s", err); return -1; }
        uint8_t head[VM_IMG_HEAD];
        int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        ssize_t n = fd >= 0 ? read(fd, head, sizeof head) : -1;
        if (fd >= 0) close(fd);
        if (vm_qcow2_overlay(head, n > 0 ? (size_t)n : 0, err, sizeof err)) { fail_start("the system disk can't be used: %s", err); return -1; }
    } else if (disk_plain(p, err, sizeof err)) { fail_start("the system disk can't be used: %s", err); return -1; }
    if (S.m.firmware == VM_FW_UEFI) {
        snprintf(p, sizeof p, "%s/nvram.qcow2", S.disks);
        if (!file_exists(p)) { fail_start("the UEFI variables are missing: %s", p); return -1; }
        if (disk_plain(p, err, sizeof err)) { fail_start("the UEFI variables can't be used: %s", err); return -1; }
        if (!file_exists(S.m.firmware_code)) { fail_start("the UEFI firmware is missing: %s", S.m.firmware_code); return -1; }
        char sum[65];
        if (!sha256_file(S.m.firmware_code, sum) && strcmp(sum, S.m.firmware_sha256))
            vlog("note: the UEFI firmware changed since this machine was made (%.12s, was %.12s); its variables are kept", sum, S.m.firmware_sha256);
    }
    if (S.m.iso[0] && !file_exists(S.m.iso)) { fail_start("the attached image is missing: %s", S.m.iso); return -1; }
    if (!file_exists(QEMU_BIN[S.m.arch])) { fail_start("%s isn't installed", QEMU_BIN[S.m.arch]); return -1; }
    if (S.m.network == VM_NET_SHARED && !file_exists(PASST)) { fail_start("passt isn't installed (Shared network)"); return -1; }
    {
        char why[160];
        int u = suspend_usable(why, sizeof why);
        if (u < 0) { vlog("resume: %s", why); set_state(VS_CRASHED, VR_RESUME_FAILED, 0, why); return -1; }
        S.resuming = u == 1;
        if (S.resuming) vlog("resume: this start continues from the saved state");
    }

    int kvm = -1;
    if (S.m.accel == VM_ACCEL_KVM) {
        kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
        if (kvm < 0) { fail_start("can't open /dev/kvm: %s", strerror(errno)); return -1; }
        S.sandbox |= VMS_KVM_FD;
    } else kvm = open("/dev/null", O_RDWR | O_CLOEXEC);
    int qmp[2], ser[2], net[2] = { -1, -1 }, qerr[2], perr[2] = { -1, -1 }, st[2], qga[2] = { -1, -1 }, lag[2] = { -1, -1 };
    if (S.m.agent && (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, qga) || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, lag))) { fail_start("out of descriptors"); return -1; }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, qmp) || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ser) ||
        pipe2(qerr, O_CLOEXEC) || pipe2(st, O_CLOEXEC)) { fail_start("out of descriptors"); return -1; }
    if (S.m.network == VM_NET_SHARED) {
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, net) || pipe2(perr, O_CLOEXEC)) { fail_start("out of descriptors"); return -1; }
        S.passt = spawn_passt(net[1], perr[1]);
        close(net[1]);
        close(perr[1]);
        if (S.passt < 0) { fail_start("can't start passt"); return -1; }
        S.passt_err = perr[0];
        vlog("passt: pid %d", (int)S.passt);
        S.sandbox |= VMS_PDEATHSIG;
    }
    build_qemu_args();
    {
        char line[8192];
        size_t o = 0;
        for (int i = 0; qargv[i] && o + 2 < sizeof line; i++) o += (size_t)snprintf(line + o, sizeof line - o, "%s%s", i ? " " : "", qargv[i]);
        vlog("qemu: %s", line);
        for (int i = 0; i < S.ncreds; i++) vlog("qemu: credential %s on fd %d (value not logged)", S.cred_name[i], (S.m.agent ? 9 : 7) + i);
    }
    pid_t pid = fork();
    if (pid < 0) { fail_start("fork: %s", strerror(errno)); return -1; }
    if (!pid) {
        child_common(qemu_procs_fd);
        /* QEMU quits on SIGTERM, and stopping the session's units sends it to every process of the machine's scope at
         * once: blocked here (QEMU never unblocks it), so lord-vm can still save the machine to disk (host_stop) and
         * then tell QEMU to quit */
        sigset_t term;
        sigemptyset(&term);
        sigaddset(&term, SIGTERM); sigaddset(&term, SIGINT); sigaddset(&term, SIGHUP);
        sigprocmask(SIG_BLOCK, &term, NULL);
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(qerr[1], 1);
        dup2(qerr[1], 2);
        /* 3 kvm, 4 QMP, 5 serial, 6 network, 7.. credentials, then the status pipe (closed by exec) */
        int fds[6 + MAX_CREDS + 1], nf = 0;
        fds[nf++] = kvm; fds[nf++] = qmp[1]; fds[nf++] = ser[1]; fds[nf++] = net[0] >= 0 ? net[0] : null;
        if (S.m.agent) { fds[nf++] = qga[1]; fds[nf++] = lag[1]; }
        for (int i = 0; i < S.ncreds; i++) fds[nf++] = S.cred_fd[i];
        fds[nf++] = st[1];
        place_fds(fds, nf);
        int stf = 3 + nf - 1;
        fcntl(stf, F_SETFD, FD_CLOEXEC);
        VmQemuJail j = { S.disks, S.m.iso[0] ? S.m.iso : NULL, S.m.firmware == VM_FW_UEFI ? S.m.firmware_code : NULL, S.m.base[0] ? S.m.base : NULL, S.m.accel == VM_ACCEL_KVM };
        int mask = vm_jail_qemu(&j);
        int32_t msg[2] = { mask, 0 };
        /* without our filter and no_new_privs QEMU doesn't run: the parent reads the mask and sees which */
        if (!(mask & VMS_SECCOMP_OURS) || !(mask & VMS_NO_NEW_PRIVS)) { write_all(stf, msg, sizeof msg[0]); _exit(126); }
        write_all(stf, msg, sizeof msg[0]);
        char *env[] = { "LC_ALL=C.UTF-8", NULL };
        execve(qargv[0], qargv, env);
        msg[1] = errno ? errno : ENOEXEC;
        write_all(stf, &msg[1], sizeof msg[1]);
        _exit(127);
    }
    S.qemu = pid;
    close(kvm); close(qmp[1]); close(ser[1]); close(qerr[1]); close(st[1]);
    if (S.m.agent) {
        close(qga[1]); close(lag[1]);
        S.qga_fd = qga[0]; S.lag_fd = lag[0];
        fcntl(S.qga_fd, F_SETFL, O_NONBLOCK); fcntl(S.lag_fd, F_SETFL, O_NONBLOCK);
        qmp_open(&S.qga, S.qga_fd);
        S.ag_state = VAS_WAITING;
        S.ag_next = now_ms() + 1000;
    }
    for (int i = 0; i < S.ncreds; i++) { close(S.cred_fd[i]); S.cred_fd[i] = -1; }
    if (net[0] >= 0) close(net[0]);
    S.qemu_err = qerr[0];
    S.serial_fd = ser[0];
    int32_t got[2] = { 0, 0 };
    ssize_t n = 0, r;
    while ((r = read(st[0], (char *)got + n, sizeof got - (size_t)n)) != 0) { if (r < 0) { if (errno == EINTR) continue; break; } n += r; if (n == sizeof got) break; }
    close(st[0]);
    if (n >= 4) S.sandbox |= got[0];
    if (n < 4 || !(got[0] & VMS_SECCOMP_OURS) || !(got[0] & VMS_NO_NEW_PRIVS)) { fail_start("QEMU's sandbox couldn't be set up (seccomp %d, no_new_privs %d)", !!(got[0] & VMS_SECCOMP_OURS), !!(got[0] & VMS_NO_NEW_PRIVS)); return -1; }
    if (n == sizeof got) { fail_start("can't run QEMU: %s", strerror(got[1])); return -1; }
    S.sandbox |= VMS_SECCOMP_QEMU | VMS_PDEATHSIG;
    vlog("qemu: pid %d; sandbox: landlock-fs=%d landlock-net=%d landlock-scope=%d seccomp(ours)=%d no-new-privs=%d kvm-by-fd=%d",
         (int)pid, !!(S.sandbox & VMS_LANDLOCK_FS), !!(S.sandbox & VMS_LANDLOCK_NET), !!(S.sandbox & VMS_LANDLOCK_SCOPE),
         !!(S.sandbox & VMS_SECCOMP_OURS), !!(S.sandbox & VMS_NO_NEW_PRIVS), !!(S.sandbox & VMS_KVM_FD));
    if (!(S.sandbox & VMS_LANDLOCK_FS)) vlog("note: no Landlock on this kernel: weaker isolation (3.3)");
    if (qmp_open(&S.qmp, qmp[0])) { fail_start("out of memory"); return -1; }
    if (qmp_handshake(&S.qmp, 20000, err, sizeof err)) {
        vlog("qmp: %s", err);
        fail_start("QEMU stopped before it was ready (%s): see the log", err);
        kill(S.qemu, SIGKILL);
        S.killed = 1;
        return 0;
    }
    S.qmp_up = 1;
    S.oom0 = read_oom_kills();
    qmp_do("query-version", NULL, P_VERSION, -1, 0, -1, -1);
    if (S.resuming) return 0;
    if (S.loadvm[0]) {
        /* -loadvm would take the first snapshot-capable disk for the memory (the UEFI variables, which hold none): the
         * job names the system disk, before a vCPU runs; cont follows when it's done (snap_concluded) */
        char a[256];
        snprintf(a, sizeof a, "{\"job-id\":\"snap\",\"tag\":\"%s\",\"vmstate\":\"sys\",\"devices\":%s}", S.loadvm, snap_devices());
        S.snap_kind = VSN_REVERT; S.snap_client = -1; S.snap_seq = 0;
        snprintf(S.snap_tag, sizeof S.snap_tag, "%s", S.loadvm);
        qmp_do("snapshot-load", a, P_SNAPJOB, -1, 0, -1, -1);
        send_job(VJ_REVERT, VJS_RUNNING, -1, S.loadvm);
        return 0;
    }                   /* the state goes in first (P_VERSION → begin_resume), then cont */
    if (S.paused_start) set_state(VS_PAUSED, VR_NONE, 0, "started paused");
    else qmp_do("cont", NULL, P_CONT_START, -1, 0, -1, -1);
    return 0;
}

/* ---- requests ---- */

static void begin_shutdown(int secs) {
    if (S.state == VS_PAUSED) qmp_do("cont", NULL, P_IGNORE, -1, 0, -1, -1);
    qmp_do("system_powerdown", NULL, P_IGNORE, -1, 0, -1, -1);
    S.host_shutdown = 1;
    S.shut_deadline = now_ms() + (long long)(secs ? secs : 60) * 1000;
    if (S.qga_up) S.shutdown_at = now_ms() + 15000;     /* ACPI first, then the agent (6.1) */
    set_state(VS_STOPPING, VR_HOST_SHUTDOWN, 0, "asked the guest to shut down");
}

static void begin_poweroff(void) {
    S.poweroff = 1;
    if (qmp_do("quit", NULL, P_IGNORE, -1, 0, -1, -1)) { kill(S.qemu, SIGKILL); S.killed = 1; }
    S.kill_deadline = now_ms() + 5000;
    set_state(VS_STOPPING, VR_POWEROFF, 0, "turning off");
}

/* VM_CDROM: the disc out (open the tray, take the medium, delete its nodes), then the new one in (its descriptor into an
 * fdset, a raw node on /dev/fdset/N, insert, close the tray). QMP runs them in order; the last one answers. */
static void cd_change(int ci, uint32_t seq, int fd, const char *path) {
    S.cd_busy = 1; S.cd_client = ci; S.cd_seq = seq; S.cd_err[0] = 0; S.cd_new_node[0] = 0; S.cd_new_fdset = 0;
    snprintf(S.cd_path, sizeof S.cd_path, "%s", fd >= 0 ? path : "");
    int bad = qmp_do("blockdev-open-tray", "{\"id\":\"cd0\",\"force\":true}", P_CD, -1, 0, -1, -1);
    char a[256];
    if (!bad && S.cd_node[0]) {
        bad |= qmp_do("blockdev-remove-medium", "{\"id\":\"cd0\"}", P_CD, -1, 1, -1, -1);     /* step 1: the old disc is out */
        snprintf(a, sizeof a, "{\"node-name\":\"%s\"}", S.cd_node);
        bad |= qmp_do("blockdev-del", a, P_CD, -1, 0, -1, -1);
        if (!strcmp(S.cd_node, "cd")) bad |= qmp_do("blockdev-del", "{\"node-name\":\"cd-file\"}", P_CD, -1, 0, -1, -1);
        if (S.cd_fdset) { snprintf(a, sizeof a, "{\"fdset-id\":%d}", S.cd_fdset); bad |= qmp_do("remove-fd", a, P_CD, -1, 0, -1, -1); }
    }
    if (!bad && fd >= 0) {
        static int next = 20;
        int set = next++;
        snprintf(S.cd_new_node, sizeof S.cd_new_node, "cd%d", set);
        S.cd_new_fdset = set;
        snprintf(a, sizeof a, "{\"fdset-id\":%d,\"opaque\":\"cd\"}", set);
        bad |= qmp_do("add-fd", a, P_CD, -1, 0, fd, -1);
        snprintf(a, sizeof a, "{\"driver\":\"raw\",\"node-name\":\"%s\",\"read-only\":true,\"file\":{\"driver\":\"file\",\"filename\":\"/dev/fdset/%d\",\"read-only\":true}}", S.cd_new_node, set);
        bad |= qmp_do("blockdev-add", a, P_CD, -1, 0, -1, -1);
        snprintf(a, sizeof a, "{\"id\":\"cd0\",\"node-name\":\"%s\"}", S.cd_new_node);
        bad |= qmp_do("blockdev-insert-medium", a, P_CD, -1, 2, -1, -1);                        /* step 2: the new one is in */
    }
    bad |= qmp_do("blockdev-close-tray", "{\"id\":\"cd0\"}", P_CD_LAST, -1, 0, -1, -1);
    if (bad) { S.cd_busy = 0; reply(ci, seq, VME_BUSY, "busy"); }
    vlog("cd: %s%s", fd >= 0 ? "inserting " : "ejecting", fd >= 0 ? path : "");
}

static void cd_done(void) {
    S.cd_busy = 0;
    if (S.cd_err[0]) vlog("cd: failed: %s", S.cd_err);
    /* machine.json says what's in the drive for the next start (lord-vm holds the lock; nothing else writes it now) */
    VmMachine m = S.m;
    snprintf(m.iso, sizeof m.iso, "%s", !S.cd_node[0] ? "" : S.cd_fdset ? S.cd_path : S.m.iso);
    vm_now_iso(m.updated);
    if (vm_machine_check(&m, NULL, 0) || vm_machine_save(S.dir, &m)) vlog("cd: note: machine.json keeps its ISO (the path can't be kept)");
    else S.m = m;
    if (S.cd_err[0]) reply(S.cd_client, S.cd_seq, VME_QMP, S.cd_err);
    else reply(S.cd_client, S.cd_seq, VME_OK, S.cd_node[0] ? "inserted" : "ejected");
}

/* ---- suspend to disk and resume (6.1, 3.6) ----
 *
 * <dir>/suspend.state  QEMU's migration stream: memory and devices, written through a descriptor lord-vm opened
 * <dir>/suspend.json   what made it: QEMU's version, the machine type, this computer's CPU, vCPUs and memory
 * Both are written as .tmp and renamed (the state last), under operation.json "suspend". The next start resumes from
 * them (-incoming defer, migrate-incoming on a descriptor) under operation.json "resume", and removes them before the
 * vCPUs run, since the disks then move on. A state made by another QEMU build, machine type or CPU isn't loaded
 * unless asked (--resume-anyway); --discard-state removes it and the machine boots from its disks. */

static void send_job(int kind, int status, int pct, const char *text) {
    VmMsg j = vm_msg(VM_JOB, 0, kind, status, pct, 0, text);
    for (int i = 0; i < MAX_CLIENTS; i++) send_to(i, &j, -1);
}

static void notice_all(int kind, int code, const char *text) {
    S.notice = kind; S.notice_code = code;
    snprintf(S.notice_text, sizeof S.notice_text, "%s", text);
    VmMsg n = vm_msg(VM_NOTICE, 0, kind, code, 0, 0, S.notice_text);
    for (int i = 0; i < MAX_CLIENTS; i++) send_to(i, &n, -1);
}

/* this computer's CPU, as a saved state cares about it: model and feature flags (amd64), implementer, part and
 * features (arm64) of the first CPU; 16 hex digits of their SHA-256 */
static void host_cpu_id(char out[17]) {
    snprintf(out, 17, "unknown");
    int fd = open("/proc/cpuinfo", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    static char buf[65536];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    Sha256 h;
    sha256_init(&h);
    static const char *const keys[] = { "model name", "flags", "CPU implementer", "CPU architecture", "CPU variant", "CPU part", "Features", NULL };
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!*line) break;                          /* the first processor only */
        for (int k = 0; keys[k]; k++) if (!strncmp(line, keys[k], strlen(keys[k]))) { sha256_update(&h, (const uint8_t *)line, strlen(line)); sha256_update(&h, (const uint8_t *)"\n", 1); }
        line = nl ? nl + 1 : NULL;
    }
    uint8_t d[32];
    sha256_final(&h, d);
    for (int i = 0; i < 8; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
}

static void state_paths(char *st, char *meta, char *tmp, char *mtmp, size_t n) {
    snprintf(st, n, "%s/suspend.state", S.dir);
    snprintf(meta, n, "%s/suspend.json", S.dir);
    if (tmp) snprintf(tmp, n, "%s/.suspend.state.tmp", S.dir);
    if (mtmp) snprintf(mtmp, n, "%s/.suspend.json.tmp", S.dir);
}

typedef struct { char qemu[64], machine_type[32], cpu[17], saved[21]; int vcpus, memory_mb; double bytes; } SuspendMeta;

static int suspend_meta_read(const char *dir, SuspendMeta *sm) {
    memset(sm, 0, sizeof *sm);
    char p[PATH_MAX + 32], buf[4096];
    snprintf(p, sizeof p, "%s/suspend.json", dir);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    JDoc d;
    if (n <= 0 || json_parse(buf, (size_t)n, &d, NULL, 0)) return -1;
    const char *q = json_str(d.root, "qemu"), *mt = json_str(d.root, "machine_type"), *cpu = json_str(d.root, "cpu"), *sv = json_str(d.root, "saved");
    if (q) snprintf(sm->qemu, sizeof sm->qemu, "%s", q);
    if (mt) snprintf(sm->machine_type, sizeof sm->machine_type, "%s", mt);
    if (cpu) snprintf(sm->cpu, sizeof sm->cpu, "%s", cpu);
    if (sv) snprintf(sm->saved, sizeof sm->saved, "%.20s", sv);
    sm->vcpus = (int)json_num(d.root, "vcpus", 0);
    sm->memory_mb = (int)json_num(d.root, "memory_mb", 0);
    sm->bytes = json_num(d.root, "bytes", 0);
    json_free(&d);
    return q && mt && cpu ? 0 : -1;
}

/* does this start take the saved state? 1 yes; 0 there's none; -1 there is one this computer can't load (why in err) */
static int suspend_usable(char *err, size_t n) {
    char st[PATH_MAX + 32], meta[PATH_MAX + 32];
    state_paths(st, meta, NULL, NULL, sizeof st);
    if (!file_exists(st)) return 0;
    SuspendMeta sm;
    if (suspend_meta_read(S.dir, &sm)) { snprintf(err, n, "the saved state's description is missing or damaged"); return -1; }
    if (strcmp(sm.machine_type, S.m.machine_type) || sm.vcpus != S.m.vcpus || sm.memory_mb != S.m.memory_mb) {
        snprintf(err, n, "the machine changed since it was suspended (%s, %d vCPU, %d MB)", sm.machine_type, sm.vcpus, sm.memory_mb);
        return -1;
    }
    if (strcmp(sm.cpu, S.cpu_id) && !S.resume_anyway) { snprintf(err, n, "it was saved on another processor"); return -1; }
    return 1;
}

static void suspend_cleanup_tmp(void) {
    char st[PATH_MAX + 32], meta[PATH_MAX + 32], tmp[PATH_MAX + 32], mtmp[PATH_MAX + 32];
    state_paths(st, meta, tmp, mtmp, sizeof st);
    unlink(tmp); unlink(mtmp);
}

static void suspend_failed(const char *why) {
    vlog("suspend: failed: %s", why);
    if (S.susp_fd >= 0) { close(S.susp_fd); S.susp_fd = -1; }
    suspend_cleanup_tmp();
    vm_op_end(S.dir);
    S.suspending = 0;
    S.migq_next = 0;
    if (S.qmp_up && !S.qemu_done) {
        if (S.susp_was_running) qmp_do("cont", NULL, P_IGNORE, -1, 0, -1, -1);
        set_state(S.susp_was_running ? VS_RUNNING : VS_PAUSED, VR_USER, 0, "");
    }
    send_job(VJ_SUSPEND, VJS_FAILED, -1, why);
    reply(S.susp_client, S.susp_seq, VME_QMP, why);
    if (S.host_stopping && S.qmp_up && !S.qemu_done) { vlog("the machine couldn't be saved: shutting it down instead"); begin_shutdown(30); }
}

static void suspend_done(void) {
    char st[PATH_MAX + 32], meta[PATH_MAX + 32], tmp[PATH_MAX + 32], mtmp[PATH_MAX + 32];
    state_paths(st, meta, tmp, mtmp, sizeof st);
    struct stat sb;
    if (S.susp_fd < 0 || fsync(S.susp_fd) || fstat(S.susp_fd, &sb) || sb.st_size <= 0) { suspend_failed("the saved state couldn't be written to disk"); return; }
    close(S.susp_fd);
    S.susp_fd = -1;
    char now[21];
    vm_now_iso(now);
    JBuf j;
    jb_init(&j);
    jb_raw(&j, "{\"qemu\":"); jb_str(&j, S.qemu_version);
    jb_raw(&j, ",\"machine_type\":"); jb_str(&j, S.m.machine_type);
    jb_raw(&j, ",\"cpu\":"); jb_str(&j, S.cpu_id);
    jb_printf(&j, ",\"vcpus\":%d,\"memory_mb\":%d,\"bytes\":%lld,\"saved\":", S.m.vcpus, S.m.memory_mb, (long long)sb.st_size);
    jb_str(&j, now);
    jb_raw(&j, "}\n");
    int fd = j.oom ? -1 : open(mtmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    int ok = fd >= 0 && !write_all(fd, j.p, j.n) && !fsync(fd);
    if (fd >= 0) close(fd);
    jb_free(&j);
    /* the description first, then the state: a state without its description is never loaded */
    if (!ok || rename(mtmp, meta) || rename(tmp, st)) { unlink(meta); suspend_failed("the saved state couldn't be kept"); return; }
    vm_sync_dir(S.dir);
    vm_op_end(S.dir);
    S.suspending = 0;
    S.suspended = 1;
    S.migq_next = 0;
    vlog("suspend: %lld MB saved in %lld ms", (long long)sb.st_size >> 20, now_ms() - S.susp_t0);
    send_job(VJ_SUSPEND, VJS_DONE, 100, "");
    reply(S.susp_client, S.susp_seq, VME_OK, "suspended");
    if (qmp_do("quit", NULL, P_IGNORE, -1, 0, -1, -1)) { kill(S.qemu, SIGKILL); S.killed = 1; }
    S.kill_deadline = now_ms() + 5000;
}

static void begin_suspend(int ci, uint32_t seq) {
    char st[PATH_MAX + 32], meta[PATH_MAX + 32], tmp[PATH_MAX + 32], mtmp[PATH_MAX + 32];
    state_paths(st, meta, tmp, mtmp, sizeof st);
    if (vm_op_begin(S.dir, "suspend", NULL, tmp, NULL)) { reply(ci, seq, VME_INTERNAL, "can't write operation.json"); return; }
    S.susp_fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (S.susp_fd < 0) { vm_op_end(S.dir); reply(ci, seq, VME_INTERNAL, "can't make the state file"); return; }
    S.susp_client = ci; S.susp_seq = seq; S.suspending = 1; S.susp_was_running = S.state == VS_RUNNING; S.susp_t0 = now_ms();
    int bad = 0;
    if (S.susp_was_running) bad |= qmp_do("stop", NULL, P_IGNORE, -1, 0, -1, -1);
    bad |= qmp_do("migrate-set-capabilities", "{\"capabilities\":[{\"capability\":\"events\",\"state\":true}]}", P_IGNORE, -1, 0, -1, -1);
    /* QEMU limits a migration to 128 MiB/s by default: a file on this computer takes what the disk gives */
    bad |= qmp_do("migrate-set-parameters", "{\"max-bandwidth\":107374182400}", P_IGNORE, -1, 0, -1, -1);
    /* the file goes to QEMU as an fdset (QEMU 10 deprecates fd: for files): it opens /dev/fdset/90, never a path */
    bad |= qmp_do("add-fd", "{\"fdset-id\":90,\"opaque\":\"suspend\"}", P_GETFD, -1, 0, S.susp_fd, -1);
    bad |= qmp_do("migrate", "{\"uri\":\"file:/dev/fdset/90\"}", P_SUSPEND, -1, 0, -1, -1);
    if (bad) { suspend_failed("QEMU is busy"); return; }
    set_state(VS_STOPPING, VR_SUSPENDED, 0, "saving the machine's memory");
    send_job(VJ_SUSPEND, VJS_RUNNING, 0, "");
    S.migq_next = now_ms() + 500;
    vlog("suspend: saving to %s", tmp);
}

/* after the QMP handshake of a start that resumes: the saved stream goes in through a descriptor */
static void begin_resume(void) {
    char st[PATH_MAX + 32], meta[PATH_MAX + 32];
    state_paths(st, meta, NULL, NULL, sizeof st);
    int fd = open(st, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || vm_op_begin(S.dir, "resume", NULL, st, NULL)) {
        if (fd >= 0) close(fd);
        snprintf(S.resume_err, sizeof S.resume_err, "can't read the saved state");
        kill(S.qemu, SIGKILL); S.killed = 1;
        return;
    }
    int bad = qmp_do("migrate-set-capabilities", "{\"capabilities\":[{\"capability\":\"events\",\"state\":true}]}", P_IGNORE, -1, 0, -1, -1);
    bad |= qmp_do("add-fd", "{\"fdset-id\":91,\"opaque\":\"resume\"}", P_GETFD, -1, 0, fd, -1);
    bad |= qmp_do("migrate-incoming", "{\"uri\":\"file:/dev/fdset/91\"}", P_RESUME, -1, 0, -1, -1);
    close(fd);
    if (bad) { snprintf(S.resume_err, sizeof S.resume_err, "QEMU didn't take the saved state"); kill(S.qemu, SIGKILL); S.killed = 1; return; }
    send_job(VJ_RESUME, VJS_RUNNING, -1, "");
    S.susp_t0 = now_ms();
    vlog("resume: loading %s", st);
}

static void resume_done(void) {
    char st[PATH_MAX + 32], meta[PATH_MAX + 32];
    state_paths(st, meta, NULL, NULL, sizeof st);
    /* from here the disks move on: the state is no longer valid, and goes before a single vCPU runs */
    unlink(st); unlink(meta);
    vm_sync_dir(S.dir);
    vm_op_end(S.dir);
    S.resumed = 1;
    vlog("resume: loaded in %lld ms", now_ms() - S.susp_t0);
    send_job(VJ_RESUME, VJS_DONE, 100, "");
    if (S.paused_start) set_state(VS_PAUSED, VR_NONE, 0, "resumed");
    else qmp_do("cont", NULL, P_CONT_START, -1, 0, -1, -1);
}

/* ---- the network heals itself (6.5) ----
 * passt ended (a crash, a kill, out of memory) while the machine runs: the keeper starts a new one, the old backend
 * comes off QEMU's hub (netdev_del hp<n>, pst<n>) and the new one goes on (getfd, netdev_add socket pst<n+1>, hubport
 * hp<n+1>). The card and the guest's interface don't change: its connections broke, the network is back. QEMU's
 * "stream" backend can't take a descriptor by name once QEMU runs (it connects from a thread); "socket" can, and passt
 * speaks the same framing. At most 5 restarts a minute; after that the machine has no network until it starts again. */
#define NET_MAX_RESTARTS 5

static void runinfo_now(void) { set_state(S.state, S.reason, S.code, NULL); }

static void net_lost(int sig, int code) {
    if (S.passt_pidfd >= 0) { close(S.passt_pidfd); S.passt_pidfd = -1; }
    S.passt_ext = 0;
    if (S.qemu_done || S.state == VS_STOPPING || S.host_stopping || !S.qmp_up) { vlog("network: passt ended (%s %d) while the machine stops", sig ? "signal" : "status", code); return; }
    long long now = now_ms();
    if (!S.net_window_at || now - S.net_window_at > 60000) { S.net_window_at = now; S.net_restarts = 0; }
    char t[160];
    if (S.keeper_fd < 0 || S.net_restarts >= NET_MAX_RESTARTS) {
        S.net_gave_up = 1;
        snprintf(t, sizeof t, "passt ended (%s %d)%s: the machine has no network until it starts again", sig ? "signal" : "status", code,
                 S.keeper_fd < 0 ? "" : ", again and again");
        vlog("network: %s", t);
        notice_all(VMN_NET_LOST, code, t);
        return;
    }
    snprintf(t, sizeof t, "passt ended (%s %d): reconnecting", sig ? "signal" : "status", code);
    vlog("network: %s", t);
    if (!S.net_lost_at) S.net_lost_at = now;
    notice_all(VMN_NET_LOST, code, t);
    /* right away the first time, then a little later each time */
    S.net_retry_at = now + (S.net_restarts == 0 ? 0 : 1000LL << (S.net_restarts - 1));
}

static void net_restart(void) {
    S.net_retry_at = 0;
    if (S.qemu_done || !S.qmp_up || S.keeper_fd < 0) return;
    char c = 'p';
    if (write(S.keeper_fd, &c, 1) != 1) { vlog("network: passt's keeper is gone"); close(S.keeper_fd); S.keeper_fd = -1; net_lost(0, 0); return; }
    struct pollfd pf = { S.keeper_fd, POLLIN, 0 };
    int32_t pid = -1;
    int fds[3] = { -1, -1, -1 };
    if (poll(&pf, 1, 3000) == 1) {
        union { struct cmsghdr h; char b[CMSG_SPACE(sizeof fds)]; } cm;
        struct iovec io = { &pid, sizeof pid };
        struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cm.b, .msg_controllen = sizeof cm.b };
        ssize_t n = recvmsg(S.keeper_fd, &mh, MSG_CMSG_CLOEXEC);
        struct cmsghdr *ch = n == sizeof pid ? CMSG_FIRSTHDR(&mh) : NULL;
        if (ch && ch->cmsg_level == SOL_SOCKET && ch->cmsg_type == SCM_RIGHTS && ch->cmsg_len == CMSG_LEN(sizeof fds)) memcpy(fds, CMSG_DATA(ch), sizeof fds);
        else pid = -1;
    }
    S.net_restarts++;
    if (pid <= 0 || fds[0] < 0) {
        for (int i = 0; i < 3; i++) if (fds[i] >= 0) close(fds[i]);
        vlog("network: a new passt couldn't be started");
        net_lost(0, 0);
        return;
    }
    S.passt_ext = pid;
    S.passt_pidfd = fds[2];
    if (S.passt_err >= 0) close(S.passt_err);
    S.passt_err = fds[1];
    fcntl(S.passt_err, F_SETFL, O_NONBLOCK);
    int old = S.net_gen, gen = ++S.net_gen;
    char a[160];
    S.net_err[0] = 0;
    snprintf(a, sizeof a, "{\"id\":\"hp%d\"}", old);
    qmp_do("netdev_del", a, P_NET, -1, 0, -1, -1);
    snprintf(a, sizeof a, "{\"id\":\"pst%d\"}", old);
    qmp_do("netdev_del", a, P_NET, -1, 0, -1, -1);
    snprintf(a, sizeof a, "{\"fdname\":\"pst%d\"}", gen);
    int bad = qmp_do("getfd", a, P_NET_ADD, -1, 0, fds[0], -1);
    close(fds[0]);
    snprintf(a, sizeof a, "{\"type\":\"socket\",\"id\":\"pst%d\",\"fd\":\"pst%d\"}", gen, gen);
    bad |= qmp_do("netdev_add", a, P_NET_ADD, -1, 0, -1, -1);
    snprintf(a, sizeof a, "{\"type\":\"hubport\",\"id\":\"hp%d\",\"hubid\":0,\"netdev\":\"pst%d\"}", gen, gen);
    bad |= qmp_do("netdev_add", a, P_NET_LAST, -1, 0, -1, -1);
    S.net_busy = !bad;
    vlog("network: passt %d started (backend pst%d)", (int)pid, gen);
    runinfo_now();
    if (bad) { kill(pid, SIGKILL); }
}

/* the last netdev_add answered */
static void net_attached(int ok, const char *desc) {
    S.net_busy = 0;
    if (!ok && !S.net_err[0]) snprintf(S.net_err, sizeof S.net_err, "%s", desc);
    if (S.net_err[0]) {
        vlog("network: QEMU didn't take the new passt: %s", S.net_err);
        if (S.passt_pidfd >= 0) syscall(SYS_pidfd_send_signal, S.passt_pidfd, SIGKILL, NULL, 0);   /* its end comes back as net_lost */
        return;
    }
    long long ms = S.net_lost_at ? now_ms() - S.net_lost_at : 0;
    S.net_lost_at = 0;
    char t[160];
    snprintf(t, sizeof t, "the network is back (passt restarted, %lld ms after it ended)", ms);
    vlog("network: %s", t);
    notice_all(VMN_NET_BACK, (int)ms, t);
}

/* ---- the computer stops, or the session ends (6.1) ----
 * The machine is saved to disk (a suspend: the next start resumes it), within 30 s; if it can't be, it's shut down
 * (ACPI, 30 s), then turned off. Comes from logind's PrepareForShutdown (lord-vm holds a delay inhibitor), from
 * SIGTERM (the session's units stopping) and from lord-server going away without saying Keep Running. */
#define HOST_STOP_SAVE_MS 30000

static void host_stop(const char *why) {
    if (S.qemu_done || S.suspended) return;
    if (!S.host_stopping) {
        S.host_stopping = 1;
        snprintf(S.host_stop_why, sizeof S.host_stop_why, "%s", why);
        S.host_stop_deadline = now_ms() + HOST_STOP_SAVE_MS;
        vlog("%s: saving the machine to disk", why);
    }
    if (S.suspending) return;                                   /* already saving */
    if (!S.qmp_up || S.state == VS_STARTING) { begin_poweroff(); return; }
    if (S.state == VS_STOPPING) return;                         /* already shutting down */
    char err[160];
    int ok_state = S.state == VS_RUNNING || S.state == VS_PAUSED;
    if (ok_state && !S.snap_kind && !S.cd_busy) { begin_suspend(-1, 0); return; }
    if (ok_state && (S.snap_kind || S.cd_busy)) return;         /* tried again from the loop once that's done */
    snprintf(err, sizeof err, "the machine can't be saved now (%s): shutting it down", STATE_NAME[S.state]);
    vlog("%s", err);
    begin_shutdown(30);
}

/* the loop's turn: a save that takes too long is given up for a shutdown */
static void host_stop_tick(long long now) {
    if (!S.host_stopping || S.qemu_done) return;
    if (S.suspending && now >= S.host_stop_deadline) {
        vlog("suspend: not done in %d s: shutting the machine down instead", HOST_STOP_SAVE_MS / 1000);
        qmp_do("migrate_cancel", NULL, P_IGNORE, -1, 0, -1, -1);
        S.host_stop_deadline = now + 600000;
        return;
    }
    if (!S.suspending && S.state != VS_STOPPING && (S.state == VS_RUNNING || S.state == VS_PAUSED) && !S.snap_kind && !S.cd_busy) {
        if (now < S.host_stop_deadline) begin_suspend(-1, 0);
        else begin_shutdown(30);
    }
}

#ifdef VM_LOGIND
/* ---- logind (6.1, 6.14): while the machine runs, the computer's shutdown waits for it to be saved (a delay
 * inhibitor), and it doesn't go to sleep when idle (an idle block inhibitor, while running and "Keep this computer
 * awake" is on). Both are logind's, so `systemd-inhibit --list` names them; closing the descriptor lets go. ---- */
static sd_bus *sysbus;
static int inh_shutdown = -1, inh_idle = -1;

static int inhibit(const char *what, const char *why, const char *mode) {
    sd_bus_message *rep = NULL;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int fd = -1, got = -1;
    if (sd_bus_call_method(sysbus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit", &e, &rep,
                           "ssss", what, "LordOS Machines", why, mode) >= 0 && sd_bus_message_read(rep, "h", &got) >= 0)
        fd = fcntl(got, F_DUPFD_CLOEXEC, 3);
    else vlog("logind: no %s inhibitor: %s", what, e.message ? e.message : "?");
    sd_bus_error_free(&e);
    sd_bus_message_unref(rep);
    return fd;
}

static int on_prepare_shutdown(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int start = 0;
    if (sd_bus_message_read(m, "b", &start) >= 0 && start) host_stop("the computer is shutting down");
    return 0;
}

static void awake_update(void) {
    if (!sysbus) return;
    int want = S.keep_awake && S.state == VS_RUNNING && !S.qemu_done && !S.host_stopping;
    if (want && inh_idle < 0) {
        char why[160];
        snprintf(why, sizeof why, "The machine \"%.80s\" is running", S.m.name);
        inh_idle = inhibit("idle", why, "block");
        if (inh_idle >= 0) vlog("logind: this computer doesn't go to sleep when idle while the machine runs");
    } else if (!want && inh_idle >= 0) {
        close(inh_idle);
        inh_idle = -1;
        vlog("logind: idle sleep allowed again");
    }
}

static void logind_open(void) {
    if (sd_bus_open_system(&sysbus) < 0) { vlog("logind: no system bus: host shutdown isn't waited for"); sysbus = NULL; return; }
    sd_bus_match_signal(sysbus, NULL, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "PrepareForShutdown",
                        on_prepare_shutdown, NULL);
    char why[160];
    snprintf(why, sizeof why, "The machine \"%.80s\" is saved to disk first", S.m.name);
    inh_shutdown = inhibit("shutdown", why, "delay");
    if (inh_shutdown >= 0) vlog("logind: a shutdown or restart of this computer waits for the machine to be saved");
    awake_update();
}

static int logind_fd(void) { return sysbus ? sd_bus_get_fd(sysbus) : -1; }
static short logind_events(void) { return sysbus ? (short)sd_bus_get_events(sysbus) : 0; }
static void logind_process(void) {
    if (!sysbus) return;
    int r;
    while ((r = sd_bus_process(sysbus, NULL)) > 0) ;
    if (r < 0) { vlog("logind: the system bus went away"); sd_bus_flush_close_unref(sysbus); sysbus = NULL; }
}
static void logind_close(void) {
    if (inh_idle >= 0) close(inh_idle);
    if (inh_shutdown >= 0) close(inh_shutdown);
    inh_idle = inh_shutdown = -1;
    if (sysbus) sd_bus_flush_close_unref(sysbus);
    sysbus = NULL;
}
#else
static void awake_update(void) {}
static void logind_open(void) { vlog("logind: not built in: host shutdown isn't waited for"); }
static int logind_fd(void) { return -1; }
static short logind_events(void) { return 0; }
static void logind_process(void) {}
static void logind_close(void) {}
#endif

/* ---- snapshots while the machine runs (6.2): QMP jobs over every writable node, memory into the system disk ---- */

static const char *snap_devices(void) { return S.m.firmware == VM_FW_UEFI ? "[\"sys\",\"fw-vars\"]" : "[\"sys\"]"; }

static void snap_end(int ok, const char *why) {
    int kind = S.snap_kind == VSN_TAKE ? VJ_SNAPSHOT : S.snap_kind == VSN_REVERT ? VJ_REVERT : VJ_DELETE;
    if (ok) vlog("snapshot: %s %s done", S.snap_kind == VSN_TAKE ? "take" : S.snap_kind == VSN_REVERT ? "revert" : "delete", S.snap_tag);
    else vlog("snapshot: %s failed: %s", S.snap_tag, why);
    send_job(kind, ok ? VJS_DONE : VJS_FAILED, ok ? 100 : -1, ok ? S.snap_tag : why);
    reply(S.snap_client, S.snap_seq, ok ? VME_OK : VME_QMP, ok ? S.snap_tag : why);
    S.snap_kind = 0;
}

/* the job ended: what QEMU says about it (query-jobs), then it's dismissed */
static void snap_concluded(const JVal *root, int ok) {
    const char *err = NULL;
    const JVal *arr = json_get(root, "return");
    for (const JVal *e = ok && arr && arr->type == J_ARR ? arr->kid : NULL; e; e = e->next) {
        const char *id = json_str(e, "id");
        if (id && !strcmp(id, "snap")) err = json_str(e, "error");
    }
    snprintf(S.snap_err, sizeof S.snap_err, "%s", err ? err : "");
    qmp_do("job-dismiss", "{\"id\":\"snap\"}", P_IGNORE, -1, 0, -1, -1);
    if (S.snap_kind == VSN_TAKE) {
        if (err) { vm_op_end(S.dir); snap_end(0, err); return; }     /* a tag left on a disk: the next start's sweep */
        qmp_do("query-block", NULL, P_SNAPSIZE, -1, 0, -1, -1);
    } else if (S.snap_kind == VSN_REVERT) {
        if (S.loadvm[0]) {
            /* a start from a snapshot: then the machine goes on as any start does */
            S.loadvm[0] = 0;
            if (err) { snap_end(0, err); fail_start("the snapshot couldn't be loaded: %.100s", err); kill(S.qemu, SIGKILL); S.killed = 1; return; }
            vm_op_end(S.dir);
            snap_end(1, "");
            if (S.paused_start) set_state(VS_PAUSED, VR_NONE, 0, "started from a snapshot");
            else qmp_do("cont", NULL, P_CONT_START, -1, 0, -1, -1);
            return;
        }
        if (err) {
            /* the disks may be half-way: the machine stops, operation.json stays, and the next start finishes going back */
            snap_end(0, err);
            begin_poweroff();
            return;
        }
        vm_op_end(S.dir);
        snap_end(1, "");
    } else {
        snap_picture_rm(S.dir, S.snap_tag);
        vm_op_end(S.dir);
        snap_end(!err, err ? err : "");
    }
}

/* after a take: the size of its memory (query-block, the system disk's snapshot list), then the entry, last */
static void snap_taken(const JVal *root) {
    double vmstate = 0;
    const JVal *arr = json_get(root, "return");
    for (const JVal *e = arr && arr->type == J_ARR ? arr->kid : NULL; e; e = e->next) {
        const JVal *ins = json_get(e, "inserted");
        const char *nn = ins ? json_str(ins, "node-name") : NULL;
        if (!nn || strcmp(nn, "sys")) continue;
        const JVal *sn = json_get(json_get(ins, "image"), "snapshots");
        for (const JVal *x = sn && sn->type == J_ARR ? sn->kid : NULL; x; x = x->next) {
            const char *name = json_str(x, "name");
            if (name && !strcmp(name, S.snap_tag)) vmstate = json_num(x, "vm-state-size", 0);
        }
    }
    VmSnap list[VM_SNAPS_MAX];
    int n = vm_snaps_load(S.dir, list, VM_SNAPS_MAX);
    if (n >= VM_SNAPS_MAX) { vm_op_end(S.dir); snap_end(0, "too many snapshots"); return; }
    VmSnap sn;
    memset(&sn, 0, sizeof sn);
    snprintf(sn.tag, sizeof sn.tag, "%s", S.snap_tag);
    snprintf(sn.name, sizeof sn.name, "%s", S.snap_name);
    vm_now_iso(sn.created);
    sn.memory = 1;
    sn.vmstate = vmstate;
    list[n++] = sn;
    if (vm_snaps_save(S.dir, list, n)) { vm_op_end(S.dir); snap_end(0, "can't write snapshots.json"); return; }
    vm_op_end(S.dir);
    snap_end(1, "");
}

static void begin_snapshot(int ci, uint32_t seq, int kind, const char *text) {
    char tag[64] = "", name[VM_NAME_MAX + 1] = "";
    const char *tab = strchr(text, '\t');
    snprintf(tag, sizeof tag, "%.*s", tab ? (int)(tab - text) : (int)strlen(text), text);
    if (tab) snprintf(name, sizeof name, "%s", tab + 1);
    if (!vm_snap_tag_valid(tag) || (kind == VSN_TAKE && !vm_name_valid(name))) { reply(ci, seq, VME_BAD_REQUEST, "a snapshot needs a tag and a name"); return; }
    VmSnap list[VM_SNAPS_MAX];
    int n = vm_snaps_load(S.dir, list, VM_SNAPS_MAX), i = vm_snaps_find(list, n, tag);
    if (kind == VSN_TAKE && (i >= 0 || n >= VM_SNAPS_MAX)) { reply(ci, seq, VME_BAD_REQUEST, i >= 0 ? "that tag exists" : "too many snapshots"); return; }
    if (kind != VSN_TAKE && i < 0) { reply(ci, seq, VME_BAD_REQUEST, "there's no such snapshot"); return; }
    if (kind == VSN_REVERT && !list[i].memory) { reply(ci, seq, VME_WRONG_STATE, "it was taken with the machine off: shut it down to go back to it"); return; }
    const char *op = kind == VSN_TAKE ? "snapshot-take" : kind == VSN_REVERT ? "snapshot-revert" : "snapshot-delete";
    if (vm_op_begin(S.dir, op, tag, NULL, NULL)) { reply(ci, seq, VME_INTERNAL, "can't write operation.json"); return; }
    if (kind == VSN_DELETE) {
        /* the entry goes first: a tag that snapshots.json doesn't list is never shown, and the sweep removes it */
        memmove(&list[i], &list[i + 1], sizeof list[0] * (size_t)(n - i - 1));
        if (vm_snaps_save(S.dir, list, n - 1)) { vm_op_end(S.dir); reply(ci, seq, VME_INTERNAL, "can't write snapshots.json"); return; }
    }
    S.snap_kind = kind; S.snap_client = ci; S.snap_seq = seq;
    snprintf(S.snap_tag, sizeof S.snap_tag, "%s", tag);
    snprintf(S.snap_name, sizeof S.snap_name, "%s", name);
    char a[512];
    const char *cmd = kind == VSN_TAKE ? "snapshot-save" : kind == VSN_REVERT ? "snapshot-load" : "snapshot-delete";
    if (kind == VSN_DELETE) snprintf(a, sizeof a, "{\"job-id\":\"snap\",\"tag\":\"%s\",\"devices\":%s}", tag, snap_devices());
    else snprintf(a, sizeof a, "{\"job-id\":\"snap\",\"tag\":\"%s\",\"vmstate\":\"sys\",\"devices\":%s}", tag, snap_devices());
    if (qmp_do(cmd, a, P_SNAPJOB, -1, 0, -1, -1)) { vm_op_end(S.dir); S.snap_kind = 0; reply(ci, seq, VME_BUSY, "busy"); return; }
    send_job(kind == VSN_TAKE ? VJ_SNAPSHOT : kind == VSN_REVERT ? VJ_REVERT : VJ_DELETE, VJS_RUNNING, -1, tag);
    vlog("snapshot: %s %s \"%s\"", cmd, tag, name);
}

/* ---- the guest agent (6.17, 7) ----
 *
 * qemu-guest-agent (Debian's qemu-guest-agent package, in the guest) answers on org.qemu.guest_agent.0: lord-vm syncs
 * with it when the guest opens the port (VSERPORT_CHANGE) or every 10 s until it answers, then reads its version and
 * the system's name, sets the guest's clock (also after a resume, an unpause or going back to a snapshot), keeps the
 * guest from sleeping while the machine runs (systemd-inhibit, unless Configure says otherwise), follows the journal
 * (journalctl through guest-exec every 5 s, into log/journal.log), shuts the guest down when ACPI didn't, and pings it
 * every 10 s. lordos-guest answers on org.lordos.agent (vmagent.h) for the clipboard. Everything the guest sends is
 * untrusted: lines and frames have hard limits, and only text goes on to lord-server. */
enum { AQ_IGNORE = 0, AQ_SYNC, AQ_INFO, AQ_OSINFO, AQ_PING, AQ_JEXEC, AQ_JSTATUS, AQ_FOPEN, AQ_FWRITE, AQ_FCLOSE, AQ_INSTALL, AQ_INHIBIT };

static void ag_broadcast(void) {
    char t[160];
    snprintf(t, sizeof t, "%s\t%s", S.ag_version, S.ag_os);
    VmMsg m = vm_msg(VM_AGENT_STATE, 0, S.ag_state, S.ag_caps, 0, 0, t);
    for (int i = 0; i < MAX_CLIENTS; i++) send_to(i, &m, -1);
}

static void ag_set(int state) {
    if (state == S.ag_state) return;
    S.ag_state = state;
    set_state(S.state, S.reason, S.code, NULL);         /* run.json says it too: the library's agent light */
    vlog("agent: %s%s%s", state == VAS_CONNECTED ? "connected" : state == VAS_WAITING ? "waiting" : "off", state == VAS_CONNECTED ? ", " : "", state == VAS_CONNECTED ? S.ag_version : "");
    ag_broadcast();
}

static int qga_do(const char *cmd, const char *args, int kind) {
    if (S.qga_fd < 0 || S.agq_n >= 32) return -1;
    if (qmp_command(&S.qga, cmd, args, -1)) return -1;
    if (kind >= 0) { S.agq[(S.agq_head + S.agq_n) % 32] = kind; S.agq_n++; }
    return 0;
}

static void qga_sync(void) {
    char a[64];
    S.agq_n = 0;                        /* nothing sent before is waited for any more */
    snprintf(a, sizeof a, "{\"id\":%ld}", ++S.sync_id);
    qga_do("guest-sync", a, AQ_SYNC);
    S.sync_at = now_ms();
}

static void ag_timesync(void) {
    if (!S.qga_up) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char a[64];
    snprintf(a, sizeof a, "{\"time\":%lld}", (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec);
    qga_do("guest-set-time", a, AQ_IGNORE);
}

static void ag_lost(const char *why) {
    if (S.qga_up) vlog("agent: %s", why);
    S.qga_up = 0; S.agq_n = 0; S.jpid = 0; S.inhibit_done = 0;
    S.ag_caps &= VAC_CLIPBOARD;
    ag_set(VAS_WAITING);
}

static void journal_append(const uint8_t *data, size_t n) {
    char p[PATH_MAX + 32], p1[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/log/journal.log", S.dir);
    struct stat st;
    if (!stat(p, &st) && st.st_size > (4 << 20)) { snprintf(p1, sizeof p1, "%s/log/journal.1.log", S.dir); rename(p, p1); }
    int fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    /* the cursor line ("-- cursor: s=…") is ours to keep, the rest goes to the file */
    const uint8_t *line = data, *end = data + n;
    while (line < end) {
        const uint8_t *nl = memchr(line, '\n', (size_t)(end - line));
        size_t l = nl ? (size_t)(nl - line) : (size_t)(end - line);
        if (l > 11 && !memcmp(line, "-- cursor: ", 11)) { if (l - 11 < sizeof S.jcursor) { memcpy(S.jcursor, line + 11, l - 11); S.jcursor[l - 11] = 0; } }
        else if (l && !(l > 3 && !memcmp(line, "-- ", 3))) { write_all(fd, line, l); write_all(fd, "\n", 1); }
        line = nl ? nl + 1 : end;
    }
    close(fd);
}

static void ag_install_step(void);

static void on_qga_reply(const JVal *root, int ok) {
    if (!S.agq_n) return;
    int kind = S.agq[S.agq_head];
    S.agq_head = (S.agq_head + 1) % 32;
    S.agq_n--;
    const JVal *r = json_get(root, "return");
    switch (kind) {
    case AQ_SYNC:
        if (ok && r && r->type == J_NUM && (long)r->num == S.sync_id) {
            S.qga_up = 1;
            S.ping_at = now_ms();
            qga_do("guest-info", NULL, AQ_INFO);
            qga_do("guest-get-osinfo", NULL, AQ_OSINFO);
            ag_timesync();
        }
        break;
    case AQ_INFO: {
        const char *v = ok ? json_str(r, "version") : NULL;
        snprintf(S.ag_version, sizeof S.ag_version, "%.40s", v ? v : "");
        int caps = S.ag_caps & VAC_CLIPBOARD;
        const JVal *cmds = json_get(r, "supported_commands");
        for (const JVal *e = cmds && cmds->type == J_ARR ? cmds->kid : NULL; e; e = e->next) {
            const char *n = json_str(e, "name");
            if (!n || !json_bool(e, "enabled", 0)) continue;
            if (!strcmp(n, "guest-shutdown")) caps |= VAC_SHUTDOWN;
            else if (!strcmp(n, "guest-set-time")) caps |= VAC_TIME;
            else if (!strcmp(n, "guest-exec")) caps |= VAC_EXEC | VAC_JOURNAL | (S.keep_awake ? VAC_AWAKE : 0);
            else if (!strcmp(n, "guest-file-write")) caps |= VAC_FILES;
        }
        S.ag_caps = caps;
        break;
    }
    case AQ_OSINFO: {
        const char *n = ok ? json_str(r, "pretty-name") : NULL;
        if (!n && ok) n = json_str(r, "name");
        /* the guest's words, shown as text: printable ASCII and UTF-8 only, short */
        char clean[96] = "";
        size_t k = 0;
        for (const unsigned char *q = (const unsigned char *)(n ? n : ""); *q && k + 1 < sizeof clean; q++) if (*q >= 0x20 && *q != 0x7f) clean[k++] = (char)*q;
        clean[k] = 0;
        if (!ag_utf8_ok((const uint8_t *)clean, k)) clean[0] = 0;
        snprintf(S.ag_os, sizeof S.ag_os, "%s", clean);
        S.ag_state = -1;
        ag_set(VAS_CONNECTED);
        if (S.keep_awake && (S.ag_caps & VAC_EXEC) && !S.inhibit_done) {
            S.inhibit_done = 1;
            qga_do("guest-exec", "{\"path\":\"systemd-inhibit\",\"arg\":[\"--what=sleep:idle\",\"--who=LordOS Machines\","
                   "\"--why=The machine is kept awake while it runs\",\"--mode=block\",\"sleep\",\"infinity\"]}", AQ_INHIBIT);
        }
        break;
    }
    case AQ_PING: if (ok) S.ping_at = now_ms(); break;
    case AQ_JEXEC: S.jpid = ok ? (long)json_num(r, "pid", 0) : 0; if (!ok) S.ag_caps &= ~VAC_JOURNAL; break;
    case AQ_JSTATUS:
        if (!ok) { S.jpid = 0; break; }
        if (!json_bool(r, "exited", 0)) break;                /* asked again at the next turn */
        {
            const char *b = json_str(r, "out-data");
            if (b) {
                size_t bl = strlen(b);
                uint8_t *out = malloc(bl * 3 / 4 + 4);
                long n = out ? ag_b64_decode(b, bl, out, bl * 3 / 4 + 4) : -1;
                if (n > 0) journal_append(out, (size_t)n);
                free(out);
            }
            S.jpid = 0;
        }
        break;
    case AQ_INHIBIT: if (ok) vlog("agent: the guest is kept awake while the machine runs"); break;
    case AQ_FOPEN:
        if (!ok) { reply(S.install_client, S.install_seq, VME_QMP, qmp_error_desc(root)); S.helper_handle = 0; break; }
        S.helper_handle = (long)(r && r->type == J_NUM ? r->num : 0);
        S.helper_off = 0;
        ag_install_step();
        break;
    case AQ_FWRITE: if (!ok) { reply(S.install_client, S.install_seq, VME_QMP, qmp_error_desc(root)); S.helper_handle = 0; } else ag_install_step(); break;
    case AQ_FCLOSE: if (ok) ag_install_step(); else { reply(S.install_client, S.install_seq, VME_QMP, qmp_error_desc(root)); S.helper_handle = 0; } break;
    case AQ_INSTALL: reply(S.install_client, S.install_seq, ok ? VME_OK : VME_QMP, ok ? "installed" : qmp_error_desc(root)); S.helper_handle = 0; vlog("agent: lordos-guest %s", ok ? "installed" : "couldn't be installed"); break;
    default: break;
    }
}

/* the steps of VAG_INSTALL: lordos-guest written in 32 KB pieces through guest-file-write, then one fixed script */
static const char INSTALL_SH[] =
    "set -e; mv -f /usr/local/bin/.lordos-guest.new /usr/local/bin/lordos-guest; chmod 755 /usr/local/bin/lordos-guest; "
    "printf '%s\\n' 'SUBSYSTEM==\"virtio-ports\", ATTR{name}==\"org.lordos.agent\", TAG+=\"uaccess\"' > /etc/udev/rules.d/71-lordos-guest.rules; "
    "mkdir -p /etc/systemd/user; printf '%s\\n' '[Unit]' 'Description=LordOS Machines clipboard' 'After=graphical-session.target' "
    "'[Service]' 'ExecStart=/usr/local/bin/lordos-guest' 'Restart=on-failure' 'RestartSec=3' '[Install]' 'WantedBy=graphical-session.target' "
    "> /etc/systemd/user/lordos-guest.service; udevadm control --reload || true; udevadm trigger --subsystem-match=virtio-ports || true; "
    "systemctl --global enable lordos-guest.service || true";
static void ag_install_step(void) {
    if (S.helper_off < S.helper_n) {
        size_t n = S.helper_n - S.helper_off < 32768 ? S.helper_n - S.helper_off : 32768;
        static char b64[32768 / 3 * 4 + 64], args[32768 / 3 * 4 + 128];
        if (ag_b64_encode(S.helper + S.helper_off, n, b64, sizeof b64) < 0) return;
        snprintf(args, sizeof args, "{\"handle\":%ld,\"buf-b64\":\"%s\"}", S.helper_handle, b64);
        S.helper_off += n;
        qga_do("guest-file-write", args, AQ_FWRITE);
        return;
    }
    if (S.helper_handle) {
        char a[64];
        snprintf(a, sizeof a, "{\"handle\":%ld}", S.helper_handle);
        S.helper_handle = 0;
        qga_do("guest-file-close", a, AQ_FCLOSE);
        return;
    }
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"path\":\"/bin/sh\",\"arg\":[\"-c\",");
    jb_str(&b, INSTALL_SH);
    jb_raw(&b, "]}");
    if (!b.oom) qga_do("guest-exec", b.p, AQ_INSTALL);
    jb_free(&b);
}

static void drain_qga(void) {
    if (qmp_fill(&S.qga) < 0) { close(S.qga_fd); S.qga_fd = -1; ag_set(VAS_OFF); return; }
    JDoc d;
    while (qmp_next(&S.qga, &d) > 0) {
        int k = qmp_kind(d.root);
        if (k == QMP_MSG_RETURN || k == QMP_MSG_ERROR) on_qga_reply(d.root, k == QMP_MSG_RETURN);
        json_free(&d);
    }
}

/* org.lordos.agent: frames from lordos-guest */
/* frames to the guest go through a queue: a picture is more than the port takes at once, and lord-vm never waits */
static uint8_t *lag_out;
static size_t lag_out_n, lag_out_off;
static void lag_flush(void) {
    while (S.lag_fd >= 0 && lag_out_off < lag_out_n) {
        ssize_t w = write(S.lag_fd, lag_out + lag_out_off, lag_out_n - lag_out_off);
        if (w > 0) { lag_out_off += (size_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) return;
        break;
    }
    lag_out_n = lag_out_off = 0;
}
static void lag_send(uint16_t kind, const uint8_t *data, uint32_t n) {
    if (S.lag_fd < 0) return;
    size_t need = lag_out_n + AG_HEAD + n;
    if (need > 2 * (size_t)AG_FRAME_MAX + 2 * AG_HEAD) { vlog("agent: the queue to the guest is full: a frame dropped"); return; }
    uint8_t *nb = realloc(lag_out, need);
    if (!nb) return;
    lag_out = nb;
    ag_frame_head(lag_out + lag_out_n, kind, 0, n);
    if (n) memcpy(lag_out + lag_out_n + AG_HEAD, data, n);
    lag_out_n = need;
    lag_flush();
}

static int clip_allows(int to_guest) {
    return S.m.clipboard == VM_CLIP_BOTH || S.m.clipboard == (to_guest ? VM_CLIP_TO_GUEST : VM_CLIP_FROM_GUEST);
}

/* A clipboard in AGK_CLIPX's layout, from either side, made safe to pass on: text that is UTF-8, HTML sanitized (from
 * UTF-8), a PNG whose header is sane (lord-server re-encodes it in lord-media's sandbox before anything decodes it for
 * real). The kept parts again in that layout, in *out (malloc); 0 something is left, -1 nothing usable. */
static int clip_clean(const uint8_t *in, size_t n, uint8_t **out, size_t *on, char *what, size_t wn) {
    AgClip c, k;
    memset(&k, 0, sizeof k);
    if (ag_clipx_parse(in, n, &c)) { snprintf(what, wn, "malformed"); return -1; }
    static char html[AG_TEXT_MAX + 1];
    uint32_t w = 0, h = 0;
    if (c.text && ag_utf8_ok(c.text, c.text_n)) { k.text = c.text; k.text_n = c.text_n; }
    if (c.html && ag_utf8_ok(c.html, c.html_n)) {
        long hn = ag_html_sanitize((const char *)c.html, c.html_n, html, sizeof html);
        if (hn > 0) { k.html = (const uint8_t *)html; k.html_n = (uint32_t)hn; }
    }
    if (c.png && !ag_png_check(c.png, c.png_n, &w, &h)) { k.png = c.png; k.png_n = c.png_n; }
    snprintf(what, wn, "%s%s%s", k.text ? "text " : "", k.html ? "html " : "", k.png ? "png " : "");
    if (k.png) { size_t l = strlen(what); snprintf(what + l, wn - l, "%ux%u ", w, h); }
    if (c.html && !k.html) { size_t l = strlen(what); snprintf(what + l, wn - l, "(html dropped) "); }
    if (c.png && !k.png) { size_t l = strlen(what); snprintf(what + l, wn - l, "(picture refused) "); }
    size_t sz = ag_clipx_size(&k);
    if (!sz) { snprintf(what, wn, "nothing usable"); return -1; }
    *out = malloc(sz);
    if (!*out) return -1;
    ag_clipx_write(&k, *out);
    *on = sz;
    return 0;
}

static void drain_lag(void) {
    if (!S.lag_buf) { S.lag_buf = malloc(AG_FRAME_MAX + AG_HEAD); S.lag_n = 0; if (!S.lag_buf) return; }
    ssize_t r = read(S.lag_fd, S.lag_buf + S.lag_n, AG_FRAME_MAX + AG_HEAD - S.lag_n);
    if (r <= 0) { if (r == 0 || (errno != EAGAIN && errno != EINTR)) { close(S.lag_fd); S.lag_fd = -1; } return; }
    S.lag_n += (size_t)r;
    for (;;) {
        AgFrame f;
        int k = ag_frame_next(S.lag_buf, S.lag_n, &f);
        if (k < 0) { vlog("agent: org.lordos.agent sent a frame over %u bytes: dropped", AG_FRAME_MAX); S.lag_n = 0; S.lag_hello = 0; return; }
        if (!k) { if (S.lag_n == AG_FRAME_MAX + AG_HEAD) S.lag_n = 0; return; }
        if (f.kind == AGK_HELLO) {
            char t[64];
            size_t l = f.len < sizeof t - 1 ? f.len : sizeof t - 1;
            memcpy(t, f.data, l); t[l] = 0;
            int good = !strncmp(t, "lordos-guest ", 13) && ag_utf8_ok((const uint8_t *)t, l);
            if (good) {
                snprintf(S.lag_version, sizeof S.lag_version, "%.40s", t + 13);
                char *sp = strchr(S.lag_version, ' ');
                if (sp) *sp = 0;
                S.lag_clipx = strstr(t + 13, " clipx") != NULL;
                if (!S.lag_hello) vlog("agent: lordos-guest %s said hello%s", S.lag_version, S.lag_clipx ? " (pictures and rich text too)" : "");
                S.lag_hello = 1;
                S.ag_caps |= VAC_CLIPBOARD;
                static const char me[] = "lord-vm 2 clipboard clipx";
                lag_send(AGK_HELLO, (const uint8_t *)me, sizeof me - 1);
                ag_broadcast();
            }
        } else if (f.kind == AGK_CLIP && S.lag_hello && clip_allows(0) && f.len && ag_utf8_ok(f.data, f.len)) {
            int fd = memfd_create("lordos-vm-clipboard", MFD_CLOEXEC | MFD_ALLOW_SEALING);
            if (fd >= 0 && !write_all(fd, f.data, f.len) && !fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)) {
                VmMsg m = vm_msg(VM_CLIP_DATA, 0, (int)f.len, 0, 0, 0, "");
                for (int i = 0; i < MAX_CLIENTS; i++) send_to(i, &m, fd);
            }
            if (fd >= 0) close(fd);
        } else if (f.kind == AGK_CLIPX && S.lag_hello && clip_allows(0) && f.len) {
            uint8_t *clean = NULL;
            size_t cn = 0;
            char what[64];
            if (clip_clean(f.data, f.len, &clean, &cn, what, sizeof what)) vlog("agent: the guest's clipboard was refused (%s)", what);
            else {
                int fd = memfd_create("lordos-vm-clipboard", MFD_CLOEXEC | MFD_ALLOW_SEALING);
                if (fd >= 0 && !write_all(fd, clean, cn) && !fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)) {
                    VmMsg m = vm_msg(VM_CLIP_DATA, 0, (int)cn, VCF_CLIPX, 0, 0, "");
                    for (int i = 0; i < MAX_CLIENTS; i++) send_to(i, &m, fd);
                    vlog("agent: the guest's clipboard: %s", what);
                }
                if (fd >= 0) close(fd);
            }
            free(clean);
        }
        /* other kinds: skipped, so either side can grow */
        memmove(S.lag_buf, S.lag_buf + k, S.lag_n - (size_t)k);
        S.lag_n -= (size_t)k;
    }
}

/* the agent's turn of the loop: sync, ping, the journal, the shutdown fallback */
static void ag_tick(long long now) {
    S.ag_next = now + 1000;
    if (S.qga_fd < 0) return;
    if (!S.qga_up) { if (now - S.sync_at > (S.qga_open ? 3000 : 10000)) qga_sync(); return; }
    if (now - S.ping_at > 25000) { ag_lost("qemu-guest-agent stopped answering"); return; }
    if (now - S.ping_at > 10000 && !S.agq_n) qga_do("guest-ping", NULL, AQ_PING);
    if (S.shutdown_at && now >= S.shutdown_at) {
        S.shutdown_at = 0;
        if (S.state == VS_STOPPING && (S.ag_caps & VAC_SHUTDOWN)) { vlog("agent: the guest didn't answer the power button: asking qemu-guest-agent"); qga_do("guest-shutdown", "{\"mode\":\"powerdown\"}", -1); }
    }
    if ((S.ag_caps & VAC_JOURNAL) && now >= S.journal_at && !S.helper_handle && S.state == VS_RUNNING) {
        S.journal_at = now + 5000;
        if (S.jpid) { char a[48]; snprintf(a, sizeof a, "{\"pid\":%ld}", S.jpid); qga_do("guest-exec-status", a, AQ_JSTATUS); }
        else {
            JBuf b;
            jb_init(&b);
            jb_raw(&b, "{\"path\":\"journalctl\",\"arg\":[\"-o\",\"short-iso\",\"--no-pager\",\"-n\",\"300\",\"--show-cursor\"");
            if (S.jcursor[0]) { jb_raw(&b, ",\"--after-cursor\","); jb_str(&b, S.jcursor); }
            jb_raw(&b, "],\"capture-output\":true}");
            if (!b.oom) qga_do("guest-exec", b.p, AQ_JEXEC);
            jb_free(&b);
        }
    }
}

static void console_out(const char *b, size_t n);
static void handle_request(int ci, const VmMsg *m, int fd) {
    if (!vm_msg_valid(m, 1)) { if (fd >= 0) close(fd); reply(ci, m->seq, VME_BAD_REQUEST, "bad request"); return; }
    if (fd >= 0 && m->type != VM_CDROM && m->type != VM_CLIP) { close(fd); fd = -1; }
    int alive = S.qemu > 0 && !S.qemu_done && S.qmp_up;
    switch (m->type) {
    case VM_STATUS: {
        VmMsg i = info_msg(), s = state_msg();
        i.seq = s.seq = m->seq;
        send_to(ci, &i, -1);
        send_to(ci, &s, -1);
        if (S.notice) { VmMsg n = vm_msg(VM_NOTICE, m->seq, S.notice, S.notice_code, 0, 0, S.notice_text); send_to(ci, &n, -1); }
        if (S.m.agent) { char t[160]; snprintf(t, sizeof t, "%s\t%s", S.ag_version, S.ag_os); VmMsg a = vm_msg(VM_AGENT_STATE, m->seq, S.ag_state, S.ag_caps, 0, 0, t); send_to(ci, &a, -1); }
        return;
    }
    case VM_PAUSE:
        if (!alive || S.state != VS_RUNNING) { reply(ci, m->seq, VME_WRONG_STATE, "not running"); return; }
        if (qmp_do("stop", NULL, P_REPLY, ci, m->seq, -1, -1)) reply(ci, m->seq, VME_BUSY, "busy");
        return;
    case VM_RESUME:
        if (!alive || S.state != VS_PAUSED) { reply(ci, m->seq, VME_WRONG_STATE, "not paused"); return; }
        if (qmp_do("cont", NULL, P_REPLY, ci, m->seq, -1, -1)) reply(ci, m->seq, VME_BUSY, "busy");
        return;
    case VM_SHUTDOWN:
        if (!alive || (S.state != VS_RUNNING && S.state != VS_PAUSED && S.state != VS_STOPPING)) { reply(ci, m->seq, VME_WRONG_STATE, "not running"); return; }
        if (S.state != VS_STOPPING) begin_shutdown(m->a);
        reply(ci, m->seq, VME_OK, "shutting down");
        return;
    case VM_POWEROFF:
        if (S.qemu <= 0 || S.qemu_done) { reply(ci, m->seq, VME_WRONG_STATE, "not running"); return; }
        begin_poweroff();
        reply(ci, m->seq, VME_OK, "turning off");
        return;
    case VM_RESET:
        if (!alive || (S.state != VS_RUNNING && S.state != VS_PAUSED)) { reply(ci, m->seq, VME_WRONG_STATE, "not running"); return; }
        if (qmp_do("system_reset", NULL, P_REPLY, ci, m->seq, -1, -1)) reply(ci, m->seq, VME_BUSY, "busy");
        return;
    case VM_DISPLAY: {
        if (S.m.display != VM_DISPLAY_WINDOW) { reply(ci, m->seq, VME_NO_DISPLAY, "this machine has no display"); return; }
        if (!alive) { reply(ci, m->seq, VME_WRONG_STATE, "not running"); return; }
        if (S.display_n >= 16) { reply(ci, m->seq, VME_BUSY, "too many display connections"); return; }
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) { reply(ci, m->seq, VME_INTERNAL, strerror(errno)); return; }
        char args[96];
        int k = ++S.display_n;
        snprintf(args, sizeof args, "{\"fdname\":\"vmdisplay%d\"}", k);
        if (qmp_do("getfd", args, P_GETFD, ci, m->seq, sv[1], -1)) { close(sv[0]); close(sv[1]); reply(ci, m->seq, VME_BUSY, "busy"); return; }
        close(sv[1]);
        snprintf(args, sizeof args, "{\"protocol\":\"@dbus-display\",\"fdname\":\"vmdisplay%d\"}", k);
        if (qmp_do("add_client", args, P_ADDCLIENT, ci, m->seq, -1, sv[0])) { close(sv[0]); reply(ci, m->seq, VME_BUSY, "busy"); }
        return;
    }
    case VM_CDROM: {
        if (!alive || (S.state != VS_RUNNING && S.state != VS_PAUSED)) { if (fd >= 0) close(fd); reply(ci, m->seq, VME_WRONG_STATE, "not running"); return; }
        if (S.cd_busy) { if (fd >= 0) close(fd); reply(ci, m->seq, VME_BUSY, "the drive is busy"); return; }
        struct stat st;
        if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode) || (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDONLY)) { close(fd); reply(ci, m->seq, VME_BAD_REQUEST, "not a file opened for reading"); return; }
        if (fd < 0 && !S.cd_node[0]) { reply(ci, m->seq, VME_OK, "ejected"); return; }
        cd_change(ci, m->seq, fd, m->text);
        if (fd >= 0) close(fd);
        return;
    }
    case VM_SUSPEND:
        if (!alive || (S.state != VS_RUNNING && S.state != VS_PAUSED)) { reply(ci, m->seq, VME_WRONG_STATE, "not running"); return; }
        if (S.suspending || S.cd_busy || S.snap_kind) { reply(ci, m->seq, VME_BUSY, "busy"); return; }
        begin_suspend(ci, m->seq);
        return;
    case VM_SNAPSHOT:
        if (!alive || (S.state != VS_RUNNING && S.state != VS_PAUSED)) { reply(ci, m->seq, VME_WRONG_STATE, "not running"); return; }
        if (S.snap_kind || S.suspending) { reply(ci, m->seq, VME_BUSY, "busy"); return; }
        begin_snapshot(ci, m->seq, m->a, m->text);
        return;
    case VM_CONSOLE: {
        if (!alive || S.serial_fd < 0) { reply(ci, m->seq, VME_WRONG_STATE, "not running"); return; }
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) { reply(ci, m->seq, VME_INTERNAL, strerror(errno)); return; }
        if (S.console_fd >= 0) close(S.console_fd);
        S.console_fd = sv[0];
        fcntl(S.console_fd, F_SETFL, O_NONBLOCK);
        VmMsg r = vm_msg(VM_CONSOLE_FD, m->seq, 0, 0, 0, 0, "");
        send_to(ci, &r, sv[1]);
        close(sv[1]);
        /* the last of what the guest wrote, so the shell opens on its prompt; without the terminal queries in it
         * (CSI … n, c, t: cursor position, attributes, size), which a new terminal would answer into the guest's
         * login prompt long after anyone asked */
        static char replay[sizeof S.ring];
        size_t start = (S.ring_at + sizeof S.ring - S.ring_n) % sizeof S.ring, o = 0;
        for (size_t k = 0; k < S.ring_n; k++) {
            char ch = S.ring[(start + k) % sizeof S.ring];
            if (ch == 0x1b && k + 1 < S.ring_n && S.ring[(start + k + 1) % sizeof S.ring] == '[') {
                size_t e = k + 2;
                while (e < S.ring_n && e < k + 24) {
                    char x = S.ring[(start + e) % sizeof S.ring];
                    if ((x >= '0' && x <= '9') || x == ';' || x == '?' || x == '>' || x == '=') { e++; continue; }
                    break;
                }
                char fin = e < S.ring_n ? S.ring[(start + e) % sizeof S.ring] : 0;
                if (fin == 'n' || fin == 'c' || fin == 't') { k = e; continue; }
            }
            replay[o++] = ch;
        }
        console_out(replay, o);
        vlog("shell: opened for client %d", ci);
        return;
    }
    case VM_AGENT:
        if (!S.m.agent) { reply(ci, m->seq, VME_DENIED, "the guest agent is off for this machine"); return; }
        if (!S.qga_up) { reply(ci, m->seq, VME_WRONG_STATE, "qemu-guest-agent isn't answering in the guest"); return; }
        if (m->a == VAG_SHUTDOWN) { qga_do("guest-shutdown", "{\"mode\":\"powerdown\"}", -1); reply(ci, m->seq, VME_OK, ""); return; }
        if (m->a == VAG_TIMESYNC) { ag_timesync(); reply(ci, m->seq, VME_OK, ""); return; }
        if (!S.helper || S.helper_handle || !(S.ag_caps & VAC_FILES) || !(S.ag_caps & VAC_EXEC)) { reply(ci, m->seq, VME_WRONG_STATE, S.helper ? "the guest agent can't write files or run commands" : "lordos-guest isn't available for this machine's architecture"); return; }
        S.install_client = ci; S.install_seq = m->seq;
        S.helper_handle = -1;
        if (qga_do("guest-file-open", "{\"path\":\"/usr/local/bin/.lordos-guest.new\",\"mode\":\"w\"}", AQ_FOPEN)) { S.helper_handle = 0; reply(ci, m->seq, VME_BUSY, "busy"); }
        return;
    case VM_CLIP: {
        if (fd < 0) { reply(ci, m->seq, VME_BAD_REQUEST, "no text"); return; }
        if (!clip_allows(1) || !S.lag_hello) { close(fd); reply(ci, m->seq, VME_DENIED, "the clipboard isn't shared toward this machine"); return; }
        static uint8_t buf[VM_CLIP_MAX];
        struct stat st;
        ssize_t n = !fstat(fd, &st) && st.st_size == m->a ? pread(fd, buf, (size_t)m->a, 0) : -1;
        close(fd);
        if (n != m->a) { reply(ci, m->seq, VME_BAD_REQUEST, "unreadable"); return; }
        if (m->b == VCF_TEXT) {
            if (n > (ssize_t)AG_TEXT_MAX || !ag_utf8_ok(buf, (size_t)n)) { reply(ci, m->seq, VME_BAD_REQUEST, "not text"); return; }
            lag_send(AGK_CLIP, buf, (uint32_t)n);
            reply(ci, m->seq, VME_OK, "");
            return;
        }
        uint8_t *clean = NULL;
        size_t cn = 0;
        char what[64];
        if (clip_clean(buf, (size_t)n, &clean, &cn, what, sizeof what)) { reply(ci, m->seq, VME_BAD_REQUEST, what); return; }
        if (S.lag_clipx) lag_send(AGK_CLIPX, clean, (uint32_t)cn);
        else {
            AgClip c;
            if (!ag_clipx_parse(clean, cn, &c) && c.text) lag_send(AGK_CLIP, c.text, c.text_n);
            else { free(clean); reply(ci, m->seq, VME_DENIED, "the guest's clipboard helper takes text only"); return; }
        }
        free(clean);
        reply(ci, m->seq, VME_OK, what);
        return;
    }
    case VM_KEEP:
        S.keep_running = m->a ? 1 : 0;
        vlog("keep running without lord-server: %s", S.keep_running ? "yes" : "no");
        reply(ci, m->seq, VME_OK, "");
        return;
    case VM_QMP:
        if (!S.dev_qmp) { reply(ci, m->seq, VME_DENIED, "lord-vm wasn't started with --dev-qmp"); return; }
        if (!S.qmp_up) { reply(ci, m->seq, VME_WRONG_STATE, "not running"); return; }
        if (S.pq_n >= MAX_PENDING || qmp_raw(&S.qmp, m->text)) { reply(ci, m->seq, VME_BAD_REQUEST, "can't send"); return; }
        pq_push(P_RAW, ci, m->seq, -1);
        vlog("dev-qmp from client %d: %s", ci, m->text);
        return;
    }
    reply(ci, m->seq, VME_BAD_REQUEST, "unknown request");
}

/* ---- QMP messages ---- */

static void on_qmp_event(const JVal *root) {
    const char *ev = json_str(root, "event");
    const JVal *data = json_get(root, "data");
    if (!strcmp(ev, "STOP")) { if (S.state == VS_RUNNING) set_state(VS_PAUSED, VR_USER, 0, ""); }
    else if (!strcmp(ev, "RESUME")) {
        if (S.state == VS_PAUSED || S.state == VS_STARTING) set_state(VS_RUNNING, S.state == VS_STARTING ? VR_NONE : VR_USER, 0, "");
        ag_timesync();                  /* the guest's clock stood still while it was paused or saved */
    }
    else if (!strcmp(ev, "SHUTDOWN")) {
        int guest = data ? json_bool(data, "guest", 0) : 0;
        const char *why = data ? json_str(data, "reason") : NULL;
        vlog("qemu: SHUTDOWN guest=%d reason=%s", guest, why ? why : "?");
        if (guest) S.guest_shutdown = 1;
        if (S.state != VS_STOPPING) set_state(VS_STOPPING, guest ? VR_GUEST_SHUTDOWN : VR_POWEROFF, 0, why ? why : "");
    } else if (!strcmp(ev, "RESET")) {
        if (S.state == VS_RUNNING || S.state == VS_PAUSED) set_state(S.state, VR_GUEST_RESET, 0, "");
    } else if (!strcmp(ev, "GUEST_PANICKED")) { S.panic = 1; vlog("qemu: the guest panicked"); }
    else if (!strcmp(ev, "POWERDOWN")) vlog("qemu: power button pressed");
    else if (!strcmp(ev, "VSERPORT_CHANGE")) {
        const char *id = data ? json_str(data, "id") : NULL;
        int open = data ? json_bool(data, "open", 0) : 0;
        if (id && !strcmp(id, "qga-port")) { S.qga_open = open; if (open) qga_sync(); else ag_lost("the guest closed qemu-guest-agent's port"); }
        else if (id && !strcmp(id, "lag-port")) {
            vlog("agent: the guest %s org.lordos.agent", open ? "opened" : "closed");
            if (!open) { S.lag_hello = 0; S.lag_clipx = 0; S.ag_caps &= ~VAC_CLIPBOARD; S.lag_n = 0; ag_broadcast(); }
        }
    } else if (!strcmp(ev, "JOB_STATUS_CHANGE")) {
        const char *id = data ? json_str(data, "id") : NULL, *st = data ? json_str(data, "status") : NULL;
        if (id && st && !strcmp(id, "snap") && !strcmp(st, "concluded") && S.snap_kind) qmp_do("query-jobs", NULL, P_JOBQ, -1, 0, -1, -1);
    } else if (!strcmp(ev, "MIGRATION")) {
        const char *st = data ? json_str(data, "status") : NULL;
        if (!st) return;
        if (S.suspending) {
            if (!strcmp(st, "completed")) suspend_done();
            else if (!strcmp(st, "failed") || !strcmp(st, "cancelled")) suspend_failed("QEMU couldn't save the machine's memory: see the log");
        } else if (S.resuming && !S.resumed) {
            if (!strcmp(st, "completed")) resume_done();
            else if (!strcmp(st, "failed")) { snprintf(S.resume_err, sizeof S.resume_err, "QEMU couldn't load the saved state"); }
        }
    }
    else vlog("qemu: event %.60s", ev);
}

static void on_qmp_reply(const JVal *root, int ok) {
    if (!S.pq_n) { vlog("qmp: a reply nobody asked for"); return; }
    Pending p = S.pq[S.pq_head];
    S.pq_head = (S.pq_head + 1) % MAX_PENDING;
    S.pq_n--;
    const char *desc = ok ? "" : qmp_error_desc(root);
    switch (p.kind) {
    case P_REPLY: reply(p.client, p.seq, ok ? VME_OK : VME_QMP, desc); break;
    case P_CONT_START:
        if (!ok) { vlog("qmp: cont: %s", desc); begin_poweroff(); }
        else if (S.state != VS_RUNNING) set_state(VS_RUNNING, VR_NONE, 0, "");
        break;
    case P_GETFD: if (!ok) vlog("qmp: getfd: %s", desc); break;
    case P_VERSION: {
        const JVal *q = json_get(json_get(root, "return"), "qemu");
        if (ok && q) snprintf(S.qemu_version, sizeof S.qemu_version, "%d.%d.%d %.40s", (int)json_num(q, "major", 0), (int)json_num(q, "minor", 0),
                              (int)json_num(q, "micro", 0), json_str(json_get(root, "return"), "package") ? json_str(json_get(root, "return"), "package") : "");
        if (ok && q) vlog("qemu: version %s", S.qemu_version);
        if (S.resuming && !S.resumed && !S.resume_err[0]) {
            SuspendMeta sm;
            if (!suspend_meta_read(S.dir, &sm) && strcmp(sm.qemu, S.qemu_version) && !S.resume_anyway) {
                snprintf(S.resume_err, sizeof S.resume_err, "it was saved by QEMU %.60s, this is %.60s", sm.qemu, S.qemu_version);
                vlog("resume: refused: %s", S.resume_err);
                if (qmp_do("quit", NULL, P_IGNORE, -1, 0, -1, -1)) { kill(S.qemu, SIGKILL); S.killed = 1; }
            } else begin_resume();
        }
        break;
    }
    case P_SUSPEND: if (!ok && S.suspending) suspend_failed(desc); break;
    case P_SNAPJOB:
        if (!ok && S.snap_kind) {
            if (S.loadvm[0]) { S.loadvm[0] = 0; snap_end(0, desc); fail_start("the snapshot couldn't be loaded: %.100s", desc); kill(S.qemu, SIGKILL); S.killed = 1; break; }
            if (S.snap_kind == VSN_DELETE) snap_picture_rm(S.dir, S.snap_tag);
            vm_op_end(S.dir);
            snap_end(0, desc);
        }
        break;
    case P_JOBQ: if (S.snap_kind) snap_concluded(root, ok); break;
    case P_NET: break;                                  /* the old backend may already be gone */
    case P_NET_ADD: if (!ok && !S.net_err[0]) snprintf(S.net_err, sizeof S.net_err, "%s", desc); break;
    case P_NET_LAST: net_attached(ok, desc); break;
    case P_SNAPSIZE: if (S.snap_kind == VSN_TAKE) snap_taken(root); break;
    case P_RESUME: if (!ok && !S.resume_err[0]) { snprintf(S.resume_err, sizeof S.resume_err, "%s", desc); kill(S.qemu, SIGKILL); S.killed = 1; } break;
    case P_MIGQ: {
        const JVal *ram = json_get(json_get(root, "return"), "ram");
        double tot = ram ? json_num(ram, "total", 0) : 0, done = ram ? json_num(ram, "transferred", 0) : 0;
        if (S.suspending && tot > 0) send_job(VJ_SUSPEND, VJS_RUNNING, done >= tot ? 99 : (int)(done * 100 / tot), "");
        break;
    }
    case P_CD: case P_CD_LAST:
        if (!ok && !S.cd_err[0]) snprintf(S.cd_err, sizeof S.cd_err, "%s", desc);
        if (ok && p.seq == 1) { S.cd_node[0] = 0; S.cd_fdset = 0; }
        if (ok && p.seq == 2) { snprintf(S.cd_node, sizeof S.cd_node, "%s", S.cd_new_node); S.cd_fdset = S.cd_new_fdset; }
        if (p.kind == P_CD_LAST) cd_done();
        break;
    case P_ADDCLIENT:
        if (ok) { VmMsg r = vm_msg(VM_DISPLAY_FD, p.seq, 0, 0, 0, 0, ""); send_to(p.client, &r, p.fd); vlog("display: connection %d handed to client %d", S.display_n, p.client); }
        else reply(p.client, p.seq, VME_QMP, desc);
        if (p.fd >= 0) close(p.fd);
        break;
    case P_RAW: {
        char text[VM_TEXT_MAX];
        JBuf b;
        jb_init(&b);
        if (ok) { const JVal *r = json_get(root, "return"); jb_raw(&b, r && r->type == J_OBJ && !r->kid ? "{}" : "return"); }
        else { jb_raw(&b, "error: "); jb_raw(&b, desc); }
        snprintf(text, sizeof text, "%s", b.p ? b.p : "");
        jb_free(&b);
        reply(p.client, p.seq, ok ? VME_OK : VME_QMP, text);
        vlog("dev-qmp answer: %s", text);
        break;
    }
    default: if (!ok) vlog("qmp: %s", desc); break;
    }
}

static void drain_qmp(void) {
    if (qmp_fill(&S.qmp) < 0) { S.qmp_up = 0; return; }
    JDoc d;
    while (qmp_next(&S.qmp, &d) > 0) {
        int k = qmp_kind(d.root);
        if (k == QMP_MSG_EVENT) on_qmp_event(d.root);
        else if (k == QMP_MSG_RETURN || k == QMP_MSG_ERROR) on_qmp_reply(d.root, k == QMP_MSG_RETURN);
        json_free(&d);
    }
}

/* ---- QEMU's end ---- */

static void qemu_exited(int status) {
    S.qemu_done = 1;
    S.qemu_status = status;
    int st = VS_STOPPED, why = VR_POWEROFF, code = 0;
    char d[160] = "";
    long oom = read_oom_kills();
    if (S.reason == VR_START_FAILED) { st = VS_CRASHED; why = VR_START_FAILED; code = WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status); snprintf(d, sizeof d, "%s", S.detail); }
    else if (S.suspended) why = VR_SUSPENDED;
    else if (S.resuming && !S.resumed) {
        /* the saved state is still there (removed only once loaded): try again, or discard it */
        st = VS_CRASHED; why = VR_RESUME_FAILED; code = WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status);
        snprintf(d, sizeof d, "%s", S.resume_err[0] ? S.resume_err : "QEMU stopped while loading the saved state: see the log");
        vm_op_end(S.dir);
    }
    else if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
        if (code != 0) { st = VS_CRASHED; why = VR_QEMU_FAILED; snprintf(d, sizeof d, "QEMU exited with %d: see the log", code); }
        else if (S.panic) { st = VS_CRASHED; why = VR_GUEST_PANIC; }
        else if (S.forced) why = VR_FORCED;
        else if (S.poweroff) why = VR_POWEROFF;
        else if (S.host_shutdown && S.guest_shutdown) why = VR_HOST_SHUTDOWN;
        else if (S.guest_shutdown) why = VR_GUEST_SHUTDOWN;
    } else if (WIFSIGNALED(status)) {
        code = WTERMSIG(status);
        if (S.killed && (S.forced || S.poweroff)) why = S.forced ? VR_FORCED : VR_POWEROFF;
        else if (oom > S.oom0) { st = VS_CRASHED; why = VR_OUT_OF_MEMORY; snprintf(d, sizeof d, "the machine went over its memory limit (%ld MB)", vm_mem_limit_mb(&S.m)); }
        else { st = VS_CRASHED; why = VR_QEMU_SIGNAL; snprintf(d, sizeof d, "QEMU died from signal %d (%s)", code, strsignal(code)); }
    }
    if (S.suspending) { if (S.susp_fd >= 0) close(S.susp_fd); S.susp_fd = -1; suspend_cleanup_tmp(); vm_op_end(S.dir); S.suspending = 0; reply(S.susp_client, S.susp_seq, VME_QMP, "QEMU stopped while saving"); }
    if (S.passt > 0) kill(S.passt, SIGTERM);
    if (S.passt_pidfd >= 0) syscall(SYS_pidfd_send_signal, S.passt_pidfd, SIGTERM, NULL, 0);
    set_state(st, why, code, d);
}

static void reap(void) {
    int status;
    pid_t p;
    while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
        if (p == S.qemu) { vlog("qemu: exited (%s %d)", WIFEXITED(status) ? "status" : "signal", WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status)); qemu_exited(status); }
        else if (p == S.passt) {
            vlog("passt: exited (%s %d)", WIFEXITED(status) ? "status" : "signal", WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
            S.passt = 0;
            if (!S.qemu_done) { int sig = WIFSIGNALED(status); net_lost(sig, sig ? WTERMSIG(status) : WEXITSTATUS(status)); }
        } else if (p == S.keeper) { vlog("network: passt's keeper ended"); S.keeper = 0; if (S.keeper_fd >= 0) close(S.keeper_fd); S.keeper_fd = -1; }
    }
}

/* ---- the loop ---- */

static void add_client(int fd, int control) {
    for (int i = 0; i < MAX_CLIENTS; i++) if (S.cl[i].fd < 0) { S.cl[i] = (Client){ fd, control }; return; }
    close(fd);
}

static void drop_client(int i) {
    int control = S.cl[i].control;
    close(S.cl[i].fd);
    S.cl[i].fd = -1;
    for (int k = 0; k < S.pq_n; k++) { Pending *p = &S.pq[(S.pq_head + k) % MAX_PENDING]; if (p->client == i) p->client = -1; }
    if (control && !S.keep_running && !S.qemu_done && S.state != VS_STOPPING) {
        /* the window went away without saying how (the session ended, lord-server stopped): saved, as at shutdown */
        if (S.qmp_up) host_stop("lord-server closed the control channel");
        else { kill(S.qemu, SIGKILL); S.killed = 1; S.poweroff = 1; }
    }
}

static int open_listener(void) {
    char base[PATH_MAX];
    if (socket_path(&S.m, S.sock_path, sizeof S.sock_path, base, sizeof base)) { S.sock_path[0] = 0; vlog("note: no runtime folder: the CLI can't reach this machine"); return -1; }
    snprintf(S.rundir, sizeof S.rundir, "%s", base);
    if (mkdir(base, 0700) && errno != EEXIST) return -1;
    struct stat st;
    if (lstat(base, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) { vlog("note: %s isn't private: no control socket", base); S.sock_path[0] = 0; return -1; }
    unlink(S.sock_path);          /* we hold the machine's lock: a socket there is stale */
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, S.sock_path, strlen(S.sock_path) + 1);      /* socket_path checked the length */
    mode_t old = umask(077);
    int r = fd >= 0 ? bind(fd, (struct sockaddr *)&a, sizeof a) : -1;
    umask(old);
    if (r || listen(fd, 4)) { if (fd >= 0) close(fd); S.sock_path[0] = 0; return -1; }
    return fd;
}

static void accept_client(void) {
    int c = accept4(S.listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (c < 0) return;
    struct ucred cr;
    socklen_t l = sizeof cr;
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &l) || cr.uid != getuid()) { close(c); return; }
    add_client(c, 0);
}

static void pump_fd(int *fd, const char *who) {
    char buf[4096];
    ssize_t n = read(*fd, buf, sizeof buf);
    if (n > 0) log_child(who, buf, (size_t)n);
    else if (n == 0 || (errno != EAGAIN && errno != EINTR)) { close(*fd); *fd = -1; }
}

static void ring_add(const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) { S.ring[S.ring_at] = b[i]; S.ring_at = (S.ring_at + 1) % sizeof S.ring; if (S.ring_n < sizeof S.ring) S.ring_n++; }
}

static void console_out(const char *b, size_t n) {
    if (S.console_fd < 0) return;
    ssize_t w = write(S.console_fd, b, n);           /* non-blocking: a console that doesn't read loses output, never stalls the guest */
    if (w < 0 && errno != EAGAIN && errno != EINTR) { close(S.console_fd); S.console_fd = -1; }
}

/* what the Shell typed, to the guest's serial port */
static void pump_console(void) {
    char buf[4096];
    ssize_t n = read(S.console_fd, buf, sizeof buf);
    if (n <= 0) { if (n == 0 || (errno != EAGAIN && errno != EINTR)) { close(S.console_fd); S.console_fd = -1; vlog("shell: closed"); } return; }
    if (S.serial_fd >= 0) {
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(S.serial_fd, buf + off, (size_t)(n - off));
            if (w < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) { struct pollfd pf = { S.serial_fd, POLLOUT, 0 }; if (poll(&pf, 1, 100) > 0) continue; } break; }
            off += w;
        }
    }
}

static void pump_serial(void) {
    char buf[8192];
    ssize_t n = read(S.serial_fd, buf, sizeof buf);
    if (n <= 0) { if (n == 0 || (errno != EAGAIN && errno != EINTR)) { close(S.serial_fd); S.serial_fd = -1; } return; }
    ring_add(buf, (size_t)n);
    console_out(buf, (size_t)n);
    if (S.serial_log >= 0 && S.serial_bytes < SERIAL_CAP) {
        size_t k = (size_t)n;
        if (S.serial_bytes + (off_t)k > SERIAL_CAP) k = (size_t)(SERIAL_CAP - S.serial_bytes);
        if (!write_all(S.serial_log, buf, k)) S.serial_bytes += (off_t)k;
        if (S.serial_bytes >= SERIAL_CAP) { const char t[] = "\n[lord-vm: serial log full]\n"; write_all(S.serial_log, t, sizeof t - 1); }
    }
}

static int sig_count;

static void loop(void) {
    while (!S.qemu_done) {
        struct pollfd p[MAX_CLIENTS + 8];
        int ci[MAX_CLIENTS + 8], n = 0;
#define WATCH(f, tag) do { if ((f) >= 0) { p[n] = (struct pollfd){ (f), POLLIN, 0 }; ci[n] = (tag); n++; } } while (0)
        WATCH(S.sig_fd, -1);
        WATCH(S.qmp_up ? S.qmp.fd : -1, -2);
        WATCH(S.serial_fd, -3);
        WATCH(S.qemu_err, -4);
        WATCH(S.passt_err, -5);
        WATCH(S.listen_fd, -6);
        WATCH(S.console_fd, -7);
        WATCH(S.qga_fd, -8);
        WATCH(S.lag_fd, -9);
        if (S.lag_fd >= 0 && lag_out_off < lag_out_n) p[n - 1].events |= POLLOUT;
        WATCH(S.passt_pidfd, -10);
        if (logind_fd() >= 0) { p[n] = (struct pollfd){ logind_fd(), logind_events(), 0 }; ci[n] = -11; n++; }
        for (int i = 0; i < MAX_CLIENTS; i++) WATCH(S.cl[i].fd, i);
#undef WATCH
        long long now = now_ms(), next = -1;
        if (S.shut_deadline) next = S.shut_deadline;
        if (S.migq_next && (next < 0 || S.migq_next < next)) next = S.migq_next;
        if (S.qga_fd >= 0 && S.ag_next && (next < 0 || S.ag_next < next)) next = S.ag_next;
        if (S.kill_deadline && (next < 0 || S.kill_deadline < next)) next = S.kill_deadline;
        int to = next < 0 ? -1 : next <= now ? 0 : (int)(next - now);
        if (poll(p, (nfds_t)n, to) < 0 && errno != EINTR) { vlog("poll: %s", strerror(errno)); break; }
        now = now_ms();
        if (S.migq_next && now >= S.migq_next) { S.migq_next = now + 500; qmp_do("query-migrate", NULL, P_MIGQ, -1, 0, -1, -1); }
        if (S.qga_fd >= 0 && S.ag_next && now >= S.ag_next) ag_tick(now);
        if (S.shut_deadline && now >= S.shut_deadline) {
            S.shut_deadline = 0;
            vlog("the guest didn't shut down in time: turning it off");
            S.forced = 1;
            if (qmp_do("quit", NULL, P_IGNORE, -1, 0, -1, -1)) { kill(S.qemu, SIGKILL); S.killed = 1; }
            S.kill_deadline = now + 5000;
        }
        if (S.kill_deadline && now >= S.kill_deadline && !S.qemu_done) {
            S.kill_deadline = 0;
            vlog("QEMU didn't quit: killing it");
            kill(S.qemu, SIGKILL);
            S.killed = 1;
        }
        for (int i = 0; i < n; i++) {
            if (!p[i].revents) continue;
            switch (ci[i]) {
            case -1: {
                struct signalfd_siginfo si;
                while (read(S.sig_fd, &si, sizeof si) == sizeof si) {
                    if (si.ssi_signo == SIGCHLD) reap();
                    else if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
                        sig_count++;
                        vlog("signal %d: %s", si.ssi_signo, sig_count == 1 ? "saving the machine, else shutting it down" : "turning off");
                        if (S.qemu_done) break;
                        if (sig_count == 1) host_stop(si.ssi_signo == SIGTERM ? "SIGTERM (the session or the computer is stopping)" : "SIGINT");
                        else if (sig_count > 1) begin_poweroff();
                    }
                }
                break;
            }
            case -2: drain_qmp(); break;
            case -3: pump_serial(); break;
            case -4: pump_fd(&S.qemu_err, "qemu"); break;
            case -5: pump_fd(&S.passt_err, "passt"); break;
            case -6: accept_client(); break;
            case -7: pump_console(); break;
            case -8: drain_qga(); break;
            case -9: if (p[i].revents & POLLOUT) lag_flush(); if (p[i].revents & (POLLIN | POLLHUP | POLLERR)) drain_lag(); break;
            case -10: {
                /* the passt the keeper started ended (not our child: no exit status, the pidfd says it's gone) */
                vlog("network: passt %d ended", (int)S.passt_ext);
                net_lost(0, 0);
                break;
            }
            case -11: logind_process(); break;
            default: {
                int k = ci[i];
                if (S.cl[k].fd < 0) break;
                if (p[i].revents & (POLLHUP | POLLERR) && !(p[i].revents & POLLIN)) { drop_client(k); break; }
                for (int budget = 0; budget < 32 && S.cl[k].fd >= 0; budget++) {
                    VmMsg m;
                    int mfd = -1;
                    int r = vm_recv(S.cl[k].fd, &m, &mfd);
                    if (r < 0) { drop_client(k); break; }
                    if (!r) break;
                    handle_request(k, &m, mfd);
                }
            }
            }
        }
    }
    /* what QEMU said last */
    for (int k = 0; k < 20 && S.qemu_err >= 0; k++) pump_fd(&S.qemu_err, "qemu");
    for (int k = 0; k < 20 && S.passt_err >= 0; k++) pump_fd(&S.passt_err, "passt");
    if (S.passt > 0) {
        for (int k = 0; k < 20; k++) { if (waitpid(S.passt, NULL, WNOHANG) == S.passt) { S.passt = 0; break; } struct timespec ts = { 0, 50000000 }; nanosleep(&ts, NULL); }
        if (S.passt > 0) { kill(S.passt, SIGKILL); waitpid(S.passt, NULL, 0); }
    }
    if (S.passt_pidfd >= 0) {
        struct pollfd pf = { S.passt_pidfd, POLLIN, 0 };
        if (poll(&pf, 1, 1000) <= 0) syscall(SYS_pidfd_send_signal, S.passt_pidfd, SIGKILL, NULL, 0);
        close(S.passt_pidfd);
        S.passt_pidfd = -1;
    }
    if (S.keeper_fd >= 0) { close(S.keeper_fd); S.keeper_fd = -1; }
    if (S.keeper > 0) { waitpid(S.keeper, NULL, 0); S.keeper = 0; }
    logind_close();
}

/* a previous lord-vm left this machine marked running: its children die with it (PDEATHSIG), but check */
static int recover(void) {
    RunInfo r;
    if (runinfo_read(S.dir, &r) || !state_active(r.state)) return 0;
    vlog("recovery: the previous lord-vm (pid %ld) ended without a final state (%s): marking it lost", r.pid, STATE_NAME[r.state]);
    long pids[2] = { r.qemu_pid, r.passt_pid };
    for (int i = 0; i < 2; i++) {
        if (pids[i] <= 1) continue;
        char path[64], buf[8192];
        snprintf(path, sizeof path, "/proc/%ld/cmdline", pids[i]);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        int match = 0;
        for (ssize_t k = 0; k + 36 <= n; k++) if (!memcmp(buf + k, S.m.id, 36)) { match = 1; break; }
        if (i == 1 && !match && strstr(buf, "passt")) match = 1;
        if (match) {
            vlog("recovery: process %ld of the old run is still there: killing it", pids[i]);
            kill((pid_t)pids[i], SIGKILL);
            /* not our child: it's gone when /proc says so; until then it still holds its disks' locks */
            for (int k = 0; k < 100; k++) {
                char st[64];
                snprintf(st, sizeof st, "/proc/%ld", pids[i]);
                if (access(st, F_OK)) break;
                struct timespec ts = { 0, 50000000 };
                nanosleep(&ts, NULL);
            }
        }
    }
    RunInfo lost = r;
    lost.state = VS_CRASHED;
    lost.reason = VR_SUPERVISOR_LOST;
    snprintf(lost.detail, sizeof lost.detail, "the previous lord-vm stopped unexpectedly; recovered at the next start");
    vm_now_iso(lost.since);
    runinfo_write(S.dir, &lost);
    char sp[PATH_MAX];
    if (!socket_path(&S.m, sp, sizeof sp, NULL, 0)) unlink(sp);
    return 1;
}

static void rotate_logs(void) {
    char a[PATH_MAX + 32], b[PATH_MAX + 32];
    snprintf(a, sizeof a, "%s/log/lord-vm.1.log", S.dir); snprintf(b, sizeof b, "%s/log/lord-vm.2.log", S.dir); rename(a, b);
    snprintf(b, sizeof b, "%s/log/lord-vm.log", S.dir); rename(b, a);
    snprintf(a, sizeof a, "%s/log/serial.1.log", S.dir); snprintf(b, sizeof b, "%s/log/serial.log", S.dir); rename(b, a);
}

static int open_lock(const char *dir) {
    char p[PATH_MAX + 16];
    snprintf(p, sizeof p, "%s/lock", dir);
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return -2; }
    return fd;
}

static int b64_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || n % 4) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || (c == '=' && i >= n - 2))) return 0;
    }
    return 1;
}

static int cred_ok(const char *s, int binary) {
    const char *eq = strchr(s, '=');
    if (!eq || eq == s || eq - s > 128 || strlen(s) > 1800) return 0;
    for (const char *p = s; p < eq; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) return 0;
    if (binary) return b64_ok(eq + 1);
    for (const unsigned char *p = (const unsigned char *)eq + 1; *p; p++) if (*p < 0x20 || *p == 0x7f) return 0;
    return 1;
}

/* systemd credentials for the guest (see the top of this file): each into a sealed memfd holding
 * "io.systemd.credential[.binary]:NAME=VALUE", the form QEMU's -smbios type=11,path= reads */
static int read_credentials(const char *path) {
    FILE *f = fopen(path, "re");
    if (!f) { fprintf(stderr, "lord-vm: can't read %s: %s\n", path, strerror(errno)); return -1; }
    struct stat st;
    if (fstat(fileno(f), &st) || (st.st_mode & 077)) { fprintf(stderr, "lord-vm: %s must be readable by its owner only (0600)\n", path); fclose(f); return -1; }
    char line[2048];
    int rc = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        int bin = !strncmp(line, "binary:", 7);
        const char *c = bin ? line + 7 : line;
        if (S.ncreds >= MAX_CREDS || !cred_ok(c, bin)) { fprintf(stderr, "lord-vm: a credential line in %s isn't NAME=VALUE (or binary:NAME=BASE64)\n", path); rc = -1; break; }
        char text[2100];
        int len = snprintf(text, sizeof text, "io.systemd.credential%s:%s", bin ? ".binary" : "", c);
        int fd = memfd_create("lordos-vm-credential", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd < 0 || write_all(fd, text, (size_t)len) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)) {
            if (fd >= 0) close(fd);
            rc = -1;
            break;
        }
        explicit_bzero(text, sizeof text);
        const char *eq = strchr(c, '=');
        snprintf(S.cred_name[S.ncreds], sizeof S.cred_name[0], "%.*s", (int)(eq - c), c);
        S.cred_fd[S.ncreds++] = fd;
    }
    explicit_bzero(line, sizeof line);
    fclose(f);
    return rc;
}

static void rm_tree(const char *dir);
/* An operation.json left by a lord-vm that stopped in the middle (3.6): finish the work or roll it back, and say which.
 * Runs under the machine's lock, before anything else touches the machine. 1 when there was something to do. */
static int ops_recover(const char *dir, int at_start) {
    VmOp o;
    int r = vm_op_read(dir, &o);
    if (r < 0) { vlog("recovery: operation.json was unreadable and is removed (no step had started)"); return 0; }
    if (!r) return 0;
    char what[200] = "", p1[PATH_MAX + 32], p2[PATH_MAX + 32];
    if (!strcmp(o.op, "suspend")) {
        snprintf(p1, sizeof p1, "%s/suspend.state", dir);
        snprintf(p2, sizeof p2, "%s/suspend.json", dir);
        if (file_exists(p1) && file_exists(p2)) snprintf(what, sizeof what, "the suspend had finished: the machine resumes from it");
        else {
            unlink(p1); unlink(p2);
            snprintf(p1, sizeof p1, "%s/.suspend.state.tmp", dir); unlink(p1);
            snprintf(p1, sizeof p1, "%s/.suspend.json.tmp", dir); unlink(p1);
            snprintf(what, sizeof what, "an interrupted suspend was rolled back: the machine starts from its disks, as after a power cut");
        }
    } else if (!strcmp(o.op, "resume")) {
        snprintf(p1, sizeof p1, "%s/suspend.state", dir);
        if (file_exists(p1)) snprintf(what, sizeof what, "an interrupted resume was rolled back: the saved state is kept");
        else snprintf(what, sizeof what, "the resume had finished");
    } else if (!strcmp(o.op, "compact")) {
        if (o.a[0] && strstr(o.a, "/disks/.system.qcow2.compact")) unlink(o.a);
        snprintf(what, sizeof what, "an interrupted compaction was rolled back: the disk is as it was");
    } else if (!strcmp(o.op, "link")) {
        /* a linked clone that didn't finish: if machine.json names the base, the source runs on it (forward); if not, the
         * base goes back to being the source's disk. The half-made clone goes either way. */
        VmMachine m;
        char err[256], sys[PATH_MAX + 32], ov[PATH_MAX + 64];
        snprintf(sys, sizeof sys, "%s/disks/system.qcow2", dir);
        snprintf(ov, sizeof ov, "%s/disks/.system.qcow2.new", dir);
        unlink(ov);
        if (o.b[0] && strstr(o.b, ".part-")) rm_tree(o.b);
        if (!vm_machine_load(dir, &m, err, sizeof err) && m.base[0] && !strcmp(m.base, o.a) && file_exists(sys))
            snprintf(what, sizeof what, "an interrupted linked clone: this machine now runs on a shared base; the clone wasn't made");
        else if (vm_base_valid(o.a) && file_exists(o.a)) {
            if (file_exists(sys)) unlink(sys);              /* an empty overlay: QEMU never ran on it */
            if (rename(o.a, sys)) { vlog("recovery: can't move %s back: %s", o.a, strerror(errno)); return 0; }
            chmod(sys, 0600);
            snprintf(what, sizeof what, "an interrupted linked clone was rolled back: the disk is back in its folder");
        } else snprintf(what, sizeof what, "an interrupted linked clone left nothing to undo");
    } else if (!strncmp(o.op, "snapshot-", 9) && vm_snap_tag_valid(o.tag)) {
        VmMachine m;
        char err[256], disks[2][PATH_MAX + 64];
        if (vm_machine_load(dir, &m, err, sizeof err)) { vlog("recovery: %s: %s", o.op, err); return 0; }
        int nd = disk_files(dir, &m, disks);
        VmSnap list[VM_SNAPS_MAX];
        int n = vm_snaps_load(dir, list, VM_SNAPS_MAX), i = vm_snaps_find(list, n, o.tag);
        if (!strcmp(o.op, "snapshot-take")) {
            if (i >= 0) snprintf(what, sizeof what, "the snapshot %s had finished", o.tag);
            else {
                for (int k = 0; k < nd; k++) qimg_snap(dir, disks[k], "-d", o.tag, err, sizeof err);
                snap_picture_rm(dir, o.tag);
                snprintf(what, sizeof what, "an interrupted snapshot was rolled back: nothing of it is left");
            }
        } else if (!strcmp(o.op, "snapshot-delete")) {
            if (i >= 0) { memmove(&list[i], &list[i + 1], sizeof list[0] * (size_t)(n - i - 1)); vm_snaps_save(dir, list, n - 1); }
            for (int k = 0; k < nd; k++) qimg_snap(dir, disks[k], "-d", o.tag, err, sizeof err);
            snap_picture_rm(dir, o.tag);
            snprintf(what, sizeof what, "an interrupted snapshot deletion was finished");
        } else if (!strcmp(o.op, "snapshot-revert")) {
            int bad = i < 0;
            for (int k = 0; !bad && k < nd; k++) bad = qimg_snap(dir, disks[k], "-a", o.tag, err, sizeof err);
            if (bad) { vlog("recovery: going back to %s couldn't be finished: %s", o.tag, err); return 0; }   /* kept for the next try */
            discard_state(dir);
            snprintf(what, sizeof what, "going back to a snapshot was finished on the disks; the machine starts as after a power cut");
        } else snprintf(what, sizeof what, "an unknown operation \"%.40s\" was dropped", o.op);
        for (int k = 0; k < nd; k++) qimg_repair_leaks(dir, disks[k]);
    } else snprintf(what, sizeof what, "an unknown operation \"%.40s\" was dropped", o.op);
    vm_op_end(dir);
    vlog("recovery: %s (%s, started %s)", what, o.op, o.started);
    if (at_start) { S.notice = VMN_RECOVERED; S.notice_code = 0; snprintf(S.notice_text, sizeof S.notice_text, "%s", what); }
    else fprintf(stderr, "lord-vm: recovery: %s\n", what);
    return 1;
}

static void discard_state(const char *dir) {
    char p[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/suspend.state", dir); unlink(p);
    snprintf(p, sizeof p, "%s/suspend.json", dir); unlink(p);
    snprintf(p, sizeof p, "%s/.suspend.state.tmp", dir); unlink(p);
    snprintf(p, sizeof p, "%s/.suspend.json.tmp", dir); unlink(p);
    vm_sync_dir(dir);
}

static int connect_machine(const VmMachine *m);
static int cmd_start(const char *dir, int argc, char **argv) {
    int detach = 0, control = -1, discard = 0;
    memset(&S, 0, sizeof S);
    S.listen_fd = S.serial_fd = S.serial_log = S.qemu_err = S.passt_err = S.sig_fd = S.lock_fd = S.memev_fd = S.notify_fd = S.susp_fd = S.console_fd = S.qga_fd = S.lag_fd = -1;
    S.keeper_fd = S.passt_pidfd = -1;
    S.qmp.fd = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) S.cl[i].fd = -1;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--paused")) S.paused_start = 1;
        else if (!strcmp(argv[i], "--detach")) detach = 1;
        else if (!strcmp(argv[i], "--keep-running")) S.keep_running = 1;
        else if (!strcmp(argv[i], "--dev-qmp")) S.dev_qmp = 1;
        else if (!strcmp(argv[i], "--resume-anyway")) S.resume_anyway = 1;
        else if (!strcmp(argv[i], "--loadvm") && i + 1 < argc) { if (!vm_snap_tag_valid(argv[++i])) { fprintf(stderr, "lord-vm: --loadvm TAG\n"); return 2; } snprintf(S.loadvm, sizeof S.loadvm, "%s", argv[i]); }
        else if (!strcmp(argv[i], "--discard-state")) discard = 1;
        else if (!strcmp(argv[i], "--control-fd") && i + 1 < argc) control = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--credentials-file") && i + 1 < argc) { if (read_credentials(argv[++i])) return 2; }
        else { fprintf(stderr, "lord-vm: unknown option %s\n", argv[i]); return 2; }
    }
    if (control >= 0) {
        int type = 0;
        socklen_t l = sizeof type;
        if (control < 3 || getsockopt(control, SOL_SOCKET, SO_TYPE, &type, &l) || type != SOCK_SEQPACKET) { fprintf(stderr, "lord-vm: --control-fd must be a SOCK_SEQPACKET socket\n"); return 2; }
        fcntl(control, F_SETFD, FD_CLOEXEC);
        fcntl(control, F_SETFL, O_NONBLOCK);
    }
    prctl(PR_SET_DUMPABLE, 0);
    signal(SIGPIPE, SIG_IGN);
    if (!realpath(dir, S.dir) || !dir_ok(S.dir)) { fprintf(stderr, "lord-vm: %s: not a machine folder (or its path has ',' or control characters)\n", dir); return 1; }
    snprintf(S.disks, sizeof S.disks, "%s/disks", S.dir);
    char err[256];
    if (vm_machine_load(S.dir, &S.m, err, sizeof err)) { fprintf(stderr, "lord-vm: %s\n", err); return 1; }
    S.lock_fd = open_lock(S.dir);
    if (S.lock_fd == -2) { fprintf(stderr, "lord-vm: %s is already running\n", S.m.name); return 3; }
    if (S.lock_fd < 0) { fprintf(stderr, "lord-vm: can't write in %s\n", S.dir); return 1; }
    {
        /* a Duplicate has the original's identity (6.3): the two never run at once (one socket, one -uuid) */
        int f = connect_machine(&S.m);
        if (f >= 0) { close(f); fprintf(stderr, "lord-vm: a machine with the same identity is running (a Duplicate and its original don't run together)\n"); return 3; }
    }

    if (detach) {
        int np[2];
        if (pipe2(np, O_CLOEXEC)) return 1;
        pid_t pid = fork();
        if (pid < 0) return 1;
        if (pid) {
            close(np[1]);
            char c = 0;
            ssize_t r;
            while ((r = read(np[0], &c, 1)) < 0 && errno == EINTR) ;
            printf("{\"started\":%s,\"pid\":%d,\"id\":\"%s\"}\n", c == 'o' ? "true" : "false", (int)pid, S.m.id);
            return c == 'o' ? 0 : 1;
        }
        close(np[0]);
        S.notify_fd = np[1];
        setsid();
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); if (null > 2) close(null); }
        log_stderr = 0;
    }
    char p[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/log", S.dir);
    mkdir(p, 0700);
    rotate_logs();
    snprintf(p, sizeof p, "%s/log/lord-vm.log", S.dir);
    log_fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    snprintf(p, sizeof p, "%s/log/serial.log", S.dir);
    S.serial_log = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    vlog("lord-vm %d: machine %s \"%s\" (%s, %s, %d vCPU, %d MB, %s)", (int)getpid(), S.m.id, S.m.name, vm_arch_name(S.m.arch),
         S.m.accel ? "tcg" : "kvm", S.m.vcpus, S.m.memory_mb, S.m.machine_type);
    RunInfo prev;
    int crashed = !runinfo_read(S.dir, &prev) && prev.state == VS_CRASHED &&
                  (prev.reason == VR_QEMU_SIGNAL || prev.reason == VR_OUT_OF_MEMORY || prev.reason == VR_SUPERVISOR_LOST);
    int lost = recover();
    ops_recover(S.dir, 1);
    if (lost || crashed) {
        /* QEMU died in the middle (killed, or with lord-vm): what it had allocated and not yet referenced is given back */
        char disks[2][PATH_MAX + 64];
        int nd = disk_files(S.dir, &S.m, disks);
        for (int k = 0; k < nd; k++) qimg_repair_leaks(S.dir, disks[k]);
    }
    snaps_sweep(S.dir, &S.m);
    if (discard) { discard_state(S.dir); vlog("the saved state was discarded: the machine starts from its disks"); }
    if (S.loadvm[0]) {
        /* back to a snapshot with its memory: QEMU loads it at start (-loadvm), under operation.json; a saved session
         * no longer fits the disks */
        VmSnap list[VM_SNAPS_MAX];
        int n = vm_snaps_load(S.dir, list, VM_SNAPS_MAX), i = vm_snaps_find(list, n, S.loadvm);
        if (i < 0 || !list[i].memory) { fprintf(stderr, "lord-vm: no snapshot %s with memory\n", S.loadvm); vlog("snapshot: no %s with memory", S.loadvm); return 1; }
        discard_state(S.dir);
        if (vm_op_begin(S.dir, "snapshot-revert", S.loadvm, NULL, NULL)) return 1;
        vlog("snapshot: starting from %s \"%s\"", S.loadvm, list[i].name);
    }
    host_cpu_id(S.cpu_id);
    if (S.m.agent) {
        /* app.json's keepAwake (on unless turned off), and lordos-guest for Install Agent: read now, before lord-vm's
         * own Landlock keeps it in the machine's folder */
        char ap[PATH_MAX + 32], buf[4096];
        snprintf(ap, sizeof ap, "%s/app.json", S.dir);
        int afd = open(ap, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        ssize_t an = afd >= 0 ? read(afd, buf, sizeof buf - 1) : -1;
        if (afd >= 0) close(afd);
        S.keep_awake = 1;
        JDoc jd;
        if (an > 0 && !json_parse(buf, (size_t)an, &jd, NULL, 0)) { S.keep_awake = json_bool(jd.root, "keepAwake", 1); json_free(&jd); }
        static const char *const HELPER[2] = { "/usr/lib/lordos/guest/lordos-guest-arm64", "/usr/lib/lordos/guest/lordos-guest-amd64" };
        int hfd = open(HELPER[S.m.arch], O_RDONLY | O_CLOEXEC);
        struct stat hs;
        if (hfd >= 0 && !fstat(hfd, &hs) && hs.st_size > 0 && hs.st_size < (8 << 20) && (S.helper = malloc((size_t)hs.st_size))) {
            S.helper_n = (size_t)hs.st_size;
            if (pread(hfd, S.helper, S.helper_n, 0) != (ssize_t)S.helper_n) { free(S.helper); S.helper = NULL; S.helper_n = 0; }
        }
        if (hfd >= 0) close(hfd);
    }
    S.state = VS_STOPPED;
    set_state(VS_STARTING, VR_NONE, 0, "");

    sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGCHLD); sigaddset(&ss, SIGTERM); sigaddset(&ss, SIGINT);
    sigprocmask(SIG_BLOCK, &ss, NULL);
    S.sig_fd = signalfd(-1, &ss, SFD_CLOEXEC | SFD_NONBLOCK);
    signal(SIGHUP, SIG_IGN);
    if (control >= 0) {
        add_client(control, 1);
        if (S.notice) { VmMsg n = vm_msg(VM_NOTICE, 0, S.notice, S.notice_code, 0, 0, S.notice_text); send_to(0, &n, -1); }
    }
    S.listen_fd = open_listener();
    setup_cgroups();
    if (start_machine()) {
        /* never ran: nothing of it may stay behind */
        if (S.passt > 0) { kill(S.passt, SIGKILL); waitpid(S.passt, NULL, 0); }
        if (S.qemu > 0) { kill(S.qemu, SIGKILL); waitpid(S.qemu, NULL, 0); }
        for (int k = 0; k < 20 && S.qemu_err >= 0; k++) pump_fd(&S.qemu_err, "qemu");
        S.qemu = 0;
    }
    if (S.qemu > 0 && !S.qemu_done) {
        fcntl(S.serial_fd, F_SETFL, O_NONBLOCK);
        fcntl(S.qemu_err, F_SETFL, O_NONBLOCK);
        if (S.passt_err >= 0) fcntl(S.passt_err, F_SETFL, O_NONBLOCK);
        /* what needs more than the machine's folder, before lord-vm's own Landlock: passt's keeper and logind */
        if (S.m.network == VM_NET_SHARED) {
            S.keeper = start_keeper(&S.keeper_fd);
            if (S.keeper < 0) { S.keeper = 0; vlog("network: no keeper: a passt that ends isn't replaced"); }
        }
        logind_open();
        S.sandbox |= vm_jail_self(S.dir, S.rundir);
        vlog("lord-vm: own Landlock %s", S.sandbox & VMS_SUPERVISOR_LANDLOCK ? "applied (machine folder only)" : "not available");
        VmMsg i = info_msg();
        for (int k = 0; k < MAX_CLIENTS; k++) send_to(k, &i, -1);
        loop();
    }
    if (S.sock_path[0]) unlink(S.sock_path);
    vlog("lord-vm: done (%s, %s)", STATE_NAME[S.state], REASON_NAME[S.reason]);
    return S.state == VS_STOPPED ? 0 : 1;
}

/* ---- the CLI's side ---- */

static int connect_machine(const VmMachine *m) {
    char sp[PATH_MAX];
    if (socket_path(m, sp, sizeof sp, NULL, 0)) return -1;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, sp, strlen(sp) + 1);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) { if (fd >= 0) close(fd); return -1; }
    return fd;
}

/* waits up to ms for one message */
static int wait_msg(int fd, VmMsg *m, int ms) {
    long long end = now_ms() + ms;
    for (;;) {
        int r = vm_recv(fd, m, NULL);
        if (r) return r;
        long long left = end - now_ms();
        if (left <= 0) return 0;
        struct pollfd p = { fd, POLLIN, 0 };
        poll(&p, 1, (int)left);
    }
}

static void print_json_str(const char *s) {
    putchar('"');
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == '"' || *c == '\\') { putchar('\\'); putchar(*c); }
        else if (*c < 0x20) printf("\\u%04x", *c);
        else putchar(*c);
    }
    putchar('"');
}

static void print_state(const VmMachine *m, int state, int reason, int code, const char *detail, int pid, int sandbox, int cg, int json, int live) {
    if (json) {
        printf("{\"id\":\"%s\",\"state\":\"%s\",\"reason\":\"%s\",\"code\":%d,\"qemu_pid\":%d,\"live\":%s", m->id, STATE_NAME[state], REASON_NAME[reason], code, pid, live ? "true" : "false");
        if (live) printf(",\"sandbox\":%d,\"landlock_fs\":%d,\"landlock_net\":%d,\"seccomp_ours\":%d,\"kvm_fd\":%d,\"supervisor_landlock\":%d,\"cgroup\":\"%s\"",
                         sandbox, !!(sandbox & VMS_LANDLOCK_FS), !!(sandbox & VMS_LANDLOCK_NET), !!(sandbox & VMS_SECCOMP_OURS), !!(sandbox & VMS_KVM_FD),
                         !!(sandbox & VMS_SUPERVISOR_LANDLOCK), cg & VMC_NESTED ? "nested" : cg & VMC_SCOPE ? "scope" : "none");
        printf(",\"detail\":");
        print_json_str(detail);
        printf("}\n");
    } else {
        printf("%s: %s (%s)%s%s\n", m->name, STATE_NAME[state], REASON_NAME[reason], detail[0] ? ": " : "", detail);
        if (live) printf("  qemu pid %d, sandbox: landlock-fs %s, landlock-net %s, our seccomp %s, /dev/kvm by descriptor %s, cgroups %s\n", pid,
                         sandbox & VMS_LANDLOCK_FS ? "yes" : "no", sandbox & VMS_LANDLOCK_NET ? "yes" : "no", sandbox & VMS_SECCOMP_OURS ? "yes" : "no",
                         sandbox & VMS_KVM_FD ? "yes" : "no", cg & VMC_NESTED ? "nested" : cg & VMC_SCOPE ? "scope" : "none");
    }
}

static int cmd_client(const char *dir, int type, int argc, char **argv) {
    int json = 0, timeout = 0;
    const char *qmptext = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout = atoi(argv[++i]);
        else if ((type == VM_QMP || type == VM_CDROM || type == VM_AGENT) && !qmptext) qmptext = argv[i];
        else { fprintf(stderr, "lord-vm: unknown option %s\n", argv[i]); return 2; }
    }
    char real[PATH_MAX], err[256];
    VmMachine m;
    if (!realpath(dir, real)) { fprintf(stderr, "lord-vm: %s: %s\n", dir, strerror(errno)); return 1; }
    int lr = vm_machine_load(real, &m, err, sizeof err);
    if (lr) { fprintf(stderr, "lord-vm: %s\n", err); return 1; }
    int fd = connect_machine(&m);
    if (fd < 0) {
        RunInfo r;
        int have = !runinfo_read(real, &r);
        int lock = open_lock(real);
        int held = lock == -2;
        if (lock >= 0) close(lock);
        if (type != VM_STATUS) { fprintf(stderr, "lord-vm: %s isn't running%s\n", m.name, held ? " here (locked by another lord-vm without a socket)" : ""); return 3; }
        if (!have) { r.state = VS_STOPPED; r.reason = VR_NONE; r.detail[0] = 0; }
        if (have && state_active(r.state) && !held) { r.state = VS_CRASHED; r.reason = VR_SUPERVISOR_LOST; snprintf(r.detail, sizeof r.detail, "lord-vm (pid %ld) is gone; the next start recovers", r.pid); }
        print_state(&m, r.state, r.reason, r.code, r.detail, 0, 0, 0, json, 0);
        return 0;
    }
    if (type == VM_QMP && (!qmptext || strlen(qmptext) >= VM_TEXT_MAX)) { fprintf(stderr, "lord-vm: --qmp takes one command of at most %d bytes\n", VM_TEXT_MAX - 1); return 2; }
    int pass = -1;
    char cdreal[PATH_MAX] = "";
    if (type == VM_CDROM) {
        if (!qmptext) { fprintf(stderr, "lord-vm: --cdrom DIR IMAGE|none\n"); return 2; }
        if (strcmp(qmptext, "none")) {
            if (!realpath(qmptext, cdreal) || (pass = open(cdreal, O_RDONLY | O_CLOEXEC | O_NOCTTY)) < 0) { fprintf(stderr, "lord-vm: %s: %s\n", qmptext, strerror(errno)); return 1; }
        }
        qmptext = strlen(cdreal) < VM_TEXT_MAX ? cdreal : "";
    }
    int a = type == VM_SHUTDOWN ? timeout : 0;
    if (type == VM_AGENT) {
        a = !qmptext ? 0 : !strcmp(qmptext, "shutdown") ? VAG_SHUTDOWN : !strcmp(qmptext, "timesync") ? VAG_TIMESYNC : !strcmp(qmptext, "install") ? VAG_INSTALL : 0;
        if (!a) { fprintf(stderr, "lord-vm: --agent DIR shutdown|timesync|install\n"); return 2; }
        qmptext = "";
    }
    VmMsg q = vm_msg((uint16_t)type, 7, a, 0, 0, 0, qmptext);
    int sent = vm_send(fd, &q, pass);
    if (pass >= 0) close(pass);
    if (sent) { fprintf(stderr, "lord-vm: can't talk to the machine\n"); return 1; }
    int info_sb = 0, info_cg = 0, rc = 1;
    long long end = now_ms() + (type == VM_SHUTDOWN ? (timeout ? timeout : 60) * 1000 + 15000 : type == VM_POWEROFF ? 15000 : type == VM_SUSPEND ? 600000 : type == VM_AGENT ? 120000 : 10000);
    int replied = 0;
    for (;;) {
        VmMsg r;
        long long left = end - now_ms();
        int g = left > 0 ? wait_msg(fd, &r, (int)left) : 0;
        if (g < 0) { if (type == VM_SHUTDOWN || type == VM_POWEROFF || (type == VM_SUSPEND && replied)) { if (!json) printf("%s: stopped\n", m.name); rc = 0; } break; }
        if (!g) { fprintf(stderr, "lord-vm: no answer in time\n"); break; }
        if (r.type == VM_INFO) { info_sb = r.c; info_cg = r.d; continue; }
        if (r.type == VM_STATE) {
            if (type == VM_STATUS) { print_state(&m, r.a, r.b, r.c, r.text, r.d, info_sb, info_cg, json, 1); rc = 0; break; }
            if ((type == VM_SHUTDOWN || type == VM_POWEROFF || type == VM_SUSPEND) && (r.a == VS_STOPPED || r.a == VS_CRASHED)) { print_state(&m, r.a, r.b, r.c, r.text, r.d, 0, 0, json, 0); rc = r.a == VS_STOPPED ? 0 : 1; break; }
            continue;
        }
        if (r.type == VM_REPLY && r.seq == 7) {
            replied = 1;
            if (r.a != VME_OK) {
                if (json || type == VM_QMP) { printf("{\"ok\":false,\"code\":%d,\"text\":", r.a); print_json_str(r.text); printf("}\n"); }
                else fprintf(stderr, "lord-vm: %s\n", r.text);
                rc = 1;
                break;
            }
            if (type == VM_SHUTDOWN || type == VM_POWEROFF || type == VM_SUSPEND) continue;
            if (json || type == VM_QMP) { printf("{\"ok\":true,\"text\":"); print_json_str(r.text); printf("}\n"); }
            rc = 0;
            break;
        }
    }
    (void)replied;
    close(fd);
    return rc;
}

/* Changes a machine that is off (Configure): the same checks as --create, under the machine's lock (exit 3 while it
 * runs). The disk only grows (qemu-img resize, jailed: it reads and writes the machine's disks/ only); an ISO is
 * checked like --iso, "none" takes it out. Prints the machine as JSON. */
static int cmd_set(const char *dir, int argc, char **argv) {
    const char *name = NULL, *iso = NULL;
    int vcpus = 0, mem = 0, disk = 0, disp = -1, net = -1, agent = -1, clip = -1;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) { fprintf(stderr, "lord-vm: %s needs a value\n", a); return 2; }
        i++;
        if (!strcmp(a, "--name")) name = v;
        else if (!strcmp(a, "--vcpus")) vcpus = atoi(v);
        else if (!strcmp(a, "--memory")) mem = atoi(v);
        else if (!strcmp(a, "--disk")) disk = atoi(v);
        else if (!strcmp(a, "--display")) disp = parse_enum(v, "window", "none");
        else if (!strcmp(a, "--network")) net = parse_enum(v, "shared", "isolated");
        else if (!strcmp(a, "--iso")) iso = v;
        else if (!strcmp(a, "--agent")) agent = parse_enum(v, "off", "on");
        else if (!strcmp(a, "--clipboard")) clip = parse_clip(v);
        else { fprintf(stderr, "lord-vm: unknown option %s\n", a); return 2; }
        if ((!strcmp(a, "--display") && disp < 0) || (!strcmp(a, "--network") && net < 0) || (!strcmp(a, "--agent") && agent < 0) || (!strcmp(a, "--clipboard") && clip < 0)) { fprintf(stderr, "lord-vm: %s %s?\n", a, v); return 2; }
    }
    char real[PATH_MAX];
    if (!realpath(dir, real)) { fprintf(stderr, "lord-vm: %s: %s\n", dir, strerror(errno)); return 1; }
    int lock = open_lock(real);
    if (lock == -2) { fprintf(stderr, "lord-vm: the machine is running: shut it down first\n"); return 3; }
    if (lock < 0) { fprintf(stderr, "lord-vm: can't take the machine's lock\n"); return 1; }
    VmMachine m;
    char err[256], warn[256];
    int rc = 1;
    if (vm_machine_load(real, &m, err, sizeof err)) { fprintf(stderr, "lord-vm: %s\n", err); goto out; }
    snprintf(S.dir, sizeof S.dir, "%s", real);
    ops_recover(real, 0);
    VmMachine was = m;
    {
        char st[PATH_MAX + 32];
        snprintf(st, sizeof st, "%s/suspend.state", real);
        if (file_exists(st) && (vcpus || mem || disk || net >= 0 || agent >= 0)) { fprintf(stderr, "lord-vm: the machine is suspended: resume it and shut it down first, or discard its saved state\n"); goto out; }
    }
    if (name) snprintf(m.name, sizeof m.name, "%s", name);
    if (vcpus) m.vcpus = vcpus;
    if (mem) m.memory_mb = mem;
    if (disp >= 0) m.display = disp;
    if (net >= 0) m.network = net;
    if (agent >= 0) m.agent = agent;
    if (clip >= 0) m.clipboard = clip;
    if (m.agent || m.clipboard) m.schema = VM_SCHEMA;
    if (disk) {
        if (disk < m.disk_gb) { fprintf(stderr, "lord-vm: the disk can only grow (it has %d GB)\n", m.disk_gb); goto out; }
        m.disk_gb = disk;
    }
    if (iso) {
        if (!strcmp(iso, "none")) m.iso[0] = 0;
        else {
            char isoreal[PATH_MAX];
            if (!realpath(iso, isoreal) || !file_exists(isoreal) || strlen(isoreal) >= sizeof m.iso) { fprintf(stderr, "lord-vm: %s isn't a file (or its path is too long)\n", iso); goto out; }
            memcpy(m.iso, isoreal, strlen(isoreal) + 1);
        }
    }
    VmHost h;
    vm_host_probe(&h);
    if (vm_machine_check(&m, err, sizeof err) || vm_machine_check_host(&m, &h, err, sizeof err, warn, sizeof warn)) { fprintf(stderr, "lord-vm: %s\n", err); goto out; }
    if (warn[0]) fprintf(stderr, "lord-vm: note: %s\n", warn);
    if (m.disk_gb > was.disk_gb && m.base[0]) { fprintf(stderr, "lord-vm: a linked clone's disk can't grow: make a full clone of it first\n"); goto out; }
    if (m.disk_gb > was.disk_gb) {
        char sys[PATH_MAX + 64], dd[PATH_MAX + 64], size[32];
        snprintf(sys, sizeof sys, "%s/disks/system.qcow2", real);
        snprintf(dd, sizeof dd, "%s/disks", real);
        snprintf(size, sizeof size, "%dG", m.disk_gb);
        if (disk_plain(sys, err, sizeof err)) { fprintf(stderr, "lord-vm: system disk: %s\n", err); goto out; }
        char *rs[] = { QEMU_IMG, "resize", "-q", "-f", "qcow2", sys, size, NULL };
        if (run_tool_in(rs, err, sizeof err, sys, dd)) { fprintf(stderr, "lord-vm: growing the disk failed: %s\n", err); goto out; }
    }
    vm_now_iso(m.updated);
    if (vm_machine_save(real, &m)) { fprintf(stderr, "lord-vm: can't write machine.json\n"); goto out; }
    printf("{\"set\":true,\"vcpus\":%d,\"memory_mb\":%d,\"disk_gb\":%d,\"network\":\"%s\",\"iso\":%s,\"agent\":%s}\n", m.vcpus, m.memory_mb, m.disk_gb,
           m.network == VM_NET_SHARED ? "shared" : "isolated", m.iso[0] ? "true" : "false", m.agent ? "true" : "false");
    rc = 0;
out:
    close(lock);
    return rc;
}

/* --snapshot DIR take NAME [TAG] | revert TAG | delete TAG | list: under the lock when the machine is off (qemu-img,
 * jailed), through its socket when it runs (VM_SNAPSHOT, waiting for the job). Prints JSON. Exit 4: going back to a
 * snapshot with memory needs a start (lord-vm --start DIR --loadvm TAG). */
static int cmd_snapshot(const char *dir, int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "lord-vm: --snapshot DIR take NAME [TAG] | revert TAG | delete TAG | list\n"); return 2; }
    const char *what = argv[0];
    int kind = !strcmp(what, "take") ? VSN_TAKE : !strcmp(what, "revert") ? VSN_REVERT : !strcmp(what, "delete") ? VSN_DELETE : !strcmp(what, "list") ? 0 : -1;
    if (kind < 0 || (kind == VSN_TAKE && argc < 2) || ((kind == VSN_REVERT || kind == VSN_DELETE) && argc < 2)) { fprintf(stderr, "lord-vm: --snapshot DIR take NAME [TAG] | revert TAG | delete TAG | list\n"); return 2; }
    char real[PATH_MAX], err[256], tag[33] = "", name[VM_NAME_MAX + 1] = "";
    if (!realpath(dir, real)) { fprintf(stderr, "lord-vm: %s: %s\n", dir, strerror(errno)); return 1; }
    VmMachine m;
    if (vm_machine_load(real, &m, err, sizeof err)) { fprintf(stderr, "lord-vm: %s\n", err); return 1; }
    snprintf(S.dir, sizeof S.dir, "%s", real);
    if (kind == VSN_TAKE) {
        if (!vm_name_valid(argv[1])) { fprintf(stderr, "lord-vm: a snapshot's name is 1 to %d bytes of text\n", VM_NAME_MAX); return 2; }
        snprintf(name, sizeof name, "%s", argv[1]);
        if (argc > 2) snprintf(tag, sizeof tag, "%s", argv[2]); else vm_snap_tag(tag);
    } else if (kind) snprintf(tag, sizeof tag, "%s", argv[1]);
    if (kind && !vm_snap_tag_valid(tag)) { fprintf(stderr, "lord-vm: %s isn't a snapshot tag\n", tag); return 2; }
    if (!kind) {
        VmSnap list[VM_SNAPS_MAX];
        int n = vm_snaps_load(real, list, VM_SNAPS_MAX);
        printf("{\"snapshots\":[");
        for (int i = 0; i < n; i++) { printf("%s{\"tag\":\"%s\",\"name\":", i ? "," : "", list[i].tag); print_json_str(list[i].name); printf(",\"created\":\"%s\",\"memory\":%s,\"vmstate\":%.0f}", list[i].created, list[i].memory ? "true" : "false", list[i].vmstate); }
        printf("]}\n");
        return 0;
    }
    int lock = open_lock(real);
    if (lock == -2) {
        /* running: the machine's own lord-vm does it */
        int fd = connect_machine(&m);
        if (fd < 0) { fprintf(stderr, "lord-vm: the machine runs but its socket can't be reached\n"); return 1; }
        char text[VM_TEXT_MAX];
        snprintf(text, sizeof text, "%s%s%s", tag, kind == VSN_TAKE ? "\t" : "", kind == VSN_TAKE ? name : "");
        VmMsg q = vm_msg(VM_SNAPSHOT, 9, kind, 0, 0, 0, text);
        if (vm_send(fd, &q, -1)) { close(fd); fprintf(stderr, "lord-vm: can't talk to the machine\n"); return 1; }
        long long end = now_ms() + 30 * 60 * 1000;
        int rc = 1;
        for (;;) {
            VmMsg r;
            long long left = end - now_ms();
            int g = left > 0 ? wait_msg(fd, &r, (int)left) : 0;
            if (g <= 0) { fprintf(stderr, "lord-vm: no answer from the machine\n"); break; }
            if (r.type == VM_REPLY && r.seq == 9) {
                if (r.a == VME_OK) { printf("{\"ok\":true,\"tag\":\"%s\",\"running\":true}\n", tag); rc = 0; }
                else { fprintf(stderr, "lord-vm: %s\n", r.text); rc = r.a == VME_WRONG_STATE ? 4 : 1; }
                break;
            }
        }
        close(fd);
        return rc;
    }
    if (lock < 0) { fprintf(stderr, "lord-vm: can't take the machine's lock\n"); return 1; }
    ops_recover(real, 0);
    int rc = 1;
    if (kind == VSN_TAKE) rc = snap_take_off(real, &m, tag, name, err, sizeof err);
    else if (kind == VSN_DELETE) rc = snap_delete_off(real, &m, tag, err, sizeof err);
    else {
        VmSnap list[VM_SNAPS_MAX];
        int n = vm_snaps_load(real, list, VM_SNAPS_MAX), i = vm_snaps_find(list, n, tag);
        if (i >= 0 && list[i].memory) { close(lock); fprintf(stderr, "lord-vm: %s holds the machine's memory: start it with --loadvm %s\n", tag, tag); return 4; }
        rc = snap_revert_off(real, &m, tag, err, sizeof err);
    }
    close(lock);
    if (rc) { fprintf(stderr, "lord-vm: %s\n", err); return 1; }
    printf("{\"ok\":true,\"tag\":\"%s\",\"running\":false}\n", tag);
    return 0;
}

/* ---- clones (6.3), compaction (6.4, 6.16) ----
 *
 * lord-vm --clone SRC DST [--name N] [--duplicate] [--linked] [--from-snapshot TAG]
 *   Clone: a new machine that runs beside the original (new id and MAC). Duplicate: the same identity, the only kind
 *   that carries a saved session and the snapshots. The disks are, first choice to last: a reflink copy (btrfs, XFS:
 *   instant, shared until either changes); with --linked on a file system without reflink (ext4), the source's disk
 *   becomes a frozen base in <library>/.shared/ and both machines get a thin overlay on it; or a full copy (a Clone is
 *   converted, which flattens and compacts; a Duplicate is copied with its holes). The new machine is made in a hidden
 *   folder and renamed into place; the source must be off. Prints {"cloned":true,"dir":...,"method":...}. */

static int copy_sparse(const char *from, const char *to) {
    int a = open(from, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (a < 0) return -1;
    struct stat st;
    if (fstat(a, &st) || !S_ISREG(st.st_mode)) { close(a); return -1; }
    int b = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (b < 0) { close(a); return -1; }
    int bad = ftruncate(b, st.st_size);
    static char buf[1 << 20];
    for (off_t off = 0; !bad && off < st.st_size;) {
        off_t d = lseek(a, off, SEEK_DATA);
        if (d < 0) break;                               /* the rest is a hole */
        off_t h = lseek(a, d, SEEK_HOLE);
        if (h < 0) h = st.st_size;
        for (off_t p = d; !bad && p < h;) {
            size_t want = (size_t)(h - p) < sizeof buf ? (size_t)(h - p) : sizeof buf;
            ssize_t r = pread(a, buf, want, p);
            if (r <= 0) { bad = 1; break; }
            if (pwrite(b, buf, (size_t)r, p) != r) bad = 1;
            p += r;
        }
        off = h;
    }
    if (!bad && fsync(b)) bad = 1;
    close(a); close(b);
    if (bad) unlink(to);
    return bad ? -1 : 0;
}

/* a reflink copy (FICLONE): 0 done, -1 this file system can't (or failed) */
static int copy_reflink(const char *from, const char *to) {
    int a = open(from, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (a < 0) return -1;
    int b = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (b < 0) { close(a); return -1; }
    int r = ioctl(b, FICLONE, a);
    if (!r) r = fsync(b);
    close(a); close(b);
    if (r) unlink(to);
    return r ? -1 : 0;
}

/* qemu-img convert of a qcow2 (with its base when it has one), jailed: it reads the disk (and the base) and writes
 * only in out_dir. snap: a snapshot's tag to copy instead of the current state, or NULL */
static int qimg_convert(const char *disk, const char *base, const char *snap, const char *out, const char *out_dir, char *err, size_t errlen) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "json:{\"driver\":\"qcow2\",\"read-only\":true,\"file\":{\"driver\":\"file\",\"filename\":");
    jb_str(&b, disk);
    if (base && base[0]) { jb_raw(&b, "},\"backing\":{\"driver\":\"qcow2\",\"backing\":null,\"file\":{\"driver\":\"file\",\"filename\":"); jb_str(&b, base); jb_raw(&b, "}}}"); }
    else jb_raw(&b, "},\"backing\":null}");
    if (b.oom) { jb_free(&b); snprintf(err, errlen, "out of memory"); return -1; }
    char sn[64];
    snprintf(sn, sizeof sn, "snapshot.name=%s", snap ? snap : "");
    char *with[] = { QEMU_IMG, "convert", "-q", "-l", sn, "-O", "qcow2", b.p, (char *)out, NULL };
    char *without[] = { QEMU_IMG, "convert", "-q", "-O", "qcow2", b.p, (char *)out, NULL };
    jail_read2 = base && base[0] ? base : NULL;
    int r = run_tool_in(snap ? with : without, err, errlen, disk, out_dir);
    jail_read2 = NULL;
    jb_free(&b);
    return r;
}

/* qemu-img create of an overlay on base, without opening the base (-u, the size given) */
static int qimg_overlay(const char *base, const char *out, const char *out_dir, int gb, char *err, size_t errlen) {
    char size[32];
    snprintf(size, sizeof size, "%dG", gb);
    char *argv[] = { QEMU_IMG, "create", "-q", "-f", "qcow2", "-u", "-b", (char *)base, "-F", "qcow2", (char *)out, size, NULL };
    return run_tool_in(argv, err, errlen, out_dir, out_dir);      /* the overlay doesn't exist yet: its folder */
}

static void rm_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[PATH_MAX + 260];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            struct stat st;
            if (!lstat(p, &st) && S_ISDIR(st.st_mode)) rm_tree(p); else unlink(p);
        }
        closedir(d);
    }
    rmdir(dir);
}

static int copy_small(const char *from, const char *to) { return file_exists(from) ? copy_sparse(from, to) : 0; }

/* a clone that lord-vm didn't finish (killed) leaves <library>/.<name>.part-<pid>: removed once that lord-vm is gone */
static void clone_sweep(const char *lib) {
    DIR *d = opendir(lib);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        const char *part = e->d_name[0] == '.' ? strstr(e->d_name, ".part-") : NULL;
        if (!part) continue;
        long pid = atol(part + 6);
        if (pid > 0 && (kill((pid_t)pid, 0) == 0 || errno != ESRCH)) continue;
        char p[PATH_MAX + 260];
        snprintf(p, sizeof p, "%s/%s", lib, e->d_name);
        rm_tree(p);
        vlog("recovery: a clone that didn't finish was removed (%s)", e->d_name);
    }
    if (d) closedir(d);
}

static int cmd_clone(const char *src, int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "lord-vm: --clone SRC DST [--name N] [--duplicate] [--linked] [--from-snapshot TAG]\n"); return 2; }
    const char *dst = argv[0], *name = NULL, *from_snap = NULL;
    int duplicate = 0, linked = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--duplicate")) duplicate = 1;
        else if (!strcmp(argv[i], "--linked")) linked = 1;
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--from-snapshot") && i + 1 < argc) from_snap = argv[++i];
        else { fprintf(stderr, "lord-vm: unknown option %s\n", argv[i]); return 2; }
    }
    char real[PATH_MAX], err[256];
    if (!realpath(src, real)) { fprintf(stderr, "lord-vm: %s: %s\n", src, strerror(errno)); return 1; }
    snprintf(S.dir, sizeof S.dir, "%s", real);
    int lock = open_lock(real);
    if (lock == -2) { fprintf(stderr, "lord-vm: the machine is running: shut it down first\n"); return 3; }
    if (lock < 0) { fprintf(stderr, "lord-vm: can't take the machine's lock\n"); return 1; }
    ops_recover(real, 0);
    VmMachine m;
    int rc = 1;
    char tmp[PATH_MAX + 64] = "", lib[PATH_MAX], dreal[PATH_MAX + 64];
    if (vm_machine_load(real, &m, err, sizeof err)) { fprintf(stderr, "lord-vm: %s\n", err); goto out; }
    char st[PATH_MAX + 32];
    snprintf(st, sizeof st, "%s/suspend.state", real);
    int suspended = file_exists(st);
    if (suspended && !duplicate) { fprintf(stderr, "lord-vm: the machine has a saved session, which holds its identity: resume it and shut it down first, or make a Duplicate\n"); goto out; }
    if (from_snap && (duplicate || !vm_snap_tag_valid(from_snap))) { fprintf(stderr, "lord-vm: --from-snapshot makes a Clone from one snapshot\n"); goto out; }
    VmSnap snaps[VM_SNAPS_MAX];
    int nsnaps = vm_snaps_load(real, snaps, VM_SNAPS_MAX);
    if (from_snap && vm_snaps_find(snaps, nsnaps, from_snap) < 0) { fprintf(stderr, "lord-vm: there's no snapshot %s\n", from_snap); goto out; }
    /* the new folder: beside the source, made hidden and renamed into place */
    snprintf(lib, sizeof lib, "%s", real);
    char *sl = strrchr(lib, '/');
    if (!sl || sl == lib) goto out;
    *sl = 0;
    clone_sweep(lib);
    if (!dir_ok(dst) || strchr(dst, '/') == NULL) { fprintf(stderr, "lord-vm: %s: an absolute folder without ',' or control characters\n", dst); goto out; }
    snprintf(dreal, sizeof dreal, "%s", dst);
    if (!access(dreal, F_OK)) { fprintf(stderr, "lord-vm: %s exists\n", dreal); goto out; }
    {
        char dparent[PATH_MAX + 64];
        snprintf(dparent, sizeof dparent, "%s", dreal);
        char *ds = strrchr(dparent, '/');
        *ds = 0;
        char rp[PATH_MAX];
        if (!realpath(dparent, rp) || strcmp(rp, lib)) { fprintf(stderr, "lord-vm: the clone goes beside the machine, in %s\n", lib); goto out; }
        snprintf(tmp, sizeof tmp, "%s/.%s.part-%d", lib, ds + 1, (int)getpid());
    }
    char p1[PATH_MAX + 128], p2[PATH_MAX + 128];
    if (mkdir(tmp, 0700)) { fprintf(stderr, "lord-vm: can't make %s: %s\n", tmp, strerror(errno)); tmp[0] = 0; goto out; }
    snprintf(p1, sizeof p1, "%s/disks", tmp); mkdir(p1, 0700);
    snprintf(p1, sizeof p1, "%s/log", tmp); mkdir(p1, 0700);
    char sdisk[PATH_MAX + 64], ddisk[PATH_MAX + 128], snv[PATH_MAX + 64], dnv[PATH_MAX + 128], ddir[PATH_MAX + 64];
    snprintf(sdisk, sizeof sdisk, "%s/disks/system.qcow2", real);
    snprintf(ddisk, sizeof ddisk, "%s/disks/system.qcow2", tmp);
    snprintf(snv, sizeof snv, "%s/disks/nvram.qcow2", real);
    snprintf(dnv, sizeof dnv, "%s/disks/nvram.qcow2", tmp);
    snprintf(ddir, sizeof ddir, "%s/disks", tmp);
    const char *method = "copy";
    VmMachine n = m;
    /* 1. reflink, whatever the kind (a snapshot to start from needs a convert) */
    if (!from_snap && !copy_reflink(sdisk, ddisk)) {
        method = "reflink";
        if ((m.firmware == VM_FW_UEFI && copy_reflink(snv, dnv) && copy_sparse(snv, dnv))) { fprintf(stderr, "lord-vm: copying the UEFI variables failed\n"); goto out; }
    } else if (linked && !duplicate && !from_snap && !nsnaps && !suspended && !m.base[0]) {
        /* 2. linked over a frozen base: the source's disk moves to .shared/ (operation.json "link" in the source) */
        char shared[PATH_MAX + 32], base[PATH_MAX + 128], ov[PATH_MAX + 64];
        snprintf(shared, sizeof shared, "%s/.shared", lib);
        mkdir(shared, 0700);
        char when[24];
        vm_snap_tag(when);
        snprintf(base, sizeof base, "%s/%.8s-%s.qcow2", shared, m.id, when + 1);
        if (!vm_base_valid(base)) { fprintf(stderr, "lord-vm: the library's path can't hold a shared disk\n"); goto out; }
        if (vm_op_begin(real, "link", NULL, base, tmp)) { fprintf(stderr, "lord-vm: can't write operation.json\n"); goto out; }
        if (rename(sdisk, base)) { vm_op_end(real); fprintf(stderr, "lord-vm: can't move the disk to %s: %s\n", shared, strerror(errno)); goto out; }
        chmod(base, 0400);
        vm_sync_dir(shared);
        snprintf(ov, sizeof ov, "%s/disks/.system.qcow2.new", real);
        char sd[PATH_MAX + 32];
        snprintf(sd, sizeof sd, "%s/disks", real);
        if (qimg_overlay(base, ov, sd, m.disk_gb, err, sizeof err) || rename(ov, sdisk)) {
            unlink(ov); rename(base, sdisk); chmod(sdisk, 0600); vm_op_end(real);
            fprintf(stderr, "lord-vm: making the overlay failed: %s\n", err); goto out;
        }
        VmMachine sm = m;
        snprintf(sm.base, sizeof sm.base, "%.1023s", base);
        vm_now_iso(sm.updated);
        if (vm_machine_save(real, &sm)) {
            unlink(sdisk); rename(base, sdisk); chmod(sdisk, 0600); vm_op_end(real);
            fprintf(stderr, "lord-vm: can't write machine.json\n"); goto out;
        }
        if (qimg_overlay(base, ddisk, ddir, m.disk_gb, err, sizeof err)) { fprintf(stderr, "lord-vm: making the clone's overlay failed: %s\n", err); vm_op_end(real); goto out; }
        if (m.firmware == VM_FW_UEFI && copy_sparse(snv, dnv)) { fprintf(stderr, "lord-vm: copying the UEFI variables failed\n"); vm_op_end(real); goto out; }
        snprintf(n.base, sizeof n.base, "%.1023s", base);
        method = "linked";
    } else if (duplicate) {
        /* 3a. a Duplicate is the same disks, snapshots and all, holes kept */
        if (copy_sparse(sdisk, ddisk) || (m.firmware == VM_FW_UEFI && copy_sparse(snv, dnv))) { fprintf(stderr, "lord-vm: copying the disks failed: %s\n", strerror(errno)); goto out; }
    } else {
        /* 3b. a Clone is converted: flattened (a linked clone's base folded in), compacted, without snapshots */
        if (qimg_convert(sdisk, m.base, from_snap, ddisk, ddir, err, sizeof err)) { fprintf(stderr, "lord-vm: copying the disk failed: %s\n", err); goto out; }
        if (m.firmware == VM_FW_UEFI && (from_snap ? qimg_convert(snv, NULL, from_snap, dnv, ddir, err, sizeof err) : copy_sparse(snv, dnv))) { fprintf(stderr, "lord-vm: copying the UEFI variables failed: %s\n", err); goto out; }
        n.base[0] = 0;
    }
    if (duplicate) {
        /* the snapshots' list and pictures, the saved session */
        snprintf(p1, sizeof p1, "%s/snapshots.json", real); snprintf(p2, sizeof p2, "%s/snapshots.json", tmp);
        if (copy_small(p1, p2)) goto out;
        snprintf(p1, sizeof p1, "%s/snapshots", real);
        DIR *sd = opendir(p1);
        if (sd) {
            snprintf(p2, sizeof p2, "%s/snapshots", tmp);
            mkdir(p2, 0700);
            struct dirent *e;
            while ((e = readdir(sd))) {
                if (e->d_name[0] == '.' || strlen(e->d_name) > 60) continue;
                char a[PATH_MAX + 200], b[PATH_MAX + 200];
                snprintf(a, sizeof a, "%s/%s", p1, e->d_name); snprintf(b, sizeof b, "%s/snapshots/%s", tmp, e->d_name);
                copy_small(a, b);
            }
            closedir(sd);
        }
        if (suspended) {
            snprintf(p1, sizeof p1, "%s/suspend.state", real); snprintf(p2, sizeof p2, "%s/suspend.state", tmp);
            if (copy_reflink(p1, p2) && copy_sparse(p1, p2)) { fprintf(stderr, "lord-vm: copying the saved session failed\n"); goto out; }
            snprintf(p1, sizeof p1, "%s/suspend.json", real); snprintf(p2, sizeof p2, "%s/suspend.json", tmp);
            if (copy_small(p1, p2)) goto out;
        }
    } else {
        /* a Clone runs beside the original: its own identity (the agent can give the guest a new machine-id later) */
        vm_new_id(n.id);
        vm_new_mac(n.mac);
    }
    snprintf(p1, sizeof p1, "%s/app.json", real); snprintf(p2, sizeof p2, "%s/app.json", tmp); copy_small(p1, p2);
    snprintf(p1, sizeof p1, "%s/thumb.png", real); snprintf(p2, sizeof p2, "%s/thumb.png", tmp); copy_small(p1, p2);
    if (name) {
        if (!vm_name_valid(name)) { fprintf(stderr, "lord-vm: the name isn't valid\n"); goto out; }
        snprintf(n.name, sizeof n.name, "%s", name);
    }
    vm_now_iso(n.created);
    memcpy(n.updated, n.created, sizeof n.updated);
    if (vm_machine_save(tmp, &n)) { fprintf(stderr, "lord-vm: can't write the clone's machine.json\n"); goto out; }
    if (rename(tmp, dreal)) { fprintf(stderr, "lord-vm: can't put the clone in place: %s\n", strerror(errno)); goto out; }
    tmp[0] = 0;
    vm_sync_dir(lib);
    if (!strcmp(method, "linked")) vm_op_end(real);
    printf("{\"cloned\":true,\"dir\":");
    print_json_str(dreal);
    printf(",\"id\":\"%s\",\"method\":\"%s\"}\n", n.id, method);
    rc = 0;
out:
    if (tmp[0]) rm_tree(tmp);
    close(lock);
    return rc;
}

/* lord-vm --compact DIR [--estimate]: the system disk rewritten without the space it no longer uses (6.4). The machine
 * must be off, without snapshots (they'd be lost), a saved session or a base. Under operation.json "compact": the new
 * file is made beside the old one, checked, and renamed over it. --estimate prints what it would take. */
static int cmd_compact(const char *dir, int argc, char **argv) {
    int estimate = argc > 0 && !strcmp(argv[0], "--estimate");
    char real[PATH_MAX], err[256];
    if (!realpath(dir, real)) { fprintf(stderr, "lord-vm: %s: %s\n", dir, strerror(errno)); return 1; }
    snprintf(S.dir, sizeof S.dir, "%s", real);
    VmMachine m;
    if (vm_machine_load(real, &m, err, sizeof err)) { fprintf(stderr, "lord-vm: %s\n", err); return 1; }
    char sys[PATH_MAX + 64], dd[PATH_MAX + 64], tmp[PATH_MAX + 64];
    snprintf(sys, sizeof sys, "%s/disks/system.qcow2", real);
    snprintf(dd, sizeof dd, "%s/disks", real);
    snprintf(tmp, sizeof tmp, "%s/disks/.system.qcow2.compact", real);
    struct stat sb;
    if (stat(sys, &sb)) { fprintf(stderr, "lord-vm: the system disk is missing\n"); return 1; }
    long long before = (long long)sb.st_blocks * 512;
    VmSnap snaps[VM_SNAPS_MAX];
    int nsnaps = vm_snaps_load(real, snaps, VM_SNAPS_MAX);
    char st[PATH_MAX + 32];
    snprintf(st, sizeof st, "%s/suspend.state", real);
    const char *why = nsnaps ? "it has snapshots, which compaction would drop: delete them first" : file_exists(st) ? "it has a saved session" : m.base[0] ? "it's a linked clone" : NULL;
    if (estimate) {
        JBuf b;
        jb_init(&b);
        jb_raw(&b, "json:{\"driver\":\"qcow2\",\"read-only\":true,\"backing\":null,\"file\":{\"driver\":\"file\",\"filename\":");
        jb_str(&b, sys);
        jb_raw(&b, "}}");
        static char out[8192];
        char *argv2[] = { QEMU_IMG, "measure", "--output=json", "-U", "-O", "qcow2", b.p, NULL };
        int r = b.oom ? -1 : run_tool_in(argv2, out, sizeof out, sys, NULL);
        jb_free(&b);
        JDoc d;
        double req = -1;
        if (!r && !json_parse(out, strlen(out), &d, NULL, 0)) { req = json_num(d.root, "required", -1); json_free(&d); }
        printf("{\"allocated\":%lld,\"required\":%.0f,\"possible\":%s,\"why\":", before, req, why ? "false" : "true");
        print_json_str(why ? why : "");
        printf("}\n");
        return req < 0 ? 1 : 0;
    }
    if (why) { fprintf(stderr, "lord-vm: can't compact: %s\n", why); return 1; }
    int lock = open_lock(real);
    if (lock == -2) { fprintf(stderr, "lord-vm: the machine is running: shut it down first\n"); return 3; }
    if (lock < 0) { fprintf(stderr, "lord-vm: can't take the machine's lock\n"); return 1; }
    ops_recover(real, 0);
    int rc = 1;
    unlink(tmp);
    if (vm_op_begin(real, "compact", NULL, tmp, NULL)) { fprintf(stderr, "lord-vm: can't write operation.json\n"); goto out; }
    if (qimg_convert(sys, NULL, NULL, tmp, dd, err, sizeof err)) { fprintf(stderr, "lord-vm: compacting failed: %s\n", err); unlink(tmp); vm_op_end(real); goto out; }
    {
        /* the new file is checked before it replaces the old one */
        char *ck[] = { QEMU_IMG, "check", "-q", "-f", "qcow2", tmp, NULL };
        int fd = open(tmp, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { fsync(fd); close(fd); }
        if (run_tool_in(ck, err, sizeof err, tmp, NULL)) { fprintf(stderr, "lord-vm: the compacted disk didn't pass its check: %s\n", err); unlink(tmp); vm_op_end(real); goto out; }
    }
    if (rename(tmp, sys)) { fprintf(stderr, "lord-vm: can't replace the disk: %s\n", strerror(errno)); unlink(tmp); vm_op_end(real); goto out; }
    vm_sync_dir(dd);
    vm_op_end(real);
    stat(sys, &sb);
    printf("{\"compacted\":true,\"before\":%lld,\"after\":%lld}\n", before, (long long)sb.st_blocks * 512);
    rc = 0;
out:
    close(lock);
    return rc;
}

/* --discard-state DIR: the saved state goes (under the lock), and the next start boots from the disks */
static int cmd_discard(const char *dir) {
    char real[PATH_MAX];
    if (!realpath(dir, real)) { fprintf(stderr, "lord-vm: %s: %s\n", dir, strerror(errno)); return 1; }
    int lock = open_lock(real);
    if (lock == -2) { fprintf(stderr, "lord-vm: the machine is running\n"); return 3; }
    if (lock < 0) { fprintf(stderr, "lord-vm: can't take the machine's lock\n"); return 1; }
    snprintf(S.dir, sizeof S.dir, "%s", real);
    ops_recover(real, 0);
    discard_state(real);
    RunInfo r;
    if (!runinfo_read(real, &r) && (r.reason == VR_SUSPENDED || r.reason == VR_RESUME_FAILED)) {
        r.state = VS_STOPPED; r.reason = VR_POWEROFF; r.code = 0;
        snprintf(r.detail, sizeof r.detail, "the saved state was discarded");
        vm_now_iso(r.since);
        runinfo_write(real, &r);
    }
    close(lock);
    printf("{\"discarded\":true}\n");
    return 0;
}

static int cmd_check(const char *dir) {
    VmMachine m;
    char err[256], warn[256];
    int r = vm_machine_load(dir, &m, err, sizeof err);
    if (r) { printf("{\"ok\":false,\"code\":%d,\"error\":\"%s\"}\n", r, err); return 1; }
    VmHost h;
    vm_host_probe(&h);
    r = vm_machine_check_host(&m, &h, err, sizeof err, warn, sizeof warn);
    printf("{\"ok\":%s,\"error\":\"%s\",\"warning\":\"%s\",\"host_cpus\":%d,\"host_ram_mb\":%ld,\"kvm\":%d,\"mem_max_mb\":%ld,\"vcpus_recommended\":%d,\"mem_limit_mb\":%ld}\n",
           r ? "false" : "true", r ? err : "", warn, h.cpus, h.ram_mb, h.kvm, vm_mem_max_mb(&h), vm_vcpus_recommended(&h), vm_mem_limit_mb(&m));
    return r ? 1 : 0;
}

static int usage(void) {
    fprintf(stderr,
            "usage: lord-vm --create DIR [--name N] [--arch arm64|amd64] [--accel kvm|tcg] [--vcpus N] [--memory MB] [--disk GB]\n"
            "                            [--firmware uefi|bios] [--display window|none] [--network shared|isolated] [--import IMAGE] [--iso FILE]\n"
            "       lord-vm --set DIR [--name N] [--vcpus N] [--memory MB] [--disk GB] [--display window|none] [--network shared|isolated] [--iso FILE|none]\n"
            "                         [--agent on|off] [--clipboard off|to-guest|from-guest|both]   (also on --create)\n"
            "       lord-vm --start DIR [--paused] [--detach] [--control-fd N] [--keep-running] [--dev-qmp] [--credentials-file FILE]\n"
            "                           [--resume-anyway] [--discard-state]\n"
            "       lord-vm --suspend DIR | --discard-state DIR | --agent DIR shutdown|timesync|install\n"
            "       lord-vm --snapshot DIR take NAME [TAG] | revert TAG | delete TAG | list   (and --start DIR --loadvm TAG)\n"
            "       lord-vm --clone DIR NEWDIR [--name N] [--duplicate] [--linked] [--from-snapshot TAG]\n"
            "       lord-vm --compact DIR [--estimate]\n"
            "       lord-vm --status DIR [--json] | --check DIR | --stop DIR [--timeout S] | --poweroff DIR | --pause DIR | --resume DIR | --reset DIR\n"
            "       lord-vm --cdrom DIR IMAGE|none   (a running machine's CD/DVD drive)\n"
            "       lord-vm --qmp DIR 'JSON'   (machines started with --dev-qmp)\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 3) return usage();
    const char *cmd = argv[1], *dir = argv[2];
    int rest = argc - 3;
    char **rv = argv + 3;
    if (!strcmp(cmd, "--create")) return cmd_create(dir, rest, rv);
    if (!strcmp(cmd, "--start")) return cmd_start(dir, rest, rv);
    if (!strcmp(cmd, "--check")) return cmd_check(dir);
    if (!strcmp(cmd, "--set")) return cmd_set(dir, rest, rv);
    if (!strcmp(cmd, "--discard-state")) return cmd_discard(dir);
    if (!strcmp(cmd, "--snapshot")) return cmd_snapshot(dir, rest, rv);
    if (!strcmp(cmd, "--clone")) return cmd_clone(dir, rest, rv);
    if (!strcmp(cmd, "--compact")) return cmd_compact(dir, rest, rv);
    static const struct { const char *name; int type; } C[] = {
        { "--status", VM_STATUS }, { "--stop", VM_SHUTDOWN }, { "--poweroff", VM_POWEROFF }, { "--pause", VM_PAUSE },
        { "--resume", VM_RESUME }, { "--reset", VM_RESET }, { "--qmp", VM_QMP }, { "--cdrom", VM_CDROM }, { "--suspend", VM_SUSPEND }, { "--agent", VM_AGENT },
    };
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) if (!strcmp(cmd, C[i].name)) return cmd_client(dir, C[i].type, rest, rv);
    return usage();
}
