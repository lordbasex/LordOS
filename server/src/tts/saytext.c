/* saytext.c — say's text: cleaning what a terminal or a file gives it, and splitting it into sentences, so the
 * first one plays while the next is synthesized (saytext.h). */
#include "saytext.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ---- text normalization (Spanish and Portuguese) ----
 * Pocket TTS's Spanish and Portuguese models never learned digits or symbols (measured: "Tengo 88 años" came out as
 * "Tengo bronce proteaños"), while they read the same text written as words perfectly. audio.cpp's text preparation
 * does no number normalization for any language: the English model reads digits because it learned them. So for es
 * and pt, say writes numbers, times, dates, money, ordinals and symbols out as words before it splits the text
 * (docs/VOICE.md, "Text normalization"). Portuguese is Brazilian Portuguese where the two differ (dezesseis,
 * dezenove, bilhão). The rules follow the usual ones (num2words, NeMo's text normalization) with tables of our own. */

/* the text being written; peso: "$" is the peso of the user's region, not the dollar */
typedef struct { char *p; size_t n, cap; int oom, peso; } Out;

static void put(Out *o, const char *s, size_t n) {
    if (o->oom || !n) return;
    if (o->n + n + 1 > o->cap) {
        size_t c = o->cap ? o->cap : 256;
        while (o->n + n + 1 > c) c *= 2;
        char *q = realloc(o->p, c);
        if (!q) { o->oom = 1; return; }
        o->p = q;
        o->cap = c;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
}
static void puts_(Out *o, const char *s) { put(o, s, strlen(s)); }
/* a word with a space on each side; tidy() leaves one space where the text needs it */
static void word(Out *o, const char *w) { put(o, " ", 1); puts_(o, w); put(o, " ", 1); }

enum { G_FULL, G_APOC, G_FEM };   /* es uno / un / una; pt um / um / uma */

typedef struct {
    const char *digit[10], *comma, *point, *and_, *minus, *plus, *equals, *percent, *number, *at, *slash, *dot, *dash,
        *underscore, *colon, *hour, *hours, *minute, *minutes, *second, *seconds, *degree, *degrees, *cents, *of, *to,
        *first_day, *month[12];
} Lex;

static const Lex LEX[2] = {
    { { "cero", "uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve" }, "coma", "punto", "y",
      "menos", "más", "igual a", "por ciento", "número", "arroba", "barra", "punto", "guion", "guion bajo", "dos puntos",
      "hora", "horas", "minuto", "minutos", "segundo", "segundos", "grado", "grados", "con", "de", "a", "uno",
      { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre",
        "diciembre" } },
    { { "zero", "um", "dois", "três", "quatro", "cinco", "seis", "sete", "oito", "nove" }, "vírgula", "ponto", "e",
      "menos", "mais", "igual a", "por cento", "número", "arroba", "barra", "ponto", "hífen", "sublinhado", "dois pontos",
      "hora", "horas", "minuto", "minutos", "segundo", "segundos", "grau", "graus", "e", "de", "a", "primeiro",
      { "janeiro", "fevereiro", "março", "abril", "maio", "junho", "julho", "agosto", "setembro", "outubro", "novembro",
        "dezembro" } },
};

/* money: the symbol before or after the amount, its name in each language, and whether that name is feminine */
typedef struct { const char *sym, *es1, *es, *pt1, *pt; int fem; } Cur;
static const Cur CUR[] = {
    { "US$", "dólar", "dólares", "dólar", "dólares", 0 },
    { "U$S", "dólar", "dólares", "dólar", "dólares", 0 },
    { "R$", "real", "reales", "real", "reais", 0 },
    { "$", "dólar", "dólares", "dólar", "dólares", 0 },
    { "\xe2\x82\xac", "euro", "euros", "euro", "euros", 0 },             /* € */
    { "\xc2\xa3", "libra", "libras", "libra", "libras", 1 },             /* £ */
    { "USD", "dólar", "dólares", "dólar", "dólares", 0 },
};
#define CUR_DOLLAR 3                    /* "$": pesos in a peso country (say_normalize's region) */
#define CUR_AFTER 3                     /* $ € £ USD also go after the amount */

/* the countries whose own currency is a peso written "$": Argentina, Mexico, Chile, Colombia, Uruguay, the Dominican
 * Republic, Cuba. Elsewhere (Spain, the United States, Ecuador, El Salvador, Puerto Rico, Brazil…) "$" is the dollar. */
static const char *const PESO_COUNTRIES[] = { "AR", "MX", "CL", "CO", "UY", "DO", "CU", NULL };

/* the country of a region: "es_AR", "es_AR.UTF-8", "es-AR" or "AR" → "AR"; 0 when none */
static int peso_region(const char *r) {
    if (!r) return 0;
    const char *c = r;
    size_t n = strcspn(r, "_-.@");
    if (r[n] == '_' || r[n] == '-') c = r + n + 1;
    else if (n != 2) return 0;
    if (!(c[0] >= 'A' && c[0] <= 'Z' && c[1] >= 'A' && c[1] <= 'Z') || (c[2] && !strchr(".@", c[2]))) return 0;
    for (int i = 0; PESO_COUNTRIES[i]; i++) if (c[0] == PESO_COUNTRIES[i][0] && c[1] == PESO_COUNTRIES[i][1]) return 1;
    return 0;
}

/* -- cardinals, 0 … 999 999 999 999 -- */
static const char *const ES_U[30] = { "cero", "uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve",
    "diez", "once", "doce", "trece", "catorce", "quince", "dieciséis", "diecisiete", "dieciocho", "diecinueve", "veinte",
    "veintiuno", "veintidós", "veintitrés", "veinticuatro", "veinticinco", "veintiséis", "veintisiete", "veintiocho",
    "veintinueve" };
static const char *const ES_T[10] = { "", "", "", "treinta", "cuarenta", "cincuenta", "sesenta", "setenta", "ochenta",
    "noventa" };
static const char *const ES_H[10] = { "", "ciento", "doscientos", "trescientos", "cuatrocientos", "quinientos",
    "seiscientos", "setecientos", "ochocientos", "novecientos" };
static const char *const PT_U[20] = { "zero", "um", "dois", "três", "quatro", "cinco", "seis", "sete", "oito", "nove",
    "dez", "onze", "doze", "treze", "catorze", "quinze", "dezesseis", "dezessete", "dezoito", "dezenove" };
static const char *const PT_T[10] = { "", "", "vinte", "trinta", "quarenta", "cinquenta", "sessenta", "setenta",
    "oitenta", "noventa" };
static const char *const PT_H[10] = { "", "cento", "duzentos", "trezentos", "quatrocentos", "quinhentos", "seiscentos",
    "setecentos", "oitocentos", "novecentos" };

/* a hundreds word, feminine when asked (doscientos → doscientas, duzentos → duzentas; ciento and cento stay) */
static void hundreds(Out *o, const char *h, int g) {
    if (g == G_FEM && strcmp(h, "ciento") && strcmp(h, "cento")) { put(o, h, strlen(h) - 2); puts_(o, "as"); }
    else puts_(o, h);
}

static void es_one(Out *o, int g) { puts_(o, g == G_APOC ? "un" : g == G_FEM ? "una" : "uno"); }

static void es_999(Out *o, unsigned n, int g) {
    if (n == 100) { puts_(o, "cien"); return; }
    unsigned h = n / 100, r = n % 100;
    if (h) { hundreds(o, ES_H[h], g); if (r) puts_(o, " "); }
    if (!r) return;
    if (r == 1) es_one(o, g);
    else if (r == 21) puts_(o, g == G_APOC ? "veintiún" : g == G_FEM ? "veintiuna" : "veintiuno");
    else if (r < 30) puts_(o, ES_U[r]);
    else {
        puts_(o, ES_T[r / 10]);
        if (r % 10 == 1) { puts_(o, " y "); es_one(o, g); }
        else if (r % 10) { puts_(o, " y "); puts_(o, ES_U[r % 10]); }
    }
}

/* 1 … 999 999: "mil" alone, "veintiún mil" (the multiplier is apocopated, or feminine with a feminine noun) */
static void es_999999(Out *o, unsigned n, int g) {
    unsigned th = n / 1000, u = n % 1000;
    if (th == 1) puts_(o, "mil");
    else if (th) { es_999(o, th, g == G_FEM ? G_FEM : G_APOC); puts_(o, " mil"); }
    if (th && u) puts_(o, " ");
    if (u) es_999(o, u, g);
}

/* Spanish counts in millions up to "novecientos noventa y nueve mil millones": 10⁹ is "mil millones" */
static void es_card(Out *o, unsigned long long n, int g) {
    if (!n) { puts_(o, "cero"); return; }
    unsigned m = (unsigned)(n / 1000000), r = (unsigned)(n % 1000000);
    if (m == 1) puts_(o, "un millón");
    else if (m) { es_999999(o, m, G_APOC); puts_(o, " millones"); }
    if (m && r) puts_(o, " ");
    if (r) es_999999(o, r, g);
}

static void pt_unit(Out *o, unsigned u, int g) {
    puts_(o, g == G_FEM && u == 1 ? "uma" : g == G_FEM && u == 2 ? "duas" : PT_U[u]);
}

static void pt_999(Out *o, unsigned n, int g) {
    if (n == 100) { puts_(o, "cem"); return; }
    unsigned h = n / 100, r = n % 100;
    if (h) { hundreds(o, PT_H[h], g); if (r) puts_(o, " e "); }
    if (!r) return;
    if (r < 20) pt_unit(o, r, g);
    else { puts_(o, PT_T[r / 10]); if (r % 10) { puts_(o, " e "); pt_unit(o, r % 10, g); } }
}

/* Brazilian short scale (bilhão). Groups go without "e" except before the last one when it is under 100 or a round
 * hundred: "mil novecentos e oitenta e sete", "dois mil e vinte e seis", "um milhão e duzentos mil". */
static void pt_card(Out *o, unsigned long long n, int g) {
    if (!n) { puts_(o, "zero"); return; }
    unsigned v[4] = { (unsigned)(n / 1000000000ull % 1000), (unsigned)(n / 1000000 % 1000), (unsigned)(n / 1000 % 1000),
                      (unsigned)(n % 1000) };
    int last = -1, first = 1;
    for (int k = 0; k < 4; k++) if (v[k]) last = k;
    for (int k = 0; k < 4; k++) {
        if (!v[k]) continue;
        if (!first) puts_(o, k == last && (v[k] < 100 || v[k] % 100 == 0) ? " e " : " ");
        first = 0;
        if (k == 0) { if (v[k] == 1) puts_(o, "um bilhão"); else { pt_999(o, v[k], G_FULL); puts_(o, " bilhões"); } }
        else if (k == 1) { if (v[k] == 1) puts_(o, "um milhão"); else { pt_999(o, v[k], G_FULL); puts_(o, " milhões"); } }
        else if (k == 2) { if (v[k] == 1) puts_(o, "mil"); else { pt_999(o, v[k], g); puts_(o, " mil"); } }
        else pt_999(o, v[k], g);
    }
}

static void card(Out *o, int L, unsigned long long n, int g) {
    put(o, " ", 1);
    if (L == 0) es_card(o, n, g); else pt_card(o, n, g);
    put(o, " ", 1);
}

/* -- ordinals, 1 … 100 -- */
static const char *const ES_O1[10] = { "", "primero", "segundo", "tercero", "cuarto", "quinto", "sexto", "séptimo",
    "octavo", "noveno" };
static const char *const ES_O1X[10] = { "décimo", "undécimo", "duodécimo", "decimotercero", "decimocuarto",
    "decimoquinto", "decimosexto", "decimoséptimo", "decimoctavo", "decimonoveno" };
static const char *const ES_O10[10] = { "", "décimo", "vigésimo", "trigésimo", "cuadragésimo", "quincuagésimo",
    "sexagésimo", "septuagésimo", "octogésimo", "nonagésimo" };
static const char *const PT_O1[10] = { "", "primeiro", "segundo", "terceiro", "quarto", "quinto", "sexto", "sétimo",
    "oitavo", "nono" };
static const char *const PT_O10[10] = { "", "décimo", "vigésimo", "trigésimo", "quadragésimo", "quinquagésimo",
    "sexagésimo", "septuagésimo", "octogésimo", "nonagésimo" };

/* fem: every word's final o made a (primera, vigésima primeira); apoc: es "1.er", "3.er" → primer, tercer */
static void ordinal(Out *o, int L, unsigned n, int fem, int apoc) {
    char b[80];
    if (n == 100) snprintf(b, sizeof b, "centésimo");
    else if (L == 0 && n >= 10 && n < 20) snprintf(b, sizeof b, "%s", ES_O1X[n - 10]);
    else {
        const char *const *u = L == 0 ? ES_O1 : PT_O1, *const *t = L == 0 ? ES_O10 : PT_O10;
        snprintf(b, sizeof b, "%s%s%s", t[n / 10], n >= 10 && n % 10 ? " " : "", u[n % 10]);
    }
    size_t k = strlen(b);
    if (apoc && k >= 7 && (!strcmp(b + k - 7, "primero") || !strcmp(b + k - 7, "tercero"))) b[--k] = 0;
    if (fem) for (size_t i = 0; i < k; i++) if (b[i] == 'o' && (i + 1 == k || b[i + 1] == ' ')) b[i] = 'a';
    word(o, b);
}

/* -- reading the text -- */
static int isd(const unsigned char *p, size_t i, size_t len) { return i < len && p[i] >= '0' && p[i] <= '9'; }
static size_t digit_run(const unsigned char *p, size_t i, size_t len) { size_t j = i; while (isd(p, j, len)) j++; return j - i; }
static int is_ascii_letter(unsigned c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/* the bytes of the letter at p[i] (ASCII, or a Latin letter with an accent: U+00C0 … U+07FF but × and ÷), else 0 */
static size_t letter_at(const unsigned char *p, size_t i, size_t len) {
    if (i >= len) return 0;
    if (is_ascii_letter(p[i])) return 1;
    if (p[i] >= 0xC3 && p[i] <= 0xDF && i + 1 < len && (p[i + 1] & 0xC0) == 0x80 &&
        !(p[i] == 0xC3 && (p[i + 1] == 0x97 || p[i + 1] == 0xB7))) return 2;
    return 0;
}

/* a letter or a digit ends just before p[i] */
static int alnum_before(const unsigned char *p, size_t i) {
    if (!i) return 0;
    unsigned c = p[i - 1];
    if (is_ascii_letter(c) || (c >= '0' && c <= '9')) return 1;
    return i >= 2 && (c & 0xC0) == 0x80 && p[i - 2] >= 0xC3 && p[i - 2] <= 0xDF &&
           !(p[i - 2] == 0xC3 && (c == 0x97 || c == 0xB7));
}

/* a space between digit groups: a space, a no-break space, a narrow no-break space or a thin space */
static size_t space_at(const unsigned char *p, size_t i, size_t len) {
    if (i < len && p[i] == ' ') return 1;
    if (has_at(p, i, len, "\xc2\xa0")) return 2;
    if (has_at(p, i, len, "\xe2\x80\xaf") || has_at(p, i, len, "\xe2\x80\x89")) return 3;
    return 0;
}

static unsigned long long value(const unsigned char *d, size_t n) {
    unsigned long long v = 0;
    for (size_t i = 0; i < n; i++) if (d[i] >= '0' && d[i] <= '9') v = v * 10 + (d[i] - '0');
    return v;
}

static void digits(Out *o, const Lex *x, const unsigned char *d, size_t n) {
    for (size_t i = 0; i < n; i++) if (d[i] >= '0' && d[i] <= '9') word(o, x->digit[d[i] - '0']);
}

/* the word after p[j] (one space on) is a noun a count can go before: "21 años" veintiún años, "1 libro" un libro;
 * not "el 1 de enero" (uno de enero) */
static const char *const NOT_NOUNS[] = { "y", "e", "o", "u", "de", "del", "a", "al", "en", "por", "para", "con", "sin",
    "que", "es", "son", "fue", "era", "más", "menos", "x", "entre", "hasta", "desde", "sobre", "ni", "se", "no", "como",
    "contra", "según", "está", "están", "veces", NULL };
static int noun_follows(const unsigned char *p, size_t j, size_t len) {
    if (j >= len || p[j] != ' ' || !letter_at(p, j + 1, len)) return 0;
    char w[24];
    size_t n = 0, k = j + 1, l;
    while ((l = letter_at(p, k, len))) {
        if (n + l >= sizeof w) return 1;
        if (l == 1) w[n++] = (char)(p[k] >= 'A' && p[k] <= 'Z' ? p[k] + 32 : p[k]);
        else { w[n++] = (char)p[k]; w[n++] = (char)p[k + 1]; }
        k += l;
    }
    return !in_list(NOT_NOUNS, w, n);
}

/* a number as written: [ip, ip+in) is its integer part (thousands separators in it), nd its digits */
typedef struct {
    int kind;                              /* 0 an integer, 1 a decimal, 2 groups (a version, an address) */
    const unsigned char *ip; size_t in, nd, max;
    const unsigned char *f; size_t fn; char fsep;
    const unsigned char *gs[8]; size_t gl[8]; char sep[8]; int ng;
} Num;

/* readable as a number (else digit by digit): no leading zero, at most max digits */
static int num_ok(const Num *m) { return !(m->nd > 1 && m->ip[0] == '0') && m->nd <= m->max; }

static void integer(Out *o, int L, const unsigned char *d, size_t n, size_t nd, size_t max, int g) {
    if ((nd > 1 && d[0] == '0') || nd > max) digits(o, &LEX[L], d, n);
    else card(o, L, value(d, n), g);
}

/* decimals: "3,5" tres coma cinco, "3,14" tres coma catorce; "3,05" and long ones digit by digit */
static void fraction(Out *o, int L, const unsigned char *d, size_t n) {
    if (n <= 3 && d[0] != '0') card(o, L, value(d, n), G_FULL);
    else digits(o, &LEX[L], d, n);
}

static void plain(Out *o, int L, const Num *m, int g) {
    const Lex *x = &LEX[L];
    if (m->kind == 2) {
        for (int k = 0; k < m->ng; k++) {
            if (k) word(o, m->sep[k] == ',' ? x->comma : x->point);
            integer(o, L, m->gs[k], m->gl[k], m->gl[k], 9, G_FULL);
        }
        return;
    }
    integer(o, L, m->ip, m->in, m->nd, m->max, m->kind == 1 ? G_FULL : g);
    if (m->kind == 1) { word(o, m->fsep == ',' ? x->comma : x->point); fraction(o, L, m->f, m->fn); }
}

static void money(Out *o, int L, const Num *m, int c) {
    const Cur *cu = &CUR[c];
    int g = cu->fem ? G_FEM : L == 0 ? G_APOC : G_FULL;
    const char *one = L == 0 ? cu->es1 : cu->pt1, *many = L == 0 ? cu->es : cu->pt;
    if (c == CUR_DOLLAR && o->peso) { one = "peso"; many = "pesos"; }
    unsigned long long v = value(m->ip, m->in);
    if (m->kind == 1 && m->fn == 2 && num_ok(m)) {               /* 25,50 € veinticinco euros con cincuenta */
        card(o, L, v, g);
        word(o, v == 1 ? one : many);
        unsigned long long cents = value(m->f, 2);
        if (cents) { word(o, LEX[L].cents); card(o, L, cents, G_FULL); }
    } else if (m->kind == 0 && num_ok(m)) {
        card(o, L, v, g);
        if (v >= 1000000 && v % 1000000 == 0) word(o, LEX[L].of);   /* un millón de euros */
        word(o, v == 1 ? one : many);
    } else { plain(o, L, m, G_FULL); word(o, many); }
}

/* "14:30", "14:30:15", "14h30": hours 0–24 in one or two digits; returns the end, 0 when it isn't a time */
static size_t time_at(const unsigned char *p, size_t i, size_t len, unsigned *h, unsigned *mi, unsigned *s, int *has_s) {
    size_t n = digit_run(p, i, len), j = i + n;
    if (n < 1 || n > 2 || j >= len) return 0;
    *h = (unsigned)value(p + i, n);
    *mi = *s = 0;
    *has_s = 0;
    if (*h > 24 || (p[j] != ':' && p[j] != 'h') || digit_run(p, j + 1, len) != 2) return 0;
    *mi = (unsigned)value(p + j + 1, 2);
    if (*mi > 59) return 0;
    int colon = p[j] == ':';
    j += 3;
    if (colon && j < len && p[j] == ':' && digit_run(p, j + 1, len) == 2 && value(p + j + 1, 2) < 60) {
        *s = (unsigned)value(p + j + 1, 2);
        *has_s = 1;
        j += 3;
    }
    if (!colon && has_at(p, j, len, "min")) j += 3;
    if (letter_at(p, j, len) || (j < len && (p[j] == ':' || p[j] == '.' || p[j] == ',') && isd(p, j + 1, len))) return 0;
    return j;
}

/* after a time or hours: "h", " h", "hs", "hrs" are taken; a following "horas"/"hora" word is left and noted */
static size_t hours_mark(const unsigned char *p, size_t j, size_t len, int *word_follows) {
    size_t k = j + (space_at(p, j, len) == 1);
    static const char *const marks[] = { "hrs", "hr", "hs", "h", NULL };
    *word_follows = 0;
    if (k > j && (has_at(p, k, len, "horas") || has_at(p, k, len, "hora"))) {
        size_t e = k + (has_at(p, k, len, "horas") ? 5 : 4);
        if (!letter_at(p, e, len)) { *word_follows = 1; return j; }
    }
    for (int t = 0; marks[t]; t++) {
        size_t l = strlen(marks[t]);
        if (has_at(p, k, len, marks[t]) && !letter_at(p, k + l, len) && !isd(p, k + l, len)) return k + l;
    }
    return j;
}

/* "15/11/2026", "15-11-2026", "15.11.2026" (a 4-digit year; "/" also takes 2), "2026-11-15" */
static size_t date_at(const unsigned char *p, size_t i, size_t len, unsigned *d, unsigned *m, const unsigned char **y, size_t *yn) {
    size_t n0 = digit_run(p, i, len), j = i + n0;
    if (j >= len) return 0;
    unsigned char s = p[j];
    if (s != '/' && s != '-' && s != '.') return 0;
    size_t n1 = digit_run(p, j + 1, len), k = j + 1 + n1;
    if (n1 < 1 || n1 > 2 || k >= len || p[k] != s) return 0;
    size_t n2 = digit_run(p, k + 1, len), e = k + 1 + n2;
    if (e < len && (p[e] == '/' || p[e] == '-' || p[e] == '.' || p[e] == ':' || p[e] == ',') && isd(p, e + 1, len)) return 0;
    if (letter_at(p, e, len)) return 0;
    if (n0 == 4 && s == '-' && n1 == 2 && n2 == 2) {
        *y = p + i; *yn = 4; *m = (unsigned)value(p + j + 1, 2); *d = (unsigned)value(p + k + 1, 2);
    } else if (n0 <= 2 && (n2 == 4 || (n2 == 2 && s == '/'))) {
        *d = (unsigned)value(p + i, n0); *m = (unsigned)value(p + j + 1, n1); *y = p + k + 1; *yn = n2;
    } else return 0;
    return *d >= 1 && *d <= 31 && *m >= 1 && *m <= 12 ? e : 0;
}

/* the number at p[i] (a digit), with what follows it that belongs to it; in_word: right after letters ("MP3",
 * "H264"); cur: a currency symbol read before it, else -1. Returns where the text goes on. */
static size_t number(Out *o, int L, const unsigned char *p, size_t i, size_t len, int in_word, int cur) {
    const Lex *x = &LEX[L];
    size_t n0 = digit_run(p, i, len), j = i + n0;

    if (!in_word && cur < 0) {
        unsigned h, mi, s, d, mo;
        int has_s, word_follows;
        size_t e = time_at(p, i, len, &h, &mi, &s, &has_s);
        if (e) {
            e = hours_mark(p, e, len, &word_follows);
            card(o, L, h, G_FEM);
            if (has_s) {                                  /* 1:02:03 una hora, dos minutos y tres segundos */
                word(o, h == 1 ? x->hour : x->hours);
                if (mi) { put(o, ",", 1); card(o, L, mi, L == 0 ? G_APOC : G_FULL); word(o, mi == 1 ? x->minute : x->minutes); }
                if (s) { word(o, x->and_); card(o, L, s, L == 0 ? G_APOC : G_FULL); word(o, s == 1 ? x->second : x->seconds); }
            } else if (mi) { word(o, x->and_); card(o, L, mi, G_FULL); }
            else if (!word_follows) word(o, h == 1 ? x->hour : x->hours);
            return e;
        }
        const unsigned char *y;
        size_t yn;
        e = date_at(p, i, len, &d, &mo, &y, &yn);
        if (e) {
            if (d == 1) word(o, x->first_day); else card(o, L, d, G_FULL);
            word(o, x->of);
            word(o, x->month[mo - 1]);
            word(o, x->of);
            card(o, L, value(y, yn), G_FULL);
            return e;
        }
        /* "4h", "48 hs": hours */
        if (n0 <= 3 && !(n0 > 1 && p[i] == '0')) {
            size_t e2 = hours_mark(p, j, len, &word_follows);
            if (e2 > j) {
                unsigned long long v = value(p + i, n0);
                card(o, L, v, G_FEM);
                word(o, v == 1 ? x->hour : x->hours);
                return e2;
            }
        }
    }

    /* the digit groups: "1.000.000", "1 000", "1.234,56", "3,5", "3.14", "192.168.0.1" */
    Num m;
    memset(&m, 0, sizeof m);
    m.gs[0] = p + i; m.gl[0] = n0; m.ng = 1;
    int spaces = 0;
    while (m.ng < 8) {
        size_t sl = 0;
        char s;
        if (j + 1 < len && (p[j] == '.' || p[j] == ',') && isd(p, j + 1, len)) { s = (char)p[j]; sl = 1; }
        else if (!in_word && (sl = space_at(p, j, len)) && n0 <= 3 && p[i] != '0' && digit_run(p, j + sl, len) == 3) {
            int all = 1;
            for (int k = 1; k < m.ng; k++) if (m.sep[k] != 's') all = 0;
            if (!all) break;
            s = 's';
            spaces += sl == 1;
        } else break;
        size_t r = digit_run(p, j + sl, len);
        m.gs[m.ng] = p + j + sl; m.gl[m.ng] = r; m.sep[m.ng] = s; m.ng++;
        j += sl + r;
    }
    /* "612 345 678" is a phone number, not 612 million: plain spaces group thousands after one or two digits, or once */
    if (spaces >= 2 && n0 == 3) {
        int k = 0;
        for (; k < m.ng && (!k || m.sep[k] == 's'); k++) integer(o, L, m.gs[k], m.gl[k], m.gl[k], 9, G_FULL);
        return (size_t)(m.gs[k - 1] + m.gl[k - 1] - p);
    }

    m.ip = p + i; m.in = n0; m.nd = n0; m.max = in_word ? 4 : 9; m.kind = 0;
    if (m.ng > 1) {
        char t = m.sep[1];
        int th = 0;
        if (n0 <= 3 && p[i] != '0') while (1 + th < m.ng && m.sep[1 + th] == t && m.gl[1 + th] == 3) th++;
        int rest = m.ng - 1 - th;
        char dec = t == ',' ? '.' : ',';
        int thousands = th >= 1 && (rest == 0 || (rest == 1 && m.sep[m.ng - 1] == dec)) && !(t == ',' && th == 1 && rest == 0);
        if (thousands) {
            m.in = (size_t)(m.gs[th] + m.gl[th] - m.ip);
            m.nd = n0 + 3 * (size_t)th;
            m.max = 12;
            if (rest) { m.kind = 1; m.f = m.gs[m.ng - 1]; m.fn = m.gl[m.ng - 1]; m.fsep = dec; j = (size_t)(m.f + m.fn - p); }
            else j = (size_t)(m.ip + m.in - p);
        } else if (t == 's') { m.ng = 1; j = i + n0; }
        else if (m.ng == 2) { m.kind = 1; m.f = m.gs[1]; m.fn = m.gl[1]; m.fsep = t; }
        else {
            /* groups: stop before a space group ("1.2 345" is 1.2, then 345) */
            int k = 1;
            while (k < m.ng && m.sep[k] != 's') k++;
            m.ng = k;
            j = (size_t)(m.gs[k - 1] + m.gl[k - 1] - p);
            if (k == 2) { m.kind = 1; m.f = m.gs[1]; m.fn = m.gl[1]; m.fsep = m.sep[1]; }
            else m.kind = 2;
        }
    }
    unsigned long long v = value(m.ip, m.in);
    int ok = num_ok(&m);

    size_t k = j + space_at(p, j, len);
    /* 25%, 25 % */
    if (k < len && p[k] == '%') { plain(o, L, &m, G_FULL); word(o, x->percent); return k + 1; }
    /* 25 €, 25€, 5 £, 10$ */
    for (int t = CUR_AFTER; cur < 0 && m.kind != 2 && t < (int)(sizeof CUR / sizeof *CUR); t++)
        if (has_at(p, k, len, CUR[t].sym) && !letter_at(p, k + strlen(CUR[t].sym), len)) { cur = t; j = k + strlen(CUR[t].sym); }
    if (cur >= 0) { money(o, L, &m, cur); return j; }
    /* 25 °C, 25°, 25ºC */
    if (m.kind != 2 && (has_at(p, k, len, "\xc2\xb0") ||
                        (has_at(p, k, len, "\xc2\xba") && (has_at(p, k + 2, len, "C") || has_at(p, k + 2, len, "F"))))) {
        size_t e = k + 2, e2 = e + space_at(p, e, len);
        const char *scale = NULL;
        if (e2 < len && (p[e2] == 'C' || p[e2] == 'F') && !letter_at(p, e2 + 1, len)) { scale = p[e2] == 'C' ? "Celsius" : "Fahrenheit"; e = e2 + 1; }
        plain(o, L, &m, L == 0 ? G_APOC : G_FULL);
        word(o, m.kind == 0 && v == 1 && ok ? x->degree : x->degrees);
        if (scale) word(o, scale);
        return e;
    }
    /* ordinals: 1.º 1.ª 1º 1ª (and es 1.er 3.er) */
    if (m.kind == 0 && m.ng == 1 && ok && v >= 1 && !in_word) {
        size_t a = j + (j < len && p[j] == '.');
        int fem = has_at(p, a, len, "\xc2\xaa"), masc = has_at(p, a, len, "\xc2\xba");
        int er = L == 0 && has_at(p, a, len, "er") && (v % 10 == 1 || v % 10 == 3) && v % 100 != 11 && v % 100 != 13;
        size_t e = a + (fem || masc || er ? 2 : 0);
        if ((fem || masc || er) && !letter_at(p, e, len)) {
            if (v <= 100) ordinal(o, L, (unsigned)v, fem, er);
            else card(o, L, v, fem ? G_FEM : G_FULL);
            return e;
        }
    }
    /* "10-20" diez a veinte; "11-5555-1234" in groups */
    if (m.kind == 0 && m.ng == 1 && !in_word && j < len && p[j] == '-' && isd(p, j + 1, len)) {
        const unsigned char *gs[8];
        size_t gl[8], e = j;
        int ng = 1;
        gs[0] = p + i; gl[0] = n0;
        while (ng < 8 && e < len && p[e] == '-' && isd(p, e + 1, len)) {
            gs[ng] = p + e + 1; gl[ng] = digit_run(p, e + 1, len); e += 1 + gl[ng]; ng++;
        }
        if (!(e < len && (p[e] == '.' || p[e] == ',') && isd(p, e + 1, len)) && !letter_at(p, e, len)) {
            if (ng == 2 && gl[0] <= 4 && gl[1] <= 4 && gs[0][0] != '0' && gs[1][0] != '0' && value(gs[0], gl[0]) < value(gs[1], gl[1])) {
                card(o, L, value(gs[0], gl[0]), G_FULL); word(o, x->to); card(o, L, value(gs[1], gl[1]), G_FULL);
            } else for (int t = 0; t < ng; t++) {
                if (t) put(o, ",", 1);
                integer(o, L, gs[t], gl[t], gl[t], 9, G_FULL);
            }
            return e;
        }
    }
    plain(o, L, &m, L == 0 && m.kind == 0 && ok && v < 1000 && !in_word && noun_follows(p, j, len) ? G_APOC : G_FULL);
    /* "24/7", "3/4" */
    if (j < len && p[j] == '/' && isd(p, j + 1, len)) { word(o, x->slash); return j + 1; }
    return j;
}

/* an address or an e-mail starting at p[i] (http://…, www.…, name@host.tld): its length, else 0 */
static size_t url_at(const unsigned char *p, size_t i, size_t len) {
    size_t e = i;
    while (e < len && p[e] != ' ' && p[e] != '\n' && p[e] != '\t') e++;
    while (e > i && p[e - 1] && strchr(".,;:!?)]}\"'", p[e - 1])) e--;
    if (e - i < 5) return 0;
    for (size_t k = i; k + 3 <= e; k++)
        if (p[k] == ':' && p[k + 1] == '/' && p[k + 2] == '/') return k > i && k + 3 < e ? e - i : 0;
    if (!memcmp(p + i, "www.", 4) || !memcmp(p + i, "WWW.", 4)) return e - i;
    for (size_t k = i + 1; k < e; k++)
        if (p[k] == '@') {
            if (!is_ascii_letter(p[k - 1]) && !isd(p, k - 1, len)) return 0;
            for (size_t t = k + 2; t + 1 < e; t++) if (p[t] == '.' && is_ascii_letter(p[t + 1])) return e - i;
            return 0;
        }
    return 0;
}

/* letters as they are, digits one by one, the punctuation named: "info@lordos.org" info arroba lordos punto org */
static void url(Out *o, const Lex *x, const unsigned char *p, size_t i, size_t e) {
    for (size_t k = i; k + 3 <= e; k++) if (p[k] == ':' && p[k + 1] == '/' && p[k + 2] == '/') { i = k + 3; break; }
    while (e > i && p[e - 1] == '/') e--;
    for (size_t k = i; k < e;) {
        unsigned c = p[k];
        if (is_ascii_letter(c) || c >= 0x80) {
            size_t a = k;
            while (k < e && (is_ascii_letter(p[k]) || p[k] >= 0x80)) k++;
            put(o, " ", 1); put(o, (const char *)p + a, k - a); put(o, " ", 1);
            continue;
        }
        if (c >= '0' && c <= '9') word(o, x->digit[c - '0']);
        else if (c == '.') word(o, x->dot);
        else if (c == '@') word(o, x->at);
        else if (c == '/') word(o, x->slash);
        else if (c == '-') word(o, x->dash);
        else if (c == '_') word(o, x->underscore);
        else if (c == ':') word(o, x->colon);
        else put(o, " ", 1);
        k++;
    }
}

/* one space where there were several, none at a line's ends, before . , ; : ! ? ) ] or after ( [ ¿ ¡ */
static size_t tidy(char *s, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ') {
            if (!o || s[o - 1] == ' ' || s[o - 1] == '\n' || s[o - 1] == '(' || s[o - 1] == '[') continue;
            if (o >= 2 && (unsigned char)s[o - 2] == 0xC2 && ((unsigned char)s[o - 1] == 0xBF || (unsigned char)s[o - 1] == 0xA1)) continue;
            s[o++] = ' ';
            continue;
        }
        if (o && s[o - 1] == ' ' && c && (strchr(".,;:!?)]", c) || c == '\n')) o--;
        s[o++] = c;
    }
    while (o && s[o - 1] == ' ') o--;
    s[o] = 0;
    return o;
}

char *say_normalize(const char *s, size_t len, const char *lang, const char *region, size_t *outlen) {
    Out o = { 0 };
    o.peso = peso_region(region);
    int L = lang && !strcmp(lang, "es") ? 0 : lang && !strcmp(lang, "pt") ? 1 : -1;
    put(&o, " ", 1);                                       /* always a buffer; tidy() drops it */
    if (L < 0) {
        put(&o, s, len);
        if (o.oom) { free(o.p); return NULL; }
        memmove(o.p, o.p + 1, o.n - 1);
        o.p[--o.n] = 0;
        if (outlen) *outlen = o.n;
        return o.p;
    }
    const Lex *x = &LEX[L];
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < len && !o.oom;) {
        unsigned c = p[i];
        int bnd = !alnum_before(p, i);
        if (bnd && c != ' ' && c != '\n') {
            size_t u = url_at(p, i, len);
            if (u) { url(&o, x, p, i, i + u); i += u; continue; }
        }
        if (c >= '0' && c <= '9') { i = number(&o, L, p, i, len, !bnd, -1); continue; }
        if (c == '#') {
            if (!bnd) put(&o, "#", 1);                            /* C# */
            else if (isd(p, i + 1, len)) word(&o, x->number);     /* #5 número cinco */
            else put(&o, " ", 1);                                 /* #LordOS LordOS, a heading's # */
            i++;
            continue;
        }
        if (c == '@') { word(&o, x->at); i++; continue; }
        if (c == '&') { word(&o, x->and_); i++; continue; }
        if (c == '%') { word(&o, x->percent); i++; continue; }
        if ((c == '+' || c == '=') && isd(p, i + 1 + space_at(p, i + 1, len), len)) { word(&o, c == '+' ? x->plus : x->equals); i++; continue; }
        if (bnd) {
            /* nº 5, n.º 5, N° 5 */
            if (c == 'n' || c == 'N') {
                size_t k = i + 1 + (i + 1 < len && p[i + 1] == '.');
                if ((has_at(p, k, len, "\xc2\xba") || has_at(p, k, len, "\xc2\xb0")) && isd(p, k + 2 + space_at(p, k + 2, len), len)) {
                    word(&o, x->number);
                    i = k + 2;
                    continue;
                }
            }
            /* $10, US$ 10, R$ 10,50, €25, £5 */
            int found = 0;
            for (size_t t = 0; t < sizeof CUR / sizeof *CUR && !found; t++) {
                size_t l = strlen(CUR[t].sym);
                if (has_at(p, i, len, CUR[t].sym)) {
                    size_t k = i + l + space_at(p, i + l, len);
                    if (isd(p, k, len)) { i = number(&o, L, p, k, len, 0, (int)t); found = 1; }
                }
            }
            if (found) continue;
            /* -5, −5 */
            if ((c == '-' && isd(p, i + 1, len)) || (has_at(p, i, len, "\xe2\x88\x92") && isd(p, i + 3, len))) {
                word(&o, x->minus);
                i += c == '-' ? 1 : 3;
                continue;
            }
        }
        if (c == '-' && !bnd && isd(p, i + 1, len)) { put(&o, " ", 1); i++; continue; }   /* COVID-19 */
        size_t k = c < 0x80 ? 1 : utf8_len(p + i, len - i);
        if (!k) k = 1;
        put(&o, (const char *)p + i, k);
        i += k;
    }
    put(&o, " ", 1);
    if (o.oom) { free(o.p); return NULL; }
    size_t n = tidy(o.p, o.n);
    if (outlen) *outlen = n;
    return o.p;
}
