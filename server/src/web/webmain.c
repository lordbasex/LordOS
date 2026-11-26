/* webmain.c — lord-web's start (see web.h and docs/WEBAPPS.md, "Starting an app" and "Isolation").
 *
 * Only lord-server starts it, from the fixed path next to its own binary, with exactly
 * `--app <app> --profile <profile>` (with `--inspect` for the developer tools), `--probe` or `--probe-icon` (webargs.c).
 * Before anything else lord-web:
 *   - refuses any other argv, and an environment that would turn WebKit's sandbox off; drops the variables that point
 *     WebKit or GStreamer at other code (an inspector server, injected bundles, other process binaries, plugin paths);
 *   - for an app: reads its own registry file read-only (webreg.c) and refuses to start on any mismatch: the file's
 *     id, the profile, any field;
 *   - takes the profile's lock (one process per app and profile: a second start asks the first to show its window
 *     over the session bus, org.lordos.WebApp1.Show, and exits);
 *   - with --inspect: refuses to start unless developer mode is on (~/.config/lordos/web/developer.json, which only
 *     lord.webapps writes), then hands WebKit its remote inspector's address, 127.0.0.1 and a free port, and opens it
 *     in the person's browser once that port listens from this process (webinspect.h);
 *   - enters its own scope, app-lordos-lordos-web-<app>.scope (the firewall's and Monitor's row for the app; the probe's
 *     is lordos-webprobe), with memory and CPU limits that all the app's profiles and WebKit's processes share (webpage_scope_limits);
 *   - restricts itself with Landlock's scoping (no abstract unix socket and no signal outside its own processes) and
 *     no_new_privs. Landlock's file rules are not used: a process under them can't mount, and WebKit's bubblewrap
 *     sandbox, which keeps the web content and network processes off the files, mounts for every child it starts. */
#include "web.h"
#include "webwin.h"
#include "webback.h"
#include "../sec/cgroup.h"
#include "../sec/sandbox.h"

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

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
    fprintf(stderr, "usage: lord-web --app <app> --profile <profile> [--inspect] | --probe | --probe-icon\n"
                    "lord-web is started by lord-server only (docs/WEBAPPS.md).\n");
}

/* 0, or -1 when the environment asks for WebKit without its sandbox */
static int clean_env(void) {
    if (getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS")) {
        fprintf(stderr, "lord-web: WebKit's sandbox is never turned off\n");
        return -1;
    }
    /* JSC_*: JavaScriptCore options (JSC_useSharedArrayBuffer would hand SharedArrayBuffer to every page, not only the
     * cross-origin-isolated ones WebKit turns it on for itself) */
    static const char *const drop[] = { "WEBKIT_", "WPE_", "JSC_", "GST_PLUGIN", "GST_REGISTRY", "GIO_EXTRA_MODULES", "GIO_MODULE_DIR", "LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT", NULL };
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
        /* the app's scope, shared by its profiles' lord-web and WebKit processes (webpage_scope_limits) */
        long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE), ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        long ram_mb = pages > 0 && psz > 0 ? (long)((unsigned long long)pages * (unsigned long long)psz >> 20) : 0;
        WebScopeLimits sl = webpage_scope_limits(ram_mb, ncpu > 0 ? (int)ncpu : 1);
        CgroupLimits lim = { .mem_high_mb = sl.mem_high_mb, .mem_max_mb = sl.mem_max_mb, .cpu_percent = sl.cpu_percent };
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

/* ---- developer tools (webinspect.h) ---- */

/* --inspect: only while developer mode is on; a free port on 127.0.0.1 for WebKit's inspector, before WebKit reads it
 * (the first web context, web_engine_start). 0, or -1: not started. */
static int inspect_prepare(Web *w) {
    char path[PATH_MAX + 32];
    size_t len = 0;
    snprintf(path, sizeof path, "%s/" WEBINSPECT_FILE, w->config_dir);
    char *text = webreg_read_file(path, WEBINSPECT_FILE_MAX, &len);
    int on = text && webinspect_parse(text, len);
    free(text);
    if (!on) { fprintf(stderr, "lord-web: --inspect needs developer mode (Settings, Web Apps): not started\n"); return -1; }
    /* WebKit refuses port 0, and its server has no authentication: the first free one of the ports every browser
     * refuses to reach (webguard.h), which WebKit binds right after (webinspect_listening checks that the socket on it
     * is ours before the browser is pointed at the proxy in front of it). WebKit's inspector server behind it takes
     * a port of its own choosing (WEBKIT_INSPECTOR_SERVER doesn't move it in 2.48): docs/WEBAPPS.md says why a
     * browser can't use it. */
    unsigned port = 0;
    for (size_t i = 0; !port && i < WEBGUARD_BACKEND_NPORTS; i++) {
        int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)WEBGUARD_BACKEND_PORTS[i]), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        if (s >= 0 && !bind(s, (struct sockaddr *)&a, sizeof a)) port = WEBGUARD_BACKEND_PORTS[i];
        if (s >= 0) close(s);
    }
    char addr[64];
    if (!webguard_backend_port_ok(port) || webinspect_address(port, addr, sizeof addr) || setenv(WEBINSPECT_ENV, addr, 1)) {
        fprintf(stderr, "lord-web: no free port on " WEBINSPECT_HOST " for the developer tools: not started\n");
        return -1;
    }
    w->inspect_port = port;
    fprintf(stderr, "lord-web: developer tools: WebKit's inspector on %s for this window only\n", addr);
    return 0;
}

