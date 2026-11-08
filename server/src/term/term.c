/* term.c — terminal sessions: PTY ↔ emulator, keyboard, mouse, clipboard, events
 * for the app (title, directory, bell, process exit) and cursor blinking. */
#include "term.h"
#include "tcapture.h"
#include "../ui/ui.h"
#include "../ui/fonts.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define OUT_MAX (16u << 20)
#define READ_CHUNK 65536
#define READ_BUDGET (1u << 20)     /* bytes per session per loop iteration: the rest, on the next one */
#define BLINK_MS 530.0
#define REPLAY_BYTES (6u << 20)    /* memory for Instant Replay per session */
#define REPLAY_EVERY_MS 100.0

static Session sessions[TERM_MAX];
static int next_id = 1;
static char *clip;
static int mono[4] = { -1, -1, -1, -1 };
static pid_t orphans[TERM_MAX * 2];
static int norphans;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
static void check_secret(Session *s);

/* ---- default profile: the "Classic" colors ---- */

void term_profile_defaults(TermProfile *p) {
    static const uint32_t pal[16] = {
        0x000000, 0xc91b00, 0x00c200, 0xc7c400, 0x2225c4, 0xca30c7, 0x00c5c7, 0xc7c7c7,
        0x686868, 0xff6e67, 0x5ffa68, 0xfffc67, 0x6871ff, 0xff77ff, 0x60fdff, 0xffffff,
    };
    memset(p, 0, sizeof *p);
    p->font_size = 13;
    p->line_spacing = 1.0f;
    p->padding = 8;
    memcpy(p->palette, pal, sizeof pal);
    p->fg = 0xc7c7c7;
    p->bg = 0x000000;
    p->cursor = 0xc7c7c7;
    p->cursor_text = 0x000000;
    p->has_cursor_text = 1;
    p->sel_bg = 0xb5d5ff;
    p->sel_fg = 0x000000;
    p->has_sel_fg = 1;
    p->link = 0x2b7ff7;
    p->guide = 0xa6e8ff;
    p->bg_opacity = 1;
    p->bold_bright = 1;
    p->use_bold = 1;
    p->use_italic = 1;
    p->cursor_style = CURSOR_BLOCK;
    p->alt_esc = 1;
    p->scrollback = 10000;
    p->mouse_reporting = 1;
    p->copy_on_select = 1;
    p->alt_scroll = 1;
    snprintf(p->word_chars, sizeof p->word_chars, "/-+\\~_.");
    for (int i = 0; i < 4; i++) p->font[i] = p->font_na[i] = -1;
}

/* ---- fonts ---- */

void term_init(const char *fonts_dir, const char *shell_dir) {
    setlocale(LC_CTYPE, "C.UTF-8");     /* wcwidth() and towlower() with Unicode */
    static const char *const files[4] = {
        "JetBrainsMono-Regular.ttf", "JetBrainsMono-Bold.ttf", "JetBrainsMono-Italic.ttf", "JetBrainsMono-BoldItalic.ttf",
    };
    for (int i = 0; i < 4; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", fonts_dir, files[i]);
        mono[i] = text_font_add(path);
    }
    fonts_set_builtin_dir(fonts_dir);
    pty_set_integration(shell_dir);
}

int term_mono_font(int bold, int italic) {
    int f = mono[(bold ? 1 : 0) + (italic ? 2 : 0)];
    return f >= 0 ? f : mono[0] >= 0 ? mono[0] : 0;
}

/* Profile font for the requested style (with the bundled one as fallback). */
int term_profile_font(const TermProfile *p, int non_ascii, int bold, int italic) {
    int i = (bold ? 1 : 0) + (italic ? 2 : 0);
    const int *set = non_ascii ? p->font_na : p->font;
    if (set[i] >= 0) return set[i];
    if (non_ascii) return -1;
    return term_mono_font(bold, italic);
}

static int load_family(const char *family, int out[4]) {
    for (int i = 0; i < 4; i++) out[i] = -1;
    if (!family || !family[0]) return 0;
    const FontEntry *base = fonts_pick(family, 0, 0);
    if (!base) base = fonts_by_ps(family);
    if (!base) return -1;
    for (int i = 0; i < 4; i++) {
        const FontEntry *e = fonts_pick(base->family, i & 1, i >> 1 & 1);
        out[i] = e ? text_font_load(e->path, e->index) : -1;
    }
    if (out[0] < 0) return -1;
    for (int i = 1; i < 4; i++) if (out[i] < 0) out[i] = out[0];
    return 0;
}

/* Terminal font and the "non-ASCII" one (by family, e.g. "Fira Code"; "" = the bundled one). */
int term_set_fonts(TermProfile *p, const char *family, const char *non_ascii) {
    int ok = 0;
    if (strcmp(family, p->font_family)) {
        if (load_family(family, p->font)) ok = -1;
        snprintf(p->font_family, sizeof p->font_family, "%s", ok ? "" : family);
    }
    if (strcmp(non_ascii, p->na_family)) {
        if (load_family(non_ascii, p->font_na)) ok = -1;
        snprintf(p->na_family, sizeof p->na_family, "%s", p->font_na[0] >= 0 ? non_ascii : "");
    }
    return ok;
}

/* Color emoji font: looked up and mapped only the first time it is needed. */
int term_color_font(void) {
    static int font = -2;
    if (font == -2) {
        const FontEntry *e = fonts_color();
        font = e ? text_font_load(e->path, e->index) : -1;
    }
    return font;
}

/* Fallback for characters the font lacks: JetBrains Mono, DejaVu Sans Mono,
 * Noto Sans Mono, DejaVu Sans, Noto Sans Symbols and finally Inter. */
int term_fallback_font(int i) {
    static int list[8], n = -1;
    if (n < 0) {
        n = 0;
        list[n++] = term_mono_font(0, 0);
        static const char *const fams[] = { "DejaVu Sans Mono", "Noto Sans Mono", "DejaVu Sans", "Noto Sans Symbols", "Noto Sans Symbols 2" };
        for (size_t k = 0; k < sizeof fams / sizeof *fams; k++) {
            const FontEntry *e = fonts_pick(fams[k], 0, 0);
            int f = e ? text_font_load(e->path, e->index) : -1;
            if (f >= 0) list[n++] = f;
        }
        list[n++] = 0;      /* Inter */
    }
    return i < n ? list[i] : -1;
}

void term_metrics(const TermProfile *p, float *cw, float *ch, float *baseline) {
    int f = term_profile_font(p, 0, 0, 0);
    float asc, desc;
    text_font_vmetrics(f, p->font_size, &asc, &desc);
    float w = roundf(text_glyph('M', f, p->font_size)->adv);
    float nat = asc - desc;
    float h = roundf(nat * (p->line_spacing > 0.5f ? p->line_spacing : 1));
    *cw = w < 1 ? 1 : w;
    *ch = h < 1 ? 1 : h;
    *baseline = roundf((*ch - nat) / 2 + asc);
}

