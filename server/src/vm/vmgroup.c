/* vmgroup.c — see vmgroup.h. */
#include "vmgroup.h"
#include "../sec/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

const char *const VMG_ROLE_NAMES[] = { "", "gateway", "workstation", NULL };
static const char *const KINDS[] = { "", "whonix", NULL };
static const char *const KEYS[] = { "schema", "id", "name", "kind", "members", "image", "disposable", "fresh_gateway", NULL };
static const char *const MKEYS[] = { "role", "dir", "start", "stop", "primary", NULL };
static const char *const IKEYS[] = { "catalog", "release", "edition", "archive_sha512", "bases", "base_sha256", NULL };

static int fail(char *err, size_t n, int code, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int fail(char *err, size_t n, int code, const char *fmt, ...) {
    if (err && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return code;
}

static int only_keys(const JVal *o, const char *const *keys, const char *where, char *err, size_t n) {
    for (const JVal *k = o->kid; k; k = k->next) {
        int known = 0;
        for (int i = 0; keys[i]; i++) if (!strcmp(k->key, keys[i])) known = 1;
        if (!known) return fail(err, n, VM_E_FIELD, "%s: unknown key \"%.40s\"", where, k->key);
    }
    return VM_OK;
}

static int get_s(const JVal *o, const char *k, char *out, size_t cap, int need, char *err, size_t n) {
    const JVal *v = json_get(o, k);
    out[0] = 0;
    if (!v) return need ? fail(err, n, VM_E_FIELD, "%s is missing", k) : VM_OK;
    if (v->type != J_STR) return fail(err, n, VM_E_FIELD, "%s must be text", k);
    if (v->len >= cap || strlen(v->str) != v->len) return fail(err, n, VM_E_RANGE, "%s is too long", k);
    memcpy(out, v->str, v->len + 1);
    return VM_OK;
}

static int get_i(const JVal *o, const char *k, int *out, int lo, int hi, char *err, size_t n) {
    const JVal *v = json_get(o, k);
    if (!v) return fail(err, n, VM_E_FIELD, "%s is missing", k);
    if (v->type != J_NUM || v->num != (double)(int)v->num || v->num < lo || v->num > hi) return fail(err, n, VM_E_RANGE, "%s must be a whole number %d to %d", k, lo, hi);
    *out = (int)v->num;
    return VM_OK;
}

static int get_b(const JVal *o, const char *k, int *out, char *err, size_t n) {
    const JVal *v = json_get(o, k);
    *out = 0;
    if (!v) return VM_OK;
    if (v->type != J_BOOL) return fail(err, n, VM_E_FIELD, "%s must be true or false", k);
    *out = v->num != 0;
    return VM_OK;
}

static int name_of(const char *const *list, const char *s) {
    for (int i = 1; list[i]; i++) if (!strcmp(list[i], s)) return i;
    return 0;
}

static int hex_ok(const char *s, size_t len) {
    if (strlen(s) != len) return 0;
    for (size_t i = 0; i < len; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

/* a release "18.2.4.3": 1 to 6 numbers of 1 to 6 digits, dot-separated */
static int release_ok(const char *s) {
    int parts = 0, digits = 0;
    if (!*s) return 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') { if (++digits > 6) return 0; continue; }
        if (*s != '.' || !digits) return 0;
        digits = 0;
        if (++parts > 5) return 0;
    }
    return digits > 0;
}

static int word_ok(const char *s, size_t max) {
    size_t n = strlen(s);
    if (!n || n > max) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_')) return 0;
    return 1;
}

int vm_group_member_name_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 8 || n > VMG_DIR_MAX || s[0] == '.' || strcmp(s + n - 7, ".lordvm")) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ',') return 0;
    }
    return 1;
}

