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
 *                            new enough for lord-vm to install Intel's driver for a GVT-g vGPU, vmgvtwin.h; perf:
 *                            they apply the performance profile, AGK_PERF); the
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
 *   AGK_STOPPING both ways  u8 AG_STOP_*: the guest powers off or restarts now; the host's echo says it was read
 *                            (below; capability "stopping" in the hello: the guest's shutdown hook is installed).
 *   AGK_TABLET guest → host  u8 mode (AG_TABLET_PRIMARY or AG_TABLET_DESKTOP, below): how the guest maps the absolute
 *                            tablet with several displays; lord-vm uses it for where clicks land (ag_tablet_layout).
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
 *   LAG_WINREFUSED guest → host  u32 id, u8 action (LAGA_*), u8 reason (LAGR_*); exactly LAG_WINREFUSED_BYTES: the guest
 *                            couldn't do that action to that window (lord-vm logs it). LAGR_ELEVATED: the window's
 *                            program runs with more rights than the helper (Windows: as administrator; its messages
 *                            and moves are refused by the system), so it closes only from inside the machine.
 *   LAGW_ELEVATED in a window's flags says the same ahead of time; the guest sets it only when the host's hello says
 *                            "elevated" (an older host refuses a flag it doesn't know).
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
/* Which display the machine's picture shows (lordos-guest-win; pure, so it is tested here). The picture only ever comes
 * from a PCI GPU (GVT-g, virtio-gpu, QXL, a passed-through card): while one display is on a PCI adapter (AGDS_HW), a
 * display that isn't (a basic or indirect display) is never chosen. Among the candidates: `locked` (an index, -1 none)
 * while it is still one; else one whose size is want_w × want_h (the size the host asked for: equal, or under it by less
 * than 8 pixels), the primary first; else the primary candidate; else the first. *lock gets what to remember: the
 * index chosen by size (or the kept lock), -1 otherwise. The index chosen, -1 for no display. */
int ag_shown_pick(const AgDisplay *d, int n, uint32_t want_w, uint32_t want_h, int locked, int *lock);
/* AGK_EXTRA host → guest (capability "extra" in the tools' hello): exactly 1 byte, 1 turn off every display that isn't on
 * a PCI adapter while the shown one is (never the shown one, never the last), 0 turn them back on. The tools say
 * "extraoff" in a new hello while they keep one off. 0 well formed */
enum { AGK_EXTRA = 20 };
int ag_extra_parse(const uint8_t *p, size_t n, int *off);
/* 1 when the displays have one the tools may turn off: one not on a PCI adapter, other than the shown one, which is */
int ag_displays_extra(const AgDisplay *d, int n, int shown);
/* where the shown display sits in the box around all the displays (the guest's whole desktop): L = { its corner x, y from
 * the box's top left, its width, height, the box's width, height }. 0 a layout to use (more than one display and the
 * box larger than the shown one, every side ≤ 32767); 1 none (L all 0: the picture is the whole desktop) */
int ag_displays_layout(const AgDisplay *d, int n, int shown, int L[6]);
/* AGK_TABLET guest → host (tools w10, "tablet" in their hello): exactly 1 byte, how the guest maps the absolute tablet
 * with several displays, as the guest's own input stack says it (lordos-guest-win reads it from the tablet's raw input:
 * MOUSE_VIRTUAL_DESKTOP set or not): AG_TABLET_PRIMARY its range covers the primary display only (what Windows does for
 * a HID tablet through mouhid, whose input carries no MOUSE_VIRTUAL_DESKTOP), AG_TABLET_DESKTOP the whole desktop.
 * Sent when it is first seen and when it changes. 0 well formed */
enum { AGK_TABLET = 21 };
enum { AG_TABLET_UNKNOWN = 0, AG_TABLET_PRIMARY = 1, AG_TABLET_DESKTOP = 2 };
int ag_tablet_parse(const uint8_t *p, size_t n, int *mode);
/* Where QEMU's tablet has to point so a click lands on the shown display's pixel, for how the guest maps the tablet
 * (mode: AG_TABLET_*; unknown counts as primary, what a Windows guest's HID tablet does): L as ag_displays_layout.
 * AG_TABLET_DESKTOP: ag_displays_layout's. Primary: the tablet covers the primary display; 1 none (the shown display is
 * the primary, or the only one: the picture is all the tablet covers), 2 none either, but the tablet can't reach the
 * shown display at all (another display is the primary: the tools make the shown one primary again). 0 a layout. */
int ag_tablet_layout(const AgDisplay *d, int n, int shown, int mode, int L[6]);
/* AGK_STOPPING guest → host: exactly 1 byte, the guest's system is about to power off (AG_STOP_POWEROFF: poweroff or
 * halt) or restart (AG_STOP_REBOOT: reboot or kexec). Sent by `lordos-guest --stopping <verb>`, which systemd-shutdown
 * runs from /usr/lib/systemd/system-shutdown/ with its verb once every service has stopped and the file systems are
 * read-only, just before the reboot(2) whose device shutdown resets virtio-gpu (QEMU issue 4918, docs/VIRTUALIZATION.md
 * "Graphics 3D"); no hello first (a fresh open of the port, the helper's own run is over by then). host → guest: the
 * same byte back once lord-vm has read it; the guest waits for it at most AG_STOP_WAIT_MS. lord-vm uses it only to name
 * the end of a run whose QEMU then aborted on that reset (a restart Machines starts again, or a shutdown); a guest's
 * tools whose hook is installed say "stopping" in their hello. 0 well formed */
enum { AGK_STOPPING = 22 };
enum { AG_STOP_POWEROFF = 1, AG_STOP_REBOOT = 2 };
#define AG_STOP_WAIT_MS 1500
int ag_stopping_parse(const uint8_t *p, size_t n, int *mode);
/* systemd-shutdown's verb as AG_STOP_*: "poweroff" and "halt" AG_STOP_POWEROFF, "reboot" and "kexec" AG_STOP_REBOOT,
 * anything else (NULL too) 0 */
int ag_stop_verb(const char *verb);
/* The guest's performance profile (docs/VIRTUALIZATION.md 7.1.4; machine.json perf_profile; tools w11, "perf" in their
 * hello). AGK_PERF host → guest: exactly 1 byte, 1 on, 0 off; lord-vm sends it after each hello of tools that said
 * "perf". With it on, the Windows tools, as the person and with nothing else from the host, switch the guest to the
 * High performance power plan and turn its visual effects (animations, shadows, fades, window contents while dragging)
 * and transparency to the minimum; they keep what was there before in the person's own registry and put it back when
 * the host says 0, when they let the port go (log off, another session at the console, exit) and when they start
 * after a crash. AGK_PERFSTATE guest → host, the outcome, for lord-vm's log only: exactly 3 bytes, u8 on (0 or 1), u8
 * the AGPF_* parts done (applied, or put back when off), u8 the parts the system refused; never a part in both. 0 well
 * formed */
enum { AGK_PERF = 23, AGK_PERFSTATE = 24 };
enum { AGPF_POWER = 1, AGPF_EFFECTS = 2, AGPF_TRANSPARENCY = 4, AGPF_ALL = 7 };
#define AG_PERFSTATE_BYTES 3
int ag_perf_parse(const uint8_t *p, size_t n, int *on);
int ag_perfstate_parse(const uint8_t *p, size_t n, int *on, unsigned *done, unsigned *failed);
int ag_perfstate_write(uint8_t out[AG_PERFSTATE_BYTES], int on, unsigned done, unsigned failed);

/* Guest → host links and notifications (docs/VIRTUALIZATION.md 6.12; tools 8 and w12, "links" and "notify" in their
 * hello). The guest is hostile: neither ever runs anything on this computer by itself.
 *   AGK_LINK guest → host: a link a program in the guest (or the person, `lordos-guest --open-on-host`) wants opened on
 *     this computer: 1..AG_LINK_MAX bytes, exactly what ag_link_ok takes, nothing else. lord-vm passes it on only while
 *     machine.json "guest_links" is on, at most AG_LINK_BURST at once and one per AG_LINK_EVERY_MS after; lord-server
 *     shows it in a bar over the machine ("‹machine› wants to open ‹link›" [Ignore] [Open]) only while the machine is
 *     in use, one at a time (another one while the bar shows is dropped, never swapped in under the person's pointer),
 *     and opens it in this computer's browser only when the person clicks Open.
 *   AGK_NOTIFY guest → host: a notification: u16 title length (1..AG_NOTE_TITLE_MAX) + title, u16 body length
 *     (0..AG_NOTE_BODY_MAX) + body, exactly that; UTF-8 without control characters or bidi overrides (ag_note_text_ok).
 *     Passed on only while machine.json "guest_notify" is on, at most AG_NOTE_BURST at once and one per
 *     AG_NOTE_EVERY_MS after; shown by lord-server as from the machine (its name first, Machines' icon), with no action,
 *     never with the host's privileges.
 * Neither exists for a machine with the anonymity profile (its agent is off, and machine.json refuses both). */
enum { AGK_LINK = 25, AGK_NOTIFY = 26 };
#define AG_LINK_MAX        2048
#define AG_NOTE_TITLE_MAX  120
#define AG_NOTE_BODY_MAX   600
#define AG_NOTE_BYTES      (4 + AG_NOTE_TITLE_MAX + AG_NOTE_BODY_MAX)
#define AG_LINK_BURST      3
#define AG_LINK_EVERY_MS   5000
/* the guest's clipboard (AGK_CLIP, AGK_CLIPX): each one becomes a memfd for lord-server and, for a picture, a re-encode
 * in lord-media; a guest that copies in a loop is held to a burst, then one per interval (a person copies far slower) */
#define AG_CLIP_BURST      4
#define AG_CLIP_EVERY_MS   250
#define AG_NOTE_BURST      5
#define AG_NOTE_EVERY_MS   3000
/* 1 when p is a link this computer may offer to open: letters, digits and - . _ ~ : / ? # [ ] @ & = + , ; % only (what
 * the launcher passes to a browser as one argument: no space, quote, backslash, parenthesis, < > { } | ^ `), every '%'
 * followed by two hex digits; http:// or https:// (scheme in any case) + a host of letters, digits, '.' and '-' (labels
 * not empty, not starting or ending with '-') or a bracketed IPv6 address, an optional :port (1..65535), then nothing
 * or '/', '?' or '#' and the rest; never a user name or password before the host ('@' in the authority: a spoofing
 * trick). No other scheme (mailto, file, javascript, data…) */
int ag_link_ok(const uint8_t *p, size_t n);
/* the guest's tools: a link as typed, with every byte outside that set (and a '%' not followed by two hex digits)
 * percent-encoded, so "https://x.org/a (b)" becomes "https://x.org/a%20%28b%29". Its length (out NUL-terminated), -1
 * when it doesn't fit cap. It doesn't make a link valid: ag_link_ok still decides */
long ag_link_encode(const uint8_t *in, size_t n, char *out, size_t cap);
/* text of a notification: lag_text_ok, and none of the bidi controls U+200E, U+200F, U+202A..U+202E, U+2066..U+2069
 * (they could make it read as the host's own words). 1 yes */
int ag_note_text_ok(const uint8_t *p, size_t n);
/* AGK_NOTIFY: 0 well formed (exact length, both texts ag_note_text_ok); pointers into the payload */
int ag_note_parse(const uint8_t *p, size_t n, const uint8_t **title, uint16_t *tn, const uint8_t **body, uint16_t *bn);
/* bytes written (≤ AG_NOTE_BYTES), 0 when they aren't a notification ag_note_parse would take or don't fit */
size_t ag_note_write(const uint8_t *title, size_t tn, const uint8_t *body, size_t bn, uint8_t *out, size_t cap);
/* A token bucket: at most `burst` at once, then one per `every_ms`. 1 allowed now (and counted), 0 not */
typedef struct { long long at; int tokens; } AgRate;
int ag_rate_take(AgRate *r, long long now_ms, int burst, int every_ms);

/* The agent's shell (docs/VIRTUALIZATION.md 6.18; tools 8 and w12, "shell" in their hello): a terminal inside the guest,
 * over this channel (no network, no SSH), as the person signed in at the guest's display, never root or an
 * administrator: the tools start that person's own login shell (Linux: their passwd entry's shell, else /bin/sh, in a
 * new pty; Windows: PowerShell in a pseudo console), never a program, argument or path the host names. Only the host
 * opens one; one at a time.
 *   AGK_SHELL_OPEN host → guest: u32 id (≥ 1, a new one per shell), u16 columns, u16 rows; exactly 8 bytes
 *   AGK_SHELL_SIZE host → guest: the same layout: the terminal's new size
 *   AGK_SHELL_DATA both ways: u32 id, then 1..AG_SHELL_CHUNK bytes (what was typed; what the shell wrote)
 *   AGK_SHELL_ACK  both ways: u32 id, u32 bytes (1..AG_SHELL_WINDOW): that many bytes of DATA were delivered (written to
 *     the pty, or to lord-server's terminal). Flow control: a side never has more than AG_SHELL_WINDOW bytes of DATA
 *     sent and not acknowledged; lord-vm ends a shell whose guest breaks that, or acknowledges more than it sent
 *   AGK_SHELL_CLOSE both ways: u32 id, i32 status; exactly 8 bytes. host → guest: end it (status 0); guest → host: it
 *     ended (the shell's exit status 0..255, 256 + a signal, or AG_SHELL_NOSTART: it couldn't start) */
enum { AGK_SHELL_OPEN = 27, AGK_SHELL_DATA = 28, AGK_SHELL_SIZE = 29, AGK_SHELL_CLOSE = 30, AGK_SHELL_ACK = 31 };
#define AG_SHELL_CHUNK    16384
#define AG_SHELL_WINDOW   (256 * 1024)
#define AG_SHELL_COLS_MAX 1000
#define AG_SHELL_ROWS_MAX 500
#define AG_SHELL_NOSTART  (-1)
/* AGK_SHELL_OPEN and AGK_SHELL_SIZE: 0 well formed (id ≥ 1, columns 1..AG_SHELL_COLS_MAX, rows 1..AG_SHELL_ROWS_MAX) */
int ag_shell_size_parse(const uint8_t *p, size_t n, uint32_t *id, int *cols, int *rows);
int ag_shell_size_write(uint8_t out[8], uint32_t id, int cols, int rows);
/* AGK_SHELL_DATA: 0 well formed (id ≥ 1, 1..AG_SHELL_CHUNK bytes; *data points into the payload) */
int ag_shell_data_parse(const uint8_t *p, size_t n, uint32_t *id, const uint8_t **data, uint32_t *dn);
/* AGK_SHELL_ACK: 0 well formed (id ≥ 1, 1..AG_SHELL_WINDOW) */
int ag_shell_ack_parse(const uint8_t *p, size_t n, uint32_t *id, uint32_t *bytes);
void ag_shell_pair_write(uint8_t out[8], uint32_t id, uint32_t v);      /* ACK and CLOSE: u32 id, u32 (or i32) value */
/* AGK_SHELL_CLOSE: 0 well formed (id ≥ 1, status AG_SHELL_NOSTART..511) */
int ag_shell_close_parse(const uint8_t *p, size_t n, uint32_t *id, int *status);

/* Files sent from the guest to this computer (docs/VIRTUALIZATION.md 6.21: "Send to LordOS"; tools 9 and w13, "send" in
 * their hello, the host's hello says "send" while machine.json takes them). Only the guest offers; one file at a time;
 * every file lands in this computer's quarantine folder for that machine (<library>/Received/<machine>), never anywhere
 * the guest names, marked as from that machine and never executable. The guest is hostile: every frame is parsed exactly,
 * a name is one plain file name, and lord-vm never has more than AG_SEND_WINDOW bytes in flight.
 *   AGK_SEND_OFFER guest → host: u32 id (≥ 1, the guest's own), u64 size (≤ AG_SEND_SIZE_MAX), u8 flags (AGSF_*, nothing
 *     else), 32 bytes key (the guest's resume key: the same file offered again after a lost connection gives the same key,
 *     opaque to the host), u16 name length (1..AG_NAME_MAX) + the file's name (ag_send_name_ok); exactly that.
 *   AGK_SEND_ACCEPT host → guest: u32 id, u8 status (AGSA_*), u64 offset, 32 bytes: the SHA-256 of the offset bytes the host
 *     already has of this key (a resume; zeros with offset 0); exactly AG_SEND_ACCEPT_BYTES. Any status but AGSA_OK has
 *     offset 0 and zeros.
 *   AGK_SEND_FROM guest → host: u32 id, u64 offset: where the data starts: the accept's offset when the guest's file starts
 *     with those very bytes (their SHA-256 matched), else 0 (the host starts the file again). Exactly 12 bytes.
 *   AGK_SEND_DATA guest → host: u32 id, u64 offset (exactly the next byte the host expects), 1..AG_SEND_CHUNK bytes.
 *   AGK_SEND_ACK host → guest: u32 id, u64 offset: every byte before it is written on this computer. The guest never has
 *     more than AG_SEND_WINDOW bytes sent past the last ACK; lord-vm ends a transfer whose guest does.
 *   AGK_SEND_END guest → host: u32 id, 32 bytes: the SHA-256 of the whole file, sent once all of its size went; exactly
 *     36 bytes. The host keeps the file only when its own SHA-256 of what it wrote is the same.
 *   AGK_SEND_DONE both ways: u32 id, u8 code (AGSD_*); exactly 5 bytes. host → guest: the outcome; guest → host: it gives
 *     up (the file couldn't be read, the person cancelled it). */
#define AG_NAME_MAX 180                            /* a file name's bytes (ag_drop_name, below) */
enum { AGK_SEND_OFFER = 32, AGK_SEND_ACCEPT = 33, AGK_SEND_FROM = 34, AGK_SEND_DATA = 35, AGK_SEND_ACK = 36, AGK_SEND_END = 37,
       AGK_SEND_DONE = 38 };
#define AG_SEND_CHUNK        65536
#define AG_SEND_WINDOW       (4u << 20)
#define AG_SEND_SIZE_MAX     (1ULL << 40)          /* 1 TiB */
#define AG_SEND_OFFER_MIN    (4 + 8 + 1 + 32 + 2 + 1)
#define AG_SEND_OFFER_BYTES  (4 + 8 + 1 + 32 + 2 + AG_NAME_MAX)
#define AG_SEND_ACCEPT_BYTES (4 + 1 + 8 + 32)
#define AG_SEND_OFFER_BURST  8                     /* offers: at most 8 at once, then one per 2 s */
#define AG_SEND_OFFER_EVERY_MS 2000
/* AGSF_EXEC: the file could be run in the guest (an x bit, an .exe): said to the person, never kept on this computer */
enum { AGSF_EXEC = 1, AGSF_ALL = 1 };
/* AGK_SEND_ACCEPT's status: taken (from offset); this machine doesn't take files (Configure, Sharing); not now (another
 * file is on its way, or no Machines window is there to show it: offer it again later); no room on this computer; refused
 * (too many offers, a name or size this computer won't take) */
enum { AGSA_OK = 0, AGSA_OFF = 1, AGSA_BUSY = 2, AGSA_SPACE = 3, AGSA_REFUSED = 4, AGSA_LAST = AGSA_REFUSED };
/* AGK_SEND_DONE's code: kept; cancelled (by the person on either side); the SHA-256 differs; this computer couldn't write
 * it, or the guest couldn't read it; no room left; later (host → guest: this computer stopped it for now, its Machines
 * window went: what came is kept, offer the same file again later and it goes on from there) */
enum { AGSD_OK = 0, AGSD_CANCELLED = 1, AGSD_BADHASH = 2, AGSD_FAILED = 3, AGSD_SPACE = 4, AGSD_LATER = 5, AGSD_LAST = AGSD_LATER };
typedef struct { uint32_t id; uint64_t size; uint8_t flags; uint8_t key[32]; char name[AG_NAME_MAX + 1]; } AgSendOffer;
/* a file name from the guest this computer may take as one: 1..AG_NAME_MAX bytes, ag_note_text_ok (UTF-8, no control or
 * bidi characters), no '/' or '\\', not "." or "..". 1 yes */
int ag_send_name_ok(const uint8_t *p, size_t n);
/* AGK_SEND_OFFER: 0 well formed (exact length; id ≥ 1, size ≤ AG_SEND_SIZE_MAX, flags within AGSF_ALL, the name
 * ag_send_name_ok), the name copied NUL-terminated */
int ag_send_offer_parse(const uint8_t *p, size_t n, AgSendOffer *o);
size_t ag_send_offer_write(const AgSendOffer *o, uint8_t *out, size_t cap);   /* bytes, 0 not one the parser takes */
/* AGK_SEND_ACCEPT: 0 well formed (id ≥ 1, a known status, offset ≤ AG_SEND_SIZE_MAX, offset 0 and zeros unless AGSA_OK) */
int ag_send_accept_parse(const uint8_t *p, size_t n, uint32_t *id, uint8_t *status, uint64_t *offset, uint8_t sha[32]);
int ag_send_accept_write(uint8_t out[AG_SEND_ACCEPT_BYTES], uint32_t id, uint8_t status, uint64_t offset, const uint8_t sha[32]);
/* AGK_SEND_FROM and AGK_SEND_ACK: u32 id (≥ 1), u64 offset (≤ AG_SEND_SIZE_MAX); 0 well formed */
int ag_send_pos_parse(const uint8_t *p, size_t n, uint32_t *id, uint64_t *offset);
int ag_send_pos_write(uint8_t out[12], uint32_t id, uint64_t offset);
/* AGK_SEND_DATA: 0 well formed (id ≥ 1, offset + bytes ≤ AG_SEND_SIZE_MAX, 1..AG_SEND_CHUNK bytes; *data into the payload) */
int ag_send_data_parse(const uint8_t *p, size_t n, uint32_t *id, uint64_t *offset, const uint8_t **data, uint32_t *dn);
void ag_send_data_head(uint8_t out[12], uint32_t id, uint64_t offset);   /* then the bytes */
/* AGK_SEND_END: 0 well formed (exactly 36 bytes, id ≥ 1) */
int ag_send_end_parse(const uint8_t *p, size_t n, uint32_t *id, uint8_t sha[32]);
int ag_send_end_write(uint8_t out[36], uint32_t id, const uint8_t sha[32]);
/* AGK_SEND_DONE: 0 well formed (exactly 5 bytes, id ≥ 1, a known code) */
int ag_send_done_parse(const uint8_t *p, size_t n, uint32_t *id, uint8_t *code);
int ag_send_done_write(uint8_t out[5], uint32_t id, uint8_t code);
/* the name a received file gets in the quarantine folder: ag_drop_name's rules (safe on Linux and Windows file systems),
 * and a leading '.' becomes '_' (nothing hidden), bidi controls refused. 0 ok, -1 nothing usable */
int ag_recv_name(const char *in, char *out, size_t cap);
/* the guest tools' side of flow control, pure so it is tested here: may `n` more bytes go, with `sent` the next offset
 * to send and `acked` the last ACK's offset? 1 yes */
int ag_send_room(uint64_t sent, uint64_t acked, uint32_t n);

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
enum { LAGW_FOCUSED = 1, LAGW_MINIMIZED = 2, LAGW_TOPMOST = 4, LAGW_OWNED = 8, LAGW_ELEVATED = 16, LAGW_ALL = 31 };
enum { LAGA_FOCUS = 1, LAGA_CLOSE = 2, LAGA_MINIMIZE = 3, LAGA_RESTORE = 4, LAGA_MOVE = 5 };
/* LAG_WINREFUSED: why. The window runs with more rights than the helper; the system refused for another reason */
enum { LAG_WINREFUSED = 19 };
enum { LAGR_ELEVATED = 1, LAGR_DENIED = 2, LAGR_LAST = LAGR_DENIED };
#define LAG_WINREFUSED_BYTES 6
/* 0 well formed: exactly LAG_WINREFUSED_BYTES, id ≠ 0, a known action and reason */
int lag_winrefused_parse(const uint8_t *p, size_t n, uint32_t *id, uint8_t *action, uint8_t *reason);
int lag_winrefused_write(uint8_t out[LAG_WINREFUSED_BYTES], uint32_t id, uint8_t action, uint8_t reason);

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
       AG_HELLO_POINTER = 128, AG_HELLO_GVT = 256, AG_HELLO_EXTRA = 512, AG_HELLO_EXTRAOFF = 1024, AG_HELLO_STOPPING = 2048,
       AG_HELLO_PERF = 4096, AG_HELLO_SHELL = 8192, AG_HELLO_LINKS = 16384, AG_HELLO_NOTIFY = 32768, AG_HELLO_SEND = 65536 };
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
