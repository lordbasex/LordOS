/* term.h — LordOS terminal engine: xterm emulator (vt.c), PTY (pty.c),
 * sessions and events (term.c) and grid drawing (term_paint.c).
 *
 * Emulation and drawing are in C so that `cat` of large files or `htop` do not
 * go through JavaScript; the Terminal app (React) only handles tabs, panes, profiles
 * and shortcuts, and talks to this module through lord.term.* (ui/js_term.c). */
#ifndef LORD_TERM_H
#define LORD_TERM_H

#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>

#include "../canvas.h"

#define TERM_MAX 64
#define TERM_MAX_COLS 1000
#define TERM_MAX_ROWS 500
#define REPLAY_FRAMES 1200      /* Instant Replay frames per session */

/* Cell attributes. */
enum {
    A_BOLD = 1 << 0, A_DIM = 1 << 1, A_ITALIC = 1 << 2, A_UNDER = 1 << 3, A_UNDER2 = 1 << 4,
    A_CURLY = 1 << 5, A_BLINK = 1 << 6, A_INVERSE = 1 << 7, A_HIDDEN = 1 << 8, A_STRIKE = 1 << 9,
    A_WIDE = 1 << 10, A_WIDE_TAIL = 1 << 11, A_OVERLINE = 1 << 12,
};

/* Cell colors: 0 = the profile's; 0x01000000|index = palette; 0x02RRGGBB = direct color. */
#define COLOR_DEFAULT 0u
#define COLOR_IDX(i) (0x01000000u | (uint32_t)(i))
#define COLOR_RGB(c) (0x02000000u | ((uint32_t)(c) & 0xffffffu))

typedef struct { uint32_t cp, fg, bg; uint16_t attr, link; } Cell;

/* Graphemes of more than one character (e + combining accent, 👍🏽, 👨‍👩‍👧, flags): cp =
 * CP_CLUSTER | index into an internal global table (vt.c), shared by all sessions. */
#define CP_CLUSTER 0x80000000u
#define CLUSTER_LEN 12

/* Per-line marks (shell integration, OSC 133 / 1337). */
enum { LINE_WRAPPED = 1, LINE_PROMPT = 2, LINE_OUTPUT = 4, LINE_MARK = 8, LINE_FAIL = 16, LINE_OK = 32 };

typedef struct {
    Cell *c;
    uint16_t len, cap;          /* cells with content / allocated */
    uint8_t flags;
    uint32_t time;              /* seconds since the epoch, when it was first written */
} Line;

/* History + screen in a ring: the screen is the last `rows` lines. */
typedef struct {
    Line *ring;
    int cap, count, head;
    long long dropped;          /* lines dropped by the history limit */
} Buffer;

typedef struct {
    long long line;             /* absolute line number */
    int col, wc, hc;            /* position and size in cells */
    int pw, ph;                 /* original pixels */
    uint32_t *px;               /* premultiplied ARGB */
    uint32_t *scaled;
    int sw, sh;
} TermImage;

typedef struct { long long l0, l1; int c0, c1; } Match;   /* inclusive */

/* Profile: what the user configures in the app (colors, font, cursor...). */
typedef struct {
    float font_size, line_spacing, padding;
    uint32_t palette[16];
    uint32_t fg, bg, bold, cursor, cursor_text, sel_bg, sel_fg, link, guide;
    float bg_opacity, min_contrast;
    int bold_bright, use_bold, use_italic, cursor_style, cursor_blink, cursor_guide;
    int blink_text, alt_esc, scrollback, mouse_reporting, timestamps, copy_on_select;
    int alt_scroll, has_sel_fg, has_cursor_text;
    char word_chars[64];
    int font[4], font_na[4];    /* chosen font and "non-ASCII" one: regular, bold, italic, both; -1 = the bundled one */
    int ligatures;              /* apply calt/liga (JetBrains Mono, Fira Code...) */
    int bidi;                   /* Arabic/Hebrew in visual order and with joined forms (not on the alternate screen) */
    char font_family[64], na_family[64];
} TermProfile;

enum { CURSOR_BLOCK, CURSOR_UNDERLINE, CURSOR_BEAM };

