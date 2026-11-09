/* sqlc.c — see sqlc.h. */
#include "sqlc.h"
#include "sqlproto.h"
#include "../doc/proto.h"
#include "../doc/wire.h"
#include "../fsops.h"
#include "../sec/json.h"
#include "../sec/netconn.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define OPEN_TIMEOUT_MS  30000      /* connect, TLS and login, all together */
#define CONNECT_MS       10000      /* each address */
#define QUIT_GRACE_MS    2000       /* close: COM_QUIT, then the helper is killed */

typedef struct {
    int used, conn;
    int ready;                      /* the open succeeded */
    int sock;                       /* channel to lord-sql, -1 before it runs */
    pid_t pid;
    int open_req;                   /* the open's request id until it is answered */
    int busy;                       /* the request in flight, 0 if none */
    int closing;
    double closing_at, started;
    char fav[129];
} Slot;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static Slot slots[SQL_MAX_CONNS];
static SqlItem *queue;
static int nqueue, capqueue;
static int next_id = 1, next_conn = 1;
static char helper[PATH_MAX], fav_path[PATH_MAX];

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

const char *sql_favorites_path(void) {
    if (!fav_path[0]) {
        const char *h = getenv("HOME");
        snprintf(fav_path, sizeof fav_path, "%s/.config/lordos/databases/favorites.json", h ? h : "/nonexistent");
    }
    return fav_path;
}

