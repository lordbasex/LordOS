/* ssh_root.c — remote login as root over SSH: an advanced option, off by default (0.8.5; Settings →
 * Sharing → Remote login → Advanced; docs/REMOTE.md, "Root over SSH (advanced)"). Administrators only,
 * and every change asks again for the administrator's own password (checked here against
 * /etc/shadow, users_check_password):
 *
 *   ssh-root <off|key|password>\n<admin password>
 *        off: no file (the shipped 10-lordos.conf keeps PermitRootLogin no); key: PermitRootLogin
 *        prohibit-password (public keys only); password: PermitRootLogin yes. Written as
 *        /etc/ssh/sshd_config.d/05-lordos-root.conf: sshd keeps the first value it reads, and 05 comes
 *        before LordOS's 10. Checked with sshd -t, then the running server reloads.
 *   ssh-root-key add\n<admin password>\n<public key line>
 *   ssh-root-key remove\n<admin password>\n<SHA256:fingerprint>
 *        one OpenSSH public key (sshkey_parse: a known type, its base64 blob starting with that type,
 *        a printable comment, no options) appended to /root/.ssh/authorized_keys or removed from it;
 *        lines it can't read are kept as they are
 *   ssh-root-password\n<admin password>\n<new root password>
 *        chpasswd on standard input, never in argv
 *   ssh-root-status
 *        read-only: "mode <off|key|password>", "password set|unset", "key <type> <fingerprint> <comment>"
 *
 * Fixed programs at fixed paths, fixed configuration text, nothing from the user reaches a shell or
 * sshd's configuration: the only user text written anywhere is a validated public key line. */
#include "ssh_root.h"
#include "../users/users_admin.h"
#include "../pkg/crypto.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SSHD        "/usr/sbin/sshd"
#define SYSTEMCTL   "/usr/bin/systemctl"
#define SSH_UNIT    "/usr/lib/systemd/system/ssh.service"
#define DROPIN_DIR  "/etc/ssh/sshd_config.d"
#define ROOT_DROPIN DROPIN_DIR "/05-lordos-root.conf"
#define ROOT_SSH    "/root/.ssh"
#define ROOT_KEYS   ROOT_SSH "/authorized_keys"
#define KEYS_MAX    (256 * 1024)

static const char text_key[] =
    "# LordOS · root over SSH (Settings → Sharing → Remote login → Advanced): public keys only.\n"
    "# Written by Settings; turn it off there to remove it. See docs/REMOTE.md.\n"
    "PermitRootLogin prohibit-password\n";
static const char text_password[] =
    "# LordOS · root over SSH (Settings → Sharing → Remote login → Advanced): public keys or password.\n"
    "# Written by Settings; turn it off there to remove it. See docs/REMOTE.md.\n"
    "PermitRootLogin yes\n";

int ssh_root_text(const char *mode, const char **text) {
    if (!strcmp(mode, "off")) { *text = NULL; return 0; }
    if (!strcmp(mode, "key")) { *text = text_key; return 0; }
    if (!strcmp(mode, "password")) { *text = text_password; return 0; }
    return -1;
}

/* ---- public keys ---- */

static const char *const TYPES[] = {
    "ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521", "ssh-rsa",
    "sk-ssh-ed25519@openssh.com", "sk-ecdsa-sha2-nistp256@openssh.com", NULL };

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* strict base64 (padding only at the end, length a multiple of 4) */
static int b64decode(const char *s, size_t n, uint8_t *out, size_t cap, size_t *olen) {
    if (!n || n % 4) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int v[4];
        for (int j = 0; j < 4; j++) {
            char c = s[i + j];
            if (c == '=') { if (i + 4 != n || j < 2 || (j == 2 && s[i + 3] != '=')) return -1; v[j] = 0; }
            else if ((v[j] = b64val((unsigned char)c)) < 0) return -1;
        }
        uint32_t w = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        int bytes = s[i + 2] == '=' ? 1 : s[i + 3] == '=' ? 2 : 3;
        if (o + (size_t)bytes > cap) return -1;
        out[o++] = (uint8_t)(w >> 16);
        if (bytes > 1) out[o++] = (uint8_t)(w >> 8);
        if (bytes > 2) out[o++] = (uint8_t)w;
    }
    *olen = o;
    return 0;
}

