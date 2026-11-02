/* display.c — Settings → Displays, lord-server side (see display.h, docs/DISPLAYS.md). */
#include "display.h"
#include "edid.h"
#include "jbuf.h"
#include "modes.h"
#include "night.h"
#include "../sec/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_SAVED 16
#define ID_MAX 96

/* One display's configuration, by identity. x, y: top-left corner relative to the main display's
 * (logical px), valid when placed. scale in percent. */
typedef struct { char id[ID_MAX]; int w, h, refresh, rot, scale, x, y, placed; } Cfg;

typedef struct {
    Cfg c[MAX_SAVED];
    int n, mirror;
    char main_id[ID_MAX];
} Conf;

static struct {
    const DisplayHost *host;
    char path[PATH_MAX];                /* ~/.config/lordos/displays.json */
    Conf saved, live, undo;             /* the file, what is shown now, before the last change */
    int capture_all;
    int dirty, retry, save_pending;
    DisplayPlan cur;                    /* what main.c applied */
    /* Night Light */
    int kelvin, soft;
    uint8_t lut[3][256];
    /* idle */
    int off_s, lock_s, sleep_s;
    double last;
    int done_off, done_lock, done_sleep, off;
} D = { .kelvin = NIGHT_K_MAX };

static void apply_tint_all(void);
static int save_now(void);

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static Kms *kms(void) { return D.host && D.host->kms ? D.host->kms() : NULL; }
static void event(const char *kind, const char *json) { if (D.host && D.host->event) D.host->event(kind, json); }
static int heads(Kms **out, int max) {
    if (D.host && D.host->heads) return D.host->heads(out, max);
    Kms *k = kms();
    if (!k || max < 1) return 0;
    out[0] = k;
    return 1;
}
static void notify_list(void) {
    char *j = display_list_json();
    if (j) { event("display", j); free(j); }
}

/* ---- identity ---- */

static void output_identity(const KmsOutput *o, char *id, size_t n, char *name, size_t nn) {
    Edid e;
    if (o->edid && edid_parse(o->edid, o->edid_len, &e) == 0) {
        edid_identity(&e, id, n);
        if (name) edid_display_name(&e, name, nn);
        return;
    }
    snprintf(id, n, "%s-%u", kms_connector_type_name(o->type), o->type_id);
    if (name) snprintf(name, nn, "%s", id);
}

/* Identity of outs[i] among the connected ones: two monitors with the same EDID identity (no
 * serial, or two virtual heads) get the connector appended, so each keeps its own place. */
static void unique_identity(const KmsOutput *o, int i, char *id, size_t len, char *name, size_t nn) {
    output_identity(&o[i], id, len, name, nn);
    for (int j = 0; j < i; j++) {
        if (!o[j].connected || !o[j].nmodes) continue;
        char other[ID_MAX];
        output_identity(&o[j], other, sizeof other, NULL, 0);
        if (strcmp(other, id)) continue;
        size_t l = strlen(id);
        snprintf(id + l, len - l, "@%s-%u", kms_connector_type_name(o[i].type), o[i].type_id);
        return;
    }
}

static Cfg *cfg_for(Conf *c, const char *id) {
    for (int i = 0; i < c->n; i++) if (!strcmp(c->c[i].id, id)) return &c->c[i];
    return NULL;
}

static Cfg *cfg_make(Conf *c, const char *id) {
    Cfg *f = cfg_for(c, id);
    if (f) return f;
    if (c->n < MAX_SAVED) f = &c->c[c->n++];
    else { memmove(c->c, c->c + 1, sizeof c->c[0] * (MAX_SAVED - 1)); f = &c->c[MAX_SAVED - 1]; }
    memset(f, 0, sizeof *f);
    snprintf(f->id, sizeof f->id, "%s", id);
    return f;
}

/* ---- displays.json ---- */

