/* handsbus.c — org.lordos.Hands1 for lordos-mcp (see handsbus.h, docs/HANDS.md) */
#include "handsbus.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char introspect[] =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">"
    "<node><interface name='" HANDS_BUS_IFACE "'>"
    "<method name='Hello'><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='Status'><arg type='s' direction='out'/></method>"
    "<method name='Windows'><arg type='s' direction='out'/></method>"
    "<method name='ReadWindow'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='Find'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='Pointer'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='Scroll'><arg type='s' direction='in'/><arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='Key'><arg type='s' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='Type'><arg type='s' direction='in'/></method>"
    "<method name='SetValue'><arg type='s' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='Screenshot'><arg type='s' direction='in'/><arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='u' direction='in'/><arg type='ay' direction='out'/><arg type='s' direction='out'/></method>"
    "<method name='OpenApp'><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='OpenPath'><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='CloseWindow'><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='AppLog'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='s' direction='out'/></method>"
    "</interface></node>";

/* ---- the arguments ---- */

static const struct { const char *member, *sig; int call; } METHODS[] = {
    { "Hello", "s", HANDS_C_HELLO }, { "Status", "", HANDS_C_STATUS }, { "Windows", "", HANDS_C_WINDOWS },
    { "ReadWindow", "ssu", HANDS_C_READ }, { "Find", "ss", HANDS_C_FIND }, { "Pointer", "ssiiii", HANDS_C_POINTER },
    { "Scroll", "siisu", HANDS_C_SCROLL }, { "Key", "su", HANDS_C_KEY }, { "Type", "s", HANDS_C_TYPE },
    { "SetValue", "ss", HANDS_C_SETVALUE }, { "Screenshot", "siiiiu", HANDS_C_SHOT }, { "OpenApp", "s", HANDS_C_OPENAPP },
    { "OpenPath", "s", HANDS_C_OPENPATH }, { "CloseWindow", "s", HANDS_C_CLOSE }, { "AppLog", "ssu", HANDS_C_APPLOG },
};

#define COORD_MAX 65536

static int coord_ok(int32_t v) { return v >= -1 && v < COORD_MAX; }

/* a window id: "" (the one in front) or hands_id_ok */
static int win_ok(const char *s) { return s && (!s[0] || hands_id_ok(s)); }