/* ---- session clipboard ---- */

static unsigned clip_gen;

const char *term_clipboard(void) { return clip ? clip : ""; }
unsigned term_clipboard_gen(void) { return clip_gen; }      /* changes with each copy (Wayland) */

static char clip_origin[64] = "terminal";
static int clip_sensitive;

void term_set_clipboard(const char *s, size_t n) {
    char *c = malloc(n + 1);
    if (!c) return;
    memcpy(c, s, n);
    c[n] = 0;
    if (clip) explicit_bzero(clip, strlen(clip));     /* the previous copy may have been a password */
    free(clip);
    clip = c;
    clip_gen++;
    snprintf(clip_origin, sizeof clip_origin, "terminal");
    clip_sensitive = 0;
}

void term_clipboard_mark(const char *origin, int sensitive) {
    snprintf(clip_origin, sizeof clip_origin, "%s", origin && origin[0] ? origin : "lordos");
    clip_sensitive = sensitive != 0;
}
const char *term_clipboard_origin(void) { return clip_origin; }
int term_clipboard_sensitive(void) { return clip_sensitive; }

/* ---- sessions ---- */

Session *term_get(int id) {
    for (int i = 0; i < TERM_MAX; i++)
        if (sessions[i].used && sessions[i].id == id) return &sessions[i];
    return NULL;
}

static void respond(Term *t, const char *s, size_t n) { term_input(t->user, s, n); }
static void tmux_cb(Term *t, int kind, const char *line, size_t n);

Session *term_create(const char *cwd, int scrollback) {
    Session *s = NULL;
    for (int i = 0; i < TERM_MAX && !s; i++) if (!sessions[i].used) s = &sessions[i];
    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    term_profile_defaults(&s->prof);
    if (scrollback > 0) s->prof.scrollback = scrollback > 1000000 ? 1000000 : scrollback;
    s->id = next_id++;
    if (vt_init(&s->t, 80, 24, s->prof.scrollback, &s->prof)) { vt_free(&s->t); return NULL; }
    s->t.respond = respond;
    s->t.tmux = tmux_cb;
    s->t.user = s;
    s->pid = pty_spawn(cwd, 80, 24, s->id, &s->fd);
    if (s->pid < 0) { vt_free(&s->t); return NULL; }
    s->pty_cols = 80;
    s->pty_rows = 24;
    s->ctrl = hold_spawn(s->fd, s->pid, s->key, sizeof s->key);
    s->used = 1;
    s->alive = 1;
    s->blink_on = 1;
    s->replay = -1;
    return s;
}

Session *term_create_fd(int fd, int scrollback) {
    Session *s = NULL;
    for (int i = 0; i < TERM_MAX && !s; i++) if (!sessions[i].used) s = &sessions[i];
    if (!s || fd < 0) return NULL;
    memset(s, 0, sizeof *s);
    term_profile_defaults(&s->prof);
    if (scrollback > 0) s->prof.scrollback = scrollback > 1000000 ? 1000000 : scrollback;
    s->id = next_id++;
    if (vt_init(&s->t, 80, 24, s->prof.scrollback, &s->prof)) { vt_free(&s->t); return NULL; }
    s->t.respond = respond;
    s->t.tmux = tmux_cb;
    s->t.user = s;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    s->fd = fd;
    s->pid = 0;                 /* no process: never signalled, never waited for */
    s->ctrl = -1;
    s->used = 1;
    s->alive = 1;
    s->blink_on = 1;
    s->replay = -1;
    return s;
}

static void reap(void) {
    int k = 0;
    for (int i = 0; i < norphans; i++)
        if (waitpid(orphans[i], NULL, WNOHANG) == 0) orphans[k++] = orphans[i];
    norphans = k;
}

static void child_exited(Session *s) {
    if (!s->alive) return;
    int st = 0;
    pid_t r = s->pid > 0 ? waitpid(s->pid, &st, WNOHANG) : 0;
    if (s->pid > 0 && r == s->pid) s->exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    else if (s->pid > 0 && norphans < (int)(sizeof orphans / sizeof *orphans)) orphans[norphans++] = s->pid;
    close(s->fd);
    s->fd = -1;
    s->alive = 0;
    if (s->ctrl >= 0) { close(s->ctrl); s->ctrl = -1; }   /* the holder exits on its own (POLLHUP) */
    s->t.events |= EV_EXIT;
}

void term_close(Session *s) {
    if (!s || !s->used) return;
    tcap_drop(s->id);                       /* a typed password waiting to be saved goes with the session */
    if (s->ctrl >= 0) {         /* the holder releases the PTY and leaves */
        if (write(s->ctrl, "q", 1) < 0) {}
        close(s->ctrl);
        s->ctrl = -1;
    }
    if (s->key[0]) { char st[300]; hold_state_path(s->key, st, sizeof st); unlink(st); }
    if (s->alive && !s->gateway && s->pid <= 0) close(s->fd);          /* a stream session: only its descriptor */
    else if (s->alive && !s->gateway) {
        kill(-s->pid, SIGHUP);
        kill(s->pid, SIGHUP);
        close(s->fd);
        if (waitpid(s->pid, NULL, WNOHANG) != s->pid && norphans < (int)(sizeof orphans / sizeof *orphans))
            orphans[norphans++] = s->pid;
    }
    vt_free(&s->t);
    free(s->out);
    free(s->tlines);
    free(s->pend);
    for (int i = 0; i < s->nframes; i++) free(s->frames[(s->frame_head + i) % REPLAY_FRAMES].cells);
    free(s->frames);
    if (s->log) fclose(s->log);
    memset(s, 0, sizeof *s);
}

/* Theme change: the nodes go away but the shells stay alive; the new theme's Terminal
 * app recovers them with lord.term.list(). */
void term_detach_all(void) {
    for (int i = 0; i < TERM_MAX; i++) sessions[i].node = 0;
}

int term_list(int *ids, int max) {
    int n = 0;
    for (int i = 0; i < TERM_MAX && n < max; i++) if (sessions[i].used) ids[n++] = sessions[i].id;
    return n;
}

