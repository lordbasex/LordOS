/* users_admin.c — the machine's users, as root in the administration helper (admin.c).
 * Used by Settings → Users (docs/SESSIONS.md). Each argument is a set of lines:
 *
 *   user-add       name \n full name \n password \n admin (0|1)
 *   user-del       name \n keep the home folder (0|1)
 *   user-passwd    name \n new password \n current password
 *                  (your own: anyone, with the current one; someone else's: only an administrator)
 *   user-admin     name \n 0|1                         (sudo group)
 *   user-avatar    name \n color (#rrggbb or empty) \n image ("" unchanged, "-" remove, or a path
 *                  in the requester's home folder; PNG or JPEG up to 4 MB)
 *                  (your own: anyone; someone else's: only an administrator)
 *   user-autologin name or empty                       (/etc/lordos/session.conf)
 *   admin-rescue-status                                (anyone, read-only: LOG users.rescueState state=<word>)
 *   admin-rescue   root password                       (anyone, only while nobody is an administrator)
 *
 * Root-side validation: names [a-z_][a-z0-9_-]*, only people's accounts (uid 1000 to
 * 59999), never your own account when deleting nor the last administrator. Passwords go through
 * chpasswd's standard input, never in argv. Avatars live in /var/lib/lordos/users
 * (read by the login screen, which can't get into home folders). */
#include "users_admin.h"
#include "admin_rescue.h"
#include "homedirs.h"

#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#define USERS_DIR "/var/lib/lordos/users"
#define SESSION_CONF "/etc/lordos/session.conf"
#define ADMIN_GROUP "sudo"
#define SUDO_BIN "/usr/bin/sudo"
#define RUN_DIR "/run/lordos"
#define RESCUE_LOCK RUN_DIR "/admin-rescue.lock"
#define RESCUE_FAIL_DELAY 2             /* seconds, with the lock held: one try at a time, machine-wide */
/* Printers (M9, docs/PRINTING.md): CUPS lets the lpadmin group administer printers. LordOS
 * administrators get it too; if CUPS isn't installed the group doesn't exist and it's
 * skipped without error. */
#define PRINT_GROUP "lpadmin"

static int in_group(const char *user, const char *group);
static void print_group(const char *name, int on) {
    if (!getgrnam(PRINT_GROUP)) return;
    const char *const add[] = { "/usr/sbin/usermod", "--append", "--groups", PRINT_GROUP, name, NULL };
    const char *const del[] = { "/usr/bin/gpasswd", "--delete", name, PRINT_GROUP, NULL };
    if (on != in_group(name, PRINT_GROUP)) admin_run(on ? add : del);
}

/* ---- validation ---- */

static int valid_name(const char *u) {
    size_t n = strlen(u);
    if (!n || n > 32 || !((u[0] >= 'a' && u[0] <= 'z') || u[0] == '_')) return 0;
    for (const char *p = u; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    return 1;
}

/* full name: up to 64 bytes, no control chars nor the /etc/passwd separators (: ,) */
static int valid_full(const char *s) {
    size_t n = strlen(s);
    if (n > 64) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p < 32 || *p == 127 || *p == ':' || *p == ',' || *p == '=') return 0;
    return 1;
}

static int valid_pass(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 256) return 0;
    for (const char *p = s; *p; p++) if (*p == '\n' || *p == '\r') return 0;
    return 1;
}

/* A person's account (not a system one) */
static struct passwd *person(const char *name) {
    if (!valid_name(name)) return NULL;
    struct passwd *pw = getpwnam(name);
    return pw && pw->pw_uid >= 1000 && pw->pw_uid < 60000 ? pw : NULL;
}

/* Splits the argument into n lines (the last one may be missing: it becomes ""). 0 if there are too many. */
static int split(char *arg, char **f, int n) {
    for (int i = 0; i < n; i++) f[i] = "";
    char *p = arg;
    for (int i = 0; i < n && p; i++) {
        f[i] = p;
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        p = nl ? nl + 1 : NULL;
    }
    return p == NULL || !*p;
}

static int in_group(const char *user, const char *group) {
    struct passwd *pw = getpwnam(user);
    struct group *g = getgrnam(group);
    if (!pw || !g) return 0;
    gid_t groups[256];
    int n = 256;
    if (getgrouplist(user, pw->pw_gid, groups, &n) < 0) return 0;
    for (int i = 0; i < n; i++) if (groups[i] == g->gr_gid) return 1;
    return 0;
}

