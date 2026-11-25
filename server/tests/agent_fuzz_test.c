/* agent_fuzz_test.c — the agent channel's parsers (vm/vmagent.c) under structured mutation (docs/VIRTUALIZATION.md 8).
 *
 * Both ends of the virtio-serial channel run these parsers on what the other end sent: lord-vm and lord-server on the
 * guest's frames (a hostile guest), lordos-guest on the host's. vm-test feeds most of them random bytes; random bytes
 * rarely get past a frame's first check, so this pass starts from well-formed payloads made by the writers (and by hand
 * where there is no writer) and changes them: bytes flipped, set to the edges (0, 0x7f, 0x80, 0xff), lengths cut and
 * grown, fields swapped between frames. Every parser, including the ones vm-test has no pass for (AGK_CLIPX, the frame
 * splitter, LAG_WINDOWS and LAG_APPLIST as parsed, the shell's frames, AGK_DISPLAY, AGK_TABLET, AGK_EXTRA, AGK_PERF,
 * AGK_PERFSTATE, AGK_STOPPING, LAG_APPS, LAG_LAUNCH, LAG_WINREFUSED). Under ASan and UBSan; checks that what a parser
 * takes stays inside the payload and is what it promises (pointers and lengths in bounds, text checked, sizes in their
 * limits), and that a stream of frames cut anywhere reassembles to the same frames. */
#include "../src/vm/vmagent.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t rs = 0x5eed1234abcdULL;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint32_t below(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }
static int chance(int pct) { return (int)below(100) < pct; }

/* a pointer the parser handed back lies inside [p, p + n) with its length */
static int inside(const uint8_t *p, size_t n, const uint8_t *q, size_t qn) { return !qn || (q && q >= p && (size_t)(q - p) <= n && qn <= n - (size_t)(q - p)); }

static size_t mutate(uint8_t *p, size_t n, size_t cap) {
    static const uint8_t E[] = { 0, 1, 0x7f, 0x80, 0xff, 0xfe, 0x20, 0x0a };
    int k = (int)below(4);
    while (k-- > 0 && n) {
        size_t i = below((uint32_t)n);
        if (chance(50)) p[i] ^= (uint8_t)(1u << below(8));
        else p[i] = E[below(sizeof E)];
    }
    if (n && chance(15)) n = below((uint32_t)n + 1);
    if (chance(10)) { size_t g = 1 + below(8); if (n + g <= cap) { for (size_t i = 0; i < g; i++) p[n + i] = (uint8_t)rnd(); n += g; } }
    if (n >= 8 && chance(10)) { size_t a = below((uint32_t)n - 4), b = below((uint32_t)n - 4); uint8_t t[4]; memcpy(t, p + a, 4); memcpy(p + a, p + b, 4); memcpy(p + b, t, 4); }
    return n;
}

static size_t text(uint8_t *p, size_t cap) {
    size_t n = below((uint32_t)cap);
    for (size_t i = 0; i < n; i++) p[i] = chance(90) ? (uint8_t)(0x20 + below(95)) : (uint8_t)rnd();
    return n;
}

#define CAP 300000
static uint8_t buf[CAP], tmp[CAP];

