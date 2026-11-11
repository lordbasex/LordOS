/* webmain.c — lord-web's start (see web.h and docs/WEBAPPS.md, "Starting an app" and "Isolation").
 *
 * Only lord-server starts it, from the fixed path next to its own binary, with exactly
 * `--app <app> --profile <profile>`, `--probe` or `--probe-icon` (webargs.c). Before anything else lord-web:
 *   - refuses any other argv, and an environment that would turn WebKit's sandbox off; drops the variables that point
 *     WebKit or GStreamer at other code (an inspector server, injected bundles, other process binaries, plugin paths);
 *   - for an app: reads its own registry file read-only (webreg.c) and refuses to start on any mismatch: the file's
 *     id, the profile, any field;
 *   - takes the profile's lock (one process per app and profile: a second start asks the first to show its window
 *     over the session bus, org.lordos.WebApp1.Show, and exits);
 *   - enters its own scope, app-lordos-lordos-web-<app>.scope (the firewall's and Monitor's row for the app; the probe's
 *     is lordos-webprobe), with memory and CPU limits that WebKit's processes share, since they are its children;
 *   - restricts itself with Landlock's scoping (no abstract unix socket and no signal outside its own processes) and
 *     no_new_privs. Landlock's file rules are not used: a process under them can't mount, and WebKit's bubblewrap
 *     sandbox, which keeps the web content and network processes off the files, mounts for every child it starts. */
#include "web.h"
#include "webwin.h"
#include "../sec/cgroup.h"
#include "../sec/sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define WEB_MEM_HIGH_MB 2048            /* the app's scope: reclaim from here */
#define WEB_MEM_MAX_MB  4096            /* and never past this */
#define WEB_CPU_PERCENT 200             /* two cores */
#define SHOW_IFACE WEBREG_SHOW_IFACE
#define SHOW_PATH  WEBREG_SHOW_PATH

extern char **environ;

const char *web_self_dir(void) {
    static char dir[PATH_MAX];
    if (dir[0]) return dir;
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    if (n <= 0) { snprintf(dir, sizeof dir, "/usr/lib/lordos/bin"); return dir; }
    dir[n] = 0;
    char *s = strrchr(dir, '/');
    if (s && s != dir) *s = 0;
    return dir;
}

static void usage(void) {
    fprintf(stderr, "usage: lord-web --app <app> --profile <profile> | --probe | --probe-icon\n"
                    "lord-web is started by lord-server only (docs/WEBAPPS.md).\n");
}

/* 0, or -1 when the environment asks for WebKit without its sandbox */
static int clean_env(void) {
    if (getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS")) {
        fprintf(stderr, "lord-web: WebKit's sandbox is never turned off\n");
        return -1;
    }
    static const char *const drop[] = { "WEBKIT_", "WPE_", "GST_PLUGIN", "GST_REGISTRY", "GIO_EXTRA_MODULES", "GIO_MODULE_DIR", "LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT", NULL };
    for (int again = 1; again;) {
        again = 0;
        for (char **e = environ; e && *e; e++) {
            for (int i = 0; drop[i]; i++) if (!strncmp(*e, drop[i], strlen(drop[i]))) {
                char name[128];
                size_t k = strcspn(*e, "=");
                if (k >= sizeof name) k = sizeof name - 1;
                memcpy(name, *e, k);
                name[k] = 0;
                unsetenv(name);
                again = 1;
                break;
            }
            if (again) break;
        }
    }
    return 0;
}

