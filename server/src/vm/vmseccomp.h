/* vmseccomp.h — QEMU's seccomp allowlist (approved by the maintainer 2026-11-23; docs/VIRTUALIZATION.md 3.3).
 *
 * The rows are in vmsyscalls.h, one per system call QEMU may make, with a rule for its arguments where that matters
 * (ioctl by the machine's devices, clone threads only, prctl options, without TCG executable memory only as a private read-only file mapping,
 * signals and limits only on itself, one exec, which lord-vm lets through). vm_sc_build turns the rows into a classic
 * BPF program: the architecture checked first (and amd64's x32 numbers refused), then a binary search on the call's
 * number, so a call costs about eight comparisons whatever its place in the table.
 *
 * Anything that isn't a row (or breaks its row's rule) gets the default action: SECCOMP_RET_USER_NOTIF when lord-vm
 * holds the filter's listener (vm_jail_qemu passes it over the status socket; QEMU never keeps it), else
 * SECCOMP_RET_KILL_PROCESS. lord-vm answers a notification by killing QEMU before the call runs and names the call in
 * its log and the failure page ("blocked by the sandbox: <name>"). Should lord-vm be gone, the kernel fails the call
 * with ENOSYS: it never runs either way. QEMU's own -sandbox is stacked on top, and its refusals win over ours.
 *
 * The learn modes exist only in a test build (lord-vm-testpaths, LORDVM_TEST_SECCOMP=learn|learn-all; never in the
 * package): the listener lets the call go on (SECCOMP_USER_NOTIF_FLAG_CONTINUE) and lord-vm logs it once.
 * learn-all also sends every row that isn't HOT to lord-vm, to see which rows a configuration really uses. drop:NAME
 * leaves one row out, so the stop and its name on the failure page can be seen with a real QEMU. */
#ifndef LORD_VM_SECCOMP_H
#define LORD_VM_SECCOMP_H

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* a row's rule (vmsyscalls.h) */
enum { VSR_OK = 1, VSR_HOT, VSR_EACCES, VSR_EPERM, VSR_ENOSYS, VSR_CLONE, VSR_IOCTL, VSR_PRCTL, VSR_SOCKETPAIR,
       VSR_MMAP, VSR_MPROTECT, VSR_MEMFD, VSR_PRLIMIT, VSR_TGKILL, VSR_SECCOMP, VSR_EXECVEAT };

typedef struct { const char *name; int nr; int rule; } VmScRow;

/* what the machine needs beyond every machine's rows */
typedef struct {
    int kvm;        /* KVM: its ioctls (type 0xAE) */
    int tcg;        /* TCG: memory both writable and executable (its code buffer) */
    int render;     /* 3D graphics or a vGPU: the render node's DRM ioctls ('d'), sync_file ('>') and dma-buf ('b') */
    int vgpu;       /* an Intel vGPU: VFIO's ioctls (';') */
    int tap;        /* a bridged network: the tap's TUN ioctls ('T' 200 and up) */
    int bti;        /* arm64: mprotect may add PROT_EXEC together with PROT_BTI (glibc's loader on a CPU with BTI) */
    pid_t self;     /* QEMU's pid (the child's own: exec keeps it) */
    int exec_fd;    /* the descriptor of QEMU's binary, the only thing execveat may run; -1: no exec */
    int notify;     /* 1: the default action is SECCOMP_RET_USER_NOTIF (lord-vm holds the listener); 0: kill */
    int learn;      /* 0; test builds: 1 learn (log, go on), 2 learn-all (every row but HOT goes to lord-vm too) */
    const char *drop; /* NULL; a test build's LORDVM_TEST_SECCOMP=drop:NAME: that row left out, to see a stop named */
} VmScOpts;

/* this computer's rows (the native table) */
const VmScRow *vm_sc_rows(int *n);
/* the BPF program for `rows` on `audit_arch` (AUDIT_ARCH_X86_64 or AUDIT_ARCH_AARCH64): the number of instructions,
 * which may exceed `cap` (nothing is written past it); -1 when a row can't be encoded */
int vm_sc_build_for(const VmScRow *rows, int n, uint32_t audit_arch, const VmScOpts *o, struct sock_filter *f, int cap);
/* the same for this computer; 0 when the architecture has no table */
int vm_sc_build(const VmScOpts *o, struct sock_filter *f, int cap);
/* the kernel's name of call `nr` on this computer, or NULL */
const char *vm_sc_name(int nr);
/* a notification of execveat: 1 when it is QEMU's one exec, the first from `child` (lord-vm answers it with
 * SECCOMP_USER_NOTIF_FLAG_CONTINUE: the child is single-threaded and runs lord-vm's code, so its arguments can't change);
 * *done is set, and every later execveat is 0 (QEMU is stopped) */
int vm_sc_exec_once(const struct seccomp_notif *req, pid_t child, int *done);
/* "name" or "ioctl 0x...", with what lord-vm logs about the arguments; for a notification */
void vm_sc_describe(const struct seccomp_data *d, char *out, size_t n);
/* the key lord-vm's learn mode logs a call once by (the call, and its ioctl request, prctl option or memory rights) */
uint64_t vm_sc_learn_key(const struct seccomp_data *d);

#endif
