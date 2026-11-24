#!/bin/sh
# guest_shell_test.sh — lordos-guest 8's agent shell and its links and notifications for the host (tools/guest/gshell.c;
# docs/VIRTUALIZATION.md 6.12, 6.18). A Python listener plays lord-vm on a Unix socket (--port takes one for tests):
#   - the hello says "shell links notify";
#   - a shell opens as this user (their login shell, never one the host names), runs what is typed, sizes itself, and
#     ends with its exit status;
#   - flow control: 2 MB of output with the host acknowledging slowly never has more than AG_SHELL_WINDOW (256 KiB)
#     unacknowledged, and arrives whole;
#   - a host that ends the shell (AGK_SHELL_CLOSE) ends its programs;
#   - `--open-on-host` sends the link percent-encoded only while the host's hello says "links"; `--notify-host` is
#     refused (exit 3, nothing sent) while it doesn't say "notify"; a file: link is refused before anything is sent;
#   - another uid can't use the helper's socket (checked as far as this test can: the socket is 0600).
# Run from server/ (tests/run_suites.sh does). Exit 77 when build/lordos-guest or python3 is missing, 1 on a failure.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lordos-guest ] && command -v python3 >/dev/null 2>&1 || { echo "needs build/lordos-guest and python3"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lgsh.XXXXXX") || exit 1
L='' G=''
trap 'kill $L $G 2>/dev/null; rm -rf "$D"' EXIT
mkdir -p "$D/py" "$D/run" "$D/home"
chmod 700 "$D/run"
cat >"$D/py/host.py" <<'EOF'
import os, socket, struct, subprocess, sys, time
path, guest, run, home = sys.argv[1:5]
WINDOW = 256 * 1024
def frame(kind, p): return struct.pack('<IHH', len(p), kind, 0) + p
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path); s.listen(1); s.settimeout(15)
open(path + '.ready', 'w').close()
c, _ = s.accept(); c.settimeout(15)
buf = b''
def frames(timeout=10):
    global buf
    end = time.time() + timeout
    while time.time() < end:
        while len(buf) >= 8:
            n, kind, _ = struct.unpack('<IHH', buf[:8])
            if len(buf) < 8 + n: break
            p = buf[8:8 + n]; buf = buf[8 + n:]
            yield kind, p
        c.settimeout(max(0.05, end - time.time()))
        try: d = c.recv(65536)
        except socket.timeout: return
        if not d: return
        buf += d
fails = []
def check(ok, what):
    print(('ok   ' if ok else 'FAIL ') + what)
    if not ok: fails.append(what)
hello = None
for k, p in frames():
    if k == 1: hello = p.decode(); break
check(hello is not None and ' shell' in hello and ' links notify' in hello and hello.startswith('lordos-guest 8'), 'hello: ' + str(hello))
c.sendall(frame(1, b'lord-vm 2 clipboard clipx clipreq elevated links'))
env = dict(os.environ, XDG_RUNTIME_DIR=run, HOME=home)
time.sleep(0.3)

# a shell: typed, sized, exit status
c.sendall(frame(27, struct.pack('<IHH', 5, 100, 30)))
c.sendall(frame(28, struct.pack('<I', 5) + b'stty size; echo hi-$((6*7)) $LORDOS_AGENT_SHELL; exit 3\n'))
out, status = b'', None
for k, p in frames(15):
    if k == 28:
        i, = struct.unpack('<I', p[:4]); out += p[4:]
        c.sendall(frame(31, struct.pack('<II', i, len(p) - 4)))
    elif k == 30:
        i, status = struct.unpack('<Ii', p); break
check(b'hi-42 1' in out, 'the shell ran what was typed: %r' % out[-300:])
check(b'30 100' in out, 'the pty has the size asked for')
check(status == 3, 'its exit status came back (%r)' % status)

