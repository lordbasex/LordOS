/* vmagent.h — org.lordos.agent, Machines' own channel into a guest (docs/VIRTUALIZATION.md 6.8, 6.17, 7.1.1), and
 * the bits lord-vm needs to talk to qemu-guest-agent.
 *
 * A virtio-serial port named org.lordos.agent: no network, no port, no VNC; lordos-guest (tools/guest), or on Windows
 * lordos-guest-win (tools/guest-win), holds the guest's end as the person signed in at the guest's display. Frames,
 * both ways:
 *
 *   u32 length (little endian, of the payload)  u16 kind  u16 channel  payload
 *
 * At most AG_FRAME_MAX bytes of payload; anything longer, or a stream that doesn't parse, is dropped whole (the guest
 * is untrusted: lord-vm parses this inside its own sandbox, never lord-server). Kinds a side doesn't know are skipped,
 * so either side can grow.
 *   AGK_HELLO  guest → host  "lordos-guest <version> <capability>…" (text: clipboard, clipx, clipreq, display, windows, or
 *                            noclip: a helper whose guest has no clipboard tool, said instead of "clipboard clipx"; a
 *                            helper says hello again when that changes; pointer: the Windows tools are new enough for
 *                            lord-vm to set the display driver's hardware pointer, docs/VIRTUALIZATION.md 7.1.2; gvt:
 *                            new enough for lord-vm to install Intel's driver for a GVT-g vGPU, vmgvtwin.h); the
 *                            host answers with its own hello
 *   AGK_CLIP   both ways     the clipboard's text, UTF-8; only in the directions the machine allows (machine.json)
 *   AGK_CLIPX  both ways     the clipboard in up to three forms at once (capability "clipx" in both hellos): parts of
 *                            u8 form (AGF_TEXT, AGF_HTML, AGF_PNG), u32 length (little endian), bytes; each form at most
 *                            once. Text and HTML are UTF-8 of at most AG_TEXT_MAX; a PNG at most AG_PNG_MAX bytes and
 *                            AG_PNG_DIM pixels a side. lord-vm checks the PNG's header and sanitizes the HTML (only
 *                            formatting tags and attributes stay, no scripts, styles that load anything, events or
 *                            links other than http, https and mailto) before anything goes on; lord-server re-encodes
 *                            the picture in lord-media's sandbox.
 *   AGK_CLIPOFFER host → guest  the host's clipboard on demand (capability "clipreq" in both hellos; 6.8): u32 gen
 *                            (≥ 1, a new one per offer), u8 forms (AGM_*: at least one, nothing else), u8 flags (AGO_*,
 *                            nothing else); exactly 6 bytes. No contents: the guest puts those forms on its clipboard
 *                            without data and asks for one only when a program there pastes. Sent only while the person
 *                            turns to the machine (its window comes to the front, a click on it). An offer replaces the
 *                            last; the host answers only requests for the last one.
 *   AGK_CLIPREQ  guest → host  u32 gen, u8 form (AGF_*); exactly 5 bytes: a program pastes that form of that offer.
 *   AGK_CLIPDATA host → guest  u32 gen, u8 form, u8 status (AGD_*), then, with AGD_OK only, the form's bytes (text and
 *                            HTML: 1..AG_TEXT_MAX of UTF-8 with no NUL; a PNG: 1..AG_PNG_MAX bytes whose header
 *                            ag_png_check believes); nothing after any other status. The answer to one request: lord-server
 *                            gives the data only while the machine is in use (its window in front, or one of its apps
 *                            mode windows, or a few seconds after), for the offer that is still current, and in "ask"
 *                            mode only once the person said yes. The guest waits AG_CLIP_WAIT_MS (AG_ASK_WAIT_MS with
 *                            AGO_ASK) and renders nothing when the answer isn't AGD_OK.
 *   AGK_OPEN   host → guest  open a file the host just dropped into the guest (capability "open" in the guest's hello;
 *                            Files' "Open with", docs/VIRTUALIZATION.md 7.3): 32 lowercase hex digits (a token lord-vm
 *                            made for that drop), u64 size (little endian), 32 bytes of SHA-256; exactly 72 bytes. No
 *                            path: the person's own copy step in the guest (the drop's script, run as that person) wrote
 *                            where it put the file under that token (%LOCALAPPDATA%\LordOS\open\<token>); the guest opens
 *                            only that file, only when the record is fresh and the file's size and SHA-256 are these, with
 *                            the system's default handler (a program runs), as the person; the record goes either way.
 *   AGK_OPENED guest → host  the outcome: the 32 hex digits of the token, u8 status (AGOS_*), u32 the system's error code
 *                            (0 with AGOS_OK); exactly 37 bytes. lord-vm logs it and answers the VM_FILE with it.
 *   AGK_PLACE  host → guest  files dropped on an apps mode window (capability "place"; 7.2.1): the drop's token, size and
 *                            SHA-256 as AGK_OPEN, then u32 the guest window's id (one of its last LAG_WINDOWS), i32 x,
 *                            i32 y (the drop point on the shown display, 0..LAG_DIM_MAX), u32 batch (≥ 1), u8 index,
 *                            u8 count (index < count ≤ AG_PLACE_MAX); exactly AG_PLACE_BYTES. The guest checks each file as
 *                            for AGK_OPEN and, once a batch is all there, hands those files to that window: an Explorer
 *                            folder window gets them moved into its folder, a window that takes dropped files
 *                            (WS_EX_ACCEPTFILES) gets WM_DROPFILES, any other gets them on the clipboard as files to paste.
 *   AGK_PLACED guest → host  per file: the token, u8 status (AGOS_*), u8 how (AGP_*), u32 the system's error code;
 *                            exactly AG_PLACED_BYTES.
 *   AGK_DISPLAY host → guest  u32 width, u32 height (little endian; 320..8192 × 200..8192): the size the machine's
 *                            window wants (capability "display" in the guest's hello; lord-vm sends it only then, and
 *                            again after each hello). The guest sets that size, or the closest it can, on the
 *                            display the machine's picture shows (on Windows the one lordos-guest-win takes for
 *                            the shown display, below; elsewhere the primary). guest → host: the same layout, the
 *                            size it set (lord-vm logs it).
 *   AGK_DISPLAYS guest → host  the guest's displays, for lord-vm's log only (nothing else uses them): u8 count
 *                            (1..AG_DISPLAYS_MAX), u8 shown (the index of the display the helper believes the
 *                            machine's picture shows, < count, or AG_DISPLAYS_NONE), then per display: i32 x, i32 y
 *                            (within ±LAG_DIM_MAX), u32 w, u32 h (1..AG_DISPLAY_MAX), u8 flags (AGDS_*: nothing else
 *                            set; at most one display primary), u8 name length (1..AG_DISPLAY_NAME_MAX) + the name
 *                            (printable ASCII, e.g. \\.\DISPLAY1); nothing after the last. Sent at hello and when
 *                            the guest's display configuration changes. Apps mode's rectangles (LAG_WINDOWS,
 *                            LAG_WINACT, AGK_PLACE) are relative to the shown display's top left corner.
 *
 * Apps mode (docs/VIRTUALIZATION.md 7.2), capability "windows" in the guest's hello (lord-vm then sets LAG_CAP_WINDOWS
 * in VM_AGENT_STATE b). Everything little endian; every length is checked by the lag_*_parse functions below, which
 * both sides use on what they receive; a frame that doesn't parse exactly (to its last byte) is ignored whole.
 *   LAG_APPS     host → guest  u8 on (0 or 1). On: the guest sends LAG_APPLIST and LAG_WINDOWS at once, then
 *                            LAG_WINDOWS whenever its windows change (at most 30 a second) and LAG_WINICON once per new
 *                            window, and hides its taskbar. Off: it stops and shows the taskbar again. Sending on again
 *                            asks for a fresh LAG_APPLIST and LAG_WINDOWS.
 *   LAG_WINDOWS  guest → host  u32 serial (grows by one per list), u16 count (≤ LAG_WIN_MAX), then per window, top of
 *                            the z-order first: u32 id, i32 x, i32 y, i32 w, i32 h (the guest's picture's pixels, clipped
 *                            to the shown display; w, h 1..LAG_DIM_MAX, x, y within ±LAG_DIM_MAX), u32 flags (LAGW_*; nothing
 *                            else set), [u32 owner id, only with LAGW_OWNED], u32 pid, u16 title length
 *                            (≤ LAG_TITLE_MAX) + title, u16 app length (≤ LAG_APP_MAX) + the program's file name
 *                            (no '/', '\' or ':'); both UTF-8. Only visible, not cloaked, not tool windows with a
 *                            non-empty rectangle; never the taskbar or the desktop.
 *   LAG_WINICON  guest → host  u32 id, u16 w, u16 h (1..LAG_ICON_MAX), then w × h × 4 bytes of R, G, B, A (straight
 *                            alpha), rows top first.
 *   LAG_WINACT   host → guest  u32 id, u8 action (LAGA_*); LAGA_MOVE adds i32 x, i32 y, i32 w, i32 h. The guest acts
 *                            only on a window that is in the last list it sent.
 *   LAG_APPLIST  guest → host  u16 count (≤ LAG_APPS_MAX), then per program: u16 name length (1..LAG_NAME_MAX) + name,
 *                            u16 token length (1..LAG_TOKEN_MAX) + token; both UTF-8. The token is the guest's own
 *                            (a Start menu shortcut's path): opaque to the host.
 *   LAG_LAUNCH   host → guest  u16 length (1..LAG_TOKEN_MAX) + a token: the guest starts that program only when the
 *                            token is, byte for byte, one of its own last LAG_APPLIST; the host never sends a command.
 * The host never asks the guest to run anything over this channel other than a program the guest itself listed, or
 * (AGK_OPEN) the one file the person just sent it from Files, by a token, never by a path or command; the guest can't
 * ask the host for anything. */
