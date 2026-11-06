/* dav.c — WebDAV transport and the per-account CalDAV/CardDAV worker (see dav.h, pim.c). */
#include "dav.h"
#include "mime.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define DAV_MAX_BODY ((size_t)48 << 20)

static int dav_connect(Dav *d, const char *host, int port, int tls) {
    if (d->c && !strcmp(d->host, host) && d->port == port && d->tls == tls) return 0;
    if (d->c) { conn_close(d->c); d->c = NULL; }
    if (!tls) { snprintf(d->err, sizeof d->err, "insecure"); return -1; }
    TlsInfo ti;
    char why[64];
    d->c = open_conn(d->a->id, 0, host, port, 1, d->a->pin_dav, &ti, why, sizeof why);
    if (!d->c) { snprintf(d->err, sizeof d->err, "%s", why); return -1; }
    snprintf(d->host, sizeof d->host, "%s", host);
    d->port = port;
    d->tls = tls;
    return 0;
}

int dav_begin(Dav *d, Account *a) {
    memset(d, 0, sizeof *d);
    d->a = a;
    pthread_mutex_lock(&G);
    char *pw = a->password ? strdup(a->password) : NULL;
    pthread_mutex_unlock(&G);
    if (!pw) { snprintf(d->err, sizeof d->err, "no-password"); return -2; }
    char up[600];
    snprintf(up, sizeof up, "%s:%s", a->user, pw);
    explicit_bzero(pw, strlen(pw));
    free(pw);
    size_t el;
    char *e = b64_encode((const unsigned char *)up, strlen(up), 0, &el);
    explicit_bzero(up, sizeof up);
    if (!e) return -1;
    snprintf(d->auth, sizeof d->auth, "Authorization: Basic %s\r\n", e);
    explicit_bzero(e, el);
    free(e);
    int tls, port;
    char host[256], path[1024];
    if (http_split_url(a->url, &tls, host, sizeof host, &port, path, sizeof path)) { snprintf(d->err, sizeof d->err, "bad URL"); return -1; }
    return dav_connect(d, host, port, tls);
}

void dav_end(Dav *d) {
    if (d->c) conn_close(d->c);
    d->c = NULL;
    explicit_bzero(d->auth, sizeof d->auth);
}

void dav_abs(const Dav *d, const char *path, char *out, size_t cap) {
    if (!strncasecmp(path, "https://", 8) || !strncasecmp(path, "http://", 7)) { snprintf(out, cap, "%s", path); return; }
    if (d->port == 443) snprintf(out, cap, "https://%s%s", d->host, path);
    else snprintf(out, cap, "https://%s:%d%s", d->host, d->port, path);
}

int dav_req(Dav *d, const char *method, const char *target, int depth, const char *body, const char *ctype, const char *extra, HttpResp *out) {
    char cur[2048];
    dav_abs(d, target, cur, sizeof cur);
    for (int hop = 0; hop < 4; hop++) {
        int tls, port;
        char host[256], path[1800];
        if (http_split_url(cur, &tls, host, sizeof host, &port, path, sizeof path)) { snprintf(d->err, sizeof d->err, "bad URL"); return -1; }
        for (int attempt = 0; attempt < 2; attempt++) {
            if (dav_connect(d, host, port, tls)) return -1;
            char hdrs[2048];
            snprintf(hdrs, sizeof hdrs, "%s%s%s%s%s%s", d->auth, depth >= 0 ? (depth == 0 ? "Depth: 0\r\n" : depth == 1 ? "Depth: 1\r\n" : "Depth: infinity\r\n") : "",
                     ctype ? "Content-Type: " : "", ctype ? ctype : "", ctype ? "\r\n" : "", extra ? extra : "");
            int r = http_request(d->c, method, host, path, hdrs, body, body ? strlen(body) : 0, DAV_MAX_BODY, 60000, out);
            if (r == -1 && attempt == 0) { conn_close(d->c); d->c = NULL; continue; }      /* the server closed a kept-alive connection */
            if (r) { snprintf(d->err, sizeof d->err, r == -2 ? "response too big" : "connection"); return -1; }
            if (!out->keep_alive) { conn_close(d->c); d->c = NULL; }
            break;
        }
        if (out->status == 401) d->auth_failed = 1;
        if ((out->status == 301 || out->status == 302 || out->status == 307 || out->status == 308) && out->location[0]) {
            char loc[1100];
            snprintf(loc, sizeof loc, "%s", out->location);
            http_resp_free(out);
            if (loc[0] == '/') { char base[1200]; Dav tmp = *d; snprintf(tmp.host, sizeof tmp.host, "%s", host); tmp.port = port; dav_abs(&tmp, loc, base, sizeof base); snprintf(cur, sizeof cur, "%s", base); }
            else snprintf(cur, sizeof cur, "%s", loc);
            continue;
        }
        snprintf(d->url, sizeof d->url, "%s", cur);
        return 0;
    }
    snprintf(d->err, sizeof d->err, "too many redirects");
    return -1;
}

/* ---- the worker ---- */

void *dav_worker(void *arg) {
    Account *a = arg;
    Job *first = job_new(J_DAV_SYNC, 0, 1);
    if (first) job_push(a, first);
    time_t last = 0;
    while (!a->stop) {
        Job *j = job_pop(a);
        if (!j) {
            struct pollfd p = { a->wake, POLLIN, 0 };
            int left = (int)(15 * 60 - (time(NULL) - last));
            if (left < 1) left = 1;
            poll(&p, 1, left * 1000);
            uint64_t v;
            if (read(a->wake, &v, sizeof v) < 0) { /* timeout */ }
            if (a->stop) break;
            if (!a->jobs && time(NULL) - last >= 15 * 60) { Job *s = job_new(J_DAV_SYNC, 0, 1); if (s) job_push(a, s); }
            continue;
        }
        /* coalesce: many queued syncs are one sync */
        int kind = j->kind;
        uint32_t req = j->req;
        job_free(j);
        while ((j = job_pop(a))) { if (j->req) { if (req) { JBuf b; jb_init(&b); jb_raw(&b, "{\"ok\":true}"); reply_done(j->req, &b); } else req = j->req; } job_free(j); }
        (void)kind;
        acct_status(a, "sync", "");
        pim_dav_sync(a);
        last = time(NULL);
        if (req) { JBuf b; jb_init(&b); jb_raw(&b, "{\"ok\":true}"); reply_done(req, &b); }
    }
    return NULL;
}
