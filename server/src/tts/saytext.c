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
 * The most common short words of each language LordOS has voices for, and the letters only one or two of them use. */
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
/* French's elisions (l'été, c'est, qu'il, d'un, j'ai) leave l, c, qu, d, j as words of their own */
static const char *const WORDS_FR[] = { "le", "les", "des", "du", "et", "est", "une", "pour", "dans", "qui", "pas", "sur",
    "au", "aux", "avec", "ce", "cette", "mais", "ou", "sont", "nous", "vous", "ils", "elle", "elles", "je", "il", "ne",
    "être", "fait", "tout", "leur", "très", "aussi", "où", "été", "c", "d", "l", "qu", "j", "ça", "sans", "chez", "votre",
    "notre", "peut", "ont", NULL };
static const char *const WORDS_DE[] = { "der", "die", "das", "und", "ist", "nicht", "ein", "eine", "zu", "den", "dem", "mit",
    "sich", "auf", "für", "von", "im", "auch", "ich", "sie", "wir", "ihr", "aber", "oder", "noch", "nach", "bei", "aus",
    "wird", "sind", "hat", "haben", "kann", "nur", "schon", "wenn", "dass", "mehr", "durch", "über", "zum", "zur", "einen",
    "einem", "jeden", "jede", "ist", "sehr", "werden", NULL };
static const char *const WORDS_IT[] = { "il", "lo", "gli", "di", "che", "è", "e", "la", "per", "non", "con", "del", "della",
    "dei", "delle", "nel", "nella", "sono", "ho", "ti", "ma", "anche", "più", "questo", "questa", "quando", "perché",
    "molto", "tutto", "alla", "dal", "sul", "ci", "io", "lui", "lei", "noi", "voi", "loro", "posso", "qualsiasi", "una",
    "un", "si", "mi", "degli", "essere", "stato", NULL };
static const char *const WORDS_NL[] = { "de", "het", "een", "en", "van", "ik", "je", "is", "dat", "niet", "op", "te", "zijn",
    "met", "voor", "die", "er", "maar", "om", "aan", "ook", "als", "bij", "nog", "wat", "naar", "kan", "dan", "wel", "hij",
    "zij", "we", "wij", "ze", "heb", "hebben", "worden", "wordt", "door", "uit", "deze", "dit", "geen", "meer", "mijn",
    "jij", "elke", "kunt", "heeft", NULL };

static int in_list(const char *const *l, const char *w, size_t n) {
    for (; *l; l++) if (strlen(*l) == n && !memcmp(*l, w, n)) return 1;
    return 0;
}

static int has_at(const unsigned char *p, size_t i, size_t len, const char *u) {
    size_t n = strlen(u);
    return i + n <= len && !memcmp(p + i, u, n);
}

#define LANGS_N 7
const char *say_language(const char *s, size_t len, const char *fallback) {
    static const char *const langs[LANGS_N] = { "es", "en", "pt", "fr", "de", "it", "nl" };
    static const char *const *const words[LANGS_N] = { WORDS_ES, WORDS_EN, WORDS_PT, WORDS_FR, WORDS_DE, WORDS_IT, WORDS_NL };
    /* letters that tell a language: ñ ¿ ¡ (es); ã õ (pt); ç (pt, fr); ê â î û œ (fr); ß ä ö (de, 2) ü (de, 1); ì ò (it) */
    static const struct { const char *u; signed char lang, score; } marks[] = {
        { "\xc3\xb1", 0, 2 }, { "\xc2\xbf", 0, 2 }, { "\xc2\xa1", 0, 2 },
        { "\xc3\xa3", 2, 2 }, { "\xc3\xb5", 2, 2 }, { "\xc3\xa7", 2, 2 }, { "\xc3\xa7", 3, 2 },
        { "\xc3\xaa", 3, 1 }, { "\xc3\xa2", 3, 1 }, { "\xc3\xae", 3, 2 }, { "\xc3\xbb", 3, 2 }, { "\xc5\x93", 3, 2 },
        { "\xc3\x9f", 4, 2 }, { "\xc3\xa4", 4, 2 }, { "\xc3\xb6", 4, 2 }, { "\xc3\xbc", 4, 1 },
        { "\xc3\xac", 5, 2 }, { "\xc3\xb2", 5, 2 },
    };
    int score[LANGS_N] = { 0 }, fb = 1;
    for (int k = 0; k < LANGS_N; k++) if (fallback && !strcmp(fallback, langs[k])) fb = k;
    const unsigned char *p = (const unsigned char *)s;
    if (len > 4000) len = 4000;
    char w[24];
    size_t wn = 0;
    for (size_t i = 0; i <= len; i++) {
        unsigned char c = i < len ? p[i] : ' ';
        int apos = has_at(p, i, len, "\xe2\x80\x99");             /* ’ ends a word as ' does: l’été */
        /* the letters of a word: ASCII ones lowercased, and any UTF-8 sequence (accents) as it is */
        if (!apos && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80)) {
            if (wn < sizeof w) w[wn] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            wn++;
            if (c >= 0xC2)
                for (size_t m = 0; m < sizeof marks / sizeof *marks; m++)
                    if (has_at(p, i, len, marks[m].u)) score[(int)marks[m].lang] += marks[m].score;
            continue;
        }
        if (wn && wn <= sizeof w)
            for (int k = 0; k < LANGS_N; k++) score[k] += in_list(words[k], w, wn);
        wn = 0;
        if (apos) i += 2;
    }
    int best = fb;
    for (int k = 0; k < LANGS_N; k++) if (score[k] > score[best]) best = k;
    return score[best] >= 2 && score[best] > score[fb] ? langs[best] : langs[fb];
}

/* ---- text normalization (every language but English) ----
 * Pocket TTS's Spanish and Portuguese models never learned digits or symbols (measured: "Tengo 88 años" came out as
 * "Tengo bronce proteaños"), while they read the same text written as words perfectly; the French, German, Italian and
 * Dutch ones were trained the same way. audio.cpp's text preparation does no number normalization for any language:
 * the English model reads digits because it learned them. So for the others, say writes numbers, times, dates,
 * money, ordinals, symbols and common abbreviations out as words before it splits the text (docs/VOICE.md, "Text
 * normalization"). Portuguese is Brazilian Portuguese where the two differ (dezesseis, dezenove, bilhão); French is
 * France's (soixante-dix, quatre-vingts), with the traditional hyphens. The rules follow the usual ones (num2words,
 * NeMo's text normalization) with tables of our own. */

