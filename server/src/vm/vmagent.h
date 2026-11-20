/* vmagent.h — org.lordos.agent, Machines' own channel into a guest (docs/VIRTUALIZATION.md 6.8, 6.17, 7.1.1), and
 * the bits lord-vm needs to talk to qemu-guest-agent.
 *
 * A virtio-serial port named org.lordos.agent: no network, no port, no VNC; lordos-guest (tools/guest) holds the guest's
 * end as the person signed in at the guest's display. Frames, both ways:
 *
 *   u32 length (little endian, of the payload)  u16 kind  u16 channel  payload
 *
 * At most AG_FRAME_MAX bytes of payload; anything longer, or a stream that doesn't parse, is dropped whole (the guest
 * is untrusted: lord-vm parses this inside its own sandbox, never lord-server). Kinds a side doesn't know are skipped,
 * so either side can grow.
 *   AGK_HELLO  guest → host  "lordos-guest <version> <capability>…" (text: clipboard, clipx, windows); the host answers
 *                            with its own hello
 *   AGK_CLIP   both ways     the clipboard's text, UTF-8; only in the directions the machine allows (machine.json)
 *   AGK_CLIPX  both ways     the clipboard in up to three forms at once (capability "clipx" in both hellos): parts of
 *                            u8 form (AGF_TEXT, AGF_HTML, AGF_PNG), u32 length (little endian), bytes; each form at most
 *                            once. Text and HTML are UTF-8 of at most AG_TEXT_MAX; a PNG at most AG_PNG_MAX bytes and
 *                            AG_PNG_DIM pixels a side. lord-vm checks the PNG's header and sanitizes the HTML (only
 *                            formatting tags and attributes stay, no scripts, styles that load anything, events or
 *                            links other than http, https and mailto) before anything goes on; lord-server re-encodes
 *                            the picture in lord-media's sandbox.
 * The host never names a program for the guest to run: in apps mode (LAG_LAUNCH, below) it can only send back one of the
 * tokens the guest's own list of apps gave it, and the guest launches only its own list's. The guest can't ask the host
 * for anything: what it sends is shown (titles, icons) or moves its own windows' places on the host. */
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

/* ---- Apps mode (docs/VIRTUALIZATION.md 7.2): each window of the guest as its own LordOS window ----
 *
 * A guest helper that can list and act on its system's windows says "windows" in its hello (LAG_CAP_WINDOWS): the
 * Windows helper (lordos-guest-win), or lordos-guest on a guest that runs LordOS (it asks the guest's own lord-server,
 * server/src/ui/js_guestwin.c). Integers little endian, every length exact (a payload with bytes left over is refused).
 *   LAG_APPS     host → guest  u8 on: apps mode on (the guest hides its own desktop: wallpaper, panels) or off
 *   LAG_WINDOWS  guest → host  u32 serial; u16 count (≤ LAG_WIN_MAX), top to bottom; each: u32 id (≠ 0, no repeats),
 *                              i32 x, y, w, h (the window's own area, in the guest framebuffer's pixels), u32 flags
 *                              (LAGW_*; an owned one is followed by u32 owner id), u32 pid, u16 title length
 *                              (≤ LAG_TITLE_MAX) + UTF-8 title, u16 app length (≤ LAG_APPNAME_MAX) + UTF-8 app or exe name
 *   LAG_WINICON  guest → host  u32 id; u16 w, h (1..LAG_ICON_MAX); w × h × 4 bytes of R, G, B, A (straight alpha)
 *   LAG_WINACT   host → guest  u32 id; u8 action (LAGA_*); LAGA_MOVE adds i32 x, y, w, h (the new area, as above)
 *   LAG_LAUNCH   host → guest  u16 length (1..LAG_TOKEN_MAX) + a token from the guest's last LAG_APPLIST
 *   LAG_APPLIST  guest → host  u16 count (≤ LAG_APPS_MAX); each: u16 length (1..LAG_APPNAME_MAX) + UTF-8 name, u16
 *                              length (1..LAG_TOKEN_MAX) + token (UTF-8). The host never makes up a token: it only sends
 *                              back one the guest listed (lord-vm checks), and the guest launches only its own list's.
 * Text is valid UTF-8 without control characters (below U+0020, U+007F). Rectangles: w, h 1..LAG_DIM_MAX and x, y within
 * ±LAG_POS_MAX, except a minimized window's (its place means nothing: anything, kept as 0, 0, 0, 0). */
