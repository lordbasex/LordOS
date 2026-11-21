/* vmproto.h — Machines (docs/VIRTUALIZATION.md, "Phase 1 implementation"): the fixed binary messages between
 * lord-server and the two per-machine helpers.
 *
 *   lord-server ──VmMsg──▶ lord-vm         supervisor of one machine: QEMU, passt, QMP, limits, state
 *   lord-server ──VdMsg──▶ lord-vmdisplay  the machine's picture and input, peer to peer D-Bus with QEMU
 *
 * Both channels are AF_UNIX SOCK_SEQPACKET: one message per packet, never split, never merged. Every message is
 * exactly sizeof(VmMsg) or sizeof(VdMsg) bytes; a packet of any other length is a protocol error and the reader
 * drops the peer. Integers are host order (both ends run on one computer). Descriptors travel as SCM_RIGHTS on
 * the message that names them, at most one per message. Nothing here is JSON and nothing here is parsed from
 * text, except VmMsg.text, which is always NUL-terminated by the reader (vm_msg_fix) before use.
 *
 * ---- lord-vm ----
 * Started as `lord-vm --control-fd 3 [--paused] <machine dir>` (lord-server keeps the other end), or by the
 * development CLI (`lord-vm --start <dir>`). Each running lord-vm also listens on
 * $XDG_RUNTIME_DIR/lordos-vm/<id>.sock (folder 0700, socket 0600, peer uid checked), with the same messages,
 * so a restarted lord-server or the CLI can reach a machine that is already running. lord-vm exits when QEMU
 * has exited, after the last VM_STATE.
 *
 *   to lord-vm (requests; seq is echoed in the VM_REPLY)
 *     VM_STATUS     → VM_INFO, then VM_STATE
 *     VM_PAUSE      QMP stop                                    → VM_REPLY
 *     VM_RESUME     QMP cont                                    → VM_REPLY
 *     VM_SHUTDOWN   a = seconds to wait (0: 60, max 600); ACPI power button, then QMP quit, then SIGKILL
 *     VM_POWEROFF   QMP quit, then SIGKILL after 5 s
 *     VM_RESET      QMP system_reset
 *     VM_DISPLAY    → VM_DISPLAY_FD with one descriptor: a D-Bus connection QEMU already accepted
 *                   (QMP getfd + add_client @dbus-display). lord-server hands it to lord-vmdisplay as fd 4.
 *                   Refused (VM_REPLY VME_NO_DISPLAY) for a machine whose display is "none".
 *     VM_QMP        development only (lord-vm --dev-qmp): text is one QMP command; the reply's text is the
 *                   start of QEMU's answer. Used by the isolation test; refused otherwise.
 *     VM_CDROM      the CD/DVD drive while the machine runs. With a descriptor (an image lord-server opened read-only
 *                   for the person): that disc goes in, through QMP add-fd and /dev/fdset (QEMU never opens a path,
 *                   so its Landlock rules don't change); text = the image's path, kept in machine.json when it fits.
 *                   Without one: the disc comes out. → VM_REPLY once the tray is closed again
 *     VM_SUSPEND    saves the machine's memory and devices to <machine>/suspend.state (QMP migrate to a descriptor
 *                   lord-vm opened) and stops QEMU; the next start resumes from it (6.1). VM_JOB reports progress;
 *                   → VM_REPLY when the state is on disk (QEMU then quits: VM_STATE stopped, suspended)
 *     VM_KEEP       a = 1: the machine keeps running when the control channel closes (Keep Running); 0: it shuts
 *                   down then, as by default
 *     VM_CONSOLE    → VM_CONSOLE_FD: a stream to the guest's serial console (6.18, the Shell): what the guest writes
 *                   comes out of it (the last 8 KB first), what's written to it goes to the guest. One at a time; a
 *                   new one replaces the old. The serial log goes on as before.
 *     VM_AGENT      the guest agent (6.17; machine.json "agent"): a = VAG_SHUTDOWN (guest-shutdown), VAG_TIMESYNC
 *                   (guest-set-time from this computer's clock), VAG_INSTALL (copies lordos-guest, the clipboard helper,
 *                   into a Linux guest through qemu-guest-agent and enables it; into a Windows guest, the LordOS guest
 *                   tools, lordos-guest-win.exe, into %ProgramFiles%\LordOS, started at every sign-in), VAG_TRANSFER (the
 *                   transfer folder, 6.7.1: a Linux guest mounts it again; a Windows guest gets WinFsp: one descriptor, its
 *                   installer, which lord-server opened in the machines' image store, text = its SHA-256 (64 hex), checked
 *                   again here and in the guest with its signature, installed by msiexec as SYSTEM from a private folder)
 *                   → VM_REPLY
 *     VM_CLIP       the host's clipboard for the guest: one descriptor, a memfd of a bytes (at most VM_CLIP_MAX); b =
 *                   VCF_TEXT: UTF-8 text; VCF_CLIPX: text, HTML and/or a PNG in AGK_CLIPX's layout (vmagent.h), checked
 *                   and sanitized again here. Sent on only when machine.json's clipboard allows to-guest and
 *                   lordos-guest said hello (as text only to a helper without "clipx"). c = 0: pushed as it is (a helper
 *                   without "clipreq", VAC_CLIP_LAZY unset). c = an offer's generation, d = AGF_*: the answer to the
 *                   guest's request (VM_CLIP_REQ) for that form of that offer: sent on as AGK_CLIPDATA, and only when
 *                   that request is waiting
 *     VM_CLIP_OFFER the host's clipboard on demand (6.8, VAC_CLIP_LAZY): a = the offer's generation (≥ 1, lord-server's
 *                   counter), b = AGM_* forms it holds, c = AGO_* flags; sent on as AGK_CLIPOFFER, no contents
 *     VM_CLIP_DENY  no clipboard for the guest: a = the offer's generation and b = the AGF_* form of a waiting request
 *                   (answered AGK_CLIPDATA refused, or stale with VCD_STALE), or a = 0 and b = 0: the guest's last
 *                   clipboard was ignored; c = VCD_* why. lord-vm writes it in the machine's log
 *     VM_DISPLAY_SIZE a, b = the machine window's size (320..8192 × 200..8192), sent by lord-server with each VD_RESIZE.
 *                   lord-vm remembers it and passes it to a guest helper that sets its own display's size (AGK_DISPLAY,
 *                   the LordOS guest tools for Windows; again after each hello); others follow QEMU's ui_info → VM_REPLY
 *     VM_FILE       a file dropped on the machine's window, for the guest: one descriptor, a regular file lord-server
 *                   opened read-only for the person (never a path); text = its name (lord-vm makes it safe for the
 *                   guest's file systems, ag_drop_name). lord-vm copies it through qemu-guest-agent, in pieces, one file
 *                   at a time (at most 8 wait), into the Downloads folder of the guest's signed-in person: on Linux
 *                   through a private temporary folder, then moved and owned by that person (runuser); on Windows
 *                   through a private SYSTEM-only folder in %SystemRoot%\Temp, then copied into their Downloads by a
 *                   one-shot scheduled task that runs as that person (refused when nobody is signed in).
 *                   When the guest has the transfer folder mounted (VAC_TRANSFER, 6.7.1), the file is first copied by
 *                   lord-vm into that folder on this computer (a reflink when the file system can; in a child process)
 *                   in a 0700 folder of its own, with the file's SHA-256, and the person's own copy in the guest reads it
 *                   from there and is named only when its size and hash match; if that way fails, the agent's way is
 *                   tried once.
 *                   a = 1: then open it there (Files' "Open with" a machine, 7.3): only for a guest whose tools said
 *                   "open" (VAC_OPEN); lord-vm makes a random token for the drop, the person's copy step records where
 *                   the file went under it, and once the copy is done lord-vm sends the guest tools AGK_OPEN (the token,
 *                   the size and SHA-256 it measured); the VM_REPLY then waits for AGK_OPENED (or AG_OPEN_WAIT_MS).
 *                   VM_FILE_PROGRESS on the way; → VM_REPLY with the file's path in the guest (the guest's words, text only) or why not
 *     VM_SNAPSHOT   a = VSN_TAKE, VSN_REVERT or VSN_DELETE; text = "TAG\tNAME" (take) or "TAG". qcow2 internal
 *                   snapshots of every writable disk at once (system disk and UEFI variables), with the machine's
 *                   memory (QMP snapshot-save, snapshot-load, snapshot-delete jobs), under operation.json; the entry
 *                   in snapshots.json is written last (6.2). VM_JOB reports it; → VM_REPLY when it's done
 *     VM_SOUND      machine.json's sound choices while the machine runs (lord-vm holds the lock, so it writes the file):
 *                   a = sound (1 plays on this computer, 0 muted), b = volume 0..100, c = microphone (1 allowed, 0 not);
 *                   d = 1: text = "OUT\tIN", the output and input (machine.json sound_out, sound_in: a node.name each, or
 *                   empty for the default; vm_sound_dev_valid), d = 0: they stay as they are.
 *                   They change nothing in QEMU: lord-server applies them itself (6.9) → VM_REPLY
 *     VM_APPS       apps mode (7.2): a = 1 on, 0 off; sent on to the guest's helper (LAG_APPS) only when the machine's
 *                   agent is on and the helper said "windows" (LAG_CAP_WINDOWS) → VM_REPLY
 *     VM_WINACT     a window of the guest: a = its id (as the guest named it), b = LAGA_* (focus, close, minimize,
 *                   restore, move); move: text = "x y w h" (decimal, the new area in the guest's pixels, lag_rect_ok)
 *                   → VM_REPLY. lord-vm builds the LAG_WINACT frame itself (lag_act_write)
 *     VM_LAUNCH     a = an app's place in the guest's list, b = that list's generation (VM_WINDATA c): lord-vm sends the
 *                   guest that app's token from its own copy of the list; refused when the list changed since → VM_REPLY
 *   from lord-vm (events and replies)
 *     VM_REPLY      seq of the request; a = VME_* (0 ok); text = reason in English, for the log
 *     VM_STATE      a = VS_*, b = VR_* (why), c = QEMU's exit status or signal, d = QEMU's pid; text = detail.
 *                   Sent to every client on each change and in answer to VM_STATUS.
 *     VM_INFO       a = vCPUs, b = memory MB, c = VMS_* bits (what the sandbox applied), d = VMC_* (cgroups);
 *                   text = the machine's id. Sent once QEMU runs, so it can follow the first VM_STATEs
 *                   (starting, paused); lord-server asks for the display once it has both.
 *     VM_DISPLAY_FD seq of the VM_DISPLAY; the descriptor; a = console width, b = height if known (else 0)
 *     VM_AGENT_STATE a = VAS_* (off, waiting, connected), b = VAC_* and LAG_CAP_WINDOWS (what works now), text = "version\tsystem" as
 *                   the guest reported them (untrusted, shown as text only). Sent on each change and after VM_STATUS
 *     VM_CLIP_DATA  the guest's clipboard (machine.json allows from-guest): a sealed memfd of a bytes; b = VCF_TEXT
 *                   (UTF-8) or VCF_CLIPX (AGK_CLIPX's layout, every part already checked: UTF-8 text, sanitized HTML,
 *                   a PNG whose header is sane; lord-server still re-encodes the picture before it's used)
 *     VM_FILE_PROGRESS seq of the VM_FILE; a = percent copied (0..100); b = 1 through the transfer folder, 0 the agent's
 *                   way; text = the file's name as the guest gets it.
 *                   Sent to the client that asked, at most every 250 ms
 *     VM_CLIP_REQ   a program in the guest pastes: a = the offer's generation, b = AGF_* form (lord-vm checked that it's
 *                   the last offer's and one of its forms). Sent to every client; one answers with VM_CLIP (c, d) or
 *                   VM_CLIP_DENY
 *     VM_WINDATA    apps mode: what the guest's helper said, checked (vmagent.h's parsers) and written again by lord-vm:
 *                   a sealed memfd of a bytes (≤ VM_WINDATA_MAX), b = LAG_WINDOWS, LAG_WINICON or LAG_APPLIST, in that
 *                   kind's layout; c = the list of apps' generation (1, 2, ...). lord-server parses it again before use
 *     VM_JOB        progress of a long request: a = VJ_* (suspend, resume, snapshot), b = VJS_* (running, done,
 *                   failed), c = percent (0..100, -1 unknown), text = detail
 *     VM_NOTICE     something the person should know that isn't a state: a = VMN_* (VMN_NET_LOST: passt ended while
 *                   the machine runs, a new one is being attached, or, after 5 in a minute, the machine has no network
 *                   until it starts again; VMN_NET_BACK: a new passt is attached, b = milliseconds without network;
 *                   VMN_RECOVERED: an interrupted operation was finished or rolled back at this start; VMN_IDENTITY: a
 *                   Clone's guest got its own hostname and machine-id; b = 1: a Windows guest's computer name, from its next
 *                   restart), b = detail number (a signal or exit status),
 *                   text = detail.
 *                   Sent to every client, and again to one that connects later (VM_STATUS)
 *
 * ---- lord-vmdisplay ----
 * Started by lord-server as `lord-vmdisplay <machine id>` with fd 3 = this channel and fd 4 = the D-Bus
 * descriptor from VM_DISPLAY_FD. It speaks D-Bus to QEMU only through fd 4, then applies the helper sandbox
 * (no files, no new sockets, no exec) before it reads anything QEMU sends about the picture.
 *
 *   The picture: VD_SIZE (helper → server) a = width, b = height. lord-server makes a memfd of width × height × 4
 *   bytes sealed against shrinking and growing (vm_picture_memfd), maps it read-only and sends it back with
 *   VD_SHM. The helper writes 32-bit pixels, B, G, R, X bytes in memory (x8r8g8b8 little endian; alpha is not
 *   meaningful, treat it as opaque), stride width × 4. It never hands lord-server QEMU's own memory: it copies
 *   the damaged area from QEMU's buffer into this one, at most VD_RATE times a second.
 *   VD_FRAME a, b, c, d = damage x, y, w, h (inside the picture), e = frame number. lord-server reads the picture
 *   (uploads it) and answers VD_FRAME_DONE e = that frame number; the helper doesn't write the picture again
 *   until then (no tearing), or until 250 ms passed with no answer.
 *   VD_CURSOR a, b = width, height (1..256), c, d = hotspot; one descriptor: a memfd sealed against writing,
 *   shrinking and growing (check with vm_cursor_check), width × height × 4 bytes of B, G, R, A (straight alpha).
 *   VD_POINTER a, b = position in picture coordinates, c = 1 visible / 0 hidden (only when QEMU reports one).
 *   VD_STATE a = VDS_*, b = VDR_*: connected, picture lost (Disable), or the QEMU side went away.
 *   VD_STATS a = messages refused since the last VD_STATS, b = updates received, c = copies made, d = width,
 *   e = height: every 5 s while something changed, for Logs and tests.
 *   VD_DMABUF (3D, a machine with graphics "3d"): the picture is a dmabuf QEMU draws with the GPU. One descriptor;
 *   a, b = width, height (also sent as VD_SIZE: the shared picture is still made), c = stride, with VD_DMABUF_FLIP set
 *   when its first row in memory is the picture's bottom, d = DRM fourcc (one of VD_FOURCC_*, one plane), e, f = the
 *   modifier's low and high 32 bits. The helper copies nothing then: on each VD_FRAME lord-server imports the dmabuf
 *   with its GPU context (EGL) and reads the damaged rows back into the shared picture, and answers VD_FRAME_DONE as
 *   before; it never waits on the dmabuf's fence (it polls it, and tries again later). Without a GPU path lord-server
 *   can't read it: the picture stays black (lord-vm starts 3D machines with 2D graphics there, --render-node none).
 *   a = 0 (no descriptor): the picture isn't a dmabuf any more.
 *
 *   Input (server → helper): VD_KEY a = Linux evdev key code (KEY_*), b = 1 down / 0 up.
 *   VD_MOTION a, b = absolute position in picture coordinates (clamped). VD_BUTTON a = VDB_*, b = 1 / 0.
 *   VD_WHEEL a = vertical steps (+ up, − down), b = horizontal steps (+ right). VD_RELEASE_ALL lets go of every
 *   key and button held (window lost focus). VD_RESIZE a, b = the window's size in pixels: the guest is asked
 *   to use it (Console.SetUIInfo), at most every 100 ms. VD_RATE a = the display's refresh rate (1..240; 60 by
 *   default). VD_CLOSE: the helper exits.
 *
 *   Sound (6.9, 6.20): QEMU's audio goes to the helper as org.qemu.Display1.AudioOutListener calls on a second peer
 *   connection. VD_AUDIO_REC a = 1 with one descriptor: a recording started; the descriptor is a ring of aring.h
 *   (sealed memfd, 48 kHz, 16-bit, one channel) that lord-media reads; the helper writes the guest's sound into it,
 *   mixed to one channel, and silence for the time the guest plays nothing, so the sound keeps time with the picture.
 *   a = 0: the recording ended.
 *   VD_AUDIO_OUT a = 1 with one descriptor: the speakers' ring (aring.h: sealed memfd, 48 kHz, 16-bit, two channels)
 *   that lord-server plays from its own PipeWire stream (media/maudio.c); the helper copies the guest's sound into it as
 *   it comes, and drops what would put it more than VD_AUDIO_AHEAD_MS ahead of what lord-server read, so a slow reader
 *   never adds delay and the helper never waits. a = 0: the sound is off (muted, or the machine's "sound" is off).
 *   VD_MIC a = 1 with one descriptor: the microphone's ring (aring.h, 48 kHz, 16-bit, one channel) that lord-server
 *   fills from the person's microphone, sent only while the machine's "microphone" is on, LordOS's microphone
 *   permission allows Machines and the guest is recording. a = 0: no microphone. Whenever there is none, the guest
 *   reads silence at the real rate, so a guest that records never stalls and never hears this computer.
 *   VD_GUEST_AUDIO (helper → server) a = VDG_OUT or VDG_IN, b = 1 the guest started that stream / 0 stopped it: the
 *   microphone opens only while the guest records, and the speakers' stream only plays while the guest plays.
 */
