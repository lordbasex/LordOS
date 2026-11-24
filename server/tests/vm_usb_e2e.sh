#!/bin/bash
# vm_usb_e2e.sh — a real host USB device into a running machine, end to end (docs/VIRTUALIZATION.md 6.6; AGENTS.md
# "Machines' USB devices"). Not in `make test`: it needs root (sudo -n), KVM, QEMU, a USB device nobody uses, a Linux guest
# image with qemu-guest-agent, and a LordOS install's /usr/lib/lordos/lib folder (where lord-vm-usb.so goes; the test
# mounts its own copy over it in a private mount namespace, the system's files never change).
#
#   VM_USB_DEV="BUS DEV VVVV:PPPP"   the device (a webcam for the camera checks: VM_USB_CAMERA=1)
#   VM_USB_HUB="BUS DEV VVVV:PPPP"   a hub of this computer (only asked for: it must be refused)
#   VM_USB_IMAGE=path.qcow2          the guest (copied; with VM_USB_CAMERA=1 it needs v4l2-ctl or apt reaching the internet)
#
# Part A (build/vm-usb-cli as the admin helper, build/lordvm-dev as lord-server): vm-usb-allow opens the node, lord-vm gives
# it to QEMU through an fdset, the guest sees the device by its name (and with VM_USB_CAMERA=1 captures 30 frames of
# 640x480 from it), out again, and this computer's drivers take it back. Part B (a lord-server started as root, in a
# private mount namespace with a scratch home over the real one and a tmpfs over the runtime folder, running
# tests/vm_usb_e2e.js as its theme): lord.vm.usbDevices, refusals by lord-server and by the admin helper, the camera and
# microphone permissions, the descriptor never left in lord-server or the helper (only QEMU holds the node), usbAuto
# while running, detach, the rule at the next start, a permission taken away. Exit 77 without what it needs.
cd "$(dirname "$0")/.." || exit 1
B=$(pwd)/build
for f in lord-vm lordvm-dev vm-usb-cli lord-server lord-vm-usb.so; do [ -e "$B/$f" ] || { echo "skipped: needs build/$f (make build/$f)"; exit 77; }; done
[ -n "${VM_USB_DEV:-}" ] && [ -n "${VM_USB_HUB:-}" ] && [ -f "${VM_USB_IMAGE:-}" ] || { echo "skipped: needs VM_USB_DEV, VM_USB_HUB and VM_USB_IMAGE"; exit 77; }
sudo -n true 2>/dev/null && [ -r /dev/kvm ] && command -v python3 >/dev/null && command -v unshare >/dev/null || { echo "skipped: needs sudo -n, KVM, python3 and unshare"; exit 77; }
[ -d /usr/lib/lordos/lib ] || { echo "skipped: needs /usr/lib/lordos/lib (a LordOS install) to mount the test's lord-vm-usb.so over"; exit 77; }
read -r BUS DEV VP <<< "$VM_USB_DEV"
read -r HBUS HDEV HVP <<< "$VM_USB_HUB"
U=$(id -u); G=$(id -g); ME=$(id -un); REALHOME=$(getent passwd "$U" | cut -d: -f6)
D=$(mktemp -d /var/tmp/lordos-usbe2e.XXXXXX) || exit 1
chmod 711 "$D"
mkdir -p "$D/home/Machines" "$D/run" "$D/lib"
chmod 700 "$D/run" "$D/home"
cp -a /usr/lib/lordos/lib/. "$D/lib/" 2>/dev/null
cp "$B/lord-vm-usb.so" "$D/lib/"
M="$D/home/Machines/UsbE2E.lordvm"
R=0
ok() { echo "ok: $*"; }
bad() { echo "FAIL: $*"; R=1; }
cleanup() {
    XDG_RUNTIME_DIR="$D/run" "$B/lord-vm" --poweroff "$M" >/dev/null 2>&1
    [ -n "${SP:-}" ] && sudo -n kill "$SP" 2>/dev/null
    [ -n "$KEEP" ] || rm -rf "$D"
}
trap cleanup EXIT
"$B/lord-vm" --create "$M" --name UsbE2E --arch amd64 --vcpus 2 --memory 2048 --display none --import "$VM_USB_IMAGE" >"$D/create.log" 2>&1 || { cat "$D/create.log"; exit 1; }
"$B/lord-vm" --set "$M" --agent on --transfer off >/dev/null || exit 1
grep -q '"usb": true' "$M/machine.json" && ok "a new machine has the USB controller (usb: true)" || bad "usb not on by default"
ID=$(sed -n 's/.*"id": "\(.*\)".*/\1/p' "$M/machine.json" | head -1)

