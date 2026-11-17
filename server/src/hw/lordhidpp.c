/* lordhidpp.c — lord-hidpp: the battery of Logitech mice and keyboards over HID++, for the Devices widget, the bar
 * and Monitor (docs/DEVICES.md). Maintainer's approval 2026-11-17 ("apruebo lord-hidpp para leer la batería"):
 * battery reads only.
 *
 * Installed root:lordos-hidpp, mode 2755. Its group is the only one that may open a Logitech device's own hidraw
 * node (packaging/udev/72-lordos-hidpp.rules: the nodes of the kernel's logitech-hidpp-device driver, never a
 * receiver's, so nothing can be paired or unpaired through them). It:
 *   1. finds those nodes in sysfs and opens them (at most HIDPP_MAX), each checked to be the character device sysfs
 *      names;
 *   2. gives up the group for good (setresgid to the caller's) and checks it can't get it back;
 *   3. applies the helpers' sandbox (no network, no files, a syscall allowlist, no privileges);
 *   4. asks each device for its battery (hidpp.c: features 0x1004, 0x1000, 0x1001 and nothing else) and writes one
 *      line per answer to stdout: "<hidraw number> <percent> <status>" (status 0 discharging, 1 charging, 2 full,
 *      3 charging without a level, its percent 0).
 * No names or other strings: lord-server finds the device from the number in sysfs itself. Takes no arguments
 * and reads no environment. */
#include "hidpp.h"
#include "../sec/sandbox.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define HIDPP_MAX 12

static int read_small(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    return 0;
}

/* a device node of the kernel's HID++ driver: its number, opened read-write; -1 when it isn't one */
static int open_node(const char *name, int *num) {
    if (strncmp(name, "hidraw", 6) || !name[6] || strlen(name) > 9) return -1;
    for (const char *p = name + 6; *p; p++) if (!isdigit((unsigned char)*p)) return -1;
    char path[96], buf[4096];
    snprintf(path, sizeof path, "/sys/class/hidraw/%s/device/uevent", name);
    if (read_small(path, buf, sizeof buf)) return -1;
    int ours = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
        if (!strcmp(line, "DRIVER=logitech-hidpp-device")) ours = 1;
    if (!ours) return -1;
    snprintf(path, sizeof path, "/sys/class/hidraw/%s/dev", name);
    unsigned maj, min;
    if (read_small(path, buf, 32) || sscanf(buf, "%u:%u", &maj, &min) != 2) return -1;
    snprintf(path, sizeof path, "/dev/%s", name);
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISCHR(st.st_mode) || st.st_rdev != makedev(maj, min)) { close(fd); return -1; }
    *num = atoi(name + 6);
    return fd;
}

int main(int argc, char **argv) {
    (void)argv;
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    prctl(PR_SET_DUMPABLE, 0);
    struct rlimit nocore = { 0, 0 };
    setrlimit(RLIMIT_CORE, &nocore);
    alarm(10);                                   /* a device that never answers can't keep it alive */
    if (argc != 1) return 2;

    int fds[HIDPP_MAX], nums[HIDPP_MAX], n = 0;
    DIR *d = opendir("/sys/class/hidraw");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < HIDPP_MAX) {
            int num, fd = open_node(e->d_name, &num);
            if (fd >= 0) { fds[n] = fd; nums[n] = num; n++; }
        }
        closedir(d);
    }

    /* the group only opened the nodes: gone now, and for good */
    gid_t gid = getgid(), egid = getegid();
    if (setgroups(0, NULL) && geteuid() == 0) return 3;
    if (setresgid(gid, gid, gid) || getegid() != gid) return 3;
    if (egid != gid && setegid(egid) == 0) return 3;

    SandboxHelperCfg cfg = {
        .name = "lordos-hidpp",
        .ro_paths = NULL,
        .extra_syscalls = (int[]){ -1 },
        .allow_ioctl_classes = 0,
        .mem_max_mb = 16,
        .ro_dir_fds = NULL,
        .rw_dir_fds = NULL,
    };
    sandbox_apply_helper(&cfg);

    for (int i = 0; i < n; i++) {
        HidppBattery b;
#ifdef HIDPP_DEBUG
        fprintf(stderr, "hidraw%d:\n", nums[i]);
#endif
        /* status 3: charging with no level ("<n> 0 3") */
        if (hidpp_battery(fds[i], 400, &b) == 0) printf("%d %d %d\n", nums[i], b.percent < 0 ? 0 : b.percent, b.percent < 0 ? 3 : b.status);
        close(fds[i]);
    }
    fflush(stdout);
    return 0;
}
