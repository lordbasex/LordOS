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
 *      scoped to itself (ABI 6). Only with a GPU backend (0.8.9.6.2): Vulkan opens the render nodes /dev/dri/renderD*
 *      (read, write, their ioctls), lists /dev/dri and reads the parts of /sys the Vulkan loader and Mesa use to find
 *      the card, never /dev/dri/card* (the display); CUDA opens /dev/nvidiactl, /dev/nvidia<N>, /dev/nvidia-uvm and
 *      reads /proc/driver/nvidia and those parts of /sys;
 *   3. seccomp (all threads): socket only AF_UNIX SOCK_STREAM, never connect, no io_uring (it would bypass the socket
 *      filter), no new namespaces, no ptrace or reading other processes' memory, no mounts, no BPF, no keys, no
 *      modules; execve never, execveat only of the descriptor LLMJAIL_ENGINE_FD with AT_EMPTY_PATH; signals only to
 *      itself (kill, tgkill, rt_*sigqueueinfo with its own pid; no tkill, pidfd_open or pidfd_send_signal).
 * Network is denied three times (no interface, Landlock, seccomp), so the only way in or out is the unix socket the
 * engine makes in its runtime folder, which only the session's user can open (0700 folder, 0600 socket). */
#ifndef LORD_LLM_JAIL_H
#define LORD_LLM_JAIL_H

#define LLMJAIL_MODEL_FD 3      /* the model, opened read-only by lord-server */
#define LLMJAIL_DIR_FD 4        /* $XDG_RUNTIME_DIR/lordos/llm, the user's, 0700 */
#define LLMJAIL_ENGINE_FD 5     /* the engine's executable (O_PATH) */
#define LLMJAIL_LANDLOCK_ABI 6  /* required: files, TCP (4) and signal/abstract-socket scoping (6); Linux 6.12 */

enum { LJ_USERNS = 1, LJ_NETNS = 2, LJ_LANDLOCK_FS = 4, LJ_LANDLOCK_NET = 8, LJ_LANDLOCK_SCOPE = 16, LJ_SECCOMP = 32, LJ_NOPRIV = 64 };

/* (0.8.9.6.2) where the engine computes: the processor only, or a GPU through one of ggml's backend modules (llama.cpp
 * loads libggml-vulkan.so / libggml-cuda.so from /usr/lib/llama.cpp only when their runtime is there). lord-server
 * chooses (engine.c) and tells lord-llm through LORD_LLM_BACKEND, a closed word; anything else is the processor. */
enum { LLMJAIL_CPU = 0, LLMJAIL_VULKAN = 1, LLMJAIL_CUDA = 2 };

typedef struct {
    int model_fd, dir_fd, engine_fd;
    int need_netns;             /* 1: fail when the network namespace can't be made (tests); 0: go on with the rest */
    int backend;                /* LLMJAIL_*: the GPU devices are reachable only for that backend (llmjail_apply) */
} LlmJailCfg;

/* Applies the three layers. Returns the mask of what is on (LJ_*), or -1 when Landlock (all of files, TCP and scoping:
 * ABI LLMJAIL_LANDLOCK_ABI, every rule the engine depends on), seccomp or no_new_privs could not be applied: the caller
 * must not exec then (fails closed; on an older kernel Sentinela has rule alerts only). The namespaces are best effort,
 * reported in the mask (unprivileged user namespaces may be off). Doesn't allocate. */
int llmjail_apply(const LlmJailCfg *cfg);

/* The fixed argument lists lord-llm runs the engine with (no caller argument ever reaches it), one per backend:
 * the processor (-ngl 0 -dev none), the first Vulkan device or the first CUDA device with as many layers as fit in its
 * free memory (-ngl auto, llama.cpp's --fit, which keeps 1 GB of the card free). */
extern const char *const *const LLMJAIL_ARGVS[3];
/* "cpu", "vulkan" or "cuda" → LLMJAIL_*; NULL or anything else → -1 */
int llmjail_backend(const char *word);

#endif