/* Saves screen and history (up to 5000 lines) to recover them on the next startup. */
static void save_state(Session *s) {
    char path[300], tmp[310];
    hold_state_path(s->key, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    size_t n;
    char *data = vt_serialize(&s->t, 5000, &n);
    FILE *f = data ? fopen(tmp, "wb") : NULL;
    if (f) {
        fchmod(fileno(f), 0600);
        /* header: version, size, title and directory */
        fprintf(f, "LORDSTATE 1 %d %d %d\n%s\n%s\n", s->t.cols, s->t.rows, s->tmux, s->t.title, s->t.cwd);
        int ok = fwrite(data, 1, n, f) == n;
        if (fclose(f) == 0 && ok) rename(tmp, path); else unlink(tmp);
    }
    free(data);
}

/* keep = 1 (restart, signal, controlled crash): the shells stay in their holders.
 * keep = 0 (log out): all of them are closed. */
void term_shutdown(int keep) {
    for (int i = 0; i < TERM_MAX; i++) {
        Session *s = &sessions[i];
        if (!s->used) continue;
        if (keep && s->alive && s->ctrl >= 0 && s->key[0]) {
            save_state(s);
            close(s->ctrl);
            close(s->fd);
            s->ctrl = -1;
            s->alive = 0;
            vt_free(&s->t);
            free(s->out);
            for (int k = 0; k < s->nframes; k++) free(s->frames[(s->frame_head + k) % REPLAY_FRAMES].cells);
            free(s->frames);
            if (s->log) fclose(s->log);
            memset(s, 0, sizeof *s);
        } else term_close(s);
    }
    free(clip);
    clip = NULL;
}

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap);
    size_t r;
    while (b && (r = fread(b + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) { if (cap >= (64u << 20)) break; char *nb = realloc(b, cap *= 2); if (!nb) { free(b); b = NULL; } else b = nb; }
    }
    fclose(f);
    if (b) *len = n;
    return b;
}

static void adopt(int fd, int ctrl, pid_t pid, const char *key) {
    Session *s = NULL;
    for (int i = 0; i < TERM_MAX && !s; i++) if (!sessions[i].used) s = &sessions[i];
    if (!s) { close(fd); close(ctrl); return; }
    memset(s, 0, sizeof *s);
    term_profile_defaults(&s->prof);
    s->id = next_id++;
    int cols = 80, rows = 24, tmux = 0;
    char path[300];
    hold_state_path(key, path, sizeof path);
    size_t len = 0;
    char *st = read_all(path, &len), *body = NULL, *title = NULL, *cwd = NULL;
    if (st && len > 10 && sscanf(st, "LORDSTATE 1 %d %d %d", &cols, &rows, &tmux) >= 2) {
        title = memchr(st, '\n', len);
        if (title) { *title++ = 0; cwd = memchr(title, '\n', len - (size_t)(title - st)); }
        if (cwd) { *cwd++ = 0; body = memchr(cwd, '\n', len - (size_t)(cwd - st)); }
        if (body) *body++ = 0;
    }
    if (cols < 2 || cols > TERM_MAX_COLS) cols = 80;
    if (rows < 1 || rows > TERM_MAX_ROWS) rows = 24;
    if (vt_init(&s->t, cols, rows, s->prof.scrollback, &s->prof)) { vt_free(&s->t); free(st); close(fd); close(ctrl); return; }
    s->t.respond = NULL;        /* what is replayed must not reply to the shell */
    if (body) {
        vt_write(&s->t, (const uint8_t *)body, len - (size_t)(body - st));
        if (title) snprintf(s->t.title, sizeof s->t.title, "%s", title);
        if (cwd) snprintf(s->t.cwd, sizeof s->t.cwd, "%s", cwd);
    }
    s->t.events = 0;
    free(st);
    s->t.respond = respond;
    s->t.tmux = tmux_cb;
    s->t.user = s;
    s->fd = fd;
    s->ctrl = ctrl;
    s->pid = pid;
    snprintf(s->key, sizeof s->key, "%s", key);
    s->used = 1;
    s->alive = 1;
    s->restored = 1;
    /* it was still in tmux -CC: the parser goes back to control mode and the app asks for the state again */
    if (tmux) { vt_enter_tmux(&s->t); s->tmux = 1; }
    s->blink_on = 1;
    s->replay = -1;
    /* the foreground program redraws itself with whatever size the new window has */
    pty_resize(s->fd, cols, rows, 0, 0);
    s->pty_cols = cols;
    s->pty_rows = rows;
}

void term_restore(void) { hold_restore(adopt); }

/* The pty's size is the grid's and nothing else's: given when the grid changes (term_paint), and with check at
 * most once a second read back (TIOCGWINSZ) and given again when it isn't the grid's. A pty whose size a program
 * or another process changed behind the Terminal's back would otherwise keep it until the window is resized:
 * the status bar said 260×56 while Claude Code drew in 80 columns. Not for a tmux pane (its size goes to tmux,
 * EV_RESIZE) or a session without a pty. */
void term_size_sync(Session *s, int check) {
    if (!s || !s->alive || s->gateway || s->fd < 0) return;
    Term *t = &s->t;
    int pw = t->cols * (int)t->cell_w, ph = t->rows * (int)t->cell_h;
    if (s->pty_cols != t->cols || s->pty_rows != t->rows) {
        pty_resize(s->fd, t->cols, t->rows, pw, ph);
        s->pty_cols = t->cols;
        s->pty_rows = t->rows;
        s->size_checked = now_ms();
        return;
    }
    if (!check) return;
    double now = now_ms();
    if (now - s->size_checked < 1000) return;
    s->size_checked = now;
    pty_sync_size(s->fd, t->cols, t->rows, pw, ph);
}

static void flush_out(Session *s) {
    while (s->out_len) {
        ssize_t w = write(s->fd, s->out, s->out_len);
        if (w <= 0) break;
        memmove(s->out, s->out + w, s->out_len - (size_t)w);
        s->out_len -= (size_t)w;
        explicit_bzero(s->out + s->out_len, (size_t)w);    /* what was typed (a password from Passwords) isn't left behind */
    }
}

static void tmux_keys(Session *v, const char *data, size_t n);

void term_input(Session *s, const char *data, size_t n) {
    if (!s || !s->alive || !n) return;
    if (s->typing && tcap_pending()) tcap_keys(s->id, data, n, now_ms());     /* Save to Passwords (tcapture.c) */
    if (s->gateway) { tmux_keys(s, data, n); return; }
    if (!s->out_len) {
        ssize_t w = write(s->fd, data, n);
        if (w > 0) { data += w; n -= (size_t)w; }
        if (!n) return;
    }
    if (s->out_len + n > OUT_MAX) return;
    if (s->out_len + n > s->out_cap) {
        size_t cap = s->out_cap ? s->out_cap : 4096;
        while (cap < s->out_len + n) cap *= 2;
        char *o = realloc(s->out, cap);
        if (!o) return;
        s->out = o;
        s->out_cap = cap;
    }
    memcpy(s->out + s->out_len, data, n);
    s->out_len += n;
}

