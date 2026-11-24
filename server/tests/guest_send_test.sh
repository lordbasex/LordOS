#!/bin/sh
# guest_send_test.sh — lordos-guest 9's "Send to LordOS" (tools/guest/gsend.c; docs/VIRTUALIZATION.md 6.21). A Python
# listener plays lord-vm on a Unix socket (--port takes one for tests):
#   - the hello says "send"; `--send-to-host` is refused (exit 3, nothing offered) while the host's hello doesn't;
#   - a 6 MB file is offered with its size and name, sent from 0 when the host has nothing, never more than
#     AG_SEND_WINDOW (4 MiB) past the host's last acknowledgement while the host acknowledges late, and ends with the
#     file's SHA-256;
#   - a lost connection mid-file: the helper offers the same file again (the same key) and, the host holding its first
#     2 MiB, goes on from there (AGK_SEND_FROM 2 MiB) and the whole file's SHA-256 is right;
#   - a host whose first bytes differ (a wrong prefix hash) gets the file again from 0;
#   - "not now" (AGSA_BUSY) keeps the file and offers it again; AGSA_OFF drops it;
#   - folders are refused (exit 2) and nothing is offered.
# Run from server/ (tests/run_suites.sh does). Exit 77 when build/lordos-guest or python3 is missing, 1 on a failure.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lordos-guest ] && command -v python3 >/dev/null 2>&1 || { echo "needs build/lordos-guest and python3"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lgsend.XXXXXX") || exit 1
L='' G=''
trap 'kill $L $G 2>/dev/null; rm -rf "$D"' EXIT
mkdir -p "$D/py" "$D/run" "$D/home" "$D/files/sub"
chmod 700 "$D/run"
head -c 6000000 /dev/urandom >"$D/files/big.bin"
head -c 3000000 /dev/urandom >"$D/files/second file.dat"
echo small >"$D/files/small.txt"
cat >"$D/py/host.py" <<'EOF'
import hashlib, os, socket, struct, subprocess, sys, time
path, guest, run, home, files = sys.argv[1:6]
WINDOW = 4 << 20
def frame(kind, p): return struct.pack('<IHH', len(p), kind, 0) + p
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path); s.listen(1); s.settimeout(20)
open(path + '.ready', 'w').close()
env = dict(os.environ, XDG_RUNTIME_DIR=run, HOME=home)
for v in ('DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS'): env.pop(v, None)
fails = []
def check(ok, what):
    print(('ok   ' if ok else 'FAIL ') + what)
    if not ok: fails.append(what)
