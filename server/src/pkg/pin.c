/* pin.c — apt's preferences for the package engine (see pin.h). */
#include "pin.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PIN_FILES 64
#define PIN_FILE_MAX (256 * 1024)
#define PIN_MAX 512
#define PAT_MAX 32

const char *pin_root = "";

enum { PT_ORIGIN, PT_RELEASE, PT_VERSION };

typedef struct {
    char *text;                         /* glob, regex body or source name */
    regex_t re;
    unsigned char kind;                 /* 0 glob, 1 regex, 2 src:name */
} Pat;

typedef struct {
    Pat pat[PAT_MAX];
    int npat, general, type, prio;
    char *value;                        /* origin host or version glob */
    char *rel_a, *rel_n, *rel_c, *rel_o, *rel_l;   /* release fields (NULL: any) */
    int rel_other;                      /* a release key we can't check (v=, b=…): never matches */
} Pin;

struct PinSet { Pin pin[PIN_MAX]; int n; };

/* ---- reading ---- */

static char *read_file(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > PIN_FILE_MAX) { close(fd); return NULL; }
    char *buf = malloc((size_t)st.st_size + 1);
    size_t n = 0;
    while (buf && n < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + n, (size_t)st.st_size - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    if (!buf) return NULL;
    buf[n] = 0;
    *len = n;
    return buf;
}

