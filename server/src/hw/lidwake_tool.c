/* lidwake_tool.c — lordos-lid-wake, the systemd-sleep hook for laptops that wake up right after they
 * go to sleep with the lid open (GitHub #172). Installed by tools/iso/mkdeb.sh as
 * /usr/lib/systemd/system-sleep/lordos-lid-wake; systemd-sleep runs it as root with "pre" or "post"
 * and the kind of sleep, around every sleep. It is not a service and runs nothing else.
 *
 *   pre:  on a portable computer with every lid open, each LID* device that is enabled in
 *         /proc/acpi/wakeup is written there (writing a name toggles it), and its name is kept in
 *         /run/lordos/lid-wake (root, 0600) first.
 *   post: each name in that note that is now disabled is written once more (back on), and the note
 *         is removed.
 *
 * Fixed paths, kernel files only, parsed by hw/lidwake.h (tests/lidwake_test.c). Libc only. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lidwake.h"

#define CHASSIS_PATH "/sys/class/dmi/id/chassis_type"
#define WAKEUP_PATH  "/proc/acpi/wakeup"
#define LID_DIR      "/proc/acpi/button/lid"
#define RUN_DIR      "/run/lordos"
#define NOTE_NAME    "lid-wake"
#define NOTE_TMP     "lid-wake.tmp"
#define WAKEUP_CAP   65536

static char wakebuf[WAKEUP_CAP];

#define LOG(...) fprintf(stderr, "lordos-lid-wake: " __VA_ARGS__)

/* The whole file into b (at most cap - 1 bytes; a longer one is refused). Its length, or -1. */
static long read_all_at(int dir, const char *path, char *b, size_t cap) {
    int fd = openat(dir, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t n = 0;
    for (;;) {
        if (n >= cap - 1) { close(fd); return -1; }
        ssize_t r = read(fd, b + n, cap - 1 - n);
        if (r < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    b[n] = 0;
    return (long)n;
}
static long read_all(const char *path, char *b, size_t cap) { return read_all_at(AT_FDCWD, path, b, cap); }

static int read_wakeup(LidWakeEntry *e) {
    long n = read_all(WAKEUP_PATH, wakebuf, sizeof wakebuf);
    if (n < 0) return -1;
    return lidwake_parse_wakeup(wakebuf, (size_t)n, e, LIDWAKE_MAX);
}

/* Every lid under /proc/acpi/button/lid, folded (hw/lidwake.h). No lid at all: unknown. */
static int lid_state(void) {
    DIR *d = opendir(LID_DIR);
    if (!d) return LIDWAKE_LID_UNKNOWN;
    int acc = LIDWAKE_LID_UNKNOWN, seen = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        int s = LIDWAKE_LID_UNKNOWN;
        if (seen < LIDWAKE_MAX && lidwake_name_valid(de->d_name, strlen(de->d_name))) {
            char path[64], b[64];
            snprintf(path, sizeof path, LID_DIR "/%s/state", de->d_name);
            long n = read_all(path, b, sizeof b);
            if (n >= 0) s = lidwake_lid_state(b, (size_t)n);
        }
        acc = lidwake_lid_fold(acc, seen, s);
        seen++;
    }
    closedir(d);
    return acc;
}

/* Writes a name read from /proc/acpi/wakeup back to it: the kernel toggles that device. */
static int toggle(const char *name) {
    if (!lidwake_name_valid(name, strlen(name))) return -1;
    int fd = open(WAKEUP_PATH, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t len = strlen(name);
    ssize_t w = write(fd, name, len);
    int err = errno;
    close(fd);
    if (w != (ssize_t)len) { errno = w < 0 ? err : EIO; return -1; }
    return 0;
}

/* /run/lordos, made if missing, only when it is root's own directory that nobody else can write. */
static int run_dir(int create) {
    if (create && mkdir(RUN_DIR, 0755) < 0 && errno != EEXIST) return -1;
    int fd = open(RUN_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022)) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    return fd;
}

/* The names kept by an earlier pre without its post (one boot; /run is emptied at boot). */
static int read_note(int dir, char names[][LIDWAKE_NAME + 1]) {
    char b[LIDWAKE_MAX * (LIDWAKE_NAME + 1) + 2];
    long n = read_all_at(dir, NOTE_NAME, b, sizeof b);
    if (n < 0) return errno == ENOENT ? 0 : -1;
    return lidwake_parse_note(b, (size_t)n, names, LIDWAKE_MAX);
}

static int write_note(int dir, char names[][LIDWAKE_NAME + 1], int n) {
    char b[LIDWAKE_MAX * (LIDWAKE_NAME + 1) + 1];
    size_t len = 0;
    for (int i = 0; i < n; i++) len += (size_t)snprintf(b + len, sizeof b - len, "%s\n", names[i]);
    unlinkat(dir, NOTE_TMP, 0);
    int fd = openat(dir, NOTE_TMP, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (fchmod(fd, 0600) < 0 || write(fd, b, len) != (ssize_t)len) {
        close(fd);
        unlinkat(dir, NOTE_TMP, 0);
        return -1;
    }
    close(fd);
    if (renameat(dir, NOTE_TMP, dir, NOTE_NAME) < 0) { unlinkat(dir, NOTE_TMP, 0); return -1; }
    return 0;
}

static int has_name(char names[][LIDWAKE_NAME + 1], int n, const char *name) {
    for (int i = 0; i < n; i++) if (!strcmp(names[i], name)) return 1;
    return 0;
}

static int pre(void) {
    char b[16];
    long n = read_all(CHASSIS_PATH, b, sizeof b);
    int type = n < 0 ? -1 : lidwake_chassis(b, (size_t)n);
    if (!chassis_portable(type)) return 0;          /* a desktop, a server, no DMI: nothing at all */
    LidWakeEntry e[LIDWAKE_MAX];
    int ne = read_wakeup(e);
    if (ne < 0) { LOG("%s doesn't read as expected; nothing changed\n", WAKEUP_PATH); return 0; }
    if (ne == 0) return 0;
    int lid = lid_state();
    unsigned mask = lidwake_plan_pre(type, lid, e, ne);
    if (!mask) return 0;                            /* lid closed or unknown, or no lid wakes */

    int dir = run_dir(1);
    if (dir < 0) { LOG("%s: %s; nothing changed\n", RUN_DIR, strerror(errno)); return 1; }
    char names[LIDWAKE_MAX][LIDWAKE_NAME + 1];
    int nn = read_note(dir, names);
    if (nn < 0) nn = 0;                             /* a bad note is replaced */
    for (int i = 0; i < ne; i++)
        if ((mask >> i & 1) && !has_name(names, nn, e[i].name) && nn < LIDWAKE_MAX)
            memcpy(names[nn++], e[i].name, sizeof names[0]);
    /* The note goes first: if this stops halfway, post still turns back on what was turned off. */
    if (write_note(dir, names, nn) < 0) {
        LOG("can't write %s/%s: %s; nothing changed\n", RUN_DIR, NOTE_NAME, strerror(errno));
        close(dir);
        return 1;
    }
    close(dir);
    int rc = 0;
    for (int i = 0; i < ne; i++) {
        if (!(mask >> i & 1)) continue;
        if (toggle(e[i].name) < 0) { LOG("%s: %s\n", e[i].name, strerror(errno)); rc = 1; continue; }
        LOG("%s off as a wake source while the lid is open\n", e[i].name);
    }
    return rc;
}

static int post(void) {
    int dir = run_dir(0);
    if (dir < 0) return errno == ENOENT ? 0 : 1;
    char names[LIDWAKE_MAX][LIDWAKE_NAME + 1];
    int nn = read_note(dir, names);
    if (nn == 0) { unlinkat(dir, NOTE_NAME, 0); close(dir); return 0; }
    if (nn < 0) {
        LOG("%s/%s doesn't read as expected; removed\n", RUN_DIR, NOTE_NAME);
        unlinkat(dir, NOTE_NAME, 0);
        close(dir);
        return 1;
    }
    LidWakeEntry e[LIDWAKE_MAX];
    int ne = read_wakeup(e);
    if (ne < 0) {
        LOG("%s doesn't read as expected; the note stays for the next wake\n", WAKEUP_PATH);
        close(dir);
        return 1;
    }
    unsigned mask = lidwake_plan_post(names, nn, e, ne);
    int rc = 0;
    for (int i = 0; i < ne; i++) {
        if (!(mask >> i & 1)) continue;
        if (toggle(e[i].name) < 0) { LOG("%s: %s\n", e[i].name, strerror(errno)); rc = 1; continue; }
        LOG("%s back on as a wake source\n", e[i].name);
    }
    unlinkat(dir, NOTE_NAME, 0);
    close(dir);
    return rc;
}

int main(int argc, char **argv) {
    umask(077);
    int what = lidwake_args(argc, argv);
    if (!what) { fprintf(stderr, "usage: lordos-lid-wake pre|post suspend|hibernate|hybrid-sleep|suspend-then-hibernate\n"); return 2; }
    if (geteuid() != 0) { LOG("runs only as root, from systemd-sleep\n"); return 1; }
    return what == 1 ? pre() : post();
}