#define LAG_WIN_MAX     64
#define LAG_TITLE_MAX   512
#define LAG_APPNAME_MAX 256
#define LAG_ICON_MAX    64
#define LAG_TOKEN_MAX   192         /* a token also fits in one VmMsg's text (VM_LAUNCH) */
#define LAG_APPS_MAX    256
#define LAG_DIM_MAX     8192
#define LAG_POS_MAX     16384
enum { LAG_APPS = 4, LAG_WINDOWS = 5, LAG_WINICON = 6, LAG_WINACT = 7, LAG_LAUNCH = 8, LAG_APPLIST = 9 };
/* the capabilities a hello names, as bits (ag_hello_caps) */
enum { LAG_CAP_CLIPBOARD = 1, LAG_CAP_CLIPX = 2, LAG_CAP_WINDOWS = 4 };
enum { LAGW_FOCUSED = 1, LAGW_MINIMIZED = 2, LAGW_TOPMOST = 4, LAGW_OWNED = 8, LAGW_ALL = 15 };
enum { LAGA_FOCUS = 1, LAGA_CLOSE = 2, LAGA_MINIMIZE = 3, LAGA_RESTORE = 4, LAGA_MOVE = 5 };

typedef struct { uint32_t id, flags, owner, pid; int32_t x, y, w, h; char title[LAG_TITLE_MAX + 1], app[LAG_APPNAME_MAX + 1]; } LagWin;
typedef struct { uint32_t serial; int n; LagWin w[LAG_WIN_MAX]; } LagWinList;
typedef struct { char name[LAG_APPNAME_MAX + 1], token[LAG_TOKEN_MAX + 1]; } LagApp;
typedef struct { int n; LagApp a[LAG_APPS_MAX]; } LagAppList;
typedef struct { uint32_t id; int action; int32_t x, y, w, h; } LagAct;
#define LAG_WINDOWS_BYTES (6 + LAG_WIN_MAX * (32 + 4 + LAG_TITLE_MAX + LAG_APPNAME_MAX))
#define LAG_APPLIST_BYTES (2 + LAG_APPS_MAX * (4 + LAG_APPNAME_MAX + LAG_TOKEN_MAX))

/* the hello's capability words ("clipboard", "clipx", "windows") as LAG_CAP_* bits; words it doesn't know are skipped */
unsigned ag_hello_caps(const uint8_t *p, size_t n);
/* text from the other side: valid UTF-8, no NUL or other control character. 1 yes */
int lag_text_ok(const uint8_t *p, size_t n);
/* Each parser: 0 the payload is well formed and within every limit above (out filled), -1 refused (out undefined). */
int lag_windows_parse(const uint8_t *p, size_t n, LagWinList *out);
size_t lag_windows_write(const LagWinList *l, uint8_t *out, size_t cap);   /* bytes written, 0 doesn't fit */
/* LAG_WINICON: *px points into p (w × h × 4 RGBA) */
int lag_icon_parse(const uint8_t *p, size_t n, uint32_t *id, int *w, int *h, const uint8_t **px);
size_t lag_icon_write(uint32_t id, int w, int h, const uint8_t *rgba, uint8_t *out, size_t cap);
int lag_act_parse(const uint8_t *p, size_t n, LagAct *a);
size_t lag_act_write(const LagAct *a, uint8_t *out, size_t cap);
/* a rectangle a window may have (w, h 1..LAG_DIM_MAX, x, y within ±LAG_POS_MAX). 1 yes */
int lag_rect_ok(int32_t x, int32_t y, int32_t w, int32_t h);
int lag_launch_parse(const uint8_t *p, size_t n, char *token, size_t cap);
size_t lag_launch_write(const char *token, uint8_t *out, size_t cap);
int lag_applist_parse(const uint8_t *p, size_t n, LagAppList *out);
size_t lag_applist_write(const LagAppList *l, uint8_t *out, size_t cap);
/* is token one of the list's? 1 yes */
int lag_applist_has(const LagAppList *l, const char *token);

#endif