void web_inspect_open(Web *w) {
    char url[128];
    if (!w->inspect_proxy || webproxy_open_url(w->inspect_proxy, url, sizeof url)) return;
    webbus_open_uri(w, url);
    explicit_bzero(url, sizeof url);
}

#define INSPECT_TICK_MS 500
#define INSPECT_TRIES   40                  /* 20 s */
#define INSPECT_FDS     1024

/* the inodes of this process's sockets */
static int own_sockets(unsigned long *out, int max) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        char p[64], l[64];
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/proc/self/fd/%.32s", e->d_name);
        ssize_t k = readlink(p, l, sizeof l - 1);
        if (k <= 0) continue;
        l[k] = 0;
        unsigned long ino = 0;
        if (sscanf(l, "socket:[%lu]", &ino) == 1 && ino) out[n++] = ino;
    }
    closedir(d);
    return n;
}

static gboolean inspect_tick(gpointer d) {
    Web *w = d;
    static unsigned long inodes[INSPECT_FDS];
    int n = own_sockets(inodes, INSPECT_FDS);
    gchar *tcp = NULL;
    gsize len = 0;
    int r = g_file_get_contents("/proc/self/net/tcp", &tcp, &len, NULL) ? webinspect_listening(tcp, len, w->inspect_port, inodes, n) : 0;
    g_free(tcp);
    if (r == 0 && ++w->inspect_tries < INSPECT_TRIES) return G_SOURCE_CONTINUE;
    w->inspect_timer = 0;
    if (r == 1 && (w->inspect_proxy = webproxy_start(w->inspect_port))) {
        fprintf(stderr, "lord-web: developer tools listen: opened in the browser\n");
        web_inspect_open(w);
        webbus_notify_inspect(w, weblang("inspect.title"), weblang("inspect.body"));
    } else {
        fprintf(stderr, "lord-web: the developer tools' port %u %s: not opened\n", w->inspect_port,
                r < 0 ? "is someone else's" : r == 1 ? "listens, but the gate in front of it couldn't" : "never listened");
        webbus_notify_text(w, weblang("inspect.failed"), "");
    }
    return G_SOURCE_REMOVE;
}

void web_inspect_watch(Web *w) {
    if (!w->inspect_port || w->inspect_timer) return;
    w->inspect_tries = 0;
    w->inspect_timer = g_timeout_add(INSPECT_TICK_MS, inspect_tick, w);
}

/* ---- one process per app and profile ---- */

/* Show(token): anyone may (a second start of the app does); Edit(command), SetColorScheme("light" | "dark") and
 * SetUserAgent("chrome" | "edge" | "firefox" | "lordos"): only lord-server, checked by pid (webbus.c) */
