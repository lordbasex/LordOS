/* xsession.c — xdg-session-management-v1: an app's windows come back where they were when it starts again
 * (docs/WAYLAND.md, "Session management").
 *
 * An app asks for a session (get_session), new or by the id it was given last time, names its windows in it
 * (add_toplevel) and, when it starts again, asks for each by name before its first commit (restore_toplevel). LordOS
 * keeps, per window of a session, its place on screen (its geometry) and whether it was maximized, updated while it
 * is open (every XS_TICK_MS, and when it closes); a restored window opens there (the shell asks, xsession_frame, and
 * fits it on today's displays with sdk/sessionmodel.js fitFrame). This works together with LordOS's own session
 * restore: that one reopens the apps after login, the app then restores its windows one by one through this, and a
 * window restored this way keeps its own place over the remembered one (sdk/wayland.js).
 *
 * The data is xsstore.h's: scoped to who lord-server checked the app is, never to an id or app_id the app sent; ids
 * are 16 bytes from getrandom; another app's id finds nothing. The file is written by lord-server only (lord.fs may not
 * touch it, fsops.c), 0600 in a 0700 folder, replaced whole (write and rename). Nothing in it can launch anything: it
 * holds window places, read back only for windows that already exist. */
#include "wlshell.h"
#include "xsstore.h"
#include "tiutf8.h"
#include "xdg-session-management-v1-protocol.h"
#include "xdg-shell-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

#define XS_TICK_MS 2000

typedef struct TopSess {
    struct wl_resource *res;            /* xdg_toplevel_session_v1 */
    struct wl_resource *top;            /* its xdg_toplevel, NULL once gone */
    struct wl_listener top_gone;
    struct wl_client *client;
    struct XsHold *hold;                /* its session object, NULL when inert */
    int id;                             /* surface id */
    char name[XS_NAME_MAX + 1];         /* "" when the name can't be stored (too long, control characters) */
    int restore, rr[4], rmax;           /* restored: the place the shell takes once (xsession_frame) */
    struct wl_list link;
} TopSess;

/* An xdg_session_v1 object and the session it holds (NULL once replaced, removed or destroyed). */
typedef struct XsHold {
    struct wl_resource *res;
    XsSess *s;
    struct wl_client *client;
    struct wl_list link;
} XsHold;

static XsStore store;
static struct wl_list tops, holds;
static struct wl_event_source *tick;
static int dirty;
static char path[512], dir[512];

static long long now_s(void) { return (long long)time(NULL); }

/* ---- the file ---- */

static void load(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0] || snprintf(dir, sizeof dir, "%s/.config/lordos/wl-sessions", home) >= (int)sizeof dir ||
        snprintf(path, sizeof path, "%s/sessions", dir) >= (int)sizeof path) { path[0] = 0; return; }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    struct stat st;
    char *buf = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() && st.st_size > 0 && st.st_size <= XS_FILE_MAX &&
        (buf = malloc((size_t)st.st_size))) {
        size_t got = 0;
        while (got < (size_t)st.st_size) {
            ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        if (got == (size_t)st.st_size) xs_load(&store, buf, got);
    }
    free(buf);
    close(fd);
}

static void save(void) {
    dirty = 0;
    if (!path[0]) return;
    size_t len = 0;
    char *b = xs_dump(&store, &len);
    if (!b) return;
    char parent[520];
    snprintf(parent, sizeof parent, "%.*s", (int)(strrchr(dir, '/') - dir), dir);
    mkdir(parent, 0700);
    if (mkdir(dir, 0700) && errno != EEXIST) { free(b); return; }
    struct stat ds;
    if (lstat(dir, &ds) || !S_ISDIR(ds.st_mode) || ds.st_uid != getuid()) { free(b); return; }
    char tmp[540];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { free(b); return; }
    size_t o = 0;
    while (o < len) {
        ssize_t n = write(fd, b + o, len - o);
        if (n <= 0) break;
        o += (size_t)n;
    }
    free(b);
    if (close(fd) || o != len || rename(tmp, path)) unlink(tmp);
}

