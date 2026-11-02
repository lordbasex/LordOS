/* fwsub.c — the user's blocklist subscriptions, lord-server's side (see fwsub.h). */
#include "fwsub.h"
#include "fwlists.h"
#include "../doc/wire.h"
#include "../fetch/fetchproto.h"
#include "../sec/json.h"
#include "../sec/netconn.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <malloc.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char id[32], url[FWS_URL_MAX + 1], format[8];
    int sub, custom, on;
    long long updated, tried;           /* Unix seconds of the last good download / the last attempt */
    long domains, nets;
    int truncated;
    char error[64];                     /* the last attempt's error code ("" if it went well) */
    char topic[16], source[96], license[96], homepage[160];   /* the catalog's, for Settings */
    long approx;
} List;

static List L[FWS_MAX_LISTS];
static int nl;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int busy, want_fetch, want_compile, job_done;
static FwLists *cur, *next_set;
static char cur_ids[FWS_MAX_LISTS][32], next_ids[FWS_MAX_LISTS][32];
static char cfg_path[PATH_MAX], txt_path[PATH_MAX], cache_dir[PATH_MAX - 64], fetch_bin[PATH_MAX], catalog_path[PATH_MAX];
static int inited;

static long long now_s(void) { return (long long)time(NULL); }

/* ---- files ---- */

static char *read_file(const char *p, size_t max, size_t *len) {
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > (off_t)max) { close(fd); return NULL; }
    char *b = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (b && got < (size_t)st.st_size) {
        ssize_t n = read(fd, b + got, (size_t)st.st_size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (!b || got != (size_t)st.st_size) { free(b); return NULL; }
    b[got] = 0;
    *len = got;
    return b;
}

static int write_file(const char *p, const char *data, size_t len) {
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) { close(fd); unlink(tmp); return -1; }
        off += (size_t)n;
    }
    if (fsync(fd) || close(fd)) { unlink(tmp); return -1; }
    return rename(tmp, p);
}

static int good_id(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > 31) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) return 0;
    return 1;
}

