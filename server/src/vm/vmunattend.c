/* vmunattend.c — see vmunattend.h. */
#include "vmunattend.h"
#include "../sec/json.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- the spec ---- */

static int word_of(const char *s, size_t max, const char *extra, int first_alpha) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n >= max) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || (extra && strchr(extra, c)))) return 0;
    }
    return !first_alpha || isalpha((unsigned char)s[0]) || s[0] == '_';
}

static int user_ok(const char *s) {
    static const char *const sys[] = { "root", "daemon", "bin", "sys", "sync", "games", "man", "lp", "mail", "news", "uucp",
        "proxy", "www-data", "backup", "list", "irc", "nobody", "systemd-network", "messagebus", "admin", "sudo", "wheel", "adm", NULL };
    if (!word_of(s, 33, "_-", 1)) return 0;
    for (const char *p = s; *p; p++) if (isupper((unsigned char)*p)) return 0;
    for (int i = 0; sys[i]; i++) if (!strcmp(s, sys[i])) return 0;
    return 1;
}

/* an xkb description: letters (UTF-8 too), digits, space and ( ) , . ' - / */
static int keydesc_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n >= 96) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) continue;
        if (!(isalnum(c) || strchr(" (),.'-/", c))) return 0;
    }
    return 1;
}

/* letters (any UTF-8 letter byte), digits, space and . ' - ; no quotes, backslashes, control characters */
static int fullname_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n >= 65) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) continue;
        if (!(isalnum(c) || c == ' ' || c == '.' || c == '-' || c == '\'')) return 0;
    }
    return 1;
}

static int hash_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 20 || n >= 128 || s[0] != '$') return 0;
    int dollars = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '$') dollars++;
        else if (!(isalnum((unsigned char)c) || c == '.' || c == '/' || c == '=')) return 0;   /* = in rounds=N */
    }
    return dollars >= 3;
}

static int hostname_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > 63 || s[0] == '-' || s[n - 1] == '-') return 0;
    for (size_t i = 0; i < n; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '-')) return 0;
    return 1;
}

/* a path inside the ISO: plain names joined by '/', no "..", not absolute */
static int isopath_ok(const char *s) {
    if (!word_of(s, 128, "._/-+", 0) || s[0] == '/' || strstr(s, "..") || strstr(s, "//")) return 0;
    return s[strlen(s) - 1] != '/';
}

/* the kernel command line: printable ASCII without quotes, backslashes or shell-ish characters */
static int append_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n >= 512) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7e || strchr("\"'\\`$;&|<>", c)) return 0;
    }
    return 1;
}

#define CP(field, key) do { const char *v_ = json_str(r, key); snprintf(u->field, sizeof u->field, "%s", v_ ? v_ : ""); } while (0)