int term_pollfds(struct pollfd *p, int max, int *ids) {
    int n = 0;
    for (int i = 0; i < TERM_MAX && n < max; i++) {
        Session *s = &sessions[i];
        if (!s->used || !s->alive || s->fd < 0) continue;
        p[n] = (struct pollfd){ s->fd, (short)(POLLIN | (s->out_len ? POLLOUT : 0)), 0 };
        ids[n++] = s->id;
    }
    return n;
}

void term_dispatch(int id, short revents) {
    Session *s = term_get(id);
    if (!s || !s->alive) return;
    if (revents & POLLOUT) flush_out(s);
    if (!(revents & (POLLIN | POLLHUP | POLLERR))) return;
    static uint8_t buf[READ_CHUNK];
    size_t total = 0;
    while (total < READ_BUDGET) {
        ssize_t r = read(s->fd, buf, sizeof buf);
        if (r > 0) {
            vt_write(&s->t, buf, (size_t)r);
            if (tcap_pending()) tcap_output(s->id, buf, (size_t)r);      /* a password rejected after Enter */
            if (s->log) fwrite(buf, 1, (size_t)r, s->log);
            total += (size_t)r;
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) break;
        child_exited(s);     /* EIO or EOF: the shell exited */
        break;
    }
    if (total) {
        term_size_sync(s, 1);       /* the program draws: for the size the grid has (at most once a second) */
        check_secret(s);
        if (s->log) fflush(s->log);
        s->screen_changed = 1;
        double now = now_ms();
        s->last_output = now;
        s->blink_on = 1;
        if (now - s->last_event >= 1000) { s->t.events |= EV_OUTPUT; s->last_event = now; }
    }
}

/* ---- a program asking for a password (Passwords in the Terminal, docs/TERMINAL.md) ----
 * Read from the pty itself, not guessed from the text: getpass(3) and its kin turn echo off and keep line
 * input. The prompt must also name a password, so a program that hides input for another reason (a raw
 * editor keeps ICANON off anyway) isn't offered one. */

static void check_secret(Session *s) {
    char prompt[sizeof s->secret_prompt];
    int on = term_asks_secret(s, prompt, sizeof prompt);
    s->secret_checked = now_ms();
    if (on == s->secret && (!on || !strcmp(prompt, s->secret_prompt))) return;
    if (on != s->secret) {                  /* Save to Passwords: what the program asks for, from its arguments */
        TcapTarget tg;
        if (!on) tcap_prompt(s->id, 0, NULL, s->secret_checked);
        else if (!term_traced() && !term_secret_target(s, prompt, &tg, NULL, 0)) tcap_prompt(s->id, 1, &tg, s->secret_checked);
    }
    s->secret = on;
    snprintf(s->secret_prompt, sizeof s->secret_prompt, "%s", on ? prompt : "");
    s->t.events |= EV_SECRET;
}

/* "Save to Passwords?" for JS: the target and a token, never the value (it stays in tcapture.c) */
static void json_put(char *o, size_t n, size_t *k, const char *key, const char *v) {
    if (*k + 32 >= n) return;               /* no room: the caller sees the object unfinished and drops it */
    int w = snprintf(o + *k, n - *k, "%s\"%s\":\"", *k > 1 ? "," : "", key);
    if (w < 0 || (size_t)w >= n - *k) { *k = n; return; }
    *k += (size_t)w;
    for (; *v && *k + 8 < n; v++) {
        unsigned char c = (unsigned char)*v;
        if (c == '"' || c == '\\') { o[(*k)++] = '\\'; o[(*k)++] = (char)c; }
        else if (c >= 0x20) o[(*k)++] = (char)c;
    }
    if (*v || *k + 1 >= n) { *k = n; return; }    /* cut short: never a truncated value */
    o[(*k)++] = '"';
    o[*k] = 0;
}

static void save_offer(int sid, unsigned token, const TcapTarget *t) {
    Session *s = term_get(sid);
    if (!s) { tcap_drop(sid); return; }
    static const char *const kinds[] = { "", "ssh", "database", "git", "other" };
    char *o = s->save_offer, tok[16];
    size_t n = sizeof s->save_offer, k = 1;
    o[0] = '{';
    snprintf(tok, sizeof tok, "%u", token);
    json_put(o, n, &k, "token", tok);
    json_put(o, n, &k, "kind", kinds[t->kind >= 0 && t->kind <= TK_OTHER ? t->kind : 0]);
    json_put(o, n, &k, "program", t->program);
    json_put(o, n, &k, "engine", t->engine);
    json_put(o, n, &k, "host", t->host);
    json_put(o, n, &k, "user", t->user);
    json_put(o, n, &k, "port", t->port);
    if (k + 2 < n) { o[k++] = '}'; o[k] = 0; } else { tcap_drop(sid); return; }
    s->t.events |= EV_SAVE;
}

void term_save_done(Session *s, const char *result) {
    if (!s) return;
    snprintf(s->save_result, sizeof s->save_result, "%s", result);
    s->t.events |= EV_SAVED;
}

void term_fill_done(Session *s, const char *result) {
    if (!s) return;
    snprintf(s->fill_result, sizeof s->fill_result, "%s", result);
    s->t.events |= EV_FILL;
}

/* ---- tmux -CC (control mode) ----
 *
 * The session where `tmux -CC` runs is the "gateway": protocol lines arrive through it.
 * The output of each pane (%output %N …) goes straight to its virtual session, without
 * going through JS; the rest (%begin/%end, %layout-change, %window-add…) is collected and
 * delivered as a "tmux" event so the app builds tabs and panes. What is written in a
 * virtual session goes back to tmux as `send-keys -H`. */

static void buf_add(char **b, size_t *n, size_t *cap, const char *d, size_t len) {
    if (*n + len + 1 > *cap) {
        size_t c = *cap ? *cap : 4096;
        while (c < *n + len + 1) c *= 2;
        if (c > (64u << 20)) return;
        char *nb = realloc(*b, c);
        if (!nb) return;
        *b = nb;
        *cap = c;
    }
    memcpy(*b + *n, d, len);
    *n += len;
    (*b)[*n] = 0;
}

static Session *tmux_find(int gateway, int pane) {
    for (int i = 0; i < TERM_MAX; i++)
        if (sessions[i].used && sessions[i].gateway == gateway && sessions[i].pane == pane) return &sessions[i];
    return NULL;
}

void term_tmux_send(Session *g, const char *cmd, size_t n) {
    if (!g || !g->tmux || !g->alive) return;
    if (!g->out_len) {      /* straight to the gateway's PTY (tmux reads commands from stdin) */
        ssize_t w = write(g->fd, cmd, n);
        if (w > 0) { cmd += w; n -= (size_t)w; }
    }
    if (n) buf_add(&g->out, &g->out_len, &g->out_cap, cmd, n);
}

