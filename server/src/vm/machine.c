/* machine.c — machine.json: parse, check, write (see machine.h). */
#include "machine.h"
#include "../sec/json.h"
#include "../doc/wire.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int fail(char *err, size_t n, int code, const char *fmt, ...) {
    if (err && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return code;
}

const char *vm_arch_name(int arch) { return arch == VM_ARCH_ARM64 ? "arm64" : arch == VM_ARCH_AMD64 ? "amd64" : "?"; }

int vm_uuid_valid(const char *s) {
    if (!s || strlen(s) != 36) return 0;
    for (int i = 0; i < 36; i++) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return 0; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

static int hex2(const char *p) {
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else return -1;
    }
    return v;
}

/* new machines get QEMU's prefix (vm_new_mac); an imported one keeps its source's address, so any unicast one that
 * isn't all zero is accepted, lowercase */
int vm_mac_valid(const char *s) {
    if (!s || strlen(s) != 17) return 0;
    int any = 0;
    for (int i = 0; i < 17; i += 3) {
        int v = hex2(s + i);
        if (v < 0) return 0;
        if (i + 2 < 17 && s[i + 2] != ':') return 0;
        if (!i && (v & 1)) return 0;
        any |= v;
    }
    return any != 0;
}

int vm_smbios_valid(const char *s) {
    if (!s || strlen(s) > VM_SMBIOS_MAX) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p < 0x20 || *p >= 0x7f) return 0;
    return 1;
}

int vm_serial_valid(const char *s) {
    if (!s || strlen(s) > VM_SERIAL_MAX) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (!((*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'z') || *p == '-' || *p == '_' || *p == '.')) return 0;
    return 1;
}

const char *const VM_NIC_MODELS[VM_NIC_COUNT + 1] = { "virtio", "e1000e", "e1000", "e1000-82545em", "e1000-82544gc", "vmxnet3", "rtl8139", "pcnet", NULL };

/* 1..64 bytes of valid UTF-8, no control characters, no '/' or '\\', not only spaces */
int vm_name_valid(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (!n || n > VM_NAME_MAX) return 0;
    int visible = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p;
        int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (k < 0 || (k == 1 && c < 0xc2) || (k == 3 && c > 0xf4)) return 0;
        uint32_t cp = k == 0 ? c : k == 1 ? c & 0x1f : k == 2 ? c & 0x0f : c & 0x07;
        for (int i = 1; i <= k; i++) {
            if ((p[i] & 0xc0) != 0x80) return 0;
            cp = cp << 6 | (p[i] & 0x3f);
        }
        if ((k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || cp == 0x2028 || cp == 0x2029) return 0;
        if (cp == '/' || cp == '\\') return 0;
        if (cp != ' ') visible = 1;
        p += k + 1;
    }
    return visible;
}

static int iso_time_valid(const char *s) {
    int y, mo, d, h, mi, se;
    char z;
    if (!s || strlen(s) != 20 || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%c", &y, &mo, &d, &h, &mi, &se, &z) != 7 || z != 'Z') return 0;
    return y >= 2000 && y <= 9999 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h <= 23 && mi <= 59 && se <= 60 && h >= 0 && mi >= 0 && se >= 0;
}

static int machine_type_valid(const char *s, int arch) {
    const char *pre = arch == VM_ARCH_ARM64 ? "virt-" : "pc-q35-";
    size_t pl = strlen(pre);
    if (strncmp(s, pre, pl)) return 0;
    const char *p = s + pl;
    int dots = 0, digits = 0;
    for (; *p; p++) {
        if (*p == '.') { if (!digits || dots) return 0; dots++; digits = 0; }
        else if (*p >= '0' && *p <= '9') digits++;
        else return 0;
    }
    return dots == 1 && digits > 0 && strlen(s) < 32;
}

/* an absolute path QEMU's options and Landlock can take as it is: no "..", no "//", no control characters,
 * no comma (QEMU's option separator) */
static int path_valid(const char *s) {
    if (!s || s[0] != '/' || strlen(s) >= VM_PATH_MAX) return 0;
    if (strstr(s, "/../") || strstr(s, "/./") || strstr(s, "//")) return 0;
    size_t n = strlen(s);
    if ((n >= 3 && !strcmp(s + n - 3, "/..")) || (n >= 2 && !strcmp(s + n - 2, "/."))) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p < 0x20 || *p == 0x7f || *p == ',') return 0;
    return 1;
}

int vm_tag_valid(const char *t) {
    size_t n = strlen(t);
    if (!n || n > VM_TAG_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

/* a is b, or inside it */
static int under_or_is(const char *a, const char *b) {
    size_t n = strlen(b);
    while (n > 1 && b[n - 1] == '/') n--;
    return !strncmp(a, b, n) && (a[n] == 0 || a[n] == '/');
}

int vm_share_allowed(const char *path, const char *home, const char *user, const char *library, char *err, size_t n) {
    if (!path_valid(path)) return fail(err, n, -1, "not an absolute path without '..', ',' or control characters");
    char real[PATH_MAX];
    if (!realpath(path, real)) return fail(err, n, -1, "the folder doesn't exist");
    if (strcmp(real, path)) return fail(err, n, -1, "the path goes through a symbolic link (the folder is %s)", real);
    struct stat st;
    if (stat(real, &st) || !S_ISDIR(st.st_mode)) return fail(err, n, -1, "not a folder");
    for (const char *c = real; (c = strchr(c, '/')); c++) if (c[1] == '.') return fail(err, n, -1, "hidden folders (settings, keys) can't be shared");
    char media[300];
    snprintf(media, sizeof media, "/media/%s", user && *user ? user : "-");
    int place = (home && *home && under_or_is(real, home) && strcmp(real, home)) || (under_or_is(real, media) && strcmp(real, media)) ||
                (under_or_is(real, "/mnt") && strcmp(real, "/mnt"));
    if (!place) return fail(err, n, -1, "only a folder inside the home folder (not all of it) or on a disk under /media or /mnt can be shared");
    if (library && *library && (under_or_is(real, library) || under_or_is(library, real)))
        return fail(err, n, -1, "the machines' library can't be shared, nor a folder holding it");
    return 0;
}

int vm_transfer_path(const char *dir, char *out, size_t n) {
    size_t l = dir ? strlen(dir) : 0;
    while (l > 1 && dir[l - 1] == '/') l--;
    const char *b = l ? memrchr(dir, '/', l) : NULL;
    if (!b || b == dir) return -1;
    size_t bl = (size_t)(dir + l - (b + 1));
    if (bl < 8 || bl > NAME_MAX || memcmp(dir + l - 7, ".lordvm", 7) || b[1] == '.') return -1;
    int w = snprintf(out, n, "%.*s/" VM_XFER_DIR "/%.*s", (int)(b - dir), dir, (int)(bl - 7), b + 1);
    if (w < 0 || (size_t)w >= n || !path_valid(out)) return -1;
    return 0;
}

/* the owner record beside a transfer folder: <library>/Transfer/.<name>.machine (outside the folder the guest sees) */
static int xfer_owner_of(const char *xp, char *out, size_t n) {
    const char *b = strrchr(xp, '/');
    if (!b) return -1;
    int w = snprintf(out, n, "%.*s/.%s.machine", (int)(b - xp), xp, b + 1);
    return w < 0 || (size_t)w >= n || strlen(b + 1) + 9 > NAME_MAX ? -1 : 0;
}
static int xfer_owner_read(const char *op, char id[37]) {
    int fd = open(op, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    char buf[64];
    ssize_t r = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() ? read(fd, buf, sizeof buf - 1) : -1;
    close(fd);
    if (r < 36) return -1;
    memcpy(id, buf, 36);
    id[36] = 0;
    return vm_uuid_valid(id) && (r == 36 || buf[36] == '\n') ? 0 : -1;
}
static int xfer_owner_write(const char *op, const char *id) {
    char tmp[PATH_MAX + 8];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", op) >= (int)sizeof tmp) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    char line[40];
    int l = snprintf(line, sizeof line, "%s\n", id);
    int ok = write(fd, line, (size_t)l) == l && !fsync(fd);
    close(fd);
    if (!ok || rename(tmp, op)) { unlink(tmp); return -1; }
    return 0;
}
static int dir_empty(const char *p) {
    DIR *d = opendir(p);
    if (!d) return 0;
    int empty = 1;
    for (struct dirent *e; empty && (e = readdir(d));) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) empty = 0;
    closedir(d);
    return empty;
}
/* xp (and its owner record) to "<name> (old YYYY-MM-DD)", " 2", " 3"… beside it; the new name in moved */
static int xfer_aside(const char *xp, char *moved, size_t mn) {
    char day[16], to[PATH_MAX], op[PATH_MAX], top2[PATH_MAX];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(day, sizeof day, "%Y-%m-%d", &tm);
    for (int i = 1; i < 100; i++) {
        int w = i == 1 ? snprintf(to, sizeof to, "%s (old %s)", xp, day) : snprintf(to, sizeof to, "%s (old %s) %d", xp, day, i);
        const char *b = strrchr(to, '/');
        if (w < 0 || (size_t)w >= sizeof to || !b || strlen(b + 1) + 9 > NAME_MAX) return -1;
        struct stat st;
        if (!lstat(to, &st) || errno != ENOENT) continue;
        if (rename(xp, to)) return -1;
        if (!xfer_owner_of(xp, op, sizeof op) && !xfer_owner_of(to, top2, sizeof top2)) rename(op, top2);
        if (moved && mn) snprintf(moved, mn, "%s", b + 1);
        return 0;
    }
    return -1;
}

/* "<name> (old YYYY-MM-DD)" or "<name> (old YYYY-MM-DD) N": a folder xfer_aside put aside, never taken back by itself */
static int xfer_aside_name(const char *s) {
    const char *o = strstr(s, " (old ");
    if (!o) return 0;
    for (const char *q; (q = strstr(o + 1, " (old "));) o = q;
    const char *d = o + 6;
    for (int i = 0; i < 10; i++) if (i == 4 || i == 7 ? d[i] != '-' : d[i] < '0' || d[i] > '9') return 0;
    if (d[10] != ')') return 0;
    const char *t = d + 11;
    if (!*t) return 1;
    if (*t++ != ' ' || !*t) return 0;
    for (; *t; t++) if (*t < '0' || *t > '9') return 0;
    return 1;
}

int vm_transfer_adopt(const char *dir, const char *id, char *from, size_t fn) {
    if (from && fn) from[0] = 0;
    char want[PATH_MAX], top[PATH_MAX], lib[PATH_MAX];
    if (!vm_uuid_valid(id) || vm_transfer_path(dir, want, sizeof want)) return -1;
    snprintf(top, sizeof top, "%s", want);
    char *sl = strrchr(top, '/');
    if (!sl) return -1;
    *sl = 0;
    const char *wname = want + (sl + 1 - top);
    snprintf(lib, sizeof lib, "%s", top);
    if (!(sl = strrchr(lib, '/'))) return -1;
    *sl = 0;
    struct stat st;
    if (!lstat(want, &st) || errno != ENOENT) return 1;          /* there already (or can't tell): nothing to take */
    DIR *d = opendir(top);
    if (!d) return 1;
    char found[NAME_MAX + 1] = "";
    int n = 0;
    for (struct dirent *e; (e = readdir(d));) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] != '.' || l < 10 || strcmp(e->d_name + l - 8, ".machine")) continue;
        char x[NAME_MAX + 1], p[PATH_MAX + 2 * NAME_MAX], who[37];
        snprintf(x, sizeof x, "%.*s", (int)(l - 9), e->d_name + 1);
        if (!x[0] || x[0] == '.' || !strcmp(x, wname) || xfer_aside_name(x)) continue;
        snprintf(p, sizeof p, "%s/%s", top, e->d_name);
        if (xfer_owner_read(p, who) || strcmp(who, id)) continue;
        snprintf(p, sizeof p, "%s/%s", top, x);
        if (lstat(p, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) continue;
        /* its machine folder still there: that machine's (a Duplicate keeps the id), not one that moved */
        snprintf(p, sizeof p, "%s/%s.lordvm", lib, x);
        if (!lstat(p, &st) || errno != ENOENT) continue;
        snprintf(found, sizeof found, "%s", x);
        n++;
    }
    closedir(d);
    if (n != 1) return 1;                                       /* none, or more than one: never a guess */
    char a[PATH_MAX + NAME_MAX + 2], oa[PATH_MAX], ow[PATH_MAX];
    snprintf(a, sizeof a, "%s/%s", top, found);
    if (xfer_owner_of(a, oa, sizeof oa) || xfer_owner_of(want, ow, sizeof ow)) return -1;
    if (renameat2(AT_FDCWD, a, AT_FDCWD, want, RENAME_NOREPLACE)) return -1;
    if (rename(oa, ow)) { if (rename(want, a)) {} return -1; }
    if (from && fn) snprintf(from, fn, "%s", found);
    return 0;
}

