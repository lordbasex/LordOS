/* vm_admin.c — Machines' actions in the admin helper (see vm_admin.h). */
#include "vm_admin.h"
#include "../pkg/pkg.h"


#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static int have(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }

static int have_glob(const char *pat) {
    glob_t g;
    int ok = !glob(pat, GLOB_NOSORT, NULL, &g) && g.gl_pathc > 0;
    globfree(&g);
    return ok;
}

static void add(char *out, size_t n, int *k, const char *pkg) {
    size_t l = strlen(out);
    snprintf(out + l, n - l, "%s%s", l ? " " : "", pkg);
    (*k)++;
}

int vm_missing_packages(int arches, char *out, size_t n) {
    int k = 0;
    out[0] = 0;
    if ((arches & 2) && !have("/usr/bin/qemu-system-x86_64")) add(out, n, &k, "qemu-system-x86");
    if ((arches & 2) && !have("/usr/share/OVMF/OVMF_CODE_4M.fd")) add(out, n, &k, "ovmf");
    if ((arches & 1) && !have("/usr/bin/qemu-system-aarch64")) add(out, n, &k, "qemu-system-arm");
    if ((arches & 1) && !have("/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd") && !have("/usr/share/AAVMF/AAVMF_CODE.fd")) add(out, n, &k, "qemu-efi-aarch64");
    if (arches && !have_glob("/usr/lib/*/qemu/ui-dbus.so")) add(out, n, &k, "qemu-system-modules-opengl");
    if (arches && !have("/usr/bin/qemu-img")) add(out, n, &k, "qemu-utils");
    if (arches && !have("/usr/bin/passt")) add(out, n, &k, "passt");
    /* shared folders (6.7): one virtiofsd per folder, only for machines that share one */
    if (arches && !have("/usr/libexec/virtiofsd")) add(out, n, &k, "virtiofsd");
    return k;
}

void vm_act_enable(const AdminReq *r, const char *arg, size_t len) {
    char buf[32];
    if (len >= sizeof buf) { admin_err("admin.invalidArgument", NULL); return; }
    memcpy(buf, arg, len);
    buf[len] = 0;
    int arches = 0;
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        if (!strcmp(tok, "arm64")) arches |= 1;
        else if (!strcmp(tok, "amd64")) arches |= 2;
        else { admin_err("admin.invalidArgument", NULL); return; }
    }
    if (!arches) { admin_err("admin.invalidArgument", NULL); return; }
    char pkgs[512];
    if (!vm_missing_packages(arches, pkgs, sizeof pkgs)) { admin_ok("vm.alreadyInstalled", NULL); return; }
    pkg_act_install(r, pkgs, strlen(pkgs));
}

/* ---- /dev/kvm ---- */

/* POSIX access list as the kernel stores it (system.posix_acl_access) */
enum { T_USER_OBJ = 1, T_USER = 2, T_GROUP_OBJ = 4, T_GROUP = 8, T_MASK = 0x10, T_OTHER = 0x20 };
typedef struct { uint16_t tag, perm; uint32_t id; } AclEnt;
#define ACL_VERSION 2
#define ACL_MAX 64

static int ent_cmp(const void *a, const void *b) {
    const AclEnt *x = a, *y = b;
    if (x->tag != y->tag) return x->tag < y->tag ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}