int sshkey_parse(const char *line, SshKey *k) {
    memset(k, 0, sizeof *k);
    if (!line) return -1;
    while (*line == ' ' || *line == '\t') line++;
    size_t n = strcspn(line, "\r\n");
    if (line[n] && line[n + strspn(line + n, "\r\n")]) return -1;       /* one line only */
    const char *sp = memchr(line, ' ', n);
    if (!sp) return -1;
    size_t tl = (size_t)(sp - line);
    if (tl >= sizeof k->type) return -1;
    memcpy(k->type, line, tl);
    int known = 0;
    for (int i = 0; TYPES[i]; i++) if (!strcmp(k->type, TYPES[i])) known = 1;
    if (!known) return -1;                                               /* also refuses options */
    const char *b = sp + 1;
    while (b < line + n && *b == ' ') b++;
    size_t bl = strcspn(b, " \r\n");
    if (!bl || bl >= sizeof k->b64) return -1;
    memcpy(k->b64, b, bl);
    if (b64decode(k->b64, bl, k->blob, sizeof k->blob, &k->bloblen)) return -1;
    /* the blob starts with the same type: uint32 length + name */
    if (k->bloblen < 4 + tl) return -1;
    uint32_t nl = (uint32_t)k->blob[0] << 24 | (uint32_t)k->blob[1] << 16 | (uint32_t)k->blob[2] << 8 | k->blob[3];
    if (nl != tl || memcmp(k->blob + 4, k->type, tl)) return -1;
    if (k->bloblen < 4 + tl + 16) return -1;                             /* a real key follows */
    const char *c = b + bl;
    while (c < line + n && *c == ' ') c++;
    size_t cl = (size_t)(line + n - c);
    while (cl && c[cl - 1] == ' ') cl--;
    if (cl >= sizeof k->comment) return -1;
    for (size_t i = 0; i < cl; i++) if ((unsigned char)c[i] < 0x20 || (unsigned char)c[i] > 0x7e) return -1;
    memcpy(k->comment, c, cl);
    return 0;
}

void sshkey_fingerprint(const SshKey *k, char out[60]) {
    uint8_t d[32];
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, k->blob, k->bloblen);
    sha256_final(&s, d);
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = out + sprintf(out, "SHA256:");
    for (int i = 0; i < 32; i += 3) {
        uint32_t w = (uint32_t)d[i] << 16 | (uint32_t)(i + 1 < 32 ? d[i + 1] : 0) << 8 | (uint32_t)(i + 2 < 32 ? d[i + 2] : 0);
        *o++ = A[(w >> 18) & 63];
        *o++ = A[(w >> 12) & 63];
        if (i + 1 < 32) *o++ = A[(w >> 6) & 63];
        if (i + 2 < 32) *o++ = A[w & 63];
    }
    *o = 0;
}

/* ---- helpers ---- */

static int installed(void) {
    struct stat st;
    return !access(SSHD, X_OK) && !stat(SSH_UNIT, &st) && S_ISREG(st.st_mode);
}
static int config_ok(void) { const char *const t[] = { SSHD, "-t", NULL }; return admin_run(t) == 0; }
static void reload(void) {
    if (!installed()) return;
    const char *const a[] = { SYSTEMCTL, "try-reload-or-restart", "ssh.service", NULL };
    admin_run(a);
}

/* Splits "a\nb\nc" in place into up to max fields (the last one keeps the rest of its line). */
static int fields(char *s, char **f, int max) {
    int n = 0;
    while (n < max) {
        f[n++] = s;
        char *nl = strchr(s, '\n');
        if (!nl) break;
        *nl = 0;
        s = nl + 1;
    }
    return n;
}

