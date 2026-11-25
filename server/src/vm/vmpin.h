/* vmpin.h — vCPU pinning (docs/VIRTUALIZATION.md 3.7.1): each of a machine's vCPU threads on its own host CPU, away from
 * the cores the desktop keeps, and QEMU's other threads (the main loop, I/O) on what is left. lord-vm does it to its own
 * child with sched_setaffinity: no root, no new privilege. The choice is pure (vmpin_plan, over a VmTopo) so it is
 * tested with invented computers; vmpin_topo_read reads the real one from sysfs, under a root the tests can point
 * elsewhere. */
#ifndef LORD_VM_VMPIN_H
#define LORD_VM_VMPIN_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <sys/types.h>

#define VMPIN_CPUS   1024           /* CPU_SETSIZE: logical CPUs this knows about */
#define VMPIN_VCPUS  64             /* VM_VCPUS_MAX */
/* a core is "efficient" (a hybrid CPU's E core) under this share of the fastest core's capacity */
#define VMPIN_EFF_PCT 80

typedef struct {
    int n;                          /* entries in c */
    struct { int cpu, pkg, core, cap; } c[VMPIN_CPUS];  /* cap: relative capacity (0: unknown, read as equal) */
    int hybrid;                     /* the capacities differ: P and E cores */
} VmTopo;

/* reads <root>/devices/system/cpu/cpu<N>/topology (physical_package_id, core_id) of the online CPUs, and their capacity:
 * cpu_capacity (arm64 and newer x86), else the cpu_core PMU's list (Intel hybrid: those are P cores), else
 * cpufreq/cpuinfo_max_freq. root is "/sys" outside the tests. 0 ok */
int vmpin_topo_read(const char *root, VmTopo *t);

enum { VMPIN_NONE = 0, VMPIN_EACH };
typedef struct {
    int mode;                       /* VMPIN_NONE: nothing is pinned (why says why) */
    int nv;                         /* vCPUs */
    int vcpu_cpu[VMPIN_VCPUS];      /* VMPIN_EACH: vCPU i runs only on this host CPU */
    cpu_set_t rest;                 /* QEMU's other threads */
    int cores, reserved;            /* physical cores allowed; kept for the desktop */
    int shared;                     /* vCPUs that share a physical core with another vCPU (SMT siblings) */
    char why[120];
} VmPinPlan;

/* Picks the host CPUs for nvcpu vCPUs among `allowed` (lord-vm's cpuset, the CPUs QEMU may run on). `taken`: CPUs other
 * machines' vCPUs are pinned to (NULL none); their cores are left alone. The rules (docs/VIRTUALIZATION.md 3.7.1):
 *  - the desktop keeps 1 physical core (2 when 6 or more are allowed), with their SMT siblings: the lowest-numbered P
 *    cores (CPU 0 takes most interrupts), or E cores when that would leave the machine fewer than 2 P cores;
 *  - the rest are ranked P before E, faster first, then by number; one vCPU per physical core while there are cores,
 *    then the siblings in the same order;
 *  - more vCPUs than those CPUs: nothing is pinned (two vCPUs on one CPU is worse than the scheduler's own choice);
 *  - fewer than 3 physical cores: nothing is pinned.
 * QEMU's other threads get every allowed CPU that no vCPU uses, without the siblings of the vCPUs' cores when that
 * still leaves some. Returns p->mode. */
int vmpin_plan(const VmTopo *t, const cpu_set_t *allowed, const cpu_set_t *taken, int nvcpu, VmPinPlan *p);

/* "auto" when the machine has no choice of its own (machine.json without "cpu_pin"): VMPIN_AUTO_VCPUS vCPUs or more on
 * VMPIN_AUTO_CORES physical cores or more (allowed ones); else off */
#define VMPIN_AUTO_VCPUS 4
#define VMPIN_AUTO_CORES 8
int vmpin_default_on(const VmTopo *t, const cpu_set_t *allowed, int nvcpu);

/* the CPUs vCPU threads of the other QEMU processes of this user are pinned to: a thread whose affinity is a single CPU
 * (what vmpin_apply sets). Reads /proc; call before lord-vm's own Landlock. A QEMU is this user's by the real uid in
 * its status: a non-dumpable one's /proc folder is root's (vmsandbox.h) */
void vmpin_taken(pid_t self_qemu, cpu_set_t *taken);

/* sets the plan on QEMU's threads: tids[i] is vCPU i's thread (query-cpus-fast's thread-id); every other thread listed
 * in task_dfd (an open /proc/<qemu>/task) gets p->rest. Returns the threads set, -1 if a vCPU's couldn't be */
int vmpin_apply(const VmPinPlan *p, int task_dfd, const int *tids, int n);

/* "0,2-4" (at most len bytes) */
void vmpin_list(const cpu_set_t *s, char *out, size_t len);

#endif
