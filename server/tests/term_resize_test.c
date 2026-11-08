/* term_resize_test.c — what Claude Code needs from the Terminal to draw where it is (0.8.8.6):
 *
 *  - the parser: a frame as Claude Code writes one (SGR in direct color, OSC 8 links, UTF-8, CHA such as
 *    ESC [ 14 G, synchronized output) gives the same screen whole, cut in two at every byte, byte by byte, and
 *    with the grid resized at every cut; an ESC right after the ESC that ends an OSC or an APC begins a
 *    sequence instead of being swallowed ("[14G" printed as text);
 *  - the grid from a terminal's box (term_grid_size): the same on every display scale and in every scaled
 *    pass (overview, minimize), and none for a box that can't hold one (no 2 × 1 SIGWINCH);
 *  - the pty's size (pty_sync_size): set back to the grid's when something else changed it;
 *  - the holders (hold.c): while one lord-server has a session, a second one of the same user gets nothing
 *    (two readers of one pty split the program's output and fight over its size), and gets it once the
 *    first is gone; a lord-server with its own XDG_RUNTIME_DIR looks for holders there.
 *
 * The binary is also the holder it starts (hold_spawn re-executes /proc/self/exe --term-holder). */
#include "term/term.h"
#include "sec/cgroup.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

void term_set_clipboard(const char *s, size_t n) { (void)s; (void)n; }
int cgroup_enter(const char *name, const CgroupLimits *lim) { (void)name; (void)lim; return -1; }   /* pty.c: never spawns here */
static void respond(Term *t, const char *s, size_t n) { (void)t; (void)s; (void)n; }

static int fails, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }

static TermProfile prof;

static void init(Term *t, int cols, int rows) {
    if (vt_init(t, cols, rows, 50, &prof)) { printf("vt_init failed\n"); exit(1); }
    t->respond = respond;
}

static void feed(Term *t, const char *s, size_t n) { vt_write(t, (const uint8_t *)s, n); }

/* 1 if both screens (cells, colors, attributes, cursor, parser state) are the same */
static int same(Term *a, Term *b) {
    if (a->cols != b->cols || a->rows != b->rows || a->cx != b->cx || a->cy != b->cy || a->state != b->state) return 0;
    for (int r = 0; r < a->rows; r++) {
        Line *x = vt_view_line(a, r, NULL), *y = vt_view_line(b, r, NULL);
        int nx = x ? x->len : 0, ny = y ? y->len : 0, n = nx > ny ? nx : ny;
        for (int c = 0; c < n; c++) {
            Cell e = { 0 }, cx = c < nx ? x->c[c] : e, cy = c < ny ? y->c[c] : e;
            if (cx.cp != cy.cp || cx.fg != cy.fg || cx.bg != cy.bg || cx.attr != cy.attr) return 0;
        }
    }
    return 1;
}

/* a '[' anywhere on the screen: the frame below has none of its own, so one is a CSI's that lost its ESC */
static int has_bracket(Term *t) {
    for (int r = 0; r < t->rows; r++) {
        Line *l = vt_view_line(t, r, NULL);
        for (int c = 0; l && c < l->len; c++) if (l->c[c].cp == '[') return 1;
    }
    return 0;
}

static uint32_t cp_at(Term *t, int row, int col) {
    Line *l = vt_view_line(t, row, NULL);
    return l && col < l->len ? l->c[col].cp : 0;
}

/* A frame the way Claude Code's renderer writes one: synchronized output, the cursor hidden, moves relative to
 * where it is, columns by CHA, colors in direct color, links (OSC 8, ended by ST and by BEL), a title, UTF-8. */
static const char frame[] =
    "\033[?2026h\033[?25l\033[2K\033[1G\033[38;2;215;119;87m\xe2\x97\x8f\033[39m Hecho. Las \033[14Gteclas "
    "especiales del \033]8;;https://example.org/mx\033\\MX Keys\033]8;;\033\\ y el MX Master 3\r\n"
    "\033[2K\033[48;2;2;40;0m+ added line\033[49m\033[K\r\n"
    "  \033[38;5;75m\033]8;id=1;file:///x\amain\033]8;;\a\033[39m \xc3\xb1 \xe2\x86\x92 \xf0\x9f\x87\xa6\xf0\x9f\x87\xb7 fin"
    "\033]0;\xe2\x97\x91 Claude Code\a\033[3A\033[5C\033[1mB\033[22m\033[2B\033[7G\033[0K\r\n"
    "\033[?25h\033[?2026l";