#ifndef LORD_VM_AGENT_H
#define LORD_VM_AGENT_H

#include <stddef.h>
#include <stdint.h>

#define AG_TEXT_MAX  (1u << 20)
#define AG_PNG_MAX   (8u << 20)
#define AG_PNG_DIM   8192u
#define AG_FRAME_MAX (AG_PNG_MAX + 2 * AG_TEXT_MAX + 64)
#define AG_HEAD 8
enum { AGK_HELLO = 1, AGK_CLIP = 2, AGK_CLIPX = 3 };
enum { AGF_TEXT = 1, AGF_HTML = 2, AGF_PNG = 3 };
enum { AGK_DISPLAY = 4, LAG_APPS = 5, LAG_WINDOWS = 6, LAG_WINICON = 7, LAG_WINACT = 8, LAG_LAUNCH = 9, LAG_APPLIST = 10 };
enum { AGK_CLIPOFFER = 11, AGK_CLIPREQ = 12, AGK_CLIPDATA = 13, AGK_OPEN = 14, AGK_OPENED = 15, AGK_PLACE = 16, AGK_PLACED = 17 };
/* AGK_CLIPOFFER's forms (1 << (AGF_* - 1)) and flags; AGK_CLIPDATA's status */
enum { AGM_TEXT = 1, AGM_HTML = 2, AGM_PNG = 4, AGM_ALL = 7 };
enum { AGO_ASK = 1, AGO_ALL = 1 };
enum { AGD_OK = 0, AGD_REFUSED = 1, AGD_STALE = 2, AGD_NONE = 3 };
#define AG_CLIP_WAIT_MS 2000u
#define AG_ASK_WAIT_MS  25000u
/* AGK_CLIPOFFER: 0 well formed (gen ≥ 1, forms within AGM_ALL and not 0, flags within AGO_ALL) */
int ag_offer_parse(const uint8_t *p, size_t n, uint32_t *gen, uint8_t *forms, uint8_t *flags);
void ag_offer_write(uint8_t out[6], uint32_t gen, uint8_t forms, uint8_t flags);
/* AGK_CLIPREQ: 0 well formed (gen ≥ 1, form AGF_TEXT, AGF_HTML or AGF_PNG) */
int ag_clipreq_parse(const uint8_t *p, size_t n, uint32_t *gen, uint8_t *form);
void ag_clipreq_write(uint8_t out[5], uint32_t gen, uint8_t form);
/* AGK_CLIPDATA: 0 well formed (see above; with AGD_OK *data points into the payload, checked as the form says) */
int ag_clipdata_parse(const uint8_t *p, size_t n, uint32_t *gen, uint8_t *form, uint8_t *status, const uint8_t **data, uint32_t *dn);
void ag_clipdata_head(uint8_t out[6], uint32_t gen, uint8_t form, uint8_t status);   /* then the data with AGD_OK */

