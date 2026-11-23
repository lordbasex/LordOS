/* llmjail.h — the sandbox lord-llm puts the local model engine in before it runs it (docs/SENTINELA.md, "Sandbox").
 *
 * The engine (llama.cpp's llama-server, built by LordOS and installed from the Store) is an upstream program: it can't
 * call sandbox_apply_helper itself, so lord-llm applies everything right before execveat and it stays on across the
 * exec (no_new_privs). Applied in this order, in a process with one thread:
 *   1. its own user and network namespace (unshare, the user's uid and gid mapped to themselves): no network interface
 *      at all, not even an up loopback, and its own abstract unix socket namespace;
 *   2. Landlock: read-only /usr (no execute), the loader's file and the engine's own file executable, the model
 *      (an fd) readable, /sys/devices/system/cpu readable, /dev/null and /dev/urandom; in the runtime folder (an fd)
 *      only creating and removing a socket; TCP bind and connect denied (ABI 4); abstract unix sockets and signals
 *      scoped to itself (ABI 6);
 *   3. seccomp (all threads): socket only AF_UNIX SOCK_STREAM, never connect, no io_uring (it would bypass the socket
 *      filter), no new namespaces, no ptrace or reading other processes' memory, no mounts, no BPF, no keys, no
 *      modules; execve never, execveat only of the descriptor LLMJAIL_ENGINE_FD with AT_EMPTY_PATH.
 * Network is denied three times (no interface, Landlock, seccomp), so the only way in or out is the unix socket the
 * engine makes in its runtime folder, which only the session's user can open (0700 folder, 0600 socket). */
#ifndef LORD_LLM_JAIL_H
#define LORD_LLM_JAIL_H

#define LLMJAIL_MODEL_FD 3      /* the model, opened read-only by lord-server */
#define LLMJAIL_DIR_FD 4        /* $XDG_RUNTIME_DIR/lordos/llm, the user's, 0700 */
#define LLMJAIL_ENGINE_FD 5     /* the engine's executable (O_PATH) */

enum { LJ_USERNS = 1, LJ_NETNS = 2, LJ_LANDLOCK_FS = 4, LJ_LANDLOCK_NET = 8, LJ_LANDLOCK_SCOPE = 16, LJ_SECCOMP = 32, LJ_NOPRIV = 64 };

typedef struct {
    int model_fd, dir_fd, engine_fd;
    int need_netns;             /* 1: fail when the network namespace can't be made (tests); 0: go on with the rest */
} LlmJailCfg;

/* Applies the three layers. Returns the mask of what is on (LJ_*), or -1 when seccomp or no_new_privs could not be
 * applied (the caller must not exec then). Landlock and the namespaces are best effort on a kernel without them,
 * reported in the mask; the seccomp filter alone already keeps the engine off the network. Doesn't allocate. */
int llmjail_apply(const LlmJailCfg *cfg);

/* The fixed argument list lord-llm runs the engine with (no caller argument ever reaches it). */
extern const char *const LLMJAIL_ARGV[];

#endif
