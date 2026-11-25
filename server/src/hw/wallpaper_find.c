/* wallpaper_find.c — one entry of the wallpapers catalog (docs/WALLPAPERS.md), apart for its test: moving wallpapers
 * (hevc, h264: kept as .mp4) and still pictures (jpeg: kept as .jpg). */
#include "wallpaper_find.h"
#include "../sec/json.h"

#include <stdio.h>
#include <string.h>

#define MAX_FILE (100ull * 1024 * 1024)           /* the host's own limit per file */

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

/* The extension a variant is kept with, from its codec; NULL for an unknown codec or a file that isn't of that kind. */
static const char *kind_ext(const char *codec, const char *file) {
    const char *ext = NULL;
    if (codec && (!strcmp(codec, "hevc") || !strcmp(codec, "h264"))) ext = "mp4";
    else if (codec && !strcmp(codec, "jpeg")) ext = "jpg";
    if (!ext || !file) return NULL;
    size_t fl = strlen(file);
    if (fl < 5 || file[fl - 4] != '.' || strcmp(file + fl - 3, ext)) return NULL;
    return ext;
}
/* A relative name under the base: [a-z0-9._/-], no "..", no "//", no leading dot or slash. */
static int file_ok(const char *f) {
    if (!f || !*f || f[0] == '/' || f[0] == '.' || strstr(f, "..") || strstr(f, "//") || strlen(f) > 200) return 0;
    for (const char *p = f; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '/' || *p == '-')) return 0;
    return 1;
}

int hw_wallpaper_find(const char *json, size_t n, const char *arg, char *url, size_t un, char *sha, size_t sn, unsigned long long *size,
                      const char **ext) {
    const char *sp = arg ? strchr(arg, ' ') : NULL;
    if (!sp || !word_ok(arg, (size_t)(sp - arg)) || !word_ok(sp + 1, strlen(sp + 1))) return -1;
    JDoc d;
    if (json_parse(json, n, &d, NULL, 0)) return -1;
    int rc = -1;
    const char *base = str(d.root, "base");
    const JVal *items = get(d.root, "items");
    if (base && !strncmp(base, "https://raw.githubusercontent.com/lordbasex/", 44) && items && items->type == J_ARR) {
        for (const JVal *it = items->kid; it && rc; it = it->next) {
            const char *id = str(it, "id");
            if (!id || strlen(id) != (size_t)(sp - arg) || strncmp(id, arg, (size_t)(sp - arg))) continue;
            const JVal *vars = get(it, "variants");
            for (const JVal *v = vars && vars->type == J_ARR ? vars->kid : NULL; v && rc; v = v->next) {
                const char *name = str(v, "name"), *file = str(v, "file"), *h = str(v, "sha256");
                const JVal *bytes = get(v, "size");
                if (!name || !file || !sha_ok(h) || !bytes || bytes->type != J_NUM || strcmp(name, sp + 1)) continue;
                if (bytes->num <= 0 || bytes->num > (double)MAX_FILE || bytes->num != (double)(unsigned long long)bytes->num) continue;
                const char *e = kind_ext(str(v, "codec"), file);
                if (!e || !file_ok(file)) continue;
                if (snprintf(url, un, "%s/%s", base, file) >= (int)un) continue;
                snprintf(sha, sn, "%s", h);
                *size = (unsigned long long)bytes->num;
                if (ext) *ext = e;
                rc = 0;
            }
        }
    }
    json_free(&d);
    return rc;
}

