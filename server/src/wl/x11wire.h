/* x11wire.h — the small part of the X11 wire protocol that LordOS's X window manager (xwm.c)
 * speaks with Xwayland. Our own code, no libxcb/libX11 (dependency policy, docs/ARCHITECTURE.md):
 * request encoding, packet framing, the connection setup reply and the ICCCM/EWMH properties the
 * window manager reads. Pure functions with no I/O, tested by tests/xwm_test.c. */
#ifndef LORD_X11WIRE_H
#define LORD_X11WIRE_H

#include <stddef.h>
#include <stdint.h>

/* ---- core request opcodes ---- */
enum {
    X_CreateWindow = 1, X_ChangeWindowAttributes = 2, X_GetWindowAttributes = 3, X_DestroyWindow = 4,
    X_MapWindow = 8, X_UnmapWindow = 10, X_ConfigureWindow = 12, X_GetGeometry = 14, X_InternAtom = 16,
    X_GetAtomName = 17, X_ChangeProperty = 18, X_DeleteProperty = 19, X_GetProperty = 20, X_SetSelectionOwner = 22, X_GetSelectionOwner = 23,
    X_ConvertSelection = 24, X_SendEvent = 25, X_SetInputFocus = 42, X_CreatePixmap = 53, X_FreePixmap = 54,
    X_CreateGC = 55, X_FreeGC = 60, X_PutImage = 72, X_QueryExtension = 98, X_KillClient = 113,
};

/* ---- events ---- */
enum {
    X_Error = 0, X_Reply = 1, X_FocusIn = 9, X_FocusOut = 10, X_CreateNotify = 16, X_DestroyNotify = 17,
    X_UnmapNotify = 18, X_MapNotify = 19, X_MapRequest = 20, X_ReparentNotify = 21, X_ConfigureNotify = 22,
    X_ConfigureRequest = 23, X_PropertyNotify = 28, X_SelectionClear = 29, X_SelectionRequest = 30,
    X_SelectionNotify = 31, X_ClientMessage = 33, X_GenericEvent = 35,
};

/* event masks and window attribute bits */
#define XM_KeyPress            0x00000001u
#define XM_PropertyChange      0x00400000u
#define XM_StructureNotify     0x00020000u
#define XM_SubstructureNotify  0x00080000u
#define XM_SubstructureRedirect 0x00100000u
#define XM_FocusChange         0x00200000u
#define XCW_BackPixel          0x00000002u
#define XCW_OverrideRedirect   0x00000200u
#define XCW_EventMask          0x00000800u
#define XCW_Cursor             0x00004000u
/* ConfigureWindow value mask */
#define XCF_X 0x01
#define XCF_Y 0x02
#define XCF_W 0x04
#define XCF_H 0x08
#define XCF_Border 0x10
#define XCF_Sibling 0x20
#define XCF_StackMode 0x40
enum { X_Above = 0, X_Below = 1 };

/* ---- output: requests are appended to one buffer ---- */
typedef struct {
    uint8_t *b;
    size_t n, cap, start;
    int oom;                    /* an allocation failed: the connection is dropped */
} XOut;

void xo_begin(XOut *o, uint8_t opcode, uint8_t data);   /* opens a request (4-byte header) */
void xo_u8(XOut *o, uint8_t v);
void xo_u16(XOut *o, uint16_t v);
void xo_u32(XOut *o, uint32_t v);
void xo_bytes(XOut *o, const void *p, size_t n);        /* padded to 4 */
void xo_pad(XOut *o, size_t n);
void xo_end(XOut *o);                                   /* writes the length (in 4-byte units) */
void xo_drop(XOut *o, size_t n);                        /* n bytes were written to the socket */
void xo_free(XOut *o);

/* The 12-byte connection setup request (little endian, X11.0, no authorization). */
void x11_setup_request(uint8_t out[12]);

typedef struct {
    uint32_t id_base, id_mask, root, root_visual;
    uint32_t max_req_bytes;     /* maximum request length, in bytes */
    uint16_t width, height;
    uint8_t root_depth, min_keycode, max_keycode;
} XSetup;
/* Parses the setup reply (header included). 1 complete and accepted, 0 needs more bytes,
 * -1 refused or malformed. */
