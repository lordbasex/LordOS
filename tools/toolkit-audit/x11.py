#!/usr/bin/python3 -I
# x11.py: the plain X11 harness of "lordos-testbench --toolkits" (installed as /usr/lib/lordos/toolkit-audit/x11.py).
#
# A plain X client under Xwayland, written against the X11 core protocol itself (Python 3 standard library only:
# socket and struct). LordOS links neither libX11 nor libxcb (docs/ARCHITECTURE.md, "Xwayland"; the window manager in
# server/src/wl/xwm.c and tools/guest/x11clip.c speak the protocol by hand the same way), so this harness needs no
# python3-xlib and nothing to be installed.
#
# It opens a 600x400 top-level window the way a toolkit-less X app does: a top row with one button at the start and
# one at the end, an entry, a check box, a slider, a combo box, a menu button and a light/dark indicator. It then runs
# the self-checks every harness shares (map, size, scale, theme, backend, decoration, first_frame, menu, combo,
# tooltip, clipboard, dnd, text, fullscreen), prints ONE JSON line on stdout and exits by itself within 5 s:
#
#   {"toolkit":"x11","version":"<X server version>","backend":"x11","checks":{"<id>":{"ok":bool|null,"ms"?:n,"detail"?:s}}}
#
# Measurement uses no test hooks, only what any X client can see. Results carry only names, versions, numbers and check
# ids: no user names, paths, host names or window titles. Only the local Unix socket of $DISPLAY is used, never TCP.
import json
import os
import re
import select
import signal
import socket
import stat
import struct
import subprocess
import sys
import time

T0 = time.monotonic()
DEADLINE = T0 + 4.6          # every wait is clipped to this; SIGALRM at 5 s prints what is done and exits
CHECK_IDS = ("map", "size", "scale", "theme", "backend", "decoration", "first_frame", "menu", "combo", "tooltip",
             "clipboard", "dnd", "text", "fullscreen")
REQ_W, REQ_H = 600, 400

result = {"toolkit": "x11", "version": "", "backend": "x11", "checks": {}}
printed = False


def ms_since(t):
    return round((time.monotonic() - t) * 1000)


def check(cid, ok, ms=None, detail=None):
    c = {"ok": ok}
    if ms is not None:
        c["ms"] = ms
    if detail:
        c["detail"] = detail
    result["checks"][cid] = c


def skip(cid, why):
    check(cid, None, detail="skipped: " + why)


