#!/usr/bin/env python3
"""webcam_check.py — a web app's camera on the real engine and this computer's real cameras (docs/WEBAPPS.md, "Cameras").

    python3 -I tests/webcam_check.py build/webcalls-check build/camuse-check [--loopback]

Serves one page on 127.0.0.1 and loads it in build/webcalls-check (tests/webcalls_check.c: a WebKit view set up as
lord-web sets one up, WebKit's sandbox on, the cameras' udev entries added to it as lord-web adds them), the permission
answered yes as LordOS's prompt would. The page, inside WebKit's web process sandbox:

  list      getUserMedia({ video: true }) once, then enumerateDevices: every camera Linux has as a video4linux capture
            device is a videoinput (counted against /sys/class/video4linux; their names are compared, never printed)
  frames    each videoinput opened by its deviceId (exact), at least 5 real frames presented by a <video>, its size, then
            stopped: a few seconds per camera, nothing recorded or sent
  indicator build/camuse-check watches the /dev/video* nodes as lord-server does (src/hw/camuse.c): while the page holds
            a camera the "camera-use" event lists a holder, and after it stops the list is empty again

--loopback: when LordOS Studio's virtual camera (v4l2loopback, card "LordOS Studio") exists and nobody has it open, a test
pattern from ffmpeg is written to it for the length of the run, so it is a capture device, and it must be listed and give
frames like the real one (at least one: see below). Without a camera: "skipped: needs a camera". Prints one line per check, exits 1 on the first
failure."""
import http.server
import json
import os
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

CHECK = sys.argv[1] if len(sys.argv) > 1 else "build/webcalls-check"
CAMUSE = sys.argv[2] if len(sys.argv) > 2 else "build/camuse-check"
LOOPBACK = "--loopback" in sys.argv

PAGE = b"""<!doctype html><title>cam</title><body><video id=v autoplay muted playsinline width=320 height=240></video><script>
(async () => {
  const r = { cams: [] };
  try {
    const s = await navigator.mediaDevices.getUserMedia({ video: true });
    s.getTracks().forEach(t => t.stop());
  } catch (e) { r.first = e.name; }
  const list = (await navigator.mediaDevices.enumerateDevices()).filter(d => d.kind === 'videoinput');
  for (const d of list) {
    const c = { label: d.label, id: !!d.deviceId };
    try {
      const s = await navigator.mediaDevices.getUserMedia({ video: { deviceId: { exact: d.deviceId } } });
      const t = s.getVideoTracks()[0];
      c.track = t.label === d.label;
      // a <video> laid out on the page (one made by script and appended presents no frame in the headless view)
      const v = document.getElementById('v'); v.srcObject = s; v.play().catch(() => {});
      let n = 0;
      await new Promise(done => { const f = () => { if (++n >= 5) done(); else v.requestVideoFrameCallback(f); };
                                  v.requestVideoFrameCallback(f); setTimeout(done, 10000); });
      c.frames = n; c.w = v.videoWidth; c.h = v.videoHeight; c.state = t.readyState + (t.muted ? ' muted' : '') + ' ' + v.readyState;
      await new Promise(f => setTimeout(f, 1500));      // the indicator's turn: held for a moment
      s.getTracks().forEach(t => t.stop()); v.srcObject = null;
    } catch (e) { c.error = e.name; }
    r.cams.push(c);
  }
  await fetch('/report', { method: 'POST', body: JSON.stringify(r) });
  document.title = 'done';
})();
</script>"""

report = {}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.end_headers()
        self.wfile.write(PAGE)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        report.update(json.loads(self.rfile.read(n)))
        self.send_response(204)
        self.end_headers()


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


def fail(msg):
    print("FAIL " + msg)
    sys.exit(1)


def ok(msg):
    print("ok   " + msg)


def capture_cards():
    """the video4linux nodes that are capture devices now: {name: card}"""
    import fcntl
    import struct
    out = {}
    base = "/sys/class/video4linux"
    for n in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        if not n.startswith("video"):
            continue
        try:
            fd = os.open("/dev/" + n, os.O_RDONLY | os.O_NONBLOCK)
        except OSError:
            continue
        try:
            buf = bytearray(104)                         # struct v4l2_capability
            fcntl.ioctl(fd, 0x80685600, buf)             # VIDIOC_QUERYCAP
            caps, dcaps = struct.unpack_from("<II", buf, 84)
            c = dcaps if caps & 0x80000000 else caps     # V4L2_CAP_DEVICE_CAPS
            if c & 0x1 and c & 0x04000000:               # VIDEO_CAPTURE and STREAMING
                out[n] = buf[16:48].split(b"\0")[0].decode("utf-8", "replace")
        except OSError:
            pass
        finally:
            os.close(fd)
    return out


def held(dev):
    """someone of ours has dev open"""
    want = os.stat(dev).st_rdev
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            for f in os.listdir("/proc/%s/fd" % p):
                try:
                    if os.readlink("/proc/%s/fd/%s" % (p, f)).startswith("/dev/") and os.stat("/proc/%s/fd/%s" % (p, f)).st_rdev == want:
                        return True
                except OSError:
                    pass
        except OSError:
            pass
    return False