int sql_available(void) {
    if (!helper[0]) {
        char self[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n <= 0) return -1;
        self[n] = 0;
        char *slash = strrchr(self, '/');
        if (!slash) return -1;
        *slash = 0;
        size_t sn = strlen(self);
        if (sn + 10 > sizeof helper) return -1;
        memcpy(helper, self, sn);
        memcpy(helper + sn, "/lord-sql", 10);
    }
    struct stat st, me;
    if (stat(helper, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) return -1;
    /* owned by root or by lord-server's owner, and not writable by anyone else */
    if (stat("/proc/self/exe", &me) || (st.st_uid != 0 && st.st_uid != me.st_uid) || (st.st_mode & 022)) return -1;
    return 0;
}

/* ---- favorites.json ---- */

static const char EMPTY_FAVS[] = "{\"version\":1,\"groups\":[],\"favorites\":[]}";

/* text without control characters, at most max bytes */
static int plain_text(const JVal *v, size_t max) {
    if (!v || v->type != J_STR || v->len > max) return 0;
    for (size_t i = 0; i < v->len; i++) { unsigned char c = (unsigned char)v->str[i]; if (c < 0x20 || c == 0x7f) return 0; }
    return 1;
}

/* optional member: missing, or plain text */
static int opt_text(const JVal *o, const char *k, size_t max) { const JVal *v = json_get(o, k); return !v || plain_text(v, max); }

/* a host name or address: no spaces or control characters, no leading '-' */
static int host_text(const JVal *v) {
    if (!v) return 1;
    if (!plain_text(v, 253)) return 0;
    if (v->len && v->str[0] == '-') return 0;
    for (size_t i = 0; i < v->len; i++) if (v->str[i] == ' ' || v->str[i] == '/' || v->str[i] == '\\') return 0;
    return 1;
}

static int opt_port(const JVal *o, const char *k) {
    const JVal *v = json_get(o, k);
    if (!v) return 1;
    return v->type == J_NUM && v->num >= 1 && v->num <= 65535 && v->num == (double)(int)v->num;
}

static int opt_bool(const JVal *o, const char *k) { const JVal *v = json_get(o, k); return !v || v->type == J_BOOL; }

static int opt_one_of(const JVal *o, const char *k, const char *const *set) {
    const JVal *v = json_get(o, k);
    if (!v) return 1;
    if (v->type != J_STR) return 0;
    for (int i = 0; set[i]; i++) if (!strcmp(v->str, set[i])) return 1;
    return 0;
}

/* names that could hold a secret: never in this file (passwords live in the keyring) */
static int secret_key(const char *k) {
    char l[64];
    size_t n = strlen(k);
    if (n >= sizeof l) n = sizeof l - 1;
    for (size_t i = 0; i < n; i++) l[i] = (char)((k[i] >= 'A' && k[i] <= 'Z') ? k[i] + 32 : k[i]);
    l[n] = 0;
    static const char *const part[] = { "password", "passwd", "passphrase", "secret", "token", "credential", "apikey", "api_key", NULL };
    static const char *const whole[] = { "pass", "pwd", "pw", "psw", NULL };
    for (int i = 0; part[i]; i++) if (strstr(l, part[i])) return 1;
    for (int i = 0; whole[i]; i++) if (!strcmp(l, whole[i])) return 1;
    return 0;
}

static int no_secrets(const JVal *v) {
    if (v->type != J_OBJ && v->type != J_ARR) return 1;
    for (const JVal *k = v->kid; k; k = k->next) {
        if (v->type == J_OBJ && k->key && secret_key(k->key)) return 0;
        if (!no_secrets(k)) return 0;
    }
    return 1;
}

/* id: letters, digits, '.', '_', '-' */
static int good_id(const JVal *v) {
    if (!v || v->type != J_STR || !v->len || v->len > 128) return 0;
    for (size_t i = 0; i < v->len; i++) {
        char c = v->str[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int bad(char *why, size_t wn, const char *s) { if (why && wn) snprintf(why, wn, "%s", s); return 0; }

static int fav_valid(const JVal *f, char *why, size_t wn) {
    static const char *const types[] = { "tcp", "socket", "ssh", "cloud", NULL };
    static const char *const modes[] = { "off", "preferred", "required", "verify", NULL };
    if (f->type != J_OBJ) return bad(why, wn, "favorite: not an object");
    if (!good_id(json_get(f, "id"))) return bad(why, wn, "favorite: bad id");
    if (!opt_text(f, "name", 256)) return bad(why, wn, "favorite: bad name");
    const JVal *g = json_get(f, "group");
    if (g && g->type != J_NULL && !good_id(g)) return bad(why, wn, "favorite: bad group");
    if (!opt_one_of(f, "type", types)) return bad(why, wn, "favorite: bad type");
    if (!host_text(json_get(f, "host"))) return bad(why, wn, "favorite: bad host");
    if (!opt_port(f, "port")) return bad(why, wn, "favorite: bad port");
    if (!opt_text(f, "user", 256) || !opt_text(f, "database", 256)) return bad(why, wn, "favorite: bad user or database");
    const JVal *s = json_get(f, "socket");
    if (s && (!plain_text(s, 200) || (s->len && s->str[0] != '/'))) return bad(why, wn, "favorite: bad socket");
    const JVal *c = json_get(f, "color");
    if (c && c->type != J_NULL && !(c->type == J_NUM && c->num >= 0 && c->num <= 6 && c->num == (double)(int)c->num))
        return bad(why, wn, "favorite: bad color");
    const JVal *ssh = json_get(f, "ssh");
    if (ssh && (ssh->type != J_OBJ || !host_text(json_get(ssh, "host")) || !opt_port(ssh, "port") ||
                !opt_text(ssh, "user", 256) || !opt_text(ssh, "key", 1000)))
        return bad(why, wn, "favorite: bad ssh");
    const JVal *tls = json_get(f, "tls");
    if (tls && (tls->type != J_OBJ || !opt_one_of(tls, "mode", modes) || !opt_text(tls, "ca", 1000) ||
                !opt_text(tls, "cert", 1000) || !opt_text(tls, "key", 1000)))
        return bad(why, wn, "favorite: bad tls");
    const JVal *cl = json_get(f, "cloud");
    if (cl && (cl->type != J_OBJ || !opt_text(cl, "service", 64) || !opt_text(cl, "region", 64) || !opt_text(cl, "iamProfile", 128)))
        return bad(why, wn, "favorite: bad cloud");
    const JVal *t = json_get(f, "trust");
    if (t) {
        if (t->type != J_STR || t->len > 128) return bad(why, wn, "favorite: bad trust");
        for (size_t i = 0; i < t->len; i++) {
            char ch = t->str[i];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F') || ch == ':')) return bad(why, wn, "favorite: bad trust");
        }
    }
    if (!opt_bool(f, "readOnly") || !opt_bool(f, "production") || !opt_bool(f, "quick")) return bad(why, wn, "favorite: bad flag");
    if (!opt_text(f, "timeZone", 64)) return bad(why, wn, "favorite: bad timeZone");
    const JVal *im = json_get(f, "imported");
    if (im && im->type != J_NULL && (im->type != J_OBJ || !opt_text(im, "from", 32) || !opt_text(im, "id", 256)))
        return bad(why, wn, "favorite: bad imported");
    return 1;
}

int sql_favorites_valid(const char *json, size_t len, char *why, size_t wn) {
    JDoc d;
    if (!json || len > SQL_FAV_FILE_MAX) return bad(why, wn, "too large");
    if (json_parse(json, len, &d, NULL, 0)) return bad(why, wn, "not JSON");
    int ok = 0;
    const JVal *r = d.root, *gs, *fs;
    if (!r || r->type != J_OBJ) { bad(why, wn, "not an object"); goto out; }
    if (json_num(r, "version", 0) != 1) { bad(why, wn, "bad version"); goto out; }
    if (!no_secrets(r)) { bad(why, wn, "holds a secret"); goto out; }
    gs = json_get(r, "groups");
    fs = json_get(r, "favorites");
    if (gs && (gs->type != J_ARR || gs->len > SQL_FAV_MAX)) { bad(why, wn, "bad groups"); goto out; }
    if (fs && (fs->type != J_ARR || fs->len > SQL_FAV_MAX)) { bad(why, wn, "bad favorites"); goto out; }
    for (const JVal *g = gs ? gs->kid : NULL; g; g = g->next) {
        const JVal *p = json_get(g, "parent");
        if (g->type != J_OBJ || !good_id(json_get(g, "id")) || !opt_text(g, "name", 256) || !opt_bool(g, "expanded") ||
            (p && p->type != J_NULL && !good_id(p))) { bad(why, wn, "bad group"); goto out; }
    }
    for (const JVal *f = fs ? fs->kid : NULL; f; f = f->next) {
        if (!fav_valid(f, why, wn)) goto out;
        /* ids are unique */
        for (const JVal *o = fs->kid; o != f; o = o->next)
            if (!strcmp(json_str(o, "id"), json_str(f, "id"))) { bad(why, wn, "duplicate id"); goto out; }
    }
    ok = 1;
out:
    json_free(&d);
    return ok;
}

static void copy(char *out, size_t cap, const char *s) { snprintf(out, cap, "%s", s ? s : ""); }

int sql_fav_find(const char *json, size_t len, const char *id, SqlFav *f) {
    memset(f, 0, sizeof *f);
    if (!id || !sql_favorites_valid(json, len, NULL, 0)) return -1;
    JDoc d;
    if (json_parse(json, len, &d, NULL, 0)) return -1;
    int found = -1;
    const JVal *fs = json_get(d.root, "favorites");
    for (const JVal *x = fs ? fs->kid : NULL; x; x = x->next) {
        if (strcmp(json_str(x, "id"), id)) continue;
        copy(f->id, sizeof f->id, id);
        copy(f->type, sizeof f->type, json_str(x, "type") ? json_str(x, "type") : "tcp");
        copy(f->host, sizeof f->host, json_str(x, "host"));
        copy(f->user, sizeof f->user, json_str(x, "user"));
        copy(f->database, sizeof f->database, json_str(x, "database"));
        copy(f->socket, sizeof f->socket, json_str(x, "socket"));
        f->port = (int)json_num(x, "port", 3306);
        const JVal *t = json_get(x, "tls");
        copy(f->tls, sizeof f->tls, t && json_str(t, "mode") ? json_str(t, "mode") : "preferred");
        copy(f->ca, sizeof f->ca, t ? json_str(t, "ca") : NULL);
        copy(f->cert, sizeof f->cert, t ? json_str(t, "cert") : NULL);
        copy(f->key, sizeof f->key, t ? json_str(t, "key") : NULL);
        copy(f->trust, sizeof f->trust, json_str(x, "trust"));
        const JVal *c = json_get(x, "cloud");
        copy(f->iam, sizeof f->iam, c ? json_str(c, "iamProfile") : NULL);
        found = 0;
        break;
    }
    json_free(&d);
    return found;
}

static int mkdirs(const char *p, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", p);
    for (char *s = tmp + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(tmp, mode); *s = '/'; }
    return mkdir(tmp, mode) && errno != EEXIST ? -1 : 0;
}

char *sql_favorites_read(size_t *len) {
    *len = 0;
    int fd = open(sql_favorites_path(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    char *b = NULL;
    if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size <= (off_t)SQL_FAV_FILE_MAX) {
        b = malloc((size_t)st.st_size + 1);
        ssize_t r = b ? read(fd, b, (size_t)st.st_size) : -1;
        if (r >= 0 && sql_favorites_valid(b, (size_t)r, NULL, 0)) { b[r] = 0; *len = (size_t)r; }
        else { free(b); b = NULL; }
    }
    if (fd >= 0) close(fd);
    if (!b) { b = strdup(EMPTY_FAVS); *len = b ? strlen(b) : 0; }
    return b;
}

int sql_favorites_save(const char *j, size_t len) {
    char why[96];
    if (!sql_favorites_valid(j, len, why, sizeof why)) {
        fprintf(stderr, "lord-server: favorites.json refused: %s\n", why);
        return 1;
    }
    const char *path = sql_favorites_path();
    char dir[PATH_MAX], tmp[PATH_MAX + 16];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    if (mkdirs(dir, 0700)) return 2;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    int ok = fd >= 0 && write(fd, j, len) == (ssize_t)len && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    ok = ok && rename(tmp, path) == 0;
    if (!ok) { unlink(tmp); return 2; }
    return 0;
}

/* ---- the queue ---- */

/* with mu held; takes data */
static void push_own(int id, int type, char *data, size_t len) {
    if (!data) return;
    if (nqueue == capqueue) {
        int nc = capqueue ? capqueue * 2 : 32;
        SqlItem *q = realloc(queue, sizeof *q * (size_t)nc);
        if (!q) { free(data); return; }
        queue = q;
        capqueue = nc;
    }
    queue[nqueue++] = (SqlItem){ id, type, data, len };
}

static void push(int id, int type, const char *json) {
    char *d = strdup(json);
    if (d) push_own(id, type, d, strlen(d));
}

static void push_err(int id, const char *code, const char *message) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"error\":");
    jb_str(&b, code);
    if (message) { jb_raw(&b, ",\"message\":"); jb_str(&b, message); }
    jb_raw(&b, "}");
    if (!b.oom) push_own(id, SQL_ERR, b.p, b.n);
    else jb_free(&b);
}

int sql_poll(SqlItem **items) {
    pthread_mutex_lock(&mu);
    int n = nqueue;
    *items = queue;
    queue = NULL;
    nqueue = capqueue = 0;
    pthread_mutex_unlock(&mu);
    return n;
}

void sql_items_free(SqlItem *items, int n) {
    for (int i = 0; i < n; i++) free(items[i].data);
    free(items);
}

/* ---- connecting ---- */

int sql_socket_ok(const char *real, const char *runtime_dir) {
    static const char *const deny[] = { "/run/user", "/run/dbus", "/run/systemd", "/run/lordos", "/run/udev",
                                        "/var/run", "/tmp/.X11-unix", "/tmp/.ICE-unix", "/proc", "/sys", "/dev", NULL };
    if (!real || real[0] != '/') return 0;
    for (int i = 0; deny[i]; i++) {
        size_t n = strlen(deny[i]);
        if (!strncmp(real, deny[i], n) && (real[n] == 0 || real[n] == '/')) return 0;
    }
    if (runtime_dir && runtime_dir[0] == '/') {
        size_t n = strlen(runtime_dir);
        while (n > 1 && runtime_dir[n - 1] == '/') n--;
        if (!strncmp(real, runtime_dir, n) && (real[n] == 0 || real[n] == '/')) return 0;
    }
    return 1;
}

static int connect_unix(const char *path, const char **why) {
    char real[PATH_MAX];
    struct stat st;
    *why = "connect";
    if (!realpath(path[0] ? path : "/run/mysqld/mysqld.sock", real) || stat(real, &st) || !S_ISSOCK(st.st_mode)) return -1;
    if (!sql_socket_ok(real, getenv("XDG_RUNTIME_DIR")) || strlen(real) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, real, strlen(real) + 1);         /* the length was checked above */
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    return fd;
}

static int connect_tcp(const char *host, int port, const char **why) {
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    *why = "dns";
    if (!host[0] || getaddrinfo(host, ps, &hints, &res)) return -1;
    *why = "connect";
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        /* made and connected by the sql connector, so the firewall counts and decides it as
         * lordos:sql (sec/netconn.h); a denied connection fails with EPERM */
        fd = netconn_socket(NC_SQL, a->ai_family, SOCK_STREAM, a->ai_addr, a->ai_addrlen);
        if (fd < 0) continue;
        struct pollfd p = { fd, POLLOUT, 0 };
        int err = 0;
        socklen_t el = sizeof err;
        if (poll(&p, 1, CONNECT_MS) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) || err) { close(fd); fd = -1; continue; }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    }
    freeaddrinfo(res);
    return fd;
}

/* A certificate or key file of the favorite: relative to home, or absolute; real; in home or a
 * mounted disk (fsops_allowed); a regular file of at most 4 MB. */
static int open_user_file(const char *path) {
    char full[PATH_MAX * 2], real[PATH_MAX];
    const char *h = getenv("HOME");
    if (!path || !path[0]) return -1;
    if (path[0] == '/') snprintf(full, sizeof full, "%s", path);
    else if (path[0] == '~' && path[1] == '/' && h) snprintf(full, sizeof full, "%s/%s", h, path + 2);
    else if (h) snprintf(full, sizeof full, "%s/%s", h, path);
    else return -1;
    if (!realpath(full, real) || !fsops_allowed(real)) return -1;
    int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > (off_t)(4 << 20)) { close(fd); return -1; }
    fcntl(fd, F_SETFL, 0);
    return fd;
}

