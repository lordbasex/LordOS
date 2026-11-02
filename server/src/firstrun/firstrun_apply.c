/* firstrun_apply.c — lordos-firstrun: applies lordos-firstrun.txt once, as root, on the first boot of
 * a LordOS Raspberry Pi image (called by /usr/lib/lordos/rpi/firstboot). See docs/IMAGER.md.
 *
 *   lordos-firstrun check <file>                         parse only; prints "ok" or the reason
 *   lordos-firstrun apply <file> [--root DIR --boot DIR] parse, apply, shred the file
 *   lordos-firstrun shred <file>                         overwrite and delete
 *
 * Rules (the same as the admin helper's, docs/ARCHITECTURE.md):
 *   - the file is parsed by fr_parse() first; if anything is wrong, nothing is applied (exit 1);
 *   - only fixed-path programs (useradd, usermod, systemctl), with an argument list built here from
 *     validated values, never a shell; every other change is a file written by this program
 *     (temporary file, fsync, rename), never one that holds user text except as data;
 *   - secrets: only the crypt hash and the derived Wi-Fi key arrive; neither is ever logged;
 *   - the file is shredded (random bytes, zeros, fsync, unlink) whatever the outcome.
 *
 * --root/--boot (tests only) apply into a mounted image: paths are prefixed, useradd/usermod get
 * --root, systemctl gets --root and nothing is started.
 *
 * Exit status: 0 applied with a user; 2 applied without a user (the board stays in setup mode);
 * 1 invalid file (nothing applied); 3 the user could not be created (system settings applied). */
#include "firstrun.h"
#include "../users/homedirs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/rfkill.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* snprintf for paths and short lines; truncation (never expected: the parts are validated and
 * short) leaves a shorter string that the following open/stat simply fails on. */
static void pathf(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void pathf(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, n, fmt, ap);
    va_end(ap);
}

static const char *ROOT = "";                  /* "" on the board */
static const char *BOOT = "/boot/firmware";
static int TEST;                                /* --root given */

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("lordos-firstrun: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

static void rp(char *out, size_t n, const char *path) { pathf(out, n, "%s%s", ROOT, path); }

/* ---- files ---- */

static int mkdirs(const char *path, mode_t mode) {
    char p[PATH_MAX];
    pathf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(p, 0755) && errno != EEXIST) return -1;
        *s = '/';
    }
    if (mkdir(p, mode) && errno != EEXIST) return -1;
    return 0;
}

