/* js_fw.c — lord.fw: per-app network permissions for the Monitor and Settings → Privacy
 * (docs/FIREWALL.md, steps 2 to 4).
 *
 * lord-server owns the user's rules (~/.config/lordos/firewall.json, the store in
 * fw/rules.c): it reads them at startup, changes them with these calls and saves them. Then it notifies
 * lord-netperm (the root service that applies them in the kernel) with a single message without data,
 * "read again" (netperm/npproto.h); lord-netperm reads the file itself. Back come the
 * state, new connections and counters. No call takes paths: the file's path is
 * fixed by the server, and firewall.json is on lord.fs's deny list (fsops.c).
 *
 *   status()                     { daemon, bpf, dns, error }
 *   state()                      { mode, trusted: [app], restricted: { app: [restriction] } }
 *   appRule(app)                 'allow' | 'deny' | null   (the {app, out, any} rule)
 *   setAppRule(app, v)           v: 'allow' | 'deny' | null (removes it); the Monitor's switch
 *   setTrusted(app, on) / setRestricted(app, [restriction]) / setMode('silent'|'ask'|'off'|'deny')
 *   rules(app)                   [{ id, dir, dst, port, proto, action, when, lastMatch }]
 *                                when: Unix seconds of the decision; lastMatch: Unix seconds of the
 *                                last connection lord-netperm reported as decided by that rule
 *                                (seen by this session since it started; 0 = not seen)
 *   ruleApps()                   [app]: the apps that have at least one rule
 *   addRule({ app, dir, dst, port, proto, action }) → id    removeRule(id)
 *   updateRule(id, { dir, dst, port, proto, action }) → id  same app; the old rule comes back if
 *                                the new one can't be added
 *   poll()                       { counts: { app: [allowed, denied, rx, tx, rxRate, txRate, flows, blocked] },
 *                                  events: [...] } (the bytes when lord-netperm's accounting is loaded,
 *                                  see npproto.h)
 *   blocks(app, restriction)     true if the app is restricted in it (sdk/notify.js asks before showing
 *                                a built-in app's notification)
 *
 * (0.8.5) Subscribed blocklists (fw/fwsub.h; docs/FIREWALL.md, "Blocklists"):
 *   lists()                      { lists: [{ id, url, format, custom, on, updated, tried, domains, nets,
 *                                  truncated, error }], busy, domains, nets }
 *   setList(id, on)              a catalog or custom list on or off
 *   addList(url, format) → id    a custom list (https only; 'hosts' | 'domains' | 'ips'); throws fw.list.*
 *   removeList(id)               a custom list
 *   updateLists(force)           0 started, 1 nothing to do, -1 already running
 *   listMatch([names], addr)     the id of the enabled list that covers that destination, or null
 *
 * The non-network restrictions are enforced in C where the resource is handed out (sec/restrict.h:
 * the broker and js_media.c read this same store); adding one to a running app takes the camera
 * or the microphone away at once (restrict_changed).
 *
 * All of them validate: app ids with fw_app_canon, destinations with fw_parse_dest, ports 1–65535. */
#include "js_fw.h"
#include "../fw/rules.h"
#include "../fw/fwmap.h"
#include "../fw/fwsub.h"
#include "js_sys.h"
#include "../sec/restrict.h"
#include "js_err.h"
#include "../netperm/npproto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENTS 256

static FwStore *store;
static char path[PATH_MAX + 64];
static int sock = -1;
static long long last_try;
static unsigned char *inbuf;
static size_t inlen;
static char *status_json, *counts_json;
static uint64_t counts_gen;             /* goes up with every COUNTS notice (netrec.c) */
static char *events[MAX_EVENTS];
static int nevents;

/* When each rule last decided a connection (from the EVENT notices, "by":"rule"): a small open
 * addressing table rule id → Unix seconds. Memory only, like the counters. */
#define MATCH_SLOTS 4096
static struct { uint32_t id; long long t; } matches[MATCH_SLOTS];

static void match_note(uint32_t id, long long t) {
    if (!id) return;
    uint32_t h = (id * 2654435761u) % MATCH_SLOTS;
    for (int k = 0; k < MATCH_SLOTS; k++, h = (h + 1) % MATCH_SLOTS)
        if (matches[h].id == id || !matches[h].id) { matches[h].id = id; matches[h].t = t; return; }
    memset(matches, 0, sizeof matches);             /* full (rules come and go): start over */
    matches[(id * 2654435761u) % MATCH_SLOTS].id = id;
    matches[(id * 2654435761u) % MATCH_SLOTS].t = t;
}