#ifndef LORD_VM_PROTO_H
#define LORD_VM_PROTO_H

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#define VM_PROTO_VERSION 1

/* ---- lord-server ↔ lord-vm ---- */

enum {
    VM_STATUS = 1, VM_PAUSE, VM_RESUME, VM_SHUTDOWN, VM_POWEROFF, VM_RESET, VM_DISPLAY, VM_QMP,   /* requests */
    VM_CDROM, VM_SUSPEND, VM_KEEP, VM_SNAPSHOT, VM_CONSOLE, VM_AGENT, VM_CLIP, VM_SOUND, VM_FILE, VM_DISPLAY_SIZE,
    VM_APPS, VM_WINACT, VM_LAUNCH, VM_CLIP_OFFER, VM_CLIP_DENY,
    VM_REQ_LAST = VM_CLIP_DENY,
    VM_REPLY = 64, VM_STATE, VM_INFO, VM_DISPLAY_FD, VM_NOTICE, VM_JOB, VM_CONSOLE_FD,           /* from lord-vm */
    VM_AGENT_STATE, VM_CLIP_DATA, VM_FILE_PROGRESS, VM_WINDATA, VM_CLIP_REQ,
};
/* VM_CLIP_DENY c: why. BACKGROUND: the machine wasn't in use (its window not in front, nor a few seconds after);
 * STALE: the host's clipboard changed since that offer, or the offer isn't the last; NONE: that form isn't there now
 * (or it's a secret); DECLINED: the person said no (ask mode), or didn't answer in time */
