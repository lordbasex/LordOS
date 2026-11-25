# mh.py — the Machines end-to-end harness (README.md here): a real headless LordOS session (build/lord-server-test with
# the real default theme, plus the test-only driver and probe) in private user and mount namespaces, with a private home
# whose Machines library is a scratch folder; driven through its remote.sock (pointer, keys) and its test hooks (pictures),
# with commands run in the session as its user (driver.js). Test only: nothing here ships.
#
# As a library (python3 -I with sys.path set to this folder, or `make -C server e2e-machines`):
#   from mh import Session, skip
#   with Session() as s:                       # starts; stops and removes everything at the end (MH_KEEP=1 keeps it)
#       s.js("launchApp('machines')")          # JS in the session (lord, wm, sdk, mh, sleep, log…); returns its value
#       s.click_text("New Machine")            # a real click (remote.sock) on what shows that text
#       s.wait_text("Choose a system", 10)
#       img = s.shot("assistant")              # the screen as lord-server shows it (test hooks), saved as PNG
#       assert img.varied(s.window_rect("machines"))
#       s.ns(["./build/lord-vm", "--status", d])   # a command in the same kind of namespaces, as the session sees them
# As a command (for shell scripts): python3 -I mh.py start|js|ui|shot|stop … (README.md).
import json, os, re, select, shutil, signal, socket, stat, struct, subprocess, sys, tempfile, threading, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../../.."))
SERVER = os.path.join(ROOT, "server")
BUILD = os.path.join(SERVER, "build")
SCRATCH = os.environ.get("MH_SCRATCH", "/var/tmp/lordos-m100-e2e")
IMAGES = os.environ.get("MH_IMAGES", "/var/tmp/lordos-m100-images")
SKIP = 77


def private_dir(path):
    """path as a folder only this account can use: made 0700 if missing; if it is already there it must be a real
    folder (not a link) of this account, and it is set to 0700. Anything else (a folder another user made first in the
    shared /var/tmp, a link) raises: the harness never works in a folder someone else can swap things in."""
    parent = os.path.dirname(os.path.abspath(path))
    if not os.path.isdir(parent):
        os.makedirs(parent)
    try:
        os.mkdir(path, 0o700)
    except FileExistsError:
        pass
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        st = os.fstat(fd)
        if st.st_uid != os.getuid():
            raise RuntimeError("%s belongs to uid %d, not this account: refusing to use it (remove it or set another path)"
                               % (path, st.st_uid))
        if st.st_mode & 0o077:
            os.fchmod(fd, 0o700)
    finally:
        os.close(fd)
    return path


def scratch():
    """SCRATCH, checked private (private_dir) before any run folder is made in it."""
    return private_dir(SCRATCH)


def own_child(D):
    """D as one of this harness's run folders: a direct child of SCRATCH, a real folder of this account, 0700. For
    paths read back from .cli-session.json or found by `reap`, before anything is removed, killed or run on them."""
    D = os.path.abspath(D)
    if os.path.dirname(D) != os.path.abspath(SCRATCH) or os.path.basename(D) in ("", ".", ".."):
        raise RuntimeError("%s isn't a session folder in %s" % (D, SCRATCH))
    st = os.lstat(D)
    if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.getuid() or st.st_mode & 0o077:
        raise RuntimeError("%s isn't a private folder of this account" % D)
    return D


def proc_uid(pid):
    """The real uid a process runs as (/proc/PID/status; its folder's owner changes for a non-dumpable process)."""
    with open("/proc/%d/status" % int(pid)) as f:
        for line in f:
            if line.startswith("Uid:"):
                return int(line.split()[1])
    raise OSError("no Uid in /proc/%s/status" % pid)


def own_server(pid):
    """pid as a lord-server of this account that leads its own process group (Session.start's start_new_session)."""
    try:
        pid = int(pid)
        if pid <= 1 or proc_uid(pid) != os.getuid() or os.getpgid(pid) != pid:
            return False
        cmd = open("/proc/%d/cmdline" % pid, "rb").read().split(b"\0")
    except (OSError, ValueError, TypeError):
        return False
    return bool(cmd) and os.path.basename(cmd[0]) in (b"lord-server", b"lord-server-test")


class Skip(Exception):
    pass


def skip(why):
    raise Skip(why)


def log(*a):
    print(*a, flush=True)


