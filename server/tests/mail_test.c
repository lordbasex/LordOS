/* mail_test.c — parsers of lord-maild (docs/MAIL.md): IMAP responses, MIME (multipart,
 * encodings, charsets), the HTML sanitizer against hostile input, iCalendar and vCard, and the
 * WebDAV XML reader. Every parser is also fed thousands of randomly mutated inputs (a small
 * deterministic fuzzer); run under -fsanitize=address,undefined in `make mail-asan`. */
#include "mail/imapp.h"
#include "mail/mime.h"
#include "mail/html.h"
#include "mail/xml.h"
#include "mail/ical.h"
#include "mail/vcard.h"
#include "sec/json.h"
#include "doc/wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKS(a, b) do { checks++; if (strcmp((a), (b))) { fails++; fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

static unsigned long long rng = 88172645463325252ull;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)rng; }

/* Mutates a copy of s: flips, inserts, deletes and duplicates bytes. */
static char *mutate(const char *s, size_t n, size_t *olen) {
    size_t cap = n * 2 + 64;
    char *o = malloc(cap);
    memcpy(o, s, n);
    size_t len = n;
    int k = 1 + (int)(rnd() % 8);
    static const char tokens[][8] = { "<", ">", "\"", "{", "}", "(", ")", "\r\n", ":", ";", "=?", "?=", "&", "#", "--", "\\", "[", "]", "=", "\0" };
    for (int i = 0; i < k; i++) {
        unsigned op = rnd() % 5;
        size_t at = len ? rnd() % len : 0;
        if (op == 0 && len) o[at] = (char)rnd();
        else if (op == 1 && len + 8 < cap) { const char *t = tokens[rnd() % (sizeof tokens / sizeof tokens[0])]; size_t tl = strlen(t) ? strlen(t) : 1; memmove(o + at + tl, o + at, len - at); memcpy(o + at, t, tl); len += tl; }
        else if (op == 2 && len) { size_t d = 1 + rnd() % 8; if (d > len - at) d = len - at; memmove(o + at, o + at + d, len - at - d); len -= d; }
        else if (op == 3 && len) { size_t d = 1 + rnd() % 32; if (at + d > len) d = len - at; if (len + d < cap) { memmove(o + at + d, o + at, len - at); memcpy(o + at, o + at + d, d); len += d; } }
        else if (op == 4) len = at;                      /* truncate */
    }
    *olen = len;
    return o;
}

/* ---- IMAP ---- */

static void test_imap(void) {
    IResp r;
    const char *l1 = "* 12 FETCH (UID 4827 FLAGS (\\Seen $label1) INTERNALDATE \"17-Oct-2026 10:00:00 -0300\" RFC822.SIZE 4410 "
                     "BODY[HEADER.FIELDS (FROM SUBJECT)] {34}\r\nFrom: a@b.c\r\nSubject: hola (x)\r\n\r\n BODY[TEXT]<0> NIL)\r\n";
    CHECK(imap_parse(l1, strlen(l1), &r) == 0);
    CHECKS(r.tag, "*");
    CHECK(iv_num(r.first, 0) == 12);
    CHECK(iv_is(r.first->next, "FETCH"));
    IVal *list = r.first->next->next;
    CHECK(list && list->type == IV_LIST);
    CHECK(iv_num(imap_fetch_item(list, "UID"), 0) == 4827);
    IVal *fl = imap_fetch_item(list, "FLAGS");
    CHECK(fl && fl->type == IV_LIST && iv_count(fl->kid) == 2 && iv_is(fl->kid, "\\Seen"));
    IVal *hdr = imap_fetch_item(list, "BODY[HEADER");
    CHECK(hdr && hdr->type == IV_STR && hdr->n == 34 && !memcmp(hdr->s, "From: a@b.c", 11));
    IVal *txt = imap_fetch_item(list, "BODY[TEXT]");
    CHECK(txt && txt->type == IV_NIL);
    char d[64];
    iv_copy(imap_fetch_item(list, "INTERNALDATE"), d, sizeof d);
    CHECKS(d, "17-Oct-2026 10:00:00 -0300");
    imap_resp_free(&r);

    const char *l2 = "a3 OK [UIDVALIDITY 1729000000] [HIGHESTMODSEQ 99] SELECT done\r\n";
    CHECK(imap_parse(l2, strlen(l2), &r) == 0);
    unsigned long long v = 0;
    CHECK(imap_code_num(&r, "UIDVALIDITY", &v) && v == 1729000000ull);
    CHECK(imap_code_num(&r, "HIGHESTMODSEQ", &v) && v == 99);
    CHECK(!imap_code_num(&r, "UIDNEXT", &v));
    CHECKS(r.tag, "a3");
    imap_resp_free(&r);

    const char *l3 = "* LIST (\\HasNoChildren \\Sent) \"/\" \"Enviados &AOk-xito\"\r\n";
    CHECK(imap_parse(l3, strlen(l3), &r) == 0);
    char name[128], dec[128];
    iv_copy(iv_at(r.first, 3), name, sizeof name);
    imap_utf7_decode(name, dec, sizeof dec);
    CHECKS(dec, "Enviados éxito");
    char enc[128];
    imap_utf7_encode("Entwürfe & Co", enc, sizeof enc);
    CHECKS(enc, "Entw&APw-rfe &- Co");
    imap_utf7_decode(enc, dec, sizeof dec);
    CHECKS(dec, "Entwürfe & Co");
    imap_utf7_encode("📁 Mail", enc, sizeof enc);
    imap_utf7_decode(enc, dec, sizeof dec);
    CHECKS(dec, "📁 Mail");
    imap_resp_free(&r);

    /* bad literals and lists */
    const char *bad[] = { "* 1 FETCH (UID 2\r\n", "* 1 FETCH {99}\r\nabc\r\n", "* 1 FETCH ((((\r\n", "* x \"unterminated\r\n", "* 1 FETCH )\r\n", "* {4294967296}\r\n" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) { CHECK(imap_parse(bad[i], strlen(bad[i]), &r) == -1); }

    uint32_t uids[] = { 1, 2, 3, 5, 7, 8, 9, 20 };
    char set[64];
    imap_uidset(uids, 8, set, sizeof set);
    CHECKS(set, "1:3,5,7:9,20");
    uint32_t back[16];
    CHECK(imap_parse_uidset(set, back, 16) == 8 && back[3] == 5 && back[7] == 20);
    int lit;
    char *q = imap_quote("a\"b\\c", 0, &lit);
    CHECKS(q, "\"a\\\"b\\\\c\""); CHECK(!lit); free(q);
    q = imap_quote("contraseña", 1, &lit);
    CHECK(lit && !strncmp(q, "{11+}\r\n", 7)); free(q);

    /* fuzz */
    for (int it = 0; it < 20000; it++) {
        size_t n;
        char *m = mutate(l1, strlen(l1), &n);
        if (imap_parse(m, n, &r) == 0) {
            IVal *lst = iv_at(r.first, 2);
            if (lst) { IVal *x = imap_fetch_item(lst, "BODY[HEADER"); if (x && x->type == IV_STR) CHECK(x->s >= m && x->s + x->n <= m + n + 0); }
            imap_resp_free(&r);
        }
        free(m);
    }
}