producer = None
if LOOPBACK:
    studio = [n for n in os.listdir("/sys/class/video4linux") if open("/sys/class/video4linux/%s/name" % n).read().strip() == "LordOS Studio"] \
        if os.path.isdir("/sys/class/video4linux") else []
    if not studio:
        print("skipped: --loopback needs LordOS Studio's virtual camera (v4l2loopback)")
    elif held("/dev/" + studio[0]):
        print("skipped: --loopback: LordOS Studio's camera is in use; it is left alone")
    else:
        producer = subprocess.Popen(["ffmpeg", "-nostdin", "-loglevel", "error", "-re", "-f", "lavfi", "-i", "testsrc2=size=640x480:rate=30",
                                     "-t", "60", "-pix_fmt", "yuyv422", "-f", "v4l2", "/dev/" + studio[0]], stdout=subprocess.DEVNULL)
        for _ in range(50):
            if studio[0] in capture_cards():
                break
            time.sleep(0.1)
        ok("LordOS Studio's camera (/dev/%s) has a test pattern written to it for this run" % studio[0])

try:
    cards = capture_cards()
    if not cards:
        print("skipped: needs a camera (no video4linux capture device)")
        sys.exit(0)
    srv = Server(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    watch = subprocess.Popen([CAMUSE, "90"], stdout=subprocess.PIPE, text=True)
    events = []
    threading.Thread(target=lambda: [events.append((time.monotonic(), l.strip())) for l in watch.stdout], daemon=True).start()
    time.sleep(0.5)
    home = tempfile.mkdtemp(prefix="webcam-check-")
    env = {k: v for k, v in os.environ.items() if not k.startswith(("WEBKIT_", "WEBCALLS_"))}
    env.update(HOME=home, XDG_CACHE_HOME=home + "/.cache", XDG_DATA_HOME=home + "/.local/share", XDG_CONFIG_HOME=home + "/.config")
    t0 = time.monotonic()
    p = subprocess.run([CHECK, "http://127.0.0.1:%d/" % port, "60"], env=env, capture_output=True, text=True, timeout=90)
    t1 = time.monotonic()
    time.sleep(1.5)                                       # the last close's scan
    watch.terminate()
    out = p.stdout.splitlines()
    sandboxed = next((l for l in out if l.startswith("cameras: ")), "")
    if p.returncode or not report:
        fail("the page didn't finish (%d): %s %s" % (p.returncode, " | ".join(out[-5:]), p.stderr[-400:]))
    ok("WebKit's sandbox has the udev entries of the video4linux nodes (%s)" % sandboxed[len("cameras: "):])
    if report.get("first"):
        fail("getUserMedia({ video: true }) failed: %s" % report["first"])
    got = [c["label"] for c in report["cams"]]
    want = sorted(set(cards.values()))
    if sorted(set(got)) != want:
        fail("the page lists %d camera(s), Linux has %d capture device(s) (names differ)" % (len(got), len(want)))
    ok("enumerateDevices lists every capture device Linux has: %d camera(s)%s" %
       (len(got), ", LordOS Studio's among them" if "LordOS Studio" in got else ""))
    for i, c in enumerate(report["cams"]):
        # LordOS Studio's camera fed by ffmpeg (--loopback): GStreamer's v4l2src stops after a few frames ("Failed to
        # allocate a buffer", the same outside WebKit's sandbox is not checked here): what this checks for it is that
        # the web process lists it and its frames reach the page
        least = 1 if c["label"] == "LordOS Studio" and producer else 5
        if c.get("error") or not c.get("id") or c.get("frames", 0) < least or not c.get("w") or not c.get("track"):
            sys.stderr.write(p.stderr[-3000:])
            fail("camera %d%s: %r" % (i, " (LordOS Studio)" if c["label"] == "LordOS Studio" else "", {k: v for k, v in c.items() if k != "label"}))
        ok("camera %d%s: opened by its deviceId inside the sandbox, %d frames at %dx%d" %
           (i, " (LordOS Studio)" if c["label"] == "LordOS Studio" else "", c["frames"], c["w"], c["h"]))
    during = [e for t, e in events if t0 <= t <= t1 and e.startswith("camera-use: ") and e != 'camera-use: {"apps":[]}']
    if not during:
        fail("the indicator's event never listed a holder while the page held a camera: %r" % events)
    if producer:
        # ffmpeg writing to the loopback holds a video4linux device itself for the whole run: "nobody after" is checked
        # by the run without --loopback
        ok("the capture indicator's \"camera-use\" (src/hw/camuse.c): %s while the page held a camera" % during[0][len("camera-use: "):])
    elif not events or events[-1][1] != 'camera-use: {"apps":[]}':
        fail("the indicator's event didn't go back to nobody after the page stopped: %r" % events[-3:])
    else:
        ok("the capture indicator's \"camera-use\" (src/hw/camuse.c): %s while the page held a camera, nobody after" % during[0][len("camera-use: "):])
finally:
    if producer:
        producer.terminate()
        producer.wait()
print("webcam-check: ok")
