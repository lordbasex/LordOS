/* sgrant_admin.c — "share-grant <id> <perm> <allow|deny|ask> [exe]" and "share-forget <id>", as root, for the session
 * user only (docs/SCREEN-SHARING.md). The file is replaced whole: a new file in the same folder, then rename. */
#include "sgrant_admin.h"
#include "sharegrant.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static SgStore st;                      /* big: not on the stack */

static int save(const AdminReq *r) {
    static char text[SG_FILE_MAX];
    int n = sg_write(&st, text, sizeof text);
    if (n < 0) return -1;
    if (mkdir(SG_DIR, 0755) && errno != EEXIST) return -1;
    struct stat ds;
    if (lstat(SG_DIR, &ds) || !S_ISDIR(ds.st_mode) || ds.st_uid != 0 || (ds.st_mode & 022)) return -1;
    char path[96], tmp[112];
    sg_path(r->uid, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0640);
    if (fd < 0) return -1;
    int ok = fchown(fd, 0, r->gid) == 0 && fchmod(fd, 0640) == 0 && write(fd, text, (size_t)n) == n && fsync(fd) == 0;
    close(fd);
    if (ok && !rename(tmp, path)) return 0;
    unlink(tmp);
    return -1;
}

void sgrant_act(const AdminReq *r, const char *arg, size_t len) {
    char a[SG_ID_MAX + SG_EXE_MAX + 64], id[SG_ID_MAX] = "", perm[16] = "", state[8] = "", exe[SG_EXE_MAX] = "", extra[2] = "";
    if (len >= sizeof a) { admin_err("share.badGrant", NULL); return; }
    memcpy(a, arg, len);
    a[len] = 0;
    int k = sscanf(a, "%127s %15s %7s %255s %1s", id, perm, state, exe, extra);
    int p = sg_perm_of(perm), s = sg_state_of(state);
    if (k < 3 || k > 4 || !sg_id_valid(id) || p < 0 || s < 0 || (k == 4 && !sg_exe_valid(exe)) ||
        (s == SG_ALLOW && !strncmp(id, "desktop:", 8) && k != 4)) {          /* a Linux app's grant names its program */
        admin_err("share.badGrant", NULL);
        return;
    }
    char path[96];
    sg_path(r->uid, path, sizeof path);
    sg_load(path, &st);                 /* no file, or a damaged one: starts empty */
    if (sg_set(&st, id, exe, p, s) || save(r)) { admin_err("share.grantFailed", NULL); return; }
    admin_ok("share.granted", NULL);
}

void sgrant_act_forget(const AdminReq *r, const char *arg, size_t len) {
    char id[SG_ID_MAX];
    if (len >= sizeof id) { admin_err("share.badGrant", NULL); return; }
    memcpy(id, arg, len);
    id[len] = 0;
    if (!sg_id_valid(id)) { admin_err("share.badGrant", NULL); return; }
    char path[96];
    sg_path(r->uid, path, sizeof path);
    sg_load(path, &st);
    sg_forget(&st, id);
    if (save(r)) { admin_err("share.grantFailed", NULL); return; }
    admin_ok("share.granted", NULL);
}