static int admins(void) {
    int n = 0;
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent()))
        if (pw->pw_uid >= 1000 && pw->pw_uid < 60000 && in_group(pw->pw_name, ADMIN_GROUP)) n++;
    endpwent();
    return n;
}

static int set_password(const char *user, const char *pass) {
    size_t n = strlen(user) + strlen(pass) + 3;
    char *line = malloc(n);
    if (!line) return -1;
    snprintf(line, n, "%s:%s\n", user, pass);
    const char *const a[] = { "/usr/sbin/chpasswd", NULL };
    int rc = admin_run_input(a, line);
    explicit_bzero(line, n);
    free(line);
    return rc;
}

/* The account has a password that can be typed (not locked, not empty). */
static int has_password(const struct spwd *sp) {
    return sp && sp->sp_pwdp && sp->sp_pwdp[0] && sp->sp_pwdp[0] != '!' && sp->sp_pwdp[0] != '*';
}

int users_check_password(const char *user, const char *pass) {
    struct spwd *sp = getspnam(user);
    if (!has_password(sp)) return 0;
    struct crypt_data cd;
    memset(&cd, 0, sizeof cd);
    const char *h = crypt_r(pass, sp->sp_pwdp, &cd);
    int ok = h && h[0] != '*' && strlen(h) == strlen(sp->sp_pwdp);
    unsigned char d = 0;
    for (size_t i = 0; ok && h[i]; i++) d |= (unsigned char)(h[i] ^ sp->sp_pwdp[i]);
    explicit_bzero(&cd, sizeof cd);
    return ok && d == 0;
}

static int write_file(const char *path, const void *data, size_t n, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.lordos", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    int ok = write(fd, data, n) == (ssize_t)n;
    if (fchmod(fd, mode)) ok = 0;
    if (close(fd)) ok = 0;
    if (ok && !rename(tmp, path)) return 0;
    unlink(tmp);
    return -1;
}

static void autologin_get(char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(SESSION_CONF, "r");
    char line[128];
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "AUTOLOGIN=%32[a-z0-9_-]", out) == 1) break;
    if (f) fclose(f);
    (void)n;
}

static int autologin_set(const char *user) {
    mkdir("/etc/lordos", 0755);
    char text[256];
    snprintf(text, sizeof text, "# Written by LordOS (Settings → Users). See docs/SESSIONS.md\n"
             "# AUTOLOGIN=user logs in automatically at boot; empty: login screen\nAUTOLOGIN=%s\n", user);
    return write_file(SESSION_CONF, text, strlen(text), 0644);
}

/* ---- actions ---- */

/* Common folders in the system language (shown by Files) and their language. */
static void prepare_home(const struct passwd *pw) {
    char lang[64] = "en_US.UTF-8";
    FILE *f = fopen("/etc/default/locale", "r");
    char line[128];
    while (f && fgets(line, sizeof line, f)) if (sscanf(line, "LANG=%63[A-Za-z_.@0-9-]", lang) == 1) break;
    if (f) fclose(f);
    char path[PATH_MAX];
    /* Escritorio, Documentos… and ~/.config/user-dirs.dirs in the system language (homedirs.c) */
    if (homedirs_create(pw->pw_dir, pw->pw_uid, pw->pw_gid, lang)) admin_log("users.homedirsWarning", NULL);
    snprintf(path, sizeof path, "%s/.config", pw->pw_dir);
    if (!mkdir(path, 0755) && chown(path, pw->pw_uid, pw->pw_gid)) { /* carry on */ }
    snprintf(path, sizeof path, "%s/.config/lordos", pw->pw_dir);
    if (!mkdir(path, 0755) && chown(path, pw->pw_uid, pw->pw_gid)) { /* carry on */ }
    char text[96];
    snprintf(path, sizeof path, "%s/.config/locale.conf", pw->pw_dir);
    snprintf(text, sizeof text, "LANG=%s\n", lang);
    if (!write_file(path, text, strlen(text), 0644) && chown(path, pw->pw_uid, pw->pw_gid)) { /* carry on */ }
    snprintf(path, sizeof path, "%s/.config/lordos/settings.json", pw->pw_dir);
    /* UI language: the system's if there's a catalog (es, en, pt); otherwise English */
    const char *ui = homedirs_ui_lang(lang);
    snprintf(text, sizeof text, "{\n  \"language\": \"%s\"\n}\n", ui);
    if (!write_file(path, text, strlen(text), 0644) && chown(path, pw->pw_uid, pw->pw_gid)) { /* carry on */ }
    snprintf(path, sizeof path, "%s/.config/lordos/.dirs", pw->pw_dir);
    if (!write_file(path, "", 0, 0644) && chown(path, pw->pw_uid, pw->pw_gid)) { /* carry on */ }
}

