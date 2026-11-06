/* install.c — LordOS installer to disk (privileged helper actions, admin.c).
 *
 * It only works on the live ISO (/run/live/medium exists); on an installed system every
 * action answers ERR. Actions:
 *
 *   install-disks           one LOG with JSON per installable disk and finally OK <n>:
 *                           { name, path, size, model, vendor, transport, removable, parts: [...] }
 *                           (no loop, ram, zram, sr, dm or md, and not the disk the live system booted from)
 *                           and, read-only: ptable, esp, espPart, freeStart, freeEnd, freeBytes (the
 *                           largest free GPT region), freeOk, freeWhy (an error code when not freeOk);
 *                           for manual partitioning lsec, first, last, tableTag, manualOk, manualWhy,
 *                           and per partition num, start, end, esp, fs and (ESPs) loaders
 *   install-run <json>      installs (in a separate process). Mode "erase": wipes the disk, GPT with
 *                           BIOS boot (1 MiB, x86-64 only), ESP (512 MiB, FAT32) and ext4 root; copies
 *                           the live system (unsquashfs), configures it and installs grub: UEFI and BIOS
 *                           on x86-64, UEFI only on arm64. Mode "free": see freepart.c: one new ext4
 *                           partition in free space of a GPT disk, grub in EFI/LordOS on its ESP.
 *                           Mode "manual": see manualpart.c: the partitions the person chose on a
 *                           GPT disk (delete, create, which is /, /home, swap and the ESP).
 *                           Progress: "LOG progress <phase> <done> <total>", phases partition,
 *                           copy, configure, bootloader.
 *   install-account <json>  the OVA image's first boot: same plan without disk, mode and swap. It only
 *                           works while /etc/lordos/first-account exists (tools/iso/build-ova.sh), and
 *                           does the install-run steps that make the person's account on the running
 *                           system; the setup account goes at the next boot (lordos-live-prep)
 *   install-reboot          reboots (only on the live system or after install-account, to finish the wizard)
 *
 * The unattended install (lord-autoinstall, autoinstall.c, docs/UNATTENDED.md) calls install-run's own code with
 * install_set_unattended(1) and three more fields, refused from anywhere else: passwordHash (a crypt(3) hash
 * the maintainer made, instead of password), sshKey (one authorized_keys line for the user) and autologin.
 *
 * The install-run JSON: { mode ("erase", the default, or "free"), disk, user, fullname, password,
 * hostname, lang, locale, timezone, kblayout, kbvariant, swap (MiB, 0 = no swap), and in mode free
 * freeStart, freeEnd (the region install-disks reported, which has to still be it), and in mode
 * manual tableTag (install-disks' one, which has to still be it), delete ("2,5"), create
 * ("root:S:E,home:S:E,swap:S:E,esp:S:E", sectors), rootPart, homePart, swapPart, espPart (existing
 * partition numbers), homeFormat, espFormat }. Every field is validated here (root), without
 * trusting the UI: the disk has to be in the install-disks list, names
 * pass strict expressions, the region has to be in SUPPORTED, the zone in zoneinfo and
 * the layout in xkb. The password never goes into an argv: it's hashed here with crypt(3)
 * (yescrypt, like Debian) and written to the new system's /etc/shadow. Every program
 * has a fixed path and its arguments are built by this file. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "install.h"
#include "freepart.h"
#include "manualpart.h"
#include "../users/homedirs.h"

#include <crypt.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define LIVE_MEDIUM "/run/live/medium"
#define SQUASHFS    LIVE_MEDIUM "/live/filesystem.squashfs"
#define TARGET      "/run/lordos-install/target"
#define MIN_DISK    FSP_MIN_BYTES              /* 8 GiB */
/* The OVA's first boot: pending, then done (lordos-live-prep removes the setup account and this file) */
#define FIRST_ACCOUNT       "/etc/lordos/first-account"
#define FIRST_ACCOUNT_DONE  "/etc/lordos/first-account.done"

/* What the architecture decides. x86-64: a BIOS boot partition and grub for both firmwares; arm64 is
 * UEFI only. The root partition's GPT type is the architecture's own (Linux root, x86-64 or ARM64). */
#if defined(__x86_64__)
#define INSTALL_BIOS     1
#define GRUB_EFI_TARGET  "--target=x86_64-efi"
#define GRUB_EFI_FILE    "grubx64.efi"
#elif defined(__aarch64__)
#define INSTALL_BIOS     0
#define GRUB_EFI_TARGET  "--target=arm64-efi"
#define GRUB_EFI_FILE    "grubaa64.efi"
#else
#error "the disk installer knows x86-64 and arm64"
#endif

static int is_live(void) { return access(SQUASHFS, R_OK) == 0; }
static int is_first_account(void) { return access(FIRST_ACCOUNT, F_OK) == 0; }

/* ---- utilities ---- */

/* Bounded copy (without snprintf("%s"), which gcc flags with -Wformat-truncation). */
static void cpy(char *d, size_t n, const char *s) {
    size_t l = strnlen(s, n - 1);
    memcpy(d, s, l);
    d[l] = 0;
}