void vm_folder_name(const char *name, char *out, size_t n) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && o + 1 < n && o < 48; p++) {
        int ok = (*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'z') || *p == ' ' || *p == '-' || *p == '_' || (*p == '.' && o);
        if (*p >= 0x80) { while ((p[1] & 0xc0) == 0x80) p++; ok = 0; }
        char ch = ok ? (char)*p : '-';
        if (ch == '-' && o && out[o - 1] == '-') continue;
        out[o++] = ch;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '-' || out[o - 1] == '.')) o--;
    out[o] = 0;
    if (!o) snprintf(out, n, "Machine");
}

int vm_rename_name_ok(const char *s) { return vm_name_valid(s) && s[0] != '.'; }

int vm_rename_plan(const char *dir, const char *name, VmRenamePlan *p) {
    memset(p, 0, sizeof *p);
    char xp[PATH_MAX], lib[PATH_MAX], cur[NAME_MAX + 1];
    if (!vm_rename_name_ok(name)) return p->problem = VM_RN_BAD_NAME;
    if (vm_transfer_path(dir, xp, sizeof xp)) return p->problem = VM_RN_NOT_MACHINE;
    size_t l = strlen(dir);
    while (l > 1 && dir[l - 1] == '/') l--;
    const char *b = memrchr(dir, '/', l);
    snprintf(lib, sizeof lib, "%.*s", (int)(b - dir), dir);
    snprintf(cur, sizeof cur, "%.*s", (int)(dir + l - (b + 1) - 7), b + 1);
    /* the folder's name: kept while it already says the same (the same characters once made safe) */
    char now[64];
    vm_folder_name(cur, now, sizeof now);
    vm_folder_name(name, p->folder, sizeof p->folder);
    p->moves = strcmp(now, p->folder) != 0;
    if (!p->moves) snprintf(p->folder, sizeof p->folder, "%s", cur);
    DIR *d = opendir(lib);
    if (!d) return p->problem = VM_RN_NOT_MACHINE;
    char want[sizeof p->folder + 8];
    snprintf(want, sizeof want, "%s.lordvm", p->folder);
    for (struct dirent *e; (e = readdir(d));) {
        size_t el = strlen(e->d_name);
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (el == l - (size_t)(b + 1 - dir) && !strncmp(e->d_name, b + 1, el))) continue;
        if (p->moves && !p->problem && !strcasecmp(e->d_name, want)) p->problem = VM_RN_FOLDER_TAKEN;
        if (e->d_name[0] == '.' || el < 8 || strcmp(e->d_name + el - 7, ".lordvm")) continue;
        /* another machine called the same, ASCII case ignored (the machines command finds machines that way) */
        char od[PATH_MAX + NAME_MAX + 2];
        snprintf(od, sizeof od, "%s/%s", lib, e->d_name);
        VmMachine *o = malloc(sizeof *o);
        if (o && vm_machine_load(od, o, NULL, 0) == VM_OK && !strcasecmp(o->name, name)) { snprintf(p->other, sizeof p->other, "%s", o->name); p->problem = VM_RN_NAME_TAKEN; }
        free(o);
        if (p->problem == VM_RN_NAME_TAKEN) break;
    }
    closedir(d);
    if (p->problem || !p->moves) return p->problem;
    /* its transfer folder follows: a folder of the new name there must be its own already (or none) */
    char top[PATH_MAX + 16];
    snprintf(top, sizeof top, "%s/" VM_XFER_DIR, lib);
    if ((d = opendir(top))) {
        for (struct dirent *e; (e = readdir(d));) if (e->d_name[0] != '.' && strcmp(e->d_name, cur) && !strcasecmp(e->d_name, p->folder)) p->problem = VM_RN_TRANSFER_TAKEN;
        closedir(d);
    }
    return p->problem;
}

int vm_transfer_claim(const char *dir, const char *id, char *out, size_t n, char *moved, size_t mn) {
    if (moved && mn) moved[0] = 0;
    if (!vm_uuid_valid(id) || vm_transfer_path(dir, out, n)) return -1;
    char top[PATH_MAX], op[PATH_MAX], who[37];
    snprintf(top, sizeof top, "%s", out);
    char *sl = strrchr(top, '/');
    if (!sl) return -1;
    *sl = 0;
    struct stat st;
    if (mkdir(top, 0700) && errno != EEXIST) return -1;
    if (lstat(top, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) return -1;
    if (xfer_owner_of(out, op, sizeof op)) return -1;
    /* none of that name: this machine's own folder from before its folder was renamed (by hand, or a rename that stopped
     * half-way) comes along */
    vm_transfer_adopt(dir, id, NULL, 0);
    if (!lstat(out, &st)) {
        int mine = S_ISDIR(st.st_mode) && st.st_uid == geteuid();
        int owner = !xfer_owner_read(op, who);
        if (mine && owner && !strcmp(who, id)) return 0;
        /* a folder nobody claimed yet and empty: this machine's */
        if (mine && !owner && dir_empty(out)) return xfer_owner_write(op, id);
        /* another machine's (a machine of the same name, deleted or renamed, or made again): kept, beside it */
        if (xfer_aside(out, moved, mn)) return -1;
    } else if (errno != ENOENT) return -1;
    if (mkdir(out, 0700)) return -1;
    int d = open(out, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (d >= 0) { if (fchmod(d, 0777)) {} close(d); }
    return xfer_owner_write(op, id);
}

int vm_transfer_aside(const char *dir, const char *id, char *moved, size_t mn) {
    char xp[PATH_MAX], op[PATH_MAX], who[37];
    if (moved && mn) moved[0] = 0;
    if (!vm_uuid_valid(id) || vm_transfer_path(dir, xp, sizeof xp) || xfer_owner_of(xp, op, sizeof op)) return -1;
    struct stat st;
    if (lstat(xp, &st) || !S_ISDIR(st.st_mode)) return 1;
    if (xfer_owner_read(op, who) || strcmp(who, id)) return 1;          /* not this machine's */
    return xfer_aside(xp, moved, mn);
}

int vm_base_valid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    return n > 15 && path_valid(s) && strstr(s, "/.shared/") && !strchr(strstr(s, "/.shared/") + 9, '/') && !strcmp(s + n - 6, ".qcow2");
}

static const char *const PROTOS[] = { "tcp", "udp", NULL };
static const char *const OPENS[] = { "this-computer", "lan", NULL };

int vm_resolution_ok(int w, int h) {
    return w >= 640 && h >= 480 && w <= 7680 && h <= 4320 && !(w & 1) && !(h & 1);
}

int vm_resolution_parse(const char *s, int *w, int *h) {
    int a = 0, b = 0, k = 0;
    const char *p = s;
    for (; *p >= '0' && *p <= '9' && k < 5; p++, k++) a = a * 10 + (*p - '0');
    if (!k || *p != 'x') return -1;
    p++; k = 0;
    for (; *p >= '0' && *p <= '9' && k < 5; p++, k++) b = b * 10 + (*p - '0');
    if (!k || *p || !vm_resolution_ok(a, b)) return -1;
    *w = a; *h = b;
    return 0;
}

int vm_ifname_valid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > VM_IFNAME_MAX || !strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return 0;
    }
    return 1;
}

int vm_sound_dev_valid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > VM_SOUND_DEV_MAX) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x21 || (unsigned char)s[i] > 0x7e) return 0;
    return 1;
}

