/* vmlistcache.h — lord.vm.list's cache and the library's change events (docs/VIRTUALIZATION.md, "The list's cache").
 *
 * lord.vm.list() reads every machine folder of ~/Machines (machine.json, run.json, operation.json, identity.json,
 * suspend.state, a group's group.json, …) on lord-server's main thread. The list is kept as JSON and given again while
 * nothing changed: an inotify watch on the library, on each machine folder (and its disks/) and on each group folder
 * (and its members) says when something did. A kept list is also dropped after VMLC_TTL_MS (what no file says: the
 * system's UEFI code updated, an idle macvtap or vGPU to remove), and as soon as an active machine's lord-vm is gone (a
 * supervisor that died writes nothing). Without inotify (no watch left), nothing is kept: every list reads the folders,
 * as before.
 *
 * The same watch is set when lord-server starts (js_vm.c, "the machines' state for the shell"): its events are
 * __lordVm('library') and, with a pidfd per active lord-vm and the sessions' apps mode, __lordVm('state'), so the shell
 * never reads the library on a timer (sdk/vmlibrary.js). Names that never change the list are left out: a machine's picture (thumb.png, rewritten while it runs), logs, and temporary or hidden files (a name starting
 * with '.', or ending in ".tmp" or '~': each is renamed to its real name, which is seen).
 *
 * The firmware check (does a UEFI machine's firmware_code still hash to its firmware_sha256?) hashes each system file
 * once per version (device, inode, size, change time), not once per machine: a cache keyed by the machine id's first
 * byte (id[0] % 16, before) let ids starting with '1' and 'a' (or with the same character) evict each other,
 * so every list hashed the 3.5 MB OVMF_CODE_4M.fd again for those machines, about 13 ms each. */
#ifndef LORD_VM_VMLISTCACHE_H
#define LORD_VM_VMLISTCACHE_H

#include <limits.h>
#include <stddef.h>

#define VMLC_TTL_MS   10000.0
#define VMLC_PIDS     256
#define VMLC_WATCHES  1024

typedef struct {
    int fd;                         /* inotify (non-blocking, close-on-exec), -1 none yet */
    int broken;                     /* a watch couldn't be added (or no inotify): nothing is kept */
    int dirty;                      /* something changed since the list was kept */
    char lib[PATH_MAX];             /* the library the kept list is of */
    char *json;                     /* the kept list (NULL: none) */
    size_t len;
    double at;                      /* when it was kept (ms, monotonic) */
    long pids[VMLC_PIDS];           /* the active machines' lord-vm when it was kept */
    int npids;
    unsigned gen;                   /* moves with every change seen: an event that matters, or a list that differs */
    int nw;
    struct { int wd; char *path; } w[VMLC_WATCHES];
} VmListCache;

void vmlc_init(VmListCache *c);
void vmlc_free(VmListCache *c);
/* Whether a change to this file name (in a watched folder) can change the list: 0 for thumb.png, *.log, names starting
 * with '.' and names ending in ".tmp" or '~'. */
int vmlc_name_matters(const char *name);
/* Watches dir (idempotent); a machine folder's disks/ too. 0, or -1 (then the cache is broken: nothing kept). */
int vmlc_watch(VmListCache *c, const char *dir);
/* Watches the library and every machine and group folder in it (and the groups' members). 0 or -1 as above. */
int vmlc_watch_library(VmListCache *c, const char *lib);
/* The inotify fd for a main loop's poll (-1 none). */
int vmlc_fd(const VmListCache *c);
/* Reads every pending event, never blocking: 1 if one mattered (the kept list is dropped and gen moves). A machine or
 * group folder that appears gets its watch. */
int vmlc_drain(VmListCache *c);
/* The kept list's JSON if it may be given now: the same library, nothing changed, younger than VMLC_TTL_MS, and
 * alive(pid) for every active machine's lord-vm it had. NULL otherwise. */
const char *vmlc_get(VmListCache *c, const char *lib, double now, int (*alive)(long), size_t *len);
/* Before reading the folders: the pending events are read and the watches set (each folder is watched before it is
 * read, so a change while reading is a change next time). */
void vmlc_begin(VmListCache *c, const char *lib);
/* An active machine's lord-vm, seen while reading. */
void vmlc_pid(VmListCache *c, long pid);
/* Keeps the list read now (gen moves when it differs from the one kept before). */
void vmlc_store(VmListCache *c, const char *json, size_t len, double now);

/* 1 when the UEFI code file at path (opened by the caller as fd) doesn't hash to want (64 lowercase hex), 0 when it
 * does, -1 when it can't be read. Each file version is hashed once (VMLC_FW slots). vmlc_fw_hashes counts the hashes
 * (tests). */
#define VMLC_FW 8
int vmlc_firmware_changed(int fd, const char *want);
extern unsigned vmlc_fw_hashes;

#endif
