#!/bin/sh
# migration_guest_port_test.sh — packaging/migrations/012-0.8.9.6-guest-agent-port.sh over fake system trees
# (LORDOS_MIGRATION_ROOT): only a tree with /dev/virtio-ports/org.lordos.agent (a Machines guest whose agent is on) is
# acted on; a computer without the port, or with only qemu-guest-agent's, is left alone; it passes `sh -eu` (as
# packaging/migrations/migrate runs it) twice. And the package's udev rule gives the port to the video group too. Run
# from server/ by tests/run_suites.sh.
set -u
TOP=$(cd "$(dirname "$0")/../.." && pwd)
M=$TOP/packaging/migrations/012-0.8.9.6-guest-agent-port.sh
T=$(mktemp -d "${TMPDIR:-/tmp}/lord-mig012.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
fails=0 n=0
# case NAME PORTS EXPECTED: a tree with those ports (space-separated names), the migration run twice, what it said
case_() {
    n=$((n + 1))
    R="$T/$1"
    mkdir -p "$R/dev/virtio-ports"
    for p in $2; do : > "$R/dev/virtio-ports/$p"; done
    for run in 1 2; do
        out=$(LORDOS_MIGRATION_ROOT=$R sh -eu "$M") || { echo "FAIL $1: run $run exited with $?"; fails=$((fails + 1)); return; }
        [ "$out" = "$3" ] || { echo "FAIL $1: run $run said '$out', expected '$3'"; fails=$((fails + 1)); return; }
    done
}
case_ guest "org.qemu.guest_agent.0 org.lordos.agent" "would apply"
case_ guest-lordos-port-only "org.lordos.agent" "would apply"
case_ computer "" ""
case_ qga-only "org.qemu.guest_agent.0" ""
n=$((n + 1))
grep -q '^SUBSYSTEM=="virtio-ports", ATTR{name}=="org.lordos.agent", TAG+="uaccess", GROUP="video", MODE="0660"$' "$TOP/packaging/udev/71-lordos-guest.rules" ||
    { echo "FAIL: the udev rule doesn't give the port to the video group"; fails=$((fails + 1)); }
echo "migration 012: $n cases, $fails failed"
[ "$fails" = 0 ]
