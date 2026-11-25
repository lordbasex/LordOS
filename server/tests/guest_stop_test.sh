#!/bin/sh
# guest_stop_test.sh — `lordos-guest --stopping VERB` (tools/guest/lordos-guest.c), what systemd-shutdown's hook runs in a
# guest just before its last step (docs/VIRTUALIZATION.md "Graphics 3D", QEMU issue 4918): one AGK_STOPPING frame
# (vmagent.h, kind 22, one byte: 1 power off, 2 restart) on the agent's port, then lord-vm's echo for at most 1.5 s. A
# Python listener plays lord-vm on a Unix socket (--port takes one for tests): it records the frames and echoes, stays
# silent, or sends another frame first. Also: no port ends at once, a verb that isn't one is refused. Run from server/
# (tests/run_suites.sh does). Exit 77 when build/lordos-guest or python3 is missing, 1 on a failure.
# Not tested here: the real virtio-serial port, systemd-shutdown running the hook, the hello's "stopping" (it looks for
# the hook under /usr/lib/systemd/system-shutdown).
cd "$(dirname "$0")/.." || exit 1
[ -x build/lordos-guest ] && command -v python3 >/dev/null 2>&1 || { echo "needs build/lordos-guest and python3"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lgs.XXXXXX") || exit 1
L=''
trap 'kill $L 2>/dev/null; rm -rf "$D"' EXIT
G=$(pwd)/build/lordos-guest
mkdir -p "$D/py"
# listen.py SOCK OUT MODE: one connection; MODE echo (the frame back), silent, other (a hello frame, then the echo),
# wrong (an echo of the other mode only). OUT gets "KIND <n> <payload hex>" per frame received
cat >"$D/py/listen.py" <<'EOF'
import socket, struct, sys
path, out, mode = sys.argv[1], sys.argv[2], sys.argv[3]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path)
s.listen(1)
s.settimeout(10)
c, _ = s.accept()
c.settimeout(10)
buf, lines = b'', []
def frame(kind, p):
    return struct.pack('<IHH', len(p), kind, 0) + p
while True:
    try:
        d = c.recv(65536)
    except socket.timeout:
        break
    if not d:
        break
    buf += d
    while len(buf) >= 8:
        n, kind, _ = struct.unpack('<IHH', buf[:8])
        if len(buf) < 8 + n:
            break
        p, buf = buf[8:8 + n], buf[8 + n:]
        lines.append('KIND %d %s' % (kind, p.hex()))
        if kind == 22:
            if mode == 'other':
                c.sendall(frame(1, b'lord-vm 2 clipboard'))
            if mode in ('echo', 'other'):
                c.sendall(frame(22, p))
            if mode == 'wrong':
                c.sendall(frame(22, bytes([3 - p[0]])))
with open(out, 'w') as f:
    f.write('\n'.join(lines) + '\n')
EOF
R=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; R=1; }
ms() { python3 -c 'import time; print(int(time.monotonic() * 1000))'; }
# case_run NAME VERB MODE WANT-PAYLOAD MIN-MS MAX-MS
case_run() {
    name=$1 verb=$2 mode=$3 want=$4 lo=$5 hi=$6
    rm -f "$D/sock" "$D/out"
    python3 -I "$D/py/listen.py" "$D/sock" "$D/out" "$mode" &
    L=$!
    i=0; while [ ! -S "$D/sock" ] && [ $i -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
    t0=$(ms)
    "$G" --port "$D/sock" --stopping "$verb"; rc=$?
    t=$(( $(ms) - t0 ))
    wait $L; L=''
    got=$(cat "$D/out" 2>/dev/null)
    if [ $rc = 0 ] && [ "$got" = "KIND 22 $want" ] && [ $t -ge $lo ] && [ $t -le $hi ]; then ok "$name (${t} ms)"
    else bad "$name: rc $rc, frames [$got], ${t} ms (want KIND 22 $want, $lo..$hi ms)"; fi
}
case_run "reboot: AGK_STOPPING 2, ends at the echo" reboot echo 02 0 1000
case_run "poweroff: AGK_STOPPING 1, ends at the echo" poweroff echo 01 0 1000
case_run "halt is a power off" halt echo 01 0 1000
case_run "kexec is a restart" kexec echo 02 0 1000
case_run "another frame before the echo is skipped" reboot other 02 0 1000
case_run "no echo: gives up after 1.5 s" poweroff silent 01 1400 3000
case_run "an echo of the other mode isn't the answer" reboot wrong 02 1400 3000
# no port at all (a computer that isn't a Machines guest): ends at once, nothing to say
t0=$(ms); "$G" --port "$D/none/port" --stopping reboot; rc=$?; t=$(( $(ms) - t0 ))
if [ $rc = 0 ] && [ $t -lt 500 ]; then ok "no port: ends at once (${t} ms)"; else bad "no port: rc $rc, ${t} ms"; fi
# a verb systemd-shutdown never passes: refused, nothing opened
rm -f "$D/sock"
"$G" --port "$D/sock" --stopping suspend 2>/dev/null; rc=$?
if [ $rc = 2 ]; then ok "an unknown verb is refused"; else bad "an unknown verb: rc $rc"; fi
"$G" --stopping 2>/dev/null; rc=$?
if [ $rc = 2 ]; then ok "--stopping without a verb is refused"; else bad "--stopping without a verb: rc $rc"; fi
exit $R
