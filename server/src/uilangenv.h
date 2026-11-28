/* uilangenv.h — the interface language for the Linux apps LordOS launches (GitHub #262).
 *
 * The desktop's language is settings.json's, not the session's LANG: a person who installed with "C.UTF-8" and chose
 * French in Settings saw Brave's dialogs and menus in English. When the SDK sets the interface language
 * (lord.text.setLanguage), lord-server puts it in its own environment, which the apps it launches and the Terminal's
 * shells inherit:
 *
 *   LANGUAGE      the language as gettext, Chromium and Qt read it ("fr", "pt_BR:pt", "zh_CN"); GNU gettext only
 *                 ignores it under the plain "C" locale, which a session never has (C.UTF-8 at least)
 *   LC_MESSAGES   only when the session's own messages locale is another language, and only to a UTF-8 locale of
 *                 that language that is generated (the caller checks with newlocale): the region of the session's
 *                 LANG first ("es" with LANG=es_MX → es_MX), then the language's own region. When none is
 *                 generated nothing is set, so an app never gets a locale it can't load.
 *
 * Pure functions, tested by tests/uilangenv_test.c; js_sys.c applies them. LANG and the other LC_* (formats, the
 * region chosen in Settings) are never changed here. */
#ifndef LORD_UILANGENV_H
#define LORD_UILANGENV_H

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* LordOS's language id → gettext's name and the region its texts are written for (assets/locales/<id>/meta.json) */
static const struct { const char *id, *gettext, *region; } uilang_table[] = {
    { "pt", "pt_BR:pt", "pt_BR" }, { "es", "es", "es_AR" }, { "en", "en", "en_US" },
    { "zh-Hans", "zh_CN", "zh_CN" }, { "zh-Hant", "zh_TW", "zh_TW" },
    { "ja", "ja", "ja_JP" }, { "ko", "ko", "ko_KR" }, { "ar", "ar", "ar_EG" }, { "fa", "fa", "fa_IR" },
    { "ur", "ur", "ur_PK" }, { "hi", "hi", "hi_IN" }, { "bn", "bn", "bn_BD" }, { "sv", "sv", "sv_SE" },
    { "el", "el", "el_GR" }, { "uk", "uk", "uk_UA" }, { "ca", "ca", "ca_ES" }, { "fil", "fil", "fil_PH" },
    { "sw", "sw", "sw_KE" }, { "ms", "ms", "ms_MY" }, { "vi", "vi", "vi_VN" }, { "da", "da", "da_DK" },
    { "cs", "cs", "cs_CZ" }, { "et", "et", "et_EE" }, { "he", "he", "he_IL" }, { "nb", "nb", "nb_NO" },
};

/* A language id as the SDK sends it: letters, '-' and '_', 1–15 characters (f_text_language checks the same) */
static inline int uilang_valid(const char *l) {
    size_t n = l ? strlen(l) : 0;
    if (!n || n > 15) return 0;
    for (const char *p = l; *p; p++) if (!isalpha((unsigned char)*p) && *p != '-' && *p != '_') return 0;
    return 1;
}

/* The language part of a locale or language id: "pt_BR.UTF-8" → "pt", "zh-Hans" → "zh", "C.UTF-8" → "C" */
static inline void uilang_base(const char *s, char *out, size_t n) {
    size_t i = 0;
    for (; s && s[i] && !strchr("_.@-:", s[i]) && i + 1 < n; i++) out[i] = (char)tolower((unsigned char)s[i]);
    out[i] = 0;
    if (s && (!strcmp(s, "C") || !strncmp(s, "C.", 2) || !strcmp(s, "POSIX"))) snprintf(out, n, "c");
}

/* LANGUAGE's value for a language id ("fr" → "fr", "pt" → "pt_BR:pt", "zh-Hant" → "zh_TW"); 0 if the id isn't valid */
static inline int uilang_language(const char *lang, char *out, size_t n) {
    if (!uilang_valid(lang)) return 0;
    for (size_t i = 0; i < sizeof uilang_table / sizeof uilang_table[0]; i++)
        if (!strcmp(uilang_table[i].id, lang)) { snprintf(out, n, "%s", uilang_table[i].gettext); return 1; }
    size_t i = 0;
    for (; lang[i] && i + 1 < n; i++) out[i] = lang[i] == '-' ? '_' : lang[i];
    out[i] = 0;
    return 1;
}

/* Up to max UTF-8 locale names to try for LC_MESSAGES, best first: the region of the session's LANG with this
 * language, then the language's own region ("fr" → fr_FR.UTF-8). Returns how many (0: the messages locale is
 * already this language, or the id isn't valid, or it is "en" under C, where messages are English anyway). */
static inline int uilang_candidates(const char *lang, const char *env_lang, const char *env_messages,
                                    char out[][32], int max) {
    if (!uilang_valid(lang) || max < 1) return 0;
    char want[16], cur[16];
    uilang_base(lang, want, sizeof want);
    uilang_base(env_messages && *env_messages ? env_messages : env_lang, cur, sizeof cur);
    if (!strcmp(want, cur) || (!strcmp(want, "en") && (!cur[0] || !strcmp(cur, "c")))) return 0;
    const char *own = NULL;
    for (size_t i = 0; i < sizeof uilang_table / sizeof uilang_table[0]; i++) if (!strcmp(uilang_table[i].id, lang)) own = uilang_table[i].region;
    char guess[32];
    if (!own) {
        char up[8];
        size_t k = 0;
        for (; want[k] && k + 1 < sizeof up; k++) up[k] = (char)toupper((unsigned char)want[k]);
        up[k] = 0;
        snprintf(guess, sizeof guess, "%s_%s", want, up);
        own = guess;
    }
    int n = 0;
    /* the session's region with this language: LANG=es_MX.UTF-8 and Spanish → es_MX (not for a script variant:
     * zh-Hans under zh_TW stays zh_CN) */
    const char *us = env_lang && !strchr(lang, '-') ? strchr(env_lang, '_') : NULL;
    if (us && isupper((unsigned char)us[1]) && isupper((unsigned char)us[2]) && (!us[3] || strchr(".@", us[3]))) {
        snprintf(out[n], 32, "%s_%c%c.UTF-8", want, us[1], us[2]);
        if (strncmp(out[n], own, strlen(own)) || out[n][strlen(own)] != '.') n++;
    }
    if (n < max) snprintf(out[n++], 32, "%s.UTF-8", own);
    return n;
}

#endif