/* AGK_OPEN and AGK_OPENED (Files' "Open with" a machine). The token: AG_OPEN_TOKEN lowercase hex digits. AGOS_*:
 * opened; no fresh record of that token (or it doesn't name a file); the file isn't the one sent (size, hash, a link);
 * the system couldn't open it (code: its error) */
#define AG_OPEN_TOKEN 32
#define AG_OPEN_BYTES (AG_OPEN_TOKEN + 8 + 32)
#define AG_OPENED_BYTES (AG_OPEN_TOKEN + 5)
#define AG_OPEN_RECORD_MS 600000u          /* a record older than this is refused (and removed) */
enum { AGOS_OK = 0, AGOS_NORECORD = 1, AGOS_CHANGED = 2, AGOS_FAILED = 3, AGOS_LAST = AGOS_FAILED };
/* 1 when t is AG_OPEN_TOKEN lowercase hex digits (n bytes, no NUL needed) */
int ag_open_token_ok(const char *t, size_t n);
/* 0 well formed (exactly AG_OPEN_BYTES, a valid token); token gets AG_OPEN_TOKEN + 1 bytes (NUL-terminated) */
int ag_open_parse(const uint8_t *p, size_t n, char token[AG_OPEN_TOKEN + 1], uint64_t *size, uint8_t sha[32]);
/* 0 written (AG_OPEN_BYTES), -1 the token isn't one */
int ag_open_write(uint8_t out[AG_OPEN_BYTES], const char *token, uint64_t size, const uint8_t sha[32]);
/* 0 well formed (exactly AG_OPENED_BYTES, a valid token, a known status, code 0 with AGOS_OK) */
int ag_opened_parse(const uint8_t *p, size_t n, char token[AG_OPEN_TOKEN + 1], uint8_t *status, uint32_t *code);
int ag_opened_write(uint8_t out[AG_OPENED_BYTES], const char *token, uint8_t status, uint32_t code);

