/* launcher.c — lord-llm: runs the local model engine for Sentinela in its sandbox (docs/SENTINELA.md, "Sandbox").
 *
 * Started only by lord-server (llm/engine.c), from the fixed path next to it, with no arguments and three descriptors
 * (llmjail.h): the model at 3 (read-only), the runtime folder at 4 ($XDG_RUNTIME_DIR/lordos/llm, the user's, 0700)
 * and the engine's executable at 5 (/usr/bin/llama-server, O_PATH). It never gets a path. In order:
 *   - checks the descriptors (a regular file, the user's 0700 folder, a regular executable owned by root);
 *   - low priority: nice 15, SCHED_BATCH, idle I/O; its own cgroup scope lordos-sentinela.scope with at most two
 *     cores and a memory cap of the model's size plus 1.5 GB (without a delegated cgroup, an address-space limit);
 *   - umask 0177 and the runtime folder as its working directory, so the socket is engine.sock there, mode 0600;
 *   - llmjail_apply (namespaces, Landlock, seccomp), stdin and stdout /dev/null; stderr is what lord-server gave it
 *     (a pipe it reads the engine's startup lines from: which GPU, how many layers; engine.c);
 *   - execveat(5, "", the fixed arguments of the backend LORD_LLM_BACKEND names, a minimal environment).
 * (0.8.9.6.2) LORD_LLM_BACKEND is "cpu", "vulkan" or "cuda" (lord-server's choice, engine.c); anything else, or none,
 * is the processor. Only that backend's GPU devices are reachable in the sandbox (llmjail.c).
 * Exit codes before the exec: 2 bad descriptors, 3 the sandbox could not be applied, 4 the exec failed. */
#include "llmjail.h"
#include "../sec/cgroup.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int fail(int code, const char *what) {
    fprintf(stderr, "lord-llm: %s (%s)\n", what, strerror(errno));
    return code;
}

int main(int argc, char **argv) {
    (void)argv;
    if (argc != 1) { errno = EINVAL; return fail(2, "takes no arguments"); }
    struct stat m, d, e;
    if (fstat(LLMJAIL_MODEL_FD, &m) || !S_ISREG(m.st_mode) || m.st_size < 1024) return fail(2, "no model at fd 3");
    if (fstat(LLMJAIL_DIR_FD, &d) || !S_ISDIR(d.st_mode) || d.st_uid != getuid() || (d.st_mode & 077)) return fail(2, "no private folder at fd 4");
    if (fstat(LLMJAIL_ENGINE_FD, &e) || !S_ISREG(e.st_mode) || (e.st_uid != 0 && e.st_uid != getuid()) || (e.st_mode & 022) ||
        !(e.st_mode & S_IXUSR)) return fail(2, "no engine at fd 5");
    /* nothing else of the caller's stays open */
    closefrom(LLMJAIL_ENGINE_FD + 1);

    /* low priority: whatever it does, the desktop comes first */
    setpriority(PRIO_PROCESS, 0, 15);
    struct sched_param sp = { 0 };
    sched_setscheduler(0, SCHED_BATCH, &sp);
    syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0, 3 << 13 /* IOPRIO_CLASS_IDLE */);
    int backend = llmjail_backend(getenv("LORD_LLM_BACKEND"));
    if (backend < 0) backend = LLMJAIL_CPU;
    long model_mb = (long)(m.st_size >> 20);
    CgroupLimits lim = { .mem_high_mb = model_mb + 1280, .mem_max_mb = model_mb + 1536, .cpu_percent = 200 };
    if (cgroup_enter("lordos-sentinela.scope", &lim)) {
        /* without a cgroup, an address-space limit; a GPU driver maps the card's memory and reserves address space of
         * its own (CUDA's unified memory reserves far more than any limit), so the limit is the processor's only */
        if (backend == LLMJAIL_CPU) {
            struct rlimit as = { (rlim_t)(model_mb + 4096) << 20, (rlim_t)(model_mb + 4096) << 20 };
            setrlimit(RLIMIT_AS, &as);
        }
    }
    struct rlimit core = { 0, 0 }, files = { 256, 256 };
    setrlimit(RLIMIT_CORE, &core);
    setrlimit(RLIMIT_NOFILE, &files);

    umask(0177);
    if (fchdir(LLMJAIL_DIR_FD)) return fail(2, "runtime folder");
    unlinkat(LLMJAIL_DIR_FD, "engine.sock", 0);           /* a previous engine's, if it was killed */
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0) return fail(3, "/dev/null");

    LlmJailCfg cfg = { .model_fd = LLMJAIL_MODEL_FD, .dir_fd = LLMJAIL_DIR_FD, .engine_fd = LLMJAIL_ENGINE_FD, .backend = backend };
    int mask = llmjail_apply(&cfg);
    if (mask < 0) return fail(3, "sandbox");
    fprintf(stderr, "lord-llm: sandbox 0x%x backend %d\n", (unsigned)mask, backend);
    fcntl(LLMJAIL_DIR_FD, F_SETFD, FD_CLOEXEC);
    fcntl(LLMJAIL_ENGINE_FD, F_SETFD, FD_CLOEXEC);
    dup2(null, 0);
    dup2(null, 1);
    /* Mesa's shader cache would be written in the home folder, which the sandbox doesn't reach: off, said once */
    char *envp[] = { "PATH=/usr/bin:/bin", "LANG=C.UTF-8", "HOME=/nonexistent", "MESA_SHADER_CACHE_DISABLE=true", NULL };
    syscall(SYS_execveat, LLMJAIL_ENGINE_FD, "", (char *const *)LLMJAIL_ARGVS[backend], envp, AT_EMPTY_PATH);
    return fail(4, "exec");
}