static int isolate(const char *scope_id) {
    char name[128];
    snprintf(name, sizeof name, "app-lordos-%s.scope", scope_id);
    int managed = getenv("LORDOS_SESSIOND") != NULL;
    const char *self = cgroup_self();
    size_t sl = strlen(self), nl = strlen(name);
    int inside = sl > nl && self[sl - nl - 1] == '/' && !strcmp(self + sl - nl, name);
    if (!inside) {
        CgroupLimits lim = { .mem_high_mb = WEB_MEM_HIGH_MB, .mem_max_mb = WEB_MEM_MAX_MB, .cpu_percent = WEB_CPU_PERCENT };
        if (!cgroup_scope_name_ok(name) || (cgroup_enter(name, &lim) && managed)) {
            fprintf(stderr, "lord-web: could not enter %s: not started\n", name);
            return -1;
        }
        cgroup_settle();                /* lord-netperm sees the new scope before the first connection */
    }
    if (sandbox_apply_scope()) {
        if (managed) { fprintf(stderr, "lord-web: Landlock scoping failed: not started\n"); return -1; }
        fprintf(stderr, "lord-web: Landlock scoping failed (development: going on)\n");
    }
    return 0;
}

void web_registry_refresh(Web *w) {
    WebApp a;
    char err[160];
    if (webreg_load(w->home, w->args.app, &a, err, sizeof err) || webreg_check_launch(&a, w->args.app, w->args.profile, err, sizeof err)) {
        fprintf(stderr, "lord-web: the registry file changed and is not valid (%s): kept as it was\n", err);
        return;
    }
    w->app = a;
    w->profile = webreg_profile(&w->app, w->args.profile);
}

/* ---- one process per app and profile ---- */

static void show_call(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method, GVariant *p, GDBusMethodInvocation *inv, gpointer d) {
    (void)c; (void)sender; (void)path; (void)iface;
    if (!strcmp(method, "Show")) {
        const char *token = NULL;
        g_variant_get(p, "(&s)", &token);
        web_show(d, token);
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}
static const GDBusInterfaceVTable show_vt = { show_call, NULL, NULL, { 0 } };
static const char SHOW_XML[] = "<node><interface name='" SHOW_IFACE "'><method name='Show'><arg type='s' direction='in'/></method></interface></node>";

/* org.lordos.WebApp.app_<app>.profile_<profile> (webreg_bus_name: the name lord-server calls too) */
static void bus_name(const Web *w, char *out, size_t n) {
    if (webreg_bus_name(w->app.id, w->profile->id, out, n) && n) out[0] = 0;
}

static void serve_show(Web *w) {
    if (!w->bus) return;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(SHOW_XML, NULL);
    if (info) {
        g_dbus_connection_register_object(w->bus, SHOW_PATH, info->interfaces[0], &show_vt, w, NULL, NULL);
        g_dbus_node_info_unref(info);
    }
    char name[256];
    bus_name(w, name, sizeof name);
    g_bus_own_name_on_connection(w->bus, name, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, NULL, NULL, NULL, NULL);
}

/* the profile is in use: ask its process to show the window */
static void ask_running(Web *w, const char *token) {
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) return;
    char name[256];
    bus_name(w, name, sizeof name);
    GVariant *r = g_dbus_connection_call_sync(bus, name, SHOW_PATH, SHOW_IFACE, "Show", g_variant_new("(s)", token ? token : ""), NULL,
                                              G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, NULL);
    if (r) g_variant_unref(r);
    g_object_unref(bus);
}

static int lock_profile(Web *w) {
    char p[PATH_MAX + 8];
    snprintf(p, sizeof p, "%s/lock", w->profile_dir);
    int fd = open(p, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { fprintf(stderr, "lord-web: %s: %s\n", p, strerror(errno)); return -2; }
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return -1; }
    return fd;                          /* held until exit */
}

static int mkdir_p(const char *path) {
    char p[PATH_MAX];
    if (snprintf(p, sizeof p, "%s", path) >= (int)sizeof p) return -1;
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(p, 0700) && errno != EEXIST) return -1;
        *s = '/';
    }
    return mkdir(p, 0700) && errno != EEXIST ? -1 : 0;
}

