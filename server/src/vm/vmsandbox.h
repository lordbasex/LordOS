/* vmsandbox.h — what lord-vm puts around QEMU, and around itself (docs/VIRTUALIZATION.md 3.3 and "Phase 1
 * implementation").
 *
 * QEMU (in the forked child, right before execve; it lasts across exec and QEMU's own -sandbox stacks on top):
 *   - its own namespaces and a private loader that makes it non-dumpable (vm_ns_enter, vm_ns_setup below);
 *   - no_new_privs; core dumps off (RLIMIT_CORE 0);
 *   - Landlock: read and execute /usr; read /etc/ld.so.cache; read and write only <machine>/disks; read the
 *     attached ISO; read its own /proc/<pid>; /dev/null; /dev/kvm on KVM machines (it arrives as a descriptor, but on
 *     arm64 QEMU also opens it by path to read the host CPU's features). No other file, no other device. ABI 4+: no TCP bind or connect. ABI 6+: no abstract unix sockets or signals outside itself.
 *     A machine with 3D graphics (VmQemuJail.render: only a Linux machine with graphics "3d") also gets, for
 *     virglrenderer and Mesa: read, write and (ABI 5+) ioctls on that render node only; list the folder /dev/dri (names
 *     only); read its GPU's PCI device folder in sysfs (vendor, device, revision, uevent). Never card0 or another
 *     device: the primary node stays refused. Every other machine gets none of it.
 *     A machine with an Intel vGPU (VmQemuJail.vgpu_dev) gets: read, write and ioctls on /dev/vfio/vfio and its own
 *     /dev/vfio/<group> only; read on that mdev's sysfs folder (the real /sys/devices/... path); the iGPU's render node
 *     as above. Our GOP option ROM is under /usr. Seccomp lets it make VFIO's ioctls (type ';') then, and only then.
 *   - seccomp, ours: an allowlist (vmseccomp.h, the rows in vmsyscalls.h), per architecture and per machine (KVM,
 *     TCG, 3D, vGPU, a tap): only the calls QEMU was measured to make, ioctl only on its devices' request types,
 *     threads only, no memory both writable and executable without TCG, signals and limits only on itself, exec only
 *     of QEMU's own binary by descriptor. No socket(2) (EACCES): its sockets are the ones lord-vm passed. Anything
 *     else stops QEMU, and lord-vm names the call (the filter's listener). QEMU's own -sandbox stacks on top.
 * lord-vm itself, once QEMU and passt run: Landlock with read and write in the machine's folder and removal in
 * its runtime folder, nothing else. Its children already exist, so this doesn't reach them. */
#ifndef LORD_VM_SANDBOX_H
#define LORD_VM_SANDBOX_H

#include <sys/types.h>