enum { ES, PT, FR, DE, IT, NL };

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
static void word(Out *o, const char *w) { if (*w) { put(o, " ", 1); puts_(o, w); put(o, " ", 1); } }

/* how "1" is said before a word: es uno / un / una; pt um / um / uma; fr un / un / une; de eins / ein / eine;
 * it uno / un / una; nl één always */
enum { G_FULL, G_APOC, G_FEM };

typedef struct {
    const char *digit[10], *comma, *point, *and_, *minus, *plus, *equals, *percent, *number, *at, *slash, *dot, *dash,
        *underscore, *colon, *hour, *hours, *minute, *minutes, *second, *seconds, *degree, *degrees, *cents, *of, *to,
        *first_day, *month[12];
    /* "14:30": NULL where the minutes follow "and" (es catorce y treinta), else the word after the hour (fr quatorze
     * heures trente, de vierzehn Uhr dreißig); clock1 for one and zero o'clock (une heure) */
    const char *clock, *clock1;
    const char *million_of;                /* un millón de euros; "" when nothing goes between (eine Million Euro) */
    const char *hour_words[3];             /* written after a time or hours: 14:30 horas, 14:30 Uhr */
    unsigned char g_clock, g_hour, g_min, g_degree, g_count, whole_hours;   /* whole_hours: 14:00 catorce horas */
} Lex;

static const Lex LEX[6] = {
    { { "cero", "uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve" }, "coma", "punto", "y",
      "menos", "más", "igual a", "por ciento", "número", "arroba", "barra", "punto", "guion", "guion bajo", "dos puntos",
      "hora", "horas", "minuto", "minutos", "segundo", "segundos", "grado", "grados", "con", "de", "a", "uno",
      { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre",
        "diciembre" },
      NULL, NULL, "de", { "horas", "hora", NULL }, G_FEM, G_FEM, G_APOC, G_APOC, G_APOC, 1 },
    { { "zero", "um", "dois", "três", "quatro", "cinco", "seis", "sete", "oito", "nove" }, "vírgula", "ponto", "e",
      "menos", "mais", "igual a", "por cento", "número", "arroba", "barra", "ponto", "hífen", "sublinhado", "dois pontos",
      "hora", "horas", "minuto", "minutos", "segundo", "segundos", "grau", "graus", "e", "de", "a", "primeiro",
      { "janeiro", "fevereiro", "março", "abril", "maio", "junho", "julho", "agosto", "setembro", "outubro", "novembro",
        "dezembro" },
      NULL, NULL, "de", { "horas", "hora", NULL }, G_FEM, G_FEM, G_FULL, G_FULL, G_FULL, 1 },
    { { "zéro", "un", "deux", "trois", "quatre", "cinq", "six", "sept", "huit", "neuf" }, "virgule", "point", "et",
      "moins", "plus", "égale", "pour cent", "numéro", "arobase", "slash", "point", "tiret", "tiret bas", "deux-points",
      "heure", "heures", "minute", "minutes", "seconde", "secondes", "degré", "degrés", "", "de", "à", "premier",
      { "janvier", "février", "mars", "avril", "mai", "juin", "juillet", "août", "septembre", "octobre", "novembre",
        "décembre" },
      "heures", "heure", "de", { "heures", "heure", NULL }, G_FEM, G_FEM, G_FEM, G_FULL, G_FULL, 0 },
    { { "null", "eins", "zwei", "drei", "vier", "fünf", "sechs", "sieben", "acht", "neun" }, "Komma", "Punkt", "und",
      "minus", "plus", "gleich", "Prozent", "Nummer", "at", "Schrägstrich", "Punkt", "Bindestrich", "Unterstrich",
      "Doppelpunkt", "Stunde", "Stunden", "Minute", "Minuten", "Sekunde", "Sekunden", "Grad", "Grad", "", "", "bis", "",
      { "Januar", "Februar", "März", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November",
        "Dezember" },
      "Uhr", "Uhr", "", { "Uhr", NULL, NULL }, G_APOC, G_FEM, G_FEM, G_APOC, G_APOC, 0 },
    { { "zero", "uno", "due", "tre", "quattro", "cinque", "sei", "sette", "otto", "nove" }, "virgola", "punto", "e",
      "meno", "più", "uguale a", "per cento", "numero", "chiocciola", "barra", "punto", "trattino", "trattino basso",
      "due punti", "ora", "ore", "minuto", "minuti", "secondo", "secondi", "grado", "gradi", "e", "di", "a", "primo",
      { "gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre",
        "novembre", "dicembre" },
      NULL, NULL, "di", { "ore", NULL, NULL }, G_FEM, G_FEM, G_APOC, G_APOC, G_APOC, 0 },
    { { "nul", "een", "twee", "drie", "vier", "vijf", "zes", "zeven", "acht", "negen" }, "komma", "punt", "en",
      "min", "plus", "is gelijk aan", "procent", "nummer", "at", "slash", "punt", "streepje", "underscore",
      "dubbele punt", "uur", "uur", "minuut", "minuten", "seconde", "seconden", "graad", "graden", "", "", "tot", "één",
      { "januari", "februari", "maart", "april", "mei", "juni", "juli", "augustus", "september", "oktober", "november",
        "december" },
      "uur", "uur", "", { "uur", NULL, NULL }, G_FULL, G_FULL, G_FULL, G_FULL, G_FULL, 0 },
};

/* money: the symbol before or after the amount, its name in each language (one, many), and whether it is feminine */
typedef struct { const char *sym, *name[6][2]; unsigned char fem[6]; } Cur;
#define DOLLAR { { "dólar", "dólares" }, { "dólar", "dólares" }, { "dollar", "dollars" }, { "Dollar", "Dollar" }, \
                 { "dollaro", "dollari" }, { "dollar", "dollar" } }
