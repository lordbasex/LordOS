#!/bin/sh
# guest_perf_test.sh — lordos-guest's performance profile (tools/guest/perf.c, docs/VIRTUALIZATION.md 7.1.4) against
# real buses: two private dbus-daemons (a "system" one and a session one, each only on a socket in a temporary folder),
# the real dconf-service started by the session bus for a temporary home, and stand-ins for power-profiles-daemon and the
# desktop portal (tests/guest_perf_mock.py; the portal reads the real dconf value through GSettings). A Python listener
# plays lord-vm on a Unix socket (--port takes one for tests). Checked: the hello says "perf"; AGK_PERF 1 sets the
# performance power profile and turns the animations off (dconf's own database), AGK_PERFSTATE says so; AGK_PERF 0
# puts both back; the port going puts them back; SIGTERM puts them back; a run killed with the profile on is put back
# at the next start from its record; a part changed in between stays; a refused profile, the older bus name, the
# portal's older Read, a desktop that isn't GNOME. Nothing here touches this computer's own buses, settings or home.
# Run from server/ (tests/run_suites.sh does). Exit 77 when something it needs is missing, 1 on a failure.
cd "$(dirname "$0")/.." || exit 1
G=$(pwd)/build/lordos-guest
DCONF=/usr/libexec/dconf-service
[ -x "$G" ] && command -v python3 >/dev/null 2>&1 && command -v dbus-daemon >/dev/null 2>&1 && [ -x "$DCONF" ] &&
    command -v gsettings >/dev/null 2>&1 && python3 -c 'from gi.repository import Gio' 2>/dev/null &&
    gsettings list-keys org.gnome.desktop.interface 2>/dev/null | grep -qx enable-animations ||
    { echo "needs build/lordos-guest, python3 with gi, dbus-daemon, dconf-service and gsettings (org.gnome.desktop.interface)"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/lgp.XXXXXX") || exit 1
P=''
trap 'kill $P 2>/dev/null; rm -rf "$D"' EXIT
mkdir -p "$D/home/.config" "$D/run" "$D/services" "$D/bin" "$D/py"
chmod 700 "$D/run"
cp tests/guest_perf_mock.py "$D/py/"
# a bus that listens only on its socket and starts only dconf-service
for b in sys ses; do
    cat >"$D/$b.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:path=$D/$b.sock</listen><servicedir>$D/services</servicedir>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy></busconfig>
EOF
done
printf '[D-BUS Service]\nName=ca.desrt.dconf\nExec=%s\n' "$DCONF" >"$D/services/ca.desrt.dconf.service"
export HOME="$D/home" XDG_CONFIG_HOME="$D/home/.config" XDG_RUNTIME_DIR="$D/run" XDG_CACHE_HOME="$D/home/.cache"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$D/ses.sock" GSETTINGS_BACKEND=dconf
unset DISPLAY WAYLAND_DISPLAY DBUS_SYSTEM_BUS_ADDRESS
dbus-daemon --config-file="$D/sys.conf" --nofork --nopidfile >/dev/null 2>&1 & P="$P $!"
dbus-daemon --config-file="$D/ses.conf" --nofork --nopidfile >/dev/null 2>&1 & P="$P $!"
i=0; while { [ ! -S "$D/sys.sock" ] || [ ! -S "$D/ses.sock" ]; } && [ $i -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
mock() {
    python3 -I "$D/py/guest_perf_mock.py" "unix:path=$D/sys.sock" "unix:path=$D/ses.sock" "$D/mode" >"$D/mock.out" 2>&1 & M=$!
    i=0; while ! grep -q ready "$D/mock.out" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
}
: >"$D/mode"
M=''
mock
P="$P $M"
gsettings set org.gnome.desktop.interface enable-animations true || { echo "FAIL gsettings through the private bus"; exit 1; }

cat >"$D/py/host.py" <<'EOF'
# host.py: plays lord-vm and checks the guest's state. Steps on argv, see the cases below
import os, socket, struct, subprocess, sys, time, signal
from gi.repository import Gio, GLib
D, G = os.environ['T_DIR'], os.environ['T_GUEST']
fails = 0
def ok(c, what):
    global fails
    print(('ok   ' if c else 'FAIL ') + what, flush=True)
    if not c: fails += 1
sysc = Gio.DBusConnection.new_for_address_sync('unix:path=%s/sys.sock' % D,
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
def ppd_name():
    names = sysc.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'ListNames', None, None, 0, 2000, None).unpack()[0]
    return ('org.freedesktop.UPower.PowerProfiles', '/org/freedesktop/UPower/PowerProfiles') if 'org.freedesktop.UPower.PowerProfiles' in names \
        else ('net.hadess.PowerProfiles', '/net/hadess/PowerProfiles')
def profile():
    n, p = ppd_name()
    return sysc.call_sync(n, p, 'org.freedesktop.DBus.Properties', 'Get', GLib.Variant('(ss)', (n, 'ActiveProfile')), None, 0, 2000, None).unpack()[0]
def set_profile(v):
    n, p = ppd_name()
    sysc.call_sync(n, p, 'org.freedesktop.DBus.Properties', 'Set', GLib.Variant('(ssv)', (n, 'ActiveProfile', GLib.Variant('s', v))), None, 0, 2000, None)
def anim():
    return subprocess.run(['gsettings', 'get', 'org.gnome.desktop.interface', 'enable-animations'], capture_output=True, text=True).stdout.strip()
def set_anim(v):
    subprocess.run(['gsettings', 'set', 'org.gnome.desktop.interface', 'enable-animations', v], check=True)
def mode(s):
    with open(D + '/mode', 'w') as f: f.write(s)
REC = D + '/home/.local/state/lordos-guest/perf'
def record():
    try:
        with open(REC) as f: return f.read()
    except OSError: return None

def frame(kind, p): return struct.pack('<IHH', len(p), kind, 0) + p
class Host:
    def __init__(self, env=None):
        path = D + '/port.sock'
        if os.path.exists(path): os.unlink(path)
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.bind(path); self.s.listen(1); self.s.settimeout(10)
        e = dict(os.environ, PATH=D + '/bin', XDG_CURRENT_DESKTOP='GNOME')
        if env: e.update(env)
        self.log = open(D + '/guest.log', 'a')
        self.g = subprocess.Popen([G, '--port', path, '--windows', '', '--system-bus', D + '/sys.sock', '--session-bus', D + '/ses.sock'],
                                  env=e, stderr=self.log, stdout=self.log)
        self.c, _ = self.s.accept(); self.c.settimeout(15)
        self.buf = b''
        self.hello = self.wait(1)
    def wait(self, kind, t=15):
        end = time.time() + t
        while time.time() < end:
            while len(self.buf) >= 8:
                n, k, _ = struct.unpack('<IHH', self.buf[:8])
                if len(self.buf) < 8 + n: break
                p, self.buf = self.buf[8:8 + n], self.buf[8 + n:]
                if k == kind: return p
            try: d = self.c.recv(65536)
            except socket.timeout: return None
            if not d: return None
            self.buf += d
        return None
    def perf(self, on):
        self.c.sendall(frame(23, bytes([on])))
        p = self.wait(24)
        return tuple(p) if p else None
    def end(self, how='close'):
        if how == 'close': self.c.close(); self.s.close(); time.sleep(0.2)
        elif how == 'term': self.g.send_signal(signal.SIGTERM)
        elif how == 'kill': self.g.kill()
        if how != 'close':
            self.g.wait(10); self.c.close(); self.s.close()
    def stop(self):
        self.g.terminate(); self.g.wait(10); self.c.close(); self.s.close()

POWER, EFFECTS = 1, 2
if len(sys.argv) > 1 and sys.argv[1] == 'hadess':
    # only the older name (net.hadess.PowerProfiles)
    h = Host()
    ok(h.perf(1) == (1, POWER | EFFECTS, 0) and profile() == 'performance', 'net.hadess.PowerProfiles only: performance through it')
    ok(h.perf(0) == (0, POWER | EFFECTS, 0) and profile() == 'balanced', 'off through it')
    h.stop()
    sys.exit(1 if fails else 0)
# 1. on, off
h = Host()
ok(h.hello is not None and b' perf' in h.hello and h.hello.startswith(b'lordos-guest 9 '), 'hello says lordos-guest 9 ... perf (%r)' % h.hello)
ok(h.perf(1) == (1, POWER | EFFECTS, 0), 'AGK_PERF 1: power profile and animations applied')
ok(profile() == 'performance' and anim() == 'false', 'the guest: performance, animations false in dconf (%s, %s)' % (profile(), anim()))
ok(record() == 'lordos-guest perf 1\npower balanced\nanim 1\n', 'the record keeps balanced and animations on')
ok(h.perf(1) == (1, POWER | EFFECTS, 0), 'AGK_PERF 1 again: already on, nothing more')
ok(h.perf(0) == (0, POWER | EFFECTS, 0), 'AGK_PERF 0: both put back')
ok(profile() == 'balanced' and anim() == 'true' and record() is None, 'the guest: balanced, animations true, no record')
ok(h.perf(0) == (0, 0, 0), 'AGK_PERF 0 with nothing applied: nothing')
# 2. the port goes
ok(h.perf(1) == (1, POWER | EFFECTS, 0), 'on again')
h.end('close')
for _ in range(50):
    if profile() == 'balanced' and anim() == 'true': break
    time.sleep(0.1)
ok(profile() == 'balanced' and anim() == 'true' and record() is None, 'the port goes: put back')
h.stop()
# 3. SIGTERM
h = Host()
h.perf(1)
h.end('term')
ok(h.g.returncode == 0 and profile() == 'balanced' and anim() == 'true' and record() is None, 'SIGTERM: put back, exit 0')
# 4. killed with the profile on: put back at the next start
h = Host()
set_profile('power-saver')
ok(h.perf(1) == (1, POWER | EFFECTS, 0) and profile() == 'performance', 'on from power-saver')
h.end('kill')
ok(profile() == 'performance' and anim() == 'false' and record() is not None, 'killed: still on, the record is there')
h = Host()
ok(profile() == 'power-saver' and anim() == 'true' and record() is None, 'the next start put it back from the record (%s, %s)' % (profile(), anim()))
# 5. a part changed in between stays
ok(h.perf(1) == (1, POWER | EFFECTS, 0), 'on')
set_profile('balanced'); set_anim('true')
ok(h.perf(0) == (0, 0, 0) and profile() == 'balanced' and anim() == 'true', 'changed by the person in between: left alone, nothing put back')
set_profile('power-saver')
# 6. already at performance, animations already off: done, nothing kept
set_profile('performance'); set_anim('false')
ok(h.perf(1) == (1, POWER | EFFECTS, 0) and record() is None, 'already there: done, no record')
ok(h.perf(0) == (0, 0, 0) and profile() == 'performance' and anim() == 'false', 'off: nothing to put back')
set_profile('balanced'); set_anim('true')
# 7. refused
mode('refuse')
ok(h.perf(1) == (1, EFFECTS, POWER) and profile() == 'balanced' and anim() == 'false', 'a refused power profile: POWER refused, EFFECTS done')
mode('')
ok(h.perf(0) == (0, EFFECTS, 0) and anim() == 'true', 'off: the animations back')
# 8. the portal's older Read
mode('oldportal')
ok(h.perf(1) == (1, POWER | EFFECTS, 0) and anim() == 'false', 'the portal without ReadOne: Read (a variant in a variant)')
ok(h.perf(0) == (0, POWER | EFFECTS, 0) and anim() == 'true' and profile() == 'balanced', 'off')
mode('')
h.stop()
# 9. not GNOME
h = Host({'XDG_CURRENT_DESKTOP': 'KDE'})
ok(h.perf(1) == (1, POWER, EFFECTS) and anim() == 'true' and profile() == 'performance', 'KDE: the animations refused, the power profile done')
ok(h.perf(0) == (0, POWER, 0) and profile() == 'balanced', 'off')
h.stop()
# 10. no buses at all
h_env = {'XDG_CURRENT_DESKTOP': 'GNOME'}
os.rename(D + '/sys.sock', D + '/sys.sock.away'); os.rename(D + '/ses.sock', D + '/ses.sock.away')
h = Host(h_env)
ok(h.perf(1) == (1, 0, POWER | EFFECTS), 'no buses: both refused')
ok(h.perf(0) == (0, 0, 0) and record() is None, 'off: nothing')
h.stop()
os.rename(D + '/sys.sock.away', D + '/sys.sock'); os.rename(D + '/ses.sock.away', D + '/ses.sock')
sys.exit(1 if fails else 0)
EOF
T_DIR="$D" T_GUEST="$G" python3 -I "$D/py/host.py"; R=$?
# 11. only the older name (net.hadess.PowerProfiles): another stand-in
kill "$M" 2>/dev/null; wait "$M" 2>/dev/null
echo hadess >"$D/mode"
mock
P="$P $M"
T_DIR="$D" T_GUEST="$G" python3 -I "$D/py/host.py" hadess || R=1
[ $R = 0 ] || { echo "-- lordos-guest's log:"; cat "$D/guest.log"; }
exit $R