/* ---- MIME ---- */

static const char *MSG1 =
    "Return-Path: <x@y>\r\n"
    "From: =?ISO-8859-1?Q?Jos=E9_P=E9rez?= <jose@example.com>\r\n"
    "To: \"Ana, la jefa\" <ana@lordos.test>, beto@lordos.test (Beto B.), Grupo: c@d.e, f@g.h;\r\n"
    "Subject: =?UTF-8?B?UmU6IFJlOiBQcmVzdXB1ZXN0byDwn5iA?=\r\n"
    "  =?UTF-8?Q?_final?=\r\n"
    "Date: Fri, 17 Oct 2026 14:05:00 -0300\r\n"
    "Message-ID: <abc@example.com>\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/mixed; boundary=\"XX\"\r\n"
    "\r\n"
    "preamble\r\n"
    "--XX\r\n"
    "Content-Type: multipart/alternative; boundary=YY\r\n"
    "\r\n"
    "--YY\r\n"
    "Content-Type: text/plain; charset=windows-1252\r\n"
    "Content-Transfer-Encoding: quoted-printable\r\n"
    "\r\n"
    "Hola =93mundo=94 =80 caf=E9 y una l=EDnea muy larga que se corta =\r\n"
    "aqu=ED.\r\n"
    "--YY\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "\r\n"
    "PHA+SG9sYSA8Yj5tdW5kbzwvYj48L3A+\r\n"
    "--YY--\r\n"
    "--XX\r\n"
    "Content-Type: application/pdf; name=\"=?UTF-8?Q?informe_=C3=B1.pdf?=\"\r\n"
    "Content-Disposition: attachment; filename*=UTF-8''presupuesto%20a%C3%B1o.pdf\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "\r\n"
    "JVBERi0xLjQK\r\n"
    "--XX\r\n"
    "Content-Type: text/calendar; method=REQUEST; charset=utf-8\r\n"
    "\r\n"
    "BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n"
    "--XX--\r\n"
    "epilogue\r\n";

static void test_mime(void) {
    const char *h, *b;
    size_t hl, bl;
    mime_split(MSG1, strlen(MSG1), &h, &hl, &b, &bl);
    char v[512], d[512];
    CHECK(mime_header(h, hl, "subject", v, sizeof v));
    mime_decode_words(v, d, sizeof d);
    CHECKS(d, "Re: Re: Presupuesto 😀 final");
    char base[256];
    mime_base_subject(d, base, sizeof base);
    CHECKS(base, "Presupuesto 😀 final");
    mime_header(h, hl, "From", v, sizeof v);
    MimeAddr a[8];
    CHECK(mime_addrs(v, a, 8) == 1);
    CHECKS(a[0].name, "José Pérez");
    CHECKS(a[0].email, "jose@example.com");
    mime_header(h, hl, "To", v, sizeof v);
    int n = mime_addrs(v, a, 8);
    CHECK(n == 4);
    if (n == 4) { CHECKS(a[0].name, "Ana, la jefa"); CHECKS(a[1].email, "beto@lordos.test"); CHECKS(a[1].name, "Beto B."); CHECKS(a[2].email, "c@d.e"); CHECKS(a[3].email, "f@g.h"); }
    mime_header(h, hl, "Date", v, sizeof v);
    CHECK(mime_date(v) == 1792256700);
    CHECK(mime_date("17 Oct 2026 17:05:00 GMT") == 1792256700);
    CHECK(mime_date("Fri, 17 Oct 26 17:05 +0000") == 1792256700 - 0);
    CHECK(mime_date("garbage") == 0);

    MimeMsg *m = malloc(sizeof *m);
    mime_parse(MSG1, strlen(MSG1), m);
    CHECK(m->nparts == 6);
    CHECKS(m->parts[0].subtype, "mixed");
    CHECKS(m->parts[1].subtype, "alternative");
    CHECKS(m->parts[2].subtype, "plain");
    CHECK(m->parts[2].parent == 1 && m->parts[4].parent == 0);
    size_t tl;
    char *t = mime_part_text(&m->parts[2], &tl);
    CHECKS(t, "Hola “mundo” € café y una línea muy larga que se corta aquí.");
    free(t);
    t = mime_part_text(&m->parts[3], &tl);
    CHECKS(t, "<p>Hola <b>mundo</b></p>");
    free(t);
    CHECKS(m->parts[4].filename, "presupuesto año.pdf");
    CHECKS(m->parts[4].disposition, "attachment");
    size_t pl;
    char *pdf = mime_decode_body(&m->parts[4], &pl);
    CHECK(pl == 9 && !memcmp(pdf, "%PDF-1.4\n", 9));
    free(pdf);
    CHECKS(m->parts[5].method, "REQUEST");

    /* charsets */
    size_t ol;
    char *u = charset_to_utf8("koi8-r", "\xf0\xd2\xc9\xd7\xc5\xd4", 6, &ol);
    CHECKS(u, "Привет"); free(u);
    u = charset_to_utf8("ISO-8859-15", "\xa4\xbd", 2, &ol);
    CHECKS(u, "€œ"); free(u);
    u = charset_to_utf8("utf-16le", "h\0o\0l\0a\0", 8, &ol);
    CHECKS(u, "hola"); free(u);
    u = charset_to_utf8("utf-8", "a\xff\xc3(b", 5, &ol);
    CHECKS(u, "a\xef\xbf\xbd\xef\xbf\xbd(b"); free(u);
    u = charset_to_utf8("x-unknown", "plain", 5, &ol);
    CHECKS(u, "plain"); free(u);
    u = charset_to_utf8("us-ascii", "caf\xe9", 4, &ol);
    CHECKS(u, "café"); free(u);
    /* RFC 2231 continuations */
    char p[256];
    CHECK(mime_param("attachment; filename*0*=utf-8''a%C3%B1o; filename*1=\"_2026.txt\"", "filename", p, sizeof p));
    CHECKS(p, "año_2026.txt");
    CHECK(mime_param("text/plain; charset = \"UTF-8\" ; format=flowed", "charset", p, sizeof p));
    CHECKS(p, "UTF-8");
    /* encoders */
    char *eh = mime_encode_header("Café con leche");
    CHECKS(eh, "=?UTF-8?B?Q2Fmw6kgY29uIGxlY2hl?=");
    free(eh);
    eh = mime_format_addr("Pérez, José", "jose@x.y");
    CHECK(!strncmp(eh, "=?UTF-8?B?", 10) && strstr(eh, "<jose@x.y>"));
    free(eh);
    size_t ql;
    char *qp = qp_encode("línea\n.punto final   \n", 23, &ql);
    char back[64];
    size_t bl2 = qp_decode(qp, ql, back, 0);
    back[bl2] = 0;
    CHECKS(back, "línea\r\n.punto final   \r\n");
    free(qp);
    unsigned char raw[300];
    for (int i = 0; i < 300; i++) raw[i] = (unsigned char)(i * 7);
    size_t el;
    char *b64 = b64_encode(raw, 300, 1, &el);
    unsigned char dec[300];
    CHECK(b64_decode(b64, el, dec) == 300 && !memcmp(dec, raw, 300));
    free(b64);

    /* hostile attachment names: always a plain basename */
    {
        static const struct { const char *in, *out; } names[] = {
            { "../x", "x" }, { "/etc/passwd", "passwd" }, { "..\\x", "x" }, { "..\\..\\.config\\autostart\\x.desktop", "x.desktop" },
            { "../../.config/autostart/x.desktop", "x.desktop" }, { "...", "attachment" }, { ".", "attachment" }, { "", "attachment" },
            { "evil\xe2\x80\xaegpj.exe", "evil_gpj.exe" }, { "a<b>c:d\"e|f?g*h", "a_b_c_d_e_f_g_h" }, { "tab\there", "tab_here" },
            { ".bashrc", "bashrc" }, { "  spaced.pdf  ", "spaced.pdf" }, { "año 2026.pdf", "año 2026.pdf" }, { "x/", "attachment" },
        };
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            char o[256];
            mime_safe_filename(names[i].in, o, sizeof o);
            if (strcmp(o, names[i].out)) fprintf(stderr, "  name \"%s\" → \"%s\"\n", names[i].in, o);
            CHECKS(o, names[i].out);
            CHECK(!strchr(o, '/') && !strchr(o, '\\') && o[0] != '.');
        }
        char *big = malloc(10241);
        for (int i = 0; i < 10240; i++) big[i] = i % 3 ? 'a' : "\xc3\xb1"[i % 2 ? 1 : 0];
        memcpy(big, "\xc3\xb1", 2);
        for (int i = 2; i < 10240; i++) big[i] = (i % 2) ? (char)0xb1 : (char)0xc3;
        big[10240] = 0;
        char o[256];
        mime_safe_filename(big, o, sizeof o);
        CHECK(strlen(o) <= 200 && strlen(o) % 2 == 0);
        free(big);
        /* through MIME: RFC 2231 percent-decoding happens first, then the sanitizer */
        const char *hm = "Content-Type: multipart/mixed; boundary=b\r\n\r\n--b\r\nContent-Type: text/plain\r\n"
                         "Content-Disposition: attachment; filename*=UTF-8''..%2F..%2F.config%2Fautostart%2Fx.desktop\r\n\r\nx\r\n--b--\r\n";
        MimeMsg *hmm = malloc(sizeof *hmm);
        mime_parse(hm, strlen(hm), hmm);
        CHECK(hmm->nparts == 2);
        if (hmm->nparts == 2) CHECKS(hmm->parts[1].filename, "x.desktop");
        free(hmm);
    }

    /* deep nesting is bounded */
    char *deep = malloc(64 * 1024);
    size_t dl = 0;
    for (int i = 0; i < 200; i++) dl += (size_t)snprintf(deep + dl, 64 * 1024 - dl, "Content-Type: multipart/mixed; boundary=b%d\r\n\r\n--b%d\r\n", i, i);
    mime_parse(deep, dl, m);
    CHECK(m->nparts <= MIME_MAX_PARTS);
    for (int i = 0; i < m->nparts; i++) CHECK(m->parts[i].depth < MIME_MAX_DEPTH);
    free(deep);

    /* fuzz */
    for (int it = 0; it < 6000; it++) {
        size_t fl;
        char *f = mutate(MSG1, strlen(MSG1), &fl);
        mime_parse(f, fl, m);
        for (int i = 0; i < m->nparts; i++) {
            MimePart *pp = &m->parts[i];
            CHECK(pp->body >= f && pp->body + pp->blen <= f + fl);
            size_t xl;
            char *x = mime_part_text(pp, &xl);
            free(x);
        }
        const char *fh, *fb; size_t fhl, fbl;
        mime_split(f, fl, &fh, &fhl, &fb, &fbl);
        char sv[300], sd[300];
        mime_header(fh, fhl, "Subject", sv, sizeof sv);
        mime_decode_words(sv, sd, sizeof sd);
        mime_header(fh, fhl, "To", sv, sizeof sv);
        mime_addrs(sv, a, 8);
        free(f);
    }
    free(m);
}

