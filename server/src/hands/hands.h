/* hands.h — "Hands" (Manos): an AI agent uses the LordOS desktop through lord-server (docs/HANDS.md). Approved by the
 * maintainer on 2026-11-26 (AGENTS.md).
 *
 * The agent is a program of the person's own (Claude Code, Lumo) talking to the session's lord-server over the session
 * bus (org.lordos.Hands1, ui/handsbus.c), most often through /usr/bin/lordos-mcp (cli/lordosmcp.c). lord-server never
 * runs a command for it. What it can do is a closed list (see HandsReq): read the windows and their UI tree, press,
 * type and scroll through lord-server's own input path, take a screenshot, open an app or a file through the shell's
 * existing paths, read an app's log. Every rule below is checked here, in C, before anything happens:
 *
 *   - off until the person turns it on for this login session (Settings → AI → Hands: lord.hands.serve, never kept on
 *     disk), and each new bus connection asks in a system-layer prompt ("This session" / "Deny") answered only by the
 *     person's real input; it shows the name the client gave and its program's path as reported, not verified (a
 *     program of the same user can't be told apart reliably: same-user processes are one trust boundary, as for any
 *     desktop automation); a grant is that connection's alone and ends when it closes or its process exits;
 *   - Stop (the banner), the panic key (Ctrl+Alt+Esc on a real keyboard, before any JavaScript), locking the screen and
 *     turning the switch off end every client at once; a stopped client gets HANDS_E_STOPPED from then on;
 *   - the person's real input wins: for HANDS_YIELD_MS after a key, a click or a move of theirs the agent's input is
 *     refused with HANDS_E_ACTIVE ("user is active");
 *   - an optional per-app allowlist (the shell's app ids);
 *   - never the system layer (permission, keyring, password and polkit prompts, the file chooser of Linux apps, the
 *     banner itself; sdk/layers.js `system` and above), a protected window or field (a password field, the vault), a
 *     node marked handsDeny (Settings → AI → Hands, the theme-trust sheet), the lock screen or the greeter (another
 *     process: Hands1 is never served there);
 *   - rate limits per kind of call; every call logged as a line of closed words (never text, names or values).
 *
 * The input it injects goes through the normal path (hit test, focus, Wayland clients) tagged as the agent's: lord-server
 * counts it (hands_ai_events) and the shell can tell (lord.hands.injecting()). It is never /dev/uinput or evdev.
 *
 * This file holds the state and the policy with no D-Bus and no compositor: the bus (handsbus.c) parses and checks the
 * arguments, then calls hands_*; what touches the screen is the HandsHost main.c gives. tests/hands_test.c drives it all
 * with a fake host. */
#ifndef LORD_HANDS_HANDS_H
#define LORD_HANDS_HANDS_H

#include <stddef.h>
#include <stdint.h>

#define HANDS_BUS_NAME  "org.lordos.Hands1"
#define HANDS_BUS_PATH  "/org/lordos/Hands1"
#define HANDS_BUS_IFACE "org.lordos.Hands1"

#define HANDS_CLIENTS     8         /* bus connections at once */
#define HANDS_EXE_MAX     256       /* the program the bus vouched for, shown in the prompt */
#define HANDS_BLACK_MAX   1024      /* windows of other apps blacked out of an allowlisted screenshot */
#define HANDS_NAME_MAX    40        /* bytes of a client's shown name */
#define HANDS_TEXT_MAX    2000      /* bytes one Type call takes */
#define HANDS_VALUE_MAX   2000      /* bytes of a SetValue */
#define HANDS_QUERY_MAX   200       /* bytes of a Find query, an AppLog pattern */
#define HANDS_ID_MAX      128       /* a window id, an app id or name, a ref */
#define HANDS_PATH_MAX    4096
#define HANDS_ALLOW_MAX   16        /* apps in the allowlist */
#define HANDS_YIELD_MS    3000      /* the person's input wins for this long */
#define HANDS_SHOT_MAXDIM 1568      /* a screenshot's longest side by default (and at most 2560) */
#define HANDS_DEPTH_MAX   60
#define HANDS_REPEAT_MAX  20
#define HANDS_SCROLL_MAX  30
#define HANDS_LOG_MAX     400       /* AppLog lines */