/* ---- places ---- */

/* The window's place now, into its session's entry (a maximized or fullscreen window keeps its last normal place). */
static void snap(TopSess *t) {
    if (!t->hold || !t->hold->s || !t->name[0] || !t->top) return;
    int r[4], mx = 0, fs = 0;
    if (!wl__window_rect(t->id, r, &mx, &fs)) return;
    XsTop *e = xs_top_put(t->hold->s, t->name);
    if (!e) return;
    int was[4] = { e->r[0], e->r[1], e->r[2], e->r[3] }, wasmax = e->max;
    if (fs) return;
    if (mx) e->max = 1;
    else xs_top_set(e, r, 0);
    if (memcmp(was, e->r, sizeof was) || wasmax != e->max) dirty = 1;
}

static int on_tick(void *data) {
    (void)data;
    TopSess *t;
    wl_list_for_each(t, &tops, link) snap(t);
    if (dirty) save();
    wl_event_source_timer_update(tick, XS_TICK_MS);
    return 0;
}

/* ---- xdg_toplevel_session_v1 ---- */

static void ts_inert(TopSess *t) {
    t->hold = NULL;
}
static void ts_forget(TopSess *t) {
    snap(t);
    if (dirty) save();
    ts_inert(t);
    if (t->top) {
        wl_list_remove(&t->top_gone.link);
        t->top = NULL;
    }
}
static void on_top_gone(struct wl_listener *l, void *data) {
    (void)data;
    TopSess *t = wl_container_of(l, t, top_gone);
    wl_list_init(&t->top_gone.link);
    ts_forget(t);
}

static int name_ok(struct wl_resource *err_on, uint32_t code, const char *name) {
    if (name && tiu_valid_prefix(name, strlen(name)) == strlen(name)) return 1;
    wl_resource_post_error(err_on, code, "the name is not valid UTF-8");
    return 0;
}
/* a live window of this session already has that name */
static TopSess *named(XsHold *h, const char *name, TopSess *except) {
    TopSess *t;
    wl_list_for_each(t, &tops, link) if (t != except && t->hold == h && t->top && !strcmp(t->name, name)) return t;
    return NULL;
}

static void ts_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void ts_rename(struct wl_client *c, struct wl_resource *r, const char *name) {
    (void)c;
    TopSess *t = wl_resource_get_user_data(r);
    if (!name_ok(r, XDG_SESSION_V1_ERROR_INVALID_NAME, name) || !t || !t->hold || !t->hold->s) return;
    char nn[XS_NAME_MAX + 1] = "";
    if (xs_name_ok(name)) snprintf(nn, sizeof nn, "%s", name);
    if (!strcmp(nn, t->name)) return;
    if ((nn[0] && named(t->hold, nn, t)) || (nn[0] && xs_top(t->hold->s, nn))) {
        wl_resource_post_error(r, XDG_SESSION_V1_ERROR_NAME_IN_USE, "the session already has a toplevel with that name");
        return;
    }
    XsTop *old = t->name[0] ? xs_top(t->hold->s, t->name) : NULL;
    if (old && nn[0]) snprintf(old->name, sizeof old->name, "%s", nn);   /* the state goes along */
    else if (old) xs_top_del(t->hold->s, t->name);
    snprintf(t->name, sizeof t->name, "%s", nn);
    dirty = 1;
}
static const struct xdg_toplevel_session_v1_interface ts_impl = { .destroy = ts_destroy, .rename = ts_rename };
static void ts_gone(struct wl_resource *r) {
    TopSess *t = wl_resource_get_user_data(r);
    if (!t) return;
    ts_forget(t);
    wl_list_remove(&t->link);
    free(t);
}

/* ---- xdg_session_v1 ---- */

static void hold_inert(XsHold *h) {
    if (h->s) h->s->inuse = 0;
    h->s = NULL;
    TopSess *t;
    wl_list_for_each(t, &tops, link) if (t->hold == h) { snap(t); ts_inert(t); }
}

