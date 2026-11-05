#!/bin/sh
# vm_isolation.sh — Machines, Phase 1's "done when" (docs/VIRTUALIZATION.md, "Phase 1 implementation"): what a
# compromised QEMU or lord-vmdisplay can't reach, and that lord-vm survives QEMU's crash and its own.
# Runs on a machine with KVM (a Pi 5, an amd64 PC), as a normal user who can open /dev/kvm, never as root.
#
#   sh tests/vm_isolation.sh BUILD_DIR GUEST_IMAGE [WORK_DIR]
#
# BUILD_DIR holds lord-vm, lord-vmdisplay and lordvm-dev; GUEST_IMAGE is a Debian 13 "nocloud" qcow2 of this
# computer's architecture (it's copied, never changed). WORK_DIR (default /tmp/lvi.$$) is removed at the end.
# How QEMU is made to try: lord-vm --dev-qmp lets the test send QEMU commands that open files and sockets
# (blockdev-add, chardev-add, netdev_add, screendump); each must fail. The guest checks its own network
# through a unit passed as a systemd credential. One line per check: PASS, FAIL or SKIP; exit 1 on any FAIL.
set -u
BIN=$(cd "$1" && pwd) || exit 2
IMG=$2
W=${3:-/tmp/lvi.$$}
D=$W/m.lordvm
VM="$BIN/lord-vm"
# LORDVM_SCOPE=1: each lord-vm starts alone in a delegated scope (systemd-run --user), so nested cgroups are tested
if [ "${LORDVM_SCOPE:-}" = 1 ]; then START="systemd-run --user --scope -q -p Delegate=yes -- $VM"; export LORDVM_SCOPE; else START=$VM; fi
pass=0 fail=0 skip=0
ok()   { pass=$((pass + 1)); echo "PASS  $1"; }
bad()  { fail=$((fail + 1)); echo "FAIL  $1${2:+: $2}"; }
skp()  { skip=$((skip + 1)); echo "SKIP  $1${2:+: $2}"; }
t()    { if eval "$2"; then ok "$1"; else bad "$1" "${3:-}"; fi; }
now()  { date +%s; }
status_field() { "$VM" --status "$D" --json | sed -n "s/.*\"$1\":\"\{0,1\}\([^,\"]*\).*/\1/p"; }
wait_state() {   # STATE SECONDS
    end=$(( $(now) + $2 ))
    while [ "$(now)" -lt "$end" ]; do [ "$(status_field state)" = "$1" ] && return 0; sleep 0.5; done
    return 1
}
wait_serial() {  # TEXT SECONDS
    end=$(( $(now) + $2 ))
    while [ "$(now)" -lt "$end" ]; do grep -aq "$1" "$D/log/serial.log" 2>/dev/null && return 0; sleep 1; done
    return 1
}
qmp_fails() { "$VM" --qmp "$D" "$1" | grep -q '"ok":false'; }
qmp_ok()    { "$VM" --qmp "$D" "$1" | grep -q '"ok":true'; }
cleanup() {
    "$VM" --poweroff "$D" >/dev/null 2>&1
    [ -n "${DEV_PID:-}" ] && kill "$DEV_PID" 2>/dev/null
    [ -n "${LPID:-}" ] && kill "$LPID" 2>/dev/null
    rm -f "$HOME/.lordvm-isotest"
    rm -rf "$W"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
trap '' PIPE                # lordvm-dev may be gone when the last command goes to it
[ "$(id -u)" = 0 ] && { echo "run as a normal user"; exit 2; }
[ -e "$W" ] && { echo "$W exists: give a new work folder"; exit 2; }
mkdir -p "$W" || exit 2
case $(uname -m) in aarch64) SER=ttyAMA0 ;; *) SER=ttyS0 ;; esac
echo "== $(uname -m), kernel $(uname -r), landlock ABI: $(cat /sys/kernel/security/lsm 2>/dev/null | tr , ' ')"