int fws_split_url(const char *url, char *host, size_t hcap, int *port, char *path, size_t pcap) {
    if (!url || strncmp(url, "https://", 8) || strlen(url) > FWS_URL_MAX) return -1;
    const char *h = url + 8, *e = h;
    while (*e && *e != '/' && *e != ':' && *e != '?' && *e != '#') e++;
    size_t hl = (size_t)(e - h);
    if (!hl || hl >= hcap || hl > 253) return -1;
    for (size_t i = 0; i < hl; i++) {
        char c = (char)tolower((unsigned char)h[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return -1;
        host[i] = c;
    }
    host[hl] = 0;
    if (host[0] == '.' || host[0] == '-' || host[hl - 1] == '.' || strstr(host, "..") || !strchr(host, '.')) return -1;
    *port = 443;
    if (*e == ':') {
        char *end;
        long p = strtol(e + 1, &end, 10);
        if (end == e + 1 || p < 1 || p > 65535 || (*end && *end != '/')) return -1;
        *port = (int)p;
        e = end;
    }
    const char *pa = *e == '/' ? e : "/";
    if (*e && *e != '/') return -1;                     /* ?query or #fragment right after the host */
    size_t pl = strlen(pa);
    if (pl >= pcap) return -1;
    for (size_t i = 0; i < pl; i++) if ((unsigned char)pa[i] <= 0x20 || (unsigned char)pa[i] >= 0x7f || pa[i] == '#') return -1;
    memcpy(path, pa, pl + 1);
    return 0;
}

/* ---- the catalog, the config and the cache ---- */

static List *find(const char *id) {
    for (int i = 0; i < nl; i++) if (!strcmp(L[i].id, id)) return &L[i];
    return NULL;
}

static int add_list(const char *id, const char *url, const char *fmt, int sub, int custom) {
    char h[256], p[FWS_URL_MAX + 1];
    int port;
    if (nl >= FWS_MAX_LISTS || !good_id(id) || find(id) || fws_split_url(url, h, sizeof h, &port, p, sizeof p) || !fwl_format_from_name(fmt)) return -1;
    List *l = &L[nl++];
    memset(l, 0, sizeof *l);
    snprintf(l->id, sizeof l->id, "%s", id);
    snprintf(l->url, sizeof l->url, "%s", url);
    snprintf(l->format, sizeof l->format, "%s", fmt);
    l->sub = sub;
    l->custom = custom;
    return 0;
}

static void load_catalog(void) {
    size_t len;
    char *t = read_file(catalog_path, 1 << 20, &len);
    JDoc d;
    if (!t || json_parse(t, len, &d, NULL, 0)) { free(t); fprintf(stderr, "lord.fw: no blocklist catalog (%s)\n", catalog_path); return; }
    const JVal *lists = json_get(d.root, "lists");
    size_t i = 0;
    for (const JVal *o = lists && lists->type == J_ARR ? lists->kid : NULL; o; o = o->next, i++) {
        if (add_list(json_str(o, "id") ? json_str(o, "id") : "", json_str(o, "url") ? json_str(o, "url") : "",
                     json_str(o, "format") ? json_str(o, "format") : "", json_num(o, "sub", 0) == 1, 0)) {
            fprintf(stderr, "lord.fw: blocklist catalog entry %zu is invalid\n", i);
            continue;
        }
        List *l = &L[nl - 1];
        const char *tp = json_str(o, "topic"), *src = json_str(o, "source"), *lic = json_str(o, "license"), *hp = json_str(o, "homepage");
        snprintf(l->topic, sizeof l->topic, "%s", tp && (!strcmp(tp, "ads") || !strcmp(tp, "trackers") || !strcmp(tp, "malware")) ? tp : "other");
        snprintf(l->source, sizeof l->source, "%s", src ? src : "");
        snprintf(l->license, sizeof l->license, "%s", lic ? lic : "");
        snprintf(l->homepage, sizeof l->homepage, "%s", hp && !strncmp(hp, "https://", 8) ? hp : "");
        l->approx = (long)json_num(o, "approx", 0);
    }
    json_free(&d);
    free(t);
}

static void load_config(void) {
    size_t len;
    char *t = read_file(cfg_path, 1 << 20, &len);
    JDoc d;
    if (!t || json_parse(t, len, &d, NULL, 0)) { free(t); return; }
    const JVal *cu = json_get(d.root, "custom");
    size_t i = 0;
    for (const JVal *o = cu && cu->type == J_ARR ? cu->kid : NULL; o && i < FWS_MAX_CUSTOM; o = o->next, i++) {
        const char *id = json_str(o, "id"), *url = json_str(o, "url"), *fmt = json_str(o, "format");
        if (id && url && fmt && !strncmp(id, "custom-", 7)) add_list(id, url, fmt, 0, 1);
    }
    const JVal *en = json_get(d.root, "enabled");
    for (const JVal *v = en && en->type == J_ARR ? en->kid : NULL; v; v = v->next) {
        if (v->type != J_STR) continue;
        List *l = find(v->str);
        if (l) l->on = 1;
    }
    json_free(&d);
    free(t);
}

static void save_config(void) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"v\":1,\"enabled\":[");
    int k = 0;
    for (int i = 0; i < nl; i++) if (L[i].on) { if (k++) jb_raw(&b, ","); jb_str(&b, L[i].id); }
    jb_raw(&b, "],\"custom\":[");
    k = 0;
    for (int i = 0; i < nl; i++) {
        if (!L[i].custom) continue;
        if (k++) jb_raw(&b, ",");
        jb_raw(&b, "{\"id\":");
        jb_str(&b, L[i].id);
        jb_raw(&b, ",\"url\":");
        jb_str(&b, L[i].url);
        jb_raw(&b, ",\"format\":");
        jb_str(&b, L[i].format);
        jb_raw(&b, "}");
    }
    jb_raw(&b, "]}\n");
    if (!b.oom && write_file(cfg_path, b.p, b.n)) fprintf(stderr, "lord.fw: could not save %s\n", cfg_path);
    jb_free(&b);
}

