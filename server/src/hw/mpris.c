/* mpris.c — the Linux players on the session bus, for Now Playing and the media keys (see mpris.h). */
#include "mpris.h"
#include "mprismeta.h"
#include "jbuf.h"
#include "../vault/dbus.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BUS          "org.freedesktop.DBus"
#define BUS_PATH     "/org/freedesktop/DBus"
#define MP_PATH      "/org/mpris/MediaPlayer2"
#define ROOT_IFACE   "org.mpris.MediaPlayer2"
#define PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
#define PROPS_IFACE  "org.freedesktop.DBus.Properties"
#define NO_TRACK     "/org/mpris/MediaPlayer2/TrackList/NoTrack"

typedef struct {
    uint32_t id;                         /* 0 = a free slot */
    char name[256], owner[256];          /* owner: its unique name, "" until GetNameOwner answers */
    MprisProps p;
    double pos_at;                       /* CLOCK_MONOTONIC ms when p.position_us was true */
} Player;

enum { Q_LIST, Q_OWNER, Q_ROOT, Q_PLAYER, Q_POS };
typedef struct { uint32_t serial, player; int kind; double since; } Pending;

static DConn conn = { .fd = -1 };
static int up, dirty, full_told;
static MprisEvent emit;
static Player players[MPRIS_MAX_PLAYERS];
static Pending pending[MPRIS_MAX_PENDING];
static uint32_t next_id = 1;

static double mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int mpris_fd(void) { return up ? conn.fd : -1; }
int mpris_wants_write(void) { return up && dbus_pending_out(&conn); }

static Player *by_id(uint32_t id) {
    for (int i = 0; id && i < MPRIS_MAX_PLAYERS; i++) if (players[i].id == id) return &players[i];
    return NULL;
}

static Player *by_name(const char *name) {
    for (int i = 0; i < MPRIS_MAX_PLAYERS; i++) if (players[i].id && !strcmp(players[i].name, name)) return &players[i];
    return NULL;
}

/* Where it is now: a playing track moves on from the last position it told. */
static int64_t estimate(const Player *pl, double now) {
    int64_t us = pl->p.position_us;
    if (pl->p.status == MPRIS_PLAYING && now > pl->pos_at) us += (int64_t)((now - pl->pos_at) * 1000);
    if (pl->p.length_us > 0 && us > pl->p.length_us) us = pl->p.length_us;
    return mpris_clamp_us(us);
}

/* Before a change of state: the estimate becomes the position, so pausing keeps the right place. */
static void settle(Player *pl, double now) {
    pl->p.position_us = estimate(pl, now);
    pl->pos_at = now;
}

/* An asynchronous call whose reply goes to on_reply as kind; 0 if it could not be sent (no free slot). */
static uint32_t call(const char *dest, const char *path, const char *iface, const char *member, const char *sig,
                     const DBuf *body, int kind, uint32_t player) {
    int slot = -1;
    for (int i = 0; i < MPRIS_MAX_PENDING && slot < 0; i++) if (!pending[i].serial) slot = i;
    if (slot < 0) return 0;
    DHdr h = { .type = DBUS_CALL, .path = path, .iface = iface, .member = member, .dest = dest, .sig = sig };
    uint32_t s = dbus_send(&conn, &h, body);
    if (s) pending[slot] = (Pending){ s, player, kind, mono_ms() };
    return s;
}

/* A call nobody answers (controls, AddMatch): the player is never waited for. */
static void tell(const char *dest, const char *path, const char *iface, const char *member, const char *sig, const DBuf *body) {
    DHdr h = { .type = DBUS_CALL, .flags = DBUS_NO_REPLY, .path = path, .iface = iface, .member = member, .dest = dest, .sig = sig };
    dbus_send(&conn, &h, body);
}

static void get_all(Player *pl, const char *iface, int kind) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, iface);
    call(pl->owner, MP_PATH, PROPS_IFACE, "GetAll", "s", &b, kind, pl->id);
    dw_free(&b);
}

static void get_position(Player *pl) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, PLAYER_IFACE);
    dw_str(&b, "Position");
    call(pl->owner, MP_PATH, PROPS_IFACE, "Get", "ss", &b, Q_POS, pl->id);
    dw_free(&b);
}

static void fetch(Player *pl) {
    get_all(pl, ROOT_IFACE, Q_ROOT);
    get_all(pl, PLAYER_IFACE, Q_PLAYER);
}

