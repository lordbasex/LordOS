/* perf.c — the performance profile in a Linux guest (perf.h says what and why). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "perf.h"
#include "dbusmin.h"
#include "../../server/src/vm/vmagent.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYSTEM_BUS "/run/dbus/system_bus_socket"
#define CALL_MS 3000
#define PORTAL_MS 5000                  /* the portal may be started for the call */
#define ANIM_KEY "/org/gnome/desktop/interface/enable-animations"

static const char *sys_bus, *ses_bus;
static struct { int active; unsigned done; char power[16]; int anim; } ps = { 0, 0, "", -1 };

void perf_buses(const char *system_bus, const char *session_bus) { sys_bus = system_bus; ses_bus = session_bus; }
int perf_active(void) { return ps.active; }

static void say(const char *what) { fprintf(stderr, "lordos-guest: performance profile: %s\n", what); }

/* ---- the record ---- */

static int profile_ok(const char *p) { return !strcmp(p, "power-saver") || !strcmp(p, "balanced") || !strcmp(p, "performance"); }

int perf_record_write(char *out, int cap, const char *power, int anim) {
    if (!power || (power[0] && (!profile_ok(power) || !strcmp(power, "performance"))) || anim < -1 || anim > 1) return 0;
    int n = snprintf(out, (size_t)cap, "lordos-guest perf 1\npower %s\nanim %s\n", power[0] ? power : "-", anim < 0 ? "-" : anim ? "1" : "0");
    return n > 0 && n < cap ? n : 0;
}

int perf_record_parse(const char *s, int n, char *power, int cap, int *anim) {
    static const char head[] = "lordos-guest perf 1\npower ";
    if (!s || n <= 0 || n > 128 || memchr(s, 0, (size_t)n) || n < (int)sizeof head - 1 || memcmp(s, head, sizeof head - 1)) return -1;
    const char *p = s + sizeof head - 1, *end = s + n, *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) return -1;
    int pl = (int)(nl - p);
    char pw[16];
    if (pl <= 0 || pl >= (int)sizeof pw) return -1;
    memcpy(pw, p, (size_t)pl);
    pw[pl] = 0;
    if (!strcmp(pw, "-")) pw[0] = 0;
    else if (!profile_ok(pw) || !strcmp(pw, "performance")) return -1;
    p = nl + 1;
    if (end - p != 7 || memcmp(p, "anim ", 5) || p[6] != '\n') return -1;
    int a = p[5] == '-' ? -1 : p[5] == '0' ? 0 : p[5] == '1' ? 1 : -2;
    if (a == -2 || (int)strlen(pw) >= cap) return -1;
    strcpy(power, pw);
    *anim = a;
    return 0;
}

/* ~/.local/state/lordos-guest/perf: "" when there's no home */
static void record_path(char *out, size_t cap, int dir_only) {
    const char *h = getenv("HOME");
    out[0] = 0;
    if (!h || h[0] != '/') return;
    if ((size_t)snprintf(out, cap, "%s/.local/state/lordos-guest%s", h, dir_only ? "" : "/perf") >= cap) out[0] = 0;
}

static void record_save(void) {
    char path[512], dir[512], tmp[540], t[128];
    record_path(path, sizeof path, 0);
    if (!path[0]) return;
    if (!ps.power[0] && ps.anim < 0) { unlink(path); return; }      /* nothing of ours left to put back */
    int n = perf_record_write(t, sizeof t, ps.power, ps.anim);
    if (!n) return;
    record_path(dir, sizeof dir, 1);
    /* ~/.local, ~/.local/state, then ours (0700) */
    char *slash = strrchr(dir, '/');
    *slash = 0;
    char *st = strrchr(dir, '/');
    *st = 0; mkdir(dir, 0755); *st = '/';
    mkdir(dir, 0755);
    *slash = '/';
    mkdir(dir, 0700);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { say("its record couldn't be written"); return; }
    int ok = write(fd, t, (size_t)n) == n;
    if (close(fd) || !ok || rename(tmp, path)) { unlink(tmp); say("its record couldn't be written"); }
}

/* ---- the power profile (system bus) ---- */

static const struct { const char *name, *path; } PPD[] = {
    { "org.freedesktop.UPower.PowerProfiles", "/org/freedesktop/UPower/PowerProfiles" },
    { "net.hadess.PowerProfiles", "/net/hadess/PowerProfiles" },
};