int vmu_parse(const char *json, size_t n, VmUnattend *u, char *err, size_t errlen) {
    memset(u, 0, sizeof *u);
    JDoc d;
    if (json_parse(json, n, &d, NULL, 0)) { snprintf(err, errlen, "the unattended spec isn't JSON"); return -1; }
    const JVal *r = d.root;
    const char *k = json_str(r, "kind");
    const char *bad = NULL;
    u->kind = !k ? 0 : !strcmp(k, "preseed") ? VMU_PRESEED : !strcmp(k, "autoinstall") ? VMU_AUTOINSTALL : !strcmp(k, "kickstart") ? VMU_KICKSTART : 0;
    /* the strings must fit their fields: a longer one is refused, not cut */
    static const char *const keys[] = { "user", "fullname", "hash", "hostname", "timezone", "keymap", "variant", "locale", "release", "arch", "kernel", "initrd", "append", "keydesc" };
    static const size_t caps[] = { 33, 65, 128, 64, 64, 32, 32, 32, 16, 8, 128, 128, 512, 96 };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0] && !bad; i++) { const char *v = json_str(r, keys[i]); if (v && strlen(v) >= caps[i]) bad = keys[i]; }
    CP(user, "user"); CP(fullname, "fullname"); CP(hash, "hash"); CP(hostname, "hostname"); CP(timezone, "timezone");
    CP(keymap, "keymap"); CP(variant, "variant"); CP(locale, "locale"); CP(release, "release"); CP(arch, "arch");
    CP(kernel, "kernel"); CP(initrd, "initrd"); CP(append, "append"); CP(keydesc, "keydesc");
    u->desktop = json_bool(r, "desktop", 1);
    json_free(&d);
    if (bad) {}
    else if (!u->kind) bad = "kind";
    else if (!user_ok(u->user)) bad = "user";
    else if (!fullname_ok(u->fullname)) bad = "fullname";
    else if (!hash_ok(u->hash)) bad = "hash";
    else if (!hostname_ok(u->hostname)) bad = "hostname";
    else if (!word_of(u->timezone, sizeof u->timezone, "/_+-", 0)) bad = "timezone";
    else if (!word_of(u->keymap, sizeof u->keymap, "_-", 0)) bad = "keymap";
    else if (u->variant[0] && !word_of(u->variant, sizeof u->variant, "_-", 0)) bad = "variant";
    else if (!keydesc_ok(u->keydesc)) bad = "keydesc";
    else if (!word_of(u->locale, sizeof u->locale, "._@-", 0)) bad = "locale";
    else if (strcmp(u->arch, "amd64") && strcmp(u->arch, "arm64")) bad = "arch";
    else if (u->kind == VMU_KICKSTART && !word_of(u->release, sizeof u->release, ".", 0)) bad = "release";
    else if (!isopath_ok(u->kernel)) bad = "kernel";
    else if (!isopath_ok(u->initrd)) bad = "initrd";
    else if (!append_ok(u->append)) bad = "append";
    if (bad) { snprintf(err, errlen, "the unattended spec's %s isn't valid", bad); memset(u, 0, sizeof *u); return -1; }
    /* an installer wants a real locale: the host's C.UTF-8 becomes en_US.UTF-8 */
    if (!strncmp(u->locale, "C.", 2) || !strcmp(u->locale, "C") || !strcmp(u->locale, "POSIX")) snprintf(u->locale, sizeof u->locale, "en_US.UTF-8");
    return 0;
}

/* ---- the answer files ---- */

typedef struct { char *p; size_t n, cap; int oom; } Buf;
static void bput(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bput(Buf *b, const char *fmt, ...) {
    if (b->oom) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0) { b->oom = 1; return; }
        if (b->p && b->n + (size_t)k < b->cap) { b->n += (size_t)k; return; }
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + (size_t)k + 1) cap *= 2;
        char *q = realloc(b->p, cap);
        if (!q) { b->oom = 1; return; }
        b->p = q;
        b->cap = cap;
    }
}

