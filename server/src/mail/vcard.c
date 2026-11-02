/* vcard.c — see vcard.h. */
#include "vcard.h"
#include "ical.h"
#include "mime.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Splits a structured value on unescaped ';' into up to max unescaped fields. */
static int split_fields(const char *v, char fields[][256], int max) {
    int n = 0;
    size_t o = 0;
    for (int i = 0; i < max; i++) fields[i][0] = 0;
    for (; v && *v && n < max; v++) {
        if (*v == '\\' && v[1]) {
            v++;
            char c = (*v == 'n' || *v == 'N') ? '\n' : *v;
            if (o + 1 < 256) fields[n][o++] = c;
            continue;
        }
        if (*v == ';') { fields[n][o] = 0; n++; o = 0; continue; }
        if (o + 1 < 256) fields[n][o++] = *v;
    }
    if (n < max) { fields[n][o] = 0; n++; }
    return n;
}

/* vCard 2.1 values may be quoted-printable and in another charset */
static char *prop_text(const IcProp *p) {
    char enc[32] = "", cs[40] = "";
    ic_param(p, "ENCODING", enc, sizeof enc);
    ic_param(p, "CHARSET", cs, sizeof cs);
    if (!strcasecmp(enc, "QUOTED-PRINTABLE")) {
        size_t n = strlen(p->value);
        char *raw = malloc(n + 1);
        if (!raw) return NULL;
        size_t rl = qp_decode(p->value, n, raw, 0);
        size_t ul;
        char *u = charset_to_utf8(cs[0] ? cs : "utf-8", raw, rl, &ul);
        free(raw);
        return u;
    }
    size_t ul;
    return cs[0] ? charset_to_utf8(cs, p->value, strlen(p->value), &ul) : utf8_clean(p->value, strlen(p->value), &ul);
}

