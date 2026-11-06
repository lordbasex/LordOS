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
 * "lpk-<x>": its scope would read as the package <x>, lpk:<x>, with that package's rules and grants. */
static inline int app_scope_reserved(const char *id) { return !strcmp(id, "lordos-rclone") || !strcmp(id, "lordos-cloud") || !strcmp(id, "lordos-rdesk") || !strncmp(id, "lpk-", 4); }

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


/* Called by lord-sessiond at startup (root, lordos.service with Delegate=yes): moves itself to the
 * "supervisor" leaf so the service root can enable memory/CPU/pids for the
 * sessions' subtrees (cgroup v2's "no internal processes" rule). 0 if it did. */
int cgroup_service_init(void);

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