void users_act_add(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    char *buf = strndup(arg, len), *f[4];
    if (!buf || !split(buf, f, 4)) { admin_err("admin.invalidArgument", NULL); free(buf); return; }
    const char *name = f[0], *full = f[1], *pass = f[2];
    int adm = !strcmp(f[3], "1");
    if (!valid_name(name) || !valid_full(full) || !valid_pass(pass)) admin_err("users.badNameOrPassword", NULL);
    else if (getpwnam(name) || getgrnam(name)) admin_err("users.exists", "name", name, NULL);
    else {
        const char *const a[] = { "/usr/sbin/useradd", "--create-home", "--shell", "/bin/bash", "--user-group", "--comment", full, name, NULL };
        struct passwd *pw;
        if (admin_run(a) || !(pw = getpwnam(name))) admin_err("sys.programError", "program", "useradd", NULL);
        else if (set_password(name, pass)) admin_err("users.setPasswordFailed", NULL);
        else {
            if (adm) { const char *const g[] = { "/usr/sbin/usermod", "--append", "--groups", ADMIN_GROUP, name, NULL }; admin_run(g); print_group(name, 1); }
            if (getgrnam("pipewire")) { const char *const g[] = { "/usr/sbin/usermod", "--append", "--groups", "pipewire", name, NULL }; admin_run(g); }   /* real-time sound (docs/SOUND.md) */
            if (getgrnam("plugdev")) { const char *const g[] = { "/usr/sbin/usermod", "--append", "--groups", "plugdev", name, NULL }; admin_run(g); }   /* FIDO2 security keys (docs/PASSWORDS.md) */
            /* sound card, camera and GPU render node: lord-sessiond sessions have no logind seat, so
             * there are no uaccess ACLs; without these groups a new user only gets a dummy output
             * (same groups as the installer's first user, minus input, which lord-sessiond lends) */
            for (const char *const *gr = (const char *const[]){ "audio", "video", "render", NULL }; *gr; gr++)
                if (getgrnam(*gr)) { const char *const g[] = { "/usr/sbin/usermod", "--append", "--groups", *gr, name, NULL }; admin_run(g); }
            prepare_home(pw);
            admin_ok("users.created", "name", name, NULL);
        }
    }
    explicit_bzero(buf, len);
    free(buf);
}

void users_act_del(const AdminReq *r, const char *arg, size_t len) {
    char *buf = strndup(arg, len), *f[2];
    if (!buf || !split(buf, f, 2)) { admin_err("admin.invalidArgument", NULL); free(buf); return; }
    const char *name = f[0];
    int keep = !strcmp(f[1], "1");
    struct passwd *pw = person(name);
    if (!pw) admin_err("users.invalid", NULL);
    else if (!strcmp(name, r->user)) admin_err("users.deleteSelf", NULL);
    else if (in_group(name, ADMIN_GROUP) && admins() <= 1) admin_err("users.lastAdmin", NULL);
    else {
        /* their session (and everything of theirs) ends first */
        const char *const k[] = { "/usr/bin/pkill", "--signal", "KILL", "--uid", name, NULL };
        admin_run(k);
        usleep(300000);
        const char *const a_keep[] = { "/usr/sbin/userdel", name, NULL };
        const char *const a_rm[] = { "/usr/sbin/userdel", "--remove", name, NULL };
        if (admin_run(keep ? a_keep : a_rm)) admin_err("sys.programError", "program", "userdel", NULL);
        else {
            char p[128], au[40];
            snprintf(p, sizeof p, USERS_DIR "/%s.json", name);
            unlink(p);
            snprintf(p, sizeof p, USERS_DIR "/%s.png", name);
            unlink(p);
            autologin_get(au, sizeof au);
            if (!strcmp(au, name)) autologin_set("");
            admin_ok("users.deleted", "name", name, NULL);
        }
    }
    free(buf);
}

