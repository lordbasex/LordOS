/* vmsyscalls.h — the system calls QEMU may make, per architecture (vmseccomp.h; docs/VIRTUALIZATION.md 3.3).
 *
 * Approved by the maintainer 2026-11-23 ("isolation layer 3"): an allowlist for every machine's QEMU, replacing the
 * denylist. The rows are what Debian 13's QEMU (1:10.0.13) was measured to make under its jail with lord-vm's learn-all
 * mode (every call that isn't HOT goes to lord-vm and is logged; 2026-11-23): amd64 KVM and TCG with 2D, KVM with 3D on
 * an AMD and an Intel render node, a Windows-style machine (TPM, secure boot, Hyper-V, USB tablet, SATA discs) at its
 * installer, SeaBIOS with compat devices, arm64 KVM, and amd64 TCG on arm64; each through boot, resize, keys, clipboard,
 * sound and microphone, a shared folder, a snapshot with memory and its revert, reset, suspend to a file and resume,
 * a CD change, passt restarted, shutdown and poweroff. A row marked "(kept)" wasn't seen in those runs; its comment
 * says why it stays. Paths that couldn't run here (an Intel vGPU, a bridged tap, hugetlb memory) were read in QEMU's
 * source and need no other call than the rows (their ioctl types are in vmseccomp.c).
 * Anything that isn't a row stops QEMU, and lord-vm names it in its log and on the machine's failure page.
 *
 * One X(name, rule) per call; `name` is the kernel's (__NR_<name>), so a row that doesn't exist on an architecture
 * doesn't compile there (tests/vmseccomp_test.c builds both tables). Rules (vmseccomp.c says how each is checked):
 *   OK        allowed, any arguments
 *   HOT       allowed, any arguments; also allowed in the learn-all mode (test builds), where it would be too slow
 *   EACCES / EPERM / ENOSYS   refused with that error and never run; QEMU, glibc or Mesa cope
 *   CLONE     threads only (QEMU's own spawn=deny rule): CLONE_THREAD|VM|FS|FILES|SIGHAND|SYSVSEM, no namespace, no
 *             vfork, no CLONE_PARENT/PTRACE/PIDFD/IO/UNTRACED; anything else EPERM (glibc's fork falls here)
 *   IOCTL     a few fixed requests, and the request types of the machine's devices (vmseccomp.c: KVM, DRM, VFIO, TUN)
 *   PRCTL     the options measured, PR_SET_DUMPABLE refused (QEMU stays non-dumpable)
 *   SOCKETPAIR AF_UNIX only
 *   MMAP / MPROTECT   no memory both writable and executable unless the machine runs TCG (its code buffer)
 *   PRLIMIT   pid 0 or QEMU's own (never another process's limits)
 *   TGKILL    QEMU's own pid (a signal to one of its threads: the vCPU kick)
 *   SECCOMP   never a filter with a listener (QEMU's own -sandbox adds one without)
 *   EXECVEAT  only lord-vm's descriptor of the QEMU binary, AT_EMPTY_PATH (the one exec; execve is not a row) */
#ifndef LORD_VM_SYSCALLS_H
#define LORD_VM_SYSCALLS_H

/* both architectures */
#define VM_SC_COMMON(X) \
    /* the hot paths: vCPU threads, the main loop, the block layer and its iothread, the D-Bus display */ \
    X(read, HOT) X(write, HOT) X(readv, HOT) X(writev, HOT) X(pread64, HOT) X(pwrite64, HOT) \
    X(preadv, HOT) X(pwritev, HOT) X(preadv2, HOT) X(pwritev2, HOT) X(ppoll, HOT) X(futex, HOT) \
    X(recvmsg, HOT) X(sendmsg, HOT) X(io_submit, HOT) X(io_getevents, HOT) X(ioctl, IOCTL) \
    /* aio=native, the event loop */ \
    X(io_setup, OK) X(io_destroy, OK) X(eventfd2, OK) X(signalfd4, OK) X(epoll_create1, OK) \
    /* (kept) QEMU's event loop moves to epoll past 64 descriptors (util/fdmon-epoll.c) */ \
    X(epoll_ctl, OK) X(epoll_pwait, OK) \
    /* io_uring: never (not approved); QEMU probes it at start and keeps its other event loop */ \
    X(io_uring_setup, EPERM) X(io_uring_enter, EPERM) X(io_uring_register, EPERM) \
    /* files: Landlock decides which */ \
    X(openat, OK) X(close, OK) X(fcntl, OK) X(lseek, OK) X(newfstatat, OK) X(fstat, OK) X(fstatfs, OK) X(statfs, OK) \
    X(getdents64, OK) X(ftruncate, OK) X(fallocate, OK) X(fdatasync, OK) X(fadvise64, OK) X(pipe2, OK) \
    X(memfd_create, OK) \
    /* its sockets are the ones lord-vm passed (QMP, serial, network, agent, D-Bus display, shares, TPM) */ \
    X(socket, EACCES) X(socketpair, SOCKETPAIR) X(sendto, OK) X(recvfrom, OK) X(setsockopt, OK) X(getsockopt, OK) \
    X(getsockname, OK) X(getpeername, OK) \
    /* (kept) GIO shuts a socket down when a D-Bus connection closes */ \
    X(shutdown, OK) \
    /* memory */ \
    X(brk, OK) X(mmap, MMAP) X(mprotect, MPROTECT) X(munmap, OK) X(madvise, OK) \
    /* (kept) glibc's realloc of a large block */ \
    X(mremap, OK) \
    /* threads and signals */ \
    X(clone, CLONE) X(clone3, ENOSYS) X(set_robust_list, OK) X(set_tid_address, OK) X(rseq, OK) X(exit, OK) \
    X(exit_group, OK) X(gettid, OK) X(getpid, OK) X(getppid, OK) X(tgkill, TGKILL) X(rt_sigaction, OK) \
    X(rt_sigprocmask, OK) X(rt_sigreturn, OK) \
    /* (kept) the kernel's restart of a sleep a signal interrupted (the vCPU kick is a signal) */ \
    X(restart_syscall, OK) \
    /* time and scheduling; (kept) clock_gettime, clock_getres, gettimeofday: the vDSO's fallback on a host whose clock \
     * it can't read; sched_yield: Mesa's fence waits */ \
    X(clock_nanosleep, OK) X(clock_gettime, OK) X(clock_getres, OK) X(gettimeofday, OK) X(sched_getaffinity, OK) \
    X(sched_yield, OK) \
    /* who and what it runs on (getuid, getgid, sysinfo, getdents64: Mesa, 3D machines) */ \
    X(getuid, OK) X(geteuid, OK) X(getgid, OK) X(getegid, OK) X(uname, OK) X(sysinfo, OK) X(getrandom, OK) \
    X(prlimit64, PRLIMIT) X(prctl, PRCTL) \
    /* kcmp: Mesa (amdgpu) compares two descriptors with it and copes without; refused as before (it can compare \
     * other processes' descriptors) */ \
    X(kcmp, EPERM) \
    /* QEMU's own -sandbox (libseccomp), and its one exec */ \
    X(seccomp, SECCOMP) X(execveat, EXECVEAT)

/* amd64 only: what glibc calls there (arm64 has the *at forms below); (kept) epoll_wait: the event loop's epoll, as
 * above; time: the vDSO's fallback */
#define VM_SC_AMD64(X) \
    X(access, OK) X(readlink, OK) X(poll, OK) X(arch_prctl, OK) X(epoll_wait, OK) X(time, OK)

/* arm64 only */
#define VM_SC_ARM64(X) \
    X(faccessat, OK) X(readlinkat, OK)

#endif
