/* js_secret.h — a secret (a password, a request that may carry one) read from a JS string into memory of our own,
 * so it can be wiped after use.
 *
 * JS_ToCString* does not always copy: for an ASCII string QuickJS returns a pointer into the JSString itself
 * (quickjs.c, JS_ToCStringLen2). Wiping that pointer wrote zeros into a string JavaScript still holds (the
 * field's value, a cached copy, an interned atom whose hash no longer matches its text). js_secret_dup copies
 * the text instead; js_secret_free wipes and frees the copy. */
#ifndef LORD_JS_SECRET_H
#define LORD_JS_SECRET_H

#include "quickjs.h"

#include <stdlib.h>
#include <string.h>

/* NULL on an exception or out of memory; *len (if given) the length in bytes, without the trailing 0 */
static inline char *js_secret_dup(JSContext *c, JSValueConst v, size_t *len) {
    size_t n = 0;
    const char *s = JS_ToCStringLen(c, &n, v);
    if (!s) return NULL;
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
        if (len) *len = n;
    }
    JS_FreeCString(c, s);
    return d;
}

static inline void js_secret_free(char *p, size_t n) {
    if (!p) return;
    explicit_bzero(p, n);
    free(p);
}

#endif