static char *preseed(const VmUnattend *u) {
    Buf b = { 0 };
    bput(&b, "#_preseed_V1\n# Made by Machines (LordOS) for an unattended install: the account's password is only a hash here.\n");
    bput(&b, "d-i debian-installer/locale string %s\n", u->locale);
    bput(&b, "d-i keyboard-configuration/xkb-keymap select %s\n", u->keymap);
    /* the installed system's keyboard-configuration asks for a layout again when only the keymap is given (seen with
     * the desktop task): its codes too */
    bput(&b, "d-i keyboard-configuration/layoutcode string %s\nd-i keyboard-configuration/variantcode string %s\n"
             "d-i keyboard-configuration/modelcode string pc105\n", u->keymap, u->variant);
    /* and with the desktop task the installed system's keyboard-configuration still asks for the layout and its
     * variant, which take xkb's description: the host's, for the same layout */
    if (u->keydesc[0]) {
        bput(&b, "keyboard-configuration keyboard-configuration/layout select %s\n", u->keydesc);
        if (!u->variant[0]) bput(&b, "keyboard-configuration keyboard-configuration/variant select %s\n", u->keydesc);
    }
    bput(&b, "d-i netcfg/choose_interface select auto\n");
    bput(&b, "d-i netcfg/get_hostname string %s\nd-i netcfg/hostname string %s\nd-i netcfg/get_domain string\n", u->hostname, u->hostname);
    bput(&b, "d-i mirror/country string manual\nd-i mirror/http/hostname string deb.debian.org\nd-i mirror/http/directory string /debian\n"
             "d-i mirror/http/proxy string\n");
    bput(&b, "d-i passwd/root-login boolean false\n");
    bput(&b, "d-i passwd/user-fullname string %s\nd-i passwd/username string %s\n", u->fullname[0] ? u->fullname : u->user, u->user);
    bput(&b, "d-i passwd/user-password-crypted password %s\n", u->hash);
    bput(&b, "d-i clock-setup/utc boolean true\nd-i time/zone string %s\nd-i clock-setup/ntp boolean true\n", u->timezone);
    bput(&b, "d-i partman-auto/method string regular\nd-i partman-auto/choose_recipe select atomic\n"
             "d-i partman-partitioning/confirm_write_new_label boolean true\nd-i partman/choose_partition select finish\n"
             "d-i partman/confirm boolean true\nd-i partman/confirm_nooverwrite boolean true\n");
    bput(&b, "d-i apt-setup/cdrom/set-first boolean false\nd-i apt-setup/services-select multiselect security, updates\n");
    bput(&b, "tasksel tasksel/first multiselect %s\n", u->desktop ? "standard, desktop, gnome-desktop" : "standard");
    /* the guest agent (Shut Down, clock, clipboard) and something that honours the power button without a desktop */
    bput(&b, "d-i pkgsel/include string qemu-guest-agent%s\n", u->desktop ? "" : " acpid acpi-support-base");
    bput(&b, "d-i pkgsel/upgrade select none\npopularity-contest popularity-contest/participate boolean false\n");
    bput(&b, "d-i grub-installer/only_debian boolean true\nd-i grub-installer/bootdev string default\n");
    bput(&b, "d-i finish-install/reboot_in_progress note\n");
    if (b.oom) { free(b.p); return NULL; }
    return b.p;
}

static char *autoinstall(const VmUnattend *u) {
    Buf b = { 0 };
    bput(&b, "#cloud-config\n# Made by Machines (LordOS) for an unattended install: the account's password is only a hash here.\n");
    bput(&b, "autoinstall:\n  version: 1\n  locale: \"%s\"\n  keyboard:\n    layout: \"%s\"\n    variant: \"%s\"\n", u->locale, u->keymap, u->variant);
    bput(&b, "  timezone: \"%s\"\n", u->timezone);
    bput(&b, "  identity:\n    hostname: \"%s\"\n    realname: \"%s\"\n    username: \"%s\"\n    password: \"%s\"\n",
         u->hostname, u->fullname[0] ? u->fullname : u->user, u->user, u->hash);
    bput(&b, "  ssh:\n    install-server: false\n  storage:\n    layout:\n      name: direct\n");
    /* the desktop from a server image: its firefox package is a transitional one whose maintainer script installs the
     * snap, which fails in the installer's target (no snapd running there) and stops the whole install; kept out */
    if (u->desktop) bput(&b, "  apt:\n    preferences:\n      - package: \"firefox\"\n        pin: \"release o=Ubuntu\"\n        pin-priority: -1\n");
    bput(&b, "  packages:\n    - qemu-guest-agent\n%s", u->desktop ? "    - ubuntu-desktop-minimal\n" : "");
    bput(&b, "  shutdown: reboot\n");
    if (b.oom) { free(b.p); return NULL; }
    return b.p;
}

