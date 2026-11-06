/* match.c — how a search text matches a package (see match.h). */
#include "match.h"

#include <string.h>

/* U+00C0–U+00FF: the base letter (× and ÷ become a space) */
static const char LATIN1[64 + 1] = "aaaaaaaceeeeiiiidnooooo ouuuuyts" "aaaaaaaceeeeiiiidnooooo ouuuuyty";
/* U+0100–U+017F (Latin Extended-A) */
static const char LATIN_A[128 + 1] =
    "aaaaaa" "cccccccc" "dddd" "eeeeeeeeee" "gggggggg" "hhhh" "iiiiiiiiii" "ii" "jj" "kkk" "llllllllll"
    "nnnnnnnnn" "oooooo" "oo" "rrrrrr" "ssssssss" "tttttt" "uuuuuuuuuuuu" "ww" "yyy" "zzzzzz" "s";
_Static_assert(sizeof LATIN1 == 65 && sizeof LATIN_A == 129, "fold tables");

static int is_alnum(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'); }

size_t pkg_fold(const char *in, char *out, size_t cap) {
    size_t o = 0;
    if (!cap) return 0;
    const unsigned char *p = (const unsigned char *)(in ? in : "");
    while (*p && o + 1 < cap) {
        unsigned c = p[0];
        if (c < 0x80) { out[o++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); p++; continue; }
        if (c >= 0xC2 && c <= 0xDF && (p[1] & 0xC0) == 0x80) {
            unsigned cp = (c & 0x1F) << 6 | (p[1] & 0x3F);
            if (cp >= 0xC0 && cp <= 0xFF) { out[o++] = LATIN1[cp - 0xC0]; p += 2; continue; }
            if (cp >= 0x100 && cp <= 0x17F) { out[o++] = LATIN_A[cp - 0x100]; p += 2; continue; }
            if (cp >= 0x300 && cp <= 0x36F) { p += 2; continue; }            /* a combining mark */
            if (o + 2 >= cap) break;
            out[o++] = (char)p[0]; out[o++] = (char)p[1]; p += 2;
            continue;
        }
        out[o++] = (char)c; p++;            /* any other byte (longer sequences, invalid UTF-8) as it is */
    }
    out[o] = 0;
    return o;
}

size_t pkg_compact(const char *in, char *out, size_t cap) {
    size_t n = pkg_fold(in, out, cap), o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)out[i];
        if (is_alnum(c) || c >= 0x80) out[o++] = (char)c;
    }
    out[o] = 0;
    return o;
}

void pkg_query_parse(PkgQuery *q, const char *text) {
    char f[512];
    memset(q, 0, sizeof *q);
    pkg_fold(text, f, sizeof f);
    for (char *p = f; *p && q->n < PKG_QUERY_WORDS;) {
        while (*p && !is_alnum((unsigned char)*p) && (unsigned char)*p < 0x80) p++;
        size_t k = 0;
        while (p[k] && (is_alnum((unsigned char)p[k]) || (unsigned char)p[k] >= 0x80)) k++;
        if (!k) break;
        size_t n = k < PKG_QUERY_WORD - 1 ? k : PKG_QUERY_WORD - 1;
        memcpy(q->w[q->n], p, n);
        q->w[q->n][n] = 0;
        q->len[q->n] = n;
        if (q->jlen + n < sizeof q->joined) { memcpy(q->joined + q->jlen, p, n); q->jlen += n; q->joined[q->jlen] = 0; }
        q->n++;
        p += k;
    }
}

int pkg_match_level(const char *folded, const char *compact, const char *w, size_t wl) {
    if (!folded || !wl) return 0;
    if (!strcmp(folded, w)) return 4;
    int best = 0;
    for (const char *m = strstr(folded, w); m; m = strstr(m + 1, w)) {
        if (m == folded) return 3;
        if (!is_alnum((unsigned char)m[-1]) && (unsigned char)m[-1] < 0x80) best = 2;
        else if (!best && wl >= 3) best = 1;
    }
    if (!best && wl >= 3 && compact && strstr(compact, w)) best = 1;
    return best;
}

static const int NAME_PTS[5] = { 0, 120, 300, 500, 1000 }, APP_PTS[5] = { 0, 110, 280, 450, 900 };

int pkg_match_score(const PkgQuery *q, const char *name, const char *app_name, const char *app_summary,
                    const char *summary, int *full) {
    *full = 0;
    if (!q->n) return 0;
    char fn[160], cn[160], fa[320], ca[320], fs1[600], fs2[600];
    pkg_fold(name, fn, sizeof fn);
    pkg_compact(name, cn, sizeof cn);
    int has_app = app_name && app_name[0];
    if (has_app) { pkg_fold(app_name, fa, sizeof fa); pkg_compact(app_name, ca, sizeof ca); }
    int summaries = 0, matched = 0, starts = 0, score = 0;
    for (int i = 0; i < q->n; i++) {
        const char *w = q->w[i];
        size_t wl = q->len[i];
        int ln = pkg_match_level(fn, cn, w, wl), la = has_app ? pkg_match_level(fa, ca, w, wl) : 0;
        int s = NAME_PTS[ln] > APP_PTS[la] ? NAME_PTS[ln] : APP_PTS[la];
        if ((ln >= 2 || la >= 2) && wl >= 2) starts++;
        if (!s) {
            if (!summaries) {
                pkg_fold(app_summary, fs1, sizeof fs1);
                pkg_fold(summary, fs2, sizeof fs2);
                summaries = 1;
            }
            int l1 = pkg_match_level(fs1, NULL, w, wl), l2 = pkg_match_level(fs2, NULL, w, wl);
            s = l1 >= 2 ? 80 : l2 >= 2 ? 50 : l1 ? 30 : l2 ? 20 : 0;
        }
        if (s) matched++;
        score += s;
    }
    /* the words written together or apart: "vs code" is "vscode", "visual studio code" is the app */
    if (q->n >= 2) {
        if (!strcmp(cn, q->joined) || (has_app && !strcmp(ca, q->joined))) { score += 1400; matched = q->n; }
        else if (strstr(cn, q->joined) || (has_app && strstr(ca, q->joined))) { score += 600; matched = q->n; }
        /* else the words together, in that order, in a summary ("based on VS Code") */
        else if (matched == q->n && summaries) {
            char ph[PKG_QUERY_WORDS * PKG_QUERY_WORD + PKG_QUERY_WORDS];
            size_t o = 0;
            for (int i = 0; i < q->n; i++) { memcpy(ph + o, q->w[i], q->len[i]); o += q->len[i]; ph[o++] = ' '; }
            ph[o - 1] = 0;
            if (strstr(fs1, ph) || strstr(fs2, ph)) score += 400;
        }
    }
    if (matched == q->n) { *full = 1; return score + 10000; }
    /* some words: only when one of them starts a word of the name, and always below the full matches */
    if (q->n < 2 || !starts) return 0;
    score /= 4;
    return score > 0 ? (score < 9000 ? score : 9000) : 0;
}