int vm_group_dir_ok(const char *p) {
    size_t n = p ? strlen(p) : 0, sl = strlen(VMG_SUFFIX);
    if (n < 2 + sl || n >= VM_PATH_MAX || p[0] != '/' || p[n - 1] == '/') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c == 0x7f || c == ',') return 0;
        if (c != '/') continue;
        size_t j = i + 1;
        while (j < n && p[j] != '/') j++;
        size_t len = j - i - 1;
        if (!len || (len == 1 && p[i + 1] == '.') || (len == 2 && p[i + 1] == '.' && p[i + 2] == '.')) return 0;
    }
    const char *base = strrchr(p, '/') + 1;
    size_t bl = strlen(base);
    return bl > sl && base[0] != '.' && !strcmp(base + bl - sl, VMG_SUFFIX);
}

int vm_group_parent(const char *dir, char *out, size_t n) {
    size_t l = dir ? strlen(dir) : 0;
    if (!l || l >= VM_PATH_MAX || l + 1 > n) return 0;
    char tmp[VM_PATH_MAX];
    memcpy(tmp, dir, l + 1);
    while (l > 1 && tmp[l - 1] == '/') tmp[--l] = 0;
    char *s = strrchr(tmp, '/');
    if (!s || s == tmp) return 0;
    *s = 0;
    if (!vm_group_dir_ok(tmp)) return 0;
    memcpy(out, tmp, strlen(tmp) + 1);
    return 1;
}

int vm_group_check(const VmGroup *g, char *err, size_t n) {
    if (g->schema != VMG_SCHEMA) return fail(err, n, VM_E_SCHEMA, "schema %d", g->schema);
    if (!vm_uuid_valid(g->id)) return fail(err, n, VM_E_RANGE, "id must be a lowercase UUID");
    if (!vm_name_valid(g->name) || g->name[0] == '.') return fail(err, n, VM_E_RANGE, "name: 1 to %d bytes of UTF-8, no control characters, '/' or '\\'", VM_NAME_MAX);
    if (g->kind != VMG_KIND_WHONIX) return fail(err, n, VM_E_RANGE, "kind is whonix");
    if (g->n < 1 || g->n > VMG_MEMBERS_MAX) return fail(err, n, VM_E_RANGE, "1 to %d members", VMG_MEMBERS_MAX);
    int seen_start = 0, seen_stop = 0, primaries = 0, roles = 0;
    for (int i = 0; i < g->n; i++) {
        const VmGroupMember *m = &g->m[i];
        if (m->role <= VM_ROLE_NONE || m->role >= VM_ROLE_COUNT) return fail(err, n, VM_E_RANGE, "members: role is gateway or workstation");
        if (roles & (1 << m->role)) return fail(err, n, VM_E_RANGE, "members: the role %s twice", VMG_ROLE_NAMES[m->role]);
        roles |= 1 << m->role;
        if (!vm_group_member_name_ok(m->dir)) return fail(err, n, VM_E_RANGE, "members: dir is a plain folder name ending .lordvm");
        for (int k = 0; k < i; k++) if (!strcasecmp(m->dir, g->m[k].dir)) return fail(err, n, VM_E_RANGE, "members: the same dir twice");
        if (m->start < 1 || m->start > g->n || m->stop < 1 || m->stop > g->n) return fail(err, n, VM_E_RANGE, "members: start and stop are 1 to %d", g->n);
        if (seen_start & (1 << m->start) || seen_stop & (1 << m->stop)) return fail(err, n, VM_E_RANGE, "members: the start and stop orders are each a permutation of 1 to %d", g->n);
        seen_start |= 1 << m->start;
        seen_stop |= 1 << m->stop;
        if (m->primary & ~1) return fail(err, n, VM_E_RANGE, "members: primary");
        primaries += m->primary;
    }
    if (primaries != 1) return fail(err, n, VM_E_RANGE, "exactly one member is primary");
    if (g->kind == VMG_KIND_WHONIX) {
        /* a Tor gateway and a workstation; the gateway starts first and stops last; the workstation is the card */
        int gw = vm_group_role(g, VM_ROLE_GATEWAY), ws = vm_group_role(g, VM_ROLE_WORKSTATION);
        if (g->n != 2 || gw < 0 || ws < 0) return fail(err, n, VM_E_RANGE, "a whonix group is a gateway and a workstation");
        if (g->m[gw].start != 1 || g->m[gw].stop != 2) return fail(err, n, VM_E_RANGE, "a whonix gateway starts first and stops last");
        if (!g->m[ws].primary) return fail(err, n, VM_E_RANGE, "a whonix group's primary member is its workstation");
    }
    if ((g->disposable | g->fresh_gateway) & ~1) return fail(err, n, VM_E_RANGE, "disposable and fresh_gateway are true or false");
    if (g->has_image) {
        if (!word_ok(g->catalog, VMG_CATALOG_MAX)) return fail(err, n, VM_E_RANGE, "image: catalog is a catalog entry's id");
        if (!release_ok(g->release)) return fail(err, n, VM_E_RANGE, "image: release is numbers and dots");
        if (!word_ok(g->edition, VMG_EDITION_MAX)) return fail(err, n, VM_E_RANGE, "image: edition is a word");
        if (!hex_ok(g->archive_sha512, 128)) return fail(err, n, VM_E_RANGE, "image: archive_sha512 is 128 lowercase hex digits");
        for (int r = 1; r < VM_ROLE_COUNT; r++) {
            int have = vm_group_role(g, r) >= 0;
            if (have != (g->base[r][0] != 0) || have != (g->base_sha256[r][0] != 0)) return fail(err, n, VM_E_RANGE, "image: each member's role has a base and its base_sha256, no other role");
            if (!have) continue;
            if (!vm_base_valid(g->base[r])) return fail(err, n, VM_E_RANGE, "image: a base is a .qcow2 in a .shared folder");
            if (!hex_ok(g->base_sha256[r], 64)) return fail(err, n, VM_E_RANGE, "image: base_sha256 is 64 lowercase hex digits");
        }
    } else if (g->kind == VMG_KIND_WHONIX) return fail(err, n, VM_E_FIELD, "a whonix group names its image");
    return VM_OK;
}