static long long match_last(uint32_t id) {
    uint32_t h = (id * 2654435761u) % MATCH_SLOTS;
    for (int k = 0; k < MATCH_SLOTS && matches[h].id; k++, h = (h + 1) % MATCH_SLOTS)
        if (matches[h].id == id) return matches[h].t;
    return 0;
}

/* lord-netperm numbers the rules on its own: on every reload it reads firewall.json into a new
 * store, so its ids are 1, 2, 3… in the file's order, not this store's ids. To know which of our
 * rules a notice is about, the saved text is read the same way here (fw_to_json, then fw_from_json
 * into a scratch store) and each of those rules is matched to ours by what identifies a rule (app,
 * direction, destination, ports, protocol). np_ids[their id] = our id. Rebuilt on every save. */
static uint32_t *np_ids;
static uint32_t n_np;

static int rule_key(const FwRule *r, char *out, size_t n) {
    char dst[FW_NAME_MAX + 8];
    if (fw_format_dest(r, dst, sizeof dst)) return -1;
    int w = snprintf(out, n, "%s|%d|%s|%u-%u|%d", r->app, (int)r->dir, dst, r->port_lo, r->port_hi, (int)r->proto);
    return w <= 0 || (size_t)w >= n ? -1 : w;
}

static void np_map_rebuild(void) {
    free(np_ids);
    np_ids = NULL;
    n_np = 0;
    size_t len = 0;
    char *j = fw_to_json(store, &len), err[80], k[FW_APP_MAX + FW_NAME_MAX + 48];
    FwStore *sh = fw_store_new();
    FwRule *mine = NULL, *theirs = NULL;
    FwMap m = { 0 };
    int a = 0, b = 0;
    if (!j || !sh || fw_from_json(sh, j, len, err, sizeof err)) goto out;
    a = fw_rule_list(store, NULL, NULL, 0);
    b = fw_rule_list(sh, NULL, NULL, 0);
    if (a <= 0 || b <= 0) goto out;
    mine = malloc((size_t)a * sizeof *mine);
    theirs = malloc((size_t)b * sizeof *theirs);
    if (!mine || !theirs) goto out;
    a = fw_rule_list(store, NULL, mine, a);
    b = fw_rule_list(sh, NULL, theirs, b);
    for (int i = 0; i < a; i++) {
        int l = rule_key(&mine[i], k, sizeof k);
        void **slot = l > 0 ? fwmap_slot(&m, k, (size_t)l, 1) : NULL;
        if (slot) *slot = (void *)(uintptr_t)mine[i].id;
    }
    uint32_t top = 0;
    for (int i = 0; i < b; i++) if (theirs[i].id > top) top = theirs[i].id;
    if (!(np_ids = calloc((size_t)top + 1, sizeof *np_ids))) goto out;
    n_np = top + 1;
    for (int i = 0; i < b; i++) {
        int l = rule_key(&theirs[i], k, sizeof k);
        void *v = l > 0 ? fwmap_get(&m, k, (size_t)l) : NULL;
        if (v) np_ids[theirs[i].id] = (uint32_t)(uintptr_t)v;
    }
out:
    fwmap_free(&m, NULL);
    free(mine);
    free(theirs);
    free(j);
    if (sh) fw_store_free(sh);
}

/* The notice is lord-netperm's own compact JSON ("by":"rule","rule":N): read just those two keys. */
static void match_event(const unsigned char *p, size_t n) {
    static const char by[] = "\"by\":\"rule\",\"rule\":";
    if (n < sizeof by || n > 2048) return;
    char j[2049];
    memcpy(j, p, n);
    j[n] = 0;
    const char *s = strstr(j, by);
    if (!s) return;
    unsigned long id = strtoul(s + sizeof by - 1, NULL, 10);
    if (id && id < n_np && np_ids[id]) match_note(np_ids[id], (long long)time(NULL));
}

static long long now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void sock_close(void) {
    if (sock >= 0) close(sock);
    sock = -1;
    inlen = 0;
    free(status_json);
    status_json = NULL;
}

static void send_reload(void) {
    static const unsigned char msg[5] = { 1, 0, 0, 0, NP_MSG_RELOAD };
    if (sock >= 0 && send(sock, msg, sizeof msg, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)sizeof msg) sock_close();
}