enum { VCD_BACKGROUND = 1, VCD_STALE, VCD_NONE, VCD_DECLINED, VCD_LAST = VCD_DECLINED };
/* VM_NOTICE a */
enum { VMN_NONE = 0, VMN_NET_LOST, VMN_RECOVERED, VMN_NET_BACK, VMN_IDENTITY, VMN_COUNT };
/* VM_JOB a, b */
enum { VJ_NONE = 0, VJ_SUSPEND, VJ_RESUME, VJ_SNAPSHOT, VJ_REVERT, VJ_DELETE, VJ_COUNT };
enum { VJS_RUNNING = 0, VJS_DONE, VJS_FAILED };
/* VM_AGENT a, VM_AGENT_STATE a and b */
enum { VAG_SHUTDOWN = 1, VAG_TIMESYNC, VAG_INSTALL, VAG_TRANSFER };
enum { VAS_OFF = 0, VAS_WAITING, VAS_CONNECTED };
/* 256: LAG_CAP_WINDOWS (vmagent.h), the guest's helper does apps mode.
 * VAC_TRANSFER: the guest has the transfer folder (6.7.1) mounted (Linux: /mnt/LordOS; Windows: VirtioFsSvc runs);
 * VAC_XFER_SETUP: a Windows guest that can have it once WinFsp is installed (VAG_TRANSFER);
 * VAC_CLIP_NOTOOL: the guest's helper is there but its clipboard can't work: the guest has no clipboard tool (the hello
 * said "noclip": a Linux guest without xclip or wl-clipboard). VAC_CLIPBOARD is set only when the hello says "clipboard";
 * VAC_CLIP_LAZY: it also said "clipreq": the host's clipboard goes in on demand (VM_CLIP_OFFER), never pushed;
 * VAC_OPEN: the guest's tools said "open": a dropped file can be opened there (VM_FILE a = 1, AGK_OPEN) */
