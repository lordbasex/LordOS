#!/bin/sh
# vm_idle_headless.sh — an idle desktop with machines and none running makes no lord.vm call (docs/VIRTUALIZATION.md,
# "The shell and the machines"; the maintainer's rule of 2026-11-26). A headless lord-server runs the real Lord Flow theme
# over a throwaway library under a temporary HOME: 12 machine folders (UEFI on this computer's OVMF code and BIOS ones)
# and a Whonix-like group, nothing running, settings.json as after the first login (guestLauncherKept: []). A few lines
# before the bundle wrap every lord.vm and lord.vmcli function and count the calls by caller (the stack's first frame
# outside the wrapper) with their time. After the start's grace (STARTUP_S) nothing may call lord.vm for IDLE_S, and no
# "stall: … call vm." line may reach the log.
#
#   tests/vm_idle_headless.sh                          the check (`make test`)
#   SERVER=… THEME=… IDLE_S=90 MEASURE=1 tests/vm_idle_headless.sh
#                                                      counts only (an older build: its own lord-server and an unminified
#                                                      bundle, LORD_NOMINIFY=1, for readable callers); exit 0
#
# Exit 77 when something is missing.
cd "$(dirname "$0")/.." || exit 1
SERVER=${SERVER:-build/lord-server}
THEME=${THEME:-../themes/default/theme.bundle.js}
IDLE_S=${IDLE_S:-20}
STARTUP_S=${STARTUP_S:-12}
[ -x "$SERVER" ] || { echo "skipped: needs $SERVER"; exit 77; }
[ -f "$THEME" ] || { echo "skipped: needs $THEME (npm run build)"; exit 77; }
FW=/usr/share/OVMF/OVMF_CODE_4M.fd
[ -f "$FW" ] || FW=/dev/null
D=$(mktemp -d "${TMPDIR:-/tmp}/lvmidle.XXXXXX") || exit 1
P=
cleanup() { [ -n "$P" ] && { kill "$P" 2>/dev/null; wait "$P" 2>/dev/null; }; [ -n "$KEEP" ] || rm -rf "$D"; }
trap cleanup EXIT
chmod 700 "$D"
L="$D/home/Machines"
mkdir -p "$L" "$D/home/.config/lordos"
machine() {
    mkdir -p "$1"
    if [ "$4" = uefi ]; then fw="\"firmware\":\"uefi\",\"firmware_code\":\"$FW\",\"firmware_sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
    else fw='"firmware":"bios",'; fi
    printf '{"schema":1,"id":"%s","name":"%s","arch":"amd64","accel":"kvm","vcpus":2,"memory_mb":2048,"disk_gb":20,%s"machine_type":"pc-q35-10.0","display":"window","network":"shared","mac":"52:54:00:12:ab:%02x","created":"2026-11-04T20:00:00Z","updated":"2026-11-04T20:00:00Z"}\n' \
        "$2" "$3" "$fw" "$5" >"$1/machine.json"
}
i=0
while [ $i -lt 12 ]; do
    case $((i % 4)) in 0) c=1;; 1) c=a;; 2) c=2;; *) c=b;; esac
    kind=uefi; [ $i -ge 8 ] && kind=bios
    machine "$L/M$i.lordvm" "$(printf '%s%07d-3b4d-4e5f-8a6b-7c8d9e0f1a2b' $c $i)" "Machine $i" $kind $i
    i=$((i + 1))
done
G="$L/Whonix.lordgroup"
mkdir -p "$G"
cat >"$G/group.json" <<'EOF'
{"schema": 1, "id": "6f1c2b3a-1111-4222-8333-444455556666", "name": "Whonix", "kind": "whonix",
 "members": [{"role": "gateway", "dir": "Gateway.lordvm", "start": 1, "stop": 2},
             {"role": "workstation", "dir": "Workstation.lordvm", "start": 2, "stop": 1, "primary": true}],
 "disposable": false, "fresh_gateway": false}