/* Connects (at most one attempt every 2 s) and asks it to read the rules. */
static void ensure(void) {
    if (sock >= 0) return;
    long long t = now_s();
    if (last_try && t - last_try < 2) return;
    last_try = t;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", NP_SOCK_PATH);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa)) { close(fd); return; }
    sock = fd;
    send_reload();
}

static void keep(char **slot, const unsigned char *p, size_t n) {
    char *s = malloc(n + 1);
    if (!s) return;
    memcpy(s, p, n);
    s[n] = 0;
    free(*slot);
    *slot = s;
}

static int lists_on;

/* Reads whatever has arrived, without waiting; a blocklist job that ended makes lord-netperm read
 * the new compiled lists. */
static void pump(void) {
    ensure();
    if (lists_on && fws_poll()) send_reload();
    if (sock < 0) return;
    if (!inbuf && !(inbuf = malloc(NP_OUT_MAX + 8))) return;
    for (;;) {
        ssize_t n = recv(sock, inbuf + inlen, NP_OUT_MAX + 8 - inlen, MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { sock_close(); return; }
        if (n < 0) break;
        inlen += (size_t)n;
        size_t off = 0;
        while (inlen - off >= 5) {
            const unsigned char *h = inbuf + off;
            uint32_t l = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
            if (l < 1 || l > NP_OUT_MAX) { sock_close(); return; }
            if (inlen - off < 4 + (size_t)l) break;
            const unsigned char *body = h + 5;
            size_t bl = l - 1;
            if (h[4] == NP_MSG_STATUS) keep(&status_json, body, bl);
            else if (h[4] == NP_MSG_COUNTS) { keep(&counts_json, body, bl); counts_gen++; }
            else if (h[4] == NP_MSG_EVENT) {
                match_event(body, bl);
                if (nevents == MAX_EVENTS) { free(events[0]); memmove(events, events + 1, (MAX_EVENTS - 1) * sizeof *events); nevents--; }
                events[nevents] = NULL;
                keep(&events[nevents], body, bl);
                if (events[nevents]) nevents++;
            }
            off += 4 + (size_t)l;
        }
        memmove(inbuf, inbuf + off, inlen - off);
        inlen -= off;
    }
}

static int save(void) {
    char dir[sizeof path];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdir(dir, 0700); }
    int r = fw_save(store, path);
    if (r) fprintf(stderr, "lord.fw: could not save %s\n", path);
    np_map_rebuild();
    ensure();
    send_reload();
    return r;
}

/* ---- argument validation ---------------------------------------------------------------- */

static int arg_app(JSContext *c, JSValueConst v, char *out) {
    if (!JS_IsString(v)) return -1;
    const char *s = JS_ToCString(c, v);
    if (!s) return -1;
    int r = strlen(s) < FW_APP_MAX ? fw_app_canon(s, out, FW_APP_MAX) : -1;
    JS_FreeCString(c, s);
    return r;
}

static int str_prop(JSContext *c, JSValueConst o, const char *k, char *out, size_t n) {
    JSValue v = JS_GetPropertyStr(c, o, k);
    int r = -1;
    out[0] = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v)) r = 1;
    else if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        if (s && strlen(s) < n) { memcpy(out, s, strlen(s) + 1); r = 0; }
        if (s) JS_FreeCString(c, s);
    }
    JS_FreeValue(c, v);
    return r;
}

/* The rule {app, out, any, any port and protocol}: the Monitor's switch. */
static const FwRule *app_rule(const char *app, FwRule *buf, int max) {
    int n = fw_rule_list(store, app, buf, max);
    for (int i = 0; i < n && i < max; i++)
        if (buf[i].dir == FW_OUT && buf[i].kind == FW_DST_ANY && buf[i].proto == FW_PROTO_ANY && buf[i].port_lo == 0 &&
            buf[i].port_hi == 65535 && buf[i].life == FW_LIFE_FOREVER) return &buf[i];
    return NULL;
}

/* ---- functions ------------------------------------------------------------------------------- */

static JSValue f_status(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    pump();
    JSValue o = status_json ? JS_ParseJSON(c, status_json, strlen(status_json), "<fw.status>") : JS_NewObject(c);
    if (JS_IsException(o) || !JS_IsObject(o)) { JS_FreeValue(c, o); o = JS_NewObject(c); }
    JS_SetPropertyStr(c, o, "daemon", JS_NewBool(c, sock >= 0));
    return o;
}

typedef struct { JSContext *c; JSValue trusted, restricted; uint32_t nt; } EachCtx;