static void test_split(void) {
    const size_t n = sizeof frame - 1;
    Term ref;
    init(&ref, 40, 8);
    feed(&ref, frame, n);
    check(!has_bracket(&ref), "the frame fed whole has no stray '['");
    check(cp_at(&ref, 0, 13) == 't', "ESC [ 14 G puts \"teclas\" at column 14");

    int split_bad = 0, byte_bad = 0, resize_bad = 0, bracket_bad = 0;
    for (size_t i = 1; i < n; i++) {
        Term t;
        init(&t, 40, 8);
        feed(&t, frame, i);
        feed(&t, frame + i, n - i);
        if (!same(&ref, &t)) { if (!split_bad) printf("  first split that differs: %zu\n", i); split_bad++; }
        vt_free(&t);

        /* the grid resized at the cut, fed in one write on each side or a byte at a time: the same */
        Term a, b;
        init(&a, 40, 8);
        init(&b, 40, 8);
        feed(&a, frame, i);
        for (size_t k = 0; k < i; k++) feed(&b, frame + k, 1);
        vt_resize(&a, 31, 6);
        vt_resize(&b, 31, 6);
        feed(&a, frame + i, n - i);
        for (size_t k = i; k < n; k++) feed(&b, frame + k, 1);
        if (!same(&a, &b)) { if (!resize_bad) printf("  first resize cut that differs: %zu\n", i); resize_bad++; }
        if (has_bracket(&a) || has_bracket(&b)) bracket_bad++;
        vt_free(&a);
        vt_free(&b);
    }
    Term t;
    init(&t, 40, 8);
    for (size_t k = 0; k < n; k++) feed(&t, frame + k, 1);
    if (!same(&ref, &t)) byte_bad = 1;
    vt_free(&t);
    vt_free(&ref);
    check(!split_bad, "cut in two at every byte: the same screen");
    check(!byte_bad, "byte by byte: the same screen");
    check(!resize_bad, "resized at every cut: the same as resizing between whole writes");
    check(!bracket_bad, "resized at every cut: no CSI printed as text");
}

static void test_string_esc(void) {
    Term t;
    init(&t, 40, 3);
    /* an OSC ended by a bare ESC, then another ESC that begins a CSI */
    const char *a = "\033]0;title\033\033[14Gx";
    feed(&t, a, strlen(a));
    check(cp_at(&t, 0, 13) == 'x' && !has_bracket(&t), "ESC after the ESC that ends an OSC begins a CSI");
    vt_free(&t);

    init(&t, 40, 3);
    const char *b = "\033_Gq=2;AAAA\033\033[3Gy z";
    feed(&t, b, strlen(b));
    check(cp_at(&t, 0, 2) == 'y' && cp_at(&t, 0, 4) == 'z' && !has_bracket(&t), "ESC after the ESC that ends an APC begins a CSI");
    vt_free(&t);

    init(&t, 40, 3);
    const char *c = "abcdef\033]0;t\033\r\033[Cx";
    feed(&t, c, strlen(c));
    check(cp_at(&t, 0, 0) == 'a' && cp_at(&t, 0, 1) == 'x' && !has_bracket(&t), "a control after the ESC that ends an OSC is run, the CSI after it too");
    vt_free(&t);

    init(&t, 40, 3);
    const char *d = "\033]8;;u\033\\L\033]8;;\033\\\033[5Gw";
    feed(&t, d, strlen(d));
    check(cp_at(&t, 0, 0) == 'L' && cp_at(&t, 0, 4) == 'w', "OSC ended by ST: unchanged");
    vt_free(&t);

    /* tmux's passthrough doubles the ESCs inside a DCS: still one string up to the real ST */
    init(&t, 40, 3);
    const char *e = "\033Ptmux;\033\033]0;x\033\033\\\033\\ok";
    feed(&t, e, strlen(e));
    check(cp_at(&t, 0, 0) == 'o' && cp_at(&t, 0, 1) == 'k', "a DCS with doubled ESCs ends at its ST");
    vt_free(&t);
}

static void test_grid(void) {
    const int pad = 8, cw = 7, ch = 17;
    int cols = 0, rows = 0;
    check(term_grid_size(1836, 969, pad, cw, ch, &cols, &rows) && cols == 260 && rows == 56, "1836 x 969: 260 x 56");
    check(!term_grid_size(0, 0, pad, cw, ch, &cols, &rows), "a 0 x 0 box: no grid");
    check(!term_grid_size(20, 500, pad, cw, ch, &cols, &rows), "a box narrower than a cell: no grid");
    check(!term_grid_size(500, 30, pad, cw, ch, &cols, &rows), "a box shorter than a cell: no grid");
    check(!term_grid_size(NAN, 500, pad, cw, ch, &cols, &rows), "NaN: no grid");
    /* every logical width and height, drawn at every display scale and every scaled pass: (w * k) / k */
    static const float ks[] = { 1, 1.25f, 1.5f, 1.75f, 2, 2.5f, 3, 0.5f, 0.35f, 0.7f, 0.49f, 0.343f, 0.2401f, 0.125f, 0.3f, 0.6f, 0.9f, 1.1f };
    int bad = 0;
    for (int w = 2 * pad + cw; w <= 4000 && bad < 5; w++) {
        int want_c = (w - 2 * pad) / cw, want_r = (w - 2 * pad) / ch;
        if (want_c < 2) want_c = 2;
        if (want_r < 1) want_r = 1;
        if (want_r > TERM_MAX_ROWS) want_r = TERM_MAX_ROWS;
        for (size_t i = 0; i < sizeof ks / sizeof *ks; i++) {
            float k = ks[i], lw = (float)w * k / k;
            int c = 0, r = 0;
            if (!term_grid_size(lw, lw < 2 * pad + ch ? 2 * pad + ch : lw, pad, cw, ch, &c, &r) || c != want_c ||
                (lw >= 2 * pad + ch && r != want_r)) {
                printf("  width %d at %.4f: %d x %d, want %d x %d\n", w, (double)k, c, r, want_c, want_r);
                bad++;
                break;
            }
        }
    }
    check(!bad, "the same grid at every scale");
}