# ---------------------------------------------------------------- pictures
class Image:
    """An RGB picture (from the test hooks' PPM). Coordinates in pixels of the headless screen."""

    def __init__(self, w, h, rgb, path=None):
        self.w, self.h, self.rgb, self.path = w, h, rgb, path

    @staticmethod
    def from_ppm(path):
        b = open(path, "rb").read()
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", b)
        if not m:
            raise ValueError("not a binary PPM: %s" % path)
        w, h = int(m.group(1)), int(m.group(2))
        return Image(w, h, b[m.end():m.end() + w * h * 3])

    def px(self, x, y):
        o = (y * self.w + x) * 3
        return tuple(self.rgb[o:o + 3])

    def _box(self, r):
        if r is None:
            return 0, 0, self.w, self.h
        x, y, w, h = (int(v) for v in r)
        x0, y0 = max(0, x), max(0, y)
        return x0, y0, min(self.w, x + w), min(self.h, y + h)

    def colors(self, r=None, step=2):
        """How many different colours a region has (sampled every `step` pixels)."""
        x0, y0, x1, y1 = self._box(r)
        seen = set()
        for y in range(y0, y1, step):
            o = (y * self.w) * 3
            row = self.rgb[o:o + self.w * 3]
            for x in range(x0, x1, step):
                seen.add(row[x * 3:x * 3 + 3])
        return len(seen)

    def varied(self, r=None, n=16):
        """Not a flat fill: the region has at least n colours (text, a picture)."""
        return self.colors(r) >= n

    def ink(self, r=None, step=2):
        """Share (0..1) of sampled pixels that aren't the region's most common colour: text on a flat background (a
        guest's console has two colours, so varied() isn't the test for it)."""
        x0, y0, x1, y1 = self._box(r)
        count = {}
        k = 0
        for y in range(y0, y1, step):
            o = (y * self.w) * 3
            row = self.rgb[o:o + self.w * 3]
            for x in range(x0, x1, step):
                p = row[x * 3:x * 3 + 3]
                count[p] = count.get(p, 0) + 1
                k += 1
        return 1 - max(count.values()) / k if k else 0.0

    def mean(self, r=None, step=4):
        x0, y0, x1, y1 = self._box(r)
        t = [0, 0, 0]
        k = 0
        for y in range(y0, y1, step):
            for x in range(x0, x1, step):
                p = self.px(x, y)
                t[0] += p[0]; t[1] += p[1]; t[2] += p[2]; k += 1
        return tuple(v / max(1, k) for v in t)

    def diff(self, other, r=None, step=2):
        """Share (0..1) of sampled pixels that differ from another picture of the same size."""
        x0, y0, x1, y1 = self._box(r)
        d = k = 0
        for y in range(y0, y1, step):
            for x in range(x0, x1, step):
                k += 1
                if self.px(x, y) != other.px(x, y):
                    d += 1
        return d / max(1, k)

    def save_png(self, out):
        raw = bytearray()
        for y in range(self.h):
            raw.append(0)
            raw += self.rgb[y * self.w * 3:(y + 1) * self.w * 3]
        ch = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
        with open(out, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0)) +
                    ch(b"IDAT", zlib.compress(bytes(raw), 6)) + ch(b"IEND", b""))
        self.path = out
        return out


# ---------------------------------------------------------------- remote.sock (server/src/remote/proto.h)
KEYS = {c: k for c, k in zip("qwertyuiop", range(16, 26))}
KEYS.update({c: k for c, k in zip("asdfghjkl", range(30, 39))})
KEYS.update({c: k for c, k in zip("zxcvbnm", range(44, 51))})
KEYS.update({c: k for c, k in zip("1234567890", range(2, 12))})
KEYS.update({" ": 57, "-": 12, "=": 13, "[": 26, "]": 27, ";": 39, "'": 40, "`": 41, "\\": 43, ",": 51, ".": 52, "/": 53, "\n": 28, "\t": 15})
SHIFTED = {c: k for c, k in zip("!@#$%^&*()", range(2, 12))}
SHIFTED.update({"_": 12, "+": 13, "{": 26, "}": 27, ":": 39, '"': 40, "~": 41, "|": 43, "<": 51, ">": 52, "?": 53})
KEY = {"enter": 28, "esc": 1, "tab": 15, "backspace": 14, "space": 57, "up": 103, "down": 108, "left": 105, "right": 106,
       "ctrl": 29, "shift": 42, "alt": 56, "meta": 125, "delete": 111, "home": 102, "end": 107, "f2": 60}


