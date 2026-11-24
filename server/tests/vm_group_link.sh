#!/bin/bash
# vm_group_link.sh — a machine group end to end, headless (docs/VIRTUALIZATION.md 19.3, 19.9, 19.10): a Whonix group made by
# lord-vm --group-create, started and stopped through a headless lord-server's session with the `machines` command, and
# looked at from inside its guests over their serial consoles (lord-vm's VM_CONSOLE). Checks:
#   - `machines start Whonix --no-window` starts the gateway, then the workstation, and the session links them (19.3);
#   - the gateway's eth0 is passt's card (slot 0x2) and its eth1 the link (slot 0x18); the workstation's only card is eth0
#     (slot 0x2), on Whonix's internal address, with its only route through 10.152.152.10;
#   - the workstation reaches the internet only through the gateway: Tor's own check says IsTor true;
#   - on the host, the link is an unnamed socket pair held by the two QEMUs and nothing else;
#   - with the gateway off, the workstation has no network (fail closed); the gateway started again is linked again by the
#     session by itself, and the workstation's network is back;
#   - `machines poweroff Whonix` turns the workstation off before the gateway.
# Its guests: Whonix's two KVM disks with a root shell on their serial console (systemd.debug_shell=ttyS0 added to their
# GRUB entries, nothing else changed), given as WHONIX_GW and WHONIX_WS (qcow2). They aren't in the repository (2 × 3 GB):
# docs/VIRTUALIZATION.md 19.10 says how to make them from the verified archive. Exit 77 without them, KVM, QEMU, the theme
# or unshare.
#
# lord-server runs as this account but in its own user and mount namespaces, with a private home, an empty folder over the
# real home and a tmpfs over the real runtime folder: it never sees the person's files or their session.
cd "$(dirname "$0")/.." || exit 1
THEME=../themes/default/theme.bundle.js
[ -f "${WHONIX_GW:-}" ] && [ -f "${WHONIX_WS:-}" ] && { [ -x build/lord-vm-testpaths ] || make -s build/lord-vm-testpaths >/dev/null 2>&1; }
[ -x build/lord-server ] && [ -x build/machines ] && [ -x build/lord-vm ] && [ -x build/lord-vm-testpaths ] && [ -f "$THEME" ] || { echo "skipped: needs build/lord-server, machines, lord-vm, lord-vm-testpaths and $THEME"; exit 77; }
[ -r /dev/kvm ] && command -v qemu-system-x86_64 >/dev/null 2>&1 && command -v unshare >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 || { echo "skipped: needs KVM, QEMU, unshare and python3"; exit 77; }
[ -f "${WHONIX_GW:-}" ] && [ -f "${WHONIX_WS:-}" ] || { echo "skipped: needs WHONIX_GW and WHONIX_WS (Whonix's disks with a serial root shell)"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/var/tmp}/lgroup.XXXXXX") || exit 1
chmod 700 "$D"
mkdir -p "$D/home/Machines" "$D/run" "$D/fake"
chmod 700 "$D/run" "$D/home"
REALHOME=$(getent passwd "$(id -u)" | cut -d: -f6)
RT=/run/user/$(id -u)
E="env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR=$D/run HOME=$D/home LANG=C.UTF-8"
G="$D/home/Machines/Whonix.lordgroup"
R=0
ok() { echo "ok: $*"; }
bad() { echo "FAIL: $*"; R=1; }
# the namespaces: root in the first only to mount, then this account again
NS="unshare -Urm sh -c 'mount --bind $D/fake $REALHOME && { [ -d $RT ] && mount -t tmpfs -o mode=700 tmpfs $RT || true; } && exec unshare --user --map-user=$(id -u) --map-group=$(id -g) \"\$0\" \"\$@\"'"
ns() { eval "$NS" '"$@"'; }
cleanup() {
    for m in Workstation Gateway; do ns $E ./build/lord-vm --poweroff "$G/$m.lordvm" >/dev/null 2>&1; done
    [ -n "${P:-}" ] && kill "$P" 2>/dev/null
    rm -rf "$D"
}
trap cleanup EXIT