static int read_line(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (!f) return -1;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    size_t l = strlen(out);                     /* no trailing spaces (model from /sys) */
    while (l && out[l - 1] == ' ') out[--l] = 0;
    return 0;
}

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static int write_file(const char *path, const char *text, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.lordos-tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    size_t n = strlen(text), off = 0;
    while (off < n) { ssize_t w = write(fd, text + off, n - off); if (w <= 0) { close(fd); unlink(tmp); return -1; } off += (size_t)w; }
    fchmod(fd, mode);
    if (close(fd) || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

static int writef(const char *path, mode_t mode, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int writef(const char *path, mode_t mode, const char *fmt, ...) {
    char *buf = NULL;
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(&buf, fmt, ap);
    va_end(ap);
    if (r < 0) return -1;
    r = write_file(path, buf, mode);
    free(buf);
    return r;
}

/* Disk (sda, nvme0n1…) holding the live medium, or "". */
static void live_disk(char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen("/proc/self/mounts", "r");
    char dev[256], mnt[256], line[1024];
    while (f && fgets(line, sizeof line, f)) {
        if (sscanf(line, "%255s %255s", dev, mnt) == 2 && !strcmp(mnt, LIVE_MEDIUM)) {
            char real[PATH_MAX], sys[PATH_MAX], rs[PATH_MAX];
            if (!realpath(dev, real)) break;
            const char *b = strrchr(real, '/') ? strrchr(real, '/') + 1 : real;
            int w = snprintf(sys, sizeof sys, "/sys/class/block/%s", b);
            if (w < 0 || (size_t)w >= sizeof sys || !realpath(sys, rs)) { cpy(out, n, b); break; }
            char part[PATH_MAX + 16];
            w = snprintf(part, sizeof part, "%s/partition", rs);
            if (w > 0 && (size_t)w < sizeof part && access(part, F_OK) == 0) { char *p = dirname(rs); cpy(out, n, strrchr(p, '/') + 1); }
            else cpy(out, n, b);
            break;
        }
    }
    if (f) fclose(f);
}

static int disk_name_ok(const char *d) {
    static const char *const skip[] = { "loop", "ram", "zram", "sr", "dm-", "md", "fd", "nbd", NULL };
    size_t n = strlen(d);
    if (!n || n > 31) return 0;
    for (const char *p = d; *p; p++) if (!isalnum((unsigned char)*p)) return 0;
    for (int i = 0; skip[i]; i++) if (!strncmp(d, skip[i], strlen(skip[i]))) return 0;
    return 1;
}

typedef struct { char name[32], model[96], vendor[64], transport[16]; unsigned long long size; int removable; } Disk;

/* Installable disks, with the live medium left out. */
static int list_disks(Disk *out, int max) {
    char live[32];
    live_disk(live, sizeof live);
    DIR *d = opendir("/sys/block");
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) && n < max) {
        if (!disk_name_ok(e->d_name) || !strcmp(e->d_name, live)) continue;
        char p[PATH_MAX], v[128], real[PATH_MAX];
        snprintf(p, sizeof p, "/sys/block/%s/device", e->d_name);
        if (access(p, F_OK)) continue;                          /* no real device */
        Disk *k = &out[n];
        memset(k, 0, sizeof *k);
        cpy(k->name, sizeof k->name, e->d_name);
        snprintf(p, sizeof p, "/sys/block/%s/size", e->d_name);
        read_line(p, v, sizeof v);
        k->size = strtoull(v, NULL, 10) * 512ull;
        if (!k->size) continue;
        snprintf(p, sizeof p, "/sys/block/%s/ro", e->d_name);
        read_line(p, v, sizeof v);
        if (v[0] == '1') continue;                              /* read-only */
        snprintf(p, sizeof p, "/sys/block/%s/removable", e->d_name);
        read_line(p, v, sizeof v);
        k->removable = v[0] == '1';
        snprintf(p, sizeof p, "/sys/block/%s/device/model", e->d_name);
        read_line(p, k->model, sizeof k->model);
        snprintf(p, sizeof p, "/sys/block/%s/device/vendor", e->d_name);
        read_line(p, k->vendor, sizeof k->vendor);
        snprintf(p, sizeof p, "/sys/block/%s", e->d_name);
        const char *t = "sata";
        if (realpath(p, real)) {
            if (strstr(real, "/usb")) t = "usb";
            else if (!strncmp(e->d_name, "nvme", 4)) t = "nvme";
            else if (!strncmp(e->d_name, "mmcblk", 6)) t = "mmc";
            else if (!strncmp(e->d_name, "vd", 2) || strstr(real, "/virtio")) t = "virtio";
        }
        cpy(k->transport, sizeof k->transport, t);
        n++;
    }
    if (d) closedir(d);
    return n;
}

static void part_path(const char *disk, int num, char *out, size_t n) {
    size_t l = strlen(disk);
    snprintf(out, n, "/dev/%s%s%d", disk, l && isdigit((unsigned char)disk[l - 1]) ? "p" : "", num);
}

/* ---- install-disks ---- */

/* Value of KEY="…" in an lsblk -P line. */
static void pair(const char *line, const char *key, char *out, size_t n) {
    out[0] = 0;
    char k[40];
    snprintf(k, sizeof k, "%s=\"", key);
    const char *p = strstr(line, k);
    if (p && (p == line || p[-1] == ' ')) {
        p += strlen(k);
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
        out[i] = 0;
    }
}

void install_act_disks(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg; (void)len;
    if (!is_live()) { admin_err("install.liveOnly", NULL); return; }
    Disk disks[32];
    int n = list_disks(disks, 32);
    for (int i = 0; i < n; i++) {
        Disk *k = &disks[i];
        char dev[48], out[16384];
        int w = snprintf(dev, sizeof dev, "/dev/%s", k->name);
        if (w < 0 || (size_t)w >= sizeof dev) continue;
        const char *const argv[] = { "/usr/bin/lsblk", "-n", "-P", "-b", "-o", "NAME,TYPE,FSTYPE,LABEL,SIZE", dev, NULL };
        if (admin_run_capture(argv, out, sizeof out) != 0) out[0] = 0;
        char *js = NULL;
        size_t jl = 0;
        FILE *f = open_memstream(&js, &jl);
        if (!f) continue;
        fprintf(f, "{\"name\":"); json_str(f, k->name);
        fprintf(f, ",\"path\":"); json_str(f, dev);
        fprintf(f, ",\"size\":%llu,\"model\":", k->size); json_str(f, k->model);
        fprintf(f, ",\"vendor\":"); json_str(f, k->vendor);
        fprintf(f, ",\"transport\":"); json_str(f, k->transport);
        fprintf(f, ",\"removable\":%s,\"usable\":%s", k->removable ? "true" : "false", k->size >= MIN_DISK ? "true" : "false");
        static MppProbe mp;
        char espname[48] = "";
        unsigned espnum = 0;
        mpp_probe(k->name, &mp);
        if (mp.fp.esps == 1) { fsp_esp_count(&mp.fp.table, &espnum); fsp_part_path(k->name, espnum, espname, sizeof espname); }
        fprintf(f, ",\"ptable\":"); json_str(f, fsp_mbr_name(mp.fp.facts.mbr));
        fprintf(f, ",\"esp\":%s,\"espPart\":", mp.fp.esps > 0 ? "true" : "false"); json_str(f, espname);
        fprintf(f, ",\"freeStart\":%llu,\"freeEnd\":%llu,\"freeBytes\":%llu,\"freeOk\":%s,\"freeWhy\":",
                (unsigned long long)mp.fp.region.start, (unsigned long long)mp.fp.region.end, (unsigned long long)mp.fp.region.bytes, mp.fp.why ? "false" : "true");
        json_str(f, mp.fp.why ? mp.fp.why : "");
        /* manual partitioning (manualpart.c): the table in sectors, and its tag for install-run */
        fprintf(f, ",\"lsec\":%u,\"first\":%llu,\"last\":%llu,\"tableTag\":", mp.fp.table.lsec,
                (unsigned long long)mp.fp.table.first, (unsigned long long)mp.fp.table.last);
        json_str(f, mp.why ? "" : mp.tag);
        fprintf(f, ",\"manualOk\":%s,\"manualWhy\":", mp.why ? "false" : "true");
        json_str(f, mp.why ? mp.why : "");
        fprintf(f, ",\"parts\":[");
        int first = 1;
        for (char *l = out, *next; l && *l; l = next) {
            next = strchr(l, '\n');
            if (next) *next++ = 0;
            char name[64], type[16], fs[32], label[64], size[32];
            pair(l, "NAME", name, sizeof name); pair(l, "TYPE", type, sizeof type);
            pair(l, "FSTYPE", fs, sizeof fs); pair(l, "LABEL", label, sizeof label); pair(l, "SIZE", size, sizeof size);
            if (strcmp(type, "part")) continue;
            fprintf(f, "%s{\"name\":", first ? "" : ","); json_str(f, name);
            fprintf(f, ",\"fstype\":"); json_str(f, fs);
            fprintf(f, ",\"label\":"); json_str(f, label);
            fprintf(f, ",\"size\":%llu", strtoull(size, NULL, 10));
            /* the table's own entry for it, when the table could be believed */
            for (int q = 0; !mp.why && q < mp.fp.table.n; q++) {
                char pp[48];
                fsp_part_path(k->name, mp.fp.table.p[q].num, pp, sizeof pp);
                if (strcmp(strrchr(pp, '/') + 1, name)) continue;
                const FspPart *tp = &mp.fp.table.p[q];
                fprintf(f, ",\"num\":%u,\"start\":%llu,\"end\":%llu,\"esp\":%s,\"fs\":", tp->num,
                        (unsigned long long)tp->start, (unsigned long long)tp->end, tp->esp ? "true" : "false");
                json_str(f, mp.facts.fstype[tp->num]);
                if (tp->esp) {
                    char names[8][33];
                    int nl = mpp_esp_loaders(pp, names, 8);
                    fprintf(f, ",\"loaders\":[");
                    for (int z = 0; z < nl; z++) { if (z) fputc(',', f); json_str(f, names[z]); }
                    fputc(']', f);
                }
            }
            fputc('}', f);
            first = 0;
        }
        fprintf(f, "]}");
        fclose(f);
        admin_say("LOG", "%s", js);
        free(js);
    }
    admin_say("OK", "%d", n);   /* i18n-ok */
}

/* ---- flat JSON of install-run ---- */

typedef struct { char mode[16], disk[32], user[33], fullname[128], password[257], hostname[64], lang[16], locale[64], timezone[64], kblayout[24], kbvariant[40]; long swap; unsigned long long free_start, free_end;
                 /* the unattended install only (install_set_unattended) */
                 char password_hash[256], ssh_key[1024]; int autologin, has_extra;
                 /* mode manual (manual.c): partitions to delete and create, the roles of existing ones */
                 char del[MP_LIST_MAX + 1], create[MP_LIST_MAX + 1], table_tag[64];
                 long root_part, home_part, swap_part, esp_part; int home_format, esp_format; } Plan;

static int unattended;
void install_set_unattended(int on) { unattended = on; }

/* A crypt(3) hash as /etc/shadow keeps it: yescrypt ($y$, Debian's default) or SHA-512 ($6$), only the
 * characters crypt uses, and one crypt itself accepts as a setting. */
int install_hash_ok(const char *h) {
    size_t n = strlen(h);
    if (n < 20 || n > 255) return 0;
    if (strncmp(h, "$y$", 3) && strncmp(h, "$6$", 3)) return 0;
    for (const char *p = h; *p; p++) if (!isalnum((unsigned char)*p) && !strchr("./$", *p)) return 0;
    struct crypt_data cd;
    memset(&cd, 0, sizeof cd);
    const char *out = crypt_r("lordos-hash-check", h, &cd);
    int ok = out && out[0] == '$' && !strncmp(out, h, 3);
    explicit_bzero(&cd, sizeof cd);
    return ok;
}

/* One authorized_keys line: a known key type, base64, an optional comment; no options, no control characters. */
int install_ssh_key_ok(const char *k) {
    static const char *const types[] = { "ssh-ed25519 ", "ssh-rsa ", "ecdsa-sha2-nistp256 ", "ecdsa-sha2-nistp384 ", "ecdsa-sha2-nistp521 ", "sk-ssh-ed25519@openssh.com ", NULL };
    size_t n = strlen(k);
    if (n < 40 || n > 1000) return 0;
    const char *b = NULL;
    for (int i = 0; types[i]; i++) if (!strncmp(k, types[i], strlen(types[i]))) b = k + strlen(types[i]);
    if (!b) return 0;
    const char *e = b;
    while (isalnum((unsigned char)*e) || *e == '+' || *e == '/' || *e == '=') e++;
    if (e - b < 16 || (*e && *e != ' ')) return 0;
    for (const char *p = k; *p; p++) if ((unsigned char)*p < 0x20 || *p == 0x7f) return 0;
    return 1;
}

/* Reads a JSON string (with escapes and \uXXXX → UTF-8). Returns the pointer past the closing quote. */
static const char *jstring(const char *p, char *out, size_t n, int *ok) {
    size_t o = 0;
    if (*p != '"') { *ok = 0; return p; }
    for (p++; *p && *p != '"'; p++) {
        unsigned cp = (unsigned char)*p;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': cp = '\n'; break; case 't': cp = '\t'; break; case 'r': cp = '\r'; break;
            case 'b': cp = '\b'; break; case 'f': cp = '\f'; break;
            case 'u': {
                char h[5] = { 0 };
                for (int i = 0; i < 4; i++) { if (!isxdigit((unsigned char)p[1 + i])) { *ok = 0; return p; } h[i] = p[1 + i]; }
                cp = (unsigned)strtoul(h, NULL, 16);
                p += 4;
                if (cp >= 0xd800 && cp < 0xe000) { *ok = 0; return p; }      /* no surrogate pairs */
                break;
            }
            case 0: *ok = 0; return p;
            default: cp = (unsigned char)*p;
            }
        }
        char u[4];
        int ul = 1;
        if (cp < 0x80 || (unsigned char)p[0] >= 0x80) u[0] = (char)cp;          /* UTF-8 bytes as-is */
        else if (cp < 0x800) { u[0] = (char)(0xc0 | cp >> 6); u[1] = (char)(0x80 | (cp & 63)); ul = 2; }
        else { u[0] = (char)(0xe0 | cp >> 12); u[1] = (char)(0x80 | ((cp >> 6) & 63)); u[2] = (char)(0x80 | (cp & 63)); ul = 3; }
        if (o + (size_t)ul >= n) { *ok = 0; return p; }
        memcpy(out + o, u, (size_t)ul);
        o += (size_t)ul;
    }
    if (*p != '"') { *ok = 0; return p; }
    out[o] = 0;
    return p + 1;
}

