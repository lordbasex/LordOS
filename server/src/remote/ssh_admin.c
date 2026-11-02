/* ssh_admin.c — root actions of remote login (SSH), in the privileged helper (admin.c).
 *
 *   ssh-set on|off           on: enables and starts ssh.service for good (systemctl enable --now);
 *                            off: disables and stops it (and ssh.socket). The package stays.
 *   ssh-allow all|admins     who can log in: all users, or only administrators (an AllowGroups
 *                            drop-in with the sudo, admin and wheel groups)
 *
 * Installing openssh-server is not done here: Settings → Sharing asks for it first with the
 * package engine's own action (pkg-install openssh-server openssh-sftp-server, server/src/pkg),
 * so the only way a Debian package reaches the system stays the Store's. These actions run
 * fixed programs at fixed paths and write only fixed text: no user text reaches sshd's
 * configuration or a command line. The safe defaults (PermitRootLogin no) are a drop-in shipped
 * by the lordos package (tools/iso/rootfs/etc/ssh/sshd_config.d/10-lordos.conf). */
#include "ssh_admin.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SSHD          "/usr/sbin/sshd"
#define SSH_KEYGEN    "/usr/bin/ssh-keygen"
#define SYSTEMCTL     "/usr/bin/systemctl"
#define SSH_UNIT      "/usr/lib/systemd/system/ssh.service"
#define SSH_SOCKET    "/usr/lib/systemd/system/ssh.socket"
#define DROPIN_DIR    "/etc/ssh/sshd_config.d"
#define ALLOW_DROPIN  DROPIN_DIR "/11-lordos-allow.conf"
#define ED25519_KEY   "/etc/ssh/ssh_host_ed25519_key"

static const char allow_admins_text[] =
    "# LordOS · remote login (Settings → Sharing): only administrators. See docs/REMOTE.md.\n"
    "# Written by Settings; choose \"All users\" there to remove it.\n"
    "AllowGroups sudo admin wheel root\n";   /* root: only matters with root over SSH on (ssh_root.c) */

int ssh_allow_text(const char *value, const char **text) {
    if (!strcmp(value, "all")) { *text = NULL; return 0; }
    if (!strcmp(value, "admins")) { *text = allow_admins_text; return 0; }
    return -1;
}

static int installed(void) {
    struct stat st;
    return !access(SSHD, X_OK) && !stat(SSH_UNIT, &st) && S_ISREG(st.st_mode);
}

/* arguments: one word with nothing after it (a trailing newline is allowed) */
static int word(const char *arg, size_t len, char *out, size_t cap) {
    while (len && (arg[len - 1] == '\n' || arg[len - 1] == ' ')) len--;
    if (!len || len >= cap || memchr(arg, 0, len)) return -1;
    memcpy(out, arg, len);
    out[len] = 0;
    return 0;
}

/* sshd -t: the configuration (with our drop-ins) is valid */
static int config_ok(void) {
    const char *const t[] = { SSHD, "-t", NULL };
    return admin_run(t) == 0;
}

static int write_dropin(const char *path, const char *text) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.lordos", path);
    if (mkdir(DROPIN_DIR, 0755) && errno != EEXIST) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    size_t n = strlen(text);
    int ok = write(fd, text, n) == (ssize_t)n;
    if (fsync(fd)) ok = 0;
    if (close(fd)) ok = 0;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

void ssh_act_set(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char v[8];
    if (word(arg, len, v, sizeof v) || (strcmp(v, "on") && strcmp(v, "off"))) { admin_err("admin.invalidArgument", NULL); return; }
    if (!strcmp(v, "off")) {
        if (!installed()) { admin_ok("ssh.off", NULL); return; }
        struct stat st;
        int sock = !stat(SSH_SOCKET, &st);
        const char *const a[] = { SYSTEMCTL, "disable", "--now", "ssh.service", sock ? "ssh.socket" : NULL, NULL };
        int rc = admin_run(a);
        if (rc) { admin_err("sys.programFailed", "program", "systemctl", "code", lerr_int(rc), NULL); return; }
        admin_ok("ssh.off", NULL);
        return;
    }
    if (!installed()) { admin_err("ssh.notInstalled", NULL); return; }
    /* host keys: the package makes them, but an image may ship without (the Raspberry Pi one) */
    if (access(ED25519_KEY, F_OK)) {
        const char *const k[] = { SSH_KEYGEN, "-A", NULL };
        int rc = admin_run(k);
        if (rc) { admin_err("sys.programFailed", "program", "ssh-keygen", "code", lerr_int(rc), NULL); return; }
    }
    if (!config_ok()) { admin_err("ssh.badConfig", NULL); return; }
    const char *const a[] = { SYSTEMCTL, "enable", "--now", "ssh.service", NULL };
    int rc = admin_run(a);
    if (rc) { admin_err("sys.programFailed", "program", "systemctl", "code", lerr_int(rc), NULL); return; }
    const char *const q[] = { SYSTEMCTL, "is-active", "--quiet", "ssh.service", NULL };
    for (int i = 0; i < 30; i++) {
        if (admin_run(q) == 0) { admin_ok("ssh.on", NULL); return; }
        usleep(100000);
    }
    admin_err("ssh.startFailed", NULL);
}

void ssh_act_allow(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char v[8];
    const char *text;
    if (word(arg, len, v, sizeof v) || ssh_allow_text(v, &text)) { admin_err("admin.invalidArgument", NULL); return; }
    if (text) {
        if (write_dropin(ALLOW_DROPIN, text)) { admin_err("sys.writeFailed", "path", ALLOW_DROPIN, NULL); return; }
        if (installed() && !config_ok()) { unlink(ALLOW_DROPIN); admin_err("ssh.badConfig", NULL); return; }
    } else if (unlink(ALLOW_DROPIN) && errno != ENOENT) { admin_err("sys.writeFailed", "path", ALLOW_DROPIN, NULL); return; }
    if (installed()) {
        /* only if it is running: a stopped server stays stopped */
        const char *const a[] = { SYSTEMCTL, "try-reload-or-restart", "ssh.service", NULL };
        int rc = admin_run(a);
        if (rc) { admin_err("sys.programFailed", "program", "systemctl", "code", lerr_int(rc), NULL); return; }
    }
    admin_ok("ssh.saved", NULL);
}