/* D-Bus error names (handsbus.c); their message is one closed word */
#define HANDS_ERR_OFF      "org.lordos.Hands1.Error.Off"         /* Settings → AI → Hands is off */
#define HANDS_ERR_ASKING   "org.lordos.Hands1.Error.Asking"      /* the person hasn't answered the prompt yet */
#define HANDS_ERR_DENIED   "org.lordos.Hands1.Error.Denied"      /* the person said Deny */
#define HANDS_ERR_STOPPED  "org.lordos.Hands1.Error.Stopped"     /* Stop, the panic key, the lock, the switch */
#define HANDS_ERR_ACTIVE   "org.lordos.Hands1.Error.UserActive"  /* the person is using the computer */
#define HANDS_ERR_REFUSED  "org.lordos.Hands1.Error.Refused"     /* a place or a field the agent never reaches */
#define HANDS_ERR_RATE     "org.lordos.Hands1.Error.RateLimited"
#define HANDS_ERR_NOTFOUND "org.lordos.Hands1.Error.NotFound"    /* no such window or ref (any more) */
#define HANDS_ERR_FAILED   "org.lordos.Hands1.Error.Failed"
#define HANDS_ERR_BUSY     "org.lordos.Hands1.Error.Busy"        /* too many clients */
#define HANDS_ERR_ACCESS   "org.freedesktop.DBus.Error.AccessDenied"
#define HANDS_ERR_ARGS     "org.freedesktop.DBus.Error.InvalidArgs"

enum {
    HANDS_OK = 0,
    HANDS_E_OFF = -1, HANDS_E_ASKING = -2, HANDS_E_DENIED = -3, HANDS_E_STOPPED = -4, HANDS_E_ACTIVE = -5,
    HANDS_E_REFUSED = -6, HANDS_E_RATE = -7, HANDS_E_NOTFOUND = -8, HANDS_E_FAILED = -9, HANDS_E_ARGS = -10,
    HANDS_E_BUSY = -11,
};

/* why a place or a field was refused (HANDS_E_REFUSED's word) */
enum {
    HANDS_WHY_NONE = 0, HANDS_WHY_SYSTEM, HANDS_WHY_PROTECTED, HANDS_WHY_PASSWORD, HANDS_WHY_DENIED_NODE,
    HANDS_WHY_ALLOWLIST, HANDS_WHY_PROMPT, HANDS_WHY_LOCKED, HANDS_WHY_POINTER_LOCK, HANDS_WHY_OUTSIDE,
    HANDS_WHY_OBSCURED, HANDS_WHY_NOT_EDITABLE, HANDS_WHY_DIALOG, HANDS_WHY_SENSITIVE_CLIP, HANDS_WHY_COUNT,
};
const char *hands_why_word(int why);       /* "system-layer", "protected", "password", "protected-dialog", … */

/* the calls (handsbus.c's methods), each its own closed word in the log */
enum {
    HANDS_C_HELLO = 0, HANDS_C_STATUS, HANDS_C_WINDOWS, HANDS_C_READ, HANDS_C_FIND, HANDS_C_POINTER, HANDS_C_SCROLL,
    HANDS_C_KEY, HANDS_C_TYPE, HANDS_C_SETVALUE, HANDS_C_SHOT, HANDS_C_OPENAPP, HANDS_C_OPENPATH, HANDS_C_CLOSE,
    HANDS_C_APPLOG, HANDS_C_COUNT,
};
const char *hands_call_word(int call);

/* pointer actions (Pointer's first argument) */
enum {
    HANDS_P_NONE = 0, HANDS_P_LEFT, HANDS_P_RIGHT, HANDS_P_MIDDLE, HANDS_P_DOUBLE, HANDS_P_TRIPLE, HANDS_P_HOVER,
    HANDS_P_DRAG, HANDS_P_COUNT,
};
int hands_pointer_action(const char *s);   /* "left_click" … "left_click_drag"; HANDS_P_NONE otherwise */
const char *hands_pointer_word(int a);

enum { HANDS_DIR_NONE = 0, HANDS_DIR_UP, HANDS_DIR_DOWN, HANDS_DIR_LEFT, HANDS_DIR_RIGHT };
int hands_direction(const char *s);

/* the clients' kinds, the only thing of a client's name that reaches a log line */
enum { HANDS_K_OTHER = 0, HANDS_K_CLAUDE, HANDS_K_LUMO };
const char *hands_kind_word(int kind);

/* ---- pure checks (tests/hands_test.c, the fuzz) ---- */

/* A client's shown name: printable UTF-8 with no control, bidi or format characters, spaces collapsed, at most
 * HANDS_NAME_MAX bytes cut on a character; into out (HANDS_NAME_MAX + 1). 0, or -1 nothing left. */
