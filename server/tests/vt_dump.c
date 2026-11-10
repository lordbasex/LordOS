/* vt_dump.c — headless test of the emulator: reads bytes from stdin, feeds them through vt.c and
 * prints the grid as text. Usage:
 *   printf 'hola\033[31m rojo' | build/vt-dump 20 5 [cols2 rows2]
 * With cols2/rows2 it resizes at the end (tests reflow). A fifth argument is searched for, or "--lines" prints
 * vt_screen_lines (each screen line after the absolute row it starts on). */
#include "term/term.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* term.c dependencies that vt.c uses. */
static char clip[4096];
void term_set_clipboard(const char *s, size_t n) { snprintf(clip, sizeof clip, "%.*s", (int)n, s); }

static void respond(Term *t, const char *s, size_t n) {
    (void)t;
    printf("<reply:");
    for (size_t i = 0; i < n; i++) printf(s[i] == 033 ? "\\e" : "%c", s[i]);
    printf(">\n");
}

static void dump(Term *t) {
    printf("--- %dx%d cursor %d,%d history %d alt %d\n", t->cols, t->rows, t->cx, t->cy,
           t->buf->count - t->rows, t->buf == &t->alt);
    for (int i = 0; i < t->buf->count; i++) {
        int row = i - (t->buf->count - t->rows);
        Line *l = &t->buf->ring[(t->buf->head + i) % t->buf->cap];
        printf("%s%c|", row < 0 ? "h " : "  ", l->flags & LINE_WRAPPED ? '+' : ' ');
        for (int x = 0; x < l->len; x++) {
            if (l->c[x].attr & A_WIDE_TAIL) continue;
            uint32_t seq[CLUSTER_LEN];
            int n = vt_cluster_cps(l->c[x].cp ? l->c[x].cp : '.', seq, CLUSTER_LEN);
            if (n > 1) putchar('[');
            for (int k = 0; k < n; k++) {
                uint32_t cp = seq[k];
                char b[5] = {0};
                if (cp < 0x80) b[0] = (char)cp;
                else if (cp < 0x800) { b[0] = (char)(0xc0 | cp >> 6); b[1] = (char)(0x80 | (cp & 0x3f)); }
                else if (cp < 0x10000) { b[0] = (char)(0xe0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 0x3f)); b[2] = (char)(0x80 | (cp & 0x3f)); }
                else { b[0] = (char)(0xf0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 0x3f)); b[2] = (char)(0x80 | (cp >> 6 & 0x3f)); b[3] = (char)(0x80 | (cp & 0x3f)); }
                fputs(b, stdout);
            }
            if (n > 1) putchar(']');
        }
        printf("|%s%s\n", l->flags & LINE_PROMPT ? " [prompt]" : "", l->flags & LINE_FAIL ? " [failed]" : l->flags & LINE_OK ? " [ok]" : "");
    }
    if (t->title[0]) printf("title: %s\n", t->title);
    if (t->cwd[0]) printf("cwd: %s\n", t->cwd);
    if (clip[0]) printf("clipboard: %s\n", clip);
}

int main(int argc, char **argv) {
    int cols = argc > 1 ? atoi(argv[1]) : 20, rows = argc > 2 ? atoi(argv[2]) : 5;
    setlocale(LC_CTYPE, "C.UTF-8");
    TermProfile p;
    memset(&p, 0, sizeof p);
    snprintf(p.word_chars, sizeof p.word_chars, "/-+\\~_.");
    Term t;
    if (vt_init(&t, cols, rows, 100, &p)) return 1;
    t.respond = respond;
    static uint8_t buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) vt_write(&t, buf, n);
    dump(&t);
    if (argc > 4) { vt_resize(&t, atoi(argv[3]), atoi(argv[4])); dump(&t); }
    if (argc > 5 && !strcmp(argv[5], "--lines")) {
        char *l = vt_screen_lines(&t);
        printf("lines:\n%s", l ? l : "");
        free(l);
    } else if (argc > 5) { vt_search(&t, argv[5], 0); printf("search '%s': %d matches\n", argv[5], t.nmatches); }
    vt_free(&t);
    return 0;
}
