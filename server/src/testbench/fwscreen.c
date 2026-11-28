/* fwscreen.c — the Frameworks screen (fwscreen.h): the toolkit audit in show mode, drawn live. */
#include "fwscreen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define C_BG       TB_RGB(18, 18, 26)
#define C_PANEL    TB_RGB(28, 28, 38)
#define C_ROW_RUN  TB_RGB(46, 38, 72)
#define C_ROW_SEL  TB_RGB(36, 36, 50)
#define C_TEXT     TB_RGB(225, 225, 235)
#define C_DIM      TB_RGB(140, 140, 160)
#define C_FAINT    TB_RGB(70, 70, 88)
#define C_HEAD     TB_RGB(180, 150, 255)
#define C_GOOD     TB_RGB(120, 230, 150)
#define C_WARN     TB_RGB(255, 190, 70)
#define C_BAD      TB_RGB(255, 90, 80)
#define C_CELL     TB_RGB(44, 44, 56)

/* the checks' short codes over the cells (toolkits.c's order); the details box names them in full */
static const char *const CODE[TK_CHECKS] = { "MAP", "SIZ", "SCL", "THM", "BKE", "DEC", "FRM", "MNU", "CMB", "TIP",
                                             "CLP", "DND", "TXT", "FUL" };
/* what a toolkit needs to be tested (the import probe's package), for a "not installed" row */
static const char *need_of(const char *toolkit) {
    if (!strcmp(toolkit, "gtk3")) return "python3-gi gir1.2-gtk-3.0";
    if (!strcmp(toolkit, "gtk4")) return "python3-gi gir1.2-gtk-4.0";
    if (!strcmp(toolkit, "qt5")) return "python3-pyqt5";
    if (!strcmp(toolkit, "qt6")) return "python3-pyqt6";
    return "python3";
}

void fw_init(FwState *s, const int *which, int n) {
    memset(s, 0, sizeof *s);
    s->state = FW_IDLE;
    s->running = -1;
    s->global = -1;
    s->sel = -1;
    for (int i = 0; i < n && i < TK_COMBOS; i++) {
        FwCombo *c = &s->c[s->n++];
        c->combo = which[i];
        c->phase = FWC_WAIT;
        c->step = -1;
        c->score = -1;
        memset(c->tick, -1, sizeof c->tick);
    }
}

void fw_event(FwState *s, const TkEvent *e, double now) {
    if (!e || e->slot < 0 || e->slot >= s->n) return;
    FwCombo *c = &s->c[e->slot];
    if (c->combo != e->combo) return;
    s->events++;
    if (s->state == FW_IDLE) s->state = FW_RUNNING;
    switch (e->kind) {
    case TK_EV_START:
        c->phase = FWC_RUN;
        c->t0 = now;
        s->running = e->slot;
        break;
    case TK_EV_STEP:
        if (c->phase == FWC_RUN && e->check >= 0 && e->check < TK_CHECKS) c->step = e->check;
        break;
    case TK_EV_CHECK:
        if (c->phase == FWC_RUN && e->check >= 0 && e->check < TK_CHECKS && e->result >= TK_SKIP && e->result <= TK_FAIL)
            c->tick[e->check] = (signed char)e->result;
        break;
    case TK_EV_DONE: {
        const TkResult *r = e->r;
        c->phase = FWC_DONE;
        c->step = -1;
        c->t1 = now;
        if (s->running == e->slot) s->running = -1;
        if (!r) break;
        c->status = r->status;
        c->fatal = r->fatal;
        c->signal_no = r->signal_no;
        c->score = r->status == TK_ST_FATAL ? 0 : r->score;
        c->failed_globals = r->failed_globals;
        c->bound = 0;
        if (r->log) for (int g = 0; g < TK_GLOBALS; g++) c->bound += r->log->bound[g] != 0;
        snprintf(c->version, sizeof c->version, "%s", r->version);
        /* the final result replaces the live ticks: the screen ends exactly as the JSON says */
        for (int i = 0; i < TK_CHECKS; i++) {
            int ran = r->status == TK_ST_RAN || (r->status == TK_ST_FATAL && r->json_ok);
            c->tick[i] = ran ? (signed char)r->checks[i].result : -1;
            snprintf(c->detail[i], sizeof c->detail[i], "%s", ran ? r->checks[i].detail : "");
        }
        break;
    }
    }
}

