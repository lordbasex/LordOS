/* vmops.c — operation.json, the intent file of 3.6 (see vmops.h). */
#include "vmops.h"
#include "../sec/json.h"
#include "../doc/wire.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void vm_sync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { fsync(fd); close(fd); }
}

int vm_op_begin(const char *dir, const char *op, const char *tag, const char *a, const char *b) {
    JBuf j;
    jb_init(&j);
    char now[21];
    vm_now_iso(now);
    jb_raw(&j, "{\"op\":"); jb_str(&j, op);
    jb_raw(&j, ",\"tag\":"); jb_str(&j, tag ? tag : "");
    jb_raw(&j, ",\"a\":"); jb_str(&j, a ? a : "");
    jb_raw(&j, ",\"b\":"); jb_str(&j, b ? b : "");
    jb_raw(&j, ",\"started\":"); jb_str(&j, now);
    jb_raw(&j, "}\n");
    if (j.oom) { jb_free(&j); return -1; }
    char tmp[VM_PATH_MAX + 64], path[VM_PATH_MAX + 64];
    snprintf(tmp, sizeof tmp, "%s/.operation.json.tmp", dir);
    snprintf(path, sizeof path, "%s/operation.json", dir);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    int ok = fd >= 0;
    for (size_t off = 0; ok && off < j.n;) {
        ssize_t w = write(fd, j.p + off, j.n - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) ok = 0; else off += (size_t)w;
    }
    if (fd >= 0) { if (ok && fsync(fd)) ok = 0; close(fd); }
    jb_free(&j);
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    vm_sync_dir(dir);
    return 0;
}

int vm_op_read(const char *dir, VmOp *o) {
    memset(o, 0, sizeof *o);
    char path[VM_PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/operation.json", dir);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    char buf[8192];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    JDoc d;
    if (n <= 0 || json_parse(buf, (size_t)n, &d, NULL, 0)) { unlink(path); return -1; }
    const char *op = json_str(d.root, "op"), *tag = json_str(d.root, "tag"), *a = json_str(d.root, "a"), *b = json_str(d.root, "b"),
               *st = json_str(d.root, "started");
    int ok = op && strlen(op) < sizeof o->op && (!tag || strlen(tag) < sizeof o->tag) && (!a || strlen(a) < sizeof o->a) && (!b || strlen(b) < sizeof o->b);
    if (ok) {
        snprintf(o->op, sizeof o->op, "%s", op);
        if (tag) snprintf(o->tag, sizeof o->tag, "%s", tag);
        if (a) snprintf(o->a, sizeof o->a, "%s", a);
        if (b) snprintf(o->b, sizeof o->b, "%s", b);
        if (st) snprintf(o->started, sizeof o->started, "%.20s", st);
    }
    json_free(&d);
    if (!ok) { unlink(path); return -1; }
    return 1;
}

int vm_op_read_peek(const char *dir, VmOp *o) {
    memset(o, 0, sizeof *o);
    char path[VM_PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/operation.json", dir);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    char buf[8192];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    JDoc d;
    if (n <= 0 || json_parse(buf, (size_t)n, &d, NULL, 0)) return 0;
    const char *op = json_str(d.root, "op");
    if (op) snprintf(o->op, sizeof o->op, "%s", op);
    json_free(&d);
    return op != NULL;
}

void vm_op_end(const char *dir) {
    char path[VM_PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/operation.json", dir);
    unlink(path);
    vm_sync_dir(dir);
}

void vm_snap_tag(char out[24]) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 24, "s%Y%m%d-%H%M%S", &tm);
}

int vm_snap_tag_valid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 2 || n > 32 || s[0] != 's') return 0;
    for (size_t i = 0; i < n; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return 0;
    return 1;
}