static int write_atomic(const char *path, const char *data, size_t n, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.lordos", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    int ok = (!n || write(fd, data, n) == (ssize_t)n) && !fchmod(fd, mode) && !fchown(fd, 0, 0);
    if (fsync(fd)) ok = 0;
    if (close(fd)) ok = 0;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

/* root's authorized_keys, read whole (a regular file of root's, not a link); "" when there is none */
static char *read_keys(size_t *len) {
    *len = 0;
    int fd = open(ROOT_KEYS, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? calloc(1, 1) : NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || st.st_size > KEYS_MAX) { close(fd); return NULL; }
    char *b = malloc((size_t)st.st_size + 1);
    ssize_t r = b ? read(fd, b, (size_t)st.st_size) : -1;
    close(fd);
    if (r < 0) { free(b); return NULL; }
    b[r] = 0;
    *len = (size_t)r;
    return b;
}

static int root_ssh_dir(void) {
    struct stat st;
    if (lstat(ROOT_SSH, &st)) {
        if (errno != ENOENT || mkdir(ROOT_SSH, 0700)) return -1;
        return chown(ROOT_SSH, 0, 0);
    }
    return S_ISDIR(st.st_mode) && st.st_uid == 0 ? chmod(ROOT_SSH, 0700) : -1;
}

/* the administrator types their own password again for every change */
static int reauth(const AdminReq *r, const char *pass) {
    if (users_check_password(r->user, pass)) return 0;
    sleep(1);
    admin_err("users.wrongPassword", NULL);
    return -1;
}

/* ---- actions ---- */

void ssh_act_root(const AdminReq *r, const char *arg, size_t len) {
    char buf[600], *f[2];
    if (len >= sizeof buf) { admin_err("admin.invalidArgument", NULL); return; }
    memcpy(buf, arg, len);
    buf[len] = 0;
    const char *text;
    if (fields(buf, f, 2) != 2 || ssh_root_text(f[0], &text)) { admin_err("admin.invalidArgument", NULL); explicit_bzero(buf, sizeof buf); return; }
    int bad = reauth(r, f[1]);
    explicit_bzero(buf, sizeof buf);
    if (bad) return;
    if (text) {
        if (mkdir(DROPIN_DIR, 0755) && errno != EEXIST) { admin_err("sys.writeFailed", "path", DROPIN_DIR, NULL); return; }
        if (write_atomic(ROOT_DROPIN, text, strlen(text), 0644)) { admin_err("sys.writeFailed", "path", ROOT_DROPIN, NULL); return; }
        if (installed() && !config_ok()) { unlink(ROOT_DROPIN); admin_err("ssh.badConfig", NULL); return; }
    } else if (unlink(ROOT_DROPIN) && errno != ENOENT) { admin_err("sys.writeFailed", "path", ROOT_DROPIN, NULL); return; }
    reload();
    admin_ok("ssh.saved", NULL);
}

void ssh_act_root_key(const AdminReq *r, const char *arg, size_t len) {
    char *buf = len < 16384 ? malloc(len + 1) : NULL, *f[3];
    if (!buf) { admin_err("admin.invalidArgument", NULL); return; }
    memcpy(buf, arg, len);
    buf[len] = 0;
    if (fields(buf, f, 3) != 3 || (strcmp(f[0], "add") && strcmp(f[0], "remove"))) { admin_err("admin.invalidArgument", NULL); explicit_bzero(buf, len); free(buf); return; }
    int add = !strcmp(f[0], "add");
    SshKey k;
    char want[60] = "";
    int parsed = add ? sshkey_parse(f[2], &k) : 0;
    if (!add) snprintf(want, sizeof want, "%.59s", f[2]);
    int bad = reauth(r, f[1]);
    explicit_bzero(f[1], strlen(f[1]));
    free(buf);
    if (bad) return;
    if (add && parsed) { admin_err("ssh.badKey", NULL); return; }
    if (!add && (strncmp(want, "SHA256:", 7) || strspn(want + 7, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") != strlen(want + 7))) {
        admin_err("admin.invalidArgument", NULL);
        return;
    }
    if (root_ssh_dir()) { admin_err("sys.writeFailed", "path", ROOT_SSH, NULL); return; }
    size_t n;
    char *cur = read_keys(&n);
    if (!cur) { admin_err("sys.readFailed", "path", ROOT_KEYS, NULL); return; }
    char *out = malloc(n + 4096);
    if (!out) { free(cur); admin_err("sys.writeFailed", "path", ROOT_KEYS, NULL); return; }
    size_t o = 0;
    int found = 0;
    char fp[60];
    if (add) sshkey_fingerprint(&k, fp);
    for (char *line = cur; *line;) {
        char *nl = strchr(line, '\n');
        size_t ll = nl ? (size_t)(nl - line) : strlen(line);
        char save = line[ll];
        line[ll] = 0;
        SshKey e;
        char efp[60] = "";
        if (!sshkey_parse(line, &e)) sshkey_fingerprint(&e, efp);
        int match = efp[0] && !strcmp(efp, add ? fp : want);
        if (match) found = 1;
        if (!(match && !add) && ll) { memcpy(out + o, line, ll); o += ll; out[o++] = '\n'; }
        line[ll] = save;
        line += ll + (nl ? 1 : 0);
    }
    free(cur);
    if (add && found) { free(out); admin_err("ssh.duplicateKey", NULL); return; }
    if (!add && !found) { free(out); admin_err("ssh.keyNotFound", NULL); return; }
    if (add) o += (size_t)snprintf(out + o, 4096, "%s %s%s%s\n", k.type, k.b64, k.comment[0] ? " " : "", k.comment);
    int rc = write_atomic(ROOT_KEYS, out, o, 0600);
    free(out);
    if (rc) { admin_err("sys.writeFailed", "path", ROOT_KEYS, NULL); return; }
    admin_ok("ssh.saved", NULL);
}

void ssh_act_root_password(const AdminReq *r, const char *arg, size_t len) {
    char buf[700], *f[2];
    if (len >= sizeof buf) { admin_err("admin.invalidArgument", NULL); return; }
    memcpy(buf, arg, len);
    buf[len] = 0;
    if (fields(buf, f, 2) != 2) { explicit_bzero(buf, sizeof buf); admin_err("admin.invalidArgument", NULL); return; }
    size_t pl = strlen(f[1]);
    int weak = pl < 8 || pl > 256 || strchr(f[1], ':');
    for (size_t i = 0; i < pl; i++) if ((unsigned char)f[1][i] < 0x20) weak = 1;
    if (reauth(r, f[0])) { explicit_bzero(buf, sizeof buf); return; }
    if (weak) { explicit_bzero(buf, sizeof buf); admin_err("ssh.weakRootPassword", NULL); return; }
    char line[300];
    snprintf(line, sizeof line, "root:%s\n", f[1]);
    explicit_bzero(buf, sizeof buf);
    const char *const a[] = { "/usr/sbin/chpasswd", NULL };
    int rc = admin_run_input(a, line);
    explicit_bzero(line, sizeof line);
    if (rc) { admin_err("sys.programFailed", "program", "chpasswd", "code", lerr_int(rc), NULL); return; }
    admin_ok("ssh.saved", NULL);
}

void ssh_act_root_status(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg; (void)len;
    char m[64] = "off";
    FILE *f = fopen(ROOT_DROPIN, "re");
    if (f) {
        char l[160];
        while (fgets(l, sizeof l, f))
            if (!strncmp(l, "PermitRootLogin ", 16)) snprintf(m, sizeof m, "%s", strstr(l, "prohibit-password") ? "key" : strstr(l, " yes") ? "password" : "off");
        fclose(f);
    }
    admin_say("LOG", "mode %s", m);                  /* i18n-ok: protocol line parsed by sdk/sshroot.js */
    struct spwd *sp = getspnam("root");
    int set = sp && sp->sp_pwdp && sp->sp_pwdp[0] && sp->sp_pwdp[0] != '!' && sp->sp_pwdp[0] != '*';
    admin_say("LOG", "password %s", set ? "set" : "unset");   /* i18n-ok: protocol line */
    size_t n;
    char *cur = read_keys(&n);
    for (char *line = cur; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        SshKey k;
        if (!sshkey_parse(line, &k)) {
            char fp[60];
            sshkey_fingerprint(&k, fp);
            admin_say("LOG", "key %s %s %s", k.type, fp, k.comment[0] ? k.comment : "-");   /* i18n-ok: protocol line */
        }
        line = nl ? nl + 1 : line + strlen(line);
    }
    free(cur);
    admin_ok("ssh.status", NULL);
}