# ---- a machine ----
"$VM" --create "$D" --name isolation-test --import "$IMG" --vcpus 2 --memory 1024 >"$W/create.json" 2>&1
t "create: machine.json, system disk, NVRAM qcow2" '[ -f "$D/machine.json" ] && [ -f "$D/disks/system.qcow2" ] && { grep -q "\"bios\"" "$D/machine.json" || [ -f "$D/disks/nvram.qcow2" ]; }' "$(cat "$W/create.json")"
"$VM" --check "$D" >"$W/check.json"; t "check: valid for this computer" 'grep -q "\"ok\":true" "$W/check.json"' "$(cat "$W/check.json")"
qemu-img create -q -f raw "$D/disks/probe.img" 1M

# host listeners the guest and QEMU must not reach
PORT=$((41000 + $$ % 8000)) PORT2=$((PORT + 1)) UPORT=$((PORT + 2))
python3 - "$PORT" "$W/listen.sock" "$W/hits" <<'EOF' &
import socket, sys, select, os
t = socket.socket(); t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); t.bind(('127.0.0.1', int(sys.argv[1]))); t.listen(8)
u = socket.socket(socket.AF_UNIX); u.bind(sys.argv[2]); u.listen(8)
while True:
    r, _, _ = select.select([t, u], [], [])
    for s in r:
        c, _ = s.accept(); open(sys.argv[3], 'a').write(('tcp' if s is t else 'unix') + '\n'); c.close()
EOF
LPID=$!
sleep 0.5

# first-boot credentials: a root password and a unit that reports the guest's network on the serial line
PW="lvi-$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')"
UNIT=$(cat <<EOF
[Unit]
After=network-online.target
Wants=network-online.target
[Service]
Type=oneshot
StandardOutput=journal
ExecStart=/bin/sh -c 'sleep 2; gw=\$\$(ip -4 route show default | cut -d" " -f3); a=\$\$(ip -4 -o addr show scope global | tr -s " " | cut -d" " -f4 | head -n1); d=\$\$(getent hosts deb.debian.org | head -n1 | cut -d" " -f1); i=\$\$(curl -s -m 10 -o /dev/null -w %%{http_code} http://1.1.1.1/ || true); h=\$\$(curl -s -m 15 -o /dev/null -w %%{http_code} http://deb.debian.org/debian/ || true); l=\$\$(curl -s -m 5 -o /dev/null -w %%{http_code} http://\$\$gw:$PORT/ || true); echo "<3>LORDVM-NET addr=\$\$a gw=\$\$gw dns=\$\$d byip=\$\$i http=\$\$h hostloop=\$\$l" >/dev/kmsg'
EOF
)
DROP=$(printf '[Unit]\nWants=lordvm-net.service\n')
umask 077
{
    echo "firstboot.locale=C.UTF-8"
    echo "firstboot.keymap=us"
    echo "firstboot.timezone=UTC"
    echo "passwd.plaintext-password.root=$PW"
    echo "binary:systemd.extra-unit.lordvm-net.service=$(printf '%s\n' "$UNIT" | base64 -w0)"
    echo "binary:systemd.unit-dropin.multi-user.target=$(printf '%s\n' "$DROP" | base64 -w0)"
} >"$W/creds"
umask 022

# ---- start, paused, with the display attached by lordvm-dev ----
mkfifo "$W/dev.in"
"$BIN/lordvm-dev" run "$D" --dev-qmp --keep-running --credentials-file "$W/creds" <"$W/dev.in" >"$W/dev.out" 2>"$W/dev.err" &
DEV_PID=$!
exec 7>"$W/dev.in"
if wait_state running 60; then ok "start: running"; else bad "start: running" "$(tail -5 "$D/log/lord-vm.log" 2>/dev/null)"; echo "== stopped: the machine didn't start"; exit 1; fi
QPID=$(status_field qemu_pid)
LVPID=$(sed -n 's/.*"pid":\([0-9]*\).*/\1/p' "$D/run.json")
PPID_PASST=$(sed -n 's/.*"passt_pid":\([0-9]*\).*/\1/p' "$D/run.json")
"$VM" --status "$D" --json >"$W/status.json"
t "sandbox: Landlock on QEMU (files)"   'grep -q "\"landlock_fs\":1" "$W/status.json"'
t "sandbox: Landlock on QEMU (TCP)"     'grep -q "\"landlock_net\":1" "$W/status.json"'
t "sandbox: our seccomp filter on QEMU" 'grep -q "\"seccomp_ours\":1" "$W/status.json"'
t "sandbox: /dev/kvm by descriptor"     'grep -q "\"kvm_fd\":1" "$W/status.json"'
t "sandbox: lord-vm's own Landlock"     'grep -q "\"supervisor_landlock\":1" "$W/status.json"'
t "QEMU: seccomp mode 2, two or more filters, no_new_privs, no capabilities" \
  'grep -q "^Seccomp:[[:space:]]*2" /proc/$QPID/status && [ "$(sed -n "s/^Seccomp_filters:[[:space:]]*//p" /proc/$QPID/status)" -ge 2 ] && grep -q "^NoNewPrivs:[[:space:]]*1" /proc/$QPID/status && grep -q "^CapEff:[[:space:]]*0*$" /proc/$QPID/status'