/* Terminal modes. */
enum {
    M_ORIGIN = 1 << 0, M_AUTOWRAP = 1 << 1, M_INSERT = 1 << 2, M_APPCURSOR = 1 << 3,
    M_APPKEYPAD = 1 << 4, M_HIDECURSOR = 1 << 5, M_BRACKETED = 1 << 6, M_FOCUS = 1 << 7,
    M_MOUSE_X10 = 1 << 8, M_MOUSE_BTN = 1 << 9, M_MOUSE_DRAG = 1 << 10, M_MOUSE_ANY = 1 << 11,
    M_MOUSE_SGR = 1 << 12, M_SYNC = 1 << 13, M_LNM = 1 << 14, M_REVERSE = 1 << 15,
    M_ALTSCROLL = 1 << 16, M_CURSOR_BLINK = 1 << 17, M_GRAPHEME = 1 << 18,
};
#define M_MOUSE (M_MOUSE_X10 | M_MOUSE_BTN | M_MOUSE_DRAG | M_MOUSE_ANY)

/* Pending events for JS. */
enum {
    EV_TITLE = 1 << 0, EV_CWD = 1 << 1, EV_BELL = 1 << 2, EV_EXIT = 1 << 3, EV_BADGE = 1 << 4,
    EV_NOTIFY = 1 << 5, EV_OUTPUT = 1 << 6, EV_PROFILE = 1 << 7, EV_ATTENTION = 1 << 8,
    EV_COPY = 1 << 9, EV_COMMAND = 1 << 10, EV_RUN = 1 << 11, EV_HOST = 1 << 12,
    EV_TMUX = 1 << 13, EV_TMUX_START = 1 << 14, EV_TMUX_END = 1 << 15, EV_RESIZE = 1 << 16,
    EV_SECRET = 1 << 17, EV_FILL = 1 << 18, EV_SAVE = 1 << 19, EV_SAVED = 1 << 20,
};

typedef struct {
    int cx, cy, wrap, origin, gl, g[2];
    Cell pen;
} SavedCursor;

typedef struct Term {
    int cols, rows;
    Buffer main, alt, *buf;
    int cx, cy, wrap_pending;
    Cell pen;
    int top, bot;
    uint32_t modes;
    uint8_t *tabs;
    int g[2], gl;               /* character sets: 0 ASCII, 1 DEC graphics */
    int cursor_shape;           /* DECSCUSR: 0 = the profile's */
    /* keys with modifiers that legacy encoding can't tell apart (Shift+Enter is a plain CR there), as the
     * program asks: the kitty keyboard protocol's flag 1, "disambiguate", a stack per screen (CSI > u
     * pushes, CSI < u pops, CSI = u sets, CSI ? u asks), and xterm's modifyOtherKeys (CSI > 4 ; n m) */
    uint8_t kitty[2][8];
    int nkitty[2];
    int other_keys;
    SavedCursor saved, saved_alt;
    uint32_t last_cp;

    /* parser */
    int state;
    int params[32], nparams, colon[32];
    char priv, inter[4];
    int ninter;
    char *str;                  /* OSC / DCS */
    size_t str_len, str_cap;
    uint32_t utf8_cp;
    int utf8_left;

    /* visible state */
    char title[256], icon_title[256], cwd[1024], badge[256], notify[512], profile_req[128];
    char command[512];
    char host[256];             /* user@host reported by the shell (OSC 1337 RemoteHost / OSC 7) */
    uint32_t palette[256];
    uint32_t fg, bg, cursor_color;
    int fg_set, bg_set, cursor_set;
    char **links;
    int nlinks;
    TermImage *images;
    int nimages;
    long long cmd_prompt;       /* prompt line of the running command (OSC 133;C) */
    long long cmd_line;         /* where the prompt ended (OSC 133;B): the command starts there */
    int cmd_col;
    char run_cmd[1024];         /* text of the last executed command */

    /* copy mode: own cursor moved with the keyboard (vi style) */
    int copy_active, copy_col, copy_sel;
    long long copy_line;

    /* selection (absolute lines of the active buffer) */
    int sel_active, sel_mode, sel_rect;
    long long sel_al, sel_bl;   /* anchor and end, as marked by the mouse */
    int sel_ac, sel_bc;
    long long sel_sl, sel_el;   /* normalized and expanded range (word/line) */
    int sel_sc, sel_ec;

    /* search */
    Match *matches;
    int nmatches, match_cur;

    /* view and damage */
    int view;                   /* lines scrolled back (0 = live) */
    uint8_t *dirty;
    int all_dirty;
    uint32_t events;
    double sync_until;
    int focused;

    TermProfile *prof;
    float cell_w, cell_h;
    void (*respond)(struct Term *t, const char *s, size_t n);
    /* tmux -CC: kind 1 = control mode starts, 0 = a protocol line, 2 = finished */
    void (*tmux)(struct Term *t, int kind, const char *line, size_t n);
    void *user;
} Term;