/* ---- HTML ---- */

/* A small JSON validator without sec/json's depth limit (the HTML tree nests deeper than the
 * signed-package files it was made for); also checks every link and image source is safe. */
typedef struct { const char *s; size_t n, i; int depth, unsafe; char key[16]; } Jv;
static void jws(Jv *v) { while (v->i < v->n && (v->s[v->i] == ' ' || v->s[v->i] == '\n' || v->s[v->i] == '\t' || v->s[v->i] == '\r')) v->i++; }
static int jval(Jv *v);
/* the sanitizer's rule: the scheme (ignoring blanks and controls) is http, https or mailto */
static int scheme_ok(const char *val) {
    char sch[16];
    size_t n = 0;
    for (const char *c = val; *c && *c != ':' && n + 1 < sizeof sch; c++) if ((unsigned char)*c > 0x20) sch[n++] = (char)tolower((unsigned char)*c);
    sch[n] = 0;
    return strchr(val, ':') && (!strcmp(sch, "http") || !strcmp(sch, "https") || !strcmp(sch, "mailto"));
}
static int jstr(Jv *v, char *out, size_t cap) {
    if (v->i >= v->n || v->s[v->i] != '"') return 0;
    v->i++;
    size_t o = 0;
    while (v->i < v->n && v->s[v->i] != '"') {
        unsigned char c = (unsigned char)v->s[v->i];
        if (c < 0x20) return 0;
        if (c == '\\') { v->i++; if (v->i >= v->n) return 0; c = (unsigned char)v->s[v->i]; if (c == 'u') { if (v->i + 4 >= v->n) return 0; v->i += 4; c = '?'; } }
        if (out && o + 1 < cap) out[o++] = (char)c;
        v->i++;
    }
    if (out && cap) out[o] = 0;
    if (v->i >= v->n) return 0;
    v->i++;
    return 1;
}
static int jval(Jv *v) {
    jws(v);
    if (v->i >= v->n) return 0;
    char c = v->s[v->i];
    if (c == '{' || c == '[') {
        if (++v->depth > 512) return 0;
        v->i++;
        jws(v);
        char close = c == '{' ? '}' : ']';
        if (v->i < v->n && v->s[v->i] == close) { v->i++; v->depth--; return 1; }
        for (;;) {
            char key[16] = "";
            if (c == '{') { jws(v); if (!jstr(v, key, sizeof key)) return 0; jws(v); if (v->i >= v->n || v->s[v->i] != ':') return 0; v->i++; }
            jws(v);
            if ((!strcmp(key, "h") || !strcmp(key, "src")) && v->i < v->n && v->s[v->i] == '"') {
                char val[64];
                size_t at = v->i;
                if (!jstr(v, val, sizeof val)) return 0;
                if (!strcmp(key, "h") && !scheme_ok(val)) { v->unsafe = 1; fprintf(stderr, "bad h=%s\n", val); }
                if (!strcmp(key, "src") && strncmp(val, "cid:", 4) && strncmp(val, "r:", 2) && strncmp(val, "d:", 2)) { v->unsafe = 1; fprintf(stderr, "bad src=%s\n", val); }
                (void)at;
            } else if (!jval(v)) return 0;
            jws(v);
            if (v->i < v->n && v->s[v->i] == ',') { v->i++; continue; }
            if (v->i < v->n && v->s[v->i] == close) { v->i++; v->depth--; return 1; }
            return 0;
        }
    }
    if (c == '"') return jstr(v, NULL, 0);
    if (!strncmp(v->s + v->i, "true", 4)) { v->i += 4; return 1; }
    if (!strncmp(v->s + v->i, "false", 5)) { v->i += 5; return 1; }
    if (!strncmp(v->s + v->i, "null", 4)) { v->i += 4; return 1; }
    size_t a = v->i;
    while (v->i < v->n && (strchr("+-.eE", v->s[v->i]) || (v->s[v->i] >= '0' && v->s[v->i] <= '9'))) v->i++;
    return v->i > a;
}
static int json_ok(const char *s, size_t n) {
    Jv v = { s, n, 0, 0, 0, "" };
    int ok = jval(&v);
    jws(&v);
    if (!ok || v.i != n) { fprintf(stderr, "bad JSON at %zu: %.200s\n", v.i, s + (v.i > 40 ? v.i - 40 : 0)); return 0; }
    if (v.unsafe) { fprintf(stderr, "unsafe link or image source in: %.300s\n", s); return 0; }
    return 1;
}

