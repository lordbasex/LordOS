/* vmsandbox.h — what lord-vm puts around QEMU, and around itself (docs/VIRTUALIZATION.md 3.3 and "Phase 1
 * implementation").
 *
 * QEMU (in the forked child, right before execve; it lasts across exec and QEMU's own -sandbox stacks on top):
 *   - no_new_privs; core dumps off (RLIMIT_CORE 0);
 *   - Landlock: read and execute /usr; read /etc/ld.so.cache; read and write only <machine>/disks; read the
 *     attached ISO; read its own /proc/<pid>; /dev/null; /dev/kvm on KVM machines (it arrives as a descriptor, but on
 *     arm64 QEMU also opens it by path to read the host CPU's features). No other file, no other device. ABI 4+: no TCP bind or connect. ABI 6+: no abstract unix sockets or signals outside itself.
 *   - seccomp, ours: no socket(2) at all (EACCES), no connect, bind, listen or accept (its sockets are the
 *     socketpairs lord-vm passed), no namespaces (unshare, setns, clone with CLONE_NEW*), no mount, ptrace,
 *     process_vm_*, bpf, perf_event_open, keyctl, io_uring, userfaultfd, module or kexec calls, no TIOCSTI.
 *     A denylist: QEMU's own allowlist (-sandbox on,...) is applied by QEMU after it starts.
 * lord-vm itself, once QEMU and passt run: Landlock with read and write in the machine's folder and removal in
 * its runtime folder, nothing else. Its children already exist, so this doesn't reach them. */
#ifndef LORD_VM_SANDBOX_H
#define LORD_VM_SANDBOX_H

typedef struct {
    const char *disks_dir;      /* <machine>/disks, read and write */
    const char *iso;            /* attached ISO or NULL, read */
    const char *fw_code;        /* UEFI code file or NULL (already under /usr) */
    const char *base;           /* a linked clone's shared base (6.3), read only, or NULL */
    int kvm;                    /* KVM machine: /dev/kvm may be opened again (arm64's -cpu host makes a scratch vCPU
                                 * by path to read the host's features); QEMU holds a KVM descriptor already */
} VmQemuJail;

/* In the child, before execve. Returns the VMS_* bits that applied; the caller refuses to exec without
 * VMS_SECCOMP_OURS and VMS_NO_NEW_PRIVS. */
int vm_jail_qemu(const VmQemuJail *j);
/* qemu-img on an image the person chose (import): no_new_privs, the same seccomp as QEMU (no sockets), Landlock
 * with read and execute /usr, read `read_file` only, read and write (and make files) only in `rw_dir` (NULL: none),
 * no TCP. In the child, before execve; returns VMS_* bits, and the caller refuses to exec without
 * VMS_NO_NEW_PRIVS and VMS_SECCOMP_OURS, and without VMS_LANDLOCK_FS where the kernel has Landlock. */
int vm_jail_tool(const char *read_file, const char *rw_dir);
/* the same with a second file it may read (a linked clone's base, to flatten or measure it) */
int vm_jail_tool2(const char *read_file, const char *read_file2, const char *rw_dir);
/* lord-vm, after its children started: VMS_SUPERVISOR_LANDLOCK when applied */
int vm_jail_self(const char *machine_dir, const char *runtime_dir);
/* the kernel's Landlock ABI (0 none) */
int vm_landlock_abi(void);

#endif