static void cache_path(const char *id, const char *ext, char *out, size_t n) {
    if ((size_t)snprintf(out, n, "%s/%.31s.%.4s", cache_dir, id, ext) >= n) out[0] = 0;
}

static void load_state(List *l) {
    char p[PATH_MAX];
    cache_path(l->id, "json", p, sizeof p);
    size_t len;
    char *t = read_file(p, 4096, &len);
    JDoc d;
    if (!t || json_parse(t, len, &d, NULL, 0)) { free(t); return; }
    l->updated = (long long)json_num(d.root, "updated", 0);
    l->tried = (long long)json_num(d.root, "tried", 0);
    l->domains = (long)json_num(d.root, "domains", 0);
    l->nets = (long)json_num(d.root, "nets", 0);
    l->truncated = json_num(d.root, "truncated", 0) == 1;
    const char *e = json_str(d.root, "error");
    snprintf(l->error, sizeof l->error, "%s", e ? e : "");
    json_free(&d);
    free(t);
}

static void save_state(const List *l) {
    char p[PATH_MAX], j[400], e[140];
    cache_path(l->id, "json", p, sizeof p);
    json_quote(e, sizeof e, l->error);
    int n = snprintf(j, sizeof j, "{\"updated\":%lld,\"tried\":%lld,\"domains\":%ld,\"nets\":%ld,\"truncated\":%d,\"error\":%s}\n",
                     l->updated, l->tried, l->domains, l->nets, l->truncated, e);
    write_file(p, j, (size_t)n);
}

static int has_copy(const List *l) {
    char p[PATH_MAX];
    cache_path(l->id, "txt", p, sizeof p);
    struct stat st;
    return !stat(p, &st) && S_ISREG(st.st_mode);
}

/* ---- the worker: downloading (lord-fetch) and compiling ---- */

typedef struct { int fd; pid_t pid; } Helper;

static int helper_start(Helper *h) {
    struct stat st, me;
    if (stat(fetch_bin, &st) || !S_ISREG(st.st_mode) || stat("/proc/self/exe", &me) ||
        (st.st_uid != 0 && st.st_uid != me.st_uid) || (st.st_mode & 022)) return -1;   /* root's or ours, not writable by others */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 3);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    char *argv[] = { "lord-fetch", NULL };
    char *envp[] = { "LANG=C.UTF-8", getenv("LORDOS_SESSIOND") ? "LORDOS_SESSIOND=1" : NULL, NULL };
    int e = posix_spawn(&h->pid, fetch_bin, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(sv[1]);
    if (e) { close(sv[0]); return -1; }
    h->fd = sv[0];
    return 0;
}

static void helper_stop(Helper *h) {
    if (h->fd < 0) return;
    doc_send(h->fd, FQ_END, 0, "{}", 2, NULL, 0);
    close(h->fd);
    h->fd = -1;
    for (int i = 0; i < 50; i++) {
        if (waitpid(h->pid, NULL, WNOHANG) == h->pid) return;
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(h->pid, SIGKILL);
    waitpid(h->pid, NULL, 0);
}

static int connect_to(const char *host, int port) {
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    if (getaddrinfo(host, ps, &hints, &res)) return -2;
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = netconn_socket(NC_LISTS, a->ai_family, SOCK_STREAM, a->ai_addr, a->ai_addrlen);
        if (fd < 0) continue;
        struct pollfd p = { fd, POLLOUT, 0 };
        int err = 0;
        socklen_t el = sizeof err;
        if (poll(&p, 1, 15000) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) || err) { close(fd); fd = -1; }
    }
    freeaddrinfo(res);
    if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