# the group, from the two disks (the test build's --dev-gateway/--dev-workstation: no archive)
ns $E LORDVM_TEST_CATALOG="$(realpath ../assets/machines/systems.json)" ./build/lord-vm-testpaths --group-create "$G" --catalog-id whonix \
    --dev-gateway "$WHONIX_GW" --dev-workstation "$WHONIX_WS" >"$D/create.log" 2>&1 && ok "group made" || { bad "lord-vm --group-create"; cat "$D/create.log"; exit 1; }
id_of() { sed -n 's/.*"id": "\(.*\)".*/\1/p' "$G/$1.lordvm/machine.json" | head -1; }
GW=$(id_of Gateway); WS=$(id_of Workstation)

# the session
ns $E LORD_GPU=0 ./build/lord-server --headless 1024x768 --theme "$THEME" --timeout 4000 >"$D/server.log" 2>&1 &
P=$!
for i in $(seq 1 100); do [ -S "$D/run/lordos-machines.sock" ] && break; sleep 0.1; done
sleep 2
out=$(ns $E ./build/machines start Whonix --no-window 2>&1); rc=$?
[ $rc = 0 ] && ok "machines start Whonix --no-window: $out" || bad "start: $rc $out"
st=$(ns $E ./build/machines status Whonix --json)
echo "$st" | grep -q '"state":"running"' && ok "the group runs" || bad "the group's state: $st"
links() { grep -c "link: connected" "$G/Workstation.lordvm/log/lord-vm.log" 2>/dev/null; }   # the workstation took a link from the session (lord-vm logs each)
for i in $(seq 1 20); do [ "$(links)" -ge 1 ] && break; sleep 0.5; done
[ "$(links)" -ge 1 ] && ok "the session linked the members" || bad "no link from the session"
gs=$(grep -m1 "state running" "$G/Gateway.lordvm/log/lord-vm.log" | cut -c1-24); ws=$(grep -m1 "state starting" "$G/Workstation.lordvm/log/lord-vm.log" | cut -c1-24)
[[ "$gs" < "$ws" ]] && ok "the gateway ran before the workstation started ($gs < $ws)" || bad "start order: gateway running $gs, workstation starting $ws"

# a guest's serial console: commands and their output
con() { # con <machine id> <timeout> <command>...
    ns python3 -I -c '
import array, re, select, socket, struct, sys, time
sock, tmo, cmds = sys.argv[1], float(sys.argv[2]), sys.argv[3:]
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); s.connect(sock)
s.send(struct.pack("<HHIiiii200s", 13, 1, 77, 0, 0, 0, 0, b""))
fd = -1
while fd < 0:
    msg, anc, fl, ad = s.recvmsg(256, socket.CMSG_SPACE(4))
    for l, t, d in anc:
        if l == socket.SOL_SOCKET and t == socket.SCM_RIGHTS: fd = array.array("i", d[:4])[0]
    if struct.unpack("<H", msg[:2])[0] == 64: sys.exit(2)
c = socket.socket(fileno=fd); c.setblocking(False)
def until(mark, t):
    b = b""; end = time.time() + t
    while time.time() < end:
        if select.select([c], [], [], 0.5)[0]:
            try: k = c.recv(65536)
            except BlockingIOError: continue
            if not k: break
            b += k
            if mark in b: return b
    return b
