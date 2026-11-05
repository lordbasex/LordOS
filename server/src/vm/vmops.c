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