static char *kickstart(const VmUnattend *u) {
    Buf b = { 0 };
    const char *a = !strcmp(u->arch, "arm64") ? "aarch64" : "x86_64";
    bput(&b, "# Made by Machines (LordOS) for an unattended install: the account's password is only a hash here.\n");
    bput(&b, "text\nlang %s\n", u->locale);
    bput(&b, "keyboard --xlayouts='%s%s%s%s'\n", u->keymap, u->variant[0] ? " (" : "", u->variant, u->variant[0] ? ")" : "");
    bput(&b, "timezone %s --utc\n", u->timezone);
    bput(&b, "network --bootproto=dhcp --hostname=%s --activate\n", u->hostname);
    bput(&b, "url --metalink=https://mirrors.fedoraproject.org/metalink?repo=fedora-%s&arch=%s\n", u->release, a);
    bput(&b, "repo --name=updates --metalink=https://mirrors.fedoraproject.org/metalink?repo=updates-released-f%s&arch=%s\n", u->release, a);
    bput(&b, "rootpw --lock\n");
    bput(&b, "user --name=%s --gecos=\"%s\" --groups=wheel --iscrypted --password=%s\n", u->user, u->fullname[0] ? u->fullname : u->user, u->hash);
    bput(&b, "zerombr\nclearpart --all --initlabel\nautopart\nbootloader\nfirstboot --disable\nreboot\n");
    bput(&b, "%%packages\n%s\nqemu-guest-agent\n%%end\n", u->desktop ? "@^workstation-product-environment" : "@^minimal-environment");
    if (b.oom) { free(b.p); return NULL; }
    return b.p;
}

char *vmu_answer(const VmUnattend *u, const char **name) {
    switch (u->kind) {
    case VMU_PRESEED: *name = "preseed.cfg"; return preseed(u);
    case VMU_AUTOINSTALL: *name = "user-data"; return autoinstall(u);
    case VMU_KICKSTART: *name = "ks.cfg"; return kickstart(u);
    }
    *name = NULL;
    return NULL;
}

char *vmu_meta_data(const VmUnattend *u, const char *instance) {
    Buf b = { 0 };
    bput(&b, "instance-id: %s\nlocal-hostname: %s\n", instance && word_of(instance, 64, "-", 0) ? instance : "lordos-machine", u->hostname);
    if (b.oom) { free(b.p); return NULL; }
    return b.p;
}

int vmu_cmdline(const VmUnattend *u, const char *label, char *out, size_t n) {
    size_t o = 0;
    for (const char *s = u->append; *s;) {
        const char *add;
        size_t al;
        if (!strncmp(s, "{label}", 7)) {
            if (!label || !word_of(label, 33, "_-.", 0)) return -1;
            add = label; al = strlen(label); s += 7;
        } else { add = s; al = 1; s++; }
        if (o + al >= n) return -1;
        memcpy(out + o, add, al);
        o += al;
    }
    out[o] = 0;
    return 0;
}

/* ---- FAT12 ---- */

#define FAT_SECTORS 4096
#define FAT_LEN     12
#define FAT_ROOT    64
#define FAT_DATA    (1 + 2 * FAT_LEN + FAT_ROOT * 32 / 512)

static void le16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void fat12_set(uint8_t *fat, unsigned n, unsigned v) {
    unsigned off = n * 3 / 2;
    if (n & 1) { fat[off] = (uint8_t)((fat[off] & 0x0F) | ((v << 4) & 0xF0)); fat[off + 1] = (uint8_t)(v >> 4); }
    else { fat[off] = (uint8_t)v; fat[off + 1] = (uint8_t)((fat[off + 1] & 0xF0) | ((v >> 8) & 0x0F)); }
}