int vm_sound_devs_parse(const char *text, char *out, size_t on, char *in, size_t inn) {
    const char *tab = text ? strchr(text, '\t') : NULL;
    if (!tab || strchr(tab + 1, '\t')) return -1;
    size_t lo = (size_t)(tab - text), li = strlen(tab + 1);
    if (lo >= on || li >= inn || lo > VM_SOUND_DEV_MAX || li > VM_SOUND_DEV_MAX) return -1;
    char a[VM_SOUND_DEV_MAX + 1], b[VM_SOUND_DEV_MAX + 1];
    memcpy(a, text, lo); a[lo] = 0;
    memcpy(b, tab + 1, li); b[li] = 0;
    if ((a[0] && !vm_sound_dev_valid(a)) || (b[0] && !vm_sound_dev_valid(b))) return -1;
    memcpy(out, a, lo + 1);
    memcpy(in, b, li + 1);
    return 0;
}

int vm_ports_check(const VmPort *p, int cnt, int network, char *err, size_t n) {
    if (cnt < 0 || cnt > VM_PORTS) return fail(err, n, -1, "at most %d forwarded ports", VM_PORTS);
    for (int i = 0; i < cnt; i++) {
        if (p[i].proto != VM_PROTO_TCP && p[i].proto != VM_PROTO_UDP) return fail(err, n, -1, "ports: proto is tcp or udp");
        if (p[i].host < VM_PORT_HOST_MIN || p[i].host > 65535) return fail(err, n, -1, "ports: this computer's port must be %d to 65535", VM_PORT_HOST_MIN);
        if (p[i].guest < 1 || p[i].guest > 65535) return fail(err, n, -1, "ports: the machine's port must be 1 to 65535");
        if (p[i].open != VM_OPEN_LOCAL && p[i].open != VM_OPEN_LAN) return fail(err, n, -1, "ports: open is this-computer or lan");
        if (p[i].open == VM_OPEN_LAN && network == VM_NET_HOSTONLY) return fail(err, n, -1, "ports: a host-only machine opens ports to this computer only");
        for (int k = 0; k < i; k++)
            if (p[k].proto == p[i].proto && p[k].host == p[i].host) return fail(err, n, -1, "ports: %s port %d of this computer twice", p[i].proto ? "udp" : "tcp", p[i].host);
    }
    return 0;
}

int vm_port_parse(const char *s, VmPort *p) {
    char proto[4], open[16] = "this-computer", tail;
    int h, g, k = s ? sscanf(s, "%3[a-z]:%d:%d:%15[a-z-]%c", proto, &h, &g, open, &tail) : 0;
    if (k != 3 && k != 4) return -1;
    if (strlen(s) > 40) return -1;
    memset(p, 0, sizeof *p);
    if (!strcmp(proto, "tcp")) p->proto = VM_PROTO_TCP; else if (!strcmp(proto, "udp")) p->proto = VM_PROTO_UDP; else return -1;
    if (!strcmp(open, "this-computer")) p->open = VM_OPEN_LOCAL; else if (!strcmp(open, "lan")) p->open = VM_OPEN_LAN; else return -1;
    p->host = h; p->guest = g;
    /* digits only between the colons: sscanf's %d took signs and spaces too */
    for (const char *c = s + strlen(proto) + 1; *c && *c != ':' ; c++) if (*c < '0' || *c > '9') return -1;
    const char *c2 = strchr(s + strlen(proto) + 1, ':');
    for (const char *c = c2 + 1; *c && *c != ':'; c++) if (*c < '0' || *c > '9') return -1;
    return vm_ports_check(p, 1, VM_NET_SHARED, NULL, 0);
}

void vm_port_spec(const VmPort *p, char *out, size_t n) { snprintf(out, n, "%s:%d:%d:%s", PROTOS[p->proto & 1], p->host, p->guest, OPENS[p->open & 1]); }

int vm_passt_ports(const VmMachine *m, int proto, char out[][48], int max) {
    int k = 0;
    for (int i = 0; i < m->n_ports && k < max; i++) {
        const VmPort *p = &m->ports[i];
        if (p->proto != proto) continue;
        /* host-only machines never listen beyond loopback, whatever the file says */
        int lan = p->open == VM_OPEN_LAN && m->network != VM_NET_HOSTONLY;
        snprintf(out[k++], 48, "%s/%d:%d", lan ? "0.0.0.0" : "127.0.0.1", p->host, p->guest);
    }
    if (!k && max > 0) snprintf(out[k++], 48, "none");
    return k;
}

int vm_bridge_ifname(const char *id, char out[VM_IFNAME_MAX + 1]) {
    if (!vm_uuid_valid(id)) return -1;
    snprintf(out, VM_IFNAME_MAX + 1, "lvm%.8s%.4s", id, id + 9);
    return 0;
}

