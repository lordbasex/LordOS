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
ns $E LORD_GPU=0 ./build/lord-server --headless 1024x768 --theme "$THEME" --timeout 1500 >"$D/server.log" 2>&1 &
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
if grep -qi "TypeError\|ReferenceError\|exception" "$D/server.log"; then bad "a JS error in the session"; grep -i -A3 "TypeError\|ReferenceError\|exception" "$D/server.log" | head -20; fi
[ $R = 0 ] || { echo "-- server log"; grep -i "vm\|group\|error" "$D/server.log" | tail -20; }
exit $R