# every descriptor QEMU holds: its sockets, pipes and anonymous files, /dev/kvm, /dev/null, the machine's disks and
# the firmware under /usr
[ -d "/proc/$QPID/fd" ] || bad "QEMU's pid" "$QPID"
odd=$(for f in /proc/$QPID/fd/*; do readlink "$f" 2>/dev/null; done | grep -v -e '^socket:' -e '^pipe:' -e '^anon_inode:' -e '^/memfd:' -e '^/dev/kvm$' -e '^/dev/null$' -e "^$D/disks/" -e '^/usr/' | sort -u)
t "QEMU's descriptors: nothing outside its folder, /usr, /dev/kvm and /dev/null" '[ -z "$odd" ]' "$odd"
t "QEMU: /dev/kvm is held (passed, not opened)" 'for f in /proc/$QPID/fd/*; do readlink "$f" 2>/dev/null; done | grep -q "^/dev/kvm$"'
cg=$(sed -n 's/^0:://p' /proc/$QPID/cgroup)
case $cg in
    */qemu) want=$(( ($(sed -n 's/.*"memory_mb": \([0-9]*\).*/\1/p' "$D/machine.json") + 384) * 1048576 )); have=$(cat /sys/fs/cgroup$cg/memory.max)
            if [ "$have" = "$want" ]; then ok "cgroup: QEMU in its own leaf ($cg), memory.max $have"; else bad "cgroup: QEMU's memory.max" "$have, want $want"; fi ;;
    *lordos-vm*) ok "cgroup: QEMU in the machine's scope ($cg), no nested leaf" ;;
    *) skp "cgroup: QEMU in $cg" "no delegated cgroup for this run (start it under a delegated scope)" ;;
esac
[ -n "$PPID_PASST" ] && [ "$PPID_PASST" -gt 0 ] && t "passt: in the machine's cgroup, listening on nothing" \
  '[ "$(sed -n "s/^0:://p" /proc/$PPID_PASST/cgroup | sed "s,/[^/]*$,,")" = "$(echo "$cg" | sed "s,/[^/]*$,,")" ] && ! ss -ltnup 2>/dev/null | grep -q "pid=$PPID_PASST,"'