/* Atomic write: <path>.lordos-new, fsync, chmod/chown, rename. Refuses to follow a symlink at path. */
static int write_file(const char *path, const void *data, size_t n, mode_t mode, uid_t uid, gid_t gid) {
    char tmp[PATH_MAX];
    struct stat st;
    if (!lstat(path, &st) && S_ISLNK(st.st_mode)) { say("refusing to replace the symlink %s", path); return -1; }
    pathf(tmp, sizeof tmp, "%s.lordos-new", path);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    const char *p = data;
    for (size_t o = 0; o < n;) {
        ssize_t w = write(fd, p + o, n - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { close(fd); unlink(tmp); return -1; }
        o += (size_t)w;
    }
    if (fchmod(fd, mode) || (uid != (uid_t)-1 && fchown(fd, uid, gid)) || fsync(fd)) { close(fd); unlink(tmp); return -1; }
    close(fd);
    if (rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

static int write_str(const char *path, const char *s, mode_t mode) { return write_file(path, s, strlen(s), mode, (uid_t)-1, 0); }

static char *read_small(const char *path, size_t max, size_t *len) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size > max) { close(fd); return NULL; }
    char *b = malloc((size_t)st.st_size + 1);
    size_t o = 0;
    while (b && o < (size_t)st.st_size) {
        ssize_t r = read(fd, b + o, (size_t)st.st_size - o);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        o += (size_t)r;
    }
    close(fd);
    if (!b) return NULL;
    b[o] = 0;
    if (len) *len = o;
    return b;
}

/* JSON string (the values are validated, but quote everything that JSON needs anyway) */
static void jstr(char *out, size_t cap, const char *s) {
    size_t o = 0;
    if (cap < 3) { if (cap) out[0] = 0; return; }
    out[o++] = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') { out[o++] = '\\'; out[o++] = (char)*p; }
        else if (*p < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", *p);
        else out[o++] = (char)*p;
    }
    out[o++] = '"';
    out[o] = 0;
}

/* ---- programs ---- */

static char *const ENV[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", NULL };
/* --root: the image's SysV helpers (update-rc.d) may be of another architecture; the unit symlinks are enough */
static char *const ENV_TEST[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", "SYSTEMCTL_SKIP_SYSV=1", NULL };

static int run(const char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); }
        syscall(SYS_close_range, 3, ~0u, 0);
        execve(argv[0], (char *const *)argv, TEST ? ENV_TEST : ENV);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

/* ---- the target's accounts (read from its own files, so --root works) ---- */

static int lookup_user(const char *name, uid_t *uid, gid_t *gid, char *home, size_t hn) {
    char p[PATH_MAX];
    rp(p, sizeof p, "/etc/passwd");
    size_t len;
    char *b = read_small(p, 4 << 20, &len);
    if (!b) return -1;
    int found = -1;
    for (char *l = strtok(b, "\n"); l; l = strtok(NULL, "\n")) {
        char *f[7] = { 0 };
        int nf = 0;
        for (char *s = l; nf < 7; nf++) { f[nf] = s; char *c = strchr(s, ':'); if (!c) { nf++; break; } *c = 0; s = c + 1; }
        if (nf < 7 || strcmp(f[0], name)) continue;
        *uid = (uid_t)strtoul(f[2], NULL, 10);
        *gid = (gid_t)strtoul(f[3], NULL, 10);
        pathf(home, hn, "%s", f[5]);
        found = 0;
        break;
    }
    free(b);
    return found;
}

static int group_exists(const char *name) {
    char p[PATH_MAX];
    rp(p, sizeof p, "/etc/group");
    char *b = read_small(p, 4 << 20, NULL);
    if (!b) return 0;
    size_t n = strlen(name);
    int ok = 0;
    for (char *l = b; l && *l; ) {
        if (!strncmp(l, name, n) && l[n] == ':') { ok = 1; break; }
        l = strchr(l, '\n');
        if (l) l++;
    }
    free(b);
    return ok;
}

/* ---- each part ---- */

static void apply_hostname(const FirstRun *fr) {
    if (!fr->has_hostname) return;
    char p[PATH_MAX], line[96];
    rp(p, sizeof p, "/etc/hostname");
    pathf(line, sizeof line, "%s\n", fr->hostname);
    if (write_str(p, line, 0644)) say("could not write /etc/hostname");
    /* /etc/hosts: replace the 127.0.1.1 line (or add one), keep the rest */
    rp(p, sizeof p, "/etc/hosts");
    size_t len = 0;
    char *old = read_small(p, 1 << 20, &len);
    size_t cap = len + 256;
    char *nw = malloc(cap);
    if (!nw) { free(old); return; }
    size_t o = 0;
    int done = 0;
    for (char *l = old; l && *l;) {
        char *e = strchr(l, '\n');
        size_t ll = e ? (size_t)(e - l) : strlen(l);
        if (ll >= 9 && !strncmp(l, "127.0.1.1", 9) && (l[9] == ' ' || l[9] == '\t')) {
            if (!done) o += (size_t)snprintf(nw + o, cap - o, "127.0.1.1\t%s\n", fr->hostname);
            done = 1;
        } else if (o + ll + 2 < cap) {
            memcpy(nw + o, l, ll);
            o += ll;
            nw[o++] = '\n';
        }
        l = e ? e + 1 : NULL;
    }
    if (!done) o += (size_t)snprintf(nw + o, cap - o, "127.0.1.1\t%s\n", fr->hostname);
    if (write_file(p, nw, o, 0644, (uid_t)-1, 0)) say("could not write /etc/hosts");
    free(nw);
    free(old);
    if (!TEST && sethostname(fr->hostname, strlen(fr->hostname))) { /* takes effect at the next boot anyway */ }
    say("computer name: %s", fr->hostname);
}

static const char *locale_of(const char *lang) {
    if (!strcmp(lang, "es")) return "es_ES.UTF-8";
    if (!strcmp(lang, "pt")) return "pt_BR.UTF-8";
    if (!strcmp(lang, "fr")) return "fr_FR.UTF-8";
    return "en_US.UTF-8";
}

static void apply_locale(const FirstRun *fr) {
    if (fr->has_timezone) {
        char z[PATH_MAX], real[PATH_MAX], zroot[PATH_MAX], zr[PATH_MAX];
        rp(zroot, sizeof zroot, "/usr/share/zoneinfo");
        pathf(z, sizeof z, "%s/%s", zroot, fr->timezone);
        struct stat st;
        /* the zone must resolve inside zoneinfo to a regular file (Debian ships some as links) */
        if (realpath(zroot, zr) && realpath(z, real) && !strncmp(real, zr, strlen(zr)) && real[strlen(zr)] == '/' &&
            !stat(real, &st) && S_ISREG(st.st_mode)) {
            char lt[PATH_MAX], target[PATH_MAX], tzf[PATH_MAX], line[96];
            rp(lt, sizeof lt, "/etc/localtime");
            pathf(target, sizeof target, "/usr/share/zoneinfo/%s", fr->timezone);
            char tmp[PATH_MAX];
            pathf(tmp, sizeof tmp, "%s.lordos-new", lt);
            unlink(tmp);
            if (symlink(target, tmp) || rename(tmp, lt)) say("could not set /etc/localtime");
            rp(tzf, sizeof tzf, "/etc/timezone");
            pathf(line, sizeof line, "%s\n", fr->timezone);
            write_str(tzf, line, 0644);
            say("time zone: %s", fr->timezone);
        } else say("time zone %s is not in this image: kept UTC", fr->timezone);
    }
    if (fr->has_lang) {
        char p[PATH_MAX], line[64], ln[64];
        rp(p, sizeof p, "/etc/default/locale");
        /* Debian 13 makes /etc/default/locale a link to /etc/locale.conf: write that file instead */
        ssize_t l = readlink(p, ln, sizeof ln - 1);
        if (l > 0) {
            ln[l] = 0;
            if (!strcmp(ln, "../locale.conf") || !strcmp(ln, "/etc/locale.conf")) rp(p, sizeof p, "/etc/locale.conf");
        }
        pathf(line, sizeof line, "LANG=%s\n", locale_of(fr->lang));
        if (write_str(p, line, 0644)) say("could not write /etc/default/locale");
        else say("language: %s", fr->lang);
    }
    if (fr->has_keyboard) {
        char sym[PATH_MAX], p[PATH_MAX], txt[256];
        pathf(sym, sizeof sym, "%s/usr/share/X11/xkb/symbols/%s", ROOT, fr->keyboard);
        struct stat st;
        if (stat(sym, &st) || !S_ISREG(st.st_mode)) say("keyboard layout %s is not in this image: kept the default", fr->keyboard);
        else {
            rp(p, sizeof p, "/etc/default/keyboard");
            pathf(txt, sizeof txt, "XKBMODEL=\"pc105\"\nXKBLAYOUT=\"%s\"\nXKBVARIANT=\"%s\"\nXKBOPTIONS=\"\"\nBACKSPACE=\"guess\"\n",
                     fr->keyboard, fr->variant);
            if (write_str(p, txt, 0644)) say("could not write /etc/default/keyboard");
            else say("keyboard: %s%s%s", fr->keyboard, fr->variant[0] ? " " : "", fr->variant);
        }
    }
}

/* cmdline.txt is one line; drop an old regdom option and add the new one */
static void apply_wifi_country(const FirstRun *fr) {
    if (!fr->has_wifi_country) return;
    char p[PATH_MAX];
    pathf(p, sizeof p, "%s/cmdline.txt", BOOT);
    size_t len;
    char *b = read_small(p, 8192, &len);
    if (!b) { say("no cmdline.txt: Wi-Fi country not set"); return; }
    b[strcspn(b, "\r\n")] = 0;
    char out[8400];
    size_t o = 0;
    for (char *w = strtok(b, " "); w; w = strtok(NULL, " ")) {
        if (!strncmp(w, "cfg80211.ieee80211_regdom=", 26)) continue;
        if (o + strlen(w) + 2 >= sizeof out) break;
        o += (size_t)snprintf(out + o, sizeof out - o, "%s%s", o ? " " : "", w);
    }
    o += (size_t)snprintf(out + o, sizeof out - o, "%scfg80211.ieee80211_regdom=%s\n", o ? " " : "", fr->wifi_country);
    free(b);
    if (write_file(p, out, o, 0755, (uid_t)-1, 0)) say("could not write cmdline.txt");
    else say("Wi-Fi country: %s (from the next boot)", fr->wifi_country);
}

static void apply_wifi(const FirstRun *fr) {
    if (!fr->has_wifi) return;
    char dir[PATH_MAX], hex[72], p[PATH_MAX], ssid_j[160], ssid_s[33], meta[320], key[96];
    rp(dir, sizeof dir, "/etc/lordos/net/wifi");
    if (mkdirs(dir, 0755)) { say("could not create %s", dir); return; }
    for (size_t i = 0; i < fr->ssid_len; i++) snprintf(hex + 2 * i, 3, "%02x", fr->ssid[i]);
    memcpy(ssid_s, fr->ssid, fr->ssid_len);
    ssid_s[fr->ssid_len] = 0;
    jstr(ssid_j, sizeof ssid_j, ssid_s);
    pathf(meta, sizeof meta, "{\"ssid\":%s,\"security\":\"%s\"%s}\n", ssid_j, fr->has_psk ? "psk" : "open", fr->hidden ? ",\"hidden\":true" : "");
    pathf(p, sizeof p, "%s/%s.json", dir, hex);
    int ok = write_str(p, meta, 0644) == 0;
    /* the derived key (psk=, lord-netd's net/wifi.c), never the password */
    int kl = snprintf(key, sizeof key, "%s%s\n", fr->has_psk ? "psk=" : "pass=", fr->has_psk ? fr->psk : "");
    pathf(p, sizeof p, "%s/%s.key", dir, hex);
    ok = ok && write_file(p, key, (size_t)kl, 0600, 0, 0) == 0;
    memset(key, 0, sizeof key);
    say(ok ? "Wi-Fi network saved (%s%s)" : "could not save the Wi-Fi network", fr->has_psk ? "WPA2" : "open", fr->hidden ? ", hidden" : "");
}

/* ~/.config/lordos/settings.json: language, keyboard, appearance and updates (sdk/settings.js) */
static void user_settings(const FirstRun *fr, const char *home, uid_t uid, gid_t gid) {
    char d1[PATH_MAX], d2[PATH_MAX], p[PATH_MAX];
    pathf(d1, sizeof d1, "%s%s/.config", ROOT, home);
    pathf(d2, sizeof d2, "%s/lordos", d1);
    if ((mkdir(d1, 0755) && errno != EEXIST) || (mkdir(d2, 0755) && errno != EEXIST)) return;
    if (chown(d1, uid, gid) || chown(d2, uid, gid)) { /* keep going: the files get the right owner */ }
    char j[2048], v[128];
    size_t o = (size_t)snprintf(j, sizeof j, "{\n  \"schema\": 1");
#define FIELD(name, val) do { jstr(v, sizeof v, val); o += (size_t)snprintf(j + o, sizeof j - o, ",\n  \"%s\": %s", name, v); } while (0)
    FIELD("language", fr->has_lang ? fr->lang : "en");
    if (fr->has_keyboard) {
        char l[40], va[64];
        jstr(l, sizeof l, fr->keyboard);
        jstr(va, sizeof va, fr->variant);
        o += (size_t)snprintf(j + o, sizeof j - o, ",\n  \"keyboards\": [{ \"layout\": %s, \"variant\": %s }],\n  \"keyboardIndex\": 0", l, va);
    }
    if (fr->has_theme) FIELD("theme", fr->theme);
    if (fr->has_mode) FIELD("mode", fr->mode);
    if (fr->has_accent) FIELD("accent", fr->accent);
    if (fr->has_wallpaper) FIELD("wallpaper", fr->wallpaper);
    if (fr->has_updates) o += (size_t)snprintf(j + o, sizeof j - o, ",\n  \"updates\": { \"autoCheck\": %s }", fr->updates ? "true" : "false");
#undef FIELD
    o += (size_t)snprintf(j + o, sizeof j - o, "\n}\n");
    pathf(p, sizeof p, "%s/settings.json", d2);
    if (write_file(p, j, o, 0644, uid, gid)) say("could not write the user's settings");
    char lc[64];
    pathf(lc, sizeof lc, "LANG=%s\n", locale_of(fr->has_lang ? fr->lang : "en"));
    pathf(p, sizeof p, "%s/locale.conf", d1);
    write_file(p, lc, strlen(lc), 0644, uid, gid);
}

static void user_ssh_keys(const FirstRun *fr, const char *home, uid_t uid, gid_t gid) {
    if (!fr->nkeys) return;
    char d[PATH_MAX], p[PATH_MAX];
    pathf(d, sizeof d, "%s%s/.ssh", ROOT, home);
    struct stat st;
    if (!lstat(d, &st) && !S_ISDIR(st.st_mode)) { say("~/.ssh is not a folder: keys not added"); return; }
    if (mkdir(d, 0700) && errno != EEXIST) return;
    if (chown(d, uid, gid) || chmod(d, 0700)) { say("could not set up ~/.ssh"); return; }
    size_t cap = (size_t)fr->nkeys * (FR_KEY_MAX + 1) + 1, o = 0;
    char *b = malloc(cap);
    if (!b) return;
    for (int i = 0; i < fr->nkeys; i++) o += (size_t)snprintf(b + o, cap - o, "%s\n", fr->keys[i]);
    pathf(p, sizeof p, "%s/authorized_keys", d);
    if (write_file(p, b, o, 0600, uid, gid)) say("could not write authorized_keys");
    else say("%d SSH key(s) added for %s", fr->nkeys, fr->user);
    free(b);
}

static int apply_user(const FirstRun *fr) {
    if (!fr->has_user) return 2;
    uid_t uid;
    gid_t gid;
    char home[PATH_MAX];
    if (!lookup_user(fr->user, &uid, &gid, home, sizeof home)) { say("user %s already exists: not created", fr->user); return 3; }
    const char *full = fr->has_fullname ? fr->fullname : fr->user;
    if (TEST) {
        const char *const a[] = { "/usr/sbin/useradd", "--root", ROOT, "--create-home", "--shell", "/bin/bash", "--user-group", "--comment", full, fr->user, NULL };
        if (run(a)) { say("useradd failed"); return 3; }
    } else {
        const char *const a[] = { "/usr/sbin/useradd", "--create-home", "--shell", "/bin/bash", "--user-group", "--comment", full, fr->user, NULL };
        if (run(a)) { say("useradd failed"); return 3; }
    }
    /* the hash goes through usermod -p (a crypt string, validated; never a plain password) */
    {
        const char *const t[] = { "/usr/sbin/usermod", "--root", ROOT, "--password", fr->hash, fr->user, NULL };
        const char *const b[] = { "/usr/sbin/usermod", "--password", fr->hash, fr->user, NULL };
        if (run(TEST ? t : b)) { say("could not set the password"); return 3; }
    }
    static const char *const GROUPS[] = { "sudo", "audio", "video", "input", "render", "pipewire", "plugdev", "lpadmin", "bluetooth", "netdev" };
    char groups[160] = "";
    for (size_t i = 0; i < sizeof GROUPS / sizeof *GROUPS; i++)
        if (group_exists(GROUPS[i])) { if (groups[0]) strcat(groups, ","); strcat(groups, GROUPS[i]); }
    if (groups[0]) {
        const char *const t[] = { "/usr/sbin/usermod", "--root", ROOT, "--append", "--groups", groups, fr->user, NULL };
        const char *const b[] = { "/usr/sbin/usermod", "--append", "--groups", groups, fr->user, NULL };
        if (run(TEST ? t : b)) say("could not add %s to its groups", fr->user);
    }
    if (lookup_user(fr->user, &uid, &gid, home, sizeof home)) { say("user %s not found after useradd", fr->user); return 3; }
    char p[PATH_MAX], h[PATH_MAX];
    rp(p, sizeof p, "/var/lib/systemd/linger");
    mkdirs(p, 0755);
    pathf(h, sizeof h, "%s/%s", p, fr->user);
    write_str(h, "", 0644);                                 /* PipeWire before login, like the installer */
    pathf(h, sizeof h, "%s%s", ROOT, home);
    homedirs_create(h, uid, gid, locale_of(fr->has_lang ? fr->lang : "en"));
    user_settings(fr, home, uid, gid);
    user_ssh_keys(fr, home, uid, gid);
    /* automatic login (lord-sessiond reads AUTOLOGIN=; empty = the login screen) */
    char conf[PATH_MAX], txt[96];
    rp(conf, sizeof conf, "/etc/lordos");
    mkdirs(conf, 0755);
    rp(conf, sizeof conf, "/etc/lordos/session.conf");
    pathf(txt, sizeof txt, "# Set by lordos-firstrun (LordOS Imager); Settings → Users changes it.\nAUTOLOGIN=%s\n", fr->autologin ? fr->user : "");
    write_str(conf, txt, 0644);
    say("user %s created (administrator)%s", fr->user, fr->autologin ? ", logs in automatically" : "");
    return 0;
}

static int systemctl(const char *verb, const char *unit) {
    if (TEST) {
        if (strcmp(verb, "enable") && strcmp(verb, "disable")) return 0;      /* nothing runs in a test */
        const char *const a[] = { "/usr/bin/systemctl", "--root", ROOT, "--quiet", verb, unit, NULL };
        return run(a);
    }
    if (!strcmp(verb, "start")) {
        const char *const a[] = { "/usr/bin/systemctl", "--no-block", "--quiet", "start", unit, NULL };
        return run(a);
    }
    const char *const a[] = { "/usr/bin/systemctl", "--quiet", verb, unit, NULL };
    return run(a);
}

static void apply_ssh(const FirstRun *fr) {
    if (!fr->has_ssh) return;
    char sshd[PATH_MAX], d[PATH_MAX], p[PATH_MAX];
    rp(sshd, sizeof sshd, "/usr/sbin/sshd");
    rp(d, sizeof d, "/etc/ssh/sshd_config.d");
    pathf(p, sizeof p, "%s/05-lordos-keys-only.conf", d);
    if (fr->ssh == FR_SSH_OFF) {
        unlink(p);
        if (!access(sshd, X_OK)) systemctl("disable", "ssh.service");
        say("SSH: off");
        return;
    }
    if (access(sshd, X_OK)) { say("this image has no SSH server: SSH not turned on"); return; }
    if (fr->ssh == FR_SSH_KEYS) {
        mkdirs(d, 0755);
        if (write_str(p, "# LordOS Imager: key logins only (docs/IMAGER.md). Remove this file to allow passwords.\n"
                         "PasswordAuthentication no\nKbdInteractiveAuthentication no\n", 0644)) say("could not write the SSH drop-in");
    } else unlink(p);
    if (systemctl("enable", "ssh.service")) say("could not enable ssh.service");
    systemctl("start", "ssh.service");
    say("SSH: on, %s", fr->ssh == FR_SSH_KEYS ? "keys only" : "password and keys");
}

static void apply_remote(const FirstRun *fr) {
    if (!fr->has_remote) return;
    char d[PATH_MAX], p[PATH_MAX];
    rp(d, sizeof d, "/etc/lordos/remote");
    rp(p, sizeof p, "/etc/lordos/remote/config");
    if (!fr->remote) {
        if (!unlink(p)) say("remote desktop: off");
        return;
    }
    mkdirs(d, 0755);
    /* the same file and keys as Settings → Sharing (remote_admin.c): local network only by default */
    if (write_str(p, "# LordOS · remote desktop (Settings → Sharing). See docs/REMOTE.md\n"
                     "enabled=1\nport=5673\nallow=all\nnet=lan\ninsecure_dev=0\n", 0644)) say("could not write the remote desktop config");
    else say("remote desktop: on, port 5673, local network only");
}

static void apply_bluetooth(const FirstRun *fr) {
    if (!fr->has_bluetooth || fr->bluetooth) return;
    if (TEST) { say("Bluetooth: off (soft-blocked at boot; test run, not applied)"); return; }
    /* a soft block for every Bluetooth radio, also the ones that appear later; systemd-rfkill keeps
     * it across boots, and Settings → Bluetooth can turn it back on */
    int fd = open("/dev/rfkill", O_RDWR | O_CLOEXEC);
    if (fd < 0) { say("no /dev/rfkill: Bluetooth left on"); return; }
    struct rfkill_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = RFKILL_TYPE_BLUETOOTH;
    ev.op = RFKILL_OP_CHANGE_ALL;
    ev.soft = 1;
    if (write(fd, &ev, RFKILL_EVENT_SIZE_V1) != RFKILL_EVENT_SIZE_V1) say("rfkill: could not block Bluetooth");
    else say("Bluetooth: off (Settings → Bluetooth turns it on)");
    close(fd);
}

/* config.txt: one block of our own, replaced on each run; the image includes it under [all] */
static void config_block(const char *body) {
    char p[PATH_MAX];
    pathf(p, sizeof p, "%s/config.txt", BOOT);
    size_t len;
    char *b = read_small(p, 1 << 20, &len);
    if (!b) { say("no config.txt: clock settings not written"); return; }
    static const char BEGIN[] = "# >>> lordos-firstrun (LordOS Imager) >>>", END[] = "# <<< lordos-firstrun <<<";
    size_t cap = len + strlen(body) + 512, o = 0;
    char *out = malloc(cap);
    if (!out) { free(b); return; }
    int skip = 0;
    for (char *l = b; l && *l;) {
        char *e = strchr(l, '\n');
        size_t ll = e ? (size_t)(e - l) : strlen(l);
        if (ll >= sizeof BEGIN - 1 && !strncmp(l, BEGIN, sizeof BEGIN - 1)) skip = 1;
        if (!skip) { memcpy(out + o, l, ll); o += ll; out[o++] = '\n'; }
        if (skip && ll >= sizeof END - 1 && !strncmp(l, END, sizeof END - 1)) skip = 0;
        l = e ? e + 1 : NULL;
    }
    if (body[0]) o += (size_t)snprintf(out + o, cap - o, "%s\n%s%s\n", BEGIN, body, END);
    if (write_file(p, out, o, 0755, (uid_t)-1, 0)) say("could not write config.txt");
    free(out);
    free(b);
}

static void fand_profile(const char *profile) {
    char p[PATH_MAX], d[PATH_MAX], j[200];
    rp(d, sizeof d, "/etc/lordos");
    mkdirs(d, 0755);
    rp(p, sizeof p, "/etc/lordos/fan.json");
    /* the same file lord-fand writes (docs/FANS.md); it reads it when it starts */
    pathf(j, sizeof j, "{\"version\":1,\"profile\":\"%s\",\"hyst\":3,\"custom\":[[55,30],[60,55],[65,100]],\"force\":\"off\",\"forceUntil\":0}\n", profile);
    if (write_str(p, j, 0644)) { say("could not write fan.json"); return; }
    if (TEST) return;
    /* lord-fand may already be running (it starts in sysinit): tell it too (root is an administrator) */
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    pathf(a.sun_path, sizeof a.sun_path, "/run/lordos/fand.sock");
    if (s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof a)) {
        char line[64];
        int n = snprintf(line, sizeof line, "PROFILE %s\n", profile);
        if (write(s, line, (size_t)n) == n) { char r[64]; if (read(s, r, sizeof r) < 0) { /* the file is enough */ } }
    }
    if (s >= 0) close(s);
}

static void apply_cooling(const FirstRun *fr) {
    if (!fr->has_cooler) return;
    int fan = fr->cooler == FR_COOL_CASE_FAN || fr->cooler == FR_COOL_ACTIVE;
    int perf = fr->has_perf ? fr->perf : (fr->cooler == FR_COOL_NONE ? FR_PERF_CONSERVATIVE : FR_PERF_BALANCED);
    static const char *const NAMES[] = { "", "balanced", "performance", "quiet", "conservative" };
    if (fan) fand_profile(perf == FR_PERF_PERFORMANCE ? "performance" : perf == FR_PERF_QUIET ? "quiet" : "balanced");
    /* CPU governor: performance holds the top clock (only with a fan); the rest keep the kernel's */
    char tf[PATH_MAX], td[PATH_MAX];
    rp(td, sizeof td, "/etc/tmpfiles.d");
    rp(tf, sizeof tf, "/etc/tmpfiles.d/lordos-cpu-governor.conf");
    if (fan && perf == FR_PERF_PERFORMANCE) {
        mkdirs(td, 0755);
        write_str(tf, "# LordOS Imager: performance profile (docs/IMAGER.md). Delete this file to go back to the default.\n"
                      "w /sys/devices/system/cpu/cpufreq/policy*/scaling_governor - - - - performance\n", 0644);
    } else unlink(tf);
    /* clocks in config.txt (read by the firmware at the next boot) */
    char body[512] = "";
    if (fan && fr->has_overclock && fr->overclock)
        pathf(body, sizeof body, "# Mild overclock, only with a fan: 2.6 GHz instead of 2.4 GHz, no extra voltage.\n"
                                    "# If the board does not start, delete this block from another computer.\n[pi5]\narm_freq=2600\n[all]\n");
    else if (fr->cooler == FR_COOL_NONE && perf == FR_PERF_CONSERVATIVE)
        pathf(body, sizeof body, "# No cooling: a lower top clock (2.0 GHz) so the board stays cooler and does not throttle.\n[pi5]\narm_freq=2000\n[all]\n");
    config_block(body);
    static const char *const COOL[] = { "", "case with fan", "active cooler", "passive case", "no cooling" };
    say("cooling: %s, %s profile%s", COOL[fr->cooler], NAMES[perf], body[0] && fan ? ", mild overclock" : body[0] ? ", lower clock" : "");
}

/* ---- shredding the file ---- */

static int fr_shred(const char *path) {
    int fd = open(path, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        struct stat st;
        if (!fstat(fd, &st) && S_ISREG(st.st_mode)) {
            unsigned char buf[4096];
            for (int pass = 0; pass < 2; pass++) {
                int ur = pass == 0 ? open("/dev/urandom", O_RDONLY | O_CLOEXEC) : -1;
                for (off_t o = 0; o < st.st_size;) {
                    size_t k = (size_t)(st.st_size - o) > sizeof buf ? sizeof buf : (size_t)(st.st_size - o);
                    if (ur < 0 || read(ur, buf, k) != (ssize_t)k) memset(buf, 0, k);
                    if (pwrite(fd, buf, k, o) != (ssize_t)k) break;
                    o += (off_t)k;
                }
                if (ur >= 0) close(ur);
                fsync(fd);
            }
        }
        close(fd);
    }
    int r = unlink(path);
    sync();
    return r;
}

/* ---- main ---- */

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: lordos-firstrun check|apply|shred <file> [--root DIR --boot DIR]\n");
        return 1;
    }
    const char *cmd = argv[1], *file = argv[2];
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--root")) { ROOT = argv[i + 1]; TEST = 1; }
        else if (!strcmp(argv[i], "--boot")) BOOT = argv[i + 1];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 1; }
    }
    if (!strcmp(cmd, "shred")) return fr_shred(file) ? 1 : 0;
    if (strcmp(cmd, "check") && strcmp(cmd, "apply")) { fprintf(stderr, "unknown command %s\n", cmd); return 1; }
    size_t len = 0;
    char *buf = read_small(file, FR_MAX_FILE, &len);
    if (!buf) {
        say("cannot read %s (missing, not a regular file or larger than 64 KiB)", file);
        if (!strcmp(cmd, "apply")) fr_shred(file);
        return 1;
    }
    FirstRun fr;
    char err[200];
    int bad = fr_parse(buf, len, &fr, err, sizeof err);
    memset(buf, 0, len);
    free(buf);
    if (!strcmp(cmd, "check")) {
        if (bad) printf("invalid: %s\n", err); else printf("ok\n");
        fr_free(&fr);
        return bad ? 1 : 0;
    }
    if (bad) {
        say("%s is invalid (%s): nothing applied, the board starts in setup mode", file, err);
        fr_shred(file);
        fr_free(&fr);
        return 1;
    }
    say("applying %s (written by the LordOS Imager)", file);
    apply_hostname(&fr);
    apply_locale(&fr);
    apply_wifi_country(&fr);
    apply_wifi(&fr);
    int rc = apply_user(&fr);
    apply_ssh(&fr);
    apply_remote(&fr);
    apply_bluetooth(&fr);
    apply_cooling(&fr);
    fr_free(&fr);
    if (fr_shred(file)) say("could not delete %s", file);
    else say("%s shredded", file);
    return rc;
}