void fw_finish(FwState *s, const TkReport *rep, int saved) {
    s->state = FW_DONE;
    s->running = -1;
    s->saved = saved == 0 ? 1 : -1;
    s->global = rep ? rep->global : -1;
    s->ran = rep ? rep->ran : 0;
    if (!rep) return;
    /* the recommendation of each Wayland row, from its toolkit's X11 row (tk_recommend) */
    for (int i = 0; i < rep->n && i < s->n; i++) {
        const TkCombo *cw = tk_combo(rep->r[i].combo);
        if (!cw || !cw->wayland) continue;
        for (int j = 0; j < rep->n; j++) {
            const TkCombo *cx = tk_combo(rep->r[j].combo);
            if (j == i || !cx || cx->wayland || strcmp(cx->toolkit, cw->toolkit)) continue;
            const char *rec = tk_recommend(&rep->r[i], &rep->r[j]);
            if (rec) snprintf(s->c[i].recommend, sizeof s->c[i].recommend, "%s", rec);
        }
    }
}

static int has_trouble(const FwCombo *c) {
    if (c->phase != FWC_DONE) return 0;
    if (c->status == TK_ST_FATAL || c->recommend[0] || c->failed_globals) return 1;
    for (int i = 0; i < TK_CHECKS; i++) if (c->tick[i] == TK_FAIL) return 1;
    return 0;
}

int fw_detail_slot(const FwState *s) {
    if (s->sel >= 0 && s->sel < s->n) return s->sel;
    if (s->running >= 0) return s->running;
    for (int i = 0; i < s->n; i++) if (has_trouble(&s->c[i])) return i;
    for (int i = s->n - 1; i >= 0; i--) if (s->c[i].phase == FWC_DONE && s->c[i].status == TK_ST_RAN) return i;
    return s->n ? 0 : -1;
}

/* ---- layout ---- */

#define PAD 16
#define ROW_Y0 92
#define ROW_H 26
#define X_ID (PAD + 26)
#define X_VER (X_ID + 150)
#define X_SCORE (X_VER + 64)
#define SCORE_W 134

static int cell_w(int w) {
    int cw = (w - (X_SCORE + SCORE_W) - PAD) / TK_CHECKS;
    return cw < 16 ? 16 : cw > 40 ? 40 : cw;
}
static int details_y(void) { return ROW_Y0 + TK_COMBOS * ROW_H + 12; }

