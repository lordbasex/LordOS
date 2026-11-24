/* mcli.c — the `machines` command's pure parts (mcli.h) */
#include "mcli.h"
#include "../sec/json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

long long mcli_size_mb(const char *s, long long bare_unit_mb) {
    if (!s) return -1;
    while (*s == ' ') s++;
    if (!isdigit((unsigned char)*s)) return -1;
    /* digits, at most one '.', then an optional unit */
    long long whole = 0, frac = 0, fdiv = 1;
    const char *p = s;
    while (isdigit((unsigned char)*p)) { if (whole > 100000000LL) return -1; whole = whole * 10 + (*p++ - '0'); }
    if (*p == '.') {
        p++;
        if (!isdigit((unsigned char)*p)) return -1;
        while (isdigit((unsigned char)*p)) { if (fdiv < 1000000) { frac = frac * 10 + (*p - '0'); fdiv *= 10; } p++; }
    }
    while (*p == ' ') p++;
    long long unit;
    if (!*p) unit = bare_unit_mb;
    else if (!strcasecmp(p, "m") || !strcasecmp(p, "mb") || !strcasecmp(p, "mib")) unit = 1;
    else if (!strcasecmp(p, "g") || !strcasecmp(p, "gb") || !strcasecmp(p, "gib")) unit = 1024;
    else if (!strcasecmp(p, "t") || !strcasecmp(p, "tb") || !strcasecmp(p, "tib")) unit = 1024 * 1024;
    else return -1;
    long long mb = whole * unit + frac * unit / fdiv;
    if (frac * unit % fdiv) return -1;          /* not a whole MB */
    return mb >= 1 && mb <= 64LL * 1024 * 1024 ? mb : -1;
}

/* ASCII case folded comparison of the first n bytes */
static int prefix_ci(const char *s, const char *q, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!s[i]) return 0;
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)q[i])) return 0;
    }
    return 1;
}

int mcli_resolve(const char *const *names, int n, const char *q, int *out, int max) {
    if (!q || !q[0]) return 0;
    size_t ql = strlen(q);
    int k = 0;
    for (int i = 0; i < n; i++)
        if (names[i] && strlen(names[i]) == ql && prefix_ci(names[i], q, ql)) { if (k < max) out[k] = i; k++; }
    if (k) return k;
    for (int i = 0; i < n; i++)
        if (names[i] && prefix_ci(names[i], q, ql)) { if (k < max) out[k] = i; k++; }
    return k;
}

/* lord-vm --set's options that pass as they are (their values are checked by lord-vm) */
static const char *const PASS[] = {
    "name", "display", "bridge", "port", "ports", "share", "shares", "iso", "agent", "clipboard", "clipboard-ask",
    "graphics", "gpu", "boot-order", "boot-menu", "sound", "volume", "microphone", "sound-out", "sound-in", "pointer",
    "transfer", "devices", "resolution", "nic-model", "cpu-pin", "perf-profile", "capture-fullscreen", NULL,
};

static char *keep(McliStore *st, const char *fmt, long long v) {
    if (st->n >= (int)(sizeof st->v / sizeof st->v[0])) return NULL;
    char *o = st->v[st->n++];
    snprintf(o, sizeof st->v[0], fmt, v);
    return o;
}
static char *keep_s(McliStore *st, const char *s, size_t len) {
    if (st->n >= (int)(sizeof st->v / sizeof st->v[0]) || len + 1 > sizeof st->v[0]) return NULL;
    char *o = st->v[st->n++];
    memcpy(o, s, len);
    o[len] = 0;
    return o;
}