static void ask_owner(Player *pl) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, pl->name);
    call(BUS, BUS_PATH, BUS, "GetNameOwner", "s", &b, Q_OWNER, pl->id);
    dw_free(&b);
}

static int owner_ok(const char *o) { return o && o[0] == ':' && strlen(o) < sizeof players[0].owner; }

/* A player's name appeared (owner given, or "" to ask the bus) or changed hands. */
static void add_player(const char *name, const char *owner) {
    if (!mpris_name_ok(name) || (owner[0] && !owner_ok(owner))) return;
    Player *pl = by_name(name);
    if (pl && !strcmp(pl->owner, owner)) return;
    if (!pl) {
        for (int i = 0; i < MPRIS_MAX_PLAYERS && !pl; i++) if (!players[i].id) pl = &players[i];
        if (!pl) {
            if (!full_told++) fprintf(stderr, "mpris: more than %d players; %s is not followed\n", MPRIS_MAX_PLAYERS, name);
            return;
        }
        pl->id = next_id++;
        if (!next_id) next_id = 1;
        snprintf(pl->name, sizeof pl->name, "%s", name);
    }
    mpris_props_init(&pl->p);
    pl->pos_at = mono_ms();
    snprintf(pl->owner, sizeof pl->owner, "%s", owner);
    dirty = 1;
    if (owner[0]) fetch(pl);
    else ask_owner(pl);
}

static void remove_player(const char *name) {
    Player *pl = by_name(name);
    if (!pl) return;
    memset(pl, 0, sizeof *pl);
    dirty = 1;
}

/* What a dictionary of properties did to a player: the position settles before the state changes. */
static void apply_props(Player *pl, DRd *r) {
    double now = mono_ms();
    settle(pl, now);
    int got = mpris_read_props(r, &pl->p);
    if (got <= 0) return;
    if (got & MP_POS) pl->pos_at = now;
    /* a new state or track: Position isn't signalled, so it is asked */
    if ((got & (MP_STATUS | MP_META)) && !(got & MP_POS)) get_position(pl);
    dirty = 1;
}

static void on_reply(const DMsg *m) {
    int i = 0;
    while (i < MPRIS_MAX_PENDING && (!pending[i].serial || pending[i].serial != m->reply_serial)) i++;
    if (i == MPRIS_MAX_PENDING) return;                 /* forgotten (timed out) or never ours */
    Pending q = pending[i];
    pending[i].serial = 0;
    Player *pl = by_id(q.player);
    /* only the bus answers for names, and only the owner the call went to for its player */
    if (q.kind == Q_LIST || q.kind == Q_OWNER) { if (!m->sender || strcmp(m->sender, BUS)) return; }
    else if (!pl || !pl->owner[0] || !m->sender || strcmp(m->sender, pl->owner)) return;
    if (q.kind == Q_OWNER && pl && m->type == DBUS_ERROR) { remove_player(pl->name); return; }
    if (m->type != DBUS_RETURN || !m->sig) return;
    DRd r;
    dr_init(&r, m);
    switch (q.kind) {
    case Q_LIST: {
        if (strcmp(m->sig, "as")) return;
        size_t end = dr_arr(&r, 4);
        while (!r.err && dr_more(&r, end)) {
            const char *n = dr_str(&r, NULL);
            if (!r.err && mpris_name_ok(n) && !by_name(n)) add_player(n, "");
        }
        break;
    }
    case Q_OWNER: {
        if (!pl || strcmp(m->sig, "s")) return;
        const char *o = dr_str(&r, NULL);
        if (r.err || !owner_ok(o)) { remove_player(pl->name); return; }
        snprintf(pl->owner, sizeof pl->owner, "%s", o);
        fetch(pl);
        break;
    }
    case Q_ROOT: case Q_PLAYER:
        if (!strcmp(m->sig, "a{sv}")) apply_props(pl, &r);
        break;
    case Q_POS: {
        int64_t us;
        if (!strcmp(m->sig, "v") && !mpris_read_position(&r, &us)) { pl->p.position_us = us; pl->pos_at = mono_ms(); dirty = 1; }
        break;
    }
    }
}

