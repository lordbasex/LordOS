/* weblangid.h — the catalog folder (assets/locales/<id>) for a glibc locale value, pure (tests/webapp_test.c).
 *
 * The language part of LC_ALL, LC_MESSAGES or LANG has two or three letters ("pt_BR.UTF-8" → "pt", "fil_PH.UTF-8" →
 * "fil"); Chinese goes to the script its country writes, as sdk/localename.js languageOfLocale does ("zh_TW", "zh_HK",
 * "zh_MO" → "zh-Hant", any other "zh" → "zh-Hans"). Anything else ("C.UTF-8", "POSIX", "", "Fil_PH") is "en". */
#ifndef LORD_WEB_WEBLANGID_H
#define LORD_WEB_WEBLANGID_H

#include <stdio.h>
#include <string.h>

#define WEBLANG_ID_MAX 8                /* "zh-Hant" and its NUL */

static inline void weblang_id(const char *l, char out[WEBLANG_ID_MAX]) {
    snprintf(out, WEBLANG_ID_MAX, "en");
    if (!l) return;
    size_t n = 0;
    while (n < 4 && l[n] >= 'a' && l[n] <= 'z') n++;
    if (n < 2 || n > 3 || (l[n] && l[n] != '_' && l[n] != '.' && l[n] != '@')) return;
    if (n == 2 && !strncmp(l, "zh", 2)) {
        const char *cc = l[2] == '_' ? l + 3 : "";
        int trad = !strncmp(cc, "TW", 2) || !strncmp(cc, "HK", 2) || !strncmp(cc, "MO", 2);
        snprintf(out, WEBLANG_ID_MAX, "%s", trad ? "zh-Hant" : "zh-Hans");
        return;
    }
    snprintf(out, WEBLANG_ID_MAX, "%.*s", (int)n, l);
}

#endif