typedef struct {
    int slot, conn, req;
    SqlFav f;
    char *password;
    size_t pwlen;
    char db[260];
} OpenJob;

static void job_free(OpenJob *j) {
    if (j->password) { explicit_bzero(j->password, j->pwlen); free(j->password); }
    explicit_bzero(j, sizeof *j);
    free(j);
}

/* SQ_OPEN's JSON, written into one buffer sized up front (no copies of the password left behind by realloc) */
static char *open_json(const OpenJob *j, int ca, int cert, int key, size_t *len, size_t *cap) {
    const SqlFav *f = &j->f;
    int tls_off = !strcmp(f->type, "socket") || !strcmp(f->tls, "off");
    size_t n = 512 + 6 * (j->pwlen + strlen(f->user) + strlen(j->db) + strlen(f->host) + strlen(f->trust) + 16);
    char *b = malloc(n);
    if (!b) return NULL;
    size_t o = 0;
    o += (size_t)snprintf(b + o, n - o, "{\"user\":");
    o += json_quote(b + o, n - o, f->user);
    o += (size_t)snprintf(b + o, n - o, ",\"password\":");
    o += json_quote(b + o, n - o, j->password ? j->password : "");
    o += (size_t)snprintf(b + o, n - o, ",\"database\":");
    o += json_quote(b + o, n - o, j->db);
    o += (size_t)snprintf(b + o, n - o, ",\"host\":");
    o += json_quote(b + o, n - o, f->host);
    o += (size_t)snprintf(b + o, n - o, ",\"tls\":\"%s\",\"trust\":", tls_off ? "off" : f->tls);
    o += json_quote(b + o, n - o, f->trust);
    o += (size_t)snprintf(b + o, n - o, ",\"ca\":%d,\"cert\":%d,\"key\":%d}", ca, cert, key);
    if (o >= n) { explicit_bzero(b, n); free(b); return NULL; }
    *len = o;
    *cap = n;
    return b;
}