static const Cur CUR[] = {
    { "US$", DOLLAR, { 0 } },
    { "U$S", DOLLAR, { 0 } },
    { "R$", { { "real", "reales" }, { "real", "reais" }, { "réal", "reais" }, { "Real", "Reais" }, { "real", "reais" },
              { "real", "reais" } }, { 0 } },
    { "$", DOLLAR, { 0 } },
    { "\xe2\x82\xac", { { "euro", "euros" }, { "euro", "euros" }, { "euro", "euros" }, { "Euro", "Euro" },     /* € */
                        { "euro", "euro" }, { "euro", "euro" } }, { 0 } },
    { "\xc2\xa3", { { "libra", "libras" }, { "libra", "libras" }, { "livre", "livres" }, { "Pfund", "Pfund" },  /* £ */
                    { "sterlina", "sterline" }, { "pond", "pond" } }, { 1, 1, 1, 0, 1, 0 } },
    { "USD", DOLLAR, { 0 } },
};
static const char *const PESO[6][2] = { { "peso", "pesos" }, { "peso", "pesos" }, { "peso", "pesos" }, { "Peso", "Pesos" },
                                        { "peso", "pesos" }, { "peso", "peso's" } };
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

/* French (France): 70 soixante-dix, 71 soixante et onze, 80 quatre-vingts, 81 quatre-vingt-un, 90 quatre-vingt-dix;
 * "cents" and "quatre-vingts" keep their s only at the end of the number or before million/milliard (deux cents,
 * deux cent mille, deux cents millions); mille never takes one. Hyphens as traditionally written (below 100 and not
 * around "et"). */
static const char *const FR_U[17] = { "zéro", "un", "deux", "trois", "quatre", "cinq", "six", "sept", "huit", "neuf",
    "dix", "onze", "douze", "treize", "quatorze", "quinze", "seize" };
static const char *const FR_T[7] = { "", "", "vingt", "trente", "quarante", "cinquante", "soixante" };

static void fr_99(Out *o, unsigned n, int g, int s) {
    if (n < 17) { puts_(o, n == 1 && g == G_FEM ? "une" : FR_U[n]); return; }
    if (n < 20) { puts_(o, "dix-"); puts_(o, FR_U[n - 10]); return; }
    unsigned t = n / 10, u = n % 10;
    if (t == 7 || t == 9) {
        puts_(o, t == 7 ? "soixante" : "quatre-vingt");
        if (t == 7 && u == 1) { puts_(o, " et onze"); return; }
        puts_(o, "-");
        fr_99(o, 10 + u, g, 0);
        return;
    }
    if (t == 8) {
        puts_(o, "quatre-vingt");
        if (!u) { if (s) puts_(o, "s"); }
        else { puts_(o, "-"); fr_99(o, u, g, 0); }
        return;
    }
    puts_(o, FR_T[t]);
    if (u == 1) { puts_(o, " et "); fr_99(o, 1, g, 0); }
    else if (u) { puts_(o, "-"); puts_(o, FR_U[u]); }
}

/* 1 … 999; s: the plural s of cents and quatre-vingts may go at its end */
static void fr_999(Out *o, unsigned n, int g, int s) {
    unsigned h = n / 100, r = n % 100;
    if (h) {
        if (h > 1) { puts_(o, FR_U[h]); puts_(o, " "); }
        puts_(o, "cent");
        if (h > 1 && !r && s) puts_(o, "s");
        if (r) puts_(o, " ");
    }
    if (r) fr_99(o, r, g, s);
}

static void fr_card(Out *o, unsigned long long n, int g) {
    if (!n) { puts_(o, "zéro"); return; }
    unsigned v[4] = { (unsigned)(n / 1000000000ull % 1000), (unsigned)(n / 1000000 % 1000), (unsigned)(n / 1000 % 1000),
                      (unsigned)(n % 1000) };
    static const char *const big[2][2] = { { "un milliard", " milliards" }, { "un million", " millions" } };
    int first = 1;
    for (int k = 0; k < 4; k++) {
        if (!v[k]) continue;
        if (!first) puts_(o, " ");
        first = 0;
        if (k < 2) { if (v[k] == 1) puts_(o, big[k][0]); else { fr_999(o, v[k], G_FULL, 1); puts_(o, big[k][1]); } }
        else if (k == 2) { if (v[k] > 1) { fr_999(o, v[k], G_FULL, 0); puts_(o, " "); } puts_(o, "mille"); }
        else fr_999(o, v[k], g, 1);
    }
}

/* German: one word below a million (zweitausendsechsundzwanzig), "eins" only at the very end (hunderteins), "ein" in
 * front (einundzwanzig, einhundert is "hundert"); eine Million, zwei Millionen, eine Milliarde */
static const char *const DE_U[20] = { "null", "eins", "zwei", "drei", "vier", "fünf", "sechs", "sieben", "acht", "neun",
    "zehn", "elf", "zwölf", "dreizehn", "vierzehn", "fünfzehn", "sechzehn", "siebzehn", "achtzehn", "neunzehn" };
static const char *const DE_T[10] = { "", "", "zwanzig", "dreißig", "vierzig", "fünfzig", "sechzig", "siebzig", "achtzig",
    "neunzig" };

static void de_99(Out *o, unsigned n, int end, int g) {
    if (n == 1) { puts_(o, !end || g == G_APOC ? "ein" : g == G_FEM ? "eine" : "eins"); return; }
    if (n < 20) { puts_(o, DE_U[n]); return; }
    unsigned u = n % 10;
    if (u) { puts_(o, u == 1 ? "ein" : DE_U[u]); puts_(o, "und"); }
    puts_(o, DE_T[n / 10]);
}

static void de_999(Out *o, unsigned n, int end, int g) {
    unsigned h = n / 100, r = n % 100;
    if (h) { if (h > 1) de_99(o, h, 0, G_FULL); puts_(o, "hundert"); }
    if (r) de_99(o, r, end, g);
}

static void de_card(Out *o, unsigned long long n, int g) {
    if (!n) { puts_(o, "null"); return; }
    if (n != 1) g = G_FULL;                         /* ein, eine: only for one itself */
    unsigned v[2] = { (unsigned)(n / 1000000000ull % 1000), (unsigned)(n / 1000000 % 1000) };
    unsigned r = (unsigned)(n % 1000000), th = r / 1000, u = r % 1000;
    static const char *const big[2][2] = { { "eine Milliarde", " Milliarden" }, { "eine Million", " Millionen" } };
    int first = 1;
    for (int k = 0; k < 2; k++) {
        if (!v[k]) continue;
        if (!first) puts_(o, " ");
        first = 0;
        if (v[k] == 1) puts_(o, big[k][0]); else { de_999(o, v[k], 1, G_FULL); puts_(o, big[k][1]); }
    }
    if (!r) return;
    if (!first) puts_(o, " ");
    if (th) { if (th > 1) de_999(o, th, 0, G_FULL); puts_(o, "tausend"); }
    if (u) de_999(o, u, 1, g);
}