/* vt.c */
uint32_t vt_base_cp(uint32_t cp);                        /* first character of the grapheme */
int   vt_cluster_cps(uint32_t cp, uint32_t *out, int max); /* all its characters */
int   vt_init(Term *t, int cols, int rows, int scrollback, TermProfile *p);
void  vt_free(Term *t);
void  vt_write(Term *t, const uint8_t *data, size_t n);
void  vt_resize(Term *t, int cols, int rows);
void  vt_reset(Term *t);
/* A key with modifiers (m: 1 + Shift 1 + Alt 2 + Ctrl 4, as in CSI sequences) in the keyboard protocol
 * the program asked for: "Enter", "Tab", "Backspace" or "Escape", or else the character cp (key NULL).
 * Writes the sequence to out and returns its length, or 0 when the legacy encoding applies. */
int   vt_key_encode(const Term *t, const char *key, uint32_t cp, int m, char *out, size_t size);
void  vt_apply_palette(Term *t);
Line *vt_view_line(Term *t, int row, long long *abs);
long long vt_first_abs(const Term *t);
long long vt_total_lines(const Term *t);
Line *vt_abs_line(Term *t, long long abs);
long long vt_cursor_abs(const Term *t);
uint32_t vt_resolve(const Term *t, uint32_t color, int is_fg);
void  vt_scroll_view(Term *t, int delta);
void  vt_clear_scrollback(Term *t);
void  vt_mark_dirty(Term *t, int row);
/* selection */
void  vt_select_start(Term *t, long long line, int col, int mode, int rect);
void  vt_select_extend(Term *t, long long line, int col);
void  vt_select_all(Term *t);
void  vt_select_clear(Term *t);
int   vt_selected(const Term *t, long long line, int col);
char *vt_selection_text(Term *t);
char *vt_text_range(Term *t, long long from, long long to);
int   vt_select_last_output(Term *t);
/* search and marks */
int   vt_search(Term *t, const char *query, int case_sensitive);
int   vt_search_move(Term *t, int dir);
void  vt_search_clear(Term *t);
int   vt_match_at(const Term *t, long long line, int col);   /* 0 no, 1 yes, 2 current */
int   vt_jump_mark(Term *t, int dir);
void  vt_set_mark(Term *t);
char *vt_url_at(Term *t, long long line, int col);
char *vt_words(Term *t, const char *prefix, int max);
char *vt_cursor_word(Term *t);
char *vt_logical_text(Term *t);
int   vt_set_matches(Term *t, const int *trip, int n);
int   vt_highlight(Term *t, const char *text, int64_t fg, int64_t bg);
char *vt_serialize(Term *t, int max_lines, size_t *len);
void  vt_enter_tmux(Term *t);
int   vt_in_tmux(const Term *t);
int   vt_copy_mode(Term *t, int on);
int   vt_copy_key(Term *t, const char *key, int ctrl, char **yank);

/* hold.c: holders that keep the shells alive if lord-server restarts */
int   hold_main(int argc, char **argv);
int   hold_spawn(int master, pid_t pid, char *key, size_t keylen);
void  hold_restore(void (*fn)(int fd, int ctrl, pid_t pid, const char *key));
void  hold_state_path(const char *key, char *out, size_t n);