enum { VAC_SHUTDOWN = 1, VAC_TIME = 2, VAC_EXEC = 4, VAC_FILES = 8, VAC_JOURNAL = 16, VAC_CLIPBOARD = 32, VAC_AWAKE = 64, VAC_DISPLAY = 128,
       VAC_TRANSFER = 512, VAC_XFER_SETUP = 1024, VAC_CLIP_NOTOOL = 2048, VAC_CLIP_LAZY = 4096, VAC_OPEN = 8192,
       VAC_ALL = 16383 };
/* how long a VM_FILE with a = 1 waits for the guest tools' AGK_OPENED after the copy (a program may ask for consent) */
#define AG_OPEN_WAIT_MS 120000
#define VM_WINDATA_MAX (1u << 20)
#define VM_CLIP_MAX (11u << 20)
/* VM_CLIP and VM_CLIP_DATA b: what the memfd holds */
enum { VCF_TEXT = 0, VCF_CLIPX = 1 };
/* VM_SNAPSHOT a */
enum { VSN_TAKE = 1, VSN_REVERT, VSN_DELETE };

/* machine state */
enum { VS_STOPPED = 0, VS_STARTING, VS_RUNNING, VS_PAUSED, VS_STOPPING, VS_CRASHED, VS_COUNT };
/* why (the last change) */
enum {
    VR_NONE = 0,
    VR_GUEST_SHUTDOWN,      /* the guest powered off */
    VR_HOST_SHUTDOWN,       /* VM_SHUTDOWN, the guest obeyed */
    VR_FORCED,              /* VM_SHUTDOWN timed out: forced off */
    VR_POWEROFF,            /* VM_POWEROFF */
    VR_QEMU_FAILED,         /* QEMU exited with an error (c = exit status); the log has its words */
    VR_QEMU_SIGNAL,         /* QEMU died from a signal (c = the signal) */
    VR_OUT_OF_MEMORY,       /* killed by the machine's memory limit */
    VR_START_FAILED,        /* never ran: text says why (missing disk, no KVM, ...) */
    VR_GUEST_PANIC,         /* the guest reported a panic */
    VR_SUPERVISOR_LOST,     /* the previous lord-vm died; found at the next start or status */
    VR_GUEST_RESET,         /* the guest or VM_RESET restarted it (state stays running) */
    VR_USER,                /* VM_PAUSE / VM_RESUME */
    VR_SUSPENDED,           /* VM_SUSPEND: the memory is in suspend.state, the next start resumes from it */
    VR_RESUME_FAILED,       /* the saved state couldn't be loaded (text says why); it's kept: try again or discard */
    VR_INSTALLED,           /* an unattended install's first start ended with the installer's restart (vmunattend.h) */
    VR_COUNT
};
/* VM_REPLY codes */
enum {
    VME_OK = 0, VME_BAD_REQUEST, VME_WRONG_STATE, VME_QMP, VME_NO_DISPLAY, VME_BUSY, VME_DENIED, VME_INTERNAL,
};
/* VM_INFO c: what the sandbox applied to QEMU */
enum {
    VMS_LANDLOCK_FS = 1, VMS_LANDLOCK_NET = 2, VMS_LANDLOCK_SCOPE = 4, VMS_SECCOMP_OURS = 8, VMS_SECCOMP_QEMU = 16,
    VMS_NO_NEW_PRIVS = 32, VMS_KVM_FD = 64, VMS_SUPERVISOR_LANDLOCK = 128, VMS_PDEATHSIG = 256,
    VMS_RENDER_NODE = 512,  /* 3D graphics: QEMU may open the GPU's render node (only a Linux machine with graphics "3d");
                             * such a machine can't be suspended or snapshotted with its memory (virgl can't be migrated) */
};
/* VM_INFO d: cgroups */
enum {
    VMC_NONE = 0,           /* no cgroup could be used: no limits */
    VMC_SCOPE = 1,          /* the machine's scope, with its memory and CPU limits (lord-sessiond or self) */
    VMC_NESTED = 2,         /* also leaves for lord-vm, QEMU and passt inside it, QEMU's with its own limits */
};

