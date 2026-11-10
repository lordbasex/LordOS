/* snimodel.c — see snimodel.h. */
#include "snimodel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the registered service ---- */

static int unique_ok(const char *s) {
    size_t n = strlen(s);
    if (n < 2 || n >= SNI_NAME_MAX || s[0] != ':') return 0;
    for (const char *p = s + 1; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) return 0;
    return 1;
}

int sni_service(const char *service, const char *sender, char dest[SNI_NAME_MAX], char path[SNI_NAME_MAX]) {
    if (!service || !sender || !unique_ok(sender)) return -1;
    if (service[0] == '/') {                        /* Ayatana and Chromium: a path on the caller's own connection */
        if (strlen(service) >= SNI_NAME_MAX || !dbus_path_valid(service)) return -1;
        snprintf(dest, SNI_NAME_MAX, "%s", sender);
        snprintf(path, SNI_NAME_MAX, "%s", service);
        return 0;
    }
    if (service[0] == ':') {                        /* a unique name: only the caller's own */
        if (strcmp(service, sender)) return -1;
    } else if (strlen(service) >= SNI_NAME_MAX || !dbus_name_valid(service)) return -1;   /* Qt, KDE: org.kde.StatusNotifierItem-<pid>-<n> */
    snprintf(dest, SNI_NAME_MAX, "%s", service);
    snprintf(path, SNI_NAME_MAX, "/StatusNotifierItem");
    return 0;
}

/* ---- pixmaps ---- */

int sni_pix_score(int w, int h) {
    int s = w > h ? w : h;
    if (s >= SNI_PIX_LO && s <= SNI_PIX_HI) return SNI_PIX_HI - s;
    if (s > SNI_PIX_HI) return 100 + (s - SNI_PIX_HI);
    return 10000 + (SNI_PIX_LO - s);
}

void sni_pix_free(SniPix *p) { free(p->rgba); memset(p, 0, sizeof *p); }

int sni_pixmap(DRd *r, SniPix *out) {
    memset(out, 0, sizeof *out);
    size_t end = dr_arr(r, 8);
    const uint8_t *best = NULL;
    int bw = 0, bh = 0, bs = 0, n = 0, over = 0;
    size_t total = 0;
    while (dr_more(r, end)) {
        dr_struct(r);
        int32_t w = dr_i32(r), h = dr_i32(r);
        uint32_t len = 0;
        const uint8_t *px = dr_bytes(r, &len);
        if (r->err) return -1;
        total += len;
        if (++n > SNI_PIX_COUNT || total > SNI_PIX_BYTES) over = 1;
        if (over || w < 1 || h < 1 || w > SNI_PIX_SIDE || h > SNI_PIX_SIDE || len != (uint32_t)w * (uint32_t)h * 4) continue;
        int s = sni_pix_score(w, h);
        if (!best || s < bs) { best = px; bw = w; bh = h; bs = s; }
    }
    if (r->err || over || !best) return -1;
    size_t np = (size_t)bw * bh;
    out->rgba = malloc(np * 4);
    if (!out->rgba) return -1;
    for (size_t i = 0; i < np; i++) {               /* A R G B, big endian, not premultiplied */
        const uint8_t *s = best + i * 4;
        uint8_t *d = out->rgba + i * 4;
        d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; d[3] = s[0];
    }
    out->w = bw;
    out->h = bh;
    return 0;
}

/* ---- text ---- */

void sni_text(const char *in, char *out, size_t n, int mnemonic) {
    size_t o = 0;
    if (!n) return;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p;) {
        unsigned c = *p;
        int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (k < 0) { p++; continue; }
        unsigned v = k == 0 ? c : k == 1 ? c & 0x1f : k == 2 ? c & 0x0f : c & 0x07;
        int bad = 0;
        for (int i = 1; i <= k; i++) {
            if ((p[i] & 0xc0) != 0x80) { bad = 1; break; }
            v = v << 6 | (p[i] & 0x3f);
        }
        if (bad) { p++; continue; }
        size_t len = (size_t)k + 1;
        const unsigned char *at = p;
        p += len;
        if ((k == 1 && v < 0x80) || (k == 2 && v < 0x800) || (k == 3 && v < 0x10000) || v > 0x10ffff || (v >= 0xd800 && v < 0xe000)) continue;
        if (v < 0x20 || v == 0x7f || (v >= 0x80 && v < 0xa0)) {          /* a newline or a tab reads as a space */
            if ((v == '\n' || v == '\t') && o && out[o - 1] != ' ' && o + 1 < n) out[o++] = ' ';
            continue;
        }
        if ((v >= 0x200b && v <= 0x200f) || (v >= 0x202a && v <= 0x202e) || (v >= 0x2066 && v <= 0x2069) || v == 0xfeff) continue;
        if (mnemonic && v == '_') {
            if (*p != '_') continue;                /* the access key's mark */
            p++;                                    /* "__": one '_' */
        }
        if (o + len >= n) break;
        memcpy(out + o, at, len);
        o += len;
    }
    out[o] = 0;
}