/* pty.c */
pid_t pty_spawn(const char *cwd, int cols, int rows, int id, int *master);
void  pty_resize(int fd, int cols, int rows, int pw, int ph);
int   pty_sync_size(int fd, int cols, int rows, int pw, int ph);
void  pty_set_integration(const char *dir);

/* term.c: sessions */
typedef struct {                /* Instant Replay frame */
    double time;                /* wall-clock ms */
    int cols, rows, cx, cy;
    Cell *cells;
} Frame;

typedef struct {
    int used, id, fd, node, alive, exit_code;
    pid_t pid;
    Term t;
    TermProfile prof;
    char *out;                  /* pending write to the shell */
    size_t out_len, out_cap;
    double last_output, last_input, last_event, last_click;
    int clicks, click_col;
    long long click_line;
    int dragging, mouse_btn;
    float gx, gy, gw, gh;       /* geometry of the last draw */
    int pty_cols, pty_rows;     /* the size last given to the pty (TIOCSWINSZ): term_size_sync */
    double size_checked;        /* when the pty's own size was last compared with the grid */
    int drawn_cx, drawn_cy, blink_on;
    int pending_render;
    Frame *frames;              /* Instant Replay: ring of past screens */
    int nframes, frame_head, replay;
    size_t frame_bytes;
    double last_frame;
    int screen_changed;
    FILE *log;                  /* session log (raw output) */
    char log_path[512];
    char key[48];               /* holder name (hold.c); "" if it has none */
    int ctrl;                   /* control connection to the holder, -1 = none */
    int restored;               /* recovered from a previous lord-server */
    /* tmux -CC */
    int tmux;                   /* this session is the "gateway" of a tmux in control mode */
    char *tlines;               /* protocol lines pending for JS */
    size_t tl_n, tl_cap;
    int gateway, pane;          /* virtual session: gateway id and pane number (%N) */
    int paused;                 /* virtual: output waits until JS loads the content */
    char *pend;
    size_t pend_n, pend_cap;
    /* a program asking for a password (term_asks_secret): the Terminal offers Passwords */
    int secret;
    char secret_prompt[200];
    double secret_checked;
    char fill_result[24];       /* how the last fill from Passwords ended (termfill.c), for JS */
    int typing;                 /* inside term_key/term_paste: the user's own input (tcapture.c captures only that) */
    char save_offer[512];       /* "Save to Passwords?" (tcapture.c): the target as JSON, never the value */
    char save_result[24];       /* how saving it ended (termfill.c) */
} Session;

void     term_init(const char *fonts_dir, const char *shell_dir);
Session *term_create(const char *cwd, int scrollback);
/* A session on a stream someone else holds the other end of (a Machines guest's serial console): what arrives is
 * drawn, what's typed is written to it. No process, no PTY, no holder; the descriptor is the session's (closed with
 * it). NULL when there's no room. */
Session *term_create_fd(int fd, int scrollback);
Session *term_get(int id);
void     term_close(Session *s);
void     term_input(Session *s, const char *data, size_t n);
/* 1 if the program on the session's pty is asking for a password now: echo off with line input (what
 * getpass and its kin set: sudo, ssh, git, gpg) and a prompt on the cursor's line that names one. The
 * prompt goes to prompt (may be NULL). Sessions without a pty (a serial console) never ask. */