/* AGK_PLACE and AGK_PLACED (files dropped on an apps mode window). AGP_*: how the guest handed them over */
#define AG_PLACE_MAX 8
#define AG_PLACE_BYTES (AG_OPEN_BYTES + 18)
#define AG_PLACED_BYTES (AG_OPEN_TOKEN + 6)
enum { AGP_NONE = 0, AGP_FOLDER = 1, AGP_DROPPED = 2, AGP_CLIPBOARD = 3, AGP_LAST = AGP_CLIPBOARD };
typedef struct { char token[AG_OPEN_TOKEN + 1]; uint64_t size; uint8_t sha[32]; uint32_t win, batch; int32_t x, y; uint8_t index, count; } AgPlace;
/* 0 well formed: exact length, a valid token, win ≠ 0, x and y 0..LAG_DIM_MAX, batch ≥ 1, index < count ≤ AG_PLACE_MAX */
int ag_place_parse(const uint8_t *p, size_t n, AgPlace *out);
int ag_place_write(uint8_t out[AG_PLACE_BYTES], const AgPlace *a);
/* 0 well formed: exact length, a valid token, a known status and how (AGP_NONE unless AGOS_OK), code 0 with AGOS_OK */
int ag_placed_parse(const uint8_t *p, size_t n, char token[AG_OPEN_TOKEN + 1], uint8_t *status, uint8_t *how, uint32_t *code);
int ag_placed_write(uint8_t out[AG_PLACED_BYTES], const char *token, uint8_t status, uint8_t how, uint32_t code);

