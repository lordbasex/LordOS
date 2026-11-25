/* webcall.c — see webcall.h. */
#include "webcall.h"
#include "webua.h"
#include "../sec/json.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int in_list(const char *s, const char *const *list) {
    for (int i = 0; s && list[i]; i++) if (!strcmp(s, list[i])) return 1;
    return 0;
}

static const char *const EVENTS[] = { "created", "closed", NULL };
static const char *const WHATS[] = { "ice-gathering", "ice-connection", "connection", "signaling", NULL };
static const char *const STATES[] = { "new", "gathering", "complete", "checking", "connecting", "connected", "completed",
                                      "disconnected", "failed", "closed", "stable", "have-local-offer", "have-remote-offer",
                                      "have-local-pranswer", "have-remote-pranswer", NULL };
static const char *const OPS[] = { "createOffer", "createAnswer", "setLocalDescription", "setRemoteDescription",
                                   "addIceCandidate", "addTrack", "addTransceiver", "getStats", "getUserMedia",
                                   "getDisplayMedia", "RTCPeerConnection", NULL };
static const char *const MEDIA_OPS[] = { "getUserMedia", "getDisplayMedia", NULL };
static const char *const DIRS[] = { "in", "out", NULL };
static const char *const KINDS[] = { "audio", "video", NULL };
static const char *const CANDIDATES[] = { "host", "srflx", "prflx", "relay", NULL };
static const char *const PROTOCOLS[] = { "udp", "tcp", NULL };
static const char *const RESULTS[] = { "granted", "failed", NULL };

/* an error's name, as DOMException names are: letters only, at most 40 */
static int name_ok(const char *s) {
    if (!s || !s[0] || strlen(s) > 40) return 0;
    for (; *s; s++) if (!isalpha((unsigned char)*s)) return 0;
    return 1;
}

/* "audio/opus", "video/VP8", "video/H264": the kind, a slash and at most 24 of [A-Za-z0-9._-] */
static int codec_ok(const char *s) {
    if (!s || (strncmp(s, "audio/", 6) && strncmp(s, "video/", 6))) return 0;
    const char *t = s + 6;
    if (!t[0] || strlen(t) > 24) return 0;
    for (; *t; t++) if (!isalnum((unsigned char)*t) && !strchr("._-", *t)) return 0;
    return 1;
}

static int num(const JVal *o, const char *key, int lo, int hi, int *out) {
    const JVal *v = json_get(o, key);
    if (!v || v->type != J_NUM || v->num != (double)(long)v->num || v->num < lo || v->num > hi) return -1;
    *out = (int)v->num;
    return 0;
}

static int flag(const JVal *o, const char *key) {
    const JVal *v = json_get(o, key);
    return v && v->type == J_BOOL ? (int)v->num : -1;
}

static size_t out_fmt(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    return k < 0 || (size_t)k >= n ? 0 : (size_t)k;
}

void webcall_label(const char *in, char *out, size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    if (!in) return;
    char tmp[64];
    size_t k = 0;
    for (const char *s = in; *s && k < 48; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || strchr(" ._()-:", c)) tmp[k++] = (char)c;
    }
    tmp[k] = 0;
    /* a stretch of digits, spaces and dashes with 5 or more digits goes (a serial number, a phone number written
     * "11 5555 1234"): nobody's business here */
    size_t j = 0;
    for (size_t i = 0; i < k;) {
        size_t r = i, digits = 0;
        while (r < k && (isdigit((unsigned char)tmp[r]) || tmp[r] == ' ' || tmp[r] == '-')) digits += isdigit((unsigned char)tmp[r++]) != 0;
        if (r == i) r = i + 1;
        else if (digits >= 5) { if (j + 1 < n) out[j++] = ' '; i = r; continue; }
        while (i < r && j + 1 < n) out[j++] = tmp[i++];
        i = r;
    }
    while (j && out[j - 1] == ' ') j--;
    size_t s = 0;
    while (s < j && out[s] == ' ') s++;
    memmove(out, out + s, j - s);
    out[j - s] = 0;
}