/* Downloads one list into its cache; l's state says how it went. */
static void fetch_one(Helper *h, List *l) {
    l->tried = now_s();
    char url[FWS_URL_MAX + 1];
    snprintf(url, sizeof url, "%s", l->url);
    for (int hop = 0; hop < 4; hop++) {
        char host[256], path[FWS_URL_MAX + 1];
        int port;
        if (fws_split_url(url, host, sizeof host, &port, path, sizeof path)) { snprintf(l->error, sizeof l->error, "badUrl"); return; }
        if (h->fd < 0 && helper_start(h)) { snprintf(l->error, sizeof l->error, "helper"); return; }
        int fd = connect_to(host, port);
        if (fd < 0) { snprintf(l->error, sizeof l->error, fd == -2 ? "dns" : "connect"); return; }
        JBuf b;
        jb_init(&b);
        jb_raw(&b, "{\"host\":");
        jb_str(&b, host);
        jb_raw(&b, ",\"path\":");
        jb_str(&b, path);
        jb_raw(&b, ",\"format\":");
        jb_str(&b, l->format);
        jb_printf(&b, ",\"sub\":%d}", l->sub);
        int sent = !b.oom && doc_send(h->fd, FQ_GET, 1, b.p, b.n, &fd, 1) == 0;
        jb_free(&b);
        close(fd);
        struct pollfd p = { h->fd, POLLIN, 0 };
        DocMsg m;
        if (!sent || poll(&p, 1, 240000) != 1 || doc_recv(h->fd, &m)) { snprintf(l->error, sizeof l->error, "helper"); helper_stop(h); return; }
        JDoc d;
        int have = m.json && !json_parse(m.json, m.len, &d, NULL, 0);
        if (m.h.op == FR_REDIRECT && have) {
            const char *loc = json_str(d.root, "location");
            if (!loc || strncmp(loc, "https://", 8) || strlen(loc) > FWS_URL_MAX) {
                snprintf(l->error, sizeof l->error, "redirect");
                json_free(&d);
                doc_msg_free(&m);
                return;
            }
            snprintf(url, sizeof url, "%s", loc);       /* only https, checked again at the top */
            json_free(&d);
            doc_msg_free(&m);
            continue;
        }
        if (m.h.op == FR_ERR && have) {
            const char *e = json_str(d.root, "error"), *det = json_str(d.root, "detail");
            if (e && !strcmp(e, "status") && det) snprintf(l->error, sizeof l->error, "status %.6s", det);
            else snprintf(l->error, sizeof l->error, "%.40s", e ? e : "helper");
            json_free(&d);
            doc_msg_free(&m);
            return;
        }
        if (m.h.op != FR_DONE || !have || m.nfds != 1) {
            snprintf(l->error, sizeof l->error, "helper");
            if (have) json_free(&d);
            doc_msg_free(&m);
            return;
        }
        /* the normalized text: a sealed memfd, read and validated line by line again */
        long sz = doc_check_sealed(m.fds[0], 0);
        char *txt = sz >= 0 && sz <= (long)FWL_FILE_MAX ? malloc((size_t)sz + 1) : NULL;
        if (txt && sz > 0) {
            void *mp = mmap(NULL, (size_t)sz, PROT_READ, MAP_PRIVATE, m.fds[0], 0);
            if (mp == MAP_FAILED) { free(txt); txt = NULL; }
            else { memcpy(txt, mp, (size_t)sz); munmap(mp, (size_t)sz); }
        }
        FwLists *chk = txt ? fwl_new() : NULL;
        long added = chk ? fwl_add_text(chk, txt, (size_t)sz, 1) : -1;
        char p2[PATH_MAX];
        cache_path(l->id, "txt", p2, sizeof p2);
        if (added <= 0 || write_file(p2, txt, (size_t)sz)) snprintf(l->error, sizeof l->error, added <= 0 ? "invalid" : "write");
        else {
            l->error[0] = 0;
            l->updated = now_s();
            l->domains = (long)fwl_domains(chk);
            l->nets = (long)fwl_nets(chk);
            l->truncated = json_num(d.root, "truncated", 0) == 1;
        }
        fwl_free(chk);
        free(txt);
        json_free(&d);
        doc_msg_free(&m);
        return;
    }
    snprintf(l->error, sizeof l->error, "redirect");
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mu);
        int fetch = want_fetch;
        want_fetch = want_compile = 0;
        List copy[FWS_MAX_LISTS];
        int n = nl;
        memcpy(copy, L, sizeof(List) * (size_t)n);
        pthread_mutex_unlock(&mu);

        Helper h = { -1, 0 };
        long long t = now_s();
        for (int i = 0; i < n && fetch; i++) {
            List *l = &copy[i];
            int stale = !has_copy(l) || t - l->updated > FWS_STALE_S;
            /* a list that failed is tried again after an hour, not in a loop */
            if (!l->on || (fetch == 1 && !stale) || (fetch == 1 && l->error[0] && t - l->tried < 3600 && has_copy(l))) continue;
            if (fetch == 3 && has_copy(l)) continue;        /* only what has no copy yet */
            fetch_one(&h, l);
            save_state(l);
            pthread_mutex_lock(&mu);
            List *o = find(l->id);
            if (o) {
                o->updated = l->updated; o->tried = l->tried; o->domains = l->domains; o->nets = l->nets;
                o->truncated = l->truncated;
                memcpy(o->error, l->error, sizeof o->error);
            }
            pthread_mutex_unlock(&mu);
        }
        helper_stop(&h);

        /* compile every enabled list that has a copy: their lines go to fwlists.txt as they are
         * (each checked again; lord-netperm drops repeated ones), and into the set lord-server keeps
         * for the Monitor (hashes only: a few bytes a name) */
        FwLists *s = fwl_new();
        char ids[FWS_MAX_LISTS][32];
        memset(ids, 0, sizeof ids);
        char tmp[PATH_MAX + 8];
        snprintf(tmp, sizeof tmp, "%s.tmp", txt_path);
        int ofd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
        int ok = ofd >= 0 && write(ofd, FWL_HEADER "\n", strlen(FWL_HEADER) + 1) == (ssize_t)strlen(FWL_HEADER) + 1;
        size_t total = 0;
        for (int i = 0; s && ok && i < n; i++) {
            if (!copy[i].on) continue;
            char p[PATH_MAX];
            cache_path(copy[i].id, "txt", p, sizeof p);
            size_t len;
            char *txt = read_file(p, FWL_FILE_MAX, &len);
            if (!txt) continue;
            int good = 1;
            for (size_t k = 0; k < len && good;) {
                const char *nl = memchr(txt + k, '\n', len - k);
                size_t e = nl ? (size_t)(nl - txt) : len;
                if (e > k && fwl_check_line(txt + k, e - k)) good = 0;
                k = e + 1;
            }
            if (!good || total + len > FWL_FILE_MAX - 64) {
                fprintf(stderr, "lord.fw: the copy of blocklist %s is %s: left out\n", copy[i].id, good ? "too large" : "invalid");
                free(txt);
                continue;
            }
            memcpy(ids[i], copy[i].id, sizeof ids[i]);
            fwl_add_text(s, txt, len, (uint16_t)(i + 1));
            for (size_t off = 0; ok && off < len;) {
                ssize_t w = write(ofd, txt + off, len - off);
                if (w <= 0) ok = 0;
                else off += (size_t)w;
            }
            if (ok && len && txt[len - 1] != '\n') ok = write(ofd, "\n", 1) == 1;
            total += len + 1;
            free(txt);
        }
        if (ofd >= 0 && (fsync(ofd) || close(ofd))) ok = 0;
        if (!ok || rename(tmp, txt_path)) { unlink(tmp); fprintf(stderr, "lord.fw: could not write %s\n", txt_path); }
        if (s) fwl_finish(s);
        malloc_trim(0);                 /* the copies were read on this thread: give their memory back */

        pthread_mutex_lock(&mu);
        fwl_free(next_set);
        next_set = s;
        memcpy(next_ids, ids, sizeof ids);
        job_done = 1;
        int again = want_fetch || want_compile;
        if (!again) busy = 0;
        pthread_mutex_unlock(&mu);
        if (!again) return NULL;
    }
}

