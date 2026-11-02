#!/bin/bash
# netperm_integration.sh — test of axia-netperm with the real kernel (docs/FIREWALL.md, step 2).
# Run it in the test VM as the session user (with passwordless sudo), with lord-sessiond
# (Delegate=yes) and axia-netperm running:
#
#   bash server/tests/netperm_integration.sh [gateway]
#
# Builds an app scope by hand (as the axia-server launcher does), writes rules to
# ~/.config/axia/firewall.json, asks axia-netperm to reload them over its socket and tests:
# deny and allow, a domain rule that leaves a single site, "LAN only", "no incoming
# connections" and "no network". It only uses the gateway, the machine itself and example.com/debian.org.
# At the end it leaves firewall.json as it was.
set -u
GW=${1:-172.17.0.1}
U=$(id -u)
SESS=/sys/fs/cgroup/system.slice/lordos.service/lordos-session-$U
SC=$SESS/app-lordos-nptest.scope
FW=~/.config/axia/firewall.json
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok:   $*"; }
bad() { fail=$((fail+1)); echo "  BAD:  $*"; }
[ -d "$SESS" ] || { echo "no LordOS session for uid $U ($SESS)"; exit 2; }
[ -S /run/lordos/netperm.sock ] || { echo "axia-netperm is not running"; exit 2; }
mkdir -p ~/.config/axia
BACKUP=$(mktemp); HAD=0
[ -f "$FW" ] && { cp "$FW" "$BACKUP"; HAD=1; }
trap 'if [ $HAD = 1 ]; then cp "$BACKUP" "$FW"; else rm -f "$FW"; fi; reload; rm -f "$BACKUP"' EXIT
sudo mkdir -p "$SC" && sudo chown "$U" "$SC" "$SC/cgroup.procs" 2>/dev/null

reload() {
    python3 - <<'PY'
import socket, struct, time
s = socket.socket(socket.AF_UNIX); s.connect('/run/lordos/netperm.sock')
s.sendall(struct.pack('<IB', 1, 1)); time.sleep(0.3); s.close()
PY
}
rules() { echo "$1" > "$FW"; reload; }

# runs python inside the scope, as the user
py() { sudo bash -c "echo \$\$ > $SC/cgroup.procs; exec setpriv --reuid=$U --regid=$(id -g) --init-groups python3 -c \"\$0\" \"\$@\"" "$@"; }
TRY='
import socket, sys, errno, ssl
kind, host, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
try:
    if kind == "bind":
        s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind((host, port)); s.listen(); print("ok")
    elif kind == "udp":
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.sendto(b"x", (host, port)); print("ok")
    elif kind == "sock":
        socket.socket(); print("ok")
    elif kind == "https":
        ai = socket.getaddrinfo(host, port, socket.AF_INET, socket.SOCK_STREAM)[0][4]
        s = socket.create_connection(ai, timeout=6)
        c = ssl.create_default_context().wrap_socket(s, server_hostname=host)
        c.sendall(b"HEAD / HTTP/1.1\r\nHost: " + host.encode() + b"\r\nConnection: close\r\n\r\n")
        print("ok", c.recv(12).decode(errors="replace"))
    else:
        s = socket.create_connection((host, port), timeout=4); print("ok")
except OSError as e:
    print(errno.errorcode.get(e.errno, str(e.errno)))
'
try() { py "$TRY" "$@" 2>&1 | tail -1; }
expect() {      # expect <result> <kind> <host> <port> <text>
    local want=$1; shift; local what=$4; local got
    got=$(try "$1" "$2" "$3")
    case "$got" in $want*) ok "$what → $got";; *) bad "$what → $got (expected $want)";; esac
}
APP=desktop:nptest
R='{"v":1,"mode":"silent","trusted":[],"restricted":{},"rules":[]}'

echo "1. no rules (silent): allowed"
rules "$R"
expect ok tcp "$GW" 53 "TCP to the gateway"

echo "2. deny the app → EPERM; allow it → works"
rules '{"v":1,"mode":"silent","trusted":[],"restricted":{},"rules":[{"app":"'$APP'","dir":"out","dst":"any","action":"deny","when":0}]}'
expect EPERM tcp "$GW" 53 "TCP denegado"
expect EPERM udp "$GW" 53 "UDP (sendto) denegado"
expect EPERM tcp ::1 22 "TCP over IPv6 denied"
rules '{"v":1,"mode":"silent","trusted":[],"restricted":{},"rules":[{"app":"'$APP'","dir":"out","dst":"any","action":"allow","when":0}]}'
expect ok tcp "$GW" 53 "TCP allowed again"

