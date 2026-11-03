/* autoinstall.c — lord-autoinstall, the unattended install from the live ISO (docs/UNATTENDED.md).
 *
 * The boot menu's "Install automatically" entry (lordos.auto=1, never the default) starts
 * lordos-autoinstall.service before the login screen. This program then:
 *
 *   1. looks for lordos-auto.conf on a file system labelled LORDOSAUTO (a small second disk, USB stick
 *      or ISO; never the disk it installs to), mounted read-only;
 *   2. reads it: key = value lines, every key known, every value checked;
 *   3. checks the target: the disk named by `disk` has to exist, be installable (install-disks' own list)
 *      and have exactly `disk_bytes` bytes. An erase install also needs `erase = yes`;
 *   4. calls install-run's own code (install.c) with the plan, plus the password as a hash, an SSH key
 *      and automatic login, which only this program may pass;
 *   5. reboots, powers off or stops, as `after` says.
 *
 * Anything missing or wrong stops it before any disk is touched: the live system then starts as usual
 * and the reason is on the console and in the journal (journalctl -u lordos-autoinstall).
 *
 *   lord-autoinstall                 the service (needs lordos.auto=1 on the kernel command line)
 *   lord-autoinstall --check FILE    reads FILE and checks it and the disk, installs nothing
 *
 * admin_say and admin_run* are this program's own: what install.c reports goes to standard output. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "install.h"
#include "freepart.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONF_LABEL  "LORDOSAUTO"
#define CONF_NAME   "lordos-auto.conf"
#define CONF_MOUNT  "/run/lordos-auto/media"
#define RESULT      "/run/lordos-auto/result"
#define CONF_MAX    16384

/* ---- the admin helper's interface, for install.c ---- */

static char last_kind[8];
static char last_line[2048];

void admin_say(const char *kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_line, sizeof last_line, fmt, ap);
    va_end(ap);
    snprintf(last_kind, sizeof last_kind, "%s", kind);
    printf("lordos-auto: %s %s\n", kind, last_line);
    fflush(stdout);
}
void admin_event(const char *fmt, ...) { (void)fmt; }

static int run_io(const char *const argv[], const char *input, char *out, size_t cap) {
    int in[2] = { -1, -1 }, p[2];
    if (pipe(p)) return -1;
    if (input && pipe(in)) { close(p[0]); close(p[1]); return -1; }
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (input) { dup2(in[0], 0); close(in[0]); close(in[1]); }
        else { int nul = open("/dev/null", O_RDONLY); if (nul >= 0) dup2(nul, 0); }
        dup2(p[1], 1);
        if (!out) dup2(p[1], 2);
        close(p[0]);
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", NULL };
        execve(argv[0], (char *const *)argv, env);
        _exit(127);
    }
    close(p[1]);
    if (input) {
        close(in[0]);
        size_t n = strlen(input);
        if (write(in[1], input, n) != (ssize_t)n) { /* the program reads what it can */ }
        close(in[1]);
    }
    size_t n = 0;
    char buf[1024];
    for (ssize_t r; (r = read(p[0], buf, sizeof buf)) > 0;) {
        if (out) { if (n + (size_t)r < cap) { memcpy(out + n, buf, (size_t)r); n += (size_t)r; } }
        else fwrite(buf, 1, (size_t)r, stdout);
    }
    if (out) out[n] = 0;
    fflush(stdout);
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}
int admin_run(const char *const argv[]) { return run_io(argv, NULL, NULL, 0); }
int admin_run_input(const char *const argv[], const char *input) { return run_io(argv, input, NULL, 0); }
int admin_run_capture(const char *const argv[], char *out, size_t cap) { return run_io(argv, NULL, out, cap); }

/* ---- the config file ---- */

typedef struct {
    char disk[32], mode[16], erase[8], user[33], fullname[128], password_hash[256], ssh_key[1024], hostname[64],
         lang[16], locale[64], timezone[64], keyboard[24], keyboard_variant[40], autologin[8], after[16];
    unsigned long long disk_bytes;
    long swap;
    int has_disk_bytes;
} Conf;