static void load(void) {
    memset(&D.saved, 0, sizeof D.saved);
    int fd = open(D.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { D.live = D.saved; return; }
    char buf[16384];
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    JDoc doc;
    if (r <= 0 || json_parse(buf, (size_t)r, &doc, NULL, 0)) {
        if (r > 0) fprintf(stderr, "displays: invalid displays.json\n");
        D.live = D.saved;
        return;
    }
    const char *m = json_str(doc.root, "main");
    if (m && strlen(m) < ID_MAX) snprintf(D.saved.main_id, sizeof D.saved.main_id, "%s", m);
    const JVal *mv = json_get(doc.root, "mirror");
    D.saved.mirror = mv && mv->type == J_BOOL && mv->num != 0;
    const JVal *ca = json_get(doc.root, "captureAll");
    D.capture_all = ca && ca->type == J_BOOL && ca->num != 0;
    const JVal *ds = json_get(doc.root, "displays");
    for (const JVal *v = ds && ds->type == J_OBJ ? ds->kid : NULL; v && D.saved.n < MAX_SAVED; v = v->next) {
        if (v->type != J_OBJ || !v->key || strlen(v->key) >= ID_MAX) continue;
        Cfg *s = &D.saved.c[D.saved.n++];
        memset(s, 0, sizeof *s);
        snprintf(s->id, sizeof s->id, "%s", v->key);
        s->w = (int)json_num(v, "w", 0);
        s->h = (int)json_num(v, "h", 0);
        s->refresh = (int)json_num(v, "refresh", 0);
        s->rot = (int)json_num(v, "rotation", 0) & 3;
        int sc = (int)json_num(v, "scale", 0);
        if (sc == 1 || sc == 2) sc *= 100;                  /* files from before fractional scales */
        s->scale = out_scale_valid(sc) ? sc : 0;            /* 0 = the recommended one */
        const JVal *px = json_get(v, "x"), *py = json_get(v, "y");
        if (px && py && px->type == J_NUM && py->type == J_NUM) {
            s->x = (int)json_num(v, "x", 0);
            s->y = (int)json_num(v, "y", 0);
            s->placed = s->x > -65536 && s->x < 65536 && s->y > -65536 && s->y < 65536;
        }
        if (s->w < 0 || s->w > 16384 || s->h < 0 || s->h > 16384) s->w = s->h = 0;
    }
    json_free(&doc);
    D.live = D.saved;
}

static int store(void) {
    if (!D.path[0]) return -1;
    HBuf b = { 0 };
    hb_printf(&b, "{\n  \"main\": ");
    hb_str(&b, D.saved.main_id);
    hb_printf(&b, ",\n  \"mirror\": %s,\n  \"captureAll\": %s,\n  \"displays\": {", D.saved.mirror ? "true" : "false", D.capture_all ? "true" : "false");
    for (int i = 0; i < D.saved.n; i++) {
        const Cfg *s = &D.saved.c[i];
        hb_printf(&b, "%s\n    ", i ? "," : "");
        hb_str(&b, s->id);
        hb_printf(&b, ": { \"w\": %d, \"h\": %d, \"refresh\": %d, \"rotation\": %d, \"scale\": %d", s->w, s->h, s->refresh, s->rot, s->scale);
        if (s->placed) hb_printf(&b, ", \"x\": %d, \"y\": %d", s->x, s->y);
        hb_printf(&b, " }");
    }
    hb_printf(&b, "\n  }\n}\n");
    char *txt = hb_take(&b);
    if (!txt) return -1;
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", D.path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    int ok = fd >= 0 && write(fd, txt, strlen(txt)) == (ssize_t)strlen(txt);
    if (fd >= 0) { ok = fsync(fd) == 0 && ok; close(fd); }
    free(txt);
    if (!ok || rename(tmp, D.path)) { unlink(tmp); return -1; }
    return 0;
}

void display_init(const DisplayHost *h) {
    D.host = h;
    const char *home = getenv("HOME");
    if (home && home[0] == '/') {
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s/.config", home);
        mkdir(dir, 0700);
        snprintf(dir, sizeof dir, "%s/.config/lordos", home);
        mkdir(dir, 0700);
        snprintf(D.path, sizeof D.path, "%s/.config/lordos/displays.json", home);
        load();
    }
    /* lord-server started on its own card as root (development): the picker ran before $HOME was
     * the user's, so the saved configuration is applied now (and the other displays are opened) */
    D.dirty = 1;
}

/* ---- the plan ---- */

static int connected_count(const KmsOutput *o, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) if (o[i].connected && o[i].nmodes) c++;
    return c;
}

