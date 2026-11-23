/* lordvm.c — lord-vm: the supervisor of one machine (docs/VIRTUALIZATION.md 3.1, 3.3, 6.1 and "Phase 1
 * implementation"). One process per running machine. It reads machine.json, places itself in the machine's
 * cgroup scope, opens /dev/kvm, starts passt and QEMU in their sandboxes, holds QEMU's control socket (QMP),
 * reports state, and exits when QEMU has exited. It draws nothing: the picture is lord-vmdisplay's.
 *
 *   lord-vm --create DIR [--name N] [--arch arm64|amd64] [--accel kvm|tcg] [--vcpus N] [--memory MB]
 *                        [--disk GB] [--firmware uefi|bios] [--display window|none]
 *                        [--network shared|isolated|bridged|hostonly]
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
#include "vmgpu.h"
#include "qmp.h"
#include "vmproto.h"
#include "vmsandbox.h"
#include "vmops.h"
#include "vmagent.h"
#include "vmimport.h"
#include "vmunattend.h"
#include "vmhoststop.h"
#include "vmgvtwin.h"
#include "vmlog.h"
#include "../sec/cgroup.h"
#include "../sec/json.h"
#include "../doc/wire.h"
#include "../pkg/crypto.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
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
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef VM_LOGIND
#include "vmlogind.h"
#endif

#define VM_DNS_ADDR "169.254.0.53"     /* the guest's nameserver in Shared mode (passt answers for it) */
#ifdef LORDVM_TEST_PATHS
/* A test build only (make build/lord-vm-testpaths, never installed): the release gates on a host whose QEMU, firmware
 * or passt live elsewhere (a hypervisor host with its own QEMU build, passt unpacked into a scratch folder) take
 * them from absolute paths in LORDVM_TEST_QEMU_<ARCH>, LORDVM_TEST_QEMU_IMG, LORDVM_TEST_PASST,
 * LORDVM_TEST_FW_CODE and LORDVM_TEST_FW_VARS, without changing the host. The package's lord-vm has none of this. */
static char *test_path(const char *env, const char *def) {
    const char *v = getenv(env);
    return (char *)(v && v[0] == '/' && !strstr(v, "..") ? v : def);
}
#define QEMU_IMG test_path("LORDVM_TEST_QEMU_IMG", "/usr/bin/qemu-img")
#define PASST    test_path("LORDVM_TEST_PASST", "/usr/bin/passt")
#define VIRTIOFSD test_path("LORDVM_TEST_VIRTIOFSD", "/usr/libexec/virtiofsd")
#define SWTPM    test_path("LORDVM_TEST_SWTPM", "/usr/bin/swtpm")
#define QEMU_BIN_OF(a) test_path((a) ? "LORDVM_TEST_QEMU_AMD64" : "LORDVM_TEST_QEMU_ARM64", QEMU_BIN[a])
#else
#define QEMU_IMG "/usr/bin/qemu-img"
#define PASST    "/usr/bin/passt"
#define VIRTIOFSD "/usr/libexec/virtiofsd"
#define SWTPM    "/usr/bin/swtpm"
#define QEMU_BIN_OF(a) QEMU_BIN[a]
#endif
static const char *const QEMU_BIN[2] = { "/usr/bin/qemu-system-aarch64", "/usr/bin/qemu-system-x86_64" };

/* UEFI code and variable templates, by architecture, first found wins */
static const char *const FW_ARM64[][2] = {
    { "/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd", "/usr/share/AAVMF/AAVMF_VARS.fd" },
    { "/usr/share/AAVMF/AAVMF_CODE.fd", "/usr/share/AAVMF/AAVMF_VARS.fd" },
};
static const char *const FW_AMD64[][2] = {
    { "/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd" },
};
/* secure boot (Windows 11, 4.3): the code built with it and the variables with Microsoft's keys enrolled (Debian's ovmf) */
static const char *const FW_AMD64_SB[2] = { "/usr/share/OVMF/OVMF_CODE_4M.secboot.fd", "/usr/share/OVMF/OVMF_VARS_4M.ms.fd" };

#define LOG_CAP      (4 << 20)
#define SERIAL_CAP   (1 << 20)
#define MAX_CLIENTS  8
#define MAX_PENDING  64
#define MAX_CREDS    16

static const char *const STATE_NAME[VS_COUNT] = { "stopped", "starting", "running", "paused", "stopping", "crashed" };
static const char *const REASON_NAME[VR_COUNT] = {
    "none", "guest-shutdown", "host-shutdown", "forced", "poweroff", "qemu-failed", "qemu-signal", "out-of-memory",
    "start-failed", "guest-panic", "supervisor-lost", "reset", "user", "suspended", "resume-failed", "installed",
    "qemu-3d-crash", "qemu-3d-stop",
};

/* ---- log ---- */

static long long now_ms_log(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int log_fd = -1, log_stderr = 1;
static off_t log_bytes;
/* the folder lord-vm.log is rotated in when it reaches LOG_CAP (vmlog.h); "" none: that log stops at its cap */
static char log_dir[PATH_MAX];
#define LOG_KEEP 3                      /* lord-vm.log, .1 and .2: at most 3 x LOG_CAP */

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
    /* full: rotated, never silent for the rest of the run (before 2026-11-22 it stopped writing at the cap) */
    if (log_fd >= 0 && log_dir[0] && log_bytes + n > LOG_CAP) {
        close(log_fd);
        log_fd = vmlog_rotate(log_dir, "lord-vm", LOG_KEEP);
        log_bytes = 0;
        static const char head[] = "lord-vm: the log goes on in a new file (lord-vm.1.log holds what came before)\n";
        if (log_fd >= 0 && write(log_fd, head, sizeof head - 1) > 0) log_bytes += (off_t)sizeof head - 1;
    }
    if (log_fd >= 0 && log_bytes < LOG_CAP) { if (write(log_fd, line, (size_t)n) > 0) log_bytes += n; }
    if (log_stderr) { ssize_t w = write(2, line, (size_t)n); (void)w; }
}

/* A child's stderr (QEMU, passt) into the log: whole lines, however its writes were cut, and a line that comes again
 * logged once, then counted (vmlog.h): QEMU calling a closed D-Bus listener wrote the same error every 30 ms */
typedef struct { const char *who; VmLineBuf lines; VmLogRep rep; } ChildLog;
static ChildLog child_log[] = { { .who = "qemu" }, { .who = "passt" }, { .who = "child" } };
#define CHILD_LOGS ((int)(sizeof child_log / sizeof child_log[0]))

static ChildLog *child_of(const char *who) {
    for (int i = 0; i < CHILD_LOGS - 1; i++) if (!strcmp(child_log[i].who, who)) return &child_log[i];
    return &child_log[CHILD_LOGS - 1];
}

/* this run's QEMU wrote its known 3D abort (vmlog_qemu_3d_abort): its end is named by vmlog_3d_abort_end (a shutdown, a
 * restart, a stop by the guest, or VR_QEMU_3D_ABORT), not a plain crash. What that needs from the run: the guest's
 * tools said they power off or restart (AGK_STOPPING, AG_STOP_*; 0 nothing), tools that would say so were connected
 * (their hello said "stopping"), and when the run first ran (now_ms, 0 never) */
static int qemu_3d_abort_seen, stop_notice, stop_announces;
static long long run_t0;

static void child_line(void *ctx, const char *line, size_t n) {
    ChildLog *c = ctx;
    char sum[160];
    if (c == &child_log[0] && vmlog_qemu_3d_abort(line, n)) qemu_3d_abort_seen = 1;
    int say = vmlog_repeat(&c->rep, line, n, now_ms_log(), sum, sizeof sum);
    if (sum[0]) vlog("%s: %s", c->who, sum);
    if (say) vlog("%s: %.*s", c->who, (int)(n > 900 ? 900 : n), line);
}

static void log_child(const char *who, const char *buf, size_t n) {
    ChildLog *c = child_of(who);
    vmlog_lines(&c->lines, buf, n, child_line, c);
}

/* the loop's tick: repeats not said yet (force: the run ends, and a line cut short is said too) */
static void child_logs_tick(long long now, int force) {
    for (int i = 0; i < CHILD_LOGS; i++) {
        ChildLog *c = &child_log[i];
        if (force) vmlog_lines_flush(&c->lines, child_line, c);
        char sum[160];
        if (vmlog_repeat_tick(&c->rep, force ? c->rep.said_at + VMLOG_SUMMARY_MS : now, sum, sizeof sum)) vlog("%s: %s", c->who, sum);
    }
}

/* when the next summary of repeats is due, or -1 */
static long long child_logs_next(void) {
    long long next = -1;
    for (int i = 0; i < CHILD_LOGS; i++)
        if (child_log[i].rep.rep && (next < 0 || child_log[i].rep.said_at + VMLOG_SUMMARY_MS < next)) next = child_log[i].rep.said_at + VMLOG_SUMMARY_MS;
    return next;
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

/* the SHA-256 of what a descriptor holds, from its start (pread: its offset stays) */
static int sha256_fd(int fd, char out[65]) {
    Sha256 s;
    sha256_init(&s);
    static uint8_t buf[1 << 16];
    for (off_t off = 0;;) {
        ssize_t r = pread(fd, buf, sizeof buf, off);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (!r) break;
        sha256_update(&s, buf, (size_t)r);
        off += r;
    }
    uint8_t d[32];
    sha256_final(&s, d);
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
    return 0;
}

static int sha256_file(const char *path, char out[65]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int r = sha256_fd(fd, out);
    close(fd);
    return r;

}
/* runs a program (no shell) and waits; its output goes to the log. 0 when it exited with 0. With `jail_read`, the
 * program runs inside vm_jail_tool (it may read only that file and write only in `jail_rw`) or not at all. */
static int run_tool_in(char *const argv[], char *out, size_t outcap, const char *jail_read, const char *jail_rw);
static int run_tool(char *const argv[], char *out, size_t outcap) { return run_tool_in(argv, out, outcap, NULL, NULL); }
static const char *jail_read2;          /* a second file the next jailed tool may read (a linked clone's base) */
static const char *const *jail_list;    /* or a list of them (an import: a descriptor, its extents, its snapshot layers) */
static int jail_n;
static void (*tool_progress)(double pct); /* qemu-img convert -p's "(12.34/100%)", as it comes */
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
            int mask = jail_n > 0 ? vm_jail_tool_n(jail_list, jail_n, jail_rw) : vm_jail_tool2(jail_read, jail_read2, jail_rw);
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
        if (tool_progress) {
            /* "    (45.00/100%)\r": the last complete one in this read */
            double last = -1;
            for (ssize_t i = 0; i < r; i++) if (tmp[i] == '(') {
                char *e;
                double v = strtod(tmp + i + 1, &e);
                if (e > tmp + i + 1 && e < tmp + r && *e == '/' && v >= 0 && v <= 100) last = v;
            }
            if (last >= 0) tool_progress(last);
            /* what isn't progress (an error) is kept */
            ssize_t o = 0;
            for (ssize_t i = 0; i < r; i++) {
                if (tmp[i] == '(') {
                    ssize_t k = i + 1;
                    while (k < r && k < i + 16 && tmp[k] != ')') k++;
                    if (k < r && tmp[k] == ')' && memchr(tmp + i, '%', (size_t)(k - i))) { i = k; continue; }
                }
                if (tmp[i] != '\r') tmp[o++] = tmp[i];
            }
            r = o;
        }
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
    char *argv[] = { (char *)QEMU_BIN_OF(arch), "-machine", "help", NULL };
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

static int firmware_for(int arch, const char **code, const char **vars);
static int firmware_sb(const char **code, const char **vars) {
    if (!file_exists(FW_AMD64_SB[0]) || !file_exists(FW_AMD64_SB[1])) return -1;
    *code = FW_AMD64_SB[0]; *vars = FW_AMD64_SB[1];
    return 0;
}
static int firmware_for(int arch, const char **code, const char **vars) {
#ifdef LORDVM_TEST_PATHS
    const char *tc = test_path("LORDVM_TEST_FW_CODE", ""), *tv = test_path("LORDVM_TEST_FW_VARS", "");
    if (tc[0] && tv[0] && file_exists(tc) && file_exists(tv)) { *code = tc; *vars = tv; return 0; }
#endif
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
static int parse_net(const char *s) { for (int i = 0; VM_NET_NAMES[i]; i++) if (!strcmp(s, VM_NET_NAMES[i])) return i; return -1; }
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

/* the machine's writable disks, in snapshot order: the system disk, the UEFI variables, then the data disks (6.22) */
#define VM_NDISKS (2 + VM_DATA_DISKS)
static int disk_files(const char *dir, const VmMachine *m, char out[VM_NDISKS][PATH_MAX + 64]) {
    int n = 0;
    snprintf(out[n++], PATH_MAX + 64, "%s/disks/system.qcow2", dir);
    if (m->firmware == VM_FW_UEFI) snprintf(out[n++], PATH_MAX + 64, "%s/disks/nvram.qcow2", dir);
    for (int i = 0; i < m->n_data; i++) snprintf(out[n++], PATH_MAX + 64, "%s/disks/data-%d.qcow2", dir, i + 1);
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
    char disks[VM_NDISKS][PATH_MAX + 64];
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

/* The TPM's state (4.3) isn't a disk qemu-img can snapshot: each snapshot keeps a copy of swtpm's one state file,
 * disks/tpm/snap-<tag>.permall (a tag is letters, digits and '-'). TPMS_TAKE copies the state there, TPMS_REVERT copies it
 * back, TPMS_DROP removes it. Machines without a TPM have nothing to do. */
enum { TPMS_TAKE, TPMS_REVERT, TPMS_DROP };
static int copy_sparse(const char *from, const char *to);
static int tpm_snap(const char *dir, const VmMachine *m, int op, const char *tag) {
    if (!m->tpm || !vm_snap_tag_valid(tag)) return 0;
    char cur[PATH_MAX + 64], snap[PATH_MAX + 128];
    snprintf(cur, sizeof cur, "%s/disks/tpm/tpm2-00.permall", dir);
    snprintf(snap, sizeof snap, "%s/disks/tpm/snap-%s.permall", dir, tag);
    if (op == TPMS_DROP) { unlink(snap); return 0; }
    const char *from = op == TPMS_TAKE ? cur : snap, *to = op == TPMS_TAKE ? snap : cur;
    if (!file_exists(from)) { if (op == TPMS_REVERT) unlink(cur); return 0; }   /* no state then: none now */
    /* a whole new file, then renamed over: never half a state */
    char tmp[PATH_MAX + 160];
    snprintf(tmp, sizeof tmp, "%s.new", to);
    unlink(tmp);
    if (copy_sparse(from, tmp)) { unlink(tmp); return -1; }
    chmod(tmp, 0600);
    if (rename(tmp, to)) { unlink(tmp); return -1; }
    return 0;
}

static int snap_take_off(const char *dir, const VmMachine *m, const char *tag, const char *name, char *err, size_t errlen) {
    VmSnap list[VM_SNAPS_MAX];
    int n = vm_snaps_load(dir, list, VM_SNAPS_MAX);
    if (n >= VM_SNAPS_MAX) { snprintf(err, errlen, "a machine keeps at most %d snapshots", VM_SNAPS_MAX); return -1; }
    if (vm_snaps_find(list, n, tag) >= 0) { snprintf(err, errlen, "there is a snapshot %s already", tag); return -1; }
    char disks[VM_NDISKS][PATH_MAX + 64];
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
    if (tpm_snap(dir, m, TPMS_TAKE, tag)) {
        char e2[256];
        for (int k = 0; k < nd; k++) qimg_snap(dir, disks[k], "-d", tag, e2, sizeof e2);
        vm_op_end(dir);
        snprintf(err, errlen, "can't keep the TPM's state");
        return -1;
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
    char disks[VM_NDISKS][PATH_MAX + 64], e2[256];
    int nd = disk_files(dir, m, disks);
    for (int k = 0; k < nd; k++) if (qimg_snap(dir, disks[k], "-d", tag, e2, sizeof e2)) vlog("snapshots: deleting %s on %s: %s", tag, disks[k], e2);
    tpm_snap(dir, m, TPMS_DROP, tag);
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
    char disks[VM_NDISKS][PATH_MAX + 64];
    int nd = disk_files(dir, m, disks);
    if (vm_op_begin(dir, "snapshot-revert", tag, NULL, NULL)) { snprintf(err, errlen, "can't write operation.json"); return -1; }
    for (int i = 0; i < nd; i++) if (qimg_snap(dir, disks[i], "-a", tag, err, errlen)) return -1;     /* operation.json stays: the next open goes on */
    if (tpm_snap(dir, m, TPMS_REVERT, tag)) { snprintf(err, errlen, "can't put the TPM's state back"); return -1; }
    discard_state(dir);
    vm_op_end(dir);
    return 0;
}

static int cmd_create(const char *dir, int argc, char **argv) {
    VmHost h;
    vm_host_probe(&h);
    int arch = h.arch >= 0 ? h.arch : VM_ARCH_AMD64;
    const char *name = NULL, *import = NULL, *iso = NULL, *unatt = NULL;
    int accel = -1, vcpus = 0, mem = 0, disk = 0, fw = -1, disp = -1, net = -1, agent = -1, clip = -1, sb = 0, tpm = 0, hv = 0, rtc = -1, devs = -1, gfx = -1;
    const char *drivers = NULL;
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
        else if (!strcmp(a, "--network")) { net = parse_net(v); if (net < 0 || net == VM_NET_BRIDGED) { fprintf(stderr, "lord-vm: --network shared|isolated|hostonly (bridged: --set with --bridge)\n"); return 2; } }
        else if (!strcmp(a, "--import")) import = v;
        else if (!strcmp(a, "--iso")) iso = v;
        else if (!strcmp(a, "--unattended")) unatt = v;
        else if (!strcmp(a, "--agent")) { agent = parse_enum(v, "off", "on"); if (agent < 0) { fprintf(stderr, "lord-vm: --agent on|off\n"); return 2; } }
        else if (!strcmp(a, "--clipboard")) { clip = parse_clip(v); if (clip < 0) { fprintf(stderr, "lord-vm: --clipboard off|to-guest|from-guest|both\n"); return 2; } }
        else if (!strcmp(a, "--secure-boot")) { sb = parse_enum(v, "off", "on"); if (sb < 0) { fprintf(stderr, "lord-vm: --secure-boot on|off\n"); return 2; } }
        else if (!strcmp(a, "--tpm")) { tpm = parse_enum(v, "off", "on"); if (tpm < 0) { fprintf(stderr, "lord-vm: --tpm on|off\n"); return 2; } }
        else if (!strcmp(a, "--hyperv")) { hv = parse_enum(v, "off", "on"); if (hv < 0) { fprintf(stderr, "lord-vm: --hyperv on|off\n"); return 2; } }
        else if (!strcmp(a, "--rtc")) { rtc = parse_enum(v, "utc", "local"); if (rtc < 0) { fprintf(stderr, "lord-vm: --rtc utc|local\n"); return 2; } }
        else if (!strcmp(a, "--devices")) { devs = !strcmp(v, "windows") ? VM_DEV_WINDOWS : parse_enum(v, "virtio", "compat"); if (devs < 0) { fprintf(stderr, "lord-vm: --devices virtio|compat|windows\n"); return 2; } }
        else if (!strcmp(a, "--drivers")) drivers = v;
        else if (!strcmp(a, "--graphics")) { gfx = parse_enum(v, "2d", "3d"); if (gfx < 0) { fprintf(stderr, "lord-vm: --graphics 2d|3d\n"); return 2; } }
        else { fprintf(stderr, "lord-vm: unknown option %s\n", a); return 2; }
    }
    /* an unattended install (vmunattend.h): the spec lord-server wrote (0600, the password only as a hash), read and
     * removed at once; it needs the ISO it boots from */
    VmUnattend un;
    if (unatt) {
        char sp[65536], why[200];
        int sfd = open(unatt, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        ssize_t sn = sfd >= 0 ? read(sfd, sp, sizeof sp - 1) : -1;
        if (sfd >= 0) close(sfd);
        unlink(unatt);
        if (sn <= 0) { fprintf(stderr, "lord-vm: can't read %s\n", unatt); return 2; }
        sp[sn] = 0;
        int bad = vmu_parse(sp, (size_t)sn, &un, why, sizeof why);
        explicit_bzero(sp, sizeof sp);
        if (bad) { fprintf(stderr, "lord-vm: %s\n", why); return 2; }
        if (!iso) { fprintf(stderr, "lord-vm: --unattended needs --iso\n"); return 2; }
        if (strcmp(un.arch, vm_arch_name(arch))) { fprintf(stderr, "lord-vm: the unattended spec is for %s\n", un.arch); return 2; }
    }
    VmMachine m;
    vm_machine_defaults(&m, &h, arch);
    if (agent >= 0) m.agent = agent;
    if (unatt) m.agent = 1;              /* the installs bring qemu-guest-agent: Shut Down asks it first (6.1) */
    if (clip >= 0) m.clipboard = clip;
    if (name) snprintf(m.name, sizeof m.name, "%s", name);
    if (accel >= 0) m.accel = accel;
    if (vcpus) m.vcpus = vcpus;
    if (mem) m.memory_mb = mem;
    if (disk) m.disk_gb = disk;
    if (disp >= 0) m.display = disp;
    if (net >= 0) m.network = net;
    const char *code = NULL, *vars = NULL;
    /* Windows 11 (4.3): secure boot (UEFI only), a TPM 2.0, Hyper-V enlightenments, the clock in local time */
    m.secure_boot = sb; m.tpm = tpm; m.hyperv = hv;
    if (rtc >= 0) m.rtc = rtc;
    if (devs >= 0) m.devices = devs;
    if (m.devices == VM_DEV_WINDOWS) m.nic_model = VM_NIC_VIRTIO;
    if (gfx >= 0) m.graphics = gfx;
    char drvreal[PATH_MAX];
    if (drivers) {
        if (!realpath(drivers, drvreal) || !file_exists(drvreal) || strlen(drvreal) >= sizeof m.drivers) { fprintf(stderr, "lord-vm: %s isn't a file\n", drivers); return 1; }
        memcpy(m.drivers, drvreal, strlen(drvreal) + 1);
    }
    if (sb && fw == VM_FW_BIOS) { fprintf(stderr, "lord-vm: secure boot needs UEFI\n"); return 2; }
    if (sb) fw = VM_FW_UEFI;
    if (tpm && !file_exists(SWTPM)) { fprintf(stderr, "lord-vm: %s isn't installed (swtpm, for the TPM)\n", SWTPM); return 1; }
    if (fw < 0) fw = firmware_for(arch, &code, &vars) ? VM_FW_BIOS : VM_FW_UEFI;
    m.firmware = fw;
    if (fw == VM_FW_UEFI) {
        if (sb ? (arch != VM_ARCH_AMD64 || firmware_sb(&code, &vars)) : firmware_for(arch, &code, &vars)) { fprintf(stderr, "lord-vm: no UEFI firmware for %s is installed (%s)\n", vm_arch_name(arch), arch == VM_ARCH_ARM64 ? "qemu-efi-aarch64" : "ovmf"); return 1; }
        snprintf(m.firmware_code, sizeof m.firmware_code, "%s", code);
        if (sha256_file(code, m.firmware_sha256)) { fprintf(stderr, "lord-vm: can't read %s\n", code); return 1; }
    }
    if (!file_exists(QEMU_BIN_OF(arch))) { fprintf(stderr, "lord-vm: %s isn't installed\n", QEMU_BIN_OF(arch)); return 1; }
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
    if (unatt) {
        char dd[PATH_MAX + 64], why[300];
        snprintf(dd, sizeof dd, "%s/disks", real);
        if (un.kind == VMU_WINDOWS && !m.drivers[0]) { fprintf(stderr, "lord-vm: a Windows install needs --drivers (virtio-win)\n"); unlink(sys); return 2; }
        /* LordOS (vmunattend.h): its installer erases only the disk the answers name, with exactly this size: the system
         * disk just made (virtio-blk in slot 1, the guest's first: vda; SATA with the compatible devices: sda) */
        if (un.kind == VMU_LORDOS) {
            snprintf(un.disk, sizeof un.disk, "%s", m.devices == VM_DEV_COMPAT ? "sda" : "vda");
            un.disk_bytes = (unsigned long long)m.disk_gb << 30;
        }
        if (un.kind == VMU_WINDOWS ? vmu_prepare_windows(dd, m.drivers, &un, why, sizeof why) : vmu_prepare(dd, m.iso, &un, m.id, why, sizeof why)) {
            fprintf(stderr, "lord-vm: the unattended install can't be prepared: %s\n", why);
            unlink(sys);
            snprintf(dd, sizeof dd, "%s/disks/nvram.qcow2", real); unlink(dd);
            return 1;
        }
        explicit_bzero(&un, sizeof un);
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
       P_SNAPJOB, P_JOBQ, P_SNAPSIZE, P_NET, P_NET_ADD, P_NET_LAST, P_MICE, P_MOUSE_SET };
typedef struct { int kind, client; uint32_t seq; int fd; } Pending;

/* a file dropped on the machine's window, for the guest (VM_FILE) */
#define FX_QUEUE 8
/* kind: a drop (FXK_DROP) or WinFsp's installer for the transfer folder (FXK_WINFSP, VAG_TRANSFER: sha is its hash) */
enum { FXK_DROP = 0, FXK_WINFSP };
/* open: Files' "Open with" a machine (VM_FILE a = 1, 7.3): once copied, the guest tools open it (AGK_OPEN) */
/* place: files dropped on an apps mode window (VM_FILE a = 2, 7.2.1): once copied, the guest tools hand them to it */
typedef struct { int fd, client, kind, open, place; uint32_t seq, pwin, pbatch; int32_t px, py; uint8_t pindex, pcount; char name[AG_NAME_MAX + 1], sha[65]; } FxReq;
/* drops copied and handed to the guest tools to open, waiting for their AGK_OPENED: the token, who asked, until when */
#define OP_MAX 12
typedef struct { char tok[AG_OPEN_TOKEN + 1], name[AG_NAME_MAX + 1], where[VM_TEXT_MAX]; int client, place; uint32_t seq; long long until; } OpWait;

/* each guest-file-write carries FX_CHUNK bytes (base64 in JSON) and FX_DEPTH of them are on their way at once: one at a
 * time, each waiting for its answer, an 8.7 GB file went at about 26 MB/s (2026-11-20) */
#define FX_CHUNK (192 * 1024)
#define FX_DEPTH 3
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
    pid_t vfs[VM_SHARES + 1];               /* shared folders (6.7): each one's virtiofsd, and QEMU's end of its socket; */
    int vfs_q[VM_SHARES + 1];               /* the last is the transfer folder's (6.7.1) */
    /* the transfer folder (6.7.1): on at this start (machine.json "transfer"), its path, a descriptor of it (O_PATH, opened
     * like a share's) for the drops; what the guest said: XG_* */
    int xfer_on, xfer_dfd, xfer_guest;
    char xfer_path[PATH_MAX];
    long xfer_pid;
    long ptr_pid;                           /* the pointer's script (PTR_PS): its guest-exec pid, -1 asked; done this run */
    long long ptr_poll_at;
    int ptr_done, lag_pointer;
    long gvt_pid;                           /* Intel's driver for a GVT-g vGPU (GVTWIN_PS): as ptr_pid */
    long long gvt_poll_at;
    int gvt_done, lag_gvt;
    long long xfer_poll_at;
    /* "Capture mouse" (VM_CAPTURE, docs/VIRTUALIZATION.md 6.25): the mouse wanted (1 relative, 0 the tablet), whether this
     * start has a relative mouse lord-vm added (arm64: one that takes over when its driver starts), the next check of
     * which mouse is active, a query-mice in flight, and what was said about it last (for the log) */
    int cap_want, cap_rel_added, mice_q, mice_said;   /* cap_rel_added: every machine with a window since 0.8.9 */
    long long mice_at;
    pid_t tpm;                              /* the TPM (Windows 11): swtpm, and QEMU's end of its control socket */
    int tpm_q;
    off_t serial_bytes;
    int lock_fd;
    int sandbox, cg_mode;
    int memev_fd;
    long oom0;
    int host_shutdown, poweroff, forced, guest_shutdown, panic, killed;
    int unattended, install_done;
    int win_seed;                        /* Windows' seed disk in this start: 1 answers and drivers, 2 drivers only */
    long long win_key_at;                /* an unattended Windows install's first seconds: Enter for "Press any key to boot
                                          * from CD or DVD" (Microsoft's DVD asks; a reset inside QEMU doesn't come here) */
    int win_keys;        /* the first start of an unattended install (disks/unattended), and its end */
    int win_armed;       /* the Enters above were set for this start (the first time it runs, paused start or not) */
    char un_cmd[640];
    long long shut_deadline, kill_deadline;
    Pending pq[MAX_PENDING];
    int pq_head, pq_n;
    int dev_qmp, paused_start, keep_running, dev_client;
    uint32_t dev_seq;
    /* 3D (virgl, 6.10): machine.json asks for it and this start can give it (decide_graphics); the render node QEMU opens */
    int gl;
    char render[32], render_want[32];
    /* an Intel GVT-g vGPU this run (decide_vgpu, docs/MACHINES-GPU.md Mode A): its render node (the iGPU's, where QEMU
     * makes the picture's dmabufs), /dev/vfio/<group>, the picture's size, and whether the guest asked for a restart
     * (QEMU runs with -action reboot=shutdown: a reset is an exit, and Machines starts it again) */
    int vgpu, vgpu_group, vgpu_w, vgpu_h, vgpu_reset;
    long long ml_wait;                  /* waiting for vm-memlock until then (ms), 0 not waiting */
    char vgpu_render[32], vgpu_dev[96];
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
    long long ag_next, sync_at, ping_at, journal_at, shutdown_at, acpi_at;
    long sync_id;
    char ag_version[48], ag_os[96], jcursor[256];
    int ag_windows;                      /* the guest said it's Windows (guest-get-osinfo "id": "mswindows") */
    long jpid;                              /* a journalctl running in the guest (guest-exec), 0 none */
    int keep_awake, inhibit_done;
    uint8_t *lag_buf;
    size_t lag_n;
    int lag_hello, lag_clipx;               /* lordos-guest said hello; it takes AGK_CLIPX (pictures, rich text) */
    int lag_clipreq;                        /* it takes the host's clipboard on demand (AGK_CLIPOFFER, 6.8) */
    uint32_t clip_gen;                      /* the last offer (VM_CLIP_OFFER): its generation and forms */
    uint8_t clip_forms;
    struct { uint32_t gen; long long at; } clip_wait[4];    /* a request waiting for lord-server's answer, per AGF_* form */
    long long clip_rate_at;                 /* the guest's requests: at most CLIP_REQ_RATE a second */
    int clip_rate_n, clip_rate_said;
    int lag_apps;                           /* apps mode is on in the guest (7.2) */
    LagAppList *lag_applist;                /* the apps the guest listed last: VM_LAUNCH sends back only these tokens */
    int lag_applist_gen;                    /* and its generation (VM_WINDATA c, VM_LAUNCH b) */
    char lag_version[48];
    uint8_t *helper;                        /* lordos-guest for VAG_INSTALL, read before lord-vm's own Landlock */
    size_t helper_n, helper_off;
    long helper_handle;
    int install_client;
    uint32_t install_seq;
    int inst_busy;                          /* VAG_INSTALL on its way, from the request to its answer */
    long inst_pid;                          /* the install's script in the guest (guest-exec), followed until it ends */
    long long inst_poll_at, inst_deadline;
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
    int host_stopping, host_quit, ran;        /* ran: its vCPUs ran, or it holds a saved guest (a paused fresh start doesn't) */
    int susp_cancelled;                       /* the host stop's save was given up (migrate_cancel sent) */
    long long host_t0;                        /* when the stop began (now_ms) */
    VmHostStopPlan hplan;                     /* each step's time from host_t0 (vmhoststop.h) */
    char host_stop_why[96];
    /* a Clone's first start (6.3): the guest gets its own hostname and machine-id through the agent (identity.json) */
    char id_hostname[64];
    int id_state;                           /* 0 none, 1 pending, 2 running in the guest, 3 done */
    char id_winname[16];                    /* a Windows guest's: the computer name it was given */
    long id_pid;
    long long id_poll_at;
    /* files dropped on the window (VM_FILE): the one being copied and the ones waiting (see fx_next) */
    FxReq fx, fxq[FX_QUEUE];
    int fxq_n, fx_step, fx_last;
    long fx_handle, fx_pid;
    long long fx_poll_at, fx_deadline, fx_told_at;
    off_t fx_off, fx_size, fx_sent;     /* written (answered), size, read and sent so far */
    size_t fx_chunks[FX_DEPTH];         /* the pieces on their way, oldest first */
    int fx_inflight, fx_eof;
    char fx_tmp[256];                      /* the private folder in the guest the file is written into */
    /* a drop through the transfer folder (6.7.1): fx_via 1; the host's copy into it (a child of lord-vm: its pid, the pipe
     * it reports on), the file's name there (.lordos-drop-<random>), whether the agent's way was tried after it failed */
    int fx_via, fx_cpipe, fx_retried, fx_stagefd, fx_dig_n;
    pid_t fx_cpid;
    char fx_stage[40], fx_token[33], fx_sha[65];
    uint8_t fx_dig[32];
    long long fx_t0;
    /* LordOS guest tools for Windows (VAG_INSTALL on a Windows guest, tools/guest-win): the program, read at the start,
     * and the install's steps (see wi_begin) */
    uint8_t *wtools;
    size_t wtools_n, wi_off;
    int wi_step, wi_client, wi_auto;
    uint32_t wi_seq;
    long wi_handle, wi_pid;
    long long wi_poll_at, wi_deadline;
    char wi_tmp[256];
    /* the guest's display (AGK_DISPLAY): the size the window wants last, sent to a helper with "display" */
    int lag_display, lag_windows;
    uint32_t disp_w, disp_h;
    char lag_disps[AG_DISPLAYS_MAX * 128];   /* the guest's displays (AGK_DISPLAYS) as last logged, "" none yet */
    int layout[6];                          /* where the picture sits in the guest's desktop (VM_GUEST_LAYOUT), 0s none */
    int lag_extra, lag_extraoff, extra_avail; /* the tools turn off an extra display (AGK_EXTRA); one is off; one is there */
    AgDisplay disp_list[AG_DISPLAYS_MAX];   /* the guest's displays (AGK_DISPLAYS) as last said, for the tablet's layout */
    int disp_n, disp_shown;
    int tablet;                             /* how the guest maps the tablet (AGK_TABLET, AG_TABLET_*; 0 not said yet) */
    int tablet_far;                         /* said in the log: the tablet covers a primary that isn't the shown display */
    /* "Open with" a machine (7.3): the helper said "open"; the drop's token and its hash on the agent's way (the
     * transfer folder's way has fx_dig); the drops waiting for the guest tools' answer */
    int lag_open, lag_place;
    uint32_t lag_win_ids[LAG_WIN_MAX];      /* the guest's last window list (apps mode), for a drop on one of them */
    int lag_win_n;
    /* the icons of the windows in that list, as VM_WINDATA gave them (lag_windata): given again to a client that turns
     * apps mode on (lord-server re-attached after a restart), since the guest sends each window's icon once */
    struct { uint32_t id; uint8_t *data; size_t n; } lag_icons[LAG_WIN_MAX];
    char fx_otok[AG_OPEN_TOKEN + 1];
    Sha256 fx_hs;
    OpWait opw[OP_MAX];
} S;

/* what the guest said of the transfer folder */
enum { XG_UNKNOWN = 0, XG_MOUNTED, XG_NEEDS_WINFSP, XG_NO_SERVICE, XG_FAILED };

/* the start's shared folders: the person's (0 .. n_shares-1) and the transfer folder (VM_SHARES) */
static int sh_on(int i) { return (i >= 0 && i < S.m.n_shares) || (i == VM_SHARES && S.xfer_on); }
static int sh_count(void) { return S.m.n_shares + (S.xfer_on ? 1 : 0); }
static const char *sh_tag(int i) { return i == VM_SHARES ? VM_XFER_TAG : S.m.share_tag[i]; }
static const char *sh_path(int i) { return i == VM_SHARES ? S.xfer_path : S.m.share_path[i]; }
static int sh_rw(int i) { return i == VM_SHARES ? 1 : S.m.share_rw[i]; }

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
    if (st == VS_RUNNING) S.ran = 1;
    if (st == VS_RUNNING && !run_t0) run_t0 = now_ms();
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
    /* Windows' DVD asks "Press any key to boot from CD or DVD" for 5 seconds: Enter a few times from when the machine
     * first runs, unattended or guided. Machines starts it paused until its window is there (--paused), so this waits
     * for that rather than for QEMU's own start (it went to the firmware's menu otherwise, 2026-11-20) */
    if (st == VS_RUNNING && S.win_seed && !S.win_armed) {
        S.win_armed = 1;
        S.win_key_at = now_ms() + 1500;
        S.win_keys = 16;
        vlog("unattended: Enter for Windows' \"Press any key to boot from CD or DVD\" in the next seconds");
    }
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

static int qemu_procs_fd = -1, passt_procs_fd = -1, vfs_procs_fd = -1, tpm_procs_fd = -1;

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
    char q[PATH_MAX + 32], p[PATH_MAX + 32], v[PATH_MAX + 32], val[64];
    snprintf(q, sizeof q, "%s/qemu", self);
    snprintf(p, sizeof p, "%s/passt", self);
    snprintf(v, sizeof v, "%s/virtiofsd", self);
    if ((mkdir(q, 0755) && errno != EEXIST) || (mkdir(p, 0755) && errno != EEXIST)) { vlog("cgroup: can't make leaves: %s", strerror(errno)); return; }
    snprintf(val, sizeof val, "%ld", limit * 1024L * 1024L);
    int lq = cg_write(q, "memory.max", val);
    snprintf(val, sizeof val, "%d 100000", cpu * 1000);
    cg_write(q, "cpu.max", val);
    cg_write(q, "memory.swap.max", "0");
    cg_write(p, "memory.max", "134217728");
    cg_write(p, "cpu.max", "200000 100000");
    if (sh_count() && (!mkdir(v, 0755) || errno == EEXIST)) {
        /* the shared folders' virtiofsd: 256 MB and two CPUs together (they cache file data in the guest, not here) */
        cg_write(v, "memory.max", "268435456");
        cg_write(v, "cpu.max", "200000 100000");
        char vf[PATH_MAX + 64];
        snprintf(vf, sizeof vf, "%s/cgroup.procs", v);
        vfs_procs_fd = open(vf, O_WRONLY | O_CLOEXEC);
    }
    char t[PATH_MAX + 32];
    snprintf(t, sizeof t, "%s/swtpm", self);
    if (S.m.tpm && (!mkdir(t, 0755) || errno == EEXIST)) {
        /* the TPM: a small program with a few kilobytes of state */
        cg_write(t, "memory.max", "67108864");
        cg_write(t, "cpu.max", "50000 100000");
        cg_write(t, "pids.max", "4");
        char tf[PATH_MAX + 64];
        snprintf(tf, sizeof tf, "%s/cgroup.procs", t);
        tpm_procs_fd = open(tf, O_WRONLY | O_CLOEXEC);
    }
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

/* passt's command line for the machine's network (6.5), into argv (NULL-terminated); the values live in static storage.
 *  - Shared: out only, as any program of this computer would go out. The host's loopback isn't the gateway
 *    (--no-map-gw). DNS: queries to one fixed address go to the host's first nameserver, loopback resolvers included
 *    (with --dns as well, passt advertised the address but didn't answer on it: measured 2026-11-05).
 *  - Host-only: every socket passt opens for the guest is bound to 127.0.0.1 (-o), so the kernel refuses any address
 *    that isn't this computer's (EINVAL: a loopback source can't leave the computer); IPv4 only (-4), so no IPv6
 *    socket goes out unbound; the guest gets fixed addresses (VM_HOSTONLY_ADDR, gateway VM_HOSTONLY_GW, which is this
 *    computer's loopback) and no nameserver. No root, no device, no rule of the system's firewall.
 *  - Both: the forwarded ports (-t, -u with the address to listen on: 127.0.0.1, or 0.0.0.0 for "lan" in Shared).
 *    passt binds them in the machine's scope, so LordOS's firewall decides them as the incoming connections of
 *    Machines (FIREWALL.md, "Machines"). */
static int vm_passt_argv(const VmMachine *m, char **argv, int max) {
    static char tv[VM_PORTS][48], uv[VM_PORTS][48];
    int n = 0, nt = vm_passt_ports(m, VM_PROTO_TCP, tv, VM_PORTS), nu = vm_passt_ports(m, VM_PROTO_UDP, uv, VM_PORTS);
    if (max < 20 + 2 * (nt + nu)) return -1;
    argv[n++] = PASST; argv[n++] = "-f"; argv[n++] = "-q"; argv[n++] = "-1"; argv[n++] = "--fd"; argv[n++] = "3";
    if (m->network == VM_NET_HOSTONLY) {
        argv[n++] = "-4"; argv[n++] = "-o"; argv[n++] = "127.0.0.1";
        argv[n++] = "-a"; argv[n++] = VM_HOSTONLY_ADDR; argv[n++] = "-n"; argv[n++] = "24"; argv[n++] = "-g"; argv[n++] = VM_HOSTONLY_GW;
        argv[n++] = "--no-dhcp-dns"; argv[n++] = "--no-dhcp-search";
    } else {
        argv[n++] = "--no-map-gw"; argv[n++] = "--dns-forward"; argv[n++] = VM_DNS_ADDR;
    }
    for (int i = 0; i < nt; i++) { argv[n++] = "-t"; argv[n++] = tv[i]; }
    for (int i = 0; i < nu; i++) { argv[n++] = "-u"; argv[n++] = uv[i]; }
    argv[n] = NULL;
    return n;
}

/* the forwarded ports are free before passt starts (6.5): passt would only say "Failed to bind" and end, and the
 * network's healing would try again. Bound and let go here, in the machine's scope, so the firewall's answer is the one
 * passt gets: EADDRINUSE another program has the port, EPERM/EACCES the firewall (no incoming connections for Machines) */
static int ports_free(const VmMachine *m, char *err, size_t errlen) {
    for (int i = 0; i < m->n_ports; i++) {
        const VmPort *p = &m->ports[i];
        int fd = socket(AF_INET, (p->proto == VM_PROTO_UDP ? SOCK_DGRAM : SOCK_STREAM) | SOCK_CLOEXEC, 0);
        if (fd < 0) { snprintf(err, errlen, "can't check this computer's port %d: %s", p->host, strerror(errno)); return -1; }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)p->host) };
        a.sin_addr.s_addr = htonl(p->open == VM_OPEN_LAN && m->network != VM_NET_HOSTONLY ? INADDR_ANY : INADDR_LOOPBACK);
        int r = bind(fd, (struct sockaddr *)&a, sizeof a), e = errno;
        close(fd);
        if (!r) continue;
        if (e == EADDRINUSE) snprintf(err, errlen, "port %d (%s) of this computer is in use by another program: forward another port", p->host, p->proto == VM_PROTO_UDP ? "udp" : "tcp");
        else if (e == EPERM || e == EACCES) snprintf(err, errlen, "the firewall doesn't let Machines receive connections (port %d): allow incoming connections for Machines in Settings, Firewall", p->host);
        else snprintf(err, errlen, "can't open port %d of this computer: %s", p->host, strerror(e));
        return -1;
    }
    return 0;
}

/* a bridged machine's macvtap (6.5), made by the admin helper's vm-net-bridge before the start: an interface named
 * after the machine (vm_bridge_ifname) on machine.json's wired card with the machine's MAC, whose /dev/tap<index> the
 * helper gave to this user. Checked here by what it is (a macvtap of that card with that address) and opened; QEMU gets
 * the descriptor. -1 and why in err */
static int open_bridge_tap(const VmMachine *m, char *err, size_t errlen) {
    char name[VM_IFNAME_MAX + 1], p[160], buf[64];
    if (vm_bridge_ifname(m->id, name)) { snprintf(err, errlen, "the machine's id is wrong"); return -1; }
    snprintf(p, sizeof p, "/sys/class/net/%s/ifindex", name);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    ssize_t r = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    if (r <= 0) { snprintf(err, errlen, "the bridged network isn't ready (%s isn't there): start the machine from Machines, which asks for it", name); return -1; }
    buf[r] = 0;
    int idx = atoi(buf);
    /* its lower card: /sys/class/net/<name>/lower_<card> */
    snprintf(p, sizeof p, "/sys/class/net/%s/lower_%s", name, m->bridge);
    struct stat st;
    if (lstat(p, &st)) { snprintf(err, errlen, "%s isn't on %s: start the machine from Machines again", name, m->bridge); return -1; }
    snprintf(p, sizeof p, "/sys/class/net/%s/address", name);
    fd = open(p, O_RDONLY | O_CLOEXEC);
    r = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    if (r < 17 || strncmp(buf, m->mac, 17)) { snprintf(err, errlen, "%s doesn't have the machine's address: start the machine from Machines again", name); return -1; }
    snprintf(p, sizeof p, "/sys/class/net/%s/macvtap/tap%d/dev", name, idx);
    fd = open(p, O_RDONLY | O_CLOEXEC);
    r = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    unsigned ma = 0, mi = 0;
    if (r <= 0 || (buf[r] = 0, sscanf(buf, "%u:%u", &ma, &mi) != 2)) { snprintf(err, errlen, "%s isn't a macvtap", name); return -1; }
    snprintf(p, sizeof p, "/dev/tap%d", idx);
    int t = open(p, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY);
    if (t < 0) { snprintf(err, errlen, "can't open %s: %s", p, strerror(errno)); return -1; }
    if (fstat(t, &st) || !S_ISCHR(st.st_mode) || major(st.st_rdev) != ma || minor(st.st_rdev) != mi || st.st_uid != getuid()) {
        close(t); snprintf(err, errlen, "%s isn't this machine's device", p); return -1;
    }
    vlog("network: bridged on %s through %s (%s)", m->bridge, name, p);
    return t;
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
    char *argv[16 + 4 * VM_PORTS];
    int n = vm_passt_argv(&S.m, argv, (int)(sizeof argv / sizeof argv[0]));
    if (n < 0) _exit(127);
    char *env[] = { NULL };
    execve(PASST, argv, env);
    _exit(127);
}

/* A shared folder (6.7): its virtiofsd, started on a listening socket in the machine's run folder, and lord-vm connects
 * to it at once; the connection goes to QEMU (which can't make sockets) and the path is removed. virtiofsd's own
 * sandbox puts it in new namespaces with only the folder mounted (pivot_root), under its own seccomp filter: neither ours
 * nor Landlock, which forbid the namespaces and mounts that sandbox is made of. It sees QEMU's guest memory (vhost-user),
 * only this machine's. Read-only unless the share says writable. The folder was checked by vm_share_allowed. Returns
 * QEMU's end of the connection, or -1 with why in err. */
static void fail_start(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* the shared folder, opened once and without following a link at any step (O_PATH), then checked by what it is: its
 * path read back from the descriptor must be the one in machine.json. virtiofsd gets this descriptor
 * (--shared-dir=/proc/self/fd/4), so a folder of the path swapped for a link after the check (by a guest that can write
 * a parent of it through another share, for one) changes nothing. -1 with why in err */
static int open_share(const char *path, char *err, size_t errlen) {
    int fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    char part[NAME_MAX + 1];
    for (const char *p = path; fd >= 0 && *p; ) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t n = strcspn(p, "/");
        if (n > NAME_MAX) { close(fd); snprintf(err, errlen, "a name in the path is too long"); return -1; }
        memcpy(part, p, n);
        part[n] = 0;
        p += n;
        int next = openat(fd, part, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        fd = next;
    }
    if (fd < 0) { snprintf(err, errlen, "the folder can't be opened without following a link: %s", strerror(errno)); return -1; }
    char self[64], real[PATH_MAX];
    snprintf(self, sizeof self, "/proc/self/fd/%d", fd);
    ssize_t r = readlink(self, real, sizeof real - 1);
    if (r <= 0 || (real[r] = 0, strcmp(real, path))) { close(fd); snprintf(err, errlen, "the folder changed while it was being opened"); return -1; }
    return fd;
}

static int spawn_virtiofsd(int i, char *err, size_t errlen) {
    int dfd = open_share(sh_path(i), err, errlen);
    if (dfd < 0) return -1;
    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/fs%d.sock", S.rundir, i);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof a.sun_path) { snprintf(err, errlen, "the run folder's path is too long"); close(dfd); return -1; }
    memcpy(a.sun_path, path, strlen(path) + 1);
    unlink(path);
    int l = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 1)) { snprintf(err, errlen, "socket: %s", strerror(errno)); if (l >= 0) close(l); close(dfd); unlink(path); return -1; }
    char logp[PATH_MAX + 64];
    snprintf(logp, sizeof logp, "%s/log/virtiofsd-%d.log", S.dir, i + 1);
    int lf = open(logp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    pid_t pid = fork();
    if (pid < 0) { snprintf(err, errlen, "fork: %s", strerror(errno)); close(l); close(dfd); if (lf >= 0) close(lf); unlink(path); return -1; }
    if (!pid) {
        child_common(vfs_procs_fd);
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(lf >= 0 ? lf : null, 1);
        dup2(lf >= 0 ? lf : null, 2);
        /* 3 the listening socket, 4 the folder as it was checked (open_share) */
        int fds[2] = { l, dfd };
        place_fds(fds, 2);
        char dir[] = "--shared-dir=/proc/self/fd/4";
        /* find-paths: a suspended machine's open files are found again by their paths when it resumes (6.1) */
        char *argv[] = { VIRTIOFSD, "--fd=3", dir, "--sandbox=namespace", "--seccomp=kill", "--cache=auto", "--migration-mode=find-paths",
                         "--migration-on-error=guest-error", "--log-level=warn", sh_rw(i) ? NULL : "--readonly", NULL };
        char *env[] = { NULL };
        execve(VIRTIOFSD, argv, env);
        _exit(127);
    }
    close(l);
    close(dfd);
    if (lf >= 0) close(lf);
    int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int ok = c >= 0 && !connect(c, (struct sockaddr *)&a, sizeof a);
    unlink(path);
    if (!ok) { snprintf(err, errlen, "can't reach virtiofsd: %s", strerror(errno)); if (c >= 0) close(c); kill(pid, SIGKILL); waitpid(pid, NULL, 0); return -1; }
    S.vfs[i] = pid;
    vlog("share %d: virtiofsd pid %d, tag %s, %s, %s", i + 1, (int)pid, sh_tag(i), sh_rw(i) ? "writable" : "read-only", sh_path(i));
    return c;
}

/* the TPM (4.3; AGENTS.md, "Machines' TPM"): Debian's swtpm, one per machine, started by lord-vm as the session user.
 * Its state is <machine>/disks/tpm (the only folder it may write); its control channel is one end of a socketpair (fd 3),
 * QEMU gets the other (QEMU then hands it the data channel over that). Around it: our no_new_privs, seccomp (no sockets
 * of its own) and Landlock (read /usr, write its folder, no TCP), its own --seccomp, a cgroup leaf, PR_SET_PDEATHSIG, no
 * environment; --terminate ends it when QEMU closes the channel. */
static int spawn_swtpm(char *err, size_t errlen) {
    if (!file_exists(SWTPM)) { snprintf(err, errlen, "swtpm isn't installed (the TPM)"); return -1; }
    char dir[PATH_MAX + 32], st[PATH_MAX + 64];
    snprintf(dir, sizeof dir, "%s/tpm", S.disks);
    if (mkdir(dir, 0700) && errno != EEXIST) { snprintf(err, errlen, "can't make the TPM's folder: %s", strerror(errno)); return -1; }
    struct stat sb;
    if (lstat(dir, &sb) || !S_ISDIR(sb.st_mode) || sb.st_uid != getuid()) { snprintf(err, errlen, "the TPM's folder isn't this account's"); return -1; }
    snprintf(st, sizeof st, "dir=%s,mode=0600", dir);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) { snprintf(err, errlen, "socketpair: %s", strerror(errno)); return -1; }
    char logp[PATH_MAX + 64];
    snprintf(logp, sizeof logp, "%s/log/swtpm.log", S.dir);
    int lf = open(logp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    pid_t pid = fork();
    if (pid < 0) { snprintf(err, errlen, "fork: %s", strerror(errno)); close(sv[0]); close(sv[1]); if (lf >= 0) close(lf); return -1; }
    if (!pid) {
        child_common(tpm_procs_fd);
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(lf >= 0 ? lf : null, 1);
        dup2(lf >= 0 ? lf : null, 2);
        int fds[1] = { sv[1] };
        place_fds(fds, 1);
        const char *reads[] = { "/etc/ld.so.cache" };
        int mask = vm_jail_tool_n(reads, 1, dir);
        if (!(mask & VMS_NO_NEW_PRIVS) || !(mask & VMS_SECCOMP_OURS) || (vm_landlock_abi() > 0 && !(mask & VMS_LANDLOCK_FS))) _exit(126);
        char *argv[] = { SWTPM, "socket", "--tpm2", "--tpmstate", st, "--ctrl", "type=unixio,clientfd=3", "--terminate",
                         "--seccomp", "action=kill", "--log", "level=1", NULL };
        char *env[] = { NULL };
        execve(SWTPM, argv, env);
        _exit(127);
    }
    close(sv[1]);
    if (lf >= 0) close(lf);
    S.tpm = pid;
    vlog("tpm: swtpm pid %d, state in disks/tpm", (int)pid);
    return sv[0];
}

/* the transfer folder (6.7.1): <library>/Transfer/<name>, made when it isn't there (Transfer 0700; the machine's folder
 * 0777 inside it, so the guest's person can write whatever their uid: virtiofsd can't take the guest's uids), then checked:
 * real folders owned by this account, no link on the way (open_share), and never the library, a machine's folder or a
 * folder holding them other than as <library>/Transfer/<name>. lord-vm keeps a descriptor of it for the drops. */
static int xfer_prepare(const char *lib, char *err, size_t errlen) {
    char want[PATH_MAX], moved[NAME_MAX + 1];
    const char *base = strrchr(S.dir, '/');
    char real[PATH_MAX + 256];
    snprintf(real, sizeof real, "%s%.255s", lib, base ? base : "/");
    /* made when it isn't there, and this machine's: a folder of another machine of the same name moves aside */
    if (vm_transfer_claim(real, S.m.id, want, sizeof want, moved, sizeof moved)) { snprintf(err, errlen, "can't make or claim it in %.120s/" VM_XFER_DIR ": %s", lib, strerror(errno)); return -1; }
    if (moved[0]) vlog("transfer: the folder of that name was another machine's: it's now %s", moved);
    int fd = open_share(want, err, errlen);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) { close(fd); snprintf(err, errlen, "%.120s isn't a folder of this account", want); return -1; }
    snprintf(S.xfer_path, sizeof S.xfer_path, "%s", want);
    if (S.xfer_dfd >= 0) close(S.xfer_dfd);
    S.xfer_dfd = fd;
    return 0;
}

/* every share's folder, checked again now (it may have become a link or moved since it was chosen), then its virtiofsd */
static int start_shares(void) {
    if (!file_exists(VIRTIOFSD)) { fail_start("virtiofsd isn't installed (shared folders)"); return -1; }
    const struct passwd *pw = getpwuid(getuid());
    char home[PATH_MAX], lib[PATH_MAX], parent[PATH_MAX], err[200];
    if (!pw || !realpath(pw->pw_dir, home)) { fail_start("shared folders: can't find the home folder"); return -1; }
    snprintf(parent, sizeof parent, "%s", S.dir);
    char *sl = strrchr(parent, '/');
    if (sl && sl != parent) *sl = 0;
    if (!realpath(parent, lib)) snprintf(lib, sizeof lib, "%s", parent);
    for (int i = 0; i <= VM_SHARES; i++) {
        if (!sh_on(i)) continue;
        if (i == VM_SHARES) {
            /* the transfer folder: made here when it isn't there, checked as the machine's own (xfer_prepare) */
            if (xfer_prepare(lib, err, sizeof err)) { fail_start("the transfer folder: %s", err); return -1; }
        } else if (vm_share_allowed(S.m.share_path[i], home, pw->pw_name, lib, err, sizeof err)) { fail_start("shared folder %s: %s", S.m.share_tag[i], err); return -1; }
        S.vfs_q[i] = spawn_virtiofsd(i, err, sizeof err);
        if (S.vfs_q[i] < 0) { fail_start("shared folder %s: %s", sh_tag(i), err); return -1; }
    }
    S.sandbox |= VMS_PDEATHSIG;
    return 0;
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

#define MAXARGS 192
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

/* ---- 3D graphics (6.10) ---- */

/* QEMU's GL display and virtio-gpu GL modules (qemu-system-modules-opengl, which also brings virglrenderer) */
static int gl_modules(void) {
    static const char *const MOD[] = { "ui-opengl.so", "hw-display-virtio-vga-gl.so", "hw-display-virtio-gpu-gl.so", NULL };
    char p[160];
    for (int i = 0; MOD[i]; i++) {
        snprintf(p, sizeof p, "/usr/lib/x86_64-linux-gnu/qemu/%s", MOD[i]);
        if (!file_exists(p)) return 0;
    }
    return 1;
}

/* a render node's path as lord-vm takes it: /dev/dri/renderD<digits>, a character device of DRM (major 226) */
static int render_node_ok(const char *p) {
    if (strncmp(p, "/dev/dri/renderD", 16) || !p[16] || strlen(p) > 24) return 0;
    for (const char *q = p + 16; *q; q++) if (*q < '0' || *q > '9') return 0;
    struct stat st;
    return !stat(p, &st) && S_ISCHR(st.st_mode) && major(st.st_rdev) == 226 && !access(p, R_OK | W_OK);
}

/* the one asked for (lord-server names the one its own GPU path uses), else the first this user can open */
static int pick_render_node(char *out, size_t n) {
    if (S.render_want[0]) {
        if (!render_node_ok(S.render_want)) return -1;
        snprintf(out, n, "%s", S.render_want);
        return 0;
    }
    for (int i = 128; i < 192; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/dri/renderD%d", i);
        if (render_node_ok(p)) { snprintf(out, n, "%s", p); return 0; }
    }
    return -1;
}

/* Graphics 3D only for a Linux machine in a window, amd64 on amd64 (QEMU 10.0's 3D aborts on arm64's firmware reset,
 * 3.2), not on an unattended install's start (vm_gl_refusal), with QEMU's GL modules and a render node; otherwise the
 * machine starts with 2D graphics and the log says why. Only then does QEMU's sandbox get the render node (vm_jail_qemu,
 * VmQemuJail.render). */
static void decide_graphics(void) {
    S.gl = 0;
    S.render[0] = 0;
    if (S.m.graphics != VM_GFX_3D) return;
    VmHost h;
    vm_host_probe(&h);
    const char *why = vm_gl_refusal(&S.m, h.arch, S.unattended);
    if (why) S.render[0] = 0;
    else if (!strcmp(S.render_want, "none")) why = "the desktop draws without a GPU";
    else if (!gl_modules()) why = "QEMU's OpenGL modules (qemu-system-modules-opengl) aren't installed";
    else if (S.render_want[0] && strcmp(S.render_want, "none") && !render_node_ok(S.render_want)) why = "the desktop's render node can't be opened";
    else if (pick_render_node(S.render, sizeof S.render)) why = "no GPU render node can be opened";
    if (why) { S.render[0] = 0; vlog("graphics: 2D instead of 3D: %s", why); return; }
    S.gl = 1;
    vlog("graphics: 3D (virgl) on %s", S.render);
}

/* ---- an Intel GVT-g vGPU (docs/MACHINES-GPU.md, Mode A) ---- */

#ifdef LORDVM_TEST_PATHS
#define GVT_ROM test_path("LORDVM_TEST_GVT_ROM", VMGPU_ROM)
#else
#define GVT_ROM VMGPU_ROM
#endif

/* the locked memory QEMU needs with a vGPU: VFIO pins the guest pages the vGPU uses (about 50 MB idle, 200 MB under 3D,
 * measured 2026-11-21), counted against QEMU's RLIMIT_MEMLOCK. Only root raises it (the hard limit is 8 MB): lord-server
 * asks the admin helper's vm-memlock for this QEMU, which sets the machine's memory + 512 MB within the cap "Memory
 * machines may lock"; until then the vCPUs don't run (vgpu_memlock_tick) */
static rlim_t vgpu_memlock(void) { return ((rlim_t)S.m.memory_mb + VMGPU_MEMLOCK_EXTRA_MB) << 20; }
static int vgpu_memlock_ok(void) {
    struct rlimit rl;
    return S.qemu > 0 && !prlimit(S.qemu, RLIMIT_MEMLOCK, NULL, &rl) && (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur >= vgpu_memlock());
}

/* machine.json asks for a vGPU: the mdev Machines asked the admin helper for (vm-gvt-create, named after the machine's
 * id) must be there with its group given to this user, our GOP option ROM in the package, QEMU's GL display and the
 * iGPU's render node. Then the vGPU is the machine's only display. -1 (and the start fails, saying why) otherwise */
static int decide_vgpu(void) {
    S.vgpu = S.vgpu_reset = 0;
    S.vgpu_render[0] = S.vgpu_dev[0] = 0;
    if (!S.m.vgpu) return 0;
    if (S.m.display != VM_DISPLAY_WINDOW) { vlog("graphics card: the machine has no display: it starts without its vGPU"); return 0; }
    char dev[96], p[200], t[256];
    snprintf(dev, sizeof dev, "/sys/bus/mdev/devices/%s", S.m.id);
    snprintf(p, sizeof p, "%s/iommu_group", dev);
    ssize_t l = readlink(p, t, sizeof t - 1);
    const char *g = l > 0 ? (t[l] = 0, strrchr(t, '/')) : NULL;
    if (!g || !g[1] || strspn(g + 1, "0123456789") != strlen(g + 1) || strlen(g + 1) > 6) {
        fail_start("the machine's Intel graphics card isn't ready: start it from Machines, which asks for it");
        return -1;
    }
    int grp = atoi(g + 1);
    /* the iGPU it belongs to: the mdev's parent PCI device, its render node */
    char real[PATH_MAX];
    if (!realpath(dev, real) || strncmp(real, "/sys/devices/", 13)) { fail_start("the machine's Intel graphics card isn't a device"); return -1; }
    char *slash = strrchr(real, '/');
    if (!slash) { fail_start("the machine's Intel graphics card isn't a device"); return -1; }
    *slash = 0;
    const char *parent = strrchr(real, '/');
    if (!parent || !vmgpu_addr_valid(parent + 1)) { fail_start("the machine's Intel graphics card isn't on a PCI device"); return -1; }
    int pk = snprintf(p, sizeof p, "%s/drm", real);
    DIR *d = pk > 0 && (size_t)pk < sizeof p ? opendir(p) : NULL;
    for (struct dirent *e; d && (e = readdir(d));)
        if (!strncmp(e->d_name, "renderD", 7) && strlen(e->d_name) < 12) snprintf(S.vgpu_render, sizeof S.vgpu_render, "/dev/dri/%s", e->d_name);
    if (d) closedir(d);
    if (!S.vgpu_render[0] || !render_node_ok(S.vgpu_render)) { fail_start("the Intel graphics card's render node can't be opened"); return -1; }
    /* /dev/vfio/<group>: given to this user by the admin helper */
    snprintf(p, sizeof p, "/dev/vfio/%d", grp);
    struct stat st;
    if (stat(p, &st) || !S_ISCHR(st.st_mode) || st.st_uid != getuid() || access(p, R_OK | W_OK)) {
        fail_start("the machine's Intel graphics card isn't this user's: start it from Machines, which asks for it");
        return -1;
    }
    if (!file_exists(GVT_ROM)) { fail_start("LordOS's GVT-g option ROM is missing (%s)", GVT_ROM); return -1; }
    if (!gl_modules()) { fail_start("QEMU's OpenGL modules (qemu-system-modules-opengl) aren't installed: Machines installs them"); return -1; }
    /* the picture: the machine's resolution, or 1920 x 1080, no larger than its type shows */
    int w = S.m.res_w ? S.m.res_w : 1920, h = S.m.res_h ? S.m.res_h : 1080;
    VmGpuType ty = { 0 };
    snprintf(p, sizeof p, "%s/mdev_type/description", dev);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    ssize_t r = fd >= 0 ? read(fd, t, sizeof t - 1) : -1;
    if (fd >= 0) close(fd);
    if (r > 0 && (t[r] = 0, !vmgpu_type_desc(t, &ty)) && (w > ty.max_w || h > ty.max_h)) {
        vlog("graphics card: %dx%d is more than its type shows: %dx%d", w, h, ty.max_w, ty.max_h);
        w = ty.max_w; h = ty.max_h;
    }
    S.vgpu = 1; S.vgpu_group = grp; S.vgpu_w = w; S.vgpu_h = h;
    snprintf(S.vgpu_dev, sizeof S.vgpu_dev, "%s", dev);
    vlog("graphics card: Intel vGPU %s of %s (group %d), %dx%d, on %s; it locks %llu MB while it runs", S.m.id, parent + 1, grp, w, h, S.vgpu_render,
         (unsigned long long)(vgpu_memlock() >> 20));
    return 0;
}

/* fds inside QEMU: 3 /dev/kvm (or /dev/null), 4 QMP, 5 serial, 6 network (or /dev/null) */
static void build_qemu_args(void) {
    const VmMachine *m = &S.m;
    int kvm = m->accel == VM_ACCEL_KVM, arm = m->arch == VM_ARCH_ARM64, uefi = m->firmware == VM_FW_UEFI;
    char esc[2 * VM_NAME_MAX + 8], p[PATH_MAX + 64];
    qargc = 0;
    arg(QEMU_BIN_OF(m->arch));
    arg("-nodefaults"); arg("-no-user-config");
    opt_escape(esc, sizeof esc, m->name);
    arg("-name"); argf("guest=%s,debug-threads=on", esc);
    arg("-uuid"); arg(m->id);
    /* shared folders (6.7): vhost-user needs the guest's memory in a shared memfd; machines without one keep private memory */
    arg("-machine"); argf("%s,dump-guest-core=off%s%s%s", m->machine_type, uefi ? ",pflash0=fw-code,pflash1=fw-vars" : "", sh_count() ? ",memory-backend=mem" : "",
                          m->secure_boot ? ",smm=on" : "");
    /* secure boot: the firmware's variables can be written only from SMM, so the guest's system can't change its own keys */
    if (m->secure_boot) { arg("-global"); arg("driver=cfi.pflash01,property=secure,value=on"); }
    if (sh_count()) { arg("-object"); argf("memory-backend-memfd,id=mem,size=%dM,share=on", m->memory_mb); }
    if (kvm) { arg("-accel"); arg("kvm,device=/dev/fdset/1"); arg("-add-fd"); arg("fd=3,set=1,opaque=kvm"); }
    else { arg("-accel"); arg("tcg,thread=multi,tb-size=256"); }
    /* Hyper-V enlightenments: what Windows asks a hypervisor for (timers, interrupts, spinlocks); with KVM only */
    arg("-cpu"); arg(!kvm ? "max" : m->hyperv ? "host,hv_relaxed,hv_vapic,hv_spinlocks=0x1fff,hv_vpindex,hv_runtime,hv_synic,hv_stimer,hv_time,"
                                                 "hv_frequencies,hv_reset,hv_tlbflush,hv_ipi" : "host");
    if (m->cpu_sockets) { arg("-smp"); argf("%d,sockets=%d,cores=%d,threads=%d", m->vcpus, m->cpu_sockets, m->cpu_cores, m->cpu_threads); }
    else { arg("-smp"); argf("%d", m->vcpus); }
    arg("-m"); argf("%d", m->memory_mb);
    {
        /* what an imported machine's guest read in its SMBIOS system table (6.22); the UUID is -uuid */
        const char *k[] = { "manufacturer", "product", "version", "serial", "sku", "family" };
        const char *v[] = { m->sm_manufacturer, m->sm_product, m->sm_version, m->sm_serial, m->sm_sku, m->sm_family };
        char t1[1024] = "type=1", e[2 * VM_SMBIOS_MAX + 4];
        int any = 0;
        for (int i = 0; i < 6; i++) if (v[i][0]) {
            opt_escape(e, sizeof e, v[i]);
            snprintf(t1 + strlen(t1), sizeof t1 - strlen(t1), ",%s=%s", k[i], e);
            any = 1;
        }
        if (any) { arg("-smbios"); arg(t1); }
    }
    if (m->rtc == VM_RTC_LOCAL) { arg("-rtc"); arg("base=localtime"); }
    /* the firmware's boot menu: SeaBIOS and OVMF wait 5 s at each start for Esc (fw_cfg's etc/boot-menu-wait) */
    if (m->boot_menu) { arg("-boot"); arg("menu=on,splash-time=5000"); }
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
    for (int i = 0; i < m->n_data; i++) {
        char fn[24], nn[16];
        snprintf(p, sizeof p, "%s/data-%d.qcow2", S.disks, i + 1);
        snprintf(fn, sizeof fn, "data%d-file", i + 1);
        snprintf(nn, sizeof nn, "data%d", i + 1);
        arg("-blockdev"); argj(blockdev_file(fn, p, 0));
        arg("-blockdev"); argj(blockdev_fmt("qcow2", nn, fn, 0));
    }
    /* Every PCI device has a fixed slot: with the slots QEMU picks, taking an ISO out after an install moved the
     * network card (enp0s3 became enp0s2) and the installed system's network didn't come up. Slots 1 to 6 are the
     * ones a machine without an ISO always had; the ISO's controller goes after them; data disks at 0x10, extra
     * network cards at 0x18 (q35 keeps 0x1f for its own controllers). Serials are letters, digits and "-_." only. */
    char ser[VM_SERIAL_MAX + 16];
    snprintf(ser, sizeof ser, "%s%s", m->disk_serial[0] ? ",serial=" : "", m->disk_serial);
    /* where the firmware starts from (machine.json's boot_order, vm_boot_indexes): each device's ",bootindex=N", or
     * nothing for one not listed. An unattended install's first start: QEMU gives the installer's kernel (-kernel)
     * boot index 0, the rest follow it */
    int bidx[VM_BOOT_DEVS];
    vm_boot_indexes(m, S.unattended ? 1 : 0, bidx);
    char bi_sys[24] = "", bi_cd[24] = "", bi_net[24] = "";
    if (bidx[VM_BOOT_DISK] >= 0) snprintf(bi_sys, sizeof bi_sys, ",bootindex=%d", bidx[VM_BOOT_DISK]);
    if (bidx[VM_BOOT_CD] >= 0) snprintf(bi_cd, sizeof bi_cd, ",bootindex=%d", bidx[VM_BOOT_CD]);
    if (bidx[VM_BOOT_NET] >= 0) snprintf(bi_net, sizeof bi_net, ",bootindex=%d", bidx[VM_BOOT_NET]);
    if (m->devices == VM_DEV_COMPAT) {
        /* devices every system has a driver for (6.22): SATA (AHCI) disks and CD/DVD; q35 has its AHCI built in (ide.0
         * to ide.5), arm64's virt gets one in the system disk's slot */
        const char *bus = arm ? "ahci" : "ide";
        if (arm) { arg("-device"); arg("ich9-ahci,id=ahci,addr=0x1"); }
        arg("-device"); argf("ide-hd,id=sysdisk,drive=sys,bus=%s.0%s%s", bus, bi_sys, ser);
        for (int i = 0; i < m->n_data; i++) {
            arg("-device"); argf("ide-hd,id=datadisk%d,drive=data%d,bus=%s.%d%s%s", i + 1, i + 1, bus, i + 1, m->data_serial[i][0] ? ",serial=" : "", m->data_serial[i]);
        }
        if (m->iso[0]) {
            arg("-blockdev"); argj(blockdev_file("cd-file", m->iso, 1));
            arg("-blockdev"); argj(blockdev_fmt("raw", "cd", "cd-file", 1));
            arg("-device"); argf("ide-cd,id=cd0,bus=%s.5,drive=cd%s", bus, bi_cd);
            snprintf(S.cd_node, sizeof S.cd_node, "cd");
        } else { arg("-device"); argf("ide-cd,id=cd0,bus=%s.5%s", bus, bi_cd); S.cd_node[0] = 0; }
    } else if (m->devices == VM_DEV_WINDOWS) {
        /* Windows (4.3): virtio disks (Setup loads viostor from the drivers), the CD/DVD drives on q35's own SATA (ide.4
         * the drivers' disc, ide.5 the installer's), which Setup reads with the drivers it has */
        arg("-device"); argf("virtio-blk-pci,id=sysdisk,drive=sys%s,addr=0x1%s", bi_sys, ser);
        for (int i = 0; i < m->n_data; i++) {
            arg("-device"); argf("virtio-blk-pci,id=datadisk%d,drive=data%d,addr=0x%x%s%s", i + 1, i + 1, 0x10 + i, m->data_serial[i][0] ? ",serial=" : "", m->data_serial[i]);
        }
        if (m->iso[0]) {
            arg("-blockdev"); argj(blockdev_file("cd-file", m->iso, 1));
            arg("-blockdev"); argj(blockdev_fmt("raw", "cd", "cd-file", 1));
            arg("-device"); argf("ide-cd,id=cd0,bus=ide.5,drive=cd%s", bi_cd);
            snprintf(S.cd_node, sizeof S.cd_node, "cd");
        } else { arg("-device"); argf("ide-cd,id=cd0,bus=ide.5%s", bi_cd); S.cd_node[0] = 0; }
        /* xHCI for the USB tablet and the seed disk, before either */
        arg("-device"); arg("qemu-xhci,id=xhci,addr=0x5");
        if (m->drivers[0]) {
            arg("-blockdev"); argj(blockdev_file("drv-file", m->drivers, 1));
            arg("-blockdev"); argj(blockdev_fmt("raw", "drv", "drv-file", 1));
            arg("-device"); arg("ide-cd,id=cd1,bus=ide.4,drive=drv");
        }
    } else {
        arg("-device"); argf("virtio-blk-pci,id=sysdisk,drive=sys%s,addr=0x1%s", bi_sys, ser);
        for (int i = 0; i < m->n_data; i++) {
            arg("-device"); argf("virtio-blk-pci,id=datadisk%d,drive=data%d,addr=0x%x%s%s", i + 1, i + 1, 0x10 + i, m->data_serial[i][0] ? ",serial=" : "", m->data_serial[i]);
        }
        /* the CD/DVD drive is always there (slot 8, after the ones a machine always had), so a disc can go in and out while
         * the machine runs (VM_CDROM); the ISO of machine.json is in it at start */
        arg("-device"); arg("virtio-scsi-pci,id=scsi0,addr=0x8");
        if (m->iso[0]) {
            arg("-blockdev"); argj(blockdev_file("cd-file", m->iso, 1));
            arg("-blockdev"); argj(blockdev_fmt("raw", "cd", "cd-file", 1));
            arg("-device"); argf("scsi-cd,id=cd0,bus=scsi0.0,drive=cd%s", bi_cd);
            snprintf(S.cd_node, sizeof S.cd_node, "cd");
        } else { arg("-device"); argf("scsi-cd,id=cd0,bus=scsi0.0%s", bi_cd); S.cd_node[0] = 0; }
    }
    if (S.win_seed) {
        /* Windows' seed disk (vmunattend.h): a removable USB stick, where Setup looks for Autounattend.xml and $WinPEDriver$ */
        snprintf(p, sizeof p, "%s/unattended/seed.img", S.disks);
        arg("-blockdev"); argj(blockdev_file("seed-file", p, 1));
        arg("-blockdev"); argj(blockdev_fmt("raw", "seed", "seed-file", 1));
        arg("-device"); arg("usb-storage,id=seed0,bus=xhci.0,drive=seed,removable=on");
    }
    if (S.unattended) {
        /* an unattended install's first start (vmunattend.h): the installer's own kernel and initrd with the answers'
         * command line, the seed disk (slot 0xa), and the installer's last restart ends QEMU instead, so lord-vm can take
         * them away before the installed system starts */
        snprintf(p, sizeof p, "%s/unattended/vmlinuz", S.disks); arg("-kernel"); arg(p);
        snprintf(p, sizeof p, "%s/unattended/initrd", S.disks); arg("-initrd"); arg(p);
        arg("-append"); arg(S.un_cmd);
        snprintf(p, sizeof p, "%s/unattended/seed.img", S.disks);
        if (file_exists(p)) {
            arg("-blockdev"); argj(blockdev_file("seed-file", p, 1));
            arg("-blockdev"); argj(blockdev_fmt("raw", "seed", "seed-file", 1));
            arg("-device"); arg("virtio-blk-pci,id=seed0,drive=seed,addr=0xa");
        }
        arg("-action"); arg("reboot=shutdown");
    }
    static const char *const NICDEV[VM_NIC_COUNT] = { "virtio-net-pci", "e1000e", "e1000", "e1000-82545em", "e1000-82544gc", "vmxnet3", "rtl8139", "pcnet" };
    if (m->network == VM_NET_BRIDGED) {
        /* the macvtap's device, opened by lord-vm (6.5): the card straight on it; no vhost (QEMU opens no device) */
        arg("-netdev"); arg("tap,id=nic0,fd=6,vhost=off");
        arg("-device"); argf("%s,netdev=nic0,mac=%s,addr=0x2%s", NICDEV[m->nic_model], m->mac, bi_net);
    }
    if (m->network == VM_NET_SHARED || m->network == VM_NET_HOSTONLY) {
        /* The card sits on a hub, and passt on another port of it: when passt ends, a new one is put on a new port
         * (QMP netdev_del, getfd, netdev_add socket + hubport) while the card stays as it was, so the guest keeps its
         * network without a restart (6.5). A card attached straight to its backend can't be given another one. */
        arg("-netdev"); arg("hubport,id=nic0,hubid=0");
        arg("-netdev"); arg("stream,id=pst0,addr.type=fd,addr.str=6");
        arg("-netdev"); arg("hubport,id=hp0,hubid=0,netdev=pst0");
        arg("-device"); argf("%s,netdev=nic0,mac=%s,addr=0x2%s", NICDEV[m->nic_model], m->mac, bi_net);
    }
    /* an imported machine's other cards: there with their addresses, so the guest keeps its interfaces; not connected */
    for (int i = 0; i < m->n_extra_macs; i++) { arg("-device"); argf("%s,id=nic%d,mac=%s,addr=0x%x", NICDEV[m->extra_models[i]], i + 1, m->extra_macs[i], 0x18 + i); }
    arg("-device"); arg("virtio-rng-pci,addr=0x3");
    /* amd64 with -nodefaults needs a display device or its kernel never starts (3.3); arm64's virt has no keyboard */
    /* 3D (decide_graphics): the same slot with virgl, and the D-Bus display shares its pictures as dmabufs (gl=on) made on
     * the render node QEMU's sandbox may open */
    if (!arm) {
        /* a chosen resolution is the EDID's preferred mode: what every driver starts with (some, the windows guests'
         * among them, never take another size from the window while they run) */
        static char vga[96];
        snprintf(vga, sizeof vga, "%s,id=video0,addr=0x4", S.gl ? "virtio-vga-gl" : "virtio-vga");
        if (m->res_w && m->res_h) snprintf(vga + strlen(vga), sizeof vga - strlen(vga), ",xres=%d,yres=%d", m->res_w, m->res_h);
        /* an Intel vGPU instead (decide_vgpu): its own display, the OpRegion QEMU hands over (the VBT Intel's driver
         * reads), and our GOP option ROM, which lights the UEFI screen on it; the guest's resets end QEMU (they crash a
         * GVT-g guest otherwise, 3 of 3) and Machines starts it again */
        if (S.vgpu) {
            arg("-device");
            argf("vfio-pci-nohotplug,id=video0,sysfsdev=%s,display=on,x-igd-opregion=on,xres=%d,yres=%d,romfile=%s,addr=0x4", S.vgpu_dev, S.vgpu_w, S.vgpu_h, GVT_ROM);
            if (!S.unattended) { arg("-action"); arg("reboot=shutdown"); }
        } else { arg("-device"); arg(vga); }
    }
    char dbus_disp[96];
    if (S.gl || S.vgpu) snprintf(dbus_disp, sizeof dbus_disp, "dbus,p2p=yes,gl=on,rendernode=%s,audiodev=snd0", S.vgpu ? S.vgpu_render : S.render);
    else snprintf(dbus_disp, sizeof dbus_disp, "dbus,p2p=yes,audiodev=snd0");
    if (m->display == VM_DISPLAY_WINDOW && (m->devices == VM_DEV_COMPAT || m->devices == VM_DEV_WINDOWS)) {
        /* a USB tablet (and keyboard on arm64) on xHCI: drivers every system has */
        if (arm) { arg("-device"); arg("virtio-gpu-pci,id=video0,addr=0x4"); }
        if (m->devices != VM_DEV_WINDOWS) { arg("-device"); arg("qemu-xhci,id=xhci,addr=0x5"); }
        if (arm) { arg("-device"); arg("usb-kbd,bus=xhci.0"); }
        arg("-device"); arg("usb-tablet,bus=xhci.0");
        /* "Capture mouse" (6.25): a relative mouse beside the tablet, used only while captured (mouse_set). Not the PS/2
         * mouse of amd64's chipset: a Linux guest drives it as VMware's VMMouse, which drops plain relative packets */
        arg("-device"); arg("usb-mouse,bus=xhci.0");
        S.cap_rel_added = 1;
        /* the same sound path as below: the HDA card after this needs its audiodev (a Windows machine in a window
         * failed to start without it, "audiodev 'snd0' not found") */
        arg("-audiodev"); arg("dbus,id=snd0,out.frequency=48000,out.channels=2,out.format=s16,in.frequency=48000,in.channels=2,in.format=s16");
        arg("-display"); arg(dbus_disp);
    } else if (m->display == VM_DISPLAY_WINDOW) {
        if (arm) { arg("-device"); arg("virtio-gpu-pci,id=video0,addr=0x4"); arg("-device"); arg("virtio-keyboard-pci,addr=0x5"); }
        arg("-device"); arg(arm ? "virtio-tablet-pci,addr=0x6" : "virtio-tablet-pci,addr=0x5");
        /* the relative mouse for "Capture mouse" (6.25), on a free slot (0x10-0x13 are the data disks, 0x18 on the extra
         * cards) */
        arg("-device"); arg("virtio-mouse-pci,addr=0x14");
        S.cap_rel_added = 1;
        /* the guest's sound goes out through the same peer-to-peer D-Bus as the picture (lord-vmdisplay listens): 48 kHz,
         * 16-bit stereo, what lord-server's recordings and speakers take without converting */
        arg("-audiodev"); arg("dbus,id=snd0,out.frequency=48000,out.channels=2,out.format=s16,in.frequency=48000,in.channels=2,in.format=s16");
        arg("-display"); arg(dbus_disp);
    } else { arg("-audiodev"); arg("none,id=snd0"); arg("-display"); arg("none"); }
    /* a sound card (HDA, which every Linux and Windows guest has a driver for), on its own slot after the CD/DVD
     * drive's: a machine made before it had none, and a state saved without it still loads (a device the saved
     * stream doesn't name keeps its reset state). Its codec has a speaker and a microphone (hda-micro) on every
     * machine, so the guest's hardware never changes with the microphone setting (snapshots and saved states keep
     * loading): what the guest's microphone hears is lord-server's choice, silence unless the machine's "microphone"
     * and LordOS's permission allow it (6.9). The codecs share one saved form ("hda-audio", the same output nodes), so
     * a state saved with the former hda-output still loads. */
    arg("-device"); arg("intel-hda,id=sound0,addr=0x9");
    arg("-device"); arg("hda-micro,id=sound0-codec0,bus=sound0.0,cad=0,audiodev=snd0");
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
    /* shared folders: each virtiofsd's connection after the credentials, a device on slots 0xb to 0xe; the transfer folder
     * (6.7.1) last, on slot 0xf, whatever the person's shares are */
    for (int i = 0, k = 0; i <= VM_SHARES; i++) {
        if (!sh_on(i)) continue;
        arg("-chardev"); argf("socket,id=vfs%d,fd=%d", i, (m->agent ? 9 : 7) + S.ncreds + k++);
        arg("-device"); argf("vhost-user-fs-pci,id=share%d,chardev=vfs%d,tag=%s,addr=0x%x", i, i, sh_tag(i), 0xb + i);
    }
    if (m->tpm) {
        /* a TPM 2.0 (CRB interface, what Windows 11 looks for) served by swtpm on the fd after the shares' */
        arg("-chardev"); argf("socket,id=chrtpm,fd=%d", (m->agent ? 9 : 7) + S.ncreds + sh_count());
        arg("-tpmdev"); arg("emulator,id=tpm0,chardev=chrtpm");
        arg("-device"); arg("tpm-crb,tpmdev=tpm0");
    }
}

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
static void start_vcpus(void);
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
    for (int i = 0; i < S.m.n_data; i++) {
        snprintf(p, sizeof p, "%s/data-%d.qcow2", S.disks, i + 1);
        if (!file_exists(p)) { fail_start("data disk %d is missing: %s", i + 1, p); return -1; }
        if (disk_plain(p, err, sizeof err)) { fail_start("data disk %d can't be used: %s", i + 1, err); return -1; }
    }
    if (S.m.iso[0] && !file_exists(S.m.iso)) { fail_start("the attached image is missing: %s", S.m.iso); return -1; }
    if (S.m.drivers[0] && !file_exists(S.m.drivers)) { vlog("note: the drivers disc is gone (%s): the machine starts without it", S.m.drivers); S.m.drivers[0] = 0; }
    S.unattended = 0;
    S.install_done = 0;
    qemu_3d_abort_seen = stop_notice = stop_announces = 0;
    run_t0 = 0;
    S.win_seed = 0;
    S.win_armed = 0;
    {
        char cp[PATH_MAX + 64];
        snprintf(cp, sizeof cp, "%s/unattended/cmdline", S.disks);
        int cfd = open(cp, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        ssize_t cn = cfd >= 0 ? read(cfd, S.un_cmd, sizeof S.un_cmd - 1) : -1;
        if (cfd >= 0) close(cfd);
        if (cn > 0) {
            S.un_cmd[cn] = 0;
            int ok = S.m.iso[0] != 0;
            for (ssize_t i = 0; i < cn; i++) if ((unsigned char)S.un_cmd[i] < 0x20 || (unsigned char)S.un_cmd[i] > 0x7e) ok = 0;
            snprintf(cp, sizeof cp, "%s/unattended/vmlinuz", S.disks);
            if (!file_exists(cp)) ok = 0;
            snprintf(cp, sizeof cp, "%s/unattended/initrd", S.disks);
            if (!file_exists(cp)) ok = 0;
            if (ok) { S.unattended = 1; vlog("unattended: this start installs the system (the installer's kernel and the answers)"); }
            else vlog("unattended: disks/unattended is incomplete or the ISO is gone: started as usual");
        }
        /* Windows (vmu_prepare_windows): no kernel of its own; the seed disk goes in as a USB stick until the guest
         * agent's first answer says the install is done */
        snprintf(cp, sizeof cp, "%s/unattended/windows", S.disks);
        char wm[16] = "";
        int wfd = open(cp, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        ssize_t wn = wfd >= 0 ? read(wfd, wm, sizeof wm - 1) : -1;
        if (wfd >= 0) close(wfd);
        snprintf(cp, sizeof cp, "%s/unattended/seed.img", S.disks);
        if (wn > 0 && !S.unattended && file_exists(cp) && S.m.devices == VM_DEV_WINDOWS) {
            S.win_seed = !strncmp(wm, "guided", 6) ? 2 : 1;
            vlog("unattended: Windows' seed disk is in (%s)", S.win_seed == 2 ? "the drivers, a guided install" : "the answers and the drivers");
        }
    }
    if (!file_exists(QEMU_BIN_OF(S.m.arch))) { fail_start("%s isn't installed", QEMU_BIN_OF(S.m.arch)); return -1; }
    if (decide_vgpu()) return -1;
    int passt_net = S.m.network == VM_NET_SHARED || S.m.network == VM_NET_HOSTONLY;
    if (passt_net && !file_exists(PASST)) { fail_start("passt isn't installed (%s network)", S.m.network == VM_NET_SHARED ? "Shared" : "Host-only"); return -1; }
    if (passt_net) {
        char perr_[200];
        if (ports_free(&S.m, perr_, sizeof perr_)) { fail_start("%s", perr_); return -1; }
    }
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
    if (S.m.network == VM_NET_BRIDGED) {
        char berr[240];
        net[0] = open_bridge_tap(&S.m, berr, sizeof berr);
        if (net[0] < 0) { fail_start("%s", berr); return -1; }
    }
    if (passt_net) {
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, net) || pipe2(perr, O_CLOEXEC)) { fail_start("out of descriptors"); return -1; }
        S.passt = spawn_passt(net[1], perr[1]);
        close(net[1]);
        close(perr[1]);
        if (S.passt < 0) { fail_start("can't start passt"); return -1; }
        S.passt_err = perr[0];
        vlog("passt: pid %d", (int)S.passt);
        S.sandbox |= VMS_PDEATHSIG;
    }
    if (sh_count() && start_shares()) return -1;
    if (S.m.tpm) {
        char terr[256];
        S.tpm_q = spawn_swtpm(terr, sizeof terr);
        if (S.tpm_q < 0) { fail_start("the TPM: %s", terr); return -1; }
        S.sandbox |= VMS_PDEATHSIG;
    }
    decide_graphics();
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
        int fds[6 + MAX_CREDS + VM_SHARES + 3], nf = 0;
        fds[nf++] = kvm; fds[nf++] = qmp[1]; fds[nf++] = ser[1]; fds[nf++] = net[0] >= 0 ? net[0] : null;
        if (S.m.agent) { fds[nf++] = qga[1]; fds[nf++] = lag[1]; }
        for (int i = 0; i < S.ncreds; i++) fds[nf++] = S.cred_fd[i];
        for (int i = 0; i <= VM_SHARES; i++) if (sh_on(i)) fds[nf++] = S.vfs_q[i];
        if (S.m.tpm) fds[nf++] = S.tpm_q;
        fds[nf++] = st[1];
        place_fds(fds, nf);
        int stf = 3 + nf - 1;
        fcntl(stf, F_SETFD, FD_CLOEXEC);
        VmQemuJail j = { S.disks, S.m.iso[0] ? S.m.iso : NULL, S.m.firmware == VM_FW_UEFI ? S.m.firmware_code : NULL, S.m.base[0] ? S.m.base : NULL,
                         S.m.drivers[0] ? S.m.drivers : NULL, S.m.accel == VM_ACCEL_KVM, S.gl ? S.render : S.vgpu ? S.vgpu_render : NULL,
                         S.vgpu ? S.vgpu_dev : NULL, S.vgpu ? S.vgpu_group : -1, S.vgpu ? GVT_ROM : NULL };
        int mask = vm_jail_qemu(&j);
        int32_t msg[2] = { mask, 0 };
        /* without our filter and no_new_privs QEMU doesn't run: the parent reads the mask and sees which */
        /* a vGPU only inside Landlock where the kernel has it: its device rules must have applied (vgpu_rules) */
        if (S.vgpu && vm_landlock_abi() >= 1 && !(mask & VMS_LANDLOCK_FS)) mask &= ~VMS_SECCOMP_OURS;
        if (!(mask & VMS_SECCOMP_OURS) || !(mask & VMS_NO_NEW_PRIVS)) { msg[0] = mask; write_all(stf, msg, sizeof msg[0]); _exit(126); }
        write_all(stf, msg, sizeof msg[0]);
        /* 3D: Mesa in QEMU can't write a shader cache (its sandbox has no home folder): told not to try */
        char *env[] = { "LC_ALL=C.UTF-8", S.gl || S.vgpu ? "MESA_SHADER_CACHE_DISABLE=true" : NULL, NULL };
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
    for (int i = 0; i <= VM_SHARES; i++) if (sh_on(i) && S.vfs_q[i] >= 0) { close(S.vfs_q[i]); S.vfs_q[i] = -1; }
    if (S.tpm_q >= 0) { close(S.tpm_q); S.tpm_q = -1; }
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
    vlog("qemu: pid %d; sandbox: landlock-fs=%d landlock-net=%d landlock-scope=%d seccomp(ours)=%d no-new-privs=%d kvm-by-fd=%d%s",
         (int)pid, !!(S.sandbox & VMS_LANDLOCK_FS), !!(S.sandbox & VMS_LANDLOCK_NET), !!(S.sandbox & VMS_LANDLOCK_SCOPE),
         !!(S.sandbox & VMS_SECCOMP_OURS), !!(S.sandbox & VMS_NO_NEW_PRIVS), !!(S.sandbox & VMS_KVM_FD), S.gl ? " render-node=1" : S.vgpu ? " vgpu=1" : "");
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
    /* a vGPU: its vCPUs run once QEMU may lock the machine's memory (vm-memlock, asked by lord-server) */
    if (S.vgpu && !vgpu_memlock_ok()) {
        S.ml_wait = now_ms() + 30000;
        vlog("graphics card: waiting until this machine's memory may be locked (%llu MB)", (unsigned long long)(vgpu_memlock() >> 20));
        /* run.json now names this QEMU: lord-server reads its pid from there for vm-memlock */
        set_state(VS_STARTING, VR_NONE, 0, "waiting for the machine's memory to be locked");
        return 0;
    }
    start_vcpus();
    return 0;
}

/* the vCPUs of a fresh start: a snapshot's memory first, or paused, or running */
static void start_vcpus(void) {
    if (S.loadvm[0]) {
        /* -loadvm would take the first snapshot-capable disk for the memory (the UEFI variables, which hold none): the
         * job names the system disk, before a vCPU runs; cont follows when it's done (snap_concluded) */
        char a[256];
        snprintf(a, sizeof a, "{\"job-id\":\"snap\",\"tag\":\"%s\",\"vmstate\":\"sys\",\"devices\":%s}", S.loadvm, snap_devices());
        S.snap_kind = VSN_REVERT; S.snap_client = -1; S.snap_seq = 0;
        snprintf(S.snap_tag, sizeof S.snap_tag, "%s", S.loadvm);
        qmp_do("snapshot-load", a, P_SNAPJOB, -1, 0, -1, -1);
        send_job(VJ_REVERT, VJS_RUNNING, -1, S.loadvm);
        return;
    }                   /* the state goes in first (P_VERSION → begin_resume), then cont */
    if (S.paused_start) set_state(VS_PAUSED, VR_NONE, 0, "started paused");
    else qmp_do("cont", NULL, P_CONT_START, -1, 0, -1, -1);
}

/* the loop's turn while a vGPU machine waits for its locked memory: on with the start, or ended after 30 s saying why */
static void vgpu_memlock_tick(long long now) {
    if (!S.ml_wait || S.qemu_done) return;
    if (vgpu_memlock_ok()) { S.ml_wait = 0; vlog("graphics card: the machine's memory may be locked"); start_vcpus(); return; }
    if (now < S.ml_wait) return;
    S.ml_wait = 0;
    fail_start("this machine's memory (%d MB + %d MB) couldn't be locked for its graphics card: Settings, Graphics cards, sets how much a machine may lock",
               S.m.memory_mb, VMGPU_MEMLOCK_EXTRA_MB);
    kill(S.qemu, SIGKILL);
    S.killed = 1;
}

/* ---- requests ---- */

static int qga_do(const char *cmd, const char *args, int kind);

/* a host stop's SIGKILL comes before logind's delay ends (vmhoststop.h), and never sooner than 1 s after the quit */
static void host_kill_cap(void) {
    if (!S.host_stopping) return;
    long long now = now_ms(), at = S.host_t0 + S.hplan.kill_at;
    if (at < now + 1000) at = now + 1000;
    if (!S.kill_deadline || at < S.kill_deadline) S.kill_deadline = at;
}

static void begin_shutdown(int secs) {
    if (S.state == VS_PAUSED) qmp_do("cont", NULL, P_IGNORE, -1, 0, -1, -1);
    S.host_shutdown = 1;
    S.shut_deadline = now_ms() + (long long)(secs ? secs : 60) * 1000;
    if (S.qga_up && (S.ag_caps & VAC_SHUTDOWN)) {
        /* the guest agent first (6.1): it shuts down whatever the guest does with its power button (a console system
         * without logind or acpid ignores it); the button too if the guest is still there 10 s later */
        vlog("agent: asking qemu-guest-agent to shut the guest down");
        qga_do("guest-shutdown", "{\"mode\":\"powerdown\"}", -1);
        S.acpi_at = now_ms() + 10000;
    } else {
        qmp_do("system_powerdown", NULL, P_IGNORE, -1, 0, -1, -1);
        if (S.qga_up) S.shutdown_at = now_ms() + 15000;     /* the agent, if it answers later */
    }
    set_state(VS_STOPPING, VR_HOST_SHUTDOWN, 0, "asked the guest to shut down");
}

static void begin_poweroff(void) {
    S.poweroff = 1;
    if (qmp_do("quit", NULL, P_IGNORE, -1, 0, -1, -1)) { kill(S.qemu, SIGKILL); S.killed = 1; }
    S.kill_deadline = now_ms() + 5000;
    host_kill_cap();
    set_state(VS_STOPPING, VR_POWEROFF, 0, "turning off");
}

/* a host stop's shutdown: the guest's own (ACPI or the agent; begin_shutdown resumes a paused machine first) until
 * the plan's quit time, then QEMU quits; with no time left for the guest, QEMU quits now */
static void host_shutdown(void) {
    long long left = S.host_t0 + S.hplan.quit_at - now_ms();
    if (left < 1000) { vlog("no time left for the guest to shut down: turning it off"); begin_poweroff(); return; }
    begin_shutdown((int)((left + 999) / 1000));
    S.shut_deadline = S.host_t0 + S.hplan.quit_at;
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

typedef struct { char qemu[64], machine_type[32], cpu[17], saved[21]; int vcpus, memory_mb, transfer; double bytes; } SuspendMeta;

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
    sm->transfer = json_bool(d.root, "transfer", 0);     /* the transfer folder's device was there (6.7.1) */
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
    if (S.host_stopping && S.qmp_up && !S.qemu_done) { vlog("the machine couldn't be saved: shutting it down instead"); host_shutdown(); }
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
    jb_printf(&j, ",\"vcpus\":%d,\"memory_mb\":%d,\"bytes\":%lld,\"transfer\":%s,\"saved\":", S.m.vcpus, S.m.memory_mb, (long long)sb.st_size, S.xfer_on ? "true" : "false");
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
    S.ran = 1;
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
 * The machine is saved to disk (a suspend: the next start resumes it); if it can't be, or the save takes too long,
 * it's shut down (ACPI or the agent), then QEMU quits, then it's killed: all inside logind's shutdown delay
 * (vmhoststop.h), so systemd's last kill never meets a QEMU (or a vGPU's VFIO device) still running. Comes from
 * logind's PrepareForShutdown (lord-vm holds a delay inhibitor), from SIGTERM (the session's units stopping) and from
 * lord-server going away without saying Keep Running. */

static long long host_delay_ms(void);

static void host_stop(const char *why) {
    if (S.qemu_done || S.suspended) return;
    if (!S.host_stopping) {
        S.host_stopping = 1;
        S.host_t0 = now_ms();
        S.hplan = vm_host_stop_plan(host_delay_ms());
        snprintf(S.host_stop_why, sizeof S.host_stop_why, "%s", why);
        vlog("%s: saving the machine to disk (save until +%lld ms, QEMU quits at +%lld ms, killed at +%lld ms)", why,
             S.hplan.save_until, S.hplan.quit_at, S.hplan.kill_at);
    }
    if (S.suspending) return;                                   /* already saving */
    if (!S.qmp_up || S.state == VS_STARTING) { begin_poweroff(); return; }
    if (S.state == VS_STOPPING) {                               /* already shutting down: within the plan */
        if (!S.shut_deadline || S.shut_deadline > S.host_t0 + S.hplan.quit_at) S.shut_deadline = S.host_t0 + S.hplan.quit_at;
        if (S.kill_deadline) host_kill_cap();
        return;
    }
    int ok_state = S.state == VS_RUNNING || S.state == VS_PAUSED;
    /* started paused and never run (Machines starts a machine paused until its window is there): no guest yet, nothing
     * to save, and a firmware that only now boots ignores the power button */
    if (S.state == VS_PAUSED && !S.ran) { vlog("the machine never ran: turning it off"); begin_poweroff(); return; }
    if (ok_state && (S.gl || S.vgpu)) { vlog("a machine with %s can't be saved to disk: shutting it down", S.vgpu ? "a graphics card" : "3D graphics (virgl can't be migrated)"); host_shutdown(); return; }
    if (ok_state && !S.hplan.save_until) { vlog("logind's delay leaves no time to save the machine: shutting it down"); host_shutdown(); return; }
    if (ok_state && !S.snap_kind && !S.cd_busy) { begin_suspend(-1, 0); return; }
    if (ok_state && (S.snap_kind || S.cd_busy)) return;         /* tried again from the loop once that's done */
    vlog("the machine can't be saved now (%s): shutting it down", STATE_NAME[S.state]);
    host_shutdown();
}

/* the loop's turn: a save that takes too long is given up for a shutdown, and at the plan's quit time QEMU quits
 * whatever is still going on (a save, a snapshot, a guest that ignores the button) */
static void host_stop_tick(long long now) {
    if (!S.host_stopping || S.qemu_done) return;
    if (now >= S.host_t0 + S.hplan.quit_at) {
        if (S.host_quit || S.poweroff || S.forced) return;      /* QEMU was told to quit already */
        S.host_quit = 1;
        S.forced = 1;
        S.shut_deadline = 0;
        vlog("the machine didn't stop in time for the computer: turning it off");
        if (S.suspending) qmp_do("migrate_cancel", NULL, P_IGNORE, -1, 0, -1, -1);
        if (qmp_do("quit", NULL, P_IGNORE, -1, 0, -1, -1)) { kill(S.qemu, SIGKILL); S.killed = 1; }
        S.kill_deadline = 0;
        host_kill_cap();
        return;
    }
    if (S.suspending) {
        if (now >= S.host_t0 + S.hplan.save_until && !S.susp_cancelled) {
            S.susp_cancelled = 1;
            vlog("suspend: not done in %lld ms: shutting the machine down instead", S.hplan.save_until);
            qmp_do("migrate_cancel", NULL, P_IGNORE, -1, 0, -1, -1);
        }
        return;
    }
    if (S.state != VS_STOPPING && (S.state == VS_RUNNING || S.state == VS_PAUSED) && !S.snap_kind && !S.cd_busy) {
        if (S.state == VS_PAUSED && !S.ran) begin_poweroff();
        else if (now < S.host_t0 + S.hplan.save_until && !S.gl && !S.vgpu) begin_suspend(-1, 0);
        else host_shutdown();
    }
}

#ifdef VM_LOGIND
/* ---- logind (6.1, 6.14): while the machine runs, the computer's shutdown waits for it (a delay inhibitor, within
 * logind's InhibitDelayMaxUSec: vmhoststop.h), and it doesn't go to sleep when idle (an idle block inhibitor, while
 * running and "Keep this computer awake" is on). Both are logind's, so `systemd-inhibit --list` names them; closing the
 * descriptor lets go. The bus itself is vmlogind.c's. ---- */
static int logind_on, inh_shutdown = -1, inh_idle = -1;

static void on_prepare_shutdown(void) { host_stop("the computer is shutting down"); }

static void awake_update(void) {
    if (!logind_on) return;
    int want = S.keep_awake && S.state == VS_RUNNING && !S.qemu_done && !S.host_stopping;
    if (want && inh_idle < 0) {
        char why[160];
        snprintf(why, sizeof why, "The machine \"%.80s\" is running", S.m.name);
        inh_idle = vml_inhibit("idle", why, "block");
        if (inh_idle >= 0) vlog("logind: this computer doesn't go to sleep when idle while the machine runs");
    } else if (!want && inh_idle >= 0) {
        close(inh_idle);
        inh_idle = -1;
        vlog("logind: idle sleep allowed again");
    }
}

static void logind_open(void) {
    if (vml_open(vlog, on_prepare_shutdown) < 0) { vlog("logind: no system bus: host shutdown isn't waited for"); return; }
    logind_on = 1;
    char why[160];
    snprintf(why, sizeof why, "The machine \"%.80s\" is saved to disk first", S.m.name);
    inh_shutdown = vml_inhibit("shutdown", why, "delay");
    if (inh_shutdown >= 0) vlog("logind: a shutdown or restart of this computer waits for the machine to be saved (up to %lld ms)", vml_delay_ms());
    awake_update();
}

static long long host_delay_ms(void) { return logind_on ? vml_delay_ms() : -1; }
static int logind_poll(struct pollfd *p, long long now, long long *next) { return logind_on && vml_poll(p, now, next); }
static void logind_process(void) { if (logind_on) vml_process(); }
/* QEMU is gone: the computer's shutdown needn't wait for the rest of lord-vm's cleanup */
static void logind_release_shutdown(void) {
    if (inh_shutdown < 0) return;
    close(inh_shutdown);
    inh_shutdown = -1;
    if (S.host_stopping) vlog("logind: the computer's shutdown no longer waits for this machine (%lld ms after the stop began)", now_ms() - S.host_t0);
}
static void logind_close(void) {
    if (inh_idle >= 0) close(inh_idle);
    if (inh_shutdown >= 0) close(inh_shutdown);
    inh_idle = inh_shutdown = -1;
    vml_close();
    logind_on = 0;
}
#else
static void awake_update(void) {}
static void logind_open(void) { vlog("logind: not built in: host shutdown isn't waited for"); }
static long long host_delay_ms(void) { return -1; }
static int logind_poll(struct pollfd *p, long long now, long long *next) { (void)p; (void)now; (void)next; return 0; }
static void logind_process(void) {}
static void logind_release_shutdown(void) {}
static void logind_close(void) {}
#endif

/* ---- snapshots while the machine runs (6.2): QMP jobs over every writable node, memory into the system disk ---- */

static const char *snap_devices(void) {
    static char b[160];
    snprintf(b, sizeof b, "[\"sys\"%s", S.m.firmware == VM_FW_UEFI ? ",\"fw-vars\"" : "");
    for (int i = 0; i < S.m.n_data; i++) snprintf(b + strlen(b), sizeof b - strlen(b), ",\"data%d\"", i + 1);
    snprintf(b + strlen(b), sizeof b - strlen(b), "]");
    return b;
}

static void snap_end(int ok, const char *why) {
    int kind = S.snap_kind == VSN_TAKE ? VJ_SNAPSHOT : S.snap_kind == VSN_REVERT ? VJ_REVERT : VJ_DELETE;
    if (ok) vlog("snapshot: %s %s done", S.snap_kind == VSN_TAKE ? "take" : S.snap_kind == VSN_REVERT ? "revert" : "delete", S.snap_tag);
    /* the TPM's copy (tpm_snap): QEMU saves and restores swtpm's state with the machine's own, so a revert while running
     * needs nothing here; the copy is for taking it back with the machine off */
    if (ok && S.snap_kind == VSN_TAKE && tpm_snap(S.dir, &S.m, TPMS_TAKE, S.snap_tag)) vlog("snapshot: %s: the TPM's copy couldn't be kept", S.snap_tag);
    if (ok && S.snap_kind == VSN_DELETE) tpm_snap(S.dir, &S.m, TPMS_DROP, S.snap_tag);
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
            S.ran = 1;
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
/* what org.lordos.agent's helper brings (kept when qemu-guest-agent comes and goes) */
#define VAC_LAG (VAC_CLIPBOARD | VAC_CLIP_NOTOOL | VAC_CLIP_LAZY | VAC_DISPLAY | VAC_OPEN | VAC_PLACE | (int)LAG_CAP_WINDOWS | VAC_EXTRA | VAC_EXTRA_OFF)
enum { AQ_IGNORE = 0, AQ_SYNC, AQ_INFO, AQ_OSINFO, AQ_PING, AQ_JEXEC, AQ_JSTATUS, AQ_FOPEN, AQ_FWRITE, AQ_FCLOSE, AQ_INSTALL, AQ_INHIBIT,
       AQ_IDEXEC, AQ_IDSTATUS, AQ_FXEXEC, AQ_FXSTATUS, AQ_FXOPEN, AQ_FXWRITE, AQ_FXCLOSE, AQ_DEVRAW,
       AQ_WIEXEC, AQ_WISTATUS, AQ_WIOPEN, AQ_WIWRITE, AQ_WICLOSE, AQ_XEXEC, AQ_XSTATUS, AQ_INSTSTATUS,
       AQ_PEXEC, AQ_PSTATUS, AQ_GEXEC, AQ_GSTATUS };

/* A Clone's first start (6.3): one fixed script, run as root by qemu-guest-agent, with the new hostname as its only
 * argument (checked: one DNS label). The machine-id is made again by systemd from the clone's new SMBIOS UUID (the old
 * D-Bus copy goes first, or it would be copied back); the hostname is set and named in /etc/hosts; SSH host keys, when
 * there are some, are made new. It prints the new values for the log. */
static const char IDENTITY_SH[] =
    "set -e; h=\"$1\"; "
    "rm -f /etc/machine-id; if [ ! -L /var/lib/dbus/machine-id ]; then rm -f /var/lib/dbus/machine-id; fi; "
    "systemd-machine-id-setup >/dev/null 2>&1 || dbus-uuidgen --ensure=/etc/machine-id; "
    "if [ -d /var/lib/dbus ] && [ ! -e /var/lib/dbus/machine-id ]; then ln -s /etc/machine-id /var/lib/dbus/machine-id; fi; "
    "hostnamectl set-hostname \"$h\" 2>/dev/null || { echo \"$h\" > /etc/hostname; hostname \"$h\"; }; "
    "if grep -q '^127\\.0\\.1\\.1' /etc/hosts; then sed -i \"s/^127\\.0\\.1\\.1.*/127.0.1.1\\t$h/\" /etc/hosts; "
    "else printf '127.0.1.1\\t%s\\n' \"$h\" >> /etc/hosts; fi; "
    "if ls /etc/ssh/ssh_host_*_key >/dev/null 2>&1; then rm -f /etc/ssh/ssh_host_*; ssh-keygen -A >/dev/null 2>&1; "
    "systemctl try-restart ssh.service >/dev/null 2>&1 || true; echo ssh-keys new; fi; "
    "echo \"hostname $(hostname) machine-id $(cat /etc/machine-id)\"";

/* The same for a Windows guest: one fixed PowerShell script, run as SYSTEM by qemu-guest-agent, with the computer name
 * (vm_winname_from_host: NetBIOS rules, checked here and again in the script) in base64 as its only data. It renames
 * the computer without restarting it: Windows uses the new name from its next restart, which the person chooses. The
 * machine SID isn't changed (that's sysprep's job; docs/VIRTUALIZATION.md 6.3). It prints the name Windows will use. */
static const char IDENTITY_PS[] =
    "$ErrorActionPreference='Stop';"
    "$h=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%s'));"
    "if($h -notmatch '^[A-Za-z0-9][A-Za-z0-9-]{0,14}$' -or $h -match '^[0-9]+$'){throw 'not a computer name'};"
    "$k='HKLM:\\SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ComputerName';"
    "if((Get-ItemProperty -LiteralPath $k).ComputerName -ne $h){Rename-Computer -NewName $h -Force -WarningAction SilentlyContinue};"
    "'computer name '+(Get-ItemProperty -LiteralPath $k).ComputerName+' from the next restart (now '+$env:COMPUTERNAME+')'";

static int qga_do(const char *cmd, const char *args, int kind);
static int fx_exec_ps(const char *script, int kind);
static void lag_send(uint16_t kind, const uint8_t *data, uint32_t n);
static void id_begin(void) {
    if (S.id_state != 1 || !(S.ag_caps & VAC_EXEC)) return;
    if (S.ag_windows) {
        char wn[VM_WINNAME_MAX + 1], b64[32], ps[sizeof IDENTITY_PS + 32];
        if (vm_winname_from_host(S.id_hostname, S.m.id, wn) || ag_b64_encode((const uint8_t *)wn, strlen(wn), b64, sizeof b64) <= 0) {
            S.id_state = 3;
            vlog("identity: %s can't be a Windows computer name: the copy keeps its name", S.id_hostname);
            return;
        }
        snprintf(S.id_winname, sizeof S.id_winname, "%s", wn);
        snprintf(ps, sizeof ps, IDENTITY_PS, b64);
        if (!fx_exec_ps(ps, AQ_IDEXEC)) { S.id_state = 2; vlog("identity: giving the guest its own computer name (%s)", wn); }
        return;
    }
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"path\":\"/bin/sh\",\"arg\":[\"-c\",");
    jb_str(&b, IDENTITY_SH);
    jb_raw(&b, ",\"lordos-identity\",");
    jb_str(&b, S.id_hostname);
    jb_raw(&b, "],\"capture-output\":true}");
    if (!b.oom && !qga_do("guest-exec", b.p, AQ_IDEXEC)) { S.id_state = 2; vlog("identity: giving the guest its own hostname (%s) and machine-id", S.id_hostname); }
    jb_free(&b);
}

/* ---- the transfer folder in the guest (6.7.1) ----
 * At each connection of the agent (and on VAG_TRANSFER) one fixed script says whether the guest has it:
 *   Linux, as root: mounts the tag at /mnt/LordOS (root's own folder: never a path in a person's home) with nosuid,nodev
 *     unless it's mounted already, then, as each person with a home (runuser), adds ~/LordOS, a link to it, when that name
 *     is free. Prints "mounted".
 *   Windows, as SYSTEM: the virtio-win guest tools' VirtIO-FS service (VirtioFsSvc) serves it as a drive once WinFsp is
 *     installed: "nosvc" without the service, "nowinfsp" without WinFsp (VAC_XFER_SETUP: VAG_TRANSFER installs it),
 *     otherwise the service is set to start by itself and started: "mounted". */
static const char XFER_MOUNT_SH[] =
    "d=/mnt/LordOS; "
    "if ! mountpoint -q \"$d\"; then "
    "[ -L \"$d\" ] && { echo 'not a folder' >&2; exit 3; }; "
    "mkdir -p \"$d\" && mount -t virtiofs -o nosuid,nodev " VM_XFER_TAG " \"$d\" || exit 4; fi; "
    "getent passwd | awk -F: '$3>=1000 && $3<60000 && $7 !~ /(nologin|false)$/ {print $1}' | while read -r u; do "
    "runuser -u \"$u\" -- /bin/sh -c '[ -d \"$HOME\" ] && [ ! -e \"$HOME/LordOS\" ] && [ ! -L \"$HOME/LordOS\" ] && ln -s /mnt/LordOS \"$HOME/LordOS\"' "
    ">/dev/null 2>&1; done; printf mounted";
static const char XFER_CHECK_PS[] =
    "$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';trap{[Console]::Error.Write($_.Exception.Message);exit 1};"
    "$s=Get-Service VirtioFsSvc -ErrorAction SilentlyContinue;if(-not $s){[Console]::Out.Write('nosvc');exit 0};"
    "$i=(Get-ItemProperty 'HKLM:\\SOFTWARE\\WOW6432Node\\WinFsp' -ErrorAction SilentlyContinue).InstallDir;"
    "if(-not $i -or -not (Test-Path -LiteralPath (Join-Path $i 'bin\\winfsp-x64.dll'))){[Console]::Out.Write('nowinfsp');exit 0};"
    "if($s.StartType -ne 'Automatic'){Set-Service VirtioFsSvc -StartupType Automatic};"
    "if($s.Status -ne 'Running'){Start-Service VirtioFsSvc};[Console]::Out.Write('mounted')";

static int fx_exec_ps(const char *script, int kind);
static int fx_exec_sh(const char *script, const char *a1, const char *a2, const char *a3, int kind);
static int fx_exec_sh5(const char *script, const char *const a[5], int kind);
static void ag_broadcast(void);
static void xfer_set(int g) {
    S.xfer_guest = g;
    S.ag_caps &= ~(VAC_TRANSFER | VAC_XFER_SETUP);
    if (g == XG_MOUNTED) S.ag_caps |= VAC_TRANSFER;
    else if (g == XG_NEEDS_WINFSP) S.ag_caps |= VAC_XFER_SETUP;
    ag_broadcast();
}
static void xfer_begin(void) {
    if (!S.xfer_on || !S.qga_up || !(S.ag_caps & VAC_EXEC) || S.xfer_pid || S.xfer_poll_at) return;
    int r = S.ag_windows ? fx_exec_ps(XFER_CHECK_PS, AQ_XEXEC) : fx_exec_sh(XFER_MOUNT_SH, NULL, NULL, NULL, AQ_XEXEC);
    if (r) vlog("transfer: the guest agent is busy: the folder is looked at the next time");
    else S.xfer_pid = -1;
}
static void xfer_reply(int kind, const JVal *root, const JVal *r, int ok) {
    if (!ok) { S.xfer_pid = 0; S.xfer_poll_at = 0; vlog("transfer: qemu-guest-agent: %s", qmp_error_desc(root)); xfer_set(XG_FAILED); return; }
    if (kind == AQ_XEXEC) { S.xfer_pid = (long)json_num(r, "pid", 0); S.xfer_poll_at = now_ms() + 300; return; }
    if (!json_bool(r, "exited", 0)) { S.xfer_poll_at = now_ms() + 300; return; }
    S.xfer_poll_at = 0; S.xfer_pid = 0;
    int code = (int)json_num(r, "exitcode", -1);
    char said[200];
    ag_exec_text(json_str(r, code == 0 ? "out-data" : "err-data"), said, sizeof said);
    int g = code != 0 ? XG_FAILED : !strcmp(said, "mounted") ? XG_MOUNTED : !strcmp(said, "nowinfsp") ? XG_NEEDS_WINFSP : !strcmp(said, "nosvc") ? XG_NO_SERVICE : XG_FAILED;
    vlog("transfer: %s", g == XG_MOUNTED ? (S.ag_windows ? "the guest's VirtIO-FS service serves the folder" : "the guest has the folder at /mnt/LordOS (~/LordOS)")
                       : g == XG_NEEDS_WINFSP ? "the guest needs WinFsp for the folder (Set Up the Transfer Folder)"
                       : g == XG_NO_SERVICE ? "the guest has no VirtIO-FS service (virtio-win guest tools)" : "the guest couldn't mount the folder");
    if (g == XG_FAILED && said[0]) vlog("transfer: the guest said (%d): %.150s", code, said);
    xfer_set(g);
}

/* ---- a Windows guest's pointer (docs/VIRTUALIZATION.md 7.1.2; docs/MACHINES-GPU.md, Part 1 step 2) ----
 * viogpudo, virtio-win's display driver, draws the pointer into the picture unless its HWCursor value is 1 (upstream's
 * default is 0): each pointer move is then a round trip through the guest before it shows (35 ms median, 97 ms at
 * worst, measured). With HWCursor 1 it hands the pointer's shape over (QEMU's CursorDefine, VD_CURSOR) and lord-server
 * draws it where the host's pointer is. Once per run, when the LordOS tools say "pointer" in their hello (they run, so
 * the person installed them), one fixed script run as SYSTEM by qemu-guest-agent sets HWCursor on the driver key of
 * every present VioGpuDod display adapter to the machine's choice (machine.json "pointer": 1 unless "guest") and
 * restarts only an adapter whose value changed (pnputil /restart-device, a second's flicker). It takes nothing from
 * the guest, the person or JS but that 0 or 1. It prints "<adapters> <restarted>". %d: 0 or 1 */
static const char PTR_PS[] =
    "$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';trap{[Console]::Error.Write($_.Exception.Message);exit 1};"
    "$w=%d;$n=0;$c=0;"
    "foreach($d in @(Get-PnpDevice -PresentOnly -Class Display -ErrorAction SilentlyContinue)){"
    "if($d.Service -ne 'VioGpuDod'){continue};"
    "$k=(Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Driver).Data;"
    "if($k -notmatch '^\\{4d36e968-e325-11ce-bfc1-08002be10318\\}\\\\\\d{4}$'){continue};$n++;"
    "$p='HKLM:\\SYSTEM\\CurrentControlSet\\Control\\Class\\'+$k;"
    "$v=(Get-ItemProperty -LiteralPath $p -Name HWCursor -ErrorAction SilentlyContinue).HWCursor;"
    "if($v -ne $w){Set-ItemProperty -LiteralPath $p -Name HWCursor -Value $w -Type DWord;pnputil.exe /restart-device $d.InstanceId|Out-Null;$c++}};"
    "[Console]::Out.Write([string]$n+' '+[string]$c)";

static void ptr_begin(void) {
    if (S.ptr_done || !S.ag_windows || !S.lag_pointer || !S.qga_up || !(S.ag_caps & VAC_EXEC) || S.ptr_pid || S.ptr_poll_at) return;
    char ps[sizeof PTR_PS + 8];
    snprintf(ps, sizeof ps, PTR_PS, S.m.pointer_guest ? 0 : 1);
    if (fx_exec_ps(ps, AQ_PEXEC)) { vlog("pointer: the guest agent is busy: tried again when the tools say hello"); return; }
    S.ptr_pid = -1;
}
static void ptr_reply(int kind, const JVal *root, const JVal *r, int ok) {
    if (!ok) { S.ptr_pid = 0; S.ptr_poll_at = 0; S.ptr_done = 1; vlog("pointer: qemu-guest-agent: %s", qmp_error_desc(root)); return; }
    if (kind == AQ_PEXEC) { S.ptr_pid = (long)json_num(r, "pid", 0); S.ptr_poll_at = now_ms() + 300; return; }
    if (!json_bool(r, "exited", 0)) { S.ptr_poll_at = now_ms() + 300; return; }
    S.ptr_poll_at = 0; S.ptr_pid = 0; S.ptr_done = 1;
    int code = (int)json_num(r, "exitcode", -1), n = 0, c = 0;
    char said[200];
    ag_exec_text(json_str(r, code == 0 ? "out-data" : "err-data"), said, sizeof said);
    if (code != 0 || sscanf(said, "%d %d", &n, &c) != 2) vlog("pointer: the guest's script failed (%d): %.150s", code, said);
    else if (!n) vlog("pointer: the guest has no VirtIO GPU display adapter (viogpudo): its pointer stays as it is");
    else vlog("pointer: %s draws it (viogpudo HWCursor %d on %d adapter%s%s)", S.m.pointer_guest ? "the guest" : "LordOS", S.m.pointer_guest ? 0 : 1,
              n, n == 1 ? "" : "s", c ? ", restarted to take it" : ", already so");
}

/* ---- Intel's driver for a Windows machine's GVT-g vGPU (docs/MACHINES-GPU.md Mode A; vmgvtwin.h) ----
 * Once per run, when the machine has a vGPU and the LordOS tools say "gvt" in their hello (w6: the person installed
 * them), the fixed GVTWIN_PS runs as SYSTEM by qemu-guest-agent: hibernation and Fast Startup off, Intel's
 * GVTWIN_DRV_VERSION on the vGPU unless it's there (downloaded from the Microsoft Update Catalog, its SHA-256 and
 * signatures checked), then that device's later driver installs denied by policy. Nothing from the guest, the person or
 * JS goes into it; what it prints is logged only, after gvtwin_parse. The download (247 MB) and the install take
 * minutes: polled each second. */
static void gvt_begin(void) {
    if (S.gvt_done || S.m.vgpu == VM_VGPU_NONE || !S.ag_windows || !S.lag_gvt || !S.qga_up || !(S.ag_caps & VAC_EXEC) || S.gvt_pid || S.gvt_poll_at) return;
    if (fx_exec_ps(GVTWIN_PS, AQ_GEXEC)) { vlog("intel driver: the guest agent is busy: tried again when the tools say hello"); return; }
    S.gvt_pid = -1;
    vlog("intel driver: checking the guest's driver for the vGPU (%s), its updates and hibernation", GVTWIN_DRV_VERSION);
}
static void gvt_reply(int kind, const JVal *root, const JVal *r, int ok) {
    if (!ok) { S.gvt_pid = 0; S.gvt_poll_at = 0; S.gvt_done = 1; vlog("intel driver: qemu-guest-agent: %s", qmp_error_desc(root)); return; }
    if (kind == AQ_GEXEC) { S.gvt_pid = (long)json_num(r, "pid", 0); S.gvt_poll_at = now_ms() + 1000; return; }
    if (!json_bool(r, "exited", 0)) { S.gvt_poll_at = now_ms() + 1000; return; }
    S.gvt_poll_at = 0; S.gvt_pid = 0; S.gvt_done = 1;
    int code = (int)json_num(r, "exitcode", -1);
    char said[400], line[400];
    GvtWinResult g;
    ag_exec_text(json_str(r, code == 0 ? "out-data" : "err-data"), said, sizeof said);
    if (code != 0 || gvtwin_parse(said, &g)) { vlog("intel driver: the guest's script failed (%d): %.200s", code, said); return; }
    gvtwin_describe(&g, line, sizeof line);
    vlog("%s", line);
}

/* VM_AGENT_STATE's text: the guest agent's version, its system and the LordOS tools' version from their hello (empty
 * until they said it; only letters, digits, '.', '-' and '_' pass, so the tab stays the separator) */
static void ag_state_text(char *t, size_t n) {
    const char *v = S.lag_hello ? S.lag_version : "";
    for (const char *c = v; *c; c++)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '-' || *c == '_')) { v = ""; break; }
    snprintf(t, n, "%s\t%s\t%s", S.ag_version, S.ag_os, v);
}

static void ag_broadcast(void) {
    char t[VM_TEXT_MAX];
    ag_state_text(t, sizeof t);
    VmMsg m = vm_msg(VM_AGENT_STATE, 0, S.ag_state, S.ag_caps, 0, 0, t);
    for (int i = 0; i < MAX_CLIENTS; i++) send_to(i, &m, -1);
}

static int qmp_do(const char *cmd, const char *args, int kind, int client, uint32_t seq, int pass_fd, int keep_fd);
/* Windows' install is done (vmunattend.h): the guest tools Setup's answers installed at the first sign-in answer. The
 * seed disk (the answers hold the account's password, only encoded) comes out of the machine, is zeroed and removed,
 * and the installer's and the drivers' discs leave machine.json, so the next start is the installed system's alone. */
static void windows_install_done(void) {
    qmp_do("device_del", "{\"id\":\"seed0\"}", P_IGNORE, -1, 0, -1, -1);
    char p[PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/unattended/seed.img", S.disks);
    int fd = open(p, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        struct stat st;
        static const char z[4096];
        if (!fstat(fd, &st)) for (off_t o = 0; o < st.st_size; o += (off_t)sizeof z) if (write(fd, z, sizeof z) < 0) break;
        fsync(fd);
        close(fd);
    }
    unlink(p);
    snprintf(p, sizeof p, "%s/unattended/windows", S.disks);
    unlink(p);
    VmMachine m;
    char err[256];
    if (!vm_machine_load(S.dir, &m, err, sizeof err)) {
        m.iso[0] = 0;
        m.drivers[0] = 0;
        if (!vm_machine_save(S.dir, &m)) { S.m.iso[0] = 0; S.m.drivers[0] = 0; }
    }
    S.win_seed = 0;
    vlog("unattended: Windows is installed (its guest agent answered): the seed disk is wiped and removed, the discs leave at the next start");
    S.wi_auto = 1;                      /* the LordOS guest tools go in once the guest agent said what it can do */
}

static void ag_set(int state) {
    if (state == S.ag_state) return;
    S.ag_state = state;
    if (state == VAS_CONNECTED && S.win_seed) windows_install_done();
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

static void fx_fail_all(const char *why);
static void wi_lost(const char *why);
static void inst_reply(int code, const char *text);
static void qga_sync(void) {
    char a[64];
    fx_fail_all("the guest agent started again");
    wi_lost("the guest agent started again");
    if (S.inst_busy) inst_reply(VME_WRONG_STATE, "the guest agent started again");
    S.agq_n = 0;                        /* nothing sent before is waited for any more */
    S.xfer_pid = 0; S.xfer_poll_at = 0;
    S.ptr_pid = 0; S.ptr_poll_at = 0;
    S.gvt_pid = 0; S.gvt_poll_at = 0;
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

/* VAG_INSTALL's answer: once, and the install is no longer busy */
static void inst_reply(int code, const char *text) {
    if (S.inst_busy) reply(S.install_client, S.install_seq, code, text);
    S.inst_busy = 0; S.helper_handle = 0; S.inst_pid = 0; S.inst_poll_at = 0;
}

static void ag_lost(const char *why) {
    fx_fail_all("qemu-guest-agent stopped answering in the guest");
    wi_lost("qemu-guest-agent stopped answering in the guest");
    if (S.qga_up) vlog("agent: %s", why);
    S.qga_up = 0; S.agq_n = 0; S.jpid = 0; S.inhibit_done = 0;
    S.xfer_pid = 0; S.xfer_poll_at = 0; S.xfer_guest = XG_UNKNOWN;
    S.ptr_pid = 0; S.ptr_poll_at = 0;
    S.gvt_pid = 0; S.gvt_poll_at = 0;
    /* an install of lordos-guest on its way: its next step's answer won't come */
    if (S.inst_busy) inst_reply(VME_WRONG_STATE, "qemu-guest-agent stopped answering in the guest");
    if (S.id_state == 2) { S.id_state = 1; S.id_poll_at = 0; }      /* tried again when the agent is back */
    S.ag_caps &= VAC_LAG;
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
static int wi_begin(int client, uint32_t seq, const char **why);
static void fx_on_reply(int kind, const JVal *root, const JVal *r, int ok);
static void wi_on_reply(int kind, const JVal *root, const JVal *r, int ok);
static void fx_fail_all(const char *why);

static void on_qga_reply(const JVal *root, int ok) {
    if (!S.agq_n) return;
    int kind = S.agq[S.agq_head];
    S.agq_head = (S.agq_head + 1) % 32;
    S.agq_n--;
    if (S.qga_up) S.ping_at = now_ms();     /* any answer says it's there (a long copy keeps the queue from emptying) */
    const JVal *r = json_get(root, "return");
    switch (kind) {
    case AQ_FXEXEC: case AQ_FXSTATUS: case AQ_FXOPEN: case AQ_FXWRITE: case AQ_FXCLOSE: fx_on_reply(kind, root, r, ok); break;
    case AQ_XEXEC: case AQ_XSTATUS: xfer_reply(kind, root, r, ok); break;
    case AQ_PEXEC: case AQ_PSTATUS: ptr_reply(kind, root, r, ok); break;
    case AQ_GEXEC: case AQ_GSTATUS: gvt_reply(kind, root, r, ok); break;
    case AQ_WIEXEC: case AQ_WISTATUS: case AQ_WIOPEN: case AQ_WIWRITE: case AQ_WICLOSE: wi_on_reply(kind, root, r, ok); break;
    case AQ_DEVRAW: {
        /* --dev-qmp: a qemu-guest-agent command for the tests; a pid, or what a guest-exec printed */
        char t[VM_TEXT_MAX], o[140];
        if (!ok) snprintf(t, sizeof t, "error: %.180s", qmp_error_desc(root));
        else if (json_get(r, "pid")) snprintf(t, sizeof t, "pid %ld", (long)json_num(r, "pid", 0));
        else if (json_get(r, "exited")) { ag_exec_text(json_str(r, "out-data"), o, sizeof o); snprintf(t, sizeof t, "exited %d exitcode %d: %s", json_bool(r, "exited", 0), (int)json_num(r, "exitcode", -1), o); }
        else snprintf(t, sizeof t, "return");
        /* the whole output in the log too (the reply holds 200 bytes), printable ASCII, in lines */
        for (int k = 0; ok && k < 2; k++) {
            const char *b = json_str(r, k ? "err-data" : "out-data");
            size_t bl = b ? strnlen(b, 1 << 20) : 0;
            uint8_t *o2 = bl ? malloc(bl) : NULL;
            long on = o2 ? ag_b64_decode(b, bl, o2, bl) : -1;
            for (long i = 0; i < on; i += 900) {
                char ln[901];
                long m = on - i < 900 ? on - i : 900;
                for (long j = 0; j < m; j++) ln[j] = o2[i + j] >= 0x20 && o2[i + j] < 0x7f ? (char)o2[i + j] : o2[i + j] == '\n' ? '|' : ' ';
                ln[m] = 0;
                vlog("dev-%s: %s", k ? "err" : "out", ln);
            }
            free(o2);
        }
        reply(S.dev_client, S.dev_seq, ok ? VME_OK : VME_QMP, t);
        break;
    }
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
        int caps = S.ag_caps & (VAC_LAG | VAC_TRANSFER | VAC_XFER_SETUP);
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
        /* Windows 11 still calls itself "Windows 10 ..." there; its build (kernel-release) is 22000 or more */
        const char *oid = ok ? json_str(r, "id") : NULL, *rel = ok ? json_str(r, "kernel-release") : NULL;
        S.ag_windows = oid && !strcmp(oid, "mswindows");
        if (S.ag_windows && rel && atol(rel) >= 22000 && !strncmp(clean, "Windows 10", 10)) clean[9] = '1';
        /* the journal (journalctl) and keeping the guest awake (systemd-inhibit) are a Linux guest's: not claimed for Windows */
        if (S.ag_windows) S.ag_caps &= ~(VAC_JOURNAL | VAC_AWAKE);
        snprintf(S.ag_os, sizeof S.ag_os, "%s", clean);
        S.ag_state = -1;
        ag_set(VAS_CONNECTED);
        if (S.keep_awake && (S.ag_caps & VAC_EXEC) && !S.ag_windows && !S.inhibit_done) {
            S.inhibit_done = 1;
            qga_do("guest-exec", "{\"path\":\"systemd-inhibit\",\"arg\":[\"--what=sleep:idle\",\"--who=LordOS Machines\","
                   "\"--why=The machine is kept awake while it runs\",\"--mode=block\",\"sleep\",\"infinity\"]}", AQ_INHIBIT);
        }
        id_begin();
        if (S.wi_auto && S.ag_windows) {
            const char *why = NULL;
            S.wi_auto = 0;
            if (wi_begin(-1, 0, &why)) vlog("agent: the LordOS guest tools for Windows weren't installed: %s", why);
        }
        xfer_begin();
        ptr_begin();
        gvt_begin();
        /* a machine that resumed from a saved state has a clipboard helper that said hello to the lord-vm before: it
         * is asked to say it again (once; its answer gets the plain hello) */
        if (!S.lag_hello && S.lag_fd >= 0) { static const char ask[] = "lord-vm 2 clipboard clipx clipreq elevated ask"; lag_send(AGK_HELLO, (const uint8_t *)ask, sizeof ask - 1); }
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
    case AQ_IDEXEC:
        if (!ok) { S.id_state = 1; vlog("identity: qemu-guest-agent couldn't run the script: %s", qmp_error_desc(root)); break; }
        S.id_pid = (long)json_num(r, "pid", 0);
        S.id_poll_at = now_ms() + 300;
        break;
    case AQ_IDSTATUS: {
        if (!ok) { S.id_state = 1; S.id_poll_at = 0; break; }
        if (!json_bool(r, "exited", 0)) { S.id_poll_at = now_ms() + 300; break; }
        S.id_poll_at = 0;
        int code = (int)json_num(r, "exitcode", -1);
        char said[200] = "";
        const char *b = json_str(r, code == 0 ? "out-data" : "err-data");
        if (b) {
            uint8_t out[600];
            long n = ag_b64_decode(b, strnlen(b, 800), out, sizeof out - 1);
            size_t k = 0;
            for (long i = 0; i < n && k + 1 < sizeof said; i++) said[k++] = out[i] >= 0x20 && out[i] < 0x7f ? (char)out[i] : ' ';
            said[k] = 0;
        }
        if (code == 0) {
            S.id_state = 3;
            vm_identity_done(S.dir);
            vlog("identity: done: %s", said);
            char t[160];
            if (S.ag_windows) snprintf(t, sizeof t, "this copy is named %s from its next restart", S.id_winname);
            else snprintf(t, sizeof t, "this copy now has its own identity: hostname %s, a new machine-id", S.id_hostname);
            notice_all(VMN_IDENTITY, S.ag_windows ? 1 : 0, t);    /* 1: a computer name, from the next restart */
        } else { S.id_state = 1; vlog("identity: the script failed (%d): %s", code, said); }
        break;
    }
    case AQ_FOPEN:
        if (!ok) { inst_reply(VME_QMP, qmp_error_desc(root)); break; }
        S.helper_handle = (long)(r && r->type == J_NUM ? r->num : 0);
        S.helper_off = 0;
        ag_install_step();
        break;
    case AQ_FWRITE: if (!ok) { inst_reply(VME_QMP, qmp_error_desc(root)); } else ag_install_step(); break;
    case AQ_FCLOSE: if (ok) ag_install_step(); else inst_reply(VME_QMP, qmp_error_desc(root)); break;
    case AQ_INSTALL:
        /* the script runs (it can take minutes: it installs the clipboard tool through the guest's package manager):
         * the install stays busy, and the answer waits, until it ends */
        if (!ok) { inst_reply(VME_QMP, qmp_error_desc(root)); vlog("agent: lordos-guest couldn't be installed"); break; }
        S.inst_pid = (long)json_num(r, "pid", 0);
        S.inst_poll_at = now_ms() + 500;
        S.inst_deadline = now_ms() + 10 * 60 * 1000;
        break;
    case AQ_INSTSTATUS: {
        int exited = ok && json_bool(r, "exited", 0);
        if (ok && !exited && now_ms() < S.inst_deadline) { S.inst_poll_at = now_ms() + 500; break; }
        int code = exited ? (int)json_num(r, "exitcode", -1) : -1;
        if (ok && !exited) {
            /* lordos-guest is in place (the script's first lines); the package manager still works in the guest */
            vlog("agent: lordos-guest installed; the clipboard tool's install still runs in the guest after 10 minutes");
            inst_reply(VME_OK, "installed");
        } else if (code == 0) {
            vlog("agent: lordos-guest installed");
            inst_reply(VME_OK, "installed");
        } else {
            char said[200] = "", t[260];
            const char *b = ok ? json_str(r, "err-data") : NULL;
            if (b) ag_exec_text(b, said, sizeof said);
            vlog("agent: lordos-guest couldn't be installed: the script failed (%d): %s", code, said);
            snprintf(t, sizeof t, "the install script failed (%d)%s%s", code, said[0] ? ": " : "", said);
            inst_reply(VME_QMP, ok ? t : qmp_error_desc(root));
        }
        break;
    }
    default: break;
    }
}

/* the steps of VAG_INSTALL: lordos-guest written in 32 KB pieces through guest-file-write, then one fixed script (no
 * argument, nothing the host or the person names): the helper, its udev rule and user service, and systemd-shutdown's hook
 * that runs `lordos-guest --stopping <verb>` (lordos-guest-local, best effort: a read-only /usr skips it; "Graphics 3D"
 * in docs/VIRTUALIZATION.md); then, best effort and
 * never failing the install (offline, a locked or unknown package manager), the clipboard tool through the guest's own
 * package manager when xclip isn't there (xclip and wl-clipboard: apt-get, dnf, zypper or pacman); then the helper
 * started again in each signed-in desktop session (uid ≥ 1000 with graphical-session.target active), so the new one
 * runs now instead of at the next sign-in. lordos-guest looks for the tool every 30 s too */
static const char INSTALL_SH[] =
    "set -e; mv -f /usr/local/bin/.lordos-guest.new /usr/local/bin/lordos-guest; chmod 755 /usr/local/bin/lordos-guest; "
    "printf '%s\\n' 'SUBSYSTEM==\"virtio-ports\", ATTR{name}==\"org.lordos.agent\", TAG+=\"uaccess\"' > /etc/udev/rules.d/71-lordos-guest.rules; "
    "mkdir -p /etc/systemd/user; printf '%s\\n' '[Unit]' 'Description=LordOS Machines clipboard' 'After=graphical-session.target' "
    "'[Service]' 'ExecStart=/usr/local/bin/lordos-guest' 'Restart=on-failure' 'RestartSec=3' '[Install]' 'WantedBy=graphical-session.target' "
    "> /etc/systemd/user/lordos-guest.service; udevadm control --reload || true; udevadm trigger --subsystem-match=virtio-ports || true; "
    "systemctl --global enable lordos-guest.service || true; "
    "{ mkdir -p /usr/lib/systemd/system-shutdown && "
      "printf '%s\\n' '#!/bin/sh' 'exec /usr/local/bin/lordos-guest --stopping \"$1\"' > /usr/lib/systemd/system-shutdown/lordos-guest-local && "
      "chmod 755 /usr/lib/systemd/system-shutdown/lordos-guest-local; } 2>/dev/null || true; "
    "command -v xclip >/dev/null 2>&1 || { "
      "if command -v apt-get >/dev/null 2>&1; then export DEBIAN_FRONTEND=noninteractive; "
        "apt-get install -y -q -o DPkg::Lock::Timeout=120 --no-install-recommends xclip wl-clipboard || "
        "{ apt-get update -q -o DPkg::Lock::Timeout=120 && apt-get install -y -q -o DPkg::Lock::Timeout=120 --no-install-recommends xclip wl-clipboard; }; "
      "elif command -v dnf >/dev/null 2>&1; then dnf install -y -q xclip wl-clipboard; "
      "elif command -v zypper >/dev/null 2>&1; then zypper --non-interactive --quiet install xclip wl-clipboard; "
      "elif command -v pacman >/dev/null 2>&1; then pacman -S --noconfirm --needed xclip wl-clipboard; fi; "
    "} </dev/null >/dev/null 2>&1 || true; "
    "for d in /run/user/[0-9]*; do u=${d##*/}; [ \"$u\" -ge 1000 ] 2>/dev/null || continue; n=$(id -nu \"$u\" 2>/dev/null) || continue; "
      "systemctl --user -M \"$n@\" daemon-reload >/dev/null 2>&1 || continue; "
      "if systemctl --user -M \"$n@\" -q is-active graphical-session.target 2>/dev/null; then "
        "systemctl --user -M \"$n@\" restart lordos-guest.service >/dev/null 2>&1 || true; fi; "
    "done; true";
static void ag_install_step(void) {
    if (S.helper_off < S.helper_n) {
        size_t n = S.helper_n - S.helper_off < 32768 ? S.helper_n - S.helper_off : 32768;
        static char b64[32768 / 3 * 4 + 64], args[32768 / 3 * 4 + 128];
        if (ag_b64_encode(S.helper + S.helper_off, n, b64, sizeof b64) < 0) { inst_reply(VME_INTERNAL, "the helper couldn't be encoded"); return; }
        snprintf(args, sizeof args, "{\"handle\":%ld,\"buf-b64\":\"%s\"}", S.helper_handle, b64);
        S.helper_off += n;
        if (qga_do("guest-file-write", args, AQ_FWRITE)) inst_reply(VME_BUSY, "busy");
        return;
    }
    if (S.helper_handle) {
        char a[64];
        snprintf(a, sizeof a, "{\"handle\":%ld}", S.helper_handle);
        S.helper_handle = 0;
        if (qga_do("guest-file-close", a, AQ_FCLOSE)) inst_reply(VME_BUSY, "busy");
        return;
    }
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"path\":\"/bin/sh\",\"arg\":[\"-c\",");
    jb_str(&b, INSTALL_SH);
    jb_raw(&b, "],\"capture-output\":true}");
    if (b.oom || qga_do("guest-exec", b.p, AQ_INSTALL)) inst_reply(VME_BUSY, "busy");
    jb_free(&b);
}

/* ---- files dropped on the machine's window (VM_FILE) ----
 * One file at a time, through qemu-guest-agent (which runs as root, or SYSTEM on Windows), into the Downloads folder of
 * the person signed in to the guest. lord-vm reads only the descriptor lord-server opened; the guest chooses no path on
 * this computer, and its answers (a folder, a path) only ever go back as text.
 *   Linux:   a private folder (mktemp -d in /var/tmp, 0700), the file written into it, then one fixed script hands the
 *            folder to the person (the first one with a seat, else the first account from 1000 up) and their own group
 *            (named: uutils' chown, Ubuntu 26.04's, leaves the group as it was for "user:") and moves the file,
 *            as that person (runuser), into their Downloads (xdg-user-dir) under a free name; the folder goes.
 *            The file is never written by root into a folder the person owns.
 *   Windows: the same shape (2026-11-20 security reviews: SYSTEM must never act by path in a folder the person
 *            controls; a link or junction put there between two steps would turn it into SYSTEM writing, deleting or
 *            planting a file anywhere). A fixed script makes %SystemRoot%\Temp\lordos-drop-<random> with a protected ACL
 *            (SYSTEM and Administrators only) and checks it; the file is written into it. The last script refuses when
 *            nobody is signed in (no SYSTEM copy into Public\Downloads, which every account can write), gives the
 *            signed-in person Traverse on the folder, Read on the file and on the copy script, Read and Write on one empty
 *            result file there, and
 *            runs a one-shot scheduled task AS that person (Interactive logon, no password stored) whose fixed script
 *            copies the file into their own Downloads under a free name (CreateNew, " (n)") and writes the outcome into
 *            the result file. SYSTEM waits for it, unregisters the task and deletes only the private folder. The copy is
 *            the person's own: whatever their Downloads points to, they only reach what their rights reach. The name
 *            reaches every script as base64, never as script text.
 * Through the transfer folder (6.7.1), when the guest has it mounted: lord-vm copies the file into the machine's transfer
 * folder on this computer itself (FX_XCOPY: a child process, a reflink where the file system can, else copy_file_range),
 * under a name it makes (.lordos-drop-<16 hex>, O_EXCL, never following a link: the guest can write that folder), then:
 *   Linux:   one fixed script finds the person (as above) and, as that person (runuser), copies /mnt/LordOS/<that name>
 *            into a hidden temporary file in their Downloads, moves it to a free name and removes the transfer copy.
 *            Root only checks that the mount is there.
 *   Windows: the private folder as above (it holds only the task's script and result file), and the person's own task
 *            copies the file from the transfer drive (found by that name) into their Downloads.
 * Any failure on that way tries the agent's way once more. lord-vm removes its file from the transfer folder whatever
 * happens. FXK_WINFSP (VAG_TRANSFER) is WinFsp's installer for a Windows guest: the agent's way into a private folder,
 * then one fixed script as SYSTEM checks its hash and signature, runs msiexec there and starts VirtioFsSvc. */
enum { FX_NONE = 0, FX_PREP, FX_OPEN, FX_WRITE, FX_CLOSE, FX_PLACE, FX_XCOPY };

static const char FX_PREP_SH[] = "umask 077; d=$(mktemp -d /var/tmp/lordos-drop.XXXXXXXX) && printf '%s' \"$d\"";
/* as the person: $1 the file, $2 its name */
static const char FX_MOVE_SH[] =
    "f=\"$1\"; n=\"$2\"; t=$(xdg-user-dir DOWNLOAD 2>/dev/null); "
    "if [ -z \"$t\" ] || [ \"$t\" = \"$HOME\" ]; then t=\"$HOME/Downloads\"; fi; "
    "mkdir -p -- \"$t\" || exit 5; "
    "case \"$n\" in ?*.*) b=\"${n%.*}\"; e=\".${n##*.}\";; *) b=\"$n\"; e=\"\";; esac; "
    "p=\"$t/$n\"; i=2; "
    "while [ -e \"$p\" ] || [ -L \"$p\" ]; do p=\"$t/$b ($i)$e\"; i=$((i+1)); [ \"$i\" -lt 1000 ] || exit 6; done; "
    "mv -n -T -- \"$f\" \"$p\" && [ ! -e \"$f\" ] || exit 7; "
    "chmod 644 -- \"$p\" 2>/dev/null; printf '%s' \"$p\"";
/* as root: $1 the private folder, $2 the name, $3 FX_MOVE_SH */
static const char FX_PLACE_SH[] =
    "d=\"$1\"; n=\"$2\"; "
    "u=$(loginctl list-sessions --no-legend 2>/dev/null | awk '$2>=1000 && $2<60000 && $4 ~ /^seat/ {print $3; exit}'); "
    "[ -n \"$u\" ] || u=$(loginctl list-sessions --no-legend 2>/dev/null | awk '$2>=1000 && $2<60000 {print $3; exit}'); "
    "[ -n \"$u\" ] || u=$(getent passwd | awk -F: '$3>=1000 && $3<60000 && $7 !~ /(nologin|false)$/ {print $1; exit}'); "
    "if [ -z \"$u\" ]; then rm -rf -- \"$d\"; echo 'no account of a person in the guest' >&2; exit 3; fi; "
    "g=$(id -gn \"$u\") && chown -R -- \"$u:$g\" \"$d\" || { rm -rf -- \"$d\"; exit 4; }; "
    "p=$(runuser -u \"$u\" -- /bin/sh -c \"$3\" lordos-drop \"$d/file\" \"$n\"); r=$?; rm -rf -- \"$d\"; "
    "[ \"$r\" -eq 0 ] || exit \"$r\"; printf '%s' \"$p\"";
/* as the person, through the transfer folder: $1 the file there (/mnt/LordOS/.lordos-drop-...), $2 its name */
static const char FX_XMOVE_SH[] =
    "d=\"$1\"; f=\"$1/file\"; n=\"$2\"; z=\"$3\"; h=\"$4\"; t=$(xdg-user-dir DOWNLOAD 2>/dev/null); "
    "if [ -z \"$t\" ] || [ \"$t\" = \"$HOME\" ]; then t=\"$HOME/Downloads\"; fi; "
    "mkdir -p -- \"$t\" || exit 5; "
    "[ -e \"$HOME/LordOS\" ] || [ -L \"$HOME/LordOS\" ] || ln -s /mnt/LordOS \"$HOME/LordOS\" 2>/dev/null; "
    /* lord-vm's folder and file as it made them: a real folder and a regular file, no link */
    "[ -d \"$d\" ] && [ ! -L \"$d\" ] && [ -f \"$f\" ] && [ ! -L \"$f\" ] || { echo 'the file in the transfer folder is not there' >&2; exit 9; }; "
    "w=$(mktemp -p \"$t\" .lordos-drop.XXXXXXXX) || exit 5; "
    /* copied and hashed in one pass; given its name only when size and SHA-256 are the ones lord-vm measured */
    "g=$(tee -- \"$w\" < \"$f\" | sha256sum) || { rm -f -- \"$w\"; exit 9; }; g=${g%% *}; "
    "[ \"$g\" = \"$h\" ] && [ \"$(stat -c %s -- \"$w\")\" = \"$z\" ] || { rm -f -- \"$w\"; echo 'the copy differs from the file dropped' >&2; exit 9; }; "
    "case \"$n\" in ?*.*) b=\"${n%.*}\"; e=\".${n##*.}\";; *) b=\"$n\"; e=\"\";; esac; "
    "p=\"$t/$n\"; i=2; "
    "while [ -e \"$p\" ] || [ -L \"$p\" ]; do p=\"$t/$b ($i)$e\"; i=$((i+1)); [ \"$i\" -lt 1000 ] || { rm -f -- \"$w\"; exit 6; }; done; "
    "mv -n -T -- \"$w\" \"$p\" && [ ! -e \"$w\" ] || { rm -f -- \"$w\"; exit 7; }; "
    "chmod 644 -- \"$p\" 2>/dev/null; printf '%s' \"$p\"";
/* as root, through the transfer folder: $1 lord-vm's folder for the file there (.lordos-drop-<hex>), $2 its name, $3
 * FX_XMOVE_SH, $4 its size, $5 its SHA-256. Exit 9: not mounted, or the copy isn't the file (the agent's way follows) */
static const char FX_XPLACE_SH[] =
    "s=\"$1\"; n=\"$2\"; mountpoint -q /mnt/LordOS || { echo 'the transfer folder is not mounted' >&2; exit 9; }; "
    "u=$(loginctl list-sessions --no-legend 2>/dev/null | awk '$2>=1000 && $2<60000 && $4 ~ /^seat/ {print $3; exit}'); "
    "[ -n \"$u\" ] || u=$(loginctl list-sessions --no-legend 2>/dev/null | awk '$2>=1000 && $2<60000 {print $3; exit}'); "
    "[ -n \"$u\" ] || u=$(getent passwd | awk -F: '$3>=1000 && $3<60000 && $7 !~ /(nologin|false)$/ {print $1; exit}'); "
    "if [ -z \"$u\" ]; then echo 'no account of a person in the guest' >&2; exit 3; fi; "
    "runuser -u \"$u\" -- /bin/sh -c \"$3\" lordos-drop \"/mnt/LordOS/$s\" \"$n\" \"$4\" \"$5\"";
/* Windows, as SYSTEM: WinFsp for the transfer folder (VAG_TRANSFER). %s: the private folder (base64 of UTF-8), then the
 * installer's SHA-256 (hex). Checks the hash and Authenticode signature of the file the agent wrote there, installs it
 * quietly (msiexec), sets the VirtIO-FS service to start by itself and starts it. The folder goes whatever happens */
static const char XFER_WINFSP_PS[] =
    "$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';trap{[Console]::Error.Write($_.Exception.Message);exit 1};"
    "$t=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%s'));"
    "try{"
    "$m=Join-Path $t 'winfsp.msi';Move-Item -LiteralPath (Join-Path $t 'file') -Destination $m;"
    "if((Get-FileHash -LiteralPath $m -Algorithm SHA256).Hash -ne '%s'){throw 'the installer is not the one LordOS checked'};"
    "$g=Get-AuthenticodeSignature -LiteralPath $m;if($g.Status -ne 'Valid'){throw ('the installer''s signature: '+$g.Status)};"
    "$s=Get-Service VirtioFsSvc -ErrorAction SilentlyContinue;if(-not $s){throw 'the VirtIO-FS service is missing (virtio-win guest tools)'};"
    "$p=Start-Process -FilePath msiexec.exe -ArgumentList @('/i',('\"'+$m+'\"'),'/qn','/norestart') -Wait -PassThru;"
    "if($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010 -and $p.ExitCode -ne 1641){throw ('msiexec ended with '+$p.ExitCode)};"
    "Set-Service VirtioFsSvc -StartupType Automatic;Start-Service VirtioFsSvc;"
    "[Console]::Out.Write([Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('mounted')))"   /* base64, as fx_exec_done reads */
    "}finally{Remove-Item -LiteralPath $t -Recurse -Force -ErrorAction SilentlyContinue}";
/* Windows, the private folder: made with its ACL at once (SYSTEM and Administrators, nothing inherited), then checked
 * (not a reparse point, owned by one of them, no other entry). Prints base64 of its path. No format: not for printf */
static const char FX_PREP_PS[] =
    "$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';trap{[Console]::Error.Write($_.Exception.Message);exit 1};"
    "$d=Join-Path $env:SystemRoot ('Temp\\lordos-drop-'+[guid]::NewGuid().ToString('N'));"
    "if(Test-Path -LiteralPath $d){throw 'already there'};"
    "$k='S-1-5-18','S-1-5-32-544';"
    "$a=New-Object Security.AccessControl.DirectorySecurity;$a.SetAccessRuleProtection($true,$false);"
    "foreach($x in $k){$a.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule((New-Object Security.Principal.SecurityIdentifier($x)),'FullControl','ContainerInherit,ObjectInherit','None','Allow')))};"
    "[IO.Directory]::CreateDirectory($d,$a)|Out-Null;"
    "if((Get-Item -LiteralPath $d -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'a reparse point'};"
    "$g=Get-Acl -LiteralPath $d;if($g.GetOwner([Security.Principal.SecurityIdentifier]).Value -notin $k){throw 'not ours'};"
    "foreach($r in $g.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){if($r.IdentityReference.Value -notin $k){throw 'not private'}};"
    "[Console]::Out.Write([Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($d)))";
/* Windows, as the person (the scheduled task's script). %s: the private folder, the name (each base64 of its UTF-8), then
 * for the transfer folder's way the file's size and SHA-256 ("" and "" the agent's way: the file is in the private
 * folder), then the open token ("" unless Files asked to open it, 7.3: lord-vm's 32 hex digits; the path the copy got
 * is written, base64, with the drop's own name ("path|name"), into %LOCALAPPDATA%\LordOS\open\<token>, the person's own folder, where only the guest tools,
 * also the person, read it once). The transfer folder's file is the one SYSTEM found by lord-vm's token (the private folder's "src"), checked
 * not to be a reparse point, copied and hashed into a hidden file in Downloads that gets its name only when size and
 * hash are lord-vm's. Writes "ok:" + base64 of the path the copy got, or "err:" + why, into the result file */
static const char FX_COPY_PS[] =
    "$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';"
    "$t=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%s'));$n=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%s'));"
    "$z='%s';$h='%s';$ot='%s';"
    "$r=Join-Path $t 'out';$p=$null;$o=$null;$w=$null;$hs=$null;"
    "function Say($x){$y=[IO.File]::Open($r,'Open','Write');$b=[Text.Encoding]::UTF8.GetBytes($x);$y.SetLength(0);$y.Write($b,0,$b.Length);$y.Close()};"
    "try{"
    "$src=Join-Path $t 'file';"
    "if($h){$src=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([IO.File]::ReadAllText((Join-Path $t 'src'))));"
    "foreach($x in @([IO.Path]::GetDirectoryName($src),$src)){if((Get-Item -LiteralPath $x -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'a reparse point in the transfer folder'}};"
    "$hs=[Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)};"
    "try{$d=(New-Object -ComObject Shell.Application).NameSpace('shell:Downloads').Self.Path}catch{$d=$null};"
    "if(-not $d){$d=Join-Path $env:USERPROFILE 'Downloads'};"
    "if(-not (Test-Path -LiteralPath $d -PathType Container)){New-Item -ItemType Directory -Path $d|Out-Null};"
    "$w=Join-Path $d ('.lordos-drop-'+[guid]::NewGuid().ToString('N')+'.part');$o=[IO.File]::Open($w,'CreateNew','Write');"
    "$s=[IO.File]::Open($src,'Open','Read','Read');"
    "try{if($hs){$buf=New-Object byte[] 4194304;while(($k=$s.Read($buf,0,$buf.Length)) -gt 0){$o.Write($buf,0,$k);$hs.AppendData($buf,0,$k)}}else{$s.CopyTo($o,4194304)}}finally{$s.Close();$o.Close();$o=$null};"
    "if($hs){$g=[BitConverter]::ToString($hs.GetHashAndReset()).Replace('-','').ToLowerInvariant();"
    "if($g -ne $h -or (Get-Item -LiteralPath $w -Force).Length -ne [long]$z){throw 'the copy differs from the file dropped'}};"
    "$b=[IO.Path]::GetFileNameWithoutExtension($n);$e=[IO.Path]::GetExtension($n);$q=Join-Path $d $n;$i=2;"
    "while(-not $p){if(-not (Test-Path -LiteralPath $q)){try{[IO.File]::Move($w,$q);$p=$q;$w=$null;continue}catch{if(-not (Test-Path -LiteralPath $q)){throw}}};"
    "if($i -gt 999){throw 'no free name'};$q=Join-Path $d ($b+' ('+$i+')'+$e);$i++};"
    "if($ot){$od=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'LordOS\\open';[IO.Directory]::CreateDirectory($od)|Out-Null;"
    "if((Get-Item -LiteralPath $od -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'a reparse point for the open record'};"
    "[IO.File]::WriteAllText((Join-Path $od $ot),[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($p))+'|'+[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($n)))};"
    "Say('ok:'+[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($p)))"
    "}catch{if($o){$o.Close()};if($w){Remove-Item -LiteralPath $w -Force -ErrorAction SilentlyContinue};Say('err:'+$_.Exception.Message+' (at '+$_.InvocationInfo.OffsetInLine+')')}";
/* Windows, the last step, as SYSTEM. %s: the private folder (base64 of UTF-8), then FX_COPY_PS (base64 of UTF-16LE),
 * which goes into the private folder as copy.ps1 for the task (a task's arguments are too short for it inline), then
 * for the transfer folder's way lord-vm's folder there (base64) and the token lord-vm wrote in it ("" and "" the agent's
 * way). SYSTEM finds the transfer drive itself: only a global drive (no person can make one) labelled LordOS whose
 * folder of that name, not a reparse point, holds that token; the person's task gets that exact path, never a search.
 * Prints the base64 path the person's copy got; the task and the private folder go whatever happens */
static const char FX_PLACE_PS[] =
    "$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';trap{[Console]::Error.Write($_.Exception.Message);exit 1};"
    "$t=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%s'));$tn=$null;$cps='%s';"
    "$sg=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%s'));$tk='%s';"
    "try{"
    "$u=(Get-CimInstance Win32_ComputerSystem).UserName;if(-not $u){throw 'no one is signed in to the machine'};"
    "$w=(New-Object Security.Principal.NTAccount($u)).Translate([Security.Principal.SecurityIdentifier]);"
    "$f=Join-Path $t 'file';$r=Join-Path $t 'out';$c=Join-Path $t 'copy.ps1';[IO.File]::WriteAllBytes($r,[byte[]]@());"
    "[IO.File]::WriteAllText($c,[Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($cps)));"
    "function Grant($p,$x,$dir){if($dir){$a=[IO.Directory]::GetAccessControl($p,'Access')}else{$a=[IO.File]::GetAccessControl($p,'Access')};"
    "$a.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($w,$x,'Allow')));"
    "if($dir){[IO.Directory]::SetAccessControl($p,$a)}else{[IO.File]::SetAccessControl($p,$a)}};"
    /* the result file needs Read too: .NET's open for writing was refused with Write alone (2026-11-20). Only that file:
     * the person can't add, rename or replace anything in the folder */
    "Grant $t 'Traverse' 1;if(Test-Path -LiteralPath $f){Grant $f 'Read' 0};Grant $c 'Read' 0;Grant $r 'Read, Write' 0;"
    "if($sg){$src=$null;foreach($v in [IO.DriveInfo]::GetDrives()){try{if($v.DriveType -ne 'Fixed' -or -not $v.IsReady -or $v.VolumeLabel -ne 'LordOS'){continue};"
    "$dd=Join-Path $v.RootDirectory.FullName $sg;$tf=Join-Path $dd 'token';"
    "if(((Get-Item -LiteralPath $dd -Force).Attributes -bor (Get-Item -LiteralPath $tf -Force).Attributes) -band [IO.FileAttributes]::ReparsePoint){continue};"
    "if([IO.File]::ReadAllText($tf).Trim() -ceq $tk){$src=Join-Path $dd 'file';break}}catch{}};"
    "if(-not $src){throw 'the transfer folder is not there'};"
    "$sf=Join-Path $t 'src';[IO.File]::WriteAllText($sf,[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($src)));Grant $sf 'Read' 0};"
    "$tn='LordOS-drop-'+[guid]::NewGuid().ToString('N');"
    "$act=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File \"'+$c+'\"');"
    "$pr=New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Limited;"
    "$st=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 30);"
    "Register-ScheduledTask -TaskName $tn -Action $act -Principal $pr -Settings $st|Out-Null;"
    "Start-ScheduledTask -TaskName $tn;$end=(Get-Date).AddMinutes(30);$k=0;"
    "while((Get-Item -LiteralPath $r).Length -eq 0){if((Get-Date) -gt $end){throw 'the copy in the person''s session took too long'};Start-Sleep -Milliseconds 250;$k++;"
    "if($k -gt 40){$i=Get-ScheduledTaskInfo -TaskName $tn;if($i.LastTaskResult -ne 267009 -and $i.LastTaskResult -ne 267011){Start-Sleep 1;"
    "if((Get-Item -LiteralPath $r).Length -eq 0){throw ('the copy in the person''s session ended without a word ('+$i.LastTaskResult+')')}}}};"
    "$o=[IO.File]::ReadAllText($r);if($o.StartsWith('ok:')){[Console]::Out.Write($o.Substring(3))}else{throw ('in the person''s session: '+$o.Substring([Math]::Min(4,$o.Length)))}"
    "}finally{if($tn){Stop-ScheduledTask -TaskName $tn -ErrorAction SilentlyContinue;Unregister-ScheduledTask -TaskName $tn -Confirm:$false -ErrorAction SilentlyContinue};"
    "Remove-Item -LiteralPath $t -Recurse -Force -ErrorAction SilentlyContinue}";
/* %s: the private folder (checked by ag_drop_staging_ok), base64 */
static const char FX_DROP_PS[] =
    "Remove-Item -LiteralPath ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%s'))) -Recurse -Force -ErrorAction SilentlyContinue";

static void fx_next(void);

/* guest-exec of a PowerShell script (pure ASCII): -EncodedCommand, base64 of its UTF-16LE */
static char *ps_encode(const char *script) {
    size_t n = strlen(script);
    uint8_t *w = malloc(n * 2 + 1);
    char *b = malloc(n * 2 / 3 * 4 + 16);
    if (w && b) {
        for (size_t i = 0; i < n; i++) { w[i * 2] = (uint8_t)script[i]; w[i * 2 + 1] = 0; }
        if (ag_b64_encode(w, n * 2, b, n * 2 / 3 * 4 + 16) <= 0) { free(b); b = NULL; }
    } else { free(b); b = NULL; }
    free(w);
    return b;
}
static int fx_exec_ps(const char *script, int kind) {
    char *b = ps_encode(script);
    int r = -1;
    {
        if (b) {
            JBuf j;
            jb_init(&j);
            jb_raw(&j, "{\"path\":\"powershell.exe\",\"arg\":[\"-NoProfile\",\"-NonInteractive\",\"-ExecutionPolicy\",\"Bypass\",\"-EncodedCommand\",");
            jb_str(&j, b);
            jb_raw(&j, "],\"capture-output\":true}");
            if (!j.oom) r = qga_do("guest-exec", j.p, kind);
            jb_free(&j);
        }
    }
    free(b);
    return r;
}
static int fx_exec_sh(const char *script, const char *a1, const char *a2, const char *a3, int kind) {
    const char *a[5] = { a1, a2, a3, NULL, NULL };
    return fx_exec_sh5(script, a, kind);
}
static int fx_exec_sh5(const char *script, const char *const a[5], int kind) {
    JBuf j;
    jb_init(&j);
    jb_raw(&j, "{\"path\":\"/bin/sh\",\"arg\":[\"-c\",");
    jb_str(&j, script);
    jb_raw(&j, ",\"lordos-drop\"");
    for (int i = 0; i < 5 && a[i]; i++) { jb_raw(&j, ","); jb_str(&j, a[i]); }
    jb_raw(&j, "],\"capture-output\":true}");
    int r = j.oom ? -1 : qga_do("guest-exec", j.p, kind);
    jb_free(&j);
    return r;
}

static void fx_progress(int force) {
    long long now = now_ms();
    if (!force && now - S.fx_told_at < 250) return;
    S.fx_told_at = now;
    int pct = S.fx_size > 0 ? (int)(S.fx_off * 100 / S.fx_size) : 0;
    if (pct > 99 && S.fx_step != FX_NONE) pct = 99;
    VmMsg m = vm_msg(VM_FILE_PROGRESS, S.fx.seq, pct, S.fx_via, 0, 0, S.fx.name);
    send_to(S.fx.client, &m, -1);
}

/* lord-vm's copy in the transfer folder: its child stopped, its file removed (the guest's copy is the person's) */
static void fx_unstage(void) {
    if (S.fx_cpid > 0) kill(S.fx_cpid, SIGKILL);
    S.fx_cpid = 0;
    if (S.fx_cpipe >= 0) close(S.fx_cpipe);
    S.fx_cpipe = -1;
    /* only through the folder's descriptor, without following a link the guest may have put there */
    if (S.fx_stagefd >= 0) { unlinkat(S.fx_stagefd, "file", 0); unlinkat(S.fx_stagefd, "token", 0); close(S.fx_stagefd); S.fx_stagefd = -1; }
    if (S.fx_stage[0] && S.xfer_dfd >= 0 && unlinkat(S.xfer_dfd, S.fx_stage, AT_REMOVEDIR) && errno != ENOENT) vlog("files: can't remove %s from the transfer folder: %s", S.fx_stage, strerror(errno));
    S.fx_stage[0] = S.fx_token[0] = S.fx_sha[0] = 0;
    S.fx_dig_n = 0;
}

static void fx_reset(void) {
    fx_unstage();
    S.fx_via = S.fx_retried = 0;
    if (S.fx.fd >= 0) close(S.fx.fd);
    memset(&S.fx, 0, sizeof S.fx);
    S.fx.fd = -1;
    S.fx_step = FX_NONE;
    S.fx_handle = S.fx_pid = 0;
    S.fx_poll_at = S.fx_deadline = 0;
    S.fx_off = S.fx_size = S.fx_sent = 0;
    S.fx_inflight = S.fx_eof = 0;
    S.fx_tmp[0] = 0;
}

static void fx_begin_agent(void);
/* the copy failed: what the guest has of it goes (when the agent still answers), the next file starts. Through the
 * transfer folder, the agent's way is tried once more first */
static void fx_fail(int code, const char *why) {
    if (S.fx_via && !S.fx_retried && S.fx.kind == FXK_DROP && S.fx.fd >= 0 && S.qga_up) {
        vlog("files: %s: the transfer folder's way failed (%s): the agent's way instead", S.fx.name, why);
        fx_unstage();
        if (S.fx_tmp[0] && S.ag_windows) {
            char b[400], ps[sizeof FX_DROP_PS + 400];
            if (ag_b64_encode((const uint8_t *)S.fx_tmp, strlen(S.fx_tmp), b, sizeof b) > 0) { snprintf(ps, sizeof ps, FX_DROP_PS, b); fx_exec_ps(ps, AQ_IGNORE); }
        }
        S.fx_tmp[0] = 0;
        S.fx_via = 0; S.fx_retried = 1;
        S.fx_handle = S.fx_pid = 0; S.fx_poll_at = 0;
        xfer_begin();                                     /* the guest's folder looked at (and mounted) again */
        S.fx_off = S.fx_sent = 0; S.fx_inflight = S.fx_eof = 0;
        fx_begin_agent();
        return;
    }
    vlog("files: %s wasn't copied: %s", S.fx.name, why);
    reply(S.fx.client, S.fx.seq, code, why);
    if (S.qga_up) {
        if (S.fx_handle > 0) { char a[48]; snprintf(a, sizeof a, "{\"handle\":%ld}", S.fx_handle); qga_do("guest-file-close", a, AQ_IGNORE); }
        /* only ever the private folder, never a path in one of the person's */
        if (S.fx_tmp[0] && !S.ag_windows) fx_exec_sh("rm -rf -- \"$1\"", S.fx_tmp, NULL, NULL, AQ_IGNORE);
        else if (S.fx_tmp[0]) {
            char b[400], ps[sizeof FX_DROP_PS + 400];
            if (ag_b64_encode((const uint8_t *)S.fx_tmp, strlen(S.fx_tmp), b, sizeof b) > 0) { snprintf(ps, sizeof ps, FX_DROP_PS, b); fx_exec_ps(ps, AQ_IGNORE); }
        }
    }
    fx_reset();
    fx_next();
}

static int open_ask(const char *where);
static void fx_done(const char *where) {
    S.fx_step = FX_NONE;
    S.fx_off = S.fx_size;
    VmMsg m = vm_msg(VM_FILE_PROGRESS, S.fx.seq, 100, S.fx_via, 0, 0, S.fx.name);
    send_to(S.fx.client, &m, -1);
    long long ms = now_ms() - S.fx_t0;
    vlog("files: %s copied into the guest (%lld bytes, %lld ms, %s): %s", S.fx.name, (long long)S.fx_size, ms, S.fx_via ? "the transfer folder's way" : "the agent's way", where);
    /* Files' "Open with" (7.3): the guest tools open it; the answer waits for theirs */
    if (S.fx.kind != FXK_DROP || (!S.fx.open && !S.fx.place)) reply(S.fx.client, S.fx.seq, VME_OK, where);
    else if (open_ask(where)) reply(S.fx.client, S.fx.seq, VME_WRONG_STATE, "the guest tools aren't connected: the file is in the guest's Downloads");
    fx_reset();
    fx_next();
}

/* ---- Files' "Open with" a machine (docs/VIRTUALIZATION.md 7.3) ----
 * The drop above, then AGK_OPEN to the guest tools: the token the person's copy step recorded the file's place under,
 * the size and SHA-256 lord-vm measured (the transfer folder's copy, or what it wrote through the agent). No path and no
 * command go to the guest; the guest tools open only the file of that record, as the person. 0 asked (the VM_REPLY waits
 * for AGK_OPENED), -1 not (the guest tools went, or too many wait) */
static int open_ask(const char *where) {
    if (S.lag_fd < 0 || !S.lag_hello || !S.ag_windows || !(S.fx.place ? S.lag_place : S.lag_open)) { vlog("files: %s wasn't handed on: the guest tools aren't connected", S.fx.name); return -1; }
    int k = -1;
    for (int i = 0; i < OP_MAX; i++) if (!S.opw[i].tok[0]) { k = i; break; }
    if (k < 0) { vlog("files: %s wasn't opened: too many files wait to be opened", S.fx.name); return -1; }
    uint8_t d[32], f[AG_OPEN_BYTES];
    if (S.fx_via) memcpy(d, S.fx_dig, 32);
    else sha256_final(&S.fx_hs, d);
    uint64_t size = (uint64_t)(S.fx_via ? S.fx_size : S.fx_sent);
    OpWait *o = &S.opw[k];
    if (S.fx.place) {
        AgPlace a;
        uint8_t pf[AG_PLACE_BYTES];
        memset(&a, 0, sizeof a);
        memcpy(a.token, S.fx_otok, sizeof a.token);
        a.size = size; memcpy(a.sha, d, 32);
        a.win = S.fx.pwin; a.x = S.fx.px; a.y = S.fx.py; a.batch = S.fx.pbatch; a.index = S.fx.pindex; a.count = S.fx.pcount;
        if (ag_place_write(pf, &a)) return -1;
        lag_send(AGK_PLACE, pf, sizeof pf);
        vlog("files: asked the guest tools to hand %s to window %08x (%u of %u, token %.8s…)", S.fx.name, a.win, a.index + 1u, a.count, S.fx_otok);
    } else {
        if (ag_open_write(f, S.fx_otok, size, d)) return -1;
        lag_send(AGK_OPEN, f, sizeof f);
        vlog("files: asked the guest tools to open %s (token %.8s…)", S.fx.name, S.fx_otok);
    }
    memcpy(o->tok, S.fx_otok, sizeof o->tok);
    snprintf(o->name, sizeof o->name, "%s", S.fx.name);
    snprintf(o->where, sizeof o->where, "%s", where);
    o->client = S.fx.client; o->seq = S.fx.seq; o->place = S.fx.place;
    /* a drop waits for its whole batch in the guest: the time grows with the files still on their way */
    o->until = now_ms() + AG_OPEN_WAIT_MS * (S.fx.place ? 5 : 1);
    return 0;
}

/* the guest tools' answer (AGK_OPENED), or none in time (o NULL-safe: status < 0 means "no answer") */
static void open_end(OpWait *o, int status, uint32_t code, int how) {
    static const char *const WHY[] = { "", "the guest tools found no record of the file (or it was too old)", "the file in the guest isn't the one sent",
                                       "the guest couldn't open it" };
    char t[VM_TEXT_MAX];
    if (status == AGOS_OK) {
        static const char *const HOW[] = { "", "into the folder of that window", "dropped on that window", "on the clipboard to paste" };
        if (o->place) vlog("files: the guest tools handed %s on (%s)", o->name, HOW[how >= 0 && how <= AGP_LAST ? how : 0]);
        else vlog("files: the guest tools opened %s", o->name);
        VmMsg r = vm_msg(VM_REPLY, o->seq, VME_OK, how > 0 ? how : 0, 0, 0, o->where);
        send_to(o->client, &r, -1);
    } else {
        if (status < 0) snprintf(t, sizeof t, "the guest tools didn't answer");
        else if (status == AGOS_FAILED) snprintf(t, sizeof t, "%s (error %u)", WHY[status], code);
        else snprintf(t, sizeof t, "%s", WHY[status]);
        vlog("files: %s wasn't opened: %s", o->name, t);
        reply(o->client, o->seq, VME_QMP, t);
    }
    memset(o, 0, sizeof *o);
}
static void open_tick(long long now) {
    for (int i = 0; i < OP_MAX; i++) if (S.opw[i].tok[0] && now > S.opw[i].until) open_end(&S.opw[i], -1, 0, 0);
}
static void open_all_end(void) {
    for (int i = 0; i < OP_MAX; i++) if (S.opw[i].tok[0]) open_end(&S.opw[i], -1, 0, 0);
}
static void open_answer(const AgFrame *f) {
    char tok[AG_OPEN_TOKEN + 1];
    uint8_t st;
    uint32_t code;
    uint8_t how = 0;
    if (f->kind == AGK_PLACED ? ag_placed_parse(f->data, f->len, tok, &st, &how, &code) : ag_opened_parse(f->data, f->len, tok, &st, &code)) {
        vlog("agent: an open answer that doesn't parse: dropped");
        return;
    }
    for (int i = 0; i < OP_MAX; i++)
        if (S.opw[i].tok[0] && !strcmp(S.opw[i].tok, tok) && S.opw[i].place == (f->kind == AGK_PLACED)) { open_end(&S.opw[i], st, code, how); return; }
    vlog("agent: an open answer for no file this side asked about: ignored");
}

/* every file that waits, and the one being copied, refused (the agent went, the machine stops) */
static void fx_fail_all(const char *why) {
    if (S.fx_step) { int up = S.qga_up; S.qga_up = 0; fx_fail(VME_WRONG_STATE, why); S.qga_up = up; }
    for (int i = 0; i < S.fxq_n; i++) { reply(S.fxq[i].client, S.fxq[i].seq, VME_WRONG_STATE, why); if (S.fxq[i].fd >= 0) close(S.fxq[i].fd); }
    S.fxq_n = 0;
}

/* keeps FX_DEPTH pieces on their way (qemu-guest-agent answers them in order); closes the file once all were answered */
static void fx_write_next(void) {
    static uint8_t buf[FX_CHUNK];
    static char b64[FX_CHUNK / 3 * 4 + 8], args[FX_CHUNK / 3 * 4 + 96];
    while (!S.fx_eof && S.fx_inflight < FX_DEPTH) {
        ssize_t n = pread(S.fx.fd, buf, sizeof buf, S.fx_sent);
        if (n < 0) { fx_fail(VME_INTERNAL, "the file couldn't be read"); return; }
        if (n == 0) { S.fx_eof = 1; break; }
        sha256_update(&S.fx_hs, buf, (size_t)n);         /* in order: what the guest gets, for AGK_OPEN */
        if (ag_b64_encode(buf, (size_t)n, b64, sizeof b64) < 0) { fx_fail(VME_INTERNAL, "internal error"); return; }
        snprintf(args, sizeof args, "{\"handle\":%ld,\"buf-b64\":\"%s\"}", S.fx_handle, b64);
        if (qga_do("guest-file-write", args, AQ_FXWRITE)) { fx_fail(VME_BUSY, "the guest agent is busy"); return; }
        S.fx_chunks[S.fx_inflight++] = (size_t)n;
        S.fx_sent += n;
    }
    if (S.fx_eof && !S.fx_inflight) {
        char a[48];
        snprintf(a, sizeof a, "{\"handle\":%ld}", S.fx_handle);
        S.fx_step = FX_CLOSE;
        if (qga_do("guest-file-close", a, AQ_FXCLOSE)) fx_fail(VME_BUSY, "the guest agent is busy");
    }
}

static void fx_open(const char *path) {
    sha256_init(&S.fx_hs);
    JBuf j;
    jb_init(&j);
    jb_raw(&j, "{\"path\":");
    jb_str(&j, path);
    jb_raw(&j, ",\"mode\":\"w\"}");
    S.fx_step = FX_OPEN;
    if (j.oom || qga_do("guest-file-open", j.p, AQ_FXOPEN)) fx_fail(VME_BUSY, "the guest agent is busy");
    jb_free(&j);
}

/* the agent's way: the private folder first (FX_PREP) */
static void fx_begin_agent(void) {
    S.fx_step = FX_PREP;
    S.fx_deadline = now_ms() + 120000;
    int r;
    if (S.ag_windows) r = fx_exec_ps(FX_PREP_PS, AQ_FXEXEC);
    else r = fx_exec_sh(FX_PREP_SH, NULL, NULL, NULL, AQ_FXEXEC);
    if (r) fx_fail(VME_BUSY, "the guest agent is busy");
}

/* the transfer folder can take this drop: on at this start, its virtiofsd running, the guest has it */
static int xfer_ready(void) { return S.xfer_on && S.xfer_dfd >= 0 && S.vfs[VM_SHARES] > 0 && S.xfer_guest == XG_MOUNTED; }

/* lord-vm's copy into the transfer folder, in a child (lord-vm's loop never waits on a file): a reflink (FICLONE) where
 * the file system can, else copy_file_range (the kernel copies; on one file system without reflink it still never
 * passes through here), else read and write; then the SHA-256 of the person's file (the guest's copy must match it).
 * It reports on the pipe: bytes so far (the copy is the first half, the hash the second), -5 and the 32 bytes of the
 * hash, -1 done, -2 - errno failed */
static void fx_copy_child(int src, int dst, int out, off_t size) {
    int64_t v;
    static uint8_t buf[1 << 20];
    off_t in = 0;
    long long told = 0;
    if (size > 0 && !ioctl(dst, FICLONE, src)) in = size;
    for (int plain = 0; in < size || !size;) {
        ssize_t n;
        if (!plain) {
            n = copy_file_range(src, &in, dst, NULL, 64 << 20, 0);
            if (n < 0 && in == 0 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP)) { plain = 1; continue; }
        } else {
            n = pread(src, buf, sizeof buf, in);
            if (n > 0) { if (write_all(dst, buf, (size_t)n)) n = -1; else in += n; }
        }
        if (n < 0) { if (errno == EINTR) continue; v = -2 - errno; write_all(out, &v, sizeof v); _exit(1); }
        if (n == 0) break;
        long long now = now_ms();
        if (now - told >= 100) { told = now; v = in / 2; write_all(out, &v, sizeof v); }
    }
    off_t copied = in;
    Sha256 h;
    sha256_init(&h);
    for (off_t off = 0;;) {
        ssize_t n = pread(src, buf, sizeof buf, off);
        if (n < 0) { if (errno == EINTR) continue; v = -2 - errno; write_all(out, &v, sizeof v); _exit(1); }
        if (!n) break;
        sha256_update(&h, buf, (size_t)n);
        off += n;
        long long now = now_ms();
        if (now - told >= 100) { told = now; v = copied / 2 + off / 2; write_all(out, &v, sizeof v); }
    }
    struct { int64_t tag; uint8_t d[32]; int64_t end; } msg = { -5, { 0 }, -1 };
    sha256_final(&h, msg.d);
    write_all(out, &msg, sizeof msg);                    /* one write: one read on the other side */
    _exit(0);
}

/* 0 when the copy into the transfer folder started (FX_XCOPY): a folder of lord-vm's own there, .lordos-drop-<16 hex>
 * (0700: in a Linux guest only the account with this computer's uid and root can enter it), holding "file" and, for
 * a Windows guest, "token" (a random value SYSTEM finds the drive by); all made without following a link */
static int fx_xcopy_start(void) {
    uint64_t rnd[3];
    if (getrandom(rnd, sizeof rnd, 0) != sizeof rnd) return -1;
    snprintf(S.fx_stage, sizeof S.fx_stage, ".lordos-drop-%016llx", (unsigned long long)rnd[0]);
    snprintf(S.fx_token, sizeof S.fx_token, "%016llx%016llx", (unsigned long long)rnd[1], (unsigned long long)rnd[2]);
    if (mkdirat(S.xfer_dfd, S.fx_stage, 0700)) { vlog("files: can't make a folder in the transfer folder: %s", strerror(errno)); S.fx_stage[0] = 0; return -1; }
    S.fx_stagefd = openat(S.xfer_dfd, S.fx_stage, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int dst = S.fx_stagefd >= 0 ? openat(S.fx_stagefd, "file", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
    if (dst < 0) { vlog("files: can't make a file in the transfer folder: %s", strerror(errno)); fx_unstage(); return -1; }
    if (S.ag_windows) {
        int tf = openat(S.fx_stagefd, "token", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (tf < 0 || write_all(tf, S.fx_token, strlen(S.fx_token))) { if (tf >= 0) close(tf); close(dst); fx_unstage(); return -1; }
        close(tf);
    }
    int pp[2];
    if (pipe2(pp, O_CLOEXEC)) { close(dst); fx_unstage(); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(dst); close(pp[0]); close(pp[1]); fx_unstage(); return -1; }
    if (!pid) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        close(pp[0]);
        fx_copy_child(S.fx.fd, dst, pp[1], S.fx_size);
    }
    close(dst);
    close(pp[1]);
    fcntl(pp[0], F_SETFL, O_NONBLOCK);
    S.fx_cpipe = pp[0];
    S.fx_cpid = pid;
    S.fx_step = FX_XCOPY;
    S.fx_dig_n = 0;
    return 0;
}

static void fx_place(void);
/* what the copy into the transfer folder says (lord-vm's loop, the pipe) */
static void fx_xcopy_read(void) {
    int64_t v[64];
    ssize_t n = read(S.fx_cpipe, v, sizeof v);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (n <= 0) { fx_fail(VME_INTERNAL, "the copy into the transfer folder stopped"); return; }
    for (size_t i = 0; i < (size_t)n / sizeof *v; i++) {
        if (S.fx_dig_n) { memcpy(S.fx_dig + (32 - 8 * S.fx_dig_n), &v[i], 8); S.fx_dig_n--; continue; }
        if (v[i] >= 0) { S.fx_off = (off_t)v[i]; fx_progress(0); continue; }
        if (v[i] == -5) { S.fx_dig_n = 4; continue; }
        if (v[i] == -1) {
            close(S.fx_cpipe); S.fx_cpipe = -1; S.fx_cpid = 0;           /* reaped by reap() */
            for (int k = 0; k < 32; k++) snprintf(S.fx_sha + k * 2, 3, "%02x", S.fx_dig[k]);
            long long ms = now_ms() - S.fx_t0;
            vlog("files: %s is in the transfer folder (%lld bytes in %lld ms, SHA-256 %.16s…)", S.fx.name, (long long)S.fx_size, ms, S.fx_sha);
            if (S.ag_windows) fx_begin_agent();                         /* the private folder for the person's task */
            else fx_place();
            return;
        }
        char why[120];
        snprintf(why, sizeof why, "the copy into the transfer folder failed: %s", strerror((int)(-2 - v[i])));
        fx_fail(VME_INTERNAL, why);
        return;
    }
}

/* the next file that waits, when nothing is being copied */
static void fx_next(void) {
    if (S.fx_step || !S.fxq_n) return;
    S.fx = S.fxq[0];
    memmove(S.fxq, S.fxq + 1, (size_t)(S.fxq_n - 1) * sizeof *S.fxq);
    S.fxq_n--;
    struct stat st;
    S.fx_size = !fstat(S.fx.fd, &st) ? st.st_size : 0;
    S.fx_off = S.fx_sent = 0;
    S.fx_inflight = S.fx_eof = 0;
    S.fx_via = S.fx_retried = 0;
    S.fx_t0 = now_ms();
    S.fx_step = FX_PREP;
    S.fx_deadline = now_ms() + 120000;
    S.fx_otok[0] = 0;
    sha256_init(&S.fx_hs);
    if (!S.qga_up) { fx_fail(VME_WRONG_STATE, "qemu-guest-agent isn't answering in the guest"); return; }
    if (S.fx.open || S.fx.place) {
        uint8_t r[AG_OPEN_TOKEN / 2];
        if (getrandom(r, sizeof r, 0) != (ssize_t)sizeof r) { fx_fail(VME_INTERNAL, "no random token"); return; }
        for (size_t i = 0; i < sizeof r; i++) snprintf(S.fx_otok + i * 2, 3, "%02x", r[i]);
    }
    if (S.fx.kind == FXK_DROP && xfer_ready()) {
        S.fx_via = 1;
        S.fx_step = FX_NONE;
        if (!fx_xcopy_start()) {
            vlog("files: copying %s (%lld bytes) into the guest through the transfer folder", S.fx.name, (long long)S.fx_size);
            fx_progress(1);
            return;
        }
        S.fx_via = 0;
        S.fx_step = FX_PREP;
    }
    vlog("files: copying %s (%lld bytes) into the guest", S.fx.name, (long long)S.fx_size);
    fx_progress(1);
    fx_begin_agent();
}

/* Windows' scripts print base64 of UTF-8 (no console code page in the way): the text, or "" when it isn't sane */
static void win_text(const char *b64, char *out, size_t cap) {
    long n = ag_b64_decode(b64, strlen(b64), (uint8_t *)out, cap - 1);
    out[n > 0 ? n : 0] = 0;
    if (n <= 0 || !ag_utf8_ok((const uint8_t *)out, (size_t)n)) { out[0] = 0; return; }
    for (long i = 0; i < n; i++) if ((unsigned char)out[i] < 0x20) { out[0] = 0; return; }
}

/* guest-exec-status of the step's script has exited: what it printed (out-data, base64) */
static void fx_exec_done(const JVal *r) {
    int code = (int)json_num(r, "exitcode", -1);
    char said[1024], txt[1024];
    ag_exec_text(json_str(r, code == 0 ? "out-data" : "err-data"), said, sizeof said);
    if (code != 0) {
        char why[VM_TEXT_MAX];
        if (S.fx_step == FX_PLACE) S.fx_tmp[0] = 0;          /* the script removed it */
        snprintf(why, sizeof why, "the guest's script failed (%d)%s%.150s", code, said[0] ? ": " : "", said);
        fx_fail(VME_QMP, why);
        return;
    }
    if (S.ag_windows) { win_text(said, txt, sizeof txt); memcpy(said, txt, sizeof said); }
    if (S.fx_step == FX_PREP) {
        /* only the private folder that script makes */
        if (!ag_drop_staging_ok(said, S.ag_windows)) { fx_fail(VME_QMP, "the guest didn't make a private folder"); return; }
        snprintf(S.fx_tmp, sizeof S.fx_tmp, "%s", said);
        if (S.fx_via) { fx_place(); return; }             /* the file is in the transfer folder already */
        char p[sizeof S.fx_tmp + 8];
        snprintf(p, sizeof p, "%s%cfile", S.fx_tmp, S.ag_windows ? '\\' : '/');
        fx_open(p);
    } else if (S.fx_step == FX_PLACE) {
        S.fx_tmp[0] = 0;                                  /* the script removed it */
        if (S.fx.kind == FXK_WINFSP) {
            if (strcmp(said, "mounted")) { fx_fail(VME_QMP, "WinFsp's installer didn't finish"); return; }
            vlog("transfer: WinFsp is installed in the guest, the VirtIO-FS service started");
            xfer_set(XG_MOUNTED);
            fx_done("the transfer folder is set up");
            return;
        }
        fx_done(said[0] ? said : S.fx.name);
    }
}

static void fx_on_reply(int kind, const JVal *root, const JVal *r, int ok) {
    if (!S.fx_step) return;
    if (!ok) { char why[VM_TEXT_MAX]; snprintf(why, sizeof why, "qemu-guest-agent: %.160s", qmp_error_desc(root)); fx_fail(VME_QMP, why); return; }
    switch (kind) {
    case AQ_FXEXEC:
        S.fx_pid = (long)json_num(r, "pid", 0);
        S.fx_poll_at = now_ms() + 150;
        break;
    case AQ_FXSTATUS:
        if (!json_bool(r, "exited", 0)) {
            if (now_ms() > S.fx_deadline) { fx_fail(VME_QMP, "the guest's script took too long"); break; }
            S.fx_poll_at = now_ms() + 200;
            break;
        }
        fx_exec_done(r);
        break;
    case AQ_FXOPEN:
        S.fx_handle = (long)(r && r->type == J_NUM ? r->num : 0);
        if (S.fx_handle <= 0) { fx_fail(VME_QMP, "the guest agent didn't open the file"); break; }
        S.fx_step = FX_WRITE;
        fx_write_next();
        break;
    case AQ_FXWRITE: {
        long count = (long)json_num(r, "count", -1);
        if (!S.fx_inflight || count != (long)S.fx_chunks[0]) { fx_fail(VME_QMP, "the guest agent wrote less than it was given (is its disk full?)"); break; }
        S.fx_off += (off_t)S.fx_chunks[0];
        memmove(S.fx_chunks, S.fx_chunks + 1, (size_t)(--S.fx_inflight) * sizeof *S.fx_chunks);
        if (S.fx_off > S.fx_size) S.fx_size = S.fx_off;
        fx_progress(0);
        fx_write_next();
        break;
    }
    case AQ_FXCLOSE:
        S.fx_handle = 0;
        fx_place();
        break;
    }
}

/* the last step: the guest's fixed script puts the file where the person gets it (or installs WinFsp) */
static void fx_place(void) {
    S.fx_step = FX_PLACE;
    /* the last script copies the whole file once more (Windows: in the person's session): its time grows with it */
    S.fx_deadline = now_ms() + 120000 + (long long)(S.fx_size / 10000);
    char bt[400];
    if (S.ag_windows && ag_b64_encode((const uint8_t *)S.fx_tmp, strlen(S.fx_tmp), bt, sizeof bt) < 0) { fx_fail(VME_INTERNAL, "internal error"); return; }
    if (S.fx.kind == FXK_WINFSP) {
        char ps[sizeof XFER_WINFSP_PS + sizeof bt + 80];
        snprintf(ps, sizeof ps, XFER_WINFSP_PS, bt, S.fx.sha);
        if (fx_exec_ps(ps, AQ_FXEXEC)) fx_fail(VME_BUSY, "the guest agent is busy");
    } else if (S.ag_windows) {
        char bn[AG_NAME_MAX / 3 * 4 + 16], bs[80] = "", sz[24] = "", copy[sizeof FX_COPY_PS + 2 * sizeof bt + 200];
        if (ag_b64_encode((const uint8_t *)S.fx.name, strlen(S.fx.name), bn, sizeof bn) < 0 ||
            (S.fx_via && ag_b64_encode((const uint8_t *)S.fx_stage, strlen(S.fx_stage), bs, sizeof bs) < 0)) { fx_fail(VME_INTERNAL, "internal error"); return; }
        if (S.fx_via) snprintf(sz, sizeof sz, "%lld", (long long)S.fx_size);
        snprintf(copy, sizeof copy, FX_COPY_PS, bt, bn, sz, S.fx_via ? S.fx_sha : "", S.fx.open || S.fx.place ? S.fx_otok : "");
        char *enc = ps_encode(copy);
        size_t pn = sizeof FX_PLACE_PS + sizeof bt + sizeof bs + sizeof S.fx_token + (enc ? strlen(enc) : 0);
        char *ps = enc ? malloc(pn) : NULL;
        if (ps) snprintf(ps, pn, FX_PLACE_PS, bt, enc, bs, S.fx_via ? S.fx_token : "");
        if (!ps) fx_fail(VME_INTERNAL, "internal error");
        else if (fx_exec_ps(ps, AQ_FXEXEC)) fx_fail(VME_BUSY, "the guest agent is busy");
        free(ps); free(enc);
    } else if (S.fx_via) {
        char sz[24];
        snprintf(sz, sizeof sz, "%lld", (long long)S.fx_size);
        const char *a[5] = { S.fx_stage, S.fx.name, FX_XMOVE_SH, sz, S.fx_sha };
        if (fx_exec_sh5(FX_XPLACE_SH, a, AQ_FXEXEC)) fx_fail(VME_BUSY, "the guest agent is busy");
    } else if (fx_exec_sh(FX_PLACE_SH, S.fx_tmp, S.fx.name, FX_MOVE_SH, AQ_FXEXEC)) fx_fail(VME_BUSY, "the guest agent is busy");
}

static void fx_tick(long long now) {
    if (!S.fx_poll_at || now < S.fx_poll_at) return;
    S.fx_poll_at = 0;
    char a[48];
    snprintf(a, sizeof a, "{\"pid\":%ld}", S.fx_pid);
    if (qga_do("guest-exec-status", a, AQ_FXSTATUS)) S.fx_poll_at = now + 200;
}

/* ---- LordOS guest tools for Windows (VAG_INSTALL on a Windows guest; docs/VIRTUALIZATION.md 7.1) ----
 * Through qemu-guest-agent, which runs as SYSTEM: SYSTEM never writes by a path the person controls.
 *   1. FX_PREP_PS: a private folder in %SystemRoot%\Temp (SYSTEM and Administrators only, checked), as for a drop;
 *   2. lordos-guest-win.exe written into it in 32 KB pieces;
 *   3. WI_INSTALL_PS: its SHA-256 checked against what lord-vm sent; %ProgramFiles%\LordOS made (or checked: not a
 *      reparse point) with its own protected ACL (SYSTEM and Administrators full control, Users read and run, owned
 *      by Administrators); a running copy stopped; the program copied in (it takes the folder's ACL); HKLM's Run key
 *      starts it at every sign-in, as the person; and for the person signed in now, a one-shot scheduled task (their
 *      Interactive logon, no password stored, no time limit) starts it at once, then goes. The private folder goes
 *      whatever happens. Nothing of the person's is touched, and the program runs as them only. */
enum { WI_NONE = 0, WI_PREP, WI_OPEN, WI_WRITE, WI_CLOSE, WI_INSTALL };
/* %s: the private folder (base64 of UTF-8), then the program's SHA-256 (64 hex digits, upper case) */
static const char WI_INSTALL_PS[] =
    "$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';trap{[Console]::Error.Write($_.Exception.Message);exit 1};"
    "$t=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%s'));"
    "try{"
    "$f=Join-Path $t 'lordos-guest-win.exe';"
    "if((Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash -ne '%s'){throw 'the copy in the guest is not the one sent'};"
    "$d=Join-Path $env:ProgramFiles 'LordOS';"
    "if(Test-Path -LiteralPath $d){if((Get-Item -LiteralPath $d -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'a reparse point in Program Files'}}"
    "else{[IO.Directory]::CreateDirectory($d)|Out-Null};"
    "$a=New-Object Security.AccessControl.DirectorySecurity;$a.SetAccessRuleProtection($true,$false);"
    "foreach($x in 'S-1-5-18','S-1-5-32-544'){$a.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule((New-Object Security.Principal.SecurityIdentifier($x)),'FullControl','ContainerInherit,ObjectInherit','None','Allow')))};"
    "$a.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule((New-Object Security.Principal.SecurityIdentifier('S-1-5-32-545')),'ReadAndExecute','ContainerInherit,ObjectInherit','None','Allow')));"
    "$a.SetOwner((New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')));"
    "[IO.Directory]::SetAccessControl($d,$a);"
    "$e=Join-Path $d 'lordos-guest-win.exe';"
    "Get-Process -Name 'lordos-guest-win' -ErrorAction SilentlyContinue|Stop-Process -Force -ErrorAction SilentlyContinue;Start-Sleep -Milliseconds 500;"
    "if(Test-Path -LiteralPath $e){if((Get-Item -LiteralPath $e -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'a reparse point in Program Files'};Remove-Item -LiteralPath $e -Force};"
    "[IO.File]::Copy($f,$e);"
    "Set-ItemProperty -LiteralPath 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run' -Name 'LordOS guest tools' -Value ('\"'+$e+'\"');"
    /* the port: vioserial lets only SYSTEM and Administrators open it; the person signed in at the machine (Interactive
     * users) may read and write it too. Found by what \\.\\Global\\org.lordos.agent points to, set as the device's
     * own security (kept across restarts), applied by restarting that one port */
    "Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;using System.Text;public static class LordPort{"
    "[DllImport(\"kernel32.dll\",CharSet=CharSet.Unicode)]static extern uint QueryDosDeviceW(string n,StringBuilder t,int m);"
    "[DllImport(\"setupapi.dll\",CharSet=CharSet.Unicode)]static extern IntPtr SetupDiCreateDeviceInfoList(IntPtr g,IntPtr w);"
    "[StructLayout(LayoutKind.Sequential)]struct DI{public int cb;public Guid g;public int inst;public IntPtr r;}"
    "[DllImport(\"setupapi.dll\",CharSet=CharSet.Unicode,SetLastError=true)]static extern bool SetupDiOpenDeviceInfoW(IntPtr s,string id,IntPtr w,int f,ref DI d);"
    "[DllImport(\"setupapi.dll\",CharSet=CharSet.Unicode,SetLastError=true)]static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr s,ref DI d,int p,byte[] b,int n);"
    "[DllImport(\"setupapi.dll\")]static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);"
    "public static string Target(string n){var b=new StringBuilder(512);return QueryDosDeviceW(n,b,512)>0?b.ToString():\"\";}"
    "public static int Sddl(string id,string sd){IntPtr s=SetupDiCreateDeviceInfoList(IntPtr.Zero,IntPtr.Zero);var d=new DI();d.cb=Marshal.SizeOf(d);int e=0;"
    "if(!SetupDiOpenDeviceInfoW(s,id,IntPtr.Zero,0,ref d))e=Marshal.GetLastWin32Error();"
    "else{byte[] b=Encoding.Unicode.GetBytes(sd+\"\\0\");if(!SetupDiSetDeviceRegistryPropertyW(s,ref d,0x18,b,b.Length))e=Marshal.GetLastWin32Error();}"
    "SetupDiDestroyDeviceInfoList(s);return e;}}';"
    "$pt=[LordPort]::Target('org.lordos.agent');if(-not $pt){throw 'the machine has no org.lordos.agent port'};"
    "$pd=Get-PnpDevice -PresentOnly|?{$_.InstanceId -match '\\\\VIOSERIALPORT\\\\'}|?{(Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_PDOName).Data -eq $pt}|Select-Object -First 1;"
    "if(-not $pd){throw 'org.lordos.agent''s device not found'};"
    "$sd='D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)';"
    "if((Get-PnpDeviceProperty -InstanceId $pd.InstanceId -KeyName DEVPKEY_Device_SecuritySDS).Data -ne 'D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GWGR;;;IU)'){"
    "$r=[LordPort]::Sddl($pd.InstanceId,$sd);if($r){throw ('the port''s security could not be set ('+$r+')')};"
    "pnputil.exe /restart-device $pd.InstanceId|Out-Null};"
    "$u=(Get-CimInstance Win32_ComputerSystem).UserName;$s='installed';"
    "if($u){$tn='LordOS-tools-'+[guid]::NewGuid().ToString('N');"
    "try{$act=New-ScheduledTaskAction -Execute $e;"
    "$pr=New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Limited;"
    "$st=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero);"
    "Register-ScheduledTask -TaskName $tn -Action $act -Principal $pr -Settings $st|Out-Null;Start-ScheduledTask -TaskName $tn;"
    "for($i=0;$i -lt 20 -and -not (Get-Process -Name 'lordos-guest-win' -ErrorAction SilentlyContinue);$i++){Start-Sleep -Milliseconds 250};"
    "if(Get-Process -Name 'lordos-guest-win' -ErrorAction SilentlyContinue){$s='installed and started'}"
    "}finally{Unregister-ScheduledTask -TaskName $tn -Confirm:$false -ErrorAction SilentlyContinue}};"
    "[Console]::Out.Write($s)"
    "}finally{Remove-Item -LiteralPath $t -Recurse -Force -ErrorAction SilentlyContinue}";

static void wi_end(int code, const char *why) {
    vlog("agent: LordOS guest tools for Windows: %s", why);
    if (S.wi_client >= 0) reply(S.wi_client, S.wi_seq, code, why);
    if (code != VME_OK && S.qga_up) {
        if (S.wi_handle > 0) { char a[48]; snprintf(a, sizeof a, "{\"handle\":%ld}", S.wi_handle); qga_do("guest-file-close", a, AQ_IGNORE); }
        if (S.wi_tmp[0] && S.wi_step < WI_INSTALL) {
            char b[400], ps[sizeof FX_DROP_PS + 400];
            if (ag_b64_encode((const uint8_t *)S.wi_tmp, strlen(S.wi_tmp), b, sizeof b) > 0) { snprintf(ps, sizeof ps, FX_DROP_PS, b); fx_exec_ps(ps, AQ_IGNORE); }
        }
    }
    S.wi_step = WI_NONE;
    S.wi_handle = S.wi_pid = 0;
    S.wi_poll_at = 0;
    S.wi_tmp[0] = 0;
    S.wi_off = 0;
}

/* starts the install (client -1: after an unattended install, nobody to answer). 0 started */
static int wi_begin(int client, uint32_t seq, const char **why) {
    if (S.dev_qmp) {
        /* development: a new build dropped into the machine's folder (lord-vm's Landlock keeps it there) is taken */
        char p[PATH_MAX + 32];
        snprintf(p, sizeof p, "%s/dev-wintools.exe", S.dir);
        int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        struct stat st;
        uint8_t *b;
        if (fd >= 0 && !fstat(fd, &st) && st.st_size > 0 && st.st_size < (8 << 20) && (b = malloc((size_t)st.st_size))) {
            if (pread(fd, b, (size_t)st.st_size, 0) == st.st_size) { free(S.wtools); S.wtools = b; S.wtools_n = (size_t)st.st_size; vlog("dev: %s", p); }
            else free(b);
        }
        if (fd >= 0) close(fd);
    }
    if (!S.wtools) { *why = "the LordOS guest tools for Windows aren't on this computer"; return -1; }
    if (S.wi_step || S.helper_handle) { *why = "busy"; return -1; }
    if (!(S.ag_caps & VAC_FILES) || !(S.ag_caps & VAC_EXEC)) { *why = "the guest agent can't write files or run commands"; return -1; }
    S.wi_client = client; S.wi_seq = seq;
    S.wi_step = WI_PREP;
    S.wi_deadline = now_ms() + 120000;
    if (fx_exec_ps(FX_PREP_PS, AQ_WIEXEC)) { S.wi_step = WI_NONE; *why = "the guest agent is busy"; return -1; }
    vlog("agent: installing the LordOS guest tools for Windows (%zu bytes)", S.wtools_n);
    return 0;
}

static void wi_write_next(void) {
    if (S.wi_off < S.wtools_n) {
        size_t n = S.wtools_n - S.wi_off < 32768 ? S.wtools_n - S.wi_off : 32768;
        static char b64[32768 / 3 * 4 + 64], args[32768 / 3 * 4 + 128];
        if (ag_b64_encode(S.wtools + S.wi_off, n, b64, sizeof b64) < 0) { wi_end(VME_INTERNAL, "internal error"); return; }
        snprintf(args, sizeof args, "{\"handle\":%ld,\"buf-b64\":\"%s\"}", S.wi_handle, b64);
        S.wi_off += n;
        if (qga_do("guest-file-write", args, AQ_WIWRITE)) wi_end(VME_BUSY, "the guest agent is busy");
        return;
    }
    char a[48];
    snprintf(a, sizeof a, "{\"handle\":%ld}", S.wi_handle);
    S.wi_handle = 0;
    S.wi_step = WI_CLOSE;
    if (qga_do("guest-file-close", a, AQ_WICLOSE)) wi_end(VME_BUSY, "the guest agent is busy");
}

static void wi_on_reply(int kind, const JVal *root, const JVal *r, int ok) {
    if (!S.wi_step) return;
    if (!ok) { char why[VM_TEXT_MAX]; snprintf(why, sizeof why, "qemu-guest-agent: %.160s", qmp_error_desc(root)); wi_end(VME_QMP, why); return; }
    switch (kind) {
    case AQ_WIEXEC: S.wi_pid = (long)json_num(r, "pid", 0); S.wi_poll_at = now_ms() + 300; break;
    case AQ_WISTATUS: {
        if (!json_bool(r, "exited", 0)) {
            if (now_ms() > S.wi_deadline) { wi_end(VME_QMP, "the guest's script took too long"); break; }
            S.wi_poll_at = now_ms() + 300;
            break;
        }
        int code = (int)json_num(r, "exitcode", -1);
        char said[1024], txt[1024];
        ag_exec_text(json_str(r, code == 0 ? "out-data" : "err-data"), said, sizeof said);
        if (code != 0) {
            char why[VM_TEXT_MAX];
            snprintf(why, sizeof why, "the guest's script failed (%d)%s%.150s", code, said[0] ? ": " : "", said);
            if (S.wi_step == WI_INSTALL) S.wi_tmp[0] = 0;           /* the script removed it */
            wi_end(VME_QMP, why);
            break;
        }
        if (S.wi_step == WI_PREP) {
            win_text(said, txt, sizeof txt);
            if (!ag_drop_staging_ok(txt, 1)) { wi_end(VME_QMP, "the guest didn't make a private folder"); break; }
            snprintf(S.wi_tmp, sizeof S.wi_tmp, "%.200s", txt);       /* ag_drop_staging_ok: at most 200 */
            JBuf b;
            jb_init(&b);
            char p[sizeof S.wi_tmp + 32];
            snprintf(p, sizeof p, "%s\\lordos-guest-win.exe", S.wi_tmp);
            jb_raw(&b, "{\"path\":");
            jb_str(&b, p);
            jb_raw(&b, ",\"mode\":\"wb\"}");
            S.wi_step = WI_OPEN;
            if (b.oom || qga_do("guest-file-open", b.p, AQ_WIOPEN)) wi_end(VME_BUSY, "the guest agent is busy");
            jb_free(&b);
        } else if (S.wi_step == WI_INSTALL) {
            S.wi_tmp[0] = 0;
            snprintf(txt, sizeof txt, "%.100s", said[0] ? said : "installed");
            wi_end(VME_OK, txt);
        }
        break;
    }
    case AQ_WIOPEN:
        S.wi_handle = (long)(r && r->type == J_NUM ? r->num : 0);
        if (S.wi_handle <= 0) { wi_end(VME_QMP, "the guest agent didn't open the file"); break; }
        S.wi_step = WI_WRITE;
        S.wi_off = 0;
        wi_write_next();
        break;
    case AQ_WIWRITE: wi_write_next(); break;
    case AQ_WICLOSE: {
        uint8_t d[32];
        char hex[65], b[400];
        sha256(S.wtools, S.wtools_n, d);
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02X", d[i]);
        if (ag_b64_encode((const uint8_t *)S.wi_tmp, strlen(S.wi_tmp), b, sizeof b) < 0) { wi_end(VME_INTERNAL, "internal error"); break; }
        char *ps = malloc(sizeof WI_INSTALL_PS + sizeof b + sizeof hex);
        if (!ps) { wi_end(VME_INTERNAL, "out of memory"); break; }
        snprintf(ps, sizeof WI_INSTALL_PS + sizeof b + sizeof hex, WI_INSTALL_PS, b, hex);
        S.wi_step = WI_INSTALL;
        if (fx_exec_ps(ps, AQ_WIEXEC)) wi_end(VME_BUSY, "the guest agent is busy");
        free(ps);
        break;
    }
    default: break;
    }
}

static void wi_lost(const char *why) {
    if (!S.wi_step) return;
    int up = S.qga_up;
    S.qga_up = 0;
    wi_end(VME_WRONG_STATE, why);
    S.qga_up = up;
}

static void wi_tick(long long now) {
    if (!S.wi_poll_at || now < S.wi_poll_at) return;
    S.wi_poll_at = 0;
    char a[48];
    snprintf(a, sizeof a, "{\"pid\":%ld}", S.wi_pid);
    if (qga_do("guest-exec-status", a, AQ_WISTATUS)) S.wi_poll_at = now + 300;
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

/* ---- the host's clipboard on demand (6.8): lord-server offers, the guest asks when a program there pastes, lord-server
 * answers only while the machine is in use (it holds that gate; lord-vm can't see the windows). What lord-vm checks: the
 * request parses, is for the last offer and one of its forms, the clipboard goes to the guest (machine.json), someone
 * is there to answer, and the guest doesn't ask more than CLIP_REQ_RATE times a second ---- */
#define CLIP_REQ_RATE 8
#define CLIP_WAIT_MS 40000
static void clip_answer(uint32_t gen, uint8_t form, uint8_t status, const uint8_t *d, uint32_t n) {
    uint8_t *b = malloc(6 + (size_t)n);
    if (!b) return;
    ag_clipdata_head(b, gen, form, status);
    if (n) memcpy(b + 6, d, n);
    lag_send(AGK_CLIPDATA, b, 6 + n);
    free(b);
}

static int clip_clients(void) { int n = 0; for (int i = 0; i < MAX_CLIENTS; i++) n += S.cl[i].fd >= 0; return n; }

static void clip_request(const AgFrame *f) {
    static const char *const FORM[4] = { "", "text", "rich text", "picture" };
    uint32_t gen;
    uint8_t form;
    if (ag_clipreq_parse(f->data, f->len, &gen, &form)) { vlog("agent: a clipboard request that doesn't parse: dropped"); return; }
    long long now = now_ms();
    if (now - S.clip_rate_at >= 1000) { S.clip_rate_at = now; S.clip_rate_n = 0; S.clip_rate_said = 0; }
    if (++S.clip_rate_n > CLIP_REQ_RATE) {
        if (!S.clip_rate_said++) vlog("agent: the guest asks for the clipboard more than %d times a second: refused", CLIP_REQ_RATE);
        clip_answer(gen, form, AGD_REFUSED, NULL, 0);
        return;
    }
    if (!S.lag_clipreq || !clip_allows(1)) { vlog("agent: the guest asked for the clipboard (%s), which isn't shared toward it: refused", FORM[form]); clip_answer(gen, form, AGD_REFUSED, NULL, 0); return; }
    if (gen != S.clip_gen) { vlog("agent: the guest asked for an old clipboard (%s, offer %u, the last is %u): refused", FORM[form], gen, S.clip_gen); clip_answer(gen, form, AGD_STALE, NULL, 0); return; }
    if (!(S.clip_forms & (1u << (form - 1)))) { vlog("agent: the guest asked for the clipboard as %s, which wasn't offered: refused", FORM[form]); clip_answer(gen, form, AGD_NONE, NULL, 0); return; }
    if (!clip_clients()) { vlog("agent: the guest asked for the clipboard (%s) with no window open on the machine: refused", FORM[form]); clip_answer(gen, form, AGD_REFUSED, NULL, 0); return; }
    if (S.clip_wait[form].gen == gen && now - S.clip_wait[form].at < CLIP_WAIT_MS) return;     /* asked already: one answer */
    S.clip_wait[form].gen = gen;
    S.clip_wait[form].at = now;
    VmMsg m = vm_msg(VM_CLIP_REQ, 0, (int)gen, form, 0, 0, "");
    for (int i = 0; i < MAX_CLIENTS; i++) send_to(i, &m, -1);
}

/* lord-server's answer to a waiting request: 1 it was waiting (now answered), 0 not */
static int clip_waiting(uint32_t gen, int form) {
    if (form < AGF_TEXT || form > AGF_PNG || !gen || S.clip_wait[form].gen != gen) return 0;
    S.clip_wait[form].gen = 0;
    return now_ms() - S.clip_wait[form].at < CLIP_WAIT_MS;
}

/* ---- apps mode (7.2): the guest's windows, icons and apps, checked here and handed to lord-server ---- */
static void lag_icons_forget(int all) {
    for (int i = 0; i < LAG_WIN_MAX; i++) {
        if (!S.lag_icons[i].data) continue;
        int listed = 0;
        for (int k = 0; k < S.lag_win_n && !all && !listed; k++) listed = S.lag_win_ids[k] == S.lag_icons[i].id;
        if (!listed) { free(S.lag_icons[i].data); memset(&S.lag_icons[i], 0, sizeof S.lag_icons[i]); }
    }
}

static void lag_icon_keep(uint32_t id, const uint8_t *data, size_t n) {
    int k = -1;
    for (int i = 0; i < LAG_WIN_MAX && k < 0; i++) if (S.lag_icons[i].data && S.lag_icons[i].id == id) k = i;
    for (int i = 0; i < LAG_WIN_MAX && k < 0; i++) if (!S.lag_icons[i].data) k = i;
    uint8_t *c = k >= 0 ? malloc(n) : NULL;
    if (!c) return;
    memcpy(c, data, n);
    free(S.lag_icons[k].data);
    S.lag_icons[k].id = id; S.lag_icons[k].data = c; S.lag_icons[k].n = n;
}

/* VM_WINDATA as a sealed memfd to one client (ci) or all (-1) */
static void windata_send(int ci, const uint8_t *out, size_t n, int kind) {
    if (!n || n > VM_WINDATA_MAX) return;
    int fd = memfd_create("lordos-vm-windows", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd >= 0 && !write_all(fd, out, n) && !fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)) {
        VmMsg m = vm_msg(VM_WINDATA, 0, (int)n, kind, S.lag_applist_gen, 0, "");
        for (int i = 0; i < MAX_CLIENTS; i++) if (ci < 0 || i == ci) send_to(i, &m, fd);
    }
    if (fd >= 0) close(fd);
}

static void lag_windows_off(void) {
    S.lag_apps = 0;
    lag_icons_forget(1);
    free(S.lag_applist);
    S.lag_applist = NULL;
}

/* a guest frame of apps mode: parsed to its last byte (vmagent.c), written again (nothing the guest sent passes on as
 * it came), and given to every client as a sealed memfd (VM_WINDATA); the list of apps is kept, with a generation, for
 * VM_LAUNCH */
static void layout_set(const int L[6]);
static void layout_update(void);
static void lag_windata(const AgFrame *f) {
    static LagWinList wl;
    uint8_t *out = NULL;
    size_t n = 0;
    if (f->kind == LAG_WINDOWS) {
        if (lag_winlist_read(f->data, f->len, &wl)) { vlog("agent: a window list that doesn't parse: dropped"); return; }
        S.lag_win_n = wl.n;
        for (int i = 0; i < wl.n; i++) S.lag_win_ids[i] = wl.w[i].id;
        lag_icons_forget(0);
        out = malloc(LAG_WINDOWS_BYTES);
        n = out ? lag_windows_write(&wl, out, LAG_WINDOWS_BYTES) : 0;
        if (S.dev_qmp) {
            vlog("agent: windows #%u: %d", wl.serial, wl.n);
            for (int i = 0; i < wl.n; i++)
                vlog("agent:   %08x %d,%d %dx%d flags %u owner %08x [%.60s] %.200s", wl.w[i].id, wl.w[i].x, wl.w[i].y, wl.w[i].w, wl.w[i].h, wl.w[i].flags, wl.w[i].owner, wl.w[i].app, wl.w[i].title);
        }
    } else if (f->kind == LAG_WINICON) {
        uint32_t id; uint16_t w, h; const uint8_t *px;
        if (lag_winicon_parse(f->data, f->len, &id, &w, &h, &px)) { vlog("agent: a window icon that doesn't parse: dropped"); return; }
        out = malloc(8 + LAG_ICON_MAX * LAG_ICON_MAX * 4);
        n = out ? lag_icon_write(id, w, h, px, out, 8 + LAG_ICON_MAX * LAG_ICON_MAX * 4) : 0;
        if (n) lag_icon_keep(id, out, n);
    } else {
        LagAppList *al = malloc(sizeof *al);
        if (!al || lag_applist_read(f->data, f->len, al)) { free(al); vlog("agent: a program list that doesn't parse: dropped"); return; }
        free(S.lag_applist);
        S.lag_applist = al;
        S.lag_applist_gen++;
        out = malloc(LAG_APPLIST_BYTES);
        n = out ? lag_applist_write(al, out, LAG_APPLIST_BYTES) : 0;
        vlog("agent: the guest lists %d programs", al->n);
    }
    windata_send(-1, out, n, f->kind);
    free(out);
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
            char t[256];                        /* the hello: version and capability words (64 was too short for "open place") */
            size_t l = f.len < sizeof t - 1 ? f.len : sizeof t - 1;
            memcpy(t, f.data, l); t[l] = 0;
            int good = !strncmp(t, "lordos-guest ", 13) && ag_utf8_ok((const uint8_t *)t, l);
            if (good) {
                snprintf(S.lag_version, sizeof S.lag_version, "%.40s", t + 13);
                char *sp = strchr(S.lag_version, ' ');
                if (sp) *sp = 0;
                S.lag_clipx = strstr(t + 13, " clipx") != NULL;
                int lazy = (ag_hello_caps((const uint8_t *)t + 13, l - 13) & AG_HELLO_CLIPREQ) && S.lag_clipx;
                if (lazy != S.lag_clipreq) { S.clip_gen = 0; S.clip_forms = 0; memset(S.clip_wait, 0, sizeof S.clip_wait); }
                S.lag_clipreq = lazy;
                S.lag_display = strstr(t + 13, " display") != NULL;
                S.lag_windows = strstr(t + 13, " windows") != NULL;
                S.lag_open = (ag_hello_caps((const uint8_t *)t + 13, l - 13) & AG_HELLO_OPEN) != 0;
                S.lag_place = (ag_hello_caps((const uint8_t *)t + 13, l - 13) & AG_HELLO_PLACE) != 0;
                S.lag_pointer = (ag_hello_caps((const uint8_t *)t + 13, l - 13) & AG_HELLO_POINTER) != 0;
                S.lag_gvt = (ag_hello_caps((const uint8_t *)t + 13, l - 13) & AG_HELLO_GVT) != 0;
                S.lag_extra = (ag_hello_caps((const uint8_t *)t + 13, l - 13) & AG_HELLO_EXTRA) != 0;
                S.lag_extraoff = (ag_hello_caps((const uint8_t *)t + 13, l - 13) & AG_HELLO_EXTRAOFF) != 0;
                if (ag_hello_caps((const uint8_t *)t + 13, l - 13) & AG_HELLO_STOPPING) stop_announces = 1;
                /* "noclip": the helper runs but the guest has no clipboard tool (xclip, wl-clipboard); it says hello
                 * again with "clipboard" once one is installed */
                unsigned hc = ag_hello_caps((const uint8_t *)t + 13, l - 13);
                int clip = (hc & AG_HELLO_CLIPBOARD) != 0, notool = !clip && (hc & AG_HELLO_NOCLIP);
                if (!S.lag_hello) vlog("agent: lordos-guest %s said hello%s%s%s%s%s%s", S.lag_version, S.lag_clipx ? " (pictures and rich text too)" : "",
                                       S.lag_clipreq ? " (the host's clipboard on demand)" : "",
                                       S.lag_display ? " (sets the display's size)" : "", S.lag_windows ? " (apps mode)" : "",
                                       S.lag_open ? (S.lag_place ? " (opens files sent to it, takes files on its windows)" : " (opens files sent to it)") : "",
                                       notool ? " (no clipboard tool in the guest: xclip or wl-clipboard)" : "");
                else if (notool != !!(S.ag_caps & VAC_CLIP_NOTOOL))
                    vlog("agent: lordos-guest %s", notool ? "has no clipboard tool in the guest any more" : "found a clipboard tool in the guest: the clipboard works");
                S.lag_hello = 1;
                if (!S.lag_windows) lag_windows_off();
                S.ag_caps = (S.ag_caps & ~VAC_LAG) | (clip ? VAC_CLIPBOARD : 0) | (clip && S.lag_clipreq ? VAC_CLIP_LAZY : 0) |
                            (notool ? VAC_CLIP_NOTOOL : 0) | (S.lag_display ? VAC_DISPLAY : 0) | (S.lag_windows ? (int)LAG_CAP_WINDOWS : 0) |
                            (S.lag_open && S.ag_windows ? VAC_OPEN : 0) | (S.lag_place && S.ag_windows ? VAC_PLACE : 0) |
                            (S.lag_extra && S.extra_avail ? VAC_EXTRA : 0) | (S.lag_extra && S.lag_extraoff ? VAC_EXTRA_OFF : 0);
                if (!S.lag_open && !S.lag_place) open_all_end();
                static const char me[] = "lord-vm 2 clipboard clipx clipreq elevated";   /* elevated: LAGW_ELEVATED is understood */
                lag_send(AGK_HELLO, (const uint8_t *)me, sizeof me - 1);
                if (S.lag_display && S.disp_w) { uint8_t d[8]; ag_display_write(d, S.disp_w, S.disp_h); lag_send(AGK_DISPLAY, d, 8); }
                ag_broadcast();
                ptr_begin();                    /* the tools say "pointer": the display driver's pointer, once per run */
                gvt_begin();                    /* the tools say "gvt": Intel's driver for the vGPU, once per run */
            }
        } else if (f.kind == AGK_STOPPING) {
            /* the guest's last shutdown step (its tools' systemd-shutdown hook, no hello: a fresh open of the port):
             * it powers off or restarts now. Kept only to name the end of a run whose QEMU then aborts on the
             * guest's virtio-gpu reset (QEMU issue 4918); the echo lets the guest go on */
            int mode;
            if (!ag_stopping_parse(f.data, f.len, &mode)) {
                if (stop_notice != mode) vlog("agent: the guest says it %s now", mode == AG_STOP_REBOOT ? "restarts" : "powers off");
                stop_notice = mode;
                uint8_t b = (uint8_t)mode;
                lag_send(AGK_STOPPING, &b, 1);
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
        } else if (f.kind == AGK_CLIPREQ && S.lag_hello) {
            clip_request(&f);
        } else if (f.kind == AGK_DISPLAY && S.lag_hello && S.lag_display) {
            uint32_t w, h;
            if (ag_display_parse(f.data, f.len, &w, &h)) vlog("agent: the guest's display size: malformed");
            else vlog("agent: the guest's display is %ux%u%s", w, h, w == S.disp_w && h == S.disp_h ? "" : " (not the size asked for)");
        } else if (f.kind == LAG_WINREFUSED && S.lag_hello && S.lag_windows) {
            /* an apps mode action the guest couldn't do (LordOS shows why from LAGW_ELEVATED in the list): logged */
            static const char *const act[] = { "", "focus", "close", "minimize", "restore", "move" };
            uint32_t id;
            uint8_t a, why;
            if (lag_winrefused_parse(f.data, f.len, &id, &a, &why)) vlog("agent: a refused window action: malformed");
            else {
                vlog("agent: the guest couldn't %s its window %u: %s", act[a], id,
                     why == LAGR_ELEVATED ? "it runs as administrator (close it from inside the machine)" : "the system refused");
                VmMsg m = vm_msg(VM_WINREFUSED, 0, (int)id, a, why, 0, "");
                for (int i = 0; i < MAX_CLIENTS; i++) send_to(i, &m, -1);
            }
        } else if (f.kind == AGK_DISPLAYS && S.lag_hello) {
            /* the guest's displays (lordos-guest-win w7 and later), for this log only: logged when they change */
            AgDisplay d[AG_DISPLAYS_MAX];
            char t[sizeof S.lag_disps];
            int shown, n = ag_displays_parse(f.data, f.len, d, &shown);
            if (n < 0) vlog("agent: the guest's displays: malformed");
            else {
                ag_displays_text(d, n, shown, t, sizeof t);
                if (strcmp(t, S.lag_disps)) { memcpy(S.lag_disps, t, sizeof t); vlog("agent: guest displays: %s", t); }
                memcpy(S.disp_list, d, sizeof S.disp_list);
                S.disp_n = n;
                S.disp_shown = shown;
                layout_update();
                /* a display the tools can turn off (Machines offers it) */
                S.extra_avail = ag_displays_extra(d, n, shown);
                int caps = (S.ag_caps & ~VAC_EXTRA) | (S.lag_extra && S.extra_avail ? VAC_EXTRA : 0);
                if (caps != S.ag_caps) { S.ag_caps = caps; ag_broadcast(); }
            }
        } else if (f.kind == AGK_TABLET && S.lag_hello) {
            /* how the guest maps the absolute tablet (tools w10, from the tablet's own raw input) */
            int mode;
            if (ag_tablet_parse(f.data, f.len, &mode)) vlog("agent: the guest's tablet mapping: malformed");
            else if (mode != S.tablet) {
                S.tablet = mode;
                vlog("agent: the guest maps the tablet over %s", mode == AG_TABLET_DESKTOP ? "its whole desktop (every display)" : "its primary display only");
                layout_update();
            }
        } else if ((f.kind == LAG_WINDOWS || f.kind == LAG_WINICON || f.kind == LAG_APPLIST) && S.lag_hello && S.lag_windows) {
            lag_windata(&f);
        } else if ((f.kind == AGK_OPENED && S.lag_hello && S.lag_open) || (f.kind == AGK_PLACED && S.lag_hello && S.lag_place)) {
            open_answer(&f);
        }
        /* other kinds: skipped, so either side can grow */
        memmove(S.lag_buf, S.lag_buf + k, S.lag_n - (size_t)k);
        S.lag_n -= (size_t)k;
    }
}

/* the guest's layout for the absolute tablet (VM_GUEST_LAYOUT), to every client; all 0s: none */
static void layout_set(const int L[6]) {
    memcpy(S.layout, L, sizeof S.layout);
    if (L[4]) vlog("agent: the tablet points at the shown display: %dx%d at %d,%d of a %dx%d desktop", L[2], L[3], L[0], L[1], L[4], L[5]);
    else vlog("agent: the tablet covers the picture (%s)", S.disp_n > 1 ? "the shown display is the one the guest maps it over" : "one display");
    VmMsg m = vm_msg(VM_GUEST_LAYOUT, 0, L[0], L[1], L[2] << 16 | L[3], L[4] << 16 | L[5], "");
    for (int i = 0; i < MAX_CLIENTS; i++) send_to(i, &m, -1);
}

/* The tablet's layout from the guest's displays and how it maps the tablet (ag_tablet_layout). A Windows guest maps a
 * HID tablet over its primary display only (mouhid sets no MOUSE_VIRTUAL_DESKTOP; tools w10 read it from the tablet's raw
 * input and say so, AGK_TABLET): with the shown display primary, the picture is all of it, as with one display. Until
 * the tools say, the primary is assumed (2026-11-22, the maintainer's guest: with the shown display primary at 0,0 and a
 * second display beside it, the whole-desktop mapping of before put clicks at about half their x) */
static void layout_update(void) {
    int L[6];
    int r = ag_tablet_layout(S.disp_list, S.disp_n, S.disp_shown, S.tablet, L);
    if (r == 2 && !S.tablet_far) vlog("agent: the guest's primary display isn't the shown one: the tablet can't reach the picture until the tools make it primary again");
    S.tablet_far = r == 2;
    if (memcmp(L, S.layout, sizeof L)) layout_set(L);
}

/* the agent's turn of the loop: sync, ping, the journal, the shutdown fallback */
static void ag_tick(long long now) {
    S.ag_next = now + 1000;
    if (S.acpi_at && now >= S.acpi_at) {
        S.acpi_at = 0;
        if (S.state == VS_STOPPING) { vlog("agent: the guest is still running: pressing the power button too"); qmp_do("system_powerdown", NULL, P_IGNORE, -1, 0, -1, -1); }
    }
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
    if (fd >= 0 && m->type != VM_CDROM && m->type != VM_CLIP && m->type != VM_FILE && (m->type != VM_QMP || !S.dev_qmp) &&
        !(m->type == VM_AGENT && m->a == VAG_TRANSFER)) { close(fd); fd = -1; }
    int alive = S.qemu > 0 && !S.qemu_done && S.qmp_up;
    switch (m->type) {
    case VM_STATUS: {
        VmMsg i = info_msg(), s = state_msg();
        i.seq = s.seq = m->seq;
        send_to(ci, &i, -1);
        send_to(ci, &s, -1);
        if (S.notice) { VmMsg n = vm_msg(VM_NOTICE, m->seq, S.notice, S.notice_code, 0, 0, S.notice_text); send_to(ci, &n, -1); }
        if (S.m.agent) { char t[VM_TEXT_MAX]; ag_state_text(t, sizeof t); VmMsg a = vm_msg(VM_AGENT_STATE, m->seq, S.ag_state, S.ag_caps, 0, 0, t); send_to(ci, &a, -1); }
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
        if (S.gl) { reply(ci, m->seq, VME_WRONG_STATE, "a machine with 3D graphics can't be suspended (virgl can't be migrated): shut it down"); return; }
        if (S.vgpu) { reply(ci, m->seq, VME_WRONG_STATE, "a machine with a graphics card can't be suspended (a vGPU can't be saved): shut it down"); return; }
        begin_suspend(ci, m->seq);
        return;
    case VM_SNAPSHOT:
        if (!alive || (S.state != VS_RUNNING && S.state != VS_PAUSED)) { reply(ci, m->seq, VME_WRONG_STATE, "not running"); return; }
        if (S.snap_kind || S.suspending) { reply(ci, m->seq, VME_BUSY, "busy"); return; }
        /* snapshots of a running machine hold its memory: QEMU can't save virgl's state */
        if ((S.gl || S.vgpu) && m->a != VSN_DELETE) { reply(ci, m->seq, VME_WRONG_STATE, "a running machine with 3D graphics can't take or revert a snapshot with its memory: shut it down first"); return; }
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
        if (fd >= 0 && m->a != VAG_TRANSFER) { close(fd); fd = -1; }
        if (!S.m.agent) { if (fd >= 0) close(fd); reply(ci, m->seq, VME_DENIED, "the guest agent is off for this machine"); return; }
        if (m->a == VAG_EXTRA_OFF || m->a == VAG_EXTRA_ON) {
            /* the guest's extra display, through its tools (not qemu-guest-agent): only when they can and it's there */
            int off = m->a == VAG_EXTRA_OFF;
            if (!S.lag_hello || !S.lag_extra) { reply(ci, m->seq, VME_WRONG_STATE, "the LordOS tools in the guest can't turn a display off (w9 and later)"); return; }
            if (off ? !S.extra_avail : !S.lag_extraoff) { reply(ci, m->seq, VME_WRONG_STATE, off ? "the guest has no extra display" : "no display was turned off"); return; }
            uint8_t b = (uint8_t)off;
            lag_send(AGK_EXTRA, &b, 1);
            vlog("agent: the guest's tools are asked to turn %s its extra display", off ? "off" : "back on");
            reply(ci, m->seq, VME_OK, off ? "asked to turn it off" : "asked to turn it back on");
            return;
        }
        if (!S.qga_up) { if (fd >= 0) close(fd); reply(ci, m->seq, VME_WRONG_STATE, "qemu-guest-agent isn't answering in the guest"); return; }
        if (m->a == VAG_TRANSFER) {
            /* the transfer folder (6.7.1): a Linux guest mounts it again; a Windows guest gets WinFsp, whose installer
             * lord-server opened in the machines' image store: its hash is checked here (text), again in the guest with
             * its signature */
            const char *no = !S.xfer_on ? "the transfer folder is off at this start (it changes at the next start)"
                           : !(S.ag_caps & VAC_EXEC) ? "the guest agent can't run commands" : NULL;
            if (!no && !S.ag_windows) { if (fd >= 0) close(fd); xfer_begin(); reply(ci, m->seq, VME_OK, "looked at again"); return; }
            struct stat st;
            char sha[65];
            if (!no && (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (32 << 20))) no = "WinFsp's installer wasn't given";
            if (!no && (strlen(m->text) != 64 || strspn(m->text, "0123456789abcdef") != 64)) no = "WinFsp's installer's hash isn't valid";
            if (!no && (sha256_fd(fd, sha) || strcmp(sha, m->text))) no = "WinFsp's installer isn't the one the catalog names";
            if (!no && (!(S.ag_caps & VAC_FILES))) no = "the guest agent can't write files";
            if (!no && S.fxq_n >= FX_QUEUE) no = "too many files are waiting";
            if (no) { if (fd >= 0) close(fd); reply(ci, m->seq, VME_WRONG_STATE, no); return; }
            FxReq *q = &S.fxq[S.fxq_n++];
            memset(q, 0, sizeof *q);
            q->fd = fd; q->client = ci; q->seq = m->seq; q->kind = FXK_WINFSP;
            snprintf(q->name, sizeof q->name, "winfsp.msi");
            memcpy(q->sha, sha, sizeof q->sha);
            vlog("transfer: installing WinFsp in the guest");
            fx_next();
            return;
        }
        if (m->a == VAG_SHUTDOWN) { qga_do("guest-shutdown", "{\"mode\":\"powerdown\"}", -1); reply(ci, m->seq, VME_OK, ""); return; }
        if (m->a == VAG_TIMESYNC) { ag_timesync(); reply(ci, m->seq, VME_OK, ""); return; }
        /* Machines' own update of older LordOS tools (docs/VIRTUALIZATION.md "Keeping the LordOS tools up to date"):
         * the same install as the button, and each attempt in this log */
        if (m->b == 1) vlog("agent: Machines updates the LordOS tools in the guest by itself (they said %s)", S.lag_hello && S.lag_version[0] ? S.lag_version : "no version");
        if (S.ag_windows) { const char *why = NULL; if (wi_begin(ci, m->seq, &why)) reply(ci, m->seq, VME_WRONG_STATE, why); return; }
        if (S.inst_busy || S.helper_handle) { reply(ci, m->seq, VME_BUSY, "busy"); return; }      /* one install at a time */
        if (!S.helper || !(S.ag_caps & VAC_FILES) || !(S.ag_caps & VAC_EXEC)) { reply(ci, m->seq, VME_WRONG_STATE, S.helper ? "the guest agent can't write files or run commands" : "lordos-guest isn't available for this machine's architecture"); return; }
        S.install_client = ci; S.install_seq = m->seq;
        S.helper_handle = -1;
        S.inst_busy = 1;
        if (qga_do("guest-file-open", "{\"path\":\"/usr/local/bin/.lordos-guest.new\",\"mode\":\"w\"}", AQ_FOPEN)) inst_reply(VME_BUSY, "busy");
        return;
    case VM_CLIP: {
        if (fd < 0) { reply(ci, m->seq, VME_BAD_REQUEST, "no text"); return; }
        if (!clip_allows(1) || !S.lag_hello) { close(fd); reply(ci, m->seq, VME_DENIED, "the clipboard isn't shared toward this machine"); return; }
        static uint8_t buf[VM_CLIP_MAX];
        struct stat st;
        ssize_t n = !fstat(fd, &st) && st.st_size == m->a ? pread(fd, buf, (size_t)m->a, 0) : -1;
        close(fd);
        if (n != m->a) { reply(ci, m->seq, VME_BAD_REQUEST, "unreadable"); return; }
        if (m->c) {
            /* the answer to the guest's request: that one form, checked and cleaned again, as AGK_CLIPDATA */
            uint8_t *clean = NULL;
            size_t cn = 0;
            char what[64];
            AgClip c;
            if (!clip_waiting((uint32_t)m->c, m->d)) { reply(ci, m->seq, VME_WRONG_STATE, "no such request waits"); return; }
            if (clip_clean(buf, (size_t)n, &clean, &cn, what, sizeof what) || ag_clipx_parse(clean, cn, &c)) {
                free(clean);
                clip_answer((uint32_t)m->c, (uint8_t)m->d, AGD_NONE, NULL, 0);
                reply(ci, m->seq, VME_BAD_REQUEST, what);
                return;
            }
            const uint8_t *d = m->d == AGF_TEXT ? c.text : m->d == AGF_HTML ? c.html : c.png;
            uint32_t dn = m->d == AGF_TEXT ? c.text_n : m->d == AGF_HTML ? c.html_n : c.png_n;
            clip_answer((uint32_t)m->c, (uint8_t)m->d, d ? AGD_OK : AGD_NONE, d, d ? dn : 0);
            if (S.dev_qmp) vlog("agent: the clipboard for the guest's paste: %s", what);
            free(clean);
            reply(ci, m->seq, d ? VME_OK : VME_BAD_REQUEST, what);
            return;
        }
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
    case VM_CLIP_OFFER: {
        if (!clip_allows(1) || !S.lag_hello || !S.lag_clipreq) { reply(ci, m->seq, VME_DENIED, "the guest's helper doesn't take the clipboard on demand, or it isn't shared toward this machine"); return; }
        uint8_t o[6];
        S.clip_gen = (uint32_t)m->a;
        S.clip_forms = (uint8_t)m->b;
        memset(S.clip_wait, 0, sizeof S.clip_wait);
        ag_offer_write(o, S.clip_gen, S.clip_forms, (uint8_t)m->c);
        lag_send(AGK_CLIPOFFER, o, 6);
        if (S.dev_qmp) vlog("agent: clipboard offer %u (forms %d%s)", S.clip_gen, m->b, m->c ? ", ask" : "");
        reply(ci, m->seq, VME_OK, "");
        return;
    }
    case VM_CLIP_DENY: {
        static const char *const WHY[VCD_LAST + 1] = { "", "the machine wasn't in use", "the clipboard changed since", "that form isn't there now",
                                                     "the person didn't allow it" };
        if (!m->a) vlog("agent: the guest changed its clipboard while the machine wasn't in use: ignored");
        else if (clip_waiting((uint32_t)m->a, m->b)) {
            vlog("agent: the guest asked for the clipboard: refused (%s)", WHY[m->c]);
            clip_answer((uint32_t)m->a, (uint8_t)m->b, m->c == VCD_STALE ? AGD_STALE : m->c == VCD_NONE ? AGD_NONE : AGD_REFUSED, NULL, 0);
        }
        reply(ci, m->seq, VME_OK, "");
        return;
    }
    case VM_APPS: {
        /* apps mode (7.2): only toward a helper that said it lists its windows */
        if (!S.lag_hello || !S.lag_windows) { reply(ci, m->seq, VME_DENIED, "the guest's helper doesn't list its windows"); return; }
        uint8_t on = (uint8_t)m->a;
        S.lag_apps = m->a;
        lag_send(LAG_APPS, &on, 1);
        vlog("agent: apps mode %s", m->a ? "on" : "off");
        reply(ci, m->seq, VME_OK, "");
        /* the icons the guest already sent (it sends each window's once): a re-attached lord-server has none */
        if (on) for (int i = 0; i < LAG_WIN_MAX; i++) if (S.lag_icons[i].data) windata_send(ci, S.lag_icons[i].data, S.lag_icons[i].n, LAG_WINICON);
        return;
    }
    case VM_WINACT: {
        if (!S.lag_hello || !S.lag_windows) { reply(ci, m->seq, VME_DENIED, "the guest's helper doesn't list its windows"); return; }
        LagAct act = { .id = (uint32_t)m->a, .action = m->b };
        if (m->b == LAGA_MOVE) {
            char tail[4];
            if (sscanf(m->text, "%d %d %d %d%3s", &act.x, &act.y, &act.w, &act.h, tail) != 4) { reply(ci, m->seq, VME_BAD_REQUEST, "a move needs x y w h"); return; }
        }
        uint8_t b[32];
        size_t n = lag_act_write(&act, b, sizeof b);
        if (!n) { reply(ci, m->seq, VME_BAD_REQUEST, "bad window action"); return; }
        lag_send(LAG_WINACT, b, (uint32_t)n);
        reply(ci, m->seq, VME_OK, "");
        return;
    }
    case VM_LAUNCH: {
        /* an app by its place in the list the guest gave, from lord-vm's own copy of it: the token goes back as the
         * guest wrote it, and only while that list is still the latest */
        if (!S.lag_hello || !S.lag_windows || !S.lag_applist) { reply(ci, m->seq, VME_DENIED, "the guest listed no apps"); return; }
        if (m->b != S.lag_applist_gen || m->a >= S.lag_applist->n) { reply(ci, m->seq, VME_DENIED, "not one of the guest's apps (its list changed)"); return; }
        uint8_t *b = malloc(LAG_TOKEN_MAX + 2);
        size_t n = b ? lag_launch_write(S.lag_applist->a[m->a].token, b, LAG_TOKEN_MAX + 2) : 0;
        if (!n) { free(b); reply(ci, m->seq, VME_BAD_REQUEST, "bad token"); return; }
        lag_send(LAG_LAUNCH, b, (uint32_t)n);
        free(b);
        vlog("agent: launching the guest's %.100s", S.lag_applist->a[m->a].name);
        reply(ci, m->seq, VME_OK, "");
        return;
    }
    case VM_SOUND: {
        /* machine.json's sound choices: nothing changes in QEMU (lord-server plays and records itself); lord-vm holds the
         * lock, so it writes the file */
        VmMachine nm = S.m;
        nm.sound = m->a; nm.volume = m->b; nm.microphone = m->c;
        if (m->d == 1 && vm_sound_devs_parse(m->text, nm.sound_out, sizeof nm.sound_out, nm.sound_in, sizeof nm.sound_in)) {
            reply(ci, m->seq, VME_BAD_REQUEST, "not a sound device's name");
            return;
        }
        nm.schema = VM_SCHEMA;
        vm_now_iso(nm.updated);
        if (vm_machine_check(&nm, NULL, 0) || vm_machine_save(S.dir, &nm)) { reply(ci, m->seq, VME_INTERNAL, "can't write machine.json"); return; }
        S.m = nm;
        vlog("sound: %s, volume %d, microphone %s, output %s, input %s", nm.sound ? "on" : "muted", nm.volume, nm.microphone ? "allowed" : "off",
             nm.sound_out[0] ? nm.sound_out : "default", nm.sound_in[0] ? nm.sound_in : "default");
        reply(ci, m->seq, VME_OK, "");
        return;
    }
    case VM_FILE: {
        struct stat st;
        char nm[AG_NAME_MAX + 1];
        const char *no = fd < 0 ? "no file" : fstat(fd, &st) || !S_ISREG(st.st_mode) || (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDONLY ? "not a file opened for reading"
                       : ag_drop_name(m->text, nm, sizeof nm) ? "the file's name can't be used" : NULL;
        int code = VME_BAD_REQUEST;
        if (!no) {
            code = VME_WRONG_STATE;
            if (!S.m.agent) { code = VME_DENIED; no = "the guest agent is off for this machine"; }
            else if (!alive || S.state != VS_RUNNING) no = S.state == VS_PAUSED ? "the machine is paused" : "not running";
            else if (!S.qga_up) no = "qemu-guest-agent isn't answering in the guest";
            else if (!(S.ag_caps & VAC_FILES) || !(S.ag_caps & VAC_EXEC)) no = "the guest agent can't write files or run commands";
            else if (S.fxq_n >= FX_QUEUE) { code = VME_BUSY; no = "too many files are waiting"; }
            /* "Open with" (7.3): only a guest whose tools open files (they said "open"; on Windows the copy step records
             * the file's place for them) */
            else if (m->a == 1 && (!S.ag_windows || !S.lag_hello || !S.lag_open)) no = "the guest tools can't open files in this machine";
            else if (m->a == 2 && (!S.ag_windows || !S.lag_hello || !S.lag_place)) no = "the guest tools can't take files on a window in this machine";
            else if (m->a == 2) {
                int known = 0;
                for (int i = 0; i < S.lag_win_n; i++) known |= S.lag_win_ids[i] == (uint32_t)m->b;
                if (!known) no = "that window isn't in the guest's list";
            }
        }
        if (no) { if (fd >= 0) close(fd); reply(ci, m->seq, code, no); return; }
        FxReq *q = &S.fxq[S.fxq_n++];
        memset(q, 0, sizeof *q);
        q->fd = fd; q->client = ci; q->seq = m->seq; q->kind = FXK_DROP; q->open = m->a == 1; q->place = m->a == 2;
        if (q->place) {
            q->pwin = (uint32_t)m->b; q->px = m->c & 0xffff; q->py = m->c >> 16;
            q->pbatch = (uint32_t)(m->d >> 16); q->pindex = (uint8_t)(m->d >> 8); q->pcount = (uint8_t)m->d;
        }
        memcpy(q->name, nm, sizeof q->name);
        fx_next();
        return;
    }
    case VM_CAPTURE:
        if (S.m.display != VM_DISPLAY_WINDOW) { reply(ci, m->seq, VME_NO_DISPLAY, "this machine has no display"); return; }
        if (!alive || (S.state != VS_RUNNING && S.state != VS_PAUSED)) { reply(ci, m->seq, VME_WRONG_STATE, "not running"); return; }
        S.cap_want = m->a;
        S.mice_said = 0;
        vlog("capture: the mouse %s", m->a ? "is captured: the guest gets a relative mouse" : "is free: the guest gets the tablet again");
        if (qmp_do("query-mice", NULL, P_MICE, ci, m->seq, -1, -1)) { reply(ci, m->seq, VME_BUSY, "busy"); return; }
        S.mice_q = 1;
        S.mice_at = now_ms() + 1000;
        return;
    case VM_KEEP:
        S.keep_running = m->a ? 1 : 0;
        vlog("keep running without lord-server: %s", S.keep_running ? "yes" : "no");
        reply(ci, m->seq, VME_OK, "");
        return;
    case VM_DISPLAY_SIZE:
        /* the window's size (lord-server sends it with VD_RESIZE): a guest that sets its own size gets it through its
         * helper; others follow QEMU's ui_info (lord-vmdisplay) */
        S.disp_w = (uint32_t)m->a; S.disp_h = (uint32_t)m->b;
        if (S.lag_hello && S.lag_display) { uint8_t d[8]; ag_display_write(d, S.disp_w, S.disp_h); lag_send(AGK_DISPLAY, d, 8); }
        reply(ci, m->seq, VME_OK, "");
        return;
    case VM_QMP: {
        if (!S.dev_qmp) { reply(ci, m->seq, VME_DENIED, "lord-vm wasn't started with --dev-qmp"); return; }
        if (!strncmp(m->text, "lag-", 4)) {
            /* development: apps mode's requests to the guest, as lord-server will send them (lordvm-dev apps, winact, launch) */
            uint8_t b[600];
            unsigned id, act;
            int on, x, y, w, h, k;
            if (sscanf(m->text, "lag-apps %d", &on) == 1) { b[0] = on ? 1 : 0; lag_send(LAG_APPS, b, 1); }
            else if ((k = sscanf(m->text, "lag-winact %x %u %d %d %d %d", &id, &act, &x, &y, &w, &h)) >= 2) {
                b[0] = (uint8_t)id; b[1] = (uint8_t)(id >> 8); b[2] = (uint8_t)(id >> 16); b[3] = (uint8_t)(id >> 24); b[4] = (uint8_t)act;
                int n = 5;
                if (k == 6) { int32_t v[4] = { x, y, w, h }; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) b[n++] = (uint8_t)((uint32_t)v[i] >> (8 * j)); }
                lag_send(LAG_WINACT, b, (uint32_t)n);
            } else if (!strncmp(m->text, "lag-launch ", 11)) {
                size_t l = strlen(m->text + 11);
                b[0] = (uint8_t)l; b[1] = (uint8_t)(l >> 8);
                memcpy(b + 2, m->text + 11, l);
                lag_send(LAG_LAUNCH, b, (uint32_t)l + 2);
            } else { reply(ci, m->seq, VME_BAD_REQUEST, "lag-apps 0|1, lag-winact ID N [X Y W H], lag-launch TOKEN"); return; }
            reply(ci, m->seq, S.lag_windows ? VME_OK : VME_WRONG_STATE, S.lag_windows ? "sent" : "sent, but the guest's helper didn't say \"windows\"");
            return;
        }
        if (!S.qmp_up) { if (fd >= 0) close(fd); reply(ci, m->seq, VME_WRONG_STATE, "not running"); return; }
        /* a command longer than the message's text comes in a descriptor (lordvm-dev qmpf) */
        static char big[1 << 20];
        const char *cmd = m->text;
        if (fd >= 0) {
            ssize_t bn = pread(fd, big, sizeof big - 1, 0);
            close(fd);
            if (bn <= 0) { reply(ci, m->seq, VME_BAD_REQUEST, "unreadable"); return; }
            big[bn] = 0;
            cmd = big;
        }
        if (strstr(cmd, "\"execute\":\"guest-")) {
            /* to qemu-guest-agent instead (the tests check what reached the guest) */
            if (!S.qga_up || S.agq_n >= 32 || qmp_raw(&S.qga, cmd)) { reply(ci, m->seq, VME_WRONG_STATE, "the guest agent isn't answering"); return; }
            S.agq[(S.agq_head + S.agq_n) % 32] = AQ_DEVRAW; S.agq_n++;
            S.dev_client = ci; S.dev_seq = m->seq;
            vlog("dev-qmp to the guest agent from client %d: %.120s", ci, cmd);
            return;
        }
        if (S.pq_n >= MAX_PENDING || qmp_raw(&S.qmp, cmd)) { reply(ci, m->seq, VME_BAD_REQUEST, "can't send"); return; }
        pq_push(P_RAW, ci, m->seq, -1);
        vlog("dev-qmp from client %d: %.120s", ci, cmd);
        return;
    }
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
        if (S.unattended && why && !strcmp(why, "guest-reset")) { S.install_done = 1; vlog("unattended: the installer restarted the machine: the install is done"); }
        /* with a vGPU every reset is an exit (-action reboot=shutdown): the guest's own ("guest-reset") and Restart's
         * ("host-qmp-system-reset"); Machines starts the machine again */
        else if (S.vgpu && why && strstr(why, "reset")) { S.vgpu_reset = 1; vlog("graphics card: the machine restarted (%s): QEMU ends and Machines starts it again", why); }
        if (S.state != VS_STOPPING) set_state(VS_STOPPING, guest ? VR_GUEST_SHUTDOWN : VR_POWEROFF, 0, why ? why : "");
    } else if (!strcmp(ev, "RESET")) {
        stop_notice = 0;                /* the guest's restart happened inside QEMU: what it said is done with */
        if (S.state == VS_RUNNING || S.state == VS_PAUSED) set_state(S.state, VR_GUEST_RESET, 0, "");
    } else if (!strcmp(ev, "GUEST_PANICKED")) { S.panic = 1; vlog("qemu: the guest panicked"); }
    else if (!strcmp(ev, "POWERDOWN")) vlog("qemu: power button pressed");
    else if (!strcmp(ev, "VSERPORT_CHANGE")) {
        const char *id = data ? json_str(data, "id") : NULL;
        int open = data ? json_bool(data, "open", 0) : 0;
        if (id && !strcmp(id, "qga-port")) { S.qga_open = open; if (open) qga_sync(); else ag_lost("the guest closed qemu-guest-agent's port"); }
        else if (id && !strcmp(id, "lag-port")) {
            vlog("agent: the guest %s org.lordos.agent", open ? "opened" : "closed");
            if (!open) { if (S.layout[4]) { static const int none[6]; layout_set(none); } S.lag_hello = 0; S.lag_disps[0] = 0; S.lag_clipx = S.lag_clipreq = S.lag_display = S.lag_windows = S.lag_open = S.lag_place = S.lag_pointer = S.lag_gvt = S.lag_extra = S.lag_extraoff = S.extra_avail = 0; S.disp_n = 0; S.tablet = 0; S.tablet_far = 0; S.lag_win_n = 0; S.clip_gen = 0; S.ag_caps &= ~VAC_LAG; S.lag_n = 0; lag_windows_off(); open_all_end(); ag_broadcast(); }
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

/* ---- "Capture mouse" (VM_CAPTURE, docs/VIRTUALIZATION.md 6.25) ----
 * QEMU sends the pointer to its active mouse: the one whose guest driver started last. Captured, the relative mouse
 * lord-vm adds beside the tablet is made the active one (HMP mouse_set: QMP has no command for it); otherwise the tablet
 * is, whenever that mouse took over by itself (its driver started after the tablet's, at each boot). Checked every
 * second while the machine runs */
static int mice_watched(void) {
    return S.qmp_up && S.state == VS_RUNNING && S.m.display == VM_DISPLAY_WINDOW && (S.cap_want || S.cap_rel_added);
}
static void mice_answer(const JVal *list, int client, uint32_t seq, const char *desc) {
    int current = 0, other = 0;
    int idx = list ? vm_mouse_pick(list, S.cap_want, &current, &other) : -1;
    if (!list) { if (client >= 0) reply(client, seq, VME_QMP, desc); vlog("capture: query-mice: %.150s", desc); return; }
    /* not captured, the tablet is set back only when lord-vm's own mouse took over: whatever QEMU chose before the
     * tablet's driver started (the PS/2 mouse during a BIOS) stays as it always was */
    if (!S.cap_want && client < 0 && !other) idx = -1, current = 1;
    if (idx < 0 && !current) {
        if (S.mice_said != 1) vlog("capture: the guest has no %s mouse: %s", S.cap_want ? "relative" : "absolute",
                                   S.cap_want ? "lord-vmdisplay moves the tablet by the motion instead" : "the pointer stays as it is");
        S.mice_said = 1;
        if (client >= 0) reply(client, seq, VME_QMP, S.cap_want ? "no relative mouse" : "no tablet");
        return;
    }
    if (current) { if (client >= 0) reply(client, seq, VME_OK, ""); return; }
    char a[64];
    snprintf(a, sizeof a, "{\"command-line\":\"mouse_set %d\"}", idx);
    if (S.mice_said != 2 + S.cap_want) {
        /* what QEMU listed, once per change, for the log (its names are QEMU's own: printable, cut short) */
        char seen[400] = "";
        size_t k = 0;
        int n = 0;
        for (const JVal *e = list->kid; e && n < 8 && k < sizeof seen - 1; e = e->next, n++) {
            const char *nm = json_str(e, "name");
            k += (size_t)snprintf(seen + k, sizeof seen - k, "%s%d %.40s%s%s", n ? ", " : "", (int)json_num(e, "index", -1), nm ? nm : "?",
                                  json_bool(e, "absolute", 0) ? " (absolute)" : "", json_bool(e, "current", 0) ? " (active)" : "");
            if (k >= sizeof seen) k = sizeof seen - 1;
        }
        for (char *c = seen; *c; c++) if ((unsigned char)*c < 0x20 || *c == 0x7f) *c = '?';
        vlog("capture: mouse %d made the active one (%s); QEMU has: %s", idx, S.cap_want ? "relative" : "the tablet", seen);
    }
    S.mice_said = 2 + S.cap_want;
    if (qmp_do("human-monitor-command", a, P_MOUSE_SET, client, seq, -1, -1) && client >= 0) reply(client, seq, VME_BUSY, "busy");
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
    case P_MICE: S.mice_q = 0; mice_answer(ok ? json_get(root, "return") : NULL, p.client, p.seq, desc); break;
    case P_MOUSE_SET:
        /* human-monitor-command answers HMP's own words as "return" (empty when mouse_set took it) */
        if (ok) { const char *said = json_str(root, "return"); if (said && *said) { ok = 0; desc = said; } }
        if (!ok) vlog("capture: mouse_set: %.150s", desc);
        if (p.client >= 0) reply(p.client, p.seq, ok ? VME_OK : VME_QMP, desc);
        break;
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

static void unattended_done(void);
static void pump_fd(int *fd, const char *who);
static void qemu_exited(int status) {
    /* what QEMU wrote just before it ended (an assertion's line names the crash): read now, while it's there */
    for (int k = 0; k < 20 && S.qemu_err >= 0; k++) {
        struct pollfd pf = { .fd = S.qemu_err, .events = POLLIN };
        if (poll(&pf, 1, 0) <= 0) break;
        pump_fd(&S.qemu_err, "qemu");
    }
    /* and what the guest's tools said last (AGK_STOPPING: QEMU may have passed it on just before it ended) */
    for (int k = 0; k < 20 && S.lag_fd >= 0; k++) {
        struct pollfd pf = { .fd = S.lag_fd, .events = POLLIN };
        if (poll(&pf, 1, 0) <= 0 || !(pf.revents & POLLIN)) break;
        drain_lag();
    }
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
        else if (S.install_done) why = VR_INSTALLED;
        else if (S.vgpu_reset) why = VR_GUEST_RESET;     /* a vGPU machine's restart: Machines starts it again */
        else if (S.host_shutdown && S.guest_shutdown) why = VR_HOST_SHUTDOWN;
        else if (S.guest_shutdown) why = VR_GUEST_SHUTDOWN;
    } else if (WIFSIGNALED(status)) {
        code = WTERMSIG(status);
        if (S.killed && (S.forced || S.poweroff)) why = S.forced ? VR_FORCED : VR_POWEROFF;
        else if (code == SIGABRT && qemu_3d_abort_seen) {
            /* QEMU issue 4918: the guest reset its virtio-gpu-gl (a Linux guest's shutdown or restart, Debian 6.12.117+)
             * and QEMU's D-Bus display asserted. A shutdown or restart is recorded as one (vmlog_3d_abort_end); else
             * Machines offers 2D graphics. code stays SIGABRT: the next start checks the disks either way */
            long long ran = run_t0 ? now_ms() - run_t0 : 0;
            int end = vmlog_3d_abort_end(S.host_shutdown, stop_notice, stop_announces, ran);
            static const char *const said[] = { "nothing", "it powers off", "it restarts" };
            vlog("qemu: the 3D abort (QEMU issue 4918) after %lld s running; asked to shut down: %s; the guest's tools said %s%s",
                 ran / 1000, S.host_shutdown ? "yes" : "no", said[stop_notice >= 0 && stop_notice <= 2 ? stop_notice : 0],
                 stop_announces ? "" : " (no hello of theirs said \"stopping\")");
            if (end == VMLOG_3D_SHUTDOWN) {
                why = S.host_shutdown ? VR_HOST_SHUTDOWN : VR_GUEST_SHUTDOWN;
                snprintf(d, sizeof d, "the guest shut down; QEMU's 3D display aborted at its last graphics reset (QEMU issue 4918)");
            } else if (end == VMLOG_3D_RESTART) {
                why = VR_GUEST_RESET;
                snprintf(d, sizeof d, "the guest restarted; QEMU's 3D display aborted at its graphics reset (QEMU issue 4918)");
            } else if (end == VMLOG_3D_GUEST_STOP) {
                why = VR_QEMU_3D_STOP;
                snprintf(d, sizeof d, "the guest shut down or restarted, without LordOS tools to say which; QEMU's 3D display aborted (QEMU issue 4918)");
            } else {
                st = VS_CRASHED; why = VR_QEMU_3D_ABORT;
                snprintf(d, sizeof d, "QEMU's 3D display aborted when the guest reset its graphics (QEMU issue 4918)");
            }
            vlog("qemu: recorded as %s (%s)", STATE_NAME[st], REASON_NAME[why]);
        }
        else if (oom > S.oom0) { st = VS_CRASHED; why = VR_OUT_OF_MEMORY; snprintf(d, sizeof d, "the machine went over its memory limit (%ld MB)", vm_mem_limit_mb(&S.m)); }
        else { st = VS_CRASHED; why = VR_QEMU_SIGNAL; snprintf(d, sizeof d, "QEMU died from signal %d (%s)", code, strsignal(code)); }
    }
    if (S.suspending) { if (S.susp_fd >= 0) close(S.susp_fd); S.susp_fd = -1; suspend_cleanup_tmp(); vm_op_end(S.dir); S.suspending = 0; reply(S.susp_client, S.susp_seq, VME_QMP, "QEMU stopped while saving"); }
    if (S.passt > 0) kill(S.passt, SIGTERM);
    if (S.passt_pidfd >= 0) syscall(SYS_pidfd_send_signal, S.passt_pidfd, SIGTERM, NULL, 0);
    for (int i = 0; i <= VM_SHARES; i++) if (S.vfs[i] > 0) kill(S.vfs[i], SIGTERM);
    if (S.tpm > 0) kill(S.tpm, SIGTERM);
    if (why == VR_INSTALLED) unattended_done();
    set_state(st, why, code, d);
}

/* the install is done: the installer's kernel, the answers and the seed disk go, and the ISO comes out of the drive
 * (machine.json without it), so the next start is the installed system's own */
static void unattended_done(void) {
    static const char *const names[] = { "vmlinuz", "initrd", "cmdline", "seed.img", NULL };
    char p[PATH_MAX + 64];
    for (int i = 0; names[i]; i++) {
        snprintf(p, sizeof p, "%s/unattended/%s", S.disks, names[i]);
        int fd = open(p, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) { struct stat st; if (!fstat(fd, &st) && st.st_size > 0 && !strcmp(names[i], "seed.img")) { static const char z[4096]; for (off_t o = 0; o < st.st_size; o += (off_t)sizeof z) if (write(fd, z, sizeof z) < 0) break; } close(fd); }
        unlink(p);
    }
    /* the empty folder stays: lord-vm's own Landlock lets it remove files in the machine's folder, not folders; a start
     * only looks for unattended/cmdline */
    VmMachine m;
    char err[256];
    if (!vm_machine_load(S.dir, &m, err, sizeof err)) {
        m.iso[0] = 0;
        if (!vm_machine_save(S.dir, &m)) S.m.iso[0] = 0;
    }
    S.unattended = 0;
    vlog("unattended: the installer's files and the seed disk are removed, the ISO is out of the drive: the next start is the installed system");
}

static int vfs_index(pid_t p) { for (int i = 0; i <= VM_SHARES; i++) if (p > 0 && S.vfs[i] == p) return i; return -1; }

static void reap(void) {
    int status;
    pid_t p;
    while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
        if (p == S.qemu) { vlog("qemu: exited (%s %d)", WIFEXITED(status) ? "status" : "signal", WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status)); qemu_exited(status); }
        else if (p == S.passt) {
            vlog("passt: exited (%s %d)", WIFEXITED(status) ? "status" : "signal", WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
            S.passt = 0;
            if (!S.qemu_done) { int sig = WIFSIGNALED(status); net_lost(sig, sig ? WTERMSIG(status) : WEXITSTATUS(status)); }
        } else if (vfs_index(p) >= 0) {
            int k = vfs_index(p);
            S.vfs[k] = 0;
            vlog("share %d: virtiofsd exited (%s %d)", k + 1, WIFEXITED(status) ? "status" : "signal", WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
            /* it also ends by itself when QEMU closes the connection (a suspend, a shutdown): news only while the machine runs on */
            if (!S.qemu_done && !S.suspending && S.state == VS_RUNNING) vlog("share %d: the folder %s is gone from the machine until it starts again", k + 1, sh_tag(k));
        } else if (p == S.tpm) {
            S.tpm = 0;
            vlog("tpm: swtpm exited (%s %d)", WIFEXITED(status) ? "status" : "signal", WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
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
    /* the window that had apps mode on is gone (Keep Running): the guest shows its own desktop again */
    if (control && S.lag_apps) { uint8_t off = 0; S.lag_apps = 0; lag_send(LAG_APPS, &off, 1); vlog("agent: apps mode off (the window closed)"); }
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
        /* the system bus first: what's queued already is dispatched here (PrepareForShutdown included, before the
         * deadlines below are read), so its POLLIN is asked for again (vmlogind.h: a queued message left it out and
         * the signal waited ~30 s, until the bus hung up at the end of the shutdown, 2026-11-23) */
        long long bus_next = -1;
        if (logind_poll(&p[n], now_ms(), &bus_next)) { ci[n] = -11; n++; }
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
        WATCH(S.fx_cpipe, -12);
        for (int i = 0; i < MAX_CLIENTS; i++) WATCH(S.cl[i].fd, i);
#undef WATCH
        long long now = now_ms(), next = -1;
        if (S.shut_deadline) next = S.shut_deadline;
        if (S.net_retry_at && (next < 0 || S.net_retry_at < next)) next = S.net_retry_at;
        if (S.host_stopping && (next < 0 || now + 500 < next)) next = now + 500;
        if (S.id_poll_at && (next < 0 || S.id_poll_at < next)) next = S.id_poll_at;
        if (S.xfer_poll_at && (next < 0 || S.xfer_poll_at < next)) next = S.xfer_poll_at;
        if (S.ptr_poll_at && (next < 0 || S.ptr_poll_at < next)) next = S.ptr_poll_at;
        if (S.gvt_poll_at && (next < 0 || S.gvt_poll_at < next)) next = S.gvt_poll_at;
        if (S.inst_poll_at && (next < 0 || S.inst_poll_at < next)) next = S.inst_poll_at;
        if (S.fx_poll_at && (next < 0 || S.fx_poll_at < next)) next = S.fx_poll_at;
        if (S.wi_poll_at && (next < 0 || S.wi_poll_at < next)) next = S.wi_poll_at;
        if (S.migq_next && (next < 0 || S.migq_next < next)) next = S.migq_next;
        if (S.qga_fd >= 0 && S.ag_next && (next < 0 || S.ag_next < next)) next = S.ag_next;
        if (S.kill_deadline && (next < 0 || S.kill_deadline < next)) next = S.kill_deadline;
        if (S.win_key_at && (next < 0 || S.win_key_at < next)) next = S.win_key_at;
        if (mice_watched() && S.mice_at && (next < 0 || S.mice_at < next)) next = S.mice_at;
        long long cl = child_logs_next();
        if (cl >= 0 && (next < 0 || cl < next)) next = cl;
        if (bus_next >= 0 && (next < 0 || bus_next < next)) next = bus_next;
        int to = next < 0 ? -1 : next <= now ? 0 : (int)(next - now);
        if (poll(p, (nfds_t)n, to) < 0 && errno != EINTR) { vlog("poll: %s", strerror(errno)); break; }
        now = now_ms();
        child_logs_tick(now, 0);
        if (S.migq_next && now >= S.migq_next) { S.migq_next = now + 500; qmp_do("query-migrate", NULL, P_MIGQ, -1, 0, -1, -1); }
        if (S.qga_fd >= 0 && S.ag_next && now >= S.ag_next) ag_tick(now);
        if (S.win_key_at && now >= S.win_key_at) {
            qmp_do("send-key", "{\"keys\":[{\"type\":\"qcode\",\"data\":\"ret\"}]}", P_IGNORE, -1, 0, -1, -1);
            S.win_key_at = --S.win_keys > 0 && S.state == VS_RUNNING ? now + 700 : 0;
        }
        if (S.net_retry_at && now >= S.net_retry_at) net_restart();
        if (mice_watched() && now >= S.mice_at) {
            /* the mouse wanted is still the active one (a guest's driver that starts later takes over) */
            S.mice_at = now + 1000;
            if (!S.mice_q && !qmp_do("query-mice", NULL, P_MICE, -1, 0, -1, -1)) S.mice_q = 1;
        }
        if (S.id_poll_at && now >= S.id_poll_at) { char a[48]; S.id_poll_at = 0; snprintf(a, sizeof a, "{\"pid\":%ld}", S.id_pid); qga_do("guest-exec-status", a, AQ_IDSTATUS); }
        if (S.xfer_poll_at && now >= S.xfer_poll_at) { char a[48]; S.xfer_poll_at = 0; snprintf(a, sizeof a, "{\"pid\":%ld}", S.xfer_pid); if (qga_do("guest-exec-status", a, AQ_XSTATUS)) S.xfer_poll_at = now + 300; }
        if (S.ptr_poll_at && now >= S.ptr_poll_at) { char a[48]; S.ptr_poll_at = 0; snprintf(a, sizeof a, "{\"pid\":%ld}", S.ptr_pid); if (qga_do("guest-exec-status", a, AQ_PSTATUS)) S.ptr_poll_at = now + 300; }
        if (S.gvt_poll_at && now >= S.gvt_poll_at) { char a[48]; S.gvt_poll_at = 0; snprintf(a, sizeof a, "{\"pid\":%ld}", S.gvt_pid); if (qga_do("guest-exec-status", a, AQ_GSTATUS)) S.gvt_poll_at = now + 1000; }
        if (S.inst_poll_at && now >= S.inst_poll_at) { char a[48]; S.inst_poll_at = 0; snprintf(a, sizeof a, "{\"pid\":%ld}", S.inst_pid); if (qga_do("guest-exec-status", a, AQ_INSTSTATUS)) S.inst_poll_at = now + 500; }
        fx_tick(now);
        open_tick(now);
        wi_tick(now);
        host_stop_tick(now);
        vgpu_memlock_tick(now);
        if (S.shut_deadline && now >= S.shut_deadline) {
            S.shut_deadline = 0;
            vlog("the guest didn't shut down in time: turning it off");
            S.forced = 1;
            if (qmp_do("quit", NULL, P_IGNORE, -1, 0, -1, -1)) { kill(S.qemu, SIGKILL); S.killed = 1; }
            S.kill_deadline = now + 5000;
            host_kill_cap();
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
            case -12: fx_xcopy_read(); break;
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
    logind_release_shutdown();
    /* what QEMU said last */
    for (int k = 0; k < 20 && S.qemu_err >= 0; k++) pump_fd(&S.qemu_err, "qemu");
    for (int k = 0; k < 20 && S.passt_err >= 0; k++) pump_fd(&S.passt_err, "passt");
    child_logs_tick(now_ms(), 1);
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
        if (n < 0) continue;
        buf[n > 0 ? n : 0] = 0;
        int match = 0;
        for (ssize_t k = 0; k + 36 <= n; k++) if (!memcmp(buf + k, S.m.id, 36)) { match = 1; break; }
        if (i == 1 && !match && strstr(buf, "passt")) match = 1;
        if (!n) {
            /* exiting: its main thread is gone (no command line any more) but other threads still hold its descriptors,
             * and with them the disks' locks. Its name says whether it's ours to wait for */
            char comm[64] = "";
            snprintf(path, sizeof path, "/proc/%ld/comm", pids[i]);
            fd = open(path, O_RDONLY | O_CLOEXEC);
            ssize_t c = fd >= 0 ? read(fd, comm, sizeof comm - 1) : -1;
            if (fd >= 0) close(fd);
            if (c > 0) comm[c] = 0;
            match = !strncmp(comm, i == 0 ? "qemu-system" : "passt", i == 0 ? 11 : 5);
        }
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

/* one credential line (NAME=VALUE or binary:NAME=BASE64) into a sealed memfd holding
 * "io.systemd.credential[.binary]:NAME=VALUE", the form QEMU's -smbios type=11,path= reads. 0 ok */
static int add_credential(const char *line) {
    int bin = !strncmp(line, "binary:", 7);
    const char *c = bin ? line + 7 : line;
    if (S.ncreds >= MAX_CREDS || !cred_ok(c, bin)) return -1;
    char text[2100];
    int len = snprintf(text, sizeof text, "io.systemd.credential%s:%s", bin ? ".binary" : "", c);
    int fd = memfd_create("lordos-vm-credential", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || write_all(fd, text, (size_t)len) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)) {
        if (fd >= 0) close(fd);
        explicit_bzero(text, sizeof text);
        return -1;
    }
    explicit_bzero(text, sizeof text);
    const char *eq = strchr(c, '=');
    snprintf(S.cred_name[S.ncreds], sizeof S.cred_name[0], "%.*s", (int)(eq - c), c);
    S.cred_fd[S.ncreds++] = fd;
    return 0;
}

/* systemd credentials for the guest (see the top of this file), from a file */
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
        if (add_credential(line)) { fprintf(stderr, "lord-vm: a credential line in %s isn't NAME=VALUE (or binary:NAME=BASE64)\n", path); rc = -1; break; }
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
    } else if (!strcmp(o.op, "import")) {
        /* the folder is in place only after machine.json was written: the import had finished (a .part folder that
         * didn't is removed whole by clone_sweep) */
        VmMachine m;
        char err[256];
        snprintf(what, sizeof what, vm_machine_load(dir, &m, err, sizeof err) ? "an import left an unreadable machine.json" : "the import had finished");
    } else if (!strncmp(o.op, "snapshot-", 9) && vm_snap_tag_valid(o.tag)) {
        VmMachine m;
        char err[256], disks[VM_NDISKS][PATH_MAX + 64];
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
    S.listen_fd = S.serial_fd = S.serial_log = S.qemu_err = S.passt_err = S.sig_fd = S.lock_fd = S.memev_fd = S.notify_fd = S.susp_fd = S.console_fd = S.qga_fd = S.lag_fd = S.fx.fd = -1;
    S.keeper_fd = S.passt_pidfd = S.tpm_q = S.xfer_dfd = S.fx_cpipe = S.fx_stagefd = -1;
    S.qmp.fd = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) S.cl[i].fd = -1;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--paused")) S.paused_start = 1;
        else if (!strcmp(argv[i], "--detach")) detach = 1;
        else if (!strcmp(argv[i], "--keep-running")) S.keep_running = 1;
        else if (!strcmp(argv[i], "--dev-qmp")) S.dev_qmp = 1;
        else if (!strcmp(argv[i], "--render-node") && i + 1 < argc) {
            /* the render node for 3D graphics (lord-server passes its own GPU's), or "none": the desktop has no GPU path,
             * so a 3D machine starts with 2D graphics */
            const char *v = argv[++i];
            if (strcmp(v, "none") && (strncmp(v, "/dev/dri/renderD", 16) || strlen(v) >= sizeof S.render_want)) { fprintf(stderr, "lord-vm: --render-node /dev/dri/renderDN|none\n"); return 2; }
            snprintf(S.render_want, sizeof S.render_want, "%s", v);
        }
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
    snprintf(log_dir, sizeof log_dir, "%s/log", S.dir);
    snprintf(p, sizeof p, "%s/log/serial.log", S.dir);
    S.serial_log = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    vlog("lord-vm %d: machine %s \"%s\" (%s, %s, %d vCPU, %d MB, %s)", (int)getpid(), S.m.id, S.m.name, vm_arch_name(S.m.arch),
         S.m.accel ? "tcg" : "kvm", S.m.vcpus, S.m.memory_mb, S.m.machine_type);
    /* the transfer folder (6.7.1): with virtiofsd there; a saved state keeps the hardware it was saved with */
    S.xfer_on = S.m.transfer && file_exists(VIRTIOFSD);
    if (S.m.transfer && !S.xfer_on) vlog("transfer: virtiofsd isn't installed (What Machines Needs): no transfer folder at this start");
    {
        char st[PATH_MAX + 32], meta[PATH_MAX + 32];
        SuspendMeta sm;
        state_paths(st, meta, NULL, NULL, sizeof st);
        if (!discard && file_exists(st) && !suspend_meta_read(S.dir, &sm)) S.xfer_on = sm.transfer;
    }
    RunInfo prev;
    /* QEMU's 3D abort recorded as the guest's shutdown or restart (state stopped, code SIGABRT) is checked too: QEMU
     * didn't close the disks itself */
    int crashed = !runinfo_read(S.dir, &prev) &&
                  ((prev.state == VS_CRASHED &&
                    (prev.reason == VR_QEMU_SIGNAL || prev.reason == VR_QEMU_3D_ABORT || prev.reason == VR_OUT_OF_MEMORY ||
                     prev.reason == VR_SUPERVISOR_LOST)) ||
                   (prev.state == VS_STOPPED && prev.code == SIGABRT &&
                    (prev.reason == VR_GUEST_SHUTDOWN || prev.reason == VR_HOST_SHUTDOWN || prev.reason == VR_GUEST_RESET ||
                     prev.reason == VR_QEMU_3D_STOP)));
    int lost = recover();
    ops_recover(S.dir, 1);
    if (lost || crashed) {
        /* QEMU died in the middle (killed, or with lord-vm): what it had allocated and not yet referenced is given back */
        char disks[VM_NDISKS][PATH_MAX + 64];
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
    {
        /* app.json's keepAwake (on unless turned off): the guest kept awake through the agent, and this computer kept
         * from idle sleep (logind) while the machine runs */
        char ap[PATH_MAX + 32], buf[4096];
        snprintf(ap, sizeof ap, "%s/app.json", S.dir);
        int afd = open(ap, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        ssize_t an = afd >= 0 ? read(afd, buf, sizeof buf - 1) : -1;
        if (afd >= 0) close(afd);
        S.keep_awake = 1;
        JDoc jd;
        if (an > 0 && !json_parse(buf, (size_t)an, &jd, NULL, 0)) { S.keep_awake = json_bool(jd.root, "keepAwake", 1); json_free(&jd); }
    }
    if (!vm_identity_read(S.dir, S.id_hostname, sizeof S.id_hostname)) {
        /* a Clone's first start (6.3): the new hostname as a systemd credential (used by a guest without
         * /etc/hostname), and through the agent once it answers: hostname, machine-id, SSH host keys */
        char line[128];
        snprintf(line, sizeof line, "system.hostname=%s", S.id_hostname);
        if (!add_credential(line)) vlog("identity: this copy is to be named %s (credential system.hostname%s)", S.id_hostname,
                                        S.m.agent ? ", then the guest agent" : "; no guest agent to change its machine-id");
        S.id_state = 1;
    }
    if (S.m.agent) {
        /* lordos-guest for Install Agent: read now, before lord-vm's own Landlock keeps it in the machine's folder */
        static const char *const HELPER[2] = { "/usr/lib/lordos/guest/lordos-guest-arm64", "/usr/lib/lordos/guest/lordos-guest-amd64" };
        int hfd = open(HELPER[S.m.arch], O_RDONLY | O_CLOEXEC);
        struct stat hs;
        if (hfd >= 0 && !fstat(hfd, &hs) && hs.st_size > 0 && hs.st_size < (8 << 20) && (S.helper = malloc((size_t)hs.st_size))) {
            S.helper_n = (size_t)hs.st_size;
            if (pread(hfd, S.helper, S.helper_n, 0) != (ssize_t)S.helper_n) { free(S.helper); S.helper = NULL; S.helper_n = 0; }
        }
        if (hfd >= 0) close(hfd);
        /* and the LordOS guest tools for a Windows guest (x86-64) */
        if (S.m.arch == VM_ARCH_AMD64) {
            /* development (--dev-qmp only): LORDVM_WINTOOLS names a build that isn't installed, and wins over the installed one */
            const char *alt = getenv("LORDVM_WINTOOLS");
            int wfd = alt && S.dev_qmp ? open(alt, O_RDONLY | O_CLOEXEC) : -1;
            if (wfd < 0) wfd = open("/usr/lib/lordos/guest/windows/lordos-guest-win.exe", O_RDONLY | O_CLOEXEC);
            if (wfd >= 0 && !fstat(wfd, &hs) && hs.st_size > 0 && hs.st_size < (8 << 20) && (S.wtools = malloc((size_t)hs.st_size))) {
                S.wtools_n = (size_t)hs.st_size;
                if (pread(wfd, S.wtools, S.wtools_n, 0) != (ssize_t)S.wtools_n) { free(S.wtools); S.wtools = NULL; S.wtools_n = 0; }
            }
            if (wfd >= 0) close(wfd);
        }
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
        for (int i = 0; i <= VM_SHARES; i++) if (S.vfs[i] > 0) { kill(S.vfs[i], SIGKILL); waitpid(S.vfs[i], NULL, 0); S.vfs[i] = 0; }
        if (S.tpm > 0) { kill(S.tpm, SIGKILL); waitpid(S.tpm, NULL, 0); S.tpm = 0; }
        if (S.qemu > 0) { kill(S.qemu, SIGKILL); waitpid(S.qemu, NULL, 0); }
        for (int k = 0; k < 20 && S.qemu_err >= 0; k++) pump_fd(&S.qemu_err, "qemu");
        S.qemu = 0;
    }
    if (S.qemu > 0 && !S.qemu_done) {
        fcntl(S.serial_fd, F_SETFL, O_NONBLOCK);
        fcntl(S.qemu_err, F_SETFL, O_NONBLOCK);
        if (S.passt_err >= 0) fcntl(S.passt_err, F_SETFL, O_NONBLOCK);
        /* what needs more than the machine's folder, before lord-vm's own Landlock: passt's keeper and logind */
        if (S.m.network == VM_NET_SHARED || S.m.network == VM_NET_HOSTONLY) {
            S.keeper = start_keeper(&S.keeper_fd);
            if (S.keeper < 0) { S.keeper = 0; vlog("network: no keeper: a passt that ends isn't replaced"); }
        }
        logind_open();
        S.sandbox |= vm_jail_self(S.dir, S.rundir, S.xfer_on && S.xfer_dfd >= 0 ? S.xfer_path : NULL);
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
    int vcpus = 0, mem = 0, disk = 0, disp = -1, net = -1, agent = -1, clip = -1, devs = -1, nicm = -1, gfx = -1, sound = -1, vol = -1, mic = -1, resw = 0, reshh = 0, res_set = 0, xfer = -1, cask = -1, ptr = -1, gpu = -1, capfs = -1;
    /* shared folders (6.7): --share ro|rw:TAG:PATH, as many as wanted up to VM_SHARES, replace the list; --shares none
     * empties it */
    const char *share[VM_SHARES + 1];
    int nshare = 0, shares_set = 0;
    /* the network (6.5): --port tcp|udp:HOST:GUEST[:this-computer|lan], as many as wanted up to VM_PORTS, replace the
     * list; --ports none empties it. --bridge CARD: the wired card a bridged machine joins */
    VmPort port[VM_PORTS];
    int nport = 0, ports_set = 0;
    const char *bridge = NULL;
    /* where the machine starts from: --boot-order auto|disk,cd,network (in any order, any of them), --boot-menu on|off.
     * Allowed while suspended: bootindex and the boot menu are fw_cfg data the saved stream doesn't carry (a state saved
     * with the disk first loaded with the network first and the menu on, and the guest went on as it was) */
    int boot[VM_BOOT_DEVS], nboot = -1, bmenu = -1;
    /* the sound's output and input (6.9): --sound-out, --sound-in NODE|default */
    const char *sout = NULL, *sin = NULL;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) { fprintf(stderr, "lord-vm: %s needs a value\n", a); return 2; }
        i++;
        if (!strcmp(a, "--boot-order")) { nboot = vm_boot_parse(v, boot); if (nboot < 0) { fprintf(stderr, "lord-vm: --boot-order auto, or disk, cd and network in the order wanted (\"network,disk\")\n"); return 2; } continue; }
        if (!strcmp(a, "--boot-menu")) { bmenu = parse_enum(v, "off", "on"); if (bmenu < 0) { fprintf(stderr, "lord-vm: --boot-menu on|off\n"); return 2; } continue; }
        if (!strcmp(a, "--port")) {
            if (nport >= VM_PORTS) { fprintf(stderr, "lord-vm: at most %d forwarded ports\n", VM_PORTS); return 2; }
            if (vm_port_parse(v, &port[nport])) { fprintf(stderr, "lord-vm: --port tcp|udp:HOST:GUEST[:this-computer|lan], this computer's port %d to 65535\n", VM_PORT_HOST_MIN); return 2; }
            nport++; ports_set = 1;
            continue;
        }
        if (!strcmp(a, "--ports")) { if (strcmp(v, "none")) { fprintf(stderr, "lord-vm: --ports none\n"); return 2; } ports_set = 1; continue; }
        if (!strcmp(a, "--bridge")) { if (!vm_ifname_valid(v)) { fprintf(stderr, "lord-vm: --bridge CARD (a network interface's name)\n"); return 2; } bridge = v; continue; }
        if (!strcmp(a, "--share")) {
            if (nshare >= VM_SHARES) { fprintf(stderr, "lord-vm: at most %d shared folders\n", VM_SHARES); return 2; }
            share[nshare++] = v; shares_set = 1;
            continue;
        }
        if (!strcmp(a, "--shares")) { if (strcmp(v, "none")) { fprintf(stderr, "lord-vm: --shares none\n"); return 2; } shares_set = 1; continue; }
        /* sound (6.9): host-side choices, also while suspended (the guest's hardware doesn't change) */
        if (!strcmp(a, "--sound")) { sound = parse_enum(v, "off", "on"); if (sound < 0) { fprintf(stderr, "lord-vm: --sound on|off\n"); return 2; } continue; }
        if (!strcmp(a, "--microphone")) { mic = parse_enum(v, "off", "on"); if (mic < 0) { fprintf(stderr, "lord-vm: --microphone on|off\n"); return 2; } continue; }
        if (!strcmp(a, "--sound-out") || !strcmp(a, "--sound-in")) {
            if (!v || (strcmp(v, "default") && !vm_sound_dev_valid(v))) { fprintf(stderr, "lord-vm: %s NODE|default (a sound device's node.name)\n", a); return 2; }
            if (!strcmp(a, "--sound-out")) sout = v; else sin = v;
            continue;
        }
        /* the transfer folder (6.7.1): a device of the machine, so like the shares, never while a state is saved */
        if (!strcmp(a, "--clipboard-ask")) { cask = parse_enum(v, "off", "on"); if (cask < 0) { fprintf(stderr, "lord-vm: --clipboard-ask on|off\n"); return 2; } continue; }
        if (!strcmp(a, "--gpu")) {
            gpu = !strcmp(v, "none") ? VM_VGPU_NONE : !strcmp(v, "intel-whole") ? VM_VGPU_WHOLE : !strcmp(v, "intel-shared") ? VM_VGPU_SHARED : -1;
            if (gpu < 0) { fprintf(stderr, "lord-vm: --gpu none|intel-whole|intel-shared\n"); return 2; }
            continue;
        }
        if (!strcmp(a, "--capture-fullscreen")) { capfs = parse_enum(v, "off", "on"); if (capfs < 0) { fprintf(stderr, "lord-vm: --capture-fullscreen on|off\n"); return 2; } continue; }
        if (!strcmp(a, "--pointer")) { ptr = parse_enum(v, "lordos", "guest"); if (ptr < 0) { fprintf(stderr, "lord-vm: --pointer lordos|guest\n"); return 2; } continue; }
        if (!strcmp(a, "--transfer")) { xfer = parse_enum(v, "off", "on"); if (xfer < 0) { fprintf(stderr, "lord-vm: --transfer on|off\n"); return 2; } continue; }
        if (!strcmp(a, "--volume")) {
            char *end = NULL;
            long x = strtol(v, &end, 10);
            if (!*v || *end || x < 0 || x > 100) { fprintf(stderr, "lord-vm: --volume 0..100\n"); return 2; }
            vol = (int)x;
            continue;
        }
        if (!strcmp(a, "--devices")) { devs = parse_enum(v, "virtio", "compat"); if (devs < 0) { fprintf(stderr, "lord-vm: --devices virtio|compat\n"); return 2; } continue; }
        if (!strcmp(a, "--graphics")) { gfx = parse_enum(v, "2d", "3d"); if (gfx < 0) { fprintf(stderr, "lord-vm: --graphics 2d|3d\n"); return 2; } continue; }
        if (!strcmp(a, "--resolution")) {
            if (!strcmp(v, "follow")) { resw = reshh = 0; res_set = 1; continue; }
            if (vm_resolution_parse(v, &resw, &reshh)) { fprintf(stderr, "lord-vm: --resolution follow|WxH (640x480 to 7680x4320, even)\n"); return 2; }
            res_set = 1;
            continue;
        }
        if (!strcmp(a, "--nic-model")) {
            for (int k = 0; VM_NIC_MODELS[k]; k++) if (!strcmp(v, VM_NIC_MODELS[k])) nicm = k;
            if (nicm < 0) { fprintf(stderr, "lord-vm: --nic-model virtio|e1000e|e1000|e1000-82545em|e1000-82544gc|vmxnet3|rtl8139|pcnet\n"); return 2; }
            continue;
        }
        if (!strcmp(a, "--name")) name = v;
        else if (!strcmp(a, "--vcpus")) vcpus = atoi(v);
        else if (!strcmp(a, "--memory")) mem = atoi(v);
        else if (!strcmp(a, "--disk")) disk = atoi(v);
        else if (!strcmp(a, "--display")) disp = parse_enum(v, "window", "none");
        else if (!strcmp(a, "--network")) net = parse_net(v);
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
        /* the graphics card changes with 2D or 3D: a saved state holds the other one's */
        if (file_exists(st) && (vcpus || mem || disk || net >= 0 || agent >= 0 || shares_set || ports_set || bridge || res_set || (gfx >= 0 && gfx != m.graphics) || (xfer >= 0 && xfer != m.transfer) || (gpu >= 0 && gpu != m.vgpu))) { fprintf(stderr, "lord-vm: the machine is suspended: resume it and shut it down first, or discard its saved state\n"); goto out; }
    }
    if (name) snprintf(m.name, sizeof m.name, "%s", name);
    if (vcpus && vcpus != m.vcpus) {
        m.vcpus = vcpus;
        /* an imported topology follows: the same sockets and threads when the new count divides, else QEMU's default */
        if (m.cpu_sockets && vcpus % (m.cpu_sockets * m.cpu_threads) == 0) m.cpu_cores = vcpus / (m.cpu_sockets * m.cpu_threads);
        else m.cpu_sockets = m.cpu_cores = m.cpu_threads = 0;
    }
    if (mem) m.memory_mb = mem;
    if (devs >= 0) m.devices = devs;
    if (nicm >= 0) { m.nic_model = nicm; for (int k = 0; k < m.n_extra_macs; k++) m.extra_models[k] = nicm; }
    if (devs >= 0 || nicm >= 0) m.schema = VM_SCHEMA;
    if (disp >= 0) m.display = disp;
    if (net >= 0) m.network = net;
    if (bridge) snprintf(m.bridge, sizeof m.bridge, "%s", bridge);
    if (ports_set) { m.n_ports = nport; memcpy(m.ports, port, sizeof port[0] * (size_t)nport); }
    if (m.network > VM_NET_ISOLATED || m.bridge[0] || m.n_ports) m.schema = VM_SCHEMA;
    if (agent >= 0) m.agent = agent;
    if (clip >= 0) m.clipboard = clip;
    if (m.agent || m.clipboard) m.schema = VM_SCHEMA;
    if (cask >= 0) { m.clipboard_ask = cask; if (cask) m.schema = VM_SCHEMA; }
    if (ptr >= 0) { m.pointer_guest = ptr; if (ptr) m.schema = VM_SCHEMA; }
    if (capfs >= 0) { m.capture_fullscreen = capfs; if (capfs) m.schema = VM_SCHEMA; }
    if (gfx >= 0) m.graphics = gfx;
    /* a vGPU and 3D graphics are one or the other: the one asked for now wins */
    if (gpu >= 0) { m.vgpu = gpu; if (gpu) { m.schema = VM_SCHEMA; if (gfx < 0) m.graphics = VM_GFX_2D; } }
    if (gfx == VM_GFX_3D && gpu < 0) m.vgpu = VM_VGPU_NONE;
    if (m.graphics) m.schema = VM_SCHEMA;
    if (res_set) { m.res_w = resw; m.res_h = reshh; if (resw) m.schema = VM_SCHEMA; }
    if (sound >= 0) m.sound = sound;
    if (vol >= 0) m.volume = vol;
    if (mic >= 0) m.microphone = mic;
    if (sound >= 0 || vol >= 0 || mic >= 0) m.schema = VM_SCHEMA;
    if (sout) { snprintf(m.sound_out, sizeof m.sound_out, "%s", strcmp(sout, "default") ? sout : ""); m.schema = VM_SCHEMA; }
    if (sin) { snprintf(m.sound_in, sizeof m.sound_in, "%s", strcmp(sin, "default") ? sin : ""); m.schema = VM_SCHEMA; }
    if (xfer >= 0) { m.transfer = xfer; m.schema = VM_SCHEMA; }
    if (nboot >= 0) { m.n_boot = nboot; memcpy(m.boot_order, boot, sizeof boot[0] * (size_t)nboot); }
    if (bmenu >= 0) m.boot_menu = bmenu;
    if (m.n_boot || m.boot_menu) m.schema = VM_SCHEMA;
    if (xfer >= 0) { m.transfer = xfer; m.schema = VM_SCHEMA; }
    if (shares_set) {
        const struct passwd *pw = getpwuid(getuid());
        char home[PATH_MAX], lib[PATH_MAX], parent[PATH_MAX];
        if (!pw || !realpath(pw->pw_dir, home)) { fprintf(stderr, "lord-vm: can't find the home folder\n"); goto out; }
        snprintf(parent, sizeof parent, "%s", real);
        char *sl = strrchr(parent, '/');
        if (sl && sl != parent) *sl = 0;
        if (!realpath(parent, lib)) snprintf(lib, sizeof lib, "%s", parent);
        m.n_shares = 0;
        for (int i = 0; i < nshare; i++) {
            const char *x = share[i], *c1 = strchr(x, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            int rw = c1 && c1 - x == 2 && !strncmp(x, "rw", 2), ro = c1 && c1 - x == 2 && !strncmp(x, "ro", 2);
            if ((!rw && !ro) || !c2 || (size_t)(c2 - c1 - 1) > VM_TAG_MAX) { fprintf(stderr, "lord-vm: --share ro|rw:TAG:PATH\n"); goto out; }
            char path[PATH_MAX], ferr[200];
            if (!realpath(c2 + 1, path)) { fprintf(stderr, "lord-vm: %s: %s\n", c2 + 1, strerror(errno)); goto out; }
            if (vm_share_allowed(path, home, pw->pw_name, lib, ferr, sizeof ferr)) { fprintf(stderr, "lord-vm: %s: %s\n", path, ferr); goto out; }
            if (strlen(path) >= sizeof m.share_path[0]) { fprintf(stderr, "lord-vm: %s: the path is too long\n", path); goto out; }
            int k = m.n_shares++;
            snprintf(m.share_tag[k], sizeof m.share_tag[k], "%.*s", (int)(c2 - c1 - 1), c1 + 1);
            memcpy(m.share_path[k], path, strlen(path) + 1);
            m.share_rw[k] = rw;
        }
        if (m.n_shares) m.schema = VM_SCHEMA;
    }
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
    char bspec[32];
    vm_boot_spec(&m, bspec, sizeof bspec);
    printf("{\"set\":true,\"vcpus\":%d,\"memory_mb\":%d,\"disk_gb\":%d,\"network\":\"%s\",\"ports\":%d,\"iso\":%s,\"agent\":%s,\"devices\":\"%s\",\"nic_model\":\"%s\",\"graphics\":\"%s\",\"gpu\":\"%s\",\"boot_order\":\"%s\",\"boot_menu\":%s,\"transfer\":%s}\n",
           m.vcpus, m.memory_mb, m.disk_gb, VM_NET_NAMES[m.network], m.n_ports, m.iso[0] ? "true" : "false", m.agent ? "true" : "false", VM_DEV_NAMES[m.devices],
           VM_NIC_MODELS[m.nic_model], m.graphics == VM_GFX_3D ? "3d" : "2d", VM_VGPU_NAMES[m.vgpu], bspec, m.boot_menu ? "true" : "false", m.transfer ? "true" : "false");
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
 * lord-vm --clone SRC DST [--name N] [--duplicate] [--linked] [--from-snapshot TAG] [--hostname H | --keep-identity]
 *   Clone: a new machine that runs beside the original (new SMBIOS UUID and MAC; at its first start the guest agent
 *   gives the guest the hostname --hostname names, "<name>-clone" by default, a new machine-id and new SSH host keys,
 *   unless --keep-identity). Duplicate: the same identity, the only kind
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
        VmOp o;
        int imp = vm_op_read_peek(p, &o) == 1 && !strcmp(o.op, "import");
        rm_tree(p);
        vlog("recovery: %s that didn't finish was removed (%s)", imp ? "an import" : "a clone", e->d_name);
    }
    if (d) closedir(d);
}

static int cmd_clone(const char *src, int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "lord-vm: --clone SRC DST [--name N] [--duplicate] [--linked] [--from-snapshot TAG] [--hostname H | --keep-identity]\n"); return 2; }
    const char *dst = argv[0], *name = NULL, *from_snap = NULL, *hostname = NULL;
    int duplicate = 0, linked = 0, keep_identity = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--duplicate")) duplicate = 1;
        else if (!strcmp(argv[i], "--linked")) linked = 1;
        else if (!strcmp(argv[i], "--keep-identity")) keep_identity = 1;
        else if (!strcmp(argv[i], "--hostname") && i + 1 < argc) { hostname = argv[++i]; if (!vm_hostname_valid(hostname)) { fprintf(stderr, "lord-vm: --hostname: one name of a-z, 0-9 and '-', at most 63\n"); return 2; } }
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
    } else if (linked && !duplicate && !from_snap && !nsnaps && !suspended && !m.base[0] && !m.n_data) {
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
    for (int i = 0; i < m.n_data; i++) {
        /* data disks (6.22) the way the system disk went: reflinked, copied with their holes, or converted */
        char sdd[PATH_MAX + 64], ddd[PATH_MAX + 128];
        snprintf(sdd, sizeof sdd, "%s/disks/data-%d.qcow2", real, i + 1);
        snprintf(ddd, sizeof ddd, "%s/disks/data-%d.qcow2", tmp, i + 1);
        int bad = !strcmp(method, "reflink") ? (copy_reflink(sdd, ddd) && copy_sparse(sdd, ddd))
                : duplicate ? copy_sparse(sdd, ddd) : qimg_convert(sdd, NULL, from_snap, ddd, ddir, err, sizeof err);
        if (bad) { fprintf(stderr, "lord-vm: copying data disk %d failed\n", i + 1); goto out; }
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
        /* a Clone runs beside the original: a new SMBIOS UUID (-uuid) and MAC now; inside the guest, a new hostname and
         * machine-id at its first start, through the agent (identity.json), unless the identity is kept */
        vm_new_id(n.id);
        vm_new_mac(n.mac);
        if (!keep_identity) {
            char hn[VM_HOSTNAME_MAX + 1];
            if (hostname) snprintf(hn, sizeof hn, "%s", hostname);
            else {
                char base[VM_HOSTNAME_MAX + 1];
                vm_hostname_from_name(m.name, base, sizeof base);
                snprintf(hn, sizeof hn, "%.*s-clone", VM_HOSTNAME_MAX - 6, base);
            }
            if (vm_identity_write(tmp, hn)) { fprintf(stderr, "lord-vm: can't write identity.json\n"); goto out; }
        }
        for (int i = 0; i < n.n_extra_macs; i++) vm_new_mac(n.extra_macs[i]);
    }
    if (m.tpm) {
        /* the TPM goes with the clone: the state now, or the snapshot's it starts from */
        snprintf(p2, sizeof p2, "%s/disks/tpm", tmp);
        if (mkdir(p2, 0700) && errno != EEXIST) { fprintf(stderr, "lord-vm: can't make the clone's TPM folder\n"); goto out; }
        if (from_snap) snprintf(p1, sizeof p1, "%s/disks/tpm/snap-%s.permall", real, from_snap);
        else snprintf(p1, sizeof p1, "%s/disks/tpm/tpm2-00.permall", real);
        snprintf(p2, sizeof p2, "%s/disks/tpm/tpm2-00.permall", tmp);
        if (copy_small(p1, p2)) { fprintf(stderr, "lord-vm: copying the TPM's state failed\n"); goto out; }
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
    char idh[VM_HOSTNAME_MAX + 1] = "";
    int has_id = !vm_identity_read(dreal, idh, sizeof idh);
    printf(",\"id\":\"%s\",\"method\":\"%s\",\"hostname\":\"%s\"}\n", n.id, method, has_id ? idh : "");
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

/* ---- importing a machine from another virtualization app (docs/VIRTUALIZATION.md 6.22) ----
 *
 * lord-vm --import-plan SRC [--arch arm64|amd64]
 *   SRC: a bundle folder (.vmwarevm, .pvm, a folder with a .vbox), a settings file (.vmx, .vbox, config.pvs, .ovf), an
 *   .ova, or one disk image (VMDK, VDI, VHDX, VHD, qcow2, raw). Prints the plan as JSON: what is kept, what changes,
 *   what can't come along, and what stops the import. Reads only.
 * lord-vm --import-machine SRC DIR [--name N] [--arch A] [--devices compat|virtio] [--network shared|isolated]
 *                                  [--firmware uefi|bios] (a loose disk's) [--vcpus N] [--memory MB] (a loose disk's)
 *   Makes the machine in <library>/.<DIR's name>.part-<pid> under operation.json "import" and renames it into place;
 *   the source is only read. Progress lines on stdout: {"progress":12.5,"disk":1,"disks":2}; the last line is
 *   {"imported":true,...}. A killed import leaves its .part folder, which the next import or clone in the library
 *   removes (clone_sweep), and nothing else.
 *
 * Every file of the source is hostile. Its settings are read through bounded parsers (vmimport.c, vmxml.c); every name it
 * gives is resolved inside the bundle (no "..", no absolute path: an absolute one is looked for by its last part only,
 * no symlink that leads out); each disk's format comes from its first bytes, never from QEMU's probing; qemu-img sees each
 * disk only as a json: spec naming every driver, with each layer's backing named by us (the base's is null) and runs in
 * the tool jail with read access to exactly the files we resolved. Chains qemu-img can't stack (VDI and .hds snapshots)
 * are merged by lord-vm itself in a jailed child that reads those files and writes one sparse raw file in disks/. */

#define IMP_SETTINGS_MAX (4u << 20)

typedef struct {
    char bundle[PATH_MAX];              /* the folder every file must be in */
    char settings[PATH_MAX];            /* the settings file, "" for a loose disk or an .ova */
    char ova[PATH_MAX];                 /* an .ova package, read in place */
    VmiTarEntry tar[VMI_OVA_MAX];
    int ntar;
    VmiMachine m;
    /* per disk, as resolved */
    struct {
        char file[VMI_MAX_CHAIN][PATH_MAX];  /* each layer's real path (in an .ova: the package) */
        long long off[VMI_MAX_CHAIN], len[VMI_MAX_CHAIN];  /* an .ova member's place, else 0 */
        int fmt[VMI_MAX_CHAIN];
        char *extents[VMI_MAX_CHAIN];    /* a VMDK descriptor's extent files, real paths, '\n'-separated (malloc'd) */
        int n;
        int flatten;                     /* VDI or .hds snapshots: lord-vm merges them */
        long long vsize, used;
        char serial[41];
    } d[VMI_MAX_DISKS];
    char blocker[512];                  /* JSON array items of what stops the import */
    char notes[1024];                   /* JSON array items of what changes or can't come along */
} Imp;

static void imp_add(char *list, size_t cap, const char *code, const char *text) {
    size_t l = strlen(list);
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "%s{\"code\":\"%s\"", l ? "," : "", code);
    if (text && text[0]) { jb_raw(&b, ",\"text\":"); jb_str(&b, text); }
    jb_raw(&b, "}");
    if (!b.oom && l + strlen(b.p) + 1 < cap) memcpy(list + l, b.p, strlen(b.p) + 1);
    jb_free(&b);
}
#define BLOCK(I, code, text) imp_add((I)->blocker, sizeof (I)->blocker, code, text)
#define NOTE(I, code, text) imp_add((I)->notes, sizeof (I)->notes, code, text)

static int ends_ci(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

/* reads a small regular file that isn't a symlink, at most max bytes; malloc'd, NUL-terminated */
static char *read_capped(const char *path, size_t max, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size > max) { close(fd); return NULL; }
    char *b = malloc((size_t)st.st_size + 1);
    size_t o = 0;
    while (b && o < (size_t)st.st_size) {
        ssize_t r = read(fd, b + o, (size_t)st.st_size - o);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        o += (size_t)r;
    }
    close(fd);
    if (b) b[o] = 0;
    *len = o;
    return b;
}

/* A name from the source, as a real path inside the bundle. rel_to: the folder the name is relative to (itself inside
 * the bundle). An absolute name is looked for by its last part in rel_to (machines move between computers with their
 * folders); a relative one must be plain (vmi_rel_ok). The result must be a regular file whose real path is inside the
 * bundle: a symlink is fine only when it stays in. 0 ok, -1 with why. */
static int imp_resolve(const Imp *I, const char *rel_to, const char *name, char out[PATH_MAX], char *why, size_t wn) {
    const char *use = name;
    if (name[0] == '/') { const char *sl = strrchr(name, '/'); use = sl + 1; }
    if (!vmi_rel_ok(use)) { snprintf(why, wn, "the source names a file outside its folder (%.120s)", name); return -1; }
    char j[PATH_MAX * 2];
    snprintf(j, sizeof j, "%s/%s", rel_to, use);
    char r[PATH_MAX];
    if (!realpath(j, r)) { snprintf(why, wn, "a file the source needs is missing: %.120s", use); return -1; }
    size_t bl = strlen(I->bundle);
    if (strncmp(r, I->bundle, bl) || r[bl] != '/') { snprintf(why, wn, "the source names a file outside its folder (%.120s)", name); return -1; }
    struct stat st;
    if (lstat(r, &st) || !S_ISREG(st.st_mode)) { snprintf(why, wn, "%.120s isn't a file", use); return -1; }
    if (!dir_ok(r)) { snprintf(why, wn, "a file's path has ',' or control characters: %.120s", use); return -1; }
    memcpy(out, r, strlen(r) + 1);
    return 0;
}

static long ova_read(void *ctx, void *buf, size_t n, long long off) { return (long)pread(*(int *)ctx, buf, n, off); }

static int head_of(const char *path, long long off, uint8_t *h, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t r = pread(fd, h, n, off);
    close(fd);
    return r < 0 ? -1 : (int)r;
}

/* a VMDK's descriptor: the text of a descriptor file, or the one inside a sparse (KDMV) file */
static int vmdk_desc(const char *path, long long off, long long len, VmiVmdk *d, char *why, size_t wn) {
    uint8_t h[512];
    int n = head_of(path, off, h, sizeof h);
    if (n < 4) { snprintf(why, wn, "a disk can't be read"); return -1; }
    char *text = NULL;
    size_t tl = 0;
    if (!memcmp(h, "KDMV", 4)) {
        if (n < 44) { snprintf(why, wn, "a VMDK header is cut short"); return -1; }
        uint64_t doff = 0, dsz = 0;
        for (int i = 7; i >= 0; i--) { doff = doff << 8 | h[28 + i]; dsz = dsz << 8 | h[36 + i]; }
        memset(d, 0, sizeof *d);
        snprintf(d->create_type, sizeof d->create_type, "monolithicSparse");
        if (!doff || !dsz) { d->nextents = 1; return 0; }        /* a stream-optimized piece without one */
        /* inside its member (an .ova's) or its file: checked before any multiplication */
        long long lim = len ? len / 512 : 0;
        if (!len) { struct stat fs; if (stat(path, &fs) || fs.st_size < 0) return -1; lim = (long long)(fs.st_size / 512); }
        if (dsz > 2048 || doff > (uint64_t)lim || dsz > (uint64_t)lim - doff) { snprintf(why, wn, "a VMDK's descriptor is outside its file"); return -1; }
        tl = (size_t)dsz * 512;
        text = malloc(tl + 1);
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        ssize_t r = fd >= 0 && text ? pread(fd, text, tl, off + (long long)doff * 512) : -1;
        if (fd >= 0) close(fd);
        if (r <= 0) { free(text); snprintf(why, wn, "a VMDK's descriptor can't be read"); return -1; }
        tl = (size_t)r;
        text[tl] = 0;
        tl = strlen(text);                                    /* it ends at the first NUL */
    } else if (!memcmp(h, "COWD", 4)) {
        snprintf(why, wn, "a disk is in an old server format (COWD), which isn't supported");
        return -1;
    } else {
        if (len) { snprintf(why, wn, "a disk in the package isn't a VMDK"); return -1; }
        text = read_capped(path, 1u << 20, &tl);
        if (!text) { snprintf(why, wn, "a disk's descriptor can't be read (or is larger than 1 MB)"); return -1; }
    }
    int r = vmi_parse_vmdk(text, tl, d, why, wn);
    free(text);
    return r;
}

static const char *fmt_name(int f) {
    static const char *const N[] = { "?", "vmdk", "vdi", "vhdx", "vpc", "qcow2", "raw", "parallels" };
    return f >= 0 && f <= VMF_PARALLELS ? N[f] : "?";
}

/* resolves disk i: every layer's file and format, a VMDK's extents and parents; decides whether qemu-img can read the
 * chain or lord-vm merges it. dir0: the folder its first name is relative to. */
static int imp_disk(Imp *I, int i, const char *dir0, char *why, size_t wn) {
    VmiDisk *src = &I->m.disk[i];
    typeof(I->d[0]) *D = &I->d[i];
    D->n = 0;
    if (I->ova[0]) {
        /* an .ova's disk: one member, read in place */
        int k = -1;
        for (int t = 0; t < I->ntar; t++) if (!strcmp(I->tar[t].name, src->chain[0].path)) k = t;
        if (k < 0) { snprintf(why, wn, "a disk named by the package isn't in it: %.80s", src->chain[0].path); return -1; }
        uint8_t h[512];
        if (head_of(I->ova, I->tar[k].offset, h, sizeof h) < 64) { snprintf(why, wn, "a disk in the package can't be read"); return -1; }
        int f = vmi_disk_format(h, 512, src->chain[0].path);
        if (f != VMF_VMDK && f != VMF_VPC && f != VMF_VHDX && f != VMF_VDI && f != VMF_QCOW2) { snprintf(why, wn, "a disk in the package has a format that isn't supported"); return -1; }
        if (f == VMF_QCOW2 && vm_qcow2_plain(h, 512, why, wn)) return -1;
        if (f == VMF_VMDK) {
            VmiVmdk vd;
            if (vmdk_desc(I->ova, I->tar[k].offset, I->tar[k].size, &vd, why, wn)) return -1;
            if (vd.parent[0]) { snprintf(why, wn, "a disk in the package depends on another file"); return -1; }
            if (strcmp(vd.create_type, "streamOptimized") && strcmp(vd.create_type, "monolithicSparse")) { snprintf(why, wn, "a disk in the package isn't self-contained (%.30s)", vd.create_type); return -1; }
        }
        snprintf(D->file[0], PATH_MAX, "%s", I->ova);
        D->off[0] = I->tar[k].offset; D->len[0] = I->tar[k].size; D->fmt[0] = f; D->n = 1;
        D->used = I->tar[k].size;
        return 0;
    }
    char rel_dir[PATH_MAX];
    snprintf(rel_dir, sizeof rel_dir, "%s", dir0);
    if (src->chain[0].fmt == VMF_PARALLELS) {
        /* a .hdd folder: its DiskDescriptor.xml names the data files, base first */
        char hdd[PATH_MAX * 2], dreal[PATH_MAX];
        const char *nm = src->chain[0].path;
        if (!vmi_rel_ok(nm)) { snprintf(why, wn, "the source names a disk outside its folder"); return -1; }
        snprintf(hdd, sizeof hdd, "%s/%s", dir0, nm);
        if (imp_resolve(I, hdd, "DiskDescriptor.xml", dreal, why, wn)) return -1;
        size_t tl;
        char *t = read_capped(dreal, 1u << 20, &tl);
        if (!t) { snprintf(why, wn, "a disk's descriptor can't be read"); return -1; }
        VmiHdd hd;
        int r = vmi_parse_hdd(t, tl, &hd, why, wn);
        free(t);
        if (r) return -1;
        if (hd.encrypted) { I->m.encrypted = 1; return 0; }
        I->m.snapshots += hd.snapshots;
        char hreal[PATH_MAX];
        if (!realpath(hdd, hreal)) { snprintf(why, wn, "a disk folder is missing"); return -1; }
        for (int k = 0; k < hd.nlayers; k++) {
            if (imp_resolve(I, hreal, hd.file[k], D->file[k], why, wn)) return -1;
            uint8_t h[64];
            if (head_of(D->file[k], 0, h, sizeof h) < 64 || vmi_disk_format(h, 64, D->file[k]) != VMF_PARALLELS) { snprintf(why, wn, "a disk's data file isn't in the expected format"); return -1; }
            D->fmt[k] = VMF_PARALLELS;
        }
        D->n = hd.nlayers;
        D->flatten = D->n > 1;
        D->vsize = hd.sectors * 512;
        return 0;
    }
    if (src->nchain > 1) {
        /* a .vbox chain: each layer from the media registry, base first */
        for (int k = 0; k < src->nchain; k++) {
            if (imp_resolve(I, rel_dir, src->chain[k].path, D->file[k], why, wn)) return -1;
            uint8_t h[512];
            int hn = head_of(D->file[k], 0, h, sizeof h);
            int f = vmi_disk_format(h, hn > 0 ? (size_t)hn : 0, D->file[k]);
            if (f != src->chain[k].fmt) { snprintf(why, wn, "a disk's file isn't the format its settings say"); return -1; }
            D->fmt[k] = f;
            if (f == VMF_VDI) {
                VmiVdi v;
                if (vmi_vdi_header(h, (size_t)hn, &v)) { snprintf(why, wn, "a VDI header is broken"); return -1; }
                if ((k == 0) != (v.type != 4)) { snprintf(why, wn, "a VDI snapshot layer isn't where the settings put it"); return -1; }
                if (k && strcasecmp(v.parent, src->chain[k - 1].uuid)) { snprintf(why, wn, "a VDI snapshot layer doesn't belong to its parent"); return -1; }
            }
        }
        D->n = src->nchain;
        for (int k = 1; k < D->n; k++) if (D->fmt[k] != D->fmt[0]) { snprintf(why, wn, "a disk's snapshots are in another format than its base"); return -1; }
        if (D->fmt[0] == VMF_VDI) D->flatten = 1;
        else if (D->fmt[0] != VMF_VMDK) { snprintf(why, wn, "snapshots of %s disks aren't supported", fmt_name(D->fmt[0])); return -1; }
    } else {
        /* one name: a VMDK may lead to its parents (a .vmx's snapshot), each named by the one above it */
        char cur[PATH_MAX];
        if (imp_resolve(I, rel_dir, src->chain[0].path, cur, why, wn)) return -1;
        char chain[VMI_MAX_CHAIN][PATH_MAX];
        int nc = 0;
        for (;;) {
            for (int k = 0; k < nc; k++) if (!strcmp(chain[k], cur)) { snprintf(why, wn, "a disk's snapshots loop"); return -1; }
            if (nc >= VMI_MAX_CHAIN) { snprintf(why, wn, "a disk has more than %d snapshot layers", VMI_MAX_CHAIN); return -1; }
            memcpy(chain[nc++], cur, sizeof cur);
            uint8_t h[512];
            int hn = head_of(cur, 0, h, sizeof h);
            int f = vmi_disk_format(h, hn > 0 ? (size_t)hn : 0, cur);
            if (f != VMF_VMDK) break;
            VmiVmdk vd;
            if (vmdk_desc(cur, 0, 0, &vd, why, wn)) return -1;
            if (vd.encrypted) { I->m.encrypted = 1; return 0; }
            if (!vd.parent[0]) break;
            char pd[PATH_MAX];
            snprintf(pd, sizeof pd, "%s", cur);
            *strrchr(pd, '/') = 0;
            if (imp_resolve(I, pd, vd.parent, cur, why, wn)) return -1;
        }
        for (int k = 0; k < nc; k++) memcpy(D->file[k], chain[nc - 1 - k], PATH_MAX);
        D->n = nc;
        if (nc > 1) I->m.snapshots = I->m.snapshots > nc - 1 ? I->m.snapshots : nc - 1;
        for (int k = 0; k < nc; k++) {
            uint8_t h[512];
            int hn = head_of(D->file[k], 0, h, sizeof h);
            D->fmt[k] = vmi_disk_format(h, hn > 0 ? (size_t)hn : 0, D->file[k]);
            if (D->fmt[k] == VMF_UNKNOWN) { snprintf(why, wn, "a disk's format isn't one that can be imported (VMDK, VDI, VHDX, VHD, qcow2, raw)"); return -1; }
            if (k && D->fmt[k] != VMF_VMDK) { snprintf(why, wn, "a disk's snapshots aren't VMDK"); return -1; }
            if (D->fmt[k] == VMF_QCOW2 && vm_qcow2_plain(h, (size_t)hn, why, wn)) return -1;
            if (D->fmt[k] == VMF_VDI) {
                VmiVdi v;
                if (vmi_vdi_header(h, (size_t)hn, &v)) { snprintf(why, wn, "a VDI header is broken"); return -1; }
                if (v.type == 4) { snprintf(why, wn, "the disk is a VDI snapshot layer: import the machine's .vbox instead"); return -1; }
            }
        }
    }
    /* every VMDK layer's extents, inside the bundle, next to their descriptor */
    for (int k = 0; k < D->n; k++) {
        if (D->fmt[k] != VMF_VMDK) continue;
        VmiVmdk vd;
        if (vmdk_desc(D->file[k], 0, 0, &vd, why, wn)) return -1;
        if (vd.encrypted) { I->m.encrypted = 1; return 0; }
        if (!strcmp(vd.create_type, "monolithicSparse") || !strcmp(vd.create_type, "streamOptimized")) continue;
        if (!strcmp(vd.create_type, "vmfsRaw") || !strcmp(vd.create_type, "vmfsRawDeviceMap") || !strcmp(vd.create_type, "fullDevice") ||
            !strcmp(vd.create_type, "partitionedDevice") || !strcmp(vd.create_type, "vmfsPassthroughRawDeviceMap")) {
            snprintf(why, wn, "a disk is a host device (%.30s), which isn't imported", vd.create_type); return -1;
        }
        char pd[PATH_MAX];
        snprintf(pd, sizeof pd, "%s", D->file[k]);
        *strrchr(pd, '/') = 0;
        JBuf b;
        jb_init(&b);
        for (int e = 0; e < vd.nextents; e++) {
            if (!vd.ext[e].file[0]) continue;                   /* ZERO */
            if (strchr(vd.ext[e].file, '/')) { jb_free(&b); snprintf(why, wn, "a disk's piece is in another folder: %.80s", vd.ext[e].file); return -1; }
            char er[PATH_MAX];
            if (imp_resolve(I, pd, vd.ext[e].file, er, why, wn)) { jb_free(&b); return -1; }
            jb_raw(&b, er); jb_raw(&b, "\n");
            struct stat st;
            if (!stat(er, &st)) D->used += (long long)st.st_blocks * 512;
        }
        if (b.oom) { jb_free(&b); snprintf(why, wn, "out of memory"); return -1; }
        D->extents[k] = b.p;
    }
    for (int k = 0; k < D->n; k++) { struct stat st; if (!stat(D->file[k], &st)) D->used += (long long)st.st_blocks * 512; }
    return 0;
}

/* the json: spec of one layer, and of the chain over it (each layer's backing named, the base's null) */
static void spec_layer(JBuf *b, const Imp *I, int i, int k, int alone) {
    const typeof(I->d[0]) *D = &I->d[i];
    jb_printf(b, "{\"driver\":\"%s\",\"read-only\":true,\"file\":", fmt_name(D->fmt[k]));
    if (D->len[k]) {
        jb_printf(b, "{\"driver\":\"raw\",\"offset\":%lld,\"size\":%lld,\"file\":{\"driver\":\"file\",\"filename\":", D->off[k], D->len[k]);
        jb_str(b, D->file[k]);
        jb_raw(b, "}}");
    } else {
        jb_raw(b, "{\"driver\":\"file\",\"filename\":");
        jb_str(b, D->file[k]);
        jb_raw(b, "}");
    }
    if (D->fmt[k] == VMF_VMDK && D->extents[k]) {
        /* a descriptor's pieces: opened by the file protocol only, never probed (their names are checked plain) */
        int ne = 0;
        for (const char *c = D->extents[k]; *c; c++) if (*c == '\n') ne++;
        jb_raw(b, ",\"extents\":[");
        for (int e = 0; e < ne; e++) jb_printf(b, "%s{\"driver\":\"file\"}", e ? "," : "");
        jb_raw(b, "]");
    }
    if (D->fmt[k] == VMF_VMDK || D->fmt[k] == VMF_QCOW2) {
        if (k == 0 || alone) jb_raw(b, ",\"backing\":null");
        else { jb_raw(b, ",\"backing\":"); spec_layer(b, I, i, k - 1, 0); }
    }
    jb_raw(b, "}");
}
/* alone: the top layer only, with no backing (qemu-img info opens without backing files, so it refuses one named) */
static char *spec_chain(const Imp *I, int i, int alone) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "json:");
    spec_layer(&b, I, i, I->d[i].n - 1, alone);
    return b.oom ? (jb_free(&b), NULL) : b.p;
}

static int jail_overflow;            /* jail_files had more files than it can list: the import says so instead of failing in qemu-img */
/* the files qemu-img may read for disk i: every layer and every extent */
static int jail_files(const Imp *I, int i, const char **out, int max) {
    int n = 0;
    for (int k = 0; k < I->d[i].n && n < max; k++) {
        int dup = 0;
        for (int q = 0; q < n; q++) if (!strcmp(out[q], I->d[i].file[k])) dup = 1;
        if (!dup) out[n++] = I->d[i].file[k];
    }
    static char ext[256][PATH_MAX];
    int ne = 0;
    jail_overflow = 0;
    for (int k = 0; k < I->d[i].n; k++) {
        for (const char *c = I->d[i].extents[k]; c && *c && n < max && ne < 256; ) {
            const char *nl = strchr(c, '\n');
            size_t l = nl ? (size_t)(nl - c) : strlen(c);
            if (l && l < PATH_MAX) {
                memcpy(ext[ne], c, l); ext[ne][l] = 0;
                int dup = 0;
                for (int q = 0; q < n; q++) if (!strcmp(out[q], ext[ne])) dup = 1;
                if (!dup) out[n++] = ext[ne++];
            }
            c = nl ? nl + 1 : c + l;
        }
        if (I->d[i].extents[k] && *I->d[i].extents[k] && (ne >= 256 || n >= max)) jail_overflow = 1;
    }
    return n;
}

/* qemu-img info of disk i's chain (jailed): its virtual size */
static int imp_info(Imp *I, int i, char *why, size_t wn) {
    if (I->d[i].flatten) {
        /* merged by lord-vm: the size is the base's header's */
        uint8_t h[512];
        int hn = head_of(I->d[i].file[0], 0, h, sizeof h);
        if (I->d[i].fmt[0] == VMF_VDI) {
            VmiVdi v;
            if (hn < 456 || vmi_vdi_header(h, (size_t)hn, &v)) { snprintf(why, wn, "a VDI header is broken"); return -1; }
            I->d[i].vsize = v.size;
        }
        return I->d[i].vsize > 0 ? 0 : -1;
    }
    char *spec = spec_chain(I, i, 1);
    if (!spec) { snprintf(why, wn, "out of memory"); return -1; }
    const char *files[VMI_MAX_CHAIN * VMI_MAX_EXTENTS + VMI_MAX_CHAIN];
    jail_list = files;
    jail_n = jail_files(I, i, files, (int)(sizeof files / sizeof files[0]));
    if (jail_overflow) { jail_list = NULL; jail_n = 0; free(spec); snprintf(why, wn, "a disk has more pieces than can be imported"); return -1; }
    char *argv[] = { QEMU_IMG, "info", "--output=json", spec, NULL };
    static char buf[65536];
    int r = run_tool_in(argv, buf, sizeof buf, files[0], NULL);
    jail_list = NULL; jail_n = 0;
    free(spec);
    if (r) { snprintf(why, wn, "QEMU can't read a disk: %.200s", buf); return -1; }
    JDoc d;
    if (json_parse(buf, strlen(buf), &d, NULL, 0)) { snprintf(why, wn, "qemu-img's answer isn't JSON"); return -1; }
    double vs = json_num(d.root, "virtual-size", 0);
    const JVal *fs = json_get(json_get(d.root, "format-specific"), "data");
    int bad = fs && json_get(fs, "data-file");
    json_free(&d);
    if (bad) { snprintf(why, wn, "a disk depends on another file: refused"); return -1; }
    if (vs <= 0) { snprintf(why, wn, "a disk has no size"); return -1; }
    I->d[i].vsize = (long long)vs;
    return 0;
}

/* detects SRC and resolves everything; blockers and notes say what the plan sheet shows. -1 only when SRC isn't
 * something that can be imported at all (why says) */
static int imp_detect(const char *src, Imp *I, int want_arch, char *why, size_t wn) {
    memset(I, 0, sizeof *I);
    char real[PATH_MAX];
    if (!realpath(src, real) || !dir_ok(real)) { snprintf(why, wn, "%s: can't be read, or its path has ',' or control characters", src); return -1; }
    struct stat st;
    if (stat(real, &st)) { snprintf(why, wn, "%s: %s", src, strerror(errno)); return -1; }
    char settings[PATH_MAX] = "";
    int kind = VMI_NONE, loose = 0;
    if (S_ISDIR(st.st_mode)) {
        /* a bundle: one settings file in it */
        snprintf(I->bundle, sizeof I->bundle, "%s", real);
        DIR *d = opendir(real);
        struct dirent *e;
        int found = 0, entries = 0;
        while (d && (e = readdir(d)) && entries++ < 4096) {
            int k = ends_ci(e->d_name, ".vmx") ? VMI_VMX : ends_ci(e->d_name, ".vbox") ? VMI_VBOX : !strcmp(e->d_name, "config.pvs") ? VMI_PVS : ends_ci(e->d_name, ".ovf") ? VMI_OVF : VMI_NONE;
            if (!k) continue;
            if (found++) { kind = -1; break; }
            kind = k;
            if ((size_t)snprintf(settings, sizeof settings, "%s/%.255s", real, e->d_name) >= sizeof settings) { kind = -1; break; }
        }
        if (d) closedir(d);
        if (kind < 0) { snprintf(why, wn, "the folder holds more than one machine's settings: choose the settings file itself"); return -1; }
        if (!kind) { snprintf(why, wn, "the folder holds no machine settings (.vmx, .vbox, config.pvs, .ovf)"); return -1; }
    } else if (S_ISREG(st.st_mode)) {
        snprintf(I->bundle, sizeof I->bundle, "%s", real);
        *strrchr(I->bundle, '/') = 0;
        if (!I->bundle[0]) snprintf(I->bundle, sizeof I->bundle, "/");
        if (ends_ci(real, ".vmx")) kind = VMI_VMX;
        else if (ends_ci(real, ".vbox")) kind = VMI_VBOX;
        else if (ends_ci(real, "/config.pvs")) kind = VMI_PVS;
        else if (ends_ci(real, ".ovf")) kind = VMI_OVF;
        else if (ends_ci(real, ".ova")) { kind = VMI_OVF; snprintf(I->ova, sizeof I->ova, "%s", real); }
        else { kind = VMI_DISK; loose = 1; }
        if (kind != VMI_DISK && !I->ova[0]) snprintf(settings, sizeof settings, "%s", real);
    } else { snprintf(why, wn, "%s isn't a file or a folder", src); return -1; }
    if (!strcmp(I->bundle, "/") || !strcmp(I->bundle, "/home") || !strncmp(I->bundle, "/proc", 5) || !strncmp(I->bundle, "/sys", 4) || !strncmp(I->bundle, "/dev", 4)) {
        snprintf(why, wn, "the source must be in a folder of its own"); return -1;
    }
    if (settings[0]) {
        char r2[PATH_MAX];
        if (!realpath(settings, r2) || lstat(r2, &st) || !S_ISREG(st.st_mode)) { snprintf(why, wn, "the settings file can't be read"); return -1; }
        snprintf(I->settings, sizeof I->settings, "%s", r2);
    }
    char *text = NULL;
    size_t tl = 0;
    if (I->ova[0]) {
        int fd = open(I->ova, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        struct stat os;
        if (fd < 0 || fstat(fd, &os)) { if (fd >= 0) close(fd); snprintf(why, wn, "the package can't be read"); return -1; }
        I->ntar = vmi_tar_list(ova_read, &fd, os.st_size, I->tar, VMI_OVA_MAX, why, wn);
        if (I->ntar < 0) { close(fd); return -1; }
        int k = -1;
        for (int t = 0; t < I->ntar; t++) if (ends_ci(I->tar[t].name, ".ovf")) { if (k >= 0) { close(fd); snprintf(why, wn, "the package holds two descriptors"); return -1; } k = t; }
        if (k < 0 || I->tar[k].size > IMP_SETTINGS_MAX) { close(fd); snprintf(why, wn, "the package holds no descriptor (.ovf), or it's too large"); return -1; }
        tl = (size_t)I->tar[k].size;
        text = malloc(tl + 1);
        if (!text || pread(fd, text, tl, I->tar[k].offset) != (ssize_t)tl) { free(text); close(fd); snprintf(why, wn, "the package's descriptor can't be read"); return -1; }
        text[tl] = 0;
        close(fd);
    } else if (settings[0]) {
        text = read_capped(I->settings, IMP_SETTINGS_MAX, &tl);
        if (!text) { snprintf(why, wn, "the settings file can't be read (or is larger than 4 MB)"); return -1; }
    }
    VmiMachine *m = &I->m;
    int r = 0;
    char perr[200] = "";
    switch (kind) {
    case VMI_VMX: r = vmi_parse_vmx(text, tl, m, perr, sizeof perr); break;
    case VMI_VBOX: r = vmi_parse_vbox(text, tl, m, perr, sizeof perr); break;
    case VMI_PVS: r = vmi_parse_pvs(text, tl, m, perr, sizeof perr); break;
    case VMI_OVF: r = vmi_parse_ovf(text, tl, m, perr, sizeof perr); break;
    default: {
        /* a loose disk: a machine around it with this computer's defaults */
        memset(m, 0, sizeof *m);
        m->kind = VMI_DISK;
        m->arch = -1;
        m->firmware = VM_FW_UEFI;
        const char *b = strrchr(real, '/') + 1;
        snprintf(m->name, sizeof m->name, "%.*s", (int)(strrchr(b, '.') && strrchr(b, '.') != b ? strrchr(b, '.') - b : (long)strlen(b)), b);
        m->ndisk = 1;
        snprintf(m->disk[0].chain[0].path, VMI_PATH, "%s", b);
        m->disk[0].nchain = 1;
        m->disk[0].boot = 1;
        uint8_t h[512];
        int hn = head_of(real, 0, h, sizeof h);
        m->disk[0].chain[0].fmt = vmi_disk_format(h, hn > 0 ? (size_t)hn : 0, real);
        if (m->disk[0].chain[0].fmt == VMF_UNKNOWN) { snprintf(why, wn, "this file isn't a machine or a disk image that can be imported"); return -1; }
    } }
    free(text);
    (void)loose;
    if (r) { snprintf(why, wn, "%s", perr); return -1; }
    if (want_arch >= 0) m->arch = want_arch;
    if (m->arch < 0) m->arch = VM_ARCH_AMD64;
    if (m->arch == VM_ARCH_ARM64) m->firmware = VM_FW_UEFI;
    /* what's beside the settings: a suspended session, the other app still running it */
    if (kind != VMI_DISK && !I->ova[0]) {
        DIR *d = opendir(I->bundle);
        struct dirent *e;
        int entries = 0;
        while (d && (e = readdir(d)) && entries++ < 4096) {
            if (ends_ci(e->d_name, ".vmss") || (kind == VMI_PVS && (ends_ci(e->d_name, ".sav") || ends_ci(e->d_name, ".mem")))) m->saved_state = 1;
            if (ends_ci(e->d_name, ".lck") || (kind == VMI_PVS && !strcmp(e->d_name, "config.pvs.lock"))) BLOCK(I, "in-use", NULL);
            if (kind == VMI_VMX && ends_ci(e->d_name, ".vmsn")) m->snapshots = m->snapshots ? m->snapshots : 1;
        }
        if (d) closedir(d);
    }
    if (m->problem[0]) BLOCK(I, "error", m->problem);
    if (m->saved_state) BLOCK(I, "saved-state", NULL);
    if (m->encrypted) BLOCK(I, "encrypted", NULL);
    if (m->more_disks) BLOCK(I, "too-many-disks", NULL);
    if (!m->ndisk && !m->problem[0] && !m->encrypted) BLOCK(I, "no-disk", NULL);
    char rel0[PATH_MAX];
    snprintf(rel0, sizeof rel0, "%s", I->settings[0] ? I->settings : real);
    *strrchr(rel0, '/') = 0;
    if (!rel0[0]) snprintf(rel0, sizeof rel0, "/");
    for (int i = 0; i < m->ndisk && !I->blocker[0]; i++) {
        char w[300];
        if (imp_disk(I, i, rel0, w, sizeof w) || (!m->encrypted && imp_info(I, i, w, sizeof w))) { BLOCK(I, "error", w); break; }
        if (m->encrypted) { BLOCK(I, "encrypted", NULL); break; }
        /* the serial the guest read: a VMDK's ddb.uuid with disk.EnableUUID, or the source's own */
        if (!strcmp(m->disk[i].serial, "ddb.uuid")) {
            VmiVmdk vd;
            char w2[200];
            m->disk[i].serial[0] = 0;
            if (I->d[i].fmt[0] == VMF_VMDK && !vmdk_desc(I->d[i].file[0], I->d[i].off[0], I->d[i].len[0], &vd, w2, sizeof w2)) {
                size_t o = 0;
                for (const char *c = vd.uuid; *c && o + 1 < sizeof m->disk[i].serial; c++) if (isxdigit((unsigned char)*c)) m->disk[i].serial[o++] = (char)tolower((unsigned char)*c);
                m->disk[i].serial[o] = 0;
            }
        }
        snprintf(I->d[i].serial, sizeof I->d[i].serial, "%s", m->disk[i].serial);
    }
    if (m->snapshots) NOTE(I, "snapshots-flattened", NULL);
    if (m->tpm) NOTE(I, "tpm", NULL);
    if (m->secure_boot) NOTE(I, "secure-boot-off", NULL);
    if (m->firmware == VM_FW_UEFI && kind != VMI_DISK) NOTE(I, "boot-entries", NULL);
    if (m->cdrom_files) NOTE(I, "cdrom", NULL);
    if (m->shared_folders) NOTE(I, "shared-folders", NULL);
    if (m->usb_filters) NOTE(I, "usb", NULL);
    if (m->more_nics) NOTE(I, "too-many-nics", NULL);
    for (int i = 0; i < m->nnic; i++) if (!m->nic[i].mac[0]) { NOTE(I, "new-mac", NULL); break; }
    if (!m->uuid[0] && kind != VMI_DISK) NOTE(I, "new-uuid", NULL);
    for (int i = 0; i < m->ndisk; i++) if (strlen(m->disk[i].serial) > VM_SERIAL_MAX || !vm_serial_valid(m->disk[i].serial)) { NOTE(I, "serial-dropped", m->disk[i].serial); break; }
    if (kind != VMI_DISK) NOTE(I, "license", NULL);
    return 0;
}

/* the VmMachine the import makes (host limits applied; what was cut is in the notes) */
static int imp_machine(Imp *I, VmMachine *n, const VmHost *h, int devices, int network, int fw_over, int vcpus, int mem, char *why, size_t wn) {
    VmiMachine *m = &I->m;
    vm_machine_defaults(n, h, m->arch);
    if (m->uuid[0]) snprintf(n->id, sizeof n->id, "%s", m->uuid);
    for (char *c = n->id; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (!vm_uuid_valid(n->id)) vm_new_id(n->id);
    char nm[VM_NAME_MAX + 1];
    snprintf(nm, sizeof nm, "%s", m->name[0] ? m->name : "Imported machine");
    if (!vm_name_valid(nm)) snprintf(nm, sizeof nm, "Imported machine");
    snprintf(n->name, sizeof n->name, "%s", nm);
    if (m->kind != VMI_DISK) { n->vcpus = m->vcpus; n->memory_mb = m->memory_mb; n->firmware = m->firmware; }
    if (fw_over >= 0) n->firmware = fw_over;
    if (vcpus > 0) n->vcpus = vcpus;
    if (mem > 0) n->memory_mb = mem;
    if (m->arch == VM_ARCH_ARM64) n->firmware = VM_FW_UEFI;
    if (m->sockets && !vcpus) { n->cpu_sockets = m->sockets; n->cpu_cores = m->cores; n->cpu_threads = m->threads; }
    if (n->vcpus > h->cpus) { NOTE(I, "vcpus-cut", NULL); n->vcpus = h->cpus; n->cpu_sockets = n->cpu_cores = n->cpu_threads = 0; }
    if (n->memory_mb > vm_mem_max_mb(h)) { NOTE(I, "memory-cut", NULL); n->memory_mb = (int)vm_mem_max_mb(h); }
    if (n->memory_mb < VM_MEM_MIN_MB) n->memory_mb = VM_MEM_MIN_MB;
    n->network = network;
    n->devices = devices;
    n->rtc = m->rtc_local ? VM_RTC_LOCAL : VM_RTC_UTC;
    /* the cards: the first is connected, the others keep their addresses */
    int nic = -1;
    for (int i = 0; i < m->nnic; i++) {
        char mac[18];
        if (m->nic[i].mac[0]) memcpy(mac, m->nic[i].mac, 18); else vm_new_mac(mac);
        if (nic < 0) { memcpy(n->mac, mac, 18); nic = i; continue; }
        if (n->n_extra_macs < VM_EXTRA_NICS && strcmp(mac, n->mac)) {
            int k = n->n_extra_macs++, model = devices == VM_DEV_COMPAT ? VM_NIC_E1000E : VM_NIC_VIRTIO;
            memcpy(n->extra_macs[k], mac, 18);
            for (int q = 0; m->nic[i].model[0] && VM_NIC_MODELS[q]; q++) if (!strcmp(m->nic[i].model, VM_NIC_MODELS[q])) model = q;
            n->extra_models[k] = model;
        }
    }
    n->nic_model = devices == VM_DEV_COMPAT ? VM_NIC_E1000E : VM_NIC_VIRTIO;
    if (nic >= 0 && m->nic[nic].model[0])
        for (int k = 0; VM_NIC_MODELS[k]; k++) if (!strcmp(m->nic[nic].model, VM_NIC_MODELS[k])) n->nic_model = k;
    snprintf(n->sm_manufacturer, sizeof n->sm_manufacturer, "%s", m->sm_manufacturer);
    snprintf(n->sm_product, sizeof n->sm_product, "%s", m->sm_product);
    snprintf(n->sm_version, sizeof n->sm_version, "%s", m->sm_version);
    snprintf(n->sm_serial, sizeof n->sm_serial, "%s", m->sm_serial);
    snprintf(n->sm_sku, sizeof n->sm_sku, "%s", m->sm_sku);
    snprintf(n->sm_family, sizeof n->sm_family, "%s", m->sm_family);
    n->disk_gb = (int)((I->d[0].vsize + (1LL << 30) - 1) >> 30);
    if (n->disk_gb < VM_DISK_MIN_GB) n->disk_gb = VM_DISK_MIN_GB;
    if (vm_serial_valid(I->d[0].serial)) memcpy(n->disk_serial, I->d[0].serial, strlen(I->d[0].serial) + 1);
    n->n_data = m->ndisk - 1;
    for (int i = 1; i < m->ndisk; i++) {
        n->data_gb[i - 1] = (int)((I->d[i].vsize + (1LL << 30) - 1) >> 30);
        if (n->data_gb[i - 1] < VM_DISK_MIN_GB) n->data_gb[i - 1] = VM_DISK_MIN_GB;
        if (vm_serial_valid(I->d[i].serial)) memcpy(n->data_serial[i - 1], I->d[i].serial, strlen(I->d[i].serial) + 1);
    }
    n->accel = h->arch == m->arch && h->kvm ? VM_ACCEL_KVM : VM_ACCEL_TCG;
    const char *code = NULL, *vars = NULL;
    if (n->firmware == VM_FW_UEFI) {
        if (firmware_for(m->arch, &code, &vars)) { snprintf(why, wn, "no UEFI firmware for %s is installed (%s)", vm_arch_name(m->arch), m->arch == VM_ARCH_ARM64 ? "qemu-efi-aarch64" : "ovmf"); return -1; }
        snprintf(n->firmware_code, sizeof n->firmware_code, "%s", code);
        if (sha256_file(code, n->firmware_sha256)) { snprintf(why, wn, "can't read %s", code); return -1; }
    } else { n->firmware_code[0] = 0; n->firmware_sha256[0] = 0; }
    if (!file_exists(QEMU_BIN_OF(m->arch))) { snprintf(why, wn, "%s isn't installed", QEMU_BIN_OF(m->arch)); return -1; }
    if (machine_type_for(m->arch, n->machine_type, sizeof n->machine_type)) { snprintf(why, wn, "can't read QEMU's machine types"); return -1; }
    char err[256];
    if (vm_machine_check(n, err, sizeof err)) { snprintf(why, wn, "%s", err); return -1; }
    return 0;
}

static void print_plan(const Imp *I, const VmMachine *n, const VmHost *h, int ok, const char *why) {
    const VmiMachine *m = &I->m;
    const char *const KIND[] = { "", "vmx", "vbox", "pvs", I->ova[0] ? "ova" : "ovf", "disk" };
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"kind\":\"%s\",\"name\":", I->ova[0] && m->kind == VMI_OVF ? "ova" : KIND[m->kind]);
    jb_str(&b, ok ? n->name : m->name);
    jb_raw(&b, ",\"source\":"); jb_str(&b, I->settings[0] ? I->settings : I->ova[0] ? I->ova : I->bundle);
    jb_raw(&b, ",\"guest_os\":"); jb_str(&b, m->guest_os);
    jb_printf(&b, ",\"windows\":%s,\"arch\":\"%s\",\"emulated\":%s,\"firmware\":\"%s\",\"secure_boot\":%s",
              m->windows ? "true" : "false", vm_arch_name(m->arch), h->arch != m->arch || !h->kvm ? "true" : "false",
              (ok ? n->firmware : m->firmware) == VM_FW_UEFI ? "uefi" : "bios", m->secure_boot ? "true" : "false");
    jb_printf(&b, ",\"vcpus\":%d,\"sockets\":%d,\"cores\":%d,\"threads\":%d,\"memory_mb\":%d", m->vcpus, m->sockets, m->cores, m->threads, m->memory_mb);
    if (ok) jb_printf(&b, ",\"vcpus_here\":%d,\"memory_mb_here\":%d,\"devices\":\"%s\",\"nic_model\":\"%s\",\"rtc\":\"%s\",\"uuid_here\":\"%s\"",
                      n->vcpus, n->memory_mb, VM_DEV_NAMES[n->devices], VM_NIC_MODELS[n->nic_model], n->rtc ? "local" : "utc", n->id);
    jb_raw(&b, ",\"uuid\":"); jb_str(&b, m->uuid);
    jb_raw(&b, ",\"uuid_how\":"); jb_str(&b, m->uuid_how);
    jb_raw(&b, ",\"smbios\":{\"manufacturer\":"); jb_str(&b, m->sm_manufacturer);
    jb_raw(&b, ",\"product\":"); jb_str(&b, m->sm_product);
    jb_raw(&b, ",\"version\":"); jb_str(&b, m->sm_version);
    jb_raw(&b, ",\"serial\":"); jb_str(&b, m->sm_serial);
    jb_raw(&b, ",\"sku\":"); jb_str(&b, m->sm_sku);
    jb_raw(&b, ",\"family\":"); jb_str(&b, m->sm_family);
    jb_raw(&b, "},\"nics\":[");
    for (int i = 0; i < m->nnic; i++) {
        jb_printf(&b, "%s{\"mac\":\"%s\",\"model\":\"%s\",\"src_model\":", i ? "," : "", m->nic[i].mac, m->nic[i].model);
        jb_str(&b, m->nic[i].src_model);
        jb_printf(&b, ",\"connected\":%s,\"first\":%s}", m->nic[i].connected ? "true" : "false", i == 0 ? "true" : "false");
    }
    jb_raw(&b, "],\"disks\":[");
    long long need = 0;
    for (int i = 0; i < m->ndisk; i++) {
        const char *f = I->d[i].n ? strrchr(I->d[i].file[I->d[i].n - 1], '/') : NULL;
        jb_printf(&b, "%s{\"file\":", i ? "," : "");
        jb_str(&b, I->ova[0] ? m->disk[i].chain[0].path : f ? f + 1 : m->disk[i].chain[0].path);
        jb_printf(&b, ",\"format\":\"%s\",\"layers\":%d,\"vsize\":%lld,\"used\":%lld,\"bus\":\"%s\",\"serial\":", fmt_name(I->d[i].n ? I->d[i].fmt[0] : m->disk[i].chain[0].fmt),
                  I->d[i].n, I->d[i].vsize, I->d[i].used, m->disk[i].bus);
        jb_str(&b, I->d[i].serial);
        jb_printf(&b, ",\"serial_kept\":%s}", I->d[i].serial[0] && vm_serial_valid(I->d[i].serial) ? "true" : "false");
        need += I->d[i].used * (I->d[i].flatten ? 2 : 1);     /* a merge writes a temporary raw file first */
    }
    jb_printf(&b, "],\"snapshots\":%d,\"saved_state\":%s,\"tpm\":%s,\"encrypted\":%s,\"needs_bytes\":%lld", m->snapshots, m->saved_state ? "true" : "false",
              m->tpm ? "true" : "false", m->encrypted ? "true" : "false", need);
    jb_printf(&b, ",\"blockers\":[%s", I->blocker);
    if (!ok && why && why[0]) { jb_printf(&b, "%s{\"code\":\"error\",\"text\":", I->blocker[0] ? "," : ""); jb_str(&b, why); jb_raw(&b, "}"); }
    jb_printf(&b, "],\"notes\":[%s]}\n", I->notes);
    if (!b.oom) fputs(b.p, stdout);
    jb_free(&b);
}

static int arg_arch(const char *v) { return !strcmp(v, "arm64") ? VM_ARCH_ARM64 : !strcmp(v, "amd64") ? VM_ARCH_AMD64 : -2; }

static int cmd_import_plan(const char *src, int argc, char **argv) {
    int arch = -1;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--arch") && i + 1 < argc) { arch = arg_arch(argv[++i]); if (arch == -2) return 2; }
        else { fprintf(stderr, "lord-vm: unknown option %s\n", argv[i]); return 2; }
    }
    static Imp I;
    char why[400] = "";
    if (imp_detect(src, &I, arch, why, sizeof why)) { printf("{\"kind\":\"\",\"blockers\":[{\"code\":\"error\",\"text\":"); print_json_str(why); printf("}],\"notes\":[]}\n"); return 1; }
    VmHost h;
    vm_host_probe(&h);
    VmMachine n;
    int ok = !I.blocker[0] && !imp_machine(&I, &n, &h, VM_DEV_COMPAT, VM_NET_SHARED, -1, 0, 0, why, sizeof why);
    print_plan(&I, &n, &h, ok, why);
    return ok ? 0 : 1;
}

/* ---- merging VDI and .hds snapshot layers (in a jailed child of lord-vm) ---- */

typedef struct { int fd; long long size; uint32_t *map; uint32_t nmap; uint64_t data_off, block, mult; int vdi; } Layer;

static int layer_open(Layer *L, const char *path, int fmt, char *why, size_t wn) {
    memset(L, 0, sizeof *L);
    L->fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (L->fd < 0 || fstat(L->fd, &st)) { snprintf(why, wn, "can't open a layer"); return -1; }
    L->size = st.st_size;
    uint8_t h[512];
    if (pread(L->fd, h, sizeof h, 0) < 64) { snprintf(why, wn, "a layer is cut short"); return -1; }
    #define LE32(p) ((uint32_t)(p)[0] | (uint32_t)(p)[1] << 8 | (uint32_t)(p)[2] << 16 | (uint32_t)(p)[3] << 24)
    uint64_t mapoff;
    if (fmt == VMF_VDI) {
        L->vdi = 1;
        uint32_t bmap = LE32(h + 340), boff = LE32(h + 344), bsize = LE32(h + 376), extra = LE32(h + 380), blocks = LE32(h + 384);
        if (LE32(h + 360) != 512 || extra || bsize < 512 || bsize > (64u << 20) || (bsize & 511) || !blocks || blocks > (16u << 20)) { snprintf(why, wn, "a VDI layer has a layout that isn't supported"); return -1; }
        L->nmap = blocks; L->block = bsize; L->data_off = boff; mapoff = bmap;
    } else {
        int ext = !memcmp(h, "WithouFreSpacExt", 16);
        uint32_t tracks = LE32(h + 28), bat = LE32(h + 32);
        if (LE32(h + 16) != 2 || !tracks || tracks > (1u << 16) || !bat || bat > (16u << 20)) { snprintf(why, wn, "an .hds layer has a layout that isn't supported"); return -1; }
        L->nmap = bat; L->block = (uint64_t)tracks * 512; L->mult = ext ? tracks : 1; mapoff = 64;
    }
    if (mapoff + (uint64_t)L->nmap * 4 > (uint64_t)L->size) { snprintf(why, wn, "a layer's block map is cut short"); return -1; }
    L->map = malloc((size_t)L->nmap * 4);
    if (!L->map || pread(L->fd, L->map, (size_t)L->nmap * 4, (off_t)mapoff) != (ssize_t)L->nmap * 4) { snprintf(why, wn, "a layer's block map can't be read"); return -1; }
    for (uint32_t i = 0; i < L->nmap; i++) { uint8_t *p = (uint8_t *)&L->map[i]; L->map[i] = LE32(p); }
    return 0;
}

/* where block b of layer L is: >0 its offset in the file, 0 not in this layer (look below), -1 zeros, -2 broken */
static long long layer_where(const Layer *L, uint32_t b) {
    uint32_t e = L->map[b];
    long long off;
    if (L->vdi) {
        if (e == 0xffffffffu) return 0;
        if (e == 0xfffffffeu) return -1;
        off = (long long)L->data_off + (long long)e * (long long)L->block;
    } else {
        if (!e) return 0;
        off = (long long)e * (long long)L->mult * 512;
    }
    return off > 0 && off + (long long)L->block <= L->size ? off : -2;
}

static int flatten(const Imp *I, int di, const char *out, long long vsize, char *why, size_t wn) {
    const typeof(I->d[0]) *D = &I->d[di];
    Layer L[VMI_MAX_CHAIN];
    int n = D->n;
    if (vsize <= 0 || vsize > ((long long)VM_DISK_MAX_GB << 30)) { snprintf(why, wn, "a disk's size is out of range"); return -1; }
    long long entries = 0;
    for (int k = 0; k < n; k++) {
        if (layer_open(&L[k], D->file[k], D->fmt[k], why, wn)) return -1;
        entries += L[k].nmap;
        /* every layer's block table is held at once: 64 M entries (256 MB) at most */
        if (entries > (64LL << 20)) { snprintf(why, wn, "a disk's snapshot layers are too many or too large to merge"); return -1; }
    }
    for (int k = 1; k < n; k++) if (L[k].block != L[0].block || L[k].nmap != L[0].nmap) { snprintf(why, wn, "a disk's snapshot layers don't line up"); return -1; }
    if ((long long)L[0].nmap * (long long)L[0].block < vsize) { snprintf(why, wn, "a disk's block map is smaller than the disk"); return -1; }
    int o = open(out, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (o < 0 || ftruncate(o, vsize)) { snprintf(why, wn, "can't make the merged disk"); return -1; }
    char *buf = malloc((size_t)L[0].block);
    if (!buf) return -1;
    uint32_t blocks = (uint32_t)((vsize + (long long)L[0].block - 1) / (long long)L[0].block);
    double last = -1;
    for (uint32_t b = 0; b < blocks; b++) {
        long long where = 0;
        int k = n - 1;
        for (; k >= 0; k--) { where = layer_where(&L[k], b); if (where) break; }
        if (where == -2) { snprintf(why, wn, "a disk's block map points outside its file"); free(buf); close(o); return -1; }
        if (where > 0) {
            long long len = (long long)L[0].block;
            if ((long long)b * len + len > vsize) len = vsize - (long long)b * len;
            if (pread(L[k].fd, buf, (size_t)len, where) != len) { snprintf(why, wn, "a disk can't be read"); free(buf); close(o); return -1; }
            int zero = 1;
            for (long long q = 0; q < len; q++) if (buf[q]) { zero = 0; break; }
            if (!zero && pwrite(o, buf, (size_t)len, (long long)b * (long long)L[0].block) != len) { snprintf(why, wn, "can't write the merged disk"); free(buf); close(o); return -1; }
        }
        double pct = 100.0 * (b + 1) / blocks;
        if (pct - last >= 1 && tool_progress) { tool_progress(pct); last = pct; }
    }
    free(buf);
    if (fsync(o)) { close(o); return -1; }
    close(o);
    for (int k = 0; k < n; k++) { close(L[k].fd); free(L[k].map); }
    return 0;
}

/* runs flatten() in a child jailed like qemu-img: it may read the layers and write only in disks/ */
static int flatten_jailed(const Imp *I, int di, const char *out, const char *dd, long long vsize, char *why, size_t wn) {
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (!pid) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        close(p[0]);
        const char *files[VMI_MAX_CHAIN];
        for (int k = 0; k < I->d[di].n; k++) files[k] = I->d[di].file[k];
        int mask = vm_jail_tool_n(files, I->d[di].n, dd);
        if (!(mask & VMS_NO_NEW_PRIVS) || !(mask & VMS_SECCOMP_OURS) || (vm_landlock_abi() >= 1 && !(mask & VMS_LANDLOCK_FS))) {
            dprintf(p[1], "the merge's sandbox couldn't be set up\n"); _exit(126);
        }
        char w[200] = "";
        int r = flatten(I, di, out, vsize, w, sizeof w);
        if (r) dprintf(p[1], "%s\n", w);
        _exit(r ? 1 : 0);
    }
    close(p[1]);
    char b[256];
    ssize_t r = read(p[0], b, sizeof b - 1);
    close(p[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    if (WIFEXITED(st) && !WEXITSTATUS(st)) return 0;
    if (r > 0) { b[r] = 0; char *nl = strchr(b, '\n'); if (nl) *nl = 0; snprintf(why, wn, "%s", b); }
    else snprintf(why, wn, "merging the snapshots failed");
    return -1;
}

static int imp_disk_i, imp_disks;
static double imp_weight[VMI_MAX_DISKS], imp_base[VMI_MAX_DISKS], imp_stage_w, imp_stage_base;
static void imp_progress(double pct) {
    double total = imp_base[imp_disk_i] + imp_weight[imp_disk_i] * (imp_stage_base + imp_stage_w * pct / 100.0);
    printf("{\"progress\":%.1f,\"disk\":%d,\"disks\":%d}\n", total, imp_disk_i + 1, imp_disks);
    fflush(stdout);
}

/* a loose disk with the same identity as a machine in the library is fine (a Duplicate's rule applies at start) */
static int cmd_import_machine(const char *src, int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "lord-vm: --import-machine SRC DIR [--name N] [--arch A] [--devices compat|virtio] [--network shared|isolated] [--firmware uefi|bios] [--vcpus N] [--memory MB]\n"); return 2; }
    const char *dst = argv[0], *name = NULL;
    int arch = -1, devices = VM_DEV_COMPAT, network = VM_NET_SHARED, fw = -1, vcpus = 0, mem = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) { fprintf(stderr, "lord-vm: %s needs a value\n", a); return 2; }
        i++;
        if (!strcmp(a, "--name")) name = v;
        else if (!strcmp(a, "--arch")) { arch = arg_arch(v); if (arch == -2) return 2; }
        else if (!strcmp(a, "--devices")) { devices = parse_enum(v, "virtio", "compat"); if (devices < 0) return 2; }
        else if (!strcmp(a, "--network")) { network = parse_enum(v, "shared", "isolated"); if (network < 0) return 2; }
        else if (!strcmp(a, "--firmware")) { fw = parse_enum(v, "uefi", "bios"); if (fw < 0) return 2; }
        else if (!strcmp(a, "--vcpus")) vcpus = atoi(v);
        else if (!strcmp(a, "--memory")) mem = atoi(v);
        else { fprintf(stderr, "lord-vm: unknown option %s\n", a); return 2; }
    }
    static Imp I;
    char why[400] = "";
    if (imp_detect(src, &I, arch, why, sizeof why)) { fprintf(stderr, "lord-vm: %s\n", why); return 1; }
    if (I.blocker[0]) {
        /* the first one, in words */
        const char *t = strstr(I.blocker, "\"text\":\"");
        if (strstr(I.blocker, "\"saved-state\"")) fprintf(stderr, "lord-vm: the machine is suspended in the other app: start it there and shut it down first\n");
        else if (strstr(I.blocker, "\"encrypted\"")) fprintf(stderr, "lord-vm: the machine or one of its disks is encrypted: remove the encryption in the other app first\n");
        else if (strstr(I.blocker, "\"in-use\"")) fprintf(stderr, "lord-vm: the other app has the machine open: quit it first\n");
        else if (strstr(I.blocker, "\"too-many-disks\"")) fprintf(stderr, "lord-vm: the machine has more than %d disks\n", VMI_MAX_DISKS);
        else if (strstr(I.blocker, "\"no-disk\"")) fprintf(stderr, "lord-vm: the machine has no disk\n");
        else if (t) { char e[300]; size_t k = 0; for (t += 8; *t && *t != '"' && k + 1 < sizeof e; t++) e[k++] = *t; e[k] = 0; fprintf(stderr, "lord-vm: %s\n", e); }
        else fprintf(stderr, "lord-vm: the machine can't be imported\n");
        return 1;
    }
    VmHost h;
    vm_host_probe(&h);
    VmMachine n;
    if (imp_machine(&I, &n, &h, devices, network, fw, vcpus, mem, why, sizeof why)) { fprintf(stderr, "lord-vm: %s\n", why); return 1; }
    if (name) {
        if (!vm_name_valid(name)) { fprintf(stderr, "lord-vm: the name isn't valid\n"); return 1; }
        snprintf(n.name, sizeof n.name, "%s", name);
    }
    char err[256], warn[256];
    if (vm_machine_check_host(&n, &h, err, sizeof err, warn, sizeof warn)) { fprintf(stderr, "lord-vm: %s\n", err); return 1; }
    /* the new folder: hidden beside where it goes, renamed into place at the end */
    if (!dir_ok(dst) || dst[0] != '/' || !strchr(dst + 1, '/')) { fprintf(stderr, "lord-vm: %s: an absolute folder without ',' or control characters\n", dst); return 2; }
    if (!access(dst, F_OK)) { fprintf(stderr, "lord-vm: %s exists\n", dst); return 3; }
    char lib[PATH_MAX], parent[PATH_MAX], tmp[PATH_MAX + 64] = "";
    snprintf(parent, sizeof parent, "%s", dst);
    char *ds = strrchr(parent, '/');
    *ds = 0;
    if (!realpath(parent[0] ? parent : "/", lib)) { fprintf(stderr, "lord-vm: %s: %s\n", parent, strerror(errno)); return 1; }
    size_t bl = strlen(I.bundle);
    if (I.settings[0] && !strncmp(lib, I.bundle, bl) && (lib[bl] == '/' || !lib[bl])) { fprintf(stderr, "lord-vm: the new machine can't go inside the source's folder\n"); return 1; }
    snprintf(S.dir, sizeof S.dir, "%s", lib);
    clone_sweep(lib);
    char final[PATH_MAX + 64];
    snprintf(final, sizeof final, "%s/%s", lib, ds + 1);
    snprintf(tmp, sizeof tmp, "%s/.%s.part-%d", lib, ds + 1, (int)getpid());
    int rc = 1, lock = -1;
    char p1[PATH_MAX + 128], dd[PATH_MAX + 80];
    if (mkdir(tmp, 0700)) { fprintf(stderr, "lord-vm: can't make %s: %s\n", tmp, strerror(errno)); return 1; }
    snprintf(dd, sizeof dd, "%s/disks", tmp); mkdir(dd, 0700);
    snprintf(p1, sizeof p1, "%s/log", tmp); mkdir(p1, 0700);
    /* what the import did stays with the machine: log/import.log */
    snprintf(p1, sizeof p1, "%s/log/import.log", tmp);
    log_fd = open(p1, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    vlog("import: from %s (%s), %d disk(s), UUID %s (%s)", I.settings[0] ? I.settings : I.ova[0] ? I.ova : I.bundle, I.m.guest_os, I.m.ndisk,
         n.id, I.m.uuid_how[0] ? I.m.uuid_how : "new");
    lock = open_lock(tmp);
    if (vm_op_begin(tmp, "import", NULL, I.settings[0] ? I.settings : I.ova[0] ? I.ova : I.bundle, final)) { fprintf(stderr, "lord-vm: can't write operation.json\n"); goto out; }
    {
        /* free space: what the source's files use, at most; qemu-img writes only what the guest wrote */
        long long need = 0;
        for (int i = 0; i < I.m.ndisk; i++) need += I.d[i].used;
        struct statvfs sv;
        if (!statvfs(lib, &sv) && (long long)sv.f_bavail * (long long)sv.f_frsize < need + (256LL << 20))
            fprintf(stderr, "lord-vm: note: %lld MB free here, the source's disks use up to %lld MB\n", (long long)sv.f_bavail * (long long)sv.f_frsize >> 20, need >> 20);
    }
    long long total = 0;
    imp_disks = I.m.ndisk;
    for (int i = 0; i < I.m.ndisk; i++) total += I.d[i].vsize > 0 ? I.d[i].vsize : 1;
    double acc = 0;
    for (int i = 0; i < I.m.ndisk; i++) { imp_weight[i] = 100.0 * (double)(I.d[i].vsize > 0 ? I.d[i].vsize : 1) / (double)total; imp_base[i] = acc; acc += imp_weight[i]; }
    tool_progress = imp_progress;
    for (int i = 0; i < I.m.ndisk; i++) {
        imp_disk_i = i;
        char out[PATH_MAX + 128], fin[PATH_MAX + 128];
        snprintf(fin, sizeof fin, i ? "%s/disks/data-%d.qcow2" : "%s/disks/system.qcow2", tmp, i);
        snprintf(out, sizeof out, "%s/disks/.import-%d.qcow2", tmp, i);
        vlog("import: disk %d of %d, %s, %d layer(s), %lld bytes", i + 1, I.m.ndisk, fmt_name(I.d[i].fmt[0]), I.d[i].n, I.d[i].vsize);
        if (I.d[i].flatten) {
            char raw[PATH_MAX + 128];
            snprintf(raw, sizeof raw, "%s/disks/.import-%d.raw", tmp, i);
            imp_stage_base = 0; imp_stage_w = 0.6;
            if (flatten_jailed(&I, i, raw, dd, I.d[i].vsize, why, sizeof why)) { fprintf(stderr, "lord-vm: merging a disk's snapshots failed: %s\n", why); unlink(raw); goto out; }
            imp_stage_base = 0.6; imp_stage_w = 0.4;
            char spec[PATH_MAX + 128];
            JBuf b;
            jb_init(&b);
            jb_raw(&b, "json:{\"driver\":\"raw\",\"read-only\":true,\"file\":{\"driver\":\"file\",\"filename\":"); jb_str(&b, raw); jb_raw(&b, "}}");
            snprintf(spec, sizeof spec, "%s", b.oom ? "" : b.p);
            jb_free(&b);
            char *cv[] = { QEMU_IMG, "convert", "-p", "-O", "qcow2", spec, out, NULL };
            int r = run_tool_in(cv, err, sizeof err, raw, dd);
            unlink(raw);
            if (r) { fprintf(stderr, "lord-vm: converting a disk failed: %s\n", err); goto out; }
        } else {
            imp_stage_base = 0; imp_stage_w = 1;
            char *spec = spec_chain(&I, i, 0);
            const char *files[VMI_MAX_CHAIN * VMI_MAX_EXTENTS + VMI_MAX_CHAIN];
            jail_list = files;
            jail_n = jail_files(&I, i, files, (int)(sizeof files / sizeof files[0]));
            char *cv[] = { QEMU_IMG, "convert", "-p", "-O", "qcow2", spec ? spec : "", out, NULL };
            int r = !spec || run_tool_in(cv, err, sizeof err, files[0], dd);
            jail_list = NULL; jail_n = 0;
            free(spec);
            if (r) { fprintf(stderr, "lord-vm: converting a disk failed: %s\n", err); goto out; }
        }
        char w2[256];
        if (disk_plain(out, w2, sizeof w2)) { fprintf(stderr, "lord-vm: the converted disk isn't plain: %s\n", w2); goto out; }
        if (rename(out, fin)) { fprintf(stderr, "lord-vm: can't put a disk in place\n"); goto out; }
        chmod(fin, 0600);
    }
    tool_progress = NULL;
    if (n.firmware == VM_FW_UEFI) {
        const char *code = NULL, *vars = NULL;
        firmware_for(n.arch, &code, &vars);
        char nv[PATH_MAX + 64];
        snprintf(nv, sizeof nv, "%s/disks/nvram.qcow2", tmp);
        char *cv[] = { QEMU_IMG, "convert", "-q", "-f", "raw", "-O", "qcow2", (char *)vars, nv, NULL };
        if (run_tool(cv, err, sizeof err)) { fprintf(stderr, "lord-vm: making the UEFI variables failed: %s\n", err); goto out; }
        chmod(nv, 0600);
    }
    printf("{\"progress\":100.0,\"disk\":%d,\"disks\":%d}\n", imp_disks, imp_disks);
    fflush(stdout);
    if (vm_machine_save(tmp, &n)) { fprintf(stderr, "lord-vm: can't write machine.json\n"); goto out; }
    {
        /* where it came from, for Configure's General and the log; the plan's notes, for the app's "what changed" */
        char ip[PATH_MAX + 64];
        snprintf(ip, sizeof ip, "%s/imported.json", tmp);
        int ifd = open(ip, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        FILE *f = ifd >= 0 ? fdopen(ifd, "w") : NULL;
        if (f) {
            static const char *const KIND[] = { "", "vmx", "vbox", "pvs", "ovf", "disk" };
            fprintf(f, "{\"from\":\"%s\",\"source\":", I.ova[0] ? "ova" : KIND[I.m.kind]);
            JBuf b; jb_init(&b); jb_str(&b, I.settings[0] ? I.settings : I.ova[0] ? I.ova : I.bundle);
            fputs(b.oom ? "\"\"" : b.p, f); jb_free(&b);
            char now[21]; vm_now_iso(now);
            fprintf(f, ",\"at\":\"%s\",\"uuid_how\":", now);
            jb_init(&b); jb_str(&b, I.m.uuid_how); fputs(b.oom ? "\"\"" : b.p, f); jb_free(&b);
            fprintf(f, ",\"guest_os\":");
            jb_init(&b); jb_str(&b, I.m.guest_os); fputs(b.oom ? "\"\"" : b.p, f); jb_free(&b);
            fprintf(f, ",\"windows\":%s,\"notes\":[%s]}\n", I.m.windows ? "true" : "false", I.notes);
            fclose(f);
        }
    }
    if (rename(tmp, final)) { fprintf(stderr, "lord-vm: can't put the machine in place: %s\n", strerror(errno)); goto out; }
    tmp[0] = 0;
    vm_sync_dir(lib);
    vm_op_end(final);
    vlog("import: %s made from %s", final, I.settings[0] ? I.settings : I.bundle);
    printf("{\"imported\":true,\"dir\":");
    print_json_str(final);
    printf(",\"id\":\"%s\",\"devices\":\"%s\",\"nic_model\":\"%s\",\"mac\":\"%s\"}\n", n.id, VM_DEV_NAMES[n.devices], VM_NIC_MODELS[n.nic_model], n.mac);
    rc = 0;
out:
    tool_progress = NULL;
    if (tmp[0]) rm_tree(tmp);
    if (lock >= 0) close(lock);
    for (int i = 0; i < VMI_MAX_DISKS; i++) for (int k = 0; k < VMI_MAX_CHAIN; k++) { free(I.d[i].extents[k]); I.d[i].extents[k] = NULL; }
    return rc;
}

static int usage(void) {
    fprintf(stderr,
            "usage: lord-vm --create DIR [--name N] [--arch arm64|amd64] [--accel kvm|tcg] [--vcpus N] [--memory MB] [--disk GB]\n"
            "                            [--firmware uefi|bios] [--display window|none] [--network shared|isolated|hostonly] [--import IMAGE] [--iso FILE]\n"
            "       lord-vm --set DIR [--name N] [--vcpus N] [--memory MB] [--disk GB] [--display window|none] [--iso FILE|none]\n"
            "                         [--network shared|isolated|bridged|hostonly] [--bridge CARD]   (bridged: a wired card's name)\n"
            "                         [--port tcp|udp:HOST:GUEST[:this-computer|lan] ...] [--ports none]   (forwarded ports, replacing the list)\n"
            "                         [--agent on|off] [--clipboard off|to-guest|from-guest|both] [--graphics 2d|3d]   (also on --create)\n"
            "                         [--boot-order auto|disk,cd,network (any order, any of them)] [--boot-menu on|off]\n"
            "                         [--share ro|rw:TAG:FOLDER ...] [--shares none]   (shared folders, replacing the list)\n"
            "                         [--sound on|off] [--volume 0..100] [--microphone on|off] [--sound-out NODE|default] [--sound-in NODE|default]   (with the machine off; Machines changes them live)\n"
            "                         [--clipboard-ask on|off]   (ask before each paste into and copy out of the machine)\n"
            "                         [--pointer lordos|guest]   (who draws a Windows guest's pointer: LordOS, or its display driver)\n"
            "                         [--capture-fullscreen on|off]   (the window captures the mouse when it goes full screen)\n"
            "                         [--transfer on|off]   (the transfer folder, <library>/Transfer/<name>, tag LordOS)\n"
            "       lord-vm --start DIR [--paused] [--detach] [--control-fd N] [--keep-running] [--dev-qmp] [--credentials-file FILE]\n"
            "                           [--render-node /dev/dri/renderDN|none]   (3D graphics: the GPU QEMU may use; none: 2D)\n"
            "                           [--resume-anyway] [--discard-state]\n"
            "       lord-vm --suspend DIR | --discard-state DIR | --agent DIR shutdown|timesync|install\n"
            "       lord-vm --snapshot DIR take NAME [TAG] | revert TAG | delete TAG | list   (and --start DIR --loadvm TAG)\n"
            "       lord-vm --clone DIR NEWDIR [--name N] [--duplicate] [--linked] [--from-snapshot TAG] [--hostname H | --keep-identity]\n"
            "       lord-vm --compact DIR [--estimate]\n"
            "       lord-vm --import-plan SRC [--arch arm64|amd64]   (a machine of another virtualization app: what comes along)\n"
            "       lord-vm --import-machine SRC DIR [--name N] [--arch A] [--devices compat|virtio] [--network shared|isolated]\n"
            "                                        [--firmware uefi|bios] [--vcpus N] [--memory MB]\n"
            "       lord-vm --status DIR [--json] | --check DIR | --stop DIR [--timeout S] | --poweroff DIR | --pause DIR | --resume DIR | --reset DIR\n"
            "       lord-vm --cdrom DIR IMAGE|none   (a running machine's CD/DVD drive)\n"
            "       lord-vm --qmp DIR 'JSON'   (machines started with --dev-qmp)\n"
            "       lord-vm --sha256 FILE   (an ISO's SHA-256, with progress lines)\n");
    return 2;
}

/* --sha256 FILE: an ISO the person downloaded from its maker's page (Windows, 4.3), hashed to be checked against the
 * catalog's list for it: {"progress":P} lines (a whole percent at a time), then {"sha256":"…","size":N}. A file only:
 * no device, no folder. */
static int cmd_sha256(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) { fprintf(stderr, "lord-vm: %s isn't a file\n", path); if (fd >= 0) close(fd); return 1; }
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    Sha256 s;
    sha256_init(&s);
    static uint8_t buf[1 << 20];
    unsigned long long done = 0;
    int last = -1;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) { if (errno == EINTR) continue; fprintf(stderr, "lord-vm: reading %s: %s\n", path, strerror(errno)); close(fd); return 1; }
        if (!r) break;
        sha256_update(&s, buf, (size_t)r);
        done += (unsigned long long)r;
        int pc = st.st_size > 0 ? (int)(done * 100ull / (unsigned long long)st.st_size) : 100;
        if (pc != last) { last = pc; printf("{\"progress\":%d}\n", pc); fflush(stdout); }
    }
    close(fd);
    uint8_t d[32];
    char hex[65];
    sha256_final(&s, d);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
    printf("{\"sha256\":\"%s\",\"size\":%llu}\n", hex, done);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) return usage();
    if (!strcmp(argv[1], "--sha256")) return cmd_sha256(argv[2]);
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
    if (!strcmp(cmd, "--import-plan")) return cmd_import_plan(dir, rest, rv);
    if (!strcmp(cmd, "--import-machine")) return cmd_import_machine(dir, rest, rv);
    static const struct { const char *name; int type; } C[] = {
        { "--status", VM_STATUS }, { "--stop", VM_SHUTDOWN }, { "--poweroff", VM_POWEROFF }, { "--pause", VM_PAUSE },
        { "--resume", VM_RESUME }, { "--reset", VM_RESET }, { "--qmp", VM_QMP }, { "--cdrom", VM_CDROM }, { "--suspend", VM_SUSPEND }, { "--agent", VM_AGENT },
    };
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) if (!strcmp(cmd, C[i].name)) return cmd_client(dir, C[i].type, rest, rv);
    return usage();
}
