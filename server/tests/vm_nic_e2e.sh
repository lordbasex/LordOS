#!/bin/bash
# vm_nic_e2e.sh — every network card model a machine can have (machine.json nic_model, `lord-vm --set --nic-model`; docs/
# VIRTUALIZATION.md 3.9, 6.22) and an import's extra cards, booted for real (not in `make test`: it needs KVM, QEMU and a
# Linux guest image with qemu-guest-agent, VM_NIC_IMAGE, copied). For each model the guest must see one card of that
# driver with the machine's MAC, get its address from passt's DHCP and reach the internet (Shared network); with two extra
# cards (an import's extra_nics) it must see them too, with their MACs and models, and have no address on them (present,
# not connected, as 6.22 says). Exit 77 without what it needs.
cd "$(dirname "$0")/.." || exit 1
B=$(pwd)/build
[ -x "$B/lord-vm" ] && [ -x "$B/lordvm-dev" ] && [ -f "${VM_NIC_IMAGE:-}" ] && [ -r /dev/kvm ] || { echo "skipped: needs build/lord-vm, build/lordvm-dev, KVM and VM_NIC_IMAGE"; exit 77; }
D=$(mktemp -d /var/tmp/lordos-nice2e.XXXXXX) || exit 1
mkdir -p "$D/run" && chmod 700 "$D/run"
M="$D/NicE2E.lordvm"
R=0
ok() { echo "ok: $*"; }
bad() { echo "FAIL: $*"; R=1; }
trap 'XDG_RUNTIME_DIR="$D/run" "$B/lord-vm" --poweroff "$M" >/dev/null 2>&1; [ -n "$KEEP" ] || rm -rf "$D"' EXIT
"$B/lord-vm" --create "$M" --name NicE2E --arch amd64 --vcpus 2 --memory 1536 --display none --import "$VM_NIC_IMAGE" >"$D/create.log" 2>&1 || { cat "$D/create.log"; exit 1; }
"$B/lord-vm" --set "$M" --agent on --transfer off --usb off >/dev/null || exit 1
cat > "$D/drive.py" <<'PY'
import base64, json, re, subprocess, sys, time
log, argv = sys.argv[1], sys.argv[sys.argv.index('--') + 1:]
cmd = ("for n in /sys/class/net/*; do b=${n##*/}; [ $b = lo ] && continue; "
       "echo CARD $b $(cat $n/address) $(basename $(readlink $n/device/driver)) $(ip -o -4 addr show dev $b scope global | awk '{print $4}' | head -1); done; "
       "timeout 10 bash -c 'exec 3<>/dev/tcp/deb.debian.org/80' && echo NET-OK || echo NET-NO")
q = json.dumps({'execute': 'guest-exec', 'arguments': {'path': '/bin/sh', 'arg': ['-c', cmd], 'capture-output': True}}, separators=(',', ':'))
open(log + '.q', 'w').write(q)
p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
import threading
lines, lf = [], open(log, 'w')
def rd():
    for l in p.stdout:
        l = l.decode(errors='replace'); lines.append(l); lf.write(l); lf.flush()
threading.Thread(target=rd, daemon=True).start()
def say(c): lf.write('>> ' + c + '\n'); lf.flush(); p.stdin.write((c + '\n').encode()); p.stdin.flush()
end = time.time() + 60
while time.time() < end and not any('"state":"paused"' in l or '"state":"running"' in l for l in lines): time.sleep(0.2)
say('resume')
end = time.time() + 90
while time.time() < end and not any('"ev":"agent","state":2' in l for l in lines): time.sleep(0.5)
time.sleep(8)                                   # DHCP on every card
say('qmpf ' + log + '.q')
end = time.time() + 10
while time.time() < end and not any(re.search(r'"text":"pid \d+"', l) for l in lines): time.sleep(0.2)
pids = [m.group(1) for l in lines for m in [re.search(r'"text":"pid (\d+)"', l)] if m]
for _ in range(10):
    time.sleep(2); n = len(lines)
    if pids: say('qmp {"execute":"guest-exec-status","arguments":{"pid":%s}}' % pids[-1])
    time.sleep(1)
    if any('"text":"exited 1' in l for l in lines[n:]): break
