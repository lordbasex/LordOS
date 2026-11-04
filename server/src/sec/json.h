/* json.h — small, strict JSON reader (RFC 8259) for the files LordOS verifies in C:
 * contenido.json and firma.json of .lpk packages, the signed catalog and the saved permissions
 * (see docs/SIGNING.md and docs/PRIVACY.md). Everything lives in a single memory block. */
#ifndef LORD_SEC_JSON_H
#define LORD_SEC_JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;
struct JVal {
    JType type;
    double num;                 /* J_NUM; J_BOOL: 0 or 1 */
    const char *str;            /* J_STR (UTF-8, already unescaped, NUL-terminated) */
    size_t len;                 /* J_STR: bytes; J_ARR/J_OBJ: number of elements */
    const char *key;            /* key, if it's an object member */
    JVal *kid, *next;           /* first child and next sibling */
};

typedef struct {
    JVal *root;
    void *mem;
} JDoc;

/* 0 on success. Rejects: text that isn't JSON, more than max_depth levels (32), duplicate keys, \u0000
 * and trailing garbage. err (optional) gets the reason and the position. */
int  json_parse(const char *text, size_t len, JDoc *doc, char *err, size_t errlen);
/* Same, with another nesting limit: for trusted-but-deep JSON such as lord-maild's message view,
 * whose HTML tree nests up to 2 * HTML_MAX_DEPTH levels (see src/mail/html.h). The parser recurses
 * once per level, so keep max_depth modest (a few hundred at most). */
int  json_parse_depth(const char *text, size_t len, JDoc *doc, char *err, size_t errlen, int max_depth);
void json_free(JDoc *doc);

const JVal *json_get(const JVal *obj, const char *key);           /* NULL if not an object or missing */
const char *json_str(const JVal *obj, const char *key);           /* NULL if not text */
double      json_num(const JVal *obj, const char *key, double def);
int         json_bool(const JVal *obj, const char *key, int def);

/* JSON text with quotes and escapes of s (to build JSON in C). Returns bytes written. */
size_t json_quote(char *out, size_t n, const char *s);

#endif
