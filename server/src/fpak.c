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

int fpak_meta_grants_home(const char *md) {
    if (!md) return 0;
    const char *ctx = strstr(md, "[Context]");
    if (!ctx || (ctx != md && ctx[-1] != '\n')) return 0;
    for (const char *line = strchr(ctx, '\n'); line && *++line && *line != '[';) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len > 12 && !strncmp(line, "filesystems=", 12)) {
            const char *p = line + 12, *stop = line + len;
            while (p < stop) {
                const char *q = memchr(p, ';', (size_t)(stop - p));
                size_t tl = q ? (size_t)(q - p) : (size_t)(stop - p);
                const char *colon = memchr(p, ':', tl);
                size_t nl = colon ? (size_t)(colon - p) : tl;
                if (nl && p[0] != '!' && ((nl == 4 && (!strncmp(p, "home", 4) || !strncmp(p, "host", 4))) ||
                                          (nl == 1 && p[0] == '~') || (nl == 2 && !strncmp(p, "~/", 2))))
                    return 1;
                p += tl + 1;
            }
        }
        line = end;
    }
    return 0;
}

/* names lord-server serves on the session bus: an app may talk to them but never own them. --talk-name sets the
 * app's policy to TALK, which replaces an OWN its metadata or an override file asked for (the command line is merged
 * last). The secret service is refused outright: talking to it would reach the person's stored secrets. */
static const char *const talk_only[] = { "org.freedesktop.Notifications", "org.freedesktop.portal.Desktop",
                                         "org.freedesktop.ScreenSaver", "org.kde.StatusNotifierWatcher", NULL };
static const char *const never_talk[] = { "org.freedesktop.Flatpak", "org.freedesktop.systemd1", "org.freedesktop.secrets",
                                          "org.freedesktop.portal.Flatpak", NULL };
/* folders the system reads to start or install something, or that hold a security decision (fsops_protected) */
static const char *const hidden[] = { ".config/lordos", ".local/share/lordos", ".cache/lordos", ".local/share/applications",
                                      ".config/autostart", ".config/systemd", ".config/environment.d", ".local/bin",
                                      ".local/share/flatpak/overrides", ".local/share/flatpak/exports", NULL };
static const char *const startup[] = { ".bashrc", ".profile", ".bash_profile", ".zshrc", NULL };

int fpak_argv(const FpakRun *r, char **argv, char *buf, size_t cap) {
    if (!r || !fpak_id_ok(r->id) || !r->arch || !arch_ok(r->arch) || !fpak_branch_ok(r->branch) || !r->home ||
        r->home[0] != '/')
        return -1;
    if (r->opened && (r->opened[0] != '/' || strchr(r->opened, ':') || strchr(r->opened, '\n'))) return -1;
    int argc = 0;
    size_t used = 0;
    int bad = 0;
#define ARG(...) do { \
        int k_ = snprintf(buf + used, cap - used, __VA_ARGS__); \
        if (k_ < 0 || (size_t)k_ >= cap - used || argc >= FPAK_ARGV_MAX - 1) { bad = 1; break; } \
        argv[argc++] = buf + used; used += (size_t)k_ + 1; \
    } while (0)
    ARG("%s", FPAK_FLATPAK);
    ARG("run");
    ARG("--arch=%s", r->arch);
    ARG("--branch=%s", r->branch);
    /* never granted, whatever the app or an override file says */
    ARG("--nosocket=session-bus");
    ARG("--nosocket=system-bus");
    for (int i = 0; never_talk[i]; i++) ARG("--no-talk-name=%s", never_talk[i]);
    for (int i = 0; talk_only[i]; i++) ARG("--talk-name=%s", talk_only[i]);
    for (int i = 0; hidden[i]; i++) ARG("--nofilesystem=~/%s", hidden[i]);
    if (r->grants_home)
        for (int i = 0; startup[i]; i++) {
            char p[4200];
            snprintf(p, sizeof p, "%s/%s", r->home, startup[i]);
            if (!access(p, F_OK)) ARG("--filesystem=~/%s:ro", startup[i]);
        }
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
    if (r->opened) ARG("--filesystem=%s", r->opened);
    ARG("%s", r->id);
    if (r->opened) ARG("%s", r->opened);
#undef ARG
    if (bad) return -1;
    argv[argc] = NULL;
    return argc;
}
