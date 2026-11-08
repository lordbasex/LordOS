/* claudebridge.c — lord-claude-status: the Terminal footer's source for Claude Code (docs/TERMINAL.md).
 *
 * Claude Code runs its status line command on every update with a JSON object on stdin: the context window
 * (context_window.used_percentage, context_window_size), the plan's limits (rate_limits.five_hour and
 * .seven_day: used_percentage, resets_at), the model and the session. The Terminal's "Connect" puts this
 * program in front of the user's own command in ~/.claude/settings.json:
 *
 *   "statusLine": { "command": "/usr/lib/lordos/bin/lord-claude-status bash ~/.claude/statusline-command.sh" }
 *
 * It keeps that JSON as ~/.cache/lordos/claude/<session_id>.json (0600, in a 0700 folder of the user's, written
 * whole and renamed into place), then runs the rest of its arguments with the same stdin, so the user's status
 * line is unchanged. With no arguments it prints nothing. It is not lord-server's: Claude Code runs it, as the
 * user, as it ran the command it wraps. Snapshots older than a week are removed. */
#include "../sec/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IN_MAX (1u << 20)                  /* Claude Code's JSON is a few KB */
#define KEEP_SECS (7 * 24 * 3600)

/* a session id as Claude Code writes it (a UUID): the file's name, so nothing else */
static int valid_session(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > 64) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || s[i] == '-')) return 0;
    return 1;
}

/* <dirfd>/<name> as a folder of ours (0700), not a link; the new fd or -1 */
static int sub_dir(int dirfd, const char *name) {
    if (mkdirat(dirfd, name, 0700) && errno != EEXIST) return -1;
    int fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || st.st_uid != getuid())) { close(fd); return -1; }
    return fd;
}

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void keep(const char *data, size_t len) {
    JDoc doc = { 0 };
    if (json_parse(data, len, &doc, NULL, 0) || !doc.root || doc.root->type != J_OBJ) { json_free(&doc); return; }
    const char *sid = json_str(doc.root, "session_id");
    char name[80];
    int ok = valid_session(sid);
    if (ok) snprintf(name, sizeof name, "%s.json", sid);
    json_free(&doc);
    const char *home = getenv("HOME");
    if (!ok || !home || home[0] != '/') return;
    int h = open(home, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (h < 0) return;
    int a = sub_dir(h, ".cache");
    close(h);
    int b = a >= 0 ? sub_dir(a, "lordos") : -1;
    if (a >= 0) close(a);
    int d = b >= 0 ? sub_dir(b, "claude") : -1;
    if (b >= 0) close(b);
    if (d < 0) return;
    char tmp[96];
    snprintf(tmp, sizeof tmp, ".%s.%d", name, (int)getpid());
    int fd = openat(d, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        int bad = write_all(fd, data, len);
        if (close(fd) || bad || renameat(d, tmp, d, name)) unlinkat(d, tmp, 0);
    }
    /* the week's sessions only */
    int dd = dup(d);
    DIR *dir = dd >= 0 ? fdopendir(dd) : NULL;
    if (!dir && dd >= 0) close(dd);
    time_t now = time(NULL);
    for (struct dirent *e; dir && (e = readdir(dir));) {
        struct stat st;
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        if (!fstatat(d, e->d_name, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && now - st.st_mtime > KEEP_SECS)
            unlinkat(d, e->d_name, 0);
    }
    if (dir) closedir(dir);
    close(d);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    char *buf = malloc(IN_MAX + 1);
    if (!buf) return 1;
    size_t len = 0;
    int over = 0;
    for (;;) {
        char *p = over ? buf : buf + len;
        size_t room = over ? IN_MAX : IN_MAX - len;
        if (!room) { over = 1; continue; }
        ssize_t r = read(0, p, room);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        if (!over) len += (size_t)r;
    }
    if (!over) keep(buf, len);
    if (argc < 2) { free(buf); return 0; }
    /* the user's own status line, with the same input */
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC)) { free(buf); return 1; }
    pid_t pid = fork();
    if (pid < 0) { free(buf); return 1; }
    if (pid == 0) {
        dup2(pfd[0], 0);
        execvp(argv[1], argv + 1);
        _exit(127);
    }
    close(pfd[0]);
    if (!over) write_all(pfd[1], buf, len);
    close(pfd[1]);
    free(buf);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