int handsbus_parse(const DMsg *m, HandsReq *r) {
    memset(r, 0, sizeof *r);
    r->call = -1;
    if (!m || !m->member) return -1;
    const char *sig = m->sig ? m->sig : "";
    for (size_t i = 0; i < sizeof METHODS / sizeof METHODS[0]; i++)
        if (!strcmp(m->member, METHODS[i].member)) {
            if (strcmp(sig, METHODS[i].sig)) return -1;
            r->call = METHODS[i].call;
        }
    if (r->call < 0) return -1;
    DRd d;
    dr_init(&d, m);
    uint32_t len = 0;
    const char *s1, *s2;
    int32_t x, y, x2, y2;
    uint32_t u;
    switch (r->call) {
    case HANDS_C_HELLO:
        s1 = dr_str(&d, &len);
        if (d.err || !hands_text_ok(s1, len, 256, 0)) return -1;
        r->text = (char *)s1;
        r->text_len = len;
        return 0;
    case HANDS_C_STATUS:
    case HANDS_C_WINDOWS:
        return 0;
    case HANDS_C_READ:
        s1 = dr_str(&d, NULL);
        s2 = dr_str(&d, NULL);
        u = dr_u32(&d);
        if (d.err || !win_ok(s1) || (r->filter = hands_filter(s2)) < 0 || u > HANDS_DEPTH_MAX) return -1;
        snprintf(r->window, sizeof r->window, "%s", s1);
        r->depth = u ? (int)u : 30;
        return 0;
    case HANDS_C_FIND:
        s1 = dr_str(&d, NULL);
        s2 = dr_str(&d, &len);
        if (d.err || !win_ok(s1) || !len || !hands_text_ok(s2, len, HANDS_QUERY_MAX, 0)) return -1;
        snprintf(r->window, sizeof r->window, "%s", s1);
        r->text = (char *)s2;
        r->text_len = len;
        return 0;
    case HANDS_C_POINTER:
        s1 = dr_str(&d, NULL);
        s2 = dr_str(&d, NULL);
        x = dr_i32(&d); y = dr_i32(&d); x2 = dr_i32(&d); y2 = dr_i32(&d);
        if (d.err || !(r->action = hands_pointer_action(s1)) || !hands_ref_ok(s2)) return -1;
        if (!coord_ok(x) || !coord_ok(y) || !coord_ok(x2) || !coord_ok(y2)) return -1;
        if (!s2[0] && (x < 0 || y < 0)) return -1;                 /* a ref or a point */
        if (r->action == HANDS_P_DRAG && (x2 < 0 || y2 < 0)) return -1;
        snprintf(r->ref, sizeof r->ref, "%s", s2);
        r->x = x; r->y = y; r->x2 = x2; r->y2 = y2;
        return 0;
    case HANDS_C_SCROLL:
        s1 = dr_str(&d, NULL);
        x = dr_i32(&d); y = dr_i32(&d);
        s2 = dr_str(&d, NULL);
        u = dr_u32(&d);
        if (d.err || !hands_ref_ok(s1) || !coord_ok(x) || !coord_ok(y) || (!s1[0] && (x < 0 || y < 0))) return -1;
        if (!(r->dir = hands_direction(s2)) || u > HANDS_SCROLL_MAX) return -1;
        snprintf(r->ref, sizeof r->ref, "%s", s1);
        r->x = x; r->y = y;
        r->amount = (int)u;
        return 0;
    case HANDS_C_KEY: {
        s1 = dr_str(&d, &len);
        u = dr_u32(&d);
        HandsChord c[8];
        int n = 0;
        if (d.err || u > HANDS_REPEAT_MAX || hands_chords(s1, c, 8, &n)) return -1;
        r->text = (char *)s1;
        r->text_len = len;
        r->repeat = (int)u;
        return 0;
    }
    case HANDS_C_TYPE:
        s1 = dr_str(&d, &len);
        if (d.err || !len || !hands_text_ok(s1, len, HANDS_TEXT_MAX, 1)) return -1;
        r->text = (char *)s1;
        r->text_len = len;
        return 0;
    case HANDS_C_SETVALUE:
        s1 = dr_str(&d, NULL);
        s2 = dr_str(&d, &len);
        if (d.err || !s1[0] || !hands_ref_ok(s1) || !hands_text_ok(s2, len, HANDS_VALUE_MAX, 1)) return -1;
        snprintf(r->ref, sizeof r->ref, "%s", s1);
        r->text = (char *)s2;
        r->text_len = len;
        return 0;
    case HANDS_C_SHOT:
        s1 = dr_str(&d, NULL);
        x = dr_i32(&d); y = dr_i32(&d); x2 = dr_i32(&d); y2 = dr_i32(&d);
        u = dr_u32(&d);
        if (d.err || !(r->shot = hands_shot_target(s1, &r->display, r->window, sizeof r->window))) return -1;
        if (x < 0 || y < 0 || x2 < 0 || y2 < 0 || x >= COORD_MAX || y >= COORD_MAX || x2 >= COORD_MAX || y2 >= COORD_MAX) return -1;
        if (r->shot == HANDS_SHOT_REGION && (x2 < 1 || y2 < 1)) return -1;
        if (u && (u < 64 || u > 2560)) return -1;
        r->x = x; r->y = y; r->w = x2; r->h = y2;
        r->maxdim = u ? (int)u : HANDS_SHOT_MAXDIM;
        return 0;
    case HANDS_C_OPENAPP:
        s1 = dr_str(&d, NULL);
        if (d.err || !hands_name_ok(s1)) return -1;
        snprintf(r->app, sizeof r->app, "%s", s1);
        return 0;
    case HANDS_C_OPENPATH:
        s1 = dr_str(&d, &len);
        if (d.err || !hands_path_ok(s1)) return -1;
        r->text = (char *)s1;
        r->text_len = len;
        return 0;
    case HANDS_C_CLOSE:
        s1 = dr_str(&d, NULL);
        if (d.err || !hands_id_ok(s1)) return -1;
        snprintf(r->window, sizeof r->window, "%s", s1);
        return 0;
    case HANDS_C_APPLOG:
        s1 = dr_str(&d, NULL);
        s2 = dr_str(&d, &len);
        u = dr_u32(&d);
        if (d.err || !hands_name_ok(s1) || !hands_text_ok(s2, len, HANDS_QUERY_MAX, 0) || u > HANDS_LOG_MAX) return -1;
        snprintf(r->app, sizeof r->app, "%s", s1);
        r->text = (char *)s2;
        r->text_len = len;
        r->amount = u ? (int)u : 100;
        return 0;
    }
    return -1;
}