static void show_call(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method, GVariant *p, GDBusMethodInvocation *inv, gpointer d) {
    (void)c; (void)path; (void)iface;
    if (!strcmp(method, "Show")) {
        const char *token = NULL;
        g_variant_get(p, "(&s)", &token);
        web_show(d, token);
    } else if (!strcmp(method, "Edit")) {
        const char *cmd = NULL;
        g_variant_get(p, "(&s)", &cmd);
        int k = webedit_command(cmd);
        if (k < 0 || !sender) {
            g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.InvalidArgs", "unknown command");
            return;
        }
        webbus_edit(d, sender, k);
    } else if (!strcmp(method, "SetColorScheme")) {
        const char *word = NULL;
        g_variant_get(p, "(&s)", &word);
        int k = webscheme_parse(word);
        if (k < 0 || !sender) {
            g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.InvalidArgs", "light or dark");
            return;
        }
        webbus_scheme(d, sender, k);
    } else if (!strcmp(method, "SetUserAgent")) {
        const char *word = NULL;
        g_variant_get(p, "(&s)", &word);
        int k = webreg_ua_value(word);
        if (k < 0 || !sender) {
            g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.InvalidArgs", "chrome, edge, firefox or lordos");
            return;
        }
        webbus_user_agent(d, sender, k);
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}
static const GDBusInterfaceVTable show_vt = { show_call, NULL, NULL, { 0 } };
static const char SHOW_XML[] = "<node><interface name='" SHOW_IFACE "'><method name='Show'><arg type='s' direction='in'/></method>"
                               "<method name='Edit'><arg type='s' direction='in'/></method>"
                               "<method name='SetColorScheme'><arg type='s' direction='in'/></method>"
                               "<method name='SetUserAgent'><arg type='s' direction='in'/></method></interface></node>";

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
    char *stext = webreg_read_file(state, WEBPAGE_STATE_MAX, &slen);
    webpage_state_parse(stext, stext ? slen : 0, &w->state);
    free(stext);
    w->audio_ms = w->note_ms = -1;
    w->scheme = webscheme_from_env(getenv(WEBSCHEME_ENV));      /* lord-server's word for the desktop's mode */
    unsetenv(WEBSCHEME_ENV);
    const char *token = getenv("XDG_ACTIVATION_TOKEN");
    char tok[256] = "";
    if (token) snprintf(tok, sizeof tok, "%s", token);
    unsetenv("XDG_ACTIVATION_TOKEN");
    unsetenv("DESKTOP_STARTUP_ID");
    int lock = lock_profile(w);
    if (lock == -1) { ask_running(w, tok); return 0; }
    if (lock < 0) return 1;
    if (w->args.inspect && inspect_prepare(w)) return 2;

    char scope[WEBREG_ID_MAX + 16];
    snprintf(scope, sizeof scope, "lordos-web-%s", w->app.id);
    if (isolate(scope)) return 1;

    weblang_init();
    /* the app's name for what WebKit sends the notification server itself: a service worker's notification
     * (registration.showNotification) goes there from WebKit, not through the view's "show-notification"
     * (docs/WEBAPPS.md, "Notifications"). Without a name GLib's is NULL: WebKit's Notify carried "[Invalid UTF-8]" and
     * GLib logged a critical for each one. The registry's name is checked UTF-8 already; anything else gets the program's name. */
    g_set_application_name(w->app.name[0] && g_utf8_validate(w->app.name, -1, NULL) ? w->app.name : "lord-web");
    w->loop = g_main_loop_new(NULL, FALSE);
    webbus_init(w);
    WebWin *win = webwin_new(w);
    if (!win) { fprintf(stderr, "lord-web: no Wayland display\n"); return 1; }
    if (webwin_init_backend(win) || web_engine_start(w)) return 1;
    webwin_attach(win);
    if (w->app.background && w->app.tray) w->tray = webtray_new(w);
    serve_show(w);
    webwin_show(win, tok);
    web_inspect_watch(w);
    g_main_loop_run(w->loop);
    webtray_free(w->tray);
    if (w->view) g_object_unref(w->view);
    webwin_free(win);
    return w->exit_code;
}

/* A crash of lord-web itself (the WebKit UI process lives in it: a C++ exception nobody catches ends in abort) leaves
 * its stack in its web/<app>-<profile>.log (lordlog.h), for addr2line: libstdc++ prints "terminate called after throwing ..." first, and the stack at
 * the abort still holds the frame that threw. Then the signal's own default (a core, the exit status lord-server
 * sees). */
static void crash_log(int sig) {
    static const char head[] = "lord-web: crashed, stack:\n";
    void *pcs[64];
    if (write(2, head, sizeof head - 1)) {}
    int n = backtrace(pcs, 64);
    backtrace_symbols_fd(pcs, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void crash_hooks(void) {
    static char alt[64 * 1024];
    stack_t st = { .ss_sp = alt, .ss_size = sizeof alt };
    sigaltstack(&st, NULL);
    void *warm[1];
    backtrace(warm, 1);                 /* loads libgcc's unwinder now, not inside the handler */
    struct sigaction sa = { .sa_handler = crash_log, .sa_flags = SA_ONSTACK | SA_RESETHAND };
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = { SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGILL };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    crash_hooks();
    static Web web;
    Web *w = &web;
    if (webargs_parse(argc, argv, &w->args) == WEBARGS_BAD) { usage(); return 2; }
    w->server_pid = getppid();          /* lord-server forks and execs lord-web itself (web/webmgr.c) */
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
        webback_process_init();
        return w->args.mode == WEBARGS_PROBE ? webprobe_run(w) : webprobe_icon_run(w);
    }
    close_range(3, ~0U, 0);
    webback_process_init();
    return run_app(w);
}