static void type_of(const IcProp *p, char *out, size_t cap) {
    char t[128] = "";
    ic_param(p, "TYPE", t, sizeof t);
    out[0] = 0;
    /* first meaningful type: home, work, cell/mobile, fax, iphone, main, other */
    char *save = NULL;
    for (char *tok = strtok_r(t, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        for (char *c = tok; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!strcmp(tok, "pref") || !strcmp(tok, "internet") || !strcmp(tok, "voice") || !strcmp(tok, "x400")) continue;
        if (!strcmp(tok, "mobile")) tok = "cell";
        snprintf(out, cap, "%s", tok);
        return;
    }
    /* item1.X-ABLabel:_$!<Other>!$_: some clients put a separate label property; ignore */
}

static void list_item(JBuf *b, int *first, const char *type, const char *value) {
    if (!value || !*value) return;
    jb_raw(b, *first ? "{\"type\":" : ",{\"type\":");
    jb_str(b, type);
    jb_raw(b, ",\"value\":");
    jb_str(b, value);
    jb_raw(b, "}");
    *first = 0;
}

static void one_card(const IcComp *c, JBuf *b) {
    char fields[7][256];
    IcProp *p;
    jb_raw(b, "{\"uid\":");
    p = ic_prop(c, "UID");
    char uid[256] = "";
    if (p) ic_unescape(p->value, uid, sizeof uid);
    if (!strncasecmp(uid, "urn:uuid:", 9)) memmove(uid, uid + 9, strlen(uid + 9) + 1);
    jb_str(b, uid);
    char *fn = NULL;
    if ((p = ic_prop(c, "FN"))) { char *t = prop_text(p); if (t) { fn = ic_unescape_dup(t); free(t); } }
    jb_raw(b, ",\"fn\":");
    jb_str(b, fn ? fn : "");
    free(fn);
    jb_raw(b, ",\"n\":{");
    if ((p = ic_prop(c, "N"))) {
        char *t = prop_text(p);
        split_fields(t ? t : "", fields, 5);
        free(t);
        static const char *k[] = { "family", "given", "middle", "prefix", "suffix" };
        for (int i = 0; i < 5; i++) { jb_printf(b, "%s\"%s\":", i ? "," : "", k[i]); jb_str(b, fields[i]); }
    }
    jb_raw(b, "}");
    if ((p = ic_prop(c, "NICKNAME"))) { char t[256]; ic_unescape(p->value, t, sizeof t); jb_raw(b, ",\"nick\":"); jb_str(b, t); }
    if ((p = ic_prop(c, "ORG"))) {
        char *t = prop_text(p);
        split_fields(t ? t : "", fields, 2);
        free(t);
        jb_raw(b, ",\"org\":"); jb_str(b, fields[0]);
        if (fields[1][0]) { jb_raw(b, ",\"dept\":"); jb_str(b, fields[1]); }
    }
    if ((p = ic_prop(c, "TITLE"))) { char t[256]; ic_unescape(p->value, t, sizeof t); jb_raw(b, ",\"title\":"); jb_str(b, t); }
    static const struct { const char *prop, *key; } lists[] = { { "EMAIL", "emails" }, { "TEL", "phones" }, { "URL", "urls" }, { "IMPP", "im" } };
    for (size_t l = 0; l < sizeof lists / sizeof lists[0]; l++) {
        jb_printf(b, ",\"%s\":[", lists[l].key);
        int first = 1;
        for (p = ic_prop(c, lists[l].prop); p; p = ic_prop_next(c, p, lists[l].prop)) {
            char ty[32], v[512];
            type_of(p, ty, sizeof ty);
            char *t = prop_text(p);
            ic_unescape(t ? t : "", v, sizeof v);
            free(t);
            if (!strcmp(lists[l].prop, "TEL") && !strncasecmp(v, "tel:", 4)) memmove(v, v + 4, strlen(v + 4) + 1);
            list_item(b, &first, ty, v);
        }
        jb_raw(b, "]");
    }
    jb_raw(b, ",\"addrs\":[");
    int first = 1;
    for (p = ic_prop(c, "ADR"); p; p = ic_prop_next(c, p, "ADR")) {
        char ty[32];
        type_of(p, ty, sizeof ty);
        char *t = prop_text(p);
        split_fields(t ? t : "", fields, 7);
        free(t);
        jb_raw(b, first ? "{\"type\":" : ",{\"type\":");
        first = 0;
        jb_str(b, ty);
        /* street = extended + street */
        char street[520];
        snprintf(street, sizeof street, "%s%s%s", fields[1], fields[1][0] && fields[2][0] ? "\n" : "", fields[2]);
        jb_raw(b, ",\"street\":"); jb_str(b, street);
        jb_raw(b, ",\"city\":"); jb_str(b, fields[3]);
        jb_raw(b, ",\"region\":"); jb_str(b, fields[4]);
        jb_raw(b, ",\"code\":"); jb_str(b, fields[5]);
        jb_raw(b, ",\"country\":"); jb_str(b, fields[6]);
        jb_raw(b, "}");
    }
    jb_raw(b, "]");
    if ((p = ic_prop(c, "BDAY"))) {
        int64_t t; int ad, y, m, d;
        char tz[8];
        const char *v = p->value;
        if (!strncmp(v, "--", 2)) {          /* vCard 4: --0517 (no year) */
            if (strlen(v) >= 6) { char out[16]; snprintf(out, sizeof out, "--%.2s-%.2s", v + 2, v + 4); jb_raw(b, ",\"bday\":"); jb_str(b, out); }
        } else if (ic_time(p, &t, &ad, tz, sizeof tz)) {
            int64_t w = t + ic_zone_offset(NULL, t);
            ic_civil_from_days(w >= 0 ? w / 86400 : (w - 86399) / 86400, &y, &m, &d);
            char out[16];
            snprintf(out, sizeof out, "%04d-%02d-%02d", y, m, d);
            jb_raw(b, ",\"bday\":"); jb_str(b, out);
        }
    }
    if ((p = ic_prop(c, "NOTE"))) { char *t = prop_text(p); char *u = ic_unescape_dup(t ? t : ""); free(t); jb_raw(b, ",\"notes\":"); jb_str(b, u ? u : ""); free(u); }
    if ((p = ic_prop(c, "CATEGORIES"))) {
        jb_raw(b, ",\"cats\":[");
        char t[512];
        snprintf(t, sizeof t, "%s", p->value);
        int f = 1;
        char *save = NULL;
        for (char *tok = strtok_r(t, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            char u[256];
            ic_unescape(tok, u, sizeof u);
            if (!u[0]) continue;
            if (!f) jb_raw(b, ",");
            jb_str(b, u);
            f = 0;
        }
        jb_raw(b, "]");
    }
    jb_printf(b, ",\"photo\":%s", ic_prop(c, "PHOTO") ? "true" : "false");
    IcProp *kind = ic_prop(c, "KIND");
    if (!kind) kind = ic_prop(c, "X-ADDRESSBOOKSERVER-KIND");
    int group = kind && !strncasecmp(kind->value, "group", 5);
    jb_printf(b, ",\"kind\":\"%s\"", group ? "group" : "individual");
    if (group) {
        jb_raw(b, ",\"members\":[");
        int f = 1;
        for (int pass = 0; pass < 2; pass++) {
            const char *pn = pass ? "X-ADDRESSBOOKSERVER-MEMBER" : "MEMBER";
            for (p = ic_prop(c, pn); p; p = ic_prop_next(c, p, pn)) {
                char u[256];
                ic_unescape(p->value, u, sizeof u);
                const char *x = !strncasecmp(u, "urn:uuid:", 9) ? u + 9 : u;
                if (!*x) continue;
                if (!f) jb_raw(b, ",");
                jb_str(b, x);
                f = 0;
            }
        }
        jb_raw(b, "]");
    }
    if ((p = ic_prop(c, "REV"))) { jb_raw(b, ",\"rev\":"); jb_str(b, p->value); }
    jb_raw(b, "}");
}

int vcard_json(const char *text, size_t len, JBuf *b) {
    const char *err;
    IcComp *root = ic_parse(text, len, &err);
    if (!root) return -1;
    int n = 0;
    for (IcComp *c = root->kid; c; c = c->next) {
        if (strcmp(c->name, "VCARD")) continue;
        if (n) jb_raw(b, ",");
        one_card(c, b);
        n++;
        if (n >= 50000) break;
    }
    ic_free(root);
    return n ? n : -1;
}

unsigned char *vcard_photo(const char *text, size_t len, size_t *olen) {
    *olen = 0;
    const char *err;
    IcComp *root = ic_parse(text, len, &err);
    if (!root) return NULL;
    unsigned char *out = NULL;
    for (IcComp *c = root->kid; c && !out; c = c->next) {
        if (strcmp(c->name, "VCARD")) continue;
        IcProp *p = ic_prop(c, "PHOTO");
        if (!p) break;
        const char *v = p->value;
        char enc[16] = "";
        ic_param(p, "ENCODING", enc, sizeof enc);
        if (!strncasecmp(v, "data:", 5)) {
            const char *comma = strchr(v, ',');
            if (!comma || !strstr(v, ";base64")) break;
            v = comma + 1;
        } else if (strcasecmp(enc, "b") && strcasecmp(enc, "base64")) break;    /* a URI: not fetched */
        size_t n = strlen(v);
        if (n > VCARD_MAX_PHOTO * 4 / 3 + 16) break;
        out = malloc(n * 3 / 4 + 4);
        if (out) *olen = b64_decode(v, n, out);
        break;
    }
    ic_free(root);
    return out;
}

void vcard_uid(const char *text, size_t len, char *out, size_t cap) {
    if (cap) out[0] = 0;
    const char *err;
    IcComp *root = ic_parse(text, len, &err);
    if (!root) return;
    for (IcComp *c = root->kid; c; c = c->next) {
        if (strcmp(c->name, "VCARD")) continue;
        IcProp *p = ic_prop(c, "UID");
        if (p) { ic_unescape(p->value, out, cap); size_t l = strlen(out); if (l >= 9 && !strncasecmp(out, "urn:uuid:", 9)) memmove(out, out + 9, l - 8); }
        break;
    }
    ic_free(root);
}

void vcard_emails(const char *text, size_t len, void (*fn)(void *u, const char *name, const char *email), void *u) {
    const char *err;
    IcComp *root = ic_parse(text, len, &err);
    if (!root) return;
    for (IcComp *c = root->kid; c; c = c->next) {
        if (strcmp(c->name, "VCARD")) continue;
        char name[256] = "";
        IcProp *p = ic_prop(c, "FN");
        if (p) { char *t = prop_text(p); ic_unescape(t ? t : "", name, sizeof name); free(t); }
        for (p = ic_prop(c, "EMAIL"); p; p = ic_prop_next(c, p, "EMAIL")) {
            char e[256];
            ic_unescape(p->value, e, sizeof e);
            if (e[0]) fn(u, name, e);
        }
    }
    ic_free(root);
}

/* ---- writing ---- */

static void field(JBuf *b, const char *s) {
    char *e = ic_escape(s ? s : "");
    if (e) { jb_raw(b, e); free(e); }
}

static const char *js(const JVal *o, const char *k) { const char *s = json_str(o, k); return s ? s : ""; }

static void typed_line(JBuf *b, const char *prop, const JVal *item, const char *extra_type) {
    const char *type = js(item, "type"), *value = js(item, "value");
    if (!*value) return;
    JBuf l;
    jb_init(&l);
    jb_raw(&l, prop);
    char t[40];
    size_t o = 0;
    for (const char *c = type; *c && o + 1 < sizeof t; c++) if (isalnum((unsigned char)*c) || *c == '-') t[o++] = (char)toupper((unsigned char)*c);
    t[o] = 0;
    if (t[0] || extra_type) {
        jb_raw(&l, ";TYPE=");
        if (extra_type) { jb_raw(&l, extra_type); if (t[0]) jb_raw(&l, ","); }
        jb_raw(&l, t);
    }
    jb_raw(&l, ":");
    field(&l, value);
    if (!l.oom) ic_line(b, l.p);
    jb_free(&l);
}

char *vcard_build(const JVal *c, const unsigned char *photo, size_t plen, const char *ptype, size_t *olen) {
    JBuf b, l;
    jb_init(&b);
    jb_raw(&b, "BEGIN:VCARD\r\nVERSION:3.0\r\nPRODID:-//Axia DE//Contacts 1.0//EN\r\n");
#define LINE(...) do { jb_init(&l); __VA_ARGS__; if (!l.oom) ic_line(&b, l.p); jb_free(&l); } while (0)
    LINE(jb_raw(&l, "UID:"); field(&l, js(c, "uid")));
    const JVal *n = json_get(c, "n");
    const char *fn = js(c, "fn");
    char fnbuf[600];
    if (!*fn) {
        snprintf(fnbuf, sizeof fnbuf, "%s%s%s", js(n, "given"), *js(n, "given") && *js(n, "family") ? " " : "", js(n, "family"));
        if (!fnbuf[0]) snprintf(fnbuf, sizeof fnbuf, "%s", js(c, "org"));
        fn = fnbuf;
    }
    LINE(jb_raw(&l, "FN:"); field(&l, fn));
    LINE(jb_raw(&l, "N:"); field(&l, js(n, "family")); jb_raw(&l, ";"); field(&l, js(n, "given")); jb_raw(&l, ";"); field(&l, js(n, "middle"));
         jb_raw(&l, ";"); field(&l, js(n, "prefix")); jb_raw(&l, ";"); field(&l, js(n, "suffix")));
    if (*js(c, "nick")) LINE(jb_raw(&l, "NICKNAME:"); field(&l, js(c, "nick")));
    if (*js(c, "org") || *js(c, "dept")) LINE(jb_raw(&l, "ORG:"); field(&l, js(c, "org")); if (*js(c, "dept")) { jb_raw(&l, ";"); field(&l, js(c, "dept")); });
    if (*js(c, "title")) LINE(jb_raw(&l, "TITLE:"); field(&l, js(c, "title")));
    const JVal *arr;
    if ((arr = json_get(c, "emails")) && arr->type == J_ARR) for (const JVal *it = arr->kid; it; it = it->next) typed_line(&b, "EMAIL", it, "INTERNET");
    if ((arr = json_get(c, "phones")) && arr->type == J_ARR) for (const JVal *it = arr->kid; it; it = it->next) typed_line(&b, "TEL", it, NULL);
    if ((arr = json_get(c, "urls")) && arr->type == J_ARR) for (const JVal *it = arr->kid; it; it = it->next) typed_line(&b, "URL", it, NULL);
    if ((arr = json_get(c, "im")) && arr->type == J_ARR) for (const JVal *it = arr->kid; it; it = it->next) typed_line(&b, "IMPP", it, NULL);
    if ((arr = json_get(c, "addrs")) && arr->type == J_ARR) {
        for (const JVal *it = arr->kid; it; it = it->next) {
            char t[40];
            size_t o = 0;
            for (const char *x = js(it, "type"); *x && o + 1 < sizeof t; x++) if (isalnum((unsigned char)*x)) t[o++] = (char)toupper((unsigned char)*x);
            t[o] = 0;
            LINE(jb_raw(&l, "ADR"); if (t[0]) { jb_raw(&l, ";TYPE="); jb_raw(&l, t); } jb_raw(&l, ":;;"); field(&l, js(it, "street")); jb_raw(&l, ";");
                 field(&l, js(it, "city")); jb_raw(&l, ";"); field(&l, js(it, "region")); jb_raw(&l, ";"); field(&l, js(it, "code")); jb_raw(&l, ";");
                 field(&l, js(it, "country")));
        }
    }
    const char *bd = js(c, "bday");
    if (strlen(bd) == 10 && bd[4] == '-' && bd[7] == '-' && isdigit((unsigned char)bd[0])) jb_printf(&b, "BDAY;VALUE=date:%.10s\r\n", bd);
    if (*js(c, "notes")) LINE(jb_raw(&l, "NOTE:"); field(&l, js(c, "notes")));
    if ((arr = json_get(c, "cats")) && arr->type == J_ARR && arr->kid) {
        LINE(jb_raw(&l, "CATEGORIES:"); int f = 1; for (const JVal *it = arr->kid; it; it = it->next) if (it->type == J_STR) { if (!f) jb_raw(&l, ","); field(&l, it->str); f = 0; });
    }
    if (!strcmp(js(c, "kind"), "group")) {
        jb_raw(&b, "X-ADDRESSBOOKSERVER-KIND:group\r\n");
        if ((arr = json_get(c, "members")) && arr->type == J_ARR)
            for (const JVal *it = arr->kid; it; it = it->next) if (it->type == J_STR) LINE(jb_raw(&l, "X-ADDRESSBOOKSERVER-MEMBER:urn:uuid:"); field(&l, it->str));
    }
    if (photo && plen && plen <= VCARD_MAX_PHOTO) {
        size_t el;
        char *e = b64_encode(photo, plen, 0, &el);
        if (e) {
            char t[16] = "JPEG";
            if (ptype && !strcasecmp(ptype, "png")) snprintf(t, sizeof t, "PNG");
            JBuf pl;
            jb_init(&pl);
            jb_printf(&pl, "PHOTO;ENCODING=b;TYPE=%s:", t);
            jb_raw(&pl, e);
            if (!pl.oom) ic_line(&b, pl.p);
            jb_free(&pl);
            free(e);
        }
    }
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    jb_printf(&b, "REV:%04d%02d%02dT%02d%02d%02dZ\r\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    jb_raw(&b, "END:VCARD\r\n");
#undef LINE
    if (b.oom) { jb_free(&b); return NULL; }
    if (olen) *olen = b.n;
    return b.p;
}