class Input:
    """Pointer and keys, injected the way the remote desktop's are (main.c's remote_pointer, remote_button, remote_wheel,
    remote_key). Over the test hooks' socket (testhooks.h: ptr, btn, wheel, key) when the harness runs as the person;
    remote.sock itself takes only root's connections (lord-sessiond's), so RemoteInput below is for a run as root."""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.s.settimeout(10)
        self.s.connect(path)

    def cmd(self, c):
        self.s.send(c.encode())
        r = self.s.recv(256).decode(errors="replace")
        if r != "ok":
            raise RuntimeError("test hook %r: %s" % (c, r))

    def move(self, x, y):
        self.cmd("ptr %d %d" % (max(0, int(x)), max(0, int(y)))); time.sleep(0.03)

    def button(self, b, down):
        self.cmd("btn %d %d" % (b, 1 if down else 0)); time.sleep(0.04)

    def wheel_steps(self, n):
        self.cmd("wheel %d" % n)

    def key(self, code, down):
        self.cmd("key %d %d" % (code, 1 if down else 0)); time.sleep(0.02)

    def click(self, x, y, button=0, n=1):
        self.move(x, y); time.sleep(0.1)
        for _ in range(n):
            self.button(button, 1); self.button(button, 0)

    def drag(self, x0, y0, x1, y1, steps=12):
        self.move(x0, y0); time.sleep(0.1); self.button(0, 1)
        for i in range(1, steps + 1):
            self.move(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps)
        time.sleep(0.1); self.button(0, 0)

    def wheel(self, x, y, steps):
        self.move(x, y)
        for _ in range(abs(steps)):
            self.wheel_steps(-1 if steps < 0 else 1); time.sleep(0.05)

    def press(self, *names):
        """press('ctrl', 'c'): a combination; names from KEY, single characters or evdev codes."""
        codes = [KEY.get(n) or KEYS.get(n) or int(n) for n in names]
        for c in codes:
            self.key(c, 1)
        for c in reversed(codes):
            self.key(c, 0)

    def type(self, text):
        """Text typed on a US keyboard (the harness's session has that layout)."""
        for ch in text:
            if (ch.isupper() and ch.lower() in KEYS) or ch in SHIFTED:
                k = SHIFTED.get(ch) or KEYS[ch.lower()]
                self.key(42, 1); self.key(k, 1); self.key(k, 0); self.key(42, 0)
            else:
                self.key(KEYS[ch], 1); self.key(KEYS[ch], 0)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


