#!/bin/bash
# netperm_measure.sh — connect() and throughput with axia-netperm off, on with no rules and on
# with 400 rules (docs/FIREWALL.md, "Measurements"). In the VM, as the session user:
#   bash server/tests/netperm_measure.sh
# Everything is local (127.0.0.1): nothing is downloaded from the internet.
set -u
cd "$(dirname "$0")/.."
cc -O2 -o /tmp/netperm-bench tests/netperm_bench.c || exit 1
U=$(id -u)
SC=/sys/fs/cgroup/system.slice/lordos.service/lordos-session-$U/app-lordos-npbench.scope
FW=~/.config/axia/firewall.json
BACKUP=$(mktemp); HAD=0
[ -f "$FW" ] && { cp "$FW" "$BACKUP"; HAD=1; }
reload() { python3 -c 'import socket,struct,time;s=socket.socket(socket.AF_UNIX);s.connect("/run/lordos/netperm.sock");s.sendall(struct.pack("<IB",1,1));time.sleep(0.5)'; }
finish() { if [ $HAD = 1 ]; then cp "$BACKUP" "$FW"; else rm -f "$FW"; fi; sudo systemctl start axia-netperm; sleep 1; reload; sudo rmdir "$SC" 2>/dev/null; rm -f "$BACKUP"; }
trap finish EXIT
sudo mkdir -p "$SC"
inscope() { sudo bash -c "echo \$\$ > $SC/cgroup.procs; exec setpriv --reuid=$U --regid=$(id -g) --init-groups $*"; }
thru() {        # 3 GB over TCP on 127.0.0.1, inside the scope
    (dd if=/dev/zero bs=1M count=3072 status=none | nc -N -l 127.0.0.1 9107) &
    sleep 0.5
    inscope "bash -c 'nc 127.0.0.1 9107 | dd of=/dev/null bs=1M 2>&1 | tail -1'"
    wait
}
run() {
    echo "== $1"
    for i in 1 2 3; do inscope /tmp/netperm-bench 20000; done
    thru
}
sudo systemctl stop axia-netperm; sleep 0.5
run "axia-netperm off"
sudo systemctl start axia-netperm; sleep 1.5
echo '{"v":1,"mode":"silent","trusted":[],"restricted":{},"rules":[]}' > "$FW"; reload
run "on, no rules (silent: every new connection is counted and reported)"
python3 - "$FW" <<'PY'
import json, sys
rules = [{"app": "desktop:npbench", "dir": "out", "dst": "any", "action": "deny", "when": 0},
         {"app": "desktop:npbench", "dir": "out", "dst": "127.0.0.0/8", "action": "allow", "when": 0}]
for i in range(200):
    rules.append({"app": "desktop:npbench", "dir": "out", "dst": f"10.{i // 256}.{i % 256}.0/24", "action": "deny" if i % 2 else "allow", "when": 0})
    rules.append({"app": "desktop:npbench", "dir": "out", "dst": f"*.sitio{i}.com", "action": "allow", "when": 0})
json.dump({"v": 1, "mode": "silent", "trusted": [], "restricted": {}, "rules": rules}, open(sys.argv[1], "w"), separators=(",", ":"))
PY
reload
run "on, 402 rules for the app (networks, wildcards, deny all except 127/8)"