/* ActiveProfile: 0 read (and *which the service that answered), -1 none answered */
static int ppd_get(Db *b, int *which, char *out, size_t cap) {
    for (int i = 0; i < 2; i++) {
        uint8_t body[256];
        DbW w;
        DbMsg r;
        dbw_init(&w, body, sizeof body);
        dbw_str(&w, PPD[i].name);
        dbw_str(&w, "ActiveProfile");
        if (w.bad) return -1;
        int c = db_call(b, PPD[i].name, PPD[i].path, "org.freedesktop.DBus.Properties", "Get", "ss", body, w.n, &r, CALL_MS);
        if (c < 0) return -1;
        if (c == 0 && !db_reply_str(&r, out, cap) && profile_ok(out)) { *which = i; return 0; }
    }
    return -1;
}
static int ppd_set(Db *b, int which, const char *profile) {
    uint8_t body[256];
    DbW w;
    DbMsg r;
    dbw_init(&w, body, sizeof body);
    dbw_str(&w, PPD[which].name);
    dbw_str(&w, "ActiveProfile");
    dbw_variant_str(&w, profile);
    if (w.bad) return -1;
    int c = db_call(b, PPD[which].name, PPD[which].path, "org.freedesktop.DBus.Properties", "Set", "ssv", body, w.n, &r, CALL_MS);
    if (c == 1) fprintf(stderr, "lordos-guest: performance profile: the power profile: %s\n", r.err);
    return c ? -1 : 0;
}

/* ---- GNOME's animations (session bus) ---- */

static int gnome(void) {
    const char *d = getenv("XDG_CURRENT_DESKTOP");
    if (!d) return 0;
    for (const char *p = d; (p = strstr(p, "GNOME")); p += 5)
        if ((p == d || p[-1] == ':') && (p[5] == 0 || p[5] == ':')) return 1;
    return 0;
}

static void session_path(char *out, size_t cap) {
    out[0] = 0;
    if (ses_bus) { snprintf(out, cap, "%s", ses_bus); return; }
    const char *a = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (a && !strncmp(a, "unix:path=/", 11)) {
        const char *p = a + 10;
        size_t l = strcspn(p, ",;");
        if (l < cap && !memchr(p, '%', l)) { memcpy(out, p, l); out[l] = 0; return; }   /* no escapes in the path */
    }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0] == '/' && (size_t)snprintf(out, cap, "%s/bus", rt) >= cap) out[0] = 0;
}

static int anim_get(Db *b, int *v) {
    static const char *const m[] = { "ReadOne", "Read" };   /* ReadOne: the portal's version 2; Read: a variant in a variant */
    for (int i = 0; i < 2; i++) {
        uint8_t body[256];
        DbW w;
        DbMsg r;
        dbw_init(&w, body, sizeof body);
        dbw_str(&w, "org.gnome.desktop.interface");
        dbw_str(&w, "enable-animations");
        int c = db_call(b, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings", m[i],
                        "ss", body, w.n, &r, PORTAL_MS);
        if (c < 0) return -1;
        if (c == 0) return db_reply_bool(&r, v);
    }
    return -1;
}
static int anim_set(Db *b, int v) {
    uint8_t cs[256], body[300];
    size_t n = db_dconf_bool(cs, sizeof cs, ANIM_KEY, v);
    DbW w;
    DbMsg r;
    dbw_init(&w, body, sizeof body);
    dbw_bytes(&w, cs, n);
    if (!n || w.bad) return -1;
    int c = db_call(b, "ca.desrt.dconf", "/ca/desrt/dconf/Writer/user", "ca.desrt.dconf.Writer", "Change", "ay", body, w.n, &r, PORTAL_MS);
    if (c == 1) fprintf(stderr, "lordos-guest: performance profile: the animations: %s\n", r.err);
    return c ? -1 : 0;
}

/* ---- on and off ---- */

static Db bus;                          /* one at a time (64 KiB of buffer) */


/* applies the profile. A part a restore couldn't put back (still in ps, from the record) is kept when it is still what
 * this side set: its earlier value is the one to put back later */