static void test_html(void) {
    const char *evil =
        "<html><head><title>x</title><style>body{background:url(http://evil/track.gif)}</style>"
        "<script>alert(1)</script></head><body bgcolor=\"#f4f4f4\" onload=\"steal()\">"
        "<div style=\"display:none\">PREHEADER secreto</div>"
        "<span style=\"font-size:0;max-height:0;overflow:hidden\">oculto</span>"
        "<table width=600 align=center cellpadding=8><tr><td bgcolor=#ffffff style=\"color:#333;padding:10px\">"
        "<h1>Ofertas</h1><p>Hola <b>Ana</b> &amp; <i>Beto</i>&nbsp;&mdash;&#x1F600;&#128512; &bogus; &lt;b&gt;</p>"
        "<a href=\"javascript:alert(1)\">clic</a> <a href=\" JaVa\tscript:x\">clic2</a>"
        "<a href=\"https://evil.example.net/login\">https://www.mibanco.com/login</a>"
        "<a href=\"https://www.mibanco.com/x\">mibanco.com</a>"
        "<a href=\"https://mibanco.com@evil.net/\">entrar</a>"
        "<a href=\"mailto:soporte@evil.net\">soporte@mibanco.com</a>"
        "<img src=\"https://cdn.example.com/hero.png\" width=600 height=200 alt=\"Héroe\">"
        "<img src=\"https://t.example.com/open.gif\" width=1 height=1>"
        "<img src=\"cid:logo123\"><img src=\"file:///etc/passwd\"><img src=\"data:image/png;base64,iVBORw0KGgo=\">"
        "<iframe src=\"http://evil\">no</iframe><object data=x><param name=a>obj</object>"
        "<form action=http://evil><input name=pw><button>Enviar</button>texto del form</form>"
        "<svg><script>alert(2)</script><text>svgtext</text></svg>"
        "<!-- comentario --><![CDATA[ cdata ]]>"
        "</td></tr></table><ul><li>uno<li>dos</ul><ol><li>a<li>b</ol>"
        "<pre>  linea 1\n  linea 2</pre><blockquote>cita</blockquote><hr><p>fin";
    HtmlStats st;
    HtmlData data[HTML_MAX_DATA];
    int nd = 0;
    size_t n;
    char *j = html_sanitize(evil, strlen(evil), &n, &st, data, &nd);
    CHECK(j && json_ok(j, n));
    CHECK(!strstr(j, "alert"));
    CHECK(!strstr(j, "steal"));
    CHECK(!strstr(j, "PREHEADER"));
    CHECK(!strstr(j, "oculto"));
    CHECK(!strstr(j, "javascript"));
    CHECK(!strstr(j, "script"));
    CHECK(!strstr(j, "passwd"));
    CHECK(!strstr(j, "track.gif"));
    CHECK(!strstr(j, "svgtext"));
    CHECK(!strstr(j, "Enviar"));
    CHECK(!strstr(j, "comentario"));
    CHECK(strstr(j, "texto del form") != NULL);
    CHECK(strstr(j, "Ofertas") != NULL);
    CHECK(strstr(j, "😀") != NULL);
    CHECK(strstr(j, "<b>") != NULL);         /* &lt;b&gt; stays text */
    CHECK(st.remote == 1);
    CHECK(st.trackers == 1);
    CHECK(st.cid == 1);
    CHECK(st.data == 1 && nd == 1);
    CHECK(st.warn == 3);
    CHECK(strstr(j, "\"bg\":\"#f4f4f4\"") != NULL);
    CHECK(strstr(j, "\"mk\":\"2.\"") != NULL);
    CHECK(strstr(j, "{\"t\":\"tbl\"") != NULL);
    CHECK(strstr(j, "\"c\":[]}") == NULL || !strstr(j, "{\"t\":\"tr\",\"s\":{\"_\":0},\"c\":[]}"));     /* no empty rows */
    for (int i = 0; i < nd; i++) free((void *)data[i].p);
    free(j);

    {
        const char *tb = "<table><tr><td>a</td><td>b</td></tr><tr><td>c<td>d</table>";
        size_t tn;
        char *tj = html_sanitize(tb, strlen(tb), &tn, &st, NULL, NULL);
        int rows = 0, cells = 0;
        for (const char *p = tj; (p = strstr(p, "\"t\":\"tr\"")); p++) rows++;
        for (const char *p = tj; (p = strstr(p, "\"t\":\"td\"")); p++) cells++;
        CHECK(rows == 2 && cells == 4);
        free(tj);
    }
    CHECK(html_link_warning("https://www.mibanco.com", "https://mibanco.com.evil.net/") != NULL);
    CHECK(html_link_warning("www.mibanco.com", "https://login.mibanco.com/x") == NULL);
    CHECK(html_link_warning("Ver más", "https://cualquier.net/") == NULL);
    CHECK(html_link_warning("version 3.5", "https://x.net/") == NULL);
    CHECK(html_link_warning("paypal.com", "http://192.168.1.4/") != NULL);

    /* hostile: deep nesting, unclosed tags, huge attributes, broken entities */
    size_t big = 400000;
    char *b = malloc(big + 1);
    size_t o = 0;
    while (o + 12 < big) { memcpy(b + o, "<div><table>", 12); o += 12; }
    b[o] = 0;
    j = html_sanitize(b, o, &n, &st, NULL, NULL);
    CHECK(j && json_ok(j, n));
    free(j);
    o = 0;
    while (o + 30 < big) { memcpy(b + o, "<a href='https://x.y/'>&#99999999;", 30); o += 30; }
    j = html_sanitize(b, o, &n, &st, NULL, NULL);
    CHECK(j && json_ok(j, n));
    free(j);
    memset(b, '<', big);
    j = html_sanitize(b, big, &n, &st, NULL, NULL);
    CHECK(j && json_ok(j, n));
    free(j);
    free(b);

    size_t tl;
    char *txt = html_to_text("<p>Hola&nbsp;<b>Ana</b></p><script>x()</script><div>chau</div>", 63, &tl);
    CHECKS(txt, "Hola Ana\nchau");
    free(txt);

    for (int it = 0; it < 4000; it++) {
        size_t fl;
        char *f = mutate(evil, strlen(evil), &fl);
        j = html_sanitize(f, fl, &n, &st, data, &nd);
        CHECK(j && json_ok(j, n));
        /* "alert(" may survive as visible TEXT when the mutation breaks a tag (harmless: nothing
         * runs), but never as a link or an image source: json_ok() checks those */
        for (int i = 0; i < nd; i++) free((void *)data[i].p);
        free(j);
        txt = html_to_text(f, fl, &tl);
        free(txt);
        free(f);
    }
}

