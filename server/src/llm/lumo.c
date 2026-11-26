/* lumo.c — the `lumo` command's pure parts (lumo.h) */
#include "lumo.h"
#include "writing.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

/* ASCII lower case, Spanish accented vowels and ñ folded to their plain letter; 0 when it doesn't fit */
static int fold(const char *in, char *out, size_t cap) {
    static const char *const ACC[][2] = {
        { "á", "a" }, { "é", "e" }, { "í", "i" }, { "ó", "o" }, { "ú", "u" }, { "ü", "u" }, { "ñ", "n" },
        { "Á", "a" }, { "É", "e" }, { "Í", "i" }, { "Ó", "o" }, { "Ú", "u" }, { "Ü", "u" }, { "Ñ", "n" },
        { "ê", "e" }, { "ç", "c" }, { "ã", "a" }, { "õ", "o" }, { "â", "a" }, { "ô", "o" },
    };
    size_t o = 0;
    for (const char *p = in; *p;) {
        if (o + 1 >= cap) return 0;
        if ((unsigned char)*p < 0x80) { out[o++] = (char)tolower((unsigned char)*p++); continue; }
        size_t k = 0;
        for (; k < sizeof ACC / sizeof ACC[0]; k++) if (!strncmp(p, ACC[k][0], strlen(ACC[k][0]))) break;
        if (k == sizeof ACC / sizeof ACC[0]) return 0;
        out[o++] = ACC[k][1][0];
        p += strlen(ACC[k][0]);
    }
    out[o] = 0;
    return 1;
}

int lumo_verb(const char *word) {
    static const struct { const char *w; int v; } V[] = {
        { "correct", LUMO_FIX }, { "fix", LUMO_FIX }, { "corregir", LUMO_FIX }, { "corrige", LUMO_FIX },
        { "improve", LUMO_IMPROVE }, { "mejorar", LUMO_IMPROVE }, { "mejora", LUMO_IMPROVE },
        { "summarize", LUMO_SHORTER }, { "summarise", LUMO_SHORTER }, { "shorten", LUMO_SHORTER }, { "shorter", LUMO_SHORTER },
        { "resumir", LUMO_SHORTER }, { "resume", LUMO_SHORTER }, { "acortar", LUMO_SHORTER },
        { "translate", LUMO_TRANSLATE }, { "traducir", LUMO_TRANSLATE }, { "traduce", LUMO_TRANSLATE },
        { "status", LUMO_STATUS }, { "estado", LUMO_STATUS },
        { "help", LUMO_HELP }, { "ayuda", LUMO_HELP }, { "--help", LUMO_HELP }, { "-h", LUMO_HELP }, { "--ayuda", LUMO_HELP },
        { "--version", LUMO_VERSION },
    };
    char f[32];
    if (!word || !fold(word, f, sizeof f)) return LUMO_NONE;
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++) if (!strcmp(f, V[i].w)) return V[i].v;
    return LUMO_NONE;
}

const char *lumo_action(int verb) {
    return verb == LUMO_FIX ? "fix" : verb == LUMO_IMPROVE ? "improve" : verb == LUMO_SHORTER ? "shorter"
         : verb == LUMO_TRANSLATE ? "translate" : NULL;
}

int lumo_lang(const char *word, char code[16]) {
    /* names in English (writing.c's) and Spanish, folded */
    static const char *const NAMES[][2] = {
        { "arabic", "ar" }, { "arabe", "ar" }, { "azerbaijani", "az" }, { "azerbaiyano", "az" }, { "bulgarian", "bg" },
        { "bulgaro", "bg" }, { "bengali", "bn" }, { "catalan", "ca" }, { "german", "de" }, { "aleman", "de" },
        { "greek", "el" }, { "griego", "el" }, { "english", "en" }, { "ingles", "en" }, { "spanish", "es" },
        { "espanol", "es" }, { "castellano", "es" }, { "persian", "fa" }, { "persa", "fa" }, { "finnish", "fi" },
        { "fines", "fi" }, { "filipino", "fil" }, { "french", "fr" }, { "frances", "fr" }, { "hindi", "hi" },
        { "hungarian", "hu" }, { "hungaro", "hu" }, { "indonesian", "id" }, { "indonesio", "id" }, { "italian", "it" },
        { "italiano", "it" }, { "japanese", "ja" }, { "japones", "ja" }, { "korean", "ko" }, { "coreano", "ko" },
        { "lithuanian", "lt" }, { "lituano", "lt" }, { "malay", "ms" }, { "malayo", "ms" }, { "dutch", "nl" },
        { "neerlandes", "nl" }, { "holandes", "nl" }, { "polish", "pl" }, { "polaco", "pl" }, { "portuguese", "pt" },
        { "portugues", "pt" }, { "romanian", "ro" }, { "rumano", "ro" }, { "russian", "ru" }, { "ruso", "ru" },
        { "swedish", "sv" }, { "sueco", "sv" }, { "swahili", "sw" }, { "suajili", "sw" }, { "thai", "th" },
        { "tailandes", "th" }, { "turkish", "tr" }, { "turco", "tr" }, { "ukrainian", "uk" }, { "ucraniano", "uk" },
        { "urdu", "ur" }, { "vietnamese", "vi" }, { "vietnamita", "vi" }, { "chinese", "zh-Hans" }, { "chino", "zh-Hans" },
    };
    char f[32];
    if (!word || !fold(word, f, sizeof f)) return -1;
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
        if (!strcmp(f, NAMES[i][0])) { strcpy(code, NAMES[i][1]); return 0; }
    if (!strcmp(f, "zh-hans") || !strcmp(f, "zh") || !strcmp(f, "zh_cn") || !strcmp(f, "zh-cn")) { strcpy(code, "zh-Hans"); return 0; }
    if (!strcmp(f, "zh-hant") || !strcmp(f, "zh_tw") || !strcmp(f, "zh-tw")) { strcpy(code, "zh-Hant"); return 0; }
    /* a code, with a region dropped: "pt_BR", "en-US" */
    char c[8];
    size_t k = strcspn(f, "_-.");
    if (k < 2 || k > 3) return -1;
    memcpy(c, f, k);
    c[k] = 0;
    if (!wr_language(c)) return -1;
    strcpy(code, c);
    return 0;
}

