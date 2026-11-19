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