static int parse_plan(const char *s, Plan *pl) {
    memset(pl, 0, sizeof *pl);
    int ok = 1;
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p++ != '{') return -1;
    while (ok) {
        while (isspace((unsigned char)*p) || *p == ',') p++;
        if (*p == '}') break;
        char key[32];
        p = jstring(p, key, sizeof key, &ok);
        while (isspace((unsigned char)*p)) p++;
        if (!ok || *p++ != ':') return -1;
        while (isspace((unsigned char)*p)) p++;
        /* the mode decides whether a disk gets erased: it is a string, once, or the plan is refused */
        if (!strcmp(key, "mode") && (*p != '"' || pl->mode[0])) return -1;
        if (*p == '"') {
            char val[1100];
            p = jstring(p, val, sizeof val, &ok);
            if (!ok) return -1;
            struct { const char *k; char *dst; size_t n; } f[] = {
                { "mode", pl->mode, sizeof pl->mode }, { "disk", pl->disk, sizeof pl->disk }, { "user", pl->user, sizeof pl->user },
                { "fullname", pl->fullname, sizeof pl->fullname }, { "password", pl->password, sizeof pl->password },
                { "hostname", pl->hostname, sizeof pl->hostname }, { "lang", pl->lang, sizeof pl->lang },
                { "locale", pl->locale, sizeof pl->locale }, { "timezone", pl->timezone, sizeof pl->timezone },
                { "kblayout", pl->kblayout, sizeof pl->kblayout }, { "kbvariant", pl->kbvariant, sizeof pl->kbvariant },
                { "passwordHash", pl->password_hash, sizeof pl->password_hash }, { "sshKey", pl->ssh_key, sizeof pl->ssh_key },
                { "delete", pl->del, sizeof pl->del }, { "create", pl->create, sizeof pl->create }, { "tableTag", pl->table_tag, sizeof pl->table_tag },
            };
            if (!strcmp(key, "passwordHash") || !strcmp(key, "sshKey")) pl->has_extra = 1;
            for (size_t i = 0; i < sizeof f / sizeof *f; i++)
                if (!strcmp(key, f[i].k)) { if (strlen(val) >= f[i].n) return -1; cpy(f[i].dst, f[i].n, val); }
        } else if (isdigit((unsigned char)*p) || *p == '-') {
            char *end;
            long v = strtol(p, &end, 10);
            if (!strcmp(key, "swap")) pl->swap = v;
            if (!strcmp(key, "rootPart")) pl->root_part = v;
            if (!strcmp(key, "homePart")) pl->home_part = v;
            if (!strcmp(key, "swapPart")) pl->swap_part = v;
            if (!strcmp(key, "espPart")) pl->esp_part = v;
            if (!strcmp(key, "freeStart") || !strcmp(key, "freeEnd")) {
                char *e2;
                errno = 0;
                unsigned long long u = strtoull(p, &e2, 10);
                if (*p == '-' || errno || e2 != end) return -1;
                *(key[4] == 'S' ? &pl->free_start : &pl->free_end) = u;
            }
            p = end;
        } else if (!strncmp(p, "true", 4) || !strncmp(p, "null", 4)) {
            if (!strcmp(key, "autologin")) { pl->has_extra = 1; pl->autologin = p[0] == 't'; }
            if (!strcmp(key, "homeFormat")) pl->home_format = p[0] == 't';
            if (!strcmp(key, "espFormat")) pl->esp_format = p[0] == 't';
            p += 4;
        } else if (!strncmp(p, "false", 5)) { if (!strcmp(key, "autologin")) pl->has_extra = 1; p += 5; }
        else return -1;
    }
    /* the unattended fields come only from lord-autoinstall; the interface can't send them */
    if (pl->has_extra && !unattended) return -1;
    return ok ? 0 : -1;
}