int vm_machine_check(const VmMachine *m, char *err, size_t n) {
    if (m->schema < 1 || m->schema > VM_SCHEMA) return fail(err, n, m->schema > VM_SCHEMA ? VM_E_NEWER : VM_E_SCHEMA, "schema %d", m->schema);
    if (!vm_uuid_valid(m->id)) return fail(err, n, VM_E_RANGE, "id is not a lowercase UUID");
    if (!vm_name_valid(m->name)) return fail(err, n, VM_E_RANGE, "name must be 1 to %d bytes of text, without '/' or control characters", VM_NAME_MAX);
    if (m->arch != VM_ARCH_ARM64 && m->arch != VM_ARCH_AMD64) return fail(err, n, VM_E_RANGE, "arch");
    if (m->accel != VM_ACCEL_KVM && m->accel != VM_ACCEL_TCG) return fail(err, n, VM_E_RANGE, "accel");
    if (m->vcpus < 1 || m->vcpus > VM_VCPUS_MAX) return fail(err, n, VM_E_RANGE, "vcpus must be 1 to %d", VM_VCPUS_MAX);
    if (m->memory_mb < VM_MEM_MIN_MB || m->memory_mb > VM_MEM_MAX_MB) return fail(err, n, VM_E_RANGE, "memory_mb must be %d to %d", VM_MEM_MIN_MB, VM_MEM_MAX_MB);
    if (m->disk_gb < VM_DISK_MIN_GB || m->disk_gb > VM_DISK_MAX_GB) return fail(err, n, VM_E_RANGE, "disk_gb must be %d to %d", VM_DISK_MIN_GB, VM_DISK_MAX_GB);
    if (m->firmware != VM_FW_UEFI && m->firmware != VM_FW_BIOS) return fail(err, n, VM_E_RANGE, "firmware");
    if (m->arch == VM_ARCH_ARM64 && m->firmware != VM_FW_UEFI) return fail(err, n, VM_E_RANGE, "arm64 machines boot with UEFI only");
    if (!machine_type_valid(m->machine_type, m->arch)) return fail(err, n, VM_E_RANGE, "machine_type must be a versioned %s type", m->arch == VM_ARCH_ARM64 ? "virt" : "pc-q35");
    if (m->firmware == VM_FW_UEFI) {
        if (!path_valid(m->firmware_code) || strncmp(m->firmware_code, "/usr/share/", 11)) return fail(err, n, VM_E_RANGE, "firmware_code must be a file under /usr/share");
        if (strlen(m->firmware_sha256) != 64) return fail(err, n, VM_E_RANGE, "firmware_sha256");
        for (int i = 0; i < 64; i++) { char c = m->firmware_sha256[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return fail(err, n, VM_E_RANGE, "firmware_sha256"); }
    } else if (m->firmware_code[0] || m->firmware_sha256[0]) return fail(err, n, VM_E_RANGE, "a bios machine has no firmware_code");
    if (m->display != VM_DISPLAY_WINDOW && m->display != VM_DISPLAY_NONE) return fail(err, n, VM_E_RANGE, "display");
    if (m->network < VM_NET_SHARED || m->network > VM_NET_HOSTONLY) return fail(err, n, VM_E_RANGE, "network");
    if (m->bridge[0] && !vm_ifname_valid(m->bridge)) return fail(err, n, VM_E_RANGE, "bridge must be a network interface's name");
    if (m->network == VM_NET_BRIDGED && !m->bridge[0]) return fail(err, n, VM_E_RANGE, "a bridged machine names the wired card it joins (bridge)");
    if (vm_ports_check(m->ports, m->n_ports, m->network, err, n)) return VM_E_RANGE;
    if (m->schema < 6 && (m->network > VM_NET_ISOLATED || m->bridge[0] || m->n_ports)) return fail(err, n, VM_E_FIELD, "the bridged and host-only networks, bridge and ports need schema 6");
    if (!vm_mac_valid(m->mac)) return fail(err, n, VM_E_RANGE, "mac must be 52:54:00:xx:xx:xx, lowercase");
    if (m->iso[0] && !path_valid(m->iso)) return fail(err, n, VM_E_RANGE, "iso must be an absolute path without '..', ',' or control characters");
    if (!iso_time_valid(m->created) || !iso_time_valid(m->updated)) return fail(err, n, VM_E_RANGE, "created and updated are UTC times like 2026-11-04T20:00:00Z");
    if (m->base[0] && !vm_base_valid(m->base)) return fail(err, n, VM_E_RANGE, "base must be a .qcow2 in a .shared folder");
    if (m->agent != 0 && m->agent != 1) return fail(err, n, VM_E_RANGE, "agent");
    if (m->clipboard < VM_CLIP_OFF || m->clipboard > VM_CLIP_BOTH) return fail(err, n, VM_E_RANGE, "clipboard");
    if (m->schema < 2 && (m->base[0] || m->agent || m->clipboard)) return fail(err, n, VM_E_FIELD, "base, agent and clipboard need schema 2");
    if (m->devices != VM_DEV_VIRTIO && m->devices != VM_DEV_COMPAT && m->devices != VM_DEV_WINDOWS) return fail(err, n, VM_E_RANGE, "devices");
    if (m->nic_model < 0 || m->nic_model >= VM_NIC_COUNT) return fail(err, n, VM_E_RANGE, "nic_model");
    if (m->n_extra_macs < 0 || m->n_extra_macs > VM_EXTRA_NICS) return fail(err, n, VM_E_RANGE, "at most %d extra_nics", VM_EXTRA_NICS);
    for (int i = 0; i < m->n_extra_macs; i++) {
        if (!vm_mac_valid(m->extra_macs[i])) return fail(err, n, VM_E_RANGE, "extra_nics: a lowercase unicast MAC address");
        if (m->extra_models[i] < 0 || m->extra_models[i] >= VM_NIC_COUNT) return fail(err, n, VM_E_RANGE, "extra_nics: model");
        if (!strcmp(m->extra_macs[i], m->mac)) return fail(err, n, VM_E_RANGE, "extra_nics: the same address twice");
        for (int k = 0; k < i; k++) if (!strcmp(m->extra_macs[i], m->extra_macs[k])) return fail(err, n, VM_E_RANGE, "extra_nics: the same address twice");
    }
    if (m->cpu_sockets || m->cpu_cores || m->cpu_threads) {
        if (m->cpu_sockets < 1 || m->cpu_cores < 1 || m->cpu_threads < 1 || (long)m->cpu_sockets * m->cpu_cores * m->cpu_threads != m->vcpus)
            return fail(err, n, VM_E_RANGE, "cpu_sockets × cpu_cores × cpu_threads must be vcpus");
    }
    const char *sm[] = { m->sm_manufacturer, m->sm_product, m->sm_version, m->sm_serial, m->sm_sku, m->sm_family };
    for (int i = 0; i < 6; i++) if (!vm_smbios_valid(sm[i])) return fail(err, n, VM_E_RANGE, "smbios: printable ASCII, at most %d bytes", VM_SMBIOS_MAX);
    if (!vm_serial_valid(m->disk_serial)) return fail(err, n, VM_E_RANGE, "disk_serial: letters, digits, '-', '_', '.', at most %d", VM_SERIAL_MAX);
    if (m->n_data < 0 || m->n_data > VM_DATA_DISKS) return fail(err, n, VM_E_RANGE, "at most %d data_disks", VM_DATA_DISKS);
    for (int i = 0; i < m->n_data; i++) {
        if (m->data_gb[i] < VM_DISK_MIN_GB || m->data_gb[i] > VM_DISK_MAX_GB) return fail(err, n, VM_E_RANGE, "data_disks: gb");
        if (!vm_serial_valid(m->data_serial[i])) return fail(err, n, VM_E_RANGE, "data_disks: serial");
    }
    if (m->n_data && m->base[0]) return fail(err, n, VM_E_RANGE, "a linked clone has no data disks");
    if (m->rtc != VM_RTC_UTC && m->rtc != VM_RTC_LOCAL) return fail(err, n, VM_E_RANGE, "rtc");
    if (m->n_shares < 0 || m->n_shares > VM_SHARES) return fail(err, n, VM_E_RANGE, "at most %d shares", VM_SHARES);
    for (int i = 0; i < m->n_shares; i++) {
        if (!vm_tag_valid(m->share_tag[i])) return fail(err, n, VM_E_RANGE, "shares: tag is 1 to %d letters, digits, '-' or '_'", VM_TAG_MAX);
        if (!path_valid(m->share_path[i])) return fail(err, n, VM_E_RANGE, "shares: path must be an absolute path without '..', ',' or control characters");
        if (m->share_rw[i] != 0 && m->share_rw[i] != 1) return fail(err, n, VM_E_RANGE, "shares: writable");
        for (int k = 0; k < i; k++) {
            if (!strcmp(m->share_tag[i], m->share_tag[k])) return fail(err, n, VM_E_RANGE, "shares: the same tag twice");
            if (!strcmp(m->share_path[i], m->share_path[k])) return fail(err, n, VM_E_RANGE, "shares: the same folder twice");
        }
    }
    if (m->schema < 4 && m->n_shares) return fail(err, n, VM_E_FIELD, "shares need schema 4");
    if (m->drivers[0] && !path_valid(m->drivers)) return fail(err, n, VM_E_RANGE, "drivers must be an absolute path without '..', ',' or control characters");
    if (m->schema < 5 && (m->devices == VM_DEV_WINDOWS || m->drivers[0])) return fail(err, n, VM_E_FIELD, "the windows devices and drivers need schema 5");
    if ((m->devices == VM_DEV_WINDOWS || m->drivers[0]) && m->arch != VM_ARCH_AMD64) return fail(err, n, VM_E_RANGE, "the windows devices are for amd64 machines for now");
    if ((m->secure_boot | m->tpm | m->hyperv) & ~1) return fail(err, n, VM_E_RANGE, "secure_boot, tpm and hyperv are true or false");
    if (m->schema < 5 && (m->secure_boot || m->tpm || m->hyperv)) return fail(err, n, VM_E_FIELD, "secure_boot, tpm and hyperv need schema 5");
    if ((m->secure_boot || m->tpm || m->hyperv) && m->arch != VM_ARCH_AMD64) return fail(err, n, VM_E_RANGE, "secure boot, TPM and Hyper-V are for amd64 machines for now");
    if (m->secure_boot && m->firmware != VM_FW_UEFI) return fail(err, n, VM_E_RANGE, "secure boot needs UEFI");
    if (m->graphics != VM_GFX_2D && m->graphics != VM_GFX_3D) return fail(err, n, VM_E_RANGE, "graphics");
    if ((m->res_w || m->res_h) && !vm_resolution_ok(m->res_w, m->res_h)) return fail(err, n, VM_E_RANGE, "resolution must be 640x480 to 7680x4320, even numbers");
    if (m->schema < 5 && m->graphics) return fail(err, n, VM_E_FIELD, "graphics needs schema 5");
    if (m->graphics == VM_GFX_3D && m->devices == VM_DEV_WINDOWS) return fail(err, n, VM_E_RANGE, "3D graphics are for Linux machines (the windows devices stay 2D)");
    if (m->graphics == VM_GFX_3D && m->arch != VM_ARCH_AMD64) return fail(err, n, VM_E_RANGE, "3D graphics are for amd64 machines for now");
    if ((m->sound | m->microphone) & ~1) return fail(err, n, VM_E_RANGE, "sound and microphone are true or false");
    if (m->volume < 0 || m->volume > 100) return fail(err, n, VM_E_RANGE, "volume must be 0 to 100");
    if (m->schema < 6 && (!m->sound || m->volume != 100 || m->microphone)) return fail(err, n, VM_E_FIELD, "sound, volume and microphone need schema 6");
    if (m->n_boot < 0 || m->n_boot > VM_BOOT_DEVS || (m->boot_menu & ~1)) return fail(err, n, VM_E_RANGE, "boot_order: at most %d devices; boot_menu is true or false", VM_BOOT_DEVS);
    for (int i = 0; i < m->n_boot; i++) {
        if (m->boot_order[i] < 0 || m->boot_order[i] >= VM_BOOT_DEVS) return fail(err, n, VM_E_RANGE, "boot_order: disk, cd or network");
        for (int k = 0; k < i; k++) if (m->boot_order[i] == m->boot_order[k]) return fail(err, n, VM_E_RANGE, "boot_order: the same device twice");
    }
    if (m->schema < 6 && (m->n_boot || m->boot_menu)) return fail(err, n, VM_E_FIELD, "boot_order and boot_menu need schema 6");
    if ((m->sound_out[0] && !vm_sound_dev_valid(m->sound_out)) || (m->sound_in[0] && !vm_sound_dev_valid(m->sound_in)))
        return fail(err, n, VM_E_RANGE, "sound_out and sound_in are a sound device's name: up to %d printable characters, no spaces", VM_SOUND_DEV_MAX);
    if (m->schema < 8 && (m->sound_out[0] || m->sound_in[0])) return fail(err, n, VM_E_FIELD, "sound_out and sound_in need schema 8");
    if (m->transfer & ~1) return fail(err, n, VM_E_RANGE, "transfer is true or false");
    if (m->clipboard_ask & ~1) return fail(err, n, VM_E_RANGE, "clipboard_ask is true or false");
    if (m->schema < 9 && m->clipboard_ask) return fail(err, n, VM_E_FIELD, "clipboard_ask needs schema 9");
    if (m->pointer_guest & ~1) return fail(err, n, VM_E_RANGE, "pointer is lordos or guest");
    if (m->vgpu < VM_VGPU_NONE || m->vgpu > VM_VGPU_SHARED) return fail(err, n, VM_E_RANGE, "gpu is intel-whole or intel-shared");
    if (m->schema < 11 && m->vgpu) return fail(err, n, VM_E_FIELD, "gpu needs schema 11");
    if (m->vgpu && (m->arch != VM_ARCH_AMD64 || m->firmware != VM_FW_UEFI)) return fail(err, n, VM_E_RANGE, "an Intel vGPU is for amd64 machines with UEFI");
    if (m->vgpu && m->graphics == VM_GFX_3D) return fail(err, n, VM_E_RANGE, "an Intel vGPU replaces 3D graphics: choose one");
    if (m->schema < 10 && m->pointer_guest) return fail(err, n, VM_E_FIELD, "pointer needs schema 10");
    if (m->capture_fullscreen & ~1) return fail(err, n, VM_E_RANGE, "capture_fullscreen is true or false");
    if (m->schema < 12 && m->capture_fullscreen) return fail(err, n, VM_E_FIELD, "capture_fullscreen needs schema 12");
    if (m->schema < 7 && m->transfer) return fail(err, n, VM_E_FIELD, "transfer needs schema 7");
    for (int i = 0; m->transfer && i < m->n_shares; i++)
        if (!strcmp(m->share_tag[i], VM_XFER_TAG)) return fail(err, n, VM_E_RANGE, "shares: the tag " VM_XFER_TAG " is the transfer folder's");
    if (m->schema < 3 && (m->devices || m->nic_model || m->n_extra_macs || m->cpu_sockets || m->sm_manufacturer[0] || m->sm_product[0] || m->sm_version[0] ||
                          m->sm_serial[0] || m->sm_sku[0] || m->sm_family[0] || m->disk_serial[0] || m->n_data || m->rtc))
        return fail(err, n, VM_E_FIELD, "devices, nic_model, extra_nics, cpu topology, smbios, serials, data_disks and rtc need schema 3");
    return VM_OK;
}

int vm_vcpus_recommended(const VmHost *h) { return h->cpus > 1 ? h->cpus - 1 : 1; }
long vm_mem_max_mb(const VmHost *h) {
    long floor = h->ram_mb <= 4096 ? VM_FLOOR_SMALL_MB : VM_FLOOR_MB;
    return h->ram_mb > floor ? h->ram_mb - floor : 0;
}
long vm_mem_limit_mb(const VmMachine *m) { return (long)m->memory_mb + (m->accel == VM_ACCEL_TCG ? VM_OVERHEAD_TCG_MB : VM_OVERHEAD_KVM_MB); }
int vm_budget_fits(const VmHost *h, long running_mb, long want_mb) {
    long floor = h->ram_mb <= 4096 ? VM_FLOOR_SMALL_MB : VM_FLOOR_MB;
    return running_mb >= 0 && want_mb > 0 && running_mb + want_mb <= h->ram_mb - floor;
}

const char *vm_gl_refusal(const VmMachine *m, int host_arch, int installing) {
    if (m->devices == VM_DEV_WINDOWS) return "3D graphics are for Linux machines";
    if (m->display != VM_DISPLAY_WINDOW) return "the machine has no display";
    if (m->arch != VM_ARCH_AMD64 || host_arch != VM_ARCH_AMD64) return "3D graphics are for amd64 machines on amd64 computers for now";
    if (installing) return "this start installs the system, a text console (and QEMU's 3D display aborts at the installer's restart): 3D comes at the next start";
    return NULL;
}

int vm_machine_check_host(const VmMachine *m, const VmHost *h, char *err, size_t n, char *warn, size_t wn) {
    if (warn && wn) warn[0] = 0;
    if (m->accel == VM_ACCEL_KVM) {
        if (h->arch != m->arch) return fail(err, n, VM_E_HOST, "KVM runs %s machines on a %s computer only; use emulation (tcg)", vm_arch_name(m->arch), vm_arch_name(m->arch));
        if (!h->kvm) return fail(err, n, VM_E_HOST, "this computer's /dev/kvm can't be opened (virtualization off, or the user isn't allowed)");
    }
    if (m->vcpus > h->cpus) return fail(err, n, VM_E_HOST, "%d vCPUs, this computer has %d", m->vcpus, h->cpus);
    long mx = vm_mem_max_mb(h);
    if (m->memory_mb > mx) return fail(err, n, VM_E_HOST, "%d MB of memory, at most %ld MB here (the desktop keeps %d MB)", m->memory_mb, mx, h->ram_mb <= 4096 ? VM_FLOOR_SMALL_MB : VM_FLOOR_MB);
    if (warn && wn && m->vcpus > vm_vcpus_recommended(h))
        snprintf(warn, wn, "%d vCPUs on a %d-CPU computer: the desktop slows down when all are busy; %d recommended", m->vcpus, h->cpus, vm_vcpus_recommended(h));
    return VM_OK;
}

/* ---- JSON ---- */

static const char *const KEYS[] = {
    "schema", "id", "name", "arch", "accel", "vcpus", "memory_mb", "disk_gb", "firmware", "machine_type",
    "firmware_code", "firmware_sha256", "display", "network", "mac", "iso", "created", "updated", "base", "agent", "clipboard",
    "devices", "nic_model", "extra_nics", "cpu_sockets", "cpu_cores", "cpu_threads", "smbios", "disk_serial", "data_disks", "rtc",
    "shares", "secure_boot", "tpm", "hyperv", "drivers", "graphics", "bridge", "ports", "sound", "volume", "microphone", "resolution",
    "boot_order", "boot_menu", "transfer", "sound_out", "sound_in", "clipboard_ask", "pointer", "gpu", "capture_fullscreen", NULL,
};

static int get_str(const JVal *o, const char *k, char *out, size_t cap, int required, char *err, size_t n) {
    const JVal *v = json_get(o, k);
    if (!v) { if (required) return fail(err, n, VM_E_FIELD, "%s is missing", k); out[0] = 0; return VM_OK; }
    if (v->type != J_STR) return fail(err, n, VM_E_FIELD, "%s must be text", k);
    if (v->len >= cap || strlen(v->str) != v->len) return fail(err, n, VM_E_RANGE, "%s is too long", k);
    memcpy(out, v->str, v->len + 1);
    return VM_OK;
}

static int get_int(const JVal *o, const char *k, int *out, char *err, size_t n) {
    const JVal *v = json_get(o, k);
    if (!v) return fail(err, n, VM_E_FIELD, "%s is missing", k);
    if (v->type != J_NUM || v->num != (double)(long long)v->num || v->num < -2147483647.0 || v->num > 2147483647.0)
        return fail(err, n, VM_E_FIELD, "%s must be a whole number", k);
    *out = (int)v->num;
    return VM_OK;
}

static int get_enum(const JVal *o, const char *k, const char *const *names, int *out, char *err, size_t n) {
    char s[32];
    int r = get_str(o, k, s, sizeof s, 1, err, n);
    if (r) return r;
    for (int i = 0; names[i]; i++) if (!strcmp(s, names[i])) { *out = i; return VM_OK; }
    return fail(err, n, VM_E_RANGE, "%s: \"%s\" isn't one of the accepted values", k, s);
}

static const char *const ARCHS[] = { "arm64", "amd64", NULL };
static const char *const ACCELS[] = { "kvm", "tcg", NULL };
static const char *const FWS[] = { "uefi", "bios", NULL };
static const char *const DISPLAYS[] = { "window", "none", NULL };
static const char *const NETS[] = { "shared", "isolated", "bridged", "hostonly", NULL };
const char *const VM_NET_NAMES[] = { "shared", "isolated", "bridged", "hostonly", NULL };

static const char *const CLIPS[] = { "off", "to-guest", "from-guest", "both", NULL };
static const char *const DEVS[] = { "virtio", "compat", "windows", NULL };
const char *const VM_DEV_NAMES[] = { "virtio", "compat", "windows", NULL };
const char *const VM_VGPU_NAMES[] = { "", "intel-whole", "intel-shared", NULL };
const char *const VM_BOOT_NAMES[] = { "disk", "cd", "network", NULL };

int vm_boot_parse(const char *s, int order[VM_BOOT_DEVS]) {
    if (!s) return -1;
    if (!strcmp(s, "auto")) return 0;
    int n = 0;
    for (const char *p = s;;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        int d = -1;
        for (int i = 0; VM_BOOT_NAMES[i]; i++) if (strlen(VM_BOOT_NAMES[i]) == len && !strncmp(p, VM_BOOT_NAMES[i], len)) d = i;
        if (d < 0 || n >= VM_BOOT_DEVS) return -1;
        for (int k = 0; k < n; k++) if (order[k] == d) return -1;
        order[n++] = d;
        if (!e) break;
        p = e + 1;
    }
    return n;
}

void vm_boot_spec(const VmMachine *m, char *out, size_t n) {
    if (!n) return;
    snprintf(out, n, "%s", m->n_boot ? "" : "auto");
    for (int i = 0; i < m->n_boot && m->boot_order[i] >= 0 && m->boot_order[i] < VM_BOOT_DEVS; i++)
        snprintf(out + strlen(out), n - strlen(out), "%s%s", i ? "," : "", VM_BOOT_NAMES[m->boot_order[i]]);
}

void vm_boot_indexes(const VmMachine *m, int first, int idx[VM_BOOT_DEVS]) {
    for (int i = 0; i < VM_BOOT_DEVS; i++) idx[i] = -1;
    if (!m->n_boot) { idx[VM_BOOT_DISK] = first; idx[VM_BOOT_CD] = first + 1; return; }
    for (int i = 0; i < m->n_boot && i < VM_BOOT_DEVS; i++)
        if (m->boot_order[i] >= 0 && m->boot_order[i] < VM_BOOT_DEVS) idx[m->boot_order[i]] = first + i;
}
static const char *const RTCS[] = { "utc", "local", NULL };
static const char *const SMB_KEYS[] = { "manufacturer", "product", "version", "serial", "sku", "family", NULL };

static int get_opt_int(const JVal *o, const char *k, int *out, char *err, size_t n) { return json_get(o, k) ? get_int(o, k, out, err, n) : VM_OK; }

/* schema 3's keys */
static int parse3(const JVal *o, VmMachine *m, char *err, size_t n) {
    int r;
    if (json_get(o, "devices") && (r = get_enum(o, "devices", DEVS, &m->devices, err, n))) return r;
    if (json_get(o, "nic_model") && (r = get_enum(o, "nic_model", VM_NIC_MODELS, &m->nic_model, err, n))) return r;
    if (json_get(o, "rtc") && (r = get_enum(o, "rtc", RTCS, &m->rtc, err, n))) return r;
    if ((r = get_opt_int(o, "cpu_sockets", &m->cpu_sockets, err, n)) || (r = get_opt_int(o, "cpu_cores", &m->cpu_cores, err, n)) ||
        (r = get_opt_int(o, "cpu_threads", &m->cpu_threads, err, n)) || (r = get_str(o, "disk_serial", m->disk_serial, sizeof m->disk_serial, 0, err, n)))
        return r;
    const JVal *x = json_get(o, "extra_nics");
    if (x) {
        if (x->type != J_ARR) return fail(err, n, VM_E_FIELD, "extra_nics must be a list");
        if (x->len > VM_EXTRA_NICS) return fail(err, n, VM_E_RANGE, "at most %d extra_nics", VM_EXTRA_NICS);
        for (const JVal *e = x->kid; e; e = e->next) {
            if (e->type != J_OBJ) return fail(err, n, VM_E_FIELD, "extra_nics: objects");
            for (const JVal *k = e->kid; k; k = k->next) if (strcmp(k->key, "mac") && strcmp(k->key, "model")) return fail(err, n, VM_E_FIELD, "extra_nics: unknown key");
            int i = m->n_extra_macs++;
            if ((r = get_str(e, "mac", m->extra_macs[i], sizeof m->extra_macs[i], 1, err, n)) || (r = get_enum(e, "model", VM_NIC_MODELS, &m->extra_models[i], err, n))) return r;
        }
    }
    const JVal *s = json_get(o, "smbios");
    if (s) {
        if (s->type != J_OBJ) return fail(err, n, VM_E_FIELD, "smbios must be an object");
        char *dst[] = { m->sm_manufacturer, m->sm_product, m->sm_version, m->sm_serial, m->sm_sku, m->sm_family };
        for (const JVal *e = s->kid; e; e = e->next) {
            int k = -1;
            for (int i = 0; SMB_KEYS[i]; i++) if (!strcmp(e->key, SMB_KEYS[i])) k = i;
            if (k < 0) return fail(err, n, VM_E_FIELD, "smbios: unknown key \"%.20s\"", e->key);
            if (e->type != J_STR || e->len > VM_SMBIOS_MAX || strlen(e->str) != e->len) return fail(err, n, VM_E_RANGE, "smbios: text of at most %d bytes", VM_SMBIOS_MAX);
            memcpy(dst[k], e->str, e->len + 1);
        }
    }
    const JVal *d = json_get(o, "data_disks");
    if (d) {
        if (d->type != J_ARR) return fail(err, n, VM_E_FIELD, "data_disks must be a list");
        if (d->len > VM_DATA_DISKS) return fail(err, n, VM_E_RANGE, "at most %d data_disks", VM_DATA_DISKS);
        for (const JVal *e = d->kid; e; e = e->next) {
            if (e->type != J_OBJ) return fail(err, n, VM_E_FIELD, "data_disks: objects");
            for (const JVal *k = e->kid; k; k = k->next) if (strcmp(k->key, "gb") && strcmp(k->key, "serial")) return fail(err, n, VM_E_FIELD, "data_disks: unknown key");
            int i = m->n_data++;
            if ((r = get_int(e, "gb", &m->data_gb[i], err, n)) || (r = get_str(e, "serial", m->data_serial[i], sizeof m->data_serial[i], 0, err, n))) return r;
        }
    }
    return VM_OK;
}

/* schema 4's keys */
static int parse4(const JVal *o, VmMachine *m, char *err, size_t n) {
    const JVal *x = json_get(o, "shares");
    if (!x) return VM_OK;
    if (x->type != J_ARR) return fail(err, n, VM_E_FIELD, "shares must be a list");
    if (x->len > VM_SHARES) return fail(err, n, VM_E_RANGE, "at most %d shares", VM_SHARES);
    for (const JVal *e = x->kid; e; e = e->next) {
        if (e->type != J_OBJ) return fail(err, n, VM_E_FIELD, "shares: objects");
        for (const JVal *k = e->kid; k; k = k->next)
            if (strcmp(k->key, "tag") && strcmp(k->key, "path") && strcmp(k->key, "writable")) return fail(err, n, VM_E_FIELD, "shares: unknown key");
        int i = m->n_shares++, r;
        if ((r = get_str(e, "tag", m->share_tag[i], sizeof m->share_tag[i], 1, err, n)) || (r = get_str(e, "path", m->share_path[i], sizeof m->share_path[i], 1, err, n))) return r;
        const JVal *w = json_get(e, "writable");
        if (w && w->type != J_BOOL) return fail(err, n, VM_E_FIELD, "shares: writable must be true or false");
        m->share_rw[i] = w ? json_bool(e, "writable", 0) : 0;
    }
    return VM_OK;
}

/* schema 6's keys: bridge and ports (6.5) */
static int parse6(const JVal *o, VmMachine *m, char *err, size_t n) {
    int r;
    if ((r = get_str(o, "bridge", m->bridge, sizeof m->bridge, 0, err, n))) return r;
    const JVal *bo = json_get(o, "boot_order"), *bm = json_get(o, "boot_menu");
    if (bo) {
        if (bo->type != J_ARR) return fail(err, n, VM_E_FIELD, "boot_order must be a list");
        if (!bo->len || bo->len > VM_BOOT_DEVS) return fail(err, n, VM_E_RANGE, "boot_order: 1 to %d devices", VM_BOOT_DEVS);
        for (const JVal *e = bo->kid; e; e = e->next) {
            int d = -1;
            if (e->type == J_STR) for (int i = 0; VM_BOOT_NAMES[i]; i++) if (!strcmp(e->str, VM_BOOT_NAMES[i])) d = i;
            if (d < 0) return fail(err, n, VM_E_RANGE, "boot_order: disk, cd or network");
            m->boot_order[m->n_boot++] = d;
        }
    }
    if (bm && bm->type != J_BOOL) return fail(err, n, VM_E_FIELD, "boot_menu must be true or false");
    m->boot_menu = bm ? json_bool(o, "boot_menu", 0) : 0;
    const JVal *x = json_get(o, "ports");
    if (!x) return VM_OK;
    if (x->type != J_ARR) return fail(err, n, VM_E_FIELD, "ports must be a list");
    if (x->len > VM_PORTS) return fail(err, n, VM_E_RANGE, "at most %d ports", VM_PORTS);
    for (const JVal *e = x->kid; e; e = e->next) {
        if (e->type != J_OBJ) return fail(err, n, VM_E_FIELD, "ports: objects");
        for (const JVal *k = e->kid; k; k = k->next)
            if (strcmp(k->key, "proto") && strcmp(k->key, "host") && strcmp(k->key, "guest") && strcmp(k->key, "open")) return fail(err, n, VM_E_FIELD, "ports: unknown key");
        VmPort *p = &m->ports[m->n_ports++];
        if ((r = get_enum(e, "proto", PROTOS, &p->proto, err, n)) || (r = get_int(e, "host", &p->host, err, n)) || (r = get_int(e, "guest", &p->guest, err, n))) return r;
        p->open = VM_OPEN_LOCAL;
        if (json_get(e, "open") && (r = get_enum(e, "open", OPENS, &p->open, err, n))) return r;
    }
    return VM_OK;
}

int vm_machine_parse(const char *text, size_t len, VmMachine *m, char *err, size_t n) {
    memset(m, 0, sizeof *m);
    if (len > 64 * 1024) return fail(err, n, VM_E_JSON, "machine.json is larger than 64 KB");
    JDoc d;
    char jerr[128];
    if (json_parse(text, len, &d, jerr, sizeof jerr)) return fail(err, n, VM_E_JSON, "not JSON: %s", jerr);
    int r = VM_OK;
    const JVal *o = d.root;
    if (o->type != J_OBJ) { r = fail(err, n, VM_E_JSON, "not a JSON object"); goto out; }
    /* schema first: a newer file is refused before its other keys are judged */
    if ((r = get_int(o, "schema", &m->schema, err, n))) { r = VM_E_SCHEMA; goto out; }
    if (m->schema > VM_SCHEMA) { r = fail(err, n, VM_E_NEWER, "schema %d is newer than this LordOS (%d): opened read-only", m->schema, VM_SCHEMA); goto out; }
    if (m->schema < 1) { r = fail(err, n, VM_E_SCHEMA, "schema %d", m->schema); goto out; }
    for (const JVal *k = o->kid; k; k = k->next) {
        int known = 0;
        for (int i = 0; KEYS[i]; i++) if (!strcmp(k->key, KEYS[i])) known = 1;
        if (!known) { r = fail(err, n, VM_E_FIELD, "unknown key \"%.40s\"", k->key); goto out; }
    }
    if ((r = get_str(o, "id", m->id, sizeof m->id, 1, err, n)) || (r = get_str(o, "name", m->name, sizeof m->name, 1, err, n)) ||
        (r = get_enum(o, "arch", ARCHS, &m->arch, err, n)) || (r = get_enum(o, "accel", ACCELS, &m->accel, err, n)) ||
        (r = get_int(o, "vcpus", &m->vcpus, err, n)) || (r = get_int(o, "memory_mb", &m->memory_mb, err, n)) ||
        (r = get_int(o, "disk_gb", &m->disk_gb, err, n)) || (r = get_enum(o, "firmware", FWS, &m->firmware, err, n)) ||
        (r = get_str(o, "machine_type", m->machine_type, sizeof m->machine_type, 1, err, n)) ||
        (r = get_str(o, "firmware_code", m->firmware_code, sizeof m->firmware_code, 0, err, n)) ||
        (r = get_str(o, "firmware_sha256", m->firmware_sha256, sizeof m->firmware_sha256, 0, err, n)) ||
        (r = get_enum(o, "display", DISPLAYS, &m->display, err, n)) || (r = get_enum(o, "network", NETS, &m->network, err, n)) ||
        (r = get_str(o, "mac", m->mac, sizeof m->mac, 1, err, n)) || (r = get_str(o, "iso", m->iso, sizeof m->iso, 0, err, n)) ||
        (r = get_str(o, "created", m->created, sizeof m->created, 1, err, n)) || (r = get_str(o, "updated", m->updated, sizeof m->updated, 1, err, n)) ||
        (r = get_str(o, "base", m->base, sizeof m->base, 0, err, n)))
        goto out;
    {
        const JVal *a = json_get(o, "agent");
        if (a && a->type != J_BOOL) { r = fail(err, n, VM_E_FIELD, "agent must be true or false"); goto out; }
        m->agent = a ? json_bool(o, "agent", 0) : 0;
        if (json_get(o, "clipboard") && (r = get_enum(o, "clipboard", CLIPS, &m->clipboard, err, n))) goto out;
    }
    if ((r = parse3(o, m, err, n)) || (r = parse4(o, m, err, n))) goto out;
    {
        /* schema 5's keys */
        const char *const B[] = { "secure_boot", "tpm", "hyperv" };
        int *dst[] = { &m->secure_boot, &m->tpm, &m->hyperv };
        for (int i = 0; i < 3; i++) {
            const JVal *v = json_get(o, B[i]);
            if (v && v->type != J_BOOL) { r = fail(err, n, VM_E_FIELD, "%s must be true or false", B[i]); goto out; }
            *dst[i] = v ? json_bool(o, B[i], 0) : 0;
        }
        if ((r = get_str(o, "drivers", m->drivers, sizeof m->drivers, 0, err, n))) goto out;
        static const char *const GFX[] = { "2d", "3d", NULL };
        if (json_get(o, "graphics") && (r = get_enum(o, "graphics", GFX, &m->graphics, err, n))) goto out;
    }
    if ((r = parse6(o, m, err, n))) goto out;
    {
        /* schema 6: the display the guest starts with ("WxH"); missing: it follows the window */
        const JVal *rv = json_get(o, "resolution");
        m->res_w = m->res_h = 0;
        if (rv && (rv->type != J_STR || vm_resolution_parse(rv->str, &m->res_w, &m->res_h))) { r = fail(err, n, VM_E_FIELD, "resolution must be \"WxH\""); goto out; }
    }
    {
        /* schema 6's keys: missing means sound on at full volume, microphone off */
        const JVal *so = json_get(o, "sound"), *mi = json_get(o, "microphone");
        if ((so && so->type != J_BOOL) || (mi && mi->type != J_BOOL)) { r = fail(err, n, VM_E_FIELD, "sound and microphone must be true or false"); goto out; }
        m->sound = so ? json_bool(o, "sound", 1) : 1;
        m->microphone = mi ? json_bool(o, "microphone", 0) : 0;
        m->volume = 100;
        if ((r = get_opt_int(o, "volume", &m->volume, err, n))) goto out;
        /* schema 7: missing means no transfer folder (a machine made before keeps its hardware) */
        const JVal *t = json_get(o, "transfer");
        if (t && t->type != J_BOOL) { r = fail(err, n, VM_E_FIELD, "transfer must be true or false"); goto out; }
        m->transfer = t ? json_bool(o, "transfer", 0) : 0;
        /* schema 8: missing (or "") means the system's default output and input */
        if ((r = get_str(o, "sound_out", m->sound_out, sizeof m->sound_out, 0, err, n)) || (r = get_str(o, "sound_in", m->sound_in, sizeof m->sound_in, 0, err, n))) goto out;
        /* schema 9: missing means trusted (the clipboard on demand, while the machine is in use) */
        const JVal *ca = json_get(o, "clipboard_ask");
        if (ca && ca->type != J_BOOL) { r = fail(err, n, VM_E_FIELD, "clipboard_ask must be true or false"); goto out; }
        m->clipboard_ask = ca ? json_bool(o, "clipboard_ask", 0) : 0;
        /* schema 10: missing means LordOS draws a Windows guest's pointer */
        char ptr[16];
        if ((r = get_str(o, "pointer", ptr, sizeof ptr, 0, err, n))) goto out;
        if (ptr[0] && strcmp(ptr, "lordos") && strcmp(ptr, "guest")) { r = fail(err, n, VM_E_RANGE, "pointer is lordos or guest"); goto out; }
        m->pointer_guest = !strcmp(ptr, "guest");
        /* schema 11: missing means no real GPU */
        char g[16];
        if ((r = get_str(o, "gpu", g, sizeof g, 0, err, n))) goto out;
        m->vgpu = VM_VGPU_NONE;
        if (g[0]) {
            for (int i = 1; VM_VGPU_NAMES[i]; i++) if (!strcmp(g, VM_VGPU_NAMES[i])) m->vgpu = i;
            if (!m->vgpu) { r = fail(err, n, VM_E_RANGE, "gpu is intel-whole or intel-shared"); goto out; }
        }
        /* schema 12: missing means the mouse isn't captured on its own in full screen */
        const JVal *cf = json_get(o, "capture_fullscreen");
        if (cf && cf->type != J_BOOL) { r = fail(err, n, VM_E_FIELD, "capture_fullscreen must be true or false"); goto out; }
        m->capture_fullscreen = cf ? json_bool(o, "capture_fullscreen", 0) : 0;
    }
    r = vm_machine_check(m, err, n);
out:
    json_free(&d);
    return r;
}

char *vm_machine_json(const VmMachine *m) {
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\n  \"schema\": %d,\n  \"id\": ", m->schema); jb_str(&b, m->id);
    jb_raw(&b, ",\n  \"name\": "); jb_str(&b, m->name);
    jb_printf(&b, ",\n  \"arch\": \"%s\",\n  \"accel\": \"%s\",\n  \"vcpus\": %d,\n  \"memory_mb\": %d,\n  \"disk_gb\": %d,\n  \"firmware\": \"%s\",\n  \"machine_type\": ",
              ARCHS[m->arch], ACCELS[m->accel], m->vcpus, m->memory_mb, m->disk_gb, FWS[m->firmware]);
    jb_str(&b, m->machine_type);
    if (m->firmware == VM_FW_UEFI) {
        jb_raw(&b, ",\n  \"firmware_code\": "); jb_str(&b, m->firmware_code);
        jb_raw(&b, ",\n  \"firmware_sha256\": "); jb_str(&b, m->firmware_sha256);
    }
    jb_printf(&b, ",\n  \"display\": \"%s\",\n  \"network\": \"%s\",\n  \"mac\": ", DISPLAYS[m->display], NETS[m->network]);
    jb_str(&b, m->mac);
    if (m->iso[0]) { jb_raw(&b, ",\n  \"iso\": "); jb_str(&b, m->iso); }
    if (m->schema >= 2) {
        if (m->base[0]) { jb_raw(&b, ",\n  \"base\": "); jb_str(&b, m->base); }
        jb_printf(&b, ",\n  \"agent\": %s,\n  \"clipboard\": \"%s\"", m->agent ? "true" : "false", CLIPS[m->clipboard]);
    }
    if (m->schema >= 3) {
        /* only what differs from a machine made here, so a new machine's file stays as it was */
        if (m->devices) jb_printf(&b, ",\n  \"devices\": \"%s\"", DEVS[m->devices]);
        if (m->nic_model) jb_printf(&b, ",\n  \"nic_model\": \"%s\"", VM_NIC_MODELS[m->nic_model]);
        if (m->n_extra_macs) {
            jb_raw(&b, ",\n  \"extra_nics\": [");
            for (int i = 0; i < m->n_extra_macs; i++) { jb_raw(&b, i ? ", {\"mac\": " : "{\"mac\": "); jb_str(&b, m->extra_macs[i]); jb_printf(&b, ", \"model\": \"%s\"}", VM_NIC_MODELS[m->extra_models[i]]); }
            jb_raw(&b, "]");
        }
        if (m->cpu_sockets) jb_printf(&b, ",\n  \"cpu_sockets\": %d,\n  \"cpu_cores\": %d,\n  \"cpu_threads\": %d", m->cpu_sockets, m->cpu_cores, m->cpu_threads);
        const char *sm[] = { m->sm_manufacturer, m->sm_product, m->sm_version, m->sm_serial, m->sm_sku, m->sm_family };
        int any = 0;
        for (int i = 0; i < 6; i++) if (sm[i][0]) {
            jb_raw(&b, any ? ", " : ",\n  \"smbios\": {");
            jb_str(&b, SMB_KEYS[i]); jb_raw(&b, ": "); jb_str(&b, sm[i]);
            any = 1;
        }
        if (any) jb_raw(&b, "}");
        if (m->disk_serial[0]) { jb_raw(&b, ",\n  \"disk_serial\": "); jb_str(&b, m->disk_serial); }
        if (m->n_data) {
            jb_raw(&b, ",\n  \"data_disks\": [");
            for (int i = 0; i < m->n_data; i++) {
                jb_printf(&b, "%s{\"gb\": %d", i ? ", " : "", m->data_gb[i]);
                if (m->data_serial[i][0]) { jb_raw(&b, ", \"serial\": "); jb_str(&b, m->data_serial[i]); }
                jb_raw(&b, "}");
            }
            jb_raw(&b, "]");
        }
        if (m->rtc) jb_printf(&b, ",\n  \"rtc\": \"%s\"", RTCS[m->rtc]);
    }
    if (m->schema >= 4 && m->n_shares) {
        jb_raw(&b, ",\n  \"shares\": [");
        for (int i = 0; i < m->n_shares; i++) {
            jb_raw(&b, i ? ", {\"tag\": " : "{\"tag\": "); jb_str(&b, m->share_tag[i]);
            jb_raw(&b, ", \"path\": "); jb_str(&b, m->share_path[i]);
            jb_printf(&b, ", \"writable\": %s}", m->share_rw[i] ? "true" : "false");
        }
        jb_raw(&b, "]");
    }
    if (m->schema >= 5) {
        if (m->secure_boot) jb_raw(&b, ",\n  \"secure_boot\": true");
        if (m->tpm) jb_raw(&b, ",\n  \"tpm\": true");
        if (m->hyperv) jb_raw(&b, ",\n  \"hyperv\": true");
        if (m->drivers[0]) { jb_raw(&b, ",\n  \"drivers\": "); jb_str(&b, m->drivers); }
        if (m->graphics == VM_GFX_3D) jb_raw(&b, ",\n  \"graphics\": \"3d\"");
        if (m->res_w && m->res_h) { char rs[48]; snprintf(rs, sizeof rs, ",\n  \"resolution\": \"%dx%d\"", m->res_w, m->res_h); jb_raw(&b, rs); }
    }
    if (m->schema >= 6) {
        if (m->bridge[0]) { jb_raw(&b, ",\n  \"bridge\": "); jb_str(&b, m->bridge); }
        if (m->n_ports) {
            jb_raw(&b, ",\n  \"ports\": [");
            for (int i = 0; i < m->n_ports; i++)
                jb_printf(&b, "%s{\"proto\": \"%s\", \"host\": %d, \"guest\": %d, \"open\": \"%s\"}", i ? ", " : "", PROTOS[m->ports[i].proto], m->ports[i].host, m->ports[i].guest, OPENS[m->ports[i].open]);
            jb_raw(&b, "]");
        }
    }
    if (m->schema >= 6 && m->n_boot) {
        jb_raw(&b, ",\n  \"boot_order\": [");
        for (int i = 0; i < m->n_boot; i++) jb_printf(&b, "%s\"%s\"", i ? ", " : "", VM_BOOT_NAMES[m->boot_order[i]]);
        jb_raw(&b, "]");
    }
    if (m->schema >= 6 && m->boot_menu) jb_raw(&b, ",\n  \"boot_menu\": true");
    if (m->schema >= 6) jb_printf(&b, ",\n  \"sound\": %s,\n  \"volume\": %d,\n  \"microphone\": %s", m->sound ? "true" : "false", m->volume, m->microphone ? "true" : "false");
    if (m->schema >= 7) jb_printf(&b, ",\n  \"transfer\": %s", m->transfer ? "true" : "false");
    if (m->schema >= 9 && m->clipboard_ask) jb_raw(&b, ",\n  \"clipboard_ask\": true");
    if (m->schema >= 10 && m->pointer_guest) jb_raw(&b, ",\n  \"pointer\": \"guest\"");
    if (m->schema >= 11 && m->vgpu) { jb_raw(&b, ",\n  \"gpu\": "); jb_str(&b, VM_VGPU_NAMES[m->vgpu]); }
    if (m->schema >= 12 && m->capture_fullscreen) jb_raw(&b, ",\n  \"capture_fullscreen\": true");
    if (m->schema >= 8 && m->sound_out[0]) { jb_raw(&b, ",\n  \"sound_out\": "); jb_str(&b, m->sound_out); }
    if (m->schema >= 8 && m->sound_in[0]) { jb_raw(&b, ",\n  \"sound_in\": "); jb_str(&b, m->sound_in); }
    jb_raw(&b, ",\n  \"created\": "); jb_str(&b, m->created);
    jb_raw(&b, ",\n  \"updated\": "); jb_str(&b, m->updated);
    jb_raw(&b, "\n}\n");
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}