echo "3. domain rule: example.com only"
rules '{"v":1,"mode":"silent","trusted":[],"restricted":{},"rules":[{"app":"'$APP'","dir":"out","dst":"any","action":"deny","when":0},{"app":"'$APP'","dir":"out","dst":"example.com","action":"allow","when":0}]}'
expect "ok HTTP" https example.com 443 "https://example.com (allowed by name)"
expect EPERM https www.debian.org 443 "https://www.debian.org (not in the rule)"
IP=$(getent ahostsv4 example.com | awk 'NR==1{print $1}')
expect ok tcp "$IP" 443 "the example.com IP the resolver returned"
expect EPERM tcp 93.184.215.14 443 "an IP the resolver did not return for example.com"

echo "4. LAN only (no internet)"
rules '{"v":1,"mode":"silent","trusted":[],"restricted":{"'$APP'":["no-internet"]},"rules":[]}'
expect ok tcp "$GW" 53 "the gateway"
expect EPERM https example.com 443 "internet"

echo "5. no incoming connections"
rules '{"v":1,"mode":"silent","trusted":[],"restricted":{"'$APP'":["no-incoming"]},"rules":[]}'
expect EPERM bind 0.0.0.0 18080 "bind/listen on 18080"
expect ok bind 0.0.0.0 0 "bind to an ephemeral port (client)"
expect ok tcp "$GW" 53 "salir sigue andando"

echo "6. no network"
rules '{"v":1,"mode":"silent","trusted":[],"restricted":{"'$APP'":["no-network"]},"rules":[]}'
expect EPERM sock 0 0 "create a socket"

echo "7. off: only explicit denials apply"
rules '{"v":1,"mode":"off","trusted":[],"restricted":{},"rules":[{"app":"'$APP'","dir":"out","dst":"'$GW'","port":"53","proto":"tcp","action":"deny","when":0}]}'
expect EPERM tcp "$GW" 53 "the explicit denial"
expect ok tcp "$GW" 22 "everything else"

echo "8. another app does not inherit the rules"
rules '{"v":1,"mode":"silent","trusted":[],"restricted":{},"rules":[{"app":"'$APP'","dir":"out","dst":"any","action":"deny","when":0}]}'
got=$(python3 -c 'import socket;socket.create_connection(("'$GW'",53),timeout=4);print("ok")' 2>&1 | tail -1)
[ "$got" = ok ] && ok "outside the scope → ok" || bad "outside the scope → $got"

echo "9. connection notices carry the names the resolver gave (hosts)"
rules '{"v":1,"mode":"silent","trusted":[],"restricted":{},"rules":[{"app":"'$APP'","dir":"out","dst":"example.com","action":"deny","when":0}]}'
HOSTPY='
import socket, struct, sys, time, json
s = socket.socket(socket.AF_UNIX); s.connect("/run/lordos/netperm.sock"); s.settimeout(8)
buf = b""; end = time.time() + 8
while time.time() < end:
    try: d = s.recv(65536)
    except socket.timeout: break
    if not d: break
    buf += d
    while len(buf) >= 5:
        l, t = struct.unpack("<IB", buf[:5])
        if len(buf) < 4 + l: break
        body = buf[5:4 + l]; buf = buf[4 + l:]
        if t == 11:
            ev = json.loads(body)
            if ev.get("app") == sys.argv[1] and ev.get("hosts"): print(" ".join(ev["hosts"])); sys.exit(0)
'
for attempt in 1 2 3; do          # the resolver may answer from its cache: try a few times
    python3 -c "$HOSTPY" "$APP" > /tmp/np-hosts-$U.txt &
    sleep 1
    try https example.com 443 >/dev/null
    wait
    grep -q "example.com" /tmp/np-hosts-$U.txt && break
    sleep 5.5                        # a repeated notice comes again only after 5 s
done
grep -q "example.com" /tmp/np-hosts-$U.txt && ok "the notice names example.com ($(cat /tmp/np-hosts-$U.txt))" || bad "no host names in the notice"
rm -f /tmp/np-hosts-$U.txt

echo "netperm-integration: $pass passed, $fail failed"
sudo rmdir "$SC" 2>/dev/null
[ $fail = 0 ]