static int re_user(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 32 || !(islower((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (; *s; s++) if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s) && *s != '_' && *s != '-') return 0;
    return 1;
}

static int re_host(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 63 || s[0] == '-' || s[n - 1] == '-') return 0;
    for (; *s; s++) if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s) && *s != '-') return 0;
    return 1;
}

static int re_simple(const char *s, size_t max, const char *extra, int allow_empty) {
    size_t n = strlen(s);
    if ((!n && !allow_empty) || n > max) return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && !strchr(extra, *s)) return 0;
    return 1;
}

static int locale_supported(const char *name) {
    if (!re_simple(name, 48, "_.-@", 0)) return 0;
    FILE *f = fopen("/usr/share/i18n/SUPPORTED", "r");
    char line[256], loc[128], cs[64];
    int found = 0;
    while (f && !found && fgets(line, sizeof line, f))
        if (sscanf(line, "%127s %63s", loc, cs) == 2 && !strcmp(loc, name) && !strcmp(cs, "UTF-8")) found = 1;
    if (f) fclose(f);
    return found;
}

/* What the person chose: the account, the computer's name, language, zone and keyboard. */
static const char *validate_person(Plan *pl) {
    static const char *const reserved[] = { "root", "daemon", "bin", "sys", "sync", "games", "man", "lp", "mail", "news", "uucp",
        "proxy", "www-data", "backup", "list", "irc", "nobody", "systemd-network", "messagebus", "pipewire", "sudo", "admin", NULL };
    if (!re_user(pl->user)) return "install.badUser";
    for (int i = 0; reserved[i]; i++) if (!strcmp(pl->user, reserved[i])) return "install.userReserved";
    struct passwd *pw = getpwnam(pl->user);
    if (pw && (strcmp(pl->user, "lordos") || is_first_account())) return "install.userTaken";
    for (const unsigned char *p = (const unsigned char *)pl->fullname; *p; p++) if (*p < 0x20 || *p == ':' || *p == ',' || *p == 0x7f) return "install.badFullName";
    if (!pl->password[0] && !pl->password_hash[0]) return "install.noPassword";
    if (pl->password_hash[0] && (pl->password[0] || !install_hash_ok(pl->password_hash))) return "install.badPasswordHash";
    if (pl->ssh_key[0] && !install_ssh_key_ok(pl->ssh_key)) return "install.badSshKey";
    if (!re_host(pl->hostname)) return "install.badHostname";
    if (!re_simple(pl->lang, 10, "-", 0)) return "install.badLanguage";
    if (!locale_supported(pl->locale)) return "install.badRegion";
    char p[PATH_MAX];
    struct stat st;
    if (!re_simple(pl->timezone, 60, "/_+-", 0) || strstr(pl->timezone, "..") || pl->timezone[0] == '/') return "install.badTimezone";
    snprintf(p, sizeof p, "/usr/share/zoneinfo/%s", pl->timezone);
    if (stat(p, &st) || !S_ISREG(st.st_mode)) return "install.unknownTimezone";
    if (!re_simple(pl->kblayout, 20, "_", 0)) return "install.badKbLayout";
    snprintf(p, sizeof p, "/usr/share/X11/xkb/symbols/%s", pl->kblayout);
    if (access(p, R_OK)) return "install.unknownKbLayout";
    if (!re_simple(pl->kbvariant, 36, "_-", 1)) return "install.badKbVariant";
    return NULL;
}

/* The manual fields of the plan as manual.c takes them; -1 if one is malformed. */
static int manual_plan(const Plan *pl, MpPlan *m) {
    memset(m, 0, sizeof *m);
    const long parts[MP_ROLES] = { [MP_ROOT] = pl->root_part, [MP_HOME] = pl->home_part, [MP_SWAP] = pl->swap_part, [MP_ESP] = pl->esp_part };
    for (int r = 0; r < MP_ROLES; r++) {
        if (parts[r] < 0 || parts[r] > FSP_MAX_PARTS) return -1;
        m->part[r] = (unsigned)parts[r];
    }
    if (mp_parse_delete(pl->del, m) || mp_parse_create(pl->create, m)) return -1;
    m->home_format = pl->home_format;
    m->esp_format = pl->esp_format;
    m->swap_file = pl->swap;
    return 0;
}

static const char *validate(Plan *pl) {
    if (fsp_mode(pl->mode) < 0) return "install.badMode";
    Disk disks[32];
    int n = list_disks(disks, 32), found = 0;
    for (int i = 0; i < n; i++) if (!strcmp(disks[i].name, pl->disk)) { found = 1; if (disks[i].size < MIN_DISK) return "install.diskTooSmall"; }
    if (!found) return "install.diskNotListed";
    const char *why = validate_person(pl);
    if (why) return why;
    if (pl->swap < 0 || pl->swap > 16384) return "install.badSwap";
    if (fsp_mode(pl->mode) == 1) {
        /* every rule of the free space mode, from the disk itself (freespace.c) */
        static FpProbe fp;
        fp_probe(pl->disk, &fp);
        if (fp.why) return fp.why;
        if (!pl->free_start || pl->free_start != fp.choice.region.start || pl->free_end != fp.choice.region.end) return "install.free.diskChanged";
    }
    if (fsp_mode(pl->mode) == 2) {
        /* every rule of manual partitioning, from the disk itself (manual.c) */
        static MpPlan mpl;
        static MppProbe mp;
        if (manual_plan(pl, &mpl)) return "install.badPlan";
        const char *why2 = mpp_check(pl->disk, pl->table_tag, &mpl, &mp);
        if (why2) return why2;
    }
    return NULL;
}
/* ---- install-run steps ---- */

static void progress(const char *phase, int done, int total) { admin_say("LOG", "progress %s %d %d", phase, done, total); }   /* i18n-ok */

static int run(const char *const argv[]) {
    int r = admin_run(argv);
    if (r) admin_log("install.stepFailed", "program", argv[0], "code", lerr_int(r), NULL);
    return r;
}

/* Program inside the new system: chroot TARGET argv... */
static int in_target(const char *const argv[]) {
    const char *full[24] = { "/usr/sbin/chroot", TARGET };
    int i = 0;
    for (; argv[i] && i < 21; i++) full[2 + i] = argv[i];
    full[2 + i] = NULL;
    return run(full);
}

/* unsquashfs with its percentage as progress of the copy phase. */
static int copy_system(void) {
    const char *const argv[] = { "/usr/bin/unsquashfs", "-f", "-percentage", "-d", TARGET, SQUASHFS, NULL };
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(p[1], 1); dup2(p[1], 2);
        syscall(SYS_close_range, 3, ~0u, 0);
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", NULL };
        execve(argv[0], (char *const *)argv, env);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) { close(p[0]); return -1; }
    char buf[256];
    int last = -1, cur = 0, any = 0;
    ssize_t n;
    while ((n = read(p[0], buf, sizeof buf)) > 0)
        for (ssize_t i = 0; i < n; i++) {
            if (isdigit((unsigned char)buf[i])) { cur = cur * 10 + (buf[i] - '0'); any = 1; }
            else { if (any && cur <= 100 && cur != last) { progress("copy", cur, 100); last = cur; } cur = 0; any = 0; }
        }
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