static int parse_role_map(const JVal *o, const char *what, char dst[][VM_PATH_MAX], size_t cap, char *err, size_t n) {
    if (o->type != J_OBJ) return fail(err, n, VM_E_FIELD, "image: %s is an object", what);
    for (const JVal *k = o->kid; k; k = k->next) {
        int r = name_of(VMG_ROLE_NAMES, k->key);
        if (!r) return fail(err, n, VM_E_FIELD, "image: %s: unknown role \"%.20s\"", what, k->key);
        if (k->type != J_STR || k->len >= cap || strlen(k->str) != k->len) return fail(err, n, VM_E_RANGE, "image: %s: text", what);
        memcpy(dst[r], k->str, k->len + 1);
    }
    return VM_OK;
}

int vm_group_parse(const char *text, size_t len, VmGroup *g, char *err, size_t n) {
    memset(g, 0, sizeof *g);
    if (!text || len > 16 * 1024) return fail(err, n, VM_E_JSON, "group.json is larger than 16 KB");
    JDoc d;
    char jerr[128];
    if (json_parse(text, len, &d, jerr, sizeof jerr)) return fail(err, n, VM_E_JSON, "not JSON: %s", jerr);
    int r = VM_OK;
    const JVal *o = d.root;
    char kind[16];
    if (o->type != J_OBJ) { r = fail(err, n, VM_E_JSON, "not a JSON object"); goto out; }
    const JVal *sv = json_get(o, "schema");
    if (!sv || sv->type != J_NUM || sv->num != (double)(int)sv->num) { r = fail(err, n, VM_E_SCHEMA, "schema is missing"); goto out; }
    g->schema = (int)sv->num;
    if (g->schema > VMG_SCHEMA) { r = fail(err, n, VM_E_NEWER, "schema %d is newer than this LordOS (%d): opened read-only", g->schema, VMG_SCHEMA); goto out; }
    if (g->schema < 1) { r = fail(err, n, VM_E_SCHEMA, "schema %d", g->schema); goto out; }
    if ((r = only_keys(o, KEYS, "group.json", err, n)) || (r = get_s(o, "id", g->id, sizeof g->id, 1, err, n)) ||
        (r = get_s(o, "name", g->name, sizeof g->name, 1, err, n)) || (r = get_s(o, "kind", kind, sizeof kind, 1, err, n)) ||
        (r = get_b(o, "disposable", &g->disposable, err, n)) || (r = get_b(o, "fresh_gateway", &g->fresh_gateway, err, n))) goto out;
    if (!(g->kind = name_of(KINDS, kind))) { r = fail(err, n, VM_E_RANGE, "kind is whonix"); goto out; }
    const JVal *ms = json_get(o, "members");
    if (!ms || ms->type != J_ARR) { r = fail(err, n, VM_E_FIELD, "members is a list"); goto out; }
    if (ms->len < 1 || ms->len > VMG_MEMBERS_MAX) { r = fail(err, n, VM_E_RANGE, "1 to %d members", VMG_MEMBERS_MAX); goto out; }
    for (const JVal *e = ms->kid; e; e = e->next) {
        VmGroupMember *m = &g->m[g->n++];
        char role[16];
        if (e->type != J_OBJ) { r = fail(err, n, VM_E_FIELD, "members: each is an object"); goto out; }
        if ((r = only_keys(e, MKEYS, "members", err, n)) || (r = get_s(e, "role", role, sizeof role, 1, err, n)) ||
            (r = get_s(e, "dir", m->dir, sizeof m->dir, 1, err, n)) || (r = get_i(e, "start", &m->start, 1, VMG_MEMBERS_MAX, err, n)) ||
            (r = get_i(e, "stop", &m->stop, 1, VMG_MEMBERS_MAX, err, n)) || (r = get_b(e, "primary", &m->primary, err, n))) goto out;
        if (!(m->role = name_of(VMG_ROLE_NAMES, role))) { r = fail(err, n, VM_E_RANGE, "members: role is gateway or workstation"); goto out; }
    }
    const JVal *im = json_get(o, "image");
    if (im) {
        if (im->type != J_OBJ) { r = fail(err, n, VM_E_FIELD, "image is an object"); goto out; }
        g->has_image = 1;
        if ((r = only_keys(im, IKEYS, "image", err, n)) || (r = get_s(im, "catalog", g->catalog, sizeof g->catalog, 1, err, n)) ||
            (r = get_s(im, "release", g->release, sizeof g->release, 1, err, n)) || (r = get_s(im, "edition", g->edition, sizeof g->edition, 1, err, n)) ||
            (r = get_s(im, "archive_sha512", g->archive_sha512, sizeof g->archive_sha512, 1, err, n))) goto out;
        const JVal *b = json_get(im, "bases"), *h = json_get(im, "base_sha256");
        if (!b || !h) { r = fail(err, n, VM_E_FIELD, "image: bases and base_sha256 are missing"); goto out; }
        char sh[VM_ROLE_COUNT][VM_PATH_MAX];
        memset(sh, 0, sizeof sh);
        if ((r = parse_role_map(b, "bases", g->base, VM_PATH_MAX, err, n)) || (r = parse_role_map(h, "base_sha256", sh, 65, err, n))) goto out;
        for (int k = 0; k < VM_ROLE_COUNT; k++) memcpy(g->base_sha256[k], sh[k], 65);
    }
    r = vm_group_check(g, err, n);
out:
    json_free(&d);
    if (r != VM_OK && r != VM_E_NEWER) { int s = g->schema; memset(g, 0, sizeof *g); g->schema = s; }
    return r;
}