# ---- part A ----
cat > "$D/drive.py" <<'PY'
import os, re, subprocess, sys, threading, time
log, cmds, argv = sys.argv[1], sys.argv[2], sys.argv[sys.argv.index('--') + 1:]
p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, pass_fds=(5,))
lines, lf = [], open(log, 'w')
def rd():
    for l in p.stdout:
        l = l.decode(errors='replace'); lines.append(l); lf.write(l); lf.flush()
threading.Thread(target=rd, daemon=True).start()
for c in open(cmds).read().split('\n'):
    if not c: continue
    if c.startswith('sleep '): time.sleep(float(c[6:])); continue
    if c.startswith('wait '):
        rx, t = c[5:].rsplit(' ', 1); end = time.time() + float(t)
        while time.time() < end and not any(re.search(rx, l) for l in lines): time.sleep(0.2)
        continue
    if '$PID' in c:
        pids = [m.group(1) for l in lines for m in [re.search(r'"text":"pid (\d+)"', l)] if m]
        c = c.replace('$PID', pids[-1] if pids else '0')
    lf.write('>> ' + c + '\n'); lf.flush(); p.stdin.write((c + '\n').encode()); p.stdin.flush()
time.sleep(1); p.stdin.close()
try: p.wait(timeout=60)
except Exception: p.kill()
PY
q() { printf '{"execute":"guest-exec","arguments":{"path":"/bin/sh","arg":["-c","%s"],"capture-output":true}}' "$1" > "$D/$2.json"; }
q 'for d in /sys/bus/usb/devices/*; do [ -f $d/idVendor ] && echo $(cat $d/idVendor)$(cat $d/idProduct); done' list
q 'command -v v4l2-ctl || (DEBIAN_FRONTEND=noninteractive apt-get -qq update && DEBIAN_FRONTEND=noninteractive apt-get -qq install -y v4l-utils >/dev/null 2>&1); command -v v4l2-ctl' inst
q 'v4l2-ctl -d /dev/video0 --set-fmt-video=width=640,height=480,pixelformat=YUYV --stream-mmap --stream-count=30 --stream-to=/tmp/f.raw >/dev/null 2>&1; stat -c %s /tmp/f.raw; od -An -tu1 -v -j 300000 -N 4096 /tmp/f.raw | xargs -n1 | sort -u | wc -l' cap
{
    echo 'wait "state":"paused" 60'; echo resume; echo 'sleep 30'
    echo "usb-in 5 $BUS $DEV $VP"; echo 'wait "ev":"usb","n":1 20'; echo 'sleep 6'
    echo "qmpf $D/list.json"; echo 'sleep 3'; echo 'qmp {"execute":"guest-exec-status","arguments":{"pid":$PID}}'; echo 'sleep 1'
    if [ "${VM_USB_CAMERA:-}" = 1 ]; then
        echo "qmpf $D/inst.json"; echo 'sleep 90'; echo 'qmp {"execute":"guest-exec-status","arguments":{"pid":$PID}}'; echo 'sleep 1'
        echo "qmpf $D/cap.json"; echo 'sleep 20'; echo 'qmp {"execute":"guest-exec-status","arguments":{"pid":$PID}}'; echo 'sleep 1'
    fi
    echo "usb-out $BUS $DEV $VP"; echo 'wait "ev":"usb","n":0 20'; echo 'sleep 3'
    echo "qmpf $D/list.json"; echo 'sleep 3'; echo 'qmp {"execute":"guest-exec-status","arguments":{"pid":$PID}}'; echo 'sleep 1'
    echo poweroff; echo 'sleep 5'
} > "$D/cmds"
WANT=${VP/:/}
sudo -n "$B/vm-usb-cli" "$ID $BUS $DEV $VP" -- unshare -Urm sh -c "mount --bind $D/lib /usr/lib/lordos/lib && exec unshare --user --map-user=$U --map-group=$G \"\$0\" \"\$@\"" \
    env XDG_RUNTIME_DIR="$D/run" python3 -I "$D/drive.py" "$D/a.log" "$D/cmds" -- "$B/lordvm-dev" run "$M" --dev-qmp >"$D/a.out" 2>&1
grep -q '"ev":"usb","n":1' "$D/a.log" && ok "A: lord-vm put the device in (VM_USB_STATE 1)" || bad "A: not in"
outs=$(grep '"text":"exited' "$D/a.log")
echo "$outs" | head -1 | grep -q "$WANT" && ok "A: the guest sees $VP on its USB bus" || bad "A: the guest doesn't list $VP: $(echo "$outs" | head -1 | cut -c1-200)"
if [ "${VM_USB_CAMERA:-}" = 1 ]; then
    cap=$(echo "$outs" | sed -n 3p)
    echo "$cap" | grep -q 'exitcode 0: 18432000 ' && ok "A: the guest captured 30 frames of 640x480 from the camera (18432000 bytes)" || bad "A: capture: $(echo "$cap" | cut -c1-200)"
    n=$(echo "$cap" | sed -n 's/.* 18432000 \([0-9]*\).*/\1/p'); [ "${n:-0}" -gt 1 ] && ok "A: the frames hold a picture, not one value ($n different byte values in 4 KB of a frame)" || bad "A: blank frames"
