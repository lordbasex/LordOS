/* cgroup.h — each app goes into its own cgroup v2 (a "scope"), so the monitoring agent
 * attributes traffic and hardware by that name and so memory and CPU limits can be set on it.
 *
 *   app-lordos-lpk-<id>.scope   isolated native .lpk apps (sandbox.c)
 *   app-lordos-<id>.scope       Linux apps (.desktop) launched by apps.c
 *
 * Reusable: called in the freshly forked child, right before execve. lord-server runs
 * without root; it needs its cgroup subtree to be delegated (lord-sessiond prepares it, see
 * docs/SESSIONS.md and docs/PRIVACY.md). If that's not possible (no delegation, missing controller,
 * old kernel), it warns in the log and goes on without a cgroup: the app runs anyway, without limits. */
#ifndef LORD_SEC_CGROUP_H
#define LORD_SEC_CGROUP_H

#include <stddef.h>
#include <string.h>

typedef struct {
    long mem_high_mb;       /* memory.high in MB (0 = no limit) */
    long mem_max_mb;        /* memory.max in MB (0 = no limit) */
    int  cpu_percent;       /* cpu.max as % of one core (0 = no limit; 100 = one core) */
} CgroupLimits;

/* Moves the current process into scope `name` inside the session's delegated subtree and
 * applies the limits. 0 if it ended up in the scope; -1 if it couldn't (already warned, the caller goes on). */
int cgroup_enter(const char *name, const CgroupLimits *lim);

/* Shortcut for the child before execve: enters app-lordos-<id>.scope with loose limits. The
 * launcher refuses to exec when it fails inside a LordOS session. */
int app_scope_enter(const char *id);   /* 0 placed; -1 not (invalid scope name, refused or no answer) */

/* Scopes of LordOS's own helpers (desktop:<id> in the firewall): a .desktop with that id must not inherit their rules.
 * "lpk-<x>": its scope would read as the package <x>, lpk:<x>, with that package's rules and grants. "lordos-web-<app>":
 * a web app's lord-web (docs/WEBAPPS.md); lordos-webprobe, its probe. */
static inline int app_scope_reserved(const char *id) { return !strcmp(id, "lordos-rclone") || !strcmp(id, "lordos-cloud") || !strcmp(id, "lordos-rdesk") || !strncmp(id, "lpk-", 4) || !strncmp(id, "fp-", 3) || !strncmp(id, "lordos-vm", 9) || !strncmp(id, "lordos-web", 10); }

/* The cgroup v2 path of the current process, relative to /sys/fs/cgroup (e.g. ".../app.slice/..."),
 * so the monitor knows which scope it's in. "" if it couldn't be read. */
const char *cgroup_self(void);

/* Called by lord-sessiond (root) in a session's child, before lord-server's execve: creates the
 * session's cgroup subtree with memory/CPU/pids enabled and moves the process into its «server»
 * leaf. The subtree stays root-owned (cgroup_harden_session); app scopes are created later by
 * lord-sessiond on request (cgroup_place_serve). Best effort: without delegation (Delegate=yes in
 * lordos.service) it warns and goes on. Returns 0 on success. See docs/SESSIONS.md. */
int cgroup_delegate_session(unsigned uid, unsigned gid);
/* cpu.weight of the session's «server» leaf (the compositor), against 100 for each app scope and helper */
#define CG_SERVER_CPU_WEIGHT "1000"

/* (docs/VIRTUALIZATION.md 3.7) The machines' parent: every machine's scope (app-lordos-lordos-vm-<id>.scope and a
 * Tor gateway's app-lordos-lordos-vm-tor-<id>.scope) goes into lordos-session-<uid>/lordos-machines.slice, made by
 * lord-sessiond (root-owned like the rest of the subtree) with fixed weights: CPU and I/O half of one app scope's, a
 * twentieth of the compositor's. Machines together then yield to the desktop and the apps under contention and still
 * use every idle core and the whole disk otherwise; each machine keeps its own memory.max and cpu.max in its scope.
 * No request names the slice: lord-sessiond puts a machine's scope there by its name alone. */
#define CG_MACHINES_SLICE      "lordos-machines.slice"
#define CG_MACHINES_CPU_WEIGHT "50"
#define CG_MACHINES_IO_WEIGHT  "default 50"
/* Only while the lord-netperm that runs knows the slice: one older than this lord-sessiond (the package updated, the
 * reboot not done yet, lordos.service restarted alone) reads only the session folder, and a machine one level down
 * would have no firewall at all. So: 1 when procs (CG_NETPERM_PROCS, systemd's root-owned file) can't be read or is
 * empty (not running: the installed one starts next), or its first process runs the installed lord-netperm (not
 * "(deleted)"); 0 otherwise, and the machine's scope goes beside the server leaf, where every lord-netperm reads it. */