int display_plan(const KmsOutput *o, int n, DisplayPlan *p) {
    memset(p, 0, sizeof *p);
    const Conf *c = &D.live;
    int idx[OUT_MAX], m = 0, main_i = -1;
    char ids[KMS_MAX_OUTPUTS][ID_MAX], names[KMS_MAX_OUTPUTS][64];
    for (int i = 0; i < n && i < KMS_MAX_OUTPUTS; i++) {
        if (!o[i].connected || !o[i].nmodes) continue;
        unique_identity(o, i, ids[i], ID_MAX, names[i], sizeof names[i]);
        if (main_i < 0 && c->main_id[0] && !strcmp(ids[i], c->main_id)) main_i = i;
    }
    for (int i = 0; i < n && i < KMS_MAX_OUTPUTS && main_i < 0; i++) if (o[i].connected && o[i].nmodes) main_i = i;
    if (main_i < 0) return 0;
    idx[m++] = main_i;                                      /* the main display first */
    for (int i = 0; i < n && i < KMS_MAX_OUTPUTS && m < OUT_MAX; i++)
        if (i != main_i && o[i].connected && o[i].nmodes) idx[m++] = i;
    ORect r[OUT_MAX];
    int placed[OUT_MAX];
    for (int j = 0; j < m; j++) {
        const KmsOutput *k = &o[idx[j]];
        DisplayOut *d = &p->o[j];
        const Cfg *f = cfg_for((Conf *)c, ids[idx[j]]);
        d->conn = k->id;
        snprintf(d->id, sizeof d->id, "%s", ids[idx[j]]);
        snprintf(d->name, sizeof d->name, "%s", names[idx[j]]);
        snprintf(d->connector, sizeof d->connector, "%s-%u", kms_connector_type_name(k->type), k->type_id);
        int mi = f && f->w ? mode_pick(k->modes, k->nmodes, f->w, f->h, f->refresh) : -1;
        if (mi < 0) mi = mode_preferred(k->modes, k->nmodes);
        if (mi < 0) mi = 0;
        d->mode = k->modes[mi];
        d->rot = f ? f->rot & 3 : 0;
        int pw = d->rot & 1 ? d->mode.vdisplay : d->mode.hdisplay, ph = d->rot & 1 ? d->mode.hdisplay : d->mode.vdisplay;
        d->mm_w = (int)(d->rot & 1 ? k->mm_h : k->mm_w);
        d->mm_h = (int)(d->rot & 1 ? k->mm_w : k->mm_h);
        d->recommended = out_scale_recommended(pw, ph, d->mm_w, d->mm_h);
        d->scale = f && out_scale_valid(f->scale) ? f->scale : d->recommended;
        d->v = out_view(0, 0, pw, ph, d->scale);
        r[j] = (ORect){ f && f->placed ? f->x : 0, f && f->placed ? f->y : 0, d->v.w, d->v.h };
        placed[j] = j == 0 || (f && f->placed);
    }
    p->n = m;
    p->main = 0;
    p->mirror = c->mirror && m > 1;
    if (p->mirror) {
        int lw = p->o[0].v.w, lh = p->o[0].v.h;
        for (int j = 1; j < m; j++) p->o[j].v = out_view_mirror(lw, lh, p->o[j].v.pw, p->o[j].v.ph);
        p->bw = lw;
        p->bh = lh;
        return m;
    }
    /* positions are relative to the main display; a display never placed goes right of the others */
    r[0].x -= r[0].x; r[0].y = 0;
    for (int j = 1; j < m; j++) {
        if (placed[j]) continue;
        int right = 0;
        for (int q = 0; q < j; q++) if (r[q].x + r[q].w > right) right = r[q].x + r[q].w;
        r[j].x = right;
        r[j].y = 0;
    }
    out_fix(r, m);
    out_normalize(r, m, &p->bw, &p->bh);
    for (int j = 0; j < m; j++) { p->o[j].v.x = r[j].x; p->o[j].v.y = r[j].y; }
    return m;
}

int display_pick(const KmsOutput *o, int n, int *index, struct drm_mode_modeinfo *mode) {
    DisplayPlan p;
    if (!display_plan(o, n, &p)) return -1;
    for (int i = 0; i < n; i++)
        if (o[i].id == p.o[0].conn) { *index = i; *mode = p.o[0].mode; return 0; }
    return -1;
}

int display_take_resync(void) {
    if (!D.dirty || !kms()) return 0;
    D.dirty = 0;
    return 1;
}

void display_resync_done(int ok, const DisplayPlan *applied) {
    if (!ok && !D.retry) {                      /* back to what worked before */
        D.live = D.undo;
        D.retry = 1;
        D.dirty = 1;
        fprintf(stderr, "displays: the new configuration failed; restoring the previous one\n");
    } else D.retry = 0;
    if (applied) D.cur = *applied;
    if (D.save_pending && !D.dirty) { D.save_pending = 0; save_now(); }
    apply_tint_all();
    notify_list();
}

const DisplayPlan *display_current(void) { return &D.cur; }

int display_cpu_only(void) {
    for (int i = 0; i < D.cur.n; i++) if (D.cur.o[i].rot) return 1;
    return 0;
}

/* ---- Night Light ---- */

