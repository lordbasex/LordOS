/* saytext.c — say's text: cleaning what a terminal or a file gives it, and splitting it into sentences, so the
 * first one plays while the next is synthesized (saytext.h). */
#include "saytext.h"

#include <stdint.h>
#include <string.h>

/* length of a valid UTF-8 sequence at p (up to n bytes), or 0 */
static size_t utf8_len(const unsigned char *p, size_t n) {
    unsigned c = p[0];
    size_t k = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (!k || k > n || (k == 2 && c < 0xC2) || c > 0xF4) return 0;
    for (size_t i = 1; i < k; i++) if ((p[i] & 0xC0) != 0x80) return 0;
    if (k == 3 && ((c == 0xE0 && p[1] < 0xA0) || (c == 0xED && p[1] >= 0xA0))) return 0;     /* overlong, surrogates */
    if (k == 4 && ((c == 0xF0 && p[1] < 0x90) || (c == 0xF4 && p[1] >= 0x90))) return 0;
    return k;
}

size_t say_clean(char *s, size_t len) {
    unsigned char *p = (unsigned char *)s;
    size_t o = 0, nl = 0;
    int space = 0;
    for (size_t i = 0; i < len;) {
        unsigned c = p[i];
        if (c == 0x1b) {                                  /* ESC: CSI "[...final", OSC "]...BEL|ST", else 2 bytes */
            if (i + 1 < len && p[i + 1] == '[') {
                i += 2;
                while (i < len && !(p[i] >= 0x40 && p[i] <= 0x7e)) i++;
                i++;
            } else if (i + 1 < len && p[i + 1] == ']') {
                i += 2;
                while (i < len && p[i] != 7 && !(p[i] == 0x1b && i + 1 < len && p[i + 1] == '\\')) i++;
                i += i < len && p[i] == 0x1b ? 2 : 1;
            } else i += 2;
            continue;
        }
        if (c == '\n') { nl++; space = 0; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { space = 1; i++; continue; }
        if (c < 0x20 || c == 0x7f) { i++; continue; }
        size_t k = utf8_len(p + i, len - i);
        if (!k) { i++; continue; }
        if (k == 2 && c == 0xC2 && p[i + 1] < 0xA0) { i += 2; continue; }   /* C1 controls */
        if (o) {
            if (nl) { size_t t = nl > 2 ? 2 : nl; while (t--) p[o++] = '\n'; }
            else if (space) p[o++] = ' ';
        }
        nl = 0;
        space = 0;
        memmove(p + o, p + i, k);
        o += k;
        i += k;
    }
    if (o < len) p[o] = 0;
    return o;
}

/* the sentence ends after s[i] (the last byte of . ! ? or …) when a space or a line break follows */
static int ends_sentence(const unsigned char *p, size_t i, size_t len) {
    int mark = p[i] == '.' || p[i] == '!' || p[i] == '?' ||
               (i >= 2 && p[i - 2] == 0xE2 && p[i - 1] == 0x80 && p[i] == 0xA6);              /* … */
    return mark && (i + 1 == len || p[i + 1] == ' ' || p[i + 1] == '\n');
}

size_t say_next(const char *s, size_t len, size_t max, size_t *skip) {
    const unsigned char *p = (const unsigned char *)s;
    size_t a = 0;
    while (a < len && (p[a] == ' ' || p[a] == '\n')) a++;
    *skip = a;
    if (a == len) return 0;
    if (max < 8) max = 8;
    size_t comma = 0, space = 0;
    for (size_t i = a; i < len; i++) {
        size_t n = i - a + 1;
        if (n > max) break;
        if (ends_sentence(p, i, len)) return n;
        if (p[i] == '\n' && i + 1 < len && p[i + 1] == '\n') return i - a;       /* a blank line */
        if (p[i] == '\n' && i > a && (p[i - 1] == ':' || p[i - 1] == ';')) return i - a;
        if ((p[i] == ',' || p[i] == ';' || p[i] == ':') && i + 1 < len && (p[i + 1] == ' ' || p[i + 1] == '\n')) comma = n;
        if (p[i] == ' ' || p[i] == '\n') space = i - a;
    }
    if (len - a <= max) return len - a;
    if (comma > max / 3) return comma;
    if (space > 0) return space;
    /* one long word: cut before max on a character boundary */
    size_t n = max;
    while (n > 1 && (p[a + n] & 0xC0) == 0x80) n--;
    return n;
}

/* ---- which language ----
 * The most common short words of each language LordOS has voices for, and the letters only one of them uses. */
static const char *const WORDS_ES[] = { "el", "la", "los", "las", "de", "del", "que", "y", "en", "un", "una", "es", "por", "con",
    "para", "no", "se", "al", "lo", "como", "más", "pero", "su", "sus", "le", "ya", "muy", "también", "fue", "ha", "este",
    "esta", "hay", "está", "son", "yo", "me", "mi", "tu", "qué", "cuando", "donde", "porque", "sin", "sobre", "entre",
    "hasta", "desde", "todo", "todos", NULL };
static const char *const WORDS_EN[] = { "the", "and", "of", "to", "a", "in", "is", "it", "that", "for", "on", "with", "as",
    "was", "be", "by", "this", "are", "or", "not", "you", "at", "from", "have", "an", "they", "which", "but", "has", "had",
    "were", "their", "will", "can", "all", "what", "there", "if", "would", "been", "more", "when", "your", "we", "our",
    "its", NULL };
static const char *const WORDS_PT[] = { "o", "a", "os", "as", "de", "do", "da", "dos", "das", "que", "e", "em", "um", "uma",
    "é", "por", "com", "para", "não", "se", "ao", "como", "mais", "mas", "seu", "sua", "você", "ele", "ela", "isso",
    "está", "são", "eu", "meu", "minha", "quando", "onde", "porque", "sem", "sobre", "entre", "até", "desde", "também",
    "foi", "tem", "há", "muito", "nos", NULL };

static int in_list(const char *const *l, const char *w, size_t n) {
    for (; *l; l++) if (strlen(*l) == n && !memcmp(*l, w, n)) return 1;
    return 0;
}

static int has_at(const unsigned char *p, size_t i, size_t len, const char *u) {
    size_t n = strlen(u);
    return i + n <= len && !memcmp(p + i, u, n);
}

const char *say_language(const char *s, size_t len, const char *fallback) {
    static const char *const langs[3] = { "es", "en", "pt" };
    int score[3] = { 0, 0, 0 }, fb = 1;
    for (int k = 0; k < 3; k++) if (fallback && !strcmp(fallback, langs[k])) fb = k;
    const unsigned char *p = (const unsigned char *)s;
    if (len > 4000) len = 4000;
    char w[24];
    size_t wn = 0;
    for (size_t i = 0; i <= len; i++) {
        unsigned char c = i < len ? p[i] : ' ';
        /* the letters of a word: ASCII ones lowercased, and any UTF-8 sequence (accents) as it is */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80) {
            if (wn < sizeof w) w[wn] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            wn++;
            if (has_at(p, i, len, "\xc3\xb1") || has_at(p, i, len, "\xc2\xbf") || has_at(p, i, len, "\xc2\xa1")) score[0] += 2;   /* ñ ¿ ¡ */
            if (has_at(p, i, len, "\xc3\xa3") || has_at(p, i, len, "\xc3\xb5") || has_at(p, i, len, "\xc3\xa7")) score[2] += 2;   /* ã õ ç */
            continue;
        }
        if (wn && wn <= sizeof w) {
            score[0] += in_list(WORDS_ES, w, wn);
            score[1] += in_list(WORDS_EN, w, wn);
            score[2] += in_list(WORDS_PT, w, wn);
        }
        wn = 0;
    }
    int best = fb;
    for (int k = 0; k < 3; k++) if (score[k] > score[best]) best = k;
    return score[best] >= 2 && score[best] > score[fb] ? langs[best] : langs[fb];
}
