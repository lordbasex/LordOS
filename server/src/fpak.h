/* fpak.h — Flatpak apps for the launcher: the pure parts (docs/FLATPAK.md, Problems 2 and 3; AGENTS.md "Flatpak as an
 * app source", E1 and E2). The id rules, reading an installation without trusting its links, and the `flatpak run`
 * command line, built from fixed tables: never an Exec line, never arguments from the caller. The launch itself (scope,
 * view, fork) is in apps.c. */
#ifndef LORD_FPAK_H
#define LORD_FPAK_H

#include <stddef.h>
#include <stdint.h>

/* The id part of app-lordos-fp-<id>.scope: cgroup_scope_name_ok allows 62 bytes for "fp-<id>" (docs/FLATPAK.md, open
 * question 1: longer ids are refused in phase 1). */
#define FPAK_ID_MAX 59
#define FPAK_BRANCH_MAX 64
#define FPAK_FLATPAK "/usr/bin/flatpak"
#define FPAK_ARGV_MAX 48

/* A Flatpak app id the launcher accepts: Flatpak's own rules (at least three dot-separated elements of [A-Za-z0-9_-],
 * none empty, none starting with a digit, '-' only in the last one) and at most FPAK_ID_MAX bytes. */
int fpak_id_ok(const char *id);
/* A branch as Flatpak writes it ("stable", "beta", "25.08"): [A-Za-z0-9._-], not starting with '.' or '-', no "..". */
int fpak_branch_ok(const char *b);

/* The installed app's active arch and branch: <root>/app/<id>/current is a link "<arch>/<branch>", and
 * <root>/app/<id>/<arch>/<branch>/active/metadata a regular file. root: ~/.local/share/flatpak or /var/lib/flatpak.
 * Every step is opened from the one before, never following a link out of the app's folder ("active" is the one link
 * followed, and only to a name in its own folder). 0 found, -1 not installed or not trusted. metadata, if not NULL,
 * gets the file's text (up to mn - 1 bytes). */
int fpak_installed(const char *root, const char *id, char *arch, size_t an, char *branch, size_t bn,
                   char *metadata, size_t mn);

typedef struct {
    const char *id, *arch, *branch;
    uint32_t restricted;            /* FW_R_* of flatpak:<id> */
    const char *home;               /* the real home folder */
    const char *opened;             /* "Open with": one real path, or NULL */
    int opened_ro;                  /* grant it read-only (the launcher's grant_ok said no to writing it) */
    int x11;                        /* the X display may be kept (unrestricted only) */
} FpakRun;

/* argv for execve(FPAK_FLATPAK, argv, env): run, --arch, --branch, the never-granted options, LordOS's restrictions,
 * the id and, for "Open with", the path. Strings go in buf (cap bytes). → argc, or -1 (bad input or no room). */
int fpak_argv(const FpakRun *r, char **argv, char *buf, size_t cap);

/* ---- org.freedesktop.portal.Flatpak.Spawn served by lord-server (docs/FLATPAK.md, "Sub-sandboxes"; AGENTS.md E9) ----
 * What an app asks for, already read from its D-Bus call. Its own command, arguments and environment go to the new
 * instance of the same app; LordOS's never-granted options and restrictions come after what it asks for (Flatpak's
 * last option wins) and the arguments after the app id (never read as flatpak options). */
#define FPAK_SPAWN_ARGS_MAX 128     /* argv entries of the command */
#define FPAK_SPAWN_ENV_MAX 64
#define FPAK_SPAWN_EXPOSE_MAX 8
#define FPAK_SPAWN_STR_MAX 4096
#define FPAK_SPAWN_ARGV_MAX (FPAK_ARGV_MAX + FPAK_SPAWN_ARGS_MAX + 2 * FPAK_SPAWN_ENV_MAX + 2 * FPAK_SPAWN_EXPOSE_MAX + 24)