int x11_parse_setup(const uint8_t *p, size_t n, XSetup *s, size_t *used);

/* Length of the next packet (event, error or reply) in p, or 0 if not complete yet. */
size_t x11_packet_len(const uint8_t *p, size_t n);

/* The full sequence number of a reply/event, from its 16 low bits and the last one sent. */
uint32_t x11_seq_widen(uint32_t last_sent, uint16_t s16);

static inline uint16_t x11_get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t x11_get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- properties (ICCCM / EWMH / Motif) ---- */

/* WM_CLASS: "instance\0class\0". Both are copied (empty if missing), printable ASCII only. */
void x11_wm_class(const uint8_t *v, size_t n, char *inst, size_t ni, char *cls, size_t nc);

/* A title from WM_NAME/_NET_WM_NAME: UTF-8 is copied (invalid bytes become U+FFFD), Latin-1
 * (STRING) is converted; control characters are dropped. */
void x11_title(const uint8_t *v, size_t n, int latin1, char *out, size_t size);

/* _MOTIF_WM_HINTS: 1 the window wants a frame, 0 it draws its own (decorations off), -1 no opinion. */
int x11_motif_decorated(const uint32_t *v, size_t n);

typedef struct { int min_w, min_h, max_w, max_h; } XSizeHints;
void x11_size_hints(const uint32_t *v, size_t n, XSizeHints *h);    /* WM_NORMAL_HINTS (0 = none) */

/* WM_HINTS: 1 if the window takes keyboard focus (input hint true or absent). */
int x11_wm_hints_input(const uint32_t *v, size_t n);

/* _NET_WM_MOVERESIZE direction → what the theme does: -1 move, 0 nothing (cancel, keyboard),
 * otherwise the resize edges (1 top, 2 bottom, 4 left, 8 right, like xdg_toplevel.resize). */
int x11_moveresize_edges(uint32_t direction);

/* Clipboard targets: 1 a text target (UTF8_STRING, text/plain;charset=utf-8, STRING, TEXT,
 * text/plain), 0 otherwise. */
int x11_text_target(const char *name);
/* Preference among the text targets an owner offers (higher is better, 0 = not text). */
int x11_text_rank(const char *name);

/* Do two X resource ids belong to the same client? (The bits above the server's resource mask
 * are the client.) The window manager answers a paste request only from the focused window's client. */
static inline int x11_same_client(uint32_t a, uint32_t b, uint32_t id_mask) { return (a & ~id_mask) == (b & ~id_mask); }

/* ---- drag and drop (XDND, version 5: freedesktop.org's Drag-and-Drop Protocol for the X Window System) ----
 * The five 32-bit data fields of its client messages. What an X client sends is untrusted: every field is
 * checked where it's read. */
#define XDND_VERSION 5
#define XDND_MAX_TYPES 32               /* types one drag offers at most (more are left out) */

/* XdndAware's version → the one both sides speak (min of the two), 0 if it is older than 3. */
int x11_xdnd_version(uint32_t aware);
/* XdndEnter from the source window src: the version, bit 0 when more than three types are in its
 * XdndTypeList property, and the first three types. */
void x11_xdnd_enter(uint32_t d[5], uint32_t src, int version, const uint32_t *types, int n);
/* XdndPosition: the pointer in root coordinates (clamped to 0..32767), its time and the action asked for. */
void x11_xdnd_position(uint32_t d[5], uint32_t src, int x, int y, uint32_t time, uint32_t action);
/* The target's reply to a position, without a rectangle (so every motion is reported to it). */
void x11_xdnd_status(uint32_t d[5], uint32_t target, int accept, uint32_t action);
/* The target is done: whether it took the drop and with which action (version 5). */
void x11_xdnd_finished(uint32_t d[5], uint32_t target, int accepted, uint32_t action);