int vmu_fat_image(const char *label, const VmuFile *files, int n, uint8_t **img, size_t *len) {
    *img = NULL;
    *len = 0;
    size_t ll = label ? strlen(label) : 0;
    if (!ll || ll > 11 || n < 0 || n > 8) return -1;
    for (size_t i = 0; i < ll; i++) if (!(isupper((unsigned char)label[i]) || isdigit((unsigned char)label[i]) || label[i] == '_' || label[i] == '-')) return -1;
    size_t total = (size_t)FAT_SECTORS * 512;
    uint8_t *d = calloc(1, total);
    if (!d) return -1;
    static const uint8_t jmp[3] = { 0xEB, 0x3C, 0x90 };
    memcpy(d, jmp, 3);
    memcpy(d + 3, "LORDOS  ", 8);
    le16(d + 11, 512); d[13] = 1; le16(d + 14, 1); d[16] = 2; le16(d + 17, FAT_ROOT); le16(d + 19, FAT_SECTORS);
    d[21] = 0xF8; le16(d + 22, FAT_LEN); le16(d + 24, 32); le16(d + 26, 64);
    d[36] = 0x80; d[38] = 0x29; le32(d + 39, 0x4C4F5244u);
    memset(d + 43, ' ', 11); memcpy(d + 43, label, ll);
    memcpy(d + 54, "FAT12   ", 8);
    d[510] = 0x55; d[511] = 0xAA;
    uint8_t *fat = d + 512, *root = d + 512 * (1 + 2 * FAT_LEN);
    fat12_set(fat, 0, 0xFF8); fat12_set(fat, 1, 0xFFF);
    int e = 0;
    memset(root, ' ', 11); memcpy(root, label, ll); root[11] = 0x08; e++;
    unsigned next = 2, clusters = FAT_SECTORS - FAT_DATA;
    for (int f = 0; f < n; f++) {
        const char *name = files[f].name;
        size_t nl = name ? strlen(name) : 0;
        if (!nl || nl > 64 || files[f].len > (1u << 20)) { free(d); return -1; }
        for (size_t i = 0; i < nl; i++) if ((unsigned char)name[i] < 0x21 || (unsigned char)name[i] > 0x7e || strchr("/\\:*?\"<>|", name[i])) { free(d); return -1; }
        uint8_t sn[11];
        char tmp[24];
        snprintf(tmp, sizeof tmp, "SEED%04u   ", (unsigned)f % 10000u);
        memcpy(sn, tmp, 11);
        uint8_t sum = 0;
        for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sn[i]);
        int parts = (int)((nl + 12) / 13);
        if (e + parts + 1 > FAT_ROOT) { free(d); return -1; }
        for (int pidx = parts; pidx >= 1; pidx--) {
            uint8_t *r = root + 32 * e++;
            r[0] = (uint8_t)(pidx | (pidx == parts ? 0x40 : 0));
            r[11] = 0x0F; r[12] = 0; r[13] = sum; le16(r + 26, 0);
            static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int c = 0; c < 13; c++) {
                size_t idx = (size_t)(pidx - 1) * 13 + (size_t)c;
                unsigned ch = idx < nl ? (unsigned char)name[idx] : idx == nl ? 0x0000 : 0xFFFF;
                le16(r + pos[c], ch);
            }
        }
        uint8_t *r = root + 32 * e++;
        memcpy(r, sn, 11);
        r[11] = 0x20;
        le16(r + 14, 0); le16(r + 16, (unsigned)((46 << 9) | (11 << 5) | 6)); le16(r + 18, (unsigned)((46 << 9) | (11 << 5) | 6));
        le16(r + 22, 0); le16(r + 24, (unsigned)((46 << 9) | (11 << 5) | 6));
        size_t need = (files[f].len + 511) / 512;
        if (!need) { le16(r + 26, 0); le32(r + 28, 0); continue; }
        if (next - 2 + need > clusters) { free(d); return -1; }
        le16(r + 26, next);
        le32(r + 28, (uint32_t)files[f].len);
        memcpy(d + (size_t)(FAT_DATA + next - 2) * 512, files[f].data, files[f].len);
        for (size_t c = 0; c < need; c++) fat12_set(fat, (unsigned)(next + c), c + 1 == need ? 0xFFF : (unsigned)(next + c + 1));
        next += (unsigned)need;
    }
    memcpy(d + 512 * (1 + FAT_LEN), fat, 512 * FAT_LEN);    /* the second FAT */
    *img = d;
    *len = total;
    return 0;
}

/* ---- cpio (newc) ---- */

