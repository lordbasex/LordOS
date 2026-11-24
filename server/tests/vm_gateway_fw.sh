#!/bin/bash
# vm_gateway_fw.sh — the Tor gateway's built-in firewall restriction end to end (docs/FIREWALL.md "The Tor gateway",
# docs/VIRTUALIZATION.md 19.5.1): a Whonix group, made and started as tests/vm_group_link.sh does (headless lord-server
# in its own user and mount namespaces, the throwaway disks WHONIX_GW and WHONIX_WS), with every process of the test
# inside build/np-torgw-test's throwaway cgroup carrying lordos:tor-gateway's policy (lord-netperm's own programs and
# compiler, private maps; never lordos.service, user@ or the running lord-netperm). Checks:
#   - the gateway's passt starts under the policy (it makes TCP sockets only) and Tor works: the workstation's
#     `curl https://check.torproject.org/api/ip` says IsTor true;
#   - with the gateway guest's own nftables flushed (its throwaway overlay), so that only the host's rule stands: a DNS
#     query over UDP to 1.1.1.1, TCP to 1.1.1.1:53 and TCP to the LAN and to this computer get nowhere, while TCP to
#     1.1.1.1:443 connects (the network path itself works).
# Needs passwordless sudo (only for the cgroup and the programs; the test itself runs as this account), KVM, QEMU, the
# built theme and the disks; exit 77 without them.
cd "$(dirname "$0")/.." || exit 1
if [ -z "${NPTOR_INNER:-}" ]; then
    THEME=../themes/default/theme.bundle.js
    [ -f "${WHONIX_GW:-}" ] && [ -f "${WHONIX_WS:-}" ] || { echo "skipped: needs WHONIX_GW and WHONIX_WS (Whonix's disks with a serial root shell)"; exit 77; }
    [ -x build/np-torgw-test ] && [ -x build/lord-server ] && [ -x build/machines ] && [ -x build/lord-vm-testpaths ] && [ -f "$THEME" ] || { echo "skipped: needs build/np-torgw-test, lord-server, machines, lord-vm-testpaths and $THEME"; exit 77; }
    [ -r /dev/kvm ] && command -v qemu-system-x86_64 >/dev/null && command -v unshare >/dev/null && command -v python3 >/dev/null || { echo "skipped: needs KVM, QEMU, unshare and python3"; exit 77; }
    sudo -n true 2>/dev/null || { echo "skipped: needs passwordless sudo (for the throwaway cgroup only)"; exit 77; }
    exec sudo -n ./build/np-torgw-test run --uid "$(id -u)" --gid "$(id -g)" -- \
        env NPTOR_INNER=1 HOME="$HOME" USER="$USER" PATH="$PATH" LANG=C.UTF-8 WHONIX_GW="$WHONIX_GW" WHONIX_WS="$WHONIX_WS" bash "$0"
fi

THEME=../themes/default/theme.bundle.js
echo "in cgroup: $(cut -d: -f3 /proc/self/cgroup)"
D=$(mktemp -d "${TMPDIR:-/var/tmp}/lgwfw.XXXXXX") || exit 1
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
NS="unshare -Urm sh -c 'mount --bind $D/fake $REALHOME && { [ -d $RT ] && mount -t tmpfs -o mode=700 tmpfs $RT || true; } && exec unshare --user --map-user=$(id -u) --map-group=$(id -g) \"\$0\" \"\$@\"'"
ns() { eval "$NS" '"$@"'; }
cleanup() {
    for m in Workstation Gateway; do ns $E ./build/lord-vm --poweroff "$G/$m.lordvm" >/dev/null 2>&1; done
    [ -n "${P:-}" ] && kill "$P" 2>/dev/null
    rm -rf "$D"
}
trap cleanup EXIT

ns $E LORDVM_TEST_CATALOG="$(realpath ../assets/machines/systems.json)" ./build/lord-vm-testpaths --group-create "$G" --catalog-id whonix \
    --dev-gateway "$WHONIX_GW" --dev-workstation "$WHONIX_WS" >"$D/create.log" 2>&1 && ok "group made" || { bad "lord-vm --group-create"; cat "$D/create.log"; exit 1; }
id_of() { sed -n 's/.*"id": "\(.*\)".*/\1/p' "$G/$1.lordvm/machine.json" | head -1; }
GW=$(id_of Gateway); WS=$(id_of Workstation)
ns $E LORD_GPU=0 ./build/lord-server --headless 1024x768 --theme "$THEME" --timeout 1500 >"$D/server.log" 2>&1 &
P=$!
for i in $(seq 1 100); do [ -S "$D/run/lordos-machines.sock" ] && break; sleep 0.1; done
sleep 2
out=$(ns $E ./build/machines start Whonix --no-window 2>&1) && ok "machines start Whonix: $out" || bad "start: $out"
grep -q "passt" "$G/Gateway.lordvm/log/lord-vm.log" && ! grep -qi "passt.*\(failed\|exited\)" "$G/Gateway.lordvm/log/lord-vm.log" && ok "the gateway's passt runs under the policy" || bad "passt: $(grep -i passt "$G/Gateway.lordvm/log/lord-vm.log" | tail -3)"

con() { # con <machine id> <timeout> <command>...  (tests/vm_group_link.sh's serial console)
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
o=$(con "$WS" 300 "for k in \$(seq 1 10); do timeout 40 curl -sS -m 35 https://check.torproject.org/api/ip && break; sleep 5; done")
echo "$o" | grep -q '"IsTor":true' && ok "workstation through Tor, with the gateway under the policy: $o" || bad "workstation through Tor: $o"

# the gateway guest's own firewall off (throwaway overlay): only the host's rule stands
HOSTIP=$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)
LANGW=$(ip -4 route show default 2>/dev/null | awk '{print $3}' | head -1)
PY='import socket,sys
def tcp(h,p):
    s=socket.socket(); s.settimeout(8)
    try: s.connect((h,p)); return "connected"
    except Exception as e: return type(e).__name__
u=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); u.settimeout(6)
u.sendto(bytes.fromhex("123401000001000000000000000001000001"),("1.1.1.1",53))
try: u.recv(512); print("udp53 answered")
except Exception as e: print("udp53", type(e).__name__)
for h,p,n in [("1.1.1.1",53,"tcp53"),("1.1.1.1",853,"tcp853"),(sys.argv[1],22,"host"),(sys.argv[2],80,"langw"),("192.168.1.1",443,"lan192"),("1.1.1.1",443,"tcp443")]:
    print(n, tcp(h,p))'
o=$(con "$GW" 120 "nft flush ruleset; echo flushed" "python3 -c '$PY' ${HOSTIP:-10.255.255.1} ${LANGW:-192.168.0.1}")
echo "$o"
echo "$o" | grep -q "flushed" || bad "the gateway's nftables weren't flushed"
echo "$o" | grep -q "^udp53 answered" && bad "a DNS query over UDP got an answer" || ok "gateway: DNS over UDP to 1.1.1.1 gets nothing"
for n in tcp53 tcp853 host langw lan192; do
    echo "$o" | grep -q "^$n connected" && bad "gateway: $n connected" || ok "gateway: $n refused ($(echo "$o" | sed -n "s/^$n //p"))"
done
echo "$o" | grep -q "^tcp443 connected" && ok "gateway: TCP 1.1.1.1:443 connects (the path works; only the rule stops the rest)" || bad "gateway: TCP 443: $(echo "$o" | grep tcp443)"
out=$(ns $E ./build/machines poweroff Whonix 2>&1) && ok "group off" || bad "poweroff: $out"
exit $R