fi
echo "$outs" | tail -1 | grep -q "$WANT" && bad "A: the guest still lists it after usb-out" || ok "A: gone from the guest after usb-out"
grep -q '"ev":"usb","n":0' "$D/a.log" && ok "A: VM_USB_STATE 0 after usb-out" || bad "A: not out"
sleep 2
for i in /sys/bus/usb/devices/*; do
    [ "$(cat "$i/busnum" 2>/dev/null)" = "$BUS" ] && [ "$(cat "$i/devnum" 2>/dev/null)" = "$DEV" ] || continue
    ls -d "$i"/*:*/driver >/dev/null 2>&1 && ok "A: this computer's drivers have it again ($(basename "$(readlink -f "$(ls -d "$i"/*:*/driver | head -1)")"))" || bad "A: no driver on the host after"
done
grep -q "seccomp: QEMU made a call" "$M/log/lord-vm.log" && bad "A: QEMU's allowlist stopped a call: $(grep 'seccomp: QEMU made' "$M/log/lord-vm.log" | tail -1)" || ok "A: QEMU's allowlist unchanged but for usbfs's ioctls and the USB rows: no call stopped"

# ---- part B ----
printf 'globalThis.USB_E2E = %s;\n' "{\"dir\":\"$REALHOME/Machines/UsbE2E.lordvm\",\"bus\":$BUS,\"dev\":$DEV,\"vendor\":\"${VP%%:*}\",\"product\":\"${VP##*:}\",\"hub\":{\"bus\":$HBUS,\"dev\":$HDEV,\"vendor\":\"${HVP%%:*}\",\"product\":\"${HVP##*:}\"}}" > "$D/theme.js"
cat tests/vm_usb_e2e.js >> "$D/theme.js"
chmod 644 "$D/theme.js"
sudo -n unshare -m --propagation private sh -c "mount --bind $D/home $REALHOME && mount -t tmpfs -o mode=700,uid=$U,gid=$G tmpfs /run/user/$U && mount --bind $D/lib /usr/lib/lordos/lib && \
    exec env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=$REALHOME XDG_RUNTIME_DIR=/run/user/$U LANG=C.UTF-8 LORD_GPU=0 $B/lord-server --headless 800x600 --user $ME --theme $D/theme.js --timeout 400" >"$D/server.log" 2>&1 &
SP=$!
L="$D/home/.cache/lordos/shell.log"           # what the theme's console.log writes (the scratch home, mounted over the real one)
usbfds() { sudo -n find /proc/[0-9]*/fd -lname '/dev/bus/usb/*' 2>/dev/null | cut -d/ -f3 | sort -u; }
seen_js=0; seen_in=0
for i in $(seq 1 400); do
    if [ $seen_js = 0 ] && grep -qs 'USBE2E MARK js-request-done' "$L"; then
        seen_js=1; sleep 1
        p=$(usbfds); [ -z "$p" ] && ok "B: after JS's own vm-usb-allow, no process holds the node (lord-server closed the descriptor JS never got)" || bad "B: the node is held by $p"
    fi
    if [ $seen_in = 0 ] && grep -qs 'USBE2E MARK in$' "$L"; then
        seen_in=1
        p=$(usbfds); qp=$(pgrep -f "qemu-system.*guest=UsbE2E" | head -1)
        [ -n "$p" ] && [ "$p" = "$qp" ] && ok "B: only QEMU holds the device's node (not lord-server, not the admin helper)" || bad "B: held by [$p], QEMU is $qp"
    fi
    grep -qs 'USBE2E DONE\|USBE2E FAIL exception' "$L" && break
    sleep 1
done
grep 'USBE2E ok\|USBE2E FAIL' "$L" | sed 's/^.*USBE2E ok /ok: B: /; s/^.*USBE2E FAIL /FAIL: B: /'
grep -q 'USBE2E FAIL' "$L" && R=1
grep -q 'USBE2E DONE' "$L" || { bad "B: the script didn't finish"; tail -20 "$L"; }
[ $R = 0 ] && echo "vm_usb_e2e: all passed" || echo "vm_usb_e2e: FAILED (logs in $D with KEEP=1)"
exit $R