static int each_app(void *u, const char *app, int trusted, uint32_t flags) {
    EachCtx *e = u;
    if (trusted) JS_SetPropertyUint32(e->c, e->trusted, e->nt++, JS_NewString(e->c, app));
    if (flags) {
        JSValue arr = JS_NewArray(e->c);
        uint32_t k = 0;
        for (uint32_t b = 1; b && b <= FW_R_ALL; b <<= 1)
            if (flags & b) JS_SetPropertyUint32(e->c, arr, k++, JS_NewString(e->c, fw_flag_name(b)));
        JS_SetPropertyStr(e->c, e->restricted, app, arr);
    }
    return 0;
}

static const char *mode_name(FwMode m) { return m == FW_MODE_ASK ? "ask" : m == FW_MODE_OFF ? "off" : m == FW_MODE_DENY ? "deny" : "silent"; }

static JSValue f_state(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue o = JS_NewObject(c);
    EachCtx e = { c, JS_NewArray(c), JS_NewObject(c), 0 };
    fw_app_each(store, each_app, &e);
    JS_SetPropertyStr(c, o, "mode", JS_NewString(c, mode_name(fw_mode(store))));
    JS_SetPropertyStr(c, o, "trusted", e.trusted);
    JS_SetPropertyStr(c, o, "restricted", e.restricted);
    return o;
}

static JSValue f_app_rule(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char app[FW_APP_MAX];
    if (argc < 1 || arg_app(c, argv[0], app)) return js_throw_code(c, "fw.invalidApp", NULL);
    FwRule buf[256];
    const FwRule *r = app_rule(app, buf, 256);
    return r ? JS_NewString(c, r->action == FW_DENY ? "deny" : "allow") : JS_NULL;
}

static JSValue f_set_app_rule(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char app[FW_APP_MAX];
    if (argc < 2 || arg_app(c, argv[0], app)) return js_throw_code(c, "fw.invalidApp", NULL);
    char v[8];
    if (JS_IsNull(argv[1])) v[0] = 0;
    else {
        const char *s = JS_IsString(argv[1]) ? JS_ToCString(c, argv[1]) : NULL;
        if (!s || (strcmp(s, "allow") && strcmp(s, "deny"))) { if (s) JS_FreeCString(c, s); return js_throw_code(c, "fw.invalidRule", "field", "action", NULL); }
        snprintf(v, sizeof v, "%s", s);
        JS_FreeCString(c, s);
    }
    if (!v[0]) {
        FwRule buf[256];
        const FwRule *r = app_rule(app, buf, 256);
        if (r) fw_rule_remove(store, r->id);
    } else {
        FwRule r;
        memset(&r, 0, sizeof r);
        snprintf(r.app, sizeof r.app, "%s", app);
        r.dir = FW_OUT;
        r.kind = FW_DST_ANY;
        r.port_lo = 0;
        r.port_hi = 65535;
        r.action = v[0] == 'd' ? FW_DENY : FW_ALLOW;
        r.life = FW_LIFE_FOREVER;
        r.when = (long long)time(NULL);
        if (fw_rule_add(store, &r, NULL)) return JS_FALSE;
    }
    return JS_NewBool(c, save() == 0);
}

static JSValue f_set_trusted(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char app[FW_APP_MAX];
    if (argc < 2 || arg_app(c, argv[0], app)) return js_throw_code(c, "fw.invalidApp", NULL);
    if (fw_set_trusted(store, app, JS_ToBool(c, argv[1]) > 0)) return JS_FALSE;
    return JS_NewBool(c, save() == 0);
}

static JSValue f_set_restricted(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char app[FW_APP_MAX];
    if (argc < 2 || arg_app(c, argv[0], app)) return js_throw_code(c, "fw.invalidApp", NULL);
    uint32_t flags = 0, len = 0;
    if (!JS_IsArray(c, argv[1])) return js_throw_code(c, "fw.invalidRestrictions", NULL);
    JSValue lv = JS_GetPropertyStr(c, argv[1], "length");
    JS_ToUint32(c, &len, lv);
    JS_FreeValue(c, lv);
    if (len > 32) return js_throw_code(c, "fw.invalidRestrictions", NULL);
    for (uint32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(c, argv[1], i);
        const char *s = JS_IsString(e) ? JS_ToCString(c, e) : NULL;
        uint32_t f = s ? fw_flag_from_name(s) : 0;
        if (s) JS_FreeCString(c, s);
        JS_FreeValue(c, e);
        if (!f) return js_throw_code(c, "fw.invalidRestrictions", NULL);
        flags |= f;
    }
    uint32_t before = fw_restricted(store, app);
    if (fw_set_restricted(store, app, flags)) return JS_FALSE;
    int ok = save() == 0;
    restrict_changed(app, before, fw_restricted(store, app));
    return JS_NewBool(c, ok);
}