/* Called with mu held. */
static int rank(int f) { return f == 2 ? 3 : f == 1 ? 2 : f == 3 ? 1 : 0; }   /* force > stale > missing > none */

static void kick(int fetch) {
    if (rank(fetch) > rank(want_fetch)) want_fetch = fetch;
    want_compile = 1;
    if (busy) return;
    busy = 1;
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &a, worker, NULL)) busy = 0;
    pthread_attr_destroy(&a);
}

/* ---- the API ---- */

int fws_init(const char *home, const char *assets, const char *bindir) {
    if (inited) return 0;
    if (!home || !assets || !bindir) return -1;
    snprintf(cfg_path, sizeof cfg_path, "%s/.config/lordos/fwlists.json", home);
    snprintf(txt_path, sizeof txt_path, "%s/.config/lordos/fwlists.txt", home);
    snprintf(catalog_path, sizeof catalog_path, "%s/fw/blocklists.json", assets);
    snprintf(fetch_bin, sizeof fetch_bin, "%s/lord-fetch", bindir);
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/.cache", home);
    mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/.cache/lordos", home);
    mkdir(p, 0700);
    snprintf(cache_dir, sizeof cache_dir, "%s/.cache/lordos/fwlists", home);
    mkdir(cache_dir, 0700);
    snprintf(p, sizeof p, "%s/.config", home);
    mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/.config/lordos", home);
    mkdir(p, 0700);
    inited = 1;
    load_catalog();
    load_config();
    int any = 0;
    for (int i = 0; i < nl; i++) { load_state(&L[i]); any |= L[i].on; }
    /* the set for the Monitor's "on a list" (the kernel's copy is lord-netperm's) */
    if (any) { pthread_mutex_lock(&mu); kick(0); pthread_mutex_unlock(&mu); }
    return 0;
}