static void apply_tint(void) { apply_tint_all(); }
static void apply_tint_all(void) {
    Kms *hs[OUT_MAX];
    int nh = heads(hs, OUT_MAX);
    double r, g, b;
    night_temp_rgb(D.kelvin, &r, &g, &b);
    int was_soft = D.soft;
    D.soft = 0;
    /* hardware gamma only if every display has a LUT; otherwise the software tint for all of them
     * (and the LUTs that exist stay neutral, so nothing is tinted twice) */
    int hw = nh > 0 && !getenv("LORD_SOFT_GAMMA");
    for (int i = 0; i < nh && hw; i++) { int gs = kms_gamma_size(hs[i]); if (gs <= 1 || gs > 4096) hw = 0; }
    int gs = 0;
    for (int i = 0; i < nh; i++) {
        int n = kms_gamma_size(hs[i]);
        if (n <= 1 || n > 4096) continue;
        uint16_t *ramp = malloc((size_t)n * 3 * sizeof *ramp);
        if (!ramp) { hw = 0; continue; }
        double rr = hw ? r : 1, gg = hw ? g : 1, bb = hw ? b : 1;
        night_ramp(n, rr, gg, bb, 1, ramp, ramp + n, ramp + 2 * n);
        if (kms_set_gamma(hs[i], n, ramp, ramp + n, ramp + 2 * n)) hw = 0;
        free(ramp);
    }
    if (hw) gs = 1;
    if (!gs && D.kelvin != NIGHT_K_MAX) {
        uint16_t R[256], G[256], B[256];
        night_ramp(256, r, g, b, 1, R, G, B);
        for (int i = 0; i < 256; i++) {
            D.lut[0][i] = (uint8_t)(R[i] >> 8);
            D.lut[1][i] = (uint8_t)(G[i] >> 8);
            D.lut[2][i] = (uint8_t)(B[i] >> 8);
        }
        D.soft = 1;
    }
    if ((D.soft || was_soft) && D.host && D.host->redraw) D.host->redraw();
    if (D.soft != was_soft) {                   /* Settings shows when the tint is by software */
        char *j = display_list_json();
        if (j) { event("display", j); free(j); }
    }
}

void display_set_temperature(int kelvin) {
    if (kelvin < NIGHT_K_MIN) kelvin = NIGHT_K_MIN;
    if (kelvin > NIGHT_K_MAX) kelvin = NIGHT_K_MAX;
    if (kelvin == D.kelvin) return;
    D.kelvin = kelvin;
    apply_tint();
}

int display_temperature(void) { return D.kelvin; }
const uint8_t *display_soft_lut(void) { return D.soft ? &D.lut[0][0] : NULL; }

void display_tint(Canvas *c, Rect r) {
    if (!D.soft) return;
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > c->w) r.x1 = c->w;
    if (r.y1 > c->h) r.y1 = c->h;
    const uint8_t *lr = D.lut[0], *lg = D.lut[1], *lb = D.lut[2];
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t *p = c->px + (size_t)y * c->stride;
        for (int x = r.x0; x < r.x1; x++) {
            uint32_t v = p[x];
            p[x] = (v & 0xff000000u) | (uint32_t)lr[v >> 16 & 255] << 16 | (uint32_t)lg[v >> 8 & 255] << 8 | lb[v & 255];
        }
    }
}

int display_sun(const char *zone, int y, int m, int d, double *rise, double *set) {
    static char *tab;
    if (!tab) {
        const char *paths[] = { "/usr/share/zoneinfo/zone1970.tab", "/usr/share/zoneinfo/zone.tab" };
        for (int i = 0; i < 2 && !tab; i++) {
            FILE *f = fopen(paths[i], "re");
            if (!f) continue;
            tab = calloc(1, 65536);
            if (tab && fread(tab, 1, 65535, f) == 0) { free(tab); tab = NULL; }
            fclose(f);
        }
        if (!tab) return -1;
    }
    double lat, lon;
    if (!zone || night_zone_coords(tab, zone, &lat, &lon)) return -1;
    return night_sun_times(y, m, d, lat, lon, rise, set);
}

/* ---- outputs, modes ---- */

static const DisplayOut *plan_out(uint32_t conn, int *index) {
    for (int i = 0; i < D.cur.n; i++) if (D.cur.o[i].conn == conn) { if (index) *index = i; return &D.cur.o[i]; }
    return NULL;
}