/* JSON text of a string (json_quote) appended to b at *o; -1 when it doesn't fit */
static int put(char *b, size_t cap, size_t *o, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int put(char *b, size_t cap, size_t *o, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(b + *o, cap - *o, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= cap - *o) return -1;
    *o += (size_t)k;
    return 0;
}
static int putq(char *b, size_t cap, size_t *o, const char *s) {
    char q[2 * VM_PATH_MAX + 8];
    if (json_quote(q, sizeof q, s) >= sizeof q - 1) return -1;
    return put(b, cap, o, "%s", q);
}

char *vm_group_json(const VmGroup *g) {
    size_t cap = 16 * 1024, o = 0;
    char *b = malloc(cap);
    if (!b) return NULL;
    int bad = put(b, cap, &o, "{\n  \"schema\": %d,\n  \"id\": ", VMG_SCHEMA) || putq(b, cap, &o, g->id) ||
              put(b, cap, &o, ",\n  \"name\": ") || putq(b, cap, &o, g->name) ||
              put(b, cap, &o, ",\n  \"kind\": \"%s\",\n  \"members\": [", KINDS[g->kind]);
    for (int i = 0; !bad && i < g->n; i++) {
        const VmGroupMember *m = &g->m[i];
        bad = put(b, cap, &o, "%s\n    { \"role\": \"%s\", \"dir\": ", i ? "," : "", VMG_ROLE_NAMES[m->role]) || putq(b, cap, &o, m->dir) ||
              put(b, cap, &o, ", \"start\": %d, \"stop\": %d%s }", m->start, m->stop, m->primary ? ", \"primary\": true" : "");
    }
    if (!bad) bad = put(b, cap, &o, "\n  ]");
    if (!bad && g->has_image) {
        bad = put(b, cap, &o, ",\n  \"image\": { \"catalog\": ") || putq(b, cap, &o, g->catalog) || put(b, cap, &o, ", \"release\": ") ||
              putq(b, cap, &o, g->release) || put(b, cap, &o, ", \"edition\": ") || putq(b, cap, &o, g->edition) ||
              put(b, cap, &o, ",\n             \"archive_sha512\": \"%s\",\n             \"bases\": {", g->archive_sha512);
        for (int r = 1, k = 0; !bad && r < VM_ROLE_COUNT; r++) {
            if (!g->base[r][0]) continue;
            bad = put(b, cap, &o, "%s \"%s\": ", k++ ? "," : "", VMG_ROLE_NAMES[r]) || putq(b, cap, &o, g->base[r]);
        }
        if (!bad) bad = put(b, cap, &o, " },\n             \"base_sha256\": {");
        for (int r = 1, k = 0; !bad && r < VM_ROLE_COUNT; r++) {
            if (!g->base[r][0]) continue;
            bad = put(b, cap, &o, "%s \"%s\": \"%s\"", k++ ? "," : "", VMG_ROLE_NAMES[r], g->base_sha256[r]);
        }
        if (!bad) bad = put(b, cap, &o, " } }");
    }
    if (!bad) bad = put(b, cap, &o, ",\n  \"disposable\": %s,\n  \"fresh_gateway\": %s\n}\n", g->disposable ? "true" : "false", g->fresh_gateway ? "true" : "false");
    if (bad) { free(b); return NULL; }
    return b;
}

int vm_group_load(const char *dir, VmGroup *g, char *err, size_t n) {
    char path[VM_PATH_MAX + 32];
    memset(g, 0, sizeof *g);
    if (snprintf(path, sizeof path, "%s/group.json", dir) >= (int)sizeof path) return fail(err, n, VM_E_IO, "path too long");
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return fail(err, n, VM_E_IO, "can't read %s: %s", path, strerror(errno));
    struct stat st;
    char buf[16 * 1024 + 1];
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 16 * 1024) { close(fd); return fail(err, n, VM_E_IO, "%s isn't a small file", path); }
    size_t off = 0;
    while (off < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + off, (size_t)st.st_size - off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        off += (size_t)r;
    }
    close(fd);
    buf[off] = 0;
    return vm_group_parse(buf, off, g, err, n);
}