int      term_asks_secret(Session *s, char *prompt, size_t n);
int      term_prompt_names_secret(const char *line);      /* the prompt names a password (tsecret.c) */
struct TcapTarget;
/* The program asking for a password on the session (its executable and arguments, from /proc) (tsecret.c). */
int      term_secret_target(Session *s, const char *prompt, struct TcapTarget *t, char *exe, size_t exen);
int      term_traced(void);                                /* this process is being traced: no secrets */
/* A fill from Passwords ended (termfill.c): "ok" or an error code, sent to JS as the "fill" event. */
void     term_fill_done(Session *s, const char *result);
/* Saving a typed password ended (termfill.c): "ok" or an error code, the "saved" event. */
void     term_save_done(Session *s, const char *result);
void     term_key(Session *s, const char *key, const char *chr, const char *base, int mods);
void     term_paste(Session *s, const char *text);
void     term_focus(Session *s, int on);
int      term_mouse(Session *s, const char *type, float lx, float ly, int button, int mods);
void     term_wheel(Session *s, int lines, float lx, float ly, int mods);
int      term_replay(Session *s, int index);            /* -1 exits; returns the index */
const char *term_log(Session *s, int on);
int      term_export(Session *s, char *path, size_t size);
int      term_pollfds(struct pollfd *p, int max, int *ids);
void     term_dispatch(int id, short revents);
int      term_damage(void);                      /* adds the damage to ui.damage; 1 if there is any */
double   term_timeout(double now);               /* ms until the next blink, -1 never */
void     term_tick(double now);
typedef void (*TermEventFn)(int id, const char *type, const char *data);
void     term_events(TermEventFn fn);
void     term_paint(Canvas *c, int id, float x, float y, float w, float h, float opacity);
/* The grid that fits a terminal's content box: lw × lh logical pixels, the profile's padding and its logical
 * cell. Only from the logical box, never from a pass's pixels: a window drawn smaller (the overview, minimize,
 * a scaled display) is the same grid, and (lw * k) / k landing a hair under a whole cell no longer takes a
 * column off for that pass. 0 when the box can't hold a grid (a pane not laid out yet, a 0 × 0 node): the
 * session keeps the one it has instead of telling the program it is 2 × 1. */
static inline int term_grid_size(float lw, float lh, int pad, int cw, int ch, int *cols, int *rows) {
    if (cw < 1 || ch < 1) return 0;
    float gw = lw - 2.0f * (float)pad, gh = lh - 2.0f * (float)pad;
    if (!(gw >= cw) || !(gh >= ch)) return 0;
    int c = (int)(gw / (float)cw + 1e-3f), r = (int)(gh / (float)ch + 1e-3f);
    *cols = c < 2 ? 2 : c > TERM_MAX_COLS ? TERM_MAX_COLS : c;
    *rows = r < 1 ? 1 : r > TERM_MAX_ROWS ? TERM_MAX_ROWS : r;
    return 1;
}
/* The pty takes the grid's size (TIOCSWINSZ, a SIGWINCH for the program) when it changed, and at most once a
 * second its own size is read back and set again if something else changed it (term.c). */
void     term_size_sync(Session *s, int check);
/* Visual order of a row with Arabic/Hebrew (term_paint.c); 0 if it stays as it is. */
int      term_row_visual(const TermProfile *p, const Term *t, const Line *l, int cols, int *vis, uint8_t *lv);
void     term_cell_at(Session *s, float lx, float ly, long long *line, int *col);
void     term_shutdown(int keep);                 /* keep: the shells stay alive for the next startup */
void     term_restore(void);                       /* recovers the sessions from the holders */
Session *term_tmux_pane(Session *gateway, int pane);  /* virtual session for a tmux pane */
void     term_tmux_send(Session *gateway, const char *cmd, size_t n);
void     term_feed(Session *s, const char *data, size_t n);
void     term_tmux_ready(Session *s);
void     term_detach_all(void);
int      term_list(int *ids, int max);
const char *term_clipboard(void);
void     term_set_clipboard(const char *s, size_t n);
unsigned term_clipboard_gen(void);
/* Where the last copied thing came from ("terminal", "lordos" or a Wayland app's app_id) and
 * whether it is sensitive (password: it does not go to the history). term_set_clipboard sets it to "terminal", 0. */
void        term_clipboard_mark(const char *origin, int sensitive);
const char *term_clipboard_origin(void);
int         term_clipboard_sensitive(void);
int      term_mono_font(int bold, int italic);
int      term_profile_font(const TermProfile *p, int non_ascii, int bold, int italic);
int      term_set_fonts(TermProfile *p, const char *family, const char *non_ascii);
int      term_color_font(void);
int      term_fallback_font(int i);                 /* -1 at the end of the list */
void     term_metrics(const TermProfile *p, float *cw, float *ch, float *baseline);
void     term_profile_defaults(TermProfile *p);
size_t   base64_decode(const char *in, size_t n, uint8_t *out);

#endif