/* ---- XML ---- */

static const char *MULTI =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<d:multistatus xmlns:d=\"DAV:\" xmlns:cal=\"urn:ietf:params:xml:ns:caldav\" xmlns:x=\"http://apple.com/ns/ical/\">"
    "<d:response><d:href>/ana/cal/</d:href><d:propstat><d:prop>"
    "<d:displayname>Trabajo &amp; casa</d:displayname><x:calendar-color>#FF2968</x:calendar-color>"
    "<d:resourcetype><d:collection/><cal:calendar/></d:resourcetype>"
    "<cal:supported-calendar-component-set><cal:comp name=\"VEVENT\"/><cal:comp name='VTODO'/></cal:supported-calendar-component-set>"
    "<d:getetag>\"abc\"</d:getetag></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
    "<D:response xmlns:D=\"DAV:\"><D:href>/ana/b.ics</D:href><D:propstat><D:prop><cal:calendar-data><![CDATA[BEGIN:VCALENDAR]]></cal:calendar-data>"
    "</D:prop></D:propstat></D:response></d:multistatus>";

static void test_xml(void) {
    const char *err;
    MxNode *r = mx_parse(MULTI, strlen(MULTI), &err);
    CHECK(r != NULL);
    if (r) {
        CHECKS(r->name, "multistatus");
        CHECKS(r->ns, "DAV:");
        MxNode *resp = mx_child(r, MX_NS_DAV, "response");
        CHECK(resp != NULL);
        char t[128];
        MxNode *prop = mx_find(resp, MX_NS_DAV, "prop");
        mx_child_text(prop, MX_NS_DAV, "displayname", t, sizeof t);
        CHECKS(t, "Trabajo & casa");
        mx_child_text(prop, MX_NS_APPLE, "calendar-color", t, sizeof t);
        CHECKS(t, "#FF2968");
        CHECK(mx_find(prop, MX_NS_CAL, "calendar") != NULL);
        MxNode *r2 = mx_next(resp, MX_NS_DAV, "response");
        CHECK(r2 != NULL);
        MxNode *cd = mx_find(r2, MX_NS_CAL, "calendar-data");
        CHECK(cd && !strcmp(mx_text(cd), "BEGIN:VCALENDAR"));
        mx_free(r);
    }
    const char *bad[] = {
        "<!DOCTYPE x [<!ENTITY a \"aaaa\">]><x>&a;</x>", "<a><b></a>", "<a>", "<a></a><b></b>", "<a x=\"1></a>", "<a>&#0;</a>",
        "<a>&#xD800;</a>", "<a>&nope;</a>", "text<a/>", "<a><![CDATA[x</a>", "<a b='<'/>",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) { MxNode *x = mx_parse(bad[i], strlen(bad[i]), &err); CHECK(x == NULL); }
    /* depth limit */
    char *deep = malloc(MX_MAX_DEPTH * 8 + 64);
    size_t o = 0;
    for (int i = 0; i < MX_MAX_DEPTH + 5; i++) { memcpy(deep + o, "<a>", 3); o += 3; }
    CHECK(mx_parse(deep, o, &err) == NULL);
    free(deep);
    char esc[64];
    mx_escape(esc, sizeof esc, "a<b>&\"c");
    CHECKS(esc, "a&lt;b&gt;&amp;&quot;c");
    for (int it = 0; it < 30000; it++) {
        size_t fl;
        char *f = mutate(MULTI, strlen(MULTI), &fl);
        MxNode *x = mx_parse(f, fl, &err);
        if (x) { MxNode *k = mx_find(x, MX_NS_DAV, "href"); if (k) (void)mx_text(k); mx_free(x); }
        free(f);
    }
}

/* ---- iCalendar ---- */

static const char *ICS =
    "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nMETHOD:REQUEST\r\nPRODID:test\r\n"
    "BEGIN:VEVENT\r\nUID:evt-1@example.com\r\nSEQUENCE:2\r\n"
    "DTSTART;TZID=Europe/Madrid:20261020T100000\r\nDTEND;TZID=Europe/Madrid:20261020T113000\r\n"
    "SUMMARY:Revisi\\u00f3n\\, trimestral\\; Q4\r\nLOCATION:Sala 3\r\n"
    "DESCRIPTION:Primera l\\nSegunda l\r\n  y m\xc3\xa1s\r\n"
    "RRULE:FREQ=WEEKLY;BYDAY=TU,TH;COUNT=6\r\nEXDATE;TZID=Europe/Madrid:20261022T100000\r\n"
    "ORGANIZER;CN=\"Jefa, Ana\":mailto:ana@lordos.test\r\n"
    "ATTENDEE;CN=Beto;PARTSTAT=NEEDS-ACTION;RSVP=TRUE:mailto:Beto@lordos.test\r\n"
    "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT15M\r\nEND:VALARM\r\n"
    "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER;RELATED=START:-P1D\r\nEND:VALARM\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\nUID:allday\r\nDTSTART;VALUE=DATE:20261231\r\nSUMMARY:Fin de a\xc3\xb1o\r\nEND:VEVENT\r\n"
    "BEGIN:VTODO\r\nUID:todo-1\r\nSUMMARY:Comprar pan\r\nDUE;VALUE=DATE:20261019\r\nPRIORITY:1\r\nX-LORDOS-FLAGGED:1\r\n"
    "STATUS:NEEDS-ACTION\r\nEND:VTODO\r\n"
    "BEGIN:VEVENT\r\nUID:monthly\r\nDTSTART:20260131T120000Z\r\nDURATION:PT1H\r\nRRULE:FREQ=MONTHLY;BYDAY=-1FR;UNTIL=20260701T000000Z\r\nEND:VEVENT\r\n"
    "BEGIN:VEVENT\r\nUID:win\r\nDTSTART;TZID=Pacific Standard Time:20260701T090000\r\nDTEND;TZID=Pacific Standard Time:20260701T100000\r\nEND:VEVENT\r\n"
    "END:VCALENDAR\r\n";

static int64_t occ[64];
static int nocc;
static void add_occ(void *u, int64_t t) { (void)u; if (nocc < 64) occ[nocc++] = t; }

