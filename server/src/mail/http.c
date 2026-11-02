/* http.c — see http.h. */
#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <limits.h>

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int http_split_url(const char *url, int *tls, char *host, size_t hcap, int *port, char *path, size_t pcap) {
    if (!url) return -1;
    const char *p;
    if (!strncasecmp(url, "https://", 8)) { *tls = 1; p = url + 8; *port = 443; }
    else if (!strncasecmp(url, "http://", 7)) { *tls = 0; p = url + 7; *port = 80; }
    else return -1;
    const char *e = p + strcspn(p, "/?#");
    const char *at = memchr(p, '@', (size_t)(e - p));
    if (at) return -1;                          /* no credentials in URLs */
    const char *colon = memchr(p, ':', (size_t)(e - p));
    size_t hl = (size_t)((colon ? colon : e) - p);
    if (!hl || hl >= hcap) return -1;
    for (size_t i = 0; i < hl; i++) {
        unsigned char c = (unsigned char)p[i];
        if (!isalnum(c) && c != '-' && c != '.') return -1;
        host[i] = (char)tolower(c);
    }
    host[hl] = 0;
    if (colon) { *port = atoi(colon + 1); if (*port <= 0 || *port > 65535) return -1; }
    const char *path0 = *e == '/' ? e : "/";
    size_t pl = strcspn(path0, "#");
    if (*e == '?') { snprintf(path, pcap, "/%.*s", (int)strcspn(e, "#"), e); return 0; }
    if (pl >= pcap) return -1;
    memcpy(path, path0, pl);
    path[pl] = 0;
    for (char *c = path; *c; c++) if ((unsigned char)*c <= 0x20 || *c == 0x7f) return -1;
    return 0;
}

void http_resp_free(HttpResp *r) { free(r->body); memset(r, 0, sizeof *r); }

/* Chunk size line: hex digits, optional ";extensions", CRLF (or LF). -1 if anything else. */
int http_chunk_size(const char *line, unsigned long long *out) {
    const char *p = line;
    if (!isxdigit((unsigned char)*p)) return -1;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(p, &end, 16);
    if (errno == ERANGE || end == p) return -1;
    if (end - p > 16) return -1;
    if (*end == ';') { end++; while (*end && *end != '\r' && *end != '\n') { if ((unsigned char)*end < 0x20 && *end != '\t') return -1; end++; } }
    while (*end == ' ' || *end == '\t') end++;
    if (!(end[0] == '\r' && end[1] == '\n' && !end[2]) && !(end[0] == '\n' && !end[1])) return -1;
    *out = v;
    return 0;
}

/* Content-Length: decimal digits only (no sign, no garbage), no overflow. -1 if invalid. */
int http_content_length(const char *v, unsigned long long *out) {
    const char *p = v;
    while (*p == ' ' || *p == '\t') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    unsigned long long x = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (x > (ULLONG_MAX - d) / 10) return -1;
        x = x * 10 + d;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p) return -1;
    *out = x;
    return 0;
}

static void hdr_copy(const char *line, const char *name, char *out, size_t cap) {
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) || line[n] != ':') return;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    size_t l = strcspn(v, "\r\n");
    if (l >= cap) l = cap - 1;
    memcpy(out, v, l);
    out[l] = 0;
}

