/* sharegrant.c — see sharegrant.h. */
#include "sharegrant.h"
#include "../sec/json.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int id_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+' || c == '@' || c == '~';
}

int sg_id_valid(const char *id) {
    const char *p;
    if (!id) return 0;
    if (!strncmp(id, "desktop:", 8)) p = id + 8;
    else if (!strncmp(id, "lpk:", 4)) p = id + 4;
    else return 0;
    size_t l = strlen(id);
    if (!*p || l >= SG_ID_MAX || p[0] == '.') return 0;
    for (; *p; p++) if (!id_char((unsigned char)*p)) return 0;
    return 1;
}

/* Only what a path needs: the action's argument is split on spaces, and this text goes into JSON and in front
 * of the person in a prompt. A program elsewhere has no valid path, so it gets no stored grant. */
int sg_exe_valid(const char *e) {
    if (!e || e[0] != '/' || strlen(e) >= SG_EXE_MAX || strstr(e, "..")) return 0;
    for (const unsigned char *p = (const unsigned char *)e; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("/._-+@~", *p))) return 0;
    return 1;
}

static const char *const perm_names[SG_PERMS] = { "screen-share", "remote-input" };
static const char *const state_names[] = { "ask", "allow", "deny" };
int sg_perm_of(const char *s) { for (int i = 0; s && i < SG_PERMS; i++) if (!strcmp(s, perm_names[i])) return i; return -1; }
int sg_state_of(const char *s) { for (int i = 0; s && i < 3; i++) if (!strcmp(s, state_names[i])) return i; return -1; }
const char *sg_perm_name(int p) { return p >= 0 && p < SG_PERMS ? perm_names[p] : "?"; }
const char *sg_state_name(int s) { return s >= 0 && s < 3 ? state_names[s] : "?"; }

int sg_parse(const char *text, size_t len, SgStore *s) {
    s->n = 0;
    JDoc d;
    if (json_parse(text, len, &d, NULL, 0)) return -1;
    const JVal *g = json_get(d.root, "grants");
    int r = -1;
    if (json_num(d.root, "v", 0) == 1 && g && g->type == J_ARR && g->len <= SG_MAX) {
        r = 0;
        for (const JVal *k = g->kid; k; k = k->next) {
            const char *id = json_str(k, "id"), *exe = json_str(k, "exe");
            int perm = sg_perm_of(json_str(k, "perm")), st = sg_state_of(json_str(k, "state"));
            if (!sg_id_valid(id) || perm < 0 || (st != SG_ALLOW && st != SG_DENY) || (exe && exe[0] && !sg_exe_valid(exe))) { r = -1; break; }
            SgEntry *e = &s->e[s->n++];
            snprintf(e->id, sizeof e->id, "%s", id);
            snprintf(e->exe, sizeof e->exe, "%s", exe ? exe : "");
            e->perm = perm;
            e->state = st;
        }
    }
    if (r) s->n = 0;
    json_free(&d);
    return r;
}

int sg_write(const SgStore *s, char *out, size_t n) {
    size_t o = (size_t)snprintf(out, n, "{\"v\":1,\"grants\":[");
    for (int i = 0; i < s->n && o < n; i++) {
        const SgEntry *e = &s->e[i];
        char id[SG_ID_MAX * 6 + 3], exe[SG_EXE_MAX * 6 + 3];     /* room for every byte escaped */
        json_quote(id, sizeof id, e->id);
        json_quote(exe, sizeof exe, e->exe);
        o += (size_t)snprintf(out + o, n - o, "%s\n{\"id\":%s,\"exe\":%s,\"perm\":\"%s\",\"state\":\"%s\"}", i ? "," : "", id, exe,
                              sg_perm_name(e->perm), sg_state_name(e->state));
    }
    if (o < n) o += (size_t)snprintf(out + o, n - o, "]}\n");
    return o < n ? (int)o : -1;
}

static const SgEntry *find(const SgStore *s, const char *id, int perm) {
    for (int i = 0; i < s->n; i++) if (s->e[i].perm == perm && !strcmp(s->e[i].id, id)) return &s->e[i];
    return NULL;
}

SgState sg_get(const SgStore *s, const char *id, const char *exe, int perm) {
    const SgEntry *e = find(s, id, perm);
    if (!e) return SG_ASK;
    if (e->state == SG_DENY) return SG_DENY;           /* a denial doesn't depend on the path */
    if (!strncmp(id, "desktop:", 8) && (!e->exe[0] || !exe || strcmp(e->exe, exe))) return SG_ASK;
    return (SgState)e->state;
}

int sg_set(SgStore *s, const char *id, const char *exe, int perm, int state) {
    const SgEntry *f = find(s, id, perm);
    if (state == SG_ASK) {
        if (f) { size_t i = (size_t)(f - s->e); memmove(&s->e[i], &s->e[i + 1], (size_t)(s->n - (int)i - 1) * sizeof s->e[0]); s->n--; }
        return 0;
    }
    SgEntry *e = f ? &s->e[f - s->e] : s->n < SG_MAX ? &s->e[s->n++] : NULL;
    if (!e) return -1;
    snprintf(e->id, sizeof e->id, "%s", id);
    snprintf(e->exe, sizeof e->exe, "%s", exe ? exe : "");
    e->perm = perm;
    e->state = state;
    return 0;
}

void sg_forget(SgStore *s, const char *id) {
    int k = 0;
    for (int i = 0; i < s->n; i++) if (strcmp(s->e[i].id, id)) s->e[k++] = s->e[i];
    s->n = k;
}

void sg_path(uid_t uid, char *out, size_t n) { snprintf(out, n, SG_DIR "/%u.json", (unsigned)uid); }

int sg_load(const char *path, SgStore *s) {
    s->n = 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    char *b = NULL;
    int r = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022) && st.st_size > 0 && st.st_size <= SG_FILE_MAX &&
        (b = malloc((size_t)st.st_size))) {
        ssize_t got = read(fd, b, (size_t)st.st_size);
        if (got == st.st_size) r = sg_parse(b, (size_t)got, s);
    }
    free(b);
    close(fd);
    return r;
}
