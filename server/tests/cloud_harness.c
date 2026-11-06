/* cloud_harness.c — lord-server's cloud code (src/cloud) on its own, for tests/cloud_integration.sh: the same thread,
 * rclone daemon, accounts and files on demand, with the keyring replaced by files in $HOME/.harness-keyring (0600)
 * and the browser by a line on stdout. Built with CLOUD_TEST: LORDOS_CLOUD_RCLONE names the rclone binary.
 *
 * One command per line on stdin, one line of JSON per command on stdout:
 *   state | providers | add KIND NAME FIELDS-JSON | answer STATE VALUE | trash ID | measure ID | delete PATH | remove ID [discard] | rename ID NAME | status PATH | free PATH
 *   keep PATH | unkeep PATH | link PATH EXPIRE | unlink PATH | links ID | pause ID|all | resume ID|all | restart
 *   about ID | website ID | options JSON | secret ID | wait SUBSTRING SECONDS | sleep MS | quit */
#include "../src/cloud/cloud.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char *last_state;
static int got_req;
static char *got_reply;

static void on_push(const char *j) { free(last_state); last_state = strdup(j); }
static void on_reply(int req, const char *j) { got_req = req; free(got_reply); got_reply = strdup(j); }
static int on_open(const char *url) { printf("{\"opened\":\"%s\"}\n", url); fflush(stdout); return 0; }

static long long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void pump(int wait_ms) {
    long long end = ms() + wait_ms;
    do {
        struct pollfd p = { cloud_fd(), POLLIN, 0 };
        long long left = end - ms();
        if (poll(&p, 1, left > 0 ? (int)left : 0) > 0) cloud_dispatch(on_push, on_reply, on_open);
    } while (ms() < end);
}

/* ---- the keyring stand-in ---- */
static void kpath(char *out, size_t n, const char *id) { snprintf(out, n, "%s/.harness-keyring/%s", getenv("HOME"), id); }
static int k_get(const char *id, char **s) {
    char p[1024];
    kpath(p, sizeof p, id);
    FILE *f = fopen(p, "r");
    if (!f) return 1;
    char b[65536];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    *s = strdup(b);
    return *s ? 0 : -1;
}
static int k_set(const char *id, const char *label, const char *s) {
    (void)label;
    char p[1024], d[1024];
    snprintf(d, sizeof d, "%s/.harness-keyring", getenv("HOME"));
    mkdir(d, 0700);
    kpath(p, sizeof p, id);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, s, strlen(s)) == (ssize_t)strlen(s);
    close(fd);
    return ok ? 0 : -1;
}
static int k_del(const char *id) { char p[1024]; kpath(p, sizeof p, id); unlink(p); return 0; }
static const CloudSecrets ks = { k_get, k_set, k_del };

static void out(char *s) { printf("%s\n", s ? s : "null"); fflush(stdout); free(s); }

