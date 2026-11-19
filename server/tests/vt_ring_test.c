/* vt_ring_test.c — (0.8.9.1) the terminal's history ring grows as it fills (vt.c buf_grow, Buffer.max): every line
 * stays in order across growth, a cleared history (ESC [ 3 J) and a reflow, and the ring never passes its limit. */
#include "term/term.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
void term_set_clipboard(const char *s, size_t n) { (void)s; (void)n; }
static int check(Term *t, long long abs, int want) {
    Line *l = vt_abs_line(t, abs); char b[16] = {0};
    if (!l) return printf("missing %lld\n", abs), 1;
    for (int i = 0; i < l->len && i < 15; i++) b[i] = (char)l->c[i].cp;
    if (atoi(b) != want) return printf("abs %lld: '%s' want %d\n", abs, b, want), 1;
    return 0;
}
int main(void) {
    static Term t; static TermProfile p; int bad = 0;
    if (vt_init(&t, 40, 10, 5000, &p)) return 1;
    char s[32];
    for (int i = 0; i < 3000; i++) { int n = snprintf(s, sizeof s, "%d\r\n", i); vt_write(&t, (const uint8_t *)s, (size_t)n); }
    printf("after 3000: cap %d count %d max %d\n", t.main.cap, t.main.count, t.main.max);
    bad += check(&t, 0, 0) + check(&t, 1500, 1500) + check(&t, 2999, 2999);
    vt_write(&t, (const uint8_t *)"\033[3J", 4);
    long long base = vt_first_abs(&t);
    for (int i = 3000; i < 7000; i++) { int n = snprintf(s, sizeof s, "%d\r\n", i); vt_write(&t, (const uint8_t *)s, (size_t)n); }
    printf("after clear+4000: cap %d count %d dropped %lld\n", t.main.cap, t.main.count, t.main.dropped);
    long long last = vt_total_lines(&t) + vt_first_abs(&t) - 2;  /* last written line (cursor on an empty one) */
    bad += check(&t, last, 6999) + check(&t, last - 3000, 3999);
    vt_resize(&t, 20, 8);
    printf("after resize: cap %d count %d max %d\n", t.main.cap, t.main.count, t.main.max);
    long long last2 = vt_first_abs(&t) + vt_total_lines(&t) - 2;
    bad += check(&t, last2, 6999);
    for (int i = 7000; i < 20000; i++) { int n = snprintf(s, sizeof s, "%d\r\n", i); vt_write(&t, (const uint8_t *)s, (size_t)n); }
    printf("after 20000: cap %d count %d max %d\n", t.main.cap, t.main.count, t.main.max);
    long long l3 = vt_first_abs(&t) + vt_total_lines(&t) - 2;
    bad += check(&t, l3, 19999) + check(&t, vt_first_abs(&t), 19999 - (int)(l3 - vt_first_abs(&t)));
    (void)base;
    bad += t.main.cap > t.main.max;
    vt_free(&t);
    printf("vt-ring-test: %s\n", bad ? "FAIL" : "ok"); return bad;
}