/* apt's rule for preferences.d names: [A-Za-z0-9_.-] only, no extension or ".pref". */
static int good_name(const char *n) {
    if (!n[0] || n[0] == '.') return 0;
    for (const char *p = n; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.') return 0;
    const char *dot = strrchr(n, '.');
    return !dot || !strcmp(dot, ".pref");
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The files in apt's order; returns how many paths were stored. */
static int list_files(char paths[][512]) {
    int n = 0;
    snprintf(paths[n++], 512, "%s/etc/apt/preferences", pin_root);
    char dir[300];
    snprintf(dir, sizeof dir, "%s/etc/apt/preferences.d", pin_root);
    DIR *d = opendir(dir);
    if (!d) return n;
    char *names[PIN_FILES];
    int nn = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nn < PIN_FILES)
        if (good_name(e->d_name) && strlen(e->d_name) < 200) names[nn++] = strdup(e->d_name);
    closedir(d);
    qsort(names, (size_t)nn, sizeof *names, cmp_str);
    for (int i = 0; i < nn; i++) {
        if (names[i] && n < PIN_FILES + 1) snprintf(paths[n++], 512, "%s/%s", dir, names[i]);
        free(names[i]);
    }
    return n;
}

unsigned long pin_stamp(void) {
    char paths[PIN_FILES + 1][512];
    int n = list_files(paths);
    unsigned long h = 1469598103u;
    for (int i = 0; i < n; i++) {
        struct stat st;
        if (stat(paths[i], &st)) continue;
        for (const char *p = paths[i]; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211u;
        h = (h ^ (unsigned long)st.st_size) * 1099511628211u;
        h = (h ^ (unsigned long)st.st_mtime) * 1099511628211u;
        h = (h ^ (unsigned long)st.st_mtim.tv_nsec) * 1099511628211u;
    }
    return h;
}

/* ---- parsing ---- */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

static void pin_clear(Pin *p) {
    for (int i = 0; i < p->npat; i++) {
        if (p->pat[i].kind == 1) regfree(&p->pat[i].re);
        free(p->pat[i].text);
    }
    free(p->value); free(p->rel_a); free(p->rel_n); free(p->rel_c); free(p->rel_o); free(p->rel_l);
    memset(p, 0, sizeof *p);
}

static int parse_packages(Pin *p, char *v) {
    if (!strcmp(v, "*")) { p->general = 1; return 0; }
    for (char *tok = strtok(v, " \t"); tok; tok = strtok(NULL, " \t")) {
        if (p->npat == PAT_MAX) return -1;
        Pat *t = &p->pat[p->npat];
        size_t l = strlen(tok);
        if (l > 2 && tok[0] == '/' && tok[l - 1] == '/') {
            tok[l - 1] = 0;
            if (regcomp(&t->re, tok + 1, REG_EXTENDED | REG_NOSUB)) return -1;
            t->kind = 1;
            t->text = strdup(tok + 1);
        } else if (!strncmp(tok, "src:", 4) && tok[4]) {
            t->kind = 2;
            t->text = strdup(tok + 4);
        } else {
            t->kind = 0;
            t->text = strdup(tok);
        }
        if (!t->text) { if (t->kind == 1) regfree(&t->re); return -1; }
        p->npat++;
    }
    return p->npat ? 0 : -1;
}

static char *unquote(const char *s) {
    size_t l = strlen(s);
    if (l >= 2 && s[0] == '"' && s[l - 1] == '"') return strndup(s + 1, l - 2);
    return strdup(s);
}

static int parse_pin(Pin *p, char *v) {
    if (!strncmp(v, "origin", 6) && (v[6] == ' ' || v[6] == '\t')) {
        p->type = PT_ORIGIN;
        p->value = unquote(trim(v + 7));
        return p->value ? 0 : -1;
    }
    if (!strncmp(v, "version", 7) && (v[7] == ' ' || v[7] == '\t')) {
        p->type = PT_VERSION;
        p->value = strdup(trim(v + 8));
        return p->value && p->value[0] ? 0 : -1;
    }
    if (!strncmp(v, "release", 7) && (v[7] == ' ' || v[7] == '\t' || !v[7])) {
        p->type = PT_RELEASE;
        char *rest = v + 7;
        for (char *tok = strtok(rest, ","); tok; tok = strtok(NULL, ",")) {
            tok = trim(tok);
            if (!*tok) continue;
            char *eq = strchr(tok, '=');
            if (!eq) { p->rel_other = 1; continue; }        /* a bare version number */
            *eq = 0;
            char *key = trim(tok), *val = unquote(trim(eq + 1)), **slot = NULL;
            if (!val) return -1;
            if (!strcmp(key, "a")) slot = &p->rel_a;
            else if (!strcmp(key, "n")) slot = &p->rel_n;
            else if (!strcmp(key, "c")) slot = &p->rel_c;
            else if (!strcmp(key, "o")) slot = &p->rel_o;
            else if (!strcmp(key, "l")) slot = &p->rel_l;
            if (slot && !*slot) *slot = val;
            else { free(val); p->rel_other = 1; }
        }
        return 0;
    }
    return -1;
}

static void parse_text(PinSet *set, char *text) {
    Pin cur;
    memset(&cur, 0, sizeof cur);
    int have_pkg = 0, have_pin = 0, have_prio = 0, bad = 0;
    char *next = text;
    for (;;) {                               /* line by line: blank lines end a stanza */
        char *line = next;
        if (line) {
            char *nl = strchr(line, '\n');
            if (nl) { *nl = 0; next = nl + 1; } else next = NULL;
            if (!nl && !*line) line = NULL;
        }
        int end = !line;
        char *l = end ? NULL : trim(line);
        if (end || !*l) {                    /* blank line or end: close the stanza */
            if (have_pkg && have_pin && have_prio && !bad && set->n < PIN_MAX) set->pin[set->n++] = cur;
            else pin_clear(&cur);
            memset(&cur, 0, sizeof cur);
            have_pkg = have_pin = have_prio = bad = 0;
            if (end) break;
            continue;
        }
        if (*l == '#') continue;
        char *colon = strchr(l, ':');
        if (!colon) { bad = 1; continue; }
        *colon = 0;
        char *key = trim(l), *val = trim(colon + 1);
        if (!strcasecmp(key, "Package")) { if (have_pkg || parse_packages(&cur, val)) bad = 1; have_pkg = 1; }
        else if (!strcasecmp(key, "Pin")) { if (have_pin || parse_pin(&cur, val)) bad = 1; have_pin = 1; }
        else if (!strcasecmp(key, "Pin-Priority")) {
            char *e;
            long v = strtol(val, &e, 10);
            if (have_prio || e == val || *trim(e) || v < -32768 || v > 32767) bad = 1;
            cur.prio = (int)v;
            have_prio = 1;
        } else if (strcasecmp(key, "Explanation")) bad = 1;
    }
}

PinSet *pin_load(void) {
    char paths[PIN_FILES + 1][512];
    int n = list_files(paths);
    PinSet *set = calloc(1, sizeof *set);
    if (!set) return NULL;
    for (int i = 0; i < n; i++) {
        size_t len;
        char *text = read_file(paths[i], &len);
        if (!text) continue;
        parse_text(set, text);
        free(text);
    }
    if (!set->n) { free(set); return NULL; }
    return set;
}

void pin_free(PinSet *p) {
    if (!p) return;
    for (int i = 0; i < p->n; i++) pin_clear(&p->pin[i]);
    free(p);
}

/* ---- matching ---- */

static int pkg_matches(const Pin *p, const char *name, const char *source) {
    for (int i = 0; i < p->npat; i++) {
        const Pat *t = &p->pat[i];
        if (t->kind == 0 && !fnmatch(t->text, name, 0)) return 1;
        if (t->kind == 1 && !regexec(&t->re, name, 0, NULL, 0)) return 1;
        if (t->kind == 2 && !strcmp(t->text, source[0] ? source : name)) return 1;
    }
    return 0;
}

/* The host of a source URI ("http://archive.raspberrypi.com/debian" → archive.raspberrypi.com). */
static void uri_host(const char *uri, char *out, size_t n) {
    out[0] = 0;
    const char *p = strstr(uri, "://");
    if (!p) return;                     /* file: and other local sources have the origin "" */
    p += 3;
    const char *at = strpbrk(p, "@/");
    if (at && *at == '@') p = at + 1;
    size_t l = strcspn(p, ":/");
    if (l >= n) l = n - 1;
    memcpy(out, p, l);
    out[l] = 0;
}

static int field_ok(const char *want, const char *have) {
    return !want || !fnmatch(want, have, 0);
}

static int pin_matches(const Pin *p, const Idx *x, long rec) {
    const IdxPkg *k = &x->pkg[rec];
    const IdxSrc *s = &x->src[k->src];
    switch (p->type) {
    case PT_ORIGIN: {
        char host[256];
        uri_host(idx_s(x, s->uri), host, sizeof host);
        return !strcasecmp(p->value, host);
    }
    case PT_VERSION:
        return !fnmatch(p->value, idx_s(x, k->version), 0);
    case PT_RELEASE:
        return !p->rel_other && field_ok(p->rel_a, idx_s(x, s->suite)) && field_ok(p->rel_n, idx_s(x, s->codename)) &&
               field_ok(p->rel_c, idx_s(x, s->component)) && field_ok(p->rel_o, idx_s(x, s->origin)) &&
               field_ok(p->rel_l, idx_s(x, s->label));
    }
    return 0;
}

int pin_priority(const PinSet *set, const Idx *x, long rec) {
    const IdxPkg *k = &x->pkg[rec];
    int dflt = (int)x->src[k->src].prio;
    if (!set) return dflt;
    const char *name = idx_s(x, k->name), *source = idx_s(x, k->source);
    /* the first specific pin naming the package decides, for the versions it matches */
    for (int i = 0; i < set->n; i++) {
        const Pin *p = &set->pin[i];
        if (p->general || !pkg_matches(p, name, source)) continue;
        if (pin_matches(p, x, rec)) return p->prio;
        break;
    }
    /* then the first general pin matching the version's source */
    for (int i = 0; i < set->n; i++) {
        const Pin *p = &set->pin[i];
        if (p->general && pin_matches(p, x, rec)) return p->prio;
    }
    return dflt;
}