def emit():
    global printed
    if printed:
        return
    printed = True
    for cid in CHECK_IDS:
        result["checks"].setdefault(cid, {"ok": None, "detail": "skipped: not reached in time"})
    sys.stdout.write(json.dumps(result, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def on_alarm(_sig, _frm):
    emit()
    os._exit(0)


def left():
    return max(0.0, DEADLINE - time.monotonic())


# ---- X protocol constants ---------------------------------------------------------------------------------------
KeyPress, FocusIn, Expose, UnmapNotify, MapNotify, ConfigureNotify = 2, 9, 12, 18, 19, 22
PropertyNotify, SelectionClear, SelectionRequest, SelectionNotify, ClientMessage = 28, 29, 30, 31, 33
EM_KEYPRESS, EM_EXPOSURE, EM_STRUCTURE, EM_SUBSTRUCTURE_NOTIFY = 0x1, 0x8000, 0x20000, 0x80000
EM_SUBSTRUCTURE_REDIRECT, EM_FOCUS, EM_PROPERTY = 0x100000, 0x200000, 0x400000
CW_BACK_PIXEL, CW_BORDER_PIXEL, CW_OVERRIDE, CW_EVENT_MASK = 0x2, 0x8, 0x200, 0x800
XA_ATOM, XA_CARDINAL, XA_STRING, XA_WINDOW, XA_WM_NAME, XA_WM_CLASS = 4, 6, 31, 33, 39, 67
XA_WM_NORMAL_HINTS, XA_WM_SIZE_HINTS, XA_WM_TRANSIENT_FOR, XA_RESOURCE_MANAGER = 40, 41, 68, 23

ATOMS = ("WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_NAME", "UTF8_STRING", "_NET_WM_STATE",
         "_NET_WM_STATE_FULLSCREEN", "_NET_SUPPORTING_WM_CHECK", "_NET_FRAME_EXTENTS", "_NET_REQUEST_FRAME_EXTENTS",
         "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_NORMAL", "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
         "_NET_WM_WINDOW_TYPE_COMBO", "_NET_WM_WINDOW_TYPE_TOOLTIP", "CLIPBOARD", "TARGETS", "XdndAware",
         "XdndSelection", "XdndEnter", "XdndPosition", "XdndStatus", "XdndDrop", "XdndFinished", "XdndLeave",
         "XdndActionCopy", "text/plain;charset=utf-8", "_LORDOS_TA_TIME", "_LORDOS_TA_CLIP", "_LORDOS_TA_DND")


class XError(Exception):
    pass


def pad4(n):
    return (n + 3) & ~3


class X:
    """One connection to the X server: requests out, replies matched by sequence number, everything else queued."""

    def __init__(self):
        self.sock = None
        self.buf = b""
        self.seq = 0
        self.events = []
        self.errors = {}
        self.replies = {}
        self.next_id = 0

    # -- connection ----------------------------------------------------------------------------------------------
    def connect(self):
        disp = os.environ.get("DISPLAY", "")
        m = re.fullmatch(r"(?:unix)?:(\d{1,4})(?:\.(\d{1,2}))?", disp)
        if not m:
            raise XError("no local $DISPLAY" if not disp else "only a local display is tested")
        self.dnum = int(m.group(1))
        path = "/tmp/.X11-unix/X%d" % self.dnum
        # Only the filesystem socket, and only when it is a socket of this user's: LordOS's Xwayland serves no abstract
        # socket (server/src/wl/xwayland.c), and any user can bind an abstract name first and collect the cookie sent
        # below; a file in the sticky /tmp/.X11-unix that another user made is refused the same way.
        try:
            st = os.lstat(path)
        except OSError as e:
            raise XError("cannot connect to the display (%s)" % type(e).__name__)
        if not stat.S_ISSOCK(st.st_mode) or st.st_uid != os.getuid():
            raise XError("the display's socket is not this user's")
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(max(0.2, left()))
        try:
            s.connect(path)
        except OSError as e:
            s.close()
            raise XError("cannot connect to the display (%s)" % type(e).__name__)
        self.sock = s
        self.sock.setblocking(False)
        name, data = self.cookie()
        req = struct.pack("<BxHHHHxx", 0x6C, 11, 0, len(name), len(data))
        req += name + b"\0" * (pad4(len(name)) - len(name)) + data + b"\0" * (pad4(len(data)) - len(data))
        self.send_raw(req)
        head = self.read_exact(8)
        extra = self.read_exact(struct.unpack_from("<H", head, 6)[0] * 4)
        if head[0] != 1:
            raise XError("the X server refused the connection")
        self.parse_setup(extra)

    def cookie(self):
        """MIT-MAGIC-COOKIE-1 for this display from the Xauthority file (never reported anywhere)."""
        fn = os.environ.get("XAUTHORITY") or os.path.join(os.path.expanduser("~"), ".Xauthority")
        try:
            with open(fn, "rb") as f:
                raw = f.read(1 << 16)
        except OSError:
            return b"", b""
        host = socket.gethostname().encode()
        best = None
        off = 0

        def field(o):
            if o + 2 > len(raw):
                return None, o
            n = struct.unpack_from(">H", raw, o)[0]
            if o + 2 + n > len(raw):
                return None, o
            return raw[o + 2:o + 2 + n], o + 2 + n

        while off + 2 <= len(raw):
            fam = struct.unpack_from(">H", raw, off)[0]
            addr, off = field(off + 2)
            num, off = field(off)
            nm, off = field(off)
            dat, off = field(off)
            if dat is None:
                break
            if nm != b"MIT-MAGIC-COOKIE-1" or num not in (str(self.dnum).encode(), b""):
                continue
            if fam == 0xFFFF or (fam == 256 and addr == host):
                return nm, dat
            if fam == 256 and best is None:
                best = (nm, dat)
        return best or (b"", b"")

    def parse_setup(self, d):
        self.release, self.rid_base, self.rid_mask = struct.unpack_from("<III", d, 0)
        vlen, = struct.unpack_from("<H", d, 16)
        nscreens, nformats = d[20], d[21]
        self.min_kc, self.max_kc = d[26], d[27]
        self.vendor = d[32:32 + vlen].decode("latin-1", "replace")
        o = 32 + pad4(vlen) + 8 * nformats
        if nscreens < 1:
            raise XError("the X server has no screen")
        (self.root, _cmap, self.white, self.black, _masks, self.sw, self.sh, _mmw, _mmh, _mn, _mx,
         self.root_visual, _bs, _su, self.root_depth, ndepths) = struct.unpack_from("<IIIIIHHHHHHIBBBB", d, o)
        o += 40
        self.vclass, self.masks = None, None
        for _ in range(ndepths):
            _depth, nvis = d[o], struct.unpack_from("<H", d, o + 2)[0]
            o += 8
            for _ in range(nvis):
                vid, vclass, _bits, _ents, rm, gm, bm = struct.unpack_from("<IBBHIII", d, o)
                if vid == self.root_visual:
                    self.vclass, self.masks = vclass, (rm, gm, bm)
                o += 24

    def pixel(self, rgb):
        if self.vclass in (4, 5) and self.masks:      # TrueColor, DirectColor
            px = 0
            for i, m in enumerate(self.masks):
                if not m:
                    continue
                shift = (m & -m).bit_length() - 1
                bits = (m >> shift).bit_length()
                c = (rgb >> (16 - 8 * i)) & 0xFF
                px |= ((c * ((1 << bits) - 1) // 255) << shift) & m
            return px
        return self.white if ((rgb >> 16) + ((rgb >> 8) & 0xFF) + (rgb & 0xFF)) > 384 else self.black

    def new_id(self):
        self.next_id += 1
        return self.rid_base | (self.next_id & self.rid_mask)

    # -- wire ------------------------------------------------------------------------------------------------------
    def send_raw(self, b):
        while b:
            if not select.select([], [self.sock], [], max(0.05, left()))[1]:
                raise XError("the X server stopped reading")
            try:
                n = self.sock.send(b)
            except BlockingIOError:
                continue
            b = b[n:]

    def req(self, op, data1, body):
        """Send one request; returns its sequence number (16 bits)."""
        body += b"\0" * (pad4(len(body)) - len(body))
        self.send_raw(struct.pack("<BBH", op, data1, 1 + len(body) // 4) + body)
        self.seq = (self.seq + 1) & 0xFFFF
        return self.seq

    def fill(self, timeout):
        r = select.select([self.sock], [], [], max(0.0, timeout))[0]
        if not r:
            return False
        try:
            chunk = self.sock.recv(65536)
        except BlockingIOError:
            return True
        if not chunk:
            raise XError("the X server closed the connection")
        self.buf += chunk
        return True

    def read_exact(self, n):
        while len(self.buf) < n:
            if not self.fill(max(0.05, left())):
                raise XError("the X server did not answer")
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def pump(self, timeout):
        """Read what arrives within timeout and sort it into replies, errors and events."""
        end = time.monotonic() + timeout
        got = False
        while True:
            while len(self.buf) >= 32:
                kind = self.buf[0]
                size = 32 + (struct.unpack_from("<I", self.buf, 4)[0] * 4 if kind in (1, 35) else 0)
                if len(self.buf) < size:
                    break
                msg, self.buf = self.buf[:size], self.buf[size:]
                seq = struct.unpack_from("<H", msg, 2)[0]
                if kind == 1:
                    self.replies[seq] = msg
                elif kind == 0:
                    self.errors[seq] = msg[1]
                else:
                    self.events.append(msg)
                got = True
            t = end - time.monotonic()
            if got or t <= 0 or not self.fill(min(t, left())):
                return got

    def reply(self, seq, timeout=1.0):
        end = min(time.monotonic() + timeout, DEADLINE)
        while seq not in self.replies and seq not in self.errors:
            t = end - time.monotonic()
            if t <= 0:
                return None
            self.pump(t)
        return self.replies.pop(seq, None)

    def sync(self):
        return self.reply(self.req(43, 0, b"")) is not None     # GetInputFocus as a round trip

    def take(self, pred):
        for i, e in enumerate(self.events):
            if pred(e):
                return self.events.pop(i)
        return None

    def wait(self, pred, timeout):
        end = min(time.monotonic() + timeout, DEADLINE)
        while True:
            e = self.take(pred)
            if e is not None:
                return e
            t = end - time.monotonic()
            if t <= 0:
                return None
            self.pump(t)

    # -- requests --------------------------------------------------------------------------------------------------
    def intern(self, names):
        seqs = [self.req(16, 0, struct.pack("<HH", len(n), 0) + n.encode()) for n in names]
        out = {}
        for n, s in zip(names, seqs):
            r = self.reply(s)
            out[n] = struct.unpack_from("<I", r, 8)[0] if r else 0
        return out

    def create_window(self, parent, x, y, w, h, back, emask, override=False, input_only=False):
        wid = self.new_id()
        mask, vals = CW_EVENT_MASK, [emask]
        if not input_only:
            mask, vals = CW_BACK_PIXEL | CW_BORDER_PIXEL | CW_EVENT_MASK, [back, self.black, emask]
        if override:
            mask |= CW_OVERRIDE
            vals = vals[:-1] + [1, emask]
        body = struct.pack("<IIhhHHHHII", wid, parent, x, y, w, h, 0, 2 if input_only else 1, 0, mask)
        body += b"".join(struct.pack("<I", v) for v in vals)
        self.req(1, 0 if input_only else self.root_depth, body)
        return wid

    def set_back(self, wid, back):
        self.req(2, 0, struct.pack("<III", wid, CW_BACK_PIXEL, back))
        self.req(61, 0, struct.pack("<IhhHH", wid, 0, 0, 0, 0))          # ClearArea: repaint with the new back

    def change_prop(self, wid, prop, ptype, fmt, data, mode=0):
        if fmt == 8:
            raw, n = data, len(data)
        else:
            raw = b"".join(struct.pack("<I", v) for v in data)
            n = len(data)
        self.req(18, mode, struct.pack("<IIIBxxxI", wid, prop, ptype, fmt, n) + raw)

    def get_prop(self, wid, prop, delete=False, length=4096):
        r = self.reply(self.req(20, 1 if delete else 0, struct.pack("<IIIII", wid, prop, 0, 0, length)))
        if not r or r[1] == 0:
            return None, None, b""
        fmt, ptype, n = r[1], struct.unpack_from("<I", r, 8)[0], struct.unpack_from("<I", r, 16)[0]
        return fmt, ptype, r[32:32 + n * (fmt // 8)]

    def cards(self, raw):
        return list(struct.unpack("<%dI" % (len(raw) // 4), raw[:len(raw) // 4 * 4]))

    def map(self, wid):
        self.req(8, 0, struct.pack("<I", wid))

    def unmap(self, wid):
        self.req(10, 0, struct.pack("<I", wid))

    def destroy(self, wid):
        self.req(4, 0, struct.pack("<I", wid))

    def move(self, wid, x, y):
        self.req(12, 0, struct.pack("<IHxxii", wid, 0x3, x, y))

    def geometry(self, wid):
        r = self.reply(self.req(14, 0, struct.pack("<I", wid)))
        return struct.unpack_from("<hhHH", r, 12) if r else None

    def to_root(self, wid, x, y):
        r = self.reply(self.req(40, 0, struct.pack("<IIhh", wid, self.root, x, y)))
        return struct.unpack_from("<hh", r, 12) if r else (x, y)

    def query_tree_parent(self, wid):
        r = self.reply(self.req(15, 0, struct.pack("<I", wid)))
        return struct.unpack_from("<I", r, 12)[0] if r else None

    def has_extension(self, name):
        r = self.reply(self.req(98, 0, struct.pack("<HH", len(name), 0) + name.encode()))
        return bool(r and r[8])

    def send_event(self, dest, mask, ev, propagate=False):
        self.req(25, 1 if propagate else 0, struct.pack("<II", dest, mask) + ev)

    def client_message(self, dest, wid, mtype, data, mask=0):
        data = list(data) + [0] * (5 - len(data))
        ev = struct.pack("<BBHII5I", ClientMessage, 32, 0, wid, mtype, *data)
        self.send_event(dest, mask, ev)

    def set_owner(self, wid, sel, ts):
        self.req(22, 0, struct.pack("<III", wid, sel, ts))

    def get_owner(self, sel):
        r = self.reply(self.req(23, 0, struct.pack("<I", sel)))
        return struct.unpack_from("<I", r, 8)[0] if r else None

    def convert(self, requestor, sel, target, prop, ts):
        self.req(24, 0, struct.pack("<IIIII", requestor, sel, target, prop, ts))

    def gc(self, wid):
        g = self.new_id()
        self.req(55, 0, struct.pack("<IIII", g, wid, 0x4, self.black))
        return g

    def fill_rects(self, wid, g, rgb, rects):
        self.req(56, 0, struct.pack("<III", g, 0x4, self.pixel(rgb)))
        self.req(70, 0, struct.pack("<II", wid, g) + b"".join(struct.pack("<hhHH", *r) for r in rects))


def ev_code(e):
    return e[0] & 0x7F


def u32(e, o):
    return struct.unpack_from("<I", e, o)[0]


# ---- the color-scheme the desktop reports (the portal, read with the system's own D-Bus tools) ---------------------
def start_theme_read():
    args = None
    for tool, argv in (("/usr/bin/busctl", ["/usr/bin/busctl", "--user", "--timeout=2", "call",
                                            "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                            "org.freedesktop.portal.Settings", "ReadOne", "ss",
                                            "org.freedesktop.appearance", "color-scheme"]),
                       ("/usr/bin/gdbus", ["/usr/bin/gdbus", "call", "--session", "--timeout", "2",
                                           "--dest", "org.freedesktop.portal.Desktop",
                                           "--object-path", "/org/freedesktop/portal/desktop",
                                           "--method", "org.freedesktop.portal.Settings.ReadOne",
                                           "org.freedesktop.appearance", "color-scheme"])):
        if os.access(tool, os.X_OK):
            args = argv
            break
    if not args:
        return None
    try:
        return subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    except OSError:
        return None


def finish_theme_read(p):
    """(ok, scheme) where scheme is dark, light or no-preference."""
    if p is None:
        return None, "no busctl or gdbus to read the portal"
    try:
        out, _ = p.communicate(timeout=max(0.1, min(2.0, left() - 2.0)))
    except subprocess.TimeoutExpired:
        p.kill()
        return False, "the portal did not answer"
    m = re.search(rb"(\d+)\D*$", out.strip()) if p.returncode == 0 else None
    if not m:
        return False, "no portal Settings color-scheme"
    v = int(m.group(1))
    return True, {0: "no-preference", 1: "dark", 2: "light"}.get(v, "unknown")


# ---- the harness ------------------------------------------------------------------------------------------------
LIGHT = {"window": 0xF4F4F4, "widget": 0xFFFFFF, "accent": 0x3584E4, "line": 0x9A9A9A, "ind": 0xFFFFFF}
DARK = {"window": 0x242424, "widget": 0x363636, "accent": 0x78AEED, "line": 0x5E5E5E, "ind": 0x101010}


class Harness:
    def __init__(self, x):
        self.x = x
        self.colors = LIGHT
        self.typed = ""
        self.keymap = {}
        self.kcodes = {}
        self.server_time = 0

    def layout(self, w, h):
        # (name, x, y, w, h): the top row's buttons sit strictly at the start and at the end
        return [("btn_start", 8, 8, 96, 28), ("btn_end", w - 8 - 96, 8, 96, 28),
                ("entry", 8, 52, min(320, w - 16), 28), ("check", 8, 96, 18, 18),
                ("slider", 8, 130, min(320, w - 16), 18), ("combo", 8, 164, 200, 28),
                ("menu_btn", 8, 208, 96, 28), ("indicator", w - 8 - 28, h - 8 - 28, 28, 28)]

    def build(self):
        x, a = self.x, self.a
        self.top = x.create_window(x.root, 0, 0, REQ_W, REQ_H, x.pixel(self.colors["window"]),
                                   EM_EXPOSURE | EM_STRUCTURE | EM_PROPERTY | EM_FOCUS | EM_KEYPRESS)
        title = os.environ.get("LORDOS_TA_TITLE", "X11")[:64].encode("utf-8", "replace")
        x.change_prop(self.top, XA_WM_NAME, XA_STRING, 8, title)
        x.change_prop(self.top, a["_NET_WM_NAME"], a["UTF8_STRING"], 8, title)
        x.change_prop(self.top, XA_WM_CLASS, XA_STRING, 8, b"lordos-toolkit-audit\0LordOS-toolkit-audit\0")
        x.change_prop(self.top, a["WM_PROTOCOLS"], XA_ATOM, 32, [a["WM_DELETE_WINDOW"]])
        x.change_prop(self.top, a["_NET_WM_WINDOW_TYPE"], XA_ATOM, 32, [a["_NET_WM_WINDOW_TYPE_NORMAL"]])
        x.change_prop(self.top, XA_WM_NORMAL_HINTS, XA_WM_SIZE_HINTS, 32, [0x2 | 0x8, 0, 0, REQ_W, REQ_H] + [0] * 13)
        x.change_prop(self.top, a["XdndAware"], XA_ATOM, 32, [5])
        self.kids = {}
        for name, kx, ky, kw, kh in self.layout(REQ_W, REQ_H):
            emask = EM_EXPOSURE | (EM_KEYPRESS | EM_FOCUS if name == "entry" else 0)
            self.kids[name] = x.create_window(self.top, kx, ky, kw, kh, x.pixel(self.colors["widget"]), emask)
        self.gc = x.gc(self.top)
        for wid in self.kids.values():
            x.map(wid)
        if self.wm:
            x.client_message(x.root, self.top, a["_NET_REQUEST_FRAME_EXTENTS"], [],
                             EM_SUBSTRUCTURE_REDIRECT | EM_SUBSTRUCTURE_NOTIFY)

    def draw(self):
        x, c, k = self.x, self.colors, self.kids
        x.set_back(self.top, x.pixel(c["window"]))
        for wid in k.values():
            x.set_back(wid, x.pixel(c["widget"]))
        x.fill_rects(k["btn_start"], self.gc, c["line"], [(0, 27, 96, 1)])
        x.fill_rects(k["btn_end"], self.gc, c["line"], [(0, 27, 96, 1)])
        x.fill_rects(k["entry"], self.gc, c["accent"], [(0, 26, 320, 2)] +
                     [(6 + 10 * i, 8, 6, 12) for i in range(min(len(self.typed), 30))])
        x.fill_rects(k["check"], self.gc, c["accent"], [(3, 3, 12, 12)])
        x.fill_rects(k["slider"], self.gc, c["accent"], [(0, 8, 160, 3), (156, 1, 16, 16)])
        x.fill_rects(k["combo"], self.gc, c["line"], [(180, 12, 10, 4)])
        x.fill_rects(k["menu_btn"], self.gc, c["line"], [(8, 9, 16, 2), (8, 13, 16, 2), (8, 17, 16, 2)])
        x.fill_rects(k["indicator"], self.gc, c["ind"], [(0, 0, 28, 28)])

    def get_time(self):
        """A server timestamp: an empty append to our own property and its PropertyNotify."""
        x, a = self.x, self.a
        x.change_prop(self.top, a["_LORDOS_TA_TIME"], XA_STRING, 8, b"", mode=2)
        e = x.wait(lambda e: ev_code(e) == PropertyNotify and u32(e, 4) == self.top
                   and u32(e, 8) == a["_LORDOS_TA_TIME"], 0.5)
        if e:
            self.server_time = u32(e, 12)
        return self.server_time

    def serve_selection(self, e, payloads):
        """Answer a SelectionRequest for a selection we own: TARGETS or one of our text targets, refuse the rest."""
        x, a = self.x, self.a
        ts, owner, req, sel, target, prop = struct.unpack_from("<IIIIII", e, 4)
        data = payloads.get(sel)
        if prop == 0:
            prop = target
        if data is None:
            prop = 0
        elif target == a["TARGETS"]:
            x.change_prop(req, prop, XA_ATOM, 32, [a["TARGETS"], a["UTF8_STRING"], a["text/plain;charset=utf-8"]])
        elif target in (a["UTF8_STRING"], a["text/plain;charset=utf-8"], XA_STRING):
            x.change_prop(req, prop, target, 8, data)
        else:
            prop = 0
        x.send_event(req, 0, struct.pack("<BxHIIIIIxxxxxxxx", SelectionNotify, 0, ts, req, sel, target, prop))

    def popup(self, cid, wtype, rx, ry, w, h):
        """An override-redirect popup of a given _NET_WM_WINDOW_TYPE: ok when it maps and gets its first Expose."""
        x, a = self.x, self.a
        if left() < 0.3:
            return skip(cid, "out of time")
        t = time.monotonic()
        p = x.create_window(x.root, rx, ry, w, h, x.pixel(self.colors["widget"]), EM_EXPOSURE | EM_STRUCTURE,
                            override=True)
        x.change_prop(p, a["_NET_WM_WINDOW_TYPE"], XA_ATOM, 32, [a[wtype]])
        x.change_prop(p, XA_WM_TRANSIENT_FOR, XA_WINDOW, 32, [self.top])
        x.map(p)
        m = x.wait(lambda e: ev_code(e) == MapNotify and u32(e, 8) == p, 0.8)
        ex = m and x.wait(lambda e: ev_code(e) == Expose and u32(e, 4) == p, 0.5)
        if ex:
            x.fill_rects(p, self.gc, self.colors["accent"], [(0, 0, w, 4)])
            x.sync()
        geo = x.geometry(p) if ex else None
        x.unmap(p)
        x.destroy(p)
        if not m:
            return check(cid, False, detail="the popup did not map")
        if not ex:
            return check(cid, False, ms_since(t), "mapped but never exposed")
        sz = "%dx%d" % (geo[2], geo[3]) if geo else "?"
        check(cid, bool(geo and geo[2] == w and geo[3] == h), ms_since(t), "override-redirect " + sz)

    # -- checks ----------------------------------------------------------------------------------------------------
    def run(self):
        x = self.x
        theme_proc = start_theme_read()
        x.connect()
        self.a = a = x.intern(ATOMS)
        xwl = x.has_extension("XWAYLAND")
        rel = x.release
        result["version"] = "%d.%d.%d" % ((rel // 100000) % 100, (rel // 1000) % 100, rel % 1000)
        check("backend", xwl, detail="x11 (Xwayland)" if xwl else "x11 (not Xwayland)")
        fmt, _t, raw = x.get_prop(x.root, a["_NET_SUPPORTING_WM_CHECK"])
        self.wm = bool(fmt == 32 and raw and x.cards(raw)[0])
        if not self.wm:
            result["checks"]["backend"]["detail"] += ", no window manager"

        # keyboard map for the text check (keycode -> keysym, first column)
        n = x.max_kc - x.min_kc + 1
        r = x.reply(x.req(101, 0, struct.pack("<BBxx", x.min_kc, n)))
        if r:
            per = r[1]
            syms = x.cards(r[32:32 + 4 * per * n])
            for i in range(n):
                s = syms[i * per] if per else 0
                if s:
                    self.keymap[x.min_kc + i] = s
                    self.kcodes.setdefault(s, x.min_kc + i)

        # window, map, first frame
        self.build()
        x.map(self.top)
        m = x.wait(lambda e: ev_code(e) == MapNotify and u32(e, 8) == self.top, 2.0)
        if not m:
            check("map", False, detail="the window did not map")
            return
        check("map", True, ms_since(T0))
        ex = x.wait(lambda e: ev_code(e) == Expose and u32(e, 4) == self.top, 1.0)
        if ex:
            self.draw()
            x.sync()
            check("first_frame", True, ms_since(T0), "first Expose drawn")
        else:
            check("first_frame", False, detail="no Expose after map")

        # size
        geo = x.geometry(self.top)
        if geo:
            check("size", (geo[2], geo[3]) == (REQ_W, REQ_H), detail="%dx%d" % (geo[2], geo[3]))
        else:
            check("size", False, detail="no geometry")

        # scale: X has one scale for every client, Xft.dpi in RESOURCE_MANAGER (xwm writes it)
        _f, _t, rm = x.get_prop(x.root, XA_RESOURCE_MANAGER, length=16384)
        dm = re.search(rb"(?m)^Xft\.dpi:\s*([0-9.]+)", rm or b"")
        if dm:
            dpi = float(dm.group(1))
            check("scale", dpi > 0, detail="scale %g (Xft.dpi %g)" % (round(dpi / 96, 2), dpi))
        else:
            check("scale", True, detail="scale 1 (no Xft.dpi)")

        # decoration: the window manager's frame (server-side) shows as _NET_FRAME_EXTENTS or a reparent
        if not self.wm:
            skip("decoration", "no window manager")
        else:
            ext = None
            end = time.monotonic() + 0.4
            while time.monotonic() < end:
                f, _t, raw = x.get_prop(self.top, a["_NET_FRAME_EXTENTS"])
                if f == 32 and len(raw) >= 16:
                    ext = x.cards(raw)[:4]
                    break
                x.wait(lambda e: ev_code(e) == PropertyNotify and u32(e, 4) == self.top
                       and u32(e, 8) == a["_NET_FRAME_EXTENTS"], 0.1)
            parent = x.query_tree_parent(self.top)
            if ext is not None:
                check("decoration", True, detail="ssd, frame %d %d %d %d" % tuple(ext))
            elif parent and parent != x.root:
                check("decoration", True, detail="ssd, reparented")
            else:
                check("decoration", False, detail="no frame from the window manager")

        # theme
        ok, scheme = finish_theme_read(theme_proc)
        if ok is None:
            skip("theme", scheme)
        else:
            check("theme", ok, detail=("color-scheme %s (portal)" % scheme) if ok else scheme)
            if ok and scheme == "dark":
                self.colors = DARK
                self.draw()
                x.sync()

        # menu, combo popup and tooltip, placed under their widgets
        ox, oy = x.to_root(self.top, 0, 0)
        self.popup("menu", "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", ox + 8, oy + 236, 180, 120)
        self.popup("combo", "_NET_WM_WINDOW_TYPE_COMBO", ox + 8, oy + 192, 200, 96)
        self.popup("tooltip", "_NET_WM_WINDOW_TYPE_TOOLTIP", ox + 30, oy + 40, 140, 24)

        self.check_text()
        self.check_clipboard()
        self.check_dnd()
        self.check_fullscreen()
        x.destroy(self.top)
        x.sync()

    def check_text(self):
        """Typed text reaches the entry: synthetic KeyPress events (SendEvent) from this client to its own entry."""
        x = self.x
        word = "lordos"
        codes = [self.kcodes.get(ord(ch)) for ch in word]
        if None in codes:
            return skip("text", "the keymap has no plain latin letters")
        entry = self.kids["entry"]
        t = time.monotonic()
        x.req(42, 2, struct.pack("<II", entry, 0))        # SetInputFocus(entry, revert to parent, CurrentTime)
        for kc in codes:
            ev = struct.pack("<BBHIIIIhhhhHBx", KeyPress, kc, 0, 0, x.root, entry, 0, 0, 0, 10, 10, 0, 1)
            x.send_event(entry, 0, ev)
        end = time.monotonic() + 0.8
        while len(self.typed) < len(word) and time.monotonic() < end:
            e = x.wait(lambda e: ev_code(e) == KeyPress and u32(e, 12) == entry, end - time.monotonic())
            if not e:
                break
            s = self.keymap.get(e[1], 0)
            if 0x20 <= s < 0x7F:
                self.typed += chr(s)
        self.draw()
        x.sync()
        check("text", self.typed == word, ms_since(t), "%d of %d characters" % (len(self.typed), len(word)))

    def check_clipboard(self):
        """CLIPBOARD round trip within this client: own it, convert it to UTF8_STRING, answer ourselves, compare."""
        x, a = self.x, self.a
        if left() < 0.3:
            return skip("clipboard", "out of time")
        t = time.monotonic()
        token = ("lordos-ta-%08x" % (os.getpid() * 2654435761 & 0xFFFFFFFF)).encode()
        ts = self.get_time()
        x.set_owner(self.top, a["CLIPBOARD"], ts)
        if x.get_owner(a["CLIPBOARD"]) != self.top:
            return check("clipboard", False, ms_since(t), "could not own CLIPBOARD")
        x.convert(self.top, a["CLIPBOARD"], a["UTF8_STRING"], a["_LORDOS_TA_CLIP"], ts)
        got = None
        end = time.monotonic() + 1.0
        while time.monotonic() < end:
            e = x.wait(lambda e: ev_code(e) in (SelectionRequest, SelectionNotify), end - time.monotonic())
            if not e:
                break
            if ev_code(e) == SelectionRequest:
                self.serve_selection(e, {a["CLIPBOARD"]: token})
            elif u32(e, 8) == self.top and u32(e, 12) == a["CLIPBOARD"]:
                if u32(e, 20):
                    got = x.get_prop(self.top, a["_LORDOS_TA_CLIP"], delete=True)[2]
                break
        if got is None:
            return check("clipboard", False, ms_since(t), "no data came back")
        check("clipboard", got == token, ms_since(t), "utf-8 text" + ("" if got == token else ", different bytes"))

    def check_dnd(self):
        """XDND drag and drop within this client: a source window drops text on the top-level (version 5)."""
        x, a = self.x, self.a
        if left() < 0.3:
            return skip("dnd", "out of time")
        t = time.monotonic()
        token = b"lordos-ta-dnd"
        src = x.create_window(self.top, 0, 0, 1, 1, 0, EM_PROPERTY, input_only=True)
        ts = self.get_time()
        x.set_owner(src, a["XdndSelection"], ts)
        if x.get_owner(a["XdndSelection"]) != src:
            x.destroy(src)
            return check("dnd", False, ms_since(t), "could not own XdndSelection")
        tgt = self.top
        rx, ry = x.to_root(tgt, 300, 200)
        x.client_message(tgt, tgt, a["XdndEnter"], [src, 5 << 24, a["text/plain;charset=utf-8"], a["UTF8_STRING"], 0])
        x.client_message(tgt, tgt, a["XdndPosition"], [src, 0, (rx & 0xFFFF) << 16 | (ry & 0xFFFF), ts,
                                                       a["XdndActionCopy"]])
        stage, got, finished = "enter", None, False
        end = time.monotonic() + 1.2

        def mine(e):
            c = ev_code(e)
            if c == ClientMessage:
                return u32(e, 8) in (a["XdndEnter"], a["XdndPosition"], a["XdndStatus"], a["XdndDrop"],
                                     a["XdndFinished"])
            return c in (SelectionRequest, SelectionNotify)

        while not finished and time.monotonic() < end:
            e = x.wait(mine, end - time.monotonic())
            if not e:
                break
            c = ev_code(e)
            if c == SelectionRequest:
                self.serve_selection(e, {a["XdndSelection"]: token})
                continue
            if c == SelectionNotify:
                if u32(e, 12) == a["XdndSelection"]:
                    if u32(e, 20):
                        got = x.get_prop(tgt, a["_LORDOS_TA_DND"], delete=True)[2]
                    x.client_message(src, src, a["XdndFinished"], [tgt, 1 if got == token else 0,
                                                                   a["XdndActionCopy"] if got == token else 0])
                continue
            mtype, d = u32(e, 8), struct.unpack_from("<5I", e, 12)
            if mtype == a["XdndEnter"] and d[0] == src:
                stage = "entered"
            elif mtype == a["XdndPosition"] and d[0] == src:      # the target accepts the copy
                x.client_message(src, src, a["XdndStatus"], [tgt, 1, 0, 0, a["XdndActionCopy"]])
            elif mtype == a["XdndStatus"] and d[0] == tgt:        # the source drops on an accepting target
                stage = "accepted" if d[1] & 1 else "refused"
                if d[1] & 1:
                    x.client_message(tgt, tgt, a["XdndDrop"], [src, 0, ts])
            elif mtype == a["XdndDrop"] and d[0] == src:          # the target asks for the data
                stage = "dropped"
                x.convert(tgt, a["XdndSelection"], a["UTF8_STRING"], a["_LORDOS_TA_DND"], d[2] or ts)
            elif mtype == a["XdndFinished"] and d[0] == tgt:
                finished = bool(d[1] & 1)
                stage = "finished"
        x.set_owner(0, a["XdndSelection"], ts)
        x.destroy(src)
        if finished and got == token:
            check("dnd", True, ms_since(t), "xdnd 5, text, copy")
        else:
            check("dnd", False, ms_since(t), "stopped at " + stage)

    def wm_state_has_fs(self):
        f, _t, raw = self.x.get_prop(self.top, self.a["_NET_WM_STATE"])
        return f == 32 and self.a["_NET_WM_STATE_FULLSCREEN"] in self.x.cards(raw)

    def check_fullscreen(self):
        """_NET_WM_STATE fullscreen on and off through the window manager, seen as the state and the size."""
        x, a = self.x, self.a
        if not self.wm:
            return skip("fullscreen", "no window manager")
        if left() < 0.4:
            return skip("fullscreen", "out of time")
        t = time.monotonic()

        def ask(add):
            x.client_message(x.root, self.top, a["_NET_WM_STATE"], [1 if add else 0, a["_NET_WM_STATE_FULLSCREEN"],
                                                                    0, 1, 0],
                             EM_SUBSTRUCTURE_REDIRECT | EM_SUBSTRUCTURE_NOTIFY)

        def settle(want_fs, timeout):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                x.wait(lambda e: ev_code(e) in (ConfigureNotify, PropertyNotify) and u32(e, 4) == self.top,
                       min(0.1, max(0.0, end - time.monotonic())))
                geo = x.geometry(self.top)
                fs = self.wm_state_has_fs()
                big = bool(geo and (geo[2], geo[3]) != (REQ_W, REQ_H))
                if fs == want_fs and big == want_fs:
                    return geo
            return None

        ask(True)
        on = settle(True, min(1.0, left() / 2))
        on_ms = ms_since(t)
        ask(False)
        off = settle(False, min(1.0, left()))
        if not on:
            return check("fullscreen", False, on_ms, "fullscreen was not applied")
        if not off:
            return check("fullscreen", False, ms_since(t), "on %dx%d, did not return to %dx%d" %
                         (on[2], on[3], REQ_W, REQ_H))
        check("fullscreen", True, ms_since(t), "on %dx%d, off %dx%d" % (on[2], on[3], off[2], off[3]))


def main():
    signal.signal(signal.SIGALRM, on_alarm)
    signal.setitimer(signal.ITIMER_REAL, 5.0)
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    x = X()
    try:
        Harness(x).run()
    except (XError, OSError, struct.error) as e:
        reason = str(e) if isinstance(e, XError) else type(e).__name__
        if "map" not in result["checks"]:
            check("map", False, detail=reason)
        for cid in CHECK_IDS:
            result["checks"].setdefault(cid, {"ok": None, "detail": "skipped: " + reason})
    signal.setitimer(signal.ITIMER_REAL, 0)
    emit()
    return 0


if __name__ == "__main__":
    sys.exit(main())