int mcli_set_args(int argc, char **args, char **out, int max, McliStore *store, const char **bad_opt, const char **bad_val) {
    int k = 0;
    *bad_opt = *bad_val = NULL;
    store->n = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = args[i];
        *bad_opt = a; *bad_val = NULL;
        if (strncmp(a, "--", 2) || !a[2]) return -1;
        /* --opt=value or --opt value */
        const char *eq = strchr(a, '='), *val;
        char *opt;
        if (eq) { opt = keep_s(store, a + 2, (size_t)(eq - a - 2)); val = eq + 1; }
        else { opt = keep_s(store, a + 2, strlen(a + 2)); if (i + 1 >= argc) return -1; val = args[++i]; }
        if (!opt) return -1;
        *bad_val = val;
        if (k + 3 > max) return -1;
        if (!strcmp(opt, "memory")) {
            long long mb = mcli_size_mb(val, 1);
            char *v = mb > 0 ? keep(store, "%lld", mb) : NULL;
            if (!v) return -1;
            out[k++] = "--memory"; out[k++] = v;
        } else if (!strcmp(opt, "cpus") || !strcmp(opt, "vcpus")) {
            char *end = NULL;
            long n = strtol(val, &end, 10);
            char *v = *val && !*end && n >= 1 && n <= 64 ? keep(store, "%lld", n) : NULL;
            if (!v) return -1;
            out[k++] = "--vcpus"; out[k++] = v;
        } else if (!strcmp(opt, "disk")) {
            long long mb = mcli_size_mb(val, 1024);
            char *v = mb > 0 && mb % 1024 == 0 ? keep(store, "%lld", mb / 1024) : NULL;
            if (!v) return -1;
            out[k++] = "--disk"; out[k++] = v;
        } else if (!strcmp(opt, "network")) {
            const char *n = !strcmp(val, "host-only") ? "hostonly" : val;
            if (strcmp(n, "shared") && strcmp(n, "isolated") && strcmp(n, "bridged") && strcmp(n, "hostonly")) return -1;
            out[k++] = "--network"; out[k++] = (char *)n;
        } else {
            int known = 0;
            for (int p = 0; PASS[p]; p++) if (!strcmp(opt, PASS[p])) known = 1;
            if (!known) { *bad_val = NULL; return -1; }
            char *o = keep_s(store, a, eq ? (size_t)(eq - a) : strlen(a));
            if (!o) return -1;
            out[k++] = o; out[k++] = (char *)val;
        }
    }
    out[k] = NULL;
    *bad_opt = *bad_val = NULL;
    return k ? 0 : -1;
}

void mcli_folder_name(const char *name, char *out, size_t n) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && o + 1 < n && o < 48; p++) {
        int ok = (*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'z') || *p == ' ' || *p == '-' || *p == '_' || (*p == '.' && o);
        if (*p >= 0x80) { while ((p[1] & 0xc0) == 0x80) p++; ok = 0; }
        char ch = ok ? (char)*p : '-';
        if (ch == '-' && o && out[o - 1] == '-') continue;
        out[o++] = ch;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '-' || out[o - 1] == '.')) o--;
    out[o] = 0;
    if (!o) snprintf(out, n, "Machine");
}

static const char *const STATES[] = { "stopped", "starting", "running", "paused", "stopping", "crashed", NULL };
static const char *const REASONS[] = {
    "none", "guest-shutdown", "host-shutdown", "forced", "poweroff", "qemu-failed", "qemu-signal", "out-of-memory",
    "start-failed", "guest-panic", "supervisor-lost", "reset", "user", "suspended", "resume-failed", "installed",
    "qemu-3d-crash", "qemu-3d-stop", "session-full", NULL,
};
static int listed(const char *const *l, const char *s) { for (int i = 0; s && l[i]; i++) if (!strcmp(l[i], s)) return 1; return 0; }

long mcli_run_pid(const char *runjson, size_t len) {
    JDoc d;
    if (!runjson || !len || json_parse(runjson, len, &d, NULL, 0)) return 0;
    double p = json_num(d.root, "pid", 0);
    json_free(&d);
    return p > 0 && p < 1e9 ? (long)p : 0;
}

void mcli_state(const char *runjson, size_t len, int alive, McliState *st) {
    memset(st, 0, sizeof *st);
    snprintf(st->state, sizeof st->state, "stopped");
    snprintf(st->reason, sizeof st->reason, "none");
    JDoc d;
    if (runjson && len && !json_parse(runjson, len, &d, NULL, 0) && d.root && d.root->type == J_OBJ) {
        const char *s = json_str(d.root, "state"), *r = json_str(d.root, "reason"), *si = json_str(d.root, "since"), *de = json_str(d.root, "detail");
        if (listed(STATES, s)) snprintf(st->state, sizeof st->state, "%s", s);
        if (listed(REASONS, r)) snprintf(st->reason, sizeof st->reason, "%s", r);
        if (si) snprintf(st->since, sizeof st->since, "%s", si);
        if (de) snprintf(st->detail, sizeof st->detail, "%s", de);
        double p = json_num(d.root, "pid", 0), q = json_num(d.root, "qemu_pid", 0);
        st->pid = p > 0 && p < 1e9 ? (long)p : 0;
        st->qemu_pid = q > 0 && q < 1e9 ? (int)q : 0;
        json_free(&d);
    }
    int act = !strcmp(st->state, "starting") || !strcmp(st->state, "running") || !strcmp(st->state, "paused") || !strcmp(st->state, "stopping");
    /* "active" with no lord-vm behind it: its supervisor died; lord-vm recovers it at the next start (js_vm.c run_state) */
    if (act && !alive) { snprintf(st->state, sizeof st->state, "stopped"); snprintf(st->reason, sizeof st->reason, "supervisor-lost"); act = 0; }
    if (!act) st->qemu_pid = 0;
    st->active = act;
    /* what Machines shows (apps/machines/model.js stateView) */
    const char *v = st->state, *r = st->reason;
    if (!strcmp(v, "stopping") && !strcmp(r, "suspended")) v = "suspending";
    else if (!strcmp(v, "stopped")) v = !strcmp(r, "supervisor-lost") ? "recovered" : !strcmp(r, "suspended") ? "suspended" : "stopped";
    snprintf(st->view, sizeof st->view, "%s", v);
}