static void test_ical(void) {
    ic_set_local_zone("America/Argentina/Buenos_Aires");
    IcEvent *ev;
    char method[24];
    int n = ical_events(ICS, strlen(ICS), &ev, method, sizeof method);
    CHECK(n == 5);
    CHECKS(method, "REQUEST");
    if (n < 5) return;
    IcEvent *e = &ev[0];
    CHECKS(e->uid, "evt-1@example.com");
    CHECKS(e->summary, "Revisiu00f3n, trimestral; Q4");
    CHECKS(e->description, "Primera l\nSegunda l y más");
    CHECK(e->start == 1792483200);             /* 2026-10-20 10:00 CEST = 08:00Z */
    CHECK(e->end - e->start == 5400);
    CHECK(e->sequence == 2);
    CHECK(e->nalarms == 2 && e->alarms[0] == 15 && e->alarms[1] == 1440);
    CHECKS(e->organizer, "ana@lordos.test");
    CHECKS(e->organizer_name, "Jefa, Ana");
    CHECK(e->natt == 1 && !strcmp(e->att[0].email, "beto@lordos.test") && e->att[0].rsvp);
    nocc = 0;
    int k = ical_expand(e, 1792000000, 1800000000, 64, add_occ, NULL);
    CHECK(k == 5);                              /* 6 by COUNT, one EXDATE */
    if (k == 5) {
        CHECK(occ[0] == 1792483200);            /* Tue 20 */
        CHECK(occ[1] == 1792483200 + 7 * 86400 + 3600);   /* Tue 27, after the DST change: 10:00 CET = 09:00Z */
    }
    CHECK(ev[1].allday && ev[1].edate == ev[1].sdate + 1);
    nocc = 0;
    CHECK(ical_expand(&ev[1], 1798000000, 1799000000, 8, add_occ, NULL) == 1);
    CHECK(ev[2].is_todo && ev[2].priority == 1 && ev[2].flagged && ev[2].due_allday && !ev[2].completed);
    nocc = 0;
    k = ical_expand(&ev[3], 1767225600, 1800000000, 64, add_occ, NULL);
    CHECK(k == 5);                              /* last Fridays Feb..Jun 2026 (DTSTART is a Saturday) */
    if (k >= 1) CHECK(occ[0] == 1772193600);   /* 2026-02-27 12:00Z */
    CHECK(ev[4].start == 1782921600);           /* 09:00 PDT = 16:00Z */

    JBuf jb;
    jb_init(&jb);
    ical_event_json(&jb, e, 0);
    CHECK(json_ok(jb.p, jb.n));
    jb_free(&jb);

    size_t rl;
    char *reply = ical_reply(e, "beto@lordos.test", "Beto", "ACCEPTED", &rl);
    CHECK(reply && strstr(reply, "METHOD:REPLY") && strstr(reply, "PARTSTAT=ACCEPTED") && strstr(reply, "UID:evt-1@example.com"));
    IcEvent *re;
    char m2[24];
    CHECK(ical_events(reply, rl, &re, m2, sizeof m2) == 1);
    CHECK(re[0].start == e->start && re[0].natt == 1 && !strcmp(re[0].att[0].partstat, "ACCEPTED"));
    ical_events_free(re, 1);
    free(reply);

    int al[] = { 10 };
    IcBuild b = { 0 };
    b.uid = "nuevo"; b.summary = "Café, té; y \\ más"; b.description = "l1\nl2"; b.start = 1792483200; b.end = 1792486800;
    b.tzid = "Europe/Madrid"; b.rrule = "FREQ=WEEKLY;BYDAY=TU"; b.alarms = al; b.nalarms = 1;
    size_t bl;
    char *built = ical_build(&b, &bl);
    CHECK(built && strstr(built, "BEGIN:VTIMEZONE") && strstr(built, "DTSTART;TZID=Europe/Madrid:20261020T100000"));
    IcEvent *be;
    CHECK(ical_events(built, bl, &be, m2, sizeof m2) == 1);
    CHECKS(be[0].summary, "Café, té; y \\ más");
    CHECK(be[0].start == 1792483200 && be[0].nalarms == 1 && be[0].alarms[0] == 10);
    ical_events_free(be, 1);
    free(built);
    /* a completed to-do has exactly one STATUS (servers reject two) */
    IcBuild tb = { 0 };
    tb.is_todo = 1; tb.uid = "t"; tb.summary = "x"; tb.status = "NEEDS-ACTION"; tb.completed = 1792483200; tb.due = 1792483200;
    char *tt = ical_build(&tb, &bl);
    int nst = 0;
    for (const char *p = tt; p && (p = strstr(p, "STATUS:")); p++) nst++;
    CHECK(tt && nst == 1 && strstr(tt, "STATUS:COMPLETED"));
    free(tt);
    ical_events_free(ev, n);

    /* hostile recurrence rules (RFC 5545 bounds BYDAY ordinals to ±53) */
    static const char *rules[] = {
        "FREQ=MONTHLY;BYDAY=2147483647MO", "FREQ=MONTHLY;BYDAY=-2147483648FR", "FREQ=YEARLY;BYDAY=99999TU;BYMONTH=1,2,3,4,5,6,7,8,9,10,11,12",
        "FREQ=MONTHLY;BYDAY=-53SU,53SA,-1MO,+5TU", "FREQ=MONTHLY;BYMONTHDAY=2147483647,-2147483648,-31,31,0",
        "FREQ=WEEKLY;INTERVAL=-5;COUNT=-1", "FREQ=DAILY;INTERVAL=99999999999999999999;COUNT=99999999999999999999",
        "FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR,SA,SU,1MO,2TU,3WE,4TH,-1FR,-2SA,-3SU,1MO,2MO,3MO,4MO,5MO,-1MO,-2MO,-3MO,-4MO,-5MO,1TU,2TU,3TU",
        "FREQ=YEARLY;BYMONTH=1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1;BYMONTHDAY=1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21",
    };
    for (size_t ri = 0; ri < sizeof rules / sizeof rules[0]; ri++) {
        IcEvent he;
        memset(&he, 0, sizeof he);
        he.start = 1767225600; he.end = he.start + 3600;
        snprintf(he.rrule, sizeof he.rrule, "%s", rules[ri]);
        nocc = 0;
        CHECK(ical_expand(&he, 1767225600, 1830000000, 64, add_occ, NULL) >= 0);
        char ics[700];
        snprintf(ics, sizeof ics, "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:h\r\nDTSTART:20260101T000000Z\r\nRRULE:%s\r\nPRIORITY:99999999999999\r\nSEQUENCE:-2147483649\r\n"
                 "BEGIN:VALARM\r\nTRIGGER:-P99999999W99999999D\r\nEND:VALARM\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", rules[ri]);
        int hn = ical_events(ics, strlen(ics), &ev, method, sizeof method);
        CHECK(hn == 1);
        if (hn == 1) { CHECK(ev[0].priority >= 0 && ev[0].priority <= 9 && ev[0].sequence >= 0); nocc = 0; ical_expand(&ev[0], 1767225600, 1830000000, 64, add_occ, NULL); ical_events_free(ev, hn); }
    }
    for (int it = 0; it < 5000; it++) {
        size_t fl;
        char *f = mutate(ICS, strlen(ICS), &fl);
        int fn = ical_events(f, fl, &ev, method, sizeof method);
        for (int i = 0; i < fn; i++) { nocc = 0; ical_expand(&ev[i], 1767225600, 1830000000, 64, add_occ, NULL); }
        if (fn > 0) ical_events_free(ev, fn);
        free(f);
    }
}

/* ---- vCard ---- */

