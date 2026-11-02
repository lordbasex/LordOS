/* print.c — see print.h. */
#include "print.h"
#include "cups.h"
#include "escl.h"
#include "mdns.h"
#include "phttp.h"
#include "../doc/wire.h"
#include "../sec/json.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_RUNNING 8

typedef struct {
    int id;
    char kind[16];
    char *json;
    size_t len;
    int docfd;
    size_t doclen;
} Task;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static JBuf events;
static int next_id = 1, running;

static void finish(Task *t, int ok, JBuf *res) {
    pthread_mutex_lock(&mu);
    if (events.n) jb_raw(&events, ",");
    jb_printf(&events, "{\"id\":%d,\"kind\":\"%s\",\"ok\":%s,\"data\":", t->id, t->kind, ok ? "true" : "false");
    jb_raw(&events, res->p && !res->oom ? res->p : "{}");
    jb_raw(&events, "}");
    running--;
    pthread_mutex_unlock(&mu);
}

/* ---- discovery ---- */

static void discover(JBuf *out) {
    static const char *const ptypes[] = { "_ipp._tcp", "_ipps._tcp" };
    static const char *const stypes[] = { "_uscan._tcp", "_uscans._tcp" };
    const char *all[4] = { ptypes[0], ptypes[1], stypes[0], stypes[1] };
    MdnsSvc *svc = calloc(48, sizeof *svc);
    int n = svc ? mdns_browse(all, 4, 1800, svc, 48) : 0;
    jb_raw(out, "{\"printers\":[");
    int k = 0;
    char v[512];
    for (int i = 0; i < n; i++) {
        MdnsSvc *s = &svc[i];
        if (strncmp(s->type, "_ipp", 4) || !s->addr[0]) continue;
        int tls = !strcmp(s->type, "_ipps._tcp");
        /* the same printer usually announces itself over ipp and ipps: shown once (ipp) */
        int dup = 0;
        for (int j = 0; tls && j < n; j++) if (j != i && !strcmp(svc[j].type, "_ipp._tcp") && !strcmp(svc[j].name, s->name)) dup = 1;
        if (dup) continue;
        if (k++) jb_raw(out, ",");
        jb_raw(out, "{\"name\":");
        jb_str(out, s->name);
        jb_raw(out, ",\"host\":");
        jb_str(out, s->host);
        jb_raw(out, ",\"addr\":");
        jb_str(out, s->addr);
        jb_printf(out, ",\"port\":%d,\"tls\":%s", s->port, tls ? "true" : "false");
        char rp[256];
        mdns_txt(s, "rp", rp, sizeof rp);
        char uri[512];
        snprintf(uri, sizeof uri, "%s://%s:%d/%s", tls ? "ipps" : "ipp", s->addr, s->port, rp[0] ? rp : "ipp/print");
        jb_raw(out, ",\"uri\":");
        jb_str(out, uri);
        static const char *const keys[][2] = { { "model", "ty" }, { "note", "note" }, { "uuid", "UUID" }, { "pdl", "pdl" }, { "color", "Color" }, { "duplex", "Duplex" }, { "kind", "kind" } };
        for (size_t q = 0; q < sizeof keys / sizeof keys[0]; q++) {
            jb_printf(out, ",\"%s\":", keys[q][0]);
            jb_str(out, mdns_txt(s, keys[q][1], v, sizeof v));
        }
        jb_raw(out, "}");
    }
    jb_raw(out, "],\"scanners\":[");
    k = 0;
    for (int i = 0; i < n; i++) {
        MdnsSvc *s = &svc[i];
        if (strncmp(s->type, "_uscan", 6) || !s->addr[0]) continue;
        if (k++) jb_raw(out, ",");
        jb_raw(out, "{\"name\":");
        jb_str(out, s->name);
        jb_raw(out, ",\"addr\":");
        jb_str(out, s->addr);
        jb_printf(out, ",\"port\":%d,\"tls\":%s,\"rs\":", s->port, !strcmp(s->type, "_uscans._tcp") ? "true" : "false");
        jb_str(out, mdns_txt(s, "rs", v, sizeof v)[0] ? v : "eSCL");
        jb_raw(out, ",\"model\":");
        jb_str(out, mdns_txt(s, "ty", v, sizeof v));
        jb_raw(out, ",\"uuid\":");
        jb_str(out, mdns_txt(s, "UUID", v, sizeof v));
        jb_raw(out, ",\"lan\":");
        jb_raw(out, phttp_lan_ipv4(s->addr) ? "true" : "false");
        jb_raw(out, "}");
    }
    jb_raw(out, "]}");
    free(svc);
}

