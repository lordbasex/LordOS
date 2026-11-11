/* webreg.c — see webreg.h. */
#include "webreg.h"
#include "../sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const KINDS[WK_COUNT] = { "notifications", "camera", "microphone", "screen", "location" };

const char *webreg_kind_name(int kind) { return kind >= 0 && kind < WK_COUNT ? KINDS[kind] : NULL; }

int webreg_id_ok(const char *s) {
    if (!s || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'))) return 0;
    size_t i = 1;
    for (; s[i]; i++) {
        if (i >= WEBREG_ID_MAX) return 0;
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return 0;
    }
    return 1;
}

int webreg_text_ok(const char *s, size_t max) {
    if (!s || !s[0]) return 0;
    size_t n = strlen(s);
    if (n > max) return 0;
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n;) {
        unsigned c = p[i];
        if (c < 0x20 || c == 0x7f) return 0;
        if (c < 0x80) { i++; continue; }
        int k = c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 && c < 0xf0 ? 2 : c >= 0xc2 && c < 0xe0 ? 1 : -1;
        if (k < 0) return 0;
        unsigned cp = c & (0x3f >> k);
        for (int j = 1; j <= k; j++) {
            if (i + (size_t)j >= n || (p[i + j] & 0xc0) != 0x80) return 0;
            cp = cp << 6 | (p[i + j] & 0x3f);
        }
        if ((k == 2 && cp < 0x800) || (k == 3 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        if (cp >= 0x80 && cp < 0xa0) return 0;      /* C1 controls */
        i += (size_t)k + 1;
    }
    return 1;
}

static int fail(char *err, size_t n, const char *fmt, ...) {
    if (err && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static int copy(char *dst, size_t n, const char *s) {
    size_t l = strlen(s);
    if (l >= n) return -1;
    memcpy(dst, s, l + 1);
    return 0;
}

/* "icons/<name>.png" or ".svg": one file in the registry's icons folder, nothing that climbs or hides */
static int icon_ok(const char *s) {
    size_t n = strlen(s);
    if (n < 10 || n > WEBREG_ICON_MAX || strncmp(s, "icons/", 6)) return 0;
    if (strcmp(s + n - 4, ".png") && strcmp(s + n - 4, ".svg")) return 0;
    if (s[6] == '.' || s[6] == '-') return 0;
    for (const char *p = s + 6; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) return 0;
    return !strstr(s, "..");
}

static int hexd(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static int color_ok(const char *s) {
    if (strlen(s) != 7 || s[0] != '#') return 0;
    for (int i = 1; i < 7; i++) if (!hexd((unsigned char)s[i])) return 0;
    return 1;
}

static int perm_word(const char *s, WebPerm *p) {
    if (!strcmp(s, "allow")) *p = WP_ALLOW;
    else if (!strcmp(s, "ask")) *p = WP_ASK;
    else if (!strcmp(s, "deny")) *p = WP_DENY;
    else return -1;
    return 0;
}

static int parse_profiles(const JVal *v, WebApp *a, char *err, size_t n) {
    if (!v || v->type != J_ARR) return fail(err, n, "profiles: not a list");
    if (!v->len || v->len > WEBREG_PROFILES_MAX) return fail(err, n, "profiles: 1 to %d", WEBREG_PROFILES_MAX);
    for (const JVal *p = v->kid; p; p = p->next) {
        if (p->type != J_OBJ) return fail(err, n, "profiles: not an object");
        WebProfile *w = &a->profiles[a->nprofiles];
        int has_id = 0, has_name = 0;
        for (const JVal *k = p->kid; k; k = k->next) {
            if (!strcmp(k->key, "id")) {
                if (k->type != J_STR || !webreg_id_ok(k->str)) return fail(err, n, "profiles: bad id");
                copy(w->id, sizeof w->id, k->str);
                has_id = 1;
            } else if (!strcmp(k->key, "name")) {
                if (k->type != J_STR || !webreg_text_ok(k->str, WEBREG_NAME_MAX)) return fail(err, n, "profiles: bad name");
                copy(w->name, sizeof w->name, k->str);
                has_name = 1;
            } else return fail(err, n, "profiles: unknown key %.40s", k->key);
        }
        if (!has_id || !has_name) return fail(err, n, "profiles: id and name are needed");
        for (int i = 0; i < a->nprofiles; i++) if (!strcmp(a->profiles[i].id, w->id)) return fail(err, n, "profiles: %s twice", w->id);
        a->nprofiles++;
    }
    return 0;
}

static int parse_perms(const JVal *v, WebApp *a, char *err, size_t n) {
    if (!v || v->type != J_OBJ) return fail(err, n, "permissions: not an object");
    for (const JVal *k = v->kid; k; k = k->next) {
        int kind = -1;
        for (int i = 0; i < WK_COUNT; i++) if (!strcmp(k->key, KINDS[i])) kind = i;
        if (kind < 0) return fail(err, n, "permissions: unknown key %.40s", k->key);
        if (k->type != J_STR || perm_word(k->str, &a->perms[kind])) return fail(err, n, "permissions: %s is not allow, ask or deny", KINDS[kind]);
    }
    return 0;
}

enum { H_SCHEMA = 1, H_ID = 2, H_NAME = 4, H_START = 8, H_SCOPE = 16, H_ORIGIN = 32, H_PROFILES = 64 };

int webreg_parse(const char *text, size_t len, WebApp *a, char *err, size_t n) {
    if (err && n) err[0] = 0;
    if (!text || !a) return fail(err, n, "nothing to read");
    if (len > WEBREG_FILE_MAX) return fail(err, n, "over %d bytes", WEBREG_FILE_MAX);
    memset(a, 0, sizeof *a);
    for (int i = 0; i < WK_COUNT; i++) a->perms[i] = WP_ASK;
    JDoc doc = { 0 };
    char jerr[96];
    if (json_parse(text, len, &doc, jerr, sizeof jerr)) return fail(err, n, "not JSON: %s", jerr);
    int r = -1, has = 0;
    const JVal *root = doc.root;
    if (!root || root->type != J_OBJ) { fail(err, n, "not an object"); goto out; }
    for (const JVal *k = root->kid; k; k = k->next) {
        const char *key = k->key;
        int str = k->type == J_STR;
        if (!strcmp(key, "schema")) {
            if (k->type != J_NUM || k->num != 1) { fail(err, n, "schema is not 1"); goto out; }
            has |= H_SCHEMA;
        } else if (!strcmp(key, "id")) {
            if (!str || !webreg_id_ok(k->str)) { fail(err, n, "bad id"); goto out; }
            copy(a->id, sizeof a->id, k->str);
            has |= H_ID;
        } else if (!strcmp(key, "name")) {
            if (!str || !webreg_text_ok(k->str, WEBREG_NAME_MAX)) { fail(err, n, "bad name"); goto out; }
            copy(a->name, sizeof a->name, k->str);
            has |= H_NAME;
        } else if (!strcmp(key, "startUrl") || !strcmp(key, "scope") || !strcmp(key, "origin")) {
            if (!str || !weburl_app_ok(k->str)) { fail(err, n, "%s is not an https address", key); goto out; }
            if (key[0] == 's' && key[1] == 't') { copy(a->start_url, sizeof a->start_url, k->str); has |= H_START; }
            else if (key[0] == 's') {
                if (strchr(k->str, '?') || strchr(k->str, '#')) { fail(err, n, "scope has a query or a fragment"); goto out; }
                copy(a->scope, sizeof a->scope, k->str);
                has |= H_SCOPE;
            } else {
                char o[WEBURL_ORIGIN_MAX + 1];
                if (weburl_origin(k->str, o, sizeof o) || strcmp(o, k->str)) { fail(err, n, "origin is not scheme://host[:port]"); goto out; }
                copy(a->origin, sizeof a->origin, k->str);
                has |= H_ORIGIN;
            }
        } else if (!strcmp(key, "icon")) {
            if (!str || !icon_ok(k->str)) { fail(err, n, "bad icon"); goto out; }
            copy(a->icon, sizeof a->icon, k->str);
        } else if (!strcmp(key, "themeColor")) {
            if (!str || !color_ok(k->str)) { fail(err, n, "themeColor is not #rrggbb"); goto out; }
            copy(a->theme_color, sizeof a->theme_color, k->str);
        } else if (!strcmp(key, "source")) {
            if (!str || (strcmp(k->str, "catalog") && strcmp(k->str, "url"))) { fail(err, n, "source is not catalog or url"); goto out; }
            a->catalog = !strcmp(k->str, "catalog");
        } else if (!strcmp(key, "background")) {
            if (k->type != J_BOOL) { fail(err, n, "background is not true or false"); goto out; }
            a->background = k->num != 0;
        } else if (!strcmp(key, "profiles")) {
            if (parse_profiles(k, a, err, n)) goto out;
            has |= H_PROFILES;
        } else if (!strcmp(key, "permissions")) {
            if (parse_perms(k, a, err, n)) goto out;
        } else { fail(err, n, "unknown key %.40s", key); goto out; }
    }
    if ((has & (H_SCHEMA | H_ID | H_NAME | H_START | H_SCOPE | H_ORIGIN | H_PROFILES)) != (H_SCHEMA | H_ID | H_NAME | H_START | H_SCOPE | H_ORIGIN | H_PROFILES)) {
        fail(err, n, "schema, id, name, startUrl, scope, origin and profiles are needed");
        goto out;
    }
    char o[WEBURL_ORIGIN_MAX + 1];
    if (weburl_origin(a->start_url, o, sizeof o) || strcmp(o, a->origin)) { fail(err, n, "startUrl is not on origin"); goto out; }
    if (weburl_origin(a->scope, o, sizeof o) || strcmp(o, a->origin)) { fail(err, n, "scope is not on origin"); goto out; }
    if (!weburl_in_scope(a->start_url, a->scope)) { fail(err, n, "scope is not a prefix of startUrl"); goto out; }
    r = 0;
out:
    json_free(&doc);
    if (r) memset(a, 0, sizeof *a);
    return r;
}

const WebProfile *webreg_profile(const WebApp *a, const char *id) {
    for (int i = 0; a && id && i < a->nprofiles; i++) if (!strcmp(a->profiles[i].id, id)) return &a->profiles[i];
    return NULL;
}

int webreg_check_launch(const WebApp *a, const char *app, const char *profile, char *err, size_t n) {
    if (!webreg_id_ok(app)) return fail(err, n, "bad app id");
    if (!webreg_id_ok(profile)) return fail(err, n, "bad profile id");
    if (strcmp(a->id, app)) return fail(err, n, "the file is for %s, not %s", a->id, app);
    if (!webreg_profile(a, profile)) return fail(err, n, "%s has no profile %s", app, profile);
    return 0;
}

int webreg_load(const char *home, const char *app, WebApp *out, char *err, size_t n) {
    if (!home || home[0] != '/' || !webreg_id_ok(app)) return fail(err, n, "bad home or app id");
    char path[4096];
    if (snprintf(path, sizeof path, "%s/.config/lordos/web/apps/%s.json", home, app) >= (int)sizeof path) return fail(err, n, "path too long");
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return fail(err, n, "%s: %s", path, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return fail(err, n, "%s is not a file", path); }
    if (st.st_size > WEBREG_FILE_MAX) { close(fd); return fail(err, n, "%s is over %d bytes", path, WEBREG_FILE_MAX); }
    char *buf = malloc(WEBREG_FILE_MAX + 1);
    if (!buf) { close(fd); return fail(err, n, "out of memory"); }
    size_t len = 0;
    for (;;) {
        ssize_t k = read(fd, buf + len, WEBREG_FILE_MAX + 1 - len);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        len += (size_t)k;
        if (len > WEBREG_FILE_MAX) break;
    }
    close(fd);
    int r = len > WEBREG_FILE_MAX ? fail(err, n, "%s is over %d bytes", path, WEBREG_FILE_MAX) : webreg_parse(buf, len, out, err, n);
    free(buf);
    return r;
}
