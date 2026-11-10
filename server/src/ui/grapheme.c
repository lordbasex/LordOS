/* grapheme.c — extended grapheme cluster boundaries (UAX #29, rules GB3–GB13) over the
 * Grapheme_Cluster_Break, Extended_Pictographic and Indic_Conjunct_Break tables of unidata.h.
 * Checked against the Unicode conformance file (tests/bidi_test.c, GraphemeBreakTest.txt). */
#include "grapheme.h"

#include <stdlib.h>

#include "unicode.h"

static int is_ctl(int g) { return g == GCB_Control || g == GCB_CR || g == GCB_LF; }

int grapheme_next(const uint32_t *cp, int n, int i) {
    if (i >= n) return n;
    int prev = uc_gcb(cp[i]);
    /* what the cluster so far ends with: regional indicators in a row; an emoji followed by
     * Extend* (1) and a ZWJ (2); an Indic consonant followed by Extend/Linker* (1) with a Linker (2) */
    int ri = prev == GCB_RI;
    int ep = uc_extpict(cp[i]) ? 1 : 0;
    int ic = uc_incb(cp[i]) == INCB_Consonant ? 1 : 0;
    int j = i + 1;
    for (; j < n; j++) {
        uint32_t c = cp[j];
        int cur = uc_gcb(c), incb = uc_incb(c), join;
        if (prev == GCB_CR && cur == GCB_LF) join = 1;                                  /* GB3 */
        else if (is_ctl(prev) || is_ctl(cur)) join = 0;                                /* GB4, GB5 */
        else if (prev == GCB_L && (cur == GCB_L || cur == GCB_V || cur == GCB_LV || cur == GCB_LVT)) join = 1;  /* GB6 */
        else if ((prev == GCB_LV || prev == GCB_V) && (cur == GCB_V || cur == GCB_T)) join = 1;              /* GB7 */
        else if ((prev == GCB_LVT || prev == GCB_T) && cur == GCB_T) join = 1;                               /* GB8 */
        else if (cur == GCB_Extend || cur == GCB_ZWJ || cur == GCB_SpacingMark) join = 1;                    /* GB9, GB9a */
        else if (prev == GCB_Prepend) join = 1;                                         /* GB9b */
        else if (incb == INCB_Consonant && ic == 2) join = 1;                           /* GB9c */
        else if (prev == GCB_ZWJ && ep == 2 && uc_extpict(c)) join = 1;                 /* GB11 */
        else if (prev == GCB_RI && cur == GCB_RI) join = ri & 1;                        /* GB12, GB13 */
        else join = 0;                                                                  /* GB999 */
        if (!join) break;
        ri = cur == GCB_RI ? ri + 1 : 0;
        if (uc_extpict(c)) ep = 1;
        else if (cur == GCB_Extend) ep = ep == 1;
        else if (cur == GCB_ZWJ) ep = ep == 1 ? 2 : 0;
        else ep = 0;
        if (incb == INCB_Consonant) ic = 1;
        else if (incb == INCB_Linker) ic = ic ? 2 : 0;
        else if (incb != INCB_Extend) ic = 0;
        prev = cur;
    }
    return j;
}

int grapheme_breaks(const uint32_t *cp, int n, uint8_t *brk) {
    for (int k = 0; k <= n; k++) brk[k] = 0;
    int count = 0;
    for (int i = 0; i < n; i = grapheme_next(cp, n, i)) { brk[i] = 1; count++; }
    brk[n] = 1;
    return count;
}

/* One code point (a bad byte counts as one, as U+FFFD). */
static uint32_t utf8_cp(const unsigned char **p) {
    const unsigned char *s = *p;
    uint32_t c = s[0];
    int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
    if (k < 0) { *p += 1; return 0xfffd; }
    c = k == 0 ? c : k == 1 ? c & 0x1f : k == 2 ? c & 0x0f : c & 0x07;
    for (int i = 1; i <= k; i++) {
        if ((s[i] & 0xc0) != 0x80) { *p += i; return 0xfffd; }
        c = c << 6 | (s[i] & 0x3f);
    }
    *p += k + 1;
    return c;
}

int *grapheme_stops_utf8(const char *s, int max, int *count) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n <= max; n++) utf8_cp(&p);
    if (n > max) return NULL;
    uint32_t *cp = malloc(sizeof *cp * (size_t)(n + 1));
    uint8_t *brk = malloc((size_t)n + 1);
    int *stops = malloc(sizeof *stops * (size_t)(n + 1));
    if (!cp || !brk || !stops) { free(cp); free(brk); free(stops); return NULL; }
    n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) cp[n++] = utf8_cp(&p);
    grapheme_breaks(cp, n, brk);
    int k = 0;
    for (int i = 0; i <= n; i++) if (brk[i]) stops[k++] = i;
    free(cp); free(brk);
    *count = k;
    return stops;
}