static int run_app(Web *w) {
    char err[200];
    if (webreg_load(w->home, w->args.app, &w->app, err, sizeof err) || webreg_check_launch(&w->app, w->args.app, w->args.profile, err, sizeof err)) {
        fprintf(stderr, "lord-web: the registry refuses %s/%s: %s\n", w->args.app, w->args.profile, err);
        return 2;
    }
    w->profile = webreg_profile(&w->app, w->args.profile);
    snprintf(w->window_id, sizeof w->window_id, "lordos-web.%s.%s", w->app.id, w->profile->id);
    if (snprintf(w->profile_dir, sizeof w->profile_dir, "%s/.local/share/lordos/web/%s/%s", w->home, w->app.id, w->profile->id) >= (int)sizeof w->profile_dir) return 2;
    if (w->app.icon[0]) {
        char p[PATH_MAX + WEBREG_ICON_PATH_MAX + 2];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", w->config_dir, w->app.icon);
        if (!lstat(p, &st) && S_ISREG(st.st_mode) && strlen(p) < sizeof w->icon_path) snprintf(w->icon_path, sizeof w->icon_path, "%s", p);
    }
    if (mkdir_p(w->profile_dir)) { fprintf(stderr, "lord-web: %s: %s\n", w->profile_dir, strerror(errno)); return 1; }
    char state[PATH_MAX + 16];
    size_t slen = 0;
    snprintf(state, sizeof state, "%s/state.json", w->profile_dir);
    char *stext = webreg_read_file(state, 4096, &slen);
    webpage_state_parse(stext, stext ? slen : 0, &w->state);
    free(stext);
    w->audio_ms = w->note_ms = -1;
    const char *token = getenv("XDG_ACTIVATION_TOKEN");
    char tok[256] = "";
    if (token) snprintf(tok, sizeof tok, "%s", token);
    unsetenv("XDG_ACTIVATION_TOKEN");
    unsetenv("DESKTOP_STARTUP_ID");
    int lock = lock_profile(w);
    if (lock == -1) { ask_running(w, tok); return 0; }
    if (lock < 0) return 1;

    char scope[WEBREG_ID_MAX + 16];
    snprintf(scope, sizeof scope, "lordos-web-%s", w->app.id);
    if (isolate(scope)) return 1;

    weblang_init();
    w->loop = g_main_loop_new(NULL, FALSE);
    webbus_init(w);
    WebWin *win = webwin_new(w);
    if (!win) { fprintf(stderr, "lord-web: no Wayland display\n"); return 1; }
    if (webwin_init_backend(win) || web_engine_start(w)) return 1;
    webwin_attach(win);
    if (w->app.background && w->app.tray) w->tray = webtray_new(w);
    serve_show(w);
    webwin_show(win, tok);
    g_main_loop_run(w->loop);
    webtray_free(w->tray);
    if (w->view) g_object_unref(w->view);
    webwin_free(win);
    return w->exit_code;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    static Web web;
    Web *w = &web;
    if (webargs_parse(argc, argv, &w->args) == WEBARGS_BAD) { usage(); return 2; }
    if (clean_env()) return 2;
    const char *home = getenv("HOME");
    if (!home || home[0] != '/' || !realpath(home, w->home)) { fprintf(stderr, "lord-web: no home folder\n"); return 2; }
    if (snprintf(w->config_dir, sizeof w->config_dir, "%s/.config/lordos/web", w->home) >= (int)sizeof w->config_dir) return 2;
    if (w->args.mode != WEBARGS_APP) {
        /* the probe's channel is fd 3, and no child of WebKit's gets it */
        int fl = fcntl(3, F_GETFD);
        if (fl < 0) { fprintf(stderr, "lord-web: the probe needs its channel on fd 3\n"); return 2; }
        fcntl(3, F_SETFD, fl | FD_CLOEXEC);
        close_range(4, ~0U, 0);
        if (isolate("lordos-webprobe")) return 1;
        wpe_loader_init("libWPEBackend-fdo-1.0.so.1");
        return w->args.mode == WEBARGS_PROBE ? webprobe_run(w) : webprobe_icon_run(w);
    }
    close_range(3, ~0U, 0);
    wpe_loader_init("libWPEBackend-fdo-1.0.so.1");
    return run_app(w);
}