int http_request(Conn *c, const char *method, const char *host, const char *path, const char *extra, const char *body, size_t blen,
                 size_t max_body, int deadline_ms, HttpResp *out) {
    memset(out, 0, sizeof *out);
    double end = now_ms() + (deadline_ms > 0 ? deadline_ms : 60000);
    if (conn_printf(c, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Axia-Mail/1.0\r\nAccept-Encoding: identity\r\n%sContent-Length: %zu\r\n\r\n",
                    method, path, host, extra ? extra : "", body ? blen : 0)) return -1;
    if (body && blen && conn_write(c, body, blen)) return -1;
    size_t ll;
    char *line = conn_line(c, &ll, 8192);
    if (!line) return -1;
    int maj = 0, mn = 0;
    if (sscanf(line, "HTTP/%d.%d %d", &maj, &mn, &out->status) != 3) { free(line); return -1; }
    free(line);
    long long clen = -1;
    int chunked = 0, close_after = maj == 1 && mn == 0, done = 0;
    size_t hdr_total = 0;
    for (int n = 0; n < 200; n++) {
        if (now_ms() > end) return -1;
        line = conn_line(c, &ll, 16384);
        if (!line) return -1;
        hdr_total += ll;
        if (hdr_total > 64 * 1024) { free(line); return -1; }          /* headers: 64 KB at most */
        if (!strcmp(line, "\r\n") || !strcmp(line, "\n")) { free(line); done = 1; break; }
        char v[64] = "";
        hdr_copy(line, "Content-Length", v, sizeof v);
        if (v[0]) {
            unsigned long long cl;
            if (http_content_length(v, &cl) || cl > (unsigned long long)LLONG_MAX) { free(line); return -1; }
            clen = (long long)cl;
        }
        v[0] = 0;
        hdr_copy(line, "Transfer-Encoding", v, sizeof v);
        if (strcasestr(v, "chunked")) chunked = 1;
        v[0] = 0;
        hdr_copy(line, "Connection", v, sizeof v);
        if (strcasestr(v, "close")) close_after = 1;
        hdr_copy(line, "Location", out->location, sizeof out->location);
        hdr_copy(line, "ETag", out->etag, sizeof out->etag);
        hdr_copy(line, "Content-Type", out->content_type, sizeof out->content_type);
        free(line);
    }
    if (!done) return -1;                                       /* more than 200 header lines */
    size_t cap = 0;
    out->blen = 0;
    if (!strcasecmp(method, "HEAD") || out->status == 204 || out->status == 304 || (out->status >= 100 && out->status < 200)) clen = 0;
    if (chunked) {
        for (;;) {
            if (now_ms() > end) { http_resp_free(out); return -1; }
            line = conn_line(c, &ll, 1024);
            if (!line) { http_resp_free(out); return -1; }
            unsigned long long sz;
            int bad = http_chunk_size(line, &sz);
            free(line);
            if (bad) { http_resp_free(out); return -1; }
            if (sz == 0) {
                /* trailers until the empty line */
                while ((line = conn_line(c, &ll, 8192))) { int done = !strcmp(line, "\r\n") || !strcmp(line, "\n"); free(line); if (done) break; }
                break;
            }
            if (sz > max_body || out->blen > max_body - sz) { http_resp_free(out); return -2; }
            if (conn_read_n(c, (size_t)sz, &out->body, &out->blen, &cap)) { http_resp_free(out); return -1; }
            line = conn_line(c, &ll, 16);
            free(line);
        }
    } else if (clen >= 0) {
        if ((unsigned long long)clen > (unsigned long long)max_body) { http_resp_free(out); return -2; }
        if (clen && conn_read_n(c, (size_t)clen, &out->body, &out->blen, &cap)) { http_resp_free(out); return -1; }
    } else {
        /* until the server closes */
        char tmp[16384];
        for (;;) {
            if (now_ms() > end) { http_resp_free(out); return -1; }
            ssize_t r = conn_read_some(c, tmp, sizeof tmp);
            if (r <= 0) break;
            size_t want = (size_t)r;
            if (want > max_body || out->blen > max_body - want) { http_resp_free(out); return -2; }
            if (out->blen + want + 1 > cap) {
                size_t nc = out->blen + want + 1;
                nc = nc <= SIZE_MAX / 2 ? nc * 2 : nc;
                char *nb = realloc(out->body, nc);
                if (!nb) { http_resp_free(out); return -1; }
                out->body = nb;
                cap = nc;
            }
            memcpy(out->body + out->blen, tmp, want);
            out->blen += want;
        }
        close_after = 1;
    }
    if (!out->body) { out->body = calloc(1, 1); }
    else out->body[out->blen] = 0;
    out->keep_alive = !close_after;
    return 0;
}
