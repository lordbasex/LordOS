/* vmlog.h — lord-vm's log kept useful for a whole run (docs/VIRTUALIZATION.md, "lord-vm's log").
 *
 * Found live on 2026-11-22: after a machine's window closed, QEMU went on calling the dead D-Bus listener and wrote
 * "Failed to call update: The connection is closed" twice every 30 ms; 32,000 such lines filled lord-vm.log's 4 MB cap
 * in 15 minutes and lord-vm wrote nothing more for the rest of the run. So:
 *
 *   - a child's stderr (QEMU, passt) is cut into whole lines (vmlog_lines), not into whatever each read() returned;
 *   - a line that comes again with the same text (its leading timestamp aside: QEMU's own) is logged once, then counted,
 *     and a summary line says how many times it came, at most every VMLOG_SUMMARY_MS (vmlog_repeat, vmlog_repeat_tick);
 *   - the log is rotated when it is full (vmlog_rotate: base.log → base.1.log → base.2.log, the oldest dropped) instead
 *     of going silent, so it stays within keep × the cap.
 *
 * Pure functions on caller-owned state, except vmlog_rotate (files in one folder); tests/vmlog_test.c. */
#ifndef LORD_VM_VMLOG_H
#define LORD_VM_VMLOG_H

#include <stddef.h>

#define VMLOG_LINE_MAX 1024
#define VMLOG_SUMMARY_MS 10000

/* bytes of a child's output, gathered into whole lines */
typedef struct {
    char buf[VMLOG_LINE_MAX];
    size_t n;
} VmLineBuf;

typedef void (*VmLineFn)(void *ctx, const char *line, size_t n);

/* p[0..n) in; fn(ctx, line, len) for every complete line (without its '\n'; empty lines skipped). A line longer than
 * VMLOG_LINE_MAX is given in pieces of that size. */
void vmlog_lines(VmLineBuf *b, const char *p, size_t n, VmLineFn fn, void *ctx);
/* what is left without a '\n' (the child ended), given as a line */
void vmlog_lines_flush(VmLineBuf *b, VmLineFn fn, void *ctx);

/* the repeats of one source's last line */
typedef struct {
    char last[VMLOG_LINE_MAX];          /* the last line logged, its key (vmlog_key) */
    size_t last_n;
    unsigned long rep;                  /* times it came again since, not logged */
    long long rep_since;                /* when the first of those came (ms) */
    long long said_at;                  /* when the line, or its last summary, was logged */
} VmLogRep;

/* the part of a line that names it: without a leading ISO 8601 timestamp ("2026-11-22T14:04:50.906182Z "), which QEMU
 * puts in front of each of its messages */
const char *vmlog_key(const char *line, size_t n, size_t *kn);

/* A line from a source at now (ms): 1 to log it, 0 a repeat of the last one (counted, not logged). *sum (cap bytes)
 * gets a summary line to log before it, or "" (the repeats of the line before, when another comes, or the count so
 * far, every VMLOG_SUMMARY_MS while it keeps coming). */
int vmlog_repeat(VmLogRep *r, const char *line, size_t n, long long now, char *sum, size_t cap);
/* the loop's tick: the repeats not said yet, once VMLOG_SUMMARY_MS went by since the last time; 1 when *sum holds one */
int vmlog_repeat_tick(VmLogRep *r, long long now, char *sum, size_t cap);

/* A line of QEMU's stderr that is QEMU's known 3D abort: QEMU 10.0-11.1 with -display dbus,gl=on, when the guest resets
 * virtio-gpu-gl (a Linux guest's shutdown or restart on kernels with virtio_dev_shutdown, Debian's 6.12.117 and later),
 * makes the placeholder surface's texture in the vCPU thread without a GL context and asserts in dbus-listener.c:
 * "qemu-system-x86_64: ../ui/dbus-listener.c:511: dbus_scanout_texture: Assertion `tex_id' failed." (QEMU issue 4918,
 * docs/VIRTUALIZATION.md "Graphics 3D"). 1 it is. */
int vmlog_qemu_3d_abort(const char *line, size_t n);

/* How a run that ended in that abort (SIGABRT after vmlog_qemu_3d_abort's line) is recorded. The abort itself says
 * nothing of why the guest reset its graphics; what lord-vm knew just before does:
 *   asked      LordOS asked the guest to shut down (Shut Down, closing the window, the computer's own stop)
 *   notice     the guest's tools said, from the guest's last shutdown step, it powers off (1, AG_STOP_POWEROFF) or
 *              restarts (2, AG_STOP_REBOOT); 0 nothing said (no tools, older tools, not shutting down)
 *   announces  tools that would have said so were connected in this run (their hello said "stopping")
 *   ran_ms     how long the guest had run (from the first time the run's state was running; 0 never)
 * VMLOG_3D_SHUTDOWN: a shutdown (asked, or the guest said poweroff): a stop like any other. VMLOG_3D_RESTART: the guest
 * said it restarts and had run VMLOG_3D_MIN_RUN_MS: Machines starts it again. VMLOG_3D_GUEST_STOP: nothing said, no
 * tools that would have, and it had run VMLOG_3D_MIN_RUN_MS: most likely the guest's own shutdown or restart, recorded
 * as stopped by the guest (never started again: which of the two isn't known). VMLOG_3D_CRASH otherwise: not tied to a
 * stop (the tools said nothing), or right after the start (a restart that aborts again before the guest is up): the
 * error screen that offers 2D graphics. */
enum { VMLOG_3D_CRASH = 0, VMLOG_3D_SHUTDOWN = 1, VMLOG_3D_RESTART = 2, VMLOG_3D_GUEST_STOP = 3 };
#define VMLOG_3D_MIN_RUN_MS 30000
int vmlog_3d_abort_end(int asked, int notice, int announces, long long ran_ms);

/* dir/base.log → dir/base.1.log, …, base.(keep-1).log (the oldest dropped), and a new, empty dir/base.log opened for
 * appending (0600, O_CLOEXEC, O_NOFOLLOW). Its fd, or -1. keep 1..9. */
int vmlog_rotate(const char *dir, const char *base, int keep);

#endif
