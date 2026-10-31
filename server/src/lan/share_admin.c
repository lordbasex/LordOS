/* share_admin.c — mounting a network share, as root, closed (approved 2026-10-25, docs/ROADMAP.md):
 *
 *   share-mount "smb|nfs\n<ipv4>\n<share or path>\n<user>\n<domain>\n<password>"
 *   share-unmount <folder>
 *
 * Everything in the argument is validated here (lan/lanaddr.c): a private IPv4 address and nothing
 * else (no names, so nothing resolves as root), a share or an absolute export path from a strict
 * set. The kernel's own clients do the work through the new mount API, one fsconfig() call per
 * option: no option string to inject into, no program, no shell, no argv. The password arrives on
 * the helper's own channel (the UI sends it with { quiet: true }), goes to one fsconfig() and is wiped; it
 * never reaches a command line, a file or a log. Options are fixed: nosuid, nodev, the session
 * user's uid and gid for SMB, soft mounts with short timeouts (lord.fs reads on the interface's
 * thread, so a server that went away holds it for that long), SMB 2 or later only.
 * The mount goes only under /media/<user>, like disks, and only a cifs or nfs mount there comes off
 * through share-unmount. */
#include "share_admin.h"
#include "lanaddr.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef FSOPEN_CLOEXEC
#define FSOPEN_CLOEXEC 1
#define FSMOUNT_CLOEXEC 1
#define FSCONFIG_SET_FLAG 0
#define FSCONFIG_SET_STRING 1
#define FSCONFIG_CMD_CREATE 6
#define MOVE_MOUNT_F_EMPTY_PATH 4
#define MOUNT_ATTR_NOSUID 2
#define MOUNT_ATTR_NODEV 4
#endif

static int fs_open(const char *t) { return (int)syscall(SYS_fsopen, t, FSOPEN_CLOEXEC); }
static int fs_str(int fd, const char *k, const char *v) { return (int)syscall(SYS_fsconfig, fd, FSCONFIG_SET_STRING, k, v, 0); }
static int fs_flag(int fd, const char *k) { return (int)syscall(SYS_fsconfig, fd, FSCONFIG_SET_FLAG, k, NULL, 0); }

/* Creates what fd was configured for and places it on dir. 0 ok, -1 with errno. */
static int fs_place(int fd, const char *dir) {
    if (syscall(SYS_fsconfig, fd, FSCONFIG_CMD_CREATE, NULL, NULL, 0)) return -1;
    int m = (int)syscall(SYS_fsmount, fd, FSMOUNT_CLOEXEC, MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV);
    if (m < 0) return -1;
    int r = (int)syscall(SYS_move_mount, m, "", AT_FDCWD, dir, MOVE_MOUNT_F_EMPTY_PATH);
    int e = errno;
    close(m);
    errno = e;
    return r;
}

static int mount_smb(const AdminReq *r, const ShareReq *q, const char *dir) {
    int fd = fs_open("cifs");
    if (fd < 0) return -1;
    char src[300], uid[16], gid[16];
    snprintf(src, sizeof src, "//%s/%s", q->addr, q->share);
    snprintf(uid, sizeof uid, "%u", (unsigned)r->uid);
    snprintf(gid, sizeof gid, "%u", (unsigned)r->gid);
    int bad = fs_str(fd, "source", src) || fs_str(fd, "ip", q->addr) || fs_str(fd, "vers", "default") ||
              fs_str(fd, "uid", uid) || fs_str(fd, "gid", gid) || fs_flag(fd, "forceuid") || fs_flag(fd, "forcegid") ||
              fs_str(fd, "file_mode", "0600") || fs_str(fd, "dir_mode", "0700") || fs_flag(fd, "soft") || fs_str(fd, "echo_interval", "3");
    if (!bad) {
        if (q->user[0]) {
            bad = fs_str(fd, "username", q->user) || (q->password[0] && fs_str(fd, "password", q->password));
            if (!bad && q->domain[0]) bad = fs_str(fd, "domain", q->domain);
            if (!bad) bad = fs_str(fd, "sec", "ntlmssp");
        } else bad = fs_flag(fd, "guest");
    }
    int rc = bad ? -1 : fs_place(fd, dir);
    int e = errno;
    close(fd);
    errno = e;
    return rc;
}

/* The NFS versions, newest first. NFSv3's lock daemon isn't there without nfs-common, so its locks
 * are local (nolock). */