void mcli_fill(const char *tmpl, const char *const *kv, char *out, size_t n) {
    size_t o = 0;
    if (!n) return;
    for (const char *p = tmpl ? tmpl : ""; *p && o + 1 < n;) {
        if (*p == '{') {
            const char *e = strchr(p + 1, '}');
            const char *val = NULL;
            if (e) for (int i = 0; kv && kv[i] && kv[i + 1]; i += 2)
                if (strlen(kv[i]) == (size_t)(e - p - 1) && !memcmp(kv[i], p + 1, (size_t)(e - p - 1))) { val = kv[i + 1]; break; }
            if (val) {
                for (; *val && o + 1 < n; val++) out[o++] = *val;
                p = e + 1;
                continue;
            }
        }
        out[o++] = *p++;
    }
    /* never end inside a UTF-8 character */
    while (o && ((unsigned char)out[o - 1] & 0xc0) == 0x80) {
        size_t s = o - 1;
        while (s && ((unsigned char)out[s] & 0xc0) == 0x80) s--;
        unsigned char c = (unsigned char)out[s];
        size_t want = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        if (o - s >= want) break;
        o = s;
    }
    if (o && (unsigned char)out[o - 1] >= 0xc0) o--;
    out[o] = 0;
}

void mcli_json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p == '\n') fputs("\\n", f);
        else if (*p == '\t') fputs("\\t", f);
        else if (*p < 0x20 || *p == 0x7f) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}

void mcli_bytes(double b, char *out, size_t n) {
    if (b >= 1e12) snprintf(out, n, "%.1f TB", b / 1e12);
    else if (b >= 1e9) snprintf(out, n, "%.1f GB", b / 1e9);
    else snprintf(out, n, "%.0f MB", b / 1e6);
}

/* ---- exec, cp, logs, mcp and apply (docs/VIRTUALIZATION.md 6.15, 6.24) ---- */

int mcli_cp_side(const char *arg, char *name, size_t nn, const char **path) {
    const char *c = arg ? strchr(arg, ':') : NULL;
    if (!c || c == arg || memchr(arg, '/', (size_t)(c - arg)) || (size_t)(c - arg) >= nn) return 0;
    memcpy(name, arg, (size_t)(c - arg));
    name[c - arg] = 0;
    *path = c[1] ? c + 1 : "~";
    return 1;
}

/* one UTF-8 character's length at p (1..4) when it is a valid one within n bytes, else 0 */
static size_t utf8_len(const unsigned char *p, size_t n) {
    unsigned c = p[0];
    if (c < 0x80) return 1;
    size_t k = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!k || k > n) return 0;
    for (size_t j = 1; j < k; j++) if ((p[j] & 0xc0) != 0x80) return 0;
    if ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] > 0x9f) || (c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] > 0x8f)) return 0;
    return k;
}

void mcli_json_bytes(FILE *f, const uint8_t *b, size_t n) {
    fputc('"', f);
    for (size_t i = 0; i < n;) {
        unsigned c = b[i];
        if (c == '"' || c == '\\') { fputc('\\', f); fputc((int)c, f); i++; continue; }
        if (c == '\n') { fputs("\\n", f); i++; continue; }
        if (c == '\t') { fputs("\\t", f); i++; continue; }
        if (c == '\r') { fputs("\\r", f); i++; continue; }
        if (!c) { fputs("\\ufffd", f); i++; continue; }        /* NUL: many JSON readers refuse it */
        if (c < 0x20 || c == 0x7f) { fprintf(f, "\\u%04x", c); i++; continue; }
        size_t k = utf8_len(b + i, n - i);
        if (!k) { fputs("\\ufffd", f); i++; continue; }
        fwrite(b + i, 1, k, f);
        i += k;
    }
    fputc('"', f);
}

