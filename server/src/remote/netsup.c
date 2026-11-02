/* netsup.c — lord-sessiond supervises lord-netd, the system network (docs/NETWORK.md, "The network
 * belongs to the system"; the process itself is admin.c built with LORD_NETD plus net/netd.c).
 *
 *   - Started at boot, before the greeter, so there is a network (and an IP) before anyone logs
 *     in, and the remote desktop is reachable.
 *   - Fixed path: <lord-sessiond's directory>/lord-netd, root-owned and not writable by others
 *     (unless insecure_dev, as for lord-server). No arguments and a minimal environment.
 *   - If it exits, it is started again with a growing delay (1, 2, 4… up to 30 s).
 *   - Restarting lord-sessiond (KillMode=process) doesn't cut the network: lord-netd keeps running
 *     and the new lord-sessiond adopts it (a pidfd on the pid in /run/lordos/netd.pid), no polling.
 *   - Leaves /run/lordos/netd.enabled: with that mark, the sessions' helpers never bring the
 *     network up themselves (while lord-netd restarts, the action answers with an error). */
#include "sessiond.h"
#include "../sec/cgroup.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define NETD_PIDFILE SD_RUN_DIR "/netd.pid"
#define NETD_MARK    SD_RUN_DIR "/netd.enabled"

static pid_t child = -1;                /* lord-netd as our child */
static int pidfd = -1;                  /* an adopted one (from an earlier lord-sessiond) */
static double next_try;                 /* when to start it again */
static int fails;
static double started;                  /* when it was started or adopted (if it lasted, the delay goes back to 1 s) */
static char path[PATH_MAX];

static void netd_path(void) {
    if (path[0]) return;
    snprintf(path, sizeof path, "%s", rd_conf.server);
    char *slash = strrchr(path, '/');
    if (slash) snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "lord-netd");
}

/* Is a lord-netd from an earlier lord-sessiond still alive? */
static int adopt(void) {
    FILE *f = fopen(NETD_PIDFILE, "r");
    int p = 0;
    if (f) { if (fscanf(f, "%d", &p) != 1) p = 0; fclose(f); }
    if (p <= 1) return -1;
    /* the pidfile is root's; the process must be root's and named lord-netd (prctl). /proc/<pid>/exe
     * is not compared: if the binary was updated while running, it points to another name */
    char proc[64], comm[32] = "";
    struct stat st;
    snprintf(proc, sizeof proc, "/proc/%d", p);
    if (stat(proc, &st) || st.st_uid != 0) return -1;
    snprintf(proc, sizeof proc, "/proc/%d/comm", p);
    f = fopen(proc, "r");
    if (!f) return -1;
    if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
    fclose(f);
    comm[strcspn(comm, "\n")] = 0;
    if (strcmp(comm, "lord-netd")) return -1;
    int fd = (int)syscall(SYS_pidfd_open, p, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    pidfd = fd;
    started = rd_now();
    sd_log("lord-netd still running (pid %d): the network was not interrupted", p);
    return 0;
}

void sd_netd_start(void) {
    netd_path();
    if (child > 0 || pidfd >= 0) return;
    struct stat st;
    if (stat(path, &st)) { if (!fails++) sd_log("no %s: each session brings the network up, as before", path); next_try = rd_now() + 3600; return; }
    if (!rd_conf.insecure_dev && (st.st_uid != 0 || (st.st_mode & 022))) {
        sd_log("%s is not root-owned or is writable by others: not started", path);
        next_try = rd_now() + 3600;
        return;
    }
    mkdir(SD_RUN_DIR, 0755);
    int m = open(NETD_MARK, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (m >= 0) close(m);
    if (!adopt()) return;
    pid_t p = fork();
    if (p == 0) {
        syscall(SYS_close_range, 3, ~0u, 0);    /* nothing from the monitor or the connections */
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) dup2(null, 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        setsid();                               /* does not die with lord-sessiond (KillMode=process) */
        cgroup_service_leaf("netd");            /* its own leaf: Monitor → Network counts its traffic (0.8.5) */
        char *const argv[] = { path, NULL };
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", NULL };
        execve(path, argv, env);
        _exit(127);
    }
    if (p < 0) { next_try = rd_now() + 5; return; }
    child = p;
    started = rd_now();
    sd_log("lord-netd started (pid %d)", (int)p);
}

int sd_netd_fd(void) { return pidfd; }

static void schedule(const char *why) {
    if (rd_now() - started > 60) fails = 0;
    double wait = fails < 5 ? (double)(1 << fails) : 30;
    fails++;
    next_try = rd_now() + wait;
    sd_log("lord-netd %s: restarting in %.0f s", why, wait);
}

void sd_netd_event(void) {                      /* the adopted one's pidfd says it exited */
    close(pidfd);
    pidfd = -1;
    schedule("(adopted) exited");
}

int sd_netd_reaped(pid_t pid, int status) {
    if (pid != child) return 0;
    child = -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 3) { next_try = rd_now() + 1; adopt(); return 1; }   /* another one was already running */
    char why[48];
    if (WIFEXITED(status)) snprintf(why, sizeof why, "exited with code %d", WEXITSTATUS(status));
    else snprintf(why, sizeof why, "killed by signal %d", WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    schedule(why);
    return 1;
}

void sd_netd_tick(void) {
    if (child > 0 || pidfd >= 0 || rd_now() < next_try) return;
    sd_netd_start();
}