static JSValue f_blocks(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char app[FW_APP_MAX];
    if (argc < 2 || arg_app(c, argv[0], app)) return js_throw_code(c, "fw.invalidApp", NULL);
    const char *s = JS_IsString(argv[1]) ? JS_ToCString(c, argv[1]) : NULL;
    uint32_t f = s ? fw_flag_from_name(s) : 0;
    if (s) JS_FreeCString(c, s);
    if (!f) return js_throw_code(c, "fw.invalidRestrictions", NULL);
    return JS_NewBool(c, fw_blocks(store, app, f));
}

static JSValue f_set_mode(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char m[16];
    if (argc < 1 || !JS_IsString(argv[0])) return js_throw_code(c, "fw.invalidMode", NULL);
    const char *s = JS_ToCString(c, argv[0]);
    snprintf(m, sizeof m, "%s", s ? s : "");
    if (s) JS_FreeCString(c, s);
    FwMode mode = !strcmp(m, "silent") ? FW_MODE_SILENT : !strcmp(m, "ask") ? FW_MODE_ASK : !strcmp(m, "off") ? FW_MODE_OFF :
                  !strcmp(m, "deny") ? FW_MODE_DENY : (FwMode)-1;
    if ((int)mode < 0) return js_throw_code(c, "fw.invalidMode", NULL);
    fw_set_mode(store, mode);
    return JS_NewBool(c, save() == 0);
}

static JSValue rule_obj(JSContext *c, const FwRule *r) {
    JSValue o = JS_NewObject(c);
    char dst[FW_NAME_MAX + 8], port[16] = "";
    fw_format_dest(r, dst, sizeof dst);
    if (!(r->port_lo == 0 && r->port_hi == 65535)) {
        if (r->port_lo == r->port_hi) snprintf(port, sizeof port, "%u", r->port_lo);
        else snprintf(port, sizeof port, "%u-%u", r->port_lo, r->port_hi);
    }
    JS_SetPropertyStr(c, o, "id", JS_NewUint32(c, r->id));
    JS_SetPropertyStr(c, o, "app", JS_NewString(c, r->app));
    JS_SetPropertyStr(c, o, "dir", JS_NewString(c, r->dir == FW_IN ? "in" : "out"));
    JS_SetPropertyStr(c, o, "dst", JS_NewString(c, dst));
    JS_SetPropertyStr(c, o, "port", JS_NewString(c, port));
    JS_SetPropertyStr(c, o, "proto", JS_NewString(c, r->proto == FW_PROTO_TCP ? "tcp" : r->proto == FW_PROTO_UDP ? "udp" : ""));
    JS_SetPropertyStr(c, o, "action", JS_NewString(c, r->action == FW_DENY ? "deny" : "allow"));
    JS_SetPropertyStr(c, o, "when", JS_NewInt64(c, r->when));
    JS_SetPropertyStr(c, o, "lastMatch", JS_NewInt64(c, match_last(r->id)));
    return o;
}

static JSValue f_rules(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char app[FW_APP_MAX];
    if (argc < 1 || arg_app(c, argv[0], app)) return js_throw_code(c, "fw.invalidApp", NULL);
    pump();                                         /* fresh lastMatch values */
    JSValue arr = JS_NewArray(c);
    int n = fw_rule_list(store, app, NULL, 0);
    if (n <= 0) return arr;
    if (n > 4096) n = 4096;
    FwRule *v = malloc((size_t)n * sizeof *v);
    if (!v) return arr;
    int got = fw_rule_list(store, app, v, n);
    if (got < n) n = got < 0 ? 0 : got;
    for (int i = 0; i < n; i++) JS_SetPropertyUint32(c, arr, (uint32_t)i, rule_obj(c, &v[i]));
    free(v);
    return arr;
}

static int parse_port(const char *s, FwRule *r) {
    r->port_lo = 0; r->port_hi = 65535;
    if (!s[0]) return 0;
    unsigned lo = 0, hi = 0;
    char extra;
    int n = sscanf(s, "%u-%u%c", &lo, &hi, &extra);
    if (n == 1) hi = lo;
    else if (n != 2) return -1;
    if (lo < 1 || hi > 65535 || lo > hi) return -1;
    r->port_lo = (uint16_t)lo;
    r->port_hi = (uint16_t)hi;
    return 0;
}