class RemoteInput(Input):
    """The same over remote.sock (server/src/remote/proto.h), as the remote desktop sends it: only for a run as root."""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.lock = threading.Lock()
        threading.Thread(target=self._drain, daemon=True).start()

    def send(self, t, d=b""):
        with self.lock:
            self.s.sendall(struct.pack("<IB", 1 + len(d), t) + d)

    def _drain(self):       # answers the frames' RP_END with RP_READY and throws the rest away
        try:
            while True:
                n, = struct.unpack("<I", self._recvn(4))
                if self._recvn(n)[:1] == b"\x03":
                    self.send(10)
        except OSError:
            pass

    def _recvn(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise OSError("closed")
            b += c
        return b

    def move(self, x, y):
        self.send(11, struct.pack("<hh", int(x), int(y))); time.sleep(0.05)

    def button(self, b, down):
        self.send(12, struct.pack("<BB", b, 1 if down else 0)); time.sleep(0.05)

    def wheel_steps(self, n):
        self.send(13, struct.pack("<b", n))

    def key(self, code, down):
        self.send(14, struct.pack("<HB", code, 1 if down else 0)); time.sleep(0.03)


# ---------------------------------------------------------------- the namespaces
def realhome():
    import pwd
    return pwd.getpwuid(os.getuid()).pw_dir


def ns_prefix(scratch):
    """unshare: root of a new user namespace only to mount (an empty folder over the real home, a tmpfs over the real
    runtime folder), then this account again in a nested one; the session never sees the person's files or session."""
    uid, gid = os.getuid(), os.getgid()
    rt = "/run/user/%d" % uid
    fake = os.path.join(scratch, "fake")
    # the paths are sh's positional parameters, never part of the script's text (as ns-run.sh does)
    inner = ('mount --bind "$1" "$2" && { [ -d "$3" ] && mount -t tmpfs -o mode=700 tmpfs "$3" || true; } && shift 3 && '
             'exec unshare --user --map-user=%d --map-group=%d "$@"') % (uid, gid)
    return ["unshare", "-Urm", "sh", "-c", inner, "ns", fake, realhome(), rt]


def need(cond, why):
    if not cond:
        skip(why)


def have(cmd):
    return shutil.which(cmd) is not None


def kvm_ok():
    return os.access("/dev/kvm", os.R_OK | os.W_OK)


# ---------------------------------------------------------------- the session
class Session:
    def __init__(self, width=1280, height=800, lang="en", settings=None, server="lord-server-test", gpu=False, name="mh", files=None):
        self.W, self.H, self.lang = width, height, lang
        self.extra_settings = settings or {}
        self.server = server
        self.gpu = gpu
        self.name = name
        self.files = files or {}                    # {path in the home: a file to put there before the session starts}
        self.proc = None
        self.remote = None
        self.n = 0
        self.keep = os.environ.get("MH_KEEP") == "1"
        self.out = os.environ.get("MH_OUT")         # where screenshots are copied (kept after the run)
        self.D = None

    # -- paths
    @property
    def home(self): return os.path.join(self.D, "home")

    @property
    def run(self): return os.path.join(self.D, "run")

    @property
    def library(self): return os.path.join(self.home, "Machines")

    def env(self, extra=None):
        e = {"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "HOME": self.home, "XDG_RUNTIME_DIR": self.run, "LANG": "C.UTF-8",
             "LORD_GPU": "1" if self.gpu else "0", "LORD_HEADLESS_DRAW": "1"}
        for k in ("MH_IMAGES",):
            if k in os.environ:
                e[k] = os.environ[k]
        if extra:
            e.update(extra)
        return e

    # -- start and stop
    def check(self):
        need(os.access(os.path.join(BUILD, self.server), os.X_OK), "needs server/build/%s (make -C server)" % self.server)
        need(os.access(os.path.join(BUILD, "lord-vm"), os.X_OK), "needs server/build/lord-vm")
        need(have("unshare") and have("node"), "needs unshare and node")
        r = subprocess.run(["unshare", "-Urm", "true"], capture_output=True)
        need(r.returncode == 0, "needs unprivileged user namespaces")
        need(os.path.isdir(os.path.join(ROOT, "node_modules", "esbuild")), "needs node_modules (npm install)")

    def bundle(self):
        b = os.environ.get("MH_BUNDLE")
        if b:
            return b
        out = os.path.join(self.D, "theme.mh.bundle.js")
        r = subprocess.run(["node", os.path.join(ROOT, "tools/vm/e2e/build-e2e.mjs"), "--out", out], cwd=ROOT,
                           capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError("the e2e bundle didn't build: " + r.stderr[-2000:])
        return out

    def start(self):
        self.check()
        self.D = tempfile.mkdtemp(prefix="%s." % self.name, dir=scratch())
        os.chmod(self.D, 0o700)
        for d in ("home", "run", "fake", "shots", "home/Machines", "home/.e2e", "home/.config/lordos"):
            os.makedirs(os.path.join(self.D, d), exist_ok=True)
        os.chmod(self.run, 0o700)
        os.chmod(self.home, 0o700)
        st = {"mode": "dark", "language": self.lang, "languages": [self.lang], "welcomeShown": True, "schema": 1,
              "keyboards": [{"layout": "us", "variant": ""}],
              "reduceMotion": True, "firstStartDone": True}
        st.update(self.extra_settings)
        with open(os.path.join(self.home, ".config/lordos/settings.json"), "w") as f:
            json.dump(st, f)
        for rel, src in self.files.items():
            self.put(rel, src)
        theme = self.bundle()
        args = ns_prefix(self.D) + [os.path.join(BUILD, self.server), "--headless", "%dx%d" % (self.W, self.H),
                                    "--theme", theme, "--fonts", "../assets/fonts", "--shell", "../assets/shell"]
        if self.server == "lord-server-test":
            args.append("--test-hooks")
        self.log = open(os.path.join(self.D, "server.log"), "w")
        self.proc = subprocess.Popen(args, cwd=SERVER, env=self.env(), stdout=self.log, stderr=subprocess.STDOUT,
                                     start_new_session=True)
        sock = os.path.join(self.run, "lordos/remote.sock")
        hooks = os.path.join(self.run, "lordos-test-hooks.sock")
        end = time.time() + 30
        while time.time() < end and not (os.path.exists(sock) and os.path.exists(hooks)):
            if self.proc.poll() is not None:
                raise RuntimeError("lord-server exited %s:\n%s" % (self.proc.returncode, self.tail()))
            time.sleep(0.1)
        if not os.path.exists(hooks):
            raise RuntimeError("no test hooks socket:\n" + self.tail())
        self.remote = RemoteInput(sock) if os.getuid() == 0 else Input(hooks)
        self.js("return 1", timeout=30)                 # the driver answers: the desktop is up
        log("session up: %s (%dx%d, %s)" % (self.D, self.W, self.H, self.server))
        return self

    def logtext(self):
        """What the session printed: server.log and its own logs (~/.cache/lordos/shell.log, compositor.log)."""
        t = ""
        for f in (os.path.join(self.D, "server.log"), os.path.join(self.home, ".cache/lordos/compositor.log"),
                  os.path.join(self.home, ".cache/lordos/shell.log")):
            try:
                t += open(f, errors="replace").read()
            except OSError:
                pass
        return t

    def tail(self, n=40):
        return "\n".join(self.logtext().splitlines()[-n:])

    def put(self, rel, src):
        """A file in the session's home: a hard link to src when it is on the same disk (a guest image is read only and
        Machines copies it in), a copy otherwise."""
        dst = os.path.join(self.home, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        try:
            os.link(src, dst)
        except OSError:
            shutil.copy(src, dst)
        return dst

    def machines(self):
        out = []
        for root, dirs, _ in os.walk(self.library):
            for d in dirs:
                if d.endswith(".lordvm"):
                    out.append(os.path.join(root, d))
        return out

    def stop(self):
        if not self.D:
            return
        for m in self.machines():                       # no machine outlives the run
            self.ns([os.path.join(BUILD, "lord-vm"), "--poweroff", m], timeout=60, check=False)
        if self.remote:
            self.remote.close()
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
                self.proc.wait(10)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        if self.keep:
            log("kept: %s" % self.D)
        else:
            shutil.rmtree(self.D, ignore_errors=True)

    def __enter__(self):
        return self.start()

    def __exit__(self, *a):
        self.stop()

    # -- the session's JS (driver.js)
    def last_cmd(self):
        """The number of the last command given to the driver (it takes them in order, from 1): for a second process
        attaching to a running session (mh.py's commands)."""
        d = os.path.join(self.home, ".e2e")
        ns = [int(m.group(1)) for f in os.listdir(d) for m in [re.match(r"(?:cmd|out)-(\d+)\.(?:js|json)$", f)] if m]
        return max(ns) if ns else 0

    def js(self, src, timeout=30):
        """Runs src (an async function's body) in the session; returns its value or raises with its error."""
        self.n += 1
        d = os.path.join(self.home, ".e2e")
        tmp = os.path.join(d, ".cmd-%d.tmp" % self.n)
        with open(tmp, "w") as f:
            f.write(src)
        os.rename(tmp, os.path.join(d, "cmd-%d.js" % self.n))
        out = os.path.join(d, "out-%d.json" % self.n)
        end = time.time() + timeout
        while time.time() < end:
            if os.path.exists(out):
                time.sleep(0.05)
                r = json.load(open(out))
                for line in r.get("logs") or []:
                    log("  js: " + line)
                if not r.get("ok"):
                    raise RuntimeError("JS failed: %s\n%s" % (r.get("error"), (r.get("detail") or "")[:1500]))
                return r.get("value")
            if self.proc.poll() is not None:
                raise RuntimeError("lord-server is gone:\n" + self.tail())
            time.sleep(0.05)
        raise TimeoutError("JS timed out after %ss: %s" % (timeout, src[:200]))

    def wait_js(self, expr, timeout=30, every=0.3, what=None):
        """Waits until the JS expression is truthy; returns its value."""
        end = time.time() + timeout
        last = None
        while time.time() < end:
            last = self.js("return (%s)" % expr)
            if last:
                return last
            time.sleep(every)
        raise TimeoutError("waited %ss for %s (last: %r)" % (timeout, what or expr, last))

    # -- what is on screen (probe.js)
    def find(self, pattern, within=None, all=False):
        pat = "new RegExp(%s, %s)" % (json.dumps(pattern.pattern), json.dumps("i" if pattern.flags & re.I else "")) \
            if isinstance(pattern, re.Pattern) else json.dumps(pattern)
        return self.js("return mh.find(%s, { within: %s, all: %s })" % (pat, json.dumps(within), "true" if all else "false"))

    def texts(self, within=None):
        return self.js("return mh.texts({ within: %s })" % json.dumps(within))

    def wait_text(self, pattern, timeout=20, within=None, nth=0):
        """The nth match (reading order; -1 the last, the button under a sheet's text) once it is on screen."""
        end = time.time() + timeout
        while time.time() < end:
            f = self.find(pattern, within, all=nth != 0)
            if f:
                return f[nth] if -len(f) <= nth < len(f) else f[-1]
            time.sleep(0.3)
        raise TimeoutError("no %r on screen after %ss; texts: %s" % (pattern, timeout, self.texts(within)[:60]))

    def gone_text(self, pattern, timeout=20, within=None):
        end = time.time() + timeout
        while time.time() < end:
            if not self.find(pattern, within):
                return True
            time.sleep(0.3)
        raise TimeoutError("%r still on screen after %ss" % (pattern, timeout))

    def click_text(self, pattern, within=None, timeout=10, button=0, n=1, dx=0, dy=0, nth=0):
        t = self.wait_text(pattern, timeout, within, nth)
        x, y, w, h = t["rect"]
        self.remote.click(x + w // 2 + dx, y + h // 2 + dy, button=button, n=n)
        time.sleep(0.3)
        return t

    def find_image(self, part, within=None, all=False):
        return self.js("return mh.findImage(%s, { within: %s, all: %s })" % (json.dumps(part), json.dumps(within), "true" if all else "false"))

    def click_icon(self, name, within=None, timeout=10, button=0):
        """An icon button (no text): Machines' symbol NAME (assets/machines/symbols/NAME-symbolic.svg), or any part of an
        image's source."""
        part = "/%s-symbolic.svg" % name if re.match(r"^[a-z0-9-]+$", name) else name
        end = time.time() + timeout
        while True:
            f = self.find_image(part, within)
            if f:
                x, y, w, h = f[0]["rect"]
                self.remote.click(x + w // 2, y + h // 2, button=button)
                time.sleep(0.3)
                return f[0]
            if time.time() > end:
                raise TimeoutError("no icon %s on screen" % part)
            time.sleep(0.3)

    def click(self, x, y, **k):
        self.remote.click(x, y, **k)
        time.sleep(0.2)

    def type(self, text):
        self.remote.type(text)

    def press(self, *keys):
        self.remote.press(*keys)

    # -- windows (driver.js wm)
    def windows(self):
        return self.js("return wm.windows()")

    def window(self, app=None, pred=None, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            for w in self.windows():
                if (app is None or w["app"] == app) and (pred is None or pred(w)) and not w.get("minimized"):
                    return w
            time.sleep(0.3)
        raise TimeoutError("no window of %s" % app)

    @staticmethod
    def rect_of(w):
        return [w["x"], w["y"], w["w"], w["h"]]

    def window_rect(self, app=None, pred=None):
        return self.rect_of(self.window(app, pred))

    # -- pictures (the test hooks' frame command)
    def hook(self, cmd, timeout=10):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        s.settimeout(timeout)
        s.connect(os.path.join(self.run, "lordos-test-hooks.sock"))
        s.send(cmd.encode())
        r = s.recv(256).decode(errors="replace")
        s.close()
        return r

    def shot(self, name, which="output"):
        ppm = os.path.join(self.run, "lordos-test-frame-%s.ppm" % which)
        try:
            os.unlink(ppm)
        except FileNotFoundError:
            pass
        r = self.hook("frame %s" % which)
        if r != "ok":
            raise RuntimeError("frame: " + r)
        end = time.time() + 5
        while not os.path.exists(ppm) and time.time() < end:
            time.sleep(0.05)
        img = Image.from_ppm(ppm)
        png = img.save_png(os.path.join(self.D, "shots", "%s.png" % name))
        if self.out:
            os.makedirs(self.out, exist_ok=True)
            shutil.copy(png, os.path.join(self.out, "%s.png" % name))
        return img

    # -- commands in the session's kind of namespaces (lord-vm, machines, a guest's console)
    def ns(self, argv, timeout=120, check=True, env=None, input=None):
        r = subprocess.run(ns_prefix(self.D) + list(argv), cwd=SERVER, env=self.env(env), capture_output=True, text=True,
                           timeout=timeout, input=input)
        if check and r.returncode:
            raise RuntimeError("%s: exit %d\n%s%s" % (" ".join(argv), r.returncode, r.stdout[-1500:], r.stderr[-1500:]))
        return r

    def lordvm(self, *args, **k):
        return self.ns([os.path.join(BUILD, "lord-vm")] + list(args), **k)

    def status(self, mdir):
        r = self.lordvm("--status", mdir, "--json", check=False)
        try:
            return json.loads(r.stdout)
        except ValueError:
            return {}

    def console(self, machine_id, timeout=30):
        return Console(self.run, machine_id, timeout)

    def serial(self, mdir):
        try:
            return open(os.path.join(mdir, "log", "serial.log"), errors="replace").read()
        except OSError:
            return ""

    def vmlog(self, mdir):
        try:
            return open(os.path.join(mdir, "log", "lord-vm.log"), errors="replace").read()
        except OSError:
            return ""

    def wait_serial(self, mdir, text, timeout=120):
        end = time.time() + timeout
        while time.time() < end:
            if text in self.serial(mdir):
                return True
            time.sleep(0.5)
        raise TimeoutError("no %r on %s's serial console after %ss" % (text, os.path.basename(mdir), timeout))

    def vms(self):
        """lord.vm.list() as the app sees it."""
        return self.js("return lord.vm.list()")

    def vm(self, name, timeout=0):
        end = time.time() + timeout
        while True:
            for m in self.vms():
                if m.get("name") == name:
                    return m
            if time.time() >= end:
                raise LookupError("no machine named %r in the library" % name)
            time.sleep(0.5)

    def wait_state(self, name, state, timeout=60):
        end = time.time() + timeout
        last = None
        while time.time() < end:
            try:
                last = self.vm(name)
                if last.get("state") == state:
                    return last
            except LookupError:
                pass
            time.sleep(0.5)
        raise TimeoutError("%s isn't %s after %ss (it is %s)" % (name, state, timeout, last and (last.get("state"), last.get("reason"))))

    def errors(self):
        """JS errors the session printed (TypeError, ReferenceError, an exception, React's)."""
        t = self.logtext()
        return [l for l in t.splitlines() if re.search(r"TypeError|ReferenceError|SyntaxError|exception|react \(", l)]


# ---------------------------------------------------------------- a guest's serial console (lord-vm's VM_CONSOLE)
class Console:
    """The serial console of a running machine (the Shell's stream, vmproto.h VM_CONSOLE → VM_CONSOLE_FD), for checks
    inside the guest. The harness's guest image logs root in there by itself (images.py)."""

    def __init__(self, run_dir, machine_id, timeout=30):
        import array
        s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        s.settimeout(timeout)
        s.connect(os.path.join(run_dir, "lordos-vm", "%s.sock" % machine_id))
        s.send(struct.pack("<HHIiiii200s", 13, 1, 77, 0, 0, 0, 0, b""))
        fd = -1
        end = time.time() + timeout
        while fd < 0 and time.time() < end:
            msg, anc, _, _ = s.recvmsg(256, socket.CMSG_SPACE(4))
            for lvl, typ, d in anc:
                if lvl == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
                    fd = array.array("i", d[:4])[0]
            if struct.unpack("<H", msg[:2])[0] == 64 and fd < 0:
                raise RuntimeError("the console was refused")
        self.ctl = s
        self.c = socket.socket(fileno=fd)
        self.c.setblocking(False)
        self.k = 0
        self.c.send(b"\r")
        self._until(b"#", 10)
        self.c.send(b"dmesg -n 1 2>/dev/null; stty -echo; export TERM=dumb PS1=; echo __W__\r")
        self._until(b"\n__W__", 15)

    def _until(self, mark, t):
        b = b""
        end = time.time() + t
        while time.time() < end:
            if select.select([self.c], [], [], 0.3)[0]:
                try:
                    k = self.c.recv(65536)
                except BlockingIOError:
                    continue
                if not k:
                    break
                b += k
                if mark in b:
                    return b
        return b

    def run(self, cmd, timeout=60):
        """The command's output (stdout and stderr), and its exit status."""
        self.k += 1
        m = "__E%d__" % self.k
        self.c.send(("%s; echo; echo %s $?\r" % (cmd, m)).encode())
        o = self._until(("\n" + m).encode(), timeout).decode(errors="replace").replace("\r", "")
        o = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", o)
        head, _, tail = o.partition("\n" + m)
        st = re.match(r" (\d+)", tail)
        return head.strip(), int(st.group(1)) if st else None

    def close(self):
        for x in (self.c, self.ctl):
            try:
                x.close()
            except OSError:
                pass


# ---------------------------------------------------------------- results
class Results:
    """ok/bad/skp lines in the gates' format, and the exit status: 0 passed, 1 a failure, 77 everything skipped."""

    def __init__(self, title):
        self.title, self.p, self.f, self.s = title, 0, 0, 0

    def ok(self, what):
        self.p += 1; log("PASS  " + what)

    def bad(self, what, why=""):
        self.f += 1; log("FAIL  " + what + (": " + str(why) if why else ""))

    def skp(self, what, why=""):
        self.s += 1; log("SKIP  " + what + (": " + str(why) if why else ""))

    def t(self, what, cond, why=""):
        (self.ok if cond else lambda w: self.bad(w, why))(what)
        return cond

    def step(self, what, fn):
        """Runs fn(); a raised error is a failure with its message, Skip a skip."""
        try:
            r = fn()
            if r is not False:
                self.ok(what)
            return r
        except Skip as e:
            self.skp(what, e)
        except Exception as e:      # noqa: BLE001 — one step's failure must not stop the others
            self.bad(what, "%s: %s" % (type(e).__name__, str(e)[:1500]))
        return None

    def end(self):
        log("== %s: %d passed, %d failed, %d skipped" % (self.title, self.p, self.f, self.s))
        return 1 if self.f else (SKIP if self.p == 0 else 0)


def main_guard(fn):
    """Runs a flow's main(); a Skip before anything ran is exit 77 with 'skipped: <why>'."""
    try:
        sys.exit(fn())
    except Skip as e:
        log("skipped: %s" % e)
        sys.exit(SKIP)


# ---------------------------------------------------------------- command line (for shell scripts)
STATE = os.path.join(SCRATCH, ".cli-session.json")


def _cli():
    a = sys.argv[1:]
    if not a:
        print(__doc__ or "usage: mh.py start|js|ui|shot|stop"); return 2
    if a[0] == "reap":
        # what a run killed with SIGKILL left: every scratch session's machines powered off, its lord-server ended
        import glob
        scratch()
        for D in glob.glob(os.path.join(SCRATCH, "*.*")):
            try:
                D = own_child(D)
            except (OSError, RuntimeError):
                continue                            # not a run folder of this account's: left alone
            s = Session()
            s.D = D
            for m in s.machines():
                s.lordvm("--poweroff", m, check=False)
            for pid in os.listdir("/proc"):
                if not pid.isdigit() or int(pid) == os.getpid():
                    continue
                try:
                    cmd = open("/proc/%s/cmdline" % pid, "rb").read().split(b"\0")
                    mine = proc_uid(pid) == os.getuid()
                except OSError:
                    continue
                if mine and cmd and cmd[0].endswith(b"/lord-server-test") and any(D.encode() in x for x in cmd):
                    os.kill(int(pid), signal.SIGTERM)
                    print("ended lord-server-test %s (%s)" % (pid, D))
            if os.environ.get("MH_KEEP") != "1":
                shutil.rmtree(D, ignore_errors=True)
        return 0
    if a[0] == "start":
        s = Session(files=dict(f.split("=", 1) for f in a[1:]))
        s.keep = True
        s.start()
        json.dump({"D": s.D, "pid": s.proc.pid, "n": s.n}, open(STATE, "w"))
        print(s.D); return 0
    scratch()                                       # the state file is trusted only inside a private SCRATCH
    st = json.load(open(STATE))
    s = Session()
    s.D = own_child(st["D"])
    s.n = s.last_cmd()

    class P:
        pid = st["pid"]
        def poll(self): return None
    s.proc = P()
    try:
        if a[0] == "js":
            print(json.dumps(s.js(a[1], float(a[2]) if len(a) > 2 else 30)))
        elif a[0] == "shot":
            img = s.shot(a[1]); print(img.path)
        elif a[0] == "ui":
            s.remote = Input(os.path.join(s.run, "lordos-test-hooks.sock"))
            for c in a[1:]:
                v, _, arg = c.partition(":")
                if v == "click": s.click(*map(int, arg.split(",")))
                elif v == "text": s.click_text(arg)
                elif v == "type": s.type(arg)
                elif v == "key": s.press(*arg.split("+"))
                elif v == "wait": time.sleep(float(arg))
            s.remote.close()
        elif a[0] == "stop":
            s.proc = None
            for m in s.machines():
                s.lordvm("--poweroff", m, check=False)
            if own_server(st["pid"]):              # this account's lord-server, leading its own group
                try:
                    os.killpg(int(st["pid"]), signal.SIGTERM)
                except ProcessLookupError:
                    pass
            else:
                log("not ending pid %r: not a lord-server of this account leading its process group" % st["pid"])
            shutil.rmtree(s.D, ignore_errors=True); os.unlink(STATE)
    finally:
        if os.path.exists(STATE):
            st["n"] = s.n; json.dump(st, open(STATE, "w"))
    return 0


if __name__ == "__main__":
    sys.exit(_cli())