/* AGK_DISPLAY: 0 a well-formed size (8 bytes, 320..8192 × 200..8192), -1 otherwise */
#define AG_DISPLAY_MIN_W 320u
#define AG_DISPLAY_MIN_H 200u
#define AG_DISPLAY_MAX   8192u
int ag_display_parse(const uint8_t *p, size_t n, uint32_t *w, uint32_t *h);
void ag_display_write(uint8_t out[8], uint32_t w, uint32_t h);

/* AGK_DISPLAYS (the guest's displays, for the log). AGDS_*: the primary display; on a hardware (PCI) adapter */
enum { AGK_DISPLAYS = 18 };
#define AG_DISPLAYS_MAX     8
#define AG_DISPLAY_NAME_MAX 32
#define AG_DISPLAYS_NONE    0xffu
#define AG_DISPLAYS_BYTES   (2 + AG_DISPLAYS_MAX * (18 + AG_DISPLAY_NAME_MAX))
enum { AGDS_PRIMARY = 1, AGDS_HW = 2, AGDS_ALL = 3 };
typedef struct { int32_t x, y; uint32_t w, h; uint8_t flags; char name[AG_DISPLAY_NAME_MAX + 1]; } AgDisplay;
/* the number of displays (1..AG_DISPLAYS_MAX) copied into out (names NUL-terminated), *shown the shown one's index or
 * -1; -1 malformed (exact length, every field within the limits above) */
int ag_displays_parse(const uint8_t *p, size_t n, AgDisplay out[AG_DISPLAYS_MAX], int *shown);
/* bytes written (≤ AG_DISPLAYS_BYTES), 0 when the list isn't one ag_displays_parse would take or doesn't fit */
size_t ag_displays_write(const AgDisplay *d, int n, int shown, uint8_t *out, size_t cap);
/* one line for a log: "\\.\DISPLAY1 1022x724 at 0,0 primary; \\.\DISPLAY2 1920x1080 at 1022,0 (shown)" */
void ag_displays_text(const AgDisplay *d, int n, int shown, char *out, size_t cap);

/* apps mode: limits, the VM_AGENT_STATE b bit (vmproto.h's VAC_* go up to VAC_DISPLAY, 128), flags, actions */
#define LAG_CAP_WINDOWS 256u
#define LAG_WIN_MAX     64
#define LAG_TITLE_MAX   512
#define LAG_APP_MAX     256
#define LAG_ICON_MAX    64
#define LAG_APPS_MAX    256
#define LAG_NAME_MAX    256
#define LAG_TOKEN_MAX   512
#define LAG_DIM_MAX     16384
enum { LAGW_FOCUSED = 1, LAGW_MINIMIZED = 2, LAGW_TOPMOST = 4, LAGW_OWNED = 8, LAGW_ALL = 15 };
enum { LAGA_FOCUS = 1, LAGA_CLOSE = 2, LAGA_MINIMIZE = 3, LAGA_RESTORE = 4, LAGA_MOVE = 5 };

typedef struct {
    uint32_t id, flags, owner, pid;                 /* owner: 0 unless LAGW_OWNED */
    int32_t x, y, w, h;
    const uint8_t *title, *app;                     /* pointers into the payload, not NUL-terminated */
    uint16_t title_n, app_n;
} LagWin;
typedef struct { uint32_t id; uint8_t action; int32_t x, y, w, h; } LagAct;
typedef struct { const uint8_t *name, *token; uint16_t name_n, token_n; } LagApp;

