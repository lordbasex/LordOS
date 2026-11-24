#!/bin/bash
# sentinela_vm_live.sh — Sentinela's watch of the machines (docs/SENTINELA.md "Machines") on a live machine, on screen: a
# headless lord-server (its own user and mount namespaces, a private home, an empty folder over the real home, a tmpfs
# over the real runtime folder, as tests/vm_group_link.sh) with Sentinela on; a real KVM machine (a Debian 13 nocloud
# disk, copied) started with `machines start --no-window`. A driver after the theme bundle logs what lord.vm.stats gives
# Sentinela for it (verified by lord-server over lord-vm's socket: sandbox bits, the kernel's seccomp and no_new_privs,
# the scope), waits for Sentinela's tick, and takes screenshots of the alerts (Settings → AI → Sentinela). Headless has no
# session manager, so the machine's QEMU isn't in an app scope of a session: the "always" rule vmScope must fire, live,
# for that machine, and vmSandbox must not (its sandbox is whole).
#   tests/sentinela_vm_live.sh DEBIAN_NOCLOUD_QCOW2 OUT_DIR       (from server/, after make and npm run build; KVM host)
# A test tool: not part of `make test`. Exit 77 without KVM, QEMU, unshare or the image.
# First run, 2026-11-24 (amd64, KVM): it FAILS, and that is a real finding. lord-vm is non-dumpable since QEMU's namespaces
# (2026-11-23), so lord-server can't read /proc/<lord-vm>/exe (a ptrace-mode check); vm_exe_is is false, the machine is
# never "verified", and Sentinela raises vmSandbox ("proof that its engine is the machine's own") on every live machine.
# The fix needs a decision on what attests lord-vm (docs/SENTINELA.md "Machines", open item).
cd "$(dirname "$0")/.." || exit 1
IMG=${1:?usage: tests/sentinela_vm_live.sh DEBIAN_NOCLOUD_QCOW2 OUT_DIR}
OUT=${2:?usage}
ROOT=$(cd .. && pwd)
[ -x build/lord-server ] && [ -x build/machines ] && [ -x build/lord-vm ] && [ -f "$ROOT/themes/default/theme.bundle.js" ] || { echo "skipped: needs build/lord-server, machines, lord-vm and the theme"; exit 77; }
[ -r /dev/kvm ] && [ -f "$IMG" ] && command -v qemu-system-x86_64 >/dev/null && command -v unshare >/dev/null || { echo "skipped: needs KVM, QEMU, unshare and the image"; exit 77; }
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
D=$(mktemp -d "${TMPDIR:-/var/tmp}/lsentvm.XXXXXX") || exit 1
chmod 700 "$D"
mkdir -p "$D/home/Machines" "$D/home/.config/lordos" "$D/run" "$D/fake"
chmod 700 "$D/run" "$D/home"
REALHOME=$(getent passwd "$(id -u)" | cut -d: -f6)
RT=/run/user/$(id -u)
E="env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR=$D/run HOME=$D/home LANG=C.UTF-8"
NS="unshare -Urm sh -c 'mount --bind $D/fake $REALHOME && { [ -d $RT ] && mount -t tmpfs -o mode=700 tmpfs $RT || true; } && exec unshare --user --map-user=$(id -u) --map-group=$(id -g) \"\$0\" \"\$@\"'"
ns() { eval "$NS" '"$@"'; }
M="$D/home/Machines/Live.lordvm"
T="$ROOT/themes/zsentvm/default"
R=0
ok() { echo "ok: $*"; }
bad() { echo "FAIL: $*"; R=1; }
cleanup() {
    ns $E ./build/lord-vm --poweroff "$M" >/dev/null 2>&1
    [ -n "${P:-}" ] && kill "$P" 2>/dev/null
    rm -rf "$D" "$ROOT/themes/zsentvm"
    node "$ROOT/tools/code-manifest.mjs" >/dev/null
}
trap cleanup EXIT
cp "$IMG" "$D/disk.qcow2"
ns $E ./build/lord-vm --create "$M" --name Live --import "$D/disk.qcow2" --vcpus 2 --memory 1024 >"$D/create.log" 2>&1 && ok "machine made" || { bad "create: $(cat "$D/create.log")"; exit 1; }
printf '{"schema": 9, "desktopChosen": true, "language": "en", "mode": "dark", "sentinela": {"on": true}}\n' >"$D/home/.config/lordos/settings.json"
printf '{ "version": 1, "apps": { "screenshots": { "screen": { "state": "allow", "when": 1 } } } }\n' >"$D/home/.config/lordos/permisos.json"
rm -rf "$ROOT/themes/zsentvm"; mkdir -p "$ROOT/themes/zsentvm"; cp -r "$ROOT/themes/default" "$T"
cat "$ROOT/themes/default/theme.bundle.js" - >"$T/theme.bundle.js" <<'EOF'
;(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const log = (s) => lord.log(`SENT-LIVE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  let n = 0;
  const shot = async (name) => {
    n++; await sleep(1200);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  (async () => {
    let m = null;
    for (let i = 0; i < 240 && !m; i++) { await sleep(1000); m = (lord.vm.list() || []).find((x) => x.name === 'Live' && x.state === 'running'); }
    if (!m) { log('machine never ran'); log('done'); return; }
    log('machine running');
    await sleep(3000);
    const st = lord.vm.stats(m.dir) || {};
    // only what Sentinela reads: no address, no file of the guest
    log(`stats verified=${st.verified} sandbox=${st.sandbox} seccomp=${st.seccomp} noNewPrivs=${st.noNewPrivs} scope=${st.scope} cgMode=${st.cgMode} passt=${st.passt}`);
    let alerts = [];
    for (let i = 0; i < 60 && !alerts.some((a) => a.machine || /^vm/.test(a.rule || '')); i++) { await sleep(1000); alerts = sdk.sentinelaAlerts.get() || []; }
    log(`alerts ${JSON.stringify(alerts.map((a) => ({ rule: a.rule, name: a.name })))}`);
    await shot('notice');
    sdk.wm.open('settings', { args: { section: 'ai', page: 'sentinela', nonce: Date.now() } });
    await sleep(2500);
    await shot('sentinela');
    log('done');
  })();
})();
EOF
node tests/perf/manifest.mjs "$ROOT" "themes/zsentvm/default/theme.bundle.js" || exit 1
ns $E LORD_GPU=0 LORD_HEADLESS_DRAW=1 ./build/lord-server --headless 1600x900 --theme "$T/theme.bundle.js" --timeout 600 >"$D/server.log" 2>&1 &
P=$!
for i in $(seq 1 100); do [ -S "$D/run/lordos-machines.sock" ] && break; sleep 0.2; done
sleep 2
out=$(ns $E ./build/machines start Live --no-window 2>&1) && ok "machines start Live --no-window: $out" || bad "start: $out"
for i in $(seq 1 300); do grep -aq 'SENT-LIVE done' "$D/home/.cache/lordos/shell.log" 2>/dev/null && break; sleep 0.5; done
grep -ah 'SENT-LIVE' "$D/home/.cache/lordos/shell.log" | sed 's/.*SENT-LIVE/ /'
L=$(grep -ah 'SENT-LIVE' "$D/home/.cache/lordos/shell.log")
echo "$L" | grep -q "stats verified=true" && ok "lord-server verified the machine's engine (SO_PEERCRED, the installed lord-vm, its own QEMU)" || bad "stats not verified"
echo "$L" | grep -q "seccomp=2 noNewPrivs=true" && ok "the kernel's seccomp and no_new_privs on QEMU" || bad "seccomp/no_new_privs as read: $(echo "$L" | grep stats)"
echo "$L" | grep -q '"rule":"vmScope"' && ok "vmScope fired live (no session scope in a headless test)" || bad "no vmScope alert"
echo "$L" | grep -q '"rule":"vmSandbox"' && bad "vmSandbox fired on a whole sandbox" || ok "no vmSandbox alert: the sandbox is whole"
for k in 1 2; do [ -f "$D/home/shot-$k.png" ] && cp "$D/home/shot-$k.png" "$OUT/sentinela-vm-live-$k.png"; done
ls "$OUT"/sentinela-vm-live-*.png 2>/dev/null
grep -ai 'TypeError\|ReferenceError' "$D/home/.cache/lordos/shell.log" | tail -5
exit $R