c.send(b"\r"); until(b"#", 5)
c.send(b"dmesg -n 1 2>/dev/null; stty -echo; export TERM=dumb PS1=; echo __W__\r"); until(b"\n__W__", 10)
c.send(b"echo __V__\r"); until(b"\n__V__", 10)
for i, cmd in enumerate(cmds):
    m = ("__E%d__" % i).encode()
    c.send(cmd.encode() + b"; echo; echo __E%d__\r" % i)
    o = until(b"\n" + m, tmo).decode(errors="replace").replace("\r", "")
    print(re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", o.split("\n__E%d__" % i)[0]).strip())
' "$D/run/lordos-vm/$1.sock" "$2" "${@:3}"
}
wait_shell() { for i in $(seq 1 60); do grep -q "root@host" "$G/$1.lordvm/log/serial.log" 2>/dev/null && return 0; sleep 2; done; return 1; }
wait_shell Gateway && wait_shell Workstation || bad "no serial shell in the guests"
sleep 20

o=$(con "$GW" 30 "for i in /sys/class/net/eth*; do echo \$(basename \$i) \$(basename \$(readlink -f \$i/device/..)); done")
echo "$o" | grep -q "eth0 0000:00:02.0" && echo "$o" | grep -q "eth1 0000:00:18.0" && ok "gateway: eth0 is passt's card (0x2), eth1 the link (0x18)" || bad "gateway's cards: $o"
o=$(con "$WS" 30 "for i in /sys/class/net/eth*; do echo \$(basename \$i) \$(basename \$(readlink -f \$i/device/..)); done" "ip -4 -br addr show eth0" "ip route")
echo "$o" | grep -q "eth0 0000:00:02.0" && ! echo "$o" | grep -q eth1 && ok "workstation: one card, eth0 at 0x2" || bad "workstation's cards: $o"
echo "$o" | grep -q "10.152.152.11/18" && ok "workstation: 10.152.152.11" || bad "workstation's address: $o"
[ "$(echo "$o" | grep -c "default")" = 1 ] && echo "$o" | grep -q "default via 10.152.152.10 dev eth0" && ok "workstation: its only way out is 10.152.152.10" || bad "workstation's routes: $o"
o=$(con "$WS" 200 "for k in \$(seq 1 8); do timeout 40 curl -sS -m 35 https://check.torproject.org/api/ip && break; sleep 5; done")
echo "$o" | grep -q '"IsTor":true' && ok "workstation: through Tor ($o)" || bad "workstation through Tor: $o"

# on the host: the link is an unnamed pair between the two QEMUs only
qg=$(sed -n 's/.*"qemu_pid": *\([0-9]*\).*/\1/p' "$G/Gateway.lordvm/run.json"); qw=$(sed -n 's/.*"qemu_pid": *\([0-9]*\).*/\1/p' "$G/Workstation.lordvm/run.json")
if [ -n "$qg" ] && [ -n "$qw" ]; then
    pair=$(ss -xpH 2>/dev/null | awk -v g="pid=$qg," -v w="pid=$qw," 'index($0, g) || index($0, w)' | awk '$5 == "*" && $7 == "*"' | wc -l)
    ok "host: $pair unnamed sockets between the QEMUs (the link among them)"
    # the anonymity profile's memory: no transparent huge pages for either QEMU (PR_SET_THP_DISABLE)
    for q in "$qg" "$qw"; do
        t=$(sed -n 's/^THP_enabled:[[:space:]]*//p' "/proc/$q/status")
        [ -z "$t" ] || [ "$t" = 0 ] && ok "QEMU $q: no transparent huge pages (THP_enabled ${t:-not shown})" || bad "QEMU $q has transparent huge pages"
    done
fi