/* LAG_APPS: 0 ok (*on 0 or 1) */
int lag_apps_parse(const uint8_t *p, size_t n, int *on);
/* LAG_WINDOWS: the number of windows (0..max, at most LAG_WIN_MAX) filled into out, or -1 malformed */
int lag_windows_parse(const uint8_t *p, size_t n, uint32_t *serial, LagWin *out, int max);
/* LAG_WINICON: 0 ok (*rgba points into the payload: w × h × 4 bytes) */
int lag_winicon_parse(const uint8_t *p, size_t n, uint32_t *id, uint16_t *w, uint16_t *h, const uint8_t **rgba);
/* LAG_WINACT: 0 ok */
int lag_winact_parse(const uint8_t *p, size_t n, LagAct *a);
/* LAG_APPLIST: the number of programs filled into out (at most max), or -1 malformed */
int lag_applist_parse(const uint8_t *p, size_t n, LagApp *out, int max);
/* LAG_LAUNCH: 0 ok (*tok points into the payload) */
int lag_launch_parse(const uint8_t *p, size_t n, const uint8_t **tok, uint16_t *tok_n);

/* the parts of an AGK_CLIPX payload (pointers into it; NULL/0 for a form that isn't there) */
typedef struct { const uint8_t *text, *html, *png; uint32_t text_n, html_n, png_n; } AgClip;
/* 0 a well-formed payload with at least one part (lengths within the limits, no form twice), -1 otherwise. Contents
 * aren't checked here: ag_utf8_ok, ag_png_check, ag_html_sanitize */
int ag_clipx_parse(const uint8_t *p, size_t n, AgClip *c);
size_t ag_clipx_size(const AgClip *c);
void ag_clipx_write(const AgClip *c, uint8_t *out);         /* ag_clipx_size(c) bytes */
/* a PNG whose header can be believed: signature, IHDR first, 1..AG_PNG_DIM a side, a valid depth and color type. 0 ok */
int ag_png_check(const uint8_t *p, size_t n, uint32_t *w, uint32_t *h);
/* HTML from the other side, made harmless: formatting tags and their text only (see the top of vmagent.c). Returns the
 * output's length (≤ cap - 1, NUL-terminated), or -1 if it doesn't fit. Input must be valid UTF-8. */
long ag_html_sanitize(const char *in, size_t n, char *out, size_t cap);

typedef struct { uint16_t kind, chan; const uint8_t *data; uint32_t len; } AgFrame;

/* the next frame at buf: >0 bytes it took (f filled), 0 not complete yet, -1 malformed (too long) */
int ag_frame_next(const uint8_t *buf, size_t n, AgFrame *f);
/* the header for a payload of len bytes */
void ag_frame_head(uint8_t out[AG_HEAD], uint16_t kind, uint16_t chan, uint32_t len);
/* valid UTF-8 without NUL (what a clipboard text may be). 1 yes */
int ag_utf8_ok(const uint8_t *p, size_t n);
/* base64 (qemu-guest-agent's file and exec data): encoded/decoded length, or -1 */
long ag_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);
long ag_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap);

/* A file dropped on the machine's window (VM_FILE): its name as the guest gets it, safe on Linux and Windows file
 * systems. The last part of `in` (after any '/'); valid UTF-8 or refused; control characters and \ : * ? " < > |
 * become '_'; leading spaces and trailing spaces and dots go; "." and ".." are refused; a DOS device name (CON, PRN,
 * AUX, NUL, COM1-9, LPT1-9, with any extension) gets a '_' in front; at most AG_NAME_MAX bytes, cut on a character
 * boundary with its extension (up to 16 bytes) kept. 0 ok (out NUL-terminated), -1 nothing usable. */
#define AG_NAME_MAX 180
int ag_drop_name(const char *in, char *out, size_t cap);
/* qemu-guest-agent's guest-exec output (out-data) as text for the log or the person: base64-decoded, every byte that
 * isn't printable ASCII or part of valid UTF-8 replaced by '?', line ends as spaces, trimmed; at most cap - 1 bytes. */
