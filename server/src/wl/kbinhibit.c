/* kbinhibit.c — see kbinhibit.h. No Wayland here: wl.c keeps the objects and sends the events. The identity is read
 * through file descriptors only (the client's /proc/<pid> and its root), never by a path a link could redirect. */
#include "kbinhibit.h"
#include "dndgate.h"
#include "keygrab.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* An app id (Flatpak's, reverse DNS) goes into permisos.json and in front of the person: plain characters. */
static int id_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}
static int id_ok(const char *s, size_t room) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n >= room || s[0] == '.' || s[0] == '-' || strstr(s, "..")) return 0;
    for (; *s; s++) if (!id_char((unsigned char)*s)) return 0;
    return 1;
}
/* A program's path: absolute, no "." or ".." component, no "//", plain characters (no space, quote or newline). */
static int exe_ok(const char *e, size_t room) {
    if (!e || e[0] != '/' || e[1] == 0 || strlen(e) >= room || strstr(e, "//") || strstr(e, "/./") || strstr(e, "/../")) return 0;
    size_t n = strlen(e);
    if (e[n - 1] == '/' || (n >= 2 && !strcmp(e + n - 2, "/.")) || (n >= 3 && !strcmp(e + n - 3, "/.."))) return 0;
    for (const unsigned char *p = (const unsigned char *)e; *p; p++)
        if (!id_char(*p) && *p != '/' && *p != '+' && *p != '@' && *p != '~') return 0;
    return 1;
}

#define FLATPAK "flatpak:"
#define PROGRAM "program:"

int kbi_key_kind(const char *key, const char **rest) {
    if (!key) return 0;
    if (!strncmp(key, FLATPAK, 8) && id_ok(key + 8, KBI_ID_MAX - 8)) { if (rest) *rest = key + 8; return 'f'; }
    if (!strncmp(key, PROGRAM, 8) && exe_ok(key + 8, KBI_ID_MAX - 8)) { if (rest) *rest = key + 8; return 'p'; }
    return 0;
}
int kbi_key_valid(const char *key) { return kbi_key_kind(key, NULL) != 0; }

int kbi_flatpak_name(const char *text, size_t len, char *out, size_t n) {
    if (!text || !out || !n) return 0;
    out[0] = 0;
    int in_app = 0, found = 0;
    for (size_t i = 0; i < len;) {
        size_t e = i;
        while (e < len && text[e] != '\n') e++;
        const char *l = text + i;
        size_t ll = e - i;
        if (ll && l[ll - 1] == '\r') ll--;
        if (memchr(l, 0, ll)) return 0;                     /* not a text file */
        if (ll && l[0] == '[') in_app = ll == 13 && !memcmp(l, "[Application]", 13);
        else if (in_app && ll > 5 && !memcmp(l, "name=", 5)) {
            if (found || ll - 5 >= n) return 0;             /* twice (which one?), or too long */
            memcpy(out, l + 5, ll - 5);
            out[ll - 5] = 0;
            found = 1;
        }
        i = e + 1;
    }
    if (found && id_ok(out, KBI_ID_MAX - 8)) return 1;
    out[0] = 0;
    return 0;
}

#define INFO_MAX (64 * 1024)

/* A Flatpak sandbox's .flatpak-info in rootfd → "flatpak:<name>". 1, or 0 (no file, a link, not a regular file,
 * too big, no valid name). */
static int flatpak_identity(int rootfd, char *out, size_t n) {
    int fd = openat(rootfd, ".flatpak-info", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    char *buf = NULL;
    ssize_t got = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= INFO_MAX && (buf = malloc((size_t)st.st_size + 1))) {
        size_t have = 0;
        while (have < (size_t)st.st_size) {
            ssize_t r = read(fd, buf + have, (size_t)st.st_size - have);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            have += (size_t)r;
        }
        got = (ssize_t)have;
    }
    close(fd);
    char name[KBI_ID_MAX];
    int ok = got > 0 && kbi_flatpak_name(buf, (size_t)got, name, sizeof name) && snprintf(out, n, FLATPAK "%s", name) < (int)n;
    free(buf);
    return ok;
}

/* The program a process in our own root and namespaces runs → "program:<path>". The kernel's name for its exe
 * must be the canonical path of a file that is still there and is the very file it runs (same device and inode),
 * so a deleted, renamed or replaced program names no one. */