/* the seeds: a well-formed payload of each kind */
static size_t seed(int kind, uint8_t *b) {
    switch (kind) {
    case 0: {               /* AGK_CLIPX */
        static uint8_t t[200], h[200], png[64];
        AgClip c = { 0 };
        if (chance(80)) { c.text = t; c.text_n = (uint32_t)text(t, sizeof t); }
        if (chance(50)) { c.html = h; c.html_n = (uint32_t)text(h, sizeof h); }
        if (chance(30)) { for (size_t i = 0; i < sizeof png; i++) png[i] = (uint8_t)rnd(); memcpy(png, "\x89PNG\r\n\x1a\n", 8); c.png = png; c.png_n = sizeof png; }
        ag_clipx_write(&c, b);
        return ag_clipx_size(&c);
    }
    case 1: { LagWinList *l = calloc(1, sizeof *l); l->serial = (uint32_t)rnd(); l->n = (int)below(6);
              for (int i = 0; i < l->n; i++) { LagWinEntry *e = &l->w[i]; e->id = below(80); e->flags = below(8); e->x = (int32_t)below(3000) - 50; e->y = (int32_t)below(2000) - 50;
                  e->w = 1 + (int32_t)below(2000); e->h = 1 + (int32_t)below(2000); size_t t = text((uint8_t *)e->title, 40); e->title[t] = 0; snprintf(e->app, sizeof e->app, "a%u", below(9)); }
              size_t n = lag_windows_write(l, b, CAP); free(l); return n; }
    case 2: { LagAppList *l = calloc(1, sizeof *l); l->n = (int)below(6);
              for (int i = 0; i < l->n; i++) { size_t t = text((uint8_t *)l->a[i].name, 30); l->a[i].name[t] = 0; snprintf(l->a[i].token, sizeof l->a[i].token, "t%u.desktop", below(9)); }
              size_t n = lag_applist_write(l, b, CAP); free(l); return n; }
    case 3: { static uint8_t px[4 * 16 * 16]; for (size_t i = 0; i < sizeof px; i++) px[i] = (uint8_t)rnd(); return lag_icon_write(below(80), 1 + (int)below(16), 1 + (int)below(16), px, b, CAP); }
    case 4: { LagAct a = { below(80), (uint8_t)below(6), (int32_t)below(3000), (int32_t)below(2000), 1 + (int32_t)below(2000), 1 + (int32_t)below(2000) }; return lag_act_write(&a, b, CAP); }
    case 5: return lag_launch_write("app.desktop", b, CAP);
    case 6: { uint32_t id = below(4), k = (uint32_t)text(b + 4, 300); memcpy(b, &id, 4); return 4 + k; }            /* AGK_SHELL_DATA */
    case 7: return ag_shell_size_write(b, below(4), 1 + (int)below(300), 1 + (int)below(200)) ? 0 : 8;
    case 8: { uint32_t v[2] = { below(4), (uint32_t)rnd() }; memcpy(b, v, 8); return 8; }                            /* ACK, CLOSE */
    case 9: ag_display_write(b, 320 + below(4000), 200 + below(3000)); return 8;
    case 10: { AgDisplay d[3]; int n = 1 + (int)below(3); memset(d, 0, sizeof d);
               for (int i = 0; i < n; i++) { d[i].x = (int32_t)(i * 1920); d[i].w = 640 + below(3000); d[i].h = 480 + below(2000); snprintf(d[i].name, sizeof d[i].name, "D%d", i); }
               return ag_displays_write(d, n, (int)below((uint32_t)n), b, CAP); }
    case 11: b[0] = (uint8_t)below(3); return 1;                                                                     /* PERF, EXTRA, TABLET, STOPPING, APPS */
    case 12: return ag_perfstate_write(b, (int)below(2), below(8), below(8)) ? 0 : AG_PERFSTATE_BYTES;
    case 13: return lag_winrefused_write(b, below(80), (uint8_t)below(6), (uint8_t)below(6)) ? 0 : LAG_WINREFUSED_BYTES;
    case 14: { uint8_t t[130], bo[300]; size_t tn = text(t, sizeof t), bn = text(bo, sizeof bo); return ag_note_write(t, tn, bo, bn, b, CAP); }
    case 15: { AgSendOffer o; memset(&o, 0, sizeof o); o.id = 1 + below(9); o.size = rnd() % (AG_SEND_SIZE_MAX + 2); o.flags = (uint8_t)below(4);
               size_t t = text((uint8_t *)o.name, 1 + below(AG_NAME_MAX)); o.name[t] = 0; if (!t) { o.name[0] = 'a'; o.name[1] = 0; }
               size_t k = ag_send_offer_write(&o, b, CAP); if (k) return k; memcpy(b, &o, 47); return 47 + text(b + 47, 200); }
    case 16: {              /* a PNG header, then IHDR: ag_png_check */
               static const uint8_t png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 1, 0, 0, 0, 0, 0x80, 8, 6, 0, 0, 0, 1, 2, 3, 4 };
               memcpy(b, png, sizeof png); return sizeof png; }
    case 17: {              /* HTML: ag_html_sanitize */
               static const char *const tags[] = { "<b>", "</b>", "<script>", "</script>", "<a href=\"javascript:x\">", "<img src=x onerror=y>", "<!--", "-->", "<p style=\"x\">", "&amp;", "&#x41;", "<", ">", "\"", "<![CDATA[", "<style>", "</i>", "<a href=\"https://x\" title=\"a on=b\" onclick=z>", "<span style=\"color: red; background:url(x)\">", "<td colspan=2 align=left>", "<a href=mailto:x>", "<font face=\"Sans\" ONload=x>" };
               size_t n = 0; int k = 1 + (int)below(20);
               for (int i = 0; i < k && n < 900; i++) { const char *t = chance(50) ? tags[below(sizeof tags / sizeof *tags)] : "text \xc3\xa9 "; size_t l = strlen(t); memcpy(b + n, t, l); n += l; }
               return n; }
    default: { const char *h = "lordos-guest 10 clipboard clipx clipreq windows perf shell links notify send"; size_t n = strlen(h); memcpy(b, h, n); return n; }
    }
}