static void tmux_keys(Session *v, const char *data, size_t n) {
    Session *g = term_get(v->gateway);
    if (!g || !g->tmux) return;
    char line[32 + 3 * 256];
    for (size_t i = 0; i < n;) {
        int k = snprintf(line, sizeof line, "send-keys -t %%%d -H", v->pane);
        for (int c = 0; c < 256 && i < n; c++, i++) k += snprintf(line + k, sizeof line - (size_t)k, " %02x", (unsigned char)data[i]);
        line[k++] = '\n';
        term_tmux_send(g, line, (size_t)k);
    }
}

/* %output: bytes < 0x20 and the backslash come as \ooo (octal). */
static size_t unescape(const char *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '\\' && i + 3 < n && in[i + 1] >= '0' && in[i + 1] <= '3') {
            out[o++] = (char)((in[i + 1] - '0') * 64 + (in[i + 2] - '0') * 8 + (in[i + 3] - '0'));
            i += 3;
        } else out[o++] = in[i];
    }
    return o;
}

static void virtual_output(Session *v, const char *d, size_t n) {
    if (v->paused) { buf_add(&v->pend, &v->pend_n, &v->pend_cap, d, n); return; }
    vt_write(&v->t, (const uint8_t *)d, n);
    v->screen_changed = 1;
    double now = now_ms();
    v->last_output = now;
    v->blink_on = 1;
    if (now - v->last_event >= 1000) { v->t.events |= EV_OUTPUT; v->last_event = now; }
}

static void tmux_cb(Term *t, int kind, const char *line, size_t n) {
    Session *g = t->user;
    if (kind == 1) { g->tmux = 1; g->t.events |= EV_TMUX_START; return; }
    if (kind == 2) {
        g->tmux = 0;
        g->t.events |= EV_TMUX_END;
        return;
    }
    if (n > 9 && !strncmp(line, "%output %", 9)) {
        char *end;
        long pane = strtol(line + 9, &end, 10);
        Session *v = *end == ' ' ? tmux_find(g->id, (int)pane) : NULL;
        if (!v) return;
        end++;
        size_t len = n - (size_t)(end - line);
        char *raw = malloc(len + 1);
        if (!raw) return;
        virtual_output(v, raw, unescape(end, len, raw));
        free(raw);
        return;
    }
    buf_add(&g->tlines, &g->tl_n, &g->tl_cap, line, n);
    buf_add(&g->tlines, &g->tl_n, &g->tl_cap, "\n", 1);
    g->t.events |= EV_TMUX;
}

Session *term_tmux_pane(Session *g, int pane) {
    if (!g || !g->tmux || pane < 0) return NULL;
    Session *s = tmux_find(g->id, pane);
    if (s) return s;
    for (int i = 0; i < TERM_MAX && !s; i++) if (!sessions[i].used) s = &sessions[i];
    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    term_profile_defaults(&s->prof);
    s->id = next_id++;
    if (vt_init(&s->t, 80, 24, s->prof.scrollback, &s->prof)) { vt_free(&s->t); return NULL; }
    s->t.respond = respond;
    s->t.user = s;
    s->fd = -1;
    s->ctrl = -1;
    s->gateway = g->id;
    s->pane = pane;
    s->paused = 1;
    s->used = 1;
    s->alive = 1;
    s->blink_on = 1;
    s->replay = -1;
    return s;
}

/* Initial content (capture-pane) and then the output that arrived meanwhile. */
void term_feed(Session *s, const char *data, size_t n) {
    if (!s || vt_in_tmux(&s->t)) return;
    vt_write(&s->t, (const uint8_t *)data, n);
    s->screen_changed = 1;
}

void term_tmux_ready(Session *s) {
    if (!s || !s->paused) return;
    s->paused = 0;
    if (s->pend_n) virtual_output(s, s->pend, s->pend_n);
    free(s->pend);
    s->pend = NULL;
    s->pend_n = s->pend_cap = 0;
}

/* ---- damage: which part of the screen must be repainted ---- */

static int cursor_blinks(const Session *s) {
    int shape = s->t.cursor_shape;
    if (shape >= 1 && shape <= 6) return shape % 2 == 1;
    return s->prof.cursor_blink || (s->t.modes & M_CURSOR_BLINK);
}

static double wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* Saves the current screen for Instant Replay (at most every 100 ms). */
static void capture_frame(Session *s, double now) {
    Term *t = &s->t;
    if (s->replay >= 0 || !s->screen_changed || now - s->last_frame < REPLAY_EVERY_MS) return;
    if (!s->frames && !(s->frames = calloc(REPLAY_FRAMES, sizeof *s->frames))) return;
    size_t need = (size_t)t->cols * t->rows * sizeof(Cell);
    while (s->nframes && (s->nframes >= REPLAY_FRAMES || s->frame_bytes + need > REPLAY_BYTES)) {
        Frame *old = &s->frames[s->frame_head];
        s->frame_bytes -= (size_t)old->cols * old->rows * sizeof(Cell);
        free(old->cells);
        old->cells = NULL;
        s->frame_head = (s->frame_head + 1) % REPLAY_FRAMES;
        s->nframes--;
    }
    Cell *cells = calloc((size_t)t->cols * t->rows, sizeof *cells);
    if (!cells) return;
    for (int r = 0; r < t->rows; r++) {
        int saved = t->view;
        t->view = 0;
        Line *l = vt_view_line(t, r, NULL);
        t->view = saved;
        if (l) memcpy(cells + (size_t)r * t->cols, l->c, (size_t)(l->len < t->cols ? l->len : t->cols) * sizeof *cells);
    }
    Frame *f = &s->frames[(s->frame_head + s->nframes) % REPLAY_FRAMES];
    *f = (Frame){ wall_ms(), t->cols, t->rows, t->cx, t->cy, cells };
    s->nframes++;
    s->frame_bytes += need;
    s->last_frame = now;
    s->screen_changed = 0;
}

int term_replay(Session *s, int index) {
    if (index < 0 || !s->nframes) { s->replay = -1; s->t.all_dirty = 1; return -1; }
    s->replay = index >= s->nframes ? s->nframes - 1 : index;
    s->t.all_dirty = 1;
    return s->replay;
}

/* Session log in ~/.local/share/lordos/terminal (raw output). */
const char *term_log(Session *s, int on) {
    if (!on) { if (s->log) fclose(s->log); s->log = NULL; return NULL; }
    if (s->log) return s->log_path;
    const char *home = getenv("HOME");
    if (!home) return NULL;
    char dir[400];
    snprintf(dir, sizeof dir, "%s/.local", home); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.local/share", home); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.local/share/lordos", home); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.local/share/lordos/terminal", home); mkdir(dir, 0700);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    snprintf(s->log_path, sizeof s->log_path, "%s/%s-sesion%d.log", dir, stamp, s->id);
    s->log = fopen(s->log_path, "a");
    return s->log ? s->log_path : NULL;
}