static char *read_small(const char *path, size_t max, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size > max) { close(fd); errno = EFBIG; return NULL; }
    char *buf = malloc((size_t)st.st_size + 1);
    size_t off = 0;
    while (buf && off < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + off, (size_t)st.st_size - off);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
        off += (size_t)r;
    }
    close(fd);
    if (!buf) return NULL;
    buf[off] = 0;
    *len = off;
    return buf;
}

int vm_machine_load(const char *dir, VmMachine *m, char *err, size_t n) {
    char path[VM_PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/machine.json", dir);
    size_t len = 0;
    char *t = read_small(path, 64 * 1024, &len);
    if (!t) return fail(err, n, VM_E_IO, "can't read %s: %s", path, strerror(errno));
    int r = vm_machine_parse(t, len, m, err, n);
    free(t);
    return r;
}

int vm_machine_save(const char *dir, const VmMachine *m0) {
    if (m0->schema > VM_SCHEMA) return VM_E_RANGE;
    VmMachine up = *m0;
    if (up.schema < 1) return VM_E_RANGE;
    {
        VmMachine c = up;
        c.schema = VM_SCHEMA;               /* checked as what is written */
        if (vm_machine_check(&c, NULL, 0)) return VM_E_RANGE;
    }
    const VmMachine *m = &up;
    if (up.schema < VM_SCHEMA) {
        /* migrated forward, with the old file kept (3.6) */
        char from[VM_PATH_MAX + 32], keep[VM_PATH_MAX + 48];
        snprintf(from, sizeof from, "%s/machine.json", dir);
        snprintf(keep, sizeof keep, "%s/machine.json.schema%d", dir, up.schema);
        size_t len = 0;
        char *old = access(keep, F_OK) ? read_small(from, 64 * 1024, &len) : NULL;
        if (old) {
            int fd = open(keep, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd >= 0) { if (write(fd, old, len) != (ssize_t)len) {} fsync(fd); close(fd); }
            free(old);
        }
        up.schema = VM_SCHEMA;
    }
    char *j = vm_machine_json(m);
    if (!j) return VM_E_IO;
    char tmp[VM_PATH_MAX + 32], path[VM_PATH_MAX + 32];
    snprintf(tmp, sizeof tmp, "%s/.machine.json.tmp", dir);
    snprintf(path, sizeof path, "%s/machine.json", dir);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    size_t len = strlen(j), off = 0;
    int r = fd < 0 ? VM_E_IO : VM_OK;
    while (!r && off < len) {
        ssize_t w = write(fd, j + off, len - off);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; r = VM_E_IO; }
        else off += (size_t)w;
    }
    if (!r && fsync(fd)) r = VM_E_IO;
    if (fd >= 0) close(fd);
    if (!r && rename(tmp, path)) r = VM_E_IO;
    if (r) unlink(tmp);
    free(j);
    return r;
}