static void on_signal(const DMsg *m) {
    if (!m->member || !m->sender || !m->sig || !m->iface) return;
    if (!strcmp(m->member, "NameOwnerChanged")) {
        if (strcmp(m->sender, BUS) || strcmp(m->iface, BUS) || strcmp(m->sig, "sss")) return;
        DRd r;
        dr_init(&r, m);
        const char *name = dr_str(&r, NULL), *old = dr_str(&r, NULL), *now = dr_str(&r, NULL);
        (void)old;
        if (r.err || !mpris_name_ok(name)) return;
        if (!now[0]) remove_player(name);
        else add_player(name, now);
        return;
    }
    if (!m->path || strcmp(m->path, MP_PATH)) return;
    int props = !strcmp(m->member, "PropertiesChanged") && !strcmp(m->iface, PROPS_IFACE) && !strcmp(m->sig, "sa{sv}as");
    int seeked = !strcmp(m->member, "Seeked") && !strcmp(m->iface, PLAYER_IFACE) && !strcmp(m->sig, "x");
    if (!props && !seeked) return;
    /* the same player may own two names (its own and an .instance one): each follows it */
    for (int i = 0; i < MPRIS_MAX_PLAYERS; i++) {
        Player *pl = &players[i];
        if (!pl->id || !pl->owner[0] || strcmp(pl->owner, m->sender)) continue;
        DRd r;
        dr_init(&r, m);
        if (seeked) {
            int64_t us = (int64_t)dr_u64(&r);
            if (!r.err) { pl->p.position_us = mpris_clamp_us(us); pl->pos_at = mono_ms(); dirty = 1; }
            continue;
        }
        const char *iface = dr_str(&r, NULL);
        if (r.err || (strcmp(iface, ROOT_IFACE) && strcmp(iface, PLAYER_IFACE))) continue;
        apply_props(pl, &r);
        /* properties it says changed without their value: read them all again */
        size_t end = dr_arr(&r, 4);
        if (!r.err && dr_more(&r, end)) get_all(pl, iface, !strcmp(iface, ROOT_IFACE) ? Q_ROOT : Q_PLAYER);
    }
}

static void add_match(const char *rule) {
    DBuf b;
    dw_init(&b);
    dw_str(&b, rule);
    tell(BUS, BUS_PATH, BUS, "AddMatch", "s", &b);
    dw_free(&b);
}

int mpris_start(MprisEvent fn) {
    emit = fn;
    int fd = dbus_connect_session();
    if (fd < 0 || dbus_open(&conn, fd, 2000)) { if (fd >= 0) dbus_close(&conn); return -1; }
    up = 1;
    add_match("type='signal',sender='" BUS "',interface='" BUS "',member='NameOwnerChanged',arg0namespace='org.mpris.MediaPlayer2'");
    add_match("type='signal',interface='" PROPS_IFACE "',member='PropertiesChanged',path='" MP_PATH "'");
    add_match("type='signal',interface='" PLAYER_IFACE "',member='Seeked',path='" MP_PATH "'");
    call(BUS, BUS_PATH, BUS, "ListNames", NULL, NULL, Q_LIST, 0);
    dbus_flush(&conn);
    return 0;
}

static void lost(void) {
    fprintf(stderr, "mpris: lost the session bus\n");
    dbus_close(&conn);
    up = 0;
    memset(players, 0, sizeof players);
    memset(pending, 0, sizeof pending);
    dirty = 1;
}