# the gateway off: no network for the workstation
ns $E ./build/lord-vm --poweroff "$G/Gateway.lordvm" >/dev/null 2>&1
o=$(con "$WS" 60 "timeout 25 curl -sS -m 20 https://check.torproject.org/api/ip; echo rc=\$?" "timeout 8 bash -c 'echo > /dev/tcp/10.152.152.10/9050'; echo tcp=\$?")
echo "$o" | grep -q "rc=0" && bad "workstation reached the internet with the gateway off: $o" || ok "gateway off: the workstation has no network"
echo "$o" | grep -q "tcp=0" && bad "the gateway's address answered while off" || ok "gateway off: 10.152.152.10 unreachable"
# started again: the session links the new pair by itself
out=$(ns $E ./build/machines start "$G/Gateway.lordvm" --no-window 2>&1) && ok "gateway started again" || bad "gateway restart: $out"
for i in $(seq 1 30); do [ "$(links)" -ge 2 ] && break; sleep 1; done
[ "$(links)" -ge 2 ] && ok "the session linked the new gateway by itself" || bad "no new link after the gateway's restart"
wait_shell Gateway; sleep 20
o=$(con "$WS" 300 "for k in \$(seq 1 10); do timeout 40 curl -sS -m 35 https://check.torproject.org/api/ip && break; sleep 5; done")
echo "$o" | grep -q '"IsTor":true' && ok "the workstation's network is back through Tor" || bad "after the gateway's restart: $o"

# the group off, the workstation first
out=$(ns $E ./build/machines poweroff Whonix 2>&1) && ok "machines poweroff Whonix: $out" || bad "poweroff: $out"
gs=$(grep "state stopped" "$G/Gateway.lordvm/log/lord-vm.log" | tail -1 | cut -c1-24); ws=$(grep "state stopped" "$G/Workstation.lordvm/log/lord-vm.log" | tail -1 | cut -c1-24)
[[ "$ws" < "$gs" ]] && ok "the workstation stopped before the gateway" || bad "stop order: workstation $ws, gateway $gs"

# ---- the shutdown (19.0): both members power off by themselves, the gateway's Tor stopping cleanly ----
st=$(ns $E ./build/machines start Whonix --no-window 2>&1) || bad "start for the shutdown check: $st"
wait_shell Gateway && wait_shell Workstation; sleep 60
# the gateway's Tor and logind on its serial log, so the stop can be read after it powered off
con "$GW" 20 "(journalctl -f -n0 -o short-monotonic -u tor@default -u systemd-logind > /dev/ttyS0 2>&1 &); echo ok" >/dev/null
con "$GW" 20 "systemctl is-active tor@default" | grep -q '^active' && ok "the gateway's Tor runs" || bad "the gateway's Tor isn't running"
t0=$(date +%s)
out=$(ns $E ./build/machines stop Whonix 2>&1) && ok "machines stop Whonix: $out ($(( $(date +%s) - t0 )) s)" || bad "stop: $out"
for m in Workstation Gateway; do
    l=$(grep "state stopped" "$G/$m.lordvm/log/lord-vm.log" | tail -1)
    echo "$l" | grep -q "forced" && bad "$m was forced off: $l" || ok "$m powered off by itself: ${l:25:80}"
done
grep -q "shut down: the power button" "$G/Workstation.lordvm/log/lord-vm.log" && ok "the keys and the power button were sent" || bad "no shutdown sequence in the workstation's log"
grep -a -q "Power key pressed" "$G/Gateway.lordvm/log/serial.log" && grep -a -q "Powering off" "$G/Gateway.lordvm/log/serial.log" && ok "the gateway: logind took the power key and powered off" || bad "the gateway's logind didn't power off"
grep -a -E -q "Stopped tor@default|tor@default.service: Deactivated successfully" "$G/Gateway.lordvm/log/serial.log" && ok "the gateway's Tor stopped cleanly" || bad "no clean stop of Tor in the gateway's log"