static void parse_all(const uint8_t *p, size_t n) {
    AgClip c;
    if (!ag_clipx_parse(p, n, &c)) {
        CHECK(inside(p, n, c.text, c.text_n) && inside(p, n, c.html, c.html_n) && inside(p, n, c.png, c.png_n));
        CHECK(c.text_n <= AG_TEXT_MAX && c.html_n <= AG_TEXT_MAX && c.png_n <= AG_PNG_MAX);
        CHECK(ag_clipx_size(&c) == n);
    }
    static LagWin w[LAG_WIN_MAX];
    uint32_t serial;
    int nw = lag_windows_parse(p, n, &serial, w, LAG_WIN_MAX);
    CHECK(nw >= -1 && nw <= LAG_WIN_MAX);
    for (int i = 0; i < nw; i++) CHECK(inside(p, n, w[i].title, w[i].title_n) && inside(p, n, w[i].app, w[i].app_n) && w[i].title_n <= LAG_TITLE_MAX && w[i].app_n <= LAG_APP_MAX);
    LagWinList *wl = malloc(sizeof *wl);
    if (!lag_winlist_read(p, n, wl)) {
        CHECK(wl->n >= 0 && wl->n <= LAG_WIN_MAX);
        for (int i = 0; i < wl->n; i++) CHECK(lag_text_ok((const uint8_t *)wl->w[i].title, strlen(wl->w[i].title)) && lag_rect_ok(wl->w[i].x, wl->w[i].y, wl->w[i].w, wl->w[i].h));
        size_t k = lag_windows_write(wl, tmp, sizeof tmp);          /* written again, it reads the same */
        LagWinList *w2 = malloc(sizeof *w2);
        CHECK(k && !lag_winlist_read(tmp, k, w2) && w2->n == wl->n && w2->serial == wl->serial);
        free(w2);
    }
    free(wl);
    static LagApp a[LAG_APPS_MAX];
    int na = lag_applist_parse(p, n, a, LAG_APPS_MAX);
    CHECK(na >= -1 && na <= LAG_APPS_MAX);
    for (int i = 0; i < na; i++) CHECK(inside(p, n, a[i].name, a[i].name_n) && inside(p, n, a[i].token, a[i].token_n) && a[i].token_n <= LAG_TOKEN_MAX);
    LagAppList *al = malloc(sizeof *al);
    if (!lag_applist_read(p, n, al)) {
        size_t k = lag_applist_write(al, tmp, sizeof tmp);
        LagAppList *a2 = malloc(sizeof *a2);
        CHECK(k && !lag_applist_read(tmp, k, a2) && a2->n == al->n);
        for (int i = 0; i < al->n; i++) CHECK(lag_applist_has(al, al->a[i].token));
        free(a2);
    }
    free(al);
    uint32_t id, u; uint16_t iw, ih; const uint8_t *px, *d; uint32_t dn; int cols, rows, st, on, mode, off; unsigned d1, d2;
    if (!lag_winicon_parse(p, n, &id, &iw, &ih, &px)) CHECK(iw && ih && iw <= LAG_ICON_MAX && ih <= LAG_ICON_MAX && inside(p, n, px, (size_t)iw * ih * 4));
    LagAct act;
    if (!lag_winact_parse(p, n, &act)) { size_t k = lag_act_write(&act, tmp, sizeof tmp); LagAct b2; CHECK(k && !lag_winact_parse(tmp, k, &b2) && b2.id == act.id && b2.action == act.action); }
    const uint8_t *tok; uint16_t tn;
    if (!lag_launch_parse(p, n, &tok, &tn)) CHECK(inside(p, n, tok, tn) && tn <= LAG_TOKEN_MAX);
    char token[LAG_TOKEN_MAX + 1];
    if (!lag_launch_token(p, n, token, sizeof token)) CHECK(strlen(token) <= LAG_TOKEN_MAX && lag_text_ok((const uint8_t *)token, strlen(token)));
    if (!ag_shell_data_parse(p, n, &id, &d, &dn)) CHECK(inside(p, n, d, dn));
    if (!ag_shell_size_parse(p, n, &id, &cols, &rows)) CHECK(cols > 0 && rows > 0);
    if (!ag_shell_ack_parse(p, n, &id, &u)) CHECK(n == 8);
    if (!ag_shell_close_parse(p, n, &id, &st)) CHECK(n == 8);
    uint32_t dw, dh;
    if (!ag_display_parse(p, n, &dw, &dh)) CHECK(dw && dh && dw <= 16384 && dh <= 16384);
    AgDisplay ds[AG_DISPLAYS_MAX];
    int shown = -1;
    int nd = ag_displays_parse(p, n, ds, &shown);
    CHECK(nd >= -1 && nd <= AG_DISPLAYS_MAX);
    if (nd > 0) {
        CHECK(shown >= -1 && shown < nd);                       /* -1: the guest shows none it knows */
        int L[6];
        if (!ag_displays_layout(ds, nd, shown, L)) CHECK(L[2] > 0 && L[3] > 0 && L[0] >= 0 && L[1] >= 0 && L[0] + L[2] <= L[4] && L[1] + L[3] <= L[5]);
        for (int k = 0; k < 2; k++) if (!ag_tablet_layout(ds, nd, shown, k ? AG_TABLET_DESKTOP : AG_TABLET_PRIMARY, L)) CHECK(L[2] > 0 && L[3] > 0 && L[0] + L[2] <= L[4] && L[1] + L[3] <= L[5]);
        char line[512];
        ag_displays_text(ds, nd, shown, line, sizeof line);
        CHECK(strlen(line) < sizeof line);
    }
    if (!ag_extra_parse(p, n, &off)) CHECK(off == 0 || off == 1);
    if (!ag_perf_parse(p, n, &on)) CHECK(on == 0 || on == 1);
    if (!ag_perfstate_parse(p, n, &on, &d1, &d2)) CHECK(on == 0 || on == 1);
    if (!ag_stopping_parse(p, n, &mode)) CHECK(mode >= 1 && mode <= 4);
    if (!ag_tablet_parse(p, n, &mode)) CHECK(mode >= 0);
    if (!lag_apps_parse(p, n, &on)) CHECK(on == 0 || on == 1);
    uint8_t action, reason;
    if (!lag_winrefused_parse(p, n, &id, &action, &reason)) CHECK(n == LAG_WINREFUSED_BYTES);
    const uint8_t *t, *bo; uint16_t ttn, bn;
    if (!ag_note_parse(p, n, &t, &ttn, &bo, &bn)) CHECK(inside(p, n, t, ttn) && inside(p, n, bo, bn) && ttn <= AG_NOTE_TITLE_MAX && bn <= AG_NOTE_BODY_MAX);
    AgSendOffer so;
    if (!ag_send_offer_parse(p, n, &so)) {
        CHECK(so.id && so.size <= AG_SEND_SIZE_MAX && strlen(so.name) <= AG_NAME_MAX && ag_send_name_ok((const uint8_t *)so.name, strlen(so.name)));
        size_t k = ag_send_offer_write(&so, tmp, sizeof tmp);
        AgSendOffer s2;
        CHECK(k == n && !ag_send_offer_parse(tmp, k, &s2) && !strcmp(s2.name, so.name));
    }
    /* names: a received file's (ag_recv_name) and a dropped one's (ag_drop_name), from any bytes */
    {
        char in[600], out[AG_NAME_MAX + 1];
        size_t l = n < sizeof in - 1 ? n : sizeof in - 1;
        memcpy(in, p, l); in[l] = 0;
        if (!ag_drop_name(in, out, sizeof out)) CHECK(out[0] && strlen(out) <= AG_NAME_MAX && !strchr(out, '/') && !strchr(out, '\\') && strcmp(out, ".") && strcmp(out, ".."));
        if (!ag_recv_name(in, out, sizeof out)) CHECK(out[0] && out[0] != '.' && strlen(out) <= AG_NAME_MAX && !strchr(out, '/') && !strchr(out, '\\'));
        char small[8];
        if (!ag_drop_name(in, small, sizeof small)) CHECK(strlen(small) < sizeof small);
    }
    uint32_t pw, ph;
    if (!ag_png_check(p, n, &pw, &ph)) CHECK(pw >= 1 && ph >= 1 && pw <= AG_PNG_DIM && ph <= AG_PNG_DIM);
    if (ag_utf8_ok(p, n)) {
        static char hs[4096];
        long hl = ag_html_sanitize((const char *)p, n, hs, sizeof hs);
        CHECK(hl >= -1 && hl < (long)sizeof hs);
        if (hl >= 0) { CHECK(strlen(hs) == (size_t)hl && ag_utf8_ok((const uint8_t *)hs, (size_t)hl)); CHECK(!strstr(hs, "<script") && !strstr(hs, "<style") && !strstr(hs, "<img"));
            /* inside every tag kept: no event handler (an attribute named on...), no script URL, no url() or expression() */
            int tags_ok = 1;
            for (const char *q = strchr(hs, '<'); q; q = strchr(q + 1, '<')) {
                const char *e = strchr(q, '>');
                if (!e) { tags_ok = 0; break; }
                char tg[1024]; size_t tl = (size_t)(e - q) < sizeof tg - 1 ? (size_t)(e - q) : sizeof tg - 1;
                for (size_t i = 0; i < tl; i++) tg[i] = (char)(q[i] >= 'A' && q[i] <= 'Z' ? q[i] + 32 : q[i]);
                tg[tl] = 0;
                /* attributes as the sanitizer writes them: name="value" (the value escaped, so no '"' inside) */
                const char *r = tg + 1;
                while (*r && *r != ' ') r++;
                while (*r == ' ') {
                    r++;
                    const char *nm = r;
                    while (*r && *r != '=' && *r != ' ') r++;
                    size_t nl = (size_t)(r - nm);
                    if (*r != '=' || r[1] != '"') { tags_ok = 0; break; }
                    const char *v = r + 2, *ve = strchr(v, '"');
                    if (!ve) { tags_ok = 0; break; }
                    if (nl >= 2 && nm[0] == 'o' && nm[1] == 'n') tags_ok = 0;                       /* an event handler */
                    if (nl == 4 && !strncmp(nm, "href", 4) && strncmp(v, "http://", 7) && strncmp(v, "https://", 8) && strncmp(v, "mailto:", 7) && v[0] != '#') tags_ok = 0;
                    if (nl == 5 && !strncmp(nm, "style", 5)) { char sv[1024]; size_t sl = (size_t)(ve - v); memcpy(sv, v, sl); sv[sl] = 0; if (strstr(sv, "url") || strstr(sv, "expression") || strstr(sv, "import") || strchr(sv, '\\')) tags_ok = 0; }
                    r = ve + 1;
                }
                if (*r && *r != '/') tags_ok = 0;
            }
            CHECK(tags_ok); }
        char tiny[3];
        hl = ag_html_sanitize((const char *)p, n, tiny, sizeof tiny);
        CHECK(hl < (long)sizeof tiny);
    }
    uint8_t *cc = NULL; size_t cn = 0; char what[96];
    if (!ag_clip_clean(p, n, &cc, &cn, what, sizeof what)) {
        AgClip c2;
        CHECK(cc && !ag_clipx_parse(cc, cn, &c2) && (!c2.text || ag_utf8_ok(c2.text, c2.text_n)) && (!c2.png || !ag_png_check(c2.png, c2.png_n, NULL, NULL)));
        CHECK(strlen(what) < sizeof what);
    }
    free(cc);
    (void)ag_hello_caps(p, n);
    (void)ag_link_ok(p, n);
}