/* A rule object from JS → r. app: the canonical app id to use (updateRule keeps the old rule's), or
 * NULL to read and check the object's "app". 0 ok; otherwise *exc holds the exception to return. */
static int rule_from_js(JSContext *c, JSValueConst o, const char *app, FwRule *r, JSValue *exc) {
    char given[FW_APP_MAX], canon[FW_APP_MAX], dir[8], dst[300], port[16], proto[8], action[8];
    *exc = JS_UNDEFINED;
    if (!JS_IsObject(o)) { *exc = js_throw_code(c, "fw.invalidRule", "field", "rule", NULL); return -1; }
    if (!app) {
        if (str_prop(c, o, "app", given, sizeof given) || fw_app_canon(given, canon, sizeof canon)) { *exc = js_throw_code(c, "fw.invalidApp", NULL); return -1; }
        app = canon;
    }
    if (str_prop(c, o, "dst", dst, sizeof dst)) { *exc = js_throw_code(c, "fw.invalidRule", "field", "dst", NULL); return -1; }
    if (str_prop(c, o, "dir", dir, sizeof dir) < 0 || str_prop(c, o, "port", port, sizeof port) < 0 ||
        str_prop(c, o, "proto", proto, sizeof proto) < 0 || str_prop(c, o, "action", action, sizeof action)) {
        *exc = js_throw_code(c, "fw.invalidRule", "field", "rule", NULL);
        return -1;
    }
    memset(r, 0, sizeof *r);
    snprintf(r->app, sizeof r->app, "%s", app);
    const char *bad = NULL;
    if (fw_parse_dest(dst, r)) { *exc = js_throw_code(c, "fw.invalidDestination", NULL); return -1; }
    if (!dir[0] || !strcmp(dir, "out")) r->dir = FW_OUT;
    else if (!strcmp(dir, "in")) r->dir = FW_IN;
    else bad = "dir";
    if (!bad && parse_port(port, r)) bad = "port";
    if (!bad) {
        if (!proto[0]) r->proto = FW_PROTO_ANY;
        else if (!strcmp(proto, "tcp")) r->proto = FW_PROTO_TCP;
        else if (!strcmp(proto, "udp")) r->proto = FW_PROTO_UDP;
        else bad = "proto";
    }
    if (!bad) {
        if (!strcmp(action, "allow")) r->action = FW_ALLOW;
        else if (!strcmp(action, "deny")) r->action = FW_DENY;
        else bad = "action";
    }
    if (bad) { *exc = js_throw_code(c, "fw.invalidRule", "field", bad, NULL); return -1; }
    r->life = FW_LIFE_FOREVER;
    r->when = (long long)time(NULL);
    return 0;
}

static JSValue f_add_rule(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    FwRule r;
    JSValue exc;
    if (argc < 1) return js_throw_code(c, "fw.invalidRule", "field", "rule", NULL);
    if (rule_from_js(c, argv[0], NULL, &r, &exc)) return exc;
    uint32_t id = 0;
    if (fw_rule_add(store, &r, &id)) return JS_FALSE;
    save();
    return JS_NewUint32(c, id);
}

/* Edits a rule: the new one replaces it (same app). If the new one can't be added, the old one is
 * put back, so a failed edit loses nothing. Adding a rule with the same destination, ports and
 * protocol as another one of the app replaces that one too (fw_rule_add), as addRule does. */