static int fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int fail(const char *fmt, ...) {
    va_list ap;
    char m[512];
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    printf("lordos-auto: STOP %s. Nothing was changed on any disk.\n", m);
    fflush(stdout);
    FILE *f = fopen(RESULT, "w");
    if (f) { fprintf(f, "STOP %s\n", m); fclose(f); }
    return 1;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* Returns NULL, or what is wrong with line `ln`. */
static const char *parse_conf(char *text, Conf *c, int *ln) {
    memset(c, 0, sizeof *c);
    snprintf(c->mode, sizeof c->mode, "erase");
    snprintf(c->lang, sizeof c->lang, "en");
    snprintf(c->locale, sizeof c->locale, "en_US.UTF-8");
    snprintf(c->timezone, sizeof c->timezone, "UTC");
    snprintf(c->keyboard, sizeof c->keyboard, "us");
    snprintf(c->autologin, sizeof c->autologin, "no");
    snprintf(c->after, sizeof c->after, "reboot");
    *ln = 0;
    for (char *save = NULL, *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        ++*ln;
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        char *eq = strchr(l, '=');
        if (!eq) return "a line without '='";
        *eq = 0;
        char *k = trim(l), *v = trim(eq + 1);
        struct { const char *k; char *dst; size_t n; } f[] = {
            { "disk", c->disk, sizeof c->disk }, { "mode", c->mode, sizeof c->mode }, { "erase", c->erase, sizeof c->erase },
            { "user", c->user, sizeof c->user }, { "fullname", c->fullname, sizeof c->fullname },
            { "password_hash", c->password_hash, sizeof c->password_hash }, { "ssh_key", c->ssh_key, sizeof c->ssh_key },
            { "hostname", c->hostname, sizeof c->hostname }, { "lang", c->lang, sizeof c->lang }, { "locale", c->locale, sizeof c->locale },
            { "timezone", c->timezone, sizeof c->timezone }, { "keyboard", c->keyboard, sizeof c->keyboard },
            { "keyboard_variant", c->keyboard_variant, sizeof c->keyboard_variant }, { "autologin", c->autologin, sizeof c->autologin },
            { "after", c->after, sizeof c->after },
        };
        int known = 0;
        for (size_t i = 0; i < sizeof f / sizeof *f; i++)
            if (!strcmp(k, f[i].k)) {
                if (strlen(v) >= f[i].n) return "a value that is too long";
                snprintf(f[i].dst, f[i].n, "%s", v);
                known = 1;
            }
        if (!strcmp(k, "disk_bytes") || !strcmp(k, "swap")) {
            char *end;
            errno = 0;
            if (!isdigit((unsigned char)*v)) return "a number that isn't one";
            unsigned long long u = strtoull(v, &end, 10);
            if (errno || *end) return "a number that isn't one";
            if (k[0] == 'd') { c->disk_bytes = u; c->has_disk_bytes = 1; }
            else { if (u > 16384) return "swap over 16384 MiB"; c->swap = (long)u; }
            known = 1;
        }
        if (!known) return "an unknown key (a typo?)";
    }
    return NULL;
}

static int yes(const char *s) { return !strcmp(s, "yes"); }
static int yes_no(const char *s) { return !strcmp(s, "yes") || !strcmp(s, "no"); }

/* The whole-disk name of a partition or disk device ("vdb1" → "vdb", "nvme0n1p2" → "nvme0n1", "sr0" → "sr0"). */
static void parent_disk(const char *dev, char *out, size_t n) {
    const char *b = strrchr(dev, '/');
    b = b ? b + 1 : dev;
    char p[PATH_MAX], real[PATH_MAX];
    snprintf(p, sizeof p, "/sys/class/block/%s", b);
    if (realpath(p, real)) {
        char part[PATH_MAX + 16];
        snprintf(part, sizeof part, "%s/partition", real);
        if (!access(part, F_OK)) {               /* a partition: its parent is the directory above */
            char *s = strrchr(real, '/');
            if (s) { *s = 0; s = strrchr(real, '/'); if (s) { snprintf(out, n, "%s", s + 1); return; } }
        }
    }
    snprintf(out, n, "%s", b);
}

static unsigned long long disk_size(const char *disk) {
    char p[PATH_MAX], v[64] = "";
    snprintf(p, sizeof p, "/sys/block/%s/size", disk);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    if (!fgets(v, sizeof v, f)) v[0] = 0;
    fclose(f);
    return strtoull(v, NULL, 10) * 512ull;
}

__attribute__((unused)) static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
        else if (ch < 0x20) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}