int lumo_text_ok(const char *s, size_t n) {
    if (!s || strlen(s) != n) return -3;
    size_t chars = 0;
    int blank = 1;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        int k = c < 0x80 ? 0 : c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xc2 && c <= 0xdf ? 1 : -1;
        if (k < 0 || i + (size_t)k >= n + (k ? 0 : 1)) return -3;
        for (int j = 1; j <= k; j++) if (((unsigned char)s[i + j] & 0xc0) != 0x80) return -3;
        if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r')) blank = 0;
        i += (size_t)k + 1;
        chars++;
    }
    if (blank) return -1;
    if (n > LUMO_TEXT_MAX || chars > LUMO_CHARS_MAX) return -2;
    return 0;
}

/* ---- the streamed text, safe for a terminal ---- */

void lumo_out_init(LumoOut *o) { memset(o, 0, sizeof *o); o->bol = 1; }

/* a whole line that is one of the request's markers: <<<TEXT x>>> or <<<END-TEXT x>>>, x 16 hex digits */
static int marker(const char *p, size_t n) {
    if (n && p[n - 1] == '\r') n--;
    size_t h = n >= 8 && !memcmp(p, "<<<TEXT ", 8) ? 8 : n >= 12 && !memcmp(p, "<<<END-TEXT ", 12) ? 12 : 0;
    if (!h || n != h + 16 + 3 || memcmp(p + n - 3, ">>>", 3)) return 0;
    for (size_t i = h; i < h + 16; i++) if (!strchr("0123456789abcdef", p[i])) return 0;
    return 1;
}

/* a code point to drop: C0 controls but \n and \t, DEL, C1, bidi controls */
static int dropped(unsigned cp) {
    return (cp < 0x20 && cp != '\n' && cp != '\t') || (cp >= 0x7f && cp <= 0x9f) || cp == 0x200e || cp == 0x200f ||
           (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069);
}

static size_t put(char *out, size_t cap, size_t w, const char *p, size_t n) {
    if (w + n + 1 > cap) return w;
    memcpy(out + w, p, n);
    return w + n;
}

size_t lumo_out_feed(LumoOut *o, const char *text, int final, char *out, size_t cap) {
    size_t n = text ? strlen(text) : 0, w = 0, i = o->seen;
    if (!cap) return 0;
    if (i > n) i = n;
    while (i < n) {
        const char *p = text + i;
        if (o->think) {
            const char *e = strstr(p, "</think>");
            if (e) { i = (size_t)(e - text) + 8; o->think = 0; continue; }
            if (final) i = n;
            break;
        }
        if (!o->started) {
            if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { i++; o->bol = *p == '\n' || o->bol; continue; }
            if (!strncmp(p, "<think>", 7)) { o->think = 1; i += 7; continue; }
            if (!final && n - i < 7 && !strncmp("<think>", p, n - i)) break;
        }
        if (o->bol) {
            size_t e = i;
            while (e < n && text[e] != '\n') e++;
            if (e == n && !final && *p == '<' && n - i < 48) break;       /* may still become a marker */
            if ((e < n || final) && marker(p, e - i)) { i = e < n ? e + 1 : e; continue; }
            o->bol = 0;
        }
        unsigned char c = (unsigned char)*p;
        int k = c < 0x80 ? 0 : c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xc2 && c <= 0xdf ? 1 : -1;
        if (k < 0) { i++; continue; }                                        /* not UTF-8: dropped */
        if (i + (size_t)k >= n) { if (final) i = n; break; }                 /* the rest of it comes later */
        unsigned cp = k ? c & (0x3f >> k) : c;
        int bad = 0;
        for (int j = 1; j <= k; j++) { unsigned char d = (unsigned char)p[j]; if ((d & 0xc0) != 0x80) bad = 1; cp = cp << 6 | (d & 0x3f); }
        if (bad) { i++; continue; }
        i += (size_t)k + 1;
        if (c == '\n') { o->blanks++; o->bol = 1; continue; }
        if (dropped(cp)) continue;
        for (; o->blanks; o->blanks--) w = put(out, cap, w, "\n", 1);
        w = put(out, cap, w, p, (size_t)k + 1);
        o->started = 1;
    }
    o->seen = i;
    out[w] = 0;
    return w;
}