static JSValue f_update_rule(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    uint32_t id = 0;
    if (argc < 2 || JS_ToUint32(c, &id, argv[0]) || !id) return js_throw_code(c, "fw.invalidRule", "field", "id", NULL);
    const FwRule *cur = fw_rule_get(store, id);
    if (!cur) return JS_FALSE;
    FwRule old = *cur, r;
    JSValue exc;
    if (rule_from_js(c, argv[1], old.app, &r, &exc)) return exc;
    if (fw_rule_remove(store, id)) return JS_FALSE;
    uint32_t nid = 0;
    if (fw_rule_add(store, &r, &nid)) {
        old.id = 0;
        fw_rule_add(store, &old, NULL);
        return JS_FALSE;
    }
    save();
    return JS_NewUint32(c, nid);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* The apps that have rules, sorted by id. */
static JSValue f_rule_apps(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue arr = JS_NewArray(c);
    int n = fw_rule_list(store, NULL, NULL, 0);
    if (n <= 0) return arr;
    if (n > FW_MAX_RULES) n = FW_MAX_RULES;
    FwRule *v = malloc((size_t)n * sizeof *v);
    if (!v) return arr;
    int got = fw_rule_list(store, NULL, v, n);
    if (got < n) n = got < 0 ? 0 : got;
    const char **ids = malloc((size_t)n * sizeof *ids);
    if (!ids) { free(v); return arr; }
    for (int i = 0; i < n; i++) ids[i] = v[i].app;
    qsort(ids, (size_t)n, sizeof *ids, cmp_str);
    uint32_t k = 0;
    for (int i = 0; i < n; i++)
        if (!i || strcmp(ids[i], ids[i - 1])) JS_SetPropertyUint32(c, arr, k++, JS_NewString(c, ids[i]));
    free(ids);
    free(v);
    return arr;
}

static JSValue f_remove_rule(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    uint32_t id = 0;
    if (argc < 1 || JS_ToUint32(c, &id, argv[0]) || !id) return js_throw_code(c, "fw.invalidRule", "field", "id", NULL);
    if (fw_rule_remove(store, id)) return JS_FALSE;
    return JS_NewBool(c, save() == 0);
}

static JSValue f_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    pump();
    JSValue o = JS_NewObject(c);
    JSValue counts = counts_json ? JS_ParseJSON(c, counts_json, strlen(counts_json), "<fw.counts>") : JS_NewObject(c);
    if (JS_IsException(counts)) counts = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "counts", counts);
    JSValue arr = JS_NewArray(c);
    uint32_t k = 0;
    for (int i = 0; i < nevents; i++) {
        JSValue e = JS_ParseJSON(c, events[i], strlen(events[i]), "<fw.event>");
        if (JS_IsException(e)) { JSValue x = JS_GetException(c); JS_FreeValue(c, x); }
        else JS_SetPropertyUint32(c, arr, k++, e);
        free(events[i]);
    }
    nevents = 0;
    JS_SetPropertyStr(c, o, "events", arr);
    return o;
}

/* The latest COUNTS notice for the traffic history (monitor/netrec.c), after reading what arrived. */
const char *js_fw_counts(uint64_t *seq) {
    pump();
    *seq = counts_gen;
    return counts_json;
}

/* ---- blocklists (0.8.5) ---- */

static void lists_init(void) {
    if (lists_on) return;
    const char *h = getenv("HOME");
    char real[PATH_MAX], self[PATH_MAX];
    if (!h || !realpath(h, real)) return;
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return;
    self[n] = 0;
    char *sl = strrchr(self, '/');
    if (!sl) return;
    *sl = 0;
    if (!fws_init(real, js_sys_assets(), self)) lists_on = 1;
}

static JSValue f_lists(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    lists_init();
    pump();
    char *j = lists_on ? fws_json() : NULL;
    if (!j) return JS_NULL;
    JSValue v = JS_ParseJSON(c, j, strlen(j), "<fw.lists>");
    free(j);
    return v;
}

static int arg_list_id(JSContext *c, JSValueConst v, char *out) {
    if (!JS_IsString(v)) return -1;
    const char *s = JS_ToCString(c, v);
    if (!s) return -1;
    size_t n = strlen(s);
    int ok = n > 0 && n < 32;
    for (size_t i = 0; ok && i < n; i++) ok = (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-';
    if (ok) memcpy(out, s, n + 1);
    JS_FreeCString(c, s);
    return ok ? 0 : -1;
}

static JSValue f_set_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char id[32];
    if (argc < 2 || arg_list_id(c, argv[0], id)) return js_throw_code(c, "fw.list.unknown", NULL);
    lists_init();
    if (!lists_on || fws_set(id, JS_ToBool(c, argv[1]))) return js_throw_code(c, "fw.list.unknown", NULL);
    return JS_TRUE;
}

static JSValue f_add_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) return js_throw_code(c, "fw.list.badUrl", NULL);
    lists_init();
    if (!lists_on) return js_throw_code(c, "fw.list.unavailable", NULL);
    const char *url = JS_ToCString(c, argv[0]), *fmt = JS_ToCString(c, argv[1]);
    char id[32] = "", err[64] = "fw.list.badUrl";
    int r = url && fmt && strlen(url) <= FWS_URL_MAX ? fws_add_custom(url, fmt, id, err, sizeof err) : -1;
    if (url) JS_FreeCString(c, url);
    if (fmt) JS_FreeCString(c, fmt);
    if (r) return js_throw_code(c, err, NULL);
    return JS_NewString(c, id);
}