/* adds (or widens) the entry of uid with rw on path; the mask covers the group class again */
static int acl_grant(const char *path, uid_t uid) {
    AclEnt e[ACL_MAX + 2];
    int n = 0;
    uint8_t raw[4 + ACL_MAX * 8];
    ssize_t l = getxattr(path, "system.posix_acl_access", raw, sizeof raw);
    struct stat st;
    if (stat(path, &st)) return -1;
    if (l >= 4) {
        uint32_t ver;
        memcpy(&ver, raw, 4);
        if (ver != ACL_VERSION || (l - 4) % 8) return -1;
        for (ssize_t o = 4; o < l && n < ACL_MAX; o += 8) memcpy(&e[n++], raw + o, 8);
    } else {
        if (l < 0 && errno != ENODATA) return -1;
        e[n++] = (AclEnt){ T_USER_OBJ, (uint16_t)((st.st_mode >> 6) & 7), (uint32_t)-1 };
        e[n++] = (AclEnt){ T_GROUP_OBJ, (uint16_t)((st.st_mode >> 3) & 7), (uint32_t)-1 };
        e[n++] = (AclEnt){ T_OTHER, (uint16_t)(st.st_mode & 7), (uint32_t)-1 };
    }
    int found = 0, mask = -1;
    uint16_t group_class = 0;
    for (int i = 0; i < n; i++) {
        if (e[i].tag == T_USER && e[i].id == uid) { e[i].perm |= 6; found = 1; }
        if (e[i].tag == T_MASK) mask = i;
    }
    if (!found) e[n++] = (AclEnt){ T_USER, 6, (uint32_t)uid };
    for (int i = 0; i < n; i++) if (e[i].tag == T_USER || e[i].tag == T_GROUP || e[i].tag == T_GROUP_OBJ) group_class |= e[i].perm;
    if (mask >= 0) e[mask].perm = group_class;
    else e[n++] = (AclEnt){ T_MASK, group_class, (uint32_t)-1 };
    qsort(e, (size_t)n, sizeof e[0], ent_cmp);
    uint32_t ver = ACL_VERSION;
    memcpy(raw, &ver, 4);
    for (int i = 0; i < n; i++) memcpy(raw + 4 + i * 8, &e[i], 8);
    return setxattr(path, "system.posix_acl_access", raw, 4 + (size_t)n * 8, 0);
}

static int in_group(const char *user, gid_t gid) {
    struct group *g = getgrgid(gid);
    if (!g) return 0;
    for (char **m = g->gr_mem; *m; m++) if (!strcmp(*m, user)) return 1;
    return 0;
}

/* /dev/kvm carries the "uaccess" tag: systemd-logind rewrites its access list on every session change (an SSH login is
 * enough) and keeps only the user of the active seat session, which a LordOS session isn't. Without the tag the entry
 * granted below stays until the next boot, and from the next login the kvm group covers it. */
#define VM_KVM_RULES "/etc/udev/rules.d/71-lordos-kvm.rules"
static void keep_kvm_acl(void) {
    static const char rule[] =
        "# LordOS Machines (vm-kvm): /dev/kvm is the kvm group's, and keeps the access list Machines gives it.\n"
        "SUBSYSTEM==\"misc\", KERNEL==\"kvm\", TAG-=\"uaccess\"\n";
    int fd = open(VM_KVM_RULES ".tmp", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return;
    int ok = write(fd, rule, sizeof rule - 1) == (ssize_t)(sizeof rule - 1) && !fsync(fd);
    close(fd);
    if (!ok || rename(VM_KVM_RULES ".tmp", VM_KVM_RULES)) { unlink(VM_KVM_RULES ".tmp"); return; }
    const char *const reload[] = { "/usr/bin/udevadm", "control", "--reload", NULL };
    const char *const change[] = { "/usr/bin/udevadm", "trigger", "--action=change", "--subsystem-match=misc", "--sysname-match=kvm", NULL };
    const char *const settle[] = { "/usr/bin/udevadm", "settle", "--timeout=5", NULL };
    if (!admin_run(reload) && !admin_run(change)) admin_run(settle);
}

void vm_act_kvm(const AdminReq *r, const char *arg, size_t len) {
    (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    struct stat st;
    if (stat("/dev/kvm", &st) || !S_ISCHR(st.st_mode)) { admin_err("vm.noKvm", NULL); return; }
    struct group *g = getgrnam("kvm");
    if (!g) { admin_err("vm.noKvmGroup", NULL); return; }
    gid_t kvm = g->gr_gid;
    if (st.st_gid != kvm) { admin_err("vm.noKvmGroup", NULL); return; }
    if (!in_group(r->user, kvm)) {
        const char *const a[] = { "/usr/sbin/usermod", "--append", "--groups", "kvm", r->user, NULL };
        if (admin_run(a)) { admin_err("vm.groupFailed", NULL); return; }
    }
    keep_kvm_acl();
    if (acl_grant("/dev/kvm", r->uid)) admin_ok("vm.kvmNextLogin", NULL);   /* the group works from the next login */
    else admin_ok("vm.kvmReady", NULL);
}