# flow control: 2 MB, acknowledged slowly
c.sendall(frame(27, struct.pack('<IHH', 6, 80, 24)))
c.sendall(frame(28, struct.pack('<I', 6) + b"stty -echo; head -c 2000000 /dev/zero | tr '\\0' a; echo; echo END-MARK; exit 0\n"))
got, unacked, peak, status, pend = 0, 0, 0, None, []
for k, p in frames(60):
    if k == 28:
        n = len(p) - 4
        got += p[4:].count(b'a'); unacked += n; peak = max(peak, unacked)
        pend.append(n)
        if sum(pend) >= 192 * 1024:               # acknowledge late, 192 KiB at a time
            t = sum(pend); pend = []
            c.sendall(frame(31, struct.pack('<II', 6, t))); unacked -= t
    elif k == 30:
        _, status = struct.unpack('<Ii', p); break
check(got >= 2000000, 'all 2 MB came (%d)' % got)
check(peak <= WINDOW, 'never more than the window unacknowledged (peak %d)' % peak)
check(status == 0, 'the second shell ended with 0 (%r)' % status)

# the host ends a shell: its programs go
c.sendall(frame(27, struct.pack('<IHH', 7, 80, 24)))
c.sendall(frame(28, struct.pack('<I', 7) + b'echo $$ > ' + home.encode() + b'/pid; sleep 300\n'))
time.sleep(1.5)
for k, p in frames(0.5): pass
c.sendall(frame(30, struct.pack('<Ii', 7, 0)))
time.sleep(1)
try:
    pid = int(open(home + '/pid').read())
    alive = os.path.exists('/proc/%d' % pid)
except Exception: alive = True
check(not alive, 'the host closing the shell ended it')

# links and notifications from this user's programs
r = subprocess.run([guest, '--open-on-host', 'https://lordos.org/a b(c)'], env=env, capture_output=True, timeout=10)
link = None
for k, p in frames(3):
    if k == 25: link = p; break
check(r.returncode == 0 and link == b'https://lordos.org/a%20b%28c%29', 'a link went on, percent-encoded (%r, %r)' % (r.returncode, link))
r = subprocess.run([guest, '--notify-host', 'Build', 'done'], env=env, capture_output=True, timeout=10)
seen = [k for k, p in frames(1.5)]
check(r.returncode == 3 and 26 not in seen, 'a notification is refused while the host says no "notify" (%d)' % r.returncode)
r = subprocess.run([guest, '--open-on-host', 'file:///etc/passwd'], env=env, capture_output=True, timeout=10)
check(r.returncode == 2, 'a file: link is refused (%d)' % r.returncode)
st = os.stat(run + '/lordos-guest.sock')
check((st.st_mode & 0o777) == 0o600, 'the socket is 0600')
# the host says notify now
c.sendall(frame(1, b'lord-vm 2 clipboard links notify'))
time.sleep(0.3)
r = subprocess.run([guest, '--notify-host', 'Build \x07done', 'x' * 900], env=env, capture_output=True, timeout=10)
note = None
for k, p in frames(3):
    if k == 26: note = p; break
ok = note is not None and r.returncode == 0
if ok:
    tn = note[0] | note[1] << 8; t = note[2:2 + tn]; bn = note[2 + tn] | note[3 + tn] << 8
    ok = t == b'Build  done' and bn == 600
check(ok, 'a notification went on, its control character a space and its body cut to 600 bytes')
sys.exit(1 if fails else 0)
EOF
SOCK="$D/port"
python3 "$D/py/host.py" "$SOCK" "$(pwd)/build/lordos-guest" "$D/run" "$D/home" & L=$!
i=0; while [ ! -e "$SOCK.ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
XDG_RUNTIME_DIR="$D/run" HOME="$D/home" build/lordos-guest --port "$SOCK" --windows "$D/none.sock" 2>"$D/guest.log" & G=$!
wait $L; rc=$?
L=''
kill $G 2>/dev/null
[ $rc -eq 0 ] || { echo "--- lordos-guest's log"; cat "$D/guest.log"; exit 1; }
echo "guest-shell: ok"