/* ---- new machines ---- */

static int random_bytes(void *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = getrandom((char *)p + off, n - off, 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)r;
    }
    return 0;
}

int vm_new_id(char out[37]) {
    uint8_t b[16];
    if (random_bytes(b, sizeof b)) return -1;
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);       /* version 4 */
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);       /* RFC 4122 variant */
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return 0;
}

int vm_new_mac(char out[18]) {
    uint8_t b[3];
    if (random_bytes(b, sizeof b)) return -1;
    snprintf(out, 18, "52:54:00:%02x:%02x:%02x", b[0], b[1], b[2]);
    return 0;
}

void vm_now_iso(char out[21]) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 21, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

void vm_machine_defaults(VmMachine *m, const VmHost *h, int arch) {
    memset(m, 0, sizeof *m);
    m->schema = VM_SCHEMA;
    vm_new_id(m->id);
    vm_new_mac(m->mac);
    snprintf(m->name, sizeof m->name, "Machine");
    m->arch = arch;
    m->accel = h->arch == arch && h->kvm ? VM_ACCEL_KVM : VM_ACCEL_TCG;
    int rec = vm_vcpus_recommended(h);
    m->vcpus = rec < 4 ? rec : 4;
    long mx = vm_mem_max_mb(h);
    m->memory_mb = mx >= 2048 ? 2048 : mx >= VM_MEM_MIN_MB ? (int)mx : VM_MEM_MIN_MB;
    m->disk_gb = 20;
    m->firmware = VM_FW_UEFI;
    m->display = VM_DISPLAY_WINDOW;
    m->network = VM_NET_SHARED;
    m->sound = 1;
    m->volume = 100;
    m->transfer = 1;
    vm_now_iso(m->created);
    memcpy(m->updated, m->created, sizeof m->updated);
}