#define CG_NETPERM_PROCS "/sys/fs/cgroup/system.slice/lord-netperm.service/cgroup.procs"
int cgroup_netperm_current(const char *procs);
/* 1 if a scope of this name belongs in CG_MACHINES_SLICE (a machine's, by its reserved prefix) */
int cgroup_scope_in_machines(const char *name);
/* The slice in the session folder sessfd: made if missing, root-owned, its controllers (memory, cpu, pids, and io when
 * the kernel has it) on and the weights written. An open directory fd of it, or -1. Root only. */
int cgroup_machines_slice(int sessfd);
/* The same placement rule for a session child name: "server", an accepted scope, or "lordos-machines.slice/<machine
 * scope>" (the noted limits of #140 below). 1 if accepted. */
int cgroup_session_child_ok(const char *name);


/* Called by lord-sessiond at startup (root, lordos.service with Delegate=yes): moves itself to the
 * "supervisor" leaf so the service root can enable memory/CPU/pids for the
 * sessions' subtrees (cgroup v2's "no internal processes" rule). 0 if it did. */
int cgroup_service_init(void);

/* (GitHub #140) lord-sessiond --release-cgroups, from lordos.service's ExecStartPre: notes the limits of the
 * sessions' leaves and scopes in CG_SAVED_LIMITS and turns the controllers off in the sessions and the service
 * root, so that systemd 258 and later can start the new main process in the unit's root cgroup while sessions
 * live on. cgroup_service_init() turns them on again and puts the limits back. Root only; 0 if the root has no
 * controller left on. */
#define CG_SAVED_LIMITS "/run/lordos/cgroup-limits"
int cgroup_service_release(void);
/* The same on an open service directory (tests): the noted lines ("<session> <leaf> <file> <value>") go to outfd
 * (-1: none). Putting them back: the number of values written; *failed (may be NULL) counts the lines whose leaf
 * exists but refused the value (the notes file is then kept for the next start). Merging, before the file is
 * replaced: the fresh notes plus the older ones for a limit the fresh don't name and whose leaf still exists, into
 * out; -1 if they don't fit (the old file is kept then). */
#define CG_NOTES_MAX 65536
int cgroup_release_at(int svcfd, int outfd);
int cgroup_restore_at(int svcfd, const char *saved, int *failed);
int cgroup_merge_notes(int svcfd, const char *old, const char *fresh, char *out, size_t cap);

/* (0.8.5) Called by lord-sessiond in lord-netd's child, before its execve: moves the process into
 * the root-owned lordos.service/<name> leaf (only "netd"), so lord-netperm can count the network
 * service's own traffic (DHCP, the VPN daemons) apart from lord-sessiond's. Best effort: 0 placed. */
int cgroup_service_leaf(const char *name);

/* (0.8.5) A root process of a session (the admin helper's download child, before it drops to
 * nobody) moves itself into scope `name` of its own session's subtree: lord-sessiond's placement
 * service only places session users, and root may write the root-owned cgroup.procs itself. Only
 * from the session's «server» leaf, only itself. 0 placed. */
int cgroup_enter_root(const char *name);

/* (0.8.5) A quarter of a second for lord-netperm to see a scope that was just created (it watches
 * the session folder with inotify) before the process in it opens a connection: otherwise the first
 * connections would meet no policy. Called after placing a process in a new scope that connects at
 * once (the Store's download child, the components' connectors). */
void cgroup_settle(void);

/* ---- Root-owned app scopes (lord-sessiond) ----
 * With lord-sessiond, a session's subtree stays root-owned: the scopes are created by root, with a
 * root-owned cgroup.procs, so an app cannot move itself to another cgroup (it would escape its
 * per-app firewall rules and accounting). cgroup_enter() asks lord-sessiond over CGROUP_SOCK to
 * place the calling process; without lord-sessiond (development) it self-migrates as before. */
#define CGROUP_SOCK "/run/lordos/cgroup.sock"

/* 1 if lord-sessiond accepts creating this scope name. */
int cgroup_scope_name_ok(const char *name);

/* chown root:root a session subtree (lordos-session-<uid>, its leaves and scopes). Root only. */
void cgroup_harden_session(int sessfd);

/* lord-sessiond: serve one connection on CGROUP_SOCK ("enter <scope> <memhigh> <memmax> <cpu%>").
 * Only a peer that is in its session's «server» leaf and runs a binary from bindir can be placed,
 * and only itself (SO_PEERCRED pid). Answers "ok" or "err <reason>". 0 if placed. */
int cgroup_place_serve(int cfd, const char *bindir, int insecure);

#endif