#define VM_TEXT_MAX 200
typedef struct {
    uint16_t type;
    uint16_t version;           /* VM_PROTO_VERSION */
    uint32_t seq;
    int32_t a, b, c, d;
    char text[VM_TEXT_MAX];
} VmMsg;

/* ---- lord-server ↔ lord-vmdisplay ---- */

enum {
    VD_READY = 1, VD_SIZE, VD_FRAME, VD_CURSOR, VD_POINTER, VD_STATE, VD_STATS, VD_DMABUF, VD_GUEST_AUDIO,   /* helper → server */
    VD_SHM = 64, VD_FRAME_DONE, VD_KEY, VD_MOTION, VD_BUTTON, VD_WHEEL, VD_RELEASE_ALL,       /* server → helper */
    VD_RESIZE, VD_RATE, VD_CLOSE, VD_AUDIO_REC, VD_AUDIO_OUT, VD_MIC,
};
/* VD_GUEST_AUDIO a: which of the guest's streams */
enum { VDG_OUT = 0, VDG_IN = 1 };
/* the most the helper keeps in the speakers' ring ahead of lord-server's reading (VD_AUDIO_OUT) */
#define VD_AUDIO_AHEAD_MS 80
enum { VDS_CONNECTED = 0, VDS_NO_PICTURE, VDS_LOST };
enum { VDR_NONE = 0, VDR_DISABLED, VDR_REFUSED, VDR_QEMU_GONE, VDR_SERVER_GONE, VDR_SETUP };
/* buttons, as QEMU's InputButton numbers */
enum { VDB_LEFT = 0, VDB_MIDDLE = 1, VDB_RIGHT = 2, VDB_SIDE = 5, VDB_EXTRA = 6 };