typedef struct {
    const char *disks_dir;      /* <machine>/disks, read and write */
    const char *iso;            /* attached ISO or NULL, read */
    const char *fw_code;        /* UEFI code file or NULL (already under /usr) */
    const char *base;           /* a linked clone's shared base (6.3), read only, or NULL */
    const char *drivers;        /* a second disc with the guest's drivers (virtio-win), read, or NULL */
    int kvm;                    /* KVM machine: /dev/kvm may be opened again (arm64's -cpu host makes a scratch vCPU
                                 * by path to read the host's features); QEMU holds a KVM descriptor already */
    const char *render;         /* 3D graphics (6.10, only a Linux machine with graphics "3d"): the GPU's render node,
                                 * /dev/dri/renderD<N>, never a primary card node; NULL for every other machine.
                                 * With an Intel vGPU, the iGPU's render node (QEMU's GL display makes its dmabufs) */
    const char *vgpu_dev;       /* an Intel GVT-g vGPU (docs/MACHINES-GPU.md Mode A): /sys/bus/mdev/devices/<uuid>, or NULL */
    int vgpu_group;             /* its IOMMU group: /dev/vfio/<group> and the container /dev/vfio/vfio; -1 none */
    const char *vgpu_rom;       /* its option ROM, read (the package's is under /usr already; a test build's may not be) */
    /* the seccomp allowlist (vmseccomp.h) */
    int tcg;                    /* TCG: its code buffer is writable and executable */
    int tap;                    /* a bridged network: the tap's TUN ioctls */
    int exec_fd;                /* the descriptor of QEMU's binary, the only thing execveat may run then (-1: none) */
    int learn;                  /* 0; a test build's learn modes (1, 2) */
    const char *sc_drop;        /* NULL; a test build's row left out (vmseccomp.h) */
    int *notify_out;            /* NULL: kill as the default action; else the filter's listener comes back here
                                 * (cloexec; -1 none), for lord-vm alone: the child sends it and closes it before exec */
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
/* the same with any number of files it may read (an import: a VMDK descriptor and its extents, a chain of snapshot
 * layers); every one must exist, or nothing is allowed */
int vm_jail_tool_n(const char *const *reads, int nreads, const char *rw_dir);
/* QEMU's namespaces and its private loader (approved by the maintainer 2026-11-23; docs/VIRTUALIZATION.md 3.3).
 *
 * Why: the person's other programs run as the same uid. Yama (ptrace_scope 1) keeps them from attaching to QEMU, but
 * not from /proc/<qemu>/fd, environ, maps or smaps (read access), so a memfd holding guest RAM could be reopened.
 *
 * Non-dumpable: execve makes a process dumpable again unless its binary or its ELF interpreter is unreadable
 * (would_dump: BINPRM_FLAGS_ENFORCE_NONDUMP). In QEMU's own mount namespace the dynamic loader is an unreadable copy
 * (mode 0100, mounted over the loader's path), so the kernel makes QEMU non-dumpable at exec; its /proc files become
 * root's, and the person's programs (lord-vm included) can't read its memory, fds, environment or maps, nor ptrace it.
 * The kernel ties QEMU's memory to the first user namespace, going up from QEMU's, where the copy's uid and gid are
 * both mapped: the person's gid is never mapped in QEMU's, so that is the computer's own, where only root has
 * CAP_SYS_PTRACE (a namespace the person owns would let any of their programs in again). lord-vm checks it after exec
 * (vm_nondumpable).
 *
 * Namespaces: user (only the person's uid mapped, so D-Bus peer credentials with lord-vmdisplay still match; no
 * group), mount (the private loader), ipc, uts and net (QEMU's sockets are the ones lord-vm passed; its own namespace
 * has only an unconfigured loopback). No pid namespace: GLib (2.84) refuses peer credentials whose pid is 0, which is
 * what QEMU would see of lord-vmdisplay from inside one, and the D-Bus display (picture, input, sound) then can't
 * authenticate (measured 2026-11-23). No cgroup namespace (lord-vm places QEMU). QEMU stays lord-vm's plain child.
 *
 * vm_userns_open: at lord-vm's start, while it is still dumpable (a non-dumpable process can't write its own
 * uid_map): a short-lived child makes the user namespace and maps the uid; lord-vm keeps it as a descriptor (cloexec).
 * -1 (errno) when the kernel refuses (no user namespaces): QEMU then runs as before, and the log says so. */
int vm_userns_open(void);
/* in the forked child, before vm_ns_setup: joins `userns` and makes the mount, ipc, uts and net namespaces. 1 when
 * all of them are there; 0 (why on stderr) when not, and the child then runs QEMU without them */
int vm_ns_enter(int userns);
/* then, before vm_jail_qemu: / private, the private loader (its copy made in tmpdir, a folder on a filesystem that
 * allows exec, or /dev/shm). VMNS_LOADER when it's there; why not on stderr */
enum { VMNS_LOADER = 1 };
int vm_ns_setup(const char *tmpdir);
/* 1 when the person's programs can't read pid's memory any more (its /proc/<pid>/environ is refused to them) */
int vm_nondumpable(pid_t pid);
/* lord-vm, after its children started: VMS_SUPERVISOR_LANDLOCK when applied. xfer_dir (or NULL): the machine's transfer
 * folder (6.7.1), where lord-vm makes, writes and removes the files dropped on the window and the one folder each sits in (never links or renames) */
int vm_jail_self(const char *machine_dir, const char *runtime_dir, const char *xfer_dir);
/* the kernel's Landlock ABI (0 none) */
int vm_landlock_abi(void);

#endif