int vmu_cpio(const VmuFile *files, int n, uint8_t **out, size_t *len) {
    Buf b = { 0 };
    for (int i = 0; i <= n; i++) {
        const char *name = i < n ? files[i].name : "TRAILER!!!";
        size_t nl = strlen(name) + 1, sz = i < n ? files[i].len : 0;
        if (nl > 255 || strchr(name, '/') || sz > (64u << 20)) { free(b.p); return -1; }
        bput(&b, "070701%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X", (unsigned)(i + 1), i < n ? 0100644u : 0u, 0u, 0u,
             1u, 0u, (unsigned)sz, 0u, 0u, 0u, 0u, (unsigned)nl, 0u);
        if (b.oom) break;
        /* the name and its NUL, then padding to 4; the data, then padding to 4 */
        size_t need = b.n + nl + 3 + sz + 3 + 1;
        if (need > b.cap) { char *q = realloc(b.p, need); if (!q) { b.oom = 1; break; } b.p = q; b.cap = need; }
        memcpy(b.p + b.n, name, nl); b.n += nl;
        while (b.n % 4) b.p[b.n++] = 0;
        if (sz) { memcpy(b.p + b.n, files[i].data, sz); b.n += sz; }
        while (b.n % 4) b.p[b.n++] = 0;
    }
    if (b.oom) { free(b.p); return -1; }
    *out = (uint8_t *)b.p;
    *len = b.n;
    return 0;
}

/* ---- ISO 9660 ---- */

static int rd(int fd, uint64_t off, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (char *)buf + got, n - got, (off_t)(off + got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}
static uint32_t gle32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int vmu_iso_label(int fd, char out[33]) {
    uint8_t s[2048];
    out[0] = 0;
    if (rd(fd, 16 * 2048, s, sizeof s) || s[0] != 1 || memcmp(s + 1, "CD001", 5)) return -1;
    memcpy(out, s + 40, 32);
    out[32] = 0;
    for (int i = 31; i >= 0 && (out[i] == ' ' || out[i] == 0); i--) out[i] = 0;
    for (int i = 0; out[i]; i++) if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e) { out[0] = 0; return -1; }
    return 0;
}

/* a record's name as this tree spells it: Rock Ridge NM, Joliet's UCS-2, or the plain name without ";1" and a final '.' */
static void rec_name(const uint8_t *r, size_t len, int joliet, char *out, size_t on, int *plain) {
    size_t nl = r[32];
    const uint8_t *nm = r + 33;
    out[0] = 0;
    *plain = 0;
    if (!joliet) {
        size_t su = 33 + nl + (nl % 2 ? 0 : 1), o = 0;
        int got = 0;
        while (su + 4 <= len) {
            const uint8_t *e = r + su;
            size_t el = e[2];
            if (el < 4 || su + el > len) break;
            if (e[0] == 'N' && e[1] == 'M' && el >= 5 && !(e[4] & 6)) {
                for (size_t i = 5; i < el && o + 1 < on; i++) out[o++] = (char)e[i];
                out[o] = 0;
                got = 1;
            }
            su += el;
        }
        if (got) return;
    }
    size_t o = 0;
    if (joliet) for (size_t i = 0; i + 1 < nl && o + 1 < on; i += 2) out[o++] = nm[i] ? '?' : (char)(nm[i + 1] ? nm[i + 1] : '?');
    else { for (size_t i = 0; i < nl && o + 1 < on; i++) out[o++] = (char)nm[i]; *plain = 1; }
    out[o] = 0;
    char *semi = strchr(out, ';');
    if (semi) *semi = 0;
    size_t l = strlen(out);
    if (l && out[l - 1] == '.') out[l - 1] = 0;
}