/* frames cut anywhere: the splitter gives back the same frames in order */
static void stream(void) {
    static uint8_t s[1 << 16];
    size_t n = 0;
    int kinds[64], lens[64], nf = 0;
    while (nf < 64 && n + AG_HEAD + 600 < sizeof s) {
        size_t l = below(600);
        uint16_t k = (uint16_t)(1 + below(40));
        ag_frame_head(s + n, k, (uint16_t)below(3), (uint32_t)l);
        for (size_t i = 0; i < l; i++) s[n + AG_HEAD + i] = (uint8_t)rnd();
        kinds[nf] = k; lens[nf] = (int)l; nf++;
        n += AG_HEAD + l;
    }
    static uint8_t in[1 << 16];
    size_t have = 0, fed = 0;
    int got = 0, ok = 1;
    while (fed < n || have) {
        size_t step = fed < n ? 1 + below(700) : 0;
        if (step > n - fed) step = n - fed;
        memcpy(in + have, s + fed, step); have += step; fed += step;
        for (;;) {
            AgFrame f;
            int used = ag_frame_next(in, have, &f);
            if (used < 0) { ok = 0; have = 0; break; }
            if (!used) break;
            if (got >= nf || f.kind != kinds[got] || (int)f.len != lens[got] || !inside(in, have, f.data, f.len)) ok = 0;
            got++;
            memmove(in, in + used, have - (size_t)used);
            have -= (size_t)used;
        }
        if (fed == n && have) { ok = ok && 0; break; }
    }
    CHECK(ok && got == nf);
    /* a header that says more than a frame may hold is refused, never waited for */
    uint8_t h[AG_HEAD];
    ag_frame_head(h, 2, 0, AG_FRAME_MAX + 1);
    AgFrame f;
    CHECK(ag_frame_next(h, AG_HEAD, &f) == -1);
}

int main(void) {
    for (int it = 0; it < 120000; it++) {
        size_t n = seed((int)below(19), buf);
        if (n > CAP) n = 0;
        n = mutate(buf, n, CAP);
        /* a copy of exactly n bytes on the heap, so ASan sees a read past the payload */
        uint8_t *p = malloc(n ? n : 1);
        memcpy(p, buf, n);
        parse_all(p, n);
        free(p);
    }
    for (int it = 0; it < 300; it++) stream();
    printf("agent-fuzz-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
