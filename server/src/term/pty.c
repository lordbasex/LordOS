/* pty.c — creates the pseudo-terminal and launches the user's shell.
 *
 * It is the only way lord-server has to run processes, and it only launches the user's own
 * login shell (the one in /etc/passwd), just like any terminal emulator.
 * The child inherits no capabilities, blocked signals or descriptors from the server. */
#include "term.h"
#include "lord_version.h"           /* build/gen: LordOS version (Makefile, from package.json) */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/capability.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>

static char integration[PATH_MAX]; /* realpath requires room for PATH_MAX bytes */

void pty_set_integration(const char *dir) {
    if (!dir || !realpath(dir, integration)) integration[0] = 0;
}

void pty_resize(int fd, int cols, int rows, int pw, int ph) {
    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, (unsigned short)pw, (unsigned short)ph };
    ioctl(fd, TIOCSWINSZ, &ws);
}

static void child(int fd, const char *cwd, int id) {
    setsid();
    ioctl(fd, TIOCSCTTY, 0);
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) close(fd);
    syscall(SYS_close_range, 3, ~0u, 0);

    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    for (int s = 1; s < 32; s++) signal(s, SIG_DFL);

    /* No capability is passed to the shell (the server keeps CAP_SYS_TTY_CONFIG). */
    struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct caps[2];
    memset(caps, 0, sizeof caps);
    syscall(SYS_capset, &hdr, caps);

    struct passwd *pw = getpwuid(getuid());
    const char *home = getenv("HOME");
    if (!home && pw) home = pw->pw_dir;
    const char *shell = pw && pw->pw_shell && pw->pw_shell[0] ? pw->pw_shell : "/bin/sh";
    struct stat st;
    if (!cwd || stat(cwd, &st) || !S_ISDIR(st.st_mode) || chdir(cwd)) {
        if (!home || chdir(home)) { if (chdir("/")) {} }
    }

    char idbuf[16];
    snprintf(idbuf, sizeof idbuf, "%d", id);
    static const char *const drop[] = { "SUDO_USER", "SUDO_UID", "SUDO_GID", "SUDO_COMMAND", "SUDO_HOME",
                                        "LORD_TERM_INTEGRATION_LOADED", "COLUMNS", "LINES", NULL };
    for (int i = 0; drop[i]; i++) unsetenv(drop[i]);
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    setenv("TERM_PROGRAM", "LordTerminal", 1);
    setenv("TERM_PROGRAM_VERSION", LORD_VERSION, 1);
    setenv("LORD_TERMINAL_SESSION", idbuf, 1);
    setenv("SHELL", shell, 1);
    if (pw) { setenv("USER", pw->pw_name, 1); setenv("LOGNAME", pw->pw_name, 1); }
    if (!getenv("LANG")) setenv("LANG", "C.UTF-8", 1);

    const char *base = strrchr(shell, '/') ? strrchr(shell, '/') + 1 : shell;
    char rc[PATH_MAX + 64], argv0[64];
    snprintf(argv0, sizeof argv0, "-%s", base);   /* "-zsh": login shell */
    if (integration[0] && !strcmp(base, "bash")) {
        /* interactive bash with integration: the rc emulates a login shell and adds the marks. */
        snprintf(rc, sizeof rc, "%s/lord-bash.sh", integration);
        if (access(rc, R_OK) == 0) execl(shell, "bash", "--rcfile", rc, "-i", (char *)NULL);
    } else if (integration[0] && !strcmp(base, "zsh")) {
        /* zsh: ZDOTDIR points to our .zshenv, which restores the user's own and loads the integration. */
        snprintf(rc, sizeof rc, "%s/zsh/.zshenv", integration);
        if (access(rc, R_OK) == 0) {
            const char *orig = getenv("ZDOTDIR");
            if (orig) setenv("LORD_ZDOTDIR", orig, 1); else unsetenv("LORD_ZDOTDIR");
            snprintf(rc, sizeof rc, "%s/zsh", integration);
            setenv("ZDOTDIR", rc, 1);
        }
    } else if (integration[0] && !strcmp(base, "fish") && !strchr(integration, '\'') && !strchr(integration, '\\')) {
        /* fish: --init-command runs after the user's configuration. */
        snprintf(rc, sizeof rc, "%s/lord-fish.fish", integration);
        if (access(rc, R_OK) == 0) {
            char cmd[sizeof rc + 16];
            snprintf(cmd, sizeof cmd, "source '%s'", rc);
            execl(shell, argv0, "--init-command", cmd, (char *)NULL);
        }
    }
    execl(shell, argv0, (char *)NULL);
    execl("/bin/sh", "-sh", (char *)NULL);
    _exit(127);
}

pid_t pty_spawn(const char *cwd, int cols, int rows, int id, int *master) {
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (m < 0) return -1;
    char slave[128];
    if (grantpt(m) || unlockpt(m) || ptsname_r(m, slave, sizeof slave)) { close(m); return -1; }
    pty_resize(m, cols, rows, 0, 0);
    /* The slave is opened before the fork: if the server read the master before the child
     * opened it, Linux would answer EIO and the session would be considered finished. */
    int s = open(slave, O_RDWR | O_NOCTTY);
    if (s < 0) { close(m); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(m); close(s); return -1; }
    if (pid == 0) { close(m); child(s, cwd, id); }
    close(s);
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
    *master = m;
    return pid;
}
