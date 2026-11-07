/* termfill.c — Passwords in the Terminal (see termfill.h). */
#include "termfill.h"
#include "term.h"
#include "tcapture.h"
#include "../sec/json.h"
#include "../vault/vclient.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define FILL_MAX 4
#define FILL_WAIT_MS 15000              /* lord-vault answers in milliseconds; a forgotten one is dropped */

static struct { int vid, sid, save; double at; } fills[FILL_MAX];     /* save: saving a typed password, not a fill */

static double mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* entry ids are lord-vault's (hex and dashes); field keys are short names ("password", "pin") */
static int valid_id(const char *s, size_t max) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > max) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_' || s[i] == '.'))
            return 0;
    return 1;
}

int termfill_start(int sid, const char *vault, const char *entry, const char *field, int index) {
    Session *s = term_get(sid);
    if (!s || !term_asks_secret(s, NULL, 0) || term_traced()) return -1;
    if (strcmp(vault, "main") && strcmp(vault, "login")) return -1;
    if (!valid_id(entry, 39) || !valid_id(field, 64) || index < 0 || index > 64) return -1;
    double now = mono_ms();
    int slot = -1;
    for (int i = 0; i < FILL_MAX; i++) {
        if (fills[i].vid && now - fills[i].at > FILL_WAIT_MS) fills[i].vid = 0;
        if (!fills[i].vid && slot < 0) slot = i;
    }
    if (slot < 0) return -1;
    char j[256];
    int n = snprintf(j, sizeof j, "{\"op\":\"reveal\",\"v\":\"%s\",\"id\":\"%s\",\"k\":\"%s\",\"i\":%d}", vault, entry, field, index);
    if (n <= 0 || (size_t)n >= sizeof j) return -1;
    int vid = vc_request_private(j, (size_t)n);
    if (vid <= 0) return -1;
    fills[slot].vid = vid;
    fills[slot].sid = sid;
    fills[slot].save = 0;
    fills[slot].at = now;
    return 0;
}

/* ---- Save to Passwords: a password typed in the Terminal (tcapture.c) ---- */

static int free_slot(double now) {
    int slot = -1;
    for (int i = 0; i < FILL_MAX; i++) {
        if (fills[i].vid && now - fills[i].at > FILL_WAIT_MS) fills[i].vid = 0;
        if (!fills[i].vid && slot < 0) slot = i;
    }
    return slot;
}

/* One field of the entry: {"k":key,"t":type,"v":value}, with a leading comma after the first. 0 if it fit. */
static int field(char *o, size_t n, size_t *k, const char *key, const char *type, const char *val) {
    char q[CAP_MAX * 6 + 8];                /* a value quoted for JSON: up to \u00XX per byte */
    size_t ql = json_quote(q, sizeof q, val);
    int w = snprintf(o + *k, n - *k, "%s{\"k\":\"%s\",\"t\":\"%s\",\"v\":%s}", *k && o[*k - 1] != '[' ? "," : "", key, type, q);
    explicit_bzero(q, ql);
    if (w < 0 || (size_t)w >= n - *k) return -1;
    *k += (size_t)w;
    return 0;
}

