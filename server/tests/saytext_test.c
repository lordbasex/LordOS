/* saytext_test.c — say's text (src/tts/saytext.c): cleaning terminal output and splitting into sentences. */
#include "tts/saytext.h"

#include <stdio.h>
#include <string.h>

static int n, bad;
static void eqs(const char *what, const char *got, size_t glen, const char *want) {
    n++;
    if (glen != strlen(want) || memcmp(got, want, glen)) { bad++; printf("✗ %s\n    got  [%.*s]\n    want [%s]\n", what, (int)glen, got, want); }
}

static void clean(const char *what, const char *in, const char *want) {
    char b[512];
    size_t l = strlen(in);
    memcpy(b, in, l + 1);
    size_t o = say_clean(b, l);
    eqs(what, b, o, want);
}

/* every piece, joined by "|" */
static void split(const char *what, const char *in, size_t max, const char *want) {
    char out[1024] = "";
    size_t len = strlen(in), off = 0, o = 0, skip;
    for (int guard = 0; guard < 100; guard++) {
        size_t k = say_next(in + off, len - off, max, &skip);
        if (!k) break;
        if (o) out[o++] = '|';
        memcpy(out + o, in + off + skip, k);
        o += k;
        off += skip + k;
    }
    eqs(what, out, o, want);
}

static void lang(const char *what, const char *in, const char *fallback, const char *want) {
    const char *got = say_language(in, strlen(in), fallback);
    eqs(what, got, strlen(got), want);
}

int main(void) {
    clean("colors out", "\x1b[1;32mlordbasex\x1b[0m@host:~$ ls", "lordbasex@host:~$ ls");
    clean("a window title out", "\x1b]0;title\x07hola", "hola");
    clean("spaces and tabs", "  uno \t  dos   tres ", "uno dos tres");
    clean("blank lines kept as one", "uno\n\n\n\ndos\ntres", "uno\n\ndos\ntres");
    clean("control characters out", "a\x01" "b\x7f" "c\rd", "abc d");
    clean("accents kept", "año, niño, acción", "año, niño, acción");
    clean("invalid UTF-8 out", "a\xff" "b\xc3" "c", "abc");
    clean("C1 controls out", "a\xc2\x9b" "b", "ab");
    split("sentences", "Hola. ¿Cómo estás? ¡Bien! Chau…", 200, "Hola.|¿Cómo estás?|¡Bien!|Chau…");
    split("a number is not a sentence end", "Versión 0.8.8.6 lista. Sí.", 200, "Versión 0.8.8.6 lista.|Sí.");
    split("blank lines", "Título\n\nPrimer párrafo", 200, "Título|Primer párrafo");
    split("a long sentence at a comma", "uno dos tres cuatro, cinco seis siete ocho nueve diez", 30, "uno dos tres cuatro,|cinco seis siete ocho nueve|diez");
    split("one long word on a character boundary", "ñññññññññññ", 9, "ññññ|ññññ|ñññ");
    split("nothing but space", "  \n ", 200, "");
    lang("Spanish", "Hola, ¿cómo estás? Esta es una prueba de la voz.", "en", "es");
    lang("English", "The quick brown fox jumps over the lazy dog and runs to the river.", "es", "en");
    lang("Portuguese", "Olá, tudo bem? Esta é uma prova da voz, não é muito difícil.", "es", "pt");
    lang("Portuguese by its letters", "A configuração do sistema está pronta para você.", "en", "pt");
    lang("Spanish over a Portuguese fallback", "El sistema está listo para que lo uses también.", "pt", "es");
    lang("English in capitals", "THE FILE IS NOT IN THE FOLDER", "es", "en");
    lang("too little to tell", "OK", "es", "es");
    lang("a command keeps the fallback", "make -C server test 2>&1 | tail", "pt", "pt");
    lang("empty", "", "en", "en");
    lang("a language without voices", "OK", "fr", "en");
    lang("terminal output in English", "Error: the file was not found in the folder and it can not be opened", "es", "en");
    printf("saytext_test: %d/%d\n", n - bad, n);
    return bad != 0;
}