say('poweroff'); time.sleep(5); p.stdin.close()
try: p.wait(timeout=60)
except Exception: p.kill()
PY
MAC=$(sed -n 's/.*"mac": "\(.*\)".*/\1/p' "$M/machine.json" | head -1)
DRV() { case $1 in virtio) echo virtio_net;; e1000e) echo e1000e;; e1000|e1000-82545em|e1000-82544gc) echo e1000;; vmxnet3) echo vmxnet3;; rtl8139) echo 8139cp;; pcnet) echo pcnet32;; esac; }
boot() { XDG_RUNTIME_DIR="$D/run" timeout 200 python3 -I "$D/drive.py" "$D/$1.log" -- "$B/lordvm-dev" run "$M" --dev-qmp >/dev/null 2>&1; grep -o 'dev-out: .*' "$D/$1.log" | sed 's/^dev-out: //' | tr '|' '\n'; }
for model in ${VM_NIC_MODELS:-virtio e1000e e1000 e1000-82545em e1000-82544gc rtl8139 pcnet vmxnet3}; do
    "$B/lord-vm" --set "$M" --nic-model "$model" >/dev/null || { bad "--nic-model $model refused"; continue; }
    o=$(boot "$model")
    card=$(echo "$o" | grep "^CARD .* $MAC ")
    drv=$(echo "$card" | awk '{print $4}'); addr=$(echo "$card" | awk '{print $5}')
    want=$(DRV "$model")
    # rtl8139: Linux takes it with 8139cp or 8139too, whichever loads first
    if { [ "$drv" = "$want" ] || { [ "$model" = rtl8139 ] && [ "$drv" = 8139too ]; }; } && [ -n "$addr" ]; then
        if echo "$o" | grep -q NET-OK; then ok "$model: the guest's $drv card, $MAC, $addr from passt, the internet reached"
        # measured 2026-11-24: QEMU 10.0's PCnet with Linux 6.12's pcnet32 takes passt's DHCP answer, then only receive errors
        # (also with passt's MTU at 1500): known, said in docs/VIRTUALIZATION.md 6.22, not a failure of Machines'
        elif [ "$model" = pcnet ]; then echo "note: pcnet: the guest's $drv card, $MAC, $addr from passt, then no traffic (QEMU's PCnet with Linux's pcnet32)"
        else bad "$model: no internet [$(echo "$o" | tr '\n' ' ' | cut -c1-200)]"; fi
    else bad "$model: [$(echo "$o" | tr '\n' ' ' | cut -c1-200)]"; fi
done
# an import's extra cards (extra_nics): present with their MAC and model, not connected
"$B/lord-vm" --set "$M" --nic-model virtio >/dev/null
python3 -I - "$M/machine.json" <<'PY'
import json, sys
p = sys.argv[1]; m = json.load(open(p))
m['schema'] = max(m['schema'], 3)
m['extra_nics'] = [{'mac': '52:54:00:ab:cd:01', 'model': 'e1000e'}, {'mac': '52:54:00:ab:cd:02', 'model': 'rtl8139'}]
json.dump(m, open(p, 'w'), indent=2)
PY
o=$(boot extra)
echo "$o" | grep -q "^CARD .* $MAC virtio_net [0-9]" && ok "extra cards: the first card is still on the network" || bad "extra: first card [$(echo "$o" | tr '\n' ' ' | cut -c1-200)]"
for x in "52:54:00:ab:cd:01 e1000e" "52:54:00:ab:cd:02 8139"; do
    set -- $x
    line=$(echo "$o" | grep "^CARD .* $1 ")
    echo "$line" | grep -q " $2" && [ "$(echo "$line" | awk '{print NF}')" = 4 ] && ok "extra card $1 ($2) is there, with no address (not connected)" || bad "extra card $1: [$line]"
done
[ $R = 0 ] && echo "vm_nic_e2e: all passed" || echo "vm_nic_e2e: FAILED (logs in $D with KEEP=1)"
exit $R