static int find_in(int fd, uint32_t extent, uint32_t size, const char *path, int joliet, int depth, uint64_t *off, uint64_t *fsize) {
    if (depth > 8 || size > (1u << 20) || !size) return -1;
    char comp[256];
    const char *slash = strchr(path, '/');
    size_t cl = slash ? (size_t)(slash - path) : strlen(path);
    if (!cl || cl >= sizeof comp) return -1;
    memcpy(comp, path, cl);
    comp[cl] = 0;
    uint8_t *dir = malloc(size);
    if (!dir || rd(fd, (uint64_t)extent * 2048, dir, size)) { free(dir); return -1; }
    int rc = -1;
    for (size_t pos = 0; pos < size;) {
        size_t len = dir[pos];
        if (!len) { pos = (pos / 2048 + 1) * 2048; continue; }
        if (len < 34 || pos + len > size || 33 + (size_t)dir[pos + 32] > len) break;
        const uint8_t *r = dir + pos;
        pos += len;
        if (r[32] == 1 && (r[33] == 0 || r[33] == 1)) continue;          /* . and .. */
        char name[256];
        int plain;
        rec_name(r, len, joliet, name, sizeof name, &plain);
        if (plain ? strcasecmp(name, comp) : strcmp(name, comp)) continue;
        int isdir = (r[25] & 2) != 0;
        if (r[25] & 0x80) break;                                          /* multi-extent */
        if (slash) { if (isdir) rc = find_in(fd, gle32(r + 2), gle32(r + 10), slash + 1, joliet, depth + 1, off, fsize); }
        else if (!isdir) { *off = (uint64_t)gle32(r + 2) * 2048; *fsize = gle32(r + 10); rc = 0; }
        break;
    }
    free(dir);
    return rc;
}

int vmu_iso_find(int fd, const char *path, uint64_t *off, uint64_t *size) {
    if (!isopath_ok(path)) return -1;
    struct stat st;
    if (fstat(fd, &st)) return -1;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 16; i < 64; i++) {
            uint8_t s[2048];
            if (rd(fd, (uint64_t)i * 2048, s, sizeof s) || memcmp(s + 1, "CD001", 5) || s[0] == 255) break;
            int joliet = s[0] == 2 && s[88] == 0x25 && s[89] == 0x2F && (s[90] == 0x40 || s[90] == 0x43 || s[90] == 0x45);
            if ((pass == 0 && s[0] != 1) || (pass == 1 && !joliet)) continue;
            const uint8_t *root = s + 156;
            if (!find_in(fd, gle32(root + 2), gle32(root + 10), path, joliet, 0, off, size) &&
                *off + *size <= (uint64_t)st.st_size) return 0;
        }
    }
    return -1;
}

/* ---- the files of the first start ---- */

static int write_file(const char *path, const void *data, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    size_t o = 0;
    while (o < n) {
        ssize_t w = write(fd, (const char *)data + o, n - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { close(fd); return -1; }
        o += (size_t)w;
    }
    return close(fd);
}

/* copies `size` bytes at `off` of the ISO into path; with `pad4` the copy is padded to a multiple of 4 (an initrd
 * another archive goes after) */
static int copy_out(int iso, uint64_t off, uint64_t size, const char *path, int pad4) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    char *buf = malloc(1 << 20);
    int rc = buf ? 0 : -1;
    for (uint64_t o = 0; !rc && o < size;) {
        size_t k = size - o > (1u << 20) ? (1u << 20) : (size_t)(size - o);
        if (rd(iso, off + o, buf, k) || write(fd, buf, k) != (ssize_t)k) rc = -1;
        o += k;
    }
    if (!rc && pad4 && size % 4) { static const char z[4]; if (write(fd, z, 4 - size % 4) != (ssize_t)(4 - size % 4)) rc = -1; }
    free(buf);
    if (close(fd)) rc = -1;
    return rc;
}

static int append_file(const char *path, const void *data, size_t n) {
    int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    int rc = write(fd, data, n) == (ssize_t)n ? 0 : -1;
    if (close(fd)) rc = -1;
    return rc;
}