typedef struct {
    uint16_t type;
    uint16_t version;
    uint32_t seq;
    int32_t a, b, c, d, e, f;
} VdMsg;

/* limits both ends apply */
#define VD_MAX_DIM      8192u
#define VD_MAX_BYTES    (256u * 1024 * 1024)
#define VD_MAX_CURSOR   256
#define VD_POLL_MAX     256         /* messages lord-server takes per poll, as Remote Desk */
/* VD_DMABUF */
#define VD_DMABUF_FLIP        0x40000000    /* in c: the first row in memory is the picture's bottom */
#define VD_DMABUF_STRIDE_MAX  (VD_MAX_DIM * 4 * 2)
#define VD_FOURCC_XRGB8888    0x34325258u   /* 'XR24' */
#define VD_FOURCC_ARGB8888    0x34325241u   /* 'AR24' */
#define VD_FOURCC_XBGR8888    0x34324258u   /* 'XB24' */
#define VD_FOURCC_ABGR8888    0x34324241u   /* 'AB24' */

/* ---- helpers shared by both ends ---- */

/* sends one fixed message, with an optional descriptor (fd < 0: none), never blocking */
static inline int vm_send_raw(int sock, const void *msg, size_t len, int fd) {
    struct iovec io = { (void *)msg, len };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } cm;
    struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1 };
    if (fd >= 0) {
        memset(&cm, 0, sizeof cm);
        mh.msg_control = cm.b;
        mh.msg_controllen = sizeof cm.b;
        struct cmsghdr *ch = CMSG_FIRSTHDR(&mh);
        ch->cmsg_level = SOL_SOCKET;
        ch->cmsg_type = SCM_RIGHTS;
        ch->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(ch), &fd, sizeof fd);
    }
    return sendmsg(sock, &mh, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)len ? 0 : -1;
}

/* Receives one fixed message without waiting. 1 got one (*fd: a descriptor that came with it, or -1), 0 nothing
 * now, -1 the peer hung up or broke the protocol (wrong length, truncated, more than one descriptor, wrong
 * version). Descriptors on a refused message are closed. */
static inline int vm_recv_raw(int sock, void *msg, size_t len, int *fd) {
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int) * 4)]; } cm;
    struct iovec io = { msg, len };
    struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cm.b, .msg_controllen = sizeof cm.b };
    if (fd) *fd = -1;
    ssize_t n = recvmsg(sock, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    int got[4], ngot = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        size_t k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < k; i++) {
            int f;
            memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof f);
            if (ngot < 4) got[ngot++] = f; else close(f);
        }
    }
    int bad = n != (ssize_t)len || (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || ngot > 1 ||
              (len >= 4 && ((const uint16_t *)msg)[1] != VM_PROTO_VERSION);
    if (bad || !fd) { for (int i = 0; i < ngot; i++) close(got[i]); return bad || n == 0 ? -1 : 1; }
    if (ngot) *fd = got[0];
    return 1;
}

static inline int vm_send(int sock, const VmMsg *m, int fd) { return vm_send_raw(sock, m, sizeof *m, fd); }
static inline int vm_recv(int sock, VmMsg *m, int *fd) {
    int r = vm_recv_raw(sock, m, sizeof *m, fd);
    if (r > 0) m->text[VM_TEXT_MAX - 1] = 0;
    return r;
}
static inline int vd_send(int sock, const VdMsg *m, int fd) { return vm_send_raw(sock, m, sizeof *m, fd); }
static inline int vd_recv(int sock, VdMsg *m, int *fd) { return vm_recv_raw(sock, m, sizeof *m, fd); }

static inline VmMsg vm_msg(uint16_t type, uint32_t seq, int a, int b, int c, int d, const char *text) {
    VmMsg m;
    memset(&m, 0, sizeof m);
    m.type = type; m.version = VM_PROTO_VERSION; m.seq = seq; m.a = a; m.b = b; m.c = c; m.d = d;
    if (text) { size_t n = strlen(text); if (n > VM_TEXT_MAX - 1) n = VM_TEXT_MAX - 1; memcpy(m.text, text, n); }
    return m;
}
static inline VdMsg vd_msg(uint16_t type, int a, int b, int c, int d, int e, int f) {
    VdMsg m;
    memset(&m, 0, sizeof m);
    m.type = type; m.version = VM_PROTO_VERSION; m.a = a; m.b = b; m.c = c; m.d = d; m.e = e; m.f = f;
    return m;
}

