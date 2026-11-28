/* uilangenv_test.c — the interface language for launched Linux apps (src/uilangenv.h, GitHub #262): LANGUAGE's value
 * and the LC_MESSAGES locales to try, best first, without ever touching what the session already has right. */
#include "../src/uilangenv.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int lang_is(const char *lang, const char *want) {
    char v[32];
    return uilang_language(lang, v, sizeof v) && !strcmp(v, want);
}

/* the candidates joined with spaces */
static const char *cands(const char *lang, const char *env_lang, const char *env_msgs) {
    static char out[160];
    char c[2][32];
    int n = uilang_candidates(lang, env_lang, env_msgs, c, 2);
    out[0] = 0;
    for (int i = 0; i < n; i++) { if (i) strcat(out, " "); strcat(out, c[i]); }
    return out;
}

int main(void) {
    /* LANGUAGE */
    CHECK(lang_is("fr", "fr"));
    CHECK(lang_is("pt", "pt_BR:pt"));
    CHECK(lang_is("zh-Hans", "zh_CN"));
    CHECK(lang_is("zh-Hant", "zh_TW"));
    CHECK(lang_is("en", "en"));
    CHECK(lang_is("xx-Yy", "xx_Yy"));                   /* a language pack's id: gettext's spelling */
    char v[32];
    CHECK(!uilang_language("", v, sizeof v));
    CHECK(!uilang_language("fr;rm", v, sizeof v));
    CHECK(!uilang_language("fr:de", v, sizeof v));
    CHECK(!uilang_language("abcdefghijklmnop", v, sizeof v));

    /* #262: a session in C.UTF-8 with French chosen → fr_FR */
    CHECK(!strcmp(cands("fr", "C.UTF-8", NULL), "fr_FR.UTF-8"));
    CHECK(!strcmp(cands("fr", NULL, NULL), "fr_FR.UTF-8"));
    /* the session's region with the chosen language first */
    CHECK(!strcmp(cands("es", "en_US.UTF-8", NULL), "es_US.UTF-8 es_AR.UTF-8"));
    CHECK(!strcmp(cands("fr", "de_CH.UTF-8", ""), "fr_CH.UTF-8 fr_FR.UTF-8"));
    CHECK(!strcmp(cands("pt", "en_GB.UTF-8", NULL), "pt_GB.UTF-8 pt_BR.UTF-8"));
    CHECK(!strcmp(cands("de", "fr_DE.UTF-8", NULL), "de_DE.UTF-8"));      /* the same as its own: once */
    /* a script variant never takes the session's region */
    CHECK(!strcmp(cands("zh-Hans", "en_TW.UTF-8", NULL), "zh_CN.UTF-8"));
    /* already that language: nothing to set */
    CHECK(!strcmp(cands("es", "es_MX.UTF-8", NULL), ""));
    CHECK(!strcmp(cands("fr", "C.UTF-8", "fr_CA.UTF-8"), ""));
    CHECK(!strcmp(cands("zh-Hant", "zh_TW.UTF-8", NULL), ""));
    /* English under C: messages are English anyway */
    CHECK(!strcmp(cands("en", "C.UTF-8", NULL), ""));
    CHECK(!strcmp(cands("en", "fr_FR.UTF-8", NULL), "en_FR.UTF-8 en_US.UTF-8"));
    /* LC_MESSAGES in another language than LANG: it is what counts */
    CHECK(!strcmp(cands("fr", "fr_FR.UTF-8", "de_DE.UTF-8"), "fr_FR.UTF-8"));
    /* an unknown language: its own ll_LL */
    CHECK(!strcmp(cands("lt", "C.UTF-8", NULL), "lt_LT.UTF-8"));
    /* a LANG without a proper region is not used */
    CHECK(!strcmp(cands("fr", "en.UTF-8", NULL), "fr_FR.UTF-8"));
    CHECK(!strcmp(cands("bad id", "C", NULL), ""));
    if (fails) { fprintf(stderr, "uilangenv-test: %d failures\n", fails); return 1; }
    printf("uilangenv-test: ok\n");
    return 0;
}