static void out_json(HBuf *b, const KmsOutput *all, int i) {
    const KmsOutput *o = &all[i];
    char id[ID_MAX], name[64], conn[48];
    unique_identity(all, i, id, sizeof id, name, sizeof name);
    snprintf(conn, sizeof conn, "%s-%u", kms_connector_type_name(o->type), o->type_id);
    Edid e;
    int has_edid = o->edid && edid_parse(o->edid, o->edid_len, &e) == 0;
    int pi = -1;
    const DisplayOut *d = plan_out(o->id, &pi);
    hb_printf(b, "{\"id\":%u,\"connector\":", o->id);
    hb_str(b, conn);
    hb_printf(b, ",\"identity\":");
    hb_str(b, id);
    hb_printf(b, ",\"name\":");
    hb_str(b, name);
    hb_printf(b, ",\"builtin\":%s,\"connected\":%s,\"current\":%s,\"main\":%s,\"active\":%s,\"mmWidth\":%u,\"mmHeight\":%u,\"diagonal\":%d,\"vendor\":",
              o->type == DRM_MODE_CONNECTOR_eDP || o->type == DRM_MODE_CONNECTOR_LVDS || o->type == DRM_MODE_CONNECTOR_DSI ? "true" : "false",
              o->connected ? "true" : "false", pi == D.cur.main ? "true" : "false", pi == D.cur.main ? "true" : "false", d ? "true" : "false",
              o->mm_w, o->mm_h, has_edid ? edid_diagonal_tenths(&e) : 0);
    hb_str(b, has_edid ? e.vendor : "");
    int pref = mode_preferred(o->modes, o->nmodes);
    hb_printf(b, ",\"preferred\":{\"w\":%d,\"h\":%d}", pref >= 0 ? o->modes[pref].hdisplay : 0, pref >= 0 ? o->modes[pref].vdisplay : 0);
    if (d) {
        hb_printf(b, ",\"mode\":{\"w\":%u,\"h\":%u,\"refresh\":%d},\"rotation\":%d,\"scale\":%d,\"recommended\":%d", d->mode.hdisplay,
                  d->mode.vdisplay, mode_refresh_mhz(&d->mode), d->rot, d->scale, d->recommended);
        hb_printf(b, ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"pw\":%d,\"ph\":%d", d->v.x, d->v.y, d->v.w, d->v.h, d->v.pw, d->v.ph);
    } else {
        int pw = pref >= 0 ? o->modes[pref].hdisplay : 0, ph = pref >= 0 ? o->modes[pref].vdisplay : 0;
        hb_printf(b, ",\"recommended\":%d", out_scale_recommended(pw, ph, (int)o->mm_w, (int)o->mm_h));
    }
    const Cfg *s = cfg_for(&D.saved, id);
    if (s) hb_printf(b, ",\"saved\":{\"w\":%d,\"h\":%d,\"refresh\":%d,\"rotation\":%d,\"scale\":%d}", s->w, s->h, s->refresh, s->rot, s->scale);
    hb_printf(b, ",\"resolutions\":[");
    int res[64];
    int nr = mode_resolutions(o->modes, o->nmodes, res, 64);
    for (int r = 0; r < nr; r++) {
        int w = o->modes[res[r]].hdisplay, h = o->modes[res[r]].vdisplay, rates[16];
        int nrt = mode_rates(o->modes, o->nmodes, w, h, rates, 16);
        hb_printf(b, "%s{\"w\":%d,\"h\":%d,\"rates\":[", r ? "," : "", w, h);
        for (int j = 0; j < nrt; j++) hb_printf(b, "%s%d", j ? "," : "", rates[j]);
        hb_printf(b, "]}");
    }
    hb_printf(b, "]}");
}

char *display_list_json(void) {
    Kms *k = kms();
    HBuf b = { 0 };
    const DisplayOut *m = D.cur.n ? &D.cur.o[D.cur.main] : NULL;
    hb_printf(&b, "{\"onMonitor\":%s,\"scale\":%d,\"rotation\":%d,\"mirror\":%s,\"captureAll\":%s,\"width\":%d,\"height\":%d,"
                  "\"temperature\":%d,\"gammaSize\":%d,\"softTint\":%s,\"maxOutputs\":%d,\"outputs\":[",
              k ? "true" : "false", m ? m->scale : 100, m ? m->rot : 0, D.cur.mirror ? "true" : "false", D.capture_all ? "true" : "false",
              D.cur.bw, D.cur.bh, D.kelvin, k ? kms_gamma_size(k) : 0, D.soft ? "true" : "false", OUT_MAX);
    if (k) {
        KmsOutput outs[KMS_MAX_OUTPUTS];
        int n = kms_outputs(k->fd, outs, KMS_MAX_OUTPUTS), first = 1;
        for (int i = 0; i < n; i++) {
            if (!outs[i].connected) continue;
            if (!first) hb_printf(&b, ",");
            first = 0;
            out_json(&b, outs, i);
        }
        kms_outputs_free(outs, n);
    }
    hb_printf(&b, "],\"backlight\":[");
    /* backlight: readable by anyone; writing goes through the display-brightness admin action */
    DIR *d = opendir("/sys/class/backlight");
    int first = 1;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.' || strlen(e->d_name) > 64) continue;
        char p[160], v[32] = "", mx[32] = "", ty[32] = "";
        const char *names[3] = { "brightness", "max_brightness", "type" };
        char *outs[3] = { v, mx, ty };
        for (int i = 0; i < 3; i++) {
            snprintf(p, sizeof p, "/sys/class/backlight/%s/%s", e->d_name, names[i]);
            int fd = open(p, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            ssize_t r = read(fd, outs[i], 31);
            close(fd);
            if (r > 0) { outs[i][r] = 0; outs[i][strcspn(outs[i], "\n")] = 0; }
        }
        hb_printf(&b, "%s{\"name\":", first ? "" : ",");
        hb_str(&b, e->d_name);
        hb_printf(&b, ",\"value\":%ld,\"max\":%ld,\"type\":", atol(v), atol(mx));
        hb_str(&b, ty);
        hb_printf(&b, "}");
        first = 0;
    }
    if (d) closedir(d);
    hb_printf(&b, "]}");
    return hb_take(&b);
}

