/* mprismeta.c — a player's MPRIS properties into bounded fields (see mprismeta.h). */
#include "mprismeta.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void mpris_props_init(MprisProps *p) {
    memset(p, 0, sizeof *p);
    p->can_control = 1;
}

int64_t mpris_clamp_us(int64_t v) { return v < 0 ? 0 : v > MPRIS_MAX_US ? MPRIS_MAX_US : v; }

int mpris_status_of(const char *s) {
    if (!s) return -1;
    if (!strcmp(s, "Playing")) return MPRIS_PLAYING;
    if (!strcmp(s, "Paused")) return MPRIS_PAUSED;
    if (!strcmp(s, "Stopped")) return MPRIS_STOPPED;
    return -1;
}

int mpris_name_ok(const char *name) {
    size_t n = name ? strlen(name) : 0, pre = sizeof MPRIS_PREFIX - 1;
    return n > pre && n <= 255 && !strncmp(name, MPRIS_PREFIX, pre) && dbus_name_valid(name);
}

/* Length of the UTF-8 character at s (1..4), or 0 if it is not a valid one; *cp gets its code point. */
static int utf8_char(const unsigned char *s, uint32_t *cp) {
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    int len = s[0] >= 0xf0 && s[0] <= 0xf4 ? 4 : s[0] >= 0xe0 ? 3 : s[0] >= 0xc2 && s[0] < 0xe0 ? 2 : 0;
    if (!len || (len == 3 && s[0] >= 0xf0)) return 0;
    uint32_t c = s[0] & (0x7f >> len);
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xc0) != 0x80) return 0;
        c = c << 6 | (s[i] & 0x3f);
    }
    if ((len == 3 && c < 0x800) || (len == 4 && (c < 0x10000 || c > 0x10ffff)) || (c >= 0xd800 && c <= 0xdfff)) return 0;
    *cp = c;
    return len;
}