EOF
machine "$G/Gateway.lordvm" "0b6c1a2e-1111-4222-8333-444455556666" Gateway bios 50
machine "$G/Workstation.lordvm" "0b6c1a2e-2222-4222-8333-444455556666" Workstation bios 51
printf '{"schema": 9, "desktopChosen": true, "language": "en", "guestLauncherKept": []}\n' >"$D/home/.config/lordos/settings.json"
cat >"$D/theme.js" <<EOF
(() => {
  const STARTUP_MS = $STARTUP_S * 1000;
  const t0 = Date.now();
  const all = {}, idle = {};
  const callerOf = () => {
    const at = String(new Error().stack || '').split('\n').filter((l) => l.includes(' at ')).slice(2);
    const f = at.find((l) => !/ at (Array\.|Object\.|apply|call)/.test(l)) || at[0] || '?';
    return f.trim().replace(/^at /, '').replace(/\(.*\/([^/]+):(\d+)(:\d+)?\)$/, '(\$1:\$2)');
  };
  const wrap = (ns, o) => {
    if (!o) return;
    for (const k of Object.getOwnPropertyNames(o)) {
      const f = o[k];
      if (typeof f !== 'function') continue;
      o[k] = function (...a) {
        const s = performance.now();
        try { return f.apply(this, a); } finally {
          const ms = performance.now() - s, key = \`\${ns}.\${k} <- \${callerOf()}\`;
          for (const c of Date.now() - t0 >= STARTUP_MS ? [all, idle] : [all]) {
            const e = c[key] || (c[key] = { n: 0, ms: 0, max: 0 });
            e.n++; e.ms += ms; if (ms > e.max) e.max = ms;
          }
        }
      };
    }
  };
  wrap('vm', globalThis.lord && lord.vm);
  wrap('vmcli', globalThis.lord && lord.vmcli);
  const dump = (tag, c) => { for (const [k, e] of Object.entries(c).sort((x, y) => y[1].n - x[1].n)) console.log(\`VMIDLE \${tag} \${e.n} calls, \${(e.ms / e.n).toFixed(3)} ms mean, \${e.max.toFixed(2)} ms max: \${k}\`); };
  setTimeout(() => { dump('all', all); dump('idle', idle); console.log('VMIDLE done'); }, STARTUP_MS + $IDLE_S * 1000);
})();
EOF
cat "$THEME" >>"$D/theme.js"
env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=C.UTF-8 \
    "$SERVER" --headless 1280x800 --theme "$D/theme.js" --timeout $((STARTUP_S + IDLE_S + 60)) >"$D/out.log" 2>&1 &
P=$!
LOGS="$D/out.log $D/home/.cache/lordos/shell.log $D/home/.cache/lordos/compositor.log"
for k in $(seq 1 $(((STARTUP_S + IDLE_S + 40) * 10))); do grep -aqs 'VMIDLE done' $LOGS && break; kill -0 $P 2>/dev/null || break; sleep 0.1; done
grep -aqs 'VMIDLE done' $LOGS || { echo "FAIL: the scene did not finish"; tail -20 $LOGS 2>/dev/null; exit 1; }
grep -ah 'VMIDLE \(all\|idle\)' $LOGS | sed 's/.*VMIDLE /  /' | sort -u
[ -n "$MEASURE" ] && { grep -ah 'stall: .*call vm\.' $LOGS | sed 's/^/  /' | head -5; exit 0; }
R=0
if grep -ah 'VMIDLE idle' $LOGS | grep -q ' vm\.'; then echo "FAIL: lord.vm called while idle with nothing running"; R=1; fi
if grep -aqs 'stall: .*call vm\.' $LOGS; then echo "FAIL: stall lines:"; grep -ah 'stall: .*call vm\.' $LOGS | head -5; R=1; fi
[ $R -eq 0 ] && echo "ok"
exit $R