static int mount_nfs(const ShareReq *q, const char *dir) {
    static const struct { const char *type, *vers; } v[] = { { "nfs4", "4.2" }, { "nfs4", "4.1" }, { "nfs4", "4.0" }, { "nfs", "3" } };
    char src[300];
    snprintf(src, sizeof src, "%s:%s", q->addr, q->share);
    int e = ENOTSUP;
    for (size_t i = 0; i < sizeof v / sizeof *v; i++) {
        int fd = fs_open(v[i].type);
        if (fd < 0) { e = errno; continue; }
        int bad = fs_str(fd, "source", src) || fs_str(fd, "addr", q->addr) || fs_str(fd, "vers", v[i].vers) || fs_str(fd, "proto", "tcp") ||
                  fs_flag(fd, "soft") || fs_str(fd, "timeo", "30") || fs_str(fd, "retrans", "1") || fs_str(fd, "sec", "sys");
        if (!bad && !strcmp(v[i].vers, "4.0")) bad = fs_str(fd, "clientaddr", "0.0.0.0");
        if (!bad && !strcmp(v[i].vers, "3")) bad = fs_flag(fd, "nolock") || fs_str(fd, "mountaddr", q->addr) || fs_str(fd, "mountvers", "3") || fs_str(fd, "mountproto", "tcp");
        int rc = bad ? -1 : fs_place(fd, dir);
        e = errno;
        close(fd);
        if (!rc) return 0;
        if (e == EACCES || e == EPERM) break;           /* the server said no: another version won't change that */
    }
    errno = e;
    return -1;
}

void share_act_mount(const AdminReq *r, const char *arg, size_t len) {
    char buf[900];
    if (len >= sizeof buf) { admin_err("admin.invalidArgument", NULL); return; }
    memcpy(buf, arg, len);
    buf[len] = 0;
    ShareReq q;
    if (share_req_parse(buf, &q)) { explicit_bzero(buf, sizeof buf); admin_err("admin.invalidArgument", NULL); return; }
    char base[96], dir[PATH_MAX], leaf[220];
    mkdir("/media", 0755);
    snprintf(base, sizeof base, "/media/%.64s", r->user);
    if (mkdir(base, 0750) == 0 || errno == EEXIST) { if (chown(base, 0, r->gid)) { /* keep going */ } chmod(base, 0750); }
    share_leaf(&q, leaf, sizeof leaf);
    snprintf(dir, sizeof dir, "%s/%s", base, leaf);
    if (mkdir(dir, 0700)) {
        struct stat st;
        /* an empty leftover folder (not a mount point) is reused */
        if (errno != EEXIST || lstat(dir, &st) || !S_ISDIR(st.st_mode) || rmdir(dir) || mkdir(dir, 0700)) {
            explicit_bzero(buf, sizeof buf);
            admin_err("share.alreadyMounted", "path", dir, NULL);
            return;
        }
    }
    if (chown(dir, r->uid, r->gid)) { /* the file system's own owner replaces it */ }
    int rc = q.nfs ? mount_nfs(&q, dir) : mount_smb(r, &q, dir);
    int e = errno;
    explicit_bzero(buf, sizeof buf);
    if (rc) {
        rmdir(dir);
        if (e == EACCES || e == EPERM || e == EKEYREJECTED) admin_err("share.denied", "name", q.share, NULL);
        else if (e == ENODEV) admin_err("share.noKernelSupport", NULL);
        else admin_err("share.mountFailed", "name", q.share, "errno", lerr_errno(e), NULL);
        return;
    }
    admin_say("OK", "%s", dir);   /* i18n-ok */
}

/* mountinfo's mount point field, unescaped, equals path and its type is a share's */
static int mounted_share(const char *path) {
    FILE *f = fopen("/proc/self/mountinfo", "re");
    if (!f) return 0;
    char line[4096];
    int yes = 0;
    while (fgets(line, sizeof line, f)) {
        char mp[2048], *sep = strstr(line, " - ");
        char type[32];
        if (!sep || sscanf(line, "%*s %*s %*s %*s %2047s", mp) != 1 || sscanf(sep + 3, "%31s", type) != 1) continue;
        char *o = mp;
        for (const char *s = mp; *s; s++, o++) {
            if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') { *o = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0')); s += 3; }
            else *o = *s;
        }
        *o = 0;
        if (!strcmp(mp, path)) yes = share_fstype(type);    /* the last mount on it decides */
    }
    fclose(f);
    return yes;
}

void share_act_unmount(const AdminReq *r, const char *arg, size_t len) {
    char path[PATH_MAX];
    if (len >= sizeof path) { admin_err("admin.invalidArgument", NULL); return; }
    memcpy(path, arg, len);
    path[len] = 0;
    if (share_path_ok(path, r->user)) { admin_err("admin.invalidArgument", NULL); return; }
    if (!mounted_share(path)) { admin_err("share.notMounted", NULL); return; }
    if (umount2(path, UMOUNT_NOFOLLOW)) {
        if (errno != EBUSY) { admin_err("share.unmountFailed", "errno", lerr_errno(errno), NULL); return; }
        admin_err("share.busy", NULL);
        return;
    }
    rmdir(path);
    admin_ok("share.unmounted", NULL);
}