static int uuid_of(const char *dev, char *out, size_t n) {
    const char *const argv[] = { "/usr/sbin/blkid", "-s", "UUID", "-o", "value", dev, NULL };
    char buf[128];
    if (admin_run_capture(argv, buf, sizeof buf)) return -1;
    buf[strcspn(buf, "\n")] = 0;
    if (!re_simple(buf, 64, "-", 0)) return -1;
    cpy(out, n, buf);
    return 0;
}

/* Changes a user's hash in the new system's etc/shadow (without going through argv). */
static int set_shadow(const char *user, const char *hash) {
    const char *path = TARGET "/etc/shadow";
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *out = NULL;
    size_t ol = 0;
    FILE *m = open_memstream(&out, &ol);
    char line[1024];
    size_t ul = strlen(user);
    int done = 0;
    while (m && fgets(line, sizeof line, f)) {
        char *c = strchr(line, ':');
        if (c && (size_t)(c - line) == ul && !strncmp(line, user, ul)) {
            char *rest = strchr(c + 1, ':');
            fprintf(m, "%s:%s%s", user, hash, rest ? rest : ":\n");
            done = 1;
        } else fputs(line, m);
    }
    fclose(f);
    if (!m) return -1;
    fclose(m);
    struct stat st;
    stat(path, &st);
    int r = done ? write_file(path, out, st.st_mode & 07777) : -1;
    if (!r) if (chown(path, st.st_uid, st.st_gid)) admin_log("install.chownWarning", NULL);
    explicit_bzero(out, ol);
    free(out);
    return r;
}

/* User's uid and gid in the new system's etc/passwd. */
static int target_ids(const char *user, uid_t *uid, gid_t *gid) {
    FILE *f = fopen(TARGET "/etc/passwd", "r");
    char line[1024];
    int ok = -1;
    size_t ul = strlen(user);
    while (f && ok && fgets(line, sizeof line, f)) {
        if (strncmp(line, user, ul) || line[ul] != ':') continue;
        unsigned u, g;
        if (sscanf(line + ul, ":%*[^:]:%u:%u:", &u, &g) == 2) { *uid = u; *gid = g; ok = 0; }
    }
    if (f) fclose(f);
    return ok;
}

static void umount_target(void) {
    static const char *const sub[] = { "/sys/firmware/efi/efivars", "/run", "/sys", "/proc", "/dev/pts", "/dev", "/boot/efi", "/home", "", NULL };
    sync();
    for (int i = 0; sub[i]; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, TARGET "%s", sub[i]);
        umount2(p, MNT_DETACH);
    }
}

static int bind(const char *what) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, TARGET "%s", what);
    mkdir(p, 0755);
    if (mount(what, p, NULL, MS_BIND | MS_REC, NULL)) return -1;
    /* slave: whatever gets unmounted in here doesn't propagate to the live system (under systemd /run,
     * /dev and /sys are "shared": unmounting TARGET/run used to unmount /run/live/medium) */
    return mount(NULL, p, NULL, MS_REC | MS_SLAVE, NULL);
}

static int disk_busy(const char *disk) {
    char dev[48], line[1024];
    snprintf(dev, sizeof dev, "/dev/%s", disk);
    size_t dl = strlen(dev);
    int busy = 0;
    FILE *f = fopen("/proc/self/mounts", "r");
    while (f && !busy && fgets(line, sizeof line, f)) if (!strncmp(line, dev, dl)) busy = 1;
    if (f) fclose(f);
    f = fopen("/proc/swaps", "r");
    while (f && !busy && fgets(line, sizeof line, f)) if (!strncmp(line, dev, dl)) busy = 1;
    if (f) fclose(f);
    return busy;
}

/* Mode free: what a failed install has to give back. Empty in mode erase. */
static FpUndo undo;
static int undo_boot_folder;
/* Mode manual: the same, from manualpart.c. */
static MppResult mres;

static void free_abort(void) {
    if (undo_boot_folder) {                     /* EFI/LordOS didn't exist before (freespace.c): it's all ours */
        const char *const a[] = { "/usr/bin/rm", "-rf", "--", TARGET "/boot/efi/EFI/" FP_BOOT_FOLDER, NULL };
        run(a);
        undo_boot_folder = 0;
    }
    if (!undo.active && !mres.undo.active) {
        /* deleted partitions were formatted over: their table entries can't honestly come back */
        if (mres.kept_new_table) admin_log("install.manual.tableKept", NULL);
        return;
    }
    umount_target();
    fp_undo(&undo);
    fp_undo(&mres.undo);
}

#define FAIL(...) do { free_abort(); admin_err(__VA_ARGS__, NULL); umount_target(); explicit_bzero(&pl, sizeof pl); return; } while (0)

/* The steps both ways of making the person's account share, under TARGET: the new system in
 * install-run, the running one (a bind mount) in install-account. Returns an error code or NULL. */