/* Italian: one word below a million, the tens' vowel dropped before uno and otto (ventuno, trentotto, centottanta),
 * tré at the end of a longer number (ventitré, centotré); mille, duemila; un milione, due milioni */
static const char *const IT_U[20] = { "zero", "uno", "due", "tre", "quattro", "cinque", "sei", "sette", "otto", "nove",
    "dieci", "undici", "dodici", "tredici", "quattordici", "quindici", "sedici", "diciassette", "diciotto", "diciannove" };
static const char *const IT_T[10] = { "", "", "venti", "trenta", "quaranta", "cinquanta", "sessanta", "settanta",
    "ottanta", "novanta" };

/* pre: something comes before it in the same word; end: the number ends with it */
static void it_99(Out *o, unsigned n, int g, int pre, int end) {
    if (n == 1) { puts_(o, g == G_APOC ? "un" : g == G_FEM ? "una" : "uno"); return; }
    if (n == 3 && pre && end) { puts_(o, "tré"); return; }
    if (n < 20) { puts_(o, IT_U[n]); return; }
    unsigned u = n % 10;
    const char *t = IT_T[n / 10];
    if (u == 1 || u == 8) put(o, t, strlen(t) - 1); else puts_(o, t);
    if (u == 3 && end) puts_(o, "tré");
    else if (u) puts_(o, IT_U[u]);
}

static void it_999(Out *o, unsigned n, int g, int pre, int end) {
    unsigned h = n / 100, r = n % 100;
    if (h) {
        if (h > 1) puts_(o, IT_U[h]);
        puts_(o, r == 8 || r / 10 == 8 ? "cent" : "cento");
    }
    if (r) it_99(o, r, g, pre || h, end);
}

static void it_card(Out *o, unsigned long long n, int g) {
    if (!n) { puts_(o, "zero"); return; }
    if (n != 1) g = G_FULL;
    unsigned v[2] = { (unsigned)(n / 1000000000ull % 1000), (unsigned)(n / 1000000 % 1000) };
    unsigned r = (unsigned)(n % 1000000), th = r / 1000, u = r % 1000;
    static const char *const big[2][2] = { { "un miliardo", " miliardi" }, { "un milione", " milioni" } };
    int first = 1;
    for (int k = 0; k < 2; k++) {
        if (!v[k]) continue;
        if (!first) puts_(o, " ");
        first = 0;
        if (v[k] == 1) puts_(o, big[k][0]); else { it_999(o, v[k], G_FULL, 0, 1); puts_(o, big[k][1]); }
    }
    if (!r) return;
    if (!first) puts_(o, " ");
    if (th == 1) puts_(o, "mille");
    else if (th) { it_999(o, th, G_FULL, 0, 0); puts_(o, "mila"); }
    if (u) it_999(o, u, g, th != 0, 1);
}

/* Dutch: one word up to the thousands, a space after "duizend" (tweeduizend zesentwintig); a diaeresis where two
 * vowels would meet (tweeëntwintig); één alone, een inside a word (eenentwintig); één miljoen, twee miljard */
static const char *const NL_U[20] = { "nul", "een", "twee", "drie", "vier", "vijf", "zes", "zeven", "acht", "negen",
    "tien", "elf", "twaalf", "dertien", "veertien", "vijftien", "zestien", "zeventien", "achttien", "negentien" };
static const char *const NL_T[10] = { "", "", "twintig", "dertig", "veertig", "vijftig", "zestig", "zeventig", "tachtig",
    "negentig" };

static void nl_99(Out *o, unsigned n) {
    if (n < 20) { puts_(o, NL_U[n]); return; }
    unsigned u = n % 10;
    if (u) { puts_(o, NL_U[u]); puts_(o, u == 2 || u == 3 ? "\xc3\xabn" : "en"); }      /* ën */
    puts_(o, NL_T[n / 10]);
}

/* alone: nothing before it in the number, so 1 is één */
static void nl_999(Out *o, unsigned n, int alone) {
    if (n == 1 && alone) { puts_(o, "één"); return; }
    unsigned h = n / 100, r = n % 100;
    if (h) { if (h > 1) nl_99(o, h); puts_(o, "honderd"); }
    if (r) nl_99(o, r);
}

static void nl_card(Out *o, unsigned long long n) {
    if (!n) { puts_(o, "nul"); return; }
    unsigned v[2] = { (unsigned)(n / 1000000000ull % 1000), (unsigned)(n / 1000000 % 1000) };
    unsigned r = (unsigned)(n % 1000000), th = r / 1000, u = r % 1000;
    static const char *const big[2] = { " miljard", " miljoen" };
    int first = 1;
    for (int k = 0; k < 2; k++) {
        if (!v[k]) continue;
        if (!first) puts_(o, " ");
        first = 0;
        nl_999(o, v[k], 1);
        puts_(o, big[k]);
    }
    if (!r) return;
    if (!first) puts_(o, " ");
    if (th) { if (th > 1) nl_999(o, th, 0); puts_(o, "duizend"); if (u) puts_(o, " "); }
    if (u) nl_999(o, u, 1);
}

static void card_in(Out *o, int L, unsigned long long n, int g) {
    switch (L) {
    case ES: es_card(o, n, g); break;
    case PT: pt_card(o, n, g); break;
    case FR: fr_card(o, n, g); break;
    case DE: de_card(o, n, g); break;
    case IT: it_card(o, n, g); break;
    default: nl_card(o, n); break;
    }
}

static void card(Out *o, int L, unsigned long long n, int g) {
    put(o, " ", 1);
    card_in(o, L, n, g);
    put(o, " ", 1);
}