static int program_identity(int procfd, char *out, size_t n) {
    char lnk[PATH_MAX], real[PATH_MAX];
    ssize_t l = readlinkat(procfd, "exe", lnk, sizeof lnk - 1);
    if (l <= 0) return 0;
    lnk[l] = 0;
    struct stat run, there;
    if (!realpath(lnk, real) || strcmp(real, lnk)) return 0;
    if (fstatat(procfd, "exe", &run, 0) || stat(real, &there) || !S_ISREG(there.st_mode) ||
        run.st_dev != there.st_dev || run.st_ino != there.st_ino) return 0;
    if (!exe_ok(real, KBI_ID_MAX - 8)) return 0;
    return snprintf(out, n, PROGRAM "%s", real) < (int)n;
}

#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif

/* A pidfd's pid in our pid namespace (its fdinfo's "Pid:"), or -1 (gone, elsewhere, unreadable). */
static int pid_of_pidfd(int fd) {
    char p[64], line[128];
    snprintf(p, sizeof p, "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(p, "re");
    if (!f) return -1;
    int pid = -1;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "Pid: %d", &pid) == 1) break;
    fclose(f);
    return pid > 0 ? pid : -1;
}

int kbi_peer_proc(int sock, int pid) {
    int pidfd = -1;
    socklen_t len = sizeof pidfd;
    if (sock < 0 || getsockopt(sock, SOL_SOCKET, SO_PEERPIDFD, &pidfd, &len) != 0 || len != sizeof pidfd) pidfd = -1;
    if (pidfd >= 0) {
        int p = pid_of_pidfd(pidfd);
        if (p <= 0 || (pid > 0 && p != pid)) { close(pidfd); return -1; }
        pid = p;
    }
    char path[32];
    int procfd = -1;
    if (pid > 0) {
        snprintf(path, sizeof path, "/proc/%d", pid);
        procfd = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    }
    /* still alive after the open: the directory is that process's, not a later one's with its pid */
    if (pidfd >= 0) {
        if (procfd >= 0 && syscall(SYS_pidfd_send_signal, pidfd, 0, NULL, 0) != 0) { close(procfd); procfd = -1; }
        close(pidfd);
    }
    return procfd;
}

int kbi_identity(int procfd, char *out, size_t n) {
    if (!out || n < KBI_ID_MAX) return 0;
    out[0] = 0;
    if (procfd < 0) return 0;
    int root = openat(procfd, "root", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (root < 0) return 0;
    int flatpak = dndg_root_sandboxed(root);
    int ok = flatpak ? flatpak_identity(root, out, n) : !dndg_proc_sandboxed(procfd) && program_identity(procfd, out, n);
    close(root);
    if (!ok) out[0] = 0;
    return ok;
}

int kbi_decide(int state, int isolated, int greeter, int has_key) {
    if (isolated || greeter || !has_key || state == 2) return KBI_NEVER;
    return state == 1 ? KBI_GRANT : KBI_PROMPT;
}

static int update(KbiState *k) {
    int want = k->allowed && !k->off;
    if (want == k->active) return KBI_SAME;
    k->active = want;
    return want ? KBI_ACTIVE : KBI_INACTIVE;
}

int kbi_set_allowed(KbiState *k, int allowed) {
    k->allowed = !!allowed;
    if (!k->allowed) k->off = 0;        /* allowed again later: it holds again */
    return update(k);
}

int kbi_toggle(KbiState *k) {
    if (!k->allowed) return KBI_SAME;
    k->off = !k->off;
    return update(k);
}

int kbi_route(int code, char letter, unsigned mods, int active) {
    if (code == KEY_ESC && mods == KG_META) return KBI_KEY_TOGGLE;
    if (!active) return KBI_KEY_LORD;
    /* the screen lock is always reachable: a person leaving a VM in front can still lock */
    if (mods == KG_META && letter == 'l') return KBI_KEY_LORD;
    if (mods == (KG_META | KG_CTRL) && letter == 'q') return KBI_KEY_LORD;
    /* the console switch, as without an inhibitor */
    if ((mods & (KG_CTRL | KG_ALT)) == (KG_CTRL | KG_ALT) && !(mods & KG_META) && code >= KEY_F1 && code <= KEY_F12) return KBI_KEY_LORD;
    if (keygrab_system_key(code)) return KBI_KEY_LORD;
    return KBI_KEY_APP;
}
