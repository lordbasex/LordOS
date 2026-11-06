/* machine.c — machine.json: parse, check, write (see machine.h). */
#include "machine.h"
#include "../sec/json.h"
#include "../doc/wire.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
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

/* QEMU's own prefix, locally administered and unicast by construction */
int vm_mac_valid(const char *s) {
    if (!s || strlen(s) != 17 || strncmp(s, "52:54:00:", 9)) return 0;
    for (int i = 9; i < 17; i += 3) {
        if (hex2(s + i) < 0) return 0;
        if (i + 2 < 17 && s[i + 2] != ':') return 0;
    }
    return 1;
}

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

int vm_base_valid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    return n > 15 && path_valid(s) && strstr(s, "/.shared/") && !strchr(strstr(s, "/.shared/") + 9, '/') && !strcmp(s + n - 6, ".qcow2");
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
    if (m->network != VM_NET_SHARED && m->network != VM_NET_ISOLATED) return fail(err, n, VM_E_RANGE, "network");
    if (!vm_mac_valid(m->mac)) return fail(err, n, VM_E_RANGE, "mac must be 52:54:00:xx:xx:xx, lowercase");
    if (m->iso[0] && !path_valid(m->iso)) return fail(err, n, VM_E_RANGE, "iso must be an absolute path without '..', ',' or control characters");
    if (!iso_time_valid(m->created) || !iso_time_valid(m->updated)) return fail(err, n, VM_E_RANGE, "created and updated are UTC times like 2026-11-04T20:00:00Z");
    if (m->base[0] && !vm_base_valid(m->base)) return fail(err, n, VM_E_RANGE, "base must be a .qcow2 in a .shared folder");
    if (m->agent != 0 && m->agent != 1) return fail(err, n, VM_E_RANGE, "agent");
    if (m->clipboard < VM_CLIP_OFF || m->clipboard > VM_CLIP_BOTH) return fail(err, n, VM_E_RANGE, "clipboard");
    if (m->schema < 2 && (m->base[0] || m->agent || m->clipboard)) return fail(err, n, VM_E_FIELD, "base, agent and clipboard need schema 2");
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
    "firmware_code", "firmware_sha256", "display", "network", "mac", "iso", "created", "updated", "base", "agent", "clipboard", NULL,
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
static const char *const NETS[] = { "shared", "isolated", NULL };
static const char *const CLIPS[] = { "off", "to-guest", "from-guest", "both", NULL };

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