void mpris_clean(const char *in, char *out, size_t n) {
    if (!n) return;
    size_t o = 0;
    const unsigned char *s = (const unsigned char *)(in ? in : "");
    while (*s) {
        uint32_t cp = 0;
        int len = utf8_char(s, &cp);
        if (!len || cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) {       /* invalid or a control character */
            if (o + 1 >= n) break;
            if (o && out[o - 1] != ' ') out[o++] = ' ';
            s += len ? len : 1;
            continue;
        }
        if (o + (size_t)len >= n) break;                              /* never half a character */
        if (cp == ' ' && (!o || out[o - 1] == ' ')) { s++; continue; }
        memcpy(out + o, s, (size_t)len);
        o += (size_t)len;
        s += len;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int mpris_art_path(const char *url, char *out, size_t n) {
    if (!n) return -1;
    out[0] = 0;
    if (!url) return -1;
    const char *p;
    if (!strncmp(url, "file:///", 8)) p = url + 7;
    else if (!strncmp(url, "file://localhost/", 17)) p = url + 16;
    else return -1;                                                   /* http(s), data:, another host */
    size_t o = 0;
    for (; *p; p++) {
        int c = (unsigned char)*p;
        if (c == '?' || c == '#') goto refused;
        if (c == '%') {
            int hi = hexval(p[1]), lo = hi < 0 ? -1 : hexval(p[2]);
            if (lo < 0) goto refused;
            c = hi << 4 | lo;
            if (!c || c == '/') goto refused;
            p += 2;
        }
        if (c < 0x20 || c == 0x7f || o + 1 >= n) goto refused;
        out[o++] = (char)c;
    }
    out[o] = 0;
    /* no ".." segment */
    for (const char *s = out; (s = strstr(s, "/..")); s += 3)
        if (s[3] == '/' || !s[3]) goto refused;
    return o > 1 ? 0 : -1;
refused:
    out[0] = 0;
    return -1;
}

/* An integer of any of the types players use for lengths and positions (x, t, i, u, d); *ok = 0 and the
 * value skipped for another type. */
static int64_t read_int(DRd *r, const char *sig, int *ok) {
    *ok = 1;
    if (!strcmp(sig, "x")) return (int64_t)dr_u64(r);
    if (!strcmp(sig, "t")) { uint64_t u = dr_u64(r); return u > (uint64_t)MPRIS_MAX_US ? MPRIS_MAX_US : (int64_t)u; }
    if (!strcmp(sig, "i")) return dr_i32(r);
    if (!strcmp(sig, "u")) return dr_u32(r);
    if (!strcmp(sig, "d")) {
        uint64_t u = dr_u64(r);
        double d;
        memcpy(&d, &u, sizeof d);
        return isnan(d) || d <= 0 ? 0 : d >= (double)MPRIS_MAX_US ? MPRIS_MAX_US : (int64_t)d;
    }
    *ok = 0;
    dr_skip(r, sig);
    return 0;
}

static void read_str(DRd *r, const char *sig, char *out, size_t n) {
    if (strcmp(sig, "s")) { dr_skip(r, sig); return; }
    const char *s = dr_str(r, NULL);
    if (!r->err) mpris_clean(s, out, n);
}

/* xesam:artist is a list (some players send one string): up to MPRIS_ARTISTS names joined with ", " */
static void read_artists(DRd *r, const char *sig, char *out, size_t n) {
    if (!strcmp(sig, "s")) { read_str(r, sig, out, n); return; }
    if (strcmp(sig, "as")) { dr_skip(r, sig); return; }
    char all[1024] = "";
    size_t len = 0;
    int count = 0;
    size_t end = dr_arr(r, 4);
    while (!r->err && dr_more(r, end)) {
        const char *s = dr_str(r, NULL);
        char one[256];
        if (r->err || count >= MPRIS_ARTISTS) continue;
        mpris_clean(s, one, sizeof one);
        if (!one[0]) continue;
        int w = snprintf(all + len, sizeof all - len, "%s%s", count ? ", " : "", one);
        if (w < 0 || (size_t)w >= sizeof all - len) break;
        len += (size_t)w;
        count++;
    }
    mpris_clean(all, out, n);
}

static void read_meta(DRd *r, MprisProps *p) {
    p->title[0] = p->artist[0] = p->album[0] = p->art[0] = p->trackid[0] = 0;
    p->length_us = 0;
    size_t end = dr_arr(r, 8);
    while (!r->err && dr_more(r, end)) {
        dr_struct(r);
        const char *k = dr_str(r, NULL), *sig = dr_variant(r);
        if (r->err || !k || !sig) break;
        if (!strcmp(k, "xesam:title")) read_str(r, sig, p->title, sizeof p->title);
        else if (!strcmp(k, "xesam:album")) read_str(r, sig, p->album, sizeof p->album);
        else if (!strcmp(k, "xesam:artist")) read_artists(r, sig, p->artist, sizeof p->artist);
        else if (!strcmp(k, "mpris:length")) { int ok; int64_t v = read_int(r, sig, &ok); if (ok) p->length_us = mpris_clamp_us(v); }
        else if (!strcmp(k, "mpris:artUrl") && !strcmp(sig, "s")) {
            const char *u = dr_str(r, NULL);
            if (!r->err && strlen(u) < 4096) mpris_art_path(u, p->art, sizeof p->art);
        } else if (!strcmp(k, "mpris:trackid") && (!strcmp(sig, "o") || !strcmp(sig, "s"))) {
            const char *t = dr_str(r, NULL);
            if (!r->err && strlen(t) < sizeof p->trackid && dbus_path_valid(t)) snprintf(p->trackid, sizeof p->trackid, "%s", t);
        } else dr_skip(r, sig);
    }
}

int mpris_read_position(DRd *r, int64_t *us) {
    const char *sig = dr_variant(r);
    if (r->err || !sig) return -1;
    int ok;
    int64_t v = read_int(r, sig, &ok);
    if (!ok || r->err) return -1;
    *us = mpris_clamp_us(v);
    return 0;
}

int mpris_read_props(DRd *r, MprisProps *p) {
    int got = 0;
    size_t end = dr_arr(r, 8);
    if (r->err) return -1;
    while (!r->err && dr_more(r, end)) {
        dr_struct(r);
        const char *k = dr_str(r, NULL), *sig = dr_variant(r);
        if (r->err || !k || !sig) break;
        if (!strcmp(k, "Identity") && !strcmp(sig, "s")) { read_str(r, sig, p->identity, sizeof p->identity); got |= MP_IDENTITY; }
        else if (!strcmp(k, "PlaybackStatus") && !strcmp(sig, "s")) {
            int st = mpris_status_of(dr_str(r, NULL));
            if (!r->err && st >= 0) { p->status = st; got |= MP_STATUS; }
        } else if (!strcmp(k, "Metadata") && !strcmp(sig, "a{sv}")) { read_meta(r, p); got |= MP_META; }
        else if (!strcmp(k, "Position")) { int ok; int64_t v = read_int(r, sig, &ok); if (ok) { p->position_us = mpris_clamp_us(v); got |= MP_POS; } }
        else if (!strcmp(sig, "b") && (!strcmp(k, "CanPlay") || !strcmp(k, "CanPause") || !strcmp(k, "CanGoNext") ||
                                       !strcmp(k, "CanGoPrevious") || !strcmp(k, "CanSeek") || !strcmp(k, "CanControl"))) {
            uint8_t v = (uint8_t)dr_bool(r);
            uint8_t *f = !strcmp(k, "CanPlay") ? &p->can_play : !strcmp(k, "CanPause") ? &p->can_pause : !strcmp(k, "CanGoNext") ? &p->can_next
                       : !strcmp(k, "CanGoPrevious") ? &p->can_prev : !strcmp(k, "CanSeek") ? &p->can_seek : &p->can_control;
            *f = v;
            got |= MP_CAPS;
        } else dr_skip(r, sig);
    }
    return r->err ? -1 : got;
}