static TopSess *new_top(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *toplevel, const char *name, int restore) {
    XsHold *h = wl_resource_get_user_data(r);
    if (!name_ok(r, XDG_SESSION_V1_ERROR_INVALID_NAME, name)) return NULL;
    TopSess *t;
    wl_list_for_each(t, &tops, link)
        if (t->top == toplevel) { wl_resource_post_error(r, XDG_SESSION_V1_ERROR_ALREADY_ADDED, "the toplevel was already added to a session"); return NULL; }
    int sid = wl__toplevel_id(toplevel);
    if (restore && sid) {
        int m = 0, cm = 0, top = 0;
        if (wl__surface_state(sid, &m, &cm, &top) && cm) {
            wl_resource_post_error(r, XDG_SESSION_V1_ERROR_ALREADY_MAPPED, "restore_toplevel after the first commit");
            return NULL;
        }
    }
    char nn[XS_NAME_MAX + 1] = "";
    if (xs_name_ok(name)) snprintf(nn, sizeof nn, "%s", name);
    if (h && h->s && nn[0] && named(h, nn, NULL)) {
        wl_resource_post_error(r, XDG_SESSION_V1_ERROR_NAME_IN_USE, "a toplevel of the session has that name");
        return NULL;
    }
    t = calloc(1, sizeof *t);
    struct wl_resource *res = t ? wl_resource_create(c, &xdg_toplevel_session_v1_interface, wl_resource_get_version(r), id) : NULL;
    if (!res) { free(t); wl_client_post_no_memory(c); return NULL; }
    t->res = res;
    t->client = c;
    t->id = sid;
    snprintf(t->name, sizeof t->name, "%s", nn);
    wl_list_init(&t->top_gone.link);
    wl_list_insert(&tops, &t->link);
    wl_resource_set_implementation(res, &ts_impl, t, ts_gone);
    if (!h || !h->s || !sid) return t;  /* an inert session or toplevel: an inert object */
    t->hold = h;
    t->top = toplevel;
    t->top_gone.notify = on_top_gone;
    wl_resource_add_destroy_listener(toplevel, &t->top_gone);
    return t;
}

static void s_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void s_remove(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    XsHold *h = wl_resource_get_user_data(r);
    if (h && h->s) {
        XsSess *s = h->s;
        TopSess *t;
        wl_list_for_each(t, &tops, link) if (t->hold == h) ts_inert(t);
        h->s = NULL;
        xs_drop(&store, s);
        save();
    }
    wl_resource_destroy(r);
}
static void s_add(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *toplevel, const char *name) {
    TopSess *t = new_top(c, r, id, toplevel, name, 0);
    if (!t || !t->hold) return;
    xs_top_del(t->hold->s, t->name);    /* a new window under that name starts from nothing */
    dirty = 1;
}
static void s_restore(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *toplevel, const char *name) {
    TopSess *t = new_top(c, r, id, toplevel, name, 1);
    if (!t || !t->hold || !t->name[0]) return;
    XsTop *e = xs_top(t->hold->s, t->name);
    if (!xs_top_has(e)) return;         /* not known: as add_toplevel, with no event */
    memcpy(t->rr, e->r, sizeof t->rr);
    t->rmax = e->max;
    t->restore = 1;
    xdg_toplevel_session_v1_send_restored(t->res);  /* before its first configure (sent at its first commit) */
}
static void s_remove_top(struct wl_client *c, struct wl_resource *r, const char *name) {
    (void)c;
    XsHold *h = wl_resource_get_user_data(r);
    if (!h || !h->s || !name) return;
    TopSess *t;
    wl_list_for_each(t, &tops, link) if (t->hold == h && !strcmp(t->name, name)) ts_inert(t);
    xs_top_del(h->s, name);
    dirty = 1;
}
static const struct xdg_session_v1_interface s_impl = {
    .destroy = s_destroy, .remove = s_remove, .add_toplevel = s_add, .restore_toplevel = s_restore, .remove_toplevel = s_remove_top,
};
static void s_gone(struct wl_resource *r) {
    XsHold *h = wl_resource_get_user_data(r);
    if (!h) return;
    hold_inert(h);                      /* the state stays, no more updates */
    if (dirty) save();
    wl_list_remove(&h->link);
    free(h);
}

