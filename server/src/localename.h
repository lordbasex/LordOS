/* localename.h — is a region one that glibc supports in UTF-8 (/usr/share/i18n/SUPPORTED)?
 *
 * SUPPORTED names its UTF-8 regions in two ways: most with the codeset ("en_GB.UTF-8 UTF-8"), but some
 * 175 without it ("en_IN UTF-8", "ca_ES@valencia UTF-8"). The installer and Settings always send the
 * codeset, before the modifier as glibc writes it: "en_IN.UTF-8", "ca_ES.UTF-8@valencia"
 * (sdk/localename.js localeOf). Both spellings of a listed region are accepted; locale-gen makes
 * either from the same source. Used by admin.c (locale-gen, system-locale) and install.c (install-run). */
#ifndef LORD_LOCALENAME_H
#define LORD_LOCALENAME_H

#include <stdio.h>
#include <string.h>

/* "en_IN" → "en_IN.UTF-8", "ca_ES@valencia" → "ca_ES.UTF-8@valencia"; a name with a codeset is kept. */
static inline void locale_utf8_form(const char *loc, char *out, size_t n) {
    const char *at = strchr(loc, '@');
    size_t base = at ? (size_t)(at - loc) : strlen(loc);
    if (memchr(loc, '.', base)) snprintf(out, n, "%s", loc);
    else snprintf(out, n, "%.*s.UTF-8%s", (int)base, loc, at ? at : "");
}

/* Does the SUPPORTED text in `f` list `name` as a UTF-8 region, in either spelling? */
static inline int locale_listed(FILE *f, const char *name) {
    char line[256], loc[128], cs[64], form[160];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%127s %63s", loc, cs) != 2 || strcmp(cs, "UTF-8")) continue;
        if (!strcmp(loc, name)) return 1;
        locale_utf8_form(loc, form, sizeof form);
        if (!strcmp(form, name)) return 1;
    }
    return 0;
}

static inline int locale_supported_utf8(const char *name) {
    FILE *f = fopen("/usr/share/i18n/SUPPORTED", "re");
    if (!f) return 0;
    int ok = locale_listed(f, name);
    fclose(f);
    return ok;
}

#endif