static char *read_big(int fd, size_t *len) {
    long sz = doc_check_sealed(fd, 0);
    if (sz < 0 || (size_t)sz > SQL_REPLY_MAX) return NULL;
    char *b = malloc((size_t)sz + 1);
    if (!b) return NULL;
    size_t off = 0;
    while (off < (size_t)sz) {
        ssize_t g = pread(fd, b + off, (size_t)sz - off, (off_t)off);
        if (g <= 0) { if (g < 0 && errno == EINTR) continue; free(b); return NULL; }
        off += (size_t)g;
    }
    b[sz] = 0;
    *len = (size_t)sz;
    return b;
}

/* {"conn":N, + the helper's object */
static char *with_conn(int conn, const char *data, size_t len, size_t *out_len) {
    if (!data || len < 2 || data[0] != '{') return NULL;
    size_t rest = len - 1;
    int empty = 0;
    for (size_t i = 1; i < len && !empty; i++) { if (data[i] == '}') empty = 1; else if (data[i] != ' ') break; }
    char head[48];
    int hn = snprintf(head, sizeof head, empty ? "{\"conn\":%d" : "{\"conn\":%d,", conn);
    char *b = malloc((size_t)hn + rest + 1);
    if (!b) return NULL;
    memcpy(b, head, (size_t)hn);
    memcpy(b + hn, data + 1, rest);
    b[hn + rest] = 0;
    *out_len = (size_t)hn + rest;
    return b;
}