static void apply(unsigned *done, unsigned *failed) {
    char cur[16], was_power[16];
    int which, was_anim = ps.anim;
    snprintf(was_power, sizeof was_power, "%s", ps.power);
    ps.power[0] = 0;
    ps.anim = -1;
    if (db_open(&bus, sys_bus ? sys_bus : SYSTEM_BUS, CALL_MS)) { *failed |= AGPF_POWER; say("no system bus: the power profile stays"); }
    else {
        if (ppd_get(&bus, &which, cur, sizeof cur)) { *failed |= AGPF_POWER; say("no power-profiles-daemon: the power profile stays"); }
        else if (!strcmp(cur, "performance")) {
            *done |= AGPF_POWER;
            snprintf(ps.power, sizeof ps.power, "%s", was_power);
            say(was_power[0] ? "the power profile is performance since an earlier run" : "the power profile is performance already");
        } else if (ppd_set(&bus, which, "performance")) { *failed |= AGPF_POWER; say("the performance power profile was refused"); }
        else { snprintf(ps.power, sizeof ps.power, "%s", cur); *done |= AGPF_POWER; fprintf(stderr, "lordos-guest: performance profile: power profile %s -> performance\n", cur); }
        db_close(&bus);
    }
    char sp[256];
    session_path(sp, sizeof sp);
    int v;
    if (!gnome()) { *failed |= AGPF_EFFECTS; say("the desktop isn't GNOME: its animations stay"); }
    else if (!sp[0] || db_open(&bus, sp, CALL_MS)) { *failed |= AGPF_EFFECTS; say("no session bus: the animations stay"); }
    else {
        if (anim_get(&bus, &v)) { *failed |= AGPF_EFFECTS; say("the animations' setting can't be read (no desktop portal): they stay"); }
        else if (!v) { *done |= AGPF_EFFECTS; ps.anim = was_anim; say(was_anim >= 0 ? "the animations are off since an earlier run" : "the animations are off already"); }
        else if (anim_set(&bus, 0)) { *failed |= AGPF_EFFECTS; say("the animations couldn't be turned off (no dconf service)"); }
        else { ps.anim = 1; *done |= AGPF_EFFECTS; say("animations turned off"); }
        db_close(&bus);
    }
    if (*failed & AGPF_POWER) snprintf(ps.power, sizeof ps.power, "%s", was_power);   /* not looked at: still to put back */
    if ((*failed & AGPF_EFFECTS) && ps.anim < 0) ps.anim = was_anim;
    ps.active = 1;
    ps.done = *done;
    record_save();
}

/* puts back what apply changed, each part only while it is still what apply set. A part that couldn't be put back
 * (a bus or a service not there) stays in the record for the next start */
static void restore(unsigned *done, unsigned *failed) {
    char cur[16], sp[256];
    int which, v;
    if (ps.power[0]) {
        if (db_open(&bus, sys_bus ? sys_bus : SYSTEM_BUS, CALL_MS) || ppd_get(&bus, &which, cur, sizeof cur)) *failed |= AGPF_POWER;
        else if (strcmp(cur, "performance")) { ps.power[0] = 0; say("the power profile was changed since: it stays"); }
        else if (ppd_set(&bus, which, ps.power)) *failed |= AGPF_POWER;
        else { *done |= AGPF_POWER; fprintf(stderr, "lordos-guest: performance profile: power profile back to %s\n", ps.power); ps.power[0] = 0; }
        db_close(&bus);
    }
    if (ps.anim >= 0) {
        session_path(sp, sizeof sp);
        if (!sp[0] || db_open(&bus, sp, CALL_MS) || anim_get(&bus, &v)) *failed |= AGPF_EFFECTS;
        else if (v) { ps.anim = -1; say("the animations were turned on since: they stay"); }
        else if (anim_set(&bus, ps.anim)) *failed |= AGPF_EFFECTS;
        else { *done |= AGPF_EFFECTS; ps.anim = -1; say("animations back"); }
        db_close(&bus);
    }
    if (*failed) say("some parts weren't put back: tried again at the next start");
    ps.active = 0;
    ps.done = 0;
    record_save();
}


void perf_set(int on, unsigned *done, unsigned *failed) {
    *done = *failed = 0;
    if (on && ps.active) { *done = ps.done; return; }
    if (on) apply(done, failed);
    else if (ps.active || ps.power[0] || ps.anim >= 0) restore(done, failed);   /* also what an earlier try left */
}

void perf_recover(void) {
    char path[512], t[129], pw[16];
    int anim;
    record_path(path, sizeof path, 0);
    if (!path[0]) return;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);      /* never stuck on a FIFO */
    if (fd < 0) return;
    struct stat st;
    ssize_t n = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() && st.st_size <= 128) n = read(fd, t, sizeof t - 1);
    close(fd);
    if (n <= 0 || perf_record_parse(t, (int)n, pw, sizeof pw, &anim)) { unlink(path); say("an unreadable record was dropped"); return; }
    snprintf(ps.power, sizeof ps.power, "%s", pw);
    ps.anim = anim;
    ps.active = 1;
    say("putting back what an earlier run left on");
    unsigned d, f;
    perf_set(0, &d, &f);
}