# ---- files ----
t "QEMU can't open /etc/hostname"   'qmp_fails "{\"execute\":\"blockdev-add\",\"arguments\":{\"driver\":\"file\",\"node-name\":\"x1\",\"filename\":\"/etc/hostname\",\"read-only\":true}}"'
echo secret >"$HOME/.lordvm-isotest"
t "QEMU can't open a file in the home folder" 'qmp_fails "{\"execute\":\"blockdev-add\",\"arguments\":{\"driver\":\"file\",\"node-name\":\"x2\",\"filename\":\"$HOME/.lordvm-isotest\",\"read-only\":true}}"'
t "QEMU can't open its own machine.json" 'qmp_fails "{\"execute\":\"blockdev-add\",\"arguments\":{\"driver\":\"file\",\"node-name\":\"x3\",\"filename\":\"$D/machine.json\",\"read-only\":true}}"'
t "QEMU can't open its logs" 'qmp_fails "{\"execute\":\"blockdev-add\",\"arguments\":{\"driver\":\"file\",\"node-name\":\"x4\",\"filename\":\"$D/log/lord-vm.log\",\"read-only\":true}}"'
t "QEMU can open a file in its disks folder (control)" 'qmp_ok "{\"execute\":\"blockdev-add\",\"arguments\":{\"driver\":\"file\",\"node-name\":\"ok1\",\"filename\":\"$D/disks/probe.img\"}}"'
qmp_ok '{"execute":"blockdev-del","arguments":{"node-name":"ok1"}}' >/dev/null
t "QEMU can't write a screenshot to /tmp" 'qmp_fails "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"/tmp/lvi-shot.ppm\"}}" && [ ! -e /tmp/lvi-shot.ppm ]'
t "QEMU can't read lord-vm's /proc" 'qmp_fails "{\"execute\":\"blockdev-add\",\"arguments\":{\"driver\":\"file\",\"node-name\":\"x5\",\"filename\":\"/proc/$LVPID/environ\",\"read-only\":true}}"'
t "QEMU can't make a file chardev outside its folder" 'qmp_fails "{\"execute\":\"chardev-add\",\"arguments\":{\"id\":\"f1\",\"backend\":{\"type\":\"file\",\"data\":{\"out\":\"/tmp/lvi-chardev\"}}}}" && [ ! -e /tmp/lvi-chardev ]'
# ---- devices ----
# /dev/kvm is left out: QEMU holds it already, and on arm64 it opens it again by path for the host CPU's features
for dev in /dev/mem /dev/mmcblk0 /dev/vda /dev/sda /dev/nvme0n1 /dev/dri/renderD128 /dev/dri/card0 /dev/net/tun /dev/vhost-net /dev/tty /dev/ptmx; do
    [ -e "$dev" ] || continue
    t "QEMU can't open $dev" 'qmp_fails "{\"execute\":\"blockdev-add\",\"arguments\":{\"driver\":\"host_device\",\"node-name\":\"d$(echo $dev | tr -c a-z0-9 _)\",\"filename\":\"$dev\",\"read-only\":true}}"'
done
# ---- network ----
t "QEMU can't connect to a TCP port on the host" 'qmp_fails "{\"execute\":\"chardev-add\",\"arguments\":{\"id\":\"n1\",\"backend\":{\"type\":\"socket\",\"data\":{\"addr\":{\"type\":\"inet\",\"data\":{\"host\":\"127.0.0.1\",\"port\":\"$PORT\"}},\"server\":false}}}}"'
t "QEMU can't listen on a TCP port" 'qmp_fails "{\"execute\":\"chardev-add\",\"arguments\":{\"id\":\"n2\",\"backend\":{\"type\":\"socket\",\"data\":{\"addr\":{\"type\":\"inet\",\"data\":{\"host\":\"127.0.0.1\",\"port\":\"$PORT2\"}},\"server\":true,\"wait\":false}}}}"'
t "QEMU can't connect to a unix socket" 'qmp_fails "{\"execute\":\"chardev-add\",\"arguments\":{\"id\":\"n3\",\"backend\":{\"type\":\"socket\",\"data\":{\"addr\":{\"type\":\"unix\",\"data\":{\"path\":\"$W/listen.sock\"}},\"server\":false}}}}"'
if [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -S "$XDG_RUNTIME_DIR/bus" ]; then
    t "QEMU can't connect to the session bus" 'qmp_fails "{\"execute\":\"chardev-add\",\"arguments\":{\"id\":\"n4\",\"backend\":{\"type\":\"socket\",\"data\":{\"addr\":{\"type\":\"unix\",\"data\":{\"path\":\"$XDG_RUNTIME_DIR/bus\"}},\"server\":false}}}}"'