int  hands_clean_name(const char *in, char *out, size_t n);
int  hands_kind_of(const char *name);       /* "Claude Code" → HANDS_K_CLAUDE, "Lumo" → HANDS_K_LUMO */
/* Valid UTF-8 of at most max bytes, no NUL, and no control characters but \n and \t (allow_lines) or none at all. */
int  hands_text_ok(const char *s, size_t n, size_t max, int allow_lines);
int  hands_ref_ok(const char *s);           /* "" or "ref_" and 1 to 9 digits */
int  hands_id_ok(const char *s);            /* a window or app id: [A-Za-z0-9._:#@+-], 1..HANDS_ID_MAX */
int  hands_name_ok(const char *s);          /* an app's id or shown name: hands_text_ok, 1..HANDS_ID_MAX, one line */
int  hands_path_ok(const char *s);          /* absolute, one line, no "/../" or "/./" parts, < HANDS_PATH_MAX */
int  hands_filter(const char *s);           /* "all" 0, "interactive" 1, "" 0; -1 otherwise */
/* A target for Screenshot: "display", "display:<id>", "window:<id>", "region"; *id gets the number or window id */
enum { HANDS_SHOT_NONE = 0, HANDS_SHOT_DISPLAY, HANDS_SHOT_WINDOW, HANDS_SHOT_REGION };
int  hands_shot_target(const char *s, uint32_t *display, char *win, size_t n);

/* A key chord: "ctrl+s", "Return", "alt+Tab", "shift+F10", "super", "a", "ctrl+shift+Left". Modifiers ctrl, alt, shift,
 * super (meta, cmd, win); one key: a named key (HandsKey) or one printable character looked up on the layout. Names
 * are case-insensitive. Never a chord the physical keyboard keeps for itself (Ctrl+Alt+Backspace, Ctrl+Alt+Fn, the
 * panic key): those are only on_input's. */
enum { HANDS_M_CTRL = 1, HANDS_M_ALT = 2, HANDS_M_SHIFT = 4, HANDS_M_SUPER = 8 };
typedef struct {
    unsigned mods;
    int code;                   /* an evdev code of a named key, or 0 */
    char ch[8];                 /* else one character (UTF-8) to find on the active layout */
} HandsChord;
int  hands_chord(const char *s, HandsChord *c);  /* 0, or -1 not a chord the agent may press */
/* chords separated by spaces ("ctrl+a Delete"): at most 8; 0 each into c[], their count in *n */
int  hands_chords(const char *s, HandsChord *c, int max, int *n);
int  hands_paste_chord(const HandsChord *c);  /* Ctrl/Super+V, Ctrl+Shift+V, Shift+Insert */

/* a token bucket: at most `burst` at once, `per_s` a second after that */
typedef struct { double tokens, last; int init; } HandsBucket;
int  hands_rate_take(HandsBucket *b, double now_ms, double per_s, double burst);   /* 1 taken, 0 limited */

/* "hands: <kind> <call>[ <action>] <result>[ <why>]" — words only from the closed tables */
size_t hands_log_line(char *out, size_t n, int kind, int call, int action, int result, int why);

/* ---- the host: what lord-server's main.c and js.c do for it ---- */

typedef struct {
    /* the shell's answer to op with args (JSON text), malloc'd JSON; NULL when the shell has none */
    char *(*shell)(const char *op, const char *json);
    /* tell the shell: the clients changed, a prompt, a log line (sdk/hands.jsx __lordHandsEvent) */
    void (*event)(const char *json);
    /* the desktop's logical size, and whether a pointer lock (a game, Machines' captured mouse) holds the pointer */
    void (*size)(int *w, int *h);
    int  (*pointer_locked)(void);
    /* 0, or HANDS_WHY_* when (x, y) is a place the agent never reaches; *node the node hit there (0 none) */
    int  (*place)(double x, double y, int *node);
    /* the node is ref's node, inside it, or holds it (the click lands on what the ref named) */
    int  (*same_target)(int hit, int ref_node);
    /* 0, or HANDS_WHY_* when the node itself is one the agent never reaches (the system band, protected, handsDeny) */
    int  (*node_place)(int node);
    /* 0, or HANDS_WHY_* when a key now would go to a place the agent never reaches (a Linux app's password field, a
     * protected window, an authentication prompt) */
    int  (*keys_place)(void);
    /* the input, through the normal path; x, y logical */
    void (*move)(double x, double y);
    void (*button)(int button, int down);
    void (*wheel)(int dx, int dy);
    /* 0 pressed and released; -1 no key types that character; -2 refused: a paste (by the key it resolves to) while the
     * clipboard holds a secret */
    int  (*key)(const HandsChord *c);
    int  (*type)(const char *text, size_t n);       /* 0, or HANDS_WHY_* stopped part-way */
    /* a PNG (malloc'd) of a display, a window's node or a logical region, the system layer and denied nodes black; *w,
     * *h its size; meta gets {"x","y","w","h","scale"} of the logical area it shows. NULL on failure. */
    /* black: more nodes painted as the protected fill (other apps' windows, with an allowlist) */
    uint8_t *(*shot)(int kind, uint32_t display, int node, double x, double y, double w, double h, int maxdim,
                     const int *black, int nblack, size_t *len, int *iw, int *ih, char *meta, size_t metan);
    double (*now)(void);                            /* ms, monotonic */
    int  (*clip_sensitive)(void);                   /* the clipboard holds a secret (Passwords' copy): no agent paste */
} HandsHost;

