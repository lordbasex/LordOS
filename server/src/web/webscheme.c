/* webscheme.c — see webscheme.h. */
#include "webscheme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int webscheme_parse(const char *name) {
    if (!name) return -1;
    if (!strcmp(name, "light")) return WEBSCHEME_LIGHT;
    if (!strcmp(name, "dark")) return WEBSCHEME_DARK;
    return -1;
}

const char *webscheme_name(int scheme) {
    return scheme == WEBSCHEME_LIGHT ? "light" : scheme == WEBSCHEME_DARK ? "dark" : NULL;
}

int webscheme_from_env(const char *value) { return webscheme_parse(value) == WEBSCHEME_DARK ? WEBSCHEME_DARK : WEBSCHEME_LIGHT; }

static int key_ok(const char *key) {
    if (!key || strlen(key) != WEBSCHEME_KEY_LEN) return 0;
    for (const char *s = key; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
    return 1;
}

static char *print(const char *fmt, const char *a, const char *key, const char *b) {
    int n = snprintf(NULL, 0, fmt, a, key, b);
    if (n < 0) return NULL;
    char *out = malloc((size_t)n + 1);
    if (out) snprintf(out, (size_t)n + 1, fmt, a, key, b);
    return out;
}

char *webscheme_script(const char *js, const char *key, int scheme) {
    if (!js || !key_ok(key) || !webscheme_name(scheme)) return NULL;
    return print("%s(\"__lordosScheme%s\", %s);\n", js, key, scheme == WEBSCHEME_DARK ? "true" : "false");
}

char *webscheme_call(const char *key, int scheme) {
    if (!key_ok(key) || !webscheme_name(scheme)) return NULL;
    return print("%s(function () { var f = window.__lordosScheme%s; if (typeof f === 'function') f(%s); })();", "", key,
                 scheme == WEBSCHEME_DARK ? "true" : "false");
}