# ---- the disposable session (19.7, isolation layer 6): in memory only, gone at power-off ----
ws_disk="$G/Workstation.lordvm/disks/system.qcow2"
before=$(sha256sum "$ws_disk" | cut -c1-64); bm=$(stat -c %Y "$ws_disk")
ls -la "$G/Workstation.lordvm/disks" > "$D/disks.before"
ns $E ./build/lord-vm --group-set "$G" --disposable on >/dev/null && ok "disposable on" || bad "lord-vm --group-set --disposable on"
# a marker the guest writes 600 MB of: afterwards it must be in no file of this computer (only in memory)
M="LORDOS-SESSION-MARK-$RANDOM$RANDOM$(date +%N)"
st=$(ns $E ./build/machines start Whonix --no-window 2>&1) || bad "start in a disposable session: $st"
wait_shell Workstation; sleep 20
grep -q "disposable session: the workstation's changes stay in memory" "$G/Workstation.lordvm/log/lord-vm.log" && ok "the workstation started in a disposable session" || bad "no disposable session in the log"
qw=$(sed -n 's/.*"qemu_pid": *\([0-9]*\).*/\1/p' "$G/Workstation.lordvm/run.json")
o=$(con "$WS" 200 "yes $M | head -c 600M > /home/user/session-blob; sync; ls -la /home/user/session-blob")
echo "$o" | grep -q session-blob && ok "the guest wrote 600 MB" || bad "the guest's write: $o"
ls -la "$G/Workstation.lordvm/disks" > "$D/disks.during"
diff -q <(awk '{print $5, $9}' "$D/disks.before") <(awk '{print $5, $9}' "$D/disks.during") >/dev/null && ok "the disks folder didn't change during the session" || { bad "the disks folder changed"; diff "$D/disks.before" "$D/disks.during"; }
[ "$(stat -c %Y "$ws_disk")" = "$bm" ] && ok "system.qcow2 wasn't written" || bad "system.qcow2 was written during the session"
# every file written since the session began, in the library, /tmp, /var/tmp and the runtime folder: none holds the marker
# (the serial log is the console's text, which never echoes it); the host's page cache of those files then can't either
hits=$(find "$D" /tmp /var/tmp -xdev -newer "$D/disks.before" -type f -size +0 2>/dev/null | grep -v '/log/serial.log$' | xargs -r grep -lF --binary-files=text "$M" 2>/dev/null | head -5)
[ -z "$hits" ] && ok "the session's data is in no file of this computer (the library, /tmp, /var/tmp, the runtime folder)" || bad "the session's data is in: $hits"
# the cap: the workstation's memory (2 GB); past it the guest is paused
o=$(con "$WS" 400 "dd if=/dev/urandom of=/home/user/session-blob2 bs=1M count=1800 status=none; sync; echo wrote" 2>&1)
for i in $(seq 1 30); do grep -q "session-full" "$G/Workstation.lordvm/log/lord-vm.log" && break; sleep 2; done
grep -q "its cap: the guest is paused" "$G/Workstation.lordvm/log/lord-vm.log" && ok "at the cap the workstation was paused" || bad "no pause at the session's cap"
s=$(ns $E ./build/machines status "$G/Workstation.lordvm" --json 2>/dev/null)
echo "$s" | grep -q 'session-full' && ok "its state says session-full" || bad "its state: $s"
out=$(ns $E ./build/machines poweroff Whonix 2>&1) && ok "the group off: $out" || bad "poweroff: $out"
grep -q "disposable session: .* MB in memory, gone" "$G/Workstation.lordvm/log/lord-vm.log" && ok "the session's memory went with QEMU" || bad "no end of the session in the log"
mb=$(sed -n 's/.*disposable session: \([0-9]*\) MB in memory, gone.*/\1/p' "$G/Workstation.lordvm/log/lord-vm.log" | tail -1)
[ "${mb:-0}" -ge 600 ] && ok "the session held ${mb} MB in memory, then nothing" || bad "the session's memory at its end: ${mb:-?} MB"
[ "$(sha256sum "$ws_disk" | cut -c1-64)" = "$before" ] && ok "system.qcow2 is byte for byte as before the session" || bad "system.qcow2 changed"
ns $E ./build/lord-vm --group-set "$G" --disposable off >/dev/null
st=$(ns $E ./build/machines start Whonix --no-window 2>&1) || bad "start after the session: $st"
wait_shell Workstation; sleep 15
o=$(con "$WS" 30 "ls /home/user/session-blob /home/user/session-blob2 2>&1 | head -2")
echo "$o" | grep -q "No such file" && ! echo "$o" | grep -q "^/home/user/session-blob$" && ok "the session's files are gone" || bad "the session's files: $o"