/* User's documents folder: XDG_DOCUMENTS_DIR from ~/.config/user-dirs.dirs (with the
 * name in their language), or ~/Documentos / ~/Documents, or the home folder. */
static void documents_dir(const char *home, char *dir, size_t n) {
    struct stat st;
    char conf[400], line[512];
    snprintf(conf, sizeof conf, "%s/.config/user-dirs.dirs", home);
    FILE *f = fopen(conf, "r");
    while (f && fgets(line, sizeof line, f)) {
        const char *k = "XDG_DOCUMENTS_DIR=\"$HOME/";
        if (strncmp(line, k, strlen(k))) continue;
        char *v = line + strlen(k), *q = strchr(v, '"');
        if (!q || strstr(v, "..")) continue;
        *q = 0;
        snprintf(dir, n, "%s/%s", home, v);
        if (!stat(dir, &st) && S_ISDIR(st.st_mode)) { fclose(f); return; }
    }
    if (f) fclose(f);
    snprintf(dir, n, "%s/Documentos", home);
    if (stat(dir, &st) || !S_ISDIR(st.st_mode)) snprintf(dir, n, "%s/Documents", home);
    if (stat(dir, &st) || !S_ISDIR(st.st_mode)) snprintf(dir, n, "%s", home);
}

/* Saves all the text (history + screen) to the documents folder. */
int term_export(Session *s, char *path, size_t size) {
    const char *home = getenv("HOME");
    if (!home) return -1;
    char dir[600];
    documents_dir(home, dir, sizeof dir);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    snprintf(path, size, "%s/terminal-%s.txt", dir, stamp);
    long long first = vt_first_abs(&s->t);
    char *txt = vt_text_range(&s->t, first, first + vt_total_lines(&s->t) - 1);
    FILE *f = txt ? fopen(path, "w") : NULL;
    if (f) { fputs(txt, f); fputc('\n', f); fclose(f); }
    free(txt);
    return f ? 0 : -1;
}

int term_damage(void) {
    int any = 0;
    double now = now_ms();
    for (int i = 0; i < TERM_MAX; i++) {
        Session *s = &sessions[i];
        if (!s->used) continue;
        Term *t = &s->t;
        if ((t->modes & M_SYNC) && now < t->sync_until) continue;
        capture_frame(s, now);
        Node *n = node_get(s->node);
        int visible = n && n->type == NODE_TERM && n->term == s->id && n->prev_box.x1 > n->prev_box.x0 && s->gw > 0;
        if (t->cx != s->drawn_cx || t->cy != s->drawn_cy) { vt_mark_dirty(t, s->drawn_cy); vt_mark_dirty(t, t->cy); }
        int lo = t->rows, hi = -1;
        for (int r = 0; r < t->rows; r++) if (t->dirty[r]) { if (r < lo) lo = r; hi = r; }
        if (visible && (t->all_dirty || hi >= 0)) {
            float cw, ch, base;
            term_metrics(&s->prof, &cw, &ch, &base);
            if (t->all_dirty || t->view > 0 || s->prof.bg_opacity < 1) damage_add((Box){ s->gx, s->gy, s->gx + s->gw, s->gy + s->gh });
            else {
                float y0 = s->gy + s->prof.padding + lo * ch, y1 = s->gy + s->prof.padding + (hi + 1) * ch;
                damage_add((Box){ floorf(s->gx), floorf(y0 - 2), ceilf(s->gx + s->gw), ceilf(y1 + 2) });
            }
            any = 1;
        }
        t->all_dirty = 0;
        memset(t->dirty, 0, (size_t)t->rows);
    }
    return any;
}

double term_timeout(double now) {
    double best = tcap_pending() ? 250 : -1;      /* a typed password's verdict, its offer's expiry */
    for (int i = 0; i < TERM_MAX; i++) {
        Session *s = &sessions[i];
        if (!s->used) continue;
        if ((s->t.modes & M_SYNC) && s->t.sync_until > now) {
            double d = s->t.sync_until - now;
            if (best < 0 || d < best) best = d;
        }
        if (!s->node || !s->t.focused || !(cursor_blinks(s) || s->prof.blink_text)) continue;
        double d = BLINK_MS - fmod(now - fmax(s->last_output, s->last_input), BLINK_MS);
        if (best < 0 || d < best) best = d;
    }
    for (int i = 0; i < TERM_MAX; i++) {
        Session *s = &sessions[i];
        if (s->used && s->last_output > s->last_event) {
            double d = s->last_event + 1000 - now;
            if (d < 0) d = 0;
            if (best < 0 || d < best) best = d;
        }
    }
    if (norphans && (best < 0 || best > 1000)) best = 1000;
    return best;
}

void term_tick(double now) {
    if (norphans) reap();
    if (tcap_pending()) tcap_tick(now, save_offer);         /* Save to Passwords: verdicts and expiry */
    for (int i = 0; i < TERM_MAX; i++) {
        Session *s = &sessions[i];
        if (!s->used) continue;
        if ((s->t.modes & M_SYNC) && s->t.sync_until && now >= s->t.sync_until) {
            s->t.modes &= ~(uint32_t)M_SYNC;
            s->t.all_dirty = 1;
        }
        /* Last activity notice of a burst (notices go out at most once per second). */
        if (s->last_output > s->last_event && now - s->last_event >= 1000) { s->t.events |= EV_OUTPUT; s->last_event = now; }
        if (s->secret && now - s->secret_checked >= 300) check_secret(s);   /* echo back on without output */
        if (!(cursor_blinks(s) || s->prof.blink_text)) { s->blink_on = 1; continue; }
        /* While output arrives the cursor stays on; afterwards it blinks. */
        int on = ((long)((now - fmax(s->last_output, s->last_input)) / BLINK_MS) % 2) == 0;
        if (on != s->blink_on) {
            s->blink_on = on;
            vt_mark_dirty(&s->t, s->t.cy);
            if (s->prof.blink_text) s->t.all_dirty = 1;
        }
    }
}