/* ---- the callers ---- */

typedef struct {
    char name[64];
    int st;                         /* 0 free, 1 asking the bus, 2 same user, 3 refused (another user, its process gone) */
    char exe[HANDS_EXE_MAX];        /* its program's path, for the prompt only (reported, not verified), "" unknown */
    int pidfd;                      /* the bus's ProcessFD: when that process exits, the connection's grant ends */
    const char *why;                /* why refused */
    uint32_t ask;
    DMsg pend;
    int has_pend;
} Caller;
static Caller callers[HANDSBUS_CALLERS];
static uint32_t ask_bus(DConn *c, const char *member, const char *sender);

static const char *err_name(int rc) {
    switch (rc) {
    case HANDS_E_OFF: return HANDS_ERR_OFF;
    case HANDS_E_ASKING: return HANDS_ERR_ASKING;
    case HANDS_E_DENIED: return HANDS_ERR_DENIED;
    case HANDS_E_STOPPED: return HANDS_ERR_STOPPED;
    case HANDS_E_ACTIVE: return HANDS_ERR_ACTIVE;
    case HANDS_E_REFUSED: return HANDS_ERR_REFUSED;
    case HANDS_E_RATE: return HANDS_ERR_RATE;
    case HANDS_E_NOTFOUND: return HANDS_ERR_NOTFOUND;
    case HANDS_E_ARGS: return HANDS_ERR_ARGS;
    case HANDS_E_BUSY: return HANDS_ERR_BUSY;
    default: return HANDS_ERR_FAILED;
    }
}

static Caller *caller_of(const char *name);
static void serve(DConn *c, const DMsg *m) {
    HandsReq r;
    if (handsbus_parse(m, &r)) {
        int known = 0;
        for (size_t i = 0; i < sizeof METHODS / sizeof METHODS[0]; i++) if (!strcmp(m->member, METHODS[i].member)) known = 1;
        dbus_error(c, m, known ? HANDS_ERR_ARGS : "org.freedesktop.DBus.Error.UnknownMethod", known ? "arguments" : "unknown method");
        return;
    }
    DBuf b;
    dw_init(&b);
    if (r.call == HANDS_C_HELLO) {
        Caller *k = caller_of(m->sender);
        int rc = hands_hello(m->sender, r.text, k ? k->exe : "");
        if (rc == HANDS_OK || rc == HANDS_E_ASKING) {
            dw_str(&b, rc == HANDS_OK ? "granted" : "asking");
            dbus_reply(c, m, "s", &b);
        } else dbus_error(c, m, err_name(rc), "hello");
        dw_free(&b);
        return;
    }
    char *out = NULL;
    uint8_t *png = NULL;
    size_t plen = 0;
    int why = 0;
    int rc = hands_call(m->sender, &r, &out, &png, &plen, &why);
    if (rc) dbus_error(c, m, err_name(rc), rc == HANDS_E_REFUSED ? hands_why_word(why) : hands_call_word(r.call));
    else if (r.call == HANDS_C_SHOT) {
        dw_bytes(&b, png, plen);
        dw_str(&b, out ? out : "{}");
        dbus_reply(c, m, "ays", &b);
    } else if (r.call == HANDS_C_SCROLL || r.call == HANDS_C_KEY || r.call == HANDS_C_TYPE || r.call == HANDS_C_SETVALUE) {
        dbus_reply(c, m, NULL, NULL);
    } else {
        dw_str(&b, out ? out : "{}");
        dbus_reply(c, m, "s", &b);
    }
    dw_free(&b);
    free(out);
    free(png);
}

static Caller *caller_of(const char *name) {
    for (int i = 0; i < HANDSBUS_CALLERS; i++) if (callers[i].st && !strcmp(callers[i].name, name)) return &callers[i];
    return NULL;
}

static void caller_drop(Caller *k) {
    if (k->has_pend) dbus_msg_free(&k->pend);
    if (k->st && k->pidfd >= 0) close(k->pidfd);
    memset(k, 0, sizeof *k);
    k->pidfd = -1;
}