/* ---- the manager ---- */

static void m_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void m_get(struct wl_client *c, struct wl_resource *r, uint32_t id, uint32_t reason, const char *session_id) {
    if (reason < XDG_SESSION_MANAGER_V1_REASON_LAUNCH || reason > XDG_SESSION_MANAGER_V1_REASON_SESSION_RESTORE) {
        wl_resource_post_error(r, XDG_SESSION_MANAGER_V1_ERROR_INVALID_REASON, "invalid reason %u", reason);
        return;
    }
    if (session_id && tiu_valid_prefix(session_id, strlen(session_id)) != strlen(session_id)) {
        wl_resource_post_error(r, XDG_SESSION_MANAGER_V1_ERROR_INVALID_SESSION_ID, "the session id is not valid UTF-8");
        return;
    }
    char owner[XS_OWNER_MAX];
    snprintf(owner, sizeof owner, "%s", wl__client_owner(c));
    XsSess *s = session_id ? xs_find(&store, session_id, owner) : NULL;
    /* held by an object: this client's is an error, another client's is replaced (that client is told) */
    XsHold *old = NULL;
    if (s && s->inuse) {
        XsHold *o;
        wl_list_for_each(o, &holds, link) if (o->s == s) { old = o; break; }
        if (old && old->client == c) {
            wl_resource_post_error(r, XDG_SESSION_MANAGER_V1_ERROR_IN_USE, "the session is in use by this client");
            return;
        }
    }
    XsHold *h = calloc(1, sizeof *h);
    struct wl_resource *res = h ? wl_resource_create(c, &xdg_session_v1_interface, wl_resource_get_version(r), id) : NULL;
    if (!res) { free(h); wl_client_post_no_memory(c); return; }
    h->res = res;
    h->client = c;
    wl_list_insert(&holds, &h->link);
    wl_resource_set_implementation(res, &s_impl, h, s_gone);
    if (old) {
        hold_inert(old);
        xdg_session_v1_send_replaced(old->res);
    }
    int found = s != NULL;
    if (!s) {
        unsigned char rnd[16];
        char nid[XS_ID_LEN + 1];
        if (getrandom(rnd, sizeof rnd, 0) != (ssize_t)sizeof rnd) { wl_client_post_no_memory(c); return; }
        xs_new_id(rnd, nid);
        s = xs_new(&store, nid, owner, now_s());
        if (!s) return;                 /* every slot in use: an inert session, never created */
    }
    h->s = s;
    s->inuse = 1;
    s->used = now_s();
    dirty = 1;
    if (found) xdg_session_v1_send_restored(res);
    else xdg_session_v1_send_created(res, s->id);
}
static const struct xdg_session_manager_v1_interface m_impl = { .destroy = m_destroy, .get_session = m_get };
static void bind_m(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &xdg_session_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &m_impl, NULL, NULL);
}

void xsession_globals(struct wl_display *dpy) {
    wl_list_init(&tops);
    wl_list_init(&holds);
    load();
    tick = wl_event_loop_add_timer(wl_display_get_event_loop(dpy), on_tick, NULL);
    if (tick) wl_event_source_timer_update(tick, XS_TICK_MS);
    wl_global_create(dpy, &xdg_session_manager_v1_interface, 1, NULL, bind_m);
}

int xsession_frame(int id, int r[4], int *maximized) {
    TopSess *t;
    wl_list_for_each(t, &tops, link)
        if (t->id == id && t->restore && t->top) {
            t->restore = 0;
            memcpy(r, t->rr, sizeof t->rr);
            *maximized = t->rmax;
            return 1;
        }
    return 0;
}
