/* companion.c — `lord-server --companion <id> <0|1>`: a packaged app's helper program (docs/GO-LINK.md 3.2).
 *
 * lord-server forked this and exec'd itself, so it starts as a new process with one thread and two inherited
 * descriptors: 3, a SOCK_SEQPACKET socket to the app, and 4, if the manifest declares screen-share, a connection to the
 * sharing API already tied to this app. It opens a third, 5, on the home folder, to list it. Nothing else comes in: no path, no argument but the id and whether the
 * network is allowed. Everything here runs before a byte of the package does:
 *   the id is a packaged app's, and this process isn't root;
 *   the whole signed package checks, its key is a Verified author's and nothing revoked it, the binary is read through one
 *   descriptor and that same descriptor is what runs, with execveat;
 *   the process joins the app's scope, then the sandbox (sandbox_exec_companion) goes on.
 * Anything that fails leaves it here: nothing runs, and the app hears why on fd 3. Run by hand, it has no fd 3
 * and stops at once, and the verification is the only thing that could ever run the binary. */
#include "app.h"
#include "../lpk.h"
#include "../sec/cgroup.h"
#include "../sec/json.h"
#include "../sec/pkgapp.h"
#include "../sec/sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

static void tell(const char *state, const char *code) {
    char q[256], j[320];
    json_quote(q, sizeof q, code ? code : "");
    int n = snprintf(j, sizeof j, "{\"companion\":\"%s\",\"code\":%s}", state, q);
    if (send(3, j, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) { /* the app is gone */ }
}

static int refuse(const char *id, const char *code) {
    fprintf(stderr, "companion[%s]: not started: %s\n", id, code);
    tell("refused", code);
    return 1;
}

/* ~/.local/share/lordos/appdata/<id>, made if missing, with no link followed on the way. */
static int data_folder(int homefd, const char *id) {
    const char *const parts[] = { ".local", "share", "lordos", "appdata", id };
    int at = dup(homefd);
    for (size_t i = 0; at >= 0 && i < sizeof parts / sizeof parts[0]; i++) {
        mkdirat(at, parts[i], 0700);
        int next = openat(at, parts[i], O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(at);
        at = next;
    }
    return at;
}

int companion_main(int argc, char **argv) {
    if (argc != 4 || !lpk_valid_id(argv[2]) || (strcmp(argv[3], "0") && strcmp(argv[3], "1"))) return 2;
    const char *id = argv[2];
    int network = argv[3][0] == '1', type = 0;
    socklen_t tl = sizeof type;
    if (getsockopt(3, SOL_SOCKET, SO_TYPE, &type, &tl) || type != SOCK_SEQPACKET) return 2;   /* not from lord-server */
    if (getuid() == 0 || geteuid() == 0) return refuse(id, "companion.root");

    /* fd 5 first, before anything can take the number: the only way the engine can list the home (sandbox.c, home_rules) */
    const char *home = getenv("HOME");
    if (!home || home[0] != '/' || !home[1]) return refuse(id, "companion.noHome");
    int listing = open(home, O_RDONLY | O_DIRECTORY);
    if (listing < 0 || (listing != 5 && (dup2(listing, 5) != 5 || close(listing)))) return refuse(id, "companion.noHome");

    PkgManifest m;
    LpkSigInfo si;
    char err[200] = "";
    int bin = -1;
    if (pkgapp_verify(id, &m, &si, &bin, err, sizeof err)) return refuse(id, err[0] ? err : "companion.notVerified");
    fprintf(stderr, "companion[%s]: %s, verified author %s\n", id, m.version, si.signer);

    char dir[PATH_MAX];
    if (pkgapp_dir(id, dir, sizeof dir)) return refuse(id, "companion.notInstalled");
    int homefd = open(home, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (homefd < 0) return refuse(id, "companion.noHome");
    mkdirat(homefd, "Downloads", 0755);
    int dl = openat(homefd, "Downloads", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);      /* a link is not followed out of the home */
    int data = data_folder(homefd, id);
    close(homefd);
    int app = open(dir, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dl < 0) return refuse(id, "companion.noDownloads");
    if (data < 0) return refuse(id, "companion.noData");
    if (app < 0) return refuse(id, "companion.notInstalled");

    char scope[80];
    sandbox_scope_name(id, scope, sizeof scope);
    CgroupLimits lim = { .mem_high_mb = 192, .mem_max_mb = 512, .cpu_percent = 100 };   /* the app's, shared with it */
    if (cgroup_enter(scope, &lim) && getenv("LORDOS_SESSIOND")) return refuse(id, "companion.noScope");
    cgroup_settle();

    /* the binary's descriptor closes at exec; 0, 1, 2 lead nowhere */
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0) return refuse(id, "companion.sandbox");
    SandboxCompanionCfg sb = { .home = home, .downloads_fd = dl, .data_fd = data, .app_fd = app, .bin_fd = bin, .network = network };
    if (sandbox_exec_companion(&sb)) return refuse(id, "companion.sandbox");
    close(dl);
    close(data);
    close(app);

    char e_home[PATH_MAX + 8], e_id[64];
    snprintf(e_home, sizeof e_home, "HOME=%s", home);
    snprintf(e_id, sizeof e_id, "LORD_APP_ID=%s", id);
    const char *lang = getenv("LANG");
    char e_lang[64];
    snprintf(e_lang, sizeof e_lang, "LANG=%s", lang && strlen(lang) < 40 ? lang : "C");
    char *const env[] = { e_home, e_id, e_lang, NULL };
    char *const av[] = { (char *)"companion", NULL };
    tell("started", "");
    dup2(null, 0);
    dup2(null, 1);
    dup2(null, 2);
    syscall(SYS_execveat, bin, "", av, env, AT_EMPTY_PATH);
    tell("refused", "companion.exec");
    return 1;
}