size_t mcli_term_clean(const uint8_t *b, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < cap;) {
        unsigned c = b[i];
        if (c == '\n' || c == '\t') { out[o++] = (char)c; i++; continue; }
        if (c < 0x20 || c == 0x7f) { out[o++] = '?'; i++; continue; }
        size_t k = utf8_len(b + i, n - i);
        /* C1 controls (U+0080..U+009F) act like escapes on some terminals */
        if (!k) { out[o++] = '?'; i++; continue; }
        if (k == 2 && c == 0xc2 && b[i + 1] < 0xa0) { out[o++] = '?'; i += 2; continue; }
        memcpy(out + o, b + i, k);
        o += k;
        i += k;
    }
    out[o] = 0;
    return o;
}

/* ---- a reproducible machine's file (6.24): a small, strict subset of TOML ---- */

static const char *const APPLY_KEYS[] = { "name", "arch", "memory", "cpus", "disk", "network", "iso", "graphics", "agent", "clipboard", "ports", "shares", NULL };

static int apply_string(const char **pp, const char *end, char *out, size_t cap) {
    const char *p = *pp;
    if (p >= end || *p != '"') return -1;
    p++;
    size_t o = 0;
    while (p < end && *p != '"') {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return -1;
        if (c == '\\') {
            if (p + 1 >= end) return -1;
            char e = p[1];
            c = e == '"' ? '"' : e == '\\' ? '\\' : e == 't' ? '\t' : 0;
            if (!c) return -1;
            p += 2;
        } else {
            size_t k = utf8_len((const unsigned char *)p, (size_t)(end - p));
            if (!k) return -1;
            if (k > 1) {
                if (o + k >= cap) return -1;
                memcpy(out + o, p, k);
                o += k; p += k;
                continue;
            }
            p++;
        }
        if (o + 1 >= cap) return -1;
        out[o++] = (char)c;
    }
    if (p >= end) return -1;
    out[o] = 0;
    *pp = p + 1;
    return 0;
}

static const char *skip_ws(const char *p, const char *end) { while (p < end && (*p == ' ' || *p == '\t')) p++; return p; }

int mcli_apply_parse(const char *text, size_t n, McliApply *a, int *line, const char **why) {
    memset(a, 0, sizeof *a);
    a->cpus = -1; a->agent = -1;
    *line = 0;
    *why = "";
    if (!text || n > MCLI_APPLY_MAX) { *why = "the file is larger than 64 KB"; return -1; }
    if (memchr(text, 0, n)) { *why = "the file holds a NUL byte"; return -1; }
    unsigned seen = 0;
    int table = 0;
    const char *p = text, *end = text + n;
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        if (!e) e = end;
        (*line)++;
        const char *q = skip_ws(p, e), *le = e;
        if (le > q && le[-1] == '\r') le--;
        if (q < le && *q != '#') {
            if (*q == '[') {
                /* the one table this file may have, first */
                if (table || seen || (size_t)(le - q) < 9 || memcmp(q, "[machine]", 9)) { *why = "only one [machine] table, before every key"; return -1; }
                const char *t = skip_ws(q + 9, le);
                if (t < le && *t != '#') { *why = "something after [machine]"; return -1; }
                table = 1;
            } else {
                const char *k = q;
                while (q < le && ((*q >= 'a' && *q <= 'z') || *q == '_')) q++;
                size_t kl = (size_t)(q - k);
                int ki = -1;
                for (int i = 0; APPLY_KEYS[i]; i++) if (strlen(APPLY_KEYS[i]) == kl && !memcmp(APPLY_KEYS[i], k, kl)) ki = i;
                if (ki < 0) { *why = "an unknown key"; return -1; }
                if (seen & (1u << ki)) { *why = "a key given twice"; return -1; }
                seen |= 1u << ki;
                q = skip_ws(q, le);
                if (q >= le || *q != '=') { *why = "a key without = and a value"; return -1; }
                q = skip_ws(q + 1, le);
                char sv[MCLI_APPLY_STR];
                const char *key = APPLY_KEYS[ki];
                if (!strcmp(key, "ports") || !strcmp(key, "shares")) {
                    int ports = key[0] == 'p';
                    if (q >= le || *q != '[') { *why = "a list in [ ]"; return -1; }
                    q = skip_ws(q + 1, le);
                    while (q < le && *q != ']') {
                        if (apply_string(&q, le, sv, ports ? sizeof a->ports[0] : sizeof a->shares[0])) { *why = "a list of strings"; return -1; }
                        if (ports ? a->n_ports >= MCLI_APPLY_PORTS : a->n_shares >= MCLI_APPLY_SHARES) { *why = "too many items"; return -1; }
                        if (ports) snprintf(a->ports[a->n_ports++], sizeof a->ports[0], "%s", sv);
                        else snprintf(a->shares[a->n_shares++], sizeof a->shares[0], "%s", sv);
                        q = skip_ws(q, le);
                        if (q < le && *q == ',') q = skip_ws(q + 1, le);
                        else if (q < le && *q != ']') { *why = "items are separated by commas"; return -1; }
                    }
                    if (q >= le) { *why = "a list ends on its line with ]"; return -1; }
                    q++;
                    if (ports) a->ports_set = 1; else a->shares_set = 1;
                } else if (!strcmp(key, "cpus")) {
                    long v = 0;
                    const char *d = q;
                    while (q < le && *q >= '0' && *q <= '9' && q - d < 4) v = v * 10 + (*q++ - '0');
                    if (q == d || (q - d > 1 && *d == '0') || v < 1 || v > 64 || (q < le && *q >= '0' && *q <= '9')) { *why = "cpus is a whole number from 1 to 64"; return -1; }
                    a->cpus = (int)v;
                } else if (!strcmp(key, "agent")) {
                    if (le - q >= 4 && !memcmp(q, "true", 4)) { a->agent = 1; q += 4; }
                    else if (le - q >= 5 && !memcmp(q, "false", 5)) { a->agent = 0; q += 5; }
                    else { *why = "agent is true or false"; return -1; }
                } else {
                    if (apply_string(&q, le, sv, sizeof sv)) { *why = "a value in double quotes"; return -1; }
                    char *dst = !strcmp(key, "name") ? a->name : !strcmp(key, "arch") ? a->arch : !strcmp(key, "memory") ? a->memory : !strcmp(key, "disk") ? a->disk :
                                !strcmp(key, "network") ? a->network : !strcmp(key, "iso") ? a->iso : !strcmp(key, "graphics") ? a->graphics : a->clipboard;
                    size_t cap = !strcmp(key, "iso") ? sizeof a->iso : !strcmp(key, "name") ? sizeof a->name : sizeof a->arch;
                    if (!sv[0] || strlen(sv) >= cap) { *why = "a value that is empty or too long"; return -1; }
                    memcpy(dst, sv, strlen(sv) + 1);
                }
                q = skip_ws(q, le);
                if (q < le && *q != '#') { *why = "something after the value"; return -1; }
            }
        }
        p = e < end ? e + 1 : end;
    }
    if (!a->name[0]) { *line = 0; *why = "name is missing"; return -1; }
    *line = 0;
    return 0;
}