int vm_group_save(const char *dir, const VmGroup *g) {
    if (g->schema > VMG_SCHEMA || vm_group_check(g, NULL, 0)) return VM_E_RANGE;
    char *j = vm_group_json(g);
    if (!j) return VM_E_IO;
    char tmp[VM_PATH_MAX + 32], path[VM_PATH_MAX + 32];
    snprintf(tmp, sizeof tmp, "%s/.group.json.tmp", dir);
    snprintf(path, sizeof path, "%s/group.json", dir);
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

int vm_group_order(const VmGroup *g, int stop, int idx[VMG_MEMBERS_MAX]) {
    int k = 0;
    for (int pos = 1; pos <= g->n; pos++)
        for (int i = 0; i < g->n; i++) if ((stop ? g->m[i].stop : g->m[i].start) == pos) idx[k++] = i;
    return k;
}

int vm_group_role(const VmGroup *g, int role) {
    for (int i = 0; i < g->n; i++) if (g->m[i].role == role) return i;
    return -1;
}

int vm_group_primary(const VmGroup *g) {
    for (int i = 0; i < g->n; i++) if (g->m[i].primary) return i;
    return -1;
}

/* ---- managing a group's folder (Delete, Rename, Repair) ---- */

static int rm_at(int dfd, const char *name, int depth, dev_t dev) {
    struct stat st;
    if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlinkat(dfd, name, 0) && errno != ENOENT ? -1 : 0;
    if (depth <= 0 || st.st_dev != dev) { errno = st.st_dev != dev ? EXDEV : ELOOP; return -1; }
    int fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        /* it became a link (or anything else) since: removed as what it is now, never followed */
        if (errno == ELOOP || errno == ENOTDIR) return unlinkat(dfd, name, 0) && errno != ENOENT ? -1 : 0;
        return -1;
    }
    struct stat fst;
    if (fstat(fd, &fst) || fst.st_dev != dev) { close(fd); errno = EXDEV; return -1; }
    DIR *d = fdopendir(fd);
    if (!d) { close(fd); return -1; }
    int r = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (rm_at(dirfd(d), e->d_name, depth - 1, dev)) r = -1;
    }
    closedir(d);
    if (r) return -1;
    return unlinkat(dfd, name, AT_REMOVEDIR) && errno != ENOENT ? -1 : 0;
}