static void remove_dir(const char *dir) {
    static const char *const names[] = { "vmlinuz", "initrd", "cmdline", "seed.img", NULL };
    char p[4200];
    for (int i = 0; names[i]; i++) { snprintf(p, sizeof p, "%s/%s", dir, names[i]); unlink(p); }
    rmdir(dir);
}

int vmu_prepare(const char *disks, const char *iso, const VmUnattend *u, const char *instance, char *err, size_t errlen) {
    char dir[4096], p[4200];
    if (snprintf(dir, sizeof dir, "%s/unattended", disks) >= (int)sizeof dir) { snprintf(err, errlen, "path too long"); return -1; }
    if (mkdir(dir, 0700) && errno != EEXIST) { snprintf(err, errlen, "can't make %s: %s", dir, strerror(errno)); return -1; }
    int fd = open(iso, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { snprintf(err, errlen, "can't read %s: %s", iso, strerror(errno)); remove_dir(dir); return -1; }
    char label[33];
    uint64_t ko, ks, io, is;
    const char *answer_name = NULL;
    char *answer = NULL, *meta = NULL;
    uint8_t *extra = NULL;
    size_t extra_len = 0;
    int rc = -1;
    char cmd[640];
    if (vmu_iso_label(fd, label)) snprintf(err, errlen, "%s isn't an ISO 9660 image", iso);
    else if (vmu_iso_find(fd, u->kernel, &ko, &ks) || ks > (64u << 20) || ks < 4096) snprintf(err, errlen, "the ISO has no installer kernel at %s", u->kernel);
    else if (vmu_iso_find(fd, u->initrd, &io, &is) || is > (512u << 20) || is < 4096) snprintf(err, errlen, "the ISO has no installer initrd at %s", u->initrd);
    else if (vmu_cmdline(u, label, cmd, sizeof cmd)) snprintf(err, errlen, "the kernel command line doesn't fit (or the ISO's label isn't a plain word)");
    else if (!(answer = vmu_answer(u, &answer_name))) snprintf(err, errlen, "out of memory");
    else {
        VmuFile f[2] = { { answer_name, answer, strlen(answer) }, { NULL, NULL, 0 } };
        int nf = 1;
        if (u->kind == VMU_AUTOINSTALL) {
            meta = vmu_meta_data(u, instance);
            f[1] = (VmuFile){ "meta-data", meta, meta ? strlen(meta) : 0 };
            nf = meta ? 2 : 0;
        }
        snprintf(p, sizeof p, "%s/vmlinuz", dir);
        if (!nf) snprintf(err, errlen, "out of memory");
        else if (copy_out(fd, ko, ks, p, 0)) snprintf(err, errlen, "can't copy the installer's kernel: %s", strerror(errno));
        else if (snprintf(p, sizeof p, "%s/initrd", dir), copy_out(fd, io, is, p, u->kind == VMU_PRESEED)) snprintf(err, errlen, "can't copy the installer's initrd: %s", strerror(errno));
        else if (u->kind == VMU_PRESEED) {
            if (vmu_cpio(f, 1, &extra, &extra_len) || append_file(p, extra, extra_len)) snprintf(err, errlen, "can't add the answers to the initrd");
            else rc = 0;
        } else {
            if (vmu_fat_image(u->kind == VMU_AUTOINSTALL ? "CIDATA" : "OEMDRV", f, nf, &extra, &extra_len)) snprintf(err, errlen, "can't make the seed disk");
            else if (snprintf(p, sizeof p, "%s/seed.img", dir), write_file(p, extra, extra_len)) snprintf(err, errlen, "can't write the seed disk: %s", strerror(errno));
            else rc = 0;
        }
        if (!rc) {
            snprintf(p, sizeof p, "%s/cmdline", dir);
            if (write_file(p, cmd, strlen(cmd))) { snprintf(err, errlen, "can't write the kernel command line"); rc = -1; }
        }
    }
    close(fd);
    if (answer) { explicit_bzero(answer, strlen(answer)); free(answer); }
    free(meta);
    free(extra);
    if (rc) remove_dir(dir);
    return rc;
}
