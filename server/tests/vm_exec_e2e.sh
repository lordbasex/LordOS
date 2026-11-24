#!/bin/bash
# vm_exec_e2e.sh — `machines exec`, `cp`, `logs` and `mcp` against a real running Linux machine (docs/VIRTUALIZATION.md
# 6.15): the program runs as the guest's signed-in person, its input, output, exit status, timeout and cut output; its
# arguments stay arguments; cp both ways with the SHA-256 checked, refusing what lies outside the person's files, a
# protected path, a link out of them and an existing file without --force; lord-vm's log names each program and never
# its arguments; the MCP server's handshake, tools, run_command, parse errors and unknown arguments.
#
# It needs a machine nobody else uses: VM_EXEC_NAME (its name in $HOME/Machines), running, with "automation": true in its
# machine.json, qemu-guest-agent answering, and VM_EXEC_USER, the person signed in to it. Run it with a private HOME and in
# private namespaces (the pattern of tests/vm_group_link.sh), never in the person's own session. VM_EXEC_OFF: optional,
# another machine of that library with automation off (its MCP call must be refused). Exit 77 without them.
# Checked on 2026-11-24 with the Ubuntu 26.04.1 server machine of the Ubuntu card (unattended install), 24 of 24.
cd "$(dirname "$0")/.." || exit 1
B=$(pwd)/build
[ -n "${VM_EXEC_NAME:-}" ] && [ -n "${VM_EXEC_USER:-}" ] && [ -x "$B/machines" ] && command -v python3 >/dev/null 2>&1 || { echo "skipped: needs VM_EXEC_NAME, VM_EXEC_USER, build/machines and python3"; exit 77; }
N=$VM_EXEC_NAME
U=$VM_EXEC_USER
DIR=$("$B/machines" status "$N" --json | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["folder"])') || exit 1
W=$(mktemp -d "$HOME/vmexec.XXXXXX") || exit 1
trap 'rm -rf "$W"' EXIT
cd "$W" || exit 1
R=0
ok() { echo "ok: $*"; }
bad() { echo "FAIL: $*"; R=1; }
o=$($B/machines exec "$N" -- id -un); [ "$o" = "$U" ] && ok "runs as the guest's person ($o), not root" || bad "id: $o"
o=$($B/machines exec "$N" -- sh -c 'echo "$HOME|$PWD"'); [ "$o" = "/home/$U|/home/$U" ] && ok "home and cwd: $o" || bad "home: $o"
o=$(printf 'abc\n' | $B/machines exec "$N" -- tr a-z A-Z); [ "$o" = ABC ] && ok "stdin piped" || bad "stdin: $o"
$B/machines exec "$N" -- sh -c 'exit 7'; [ $? = 7 ] && ok "exit status passed" || bad "exit status"
o=$($B/machines exec "$N" -- printf '%s|' 'a b' '$(id)' "'q'" ';ls'); [ "$o" = "a b|\$(id)|'q'|;ls|" ] && ok "arguments stay arguments: $o" || bad "args: $o"
$B/machines exec "$N" --timeout 2 -- sleep 30; [ $? = 124 ] && ok "timeout stops it (124)" || bad "timeout"
n=$($B/machines exec "$N" -- head -c 300000 /dev/zero | wc -c); [ "$n" = 131072 ] && ok "output cut at 128 KiB" || bad "cut: $n"
$B/machines exec "$N" -- no-such-program-xyz; [ $? = 127 ] && ok "missing program: 127" || bad "127"
$B/machines exec "$N" -- sudo -n true 2>/dev/null; [ $? != 0 ] && ok "no root without a password (sudo -n fails)" || bad "sudo -n worked"
$B/machines exec "$N" -- rm -f vmexec-big.bin
head -c 3000000 /dev/urandom > vmexec-big.bin
$B/machines cp vmexec-big.bin "$N:~/" && ok "cp into the machine" || bad "cp in"
g=$($B/machines exec "$N" -- sha256sum vmexec-big.bin | cut -d' ' -f1); h=$(sha256sum vmexec-big.bin | cut -d' ' -f1)
[ "$g" = "$h" ] && ok "same SHA-256 in the guest" || bad "hash in: $g vs $h"
$B/machines cp vmexec-big.bin "$N:~/"; [ $? != 0 ] && ok "existing file refused without --force" || bad "overwrite without force"
$B/machines cp --force vmexec-big.bin "$N:~/vmexec-big.bin" && ok "--force replaces" || bad "force"
$B/machines cp "$N:vmexec-big.bin" back.bin && cmp -s vmexec-big.bin back.bin && ok "cp out of the machine, same bytes" || bad "cp out"
$B/machines cp "$N:vmexec-big.bin" back.bin; [ $? != 0 ] && ok "existing host file refused" || bad "host overwrite"
$B/machines cp "$N:/etc/shadow" sh.txt; [ $? != 0 ] && [ ! -e sh.txt ] && ok "a file the guest person can't read stays there" || bad "shadow"
$B/machines cp "$N:vmexec-big.bin" /etc/x.bin; [ $? != 0 ] && ok "outside the person's files refused" || bad "/etc"
mkdir -p "$HOME/.config/lordos"; s0=$(sha256sum "$HOME/.config/lordos/settings.json" 2>/dev/null)
$B/machines cp --force "$N:vmexec-big.bin" "$HOME/.config/lordos/settings.json"
[ $? != 0 ] && [ "$(sha256sum "$HOME/.config/lordos/settings.json" 2>/dev/null)" = "$s0" ] && ok "a protected path refused, even with --force" || bad "protected"
ln -sf /etc/passwd link.txt; $B/machines cp link.txt "$N:~/"; [ $? != 0 ] && ok "a link out of the files refused" || bad "symlink read"
$B/machines cp vmexec-big.bin back.bin; [ $? = 2 ] && ok "two local sides refused" || bad "two local"
$B/machines exec "$N" -- rm -f vmexec-big.bin
$B/machines logs "$N" --machine --lines 5 | grep -q "automation: program" && ok "lord-vm's log names each program" || bad "log"
grep -q "exec: id" "$DIR/log/automation.log" && ok "automation.log written" || bad "automation.log"
! grep -q "\$(id)" "$DIR/log/lord-vm.log" && ok "arguments never in the log" || bad "args in log"
J=$(python3 -I -c 'import json,sys; print(json.dumps(sys.argv[1]))' "$N")
JO=$(python3 -I -c 'import json,sys; print(json.dumps(sys.argv[1]))' "${VM_EXEC_OFF:-no such machine}")
o=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"1"}}}' \
  '{"jsonrpc":"2.0","method":"notifications/initialized"}' '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"run_command\",\"arguments\":{\"name\":$J,\"command\":[\"sh\",\"-c\",\"cat; id -un\"],\"input\":\"hi\\n\"}}}" \
  '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"list_machines","arguments":{}}}' \
  'not json' "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"run_command\",\"arguments\":{\"name\":$J,\"command\":[\"true\"],\"evil\":1}}}" \
  "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"get_logs\",\"arguments\":{\"name\":$J,\"which\":\"machine\",\"lines\":3}}}" \
  "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"run_command\",\"arguments\":{\"name\":$JO,\"command\":[\"true\"]}}}" | $B/machines mcp 2>/dev/null)
echo "$o" | python3 -I -c '
import json,sys
user, name = sys.argv[1], sys.argv[2]
ls=[json.loads(l) for l in sys.stdin if l.strip()]
by={m.get("id"):m for m in ls}
assert by[1]["result"]["protocolVersion"]=="2025-06-18"
assert len(by[2]["result"]["tools"])==8
r=json.loads(by[3]["result"]["content"][0]["text"]); assert r["stdout"]=="hi\n"+user+"\n" and r["exit"]==0, r
assert name in [m["name"] for m in json.loads(by[4]["result"]["content"][0]["text"])]
assert any(m.get("error",{}).get("code")==-32700 for m in ls)
assert by[5]["error"]["code"]==-32602
assert not by[6]["result"]["isError"]
assert by[7]["result"]["isError"]
' "$U" "$N" && ok "MCP: initialize, tools/list, run_command, list, parse errors, unknown argument, logs, a machine without automation" || bad "mcp: $o"
exit $R