int vm_group_rm_at(int dfd, const char *name, int depth) {
    struct stat st;
    if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) { errno = EINVAL; return -1; }
    if (fstat(dfd, &st)) return -1;
    return rm_at(dfd, name, depth, st.st_dev);
}

static int lock_file_at(int dfd, const char *name) {
    int fd = openat(dfd, name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return -2; }
    return fd;
}
int vm_group_lock_at(int gfd) { return lock_file_at(gfd, ".lock"); }
int vm_group_member_lock_at(int mfd) { return lock_file_at(mfd, "lock"); }

static int ends(const char *s, const char *suf) {
    size_t n = strlen(s), k = strlen(suf);
    return n > k && !strcmp(s + n - k, suf);
}

/* the base of the machine folder dir/name, when its machine.json loads: 1 when it names `base` */
static int names_base(const char *dir, const char *name, const char *base) {
    char p[VM_PATH_MAX + 512];
    if (snprintf(p, sizeof p, "%s/%s", dir, name) >= (int)sizeof p) return 0;
    VmMachine *m = malloc(sizeof *m);
    int r = m && vm_machine_load(p, m, NULL, 0) == VM_OK && !strcmp(m->base, base);
    free(m);
    return r;
}

int vm_group_base_users(const char *lib, const char *trash, const char *skip, const char *base) {
    int n = 0;
    DIR *d = opendir(lib);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        if (ends(e->d_name, ".lordvm")) { n += names_base(lib, e->d_name, base); continue; }
        if (!ends(e->d_name, VMG_SUFFIX) || (skip && !strcmp(e->d_name, skip))) continue;
        char gd[VM_PATH_MAX + 512];
        struct stat st;
        if (snprintf(gd, sizeof gd, "%s/%s", lib, e->d_name) >= (int)sizeof gd || lstat(gd, &st) || !S_ISDIR(st.st_mode)) continue;
        VmGroup *g = malloc(sizeof *g);
        if (g && vm_group_load(gd, g, NULL, 0) == VM_OK)
            for (int r = 1; r < VM_ROLE_COUNT; r++) if (!strcmp(g->base[r], base)) n++;
        free(g);
        DIR *gdir = opendir(gd);
        for (struct dirent *m; gdir && (m = readdir(gdir));) if (m->d_name[0] != '.' && ends(m->d_name, ".lordvm")) n += names_base(gd, m->d_name, base);
        if (gdir) closedir(gdir);
    }
    if (d) closedir(d);
    if (trash && (d = opendir(trash))) {
        for (struct dirent *e; (e = readdir(d));) if (e->d_name[0] != '.' && ends(e->d_name, ".lordvm")) n += names_base(trash, e->d_name, base);
        closedir(d);
    }
    return n;
}