void users_act_passwd(const AdminReq *r, const char *arg, size_t len) {
    char *buf = strndup(arg, len), *f[3];
    if (!buf || !split(buf, f, 3)) { admin_err("admin.invalidArgument", NULL); free(buf); return; }
    const char *name = f[0], *pass = f[1], *old = f[2];
    int self = !strcmp(name, r->user);
    if (!person(name) || !valid_pass(pass)) admin_err("users.badNameOrPassword", NULL);
    else if (!self && !r->admin) admin_err("users.passwordNotAllowed", NULL);
    else if (self && !users_check_password(name, old)) { sleep(1); admin_err("users.wrongPassword", NULL); }
    else if (set_password(name, pass)) admin_err("users.setPasswordFailed", NULL);
    else admin_ok("users.passwordChanged", NULL);
    explicit_bzero(buf, len);
    free(buf);
}

void users_act_admin(const AdminReq *r, const char *arg, size_t len) {
    char *buf = strndup(arg, len), *f[2];
    if (!buf || !split(buf, f, 2)) { admin_err("admin.invalidArgument", NULL); free(buf); return; }
    const char *name = f[0];
    int on = !strcmp(f[1], "1");
    (void)r;
    if (!person(name) || (strcmp(f[1], "0") && strcmp(f[1], "1"))) admin_err("admin.invalidArgument", NULL);
    else if (!on && in_group(name, ADMIN_GROUP) && admins() <= 1) admin_err("users.lastAdmin", NULL);
    else {
        const char *const add[] = { "/usr/sbin/usermod", "--append", "--groups", ADMIN_GROUP, name, NULL };
        const char *const del[] = { "/usr/bin/gpasswd", "--delete", name, ADMIN_GROUP, NULL };
        if (on == in_group(name, ADMIN_GROUP)) admin_ok("users.unchanged", NULL);
        else if (admin_run(on ? add : del)) admin_err("users.groupFailed", NULL);
        else { print_group(name, on); admin_ok(on ? "users.nowAdmin" : "users.noLongerAdmin", "name", name, NULL); }
    }
    free(buf);
}

/* Reads an image from the requester's home folder, WITH THEIR PERMISSIONS (setfsuid): root
 * reads nothing the user couldn't read. */
static unsigned char *read_user_image(const AdminReq *r, const char *path, size_t *n) {
    char real[PATH_MAX], home[PATH_MAX];
    if (path[0] != '/' || !realpath(r->home, home)) return NULL;
    setfsgid(r->gid);
    setfsuid(r->uid);
    unsigned char *data = NULL;
    size_t hl = strlen(home);
    int fd = -1;
    struct stat st;
    if (realpath(path, real) && !strncmp(real, home, hl) && real[hl] == '/' && (fd = open(real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) >= 0 &&
        !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 8 && st.st_size <= (4 << 20) && (data = malloc((size_t)st.st_size))) {
        if (read(fd, data, (size_t)st.st_size) != st.st_size) { free(data); data = NULL; }
        else *n = (size_t)st.st_size;
    }
    if (fd >= 0) close(fd);
    setfsuid(0);
    setfsgid(0);
    static const unsigned char png[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    if (data && memcmp(data, png, 8) && !(data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff)) { free(data); data = NULL; }
    return data;
}

void users_act_avatar(const AdminReq *r, const char *arg, size_t len) {
    char *buf = strndup(arg, len), *f[3];
    if (!buf || !split(buf, f, 3)) { admin_err("admin.invalidArgument", NULL); free(buf); return; }
    const char *name = f[0], *color = f[1], *img = f[2];
    int ok_color = !color[0] || (strlen(color) == 7 && color[0] == '#' && strspn(color + 1, "0123456789abcdefABCDEF") == 6);
    if (!person(name) || !ok_color) admin_err("admin.invalidArgument", NULL);
    else if (strcmp(name, r->user) && !r->admin) admin_err("users.avatarNotAllowed", NULL);
    else {
        mkdir("/var/lib/lordos", 0755);
        mkdir(USERS_DIR, 0755);
        char p[128], json[64];
        snprintf(p, sizeof p, USERS_DIR "/%s.json", name);
        snprintf(json, sizeof json, "{\"color\":\"%s\"}\n", color);
        int bad = write_file(p, json, strlen(json), 0644);
        snprintf(p, sizeof p, USERS_DIR "/%s.png", name);
        if (!strcmp(img, "-")) unlink(p);
        else if (img[0]) {
            size_t n = 0;
            unsigned char *data = read_user_image(r, img, &n);
            if (!data) { admin_err("users.badAvatarImage", NULL); free(buf); return; }
            bad |= write_file(p, data, n, 0644);
            free(data);
        }
        if (bad) admin_err("users.avatarFailed", NULL);
        else admin_ok("users.avatarSaved", NULL);
    }
    free(buf);
}

void users_act_autologin(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    (void)len;
    if (arg[0] && !person(arg)) { admin_err("users.invalid", NULL); return; }
    if (autologin_set(arg)) admin_err("sys.writeFailed", "path", SESSION_CONF, NULL);
    else admin_ok(arg[0] ? "users.autologinOn" : "users.autologinOff", NULL);
}

/* ---- a system with no administrator (admin_rescue.h, docs/INSTALL-DEBIAN.md) ---- */

/* Member of sudo, admin or wheel by the system's files right now, as admin.c's is_admin. Fails
 * closed: a list of groups too long to read counts as an administrator. */
static int admin_member(const struct passwd *pw) {
    gid_t groups[256];
    int n = 256;
    if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &n) < 0) return 1;
    for (int i = 0; i < n; i++) {
        struct group *g = getgrgid(groups[i]);
        if (g && rescue_group_is_admin(g->gr_name)) return 1;
    }
    return 0;
}