static const char *setup_person(Plan *pl, int *step, int steps) {
    writef(TARGET "/etc/hostname", 0644, "%s\n", pl->hostname);
    writef(TARGET "/etc/hosts", 0644, "127.0.0.1\tlocalhost\n127.0.1.1\t%s\n::1\tlocalhost ip6-localhost ip6-loopback\n", pl->hostname);

    /* a kept /home (mode manual) may already have this user's folder: it stays, with its files */
    char home[PATH_MAX];
    struct stat hst;
    snprintf(home, sizeof home, TARGET "/home/%s", pl->user);
    const int had_home = lstat(home, &hst) == 0;
    if (had_home && !S_ISDIR(hst.st_mode)) return "install.manual.homeNotFolder";

    /* the user, with their password hashed here (yescrypt) */
    { const char *const a[] = { "/usr/sbin/useradd", "-m", "-s", "/bin/bash", "-c", pl->fullname, "-G", "sudo,audio,video,input,render,pipewire", pl->user, NULL };
      if (in_target(a)) return "install.userFailed"; }
    /* CUPS printer administrator (lpadmin group, docs/PRINTING.md); if CUPS didn't come in
     * the image, the group doesn't exist and nothing happens */
    { const char *const a[] = { "/usr/sbin/usermod", "--append", "--groups", "lpadmin", pl->user, NULL }; if (in_target(a)) { /* no CUPS */ } }
    { const char *const a[] = { "/usr/sbin/usermod", "--append", "--groups", "plugdev", pl->user, NULL }; if (in_target(a)) { /* no plugdev group */ } }   /* FIDO2 keys (docs/PASSWORDS.md) */
    struct crypt_data cd;
    memset(&cd, 0, sizeof cd);
    const char *hash;
    if (pl->password_hash[0]) hash = pl->password_hash;            /* unattended: made by the maintainer */
    else {
        char *salt = crypt_gensalt_ra("$y$", 0, NULL, 0);
        hash = salt ? crypt_r(pl->password, salt, &cd) : NULL;
        free(salt);
    }
    if (!hash || hash[0] == '*') return "install.hashFailed";
    if (set_shadow(pl->user, hash) || set_shadow("root", "*")) { explicit_bzero(&cd, sizeof cd); return "install.passwordFailed"; }
    explicit_bzero(&cd, sizeof cd);
    explicit_bzero(pl->password, sizeof pl->password);
    mkdir(TARGET "/var/lib/systemd/linger", 0755);
    { char p[PATH_MAX]; snprintf(p, sizeof p, TARGET "/var/lib/systemd/linger/%s", pl->user); write_file(p, "", 0644); }
    mkdir(TARGET "/etc/lordos", 0755);
    /* lord-sessiond (docs/SESSIONS.md): the user logs in through the login screen; autologin
     * is turned on in Settings → Users (AUTOLOGIN=). USER= is for the pre-M8
     * lordos-session. */
    writef(TARGET "/etc/lordos/session.conf", 0644, "# Written by the LordOS installer. See docs/SESSIONS.md\n"
           "# AUTOLOGIN=user logs in automatically at boot; empty: login screen\nAUTOLOGIN=%s\nUSER=%s\n", pl->autologin ? pl->user : "", pl->user);
    progress("configure", ++*step, steps);

    /* language, region, time zone and keyboard */
    { char p[PATH_MAX], line[256]; snprintf(p, sizeof p, TARGET "/etc/locale.gen");
      FILE *f = fopen(p, "r"); int has = 0; size_t ll = strlen(pl->locale);
      while (f && fgets(line, sizeof line, f)) if (!strncmp(line, pl->locale, ll) && line[ll] == ' ') has = 1;
      if (f) fclose(f);
      if (!has) { f = fopen(p, "a"); if (f) { fprintf(f, "%s UTF-8\n", pl->locale); fclose(f); }
                  const char *const a[] = { "/usr/sbin/locale-gen", NULL }; in_target(a); } }
    writef(TARGET "/etc/default/locale", 0644, "LANG=%s\n", pl->locale);
    { char zp[PATH_MAX]; snprintf(zp, sizeof zp, "/usr/share/zoneinfo/%s", pl->timezone);
      unlink(TARGET "/etc/localtime"); if (symlink(zp, TARGET "/etc/localtime")) admin_log("install.timezoneWarning", NULL);
      writef(TARGET "/etc/timezone", 0644, "%s\n", pl->timezone); }
    writef(TARGET "/etc/default/keyboard", 0644, "XKBMODEL=\"pc105\"\nXKBLAYOUT=\"%s\"\nXKBVARIANT=\"%s\"\nXKBOPTIONS=\"\"\nBACKSPACE=\"guess\"\n", pl->kblayout, pl->kbvariant);
    progress("configure", ++*step, steps);

    /* the user's LordOS settings: chosen language and keyboard */
    uid_t uid; gid_t gid;
    if (target_ids(pl->user, &uid, &gid)) return "install.userMissing";
    /* the kept folder belonged to another uid (an earlier system): only that owner's files become
     * this user's; -h never follows a link out of the folder */
    if (had_home && hst.st_uid != uid) {
        char from[32], to[48];
        snprintf(from, sizeof from, "--from=%u", (unsigned)hst.st_uid);
        snprintf(to, sizeof to, "%u:%u", (unsigned)uid, (unsigned)gid);
        const char *const a[] = { "/usr/bin/chown", "-R", "-h", from, to, "--", home, NULL };
        if (run(a)) admin_log("install.chownWarning", NULL);
    }
    /* a kept folder keeps its own settings, and nothing is made or written through a link in it */
    { char p[PATH_MAX];
      struct stat cst;
      snprintf(p, sizeof p, TARGET "/home/%s/.config", pl->user); mkdir(p, 0755);
      int may = lstat(p, &cst) == 0 && S_ISDIR(cst.st_mode);
      if (may && lchown(p, uid, gid)) admin_log("install.chownWarning", NULL);
      snprintf(p, sizeof p, TARGET "/home/%s/.config/lordos", pl->user);
      if (may) { mkdir(p, 0755); may = lstat(p, &cst) == 0 && S_ISDIR(cst.st_mode); }
      if (may && lchown(p, uid, gid)) admin_log("install.chownWarning", NULL);
      snprintf(p, sizeof p, TARGET "/home/%s/.config/lordos/settings.json", pl->user);
      if (may && (!had_home || access(p, F_OK))) {
          writef(p, 0644, "{\n  \"language\": \"%s\",\n  \"keyboards\": [{ \"layout\": \"%s\", \"variant\": \"%s\" }],\n  \"keyboardIndex\": 0\n}\n", pl->lang, pl->kblayout, pl->kbvariant);
          if (lchown(p, uid, gid)) admin_log("install.chownWarning", NULL);
      }
      snprintf(p, sizeof p, TARGET "/home/%s/.config/locale.conf", pl->user);
      if (may && (!had_home || access(p, F_OK))) { writef(p, 0644, "LANG=%s\n", pl->locale); if (lchown(p, uid, gid)) admin_log("install.chownWarning", NULL); } }
    /* Escritorio, Documentos, Descargas… in the chosen language and ~/.config/user-dirs.dirs (the
     * same list as Settings → Users and the live system: users/homedirs.c) */
    { char p[PATH_MAX]; snprintf(p, sizeof p, TARGET "/home/%s", pl->user);
      if (homedirs_create(p, uid, gid, pl->locale)) admin_log("install.homedirsWarning", NULL); }
    /* unattended: the maintainer's SSH key, for when remote login is turned on (Settings → Sharing) */
    if (pl->ssh_key[0]) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, TARGET "/home/%s/.ssh", pl->user);
        mkdir(p, 0700);
        if (chown(p, uid, gid)) admin_log("install.chownWarning", NULL);
        snprintf(p, sizeof p, TARGET "/home/%s/.ssh/authorized_keys", pl->user);
        if (writef(p, 0600, "%s\n", pl->ssh_key)) return "install.sshKeyFailed";
        if (chown(p, uid, gid)) admin_log("install.chownWarning", NULL);
    }
    progress("configure", ++*step, steps);
    return NULL;
}