typedef struct { uint32_t win; int version, more, n; uint32_t types[3]; } XdndEnter;
/* 1 if the enter can be spoken (a window, version 3 or newer); its non-zero types in order. */
int x11_xdnd_parse_enter(const uint32_t d[5], XdndEnter *e);
typedef struct { uint32_t win; int accept; uint32_t action; } XdndStatus;
void x11_xdnd_parse_status(const uint32_t d[5], XdndStatus *s);    /* XdndStatus (and XdndFinished: accept, action) */
void x11_xdnd_xy(uint32_t v, int *x, int *y);                       /* "x << 16 | y" */

/* A list of atoms in a property (XdndTypeList, TARGETS): only type ATOM, format 32, the bytes the reply holds;
 * zeros are skipped and at most max kept. Returns how many. */
int x11_atom_list(const uint8_t *v, size_t bytes, int format, uint32_t type, uint32_t *out, int max);
/* An X client's type name (an atom's name, n bytes) as a Wayland MIME type in out: a name with a '/' as it is
 * (printable ASCII only, shorter than size), UTF8_STRING as text/plain;charset=utf-8. 0 for anything else
 * (TARGETS, MULTIPLE, Latin-1 STRING, a name with a control character). */
int x11_dnd_mime(const uint8_t *name, size_t n, char *out, size_t size);

/* X-Resource QueryClientIds reply (asked with the PID mask for every client): each client's resource base and
 * pid. Returns how many; a malformed entry ends the list; -1 if the reply has more than max entries. */
int x11_xres_pids(const uint8_t *r, size_t n, uint32_t *base, uint32_t *pid, int max);
/* Whether two X resources (windows) belong to clients of one process, as X-Resource tells it: both clients have a
 * known pid and it is the same. A toolkit's clipboard code that opens its own X connection (arboard, used by Rust
 * apps, has a helper window on a second connection) is still that app. 0 if n < 0 (too many clients), either has
 * no pid known, or a pid is out of range. */
int x11_same_process(uint32_t a, uint32_t b, const uint32_t *base, const uint32_t *pid, int n, uint32_t id_mask);
/* Whether an X drag of files may reach LordOS (Files copies them): the selection owner's client has a pid
 * the server knows, and no client of the display is sandboxed (sandboxed(pid) != 0; it is called for every pid):
 * on one X display any client can stand in for another's selection, so one sandboxed client is enough to
 * refuse. 0 if owner is 0, n < 0 (too many clients) or no pid is known for the owner. */
int x11_xdnd_trusted(uint32_t owner, const uint32_t *base, const uint32_t *pid, int n, uint32_t id_mask,
                     int (*sandboxed)(int pid));

/* An X app's drag's owner as X-Resource tells it: the window that owns XdndSelection, its client's pid (0: none
 * known) and whether its files may reach LordOS (x11_xdnd_trusted). */
typedef struct { uint32_t win, pid; int trusted; } XdndOwner;
void x11_xdnd_owner(XdndOwner *o, uint32_t win, const uint32_t *base, const uint32_t *pid, int n, uint32_t id_mask,
                    int (*sandboxed)(int pid));
/* Whether the data of an X app's drag may be read now: XdndSelection is still owned (cur, GetSelectionOwner)
 * by the very window that owned it at XdndEnter, and, when now is given (X-Resource asked again right before
 * the read), that window's client has the same pid and the same trust answer. Another X client taking the
 * selection, or one that connected meanwhile and got the same window id, refuses it. */
int x11_xdnd_same_owner(const XdndOwner *then, uint32_t cur, const XdndOwner *now);
/* A SelectionNotify of an X app's drag's data (the 32-byte event): 1 if it answers the ConvertSelection we
 * sent (requestor, selection, target and property exactly ours), 0 if it is the owner's refusal of it (the
 * property None), -1 to ignore it: one sent with SendEvent (the code's 0x80 bit: any X client can send one),
 * another event, or an answer to something else. */
int x11_xdnd_notify(const uint8_t ev[32], uint32_t requestor, uint32_t selection, uint32_t target, uint32_t prop);

/* First free display number (0..63), given a test for whether :n is in use (a live lock file or
 * a socket). -1 if they are all taken. */
int x11_pick_display(int (*taken)(int n, void *ctx), void *ctx);

#endif
