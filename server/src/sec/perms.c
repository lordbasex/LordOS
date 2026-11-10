/* perms.c — per-app permissions with system-drawn prompts (see perms.h). In-memory table + permisos.json. */
#include "perms.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *const names[PERM_COUNT] = {
    "files", "clipboard", "notifications", "network", "camera", "microphone",
    "screen", "listen", "usb", "bluetooth", "apps", "status-item", "shortcuts",
};

const char *perm_name(Perm p) { return (p >= 0 && p < PERM_COUNT) ? names[p] : "?"; }
int perm_from_name(const char *s) { for (int i = 0; i < PERM_COUNT; i++) if (!strcmp(names[i], s)) return i; return -1; }
int perm_declarable(const char *s) { int p = perm_from_name(s); return p >= 0 && p != PERM_SHORTCUTS; }

#define MAX_ENTRIES 512
typedef struct { char app[PERM_APP_MAX]; unsigned char state[PERM_COUNT]; long long when[PERM_COUNT]; int used; } Row;
static Row rows[MAX_ENTRIES];
static int nrows;
static char file_path[PATH_MAX];
static int loaded;

const char *perm_file_suffix(void) { return "/.config/lordos/permisos.json"; }

static Row *find(const char *app, int create) {
    for (int i = 0; i < nrows; i++) if (rows[i].used && !strcmp(rows[i].app, app)) return &rows[i];
    if (!create) return NULL;
    for (int i = 0; i < nrows; i++) if (!rows[i].used) { memset(&rows[i], 0, sizeof rows[i]); snprintf(rows[i].app, sizeof rows[i].app, "%s", app); rows[i].used = 1; return &rows[i]; }
    if (nrows < MAX_ENTRIES) { Row *r = &rows[nrows++]; memset(r, 0, sizeof *r); snprintf(r->app, sizeof r->app, "%s", app); r->used = 1; return r; }
    return NULL;
}

static void load(void) {
    loaded = 1;
    int fd = open(file_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    char buf[256 * 1024];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    JDoc d;
    if (json_parse(buf, (size_t)n, &d, NULL, 0)) { fprintf(stderr, "permisos.json is invalid: ignored\n"); return; }
    const JVal *apps = json_get(d.root, "apps");
    for (const JVal *a = apps && apps->type == J_OBJ ? apps->kid : NULL; a; a = a->next) {
        if (a->type != J_OBJ || !a->key || strlen(a->key) >= PERM_APP_MAX) continue;   /* cut short it would be another's */
        Row *r = find(a->key, 1);
        if (!r) continue;
        for (const JVal *p = a->kid; p; p = p->next) {
            int idx = perm_from_name(p->key);
            if (idx < 0 || p->type != J_OBJ) continue;
            const char *st = json_str(p, "state");
            r->state[idx] = st && !strcmp(st, "allow") ? PERM_ALLOW : st && !strcmp(st, "deny") ? PERM_DENY : PERM_ASK;
            r->when[idx] = (long long)json_num(p, "when", 0);
        }
    }
    json_free(&d);
}

void perm_init(const char *home) {
    snprintf(file_path, sizeof file_path, "%s%s", home, perm_file_suffix());
    load();
}

PermState perm_get(const char *app_id, Perm p) {
    if (!loaded || !app_id || p < 0 || p >= PERM_COUNT) return PERM_ASK;
    Row *r = find(app_id, 0);
    return r ? (PermState)r->state[p] : PERM_ASK;
}

static int save(void) {
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", file_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "{\n  \"version\": 1,\n  \"apps\": {\n");
    int firstApp = 1;
    char q[PERM_APP_MAX * 6 + 3];
    for (int i = 0; i < nrows; i++) {
        Row *r = &rows[i];
        if (!r->used) continue;
        int any = 0;
        for (int p = 0; p < PERM_COUNT; p++) if (r->state[p] != PERM_ASK) any = 1;
        if (!any) continue;
        json_quote(q, sizeof q, r->app);
        fprintf(f, "%s    %s: {\n", firstApp ? "" : ",\n", q);
        firstApp = 0;
        int firstP = 1;
        for (int p = 0; p < PERM_COUNT; p++) {
            if (r->state[p] == PERM_ASK) continue;
            fprintf(f, "%s      \"%s\": { \"state\": \"%s\", \"when\": %lld }", firstP ? "" : ",\n",
                    names[p], r->state[p] == PERM_ALLOW ? "allow" : "deny", r->when[p]);
            firstP = 0;
        }
        fprintf(f, "\n    }");
    }
    fprintf(f, "\n  }\n}\n");
    int ok = !ferror(f);
    if (fclose(f) || !ok) { unlink(tmp); return -1; }
    if (rename(tmp, file_path)) { unlink(tmp); return -1; }
    return 0;
}

int perm_set(const char *app_id, Perm p, PermState state) {
    if (!loaded || !app_id || p < 0 || p >= PERM_COUNT || strlen(app_id) >= PERM_APP_MAX) return -1;
    Row *r = find(app_id, 1);
    if (!r) return -1;
    r->state[p] = (unsigned char)state;
    r->when[p] = (long long)time(NULL);
    return save();
}

void perm_forget(const char *app_id) {
    Row *r = find(app_id, 0);
    if (r) { r->used = 0; save(); }
}

int perm_list(PermEntry *out, int max) {
    int n = 0;
    for (int i = 0; i < nrows && n < max; i++) {
        if (!rows[i].used) continue;
        for (int p = 0; p < PERM_COUNT && n < max; p++) {
            if (rows[i].state[p] == PERM_ASK) continue;
            memcpy(out[n].app, rows[i].app, sizeof out[n].app);     /* the same size, NUL included */
            out[n].perm = (Perm)p;
            out[n].state = (PermState)rows[i].state[p];
            out[n].when = rows[i].when[p];
            n++;
        }
    }
    return n;
}