void ag_exec_text(const char *b64, char *out, size_t cap);
/* the private folder a drop is written into, as the guest's script named it: Linux /var/tmp/lordos-drop.XXXXXXXX
 * (8 letters or digits); Windows <drive>:\...\Temp\lordos-drop-<32 lowercase hex> (printable ASCII, no "..", no '/').
 * 1 it is one, 0 not */
int ag_drop_staging_ok(const char *p, int windows);

/* ---- Apps mode, copied out (lord-vm's relay, lord-server, lordos-guest's relay and its lord-server endpoint) ----
 * The frames above, parsed with the lag_*_parse functions and then held as copies (NUL-terminated text) so they can
 * outlive the payload and be written again: nothing a guest sends passes on as it came. On top of the parsers' checks,
 * text must have no control character (lag_text_ok). A minimized window may have no place; it is kept, and written
 * again, as 0, 0, 1, 1. */
#define LAG_APPNAME_MAX LAG_APP_MAX
/* the hello's capability words as bits (ag_hello_caps); LAG_CAP_WINDOWS is the VM_AGENT_STATE bit, not one of these */
enum { AG_HELLO_CLIPBOARD = 1, AG_HELLO_CLIPX = 2, AG_HELLO_WINDOWS = 4, AG_HELLO_NOCLIP = 8, AG_HELLO_CLIPREQ = 16, AG_HELLO_OPEN = 32, AG_HELLO_PLACE = 64,
       AG_HELLO_POINTER = 128, AG_HELLO_GVT = 256 };
typedef struct { uint32_t id, flags, owner, pid; int32_t x, y, w, h; char title[LAG_TITLE_MAX + 1], app[LAG_APP_MAX + 1]; } LagWinEntry;
typedef struct { uint32_t serial; int n; LagWinEntry w[LAG_WIN_MAX]; } LagWinList;
typedef struct { char name[LAG_NAME_MAX + 1], token[LAG_TOKEN_MAX + 1]; } LagAppEntry;
typedef struct { int n; LagAppEntry a[LAG_APPS_MAX]; } LagAppList;
#define LAG_WINDOWS_BYTES (6 + LAG_WIN_MAX * (36 + LAG_TITLE_MAX + LAG_APP_MAX))
#define LAG_APPLIST_BYTES (2 + LAG_APPS_MAX * (4 + LAG_NAME_MAX + LAG_TOKEN_MAX))

unsigned ag_hello_caps(const uint8_t *p, size_t n);
/* text from the other side: valid UTF-8, no NUL or other control character (C0, DEL, C1). 1 yes */
int lag_text_ok(const uint8_t *p, size_t n);
/* a rectangle a window may have (w, h 1..LAG_DIM_MAX, x, y within ±LAG_DIM_MAX). 1 yes */
int lag_rect_ok(int32_t x, int32_t y, int32_t w, int32_t h);
/* 0 well formed (lag_windows_parse, no control characters), copied into *out; -1 refused */
int lag_winlist_read(const uint8_t *p, size_t n, LagWinList *out);
size_t lag_windows_write(const LagWinList *l, uint8_t *out, size_t cap);   /* bytes written, 0 doesn't fit */
size_t lag_icon_write(uint32_t id, int w, int h, const uint8_t *rgba, uint8_t *out, size_t cap);
size_t lag_act_write(const LagAct *a, uint8_t *out, size_t cap);
/* LAG_LAUNCH's token, NUL-terminated into token (cap > LAG_TOKEN_MAX); 0 ok */
int lag_launch_token(const uint8_t *p, size_t n, char *token, size_t cap);
size_t lag_launch_write(const char *token, uint8_t *out, size_t cap);
int lag_applist_read(const uint8_t *p, size_t n, LagAppList *out);
size_t lag_applist_write(const LagAppList *l, uint8_t *out, size_t cap);
/* is token one of the list's? 1 yes */
int lag_applist_has(const LagAppList *l, const char *token);

#endif