/* ~/.cache/axia/scan: where scans stay until they're saved */
static int scan_dir(char *out, size_t n) {
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') return -1;
    const char *parts[] = { ".cache", ".cache/axia", ".cache/axia/scan" };
    for (int i = 0; i < 3; i++) {
        snprintf(out, n, "%s/%s", home, parts[i]);
        if (mkdir(out, 0700) && errno != EEXIST) return -1;
    }
    return 0;
}

static void *run(void *arg) {
    Task *t = arg;
    JDoc d = { 0 };
    JBuf out;
    jb_init(&out);
    int ok = 0;
    if (t->json && json_parse(t->json, t->len, &d, NULL, 0)) { jb_raw(&out, "{\"error\":\"bad-request\"}"); goto done; }
    const JVal *a = d.root;
    const char *k = t->kind;
    if (!strcmp(k, "printers")) ok = !cups_printers(&out);
    else if (!strcmp(k, "jobs")) ok = !cups_jobs(&out);
    else if (!strcmp(k, "cancel")) ok = !cups_cancel((int)json_num(a, "job", 0), &out);
    else if (!strcmp(k, "submit")) ok = !cups_submit(json_str(a, "printer"), t->docfd, t->doclen, json_get(a, "options"), &out);
    else if (!strcmp(k, "add")) ok = !cups_add(json_str(a, "name"), json_str(a, "uri"), json_str(a, "info"), json_str(a, "location"), &out);
    else if (!strcmp(k, "remove")) ok = !cups_remove(json_str(a, "name"), &out);
    else if (!strcmp(k, "default")) ok = !cups_set_default(json_str(a, "name"), &out);
    else if (!strcmp(k, "discover")) { discover(&out); ok = 1; }
    else if (!strcmp(k, "scancaps")) {
        const char *rs = json_str(a, "rs");
        ok = !escl_caps(json_str(a, "addr") ? json_str(a, "addr") : "", (int)json_num(a, "port", 80), rs ? rs : "eSCL", &out);
    } else if (!strcmp(k, "scan")) {
        char dir[PATH_MAX];
        const char *rs = json_str(a, "rs");
        if (scan_dir(dir, sizeof dir)) jb_raw(&out, "{\"error\":\"write\"}");
        else ok = !escl_scan(json_str(a, "addr") ? json_str(a, "addr") : "", (int)json_num(a, "port", 80), rs ? rs : "eSCL", json_get(a, "settings"), dir, &out);
    } else jb_raw(&out, "{\"error\":\"unknown\"}");
done:
    finish(t, ok, &out);
    jb_free(&out);
    json_free(&d);
    if (t->docfd >= 0) close(t->docfd);
    free(t->json);
    free(t);
    return NULL;
}

int print_task(const char *kind, const char *json, size_t len, int docfd, size_t doclen) {
    Task *t = calloc(1, sizeof *t);
    if (!t || strlen(kind) >= sizeof t->kind) { free(t); if (docfd >= 0) close(docfd); return -1; }
    snprintf(t->kind, sizeof t->kind, "%s", kind);
    t->docfd = docfd;
    t->doclen = doclen;
    if (json && len) {
        t->json = malloc(len + 1);
        if (!t->json) { free(t); if (docfd >= 0) close(docfd); return -1; }
        memcpy(t->json, json, len);
        t->json[len] = 0;
        t->len = len;
    }
    pthread_mutex_lock(&mu);
    if (running >= MAX_RUNNING) { pthread_mutex_unlock(&mu); free(t->json); free(t); if (docfd >= 0) close(docfd); return -1; }
    t->id = next_id++;
    running++;
    int id = t->id;
    pthread_mutex_unlock(&mu);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 512 * 1024);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int r = pthread_create(&th, &at, run, t);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&at);
    if (r) {
        pthread_mutex_lock(&mu);
        running--;
        pthread_mutex_unlock(&mu);
        if (docfd >= 0) close(docfd);
        free(t->json);
        free(t);
        return -1;
    }
    return id;
}

char *print_poll(void) {
    pthread_mutex_lock(&mu);
    char *out = NULL;
    if (events.n) {
        out = malloc(events.n + 3);
        if (out) { out[0] = '['; memcpy(out + 1, events.p, events.n); out[events.n + 1] = ']'; out[events.n + 2] = 0; }
        jb_free(&events);
    }
    pthread_mutex_unlock(&mu);
    return out;
}

int print_pending(void) {
    pthread_mutex_lock(&mu);
    int r = running;
    pthread_mutex_unlock(&mu);
    return r;
}