int vm_snaps_load(const char *dir, VmSnap *s, int max) {
    char path[VM_PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/snapshots.json", dir);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    static char buf[128 * 1024];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    JDoc d;
    if (n <= 0 || json_parse(buf, (size_t)n, &d, NULL, 0)) return 0;
    int k = 0;
    const JVal *arr = json_get(d.root, "snapshots");
    for (const JVal *e = arr && arr->type == J_ARR ? arr->kid : NULL; e && k < max; e = e->next) {
        const char *tag = json_str(e, "tag"), *name = json_str(e, "name"), *cr = json_str(e, "created");
        if (!vm_snap_tag_valid(tag) || !name || !vm_name_valid(name)) continue;
        memset(&s[k], 0, sizeof s[k]);
        snprintf(s[k].tag, sizeof s[k].tag, "%s", tag);
        snprintf(s[k].name, sizeof s[k].name, "%s", name);
        if (cr) snprintf(s[k].created, sizeof s[k].created, "%.20s", cr);
        s[k].memory = json_bool(e, "memory", 0);
        s[k].vmstate = json_num(e, "vmstate", 0);
        k++;
    }
    json_free(&d);
    return k;
}

int vm_snaps_save(const char *dir, const VmSnap *s, int n) {
    JBuf j;
    jb_init(&j);
    jb_raw(&j, "{\"snapshots\":[");
    for (int i = 0; i < n; i++) {
        jb_raw(&j, i ? ",\n  {\"tag\":" : "\n  {\"tag\":"); jb_str(&j, s[i].tag);
        jb_raw(&j, ",\"name\":"); jb_str(&j, s[i].name);
        jb_raw(&j, ",\"created\":"); jb_str(&j, s[i].created);
        jb_printf(&j, ",\"memory\":%s,\"vmstate\":%.0f}", s[i].memory ? "true" : "false", s[i].vmstate);
    }
    jb_raw(&j, "\n]}\n");
    if (j.oom) { jb_free(&j); return -1; }
    char tmp[VM_PATH_MAX + 64], path[VM_PATH_MAX + 64];
    snprintf(tmp, sizeof tmp, "%s/.snapshots.json.tmp", dir);
    snprintf(path, sizeof path, "%s/snapshots.json", dir);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    int ok = fd >= 0;
    for (size_t off = 0; ok && off < j.n;) {
        ssize_t w = write(fd, j.p + off, j.n - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) ok = 0; else off += (size_t)w;
    }
    if (fd >= 0) { if (ok && fsync(fd)) ok = 0; close(fd); }
    jb_free(&j);
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    vm_sync_dir(dir);
    return 0;
}

int vm_snaps_find(const VmSnap *s, int n, const char *tag) {
    for (int i = 0; i < n; i++) if (!strcmp(s[i].tag, tag)) return i;
    return -1;
}

int vm_snap_note_valid(const char *s) {
    if (!s || strlen(s) > VM_SNAP_NOTE_MAX) return 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p;
        int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (k < 0 || (k == 1 && c < 0xc2) || (k == 3 && c > 0xf4)) return 0;
        unsigned cp = k == 0 ? c : k == 1 ? c & 0x1f : k == 2 ? c & 0x0f : c & 0x07;
        for (int i = 1; i <= k; i++) {
            if ((p[i] & 0xc0) != 0x80) return 0;
            cp = cp << 6 | (p[i] & 0x3f);
        }
        if ((k == 2 && cp < 0x800) || (k == 3 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        if ((cp < 0x20 && cp != '\n') || (cp >= 0x7f && cp < 0xa0) || cp == 0x2028 || cp == 0x2029 || (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069)) return 0;
        p += k + 1;
    }
    return 1;
}

/* ---- identity.json (6.3) ---- */

int vm_hostname_valid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > VM_HOSTNAME_MAX || s[0] == '-' || s[n - 1] == '-') return 0;
    for (size_t i = 0; i < n; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return 0;
    return 1;
}

void vm_hostname_from_name(const char *name, char *out, size_t n) {
    size_t o = 0;
    int dash = 0;
    for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p && o + 1 < n && o < VM_HOSTNAME_MAX; p++) {
        char c = (char)*p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { out[o++] = c; dash = 0; }
        else if (o && !dash) { out[o++] = '-'; dash = 1; }
    }
    while (o && out[o - 1] == '-') o--;
    out[o] = 0;
    if (!o) snprintf(out, n, "machine");
}

int vm_winname_valid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    int letter = 0;
    if (!n || n > VM_WINNAME_MAX || s[0] == '-') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-') letter = 1;
        else if (!(c >= '0' && c <= '9')) return 0;
    }
    return letter;
}

int vm_winname_from_host(const char *host, const char *id, char out[VM_WINNAME_MAX + 1]) {
    out[0] = 0;
    if (!vm_hostname_valid(host)) return -1;
    size_t n = strlen(host);
    if (n <= VM_WINNAME_MAX) snprintf(out, VM_WINNAME_MAX + 1, "%s", host);
    else {
        char hex[5];
        size_t k = 0;
        for (const char *p = id ? id : ""; *p && k < 4; p++) if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')) hex[k++] = *p;
        hex[k] = 0;
        size_t m = 10;
        while (m && host[m - 1] == '-') m--;
        snprintf(out, VM_WINNAME_MAX + 1, "%.*s%s%s", (int)m, host, k ? "-" : "", hex);
    }
    if (!vm_winname_valid(out)) { out[0] = 0; return -1; }
    return 0;
}

int vm_identity_write(const char *dir, const char *hostname) {
    if (!vm_hostname_valid(hostname)) return -1;
    char tmp[VM_PATH_MAX + 64], path[VM_PATH_MAX + 64], buf[128];
    snprintf(tmp, sizeof tmp, "%s/.identity.json.tmp", dir);
    snprintf(path, sizeof path, "%s/identity.json", dir);
    int len = snprintf(buf, sizeof buf, "{\"hostname\":\"%s\"}\n", hostname);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    int ok = fd >= 0 && write(fd, buf, (size_t)len) == len && !fsync(fd);
    if (fd >= 0) close(fd);
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    vm_sync_dir(dir);
    return 0;
}

int vm_identity_read(const char *dir, char *hostname, size_t n) {
    char path[VM_PATH_MAX + 64], buf[512];
    snprintf(path, sizeof path, "%s/identity.json", dir);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    JDoc d;
    if (r <= 0 || json_parse(buf, (size_t)r, &d, NULL, 0)) return -1;
    const char *h = json_str(d.root, "hostname");
    int ok = h && vm_hostname_valid(h) && strlen(h) < n;
    if (ok) snprintf(hostname, n, "%s", h);
    json_free(&d);
    return ok ? 0 : -1;
}

void vm_identity_done(const char *dir) {
    char path[VM_PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/identity.json", dir);
    unlink(path);
    vm_sync_dir(dir);
}