/* Spawn's flags (flatpak-portal's FlatpakSpawnFlags) and sandbox-flags */
#define FPAK_SPAWN_CLEAR_ENV      1u
#define FPAK_SPAWN_LATEST_VERSION 2u
#define FPAK_SPAWN_SANDBOX        4u
#define FPAK_SPAWN_NO_NETWORK     8u
#define FPAK_SPAWN_WATCH_BUS      16u
#define FPAK_SPAWN_EXPOSE_PIDS    32u
#define FPAK_SPAWN_NOTIFY_START   64u
#define FPAK_SPAWN_SHARE_PIDS     128u
#define FPAK_SPAWN_EMPTY_APP      256u
#define FPAK_SPAWN_FLAGS_ALL      511u
#define FPAK_SANDBOX_SHARE_DISPLAY 1u
#define FPAK_SANDBOX_SHARE_SOUND   2u
#define FPAK_SANDBOX_SHARE_GPU     4u
#define FPAK_SANDBOX_ALLOW_DBUS    8u
#define FPAK_SANDBOX_ALLOW_A11Y    16u
#define FPAK_SANDBOX_FLAGS_ALL     31u

typedef struct {
    uint32_t flags, sandbox_flags;
    const char *cwd;                                /* absolute, or NULL */
    const char *const *args;                        /* the command and its arguments, NULL-terminated; args[0] = --command */
    const char *const *env;                         /* "NAME=value", NULL-terminated (may be NULL) */
    const char *const *unset_env;                   /* NAME, NULL-terminated (may be NULL) */
    const char *const *expose, *const *expose_ro;   /* names under ~/.var/app/<id>/sandbox, NULL-terminated (may be NULL) */
    int parent_pid;                                 /* the caller's instance in the view's PID namespace (EXPOSE/SHARE_PIDS) */
    int instance_fd;                                /* --instance-id-fd (NOTIFY_START), or -1 */
} FpakSpawn;

/* What an app asked for is acceptable: known flags only, an absolute cwd with no ".." component, 1 to
 * FPAK_SPAWN_ARGS_MAX arguments with a non-empty command, environment names [A-Za-z_][A-Za-z0-9_]* (none of
 * LD_PRELOAD and the like matter: they reach only the app's own new sandbox), expose names that are one plain path
 * element, every string under FPAK_SPAWN_STR_MAX bytes and none with a newline in an option. 1 yes, 0 no. */
int fpak_spawn_ok(const FpakSpawn *s);

/* argv for a Spawn of app r->id: as fpak_argv (r->opened must be NULL), with what s asks for. argv has room for
 * FPAK_SPAWN_ARGV_MAX entries. → argc, or -1. */
int fpak_spawn_argv(const FpakRun *r, const FpakSpawn *s, char **argv, char *buf, size_t cap);

/* ---- the closed list of `flatpak --user` management commands (docs/FLATPAK.md, Problem 4; AGENTS.md E3) ----
 * Run by lord-server in lordos-net-flathub.scope (firewall component lordos:flathub). The only argument a caller gives
 * is a ref, validated here; never a URL, remote name, key, file or option. Flatpak's own TLS and signature checks are
 * never relaxed (no --no-gpg-verify; --from only with the .flatpakrepo the lordos package ships). */
enum {
    FPAK_CMD_REMOTE_ADD,        /* remote-add --if-not-exists --from flathub <shipped .flatpakrepo> */
    FPAK_CMD_APPSTREAM,         /* update --appstream flathub */
    FPAK_CMD_INFO,              /* remote-info --show-metadata flathub <ref> */
    FPAK_CMD_INSTALL,           /* install flathub <ref> */
    FPAK_CMD_UPDATE,            /* update [<ref>] */
    FPAK_CMD_UNINSTALL,         /* uninstall <ref> */
    FPAK_CMD_UNUSED,            /* uninstall --unused */
    FPAK_CMD_LIST,              /* list --app --columns=application,branch,version,origin */
    FPAK_CMD_COUNT
};
#define FPAK_REPO_FILE "/usr/lib/lordos/assets/flatpak/flathub.flatpakrepo"
#define FPAK_REF_MAX 160
/* "app/<id>/<arch>/<branch>": an app's ref (never a runtime's: Flatpak resolves those), the id as fpak_id_ok, the arch
 * the native one, the branch as fpak_branch_ok. */
int fpak_ref_ok(const char *ref, const char *arch);
/* argv for one command of the list (ref NULL or "" where it takes none or is optional). → argc, or -1 (unknown command,
 * a ref where none is taken or a missing one, a bad ref, no room). */
int fpak_cmd_argv(int cmd, const char *ref, const char *arch, char **argv, char *buf, size_t cap);

#endif
