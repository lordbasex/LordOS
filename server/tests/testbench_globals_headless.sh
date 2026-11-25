#!/bin/sh
# testbench_globals_headless.sh — `lordos-testbench --globals` against a headless build/lord-server-test (--test-hooks, CPU
# path, a private runtime folder and home; never a live session). Wants every global of LordOS's protocol table
# (src/testbench/expected_globals.h, from docs/wayland-protocols.json) offered at its version:
#   - an ordinary client, with the test hooks' fake VR headset plugged in: everything but xwayland_shell_v1 (Xwayland's
#     only), and wp_linux_drm_syncobj_manager_v1 only on a machine with a render node that has timeline syncobjs;
#   - the test hooks' Xwayland-like connection (WAYLAND_SOCKET): xwayland_shell_v1 too.
# Together: all of them, none older or missing. Run from server/ (tests/run_suites.sh does). Exit 77 when something it
# needs is missing, 1 on a failure. TBG_DEBUG=1 shows the reports.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lord-server-test ] || { echo "skipped: needs build/lord-server-test"; exit 77; }
[ -x build/lordos-testbench ] || { echo "skipped: needs build/lordos-testbench"; exit 77; }
[ -f ../themes/default/theme.bundle.js ] || { echo "skipped: needs the theme bundle (npm run build)"; exit 77; }
command -v python3 >/dev/null 2>&1 || { echo "skipped: needs python3"; exit 77; }
P='' D=''
stop() { [ -n "$P" ] && { kill $P 2>/dev/null; sleep 0.3; kill -9 $P 2>/dev/null; }; [ -n "$D" ] && rm -rf "$D"; P='' D=''; }
trap stop EXIT
D=$(mktemp -d "${TMPDIR:-/tmp}/ltbg.XXXXXX") || exit 1
chmod 700 "$D"
mkdir -p "$D/home/.config/lordos"
printf '{"schema": 9, "desktopChosen": true, "language": "en", "welcomeShown": true}\n' >"$D/home/.config/lordos/settings.json"
env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u DISPLAY -u XAUTHORITY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LORD_HEADLESS_DRAW=1 LANG=en_US.UTF-8 \
    ./build/lord-server-test --headless 1280x800 --theme ../themes/default/theme.bundle.js --timeout 90 --user "$(id -un)" --test-hooks >"$D/server.log" 2>&1 &
P=$!
i=0 sock=''
while [ $i -lt 120 ] && [ -z "$sock" ]; do
    sleep 0.25; i=$((i + 1))
    for s in "$D"/wayland-[0-9]*; do case $s in *.lock) ;; *) [ -S "$s" ] && sock=$(basename "$s");; esac; done
done
[ -n "$sock" ] || { echo "FAIL: lord-server made no Wayland socket"; tail -n 30 "$D/server.log"; exit 1; }
i=0
while [ $i -lt 40 ] && [ ! -S "$D/lordos-test-hooks.sock" ]; do sleep 0.25; i=$((i + 1)); done
sleep 1
python3 - "$D" "$sock" <<'EOF'
import array, json, os, socket, subprocess, sys
d, sock = sys.argv[1], sys.argv[2]
h = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
h.connect(os.path.join(d, 'lordos-test-hooks.sock'))
h.settimeout(10)
def hook(cmd, fds=False):
    h.send(cmd.encode())
    if not fds:
        return h.recv(256).decode(), None
    msg, anc, _, _ = h.recvmsg(256, socket.CMSG_SPACE(4))
    fd = None
    for lvl, typ, data in anc:
        if lvl == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
            fd = array.array('i', data[:4])[0]
    return msg.decode(), fd
env = {k: v for k, v in os.environ.items() if k not in ('WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'DISPLAY')}
env.update(XDG_RUNTIME_DIR=d, HOME=os.path.join(d, 'home'))
r, _ = hook('lease plug')
print('hook lease plug:', r)
def run(extra, pass_fds=()):
    e = dict(env, **extra)
    p = subprocess.run(['build/lordos-testbench', '--globals'], env=e, capture_output=True, text=True, timeout=20, pass_fds=pass_fds)
    if p.returncode != 0:
        print('FAIL: lordos-testbench --globals exited', p.returncode, p.stderr.strip()); sys.exit(1)
    j = json.loads(p.stdout)
    if os.environ.get('TBG_DEBUG'): print(json.dumps(j['table'], indent=1))
    return j['table']
plain = run({'WAYLAND_DISPLAY': sock})
r, fd = hook('xwl', fds=True)
print('hook xwl:', r, 'fd' if fd is not None else 'no fd')
if fd is None:
    print('FAIL: the xwl hook gave no connection'); sys.exit(1)
os.set_inheritable(fd, True)
xwl = run({'WAYLAND_SOCKET': str(fd)}, pass_fds=(fd,))
bad = 0
rows = {}
for t, who in ((plain, 'ordinary client'), (xwl, 'Xwayland connection')):
    print(f"{who}: {t['offered']} of {t['expected']} offered, {t['newer']} newer, {t['older']} older, {t['missing']} missing, {t['notHere']} only to others; extra {t['extra']}")
    for row in t['rows']:
        if row['state'] in ('older', 'missing'):
            print(f"FAIL: {who}: {row['interface']} {row['state']} (advertised {row['advertised']}, expected {row['expected']})"); bad += 1
        if row['state'] in ('offered', 'newer'):
            rows[row['interface']] = row
        if row['state'] == 'newer':
            print(f"FAIL: {who}: {row['interface']} advertised at {row['advertised']}, the table says {row['expected']}: update docs/wayland-protocols.json"); bad += 1
    if t['extra']:
        print(f"FAIL: {who}: globals the table doesn't list: {t['extra']}"); bad += 1
expected = plain['expected']
xs = next(r for r in plain['rows'] if r['interface'] == 'xwayland_shell_v1')
if xs['state'] != 'not-here':
    print('FAIL: an ordinary client sees xwayland_shell_v1'); bad += 1
if 'wp_drm_lease_device_v1' not in rows:
    print('FAIL: the plugged headset did not bring wp_drm_lease_device_v1'); bad += 1
missing = [r['interface'] for r in plain['rows'] if r['interface'] not in rows]
syncobj_only = missing == ['wp_linux_drm_syncobj_manager_v1']
if missing and not syncobj_only:
    print('FAIL: never offered:', missing); bad += 1
elif syncobj_only:
    print('note: no render node with timeline syncobjs here, so explicit sync is not offered (as lord-server should)')
print(f"together: {len(rows)} of {expected} offered")
sys.exit(1 if bad else 0)
EOF
R=$?
[ $R = 0 ] && echo "testbench_globals_headless: ok"
exit $R
