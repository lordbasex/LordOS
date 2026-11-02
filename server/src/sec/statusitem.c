/* statusitem.c — see statusitem.h. */
#include "statusitem.h"
#include "json.h"
#include "../lpk.h"

#include <stdio.h>
#include <string.h>

int statusitem_id_ok(const char *id) {
    size_t n = id ? strlen(id) : 0;
    if (!n || n > 32) return 0;
    for (const char *p = id; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return 0;
    return 1;
}

/* Text for a bar or a menu: bounded, on one line, valid UTF-8, and nothing that reorders or hides what it says. */
static int label_ok(const char *s) {
    size_t n = strlen(s);
    if (n > STATUS_LABEL_MAX) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned c = *p;
        int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (k < 0 || c < 0x20 || c == 0x7f) return 0;
        unsigned v = k == 0 ? c : k == 1 ? c & 0x1f : k == 2 ? c & 0x0f : c & 0x07;
        for (int i = 1; i <= k; i++) {
            if ((p[i] & 0xc0) != 0x80) return 0;
            v = v << 6 | (p[i] & 0x3f);
        }
        if ((k == 1 && v < 0x80) || (k == 2 && v < 0x800) || (k == 3 && v < 0x10000) || v > 0x10ffff || (v >= 0xd800 && v < 0xe000)) return 0;
        if ((v >= 0x80 && v < 0xa0) || (v >= 0x200b && v <= 0x200f) || (v >= 0x202a && v <= 0x202e) || (v >= 0x2066 && v <= 0x2069) || v == 0xfeff) return 0;
        p += k + 1;
    }
    return 1;
}

#define ADD(...) do { int k_ = snprintf(out + o, n - o, __VA_ARGS__); if (k_ < 0 || (size_t)k_ >= n - o) goto done; o += (size_t)k_; } while (0)

int statusitem_clean(const char *app, const char *json, size_t len, char *out, size_t n) {
    if (!lpk_valid_id(app) || len > 16 * 1024) return -1;
    JDoc d;
    if (json_parse(json, len, &d, NULL, 0)) return -1;
    int r = -1;
    size_t o = 0;
    const char *icon = json_str(d.root, "icon"), *title = json_str(d.root, "title");
    const JVal *menu = json_get(d.root, "menu"), *alert = json_get(d.root, "alert");
    size_t il = icon ? strlen(icon) : 0;
    char q[STATUS_LABEL_MAX * 6 + 3], q2[STATUS_LABEL_MAX * 6 + 3];
    if (!d.root || d.root->type != J_OBJ || !icon || il < 5 || !lpk_valid_path(icon) || strcmp(icon + il - 4, ".png")) goto done;
    if (title && !label_ok(title)) goto done;
    if (alert && alert->type != J_BOOL) goto done;
    if (menu && (menu->type != J_ARR || menu->len > STATUS_MENU_MAX)) goto done;
    json_quote(q, sizeof q, icon);
    json_quote(q2, sizeof q2, title ? title : "");
    ADD("{\"app\":\"%s\",\"icon\":%s,\"title\":%s,\"alert\":%s,\"menu\":[", app, q, q2, alert && alert->num ? "true" : "false");
    int first = 1;
    for (const JVal *m = menu ? menu->kid : NULL; m; m = m->next) {
        const char *id = json_str(m, "id"), *label = json_str(m, "label");
        const JVal *off = json_get(m, "off"), *line = json_get(m, "line");
        if (m->type != J_OBJ || (off && off->type != J_BOOL) || (line && line->type != J_BOOL)) goto done;
        if (!first) ADD(",");
        first = 0;
        if (line && line->num) { ADD("{\"line\":true}"); continue; }
        if (!id || !statusitem_id_ok(id) || !label || !label[0] || !label_ok(label)) goto done;
        json_quote(q, sizeof q, label);
        ADD("{\"id\":\"%s\",\"label\":%s,\"off\":%s}", id, q, off && off->num ? "true" : "false");
    }
    ADD("]}");
    r = 0;
done:
    json_free(&d);
    return r;
}