static void test_pty_size(void) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    char name[128];
    if (m < 0 || grantpt(m) || unlockpt(m) || ptsname_r(m, name, sizeof name)) { printf("skipped pty size: no pty\n"); return; }
    int s = open(name, O_RDWR | O_NOCTTY);
    pty_resize(m, 260, 56, 0, 0);
    check(pty_sync_size(m, 260, 56, 0, 0) == 0, "the grid's size: nothing to set");
    /* something else sets it (a program's `stty cols 80`, another lord-server) */
    struct winsize ws = { 24, 80, 0, 0 };
    ioctl(s, TIOCSWINSZ, &ws);
    check(pty_sync_size(m, 260, 56, 0, 0) == 1, "another size: set again");
    memset(&ws, 0, sizeof ws);
    ioctl(s, TIOCGWINSZ, &ws);
    check(ws.ws_col == 260 && ws.ws_row == 56, "the program sees the grid's size");
    int p[2];
    if (!pipe(p)) {
        check(pty_sync_size(p[0], 260, 56, 0, 0) == -1, "not a pty: -1");
        close(p[0]);
        close(p[1]);
    }
    close(s);
    close(m);
}

static int got_fd, got_ctrl, got_n;
static void adopt(int fd, int ctrl, pid_t pid, const char *key) {
    (void)pid; (void)key;
    got_n++;
    got_fd = fd;
    got_ctrl = ctrl;
}

static void test_holder(void) {
    char tmp[] = "/tmp/lord-hold-test-XXXXXX";
    if (!mkdtemp(tmp)) { printf("skipped holders: no temporary folder\n"); return; }
    setenv("XDG_RUNTIME_DIR", tmp, 1);
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    char name[128];
    if (m < 0 || grantpt(m) || unlockpt(m) || ptsname_r(m, name, sizeof name)) { printf("skipped holders: no pty\n"); return; }
    int slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);      /* the shell's end, open as a shell keeps it */
    char key[48];
    int ctrl = hold_spawn(m, getpid(), key, sizeof key);
    check(ctrl >= 0, "a holder starts");
    if (ctrl < 0) return;
    char sock[400];
    snprintf(sock, sizeof sock, "%s/lordos-term/%s.sock", tmp, key);
    for (int i = 0; i < 200 && access(sock, F_OK); i++) usleep(10000);
    check(!access(sock, F_OK), "its socket is in $XDG_RUNTIME_DIR/lordos-term");
    usleep(50000);      /* the holder in its poll */

    /* the first lord-server (this process, by the socket pair) has it: a second one gets nothing */
    got_n = 0;
    hold_restore(adopt);
    check(got_n == 0, "while a lord-server has the session, a second one doesn't get it");
    check(!access(sock, F_OK), "and the holder stays");

    /* the first one goes away: the next one gets it */
    close(ctrl);
    usleep(50000);
    hold_restore(adopt);
    check(got_n == 1 && got_fd >= 0, "once it is gone, the next lord-server gets the session");
    int first = got_ctrl;
    if (got_n == 1) {
        /* the same master: what one end writes the other reads */
        int ok = write(got_fd, "", 0) == 0;
        check(ok, "the master works");
        got_n = 0;
        hold_restore(adopt);
        check(got_n == 0, "and while that one has it, nobody else");
        /* the server closes the session: 'q' */
        if (write(first, "q", 1) != 1) check(0, "q");
        for (int i = 0; i < 200 && !access(sock, F_OK); i++) usleep(10000);
        check(access(sock, F_OK) != 0, "'q' ends the holder");
        close(first);
        close(got_fd);
    }
    close(m);
    close(slave);
    char st[400];
    snprintf(st, sizeof st, "%s/lordos-term", tmp);
    rmdir(st);
    rmdir(tmp);
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--term-holder")) return hold_main(argc, argv);
    setlocale(LC_CTYPE, "C.UTF-8");
    signal(SIGPIPE, SIG_IGN);
    memset(&prof, 0, sizeof prof);
    test_split();
    test_string_esc();
    test_grid();
    test_pty_size();
    test_holder();
    printf("term-resize-test: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
