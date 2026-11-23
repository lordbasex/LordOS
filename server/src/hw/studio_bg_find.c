/* studio_bg_find.c — one file of LordOS Studio's background catalog (see studio_bg_find.h). */
#include "studio_bg_find.h"
#include "../sec/json.h"

#include <stdio.h>
#include <string.h>

static const JVal *get(const JVal *o, const char *k) {
    if (!o || o->type != J_OBJ) return NULL;
    for (const JVal *m = o->kid; m; m = m->next) if (!strcmp(m->key, k)) return m;
    return NULL;
}
static const char *str(const JVal *o, const char *k) {
    const JVal *v = get(o, k);
    return v && v->type == J_STR ? v->str : NULL;
}
static int sha_ok(const char *s) {
    if (!s || strlen(s) != 64) return 0;
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
    return 1;
}
static int word_ok(const char *s, size_t n) {
    if (!n || n > 48) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '.')) return 0;
    return 1;
}
/* a relative file name under the base: [a-z0-9._/-], no "..", no leading slash or dot, ending in .ext */
static int file_ok(const char *f, const char *ext) {
    size_t n = f ? strlen(f) : 0, e = strlen(ext);
    if (n < e + 2 || n > 200 || f[0] == '/' || f[0] == '.' || strstr(f, "..") || strstr(f, "//")) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((f[i] >= 'a' && f[i] <= 'z') || (f[i] >= '0' && f[i] <= '9') || f[i] == '-' || f[i] == '.' || f[i] == '_' || f[i] == '/')) return 0;
    return f[n - e - 1] == '.' && !strcmp(f + n - e, ext);
}

int hw_studio_bg_find(const char *json, size_t n, const char *arg, char *url, size_t un, char *sha, size_t sn,
                      unsigned long long *size, const char **ext) {
    const char *sp = arg ? strchr(arg, ' ') : NULL;
    if (!sp || !word_ok(arg, (size_t)(sp - arg)) || !word_ok(sp + 1, strlen(sp + 1))) return -1;
    JDoc d;
    if (json_parse(json, n, &d, NULL, 0)) return -1;
    int rc = -1;
    const char *base = str(d.root, "base");
    const JVal *items = get(d.root, "items");
    if (base && !strncmp(base, "https://raw.githubusercontent.com/lordbasex/", 44) && !strchr(base + 44, '?') && !strchr(base + 44, '#') &&
        items && items->type == J_ARR) {
        for (const JVal *it = items->kid; it && rc; it = it->next) {
            const char *id = str(it, "id"), *kind = str(it, "kind");
            if (!id || strlen(id) != (size_t)(sp - arg) || strncmp(id, arg, (size_t)(sp - arg))) continue;
            const char *x = kind && !strcmp(kind, "picture") ? "jpg" : kind && !strcmp(kind, "video") ? "mp4" : NULL;
            if (!x) continue;
            const JVal *vars = get(it, "variants");
            for (const JVal *v = vars && vars->type == J_ARR ? vars->kid : NULL; v && rc; v = v->next) {
                const char *name = str(v, "name"), *file = str(v, "file"), *h = str(v, "sha256");
                const JVal *bytes = get(v, "size");
                if (!name || !file_ok(file, x) || !sha_ok(h) || !bytes || bytes->type != J_NUM || strcmp(name, sp + 1)) continue;
                if (bytes->num <= 0 || bytes->num > (double)STUDIO_BG_MAX_FILE || bytes->num != (double)(unsigned long long)bytes->num) continue;
                if (snprintf(url, un, "%s/%s", base, file) >= (int)un) continue;
                snprintf(sha, sn, "%s", h);
                *size = (unsigned long long)bytes->num;
                *ext = x;
                rc = 0;
            }
        }
    }
    json_free(&d);
    return rc;
}