/* ---- MCP (JSON-RPC 2.0, one message per line) ---- */

int mcli_rpc_parse(const char *line, size_t n, McliRpc *r) {
    memset(r, 0, sizeof *r);
    if (!line || !n || n > MCLI_RPC_MAX) return -1;
    if (json_parse(line, n, &r->doc, NULL, 0)) return -1;
    const JVal *root = r->doc.root;
    if (!root || root->type != J_OBJ) { json_free(&r->doc); return -1; }
    const JVal *v = json_get(root, "jsonrpc"), *m = json_get(root, "method"), *id = json_get(root, "id"), *pa = json_get(root, "params");
    if (!v || v->type != J_STR || strcmp(v->str, "2.0") || !m || m->type != J_STR || !m->str[0] || strlen(m->str) > 64 ||
        (pa && pa->type != J_OBJ && pa->type != J_ARR) || (id && id->type != J_NUM && id->type != J_STR)) { json_free(&r->doc); return -1; }
    if (id && id->type == J_STR && strlen(id->str) > 128) { json_free(&r->doc); return -1; }
    if (id && id->type == J_NUM && (id->num != (double)(long long)id->num || id->num < -9e15 || id->num > 9e15)) { json_free(&r->doc); return -1; }
    for (const JVal *k = root->kid; k; k = k->next)
        if (strcmp(k->key, "jsonrpc") && strcmp(k->key, "method") && strcmp(k->key, "id") && strcmp(k->key, "params")) { json_free(&r->doc); return -1; }
    r->method = m->str;
    r->params = pa;
    r->has_id = id != NULL;
    if (id && id->type == J_NUM) snprintf(r->id, sizeof r->id, "%lld", (long long)id->num);
    else if (id) {
        FILE *f = fmemopen(r->id, sizeof r->id, "w");
        if (!f) { json_free(&r->doc); return -1; }
        mcli_json_str(f, id->str);
        fclose(f);
    }
    return 0;
}

void mcli_rpc_free(McliRpc *r) { json_free(&r->doc); memset(r, 0, sizeof *r); }