else skp "QEMU and the session bus" "no session bus here"; fi
t "QEMU can't send UDP" 'qmp_fails "{\"execute\":\"chardev-add\",\"arguments\":{\"id\":\"n5\",\"backend\":{\"type\":\"udp\",\"data\":{\"remote\":{\"type\":\"inet\",\"data\":{\"host\":\"127.0.0.1\",\"port\":\"$UPORT\"}}}}}}"'
"$VM" --qmp "$D" "{\"execute\":\"netdev_add\",\"arguments\":{\"type\":\"user\",\"id\":\"u1\",\"hostfwd\":[{\"str\":\"tcp:127.0.0.1:$PORT2-:22\"}]}}" >"$W/nd.json"
t "QEMU can't open its own user network with a forwarded port" '! ss -ltn 2>/dev/null | grep -q ":$PORT2 "' "$(cat "$W/nd.json")"
t "nothing reached the host's listeners" '[ ! -s "$W/hits" ]' "$(cat "$W/hits" 2>/dev/null)"

# ---- the guest: picture, keyboard, network through passt ----
echo "wait-frames 1 60" >&7
t "display: a first picture" 'n=0; while [ $n -lt 60 ] && ! grep -q first_frame "$W/dev.out"; do sleep 1; n=$((n+1)); done; grep -q first_frame "$W/dev.out"' "$(tail -3 "$W/dev.err")"
VDPID=$(sed -n 's/.*"ev":"vmdisplay","pid":\([0-9]*\).*/\1/p' "$W/dev.out")
t "lord-vmdisplay: seccomp, no_new_privs" 'grep -q "^Seccomp:[[:space:]]*2" /proc/$VDPID/status && grep -q "^NoNewPrivs:[[:space:]]*1" /proc/$VDPID/status'
vdodd=$(for f in /proc/$VDPID/fd/*; do readlink "$f" 2>/dev/null; done | grep -v -e '^socket:' -e '^/memfd:' -e '^anon_inode:' -e '^/dev/null$' -e '^pipe:' | sort -u)
t "lord-vmdisplay holds no file" '[ -z "$vdodd" ]' "$vdodd"
"$BIN/lord-vmdisplay" --selftest >"$W/selftest.json" 2>/dev/null
# /dev/null stays open to the shared helper sandbox (sandbox.c), on purpose
t "lord-vmdisplay's sandbox: no files, no devices but /dev/null, no sockets, no fork" 'grep -q "\"open_etc\":false,\"open_home\":false,\"open_dev_null\":true,\"open_dev_kvm\":false,\"create_tmp\":false,\"socket_inet\":false,\"socket_unix\":false,\"socketpair\":false,\"fork\":false" "$W/selftest.json"' "$(cat "$W/selftest.json")"
if wait_serial "LORDVM-NET" 240; then
    line=$(grep -a "LORDVM-NET" "$D/log/serial.log" | head -n1 | tr -d '\r')
    echo "      guest: $line"
    t "guest network: an address from passt" 'echo "$line" | grep -q "addr=[0-9]"'
    t "guest can't reach the host's loopback" '! echo "$line" | grep -q "hostloop=[1-9]"'
    case $line in *dns=[0-9]*) ok "guest DNS through passt" ;; *) skp "guest DNS" "no answer ($line)" ;; esac
    case $line in *http=200*|*http=30*) ok "guest reaches the internet through passt" ;; *) skp "guest internet" "no answer from deb.debian.org ($line)" ;; esac
else bad "guest network report on the serial line" "$(tail -c 400 "$D/log/serial.log")"; fi
wait_serial "login:" 120
sleep 3
echo "png $W/before.png" >&7
sleep 1
t "display: the picture isn't black" 'grep "\"cmd\":\"png\"" "$W/dev.out" | tail -n1 | grep -qv "\"lit_pct\":0.00"'
TOKEN="KBD$(od -An -N4 -tx1 /dev/urandom | tr -d ' \n')"
# first boot can still be busy at the prompt: the login is tried up to three times
kbd=0
for try in 1 2 3; do
    printf 'type root\\n\n' >&7; sleep 3
    printf 'type %s\\n\n' "$PW" >&7; sleep 8
    printf 'type echo %s > /dev/%s\\n\n' "$TOKEN" "$SER" >&7
    if wait_serial "$TOKEN" 20; then kbd=$try; break; fi
done
t "keyboard through lord-vmdisplay: typed command ran in the guest (try $kbd)" '[ "$kbd" -gt 0 ]'
echo "move 100 100" >&7; echo "click 200 150" >&7; echo "wheel -2" >&7; echo "release" >&7
echo "resize 1024 768" >&7
sleep 4
echo "png $W/after.png" >&7
sleep 1
grep -q '"ev":"picture","w":1024,"h":768' "$W/dev.out" && ok "resize: the guest took 1024 × 768" || skp "resize: the guest kept its size" "$(grep '"ev":"picture"' "$W/dev.out" | tail -n1)"
t "lord-vmdisplay refused nothing, lordvm-dev refused nothing" '! grep -q -e "\"ev\":\"refused\"" -e "\"refused\":[1-9]" "$W/dev.out" && ! grep -q "lord-vmdisplay: refused" "$W/dev.err"' "$(grep -h -e "ev.:.refused" -e "refused.:[1-9]" -e "vmdisplay: refused" "$W/dev.out" "$W/dev.err" | head -3)"
# secrets never on a command line nor in a log
t "the root password isn't on QEMU's command line" '! tr "\0" " " </proc/$QPID/cmdline | grep -q "$PW"'
t "the root password isn't in the machine's logs" '! grep -rqa "$PW" "$D/log" "$D/run.json" "$D/machine.json"'

# ---- lifecycle ----
"$VM" --pause "$D" >/dev/null; t "pause" 'wait_state paused 10'
"$VM" --resume "$D" >/dev/null; t "resume" 'wait_state running 10'
t "a second lord-vm on the same machine is refused" '"$VM" --start "$D" >/dev/null 2>&1; [ $? -eq 3 ]'

# QEMU crashes: lord-vm reports it, cleans up and exits
kill -SEGV "$QPID"
sleep 3
t "QEMU crash: lord-vm reports crashed (signal 11) and exits" 'grep -q "\"state\":\"crashed\",\"reason\":\"qemu-signal\",\"code\":11" "$D/run.json" && ! kill -0 "$LVPID" 2>/dev/null' "$(cat "$D/run.json")"
t "QEMU crash: passt is gone" '! kill -0 "$PPID_PASST" 2>/dev/null'
t "QEMU crash: no control socket left" '[ ! -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/lordos-vm/$(sed -n "s/.*\"id\": \"\([^\"]*\)\".*/\1/p" "$D/machine.json").sock" ]'
echo quit >&7 2>/dev/null
exec 7>&-
wait "$DEV_PID" 2>/dev/null
DEV_PID=