/* Everything that can be checked before touching anything; fills the free-space region in mode free. */
static int check(Conf *c, const char *conf_dev, unsigned long long *free_start, unsigned long long *free_end) {
    if (!c->disk[0]) return fail("lordos-auto.conf names no disk");
    for (const char *p = c->disk; *p; p++) if (!isalnum((unsigned char)*p)) return fail("the disk name \"%s\" isn't a plain device name", c->disk);
    if (!c->has_disk_bytes) return fail("lordos-auto.conf has no disk_bytes (the disk's exact size, so no other disk is ever erased)");
    unsigned long long size = disk_size(c->disk);
    if (!size) return fail("there is no disk /dev/%s on this computer", c->disk);
    if (size != c->disk_bytes) return fail("/dev/%s has %llu bytes, lordos-auto.conf expects %llu", c->disk, size, c->disk_bytes);
    if (conf_dev) {
        char parent[64];
        parent_disk(conf_dev, parent, sizeof parent);
        if (!strcmp(parent, c->disk)) return fail("/dev/%s holds lordos-auto.conf itself", c->disk);
    }
    if (strcmp(c->mode, "erase") && strcmp(c->mode, "free")) return fail("mode is \"%s\": erase or free", c->mode);
    if (!strcmp(c->mode, "erase") && !yes(c->erase)) return fail("mode erase wipes /dev/%s: say so with erase = yes", c->disk);
    if (!c->user[0] || !c->hostname[0]) return fail("user and hostname are required");
    if (!c->password_hash[0]) return fail("password_hash is required (the password itself never goes in this file)");
    if (!install_hash_ok(c->password_hash)) return fail("password_hash isn't a yescrypt ($y$) or SHA-512 ($6$) hash");
    if (c->ssh_key[0] && !install_ssh_key_ok(c->ssh_key)) return fail("ssh_key isn't one public key line");
    if (!yes_no(c->autologin)) return fail("autologin is yes or no");
    if (strcmp(c->after, "reboot") && strcmp(c->after, "poweroff") && strcmp(c->after, "stay")) return fail("after is reboot, poweroff or stay");
    if (!strcmp(c->mode, "free")) {
        static FpProbe fp;
        fp_probe(c->disk, &fp);
        if (fp.why) return fail("/dev/%s can't take an install in free space (%s)", c->disk, fp.why);
        *free_start = fp.choice.region.start;
        *free_end = fp.choice.region.end;
    }
    return 0;
}

__attribute__((unused)) static char *read_conf(const char *path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > CONF_MAX) { close(fd); return NULL; }
    char *buf = calloc(1, (size_t)st.st_size + 1);
    ssize_t r = buf ? read(fd, buf, (size_t)st.st_size) : -1;
    close(fd);
    if (r != st.st_size) { free(buf); return NULL; }
    return buf;
}

__attribute__((unused)) static int cmdline_auto(void) {
    char line[4096] = "";
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f) return 0;
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    for (char *save = NULL, *t = strtok_r(line, " \n", &save); t; t = strtok_r(NULL, " \n", &save))
        if (!strcmp(t, "lordos.auto=1")) return 1;
    return 0;
}

/* Mounts the LORDOSAUTO file system read-only and returns its device, or NULL. */
__attribute__((unused)) static const char *mount_conf(void) {
    static char dev[PATH_MAX];
    if (!realpath("/dev/disk/by-label/" CONF_LABEL, dev)) return NULL;
    mkdir("/run/lordos-auto", 0700);
    mkdir(CONF_MOUNT, 0700);
    umount2(CONF_MOUNT, MNT_DETACH);
    static const char *const fs[] = { "vfat", "iso9660", "ext4", NULL };
    for (int i = 0; fs[i]; i++)
        if (!mount(dev, CONF_MOUNT, fs[i], MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL)) return dev;
    return NULL;
}