static int any_admin(void) {
    int found = 0;
    setpwent();
    struct passwd *pw;
    while (!found && (pw = getpwent())) found = rescue_account_counts(pw->pw_uid, admin_member(pw));
    endpwent();
    return found;
}

static int sudo_ready(void) {
    struct stat st;
    return getgrnam(ADMIN_GROUP) && !stat(SUDO_BIN, &st) && S_ISREG(st.st_mode) && st.st_uid == 0;
}

/* Always read again as root: nothing here comes from the UI. */
static RescueState rescue_now(const AdminReq *r) {
    struct passwd *pw = person(r->user);
    RescueFacts f = { .person = pw != NULL, .self_member = pw && admin_member(pw) };
    f.any_admin = any_admin();
    f.sudo_ready = sudo_ready();
    f.root_password = has_password(getspnam("root"));
    return rescue_state(&f);
}

void users_act_rescue_status(const AdminReq *r, const char *arg, size_t len) {
    (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    admin_log("users.rescueState", "state", rescue_word(rescue_now(r)), NULL);
    admin_ok("users.unchanged", NULL);
}

/* The argument is only the root password; the user is always r->user. Runs in a child (A_FORK): the
 * lock (not waited for) and the delay hold every session to one try at a time. */
void users_act_rescue(const AdminReq *r, const char *arg, size_t len) {
    (void)len;
    mkdir(RUN_DIR, 0755);
    int lock = open(RESCUE_LOCK, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        if (lock >= 0) close(lock);
        admin_err("users.rescueBusy", NULL);
        return;
    }
    RescueState st = rescue_now(r);
    const char *refused = rescue_refusal(st);
    if (refused) admin_err(refused, NULL);
    else if (!valid_pass(arg) || !users_check_password("root", arg)) {
        syslog(LOG_AUTHPRIV | LOG_WARNING, "lord-admin: admin-rescue: wrong root password from %s", r->user);
        sleep(RESCUE_FAIL_DELAY);
        admin_err("users.rescueWrongPassword", NULL);
    } else {
        const char *const a[] = { "/usr/sbin/usermod", "--append", "--groups", ADMIN_GROUP, r->user, NULL };
        if (admin_run(a) || !in_group(r->user, ADMIN_GROUP)) admin_err("users.groupFailed", NULL);
        else {
            print_group(r->user, 1);
            syslog(LOG_AUTHPRIV | LOG_NOTICE, "lord-admin: admin-rescue: %s added to the %s group", r->user, ADMIN_GROUP);
            admin_ok("users.rescueDone", "name", r->user, NULL);
        }
    }
    close(lock);
}
