#!/usr/bin/python3 -I
# x11_test.py: tests for x11.py, the plain X11 harness of "lordos-testbench --toolkits".
#
#   python3 tools/toolkit-audit/x11_test.py
#
# Runs the harness against a private Xvfb (never the session's display): once with no window manager, once with a tiny
# window manager written here (it answers MapRequest, _NET_REQUEST_FRAME_EXTENTS and _NET_WM_STATE fullscreen the way
# LordOS's xwm does), and once with no display at all. Each run must print exactly one JSON line with every shared
# check id, within 5 s. Prints "skipped: needs Xvfb" when Xvfb is not installed.
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = os.path.join(HERE, "x11.py")
spec = importlib.util.spec_from_file_location("x11harness", HARNESS)
xh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(xh)
xh.DEADLINE = time.monotonic() + 3600     # the window manager below lives longer than one harness run

fails = 0


def expect(cond, what):
    global fails
    if not cond:
        fails += 1
        print("FAIL:", what)


def run_harness(env):
    t = time.monotonic()
    p = subprocess.run([sys.executable, "-I", HARNESS], env=env, capture_output=True, timeout=8)
    took = time.monotonic() - t
    lines = p.stdout.decode().splitlines()
    expect(p.returncode == 0, "exit status %d" % p.returncode)
    expect(len(lines) == 1, "one JSON line, got %d" % len(lines))
    expect(took < 5.0, "finished in %.1f s" % took)
    expect(p.stderr == b"", "nothing on stderr: %r" % p.stderr[:200])
    r = json.loads(lines[0]) if lines else {}
    expect(set(r) == {"toolkit", "version", "backend", "checks"}, "top-level keys %s" % sorted(r))
    expect(r.get("toolkit") == "x11" and r.get("backend") == "x11", "toolkit and backend")
    checks = r.get("checks", {})
    expect(set(checks) == set(xh.CHECK_IDS), "check ids %s" % sorted(checks))
    for cid, c in checks.items():
        expect(c.get("ok") in (True, False, None), "%s ok is a bool or null" % cid)
        expect(set(c) <= {"ok", "ms", "detail"}, "%s keys" % cid)
        expect(c.get("ok") is not None or c.get("detail", "").startswith("skipped: "), "%s skip has a reason" % cid)
        expect(isinstance(c.get("ms", 0), int), "%s ms is an integer" % cid)
    text = lines[0] if lines else ""
    expect(os.path.expanduser("~") not in text and "/tmp" not in text, "no paths in the result")
    return checks


class FakeWM(threading.Thread):
    """Just enough of a window manager: maps, frames and fullscreens top-levels."""

    def __init__(self):
        super().__init__(daemon=True)
        self.x = xh.X()
        self.x.connect()
        self.a = self.x.intern(xh.ATOMS)
        x, a = self.x, self.a
        self.check = x.create_window(x.root, -10, -10, 1, 1, 0, 0, input_only=True)
        x.change_prop(x.root, a["_NET_SUPPORTING_WM_CHECK"], xh.XA_WINDOW, 32, [self.check])
        x.req(2, 0, struct.pack("<III", x.root, xh.CW_EVENT_MASK,
                                xh.EM_SUBSTRUCTURE_REDIRECT | xh.EM_SUBSTRUCTURE_NOTIFY))
        x.sync()
        self.stop = False

    def frame(self, wid):
        self.x.change_prop(wid, self.a["_NET_FRAME_EXTENTS"], xh.XA_CARDINAL, 32, [1, 1, 30, 1])

    def run(self):
        x, a = self.x, self.a
        while not self.stop:
            x.pump(0.05)
            while x.events:
                e = x.events.pop(0)
                code = e[0] & 0x7F
                if code == 20:                                   # MapRequest
                    wid = xh.u32(e, 8)
                    self.frame(wid)
                    x.map(wid)
                elif code == xh.ClientMessage:
                    wid, mtype = xh.u32(e, 4), xh.u32(e, 8)
                    d = struct.unpack_from("<5I", e, 12)
                    if mtype == a["_NET_REQUEST_FRAME_EXTENTS"]:
                        self.frame(wid)
                    elif mtype == a["_NET_WM_STATE"] and a["_NET_WM_STATE_FULLSCREEN"] in d[1:3]:
                        on = d[0] == 1
                        w, h = (x.sw, x.sh) if on else (xh.REQ_W, xh.REQ_H)
                        x.req(12, 0, struct.pack("<IHxxiiII", wid, 0xF, 0, 0, w, h))
                        x.change_prop(wid, a["_NET_WM_STATE"], xh.XA_ATOM, 32,
                                      [a["_NET_WM_STATE_FULLSCREEN"]] if on else [])
            x.sync()


def main():
    xvfb = shutil.which("Xvfb")
    if not xvfb:
        print("skipped: needs Xvfb")
        return 0
    disp = next(n for n in range(90, 200) if not os.path.exists("/tmp/.X11-unix/X%d" % n))
    srv = subprocess.Popen([xvfb, ":%d" % disp, "-nolisten", "tcp", "-screen", "0", "1280x800x24"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists("/tmp/.X11-unix/X%d" % disp):
                break
            time.sleep(0.05)
        time.sleep(0.2)
        env = {k: v for k, v in os.environ.items() if k not in ("WAYLAND_DISPLAY", "XAUTHORITY", "DISPLAY",
                                                               "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR")}
        env["DISPLAY"] = ":%d" % disp
        env["XAUTHORITY"] = "/nonexistent"

        c = run_harness(env)
        for cid in ("map", "size", "first_frame", "menu", "combo", "tooltip", "text", "clipboard", "dnd", "scale"):
            expect(c[cid]["ok"] is True, "no WM: %s ok (%s)" % (cid, c[cid]))
        expect(c["size"].get("detail") == "600x400", "no WM: 600x400")
        expect(c["decoration"]["ok"] is None and c["fullscreen"]["ok"] is None, "no WM: decoration/fullscreen skipped")
        expect(c["backend"]["ok"] is False, "Xvfb is not Xwayland")

        os.environ["DISPLAY"] = env["DISPLAY"]   # the window manager connects to the private Xvfb only
        os.environ["XAUTHORITY"] = env["XAUTHORITY"]
        wm = FakeWM()
        wm.start()
        c = run_harness(env)
        wm.stop = True
        wm.join(1)
        expect(c["decoration"]["ok"] is True, "WM: decoration (%s)" % c["decoration"])
        expect(c["fullscreen"]["ok"] is True, "WM: fullscreen (%s)" % c["fullscreen"])
        expect(c["fullscreen"].get("detail") == "on 1280x800, off 600x400", "WM: fullscreen sizes")
        expect(c["map"]["ok"] is True and c["size"]["ok"] is True, "WM: map and size")

        env.pop("DISPLAY")
        c = run_harness(env)
        expect(c["map"]["ok"] is False, "no display: map fails")
        expect(all(c[k]["ok"] is None for k in xh.CHECK_IDS if k != "map"), "no display: the rest skipped")

        env["DISPLAY"] = "somehost:0"
        c = run_harness(env)
        expect(c["map"]["ok"] is False and "local" in c["map"].get("detail", ""), "remote display refused")
    finally:
        srv.terminate()
        srv.wait(5)
    print("x11 harness: %s" % ("ok" if not fails else "%d failures" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
