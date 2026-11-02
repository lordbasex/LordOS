#!/bin/bash
# netperm_cut.sh — open connections are cut when a rule changes to deny (docs/FIREWALL.md,
# "Existing connections are cut"). Run it in the test VM as the session user (passwordless sudo),
# with lord-sessiond and axia-netperm running:
#
#   bash server/tests/netperm_cut.sh [lan-host]
#
# Like netperm_integration.sh it builds an app scope by hand, writes ~/.config/axia/firewall.json
# (restored at the end) and asks axia-netperm to reload. Inside the scope it starts long downloads
# (TCP, and QUIC with curl --http3-only when the network allows UDP 443) and an idle TCP session to
# the SSH port of lan-host, then changes the rules and measures how fast the traffic stops and what
# the app sees. Then the rule goes back to allow and a new connection must work at once.
set -u
LAN=${1:-172.17.0.2}                                     # a LAN machine with an SSH port (only the banner is read)
TCP_URL=${TCP_URL:-https://deb.debian.org/debian/ls-lR.gz}
TCP_HOST=$(echo "$TCP_URL" | sed -E 's#https?://([^/]+)/.*#\1#')
QUIC_URL=${QUIC_URL:-"https://www.cloudflare.com/?n=[1-200]"}   # any HTTP/3 site; a range is many requests on one QUIC connection
ISO_DIR=https://cdimage.debian.org/debian-cd/current/amd64/iso-cd/
OTHER_URL=${OTHER_URL:-$ISO_DIR$(curl -s --max-time 8 "$ISO_DIR" | grep -o 'debian-[0-9.]*-amd64-netinst.iso' | head -1)}
U=$(id -u)
SESS=/sys/fs/cgroup/system.slice/lordos.service/lordos-session-$U
SC=$SESS/app-lordos-npcut.scope
FW=~/.config/axia/firewall.json
APP=desktop:npcut
T=$(mktemp -d)
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok:   $*"; }
bad() { fail=$((fail+1)); echo "  BAD:  $*"; }
[ -d "$SESS" ] || { echo "no LordOS session for uid $U ($SESS)"; exit 2; }
[ -S /run/lordos/netperm.sock ] || { echo "axia-netperm is not running"; exit 2; }
mkdir -p ~/.config/axia
HAD=0
[ -f "$FW" ] && { cp "$FW" "$T/fw.backup"; HAD=1; }
# ends everything still running in the scope (cgroup.kill) and the background jobs
stopall() { echo 1 | sudo tee "$SC/cgroup.kill" >/dev/null 2>&1; jobs -p | xargs -r kill 2>/dev/null; wait 2>/dev/null; }
cleanup() {
    stopall
    if [ $HAD = 1 ]; then cp "$T/fw.backup" "$FW"; else rm -f "$FW"; fi
    reload
    sleep 0.3
    sudo rmdir "$SC" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT
sudo mkdir -p "$SC"

reload() {
    python3 -c 'import socket,struct,time;s=socket.socket(socket.AF_UNIX);s.connect("/run/lordos/netperm.sock");s.sendall(struct.pack("<IB",1,1));time.sleep(0.05);s.close()'
}
rules() { echo "$1" > "$FW"; reload; }
R0='{"v":1,"mode":"silent","trusted":[],"restricted":{},"rules":[]}'
DENY='{"v":1,"mode":"silent","trusted":[],"restricted":{},"rules":[{"app":"'$APP'","dir":"out","dst":"any","action":"deny","when":0}]}'
ALLOW='{"v":1,"mode":"silent","trusted":[],"restricted":{},"rules":[{"app":"'$APP'","dir":"out","dst":"any","action":"allow","when":0}]}'

inscope() { sudo bash -c "echo \$\$ > $SC/cgroup.procs; exec setpriv --reuid=$U --regid=$(id -g) --init-groups \"\$@\"" inscope "$@"; }
now() { date +%s.%N; }
sub() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.2f", a - b }'; }
size() { stat -c %s "$1" 2>/dev/null || echo 0; }

# dl <name> <url> [curl args]: a rate-limited download in the scope, in the background (to stdout,
# so a URL range like "?n=[1-500]" is many requests on one connection)
dl() {
    local n=$1 url=$2; shift 2
    rm -f "$T/$n.data" "$T/$n.rc" "$T/$n.end" "$T/$n.err"
    ( inscope curl -4 -sSL --limit-rate 800k --max-time 120 "$@" "$url" > "$T/$n.data" 2> "$T/$n.err"; echo $? > "$T/$n.rc"; now > "$T/$n.end" ) &
}
# grows <name>: did the file grow over 1 s?
grows() { local a b; a=$(size "$T/$1.data"); sleep 1; b=$(size "$T/$1.data"); [ "$b" -gt "$a" ]; }
# stopped <name> <since>: the download ended (or stopped growing) after the rule changed
stopped() {
    local n=$1 t0=$2 i
    for i in $(seq 1 40); do [ -f "$T/$n.rc" ] && break; sleep 0.1; done
    if [ -f "$T/$n.rc" ]; then
        printf '%s %.2f s, curl exit %s: %s' "ended after" "$(sub "$(cat "$T/$n.end")" "$t0")" "$(cat "$T/$n.rc")" "$(tail -1 "$T/$n.err")"
        return 0
    fi
    local a b; a=$(size "$T/$n.data"); sleep 1; b=$(size "$T/$n.data")
    [ "$a" = "$b" ] && { printf 'no bytes for 1 s (still waiting)'; return 0; }
    printf 'still receiving'; return 1
}
# idle <name> <host> <port>: a TCP session that reads the banner and then waits; prints what recv got
idle() {
    local n=$1
    rm -f "$T/$n.out"
    ( inscope python3 -c '
import socket, sys, time, errno
s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=5); s.recv(64)
print("open", flush=True); s.settimeout(12)
try:
    d = s.recv(64); r = "eof" if not d else "data"
except socket.timeout: r = "timeout"
except OSError as e: r = errno.errorcode.get(e.errno, str(e.errno))
print(r, "%.3f" % time.time(), flush=True)' "$2" "$3" > "$T/$n.out" 2>&1 ) &
}
idle_wait() { local i; for i in $(seq 1 50); do grep -q open "$T/$1.out" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
idle_result() { local i; for i in $(seq 1 ${2:-30}); do [ "$(wc -l < "$T/$1.out")" -ge 2 ] && break; sleep 0.1; done; sed -n 2p "$T/$1.out"; }

counts() {   # allowed denied blocked of the app, from one COUNTS message
    python3 - "$APP" <<'PY'
import socket, struct, sys, json, time
s = socket.socket(socket.AF_UNIX); s.connect("/run/lordos/netperm.sock"); s.settimeout(3)
buf = b""; end = time.time() + 3
while time.time() < end:
    try: d = s.recv(1 << 20)
    except socket.timeout: break
    if not d: break
    buf += d
    while len(buf) >= 5:
        l, t = struct.unpack("<IB", buf[:5])
        if len(buf) < 4 + l: break
        body = buf[5:4 + l]; buf = buf[4 + l:]
        if t == 12:
            v = json.loads(body).get(sys.argv[1])
            if v: print(v[0], v[1], v[7] if len(v) > 7 else "-"); sys.exit(0)
print("- - -")
PY
}

rules "$R0"

echo "1. app-wide deny cuts a TCP download and an idle TCP session"
dl tcp "$TCP_URL"
idle ssh "$LAN" 22
sleep 2.5
grows tcp && ok "the TCP download runs" || bad "the TCP download doesn't run ($(cat "$T/tcp.err"))"
idle_wait ssh && ok "the idle session to $LAN:22 is open" || bad "no session to $LAN:22 ($(cat "$T/ssh.out"))"
t0=$(now); rules "$DENY"
r=$(stopped tcp "$t0") && ok "TCP download: $r" || bad "TCP download: $r"
grep -q "Software caused connection abort\|reset\|Recv failure" "$T/tcp.err" && ok "curl saw the connection end (not a stall)" || bad "curl: $(cat "$T/tcp.err")"
res=$(idle_result ssh)
case "$res" in ECONNABORTED*|ECONNRESET*) ok "idle session: $res (cut after $(sub "${res#* }" "$t0") s)";; *) bad "idle session: $res";; esac
read -r al de bl < <(counts)
[ "$de" != "-" ] && [ "$de" -ge 1 ] && ok "counters: allowed $al, denied $de, blocked $bl bytes" || bad "counters: $al $de $bl"
t1=$(now); rules "$ALLOW"
inscope curl -4 -sS -o /dev/null --max-time 8 -w '%{http_code}' "https://$TCP_HOST/" > "$T/again" 2>&1
grep -q '^[23]' "$T/again" && ok "allowed again: a new connection works at once ($(cat "$T/again"), $(sub "$(now)" "$t1") s)" || bad "allowed again: $(cat "$T/again")"

echo "2. QUIC (HTTP/3, connected UDP)"
rules "$R0"
dl quic "$QUIC_URL" --http3-only
sleep 3
if grows quic; then
    ok "the HTTP/3 download runs ($(ss -Huanp 2>/dev/null | grep -c ':443') UDP sockets to :443)"
    t0=$(now); rules "$DENY"
    r=$(stopped quic "$t0") && ok "HTTP/3 download: $r" || bad "HTTP/3 download: $r"
    rules "$ALLOW"
    inscope curl -4 -sS --http3-only -o /dev/null --max-time 8 -w '%{http_version}' "${QUIC_URL%%\?*}" > "$T/again" 2>&1
    grep -q '^3' "$T/again" && ok "allowed again: a new HTTP/3 request works" || bad "allowed again over HTTP/3: $(cat "$T/again")"
else
    echo "  skipped: no HTTP/3 here ($(tail -1 "$T/quic.err"))"
fi
stopall

echo "3. a domain rule cuts only that destination"
rules "$R0"
dl tcp "$TCP_URL"
dl other "$OTHER_URL"
sleep 3
grows tcp && grows other && ok "both downloads run" || bad "the downloads don't run"
t0=$(now)
rules '{"v":1,"mode":"silent","trusted":[],"restricted":{},"rules":[{"app":"'$APP'","dir":"out","dst":"'$TCP_HOST'","action":"deny","when":0}]}'
r=$(stopped tcp "$t0") && ok "$TCP_HOST: $r" || bad "$TCP_HOST: $r"
[ ! -f "$T/other.rc" ] && grows other && ok "the other destination keeps downloading" || bad "the other destination stopped too ($(cat "$T/other.err"))"
stopall

echo "4. \"no internet\" cuts the internet, not the LAN"
rules "$R0"
dl tcp "$TCP_URL"
idle ssh "$LAN" 22
sleep 2.5
idle_wait ssh || bad "no session to $LAN:22"
t0=$(now)
rules '{"v":1,"mode":"silent","trusted":[],"restricted":{"'$APP'":["no-internet"]},"rules":[]}'
r=$(stopped tcp "$t0") && ok "internet download: $r" || bad "internet download: $r"
res=$(idle_result ssh 20)
[ -z "$res" ] && ok "the LAN session stays open" || bad "the LAN session ended: $res"
stopall

echo "5. a server in the scope keeps its LAN clients when outgoing internet is denied"
if [ -n "${CLIENT_SSH:-}" ]; then        # e.g. CLIENT_SSH="ssh -i key root@172.17.0.2": a LAN machine runs the client
    rules "$R0"
    ( inscope python3 -c '
import socket
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("0.0.0.0", 18093)); s.listen()
c, _ = s.accept(); c.settimeout(8); n = 0
try:
    while True:
        d = c.recv(65536)
        if not d: break
        n += len(d)
except OSError: pass
print(n, flush=True)' > "$T/srv.out" 2>&1 ) &
    sleep 1
    IP=$(ip -4 -o addr show scope global | awk 'NR==1{split($4,a,"/");print a[1]}')
    $CLIENT_SSH "python3 -c 'import socket,time
c=socket.create_connection((\"$IP\",18093)); end=time.time()+4
while time.time()<end: c.sendall(b\"x\"*1000); time.sleep(0.01)
c.close()'" &
    sleep 1
    rules '{"v":1,"mode":"silent","trusted":[],"restricted":{},"rules":[{"app":"'$APP'","dir":"out","dst":"any","action":"deny","when":0}]}'
    for i in $(seq 1 120); do [ -s "$T/srv.out" ] && break; sleep 0.1; done
    n=$(cat "$T/srv.out" 2>/dev/null)
    [ "${n:-0}" -gt 300000 ] && ok "the server received ${n} bytes (4 s, the rule changed after 1 s)" || bad "the server got ${n:-nothing}"
    stopall
else
    echo "  skipped: set CLIENT_SSH to a command that runs python3 on a LAN machine"
fi

echo "netperm-cut: $pass passed, $fail failed"
[ $fail = 0 ]
