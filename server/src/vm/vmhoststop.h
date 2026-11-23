/* vmhoststop.h — how long each step of a host stop may take (docs/VIRTUALIZATION.md 6.1). The computer's shutdown
 * waits for lord-vm only while logind's delay lasts (InhibitDelayMaxUSec; LordOS sets 30 s); what's still running after
 * that is killed by systemd at the very end of the shutdown, QEMU and a vGPU's VFIO device included. So the whole
 * sequence fits inside the delay: the save to disk, then the guest's own shutdown (ACPI or the agent), then QEMU's quit
 * (a clean exit: disks flushed, VFIO closed), then SIGKILL, and lord-vm lets go of the inhibitor once QEMU is gone,
 * before logind gives up on it. Times are from the start of the stop, in ms. Pure: tests/vm_test.c checks it. */
#ifndef LORD_VM_HOSTSTOP_H
#define LORD_VM_HOSTSTOP_H

#define VM_HOST_DELAY_DEFAULT_MS 30000  /* packaging/systemd/logind-lordos-machines.conf; also SIGTERM's budget */
#define VM_HOST_MARGIN_MS 1000          /* QEMU gone and the inhibitor closed this long before logind's delay ends */
#define VM_HOST_QUIT_MS 3000            /* QEMU's quit, before SIGKILL */
#define VM_HOST_ACPI_MIN_MS 8000        /* a save given up still leaves the guest this long to shut down */
#define VM_HOST_SAVE_MIN_MS 3000        /* less than this for a save isn't worth starting one */

typedef struct {
    long long save_until;               /* a save to disk still running then is cancelled; 0: no save */
    long long quit_at;                  /* the guest's shutdown ends: QEMU is told to quit */
    long long kill_at;                  /* QEMU still there: SIGKILL */
} VmHostStopPlan;

/* delay_ms: logind's delay (-1 unknown: the default) */
static inline VmHostStopPlan vm_host_stop_plan(long long delay_ms) {
    VmHostStopPlan p;
    if (delay_ms < 0) delay_ms = VM_HOST_DELAY_DEFAULT_MS;
    if (delay_ms > 600000) delay_ms = 600000;
    p.kill_at = delay_ms - VM_HOST_MARGIN_MS;
    if (p.kill_at < 500) p.kill_at = 500;
    p.quit_at = p.kill_at - VM_HOST_QUIT_MS;
    if (p.quit_at < 0) p.quit_at = 0;
    p.save_until = p.quit_at - VM_HOST_ACPI_MIN_MS;
    if (p.save_until < VM_HOST_SAVE_MIN_MS) p.save_until = 0;
    return p;
}

#endif