/* ---- dbusmenu ---- */

typedef struct {
    HBuf *b;
    int32_t *ids;
    int max, nids, count;
} Menu;

#define ENTRY_SIG "(ia{sv}av)"

/* One (ia{sv}av) entry at depth d (1 for the root's children). Writes it to the JSON unless it is hidden, past the
 * item cap or past the depth (its children then are read past). *comma: an entry was already written at this level. */
static int entry(DRd *r, Menu *m, int d, int *comma) {
    dr_struct(r);
    int32_t id = dr_i32(r);
    char label[SNI_TEXT_MAX] = "", type[16] = "", toggle[16] = "", display[16] = "";
    int enabled = 1, visible = 1, state = 0;
    size_t pend = dr_arr(r, 8);
    while (dr_more(r, pend)) {
        dr_struct(r);
        const char *key = dr_str(r, NULL), *sig = dr_variant(r);
        if (r->err) return -1;
        if (!strcmp(key, "label") && !strcmp(sig, "s")) sni_text(dr_str(r, NULL), label, sizeof label, 1);
        else if (!strcmp(key, "type") && !strcmp(sig, "s")) snprintf(type, sizeof type, "%s", dr_str(r, NULL));
        else if (!strcmp(key, "toggle-type") && !strcmp(sig, "s")) snprintf(toggle, sizeof toggle, "%s", dr_str(r, NULL));
        else if (!strcmp(key, "children-display") && !strcmp(sig, "s")) snprintf(display, sizeof display, "%s", dr_str(r, NULL));
        else if (!strcmp(key, "enabled") && !strcmp(sig, "b")) enabled = dr_bool(r);
        else if (!strcmp(key, "visible") && !strcmp(sig, "b")) visible = dr_bool(r);
        else if (!strcmp(key, "toggle-state") && !strcmp(sig, "i")) state = dr_i32(r);
        else dr_skip(r, sig);
    }
    if (r->err) return -1;
    int keep = visible && d <= SNI_MENU_DEPTH && m->count < SNI_MENU_ITEMS;
    int sep = !strcmp(type, "separator");
    /* the children: a submenu when there are any (or children-display says so); one past the depth is shown off */
    size_t cend = dr_arr(r, 1);
    int has = dr_more(r, cend) || !strcmp(display, "submenu");
    if (has && d >= SNI_MENU_DEPTH) enabled = 0;
    if (keep) {
        m->count++;
        if (*comma) hb_printf(m->b, ",");
        *comma = 1;
        if (sep) hb_printf(m->b, "{\"sep\":true}");
        else {
            hb_printf(m->b, "{\"id\":%d,\"label\":", (int)id);
            hb_str(m->b, label);
            hb_printf(m->b, ",\"enabled\":%s", enabled ? "true" : "false");
            if (!strcmp(toggle, "checkmark") || !strcmp(toggle, "radio"))
                hb_printf(m->b, ",\"toggle\":\"%s\",\"on\":%s", toggle, state == 1 ? "true" : "false");
        }
    }
    if (keep && !sep && has && d < SNI_MENU_DEPTH) hb_printf(m->b, ",\"sub\":[");
    int sub = 0;
    while (dr_more(r, cend)) {
        const char *sig = dr_variant(r);
        if (r->err) return -1;
        if (strcmp(sig, ENTRY_SIG)) { dr_skip(r, sig); continue; }
        if (keep && !sep && d < SNI_MENU_DEPTH) { if (entry(r, m, d + 1, &sub)) return -1; }
        else dr_skip(r, sig);
    }
    if (r->err) return -1;
    if (keep && !sep) {
        if (has && d < SNI_MENU_DEPTH) hb_printf(m->b, "]");
        hb_printf(m->b, "}");
        if (!has && enabled && m->nids < m->max) m->ids[m->nids++] = id;
    }
    return 0;
}

int sni_menu_json(DRd *r, HBuf *b, int32_t *ids, int max, int *nids) {
    Menu m = { b, ids, max, 0, 0 };
    *nids = 0;
    dr_struct(r);
    dr_i32(r);                                      /* the root's id */
    size_t pend = dr_arr(r, 8);
    while (dr_more(r, pend)) {                      /* the root's own properties say nothing we show */
        dr_struct(r);
        dr_str(r, NULL);
        const char *sig = dr_variant(r);
        if (r->err) return -1;
        dr_skip(r, sig);
    }
    hb_printf(b, "[");
    size_t cend = dr_arr(r, 1);
    int comma = 0;
    while (dr_more(r, cend)) {
        const char *sig = dr_variant(r);
        if (r->err) return -1;
        if (strcmp(sig, ENTRY_SIG)) { dr_skip(r, sig); continue; }
        if (entry(r, &m, 1, &comma)) return -1;
    }
    hb_printf(b, "]");
    *nids = m.nids;
    return r->err ? -1 : 0;
}