void term_events(TermEventFn fn) {
    for (int i = 0; i < TERM_MAX; i++) {
        Session *s = &sessions[i];
        if (!s->used || !s->t.events) continue;
        uint32_t e = s->t.events;
        s->t.events = 0;
        int id = s->id;
        char code[16];
        if (e & EV_TITLE) fn(id, "title", s->t.title);
        if (e & EV_CWD) fn(id, "cwd", s->t.cwd);
        if (e & EV_BELL) fn(id, "bell", "");
        if (e & EV_BADGE) fn(id, "badge", s->t.badge);
        if (e & EV_NOTIFY) fn(id, "notify", s->t.notify);
        if (e & EV_PROFILE) fn(id, "profile", s->t.profile_req);
        if (e & EV_ATTENTION) fn(id, "attention", "");
        if (e & EV_COPY) fn(id, "copy", "");
        if (e & EV_COMMAND) fn(id, "command", s->t.command);
        if (e & EV_RUN) fn(id, "run", s->t.run_cmd);
        if (e & EV_HOST) fn(id, "host", s->t.host);
        if (e & EV_TMUX_START) fn(id, "tmux-start", "");
        if ((e & EV_TMUX) && s->tl_n) {
            char *lines = s->tlines;
            s->tlines = NULL;
            s->tl_n = s->tl_cap = 0;
            fn(id, "tmux", lines);
            free(lines);
            if (!term_get(id)) continue;
        }
        if (e & EV_RESIZE) { snprintf(code, sizeof code, "%d %d", s->t.cols, s->t.rows); fn(id, "resize", code); }
        if (e & EV_TMUX_END) fn(id, "tmux-end", "");
        if (e & EV_OUTPUT) fn(id, "output", "");
        if (e & EV_SECRET) fn(id, "secret", s->secret ? s->secret_prompt : "");
        if (e & EV_FILL) fn(id, "fill", s->fill_result);
        if (e & EV_SAVE) fn(id, "save", s->save_offer);
        if (e & EV_SAVED) fn(id, "saved", s->save_result);
        if (e & EV_EXIT) { snprintf(code, sizeof code, "%d", s->exit_code); fn(id, "exit", code); }
        if (!term_get(id)) continue;    /* JS may have closed the session in a callback */
    }
}

/* ---- keyboard ---- */

enum { MOD_SHIFT = 1, MOD_ALT = 2, MOD_CTRL = 4, MOD_META = 8 };

static void send(Session *s, const char *str) { term_input(s, str, strlen(str)); }

static void sendf(Session *s, const char *fmt, int a, int b) {
    char buf[32];
    snprintf(buf, sizeof buf, fmt, a, b);
    send(s, buf);
}

static int ctrl_code(int c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 1;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
    switch (c) {
    case '@': case ' ': case '2': return 0;
    case '[': case '3': return 27;
    case '\\': case '4': return 28;
    case ']': case '5': return 29;
    case '^': case '6': return 30;
    case '_': case '-': case '7': case '/': return 31;
    case '8': case '?': return 127;
    default: return -1;
    }
}

static void key_input(Session *s, const char *key, const char *chr, const char *base, int mods);
void term_key(Session *s, const char *key, const char *chr, const char *base, int mods) {
    s->typing = 1;                          /* what goes out from here is the user's (tcapture.c) */
    key_input(s, key, chr, base, mods);
    s->typing = 0;
}

static void key_input(Session *s, const char *key, const char *chr, const char *base, int mods) {
    Term *t = &s->t;
    if (t->view) { t->view = 0; t->all_dirty = 1; }
    s->last_input = now_ms();       /* the cursor stops blinking while typing */
    s->blink_on = 1;
    int m = 1 + (mods & MOD_SHIFT ? 1 : 0) + (mods & MOD_ALT ? 2 : 0) + (mods & MOD_CTRL ? 4 : 0);
    static const struct { const char *name; char fin; int num; } keys[] = {
        { "ArrowUp", 'A', 0 }, { "ArrowDown", 'B', 0 }, { "ArrowRight", 'C', 0 }, { "ArrowLeft", 'D', 0 },
        { "Home", 'H', 0 }, { "End", 'F', 0 }, { "Insert", '~', 2 }, { "Delete", '~', 3 },
        { "PageUp", '~', 5 }, { "PageDown", '~', 6 }, { "F1", 'P', 0 }, { "F2", 'Q', 0 }, { "F3", 'R', 0 },
        { "F4", 'S', 0 }, { "F5", '~', 15 }, { "F6", '~', 17 }, { "F7", '~', 18 }, { "F8", '~', 19 },
        { "F9", '~', 20 }, { "F10", '~', 21 }, { "F11", '~', 23 }, { "F12", '~', 24 },
    };
    if (key && key[0]) {
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
            if (strcmp(key, keys[i].name)) continue;
            if (keys[i].num) {
                if (m > 1) sendf(s, "\033[%d;%d~", keys[i].num, m);
                else sendf(s, "\033[%d~", keys[i].num, 0);
            } else if (m > 1) {
                char buf[16];
                snprintf(buf, sizeof buf, "\033[1;%d%c", m, keys[i].fin);
                send(s, buf);
            } else {
                int ss3 = key[0] == 'F' || (t->modes & M_APPCURSOR);
                char buf[4] = { 033, ss3 ? 'O' : '[', keys[i].fin, 0 };
                send(s, buf);
            }
            return;
        }
        char seq[32];
        int sn = vt_key_encode(t, key, 0, m, seq, sizeof seq);   /* the protocol the program asked for */
        if (sn) { term_input(s, seq, (size_t)sn); return; }
        if (!strcmp(key, "Enter")) { send(s, mods & MOD_ALT ? "\033\r" : (t->modes & M_LNM) ? "\r\n" : "\r"); return; }
        if (!strcmp(key, "Tab")) { send(s, mods & MOD_SHIFT ? "\033[Z" : mods & MOD_ALT ? "\033\t" : "\t"); return; }
        if (!strcmp(key, "Backspace")) { send(s, mods & MOD_CTRL ? "\b" : mods & MOD_ALT ? "\033\177" : "\177"); return; }
        if (!strcmp(key, "Escape")) { send(s, "\033"); return; }
    }
    const char *text = chr && chr[0] ? chr : base;
    if (!text || !text[0]) return;
    if (mods & (MOD_CTRL | MOD_ALT) && base && base[0] && !base[1]) {
        char seq[32];
        int sn = vt_key_encode(t, NULL, (unsigned char)base[0], m, seq, sizeof seq);
        if (sn) { term_input(s, seq, (size_t)sn); return; }
    }
    if (mods & MOD_CTRL) {
        int c = ctrl_code((unsigned char)text[0]);
        if (c >= 0) {
            char buf[3] = { 033, (char)c, 0 };
            if (mods & MOD_ALT) term_input(s, buf, 2); else term_input(s, buf + 1, 1);
            return;
        }
    }
    if ((mods & MOD_ALT) && s->prof.alt_esc) send(s, "\033");
    send(s, text);
}