int termfill_save(int sid, unsigned token) {
    Session *s = term_get(sid);
    TcapTarget t;
    const char *val = s && !term_traced() ? tcap_value(sid, token, NULL, &t) : NULL;
    double now = mono_ms();
    int slot = val ? free_slot(now) : -1;
    if (slot < 0) return -1;
    /* where it goes in Passwords: its category's own fields, tagged "Terminal" and the program */
    static const char *const cats[] = { "network", "ssh", "database", "internet", "network" };
    const char *cat = cats[t.kind >= 0 && t.kind <= TK_OTHER ? t.kind : 0];
    char title[256], url[160], qt[300], qp[80];
    snprintf(title, sizeof title, "%s %s%s%s", t.program, t.user, t.user[0] ? "@" : "", t.host);
    json_quote(qt, sizeof qt, title);
    json_quote(qp, sizeof qp, t.program);
    static char j[CAP_MAX * 6 + 2048];      /* the request, with the password in it: wiped below */
    size_t k = 0;
    int w = snprintf(j, sizeof j, "{\"op\":\"save\",\"v\":\"main\",\"entry\":{\"cat\":\"%s\",\"title\":%s,\"tags\":[\"Terminal\",%s],\"f\":[",
                     cat, qt, qp);
    int bad = w < 0 || (size_t)w >= sizeof j;
    if (!bad) k = (size_t)w;
    if (!bad && t.kind == TK_GIT) {
        snprintf(url, sizeof url, "https://%s", t.host);
        bad = field(j, sizeof j, &k, "url", "url", url) || field(j, sizeof j, &k, "username", "text", t.user);
    } else if (!bad) {
        if (t.kind == TK_DB) bad = field(j, sizeof j, &k, "engine", "text", t.engine);
        if (!bad && t.kind == TK_OTHER) bad = field(j, sizeof j, &k, "device", "text", t.program);
        if (!bad) bad = field(j, sizeof j, &k, "host", "text", t.host);
        if (!bad && t.port[0]) bad = field(j, sizeof j, &k, "port", "number", t.port);
        if (!bad) bad = field(j, sizeof j, &k, "username", "text", t.user);
    }
    if (!bad) bad = field(j, sizeof j, &k, "password", "hidden", val);
    if (!bad) { w = snprintf(j + k, sizeof j - k, "]}}"); bad = w < 0 || (size_t)w >= sizeof j - k; if (!bad) k += (size_t)w; }
    int vid = bad ? -1 : vc_request_private(j, k);
    explicit_bzero(j, sizeof j);
    tcap_drop(sid);                         /* the value is in lord-vault's hands now, or lost: not kept here */
    if (vid <= 0) return -1;
    fills[slot].vid = vid;
    fills[slot].sid = sid;
    fills[slot].save = 1;
    fills[slot].at = now;
    return 0;
}

int termfill_reply(uint32_t id, const char *rep, size_t len) {
    int k = -1;
    for (int i = 0; i < FILL_MAX && k < 0; i++) if (fills[i].vid && (uint32_t)fills[i].vid == id) k = i;
    if (k < 0) return 0;
    int sid = fills[k].sid, save = fills[k].save;
    fills[k].vid = 0;
    Session *s = term_get(sid);
    JDoc d;
    if (save) {                             /* {"ok":true,…} or {"error":"locked"…}: no secret in it */
        int parsed = !json_parse(rep, len, &d, NULL, 0);
        const char *err = parsed ? json_str(d.root, "error") : "error";
        char result[24];
        snprintf(result, sizeof result, "%s", !err ? "ok" : strlen(err) < sizeof result ? err : "error");
        if (parsed) json_free(&d);          /* the reply may carry the saved entry: the caller wipes rep */
        if (s) term_save_done(s, result);
        return 1;
    }
    if (json_parse(rep, len, &d, NULL, 0)) { if (s) term_fill_done(s, "error"); return 1; }
    const JVal *v = json_get(d.root, "value");
    const char *err = json_str(d.root, "error");
    char result[24];
    if (!v || v->type != J_STR) snprintf(result, sizeof result, "%s", err && strlen(err) < sizeof result ? err : "error");
    /* the program may have stopped asking meanwhile (it timed out, the user typed it): then the password
     * would be echoed to whatever reads the pty now, so it isn't written */
    else if (!s || !term_asks_secret(s, NULL, 0)) snprintf(result, sizeof result, "not-asking");
    else if (memchr(v->str, '\n', v->len) || memchr(v->str, '\r', v->len)) snprintf(result, sizeof result, "multiline");
    else {
        term_input(s, v->str, v->len);
        term_input(s, "\r", 1);
        snprintf(result, sizeof result, "ok");
    }
    if (v && v->type == J_STR) explicit_bzero((char *)v->str, v->len);
    json_free(&d);
    if (s) term_fill_done(s, result);
    return 1;
}

int termfill_pending(void) {
    for (int i = 0; i < FILL_MAX; i++) if (fills[i].vid) return 1;
    return 0;
}