void install_act_run(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    Plan pl;
    if (!is_live()) { admin_err("install.liveOnly", NULL); return; }
    if (parse_plan(arg, &pl)) { admin_err("install.badPlan", NULL); return; }
    const char *why = validate(&pl);
    if (why) { explicit_bzero(&pl, sizeof pl); admin_err(why, NULL); return; }
    if (disk_busy(pl.disk)) { admin_err("install.diskBusy", "disk", pl.disk, NULL); explicit_bzero(&pl, sizeof pl); return; }

    const int beside = fsp_mode(pl.mode) == 1, manual = fsp_mode(pl.mode) == 2;
    /* both share a disk with other systems: grub in EFI/LordOS with its own firmware entry */
    const int shared = beside || manual;
    char dev[48], esp[48], root[48];
    snprintf(dev, sizeof dev, "/dev/%s", pl.disk);
    if (!shared) {
    part_path(pl.disk, INSTALL_BIOS ? 2 : 1, esp, sizeof esp);
    part_path(pl.disk, INSTALL_BIOS ? 3 : 2, root, sizeof root);
    }
    admin_log(beside ? "install.free.installing" : manual ? "install.manual.installing" : "install.installingTo", "dev", dev, NULL);

    /* 1. partition. Mode free: one new partition, nothing else (freepart.c). Mode manual: what the
     * person chose, deleted, created and formatted in manualpart.c */
    progress("partition", 0, 4);
    static MpPlan mpl;
    if (manual) {
        if (manual_plan(&pl, &mpl)) FAIL("install.badPlan");
        const char *why2 = mpp_apply(pl.disk, pl.table_tag, &mpl, &mres);
        if (why2) FAIL(why2);
        cpy(esp, sizeof esp, mres.dev[MP_ESP]);
        cpy(root, sizeof root, mres.dev[MP_ROOT]);
        progress("partition", 3, 4);
    } else if (beside) {
        const char *why2 = fp_create(pl.disk, pl.free_start, pl.free_end, &undo);
        if (why2) FAIL(why2);
        cpy(esp, sizeof esp, undo.esp);
        cpy(root, sizeof root, undo.root);
        progress("partition", 3, 4);
    } else {
    /* GPT with BIOS boot (1 MiB, x86-64 only), ESP (512 MiB) and root */
    { const char *const a[] = { "/usr/sbin/wipefs", "-a", dev, NULL }; if (run(a)) FAIL("install.wipeFailed"); }
    { const char *const a[] = { "/usr/sbin/sgdisk", "--zap-all", dev, NULL }; run(a); }
#if INSTALL_BIOS
    { const char *const a[] = { "/usr/sbin/sgdisk", "-n", "1:0:+1M", "-t", "1:ef02", "-c", "1:biosboot",
                                "-n", "2:0:+512M", "-t", "2:ef00", "-c", "2:EFI",
                                "-n", "3:0:0", "-t", "3:8304", "-c", "3:LordOS", dev, NULL };
      if (run(a)) FAIL("install.partitionFailed"); }
#else
    { const char *const a[] = { "/usr/sbin/sgdisk", "-n", "1:0:+512M", "-t", "1:ef00", "-c", "1:EFI",
                                "-n", "2:0:0", "-t", "2:8305", "-c", "2:LordOS", dev, NULL };
      if (run(a)) FAIL("install.partitionFailed"); }
#endif
    progress("partition", 1, 4);
    { const char *const a[] = { "/usr/bin/udevadm", "settle", "--timeout=10", NULL }; run(a); }
    for (int i = 0; i < 50 && (access(esp, F_OK) || access(root, F_OK)); i++) usleep(200000);
    if (access(esp, F_OK) || access(root, F_OK)) FAIL("install.noPartitions");
    { const char *const a[] = { "/usr/sbin/mkfs.vfat", "-F", "32", "-n", "EFI", esp, NULL }; if (run(a)) FAIL("install.formatEfiFailed"); }
    progress("partition", 2, 4);
    { const char *const a[] = { "/usr/sbin/mkfs.ext4", "-F", "-q", "-L", "LordOS", root, NULL }; if (run(a)) FAIL("install.formatRootFailed"); }
    progress("partition", 3, 4);
    }
    mkdir("/run/lordos-install", 0700);
    mkdir(TARGET, 0755);
    umount_target();
    if (mount(root, TARGET, "ext4", 0, NULL)) FAIL("install.mountFailed", "path", root, "errno", lerr_errno(errno));
    progress("partition", 4, 4);

    /* 2. copy the live system */
    progress("copy", 0, 100);
    if (copy_system()) FAIL("install.copyFailed");
    progress("copy", 100, 100);
    mkdir(TARGET "/boot/efi", 0755);
    if (mount(esp, TARGET "/boot/efi", "vfat", 0, "umask=0077")) FAIL("install.mountEfiFailed");
    if (bind("/dev") || bind("/proc") || bind("/sys") || bind("/run")) FAIL("install.prepareFailed");

    /* 3. configure */
    const int steps = 9;
    int step = 0;
    progress("configure", step, steps);
    char ru[80], eu[80];
    if (uuid_of(root, ru, sizeof ru) || uuid_of(esp, eu, sizeof eu)) FAIL("install.uuidFailed");
    char swapline[160] = "";
    if (pl.swap > 0) snprintf(swapline, sizeof swapline, "/swapfile none swap sw 0 0\n");
    /* mode manual: a /home and a swap partition of their own */
    char homeline[160] = "";
    if (manual && mres.dev[MP_HOME][0]) {
        char hu[80];
        if (uuid_of(mres.dev[MP_HOME], hu, sizeof hu)) FAIL("install.uuidFailed");
        snprintf(homeline, sizeof homeline, "UUID=%s /home %s defaults 0 %d\n", hu, mres.home_fs, mp_fsck_pass(mres.home_fs));
    }
    if (manual && mres.dev[MP_SWAP][0]) {
        char su[80];
        if (uuid_of(mres.dev[MP_SWAP], su, sizeof su)) FAIL("install.uuidFailed");
        snprintf(swapline, sizeof swapline, "UUID=%s none swap sw 0 0\n", su);
    }
    /* a shared ESP is never checked or repaired at boot, and its absence doesn't stop this system;
     * one LordOS formatted is its own, as in an erase install */
    const int esp_shared = beside || (manual && !mres.esp_formatted);
    if (writef(TARGET "/etc/fstab", 0644,
               esp_shared ? "# /etc/fstab — generated by the LordOS installer\n"
                            "UUID=%s / ext4 errors=remount-ro 0 1\nUUID=%s /boot/efi vfat umask=0077,nofail 0 0\n%s%s"
                          : "# /etc/fstab — generated by the LordOS installer\n"
                            "UUID=%s / ext4 errors=remount-ro 0 1\nUUID=%s /boot/efi vfat umask=0077 0 1\n%s%s", ru, eu, homeline, swapline))
        FAIL("sys.writeFailed", "path", "/etc/fstab");
    progress("configure", ++step, steps);

    /* drop the live-only bits: live-boot, the test user and its passwordless sudo */
    unlink(TARGET "/etc/sudoers.d/lordos-live");
    unlink(TARGET "/var/lib/systemd/linger/lordos");
    unlink(TARGET "/etc/sysctl.d/50-lordos-ping.conf.dpkg-old");
    { const char *const a[] = { "/usr/bin/dpkg", "--purge", "live-boot", "live-boot-initramfs-tools", NULL }; in_target(a); }
    { const char *const a[] = { "/usr/sbin/userdel", "-r", "lordos", NULL }; in_target(a); }
    /* mode manual: /home goes on its partition only now, after the live user's folder is gone, so
     * that removal can never reach a kept /home */
    if (manual && mres.dev[MP_HOME][0]) {
        mkdir(TARGET "/home", 0755);
        if (mount(mres.dev[MP_HOME], TARGET "/home", mres.home_fs, MS_NOSUID | MS_NODEV, NULL))
            FAIL("install.mountFailed", "path", mres.dev[MP_HOME], "errno", lerr_errno(errno));
    }
    progress("configure", ++step, steps);

    { const char *why = setup_person(&pl, &step, steps);
      if (why) FAIL(why); }

    /* swap as a file */
    if (pl.swap > 0) {
        char sz[24]; snprintf(sz, sizeof sz, "%ldM", pl.swap);
        const char *const a[] = { "/usr/bin/fallocate", "-l", sz, TARGET "/swapfile", NULL };
        if (!run(a)) { chmod(TARGET "/swapfile", 0600); const char *const b[] = { "/usr/sbin/mkswap", "-q", TARGET "/swapfile", NULL }; run(b); }
    }
    progress("configure", ++step, steps);

    /* machine identity and boot */
    unlink(TARGET "/etc/machine-id");
    { const char *const a[] = { "/usr/bin/systemd-machine-id-setup", NULL }; in_target(a); }
    /* the boot menu theme and the boot splash (docs/ISO.md, "Boot splash and boot menu"): the theme
     * and Plymouth come in the copied system; without them, the plain menu and text boot as before */
    int splash = access(TARGET "/usr/bin/plymouth", X_OK) == 0 && access(TARGET "/usr/share/plymouth/themes/lordos/lordos.plymouth", R_OK) == 0;
    int theme = access(TARGET "/usr/share/grub/themes/lordos/theme.txt", R_OK) == 0;
    /* beside another system: a menu with time to choose, and that system in it when os-prober came
     * in the image */
    int prober = shared && access(TARGET "/usr/bin/os-prober", X_OK) == 0;
    writef(TARGET "/etc/default/grub", 0644,
           "# generated by the LordOS installer\nGRUB_DEFAULT=0\nGRUB_TIMEOUT=%d\nGRUB_DISTRIBUTOR=LordOS\n"
           "GRUB_CMDLINE_LINUX_DEFAULT=\"quiet %sloglevel=3\"\nGRUB_CMDLINE_LINUX=\"\"\nGRUB_DISABLE_OS_PROBER=%s\n"
           "GRUB_GFXMODE=auto\nGRUB_GFXPAYLOAD_LINUX=keep\n%s",
           shared ? 5 : 2,
           splash ? "splash vt.global_cursor_default=0 " : "",
           prober ? "false" : "true",
           theme ? "GRUB_TERMINAL_OUTPUT=gfxterm\nGRUB_THEME=/usr/share/grub/themes/lordos/theme.txt\n" : "");
    progress("configure", ++step, steps);
    { const char *const a[] = { "/usr/sbin/update-initramfs", "-u", "-k", "all", NULL }; if (in_target(a)) FAIL("install.initramfsFailed"); }
    progress("configure", ++step, steps);
    progress("configure", steps, steps);

    /* 4. boot: grub for UEFI (removable path, works on any firmware) and, on x86-64, for BIOS.
     * Mode free: its own folder and its own firmware entry; never the removable path, which may be
     * another system's, and never the disk's first sectors */
    const int boot_steps = shared ? 3 : INSTALL_BIOS ? 3 : 2;
    int boot_step = 0;
    progress("bootloader", 0, boot_steps);
    if (shared) {
        /* mode manual may reuse a folder an earlier LordOS left: then it isn't ours to remove */
        undo_boot_folder = beside || mres.esp_formatted || access(TARGET "/boot/efi/EFI/" FP_BOOT_FOLDER, F_OK) != 0;
        const char *const a[] = { "/usr/sbin/grub-install", GRUB_EFI_TARGET, "--efi-directory=/boot/efi", "--bootloader-id=" FP_BOOT_FOLDER, "--no-nvram", NULL };
        if (in_target(a) || access(TARGET "/boot/efi/EFI/" FP_BOOT_FOLDER "/" GRUB_EFI_FILE, R_OK)) FAIL("install.grubEfiFailed");
        /* an EFI partition LordOS formatted holds nobody else's loader: the removable path too, so it
         * boots on firmware that forgets its entries */
        if (manual && mres.esp_formatted) {
            const char *const b[] = { "/usr/sbin/grub-install", GRUB_EFI_TARGET, "--efi-directory=/boot/efi", "--removable", "--no-nvram", NULL };
            if (in_target(b)) FAIL("install.grubEfiFailed");
        }
    } else {
    { const char *const a[] = { "/usr/sbin/grub-install", GRUB_EFI_TARGET, "--efi-directory=/boot/efi", "--removable", "--no-nvram", NULL };
      if (in_target(a)) FAIL("install.grubEfiFailed"); }
    }
    progress("bootloader", ++boot_step, boot_steps);
#if INSTALL_BIOS
    if (!shared) {          /* never the first sectors of a disk other systems use */
    { const char *const a[] = { "/usr/sbin/grub-install", "--target=i386-pc", dev, NULL };
      if (in_target(a)) FAIL("install.grubBiosFailed"); }
    progress("bootloader", ++boot_step, boot_steps);
    }
#endif
    { const char *const a[] = { "/usr/sbin/grub-mkconfig", "-o", "/boot/grub/grub.cfg", NULL };
      if (in_target(a)) FAIL("install.grubConfigFailed"); }
    progress("bootloader", ++boot_step, boot_steps);
    if (shared) {
        /* last on purpose: after this nothing can fail, so no entry needs undoing. It only adds an
         * entry: the others stay, this one goes first in the boot order */
        char pn[16];
        snprintf(pn, sizeof pn, "%u", manual ? mres.esp_num : undo.esp_num);
        const char *const a[] = { "/usr/bin/efibootmgr", "-q", "-c", "-d", dev, "-p", pn, "-L", "LordOS",
                                  "-l", "\\EFI\\" FP_BOOT_FOLDER "\\" GRUB_EFI_FILE, NULL };
        if (run(a)) FAIL("install.free.bootEntryFailed");
        progress("bootloader", ++boot_step, boot_steps);
        fp_commit(&undo);
        fp_commit(&mres.undo);
        mres.kept_new_table = 0;
    }
    umount_target();
    explicit_bzero(&pl, sizeof pl);
    admin_ok("install.done", "dev", dev, NULL);
}