void vm_group_folder_name(const char *name, char *out, size_t n) {
    char f[64];
    vm_folder_name(name, f, sizeof f);
    snprintf(out, n, "%s%s", f, VMG_SUFFIX);
}

int vm_group_rename_plan(const char *gdir, const char *name, VmGroupRenamePlan *p) {
    memset(p, 0, sizeof *p);
    if (!vm_rename_name_ok(name)) return p->problem = VMG_RN_BAD_NAME;
    if (!vm_group_dir_ok(gdir)) return p->problem = VMG_RN_NOT_GROUP;
    const char *b = strrchr(gdir, '/');
    char lib[VM_PATH_MAX], cur[NAME_MAX + 1], now[256];
    snprintf(lib, sizeof lib, "%.*s", (int)(b - gdir), gdir);
    snprintf(cur, sizeof cur, "%s", b + 1);
    /* the folder: kept while the name already gives the same one (made safe), as for a machine (6.26) */
    char stem[NAME_MAX + 1];
    snprintf(stem, sizeof stem, "%.*s", (int)(strlen(cur) - strlen(VMG_SUFFIX)), cur);
    vm_group_folder_name(stem, now, sizeof now);
    vm_group_folder_name(name, p->folder, sizeof p->folder);
    p->moves = strcmp(now, p->folder) != 0;
    if (!p->moves) snprintf(p->folder, sizeof p->folder, "%s", cur);
    DIR *d = opendir(lib);
    if (!d) return p->problem = VMG_RN_NOT_GROUP;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, cur)) continue;
        if (p->moves && !p->problem && !strcasecmp(e->d_name, p->folder)) p->problem = VMG_RN_FOLDER_TAKEN;
        if (e->d_name[0] == '.') continue;
        char od[VM_PATH_MAX + 512];
        if (snprintf(od, sizeof od, "%s/%s", lib, e->d_name) >= (int)sizeof od) continue;
        /* another group or machine called the same (ASCII case ignored: `machines` finds both by name that way) */
        char other[VM_NAME_MAX + 1] = "";
        if (ends(e->d_name, VMG_SUFFIX)) {
            VmGroup *g = malloc(sizeof *g);
            if (g && vm_group_load(od, g, NULL, 0) == VM_OK) snprintf(other, sizeof other, "%s", g->name);
            free(g);
        } else if (ends(e->d_name, ".lordvm")) {
            VmMachine *m = malloc(sizeof *m);
            if (m && vm_machine_load(od, m, NULL, 0) == VM_OK) snprintf(other, sizeof other, "%s", m->name);
            free(m);
        }
        if (other[0] && !strcasecmp(other, name)) { snprintf(p->other, sizeof p->other, "%s", other); p->problem = VMG_RN_NAME_TAKEN; break; }
    }
    closedir(d);
    return p->problem;
}