void handsbus_call(DConn *c, const DMsg *m) {
    /* only a call addressed to org.lordos.Hands1 itself: never one routed through another name this connection owns */
    if (!m->dest || strcmp(m->dest, HANDS_BUS_NAME)) { dbus_error(c, m, "org.freedesktop.DBus.Error.UnknownObject", "no such object"); return; }
    if (m->member && !strcmp(m->member, "Introspect") && m->iface && !strcmp(m->iface, "org.freedesktop.DBus.Introspectable")) {
        DBuf b;
        dw_init(&b);
        dw_str(&b, introspect);
        dbus_reply(c, m, "s", &b);
        dw_free(&b);
        return;
    }
    if (!m->member || (m->iface && strcmp(m->iface, HANDS_BUS_IFACE))) { dbus_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method"); return; }
    if (!m->sender || m->sender[0] != ':' || strlen(m->sender) >= sizeof callers[0].name || (m->flags & DBUS_NO_REPLY)) {
        dbus_error(c, m, HANDS_ERR_ARGS, "caller");
        return;
    }
    Caller *k = caller_of(m->sender);
    /* the process that opened the connection still lives (its pidfd): when it exits, the grant ends with it */
    if (k && k->st == 2 && k->pidfd >= 0 && syscall(SYS_pidfd_send_signal, k->pidfd, 0, NULL, 0)) { k->st = 3; k->why = "program gone"; }
    if (k && k->st == 2) { serve(c, m); return; }
    if (k && k->st == 3) { dbus_error(c, m, HANDS_ERR_ACCESS, k->why ? k->why : "refused"); return; }
    if (k) { dbus_error(c, m, HANDS_ERR_BUSY, "one call at a time"); return; }   /* still asking the bus */
    for (int i = 0; i < HANDSBUS_CALLERS && !k; i++) if (!callers[i].st) k = &callers[i];
    if (!k) { dbus_error(c, m, HANDS_ERR_BUSY, "too many callers"); return; }
    /* the caller's uid and process, from the bus (GetConnectionCredentials: UnixUserID, ProcessFD), before its first call */
    memset(k, 0, sizeof *k);
    k->pidfd = -1;
    uint32_t serial = ask_bus(c, "GetConnectionCredentials", m->sender);
    if (!serial || dbus_msg_parse(m->raw, m->raw_len, &k->pend)) { dbus_error(c, m, HANDS_ERR_FAILED, "no answer from the bus"); return; }
    k->has_pend = 1;
    k->st = 1;
    k->ask = serial;
    snprintf(k->name, sizeof k->name, "%s", m->sender);
}

/* The program path of the process a pidfd holds, for the prompt only: reported, never an identity (a program of the
 * same user can be anything: docs/HANDS.md "Threat model"). 0 with the path in out, -1 unknown. */
int handsbus_exe_of_pidfd(int pidfd, char *out, size_t n) {
    out[0] = 0;
    if (pidfd < 0 || n < 2) return -1;
    char p[64], line[256];
    snprintf(p, sizeof p, "/proc/self/fdinfo/%d", pidfd);
    FILE *f = fopen(p, "re");
    long pid = 0;
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "Pid:", 4)) pid = strtol(line + 4, NULL, 10);
    if (f) fclose(f);
    if (pid <= 0 || pid > INT_MAX) return -1;
    char link[64], exe[PATH_MAX];
    snprintf(link, sizeof link, "/proc/%ld/exe", pid);
    ssize_t l = readlink(link, exe, sizeof exe - 1);
    if (l <= 0 || syscall(SYS_pidfd_send_signal, pidfd, 0, NULL, 0)) return -1;
    exe[l] = 0;
    if (exe[0] != '/' || (size_t)l >= n || !hands_text_ok(exe, (size_t)l, n - 1, 0)) return -1;
    snprintf(out, n, "%s", exe);
    return 0;
}

static uint32_t ask_bus(DConn *c, const char *member, const char *sender) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, sender);
    DHdr h = { .type = DBUS_CALL, .path = "/org/freedesktop/DBus", .iface = "org.freedesktop.DBus",
               .member = member, .dest = "org.freedesktop.DBus", .sig = "s" };
    uint32_t serial = dbus_send(c, &h, &b);
    dw_free(&b);
    return serial;
}

/* A sandboxed caller (a Flatpak app, or the D-Bus proxy its sandbox talks through): its root holds /.flatpak-info, its
 * cgroup is an app-flatpak-* scope, or it is xdg-dbus-proxy. Such a caller never reaches Hands, whatever it calls itself
 * (as fpspawn.c tells a Flatpak caller). 1 sandboxed or unknowable, 0 not. */