/* a year: German and Dutch say 1100–1999 in hundreds (neunzehnhundertsiebenundachtzig, negentienhonderdzevenentachtig) */
static int hundreds_year(int L, unsigned long long v) { return (L == DE || L == NL) && v >= 1100 && v <= 1999 && v % 1000 >= 100; }
static void year(Out *o, int L, unsigned long long v) {
    if (!hundreds_year(L, v)) { card(o, L, v, G_FULL); return; }
    unsigned c = (unsigned)(v / 100), r = (unsigned)(v % 100);
    put(o, " ", 1);
    if (L == DE) { de_99(o, c, 0, G_FULL); puts_(o, "hundert"); if (r) de_99(o, r, 1, G_FULL); }
    else { nl_99(o, c); puts_(o, "honderd"); if (r) nl_99(o, r); }
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
static const char *const IT_O[11] = { "", "primo", "secondo", "terzo", "quarto", "quinto", "sesto", "settimo", "ottavo",
    "nono", "decimo" };

static int ends(const char *b, size_t k, const char *s) { size_t n = strlen(s); return k >= n && !strcmp(b + k - n, s); }

/* n's cardinal into b (at most cap - 1 bytes) */
static void card_str(int L, unsigned n, char *b, size_t cap) {
    Out t = { 0 };
    card_in(&t, L, n, G_FULL);
    snprintf(b, cap, "%.*s", t.oom ? 0 : (int)t.n, t.p ? t.p : "");
    free(t.p);
}

/* fem: every word's final o made a (primera, vigésima primeira, prima, ventesima); apoc: es "1.er", "3.er" → primer,
 * tercer; de_end: German's ending (dritte, dritten, dritter) */
static void ordinal(Out *o, int L, unsigned n, int fem, int apoc, const char *de_end) {
    char b[120];
    size_t k;
    if (L == FR) {
        /* premier, première; else the cardinal and -ième: cinquième, neuvième, vingt et unième, quatre-vingtième */
        if (n == 1) { word(o, fem ? "première" : "premier"); return; }
        card_str(L, n, b, sizeof b - 8);
        k = strlen(b);
        if (ends(b, k, "cinq")) strcat(b, "u");
        else if (ends(b, k, "neuf")) b[k - 1] = 'v';
        else if (ends(b, k, "vingts") || ends(b, k, "cents")) b[k - 1] = 0;
        else if (k && b[k - 1] == 'e') b[k - 1] = 0;
        strcat(b, "ième");
        word(o, b);
        return;
    }
    if (L == DE) {
        /* erste, dritte, siebte, achte; zweite … neunzehnte; zwanzigste, einundzwanzigste, hundertste */
        static const char *const irr[9] = { "", "erst", "", "dritt", "", "", "", "siebt", "acht" };
        if (n < 9 && *irr[n]) snprintf(b, sizeof b, "%s", irr[n]);
        else {
            card_str(L, n, b, sizeof b - 8);
            strcat(b, n < 20 ? "t" : "st");
        }
        strcat(b, de_end);
        word(o, b);
        return;
    }
    if (L == IT) {
        /* primo … decimo; then the cardinal without its last vowel and -esimo: undicesimo, ventunesimo, ventitreesimo,
         * ventiseiesimo, centesimo */
        if (n <= 10) snprintf(b, sizeof b, "%s", IT_O[n]);
        else {
            card_str(L, n, b, sizeof b - 8);
            k = strlen(b);
            if (ends(b, k, "tré")) { b[k - strlen("tré")] = 0; strcat(b, "tre"); }
            else if (!ends(b, k, "sei")) b[k - 1] = 0;
            strcat(b, "esimo");
        }
        if (fem) b[strlen(b) - 1] = 'a';
        word(o, b);
        return;
    }
    if (L == NL) {
        /* eerste, derde, achtste; tweede … negentiende; twintigste, eenentwintigste, honderdste */
        if (n == 1) snprintf(b, sizeof b, "eerste");
        else if (n == 3) snprintf(b, sizeof b, "derde");
        else if (n == 8) snprintf(b, sizeof b, "achtste");
        else {
            card_str(L, n, b, sizeof b - 8);
            strcat(b, n < 20 ? "de" : "ste");
        }
        word(o, b);
        return;
    }
    if (n == 100) snprintf(b, sizeof b, "centésimo");
    else if (L == ES && n >= 10 && n < 20) snprintf(b, sizeof b, "%s", ES_O1X[n - 10]);
    else {
        const char *const *u = L == ES ? ES_O1 : PT_O1, *const *t = L == ES ? ES_O10 : PT_O10;
        snprintf(b, sizeof b, "%s%s%s", t[n / 10], n >= 10 && n % 10 ? " " : "", u[n % 10]);
    }
    k = strlen(b);
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

/* the word starting at p[k] (letters), lowercased into w; its length in bytes, 0 when none or too long */
static size_t word_at(const unsigned char *p, size_t k, size_t len, char *w, size_t cap) {
    size_t n = 0, l;
    while ((l = letter_at(p, k, len))) {
        if (n + l >= cap) return 0;
        if (l == 1) w[n++] = (char)(p[k] >= 'A' && p[k] <= 'Z' ? p[k] + 32 : p[k]);
        else { w[n++] = (char)p[k]; w[n++] = (char)p[k + 1]; }
        k += l;
    }
    w[n] = 0;
    return n;
}

/* the word after p[j] (one space on) is a noun a count can go before: "21 años" veintiún años, "1 libro" un libro,
 * "1 Buch" ein Buch (German nouns are capitalized); not "el 1 de enero" (uno de enero) */
static const char *const NOT_NOUNS[] = { "y", "e", "o", "u", "de", "del", "a", "al", "en", "por", "para", "con", "sin",
    "que", "es", "son", "fue", "era", "más", "menos", "x", "entre", "hasta", "desde", "sobre", "ni", "se", "no", "como",
    "contra", "según", "está", "están", "veces",
    "et", "ou", "du", "des", "au", "pour", "avec", "sans", "est", "sont", "plus", "moins", "sur", "fois",
    "und", "oder", "bis", "von", "zu", "mit", "ist", "sind", "mal", "plus", "minus", "im", "in", "um",
    "di", "da", "con", "per", "su", "tra", "fra", "è", "sono", "più", "meno", "volte",
    "en", "of", "tot", "van", "met", "is", "zijn", "min", "keer", "op", NULL };
static int noun_follows(int L, const unsigned char *p, size_t j, size_t len) {
    if (j >= len || p[j] != ' ' || !letter_at(p, j + 1, len)) return 0;
    if (L == DE && !(p[j + 1] >= 'A' && p[j + 1] <= 'Z')) return 0;
    char w[24];
    size_t n = word_at(p, j + 1, len, w, sizeof w);
    return !n || !in_list(NOT_NOUNS, w, n);
}

/* a month's name follows p[j] (one space on): "1 janvier", "3. Mai" */
static int month_follows(int L, const unsigned char *p, size_t j, size_t len) {
    if (j >= len || p[j] != ' ') return 0;
    char w[24], m[24];
    size_t n = word_at(p, j + 1, len, w, sizeof w);
    for (int k = 0; n && k < 12; k++) {
        snprintf(m, sizeof m, "%s", LEX[L].month[k]);
        if (m[0] >= 'A' && m[0] <= 'Z') m[0] = (char)(m[0] + 32);
        if (!strcmp(w, m)) return 1;
    }
    return 0;
}

/* the word just before p[i] (a space, then letters), lowercased into w; its length, 0 when none */
static size_t word_before(const unsigned char *p, size_t i, char *w, size_t cap) {
    if (!i || p[i - 1] != ' ') return 0;
    size_t e = i - 1, a = e;
    while (a > 0 && is_ascii_letter(p[a - 1])) a--;
    if (a == e || e - a >= cap || (a > 0 && p[a - 1] >= 0x80)) return 0;
    for (size_t k = a; k < e; k++) w[k - a] = (char)(p[k] >= 'A' && p[k] <= 'Z' ? p[k] + 32 : p[k]);
    w[e - a] = 0;
    return e - a;
}

/* German ordinals are written "3." : the ending the word before asks for (am dritten, der dritte), else -er (a date:
 * "3. Mai" dritter Mai); NULL when nothing before makes it an ordinal */
static const char *de_ordinal_end(const unsigned char *p, size_t i) {
    static const char *const en[] = { "am", "im", "vom", "zum", "beim", "ab", "seit", "den", "dem", "des", "zur", NULL };
    static const char *const e[] = { "der", "die", "das", NULL };
    char w[8];
    size_t n = word_before(p, i, w, sizeof w);
    if (n && in_list(en, w, n)) return "en";
    if (n && in_list(e, w, n)) return "e";
    return NULL;
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
    const Lex *x = &LEX[L];
    int g = cu->fem[L] ? G_FEM : L == ES || L == DE || L == IT ? G_APOC : G_FULL;
    const char *one = cu->name[L][0], *many = cu->name[L][1];
    if (c == CUR_DOLLAR && o->peso) { one = PESO[L][0]; many = PESO[L][1]; }
    unsigned long long v = value(m->ip, m->in);
    if (m->kind == 1 && m->fn == 2 && num_ok(m)) {               /* 25,50 € veinticinco euros con cincuenta */
        card(o, L, v, g);
        word(o, v == 1 ? one : many);
        unsigned long long cents = value(m->f, 2);
        if (cents) { word(o, x->cents); card(o, L, cents, G_FULL); }
    } else if (m->kind == 0 && num_ok(m)) {
        card(o, L, v, g);
        if (v >= 1000000 && v % 1000000 == 0 && *x->million_of) {  /* un millón de euros, un million d'euros */
            if (L == FR && strchr("aeiouéh", many[0])) { puts_(o, " d'"); puts_(o, many); put(o, " ", 1); return; }
            word(o, x->million_of);
        }
        word(o, v == 1 ? one : many);
    } else { plain(o, L, m, G_FULL); word(o, many); }
}

/* "14:30", "14:30:15", "14h30" (and French "14 h 30", German "14.30 Uhr"): hours 0–24 in one or two digits; returns
 * the end, 0 when it isn't a time */
static size_t time_at(int L, const unsigned char *p, size_t i, size_t len, unsigned *h, unsigned *mi, unsigned *s, int *has_s) {
    size_t n = digit_run(p, i, len), j = i + n, sl;
    if (n < 1 || n > 2 || j >= len) return 0;
    *h = (unsigned)value(p + i, n);
    *mi = *s = 0;
    *has_s = 0;
    int colon = p[j] == ':';
    if (colon || p[j] == 'h') sl = 1;
    else if (L == FR && has_at(p, j, len, " h ")) sl = 3;
    else if (L == DE && p[j] == '.' && digit_run(p, j + 1, len) == 2 && has_at(p, j + 3, len, " Uhr") && !letter_at(p, j + 7, len)) sl = 1;
    else return 0;
    if (*h > 24 || digit_run(p, j + sl, len) != 2) return 0;
    *mi = (unsigned)value(p + j + sl, 2);
    if (*mi > 59) return 0;
    j += sl + 2;
    if (colon && j < len && p[j] == ':' && digit_run(p, j + 1, len) == 2 && value(p + j + 1, 2) < 60) {
        *s = (unsigned)value(p + j + 1, 2);
        *has_s = 1;
        j += 3;
    }
    if (!colon && has_at(p, j, len, "min")) j += 3;
    if (letter_at(p, j, len) || (j < len && (p[j] == ':' || p[j] == '.' || p[j] == ',') && isd(p, j + 1, len))) return 0;
    return j;
}

/* after a time or hours: "h", " h", "hs", "hrs" are taken; a following hour word (horas, hora, heures, Uhr, ore, uur)
 * is left and noted, *word_end being where it ends */
static size_t hours_mark(int L, const unsigned char *p, size_t j, size_t len, int *word_follows, size_t *word_end) {
    size_t k = j + (space_at(p, j, len) == 1);
    static const char *const marks[] = { "hrs", "hr", "hs", "h", NULL };
    *word_follows = 0;
    for (int t = 0; k > j && t < 3 && LEX[L].hour_words[t]; t++) {
        const char *hw = LEX[L].hour_words[t];
        size_t e = k + strlen(hw);
        if (has_at(p, k, len, hw) && !letter_at(p, e, len)) { *word_follows = 1; *word_end = e; return j; }
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

/* a date, day first: es quince de noviembre de dos mil veintiséis, fr quinze novembre deux mille vingt-six (premier),
 * de fünfzehnter November (am fünfzehnten), it quindici novembre (primo), nl vijftien november */
static void date(Out *o, int L, const unsigned char *p, size_t i, unsigned d, unsigned mo, unsigned long long y) {
    const Lex *x = &LEX[L];
    if (L == DE) { const char *e = de_ordinal_end(p, i); ordinal(o, L, d, 0, 0, e ? e : "er"); }
    else if (d == 1 && L != NL) word(o, x->first_day);
    else card(o, L, d, G_FULL);
    if (L == ES || L == PT) word(o, x->of);
    word(o, x->month[mo - 1]);
    if (L == ES || L == PT) word(o, x->of);
    year(o, L, y);
}

/* the number at p[i] (a digit), with what follows it that belongs to it; in_word: right after letters ("MP3",
 * "H264"); cur: a currency symbol read before it, else -1. Returns where the text goes on. */
static size_t number(Out *o, int L, const unsigned char *p, size_t i, size_t len, int in_word, int cur) {
    const Lex *x = &LEX[L];
    size_t n0 = digit_run(p, i, len), j = i + n0;

    if (!in_word && cur < 0) {
        unsigned h, mi, s, d, mo;
        int has_s, word_follows;
        size_t we = 0;
        size_t e = time_at(L, p, i, len, &h, &mi, &s, &has_s);
        if (e) {
            e = hours_mark(L, p, e, len, &word_follows, &we);
            if (has_s) {                                  /* 1:02:03 una hora, dos minutos y tres segundos */
                card(o, L, h, x->g_hour);
                word(o, h == 1 ? x->hour : x->hours);
                if (mi) { put(o, ",", 1); card(o, L, mi, x->g_min); word(o, mi == 1 ? x->minute : x->minutes); }
                if (s) { word(o, x->and_); card(o, L, s, x->g_min); word(o, s == 1 ? x->second : x->seconds); }
            } else if (x->clock) {                        /* quatorze heures trente, vierzehn Uhr dreißig */
                card(o, L, h, x->g_clock);
                word(o, h <= 1 ? x->clock1 : x->clock);
                if (mi) card(o, L, mi, G_FULL);
                if (word_follows) e = we;                 /* 14:30 Uhr: the word is said once */
            } else {
                card(o, L, h, x->g_clock);
                if (mi) { word(o, x->and_); card(o, L, mi, G_FULL); }
                else if (!word_follows && x->whole_hours) word(o, h == 1 ? x->hour : x->hours);
            }
            return e;
        }
        const unsigned char *y;
        size_t yn;
        e = date_at(p, i, len, &d, &mo, &y, &yn);
        if (e) { date(o, L, p, i, d, mo, value(y, yn)); return e; }
        /* "4h", "48 hs": hours */
        if (n0 <= 3 && !(n0 > 1 && p[i] == '0')) {
            size_t e2 = hours_mark(L, p, j, len, &word_follows, &we);
            if (e2 > j) {
                unsigned long long v = value(p + i, n0);
                card(o, L, v, x->g_hour);
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
        plain(o, L, &m, x->g_degree);
        word(o, m.kind == 0 && v == 1 && ok ? x->degree : x->degrees);
        if (scale) word(o, scale);
        return e;
    }
    /* ordinals: es, pt, it 1.º 1.ª 1º 1ª (and es 1.er 3.er); fr 1er 1re 1ère 2e 2ème 2nd 2nde 2ᵉ; nl 1e 2de 8ste;
     * de 3. after an article or before a month (am 3. Mai) */
    if (m.kind == 0 && m.ng == 1 && ok && v >= 1 && !in_word) {
        size_t e = 0;
        int fem = 0, er = 0;
        if (L == ES || L == PT || L == IT) {
            size_t a = j + (j < len && p[j] == '.');
            int masc = has_at(p, a, len, "\xc2\xba");
            fem = has_at(p, a, len, "\xc2\xaa");
            er = L == ES && has_at(p, a, len, "er") && (v % 10 == 1 || v % 10 == 3) && v % 100 != 11 && v % 100 != 13;
            if (fem || masc || er) e = a + 2;
        } else if (L == FR) {
            static const char *const suf[] = { "\xe1\xb5\x89", "ème", "ième", "è", "e", NULL };   /* ᵉ */
            if (v == 1 && (has_at(p, j, len, "ère") || has_at(p, j, len, "re"))) { fem = 1; e = j + (p[j] == 'r' ? 2 : 4); }
            else if (v == 1 && has_at(p, j, len, "er")) e = j + 2;
            else if (v == 2 && (has_at(p, j, len, "nde") || has_at(p, j, len, "nd"))) {
                e = j + (has_at(p, j, len, "nde") ? 3 : 2);
                if (!letter_at(p, e, len)) { word(o, e - j == 3 ? "seconde" : "second"); return e; }
                e = 0;
            } else
                for (int t = 0; v >= 2 && suf[t] && !e; t++) if (has_at(p, j, len, suf[t])) e = j + strlen(suf[t]);
        } else if (L == NL) {
            static const char *const suf[] = { "ste", "de", "e", NULL };
            for (int t = 0; suf[t] && !e; t++) if (has_at(p, j, len, suf[t])) e = j + strlen(suf[t]);
        } else if (L == DE && v <= 31 && j + 1 < len && p[j] == '.' && p[j + 1] == ' ') {
            const char *end = de_ordinal_end(p, i);
            if (end || month_follows(L, p, j + 1, len)) { ordinal(o, L, (unsigned)v, 0, 0, end ? end : "er"); return j + 1; }
        }
        if (e && !letter_at(p, e, len) && !isd(p, e, len)) {
            if (v <= 100) ordinal(o, L, (unsigned)v, fem, er, "");
            else card(o, L, v, fem ? G_FEM : G_FULL);
            return e;
        }
    }
    /* fr, it: "1 janvier", "1 maggio" premier janvier, primo maggio */
    if ((L == FR || L == IT) && m.kind == 0 && m.ng == 1 && v == 1 && !in_word && month_follows(L, p, j, len)) {
        word(o, x->first_day);
        return j;
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
    /* de, nl: a year in hundreds (1987 neunzehnhundertsiebenundachtzig) */
    if (m.kind == 0 && m.ng == 1 && n0 == 4 && ok && !in_word && hundreds_year(L, v)) year(o, L, v);
    else plain(o, L, &m, x->g_count != G_FULL && m.kind == 0 && ok && v < 1000 && !in_word && noun_follows(L, p, j, len) ? G_APOC : G_FULL);
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

/* -- abbreviations (French, German, Italian, Dutch): as written, at the start of a word, not followed by a letter or a
 * digit. ends: 1, it can end a sentence (usw., etc.): its period is kept for one when a capital or the end follows;
 * 2, only before a name (M. Dupont: a space and a capital). -- */
typedef struct { const char *ab, *full; unsigned char ends; } Abbr;
static const Abbr ABBR_FR[] = {
    { "av. J.-C.", "avant Jésus-Christ", 1 }, { "apr. J.-C.", "après Jésus-Christ", 1 }, { "c.-à-d.", "c'est-à-dire", 0 },
    { "p. ex.", "par exemple", 0 }, { "etc.", "et cetera", 1 }, { "env.", "environ", 0 }, { "MM.", "messieurs", 2 },
    { "M.", "monsieur", 2 },
    { "Mmes", "mesdames", 0 }, { "Mme", "madame", 0 }, { "Mlles", "mesdemoiselles", 0 }, { "Mlle", "mademoiselle", 0 },
    { "Dr.", "docteur", 0 }, { "Dr", "docteur", 0 }, { "Pr", "professeur", 0 }, { NULL, NULL, 0 } };
static const Abbr ABBR_DE[] = {
    { "z. B.", "zum Beispiel", 0 }, { "z.B.", "zum Beispiel", 0 }, { "d. h.", "das heißt", 0 }, { "d.h.", "das heißt", 0 },
    { "u. a.", "unter anderem", 0 }, { "u.a.", "unter anderem", 0 }, { "usw.", "und so weiter", 1 },
    { "bzw.", "beziehungsweise", 0 }, { "ca.", "circa", 0 }, { "Nr.", "Nummer", 0 }, { "Dr.", "Doktor", 0 },
    { "Prof.", "Professor", 0 }, { "evtl.", "eventuell", 1 }, { "ggf.", "gegebenenfalls", 1 }, { "inkl.", "inklusive", 0 },
    { "vgl.", "vergleiche", 0 }, { "bzgl.", "bezüglich", 0 }, { "Str.", "Straße", 1 }, { "etc.", "et cetera", 1 },
    { NULL, NULL, 0 } };
static const Abbr ABBR_IT[] = {
    { "Sig.ra", "signora", 0 }, { "Sig.na", "signorina", 0 }, { "Sig.", "signor", 0 }, { "Dott.ssa", "dottoressa", 0 },
    { "Dott.", "dottor", 0 }, { "Prof.ssa", "professoressa", 0 }, { "Prof.", "professor", 0 }, { "ecc.", "eccetera", 1 },
    { "pag.", "pagina", 0 }, { "p.es.", "per esempio", 0 }, { "ad es.", "ad esempio", 0 }, { "tel.", "telefono", 0 },
    { "etc.", "eccetera", 1 }, { NULL, NULL, 0 } };
static const Abbr ABBR_NL[] = {
    { "bijv.", "bijvoorbeeld", 0 }, { "d.w.z.", "dat wil zeggen", 0 }, { "enz.", "enzovoort", 1 },
    { "o.a.", "onder andere", 1 }, { "m.b.t.", "met betrekking tot", 0 }, { "i.p.v.", "in plaats van", 0 },
    { "nr.", "nummer", 0 }, { "Nr.", "nummer", 0 }, { "ca.", "circa", 0 }, { "dhr.", "de heer", 0 },
    { "mevr.", "mevrouw", 0 }, { "blz.", "bladzijde", 0 }, { "incl.", "inclusief", 0 }, { "t/m", "tot en met", 0 },
    { "etc.", "et cetera", 1 }, { NULL, NULL, 0 } };

/* the abbreviation at p[i], written out; returns its length, 0 when none */
static size_t abbreviation(Out *o, int L, const unsigned char *p, size_t i, size_t len) {
    const Abbr *a = L == FR ? ABBR_FR : L == DE ? ABBR_DE : L == IT ? ABBR_IT : L == NL ? ABBR_NL : NULL;
    for (; a && a->ab; a++) {
        size_t n = strlen(a->ab);
        if (!has_at(p, i, len, a->ab) || letter_at(p, i + n, len) || isd(p, i + n, len)) continue;
        size_t k = i + n;
        if (a->ends == 2) {
            if (k + 1 < len && p[k] == ' ' && p[k + 1] >= 'A' && p[k + 1] <= 'Z') { word(o, a->full); return n; }
            continue;
        }
        word(o, a->full);
        if (a->ab[n - 1] == '.' && (k == len || p[k] == '\n' ||
                                    (a->ends && k + 1 < len && p[k] == ' ' && p[k + 1] >= 'A' && p[k + 1] <= 'Z')))
            put(o, ".", 1);
        return n;
    }
    return 0;
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
    static const char *const langs[6] = { "es", "pt", "fr", "de", "it", "nl" };
    Out o = { 0 };
    o.peso = peso_region(region);
    int L = -1;
    for (int k = 0; lang && k < 6; k++) if (!strcmp(lang, langs[k])) L = k;
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
        if (c >= '0' && c <= '9') {
            size_t at = o.n;
            int elided = (i >= 2 && p[i - 1] == '\'' && letter_at(p, i - 2, len)) ||    /* l'11e: no space after l' */
                         (i >= 4 && has_at(p, i - 3, len, "\xe2\x80\x99") && letter_at(p, i - 4, len));
            i = number(&o, L, p, i, len, !bnd, -1);
            if (elided && at < o.n && o.p[at] == ' ') { memmove(o.p + at, o.p + at + 1, o.n - at - 1); o.n--; }
            continue;
        }
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
            size_t ab = is_ascii_letter(c) ? abbreviation(&o, L, p, i, len) : 0;
            if (ab) { i += ab; continue; }
            /* nº 5, n.º 5, N° 5 */
            if (c == 'n' || c == 'N') {
                size_t k = i + 1 + (i + 1 < len && p[i + 1] == '.');
                if ((has_at(p, k, len, "\xc2\xba") || has_at(p, k, len, "\xc2\xb0")) && isd(p, k + 2 + space_at(p, k + 2, len), len)) {
                    word(&o, x->number);
                    i = k + 2;
                    continue;
                }
                /* it n. 5 */
                if (L == IT && i + 1 < len && p[i + 1] == '.' && isd(p, i + 2 + space_at(p, i + 2, len), len)) {
                    word(&o, x->number);
                    i += 2;
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
