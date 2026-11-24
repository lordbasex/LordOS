/* vmrecv.c — the machine's quarantine folder for files sent from the guest (see vmrecv.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vmrecv.h"
#include "vmagent.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif

static int own_dir(int fd) {
    struct stat st;
    return fd >= 0 && !fstat(fd, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid() && !(st.st_mode & 022);
}

int vmrecv_dir_open(const char *machine_dir, char *shown, size_t sn) {
    char lib[PATH_MAX];
    if (!machine_dir || machine_dir[0] != '/' || strlen(machine_dir) >= sizeof lib) return -1;
    snprintf(lib, sizeof lib, "%s", machine_dir);
    size_t l = strlen(lib);
    while (l > 1 && lib[l - 1] == '/') lib[--l] = 0;
    char *slash = strrchr(lib, '/');
    if (!slash || slash == lib) return -1;
    *slash = 0;
    char base[NAME_MAX + 1];
    snprintf(base, sizeof base, "%s", slash + 1);
    size_t bl = strlen(base);
    if (bl <= 7 || strcmp(base + bl - 7, ".lordvm")) return -1;
    base[bl - 7] = 0;
    if (!base[0] || base[0] == '.') return -1;
    /* the library itself may be a link the person made (a library on another disk); nothing below it is followed */
    int lf = open(lib, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (lf < 0) return -1;
    if (mkdirat(lf, "Received", 0700) && errno != EEXIST) { close(lf); return -1; }
    int rf = openat(lf, "Received", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(lf);
    if (!own_dir(rf)) { if (rf >= 0) close(rf); return -1; }
    if (mkdirat(rf, base, 0700) && errno != EEXIST) { close(rf); return -1; }
    int df = openat(rf, base, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(rf);
    if (!own_dir(df)) { if (df >= 0) close(df); return -1; }
    if (shown) snprintf(shown, sn, "%s/Received/%s", lib, base);
    return df;
}

void vmrecv_parts_trim(int dfd, const char *keep) {
    int d2 = openat(dfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *d = d2 >= 0 ? fdopendir(d2) : NULL;
    if (!d) { if (d2 >= 0) close(d2); return; }
    struct { char n[VMRECV_PART_NAME]; time_t t; } p[64];
    int k = 0;
    time_t now = time(NULL);
    for (struct dirent *e; (e = readdir(d));) {
        if (strncmp(e->d_name, ".lordos-part-", 13) || strlen(e->d_name) >= sizeof p[0].n || (keep && !strcmp(e->d_name, keep))) continue;
        struct stat st;
        if (fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) continue;
        if (now - st.st_mtime > (time_t)VMRECV_PART_DAYS * 86400) { unlinkat(dfd, e->d_name, 0); continue; }
        if (k < 64) { snprintf(p[k].n, sizeof p[k].n, "%s", e->d_name); p[k].t = st.st_mtime; k++; }
    }
    closedir(d);
    while (k >= VMRECV_PARTS_MAX) {                         /* room for this one */
        int o = 0;
        for (int i = 1; i < k; i++) if (p[i].t < p[o].t) o = i;
        unlinkat(dfd, p[o].n, 0);
        p[o] = p[--k];
    }
}

int vmrecv_part_open(int dfd, const uint8_t key[32], uint64_t size, char part[VMRECV_PART_NAME], uint64_t *have, int *status) {
    *have = 0;
    *status = AGSA_BUSY;
    int n = snprintf(part, VMRECV_PART_NAME, ".lordos-part-");
    for (int i = 0; i < 32; i++) n += snprintf(part + n, (size_t)(VMRECV_PART_NAME - n), "%02x", key[i]);
    vmrecv_parts_trim(dfd, part);
    int fresh = faccessat(dfd, part, F_OK, AT_SYMLINK_NOFOLLOW) != 0;
    int pf = openat(dfd, part, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK, 0600);
    struct stat ps;
    if (pf < 0 || fstat(pf, &ps) || !S_ISREG(ps.st_mode) || ps.st_nlink != 1 || ps.st_uid != getuid()) { if (pf >= 0) close(pf); return -1; }
    fcntl(pf, F_SETFL, 0);
    /* longer than this file: it can't be its start */
    if ((uint64_t)ps.st_size > size && ftruncate(pf, 0)) { close(pf); return -1; }
    uint64_t h = (uint64_t)ps.st_size <= size ? (uint64_t)ps.st_size : 0;
    struct statvfs vs;
    if (fstatvfs(dfd, &vs) || (uint64_t)vs.f_bavail * vs.f_frsize < size - h + VMRECV_RESERVE) {
        close(pf);
        if (fresh) unlinkat(dfd, part, 0);
        *status = AGSA_SPACE;
        return -1;
    }
    *have = h;
    *status = AGSA_OK;
    return pf;
}

int vmrecv_finish(int dfd, int fd, const char *part, const char *name, const char *machine_name, const char *machine_id,
                  uint64_t size, char *final, size_t fn, int *marked) {
    struct stat st;
    *marked = 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != size || fchmod(fd, 0600)) { unlinkat(dfd, part, 0); return -1; }
    char origin[96];
    snprintf(origin, sizeof origin, "lordos-machine:%s", machine_id ? machine_id : "");
    *marked = !fsetxattr(fd, "user.lordos.quarantine", "1", 1, 0);
    if (*marked) {
        if (machine_name) fsetxattr(fd, "user.lordos.from", machine_name, strlen(machine_name), 0);
        if (machine_id) fsetxattr(fd, "user.lordos.machine", machine_id, strlen(machine_id), 0);
        fsetxattr(fd, "user.xdg.origin.url", origin, strlen(origin), 0);
    }
    char cand[AG_NAME_MAX + 16];
    const char *dot = strrchr(name, '.');
    size_t stem = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    for (int n = 1; n < 1000; n++) {
        if (n == 1) snprintf(cand, sizeof cand, "%s", name);
        else snprintf(cand, sizeof cand, "%.*s (%d)%s", (int)stem, name, n, name + stem);
        if (!syscall(SYS_renameat2, dfd, part, dfd, cand, RENAME_NOREPLACE)) { snprintf(final, fn, "%s", cand); return 0; }
        if (errno != EEXIST) break;
    }
    unlinkat(dfd, part, 0);
    return -1;
}