int handsbus_sandboxed(long pid) {
    if (pid <= 0 || pid > INT_MAX) return 1;
    char p[96], buf[4096];
    snprintf(p, sizeof p, "/proc/%ld/root/.flatpak-info", pid);
    if (!access(p, F_OK)) return 1;
    snprintf(p, sizeof p, "/proc/%ld/cgroup", pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 1;
    buf[n] = 0;
    if (strstr(buf, "app-flatpak-") || strstr(buf, "flatpak-session-helper")) return 1;
    char exe[PATH_MAX];
    snprintf(p, sizeof p, "/proc/%ld/exe", pid);
    ssize_t l = readlink(p, exe, sizeof exe - 1);
    if (l > 0) {
        exe[l] = 0;
        const char *b = strrchr(exe, '/');
        if (b && !strncmp(b + 1, "xdg-dbus-proxy", 14)) return 1;
    }
    return 0;
}

static long pid_of_pidfd(int pidfd) {
    if (pidfd < 0) return 0;
    char p[64], line[256];
    snprintf(p, sizeof p, "/proc/self/fdinfo/%d", pidfd);
    FILE *f = fopen(p, "re");
    long pid = 0;
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "Pid:", 4)) pid = strtol(line + 4, NULL, 10);
    if (f) fclose(f);
    return pid;
}

/* GetConnectionCredentials' a{sv}: UnixUserID (u), ProcessID (u), ProcessFD (h: dup'd into *pidfd), the rest skipped */
static void credentials(const DMsg *m, uint32_t *uid, int *pidfd, uint32_t *pid) {
    *uid = (uint32_t)-1;
    *pidfd = -1;
    *pid = 0;
    if (m->type != DBUS_RETURN || !m->sig || strcmp(m->sig, "a{sv}")) return;
    DRd r;
    dr_init(&r, m);
    size_t end = dr_arr(&r, 8);
    while (!r.err && dr_more(&r, end)) {
        dr_struct(&r);
        const char *key = dr_str(&r, NULL), *vs = dr_variant(&r);
        if (r.err || !key || !vs) break;
        if (!strcmp(key, "UnixUserID") && !strcmp(vs, "u")) *uid = dr_u32(&r);
        else if (!strcmp(key, "ProcessID") && !strcmp(vs, "u")) *pid = dr_u32(&r);
        else if (!strcmp(key, "ProcessFD") && !strcmp(vs, "h")) {
            uint32_t i = dr_u32(&r);
            if (!r.err && i < (uint32_t)m->n_fds && m->fds[i] >= 0 && *pidfd < 0) *pidfd = fcntl(m->fds[i], F_DUPFD_CLOEXEC, 3);
        } else dr_skip(&r, vs);
    }
    if (r.err) { *uid = (uint32_t)-1; if (*pidfd >= 0) close(*pidfd); *pidfd = -1; }
}

int handsbus_reply(DConn *c, const DMsg *m) {
    for (int i = 0; i < HANDSBUS_CALLERS; i++) {
        Caller *k = &callers[i];
        if (k->st != 1 || k->ask != m->reply_serial) continue;
        uint32_t uid, pid;
        int pidfd;
        credentials(m, &uid, &pidfd, &pid);
        k->pidfd = pidfd;
        long who = pidfd >= 0 ? pid_of_pidfd(pidfd) : (long)pid;
        if (uid != (uint32_t)getuid()) { k->st = 3; k->why = "another user"; }
        else if (handsbus_sandboxed(who)) { k->st = 3; k->why = "sandboxed"; }
        else {
            handsbus_exe_of_pidfd(pidfd, k->exe, sizeof k->exe);   /* shown as reported; a bus without ProcessFD: unknown */
            k->st = 2;
        }
        if (k->has_pend) {
            if (k->st == 2) serve(c, &k->pend);
            else dbus_error(c, &k->pend, HANDS_ERR_ACCESS, k->why ? k->why : "refused");
            dbus_msg_free(&k->pend);
            k->has_pend = 0;
        }
        return 1;
    }
    return 0;
}

void handsbus_gone(const char *name) {
    Caller *k = caller_of(name);
    if (k) caller_drop(k);
    hands_gone(name);
}

void handsbus_lost(void) {
    for (int i = 0; i < HANDSBUS_CALLERS; i++) caller_drop(&callers[i]);
    hands_lost();
}