int fws_poll(void) {
    if (!inited) return 0;
    pthread_mutex_lock(&mu);
    int r = job_done;
    if (r) {
        fwl_free(cur);
        cur = next_set;
        next_set = NULL;
        memcpy(cur_ids, next_ids, sizeof cur_ids);
        job_done = 0;
    }
    pthread_mutex_unlock(&mu);
    return r;
}

int fws_set(const char *id, int on) {
    pthread_mutex_lock(&mu);
    List *l = id ? find(id) : NULL;
    if (!l) { pthread_mutex_unlock(&mu); return -1; }
    l->on = !!on;
    save_config();
    kick(on ? 3 : 0);                   /* enabling one downloads it if there's no copy yet */
    pthread_mutex_unlock(&mu);
    return 0;
}

int fws_add_custom(const char *url, const char *format, char *id_out, char *err, size_t errlen) {
    char h[256], p[FWS_URL_MAX + 1];
    int port;
    if (fws_split_url(url, h, sizeof h, &port, p, sizeof p)) { snprintf(err, errlen, "fw.list.badUrl"); return -1; }
    if (!fwl_format_from_name(format)) { snprintf(err, errlen, "fw.list.badFormat"); return -1; }
    pthread_mutex_lock(&mu);
    int ncu = 0;
    for (int i = 0; i < nl; i++) {
        ncu += L[i].custom;
        if (!strcmp(L[i].url, url)) { pthread_mutex_unlock(&mu); snprintf(err, errlen, "fw.list.exists"); return -1; }
    }
    if (ncu >= FWS_MAX_CUSTOM || nl >= FWS_MAX_LISTS) { pthread_mutex_unlock(&mu); snprintf(err, errlen, "fw.list.tooMany"); return -1; }
    char id[32];
    for (int k = 1; k < 1000; k++) { snprintf(id, sizeof id, "custom-%d", k); if (!find(id)) break; }
    if (add_list(id, url, format, 0, 1)) { pthread_mutex_unlock(&mu); snprintf(err, errlen, "fw.list.badUrl"); return -1; }
    L[nl - 1].on = 1;
    save_config();
    kick(3);
    pthread_mutex_unlock(&mu);
    snprintf(id_out, 32, "%s", id);
    return 0;
}

