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
#include "vmimport.h"
#include "vmunattend.h"
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
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <strings.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef VM_LOGIND
#include <systemd/sd-bus.h>
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
#define QEMU_BIN_OF(a) test_path((a) ? "LORDVM_TEST_QEMU_AMD64" : "LORDVM_TEST_QEMU_ARM64", QEMU_BIN[a])
#else
#define QEMU_IMG "/usr/bin/qemu-img"
#define PASST    "/usr/bin/passt"
#define VIRTIOFSD "/usr/libexec/virtiofsd"
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

#define LOG_CAP      (4 << 20)
#define SERIAL_CAP   (1 << 20)
#define MAX_CLIENTS  8
#define MAX_PENDING  64
#define MAX_CREDS    16

static const char *const STATE_NAME[VS_COUNT] = { "stopped", "starting", "running", "paused", "stopping", "crashed" };
static const char *const REASON_NAME[VR_COUNT] = {
    "none", "guest-shutdown", "host-shutdown", "forced", "poweroff", "qemu-failed", "qemu-signal", "out-of-memory",
    "start-failed", "guest-panic", "supervisor-lost", "reset", "user", "suspended", "resume-failed", "installed",
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
    discard_state(dir);
    vm_op_end(dir);
    return 0;
}

static int cmd_create(const char *dir, int argc, char **argv) {
    VmHost h;
    vm_host_probe(&h);
    int arch = h.arch >= 0 ? h.arch : VM_ARCH_AMD64;
    const char *name = NULL, *import = NULL, *iso = NULL, *unatt = NULL;
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
        else if (!strcmp(a, "--unattended")) unatt = v;
        else if (!strcmp(a, "--agent")) { agent = parse_enum(v, "off", "on"); if (agent < 0) { fprintf(stderr, "lord-vm: --agent on|off\n"); return 2; } }
        else if (!strcmp(a, "--clipboard")) { clip = parse_clip(v); if (clip < 0) { fprintf(stderr, "lord-vm: --clipboard off|to-guest|from-guest|both\n"); return 2; } }
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
    if (fw < 0) fw = firmware_for(arch, &code, &vars) ? VM_FW_BIOS : VM_FW_UEFI;
    m.firmware = fw;
    if (fw == VM_FW_UEFI) {
        if (firmware_for(arch, &code, &vars)) { fprintf(stderr, "lord-vm: no UEFI firmware for %s is installed (%s)\n", vm_arch_name(arch), arch == VM_ARCH_ARM64 ? "qemu-efi-aarch64" : "ovmf"); return 1; }
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
        if (vmu_prepare(dd, m.iso, &un, m.id, why, sizeof why)) {
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
    pid_t vfs[VM_SHARES];                   /* shared folders (6.7): each one's virtiofsd, and QEMU's end of its socket */
    int vfs_q[VM_SHARES];
    off_t serial_bytes;
    int lock_fd;
    int sandbox, cg_mode;
    int memev_fd;
    long oom0;
    int host_shutdown, poweroff, forced, guest_shutdown, panic, killed;
    int unattended, install_done;        /* the first start of an unattended install (disks/unattended), and its end */
    char un_cmd[640];
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
    long long ag_next, sync_at, ping_at, journal_at, shutdown_at, acpi_at;
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

static int qemu_procs_fd = -1, passt_procs_fd = -1, vfs_procs_fd = -1;

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
    if (S.m.n_shares && (!mkdir(v, 0755) || errno == EEXIST)) {
        /* the shared folders' virtiofsd: 256 MB and two CPUs together (they cache file data in the guest, not here) */
        cg_write(v, "memory.max", "268435456");
        cg_write(v, "cpu.max", "200000 100000");
        char vf[PATH_MAX + 64];
        snprintf(vf, sizeof vf, "%s/cgroup.procs", v);
        vfs_procs_fd = open(vf, O_WRONLY | O_CLOEXEC);
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
    int dfd = open_share(S.m.share_path[i], err, errlen);
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
                         "--migration-on-error=guest-error", "--log-level=warn", S.m.share_rw[i] ? NULL : "--readonly", NULL };
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
    vlog("share %d: virtiofsd pid %d, tag %s, %s, %s", i + 1, (int)pid, S.m.share_tag[i], S.m.share_rw[i] ? "writable" : "read-only", S.m.share_path[i]);
    return c;
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
    for (int i = 0; i < S.m.n_shares; i++) {
        if (vm_share_allowed(S.m.share_path[i], home, pw->pw_name, lib, err, sizeof err)) { fail_start("shared folder %s: %s", S.m.share_tag[i], err); return -1; }
        S.vfs_q[i] = spawn_virtiofsd(i, err, sizeof err);
        if (S.vfs_q[i] < 0) { fail_start("shared folder %s: %s", S.m.share_tag[i], err); return -1; }
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
    arg("-machine"); argf("%s,dump-guest-core=off%s%s", m->machine_type, uefi ? ",pflash0=fw-code,pflash1=fw-vars" : "", m->n_shares ? ",memory-backend=mem" : "");
    if (m->n_shares) { arg("-object"); argf("memory-backend-memfd,id=mem,size=%dM,share=on", m->memory_mb); }
    if (kvm) { arg("-accel"); arg("kvm,device=/dev/fdset/1"); arg("-add-fd"); arg("fd=3,set=1,opaque=kvm"); }
    else { arg("-accel"); arg("tcg,thread=multi,tb-size=256"); }
    arg("-cpu"); arg(kvm ? "host" : "max");
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
    /* an unattended install's first start: QEMU gives the installer's kernel (-kernel) boot index 0 */
    const char *bi_sys = S.unattended ? "1" : "0", *bi_cd = S.unattended ? "2" : "1";
    if (m->devices == VM_DEV_COMPAT) {
        /* devices every system has a driver for (6.22): SATA (AHCI) disks and CD/DVD; q35 has its AHCI built in (ide.0
         * to ide.5), arm64's virt gets one in the system disk's slot */
        const char *bus = arm ? "ahci" : "ide";
        if (arm) { arg("-device"); arg("ich9-ahci,id=ahci,addr=0x1"); }
        arg("-device"); argf("ide-hd,id=sysdisk,drive=sys,bus=%s.0,bootindex=%s%s", bus, bi_sys, ser);
        for (int i = 0; i < m->n_data; i++) {
            arg("-device"); argf("ide-hd,id=datadisk%d,drive=data%d,bus=%s.%d%s%s", i + 1, i + 1, bus, i + 1, m->data_serial[i][0] ? ",serial=" : "", m->data_serial[i]);
        }
        if (m->iso[0]) {
            arg("-blockdev"); argj(blockdev_file("cd-file", m->iso, 1));
            arg("-blockdev"); argj(blockdev_fmt("raw", "cd", "cd-file", 1));
            arg("-device"); argf("ide-cd,id=cd0,bus=%s.5,drive=cd,bootindex=%s", bus, bi_cd);
            snprintf(S.cd_node, sizeof S.cd_node, "cd");
        } else { arg("-device"); argf("ide-cd,id=cd0,bus=%s.5,bootindex=%s", bus, bi_cd); S.cd_node[0] = 0; }
    } else {
        arg("-device"); argf("virtio-blk-pci,id=sysdisk,drive=sys,bootindex=%s,addr=0x1%s", bi_sys, ser);
        for (int i = 0; i < m->n_data; i++) {
            arg("-device"); argf("virtio-blk-pci,id=datadisk%d,drive=data%d,addr=0x%x%s%s", i + 1, i + 1, 0x10 + i, m->data_serial[i][0] ? ",serial=" : "", m->data_serial[i]);
        }
        /* the CD/DVD drive is always there (slot 8, after the ones a machine always had), so a disc can go in and out while
         * the machine runs (VM_CDROM); the ISO of machine.json is in it at start */
        arg("-device"); arg("virtio-scsi-pci,id=scsi0,addr=0x8");
        if (m->iso[0]) {
            arg("-blockdev"); argj(blockdev_file("cd-file", m->iso, 1));
            arg("-blockdev"); argj(blockdev_fmt("raw", "cd", "cd-file", 1));
            arg("-device"); argf("scsi-cd,id=cd0,bus=scsi0.0,drive=cd,bootindex=%s", bi_cd);
            snprintf(S.cd_node, sizeof S.cd_node, "cd");
        } else { arg("-device"); argf("scsi-cd,id=cd0,bus=scsi0.0,bootindex=%s", bi_cd); S.cd_node[0] = 0; }
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
    if (m->network == VM_NET_SHARED) {
        /* The card sits on a hub, and passt on another port of it: when passt ends, a new one is put on a new port
         * (QMP netdev_del, getfd, netdev_add socket + hubport) while the card stays as it was, so the guest keeps its
         * network without a restart (6.5). A card attached straight to its backend can't be given another one. */
        arg("-netdev"); arg("hubport,id=nic0,hubid=0");
        arg("-netdev"); arg("stream,id=pst0,addr.type=fd,addr.str=6");
        arg("-netdev"); arg("hubport,id=hp0,hubid=0,netdev=pst0");
        arg("-device"); argf("%s,netdev=nic0,mac=%s,addr=0x2", NICDEV[m->nic_model], m->mac);
    }
    /* an imported machine's other cards: there with their addresses, so the guest keeps its interfaces; not connected */
    for (int i = 0; i < m->n_extra_macs; i++) { arg("-device"); argf("%s,id=nic%d,mac=%s,addr=0x%x", NICDEV[m->extra_models[i]], i + 1, m->extra_macs[i], 0x18 + i); }
    arg("-device"); arg("virtio-rng-pci,addr=0x3");
    /* amd64 with -nodefaults needs a display device or its kernel never starts (3.3); arm64's virt has no keyboard */
    if (!arm) { arg("-device"); arg("virtio-vga,id=video0,addr=0x4"); }
    if (m->display == VM_DISPLAY_WINDOW && m->devices == VM_DEV_COMPAT) {
        /* a USB tablet (and keyboard on arm64) on xHCI: drivers every system has */
        if (arm) { arg("-device"); arg("virtio-gpu-pci,id=video0,addr=0x4"); }
        arg("-device"); arg("qemu-xhci,id=xhci,addr=0x5");
        if (arm) { arg("-device"); arg("usb-kbd,bus=xhci.0"); }
        arg("-device"); arg("usb-tablet,bus=xhci.0");
        arg("-display"); arg("dbus,p2p=yes");
    } else if (m->display == VM_DISPLAY_WINDOW) {
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
    /* shared folders: each virtiofsd's connection after the credentials, a device on slots 0xb to 0xe */
    for (int i = 0; i < m->n_shares; i++) {
        arg("-chardev"); argf("socket,id=vfs%d,fd=%d", i, (m->agent ? 9 : 7) + S.ncreds + i);
        arg("-device"); argf("vhost-user-fs-pci,id=share%d,chardev=vfs%d,tag=%s,addr=0x%x", i, i, m->share_tag[i], 0xb + i);
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
    S.unattended = 0;
    S.install_done = 0;
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
    }
    if (!file_exists(QEMU_BIN_OF(S.m.arch))) { fail_start("%s isn't installed", QEMU_BIN_OF(S.m.arch)); return -1; }
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
    if (S.m.n_shares && start_shares()) return -1;
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
        int fds[6 + MAX_CREDS + VM_SHARES + 1], nf = 0;
        fds[nf++] = kvm; fds[nf++] = qmp[1]; fds[nf++] = ser[1]; fds[nf++] = net[0] >= 0 ? net[0] : null;
        if (S.m.agent) { fds[nf++] = qga[1]; fds[nf++] = lag[1]; }
        for (int i = 0; i < S.ncreds; i++) fds[nf++] = S.cred_fd[i];
        for (int i = 0; i < S.m.n_shares; i++) fds[nf++] = S.vfs_q[i];
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
    for (int i = 0; i < S.m.n_shares; i++) if (S.vfs_q[i] >= 0) { close(S.vfs_q[i]); S.vfs_q[i] = -1; }
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

static int qga_do(const char *cmd, const char *args, int kind);
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
enum { AQ_IGNORE = 0, AQ_SYNC, AQ_INFO, AQ_OSINFO, AQ_PING, AQ_JEXEC, AQ_JSTATUS, AQ_FOPEN, AQ_FWRITE, AQ_FCLOSE, AQ_INSTALL, AQ_INHIBIT,
       AQ_IDEXEC, AQ_IDSTATUS };

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

static int qga_do(const char *cmd, const char *args, int kind);
static void lag_send(uint16_t kind, const uint8_t *data, uint32_t n);
static void id_begin(void) {
    if (S.id_state != 1 || !(S.ag_caps & VAC_EXEC)) return;
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
    if (S.id_state == 2) { S.id_state = 1; S.id_poll_at = 0; }      /* tried again when the agent is back */
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
        id_begin();
        /* a machine that resumed from a saved state has a clipboard helper that said hello to the lord-vm before: it
         * is asked to say it again (once; its answer gets the plain hello) */
        if (!S.lag_hello && S.lag_fd >= 0) { static const char ask[] = "lord-vm 2 clipboard clipx ask"; lag_send(AGK_HELLO, (const uint8_t *)ask, sizeof ask - 1); }
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
            snprintf(t, sizeof t, "this copy now has its own identity: hostname %s, a new machine-id", S.id_hostname);
            notice_all(VMN_IDENTITY, 0, t);
        } else { S.id_state = 1; vlog("identity: the script failed (%d): %s", code, said); }
        break;
    }
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
        if (S.unattended && why && !strcmp(why, "guest-reset")) { S.install_done = 1; vlog("unattended: the installer restarted the machine: the install is done"); }
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

static void unattended_done(void);
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
        else if (S.install_done) why = VR_INSTALLED;
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
    for (int i = 0; i < VM_SHARES; i++) if (S.vfs[i] > 0) kill(S.vfs[i], SIGTERM);
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

static int vfs_index(pid_t p) { for (int i = 0; i < VM_SHARES; i++) if (p > 0 && S.vfs[i] == p) return i; return -1; }

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
            if (!S.qemu_done && !S.suspending && S.state == VS_RUNNING) vlog("share %d: the folder %s is gone from the machine until it starts again", k + 1, S.m.share_tag[k]);
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
        if (S.net_retry_at && (next < 0 || S.net_retry_at < next)) next = S.net_retry_at;
        if (S.host_stopping && (next < 0 || now + 500 < next)) next = now + 500;
        if (S.id_poll_at && (next < 0 || S.id_poll_at < next)) next = S.id_poll_at;
        if (S.migq_next && (next < 0 || S.migq_next < next)) next = S.migq_next;
        if (S.qga_fd >= 0 && S.ag_next && (next < 0 || S.ag_next < next)) next = S.ag_next;
        if (S.kill_deadline && (next < 0 || S.kill_deadline < next)) next = S.kill_deadline;
        int to = next < 0 ? -1 : next <= now ? 0 : (int)(next - now);
        if (poll(p, (nfds_t)n, to) < 0 && errno != EINTR) { vlog("poll: %s", strerror(errno)); break; }
        now = now_ms();
        if (S.migq_next && now >= S.migq_next) { S.migq_next = now + 500; qmp_do("query-migrate", NULL, P_MIGQ, -1, 0, -1, -1); }
        if (S.qga_fd >= 0 && S.ag_next && now >= S.ag_next) ag_tick(now);
        if (S.net_retry_at && now >= S.net_retry_at) net_restart();
        if (S.id_poll_at && now >= S.id_poll_at) { char a[48]; S.id_poll_at = 0; snprintf(a, sizeof a, "{\"pid\":%ld}", S.id_pid); qga_do("guest-exec-status", a, AQ_IDSTATUS); }
        host_stop_tick(now);
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
        for (int i = 0; i < VM_SHARES; i++) if (S.vfs[i] > 0) { kill(S.vfs[i], SIGKILL); waitpid(S.vfs[i], NULL, 0); S.vfs[i] = 0; }
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
    int vcpus = 0, mem = 0, disk = 0, disp = -1, net = -1, agent = -1, clip = -1, devs = -1, nicm = -1;
    /* shared folders (6.7): --share ro|rw:TAG:PATH, as many as wanted up to VM_SHARES, replace the list; --shares none
     * empties it */
    const char *share[VM_SHARES + 1];
    int nshare = 0, shares_set = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) { fprintf(stderr, "lord-vm: %s needs a value\n", a); return 2; }
        i++;
        if (!strcmp(a, "--share")) {
            if (nshare >= VM_SHARES) { fprintf(stderr, "lord-vm: at most %d shared folders\n", VM_SHARES); return 2; }
            share[nshare++] = v; shares_set = 1;
            continue;
        }
        if (!strcmp(a, "--shares")) { if (strcmp(v, "none")) { fprintf(stderr, "lord-vm: --shares none\n"); return 2; } shares_set = 1; continue; }
        if (!strcmp(a, "--devices")) { devs = parse_enum(v, "virtio", "compat"); if (devs < 0) { fprintf(stderr, "lord-vm: --devices virtio|compat\n"); return 2; } continue; }
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
        if (file_exists(st) && (vcpus || mem || disk || net >= 0 || agent >= 0 || shares_set)) { fprintf(stderr, "lord-vm: the machine is suspended: resume it and shut it down first, or discard its saved state\n"); goto out; }
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
    if (agent >= 0) m.agent = agent;
    if (clip >= 0) m.clipboard = clip;
    if (m.agent || m.clipboard) m.schema = VM_SCHEMA;
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
    printf("{\"set\":true,\"vcpus\":%d,\"memory_mb\":%d,\"disk_gb\":%d,\"network\":\"%s\",\"iso\":%s,\"agent\":%s,\"devices\":\"%s\",\"nic_model\":\"%s\"}\n", m.vcpus, m.memory_mb, m.disk_gb,
           m.network == VM_NET_SHARED ? "shared" : "isolated", m.iso[0] ? "true" : "false", m.agent ? "true" : "false", m.devices ? "compat" : "virtio", VM_NIC_MODELS[m.nic_model]);
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
                      n->vcpus, n->memory_mb, n->devices ? "compat" : "virtio", VM_NIC_MODELS[n->nic_model], n->rtc ? "local" : "utc", n->id);
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
    printf(",\"id\":\"%s\",\"devices\":\"%s\",\"nic_model\":\"%s\",\"mac\":\"%s\"}\n", n.id, n.devices ? "compat" : "virtio", VM_NIC_MODELS[n.nic_model], n.mac);
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
            "                            [--firmware uefi|bios] [--display window|none] [--network shared|isolated] [--import IMAGE] [--iso FILE]\n"
            "       lord-vm --set DIR [--name N] [--vcpus N] [--memory MB] [--disk GB] [--display window|none] [--network shared|isolated] [--iso FILE|none]\n"
            "                         [--agent on|off] [--clipboard off|to-guest|from-guest|both]   (also on --create)\n"
            "                         [--share ro|rw:TAG:FOLDER ...] [--shares none]   (shared folders, replacing the list)\n"
            "       lord-vm --start DIR [--paused] [--detach] [--control-fd N] [--keep-running] [--dev-qmp] [--credentials-file FILE]\n"
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
    if (!strcmp(cmd, "--import-plan")) return cmd_import_plan(dir, rest, rv);
    if (!strcmp(cmd, "--import-machine")) return cmd_import_machine(dir, rest, rv);
    static const struct { const char *name; int type; } C[] = {
        { "--status", VM_STATUS }, { "--stop", VM_SHUTDOWN }, { "--poweroff", VM_POWEROFF }, { "--pause", VM_PAUSE },
        { "--resume", VM_RESUME }, { "--reset", VM_RESET }, { "--qmp", VM_QMP }, { "--cdrom", VM_CDROM }, { "--suspend", VM_SUSPEND }, { "--agent", VM_AGENT },
    };
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) if (!strcmp(cmd, C[i].name)) return cmd_client(dir, C[i].type, rest, rv);
    return usage();
}
