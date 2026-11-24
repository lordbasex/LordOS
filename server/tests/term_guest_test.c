/* term_guest_test.c — a Machines guest's terminal stream (term_create_fd: the serial console and the agent's shell,
 * docs/VIRTUALIZATION.md 6.18) never acts on this computer: OSC 52 and iTerm's Copy don't set the clipboard, tmux's
 * control mode (DCS 1000p) doesn't start (so no %output reaches a pane that could), OSC 8 makes no link, and the only
 * events passed on are output, size, bell and the end; an ordinary Terminal session keeps all of them. */
#include "term/term.h"
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); } } while (0)

static char seen[1024];
static void ev(int id, const char *what, const char *arg) { (void)id; (void)arg; strncat(seen, what, sizeof seen - strlen(seen) - 2); strcat(seen, " "); }

static void pump(int id) {
    for (int i = 0; i < 20; i++) {
        struct pollfd p[8];
        int ids[8], n = term_pollfds(p, 8, ids);
        if (poll(p, (nfds_t)n, 20) <= 0) continue;
        for (int k = 0; k < n; k++) if (p[k].revents && ids[k] == id) term_dispatch(id, p[k].revents);
    }
}

int main(void) {
    static const char hostile[] =
        "\033]52;c;aG9zdGlsZQ==\007"                         /* OSC 52: "hostile" */
        "\033]1337;Copy=:aG9zdGlsZQ==\007"                   /* iTerm's Copy */
        "\033]8;;https://evil.example/\007click\033]8;;\007" /* a link */
        "\033]9;You have mail\007\033]0;sudo password\007\033]7;file:///etc\007\033]1337;SetProfile=x\007"
        "\033]133;A\007$ \033]133;B\007rm -rf /\r\n\033]133;C\007"
        "\033P1000p%begin 1 1 0\r\n%end 1 1 0\r\n%output %0 \033]52;c;aG9zdGlsZQ==\007\r\n\033\\"   /* tmux -CC */
        "plain\r\n";
    int sv[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    Session *g = term_create_fd(sv[0], 100);
    CHECK(g && g->t.no_clip_set && !g->t.tmux);
    int before = term_list(NULL, 0);
    CHECK(write(sv[1], hostile, sizeof hostile - 1) == (ssize_t)(sizeof hostile - 1));
    pump(g->id);
    term_events(ev);
    CHECK(!strstr(term_clipboard(), "hostile"));
    CHECK(!strstr(seen, "title") && !strstr(seen, "notify") && !strstr(seen, "copy") && !strstr(seen, "run") &&
          !strstr(seen, "tmux") && !strstr(seen, "profile") && !strstr(seen, "cwd") && !strstr(seen, "command"));
    CHECK(!g->tmux && term_list(NULL, 0) == before);          /* no control mode, no pane */
    CHECK(!g->t.nlinks);
    /* a pane of a guest gateway inherits the guard (defence in depth): made by hand here, as tmux_cb would */
    g->tmux = 1;
    Session *pane = term_tmux_pane(g, 0);
    CHECK(pane && pane->t.no_clip_set);
    if (pane) { const char o[] = "\033]52;c;aG9zdGlsZQ==\007"; term_feed(pane, o, sizeof o - 1); CHECK(!strstr(term_clipboard(), "hostile")); term_close(pane); }
    g->tmux = 0;
    term_close(g);
    close(sv[1]);
    /* an ordinary session (the same emulator, not a guest's) still takes OSC 52 */
    int sv2[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sv2));
    Session *o = term_create_fd(sv2[0], 100);
    o->t.no_clip_set = 0;
    seen[0] = 0;
    CHECK(write(sv2[1], "\033]52;c;b3Jk\007", 12) == 12);
    pump(o->id);
    term_events(ev);
    CHECK(!strcmp(term_clipboard(), "ord") && strstr(seen, "copy"));
    term_close(o);
    close(sv2[1]);
    printf("term-guest-test: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