/* the cert event: lord-server's own conn and fav, the helper's strings checked */
static void push_cert(Slot *s, const char *json, size_t len) {
    JDoc d;
    if (!json || json_parse(json, len, &d, NULL, 0)) return;
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"ev\":\"cert\",\"conn\":%d,\"fav\":", s->conn);
    jb_str(&b, s->fav);
    static const char *const keys[] = { "sha256", "subject", "issuer", "problem", NULL };
    for (int i = 0; keys[i]; i++) {
        const char *v = json_str(d.root, keys[i]);
        jb_printf(&b, ",\"%s\":", keys[i]);
        if (v && strlen(v) < 512) jb_str(&b, v);
        else jb_str(&b, "");
    }
    jb_raw(&b, "}");
    json_free(&d);
    if (!b.oom) push_own(0, SQL_EVT, b.p, b.n);
    else jb_free(&b);
}

/* with mu held: a reply from the helper for request id */
static void on_reply(Slot *s, int id, int type, char *data, size_t len) {
    if (id && id == s->open_req) {
        s->open_req = 0;
        if (type == SQL_OK) {
            size_t n = 0;
            char *w = with_conn(s->conn, data, len, &n);
            free(data);
            if (!w) { push_err(id, "closed", "bad reply"); return; }
            s->ready = 1;
            push_own(id, SQL_OK, w, n);
            return;
        }
        push_own(id, SQL_ERR, data, len);
        return;
    }
    if (id && id == s->busy) {
        s->busy = 0;
        push_own(id, type, data, len);
        return;
    }
    free(data);                     /* nothing was asked with that id */
}