void vm_host_probe(VmHost *h) {
    struct utsname u;
    h->arch = -1;
    if (!uname(&u)) {
        if (!strcmp(u.machine, "aarch64")) h->arch = VM_ARCH_ARM64;
        else if (!strcmp(u.machine, "x86_64")) h->arch = VM_ARCH_AMD64;
    }
    long c = sysconf(_SC_NPROCESSORS_ONLN);
    h->cpus = c > 0 ? (int)c : 1;
    struct sysinfo si;
    h->ram_mb = !sysinfo(&si) ? (long)((unsigned long long)si.totalram * si.mem_unit / (1024 * 1024)) : 0;
    int fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    h->kvm = fd >= 0;
    if (fd >= 0) close(fd);
}

/* ---- images (vm_image_kind) ---- */

static uint64_t be64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int qcow2_check(const uint8_t *h, size_t n, int backing_ok, char *err, size_t errlen) {
    if (n < 72 || memcmp(h, "QFI\xfb", 4)) { fail(err, errlen, -1, "not a qcow2 image"); return -1; }
    uint32_t ver = be32(h + 4);
    if (ver != 2 && ver != 3) { fail(err, errlen, -1, "qcow2 version %u isn't supported", ver); return -1; }
    if (!backing_ok && (be64(h + 8) || be32(h + 16))) { fail(err, errlen, -1, "the image depends on a backing file: refused"); return -1; }
    if (ver == 3) {
        if (n < 104) { fail(err, errlen, -1, "the qcow2 header is cut short"); return -1; }
        uint64_t incompat = be64(h + 72);
        uint32_t hlen = be32(h + 100);
        if (hlen < 104 || hlen > 4096) { fail(err, errlen, -1, "the qcow2 header length is wrong"); return -1; }
        if (incompat & 4) { fail(err, errlen, -1, "the image keeps its data in another file: refused"); return -1; }
        if (incompat & 2) { fail(err, errlen, -1, "the image is marked corrupt"); return -1; }
        /* bits QEMU doesn't know (0 dirty, 3 compression type, 4 extended L2 are fine) */
        if (incompat & ~(uint64_t)0x1f) { fail(err, errlen, -1, "the image needs qcow2 features this computer doesn't have"); return -1; }
    }
    return 0;
}
int vm_qcow2_plain(const uint8_t *h, size_t n, char *err, size_t errlen) { return qcow2_check(h, n, 0, err, errlen); }
int vm_qcow2_overlay(const uint8_t *h, size_t n, char *err, size_t errlen) { return qcow2_check(h, n, 1, err, errlen); }