class Conn:
    def __init__(self):
        self.c, _ = s.accept(); self.c.settimeout(20); self.buf = b''
    def frames(self, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            while len(self.buf) >= 8:
                n, kind, _ = struct.unpack('<IHH', self.buf[:8])
                if len(self.buf) < 8 + n: break
                p = self.buf[8:8 + n]; self.buf = self.buf[8 + n:]
                yield kind, p
            self.c.settimeout(max(0.05, end - time.time()))
            try: d = self.c.recv(1 << 20)
            except socket.timeout: return
            if not d: return
            self.buf += d
    def send(self, kind, p): self.c.sendall(frame(kind, p))
    def hello(self, words):
        h = None
        for k, p in self.frames():
            if k == 1: h = p.decode(); break
        self.send(1, b'lord-vm 2 clipboard clipx clipreq elevated' + words)
        time.sleep(0.3)
        return h
def offer_of(p):
    i, size, flags = struct.unpack('<IQB', p[:13]); key = p[13:45]; nl = p[45] | p[46] << 8
    return i, size, flags, key, p[47:47 + nl].decode()
def accept(c, i, status, off=0, sha=b'\0' * 32): c.send(33, struct.pack('<IBQ', i, status, off) + sha)
def cli(*args): return subprocess.run([guest, '--send-to-host', *args], env=env, capture_output=True, timeout=10).returncode
def wait_offer(c, t=10):
    for k, p in c.frames(t):
        if k == 32: return offer_of(p)
    return None

c = Conn()
h = c.hello(b'')
check(h is not None and h.startswith('lordos-guest 9') and ' send' in h, 'hello: ' + str(h))
r = cli(files + '/small.txt')
check(r == 3 and wait_offer(c, 1.5) is None, 'refused while the host says no "send" (%d)' % r)
c.send(1, b'lord-vm 2 clipboard send'); time.sleep(0.3)
check(cli(files + '/sub') == 2, 'a folder is refused')

# a whole file, acknowledged late
big = open(files + '/big.bin', 'rb').read()
check(cli(files + '/big.bin') == 0, 'big.bin queued')
o = wait_offer(c)
check(o is not None and o[1] == len(big) and o[4] == 'big.bin' and o[0] > 0, 'the offer: size and name (%r)' % (o and (o[1], o[4]),))
key1 = o[3]
accept(c, o[0], 0)
got, acked, peak, end, frm = bytearray(), 0, 0, None, None
for k, p in c.frames(2):                               # no acknowledgement at all: it stops at the window
    if k == 34: frm = struct.unpack('<IQ', p)[1]
    elif k == 35: got += p[12:]
check(len(got) == WINDOW, 'with no acknowledgement it stops at exactly the window (%d)' % len(got))
acked = len(got); c.send(36, struct.pack('<IQ', o[0], acked))
for k, p in c.frames(30):
    if k == 34: frm = struct.unpack('<IQ', p)[1]
    elif k == 35:
        i, off = struct.unpack('<IQ', p[:12])
        if off != len(got): fails.append('a piece out of order'); break
        got += p[12:]
        peak = max(peak, len(got) - acked)
        if len(got) - acked >= 3 << 20:              # acknowledge late, 3 MiB at a time
            acked = len(got); c.send(36, struct.pack('<IQ', i, acked))
    elif k == 37: end = p[4:]; break
c.send(36, struct.pack('<IQ', o[0], len(got)))
check(frm == 0, 'it started at 0 (%r)' % frm)
check(bytes(got) == big, 'all 6 MB came whole (%d)' % len(got))
check(peak <= WINDOW, 'never more than the window unacknowledged (peak %d)' % peak)
check(end == hashlib.sha256(big).digest(), "the end carries the file's SHA-256")
c.send(38, struct.pack('<IB', o[0], 0))

# a lost connection mid-file, then a resume from what the host kept
sec = open(files + '/second file.dat', 'rb').read()
check(cli(files + '/second file.dat') == 0, 'the second file queued')
o = wait_offer(c)
accept(c, o[0], 0)
part = bytearray()
for k, p in c.frames(10):
    if k == 35:
        part += p[12:]
        if len(part) >= 2 << 20: break
c.c.close()
kept = bytes(part[:2 << 20])
c = Conn()
c.hello(b' send')
o2 = wait_offer(c, 15)
check(o2 is not None and o2[3] == o[3] and o2[4] == 'second file.dat', 'offered again with the same key after the lost connection')
accept(c, o2[0], 0, len(kept), hashlib.sha256(kept).digest())
rest, frm, end = bytearray(), None, None
for k, p in c.frames(20):
    if k == 34: frm = struct.unpack('<IQ', p)[1]
    elif k == 35:
        i, off = struct.unpack('<IQ', p[:12])
        rest += p[12:]
        c.send(36, struct.pack('<IQ', i, off + len(p) - 12))
    elif k == 37: end = p[4:]; break
check(frm == len(kept), 'it went on from what the host kept (%r)' % frm)
check(kept + bytes(rest) == sec and end == hashlib.sha256(sec).digest(), 'the resumed file is whole and its SHA-256 right')
c.send(38, struct.pack('<IB', o2[0], 0))

# the host's first bytes differ: from 0 again; "not now" first, then a wrong prefix
check(cli(files + '/small.txt') == 0, 'small.txt queued')
o = wait_offer(c)
accept(c, o[0], 2)                                     # AGSA_BUSY: offered again in 5 s
o = wait_offer(c, 10)
check(o is not None and o[4] == 'small.txt', 'offered again after "not now"')
accept(c, o[0], 0, 3, hashlib.sha256(b'xyz').digest())
frm = None
for k, p in c.frames(5):
    if k == 34: frm = struct.unpack('<IQ', p)[1]
    if k == 35: c.send(36, struct.pack('<IQ', o[0], 6))
    if k == 37: break
check(frm == 0, 'a prefix that differs starts again from 0 (%r)' % frm)
c.send(38, struct.pack('<IB', o[0], 0))

# AGSA_OFF drops it
check(cli(files + '/big.bin') == 0, 'big.bin queued again')
o = wait_offer(c)
accept(c, o[0], 1)
check(wait_offer(c, 7) is None, 'a machine that takes no files: the file is dropped, not offered again')
sys.exit(1 if fails else 0)
EOF
SOCK="$D/port"
python3 "$D/py/host.py" "$SOCK" "$(pwd)/build/lordos-guest" "$D/run" "$D/home" "$D/files" & L=$!
i=0; while [ ! -e "$SOCK.ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
env -u DISPLAY -u WAYLAND_DISPLAY -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR="$D/run" HOME="$D/home" build/lordos-guest --port "$SOCK" --windows "$D/none.sock" 2>"$D/guest.log" & G=$!
wait $L; rc=$?
L=''
kill $G 2>/dev/null
[ $rc -eq 0 ] || { echo "--- lordos-guest's log"; cat "$D/guest.log"; exit 1; }
echo "guest-send: ok"