static void *conn_thread(void *arg) {
    OpenJob *j = arg;
    Slot *s = &slots[j->slot];
    const char *why = "connect";
    int fd = !strcmp(j->f.type, "socket") ? connect_unix(j->f.socket, &why) : connect_tcp(j->f.host, j->f.port, &why);
    int files[3] = { -1, -1, -1 }, nfiles = 0, has[3] = { 0, 0, 0 };
    const char *fail = fd < 0 ? why : NULL, *detail = NULL;
    int sv[2] = { -1, -1 };
    pid_t pid = -1;
    if (!fail && strcmp(j->f.type, "socket") && strcmp(j->f.tls, "off")) {
        const char *paths[3] = { j->f.ca, j->f.cert, j->f.key };
        static const char *const names[3] = { "ca", "cert", "key" };
        for (int i = 0; i < 3 && !fail; i++) {
            if (!paths[i][0]) continue;
            int x = open_user_file(paths[i]);
            if (x < 0) { fail = "tls"; detail = names[i]; break; }
            files[nfiles++] = x;
            has[i] = 1;
        }
    }
    if (!fail && socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) fail = "unavailable";
    if (!fail) {
        /* above 4 first, so the dup2s below can't overwrite each other */
        int a = fcntl(sv[1], F_DUPFD_CLOEXEC, 10), b = fcntl(fd, F_DUPFD_CLOEXEC, 10);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, a, 3);
        posix_spawn_file_actions_adddup2(&fa, b, 4);
        posix_spawn_file_actions_addclosefrom_np(&fa, 5);     /* nothing else of lord-server's reaches it */
        posix_spawnattr_t at;
        posix_spawnattr_init(&at);
        sigset_t none, all;
        sigemptyset(&none);
        sigfillset(&all);
        posix_spawnattr_setsigmask(&at, &none);
        posix_spawnattr_setsigdefault(&at, &all);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        char *envp[] = { "LANG=C.UTF-8", NULL };
        char *argv[] = { "lord-sql", NULL };
        int e = a >= 0 && b >= 0 ? posix_spawn(&pid, helper, &fa, &at, argv, envp) : -1;
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&at);
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        close(sv[1]);
        sv[1] = -1;
        if (e) { pid = -1; fail = "unavailable"; }
    }
    if (fd >= 0) close(fd);
    pthread_mutex_lock(&mu);
    if (!fail) { s->sock = sv[0]; s->pid = pid; }
    int closing = s->closing;
    pthread_mutex_unlock(&mu);
    if (!fail && closing) kill(pid, SIGKILL);
    if (!fail && !closing) {
        size_t n = 0, cap = 0;
        char *msg = open_json(j, has[0], has[1], has[2], &n, &cap);
        if (!msg || doc_send(sv[0], SQ_OPEN, (uint32_t)j->req, msg, n, nfiles ? files : NULL, nfiles)) fail = "unavailable";
        if (msg) { explicit_bzero(msg, cap); free(msg); }
    }
    for (int i = 0; i < nfiles; i++) close(files[i]);
    int req = j->req;
    job_free(j);                     /* the password is gone from lord-server from here on */
    if (fail) {
        pthread_mutex_lock(&mu);
        if (s->open_req == req) { s->open_req = 0; push_err(req, s->closing ? "closed" : fail, detail); }
        pthread_mutex_unlock(&mu);
    }
    /* the helper's answers, until its channel closes */
    while (!fail) {
        struct pollfd p = { sv[0], POLLIN, 0 };
        int r = poll(&p, 1, 250);
        double now = now_ms();
        pthread_mutex_lock(&mu);
        int kill_now = (s->closing && now - s->closing_at > QUIT_GRACE_MS) || (!s->ready && s->open_req && now - s->started > OPEN_TIMEOUT_MS);
        if (!s->ready && s->open_req && now - s->started > OPEN_TIMEOUT_MS) { push_err(s->open_req, "connect", "timeout"); s->open_req = 0; }
        pthread_mutex_unlock(&mu);
        if (kill_now) { kill(pid, SIGKILL); break; }
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (r < 0) break; continue; }
        DocMsg m;
        if (doc_recv(sv[0], &m)) break;
        int id = (int)m.h.id;
        char *data = NULL;
        size_t len = 0;
        int type = m.h.op == SR_ERR ? SQL_ERR : SQL_OK;
        if (m.h.op == SR_BIG && m.nfds == 1) {
            data = read_big(m.fds[0], &len);
            if (m.json && strstr(m.json, "\"err\"")) type = SQL_ERR;
        } else if ((m.h.op == SR_OK || m.h.op == SR_ERR) && m.json) {
            data = m.json;
            len = m.len;
            m.json = NULL;
        }
        pthread_mutex_lock(&mu);
        if (m.h.op == SR_EVT) { if (s->open_req && id == s->open_req) push_cert(s, m.json, m.len); }
        else if (data) on_reply(s, id, type, data, len);
        else if (id && (id == s->open_req || id == s->busy)) {
            if (id == s->open_req) s->open_req = 0; else s->busy = 0;
            push_err(id, "closed", "bad reply");
        }
        pthread_mutex_unlock(&mu);
        doc_msg_free(&m);
    }
    /* the end: the helper exited, crashed or was killed */
    int status = 0;
    if (pid > 0) {
        double t0 = now_ms();
        while (waitpid(pid, &status, WNOHANG) == 0) {
            if (now_ms() - t0 > 1000) { kill(pid, SIGKILL); waitpid(pid, &status, 0); break; }
            struct timespec ts = { 0, 10 * 1000000 };
            nanosleep(&ts, NULL);
        }
    }
    pthread_mutex_lock(&mu);
    if (s->sock >= 0) close(s->sock);
    else if (sv[0] >= 0) close(sv[0]);
    s->sock = -1;
    if (s->open_req) push_err(s->open_req, "closed", NULL);
    if (s->busy) push_err(s->busy, "closed", NULL);
    if (s->ready && !s->closing) {
        char ev[128];
        const char *reason = WIFSIGNALED(status) ? "crash" : WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "server" : "error";
        snprintf(ev, sizeof ev, "{\"ev\":\"closed\",\"conn\":%d,\"reason\":\"%s\"}", s->conn, reason);
        push(0, SQL_EVT, ev);
    }
    memset(s, 0, sizeof *s);
    s->sock = -1;
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* ---- requests ---- */