static int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++) if ((s[a - b + i] | 32) != suf[i]) return 0;
    return 1;
}

int vm_image_kind(const uint8_t *head, size_t n, const char *name, char *err, size_t errlen) {
    if (n >= 4 && !memcmp(head, "QFI\xfb", 4)) return vm_qcow2_plain(head, n, err, errlen) ? -1 : VM_IMG_QCOW2;
    if (name && (ends_with(name, ".img") || ends_with(name, ".raw"))) {
        if (n < 512) { fail(err, errlen, -1, "the image is too small to be a disk"); return -1; }
        return VM_IMG_RAW;
    }
    fail(err, errlen, -1, ends_with(name ? name : "", ".qcow2") ? "not a qcow2 image" : "only qcow2 images (or raw .img) can be imported");
    return -1;
}

int vm_mouse_pick(const JVal *list, int relative, int *current, int *added) {
    int best = -1, best_rank = 0, best_cur = 0, n = 0;
    *current = 0;
    *added = 0;
    if (!list || list->type != J_ARR) return -1;
    for (const JVal *e = list->kid; e && n < 32; e = e->next, n++) {
        if (e->type != J_OBJ) continue;
        const JVal *ab = json_get(e, "absolute"), *cu = json_get(e, "current"), *ix = json_get(e, "index");
        const char *name = json_str(e, "name");
        if (!ab || ab->type != J_BOOL || !ix || ix->type != J_NUM || ix->num < 0 || ix->num > 1000 || ix->num != (int)ix->num) continue;
        int is_abs = json_bool(e, "absolute", 0), is_cur = cu && cu->type == J_BOOL && json_bool(e, "current", 0);
        int ps2 = name && strstr(name, "PS/2") != NULL, tablet = name && strstr(name, "Tablet") != NULL;
        if (is_cur && !is_abs && !ps2) *added = 1;
        if (is_abs == !!relative) continue;
        int rank = relative ? (ps2 ? 1 : 2) : (tablet ? 2 : 1);
        if (rank > best_rank) { best_rank = rank; best = (int)ix->num; best_cur = is_cur; }
    }
    *current = best_cur;
    return best;
}
