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
    "transfer", "devices", "resolution", "nic-model", NULL,
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
    "qemu-3d-crash", "qemu-3d-stop", NULL,
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