/* The OVA's first boot. No disk and no copy: the system is already there, and the person's account is
 * made in it with install-run's own steps. TARGET is the running system (a private bind mount, so
 * nothing else needs mounting: these steps only touch /etc and /home). lordos, the setup account the
 * session runs as, can't be removed from inside it: it loses what lets it log in here and
 * lordos-live-prep removes it on the next boot. */
void install_act_account(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)len;
    Plan pl;
    if (!is_first_account()) { admin_err("install.noFirstAccount", NULL); return; }
    if (parse_plan(arg, &pl)) { admin_err("install.badPlan", NULL); return; }
    const char *why = validate_person(&pl);
    if (why) { explicit_bzero(&pl, sizeof pl); admin_err(why, NULL); return; }
    admin_log("install.creatingAccount", "user", pl.user, NULL);
    mkdir("/run/lordos-install", 0700);
    mkdir(TARGET, 0755);
    umount_target();
    if (mount("/", TARGET, NULL, MS_BIND, NULL) || mount(NULL, TARGET, NULL, MS_PRIVATE, NULL))
        FAIL("install.mountFailed", "path", "/", "errno", lerr_errno(errno));
    int step = 0;
    const int steps = 3;
    progress("configure", step, steps);
    why = setup_person(&pl, &step, steps);
    if (why) {                                  /* nothing half made is left in the way of a new try */
        const char *const a[] = { "/usr/sbin/userdel", "-r", pl.user, NULL };
        in_target(a);
        FAIL(why);
    }
    /* last, and in this order: until the first file is written the setup still logs in */
    unlink("/var/lib/systemd/linger/lordos");
    if (write_file(FIRST_ACCOUNT_DONE, "", 0644) || unlink(FIRST_ACCOUNT)) FAIL("sys.writeFailed", "path", FIRST_ACCOUNT);
    umount_target();
    explicit_bzero(&pl, sizeof pl);
    admin_ok("install.accountDone", NULL);
}

/* Reboots without waiting around: a separate process (outlives the helper, which dies with
 * lord-server when the session shuts down) asks systemd to reboot without blocking (--no-block) and, if
 * after 20 s the system is still alive and nobody killed it, does sync + reboot(2). */
void install_act_reboot(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg; (void)len;
    if (!is_live() && access("/run/live", F_OK) && access(FIRST_ACCOUNT_DONE, F_OK)) { admin_err("install.liveOnly", NULL); return; }
    sync();
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        signal(SIGHUP, SIG_IGN);            /* SIGTERM stays default: if systemd sends it, shutdown is already underway */
        prctl(PR_SET_NAME, "lordos-reboot");
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        syscall(SYS_close_range, 3, ~0u, 0);
        pid_t c = fork();
        if (c == 0) {
            char *const argv[] = { "/usr/bin/systemctl", "--no-block", "reboot", NULL };
            char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL };
            execve(argv[0], argv, env);
            _exit(127);
        }
        if (c > 0) waitpid(c, NULL, 0);
        sleep(20);
        sync();
        reboot(RB_AUTOBOOT);
        _exit(0);
    }
    if (pid < 0) { admin_err("install.rebootFailed", NULL); return; }
    waitpid(pid, NULL, 0);
    admin_ok("install.rebooting", NULL);
}