size_t webcall_line(const char *json, size_t len, char *out, size_t n) {
    JDoc d;
    if (!json || !out || !n || len > WEBCALL_MSG_MAX || json_parse(json, len, &d, NULL, 0)) return 0;
    size_t r = 0;
    const JVal *o = d.root;
    const char *k = o && o->type == J_OBJ ? json_str(o, "k") : NULL;
    int id = 0;
    if (!k) goto done;
    if (!strcmp(k, "env")) {
        int coi = flag(o, "coi"), sab = flag(o, "sab"), at = flag(o, "atomics"), rtc = flag(o, "rtc"), gum = flag(o, "gum");
        if (coi < 0 || sab < 0 || at < 0 || rtc < 0 || gum < 0) goto done;
        r = out_fmt(out, n, "page: crossOriginIsolated %s, SharedArrayBuffer %s, Atomics %s, RTCPeerConnection %s, getUserMedia %s",
                    coi ? "yes" : "no", sab ? "yes" : "no", at ? "yes" : "no", rtc ? "yes" : "no", gum ? "yes" : "no");
    } else if (!strcmp(k, "pc")) {
        const char *ev = json_str(o, "ev");
        if (num(o, "id", 1, 10000, &id) || !in_list(ev, EVENTS)) goto done;
        r = out_fmt(out, n, "peer connection %d %s", id, ev);
    } else if (!strcmp(k, "state")) {
        const char *what = json_str(o, "what"), *st = json_str(o, "state");
        if (num(o, "id", 1, 10000, &id) || !in_list(what, WHATS) || !in_list(st, STATES)) goto done;
        r = out_fmt(out, n, "peer connection %d: %s %s", id, what, st);
    } else if (!strcmp(k, "media")) {
        const char *dir = json_str(o, "dir"), *kind = json_str(o, "kind"), *codec = json_str(o, "codec");
        int w = 0, h = 0, fps = 0;
        if (num(o, "id", 1, 10000, &id) || !in_list(dir, DIRS) || !in_list(kind, KINDS)) goto done;
        if (codec && !codec_ok(codec)) goto done;
        if (!strcmp(kind, "video") && !num(o, "w", 0, 16384, &w) && !num(o, "h", 0, 16384, &h) && !num(o, "fps", 0, 1000, &fps) && w && h)
            r = out_fmt(out, n, "peer connection %d: %s %s %s %dx%d %d fps", id, dir, kind, codec ? codec : "(no codec)", w, h, fps);
        else r = out_fmt(out, n, "peer connection %d: %s %s %s", id, dir, kind, codec ? codec : "(no codec)");
    } else if (!strcmp(k, "pair")) {
        const char *l = json_str(o, "local"), *rm = json_str(o, "remote"), *p = json_str(o, "proto");
        if (num(o, "id", 1, 10000, &id) || !in_list(l, CANDIDATES) || !in_list(rm, CANDIDATES) || !in_list(p, PROTOCOLS)) goto done;
        r = out_fmt(out, n, "peer connection %d: candidate pair %s -> %s over %s", id, l, rm, p);
    } else if (!strcmp(k, "error")) {
        const char *op = json_str(o, "op"), *name = json_str(o, "name");
        if (num(o, "id", 0, 10000, &id) || !in_list(op, OPS) || !name_ok(name)) goto done;
        if (id) r = out_fmt(out, n, "peer connection %d: %s failed: %s", id, op, name);
        else r = out_fmt(out, n, "%s failed: %s", op, name);
    } else if (!strcmp(k, "gum")) {
        const char *op = json_str(o, "op"), *res = json_str(o, "result"), *name = json_str(o, "name");
        int a = flag(o, "audio"), v = flag(o, "video");
        if (!in_list(op, MEDIA_OPS) || !in_list(res, RESULTS) || a < 0 || v < 0) goto done;
        const char *what = a && v ? "audio+video" : a ? "audio" : v ? "video" : "nothing";
        if (!strcmp(res, "failed")) {
            if (!name_ok(name)) goto done;
            r = out_fmt(out, n, "%s %s: failed: %s", op, what, name);
            goto done;
        }
        char labels[256] = "";
        size_t at = 0;
        const JVal *ls = json_get(o, "labels");
        int count = 0;
        for (const JVal *l = ls && ls->type == J_ARR ? ls->kid : NULL; l && count < 4; l = l->next) {
            if (l->type != J_STR) continue;
            char one[64];
            webcall_label(l->str, one, sizeof one);
            if (!one[0]) snprintf(one, sizeof one, "(no label)");
            int w = snprintf(labels + at, sizeof labels - at, "%s%s", count ? ", " : "", one);
            if (w < 0 || (size_t)w >= sizeof labels - at) break;
            at += (size_t)w;
            count++;
        }
        r = count ? out_fmt(out, n, "%s %s: granted (%s)", op, what, labels) : out_fmt(out, n, "%s %s: granted", op, what);
    }
done:
    json_free(&d);
    return r;
}

int webcall_rate(double now_s, unsigned *dropped) {
    static double start = -1;
    static unsigned count, lost;
    if (start < 0 || now_s - start >= 60 || now_s < start) { start = now_s; count = 0; }
    if (count >= WEBCALL_RATE) { lost++; return 0; }
    count++;
    if (dropped) *dropped = lost;
    lost = 0;
    return 1;
}

/* a host written as an IP address (v4 digits and dots, or a bracketed v6) */
static int host_is_ip(const char *origin) {
    const char *h = strstr(origin, "://");
    if (!h) return 1;
    h += 3;
    if (*h == '[') return 1;
    size_t len = strcspn(h, ":");
    for (size_t i = 0; i < len; i++) if (!isdigit((unsigned char)h[i]) && h[i] != '.') return 0;
    return 1;
}

size_t webcall_hints_line(const char *origin, unsigned mask, char *out, size_t n) {
    if (!origin || !out || !n) return 0;
    char names[512] = "";
    size_t at = 0;
    for (unsigned b = 1; b & WEBUA_HINT_ALL; b <<= 1) {
        if (!(mask & b)) continue;
        int k = snprintf(names + at, sizeof names - at, "%s%s", at ? ", " : "", webua_hint_name(b));
        if (k < 0 || (size_t)k >= sizeof names - at) return 0;
        at += (size_t)k;
    }
    return out_fmt(out, n, "client hints asked by %s: %s", host_is_ip(origin) ? "an address" : origin, at ? names : "none");
}

void webcall_log(const char *line) {
    if (!line || !line[0]) return;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    unsigned dropped = 0;
    if (!webcall_rate((double)t.tv_sec + t.tv_nsec / 1e9, &dropped)) return;
    if (dropped) fprintf(stderr, "webcall: %u lines dropped (more than %d a minute)\n", dropped, WEBCALL_RATE);
    fprintf(stderr, "webcall: %s\n", line);
}

void webcall_log_hints(const char *origin, unsigned mask) {
    char line[700];
    if (webcall_hints_line(origin, mask, line, sizeof line)) webcall_log(line);
}
