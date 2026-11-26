/* webcall.c — see webcall.h. */
#include "webcall.h"
#include "webua.h"
#include "weburl.h"
#include "../sec/json.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
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

/* case-insensitive substring */
static int has(const char *s, const char *word) {
    size_t n = strlen(word);
    for (; s && *s; s++) if (!strncasecmp(s, word, n)) return 1;
    return 0;
}

/* "(046d:082d)": a USB vendor and product id, as PipeWire and V4L2 name USB devices */
static int has_usb_id(const char *s) {
    for (; s && *s; s++) {
        if (*s != '(') continue;
        int ok = 1;
        for (int i = 1; i <= 9 && ok; i++) ok = i == 5 ? s[i] == ':' : isxdigit((unsigned char)s[i]) != 0;
        if (ok && s[10] == ')') return 1;
    }
    return 0;
}

const char *webcall_dip_word(const char *h) {
    if (!h) return "none";
    while (*h == ' ' || *h == '\t') h++;
    size_t n = strcspn(h, ";");
    while (n && (h[n - 1] == ' ' || h[n - 1] == '\t')) n--;
    static const char *const words[] = { "none", "isolate-and-require-corp", "isolate-and-credentialless", NULL };
    if (!n) return "none";
    for (int i = 0; words[i]; i++) if (strlen(words[i]) == n && !strncmp(h, words[i], n)) return words[i];
    return "other";
}

static const char *dip_word = "none";

void webcall_set_dip(const char *header) { dip_word = webcall_dip_word(header); }

const char *webcall_device_class(const char *op, const char *label) {
    if (op && !strcmp(op, "getDisplayMedia")) return "screen";
    if (!label || !label[0]) return "unnamed";
    if (has(label, "LordOS Studio")) return "lordos-studio";
    if (has(label, "virtual") || has(label, "loopback") || has(label, "dummy") || has(label, "monitor of")) return "virtual";
    if (has(label, "bluetooth") || has(label, "bluez") || has(label, "headset") || has(label, "hands-free") || has(label, "handsfree")
        || has(label, "a2dp") || has(label, "hfp")) return "bluetooth";
    if (has(label, "usb") || has_usb_id(label) || has(label, "webcam")) return "usb";
    if (has(label, "built-in") || has(label, "internal") || has(label, "integrated") || has(label, "analog stereo")
        || has(label, "digital microphone") || has(label, "hda ")) return "built-in";
    return "other";
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
        r = out_fmt(out, n, "page: crossOriginIsolated %s, SharedArrayBuffer %s, Atomics %s, RTCPeerConnection %s, getUserMedia %s, DIP %s",
                    coi ? "yes" : "no", sab ? "yes" : "no", at ? "yes" : "no", rtc ? "yes" : "no", gum ? "yes" : "no", dip_word);
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
        /* what kind of device each track came from, in closed words (webcall_device_class): never its label, which
         * can be a person's name ("<name>'s headphones") or a serial number */
        char kinds[256] = "";
        size_t at = 0;
        const JVal *ts = json_get(o, "tracks");
        int count = 0;
        for (const JVal *t = ts && ts->type == J_ARR ? ts->kid : NULL; t && count < 4; t = t->next) {
            const char *tk = t->type == J_OBJ ? json_str(t, "kind") : NULL, *label = t->type == J_OBJ ? json_str(t, "label") : NULL;
            if (!in_list(tk, KINDS)) continue;
            int w = snprintf(kinds + at, sizeof kinds - at, "%s%s %s", count ? ", " : "", tk, webcall_device_class(op, label));
            if (w < 0 || (size_t)w >= sizeof kinds - at) break;
            at += (size_t)w;
            count++;
        }
        r = count ? out_fmt(out, n, "%s %s: granted (%s)", op, what, kinds) : out_fmt(out, n, "%s %s: granted", op, what);
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

size_t webcall_hints_line(unsigned mask, char *out, size_t n) {
    if (!out || !n) return 0;
    char names[512] = "";
    size_t at = 0;
    for (unsigned b = 1; b & WEBUA_HINT_ALL; b <<= 1) {
        if (!(mask & b)) continue;
        int k = snprintf(names + at, sizeof names - at, "%s%s", at ? ", " : "", webua_hint_name(b));
        if (k < 0 || (size_t)k >= sizeof names - at) return 0;
        at += (size_t)k;
    }
    return out_fmt(out, n, "client hints asked by the app: %s", at ? names : "none");
}

int webcall_hints_site(const char *uri, const char *scope, const char *app_origin, char *out, size_t n) {
    char mine[256];
    if (!uri || !scope || !app_origin || !weburl_in_scope(uri, scope) || webua_origin(uri, out, n) || webua_origin(app_origin, mine, sizeof mine)
        || strcmp(out, mine)) return -1;
    return 0;
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

void webcall_log_hints(unsigned mask) {
    char line[700];
    if (webcall_hints_line(mask, line, sizeof line)) webcall_log(line);
}
