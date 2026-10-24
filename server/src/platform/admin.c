/* Standalone desktop: system administration belongs to the host distribution.
 * Keep the SDK's unavailable-helper contract; never fork a root helper or
 * restore LordOS networking when running alongside NetworkManager. */
#include "../admin.h"
#include <errno.h>

int admin_start(const char *user) { (void)user; return 0; }
int admin_fd(void) { return -1; }
int admin_known(const char *action) { (void)action; return 0; }
int admin_send(const char *action, const char *arg, size_t len) {
    (void)action; (void)arg; (void)len; errno = ENOTSUP; return -1;
}
void admin_dispatch(void (*fn)(int, const char *, const char *)) { (void)fn; }
/* Networking's legacy parsers share some code with its privileged side. These
 * entrypoints are deliberately unavailable in the standalone binary. */
void admin_say(const char *kind, const char *fmt, ...) { (void)kind; (void)fmt; }
void admin_event(const char *fmt, ...) { (void)fmt; }
int admin_run(const char *const argv[]) { (void)argv; return -1; }
int admin_run_input(const char *const argv[], const char *input) { (void)argv; (void)input; return -1; }
int admin_run_capture(const char *const argv[], char *out, size_t n) {
    (void)argv; if (n) out[0] = 0; return -1;
}
int admin_daemon_exec(const char *name, const char *const argv[], const char *const env[]) {
    (void)name; (void)argv; (void)env; return -1;
}
int admin_daemon_fork(const char *name, void (*fn)(void *), void *arg) {
    (void)name; (void)fn; (void)arg; return -1;
}
int admin_daemon_stop(const char *name) { (void)name; return -1; }
int admin_daemon_running(const char *name) { (void)name; return 0; }