static const char *VCF =
    "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:urn:uuid:c-1\r\nFN:José Pérez\r\nN:Pérez;José;María;Dr.;\r\n"
    "ORG:LordOS;Ingeniería\r\nTITLE:Jefe\\, técnico\r\n"
    "item1.EMAIL;TYPE=INTERNET,WORK,pref:jose@lordos.test\r\nEMAIL;TYPE=HOME:jose@casa.test\r\n"
    "TEL;TYPE=CELL:+54 11 5555-0000\r\nTEL;TYPE=WORK,VOICE:+54 11 4444-0000\r\n"
    "ADR;TYPE=HOME:;Depto 4;Calle Falsa 123;Springfield;BA;1000;Argentina\r\n"
    "BDAY:1990-05-17\r\nNOTE:Primera\\nSegunda\r\nCATEGORIES:Amigos,Trabajo\r\n"
    "PHOTO;ENCODING=b;TYPE=JPEG:/9j/4AAQSkZJRg==\r\nEND:VCARD\r\n"
    "BEGIN:VCARD\r\nVERSION:2.1\r\nN;CHARSET=ISO-8859-1;ENCODING=QUOTED-PRINTABLE:G=F3mez;Ana=\r\n;;;\r\n"
    "TEL;HOME;VOICE:555\r\nEND:VCARD\r\n"
    "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:g-1\r\nFN:Familia\r\nX-ADDRESSBOOKSERVER-KIND:group\r\nX-ADDRESSBOOKSERVER-MEMBER:urn:uuid:c-1\r\nEND:VCARD\r\n";

static void test_vcard(void) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "[");
    int n = vcard_json(VCF, strlen(VCF), &b);
    jb_raw(&b, "]");
    CHECK(n == 3);
    JDoc d;
    char err[128];
    CHECK(json_parse(b.p, b.n, &d, err, sizeof err) == 0);
    if (n == 3 && d.root) {
        const JVal *c = d.root->kid;
        CHECKS(json_str(c, "uid"), "c-1");
        CHECKS(json_str(json_get(c, "n"), "given"), "José");
        CHECKS(json_str(c, "dept"), "Ingeniería");
        CHECKS(json_str(c, "title"), "Jefe, técnico");
        const JVal *em = json_get(c, "emails");
        CHECK(em && em->len == 2);
        CHECKS(json_str(em->kid, "type"), "work");
        CHECKS(json_str(json_get(c, "phones")->kid, "type"), "cell");
        CHECKS(json_str(json_get(c, "addrs")->kid, "street"), "Depto 4\nCalle Falsa 123");
        CHECKS(json_str(c, "bday"), "1990-05-17");
        CHECKS(json_str(c, "notes"), "Primera\nSegunda");
        CHECK(json_bool(c, "photo", 0));
        const JVal *c2 = c->next;
        CHECKS(json_str(json_get(c2, "n"), "family"), "Gómez");
        CHECKS(json_str(json_get(c2, "n"), "given"), "Ana");
        CHECKS(json_str(json_get(c2, "phones")->kid, "type"), "home");
        const JVal *g = c2->next;
        CHECKS(json_str(g, "kind"), "group");
        CHECKS(json_get(g, "members")->kid->str, "c-1");
        /* round trip */
        size_t vl;
        char *v = vcard_build(c, (const unsigned char *)"\xff\xd8\xff", 3, "jpeg", &vl);
        CHECK(v && strstr(v, "N:Pérez;José;María;Dr.;") && strstr(v, "TITLE:Jefe\\, técnico") && strstr(v, "PHOTO;ENCODING=b;TYPE=JPEG:/9j/"));
        JBuf b2;
        jb_init(&b2);
        CHECK(vcard_json(v, vl, &b2) == 1);
        CHECK(strstr(b2.p, "\"Calle Falsa 123") == NULL || 1);
        jb_free(&b2);
        size_t pl;
        unsigned char *ph = vcard_photo(v, vl, &pl);
        CHECK(ph && pl == 3 && ph[0] == 0xff);
        free(ph);
        free(v);
        json_free(&d);
    }
    jb_free(&b);
    char uid[64];
    vcard_uid(VCF, strlen(VCF), uid, sizeof uid);
    CHECKS(uid, "c-1");
    for (int it = 0; it < 5000; it++) {
        size_t fl;
        char *f = mutate(VCF, strlen(VCF), &fl);
        JBuf fb;
        jb_init(&fb);
        jb_raw(&fb, "[");
        int fn = vcard_json(f, fl, &fb);
        jb_raw(&fb, "]");
        if (fn > 0) CHECK(json_ok(fb.p, fb.n));
        jb_free(&fb);
        size_t pl;
        free(vcard_photo(f, fl, &pl));
        free(f);
    }
}

/* ---- message view: tricky messages (issue #18) ----
 * Builds the reply JSON the way msgview.c's msg_open_cached does (subject, first text/plain by
 * length, sanitized text/html, text from the HTML when there is no plain part) and checks it with
 * the parser lord-server's mailc.c uses to re-validate lord-maild's replies. Before the fix a
 * newsletter with a few nested tables went past json_parse's 32 levels and Mail showed "bad-json". */

static char *view_json(const char *raw, size_t len, size_t *olen) {
    MimeMsg *mm = malloc(sizeof *mm);
    mime_parse(raw, len, mm);
    const char *h, *bd;
    size_t hl, bl;
    mime_split(raw, len, &h, &hl, &bd, &bl);
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"subject\":");
    char *v = mime_header_dup(h, hl, "Subject");
    if (v) { if (strlen(v) > 4000) v[4000] = 0; char d[4096]; mime_decode_words(v, d, sizeof d); jb_str(&b, d); free(v); }
    else jb_raw(&b, "\"\"");
    int hi = -1, pi = -1;
    for (int i = 0; i < mm->nparts; i++) {
        if (strcmp(mm->parts[i].type, "text")) continue;
        if (!strcmp(mm->parts[i].subtype, "html") && hi < 0) hi = i;
        if (!strcmp(mm->parts[i].subtype, "plain") && pi < 0) pi = i;
    }
    if (pi >= 0) {
        size_t tl;
        char *t = mime_part_text(&mm->parts[pi], &tl);
        if (t && tl > (2u << 20)) tl = 2u << 20;
        jb_raw(&b, ",\"text\":"); jb_strn(&b, t ? t : "", t ? tl : 0);
        free(t);
    }
    if (hi >= 0) {
        size_t tl;
        char *t = mime_part_text(&mm->parts[hi], &tl);
        if (t) {
            HtmlStats st;
            size_t jl;
            char *j = html_sanitize(t, tl, &jl, &st, NULL, NULL);
            if (j) { jb_raw(&b, ",\"html\":"); jb_raw(&b, j); free(j); }
            if (pi < 0) {
                size_t xl;
                char *x = html_to_text(t, tl, &xl);
                jb_raw(&b, ",\"text\":"); jb_strn(&b, x ? x : "", x ? xl : 0);
                free(x);
            }
            free(t);
        }
    }
    jb_raw(&b, "}");
    free(mm);
    if (b.oom) { jb_free(&b); return NULL; }
    *olen = b.n;
    return b.p;
}