void hands_init(const HandsHost *h);
const HandsHost *hands_host(void);

/* ---- the session's state (the shell and main.c) ---- */

/* Settings → AI → Hands: on for this session, and the allowlist (app ids, "" none = every app) */
void hands_serve(int on, const char *const *allow, int nallow);
int  hands_on(void);
/* the person's answer to the prompt for one connection (its unique bus name): 1 This session, 0 Deny */
void hands_answer(const char *bus, int allow);
/* the banner's Stop (and turning it off): every client stopped, every grant forgotten */
void hands_stop(const char *why_word);
/* the panic key: hands_stop, and Hands off for the session */
void hands_panic(void);
/* the screen was locked or the session left the monitor: as Stop */
void hands_away(void);
/* a physical key, click or move (on_input): the agent waits HANDS_YIELD_MS */
void hands_person_input(void);
/* lord-server --headless --hands-qa: a private desktop for an agent's QA (docs/HANDS.md): on, every client granted with
 * no prompt, no person to yield to. Never on a monitor or the login screen (main.c checks). */
void hands_qa(int on);
int  hands_qa_mode(void);
/* the agent's input is being delivered now (lord.hands.injecting) and how many events it has sent */
int  hands_injecting(void);
/* Fail closed: the agent's input is being delivered, or a Hands client is active and the agent's input came after the
 * person's last real click or key (motion never counts, time never clears it): a security-state change now is the
 * agent's doing and is refused (js.c prof_call, the broker, term.fill). hands_agent_recent is the same. */
int  hands_agent_owns_input(void);
int  hands_agent_recent(void);
int  hands_active(void);                     /* a Hands client is granted and connected */
void hands_person_press(void);               /* a real click or key (on_input, a remote viewer): the person's own again */
/* what a lord.<ns>.<fn> native is to an agent's input: HANDS_G_REFUSE (a security-state change), HANDS_G_CLIP
 * (lord.clipboard.get: "" while the clipboard holds a secret) or HANDS_G_NONE (js.c prof_call) */
enum { HANDS_G_NONE = 0, HANDS_G_REFUSE, HANDS_G_CLIP };
int  hands_native_guard(const char *name);
unsigned long hands_ai_events(void);
/* {"on","qa","allow":[…],"clients":[{"bus","name","exe","kind","state","actions"}],"events"} for the shell */
char *hands_status_json(void);

/* ---- a bus client (handsbus.c) ---- */

/* the client named itself; exe the program path handsbus.c read for its process ("" unknown), shown as reported, never
 * as an identity: HANDS_OK granted, HANDS_E_ASKING (a prompt
 * is up), or an error */
int  hands_hello(const char *bus, const char *name, const char *exe);
void hands_gone(const char *bus);           /* left the bus */
void hands_lost(void);                      /* the bus went away */
/* 0 when that client may act now (on, granted, not stopped, not throttled for this kind); else HANDS_E_* */
int  hands_may(const char *bus, int call);

/* The calls, after handsbus.c checked every argument. *out (malloc'd) is the answer text; *why the refusal's word. */
typedef struct {
    int call;
    char window[HANDS_ID_MAX + 1], ref[16], app[HANDS_ID_MAX + 1];
    int filter, depth, action, dir, amount, repeat;
    double x, y, x2, y2, w, h;
    int shot;
    uint32_t display;
    int maxdim;
    char *text;                                 /* Type, SetValue, Find, Key, AppLog pattern, OpenPath (borrowed) */
    size_t text_len;
} HandsReq;
int  hands_call(const char *bus, const HandsReq *r, char **out, uint8_t **png, size_t *png_len, int *why);

#endif