/* The identity (and the KMS output) of a connector, for the requests below. */
static int conn_identity(uint32_t conn, char *id, size_t n, KmsOutput *outs, int *nouts, int *index) {
    Kms *k = kms();
    if (!k) return -1;
    *nouts = kms_outputs(k->fd, outs, KMS_MAX_OUTPUTS);
    for (int i = 0; i < *nouts; i++)
        if (outs[i].id == conn && outs[i].connected && outs[i].nmodes) {
            unique_identity(outs, i, id, n, NULL, 0);
            *index = i;
            return 0;
        }
    kms_outputs_free(outs, *nouts);
    return -1;
}

int display_request(uint32_t conn, int w, int h, int refresh_mhz, int rotation, int scale) {
    if (scale == 1 || scale == 2) scale *= 100;
    if (rotation < 0 || rotation > 3 || !out_scale_valid(scale)) return -1;
    KmsOutput outs[KMS_MAX_OUTPUTS];
    int n = 0, i = -1, ret = -1;
    char id[ID_MAX];
    if (conn_identity(conn, id, sizeof id, outs, &n, &i)) return -1;
    int m = w > 0 ? mode_pick(outs[i].modes, outs[i].nmodes, w, h, refresh_mhz) : mode_preferred(outs[i].modes, outs[i].nmodes);
    if (m >= 0) {
        D.undo = D.live;
        Cfg *f = cfg_make(&D.live, id);
        f->w = outs[i].modes[m].hdisplay;
        f->h = outs[i].modes[m].vdisplay;
        f->refresh = mode_refresh_mhz(&outs[i].modes[m]);
        f->rot = rotation;
        f->scale = scale;
        D.dirty = 1;
        ret = 0;
    }
    kms_outputs_free(outs, n);
    return ret;
}

int display_set_main(uint32_t conn) {
    KmsOutput outs[KMS_MAX_OUTPUTS];
    int n = 0, i = -1;
    char id[ID_MAX];
    if (conn_identity(conn, id, sizeof id, outs, &n, &i)) return -1;
    kms_outputs_free(outs, n);
    D.undo = D.live;
    /* the arrangement stays: positions are relative to the main display, so they shift with it */
    const DisplayOut *old = D.cur.n ? &D.cur.o[D.cur.main] : NULL;
    for (int j = 0; j < D.cur.n; j++) {
        Cfg *f = cfg_make(&D.live, D.cur.o[j].id);
        f->x = D.cur.o[j].v.x - (old ? old->v.x : 0);
        f->y = D.cur.o[j].v.y - (old ? old->v.y : 0);
        f->placed = 1;
    }
    const DisplayOut *nw = plan_out(conn, NULL);
    if (nw) for (int j = 0; j < D.cur.n; j++) {
        Cfg *f = cfg_make(&D.live, D.cur.o[j].id);
        f->x -= nw->v.x - (old ? old->v.x : 0);
        f->y -= nw->v.y - (old ? old->v.y : 0);
    }
    snprintf(D.live.main_id, sizeof D.live.main_id, "%s", id);
    D.dirty = 1;
    return 0;
}

int display_set_mirror(int on) {
    if (!kms()) return -1;
    D.undo = D.live;
    D.live.mirror = on != 0;
    D.dirty = 1;
    return 0;
}

void display_set_capture_all(int all) {
    if (D.capture_all == (all != 0)) return;
    D.capture_all = all != 0;
    store();
}
int display_capture_all(void) { return D.capture_all; }

