#!/bin/bash
# machines_shell_test.sh — `machines shell` and `machines console` (docs/VIRTUALIZATION.md 6.15, 6.18) against a stand-in
# lord-vm: a Python listener on the machine's socket ($XDG_RUNTIME_DIR/lordos-vm/<id>.sock, SOCK_SEQPACKET, the fixed
# VmMsg of vmproto.h) that answers VM_SHELL (VSH_OPEN, the terminal's size: 80×24 without one) or VM_CONSOLE with one end of
# a socket pair and echoes what comes through it, while sending other clients' news on the control socket. Checks: the
# request's kind and size, what is typed reaches the stream and what comes back is printed, news on the socket doesn't
# stop the relay, a stream that ends ends the command; a machine whose lord-vm isn't there says it isn't running.
# No QEMU, KVM or guest. Run from server/ (tests/run_suites.sh does). Exit 77 without build/machines, a C compiler or python3.
cd "$(dirname "$0")/.." || exit 1
[ -x build/machines ] && command -v python3 >/dev/null 2>&1 && command -v cc >/dev/null 2>&1 || { echo "skipped: needs build/machines, cc and python3"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/tmp}/mshell.XXXXXX") || exit 1
L=''
trap 'kill $L 2>/dev/null; rm -rf "$D"' EXIT
mkdir -p "$D/home/Machines/T.lordvm" "$D/run/lordos-vm" "$D/py"
chmod 700 "$D/run" "$D/run/lordos-vm"
ID=0b6c1a2e-1111-4222-8333-444455556666
cat >"$D/home/Machines/T.lordvm/machine.json" <<EOF
{"schema": 1, "id": "$ID", "name": "T", "arch": "amd64", "accel": "kvm", "vcpus": 1, "memory_mb": 512, "disk_gb": 1,
 "firmware": "bios", "machine_type": "pc-q35-10.0", "display": "none", "network": "isolated", "mac": "52:54:00:12:34:56",
 "created": "2026-11-24T00:00:00Z", "updated": "2026-11-24T00:00:00Z"}
EOF
# the protocol's numbers, from the header itself
printf '#include "vm/vmproto.h"\n#include <stdio.h>\nint main(void){printf("%%d %%d %%d %%d %%d %%d\\n",VM_SHELL,VM_SHELL_FD,VM_CONSOLE,VM_CONSOLE_FD,VM_PROTO_VERSION,(int)sizeof(VmMsg));return 0;}\n' >"$D/n.c"
cc -D_GNU_SOURCE -Isrc -o "$D/n" "$D/n.c" || exit 1
set -- $("$D/n")
cat >"$D/py/vm.py" <<'EOF'
import array, os, socket, struct, sys
path, out = sys.argv[1], sys.argv[2]
SHELL, SHELL_FD, CONSOLE, CONSOLE_FD, VER, SIZE = map(int, sys.argv[3:9])
FMT = '<HHIiiii%ds' % (SIZE - 24)
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.bind(path); os.chmod(path, 0o600); s.listen(4)
log = open(out, 'w', buffering=1)
for round in range(2):
    c, _ = s.accept()
    t, v, seq, a, b, cc, d, _ = struct.unpack(FMT, c.recv(SIZE))
    log.write('req %d a=%d b=%d c=%d\n' % (t, a, b, cc))
    x, y = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    c.send(struct.pack(FMT, 65, VER, 0, 2, 0, 0, 0, b''))             # someone else's news first (VM_STATE)
    kind = SHELL_FD if t == SHELL else CONSOLE_FD
    c.sendmsg([struct.pack(FMT, kind, VER, seq, 0, 0, 0, 0, b'')], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [x.fileno()]))])
    x.close()
    y.sendall(b'prompt$ ')
    for _ in range(5):
        c.send(struct.pack(FMT, 65, VER, 0, 2, 0, 0, 0, b''))         # more news while it relays
    data = y.recv(4096)
    log.write('got %r\n' % data)
    y.sendall(b'ECHO:' + data)
    y.close()
    c.close()
EOF
python3 -I "$D/py/vm.py" "$D/run/lordos-vm/$ID.sock" "$D/log" "$@" &
L=$!
for i in $(seq 1 50); do [ -S "$D/run/lordos-vm/$ID.sock" ] && break; sleep 0.1; done
E="env -u DISPLAY -u WAYLAND_DISPLAY HOME=$D/home XDG_RUNTIME_DIR=$D/run LANG=C.UTF-8"
R=0
o=$( (sleep 0.5; printf 'hello\n') | timeout 10 $E build/machines shell T)
rc=$?
case "$o" in *"prompt\$ "*"ECHO:hello"*) echo "ok: shell: typed reaches the guest's stream and its answer is printed (rc $rc)" ;; *) echo "FAIL: shell: $o"; R=1 ;; esac
grep -q "^req $1 a=1 b=80 c=24$" "$D/log" && echo "ok: VM_SHELL VSH_OPEN 80x24 without a terminal" || { echo "FAIL: shell request: $(cat "$D/log")"; R=1; }
o=$( (sleep 0.5; printf 'root\n') | timeout 10 $E build/machines console T)
case "$o" in *"ECHO:root"*) echo "ok: console relays both ways" ;; *) echo "FAIL: console: $o"; R=1 ;; esac
grep -q "^req $3 " "$D/log" && echo "ok: VM_CONSOLE asked" || { echo "FAIL: console request"; R=1; }
wait $L 2>/dev/null; L=''
o=$(timeout 10 $E build/machines shell T 2>&1 </dev/null); rc=$?
[ $rc = 3 ] && echo "ok: no lord-vm: not running (3)" || { echo "FAIL: no lord-vm ($rc): $o"; R=1; }
exit $R