static Slot *find_conn(int conn) {
    for (int i = 0; i < SQL_MAX_CONNS; i++) if (slots[i].used && slots[i].conn == conn) return &slots[i];
    return NULL;
}

static int good_db(const char *s, size_t n) {
    if (!s || !n || n > 256) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) return 0;
    return 1;
}

static int do_open(int id, const JVal *root) {
    const JVal *fv = json_get(root, "fav"), *pv = json_get(root, "password"), *dv = json_get(root, "database");
    if (!fv || fv->type != J_STR || (pv && pv->type != J_STR && pv->type != J_NULL) ||
        (dv && dv->type != J_NULL && (dv->type != J_STR || (dv->len && !good_db(dv->str, dv->len))))) { push_err(id, "notfound", NULL); return 0; }
    if (pv && pv->type == J_STR && pv->len > 500) { push_err(id, "toolarge", NULL); return 0; }
    size_t flen;
    char *favs = sql_favorites_read(&flen);
    OpenJob *j = calloc(1, sizeof *j);
    if (!favs || !j || sql_fav_find(favs, flen, fv->str, &j->f)) {
        free(favs);
        free(j);
        push_err(id, "notfound", NULL);
        return 0;
    }
    free(favs);
    const char *pw = pv && pv->type == J_STR ? pv->str : "";
    if (!strcmp(j->f.type, "ssh") || (!strcmp(j->f.type, "cloud") && !pw[0] && j->f.iam[0])) {
        job_free(j);
        push_err(id, "unsupported", NULL);
        return 0;
    }
    j->pwlen = strlen(pw) + 1;
    j->password = malloc(j->pwlen);
    if (!j->password) { job_free(j); push_err(id, "unavailable", NULL); return 0; }
    memcpy(j->password, pw, j->pwlen);
    snprintf(j->db, sizeof j->db, "%s", dv && dv->type == J_STR && dv->len ? dv->str : j->f.database);
    int slot = -1;
    for (int i = 0; i < SQL_MAX_CONNS && slot < 0; i++) if (!slots[i].used) slot = i;
    if (slot < 0) { job_free(j); push_err(id, "busy", "connections"); return 0; }
    Slot *s = &slots[slot];
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->sock = -1;
    s->conn = next_conn++;
    if (next_conn > 1 << 30) next_conn = 1;
    s->open_req = id;
    s->started = now_ms();
    snprintf(s->fav, sizeof s->fav, "%s", j->f.id);
    j->slot = slot;
    j->conn = s->conn;
    j->req = id;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, conn_thread, j)) {
        job_free(j);
        memset(s, 0, sizeof *s);
        s->sock = -1;
        push_err(id, "unavailable", NULL);
    }
    pthread_attr_destroy(&at);
    return 0;
}