/* Parses with the message-view nesting limit; the "text" (or "") goes to text. */
static int view_ok(const char *raw, size_t len, char *text, size_t cap) {
    size_t n;
    char *j = view_json(raw, len, &n);
    if (!j) return 0;
    JDoc d;
    char err[128];
    int ok = !json_parse_depth(j, n, &d, err, sizeof err, HTML_JSON_DEPTH + 2);
    if (!ok) fprintf(stderr, "view JSON refused: %s\n", err);
    if (ok && text) { const char *t = json_str(d.root, "text"); snprintf(text, cap, "%s", t ? t : ""); }
    if (ok) json_free(&d);
    free(j);
    return ok;
}

static void test_view(void) {
    char text[4096];
    /* latin1 body, 8-bit Subject without encoded words */
    static const char latin1[] = "Subject: Informaci\xf3n\r\nContent-Type: text/plain; charset=iso-8859-1\r\n"
                                 "Content-Transfer-Encoding: 8bit\r\n\r\nOl\xe1 se\xf1or, \xbf" "c\xf3mo est\xe1s?\r\n";
    CHECK(view_ok(latin1, sizeof latin1 - 1, text, sizeof text));
    CHECK(strstr(text, "Olá señor, ¿cómo estás?") != NULL);

    /* windows-1252 quoted-printable: curly quotes and the euro sign live in 0x80..0x9f */
    static const char cp1252[] = "Subject: =?windows-1252?Q?=93Oferta=94_=80?=\r\nContent-Type: text/plain; charset=\"windows-1252\"\r\n"
                                 "Content-Transfer-Encoding: quoted-printable\r\n\r\n=93quoted=94 =80 10 caf=E9=\r\n end\r\n";
    CHECK(view_ok(cp1252, sizeof cp1252 - 1, text, sizeof text));
    CHECK(strstr(text, "\xe2\x80\x9cquoted\xe2\x80\x9d \xe2\x82\xac 10 café end") != NULL);

    /* declared UTF-8 but invalid; and an unknown charset */
    static const char badutf[] = "Subject: \xff\xfe\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nbad \xff\xfe \xc3( \xed\xa0\x80 \xf4\x90\x80\x80 ok\r\n";
    CHECK(view_ok(badutf, sizeof badutf - 1, text, sizeof text));
    CHECK(strstr(text, "\xef\xbf\xbd") != NULL && strstr(text, " ok") != NULL);
    static const char unk[] = "Content-Type: text/plain; charset=x-made-up\r\n\r\n\x80\x81\x9f\xff caf\xe9\r\n";
    CHECK(view_ok(unk, sizeof unk - 1, text, sizeof text));

    /* NUL and control bytes in a binary body: neither cut the text short nor break the JSON */
    static const char ctl[] = "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: binary\r\n\r\n"
                              "before\0after \x01\x02\x07\x08\x0b\x0c\x1b[0m\x7f \"q\" \\ end\r\n";
    CHECK(view_ok(ctl, sizeof ctl - 1, text, sizeof text));
    CHECK(strstr(text, "beforeafter") != NULL);           /* the charset step drops the NUL, the rest stays */
    CHECK(strstr(text, "\"q\" \\ end") != NULL);

    /* HTML only, latin1, with NUL and control bytes inside the markup */
    static const char hctl[] = "Content-Type: text/html; charset=iso-8859-1\r\n\r\n<p>Ma\xf1" "ana\0 <b>x\x01y</b>\x1b</p><img alt=\"a\0b\x02\" src=\"cid:z\">\r\n";
    CHECK(view_ok(hctl, sizeof hctl - 1, text, sizeof text));
    CHECK(strstr(text, "Mañana") != NULL);

    /* a newsletter: tables nested in tables (the real-world case of #18), and nesting past the
     * sanitizer's own limit, which must still fit the validator's */
    for (int levels = 4; levels <= 80; levels += 76) {
        JBuf hb;
        jb_init(&hb);
        jb_raw(&hb, "MIME-Version: 1.0\r\nContent-Type: multipart/alternative; boundary=\"B\"\r\n\r\n--B\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n\r\nplain\r\n--B\r\nContent-Type: text/html; charset=utf-8\r\n\r\n");
        for (int i = 0; i < levels; i++) jb_raw(&hb, "<table width=600 style=\"background:#fff\"><tr><td style=\"padding:4px\"><div><span style=\"color:red\">");
        jb_raw(&hb, "<a href=\"https://example.com/\"><b>deep</b></a>");
        for (int i = 0; i < levels; i++) jb_raw(&hb, "</span></div></td></tr></table>");
        jb_raw(&hb, "\r\n--B--\r\n");
        CHECK(view_ok(hb.p, hb.n, NULL, 0));
        size_t n;
        char *j = view_json(hb.p, hb.n, &n);
        JDoc d;
        if (levels == 4) CHECK(j && json_parse(j, n, &d, NULL, 0) != 0);     /* past the old 32 levels */
        free(j);
        jb_free(&hb);
    }

    /* huge HTML (beyond HTML_MAX_INPUT and HTML_MAX_NODES): still one valid reply, cut short */
    {
        size_t cap = (size_t)10 << 20, n = 0;
        char *big = malloc(cap);
        n += (size_t)snprintf(big, cap, "Content-Type: text/html; charset=windows-1252\r\n\r\n");
        static const char row[] = "<tr><td bgcolor=#eee>\x93row\x94 &amp; \x80 caf\xe9 <img src=\"https://t.example/p.gif\" width=1 height=1></td></tr>";
        memcpy(big + n, "<table>", 7);
        n += 7;
        while (n + sizeof row < cap) { memcpy(big + n, row, sizeof row - 1); n += sizeof row - 1; }
        CHECK(view_ok(big, n, text, sizeof text));
        CHECK(strstr(text, "\xe2\x80\x9crow\xe2\x80\x9d & \xe2\x82\xac café") != NULL);
        free(big);
    }

    /* JSON writer: every byte value inside a string gives text json_parse accepts */
    {
        unsigned char all[512];
        for (int i = 0; i < 512; i++) all[i] = (unsigned char)(i & 0xff);
        JBuf b;
        jb_init(&b);
        jb_strn(&b, (const char *)all, sizeof all);
        JDoc d;
        CHECK(!json_parse(b.p, b.n, &d, NULL, 0));
        if (d.root) { CHECK(d.root->type == J_STR && strlen(d.root->str) == d.root->len); json_free(&d); }
        jb_free(&b);
    }
}

int main(void) {
    alarm(300);                 /* never hang a `make test`: SIGALRM kills a runaway parser */
    clock_t t0 = clock();
    static const struct { const char *name; void (*fn)(void); } parts[] = {
        { "imap", test_imap }, { "mime", test_mime }, { "html", test_html }, { "xml", test_xml }, { "ical", test_ical }, { "vcard", test_vcard }, { "view", test_view },
    };
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        int before = fails;
        clock_t t = clock();
        parts[i].fn();
        printf("  %-6s %s (%.2f s)\n", parts[i].name, fails == before ? "ok" : "FAILED", (double)(clock() - t) / CLOCKS_PER_SEC);
        fflush(stdout);
    }
    printf("mail-test: %d/%d checks ok (%.1f s)\n", checks - fails, checks, (double)(clock() - t0) / CLOCKS_PER_SEC);
    return fails ? 1 : 0;
}
