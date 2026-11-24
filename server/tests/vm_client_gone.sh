#!/bin/sh
# A client of a running machine's lord-vm that shut its reading side and kept its socket (shutdown SHUT_RD) never hangs
# up, so poll never says it went: lord-vm used to keep it for the machine's whole run and refuse every broadcast to it
# again ("message 71 not delivered" in a loop, 2026-11-24). Now the first refused send marks it and the next loop drops
# it. A real KVM boot of the harness's agent image (its qemu-guest-agent connecting is a broadcast).
#   sh ../tools/vm/e2e/machines/ns-run.sh sh tests/vm_client_gone.sh BUILD_DIR AGENT_QCOW2   (from server/; not in make test)
set -u
B=$(cd "$1" && pwd) || exit 2
IMG=$2
[ -n "${MH_NS_DIR:-}" ] || { echo "skipped: run it through tools/vm/e2e/machines/ns-run.sh"; exit 0; }
[ -c /dev/kvm ] && [ -w /dev/kvm ] || { echo "skipped: needs KVM"; exit 0; }
W=$MH_NS_DIR/gone
D=$W/x.lordvm
mkdir -p "$W"
"$B/lord-vm" --create "$D" --name client-gone --import "$IMG" --vcpus 1 --memory 1024 >/dev/null 2>&1 || { echo "FAIL  create"; exit 1; }
"$B/lord-vm" --set "$D" --agent on >/dev/null 2>&1
ID=$(sed -n 's/.*"id": "\([^"]*\)".*/\1/p' "$D/machine.json" | head -n1)
mkfifo "$W/in"
"$B/lordvm-dev" run "$D" <"$W/in" >"$W/out" 2>&1 &
exec 7>"$W/in"
sleep 6
python3 -I -c 'import socket, sys, time
h = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); h.connect(sys.argv[1])
h.shutdown(socket.SHUT_RD)        # reads no more, stays open
time.sleep(120)' "$XDG_RUNTIME_DIR/lordos-vm/$ID.sock" &
HP=$!
end=$(( $(date +%s) + 90 ))
while [ "$(date +%s)" -lt "$end" ] && ! grep -q "agent: connected" "$D/log/lord-vm.log"; do sleep 1; done
sleep 3
echo poweroff >&7
sleep 3
kill $HP 2>/dev/null
n=$(grep -c "not delivered" "$D/log/lord-vm.log")
pass=0
if grep -q "dropped (it can't receive)" "$D/log/lord-vm.log" && [ "$n" -le 2 ]; then echo "PASS  the half-closed client is dropped ($n refused sends)"; pass=1
else echo "FAIL  the half-closed client: $n refused sends, $(grep -c 'dropped' "$D/log/lord-vm.log") drops"; fi
[ "$pass" = 1 ]