# lord-vm itself dies: QEMU and passt go with it, the next start recovers
$START --start "$D" --detach >"$W/start2.json" 2>&1
t "restart after the crash" 'grep -q "\"started\":true" "$W/start2.json"' "$(cat "$W/start2.json")"
QPID=$(status_field qemu_pid)
LVPID=$(sed -n 's/.*"pid":\([0-9]*\).*/\1/p' "$D/run.json")
PPID_PASST=$(sed -n 's/.*"passt_pid":\([0-9]*\).*/\1/p' "$D/run.json")
kill -KILL "$LVPID"
sleep 2
t "lord-vm killed: QEMU and passt end with it" '! kill -0 "$QPID" 2>/dev/null && ! kill -0 "$PPID_PASST" 2>/dev/null'
t "lord-vm killed: status says the supervisor was lost" '"$VM" --status "$D" --json | grep -q "\"reason\":\"supervisor-lost\""'
$START --start "$D" --detach >"$W/start3.json" 2>&1
t "recovery: starts again over the stale lock and socket" 'grep -q "\"started\":true" "$W/start3.json" && grep -q "recovery:" "$D/log/lord-vm.log"' "$(cat "$W/start3.json")"
# a clean shutdown (ACPI), timed
# ACPI is heard once the guest has booted (a nested amd64 guest takes about 40 s)
wait_serial "login:" 180
sleep 5
t0=$(now)
"$VM" --stop "$D" --timeout 90 --json >"$W/stop.json"
t "shutdown: the guest powers off (ACPI) in $(( $(now) - t0 )) s" 'grep -q "\"state\":\"stopped\",\"reason\":\"host-shutdown\"" "$W/stop.json"' "$(cat "$W/stop.json")"

echo "== $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