int display_arrange(uint32_t conn, int x, int y, int apply, int *sx, int *sy) {
    if (D.cur.mirror || D.cur.n < 2) return -1;
    int mv = -1;
    plan_out(conn, &mv);
    if (mv < 0) return -1;
    ORect r[OUT_MAX];
    for (int j = 0; j < D.cur.n; j++) r[j] = (ORect){ D.cur.o[j].v.x, D.cur.o[j].v.y, D.cur.o[j].v.w, D.cur.o[j].v.h };
    /* a position that already touches and doesn't overlap (Settings snaps it, sdk/displaylayout.js)
     * is kept exactly; anything else is snapped next to a display */
    int snap = out_fits(r, D.cur.n, mv, x, y) ? 0 : 24;
    out_snap(r, D.cur.n, mv, x, y, snap, &r[mv].x, &r[mv].y);
    *sx = r[mv].x;
    *sy = r[mv].y;
    if (!apply) return 0;
    D.undo = D.live;
    const DisplayOut *m = &D.cur.o[D.cur.main];
    int mx = mv == D.cur.main ? r[mv].x : m->v.x, my = mv == D.cur.main ? r[mv].y : m->v.y;
    for (int j = 0; j < D.cur.n; j++) {
        Cfg *f = cfg_make(&D.live, D.cur.o[j].id);
        f->x = r[j].x - mx;
        f->y = r[j].y - my;
        f->placed = 1;
    }
    D.dirty = 1;
    return 0;
}

int display_save(uint32_t conn) {
    (void)conn;                         /* the whole arrangement is kept together */
    if (!kms()) return -1;
    if (D.dirty) { D.save_pending = 1; return 0; }     /* after main.c applies the change */
    return save_now();
}

static int save_now(void) {
    for (int j = 0; j < D.cur.n; j++) {
        const Cfg *f = cfg_for(&D.live, D.cur.o[j].id);
        Cfg *s = cfg_make(&D.saved, D.cur.o[j].id);
        const DisplayOut *d = &D.cur.o[j];
        s->w = d->mode.hdisplay;
        s->h = d->mode.vdisplay;
        s->refresh = mode_refresh_mhz(&d->mode);
        s->rot = d->rot;
        s->scale = f && f->scale ? d->scale : 0;     /* never chosen: keeps following the recommendation */
        if (f && f->placed) { s->x = f->x; s->y = f->y; s->placed = 1; }
    }
    if (D.cur.n) snprintf(D.saved.main_id, sizeof D.saved.main_id, "%s", D.cur.o[D.cur.main].id);
    D.saved.mirror = D.live.mirror;
    /* what is shown is now what is kept */
    for (int i = 0; i < D.saved.n; i++) {
        Cfg *l = cfg_make(&D.live, D.saved.c[i].id);
        *l = D.saved.c[i];
    }
    snprintf(D.live.main_id, sizeof D.live.main_id, "%s", D.saved.main_id);
    D.undo = D.live;
    return store();
}

void display_attached(void) {
    D.off = 0;
    D.last = now_ms();
    D.done_off = D.done_lock = D.done_sleep = 0;
    D.dirty = 1;                        /* the other displays (and their saved modes) */
    apply_tint_all();
    notify_list();
}

void display_detached(void) { D.off = 0; D.soft = D.soft && D.kelvin != NIGHT_K_MAX; memset(&D.cur, 0, sizeof D.cur); }

static unsigned hotplug_seq;
unsigned display_hotplug_seq(void) { return hotplug_seq; }

void display_hotplug(void) {
    hotplug_seq++;
    Kms *k = kms();
    if (k) {
        KmsOutput outs[KMS_MAX_OUTPUTS];
        int n = kms_outputs(k->fd, outs, KMS_MAX_OUTPUTS);
        fprintf(stderr, "displays: hotplug, %d connected\n", connected_count(outs, n));
        kms_outputs_free(outs, n);
        D.dirty = 1;                    /* main.c opens the new display or closes the one unplugged */
    }
    notify_list();
}

/* ---- idle ---- */

void display_set_idle(int off_s, int lock_s, int sleep_s) {
    D.off_s = off_s > 0 ? off_s : 0;
    D.lock_s = lock_s > 0 ? lock_s : 0;
    D.sleep_s = sleep_s > 0 ? sleep_s : 0;
    D.last = now_ms();
    D.done_off = D.done_lock = D.done_sleep = 0;
}

static int dpms_all(int on) {
    Kms *hs[OUT_MAX];
    int nh = heads(hs, OUT_MAX), ok = 0;
    for (int i = 0; i < nh; i++) ok |= kms_dpms(hs[i], on) == 0;
    if (!ok && nh) fprintf(stderr, "displays: the card did not take DPMS %s\n", on ? "on" : "off");
    return ok;
}