void mpris_dispatch(void) {
    if (!up) return;
    DMsg m;
    int r;
    while ((r = dbus_next(&conn, &m)) == 1) {
        if ((m.type == DBUS_RETURN || m.type == DBUS_ERROR) && m.reply_serial) on_reply(&m);
        else if (m.type == DBUS_SIGNAL) on_signal(&m);
        else if (m.type == DBUS_CALL && !(m.flags & DBUS_NO_REPLY))
            dbus_error(&conn, &m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
        dbus_msg_free(&m);
    }
    if (r < 0 || dbus_flush(&conn) < 0) lost();
}

void mpris_tick(void) {
    double now = mono_ms();
    for (int i = 0; i < MPRIS_MAX_PENDING; i++)
        if (pending[i].serial && now - pending[i].since >= MPRIS_CALL_MS) pending[i].serial = 0;   /* never answered */
    if (up && dbus_pending_out(&conn) && dbus_flush(&conn) < 0) lost();
    if (!dirty || !emit) return;
    dirty = 0;
    char *j = mpris_json();
    if (j) { emit("mpris", j); free(j); }
}

static const char *status_name(int s) { return s == MPRIS_PLAYING ? "playing" : s == MPRIS_PAUSED ? "paused" : "stopped"; }

char *mpris_json(void) {
    HBuf b = { 0 };
    double now = mono_ms();
    hb_printf(&b, "[");
    int first = 1;
    for (int i = 0; i < MPRIS_MAX_PLAYERS; i++) {
        const Player *pl = &players[i];
        if (!pl->id || !pl->owner[0]) continue;
        int dup = 0;                                     /* one entry per player, not per name */
        for (int k = 0; k < i && !dup; k++) dup = players[k].id && !strcmp(players[k].owner, pl->owner);
        if (dup) continue;
        const MprisProps *p = &pl->p;
        int c = p->can_control;
        hb_printf(&b, "%s{\"name\":", first ? "" : ",");
        hb_str(&b, pl->name);
        hb_printf(&b, ",\"identity\":");
        hb_str(&b, p->identity);
        hb_printf(&b, ",\"status\":\"%s\",\"title\":", status_name(p->status));
        hb_str(&b, p->title);
        hb_printf(&b, ",\"artist\":");
        hb_str(&b, p->artist);
        hb_printf(&b, ",\"album\":");
        hb_str(&b, p->album);
        hb_printf(&b, ",\"art\":");
        hb_str(&b, p->art);
        hb_printf(&b, ",\"length\":%lld,\"pos\":%lld,\"canPlay\":%s,\"canPause\":%s,\"canNext\":%s,\"canPrev\":%s,\"canSeek\":%s}",
                  (long long)(p->length_us / 1000), (long long)(estimate(pl, now) / 1000),
                  c && p->can_play ? "true" : "false", c && p->can_pause ? "true" : "false", c && p->can_next ? "true" : "false",
                  c && p->can_prev ? "true" : "false", c && p->can_seek && p->length_us > 0 ? "true" : "false");
        first = 0;
    }
    hb_printf(&b, "]");
    return hb_take(&b);
}

int mpris_control(const char *name, int cmd, double ms) {
    Player *pl = up && name ? by_name(name) : NULL;
    if (!pl || !pl->owner[0] || !pl->p.can_control) return -1;
    const MprisProps *p = &pl->p;
    const char *member = NULL;
    switch (cmd) {
    case MPRIS_TOGGLE: member = p->can_pause ? "PlayPause" : p->can_play && p->status != MPRIS_PLAYING ? "Play" : NULL; break;
    case MPRIS_PLAY: member = p->can_play ? "Play" : NULL; break;
    case MPRIS_PAUSE: member = p->can_pause ? "Pause" : NULL; break;
    case MPRIS_NEXT: member = p->can_next ? "Next" : NULL; break;
    case MPRIS_PREV: member = p->can_prev ? "Previous" : NULL; break;
    case MPRIS_SEEK: {
        if (!p->can_seek || p->length_us <= 0 || !isfinite(ms)) return -1;
        double now = mono_ms();
        int64_t to = ms <= 0 ? 0 : ms * 1000 >= (double)p->length_us ? p->length_us : (int64_t)(ms * 1000);
        DBuf b;
        dw_init(&b);
        if (p->trackid[0] && strcmp(p->trackid, NO_TRACK)) {
            dw_str(&b, p->trackid);
            dw_u64(&b, (uint64_t)to);
            tell(pl->owner, MP_PATH, PLAYER_IFACE, "SetPosition", "ox", &b);
        } else {                                         /* no track id: a relative Seek from where it is */
            dw_u64(&b, (uint64_t)(to - estimate(pl, now)));
            tell(pl->owner, MP_PATH, PLAYER_IFACE, "Seek", "x", &b);
        }
        dw_free(&b);
        pl->p.position_us = to;
        pl->pos_at = now;
        dirty = 1;
        get_position(pl);
        dbus_flush(&conn);
        return 0;
    }
    default: return -1;
    }
    if (!member) return -1;
    tell(pl->owner, MP_PATH, PLAYER_IFACE, member, NULL, NULL);
    dbus_flush(&conn);
    return 0;
}

void mpris_refresh(void) {
    if (!up) return;
    for (int i = 0; i < MPRIS_MAX_PLAYERS; i++) if (players[i].id && players[i].owner[0]) get_all(&players[i], PLAYER_IFACE, Q_PLAYER);
    dbus_flush(&conn);
}