# ---- Start Fresh (19.7): the workstation only, then the gateway too ----
o=$(con "$WS" 30 "echo kept > /home/user/fresh-marker; sync; cat /home/user/fresh-marker")
echo "$o" | grep -q kept || bad "the marker: $o"
ns $E ./build/machines poweroff Whonix >/dev/null 2>&1
out=$(ns $E ./build/machines fresh Whonix 2>&1 </dev/null); [ $? = 2 ] && ok "fresh without --yes and no terminal is refused" || bad "fresh without --yes: $out"
gw0=$(id_of Gateway); ws0=$(id_of Workstation); wsmac0=$(sed -n 's/.*"mac": "\(.*\)".*/\1/p' "$G/Workstation.lordvm/machine.json" | head -1)
out=$(ns $E ./build/machines fresh Whonix --yes 2>&1) && ok "machines fresh Whonix --yes: $out" || bad "fresh: $out"
[ "$(id_of Workstation)" != "$ws0" ] && ok "the workstation has a new id" || bad "the workstation's id didn't change"
[ "$(sed -n 's/.*"mac": "\(.*\)".*/\1/p' "$G/Workstation.lordvm/machine.json" | head -1)" != "$wsmac0" ] && ok "the workstation has a new MAC address" || bad "its MAC didn't change"
[ "$(id_of Gateway)" = "$gw0" ] && ok "the gateway kept its identity" || bad "the gateway changed without --gateway"
[ "$(stat -c %s "$ws_disk")" -lt 10000000 ] && ok "the workstation's disk is an empty overlay again ($(stat -c %s "$ws_disk") bytes)" || bad "the workstation's disk: $(stat -c %s "$ws_disk") bytes"
WS=$(id_of Workstation)
st=$(ns $E ./build/machines start Whonix --no-window 2>&1) || bad "start after Start Fresh: $st"
wait_shell Workstation; sleep 15
o=$(con "$WS" 30 "cat /home/user/fresh-marker 2>&1")
echo "$o" | grep -q "No such file" && ok "the workstation's file is gone after Start Fresh" || bad "after Start Fresh: $o"
ns $E ./build/machines poweroff Whonix >/dev/null 2>&1
out=$(ns $E ./build/machines fresh Whonix --gateway --yes 2>&1) && ok "machines fresh Whonix --gateway --yes: $out" || bad "fresh --gateway: $out"
[ "$(id_of Gateway)" != "$gw0" ] && ok "the gateway has a new id too" || bad "the gateway's id didn't change with --gateway"
ns $E ./build/lord-vm --group-set "$G" --disposable on >/dev/null
out=$(ns $E ./build/machines start Whonix --member gateway --no-window 2>&1) && ok "--member gateway starts the gateway alone: $out" || bad "--member gateway: $out"
s=$(ns $E ./build/machines status Whonix --json)
echo "$s" | grep -q '"state":"partial"' && ok "the group is partial" || bad "after --member gateway: $s"
ns $E ./build/machines poweroff Whonix --member gateway >/dev/null 2>&1 && ok "--member gateway powers the gateway off" || bad "poweroff --member gateway"

if grep -qi "TypeError\|ReferenceError\|exception" "$D/server.log"; then bad "a JS error in the session"; grep -i -A3 "TypeError\|ReferenceError\|exception" "$D/server.log" | head -20; fi
[ $R = 0 ] || { echo "-- server log"; grep -i "vm\|group\|error" "$D/server.log" | tail -20; }
exit $R