int sql_request(const char *json, size_t len) {
    if (sql_available()) return -1;
    JDoc d;
    int parsed = len <= SQL_TEXT_MAX + 65536 && !json_parse(json, len, &d, NULL, 0);
    pthread_mutex_lock(&mu);
    int id = next_id++;
    if (next_id > 1 << 30) next_id = 1;
    if (!parsed) { push_err(id, len > SQL_TEXT_MAX ? "toolarge" : "unsupported", NULL); pthread_mutex_unlock(&mu); return id; }
    const JVal *r = d.root;
    const char *op = r && r->type == J_OBJ ? json_str(r, "op") : NULL;
    if (!op) { push_err(id, "unsupported", NULL); goto out; }
    if (!strcmp(op, "open")) {
        do_open(id, r);
        /* the password's copy inside the parsed document */
        const JVal *pv = json_get(r, "password");
        if (pv && pv->type == J_STR) explicit_bzero((char *)pv->str, pv->len);
        goto out;
    }
    int qop = !strcmp(op, "query") ? SQ_QUERY : !strcmp(op, "use") ? SQ_USE : !strcmp(op, "ping") ? SQ_PING : !strcmp(op, "close") ? SQ_END : 0;
    if (!qop) { push_err(id, "unsupported", NULL); goto out; }
    Slot *s = find_conn((int)json_num(r, "conn", 0));
    if (!s || s->closing) { push_err(id, "closed", NULL); goto out; }
    if (qop == SQ_END) {
        s->closing = 1;
        s->closing_at = now_ms();
        if (s->pid > 0 && (s->busy || !s->ready)) kill(s->pid, SIGKILL);      /* cancel what runs */
        else if (s->sock >= 0) doc_send(s->sock, SQ_END, 0, "{}", 2, NULL, 0);
        push(id, SQL_OK, "{}");
        goto out;
    }
    if (!s->ready || s->busy) { push_err(id, "busy", NULL); goto out; }
    if (qop == SQ_QUERY) {
        const JVal *sv = json_get(r, "sql");
        const JVal *mr = json_get(r, "maxRows");
        if (!sv || sv->type != J_STR || !sv->len || (mr && mr->type != J_NUM)) { push_err(id, "unsupported", NULL); goto out; }
        if (sv->len > SQL_TEXT_MAX) { push_err(id, "toolarge", NULL); goto out; }
    } else if (qop == SQ_USE) {
        const JVal *dv = json_get(r, "db");
        if (!dv || dv->type != J_STR || !good_db(dv->str, dv->len)) { push_err(id, "unsupported", NULL); goto out; }
    }
    /* the helper reads only what it needs from the same JSON */
    if (s->sock < 0 || doc_send(s->sock, (uint32_t)qop, (uint32_t)id, json, len, NULL, 0)) { push_err(id, "closed", NULL); goto out; }
    s->busy = id;
out:
    pthread_mutex_unlock(&mu);
    json_free(&d);
    return id;
}
