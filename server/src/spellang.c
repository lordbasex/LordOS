/* spellang.c — see spellang.h. */
#include "spellang.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* a language's usual region, for one the locale doesn't name */
static const char *usual_region(const char *lang) {
    static const char *const T[][2] = { { "en", "US" }, { "es", "ES" }, { "pt", "BR" }, { "fr", "FR" }, { "de", "DE" }, { "it", "IT" },
                                        { "nl", "NL" }, { "sv", "SE" }, { "da", "DK" }, { "ca", "ES" }, { "el", "GR" }, { "uk", "UA" } };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(T[i][0], lang)) return T[i][1];
    return NULL;
}

static int has(const char *const *inst, int n, const char *tag) {
    for (int i = 0; i < n; i++) if (inst[i] && !strcmp(inst[i], tag)) return 1;
    return 0;
}

/* lang's dictionary (region first) into out; 1 if one was found */
static int pick(const char *lang, const char *region, const char *const *inst, int n, char out[SPELLANG_TAG_MAX]) {
    char t[SPELLANG_TAG_MAX];
    const char *usual = usual_region(lang);
    if (region && region[0]) { snprintf(t, sizeof t, "%s_%s", lang, region); if (has(inst, n, t)) { snprintf(out, SPELLANG_TAG_MAX, "%s", t); return 1; } }
    if (usual) { snprintf(t, sizeof t, "%s_%s", lang, usual); if (has(inst, n, t)) { snprintf(out, SPELLANG_TAG_MAX, "%s", t); return 1; } }
    if (has(inst, n, lang)) { snprintf(out, SPELLANG_TAG_MAX, "%s", lang); return 1; }
    size_t l = strlen(lang);
    for (int i = 0; i < n; i++)
        if (inst[i] && !strncmp(inst[i], lang, l) && inst[i][l] == '_') { snprintf(out, SPELLANG_TAG_MAX, "%s", inst[i]); return 1; }
    return 0;
}

int spellang_choose(const char *locale, const char *const *installed, int n, char out[][SPELLANG_TAG_MAX], int max) {
    char lang[4] = "", region[3] = "";
    const char *s = locale ? locale : "";
    size_t l = 0;
    while (l < 3 && s[l] >= 'a' && s[l] <= 'z') l++;
    if (l >= 2 && (!s[l] || s[l] == '_' || s[l] == '-' || s[l] == '.' || s[l] == '@')) {
        memcpy(lang, s, l);
        lang[l] = 0;
        if ((s[l] == '_' || s[l] == '-') && s[l + 1] >= 'A' && s[l + 1] <= 'Z' && s[l + 2] >= 'A' && s[l + 2] <= 'Z' &&
            (!s[l + 3] || s[l + 3] == '.' || s[l + 3] == '@')) { region[0] = s[l + 1]; region[1] = s[l + 2]; region[2] = 0; }
    } else snprintf(lang, sizeof lang, "en");
    int k = 0;
    if (max > 0 && pick(lang, region, installed, n, out[k])) k++;
    if (k < max && strcmp(lang, "en") && pick("en", NULL, installed, n, out[k])) k++;
    return k;
}

static int by_name(const void *a, const void *b) { return strcmp(a, b); }

int spellang_installed(const char *dir, char out[][SPELLANG_TAG_MAX], int max) {
    DIR *d = dir ? opendir(dir) : NULL;
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || l - 4 >= SPELLANG_TAG_MAX || strcmp(e->d_name + l - 4, ".dic") || e->d_name[0] == '.') continue;
        char aff[512];
        snprintf(aff, sizeof aff, "%s/%.*s.aff", dir, (int)(l - 4), e->d_name);
        if (access(aff, R_OK)) continue;
        memcpy(out[n], e->d_name, l - 4);
        out[n][l - 4] = 0;
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof out[0], by_name);
    return n;
}