#ifndef AUTOINSTALL_TEST
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *check_only = argc == 3 && !strcmp(argv[1], "--check") ? argv[2] : NULL;
    if (argc > 1 && !check_only) { fprintf(stderr, "usage: lord-autoinstall [--check lordos-auto.conf]\n"); return 2; }
    if (geteuid() != 0) { fprintf(stderr, "lord-autoinstall: run as root\n"); return 2; }
    mkdir("/run/lordos-auto", 0700);

    const char *conf_dev = NULL;
    char path[PATH_MAX];
    if (check_only) snprintf(path, sizeof path, "%s", check_only);
    else {
        if (!cmdline_auto()) return fail("the computer wasn't started with the boot menu's Install automatically entry");
        if (access("/run/live/medium/live/filesystem.squashfs", R_OK)) return fail("this isn't the live system");
        printf("lordos-auto: looking for %s on a disk labelled %s\n", CONF_NAME, CONF_LABEL);
        for (int i = 0; i < 20 && !(conf_dev = mount_conf()); i++) sleep(1);     /* a USB stick may come late */
        if (!conf_dev) return fail("no file system labelled %s was found", CONF_LABEL);
        snprintf(path, sizeof path, CONF_MOUNT "/" CONF_NAME);
    }
    char *text = read_conf(path);
    if (conf_dev) umount2(CONF_MOUNT, MNT_DETACH);
    if (!text) return fail("%s can't be read (missing, empty, not a regular file or over %d bytes)", check_only ? path : CONF_NAME, CONF_MAX);

    Conf c;
    int ln;
    const char *why = parse_conf(text, &c, &ln);
    explicit_bzero(text, strlen(text));
    free(text);
    if (why) return fail("lordos-auto.conf, line %d: %s", ln, why);
    unsigned long long fs = 0, fe = 0;
    if (check(&c, conf_dev, &fs, &fe)) return 1;
    printf("lordos-auto: the plan: %s install on /dev/%s (%llu bytes), user %s, computer %s, %s, %s, %s, keyboard %s%s%s, "
           "swap %ld MiB, automatic login %s, SSH key %s, then %s\n",
           c.mode, c.disk, c.disk_bytes, c.user, c.hostname, c.lang, c.locale, c.timezone, c.keyboard,
           c.keyboard_variant[0] ? " " : "", c.keyboard_variant, c.swap, c.autologin, c.ssh_key[0] ? "yes" : "no", c.after);
    if (check_only) { printf("lordos-auto: CHECK OK, nothing installed\n"); return 0; }

    char *json = NULL;
    size_t jl = 0;
    FILE *j = open_memstream(&json, &jl);
    if (!j) return fail("out of memory");
    fprintf(j, "{\"mode\":"); json_str(j, c.mode);
    fprintf(j, ",\"disk\":"); json_str(j, c.disk);
    fprintf(j, ",\"user\":"); json_str(j, c.user);
    fprintf(j, ",\"fullname\":"); json_str(j, c.fullname[0] ? c.fullname : c.user);
    fprintf(j, ",\"passwordHash\":"); json_str(j, c.password_hash);
    if (c.ssh_key[0]) { fprintf(j, ",\"sshKey\":"); json_str(j, c.ssh_key); }
    fprintf(j, ",\"autologin\":%s", yes(c.autologin) ? "true" : "false");
    fprintf(j, ",\"hostname\":"); json_str(j, c.hostname);
    fprintf(j, ",\"lang\":"); json_str(j, c.lang);
    fprintf(j, ",\"locale\":"); json_str(j, c.locale);
    fprintf(j, ",\"timezone\":"); json_str(j, c.timezone);
    fprintf(j, ",\"kblayout\":"); json_str(j, c.keyboard);
    fprintf(j, ",\"kbvariant\":"); json_str(j, c.keyboard_variant);
    fprintf(j, ",\"swap\":%ld", c.swap);
    if (!strcmp(c.mode, "free")) fprintf(j, ",\"freeStart\":%llu,\"freeEnd\":%llu", fs, fe);
    fprintf(j, "}");
    fclose(j);

    printf("lordos-auto: installing; this takes a few minutes\n");
    install_set_unattended(1);
    AdminReq req = { .id = 1, .user = "root", .home = "/root", .uid = 0, .gid = 0, .admin = 1 };
    install_act_run(&req, json, jl);
    explicit_bzero(json, jl);
    free(json);
    explicit_bzero(&c.password_hash, sizeof c.password_hash);

    int ok = !strcmp(last_kind, "OK");
    FILE *f = fopen(RESULT, "w");
    if (f) { fprintf(f, "%s %s\n", ok ? "DONE" : "FAILED", last_line); fclose(f); }
    if (!ok) { printf("lordos-auto: FAILED %s\n", last_line); return 1; }
    printf("lordos-auto: DONE, LordOS is installed on /dev/%s\n", c.disk);
    sync();
    if (!strcmp(c.after, "stay")) return 0;
    printf("lordos-auto: %s in 10 seconds\n", !strcmp(c.after, "reboot") ? "restarting" : "powering off");
    sleep(10);
    sync();
    const char *const a[] = { "/usr/bin/systemctl", !strcmp(c.after, "reboot") ? "reboot" : "poweroff", NULL };
    if (admin_run(a)) reboot(!strcmp(c.after, "reboot") ? RB_AUTOBOOT : RB_POWER_OFF);
    return 0;
}
#endif