/* Is this message one the receiving side may act on? `to_vm` 1: lord-vm checks a request; 0: lord-server
 * checks an event. Ranges only; the meaning is checked where it's used. */
static inline int vm_msg_valid(const VmMsg *m, int to_vm) {
    if (m->version != VM_PROTO_VERSION) return 0;
    if (to_vm) return m->type >= VM_STATUS && m->type <= VM_REQ_LAST && (m->type != VM_SHUTDOWN || (m->a >= 0 && m->a <= 600)) &&
                      (m->type != VM_SNAPSHOT || (m->a >= VSN_TAKE && m->a <= VSN_DELETE)) &&
                      (m->type != VM_AGENT || (m->a >= VAG_SHUTDOWN && m->a <= VAG_TRANSFER)) &&
                      (m->type != VM_CLIP || (m->a > 0 && (uint32_t)m->a <= VM_CLIP_MAX && (m->b == VCF_TEXT || m->b == VCF_CLIPX) &&
                                              m->c >= 0 && (!m->c || (m->b == VCF_CLIPX && m->d >= 1 && m->d <= 3)))) &&
                      (m->type != VM_CLIP_OFFER || (m->a > 0 && m->b >= 1 && m->b <= 7 && (m->c == 0 || m->c == 1))) &&
                      (m->type != VM_CLIP_DENY || (m->a >= 0 && m->c >= 1 && m->c <= VCD_LAST && (m->a ? m->b >= 1 && m->b <= 3 : m->b == 0))) &&
                      (m->type != VM_SOUND || ((m->a == 0 || m->a == 1) && m->b >= 0 && m->b <= 100 && (m->c == 0 || m->c == 1) && (m->d == 0 || m->d == 1))) &&
                      (m->type != VM_FILE || (m->text[0] && (m->a == 0 || m->a == 1))) &&
                      (m->type != VM_DISPLAY_SIZE || (m->a >= 320 && m->b >= 200 && (uint32_t)m->a <= VD_MAX_DIM && (uint32_t)m->b <= VD_MAX_DIM)) &&
                      (m->type != VM_APPS || m->a == 0 || m->a == 1) &&
                      (m->type != VM_WINACT || (m->a != 0 && m->b >= 1 && m->b <= 5)) &&
                      (m->type != VM_LAUNCH || (m->a >= 0 && m->a < 256 && m->b > 0));
    switch (m->type) {
    case VM_REPLY: return m->a >= VME_OK && m->a <= VME_INTERNAL;
    case VM_STATE: return m->a >= 0 && m->a < VS_COUNT && m->b >= 0 && m->b < VR_COUNT;
    case VM_INFO: return m->a > 0 && m->b > 0;
    case VM_DISPLAY_FD: return m->a >= 0 && m->b >= 0 && (uint32_t)m->a <= VD_MAX_DIM && (uint32_t)m->b <= VD_MAX_DIM;
    case VM_NOTICE: return m->a > VMN_NONE && m->a < VMN_COUNT;
    case VM_CONSOLE_FD: return 1;
    case VM_AGENT_STATE: return m->a >= VAS_OFF && m->a <= VAS_CONNECTED && m->b >= 0 && m->b <= VAC_ALL;
    /* b: LAG_WINDOWS (6), LAG_WINICON (7) or LAG_APPLIST (10), vmagent.h; c: LAG_APPLIST's generation (VM_LAUNCH) */
    case VM_WINDATA: return m->a > 0 && (uint32_t)m->a <= VM_WINDATA_MAX && (m->b == 6 || m->b == 7 || m->b == 10) && m->c >= 0;
    case VM_CLIP_DATA: return m->a > 0 && (uint32_t)m->a <= VM_CLIP_MAX && (m->b == VCF_TEXT || m->b == VCF_CLIPX);
    case VM_FILE_PROGRESS: return m->a >= 0 && m->a <= 100;
    case VM_CLIP_REQ: return m->a > 0 && m->b >= 1 && m->b <= 3;
    case VM_JOB: return m->a > VJ_NONE && m->a < VJ_COUNT && m->b >= VJS_RUNNING && m->b <= VJS_FAILED && m->c >= -1 && m->c <= 100;
    default: return 0;
    }
}

/* the same for the display channel: `to_helper` 1: lord-vmdisplay checks what lord-server sent. Picture
 * coordinates are checked against the current size where they're used. */