void term_paste(Session *s, const char *text) {
    size_t n = strlen(text);
    char *buf = malloc(n + 1);
    if (!buf) return;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\r' && text[i + 1] == '\n') continue;
        if (text[i] == '\033' && !strncmp(text + i, "\033[201~", 6)) { i += 5; continue; }
        buf[o++] = text[i] == '\n' ? '\r' : text[i];
    }
    if (s->t.modes & M_BRACKETED) send(s, "\033[200~");
    s->typing = 1;                          /* a pasted password is the user's too (tcapture.c) */
    term_input(s, buf, o);
    s->typing = 0;
    if (s->t.modes & M_BRACKETED) send(s, "\033[201~");
    explicit_bzero(buf, o);                 /* it may have been a password */
    free(buf);
    if (s->t.view) { s->t.view = 0; s->t.all_dirty = 1; }
}

void term_focus(Session *s, int on) {
    if (s->t.focused == on) return;
    s->t.focused = on;
    vt_mark_dirty(&s->t, s->t.cy);
    if (s->t.modes & M_FOCUS) send(s, on ? "\033[I" : "\033[O");
}

/* ---- mouse ---- */

void term_cell_at(Session *s, float lx, float ly, long long *line, int *col) {
    float cw, ch, base;
    term_metrics(&s->prof, &cw, &ch, &base);
    Term *t = &s->t;
    int c = (int)floorf((lx - s->prof.padding) / cw), r = (int)floorf((ly - s->prof.padding) / ch);
    *col = c < 0 ? 0 : c >= t->cols ? t->cols - 1 : c;
    if (r < 0) r = 0;
    if (r >= t->rows) r = t->rows - 1;
    Line *ln = vt_view_line(t, r, line);
    static int vis[TERM_MAX_COLS];
    static uint8_t lv[TERM_MAX_COLS];
    if (term_row_visual(&s->prof, t, ln, t->cols, vis, lv))     /* visual column → logical cell */
        for (int i = 0; i < t->cols; i++) if (vis[i] == *col) { *col = i; break; }
}

static void report(Session *s, int code, int col, int row, int release) {
    char buf[64];
    if (s->t.modes & M_MOUSE_SGR) {
        snprintf(buf, sizeof buf, "\033[<%d;%d;%d%c", code, col + 1, row + 1, release ? 'm' : 'M');
        send(s, buf);
        return;
    }
    if (release) code = (code & ~3) | 3;
    if (col + 33 > 255 || row + 33 > 255) return;
    char legacy[7] = { 033, '[', 'M', (char)(32 + code), (char)(33 + col), (char)(33 + row), 0 };
    term_input(s, legacy, 6);
}

static int mod_bits(int mods) {
    return (mods & MOD_SHIFT ? 4 : 0) | (mods & (MOD_ALT | MOD_META) ? 8 : 0) | (mods & MOD_CTRL ? 16 : 0);
}

static int reporting(const Session *s, int mods) {
    return (s->t.modes & M_MOUSE) && s->prof.mouse_reporting && !(mods & MOD_SHIFT);
}

/* type: "down" | "up" | "move". Returns 1 if the terminal app used it, 2 if the
 * selection changed, 3 if it was also copied to the clipboard. */
int term_mouse(Session *s, const char *type, float lx, float ly, int button, int mods) {
    Term *t = &s->t;
    long long line;
    int col;
    term_cell_at(s, lx, ly, &line, &col);
    int row = (int)(line - (vt_first_abs(t) + t->buf->count - t->rows - t->view));
    if (reporting(s, mods)) {
        int b = mod_bits(mods);
        if (!strcmp(type, "down")) {
            s->mouse_btn = button;
            s->dragging = 1;
            report(s, button | b, col, row, 0);
        } else if (!strcmp(type, "up")) {
            s->dragging = 0;
            if (!(t->modes & M_MOUSE_X10)) report(s, button | b, col, row, 1);
        } else if (col != s->click_col || line != s->click_line) {
            if (s->dragging && (t->modes & (M_MOUSE_DRAG | M_MOUSE_ANY))) report(s, 32 + s->mouse_btn + b, col, row, 0);
            else if (!s->dragging && (t->modes & M_MOUSE_ANY)) report(s, 35 + b, col, row, 0);
        }
        s->click_col = col;
        s->click_line = line;
        return 1;
    }
    if (!strcmp(type, "down")) {
        if (button == 1) { term_paste(s, term_clipboard()); return 1; }
        if (button != 0) return 0;
        double now = now_ms();
        s->clicks = now - s->last_click < 400 && line == s->click_line && abs(col - s->click_col) <= 1 ? s->clicks % 4 + 1 : 1;
        s->last_click = now;
        s->click_line = line;
        s->click_col = col;
        s->dragging = 1;
        if ((mods & MOD_SHIFT) && t->sel_active) vt_select_extend(t, line, col);
        else if (s->clicks == 1) { vt_select_clear(t); s->mouse_btn = -1; }
        else vt_select_start(t, line, col, s->clicks - 1, 0);
        return 2;
    }
    if (!strcmp(type, "move")) {
        if (!s->dragging) return 0;
        if (s->mouse_btn == -1 && (line != s->click_line || col != s->click_col)) {
            vt_select_start(t, s->click_line, s->click_col, 0, (mods & MOD_ALT) != 0);
            s->mouse_btn = 0;
        }
        if (ly < s->prof.padding) vt_scroll_view(t, 1);
        else if (ly > s->gh - s->prof.padding) vt_scroll_view(t, -1);
        if (t->sel_active) vt_select_extend(t, line, col);
        return 2;
    }
    /* up */
    if (!s->dragging) return 0;
    s->dragging = 0;
    if (t->sel_active && s->prof.copy_on_select) {
        char *txt = vt_selection_text(t);
        if (txt && txt[0]) { term_set_clipboard(txt, strlen(txt)); free(txt); return 3; }
        free(txt);
    }
    return 2;
}

void term_wheel(Session *s, int lines, float lx, float ly, int mods) {
    Term *t = &s->t;
    if (!lines) return;
    if (reporting(s, mods)) {
        long long line;
        int col;
        term_cell_at(s, lx, ly, &line, &col);
        int row = (int)(line - (vt_first_abs(t) + t->buf->count - t->rows - t->view));
        for (int i = 0; i < abs(lines) && i < 20; i++) report(s, (lines > 0 ? 64 : 65) | mod_bits(mods), col, row, 0);
        return;
    }
    if (t->buf == &t->alt && (t->modes & M_ALTSCROLL) && s->prof.alt_scroll) {
        const char *seq = lines > 0 ? ((t->modes & M_APPCURSOR) ? "\033OA" : "\033[A")
                                    : ((t->modes & M_APPCURSOR) ? "\033OB" : "\033[B");
        for (int i = 0; i < abs(lines) && i < 50; i++) send(s, seq);
        return;
    }
    vt_scroll_view(t, lines);
}