typedef struct { int x, y, w, h; } Box;
static Box btn_again(int w, int h) { return (Box){ w - PAD - 132 - 8 - 96, h - 30, 132, 22 }; }
static Box btn_back(int w, int h) { return (Box){ w - PAD - 96, h - 30, 96, 22 }; }
static int in_box(Box b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

int fw_hit(const FwState *s, int w, int h, int x, int y) {
    if (s->state == FW_DONE && in_box(btn_again(w, h), x, y)) return FW_HIT_AGAIN;
    if (in_box(btn_back(w, h), x, y)) return FW_HIT_BACK;
    if (y >= ROW_Y0 && y < ROW_Y0 + s->n * ROW_H && x >= PAD && x < w - PAD) return (y - ROW_Y0) / ROW_H;
    return FW_HIT_NONE;
}

static uint32_t score_colour(double sc) { return sc >= 80 ? C_GOOD : sc >= 50 ? C_WARN : C_BAD; }

static void button(Surf *o, Box b, const char *t, int on) {
    d_fill(o, b.x, b.y, b.w, b.h, on ? C_ROW_RUN : C_CELL);
    d_frame(o, b.x, b.y, b.w, b.h, 1, on ? C_HEAD : C_FAINT);
    d_text_c(o, b.x + b.w / 2, b.y + b.h / 2, 1, on ? C_TEXT : C_DIM, t);
}

static void spinner(Surf *o, int cx, int cy, double now) {
    int k = (int)(now / 110) % 8;
    for (int i = 0; i < 8; i++) {
        double a = i * M_PI / 4;
        int age = (k - i + 8) % 8;
        uint32_t c = age == 0 ? C_WARN : age < 3 ? TB_RGB(150, 120, 60) : C_FAINT;
        d_circle(o, cx + (int)lround(6 * cos(a)), cy + (int)lround(6 * sin(a)), 1, c);
    }
}

static void draw_cells(Surf *o, const FwCombo *c, int x0, int y, int cw, double now, int faint) {
    int sz = cw - 4 < ROW_H - 6 ? cw - 4 : ROW_H - 6;
    for (int i = 0; i < TK_CHECKS; i++) {
        int x = x0 + i * cw + (cw - sz) / 2, yy = y + (ROW_H - sz) / 2;
        int t = c->tick[i];
        if (faint) { d_frame(o, x, yy, sz, sz, 1, TB_RGB(34, 34, 44)); continue; }
        if (t == TK_PASS) { d_fill(o, x, yy, sz, sz, TB_RGB(30, 80, 50)); d_tick(o, x, yy, sz, C_GOOD); }
        else if (t == TK_FAIL) { d_fill(o, x, yy, sz, sz, TB_RGB(90, 30, 30)); d_cross(o, x, yy, sz, C_BAD); }
        else if (t == TK_SKIP) { d_fill(o, x, yy, sz, sz, C_CELL); d_fill(o, x + sz / 4, yy + sz / 2 - 1, sz / 2, 2, C_DIM); }
        else {
            d_fill(o, x, yy, sz, sz, C_CELL);
            if (c->phase == FWC_RUN && c->step == i && ((long)(now / 250) & 1) == 0) d_frame(o, x - 1, yy - 1, sz + 2, sz + 2, 2, C_WARN);
            else if (c->phase == FWC_RUN && c->step == i) d_frame(o, x, yy, sz, sz, 1, C_WARN);
        }
    }
}

static void draw_row(Surf *o, const FwState *s, int i, double now, int cw) {
    const FwCombo *c = &s->c[i];
    const TkCombo *k = tk_combo(c->combo);
    int y = ROW_Y0 + i * ROW_H, cy = y + ROW_H / 2;
    int sel = fw_detail_slot(s) == i;
    if (c->phase == FWC_RUN) d_fill(o, PAD - 6, y + 1, o->w - 2 * PAD + 12, ROW_H - 2, C_ROW_RUN);
    else if (sel) d_fill(o, PAD - 6, y + 1, o->w - 2 * PAD + 12, ROW_H - 2, C_ROW_SEL);
    if (c->phase == FWC_RUN) d_fill(o, PAD - 6, y + 1, 3, ROW_H - 2, C_WARN);
    int notrun = c->phase == FWC_DONE && c->status != TK_ST_RAN && c->status != TK_ST_FATAL;
    uint32_t idc = c->phase == FWC_WAIT || notrun ? C_DIM : C_TEXT;
    /* the status mark */
    if (c->phase == FWC_WAIT) d_ring(o, PAD + 8, cy, 7, 2, C_FAINT);
    else if (c->phase == FWC_RUN) spinner(o, PAD + 8, cy, now);
    else if (c->status == TK_ST_RAN) d_tick(o, PAD, cy - 8, 16, c->score >= 0 ? score_colour(c->score) : C_DIM);
    else if (c->status == TK_ST_FATAL) d_cross(o, PAD, cy - 8, 16, C_BAD);
    else d_fill(o, PAD + 3, cy - 1, 10, 3, C_FAINT);
    d_textl(o, X_ID, cy - 7, 2, idc, k ? k->id : "?");
    if (c->version[0]) {
        char v[16];
        snprintf(v, sizeof v, "%.10s", c->version);
        d_textl(o, X_VER, cy - 3, 1, C_DIM, v);
    }
    /* the score, or what happened */
    char t[64];
    if (c->phase == FWC_RUN) {
        snprintf(t, sizeof t, "%.1f s", (now - c->t0) / 1000.0);
        d_textl(o, X_SCORE, cy - 3, 1, C_WARN, t);
    } else if (c->phase == FWC_DONE && c->status == TK_ST_RAN) {
        if (c->score >= 0) snprintf(t, sizeof t, "%.1f %%", c->score); else snprintf(t, sizeof t, "-");
        d_textl(o, X_SCORE, cy - 7, 2, c->score >= 0 ? score_colour(c->score) : C_DIM, t);
    } else if (c->phase == FWC_DONE && c->status == TK_ST_FATAL) {
        d_textl(o, X_SCORE, cy - 7, 2, C_BAD, "0 %");
        d_textl(o, X_SCORE + 44, cy - 3, 1, C_BAD, tk_fatal_id(c->fatal));
    } else if (c->phase == FWC_DONE) {
        d_textl(o, X_SCORE, cy - 3, 1, C_DIM, tk_status_id(c->status));
    }
    draw_cells(o, c, X_SCORE + SCORE_W, y, cw, now, c->phase == FWC_WAIT || notrun);
}

static int line(Surf *o, int x, int *y, uint32_t c, const char *t, int ymax) {
    if (*y > ymax) return 0;
    d_textl(o, x, *y, 1, c, t);
    *y += 13;
    return 1;
}

static void draw_details(Surf *o, const FwState *s, double now) {
    int y0 = details_y(), h = o->h - 40 - y0;
    if (h < 40) return;
    d_fill(o, PAD - 6, y0, o->w - 2 * PAD + 12, h, C_PANEL);
    int i = fw_detail_slot(s);
    if (i < 0) return;
    const FwCombo *c = &s->c[i];
    const TkCombo *k = tk_combo(c->combo);
    if (!k) return;
    int x = PAD + 4, y = y0 + 10, ymax = y0 + h - 14, maxch = (o->w - 2 * PAD - 40) / 6;
    char t[256];
    d_textl(o, x, y, 2, C_HEAD, k->id);
    int xr = x + d_text_w(k->id, 2) + 14;
    if (c->version[0]) { snprintf(t, sizeof t, "%s %s", k->label, c->version); d_textl(o, xr, y + 4, 1, C_DIM, t); }
    else d_textl(o, xr, y + 4, 1, C_DIM, k->label);
    y += 24;
    if (c->phase == FWC_WAIT) { line(o, x, &y, C_DIM, "waiting", ymax); return; }
    if (c->phase == FWC_RUN) {
        snprintf(t, sizeof t, "step: %s", c->step >= 0 ? tk_check_id(c->step) : "map");
        d_textl(o, x, y, 2, C_WARN, t);
        y += 22;
        int pass = 0, fail = 0, skip = 0;
        for (int j = 0; j < TK_CHECKS; j++) { pass += c->tick[j] == TK_PASS; fail += c->tick[j] == TK_FAIL; skip += c->tick[j] == TK_SKIP; }
        snprintf(t, sizeof t, "pass %d  fail %d  skip %d  of %d", pass, fail, skip, TK_CHECKS);
        line(o, x, &y, C_TEXT, t, ymax);
        snprintf(t, sizeof t, "%.1f s", (now - c->t0) / 1000.0);
        line(o, x, &y, C_DIM, t, ymax);
        return;
    }
    if (c->status != TK_ST_RAN && c->status != TK_ST_FATAL) {
        line(o, x, &y, C_DIM, tk_status_id(c->status), ymax);
        if (c->status == TK_ST_NOT_INSTALLED) {
            snprintf(t, sizeof t, "needs: %s", need_of(k->toolkit));
            line(o, x, &y, C_DIM, t, ymax);
        }
        return;
    }
    if (c->status == TK_ST_FATAL) {
        if (c->fatal == TK_F_CRASH) snprintf(t, sizeof t, "fatal: %s  signal %d", tk_fatal_id(c->fatal), c->signal_no);
        else snprintf(t, sizeof t, "fatal: %s", tk_fatal_id(c->fatal));
        line(o, x, &y, C_BAD, t, ymax);
    }
    int fails = 0;
    for (int j = 0; j < TK_CHECKS; j++) {
        if (c->tick[j] != TK_FAIL) continue;
        fails++;
        if (y > ymax) break;
        d_cross(o, x, y - 3, 12, C_BAD);
        snprintf(t, sizeof t, "%-12s %.*s", tk_check_id(j), maxch > 16 ? maxch - 16 : 1, c->detail[j]);
        d_textl(o, x + 16, y, 1, C_TEXT, t);
        y += 13;
    }
    for (int j = 0; j < TK_CHECKS && y <= ymax; j++) {
        if (c->tick[j] != TK_SKIP) continue;
        d_fill(o, x + 3, y + 3, 6, 2, C_DIM);
        snprintf(t, sizeof t, "%-12s %.*s", tk_check_id(j), maxch > 16 ? maxch - 16 : 1, c->detail[j]);
        d_textl(o, x + 16, y, 1, C_DIM, t);
        y += 13;
    }
    if (k->wayland && c->status == TK_ST_RAN) {
        snprintf(t, sizeof t, "globals: %d bound, %d failed", c->bound, c->failed_globals);
        line(o, x, &y, c->failed_globals ? C_BAD : C_DIM, t, ymax);
    }
    if (!fails && c->status == TK_ST_RAN) {
        d_tick(o, x, y - 3, 12, C_GOOD);
        int pass = 0;
        for (int j = 0; j < TK_CHECKS; j++) pass += c->tick[j] == TK_PASS;
        snprintf(t, sizeof t, "%d/%d pass", pass, TK_CHECKS);
        d_textl(o, x + 16, y, 1, C_GOOD, t);
        y += 13;
    }
    if (c->recommend[0] && y <= ymax) {
        y += 4;
        snprintf(t, sizeof t, "-> %s", c->recommend);
        d_textl(o, x, y, 2, C_WARN, t);
    }
}

void fw_draw(Surf *o, const FwState *s, double now) {
    d_fill(o, 0, 0, o->w, o->h, C_BG);
    int cw = cell_w(o->w);
    d_text(o, PAD, 14, 2, C_TEXT, "FRAMEWORKS");
    d_textl(o, PAD + d_text_w("FRAMEWORKS", 2) + 14, 18, 1, C_DIM, "GTK 3  GTK 4  Qt 5  Qt 6  X11");
    /* the global score, or how far the run is */
    char t[96];
    int done = 0;
    for (int i = 0; i < s->n; i++) done += s->c[i].phase == FWC_DONE;
    if (s->state == FW_REFUSED) d_textl(o, PAD, 44, 1, C_BAD, "not as root");
    else if (s->state == FW_DONE) {
        if (s->global >= 0) {
            snprintf(t, sizeof t, "%.1f %%", s->global);
            d_textl(o, o->w - PAD - d_text_w(t, 3), 10, 3, score_colour(s->global), t);
            snprintf(t, sizeof t, "global  %d/%d", s->ran, s->n);
        } else snprintf(t, sizeof t, "global  -  0/%d", s->n);
        d_textl(o, o->w - PAD - d_text_w(t, 1) - (s->global >= 0 ? d_text_w("100.0 %", 3) + 12 : 0), 20, 1, C_DIM, t);
    } else {
        snprintf(t, sizeof t, "%d/%d", done, s->n);
        d_textl(o, o->w - PAD - d_text_w(t, 2), 14, 2, C_WARN, t);
    }
    /* a progress bar under the title */
    int bw = o->w - 2 * PAD;
    d_fill(o, PAD, 40, bw, 3, C_CELL);
    if (s->n) d_fill(o, PAD, 40, bw * done / s->n, 3, s->state == FW_DONE ? C_GOOD : C_WARN);
    d_textl(o, PAD, 52, 1, C_FAINT, "lordos-testbench --toolkits --show");
    /* the cells' header: each check's short code */
    int x0 = X_SCORE + SCORE_W;
    for (int i = 0; i < TK_CHECKS; i++) d_text_c(o, x0 + i * cw + cw / 2, ROW_Y0 - 10, 1, C_DIM, CODE[i]);
    for (int i = 0; i < s->n; i++) draw_row(o, s, i, now, cw);
    draw_details(o, s, now);
    /* the footer: where the result went, and the buttons */
    if (s->saved > 0) d_textl(o, PAD, o->h - 24, 1, C_DIM, "-> ~/.cache/lordos/toolkit-audit.json");
    else if (s->saved < 0) d_textl(o, PAD, o->h - 24, 1, C_BAD, "~/.cache/lordos: not saved");
    else d_textl(o, PAD, o->h - 24, 1, C_FAINT, "UP/DOWN");
    if (s->state == FW_DONE) button(o, btn_again(o->w, o->h), "R: RUN AGAIN", 1);
    button(o, btn_back(o->w, o->h), "ESC: CARD", 0);
}