void display_wake(void) {
    if (!D.off) return;
    Kms *k = kms();
    D.off = 0;
    if (k) dpms_all(1);
    if (D.host && D.host->redraw) D.host->redraw();
    event("idle", "{\"stage\":\"wake\"}");
}

void display_activity(double now) {
    D.last = now;
    D.done_off = D.done_lock = D.done_sleep = 0;
    if (D.off) display_wake();
}

static double deadline(int secs, int done) { return secs && !done ? D.last + secs * 1000.0 : -1; }

double display_timeout(double now) {
    if (!kms()) return -1;
    double best = -1, ds[3] = { deadline(D.off_s, D.done_off), deadline(D.lock_s, D.done_lock), deadline(D.sleep_s, D.done_sleep) };
    for (int i = 0; i < 3; i++) {
        if (ds[i] < 0) continue;
        double left = ds[i] - now;
        if (left < 0) left = 0;
        if (best < 0 || left < best) best = left;
    }
    return best;
}

void display_tick(double now) {
    Kms *k = kms();
    if (!k) return;
    double off = deadline(D.off_s, D.done_off), lock = deadline(D.lock_s, D.done_lock), slp = deadline(D.sleep_s, D.done_sleep);
    if (off >= 0 && now >= off) {
        D.done_off = 1;
        if (dpms_all(0)) D.off = 1;
        event("idle", "{\"stage\":\"off\"}");
    }
    if (lock >= 0 && now >= lock) { D.done_lock = 1; event("idle", "{\"stage\":\"lock\"}"); }
    if (slp >= 0 && now >= slp) { D.done_sleep = 1; event("idle", "{\"stage\":\"sleep\"}"); }
}

int display_is_off(void) { return D.off; }

/* ---- software rotation ---- */

Rect display_rotate_rect(Rect r, int lw, int lh, int rot) {
    Rect o;
    switch (rot & 3) {
    case 1: o = (Rect){ lh - r.y1, r.x0, lh - r.y0, r.x1 }; break;    /* (x, y) → (lh-1-y, x) */
    case 2: o = (Rect){ lw - r.x1, lh - r.y1, lw - r.x0, lh - r.y0 }; break;
    case 3: o = (Rect){ r.y0, lw - r.x1, r.y1, lw - r.x0 }; break;     /* (x, y) → (y, lw-1-x) */
    default: o = r; break;
    }
    return o;
}

/* Copies rect r (logical coordinates, inside lw×lh) to dst rotated; the logical pixel (x, y) is
 * px[(y - oy) * stride + (x - ox)]. */
void display_rotate_copy_at(const uint32_t *px, int stride, int ox, int oy, Rect r, int lw, int lh, Canvas *dst, int rot) {
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > lw) r.x1 = lw;
    if (r.y1 > lh) r.y1 = lh;
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
    rot &= 3;
    for (int y = r.y0; y < r.y1; y++) {
        const uint32_t *s = px + (size_t)(y - oy) * stride;
        switch (rot) {
        case 0: memcpy(dst->px + (size_t)y * dst->stride + r.x0, s + (r.x0 - ox), (size_t)(r.x1 - r.x0) * 4); break;
        case 1: { uint32_t *col = dst->px + (lh - 1 - y);                 /* column lh-1-y, rows x */
                  for (int x = r.x0; x < r.x1; x++) col[(size_t)x * dst->stride] = s[x - ox];
                  break; }
        case 2: { uint32_t *row = dst->px + (size_t)(lh - 1 - y) * dst->stride;
                  for (int x = r.x0; x < r.x1; x++) row[lw - 1 - x] = s[x - ox];
                  break; }
        case 3: { uint32_t *col = dst->px + y;                            /* column y, rows lw-1-x */
                  for (int x = r.x0; x < r.x1; x++) col[(size_t)(lw - 1 - x) * dst->stride] = s[x - ox];
                  break; }
        }
    }
}

void display_rotate_copy(const Canvas *src, Rect r, Canvas *dst, int rot) {
    display_rotate_copy_at(src->px, src->stride, 0, 0, r, src->w, src->h, dst, rot);
}

void display_unrotate_point(int rot, double px, double py, double *lx, double *ly) {
    switch (rot & 3) {
    case 1: *lx = py; *ly = 1 - px; break;
    case 2: *lx = 1 - px; *ly = 1 - py; break;
    case 3: *lx = 1 - py; *ly = px; break;
    default: *lx = px; *ly = py; break;
    }
}