static inline int vd_msg_valid(const VdMsg *m, int to_helper) {
    if (m->version != VM_PROTO_VERSION) return 0;
    if (to_helper) switch (m->type) {
    case VD_SHM: case VD_FRAME_DONE: case VD_RELEASE_ALL: case VD_CLOSE: return 1;
    case VD_KEY: return m->a > 0 && m->a < 768 && (m->b == 0 || m->b == 1);
    case VD_MOTION: return m->a >= 0 && m->b >= 0 && (uint32_t)m->a < VD_MAX_DIM && (uint32_t)m->b < VD_MAX_DIM;
    case VD_BUTTON: return (m->a == VDB_LEFT || m->a == VDB_MIDDLE || m->a == VDB_RIGHT || m->a == VDB_SIDE || m->a == VDB_EXTRA) && (m->b == 0 || m->b == 1);
    case VD_WHEEL: return m->a >= -32 && m->a <= 32 && m->b >= -32 && m->b <= 32;
    case VD_RESIZE: return m->a >= 320 && m->b >= 200 && (uint32_t)m->a <= VD_MAX_DIM && (uint32_t)m->b <= VD_MAX_DIM;
    case VD_RATE: return m->a >= 1 && m->a <= 240;
    case VD_AUDIO_REC: case VD_AUDIO_OUT: case VD_MIC: return m->a == 0 || m->a == 1;
    default: return 0;
    }
    switch (m->type) {
    case VD_READY: return 1;
    case VD_SIZE: return m->a > 0 && m->b > 0 && (uint32_t)m->a <= VD_MAX_DIM && (uint32_t)m->b <= VD_MAX_DIM &&
                         (uint64_t)m->a * (uint64_t)m->b * 4 <= VD_MAX_BYTES;
    case VD_FRAME: return m->a >= 0 && m->b >= 0 && m->c > 0 && m->d > 0;
    case VD_CURSOR: return m->a > 0 && m->b > 0 && m->a <= VD_MAX_CURSOR && m->b <= VD_MAX_CURSOR &&
                           m->c >= 0 && m->d >= 0 && m->c < m->a && m->d < m->b;
    case VD_POINTER: return m->c == 0 || m->c == 1;
    case VD_STATE: return m->a >= VDS_CONNECTED && m->a <= VDS_LOST && m->b >= VDR_NONE && m->b <= VDR_SETUP;
    case VD_STATS: return m->a >= 0 && m->b >= 0 && m->c >= 0;
    case VD_DMABUF: {
        if (!m->a) return !m->b;
        uint32_t f = (uint32_t)m->d;
        int32_t st = m->c & ~VD_DMABUF_FLIP;
        return m->a > 0 && m->b > 0 && (uint32_t)m->a <= VD_MAX_DIM && (uint32_t)m->b <= VD_MAX_DIM &&
               (uint64_t)m->a * (uint64_t)m->b * 4 <= VD_MAX_BYTES && st >= m->a * 4 && st % 4 == 0 && (uint32_t)st <= VD_DMABUF_STRIDE_MAX &&
               (f == VD_FOURCC_XRGB8888 || f == VD_FOURCC_ARGB8888 || f == VD_FOURCC_XBGR8888 || f == VD_FOURCC_ABGR8888);
    }
    case VD_GUEST_AUDIO: return (m->a == VDG_OUT || m->a == VDG_IN) && (m->b == 0 || m->b == 1);
    default: return 0;
    }
}

/* a damage rectangle inside a w × h picture (lord-server checks every VD_FRAME with it) */
static inline int vd_rect_inside(const VdMsg *m, int w, int h) {
    return m->a >= 0 && m->b >= 0 && m->c > 0 && m->d > 0 && (int64_t)m->a + m->c <= w && (int64_t)m->b + m->d <= h;
}

/* lord-vmdisplay, the speakers' ring (VD_AUDIO_OUT, 48 kHz stereo s16): may n more bytes go in without putting it more
 * than VD_AUDIO_AHEAD_MS ahead of lord-server's reading? A reader cursor past the writer's, or further back than the
 * ring holds (lord-server's side is not trusted), counts as full. 1 yes */
static inline int vd_audio_fits(uint64_t rpos, uint64_t wpos, uint32_t size, size_t n) {
    uint64_t used = (rpos > wpos || wpos - rpos > size) ? size : wpos - rpos;
    uint64_t cap = (uint64_t)48000 * 4 * VD_AUDIO_AHEAD_MS / 1000;
    if (cap > size) cap = size;
    return used + n <= cap;
}

/* lord-vmdisplay, the guest's microphone (AudioInListener.Read): how many frames a stream started at t0_ms owes at
 * now_ms, having been given *given: the real rate (48 per ms), at most `room` and 100 ms per answer. After a gap longer
 * than 100 ms (a paused machine) the old time is forgotten (*given moves up), so no burst follows. */
static inline size_t vd_mic_owed(long long t0_ms, long long now_ms, uint64_t *given, size_t room) {
    uint64_t due = now_ms > t0_ms ? (uint64_t)(now_ms - t0_ms) * 48 : 0;
    if (due > *given + 4800) *given = due - 4800;
    uint64_t owed = due > *given ? due - *given : 0;
    if (owed > room) owed = room;
    if (owed > 4800) owed = 4800;
    return (size_t)owed;
}

/* lord-server: the picture for VD_SIZE, sealed so the helper can't shrink it under lord-server's mapping */
static inline int vm_picture_memfd(size_t len) {
    int fd = (int)syscall(SYS_memfd_create, "lordos-vmdisplay", 3u /* MFD_CLOEXEC | MFD_ALLOW_SEALING */);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)len) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) { close(fd); return -1; }
    return fd;
}

/* lord-server: is the cursor memfd from VD_CURSOR safe to map? sealed against shrink, grow and write, and exactly
 * w × h × 4 bytes. 0 yes. */
static inline int vm_cursor_check(int fd, int w, int h) {
    int seals = fcntl(fd, F_GET_SEALS);
    struct stat st;
    int need = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    if (seals < 0 || (seals & need) != need || fstat(fd, &st) || !S_ISREG(st.st_mode)) return -1;
    return w > 0 && h > 0 && w <= VD_MAX_CURSOR && h <= VD_MAX_CURSOR && st.st_size == (off_t)w * h * 4 ? 0 : -1;
}

#endif