static JSValue f_remove_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char id[32];
    if (argc < 1 || arg_list_id(c, argv[0], id)) return js_throw_code(c, "fw.list.unknown", NULL);
    lists_init();
    if (!lists_on || fws_remove_custom(id)) return js_throw_code(c, "fw.list.unknown", NULL);
    return JS_TRUE;
}

static JSValue f_update_lists(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    lists_init();
    if (!lists_on) return JS_NewInt32(c, 1);
    return JS_NewInt32(c, fws_update(argc > 0 && JS_ToBool(c, argv[0])));
}

/* listMatch([names], addr): up to 8 names (each checked by the set's own normalizer) and an
 * address in text form, or null */
static JSValue f_list_match(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (!lists_on) return JS_NULL;
    const char *names[8] = { NULL };
    int n = 0;
    if (argc > 0 && JS_IsArray(c, argv[0])) {
        for (uint32_t i = 0; i < 8; i++) {
            JSValue v = JS_GetPropertyUint32(c, argv[0], i);
            if (JS_IsString(v)) { const char *s = JS_ToCString(c, v); if (s && strlen(s) < FW_NAME_MAX) names[n++] = s; else if (s) JS_FreeCString(c, s); }
            JS_FreeValue(c, v);
        }
    }
    uint8_t a[16] = { 0 };
    int fam = 0;
    if (argc > 1 && JS_IsString(argv[1])) {
        const char *s = JS_ToCString(c, argv[1]);
        if (s && inet_pton(AF_INET, s, a) == 1) fam = 4;
        else if (s && inet_pton(AF_INET6, s, a) == 1) fam = 6;
        if (s) JS_FreeCString(c, s);
    }
    const char *id = fws_match(names, n, fam, fam ? a : NULL);
    for (int i = 0; i < n; i++) JS_FreeCString(c, names[i]);
    return id ? JS_NewString(c, id) : JS_NULL;
}

static const JSCFunctionListEntry fw_funcs[] = {
    JS_CFUNC_DEF("lists", 0, f_lists),
    JS_CFUNC_DEF("setList", 2, f_set_list),
    JS_CFUNC_DEF("addList", 2, f_add_list),
    JS_CFUNC_DEF("removeList", 1, f_remove_list),
    JS_CFUNC_DEF("updateLists", 1, f_update_lists),
    JS_CFUNC_DEF("listMatch", 2, f_list_match),
    JS_CFUNC_DEF("status", 0, f_status),
    JS_CFUNC_DEF("state", 0, f_state),
    JS_CFUNC_DEF("appRule", 1, f_app_rule),
    JS_CFUNC_DEF("setAppRule", 2, f_set_app_rule),
    JS_CFUNC_DEF("setTrusted", 2, f_set_trusted),
    JS_CFUNC_DEF("setRestricted", 2, f_set_restricted),
    JS_CFUNC_DEF("setMode", 1, f_set_mode),
    JS_CFUNC_DEF("rules", 1, f_rules),
    JS_CFUNC_DEF("addRule", 1, f_add_rule),
    JS_CFUNC_DEF("removeRule", 1, f_remove_rule),
    JS_CFUNC_DEF("updateRule", 2, f_update_rule),
    JS_CFUNC_DEF("ruleApps", 0, f_rule_apps),
    JS_CFUNC_DEF("poll", 0, f_poll),
    JS_CFUNC_DEF("blocks", 2, f_blocks),
};

void js_fw_register(JSContext *c, JSValue lord) {
    if (!store) {
        store = fw_store_new();
        const char *h = getenv("HOME");
        char real[PATH_MAX];
        if (!h || !realpath(h, real)) snprintf(real, sizeof real, "/nonexistent");
        snprintf(path, sizeof path, "%s/.config/lordos/firewall.json", real);
        char err[200] = "";
        int r = store ? fw_load(store, path, err, sizeof err) : -1;
        if (r < 0) fprintf(stderr, "lord.fw: %s invalid (%s): starting without rules\n", path, err);
        np_map_rebuild();
        restrict_set_store(store);                  /* the broker and js_media.c check it (sec/restrict.h) */
        ensure();                                   /* lord-netperm reads this session's rules */
    }
    if (!store) return;
    JSValue fw = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, fw, fw_funcs, sizeof fw_funcs / sizeof fw_funcs[0]);
    JS_SetPropertyStr(c, lord, "fw", fw);
}