/* an answer now, or the one that comes through reply(req) */
static void later(char *r, int req) {
    if (r || !req) { out(r); return; }
    got_req = 0;
    long long end = ms() + 120000;
    while (got_req != req && ms() < end) pump(100);
    out(got_req == req ? strdup(got_reply) : strdup("{\"error\":\"timeout\"}"));
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (getenv("LORDOS_ASSETS")) cloud_set_assets(getenv("LORDOS_ASSETS"));
    if (cloud_start(getenv("HOME"), &ks)) { printf("{\"error\":\"start\"}\n"); return 1; }
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, stdin) > 0) {
        line[strcspn(line, "\n")] = 0;
        char *cmd = strtok(line, " ");
        if (!cmd) continue;
        char *a1 = strtok(NULL, " "), *rest = a1 ? a1 + strlen(a1) + 1 : NULL;
        pump(10);
        if (!strcmp(cmd, "state")) out(cloud_state_json());
        else if (!strcmp(cmd, "providers")) { int req = 0; char *r = cloud_providers(&req); later(r, req); }
        else if (!strcmp(cmd, "add") && a1) {
            char *name = strtok(NULL, " "), *fields = name ? name + strlen(name) + 1 : NULL;
            int req = 0;
            if (!fields) out(strdup("{\"error\":\"usage\"}"));
            else { char *r = cloud_add(a1, name, fields, &req); later(r, req); }
        } else if (!strcmp(cmd, "remove") && a1) { char *d = strtok(NULL, " "); out(cloud_remove(a1, d && !strcmp(d, "discard"))); }
        else if (!strcmp(cmd, "rename") && a1 && rest) out(cloud_rename(a1, rest));
        else if (!strcmp(cmd, "status") && a1) { const char *p[] = { a1 }; out(cloud_status(p, 1)); }
        else if (!strcmp(cmd, "free") && a1) { const char *p[] = { a1 }; out(cloud_free(p, 1)); }
        else if (!strcmp(cmd, "keep") && a1) { const char *p[] = { a1 }; cloud_keep(p, 1, 1); out(strdup("{\"ok\":true}")); }
        else if (!strcmp(cmd, "unkeep") && a1) { const char *p[] = { a1 }; cloud_keep(p, 1, 0); out(strdup("{\"ok\":true}")); }
        else if ((!strcmp(cmd, "link") || !strcmp(cmd, "unlink")) && a1) {
            int req = 0;
            char *r = !strcmp(cmd, "link") ? cloud_link(a1, rest ? rest : "never", &req) : cloud_unlink(a1, &req);
            if (r || !req) { out(r); continue; }
            got_req = 0;
            long long end = ms() + 60000;
            while (got_req != req && ms() < end) pump(100);
            out(got_req == req ? strdup(got_reply) : strdup("{\"error\":\"timeout\"}"));
        } else if (!strcmp(cmd, "links") && a1) out(cloud_links(a1));
        else if (!strcmp(cmd, "answer") && a1) { int req = 0; char *r = cloud_answer(a1, rest ? rest : "", &req); later(r, req); }
        else if (!strcmp(cmd, "trash") && a1) { int req = 0; char *r = cloud_empty_trash(a1, &req); later(r, req); }
        else if (!strcmp(cmd, "measure") && a1) { int req = 0; char *r = cloud_measure(a1, &req); later(r, req); }
        else if (!strcmp(cmd, "delete") && a1) { int req = 0; const char *p[] = { a1 }; char *r = cloud_delete(p, 1, &req); later(r, req); }
        else if (!strcmp(cmd, "pause") && a1) { cloud_pause(strcmp(a1, "all") ? a1 : NULL, 1); out(strdup("{\"ok\":true}")); }
        else if (!strcmp(cmd, "resume") && a1) { cloud_pause(strcmp(a1, "all") ? a1 : NULL, 0); out(strdup("{\"ok\":true}")); }
        else if (!strcmp(cmd, "restart")) { cloud_restart(); out(strdup("{\"ok\":true}")); }
        else if (!strcmp(cmd, "about") && a1) { cloud_refresh_about(a1); out(strdup("{\"ok\":true}")); }
        else if (!strcmp(cmd, "website") && a1) { char *w = cloud_website(a1); if (w) { printf("\"%s\"\n", w); fflush(stdout); free(w); } else out(NULL); }
        else if (!strcmp(cmd, "options") && a1) { char opts[4096]; snprintf(opts, sizeof opts, "%s%s%s", a1, rest ? " " : "", rest ? rest : ""); out(cloud_set_options(opts)); }
        else if (!strcmp(cmd, "secret") && a1) { char *s = NULL; out(k_get(a1, &s) ? strdup("null") : s); }
        else if (!strcmp(cmd, "wait") && a1) {
            /* until the state has this text (spaces as '+') or the seconds pass */
            for (char *p = a1; *p; p++) if (*p == '+') *p = ' ';
            int secs = rest ? atoi(rest) : 30;
            long long end = ms() + secs * 1000LL;
            int ok = 0;
            while (!ok && ms() < end) {
                char *s = cloud_state_json();
                ok = s && strstr(s, a1);
                free(s);
                if (!ok) pump(200);
            }
            out(strdup(ok ? "{\"ok\":true}" : "{\"error\":\"timeout\"}"));
        } else if (!strcmp(cmd, "sleep") && a1) { pump(atoi(a1)); out(strdup("{\"ok\":true}")); }
        else if (!strcmp(cmd, "quit")) { cloud_stop(); out(strdup("{\"ok\":true}")); break; }
        else out(strdup("{\"error\":\"usage\"}"));
    }
    free(line);
    return 0;
}
