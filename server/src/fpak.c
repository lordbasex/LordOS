/* fpak.c — Flatpak apps for the launcher: id rules, the installation and the `flatpak run` command line (fpak.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "fpak.h"
#include "fw/rules.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int fpak_id_ok(const char *id) {
    size_t n = id ? strlen(id) : 0;
    if (!n || n > FPAK_ID_MAX) return 0;
    int elems = 1;
    const char *start = id;
    for (const char *p = id;; p++) {
        if (*p == '.' || !*p) {
            if (p == start || isdigit((unsigned char)*start)) return 0;      /* empty, or starting with a digit */
            if (!*p) break;
            elems++;
            start = p + 1;
        } else if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return 0;
    }
    if (elems < 3) return 0;
    const char *last = strrchr(id, '.');
    for (const char *p = id; p < last; p++) if (*p == '-') return 0;          /* '-' only in the last element */
    return 1;
}

int fpak_branch_ok(const char *b) {
    size_t n = b ? strlen(b) : 0;
    if (!n || n >= FPAK_BRANCH_MAX || b[0] == '.' || b[0] == '-' || strstr(b, "..")) return 0;
    for (const char *p = b; *p; p++) if (!isalnum((unsigned char)*p) && !strchr("._-", *p)) return 0;
    return 1;
}

static int arch_ok(const char *a) {
    size_t n = strlen(a);
    if (!n || n > 16) return 0;
    for (const char *p = a; *p; p++) if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) && *p != '_') return 0;
    return 1;
}

/* a deployment folder name: the commit's 64 hex digits */
static int commit_ok(const char *c) {
    if (strlen(c) != 64) return 0;
    for (const char *p = c; *p; p++) if (!isxdigit((unsigned char)*p) || isupper((unsigned char)*p)) return 0;
    return 1;
}

