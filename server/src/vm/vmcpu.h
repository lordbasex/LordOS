/* vmcpu.h — how much of its vCPUs a machine really uses (docs/VIRTUALIZATION.md, "vCPU recommendation"). lord-vm reads
 * the CPU time of QEMU's vCPU threads ("CPU n/KVM", -name debug-threads=on) once a minute while the machine runs, from
 * /proc/<qemu>/task/<tid>/stat files it opened before its own Landlock (re-read with pread: nothing new is opened
 * later), and keeps a small rolling summary in the machine's folder, <dir>/cpu-usage.json:
 *
 *   {"format":1,"windows":[[end, seconds, vcpus, avg, peak], ...]}
 *
 * one entry per 5 minutes of running time (the last one of a run may be shorter, at least a minute), oldest first, at
 * most VMCPU_KEEP of them and none older than VMCPU_MAX_AGE_S: end in Unix seconds, avg the cores used on average over
 * the window and peak its busiest minute, both in hundredths of a core. The Machines app reads it and suggests a vCPU
 * count (apps/machines/model.js vcpuAdvice); nothing here changes the machine. Pure functions but vmcpu_open/_ticks/
 * _save, tested by server/tests/vmcpu_test.c. */
#ifndef LORD_VMCPU_H
#define LORD_VMCPU_H

#include <stddef.h>

#define VMCPU_FORMAT 1
#define VMCPU_KEEP 288                 /* 24 h of running time at 5 minutes each (~9 KB of JSON) */
/* a window of 5 minutes, a sample a minute (a development build may pass shorter ones with -D) */
#ifndef VMCPU_WINDOW_S
#define VMCPU_WINDOW_S 300
#endif
#ifndef VMCPU_SAMPLE_MS
#define VMCPU_SAMPLE_MS 60000
#endif
#define VMCPU_MAX_AGE_S (30LL * 86400)
#define VMCPU_FDS 64                   /* VM_VCPUS_MAX */
#define VMCPU_FILE "cpu-usage.json"

typedef struct { long long t; int secs, vcpus, avg, peak; } VmCpuWin;

typedef struct {
    VmCpuWin w[VMCPU_KEEP];
    int n, vcpus;
    /* the last sample, and the window being filled */
    long long base_ticks, base_ms;
    int have_base;
    long long win_ms;
    double core_ms;                    /* cores × milliseconds */
    int peak;
} VmCpu;

/* a /proc/<pid>/task/<tid>/stat line → the thread's name (comm) and its user + system time in clock ticks; 0, or -1
 * when it isn't one */
int vmcpu_stat_parse(const char *buf, size_t n, char *comm, size_t cn, unsigned long long *ticks);
/* a vCPU thread's name as QEMU gives it with debug-threads=on: "CPU <n>/KVM", "CPU <n>/TCG", ... */
int vmcpu_is_vcpu(const char *comm);

/* opens the stat file of each vCPU thread of pid (at most max) into fds; how many */
int vmcpu_open(long pid, int *fds, int max);
/* their total user + system time in clock ticks, read again from offset 0; -1 when one can't be read (QEMU ended) */
long long vmcpu_ticks(const int *fds, int n);

void vmcpu_init(VmCpu *c, int vcpus);
/* one sample: the vCPU threads' total ticks at now_ms (a monotonic clock), hz ticks a second, wall the time of day in
 * Unix seconds. 1 when it closed a window (time to save), else 0. A sample after more than three periods (the
 * computer slept, the machine was paused) or with fewer ticks than the last only starts over. */
int vmcpu_sample(VmCpu *c, long long now_ms, long long ticks, long hz, long long wall);
/* the machine isn't running its vCPUs (paused, saving): the next sample starts over */
void vmcpu_hold(VmCpu *c);
/* the run ended: the window being filled is kept when it has a minute or more; 1 when it was */
int vmcpu_flush(VmCpu *c, long long wall);
/* the windows of an earlier cpu-usage.json (malformed entries and the ones older than VMCPU_MAX_AGE_S at wall
 * dropped); 0, or -1 when it isn't one (c is left with none) */
int vmcpu_load(VmCpu *c, const char *text, size_t n, long long wall);
/* the file's text; its length, or 0 when out was too small */
size_t vmcpu_json(const VmCpu *c, char *out, size_t n);
/* <dir>/cpu-usage.json, written as .tmp and renamed (0600); 0 or -1 */
int vmcpu_save(const VmCpu *c, const char *dir);

#endif
