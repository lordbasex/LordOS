/* fpspawner.h — the Spawn helper inside a Flatpak app's view (docs/FLATPAK.md, "Sub-sandboxes"; AGENTS.md E9).
 *
 * Why it exists: Chromium (QtWebEngine, CEF) and zypak (Electron) ask Flatpak's Spawn for a sub-sandbox that joins the
 * app's own user and PID namespaces (FLATPAK_SPAWN_FLAGS_EXPOSE_PIDS / SHARE_PIDS). `flatpak run --parent-pid=N` then
 * opens /proc/N/root/run/.userns and /proc/N/ns/pid, which the kernel allows only to a process whose Landlock domain
 * is the app's own or one above it. A `flatpak run` that lord-server starts in a fresh domain (Landlock's scoping applied
 * again) can't, so bubblewrap fails ("creation of new user namespaces was not disabled as requested") and Chromium
 * stops. This helper is forked by the launcher inside the app's view right after the app's Landlock scoping, before
 * `flatpak run` of the app: the same scope, view namespaces and Landlock domain as the app, so what it starts has
 * exactly the app's confinement and no more. It holds no capability, is not dumpable, and takes requests only from
 * lord-server, over a socket pair lord-server made (never reachable from the app: the app's end is closed at exec).
 *
 * The protocol (SOCK_SEQPACKET, one FpswMsg per packet):
 *   helper → lord-server  HELLO   + its own pidfd (lord-server learns which view it serves)
 *   lord-server → helper  SPAWN   + a sealed memfd (argv for /usr/bin/flatpak, then envp) + the fds to place + the
 *                                   --instance-id-fd pipe when inst_fd >= 0
 *   helper → lord-server  STARTED + the new process's pidfd   |  FAILED (status: the step)
 *   helper → lord-server  EXITED  (status: the wait status)
 * The helper only ever executes /usr/bin/flatpak with an argv whose second word is "run" (built by fpak_spawn_argv in
 * lord-server); it checks that again. */
#ifndef LORD_FPSPAWNER_H
#define LORD_FPSPAWNER_H

#include <stddef.h>
#include <stdint.h>

#define FPSW_FD 4                       /* the helper's socket in the launcher's --launch-view process */
#define FPSW_MAGIC 0x4c535057u
#define FPSW_MAX_FDS 16                 /* fds one Spawn may place (DBUS_MAX_FDS) */
#define FPSW_MAX_KIDS 32                /* processes one view's helper runs at once (fpspawn.c's MAX_PER_APP) */
#define FPSW_PAYLOAD_MAX (1u << 20)
#define FPSW_ARGV_MAX 512
#define FPSW_ENV_MAX 512

enum { FPSW_HELLO = 1, FPSW_SPAWN, FPSW_STARTED, FPSW_FAILED, FPSW_EXITED };

typedef struct {
    uint32_t magic, type, seq;
    int32_t status;                     /* FAILED: the step; EXITED: the wait status */
    uint32_t nfd;                       /* SPAWN: how many fds to place */
    int32_t inst_fd;                    /* SPAWN: the number for --instance-id-fd, or -1 */
    uint32_t target[FPSW_MAX_FDS];      /* SPAWN: the number each fd gets in the new process (0-1023) */
} FpswMsg;

/* A SPAWN header that may be served, given how many fds came with it: the magic and type, at most FPSW_MAX_FDS
 * targets, each 0-1023 and different, inst_fd -1 or 3-1023 and not a target, and exactly 1 (the payload) + nfd
 * (+1 with inst_fd) fds. 1 yes, 0 no. */
int fpsw_spawn_ok(const FpswMsg *m, int nfds);

/* The payload: argv strings, an empty string, envp strings, an empty string, all NUL-terminated. → bytes written to
 * out (cap bytes), or -1. */
long fpsw_pack(char *const argv[], char *const envp[], char *out, size_t cap);
/* Reads it back (pointers into buf, which must end with the last NUL) and checks it: argv[0] is /usr/bin/flatpak and
 * argv[1] "run", every envp entry has '=' after a non-empty name. 0, or -1. */
int fpsw_unpack(char *buf, size_t n, char **argv, int amax, char **envp, int emax);

/* The helper's loop (in the app's view, after its Landlock scoping): never returns. */
void fpsw_main(int sock);

#endif