static int sub(int dir, const char *name) {
    return openat(dir, name, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

int fpak_installed(const char *root, const char *id, char *arch, size_t an, char *branch, size_t bn,
                   char *metadata, size_t mn) {
    if (!root || root[0] != '/' || !fpak_id_ok(id)) return -1;
    int fds[6], nf = 0, ok = -1;
    char link[128], cur[128];
    if ((fds[nf] = open(root, O_PATH | O_DIRECTORY | O_CLOEXEC)) < 0) return -1;
    nf++;
    if ((fds[nf] = sub(fds[nf - 1], "app")) < 0) goto out;
    nf++;
    if ((fds[nf] = sub(fds[nf - 1], id)) < 0) goto out;
    nf++;
    ssize_t l = readlinkat(fds[nf - 1], "current", cur, sizeof cur - 1);       /* "<arch>/<branch>" */
    if (l <= 0) goto out;
    cur[l] = 0;
    char *slash = strchr(cur, '/');
    if (!slash || strchr(slash + 1, '/')) goto out;
    *slash = 0;
    if (!arch_ok(cur) || !fpak_branch_ok(slash + 1)) goto out;
    if ((fds[nf] = sub(fds[nf - 1], cur)) < 0) goto out;
    nf++;
    if ((fds[nf] = sub(fds[nf - 1], slash + 1)) < 0) goto out;
    nf++;
    l = readlinkat(fds[nf - 1], "active", link, sizeof link - 1);              /* the commit's folder, next to it */
    if (l <= 0) goto out;
    link[l] = 0;
    if (!commit_ok(link)) goto out;
    if ((fds[nf] = sub(fds[nf - 1], link)) < 0) goto out;
    nf++;
    int m = openat(fds[nf - 1], "metadata", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (m < 0) goto out;
    struct stat st;
    if (fstat(m, &st) || !S_ISREG(st.st_mode)) { close(m); goto out; }
    if (metadata && mn) {
        ssize_t r = read(m, metadata, mn - 1);
        metadata[r > 0 ? r : 0] = 0;
    }
    close(m);
    if (snprintf(arch, an, "%s", cur) >= (int)an || snprintf(branch, bn, "%s", slash + 1) >= (int)bn) goto out;
    ok = 0;
out:
    while (nf > 0) close(fds[--nf]);
    return ok;
}

/* names lord-server serves on the session bus: an app may talk to them but never own them. --talk-name sets the
 * app's policy to TALK, which replaces an OWN its metadata or an override file asked for (the command line is merged
 * last). The secret service is refused outright: talking to it would reach the person's stored secrets. */
static const char *const talk_only[] = { "org.freedesktop.Notifications", "org.freedesktop.portal.Desktop",
                                         "org.freedesktop.ScreenSaver", "org.kde.StatusNotifierWatcher",
                                         "org.freedesktop.portal.Flatpak", NULL };   /* the last: lord-server's Spawn (E9) */
static const char *const never_talk[] = { "org.freedesktop.Flatpak", "org.freedesktop.systemd1", "org.freedesktop.secrets", NULL };
/* folders the system reads to start or install something, or that hold a security decision (fsops_protected): hidden
 * where Flatpak can hide them; the launcher's view also keeps them, and the shell's startup files, read-only */
static const char *const hidden[] = { ".config/lordos", ".local/share/lordos", ".cache/lordos", ".local/share/applications",
                                      ".config/autostart", ".config/systemd", ".config/environment.d", ".local/bin",
                                      ".local/share/flatpak", ".local/share/flatpak/overrides", ".local/share/flatpak/exports",
                                      ".cache/flatpak", NULL };

static int plain_name(const char *n) {
    size_t l = n ? strlen(n) : 0;
    if (!l || l > 255 || n[0] == '.' || n[0] == '-') return 0;
    for (size_t i = 0; i < l; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int env_name_ok(const char *n, size_t l) {
    if (!l || l > 255 || !((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= 'A' && n[0] <= 'Z') || n[0] == '_')) return 0;
    for (size_t i = 1; i < l; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return 1;
}

static int str_ok(const char *v, int option) {
    return v && strlen(v) < FPAK_SPAWN_STR_MAX && (!option || !strchr(v, '\n'));
}

static int list_len(const char *const *l) { int n = 0; while (l && l[n]) n++; return n; }

int fpak_spawn_ok(const FpakSpawn *s) {
    if (!s || (s->flags & ~FPAK_SPAWN_FLAGS_ALL) || (s->sandbox_flags & ~FPAK_SANDBOX_FLAGS_ALL)) return 0;
    if ((s->flags & (FPAK_SPAWN_EXPOSE_PIDS | FPAK_SPAWN_SHARE_PIDS)) && s->parent_pid <= 0) return 0;
    if (s->cwd) {
        if (s->cwd[0] != '/' || !str_ok(s->cwd, 1)) return 0;
        for (const char *p = s->cwd; (p = strstr(p, "..")); p += 2)
            if (p[-1] == '/' && (p[2] == '/' || !p[2])) return 0;
    }
    int na = list_len(s->args);
    if (na < 1 || na > FPAK_SPAWN_ARGS_MAX || !s->args[0][0]) return 0;
    for (int i = 0; i < na; i++) if (!str_ok(s->args[i], i == 0)) return 0;
    if (list_len(s->env) > FPAK_SPAWN_ENV_MAX || list_len(s->unset_env) > FPAK_SPAWN_ENV_MAX) return 0;
    for (int i = 0; s->env && s->env[i]; i++) {
        const char *eq = strchr(s->env[i], '=');
        if (!eq || !env_name_ok(s->env[i], (size_t)(eq - s->env[i])) || !str_ok(s->env[i], 1)) return 0;
    }
    for (int i = 0; s->unset_env && s->unset_env[i]; i++) if (!env_name_ok(s->unset_env[i], strlen(s->unset_env[i]))) return 0;
    if (list_len(s->expose) > FPAK_SPAWN_EXPOSE_MAX || list_len(s->expose_ro) > FPAK_SPAWN_EXPOSE_MAX) return 0;
    for (int i = 0; s->expose && s->expose[i]; i++) if (!plain_name(s->expose[i])) return 0;
    for (int i = 0; s->expose_ro && s->expose_ro[i]; i++) if (!plain_name(s->expose_ro[i])) return 0;
    return 1;
}

static int build_argv(const FpakRun *r, const FpakSpawn *sp, char **argv, int max, char *buf, size_t cap) {
    if (!r || !fpak_id_ok(r->id) || !r->arch || !arch_ok(r->arch) || !fpak_branch_ok(r->branch) || !r->home ||
        r->home[0] != '/')
        return -1;
    if (r->opened && (sp || r->opened[0] != '/' || strchr(r->opened, ':') || strchr(r->opened, '\n'))) return -1;
    if (sp && !fpak_spawn_ok(sp)) return -1;
    int argc = 0;
    size_t used = 0;
    int bad = 0;
#define ARG(...) do { \
        int k_ = snprintf(buf + used, cap - used, __VA_ARGS__); \
        if (k_ < 0 || (size_t)k_ >= cap - used || argc >= max - 1) { bad = 1; break; } \
        argv[argc++] = buf + used; used += (size_t)k_ + 1; \
    } while (0)
    ARG("%s", FPAK_FLATPAK);
    ARG("run");
    ARG("--arch=%s", r->arch);
    ARG("--branch=%s", r->branch);
    if (sp) {
        /* what the app asked for its new instance (flatpak-portal's mapping); everything after this can only take away */
        uint32_t f = sp->flags, sf = sp->sandbox_flags;
        ARG("--die-with-parent");      /* the new sandbox ends with the process lord-server watches */
        if (f & FPAK_SPAWN_SANDBOX) {
            ARG("--sandbox");
            if (sf & FPAK_SANDBOX_SHARE_DISPLAY) { ARG("--socket=wayland"); ARG("--socket=fallback-x11"); }
            if (sf & FPAK_SANDBOX_SHARE_SOUND) ARG("--socket=pulseaudio");
            if (sf & FPAK_SANDBOX_SHARE_GPU) ARG("--device=dri");
            ARG(sf & FPAK_SANDBOX_ALLOW_DBUS ? "--session-bus" : "--no-session-bus");
            ARG(sf & FPAK_SANDBOX_ALLOW_A11Y ? "--a11y-bus" : "--no-a11y-bus");
        }
        if (f & FPAK_SPAWN_NO_NETWORK) ARG("--unshare=network");
        if (f & FPAK_SPAWN_EMPTY_APP) ARG("--app-path=");
        if (f & (FPAK_SPAWN_EXPOSE_PIDS | FPAK_SPAWN_SHARE_PIDS)) {
            ARG("--parent-pid=%d", sp->parent_pid);
            ARG(f & FPAK_SPAWN_SHARE_PIDS ? "--parent-share-pids" : "--parent-expose-pids");
        }
        if (sp->instance_fd >= 0) ARG("--instance-id-fd=%d", sp->instance_fd);
        for (int i = 0; sp->expose && sp->expose[i]; i++) ARG("--filesystem=%s/.var/app/%s/sandbox/%s", r->home, r->id, sp->expose[i]);
        for (int i = 0; sp->expose_ro && sp->expose_ro[i]; i++) ARG("--filesystem=%s/.var/app/%s/sandbox/%s:ro", r->home, r->id, sp->expose_ro[i]);
        if (sp->cwd) ARG("--cwd=%s", sp->cwd);
        for (int i = 0; sp->env && sp->env[i]; i++) ARG("--env=%s", sp->env[i]);
        for (int i = 0; sp->unset_env && sp->unset_env[i]; i++) ARG("--unset-env=%s", sp->unset_env[i]);
        ARG("--command=%s", sp->args[0]);
    }
    /* never granted, whatever the app or an override file says */
    ARG("--nosocket=session-bus");
    ARG("--nosocket=system-bus");
    for (int i = 0; never_talk[i]; i++) ARG("--no-talk-name=%s", never_talk[i]);
    for (int i = 0; talk_only[i]; i++) ARG("--talk-name=%s", talk_only[i]);
    for (int i = 0; hidden[i]; i++) ARG("--nofilesystem=~/%s", hidden[i]);
    /* LordOS's restrictions, also in Flatpak's own terms (the kernel paths stay the boundary) */
    uint32_t x = r->restricted;
    if (x & FW_R_NO_NETWORK) ARG("--unshare=network");
    if (x & FW_R_NO_USB) ARG("--nodevice=usb");
    if (x & FW_R_NO_BLUETOOTH) ARG("--disallow=bluetooth");
    if (x & FW_R_NO_FILES) { ARG("--nofilesystem=home"); ARG("--nofilesystem=host"); }
    if (!r->x11 || (x & (FW_R_NO_FILES | FW_R_NO_MIC | FW_R_NO_CAMERA | FW_R_NO_USB | FW_R_NO_BLUETOOTH))) {
        ARG("--nosocket=x11");
        ARG("--nosocket=fallback-x11");
    }
    if (r->opened) ARG("--filesystem=%s%s", r->opened, r->opened_ro ? ":ro" : "");
    ARG("%s", r->id);
    if (r->opened) ARG("%s", r->opened);
    for (int i = 1; sp && sp->args[i]; i++) ARG("%s", sp->args[i]);
#undef ARG
    if (bad) return -1;
    argv[argc] = NULL;
    return argc;
}

int fpak_argv(const FpakRun *r, char **argv, char *buf, size_t cap) { return build_argv(r, NULL, argv, FPAK_ARGV_MAX, buf, cap); }

int fpak_spawn_argv(const FpakRun *r, const FpakSpawn *s, char **argv, char *buf, size_t cap) {
    if (!s) return -1;
    return build_argv(r, s, argv, FPAK_SPAWN_ARGV_MAX, buf, cap);
}

int fpak_ref_ok(const char *ref, const char *arch) {
    if (!ref || !arch || strlen(ref) >= FPAK_REF_MAX || strncmp(ref, "app/", 4)) return 0;
    char buf[FPAK_REF_MAX], *parts[4] = { 0 };
    snprintf(buf, sizeof buf, "%s", ref);
    int n = 0;
    for (char *p = buf, *tok; n < 4 && (tok = strsep(&p, "/")); ) parts[n++] = tok;
    if (n != 4 || strchr(ref + 4, '/') == NULL) return 0;
    /* exactly three slashes */
    int slashes = 0;
    for (const char *q = ref; *q; q++) slashes += *q == '/';
    return slashes == 3 && fpak_id_ok(parts[1]) && !strcmp(parts[2], arch) && fpak_branch_ok(parts[3]);
}

int fpak_cmd_argv(int cmd, const char *ref, const char *arch, char **argv, char *buf, size_t cap) {
    int has = ref && ref[0];
    int needs = cmd == FPAK_CMD_INFO || cmd == FPAK_CMD_INSTALL || cmd == FPAK_CMD_UNINSTALL;
    int takes = needs || cmd == FPAK_CMD_UPDATE;
    if (cmd < 0 || cmd >= FPAK_CMD_COUNT || (needs && !has) || (has && !takes) || (has && !fpak_ref_ok(ref, arch))) return -1;
    static const char *const fixed[FPAK_CMD_COUNT][8] = {
        [FPAK_CMD_REMOTE_ADD] = { "remote-add", "--user", "--if-not-exists", "--from", "flathub", FPAK_REPO_FILE, NULL },
        [FPAK_CMD_APPSTREAM]  = { "update", "--user", "--noninteractive", "--appstream", "flathub", NULL },
        [FPAK_CMD_INFO]       = { "remote-info", "--user", "--show-metadata", "flathub", NULL },
        [FPAK_CMD_INSTALL]    = { "install", "--user", "--noninteractive", "-y", "flathub", NULL },
        [FPAK_CMD_UPDATE]     = { "update", "--user", "--noninteractive", "-y", NULL },
        [FPAK_CMD_UNINSTALL]  = { "uninstall", "--user", "--noninteractive", "-y", NULL },
        [FPAK_CMD_UNUSED]     = { "uninstall", "--user", "--noninteractive", "-y", "--unused", NULL },
        [FPAK_CMD_LIST]       = { "list", "--user", "--app", "--columns=application,branch,version,origin", NULL },
        [FPAK_CMD_REMOTE_DELETE] = { "remote-delete", "--user", "--force", "flathub", NULL },
    };
    int argc = 0;
    size_t used = 0;
    const char *list[12];
    int k = 0;
    list[k++] = FPAK_FLATPAK;
    for (int i = 0; fixed[cmd][i]; i++) list[k++] = fixed[cmd][i];
    if (has) list[k++] = ref;
    for (int i = 0; i < k; i++) {
        size_t l = strlen(list[i]) + 1;
        if (used + l > cap || argc >= FPAK_ARGV_MAX - 1) return -1;
        memcpy(buf + used, list[i], l);
        argv[argc++] = buf + used;
        used += l;
    }
    argv[argc] = NULL;
    return argc;
}