int fws_remove_custom(const char *id) {
    pthread_mutex_lock(&mu);
    List *l = id ? find(id) : NULL;
    if (!l || !l->custom) { pthread_mutex_unlock(&mu); return -1; }
    char p[PATH_MAX];
    cache_path(l->id, "txt", p, sizeof p);
    unlink(p);
    cache_path(l->id, "json", p, sizeof p);
    unlink(p);
    int k = (int)(l - L);
    memmove(&L[k], &L[k + 1], sizeof(List) * (size_t)(nl - k - 1));
    nl--;
    save_config();
    kick(0);
    pthread_mutex_unlock(&mu);
    return 0;
}

int fws_update(int force) {
    pthread_mutex_lock(&mu);
    int any = 0;
    long long t = now_s();
    for (int i = 0; i < nl; i++) {
        if (!L[i].on) continue;
        int stale = t - L[i].updated > FWS_STALE_S && !(L[i].error[0] && t - L[i].tried < 3600);
        if (force || stale || !L[i].updated) any = 1;
    }
    int r = !any ? 1 : busy ? -1 : 0;
    if (any) kick(force ? 2 : 1);
    pthread_mutex_unlock(&mu);
    return r;
}

const char *fws_match(const char *const *names, int n, int fam, const uint8_t *addr) {
    if (!cur) return NULL;
    uint16_t t = 0;
    for (int i = 0; i < n && !t; i++) t = fwl_match_name(cur, names[i]);
    if (!t && (fam == 4 || fam == 6) && addr) t = fwl_match_addr(cur, fam, addr);
    return t && t <= FWS_MAX_LISTS && cur_ids[t - 1][0] ? cur_ids[t - 1] : NULL;
}

char *fws_json(void) {
    JBuf b;
    jb_init(&b);
    pthread_mutex_lock(&mu);
    jb_raw(&b, "{\"lists\":[");
    for (int i = 0; i < nl; i++) {
        const List *l = &L[i];
        jb_raw(&b, i ? ",{\"id\":" : "{\"id\":");
        jb_str(&b, l->id);
        jb_raw(&b, ",\"url\":");
        jb_str(&b, l->url);
        jb_raw(&b, ",\"format\":");
        jb_str(&b, l->format);
        jb_raw(&b, ",\"error\":");
        jb_str(&b, l->error);
        jb_raw(&b, ",\"topic\":");
        jb_str(&b, l->custom ? "custom" : l->topic);
        jb_raw(&b, ",\"source\":");
        jb_str(&b, l->source);
        jb_raw(&b, ",\"license\":");
        jb_str(&b, l->license);
        jb_raw(&b, ",\"homepage\":");
        jb_str(&b, l->homepage);
        jb_printf(&b, ",\"approx\":%ld", l->approx);
        jb_printf(&b, ",\"custom\":%s,\"on\":%s,\"updated\":%lld,\"tried\":%lld,\"domains\":%ld,\"nets\":%ld,\"truncated\":%s}",
                  l->custom ? "true" : "false", l->on ? "true" : "false", l->updated, l->tried, l->domains, l->nets,
                  l->truncated ? "true" : "false");
    }
    jb_printf(&b, "],\"busy\":%s,\"domains\":%zu,\"nets\":%zu}", busy ? "true" : "false", fwl_domains(cur), fwl_nets(cur));
    pthread_mutex_unlock(&mu);
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}
