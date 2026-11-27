#!/bin/sh
# testbench_selection_headless.sh — the test bench's clipboard and primary-selection checks (src/testbench/checks.c)
# against a headless build/lord-server-test while ANOTHER client holds both selections (a first lordos-testbench --auto
# still running), as on a real desktop where the person's last selected text is someone else's. The second test bench
# must read back its own text: before the fix it took the selection it was told at once (the first one's, or a source
# that no longer answers) for its own, "OFFER CAME, NO TEXT" (BUG-24 of the ws170 QA). Run from server/ (tests/run_suites.sh
# does). Exit 77 when something it needs is missing, 1 on a failure.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lord-server-test ] || { echo "skipped: needs build/lord-server-test"; exit 77; }
[ -x build/lordos-testbench ] || { echo "skipped: needs build/lordos-testbench"; exit 77; }
[ -f ../themes/default/theme.bundle.js ] || { echo "skipped: needs the theme bundle (npm run build)"; exit 77; }
command -v python3 >/dev/null 2>&1 || { echo "skipped: needs python3"; exit 77; }
P='' D='' F=''
stop() { [ -n "$F" ] && kill $F 2>/dev/null; [ -n "$P" ] && { kill $P 2>/dev/null; sleep 0.3; kill -9 $P 2>/dev/null; }; [ -n "$D" ] && rm -rf "$D"; P='' D='' F=''; }
trap stop EXIT
D=$(mktemp -d "${TMPDIR:-/tmp}/ltbs.XXXXXX") || exit 1
chmod 700 "$D"
mkdir -p "$D/home/.config/lordos"
printf '{"schema": 9, "desktopChosen": true, "language": "en", "welcomeShown": true}\n' >"$D/home/.config/lordos/settings.json"
env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u DISPLAY -u XAUTHORITY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LORD_HEADLESS_DRAW=1 LANG=en_US.UTF-8 \
    ./build/lord-server-test --headless 1280x800 --theme ../themes/default/theme.bundle.js --timeout 90 --test-hooks >"$D/server.log" 2>&1 &
P=$!
i=0 sock=''
while [ $i -lt 120 ] && [ -z "$sock" ]; do
    sleep 0.25; i=$((i + 1))
    for s in "$D"/wayland-[0-9]*; do case $s in *.lock) ;; *) [ -S "$s" ] && sock=$(basename "$s");; esac; done
done
[ -n "$sock" ] || { echo "FAIL: lord-server made no Wayland socket"; tail -n 30 "$D/server.log"; exit 1; }
sleep 2
tb() { env -u DISPLAY -u WAYLAND_SOCKET XDG_RUNTIME_DIR="$D" HOME="$D/home" WAYLAND_DISPLAY="$sock" timeout 40 ./build/lordos-testbench --auto "$1"; }
tb 10 >"$D/first.json" 2>/dev/null &
F=$!
sleep 3
tb 3 >"$D/second.json" 2>"$D/second.err"
python3 - "$D/second.json" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception as e:
    print('FAIL: no report from the second test bench:', e); sys.exit(1)
rows = {c['id']: c for c in d.get('checks', [])}
bad = 0
for k in ('clipboard round trip', 'primary selection'):
    c = rows.get(k)
    print(f"{k}: {c['state'] if c else 'missing'} ({c.get('detail', '') if c else ''})")
    if not c or c['state'] != 'pass':
        bad += 1
sys.exit(1 if bad else 0)
PY
R=$?
[ $R -eq 0 ] && echo "ok"
exit $R
