#!/bin/sh
# migration_guest_port_test.sh — packaging/migrations/012-0.8.9.6-guest-agent-port.sh and 013-0.8.9.6-guest-agent-admins.sh over fake system trees
# (LORDOS_MIGRATION_ROOT): only a tree with /dev/virtio-ports/org.lordos.agent (a Machines guest whose agent is on) is
# acted on; a computer without the port, or with only qemu-guest-agent's, is left alone; it passes `sh -eu` (as
# packaging/migrations/migrate runs it) twice. The package's udev rule gives the port to the sudo group and no rule to video
# (F9, F13); 013 rewrites only the exact old video-group rule in /etc. Run
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
grep -q '^SUBSYSTEM=="virtio-ports", ATTR{name}=="org.lordos.agent", TAG+="uaccess", GROUP="sudo", MODE="0660"$' "$TOP/packaging/udev/71-lordos-guest.rules" ||
    { echo "FAIL: the udev rule doesn't give the port to the guest's administrators (sudo)"; fails=$((fails + 1)); }
# F9, F13: no rule gives the port to a group every desktop account is in
for f in "$TOP/packaging/udev/71-lordos-guest.rules" "$TOP/tools/guest/install.sh" "$TOP/server/src/vm/lordvm.c"; do
    n=$((n + 1))
    if grep -q 'GROUP=\\\{0,1\}"video' "$f"; then echo "FAIL: $f still gives the agent port to the video group"; fails=$((fails + 1)); fi
done

# migration 013: the old video-group rule an earlier disc or Install Agent wrote in /etc becomes the sudo-group one;
# any other content stays; the port decides whether anything else would run; `sh -eu`, twice
M13=$TOP/packaging/migrations/013-0.8.9.6-guest-agent-admins.sh
OLD='SUBSYSTEM=="virtio-ports", ATTR{name}=="org.lordos.agent", TAG+="uaccess", GROUP="video", MODE="0660"'
NEW='SUBSYSTEM=="virtio-ports", ATTR{name}=="org.lordos.agent", TAG+="uaccess", GROUP="sudo", MODE="0660"'
OWN='SUBSYSTEM=="virtio-ports", ATTR{name}=="org.lordos.agent", OWNER="alex", MODE="0600"'
UA='SUBSYSTEM=="virtio-ports", ATTR{name}=="org.lordos.agent", TAG+="uaccess"'
# case13 NAME PORTS ETC-RULE ("" none) EXPECTED-OUTPUT EXPECTED-RULE
case13() {
    n=$((n + 1))
    R="$T/m13-$1"
    mkdir -p "$R/dev/virtio-ports" "$R/etc/udev/rules.d"
    for p in $2; do : > "$R/dev/virtio-ports/$p"; done
    [ -z "$3" ] || printf '%s\n' "$3" > "$R/etc/udev/rules.d/71-lordos-guest.rules"
    for run in 1 2; do
        out=$(LORDOS_MIGRATION_ROOT=$R sh -eu "$M13") || { echo "FAIL 013 $1: run $run exited with $?"; fails=$((fails + 1)); return; }
        [ "$out" = "$4" ] || { echo "FAIL 013 $1: run $run said '$out', expected '$4'"; fails=$((fails + 1)); return; }
        got=$(cat "$R/etc/udev/rules.d/71-lordos-guest.rules" 2>/dev/null || true)
        [ "$got" = "$5" ] || { echo "FAIL 013 $1: run $run left the rule '$got', expected '$5'"; fails=$((fails + 1)); return; }
    done
}
case13 guest-old-rule "org.lordos.agent" "$OLD" "would apply" "$NEW"
case13 guest-no-etc-rule "org.lordos.agent" "" "would apply" ""
case13 agent-off-old-rule "" "$OLD" "" "$NEW"
case13 own-rule-kept "org.lordos.agent" "$OWN" "would apply" "$OWN"
case13 other-guest-rule-kept "org.lordos.agent" "$UA" "would apply" "$UA"
case13 computer "" "" "" ""
echo "migrations 012, 013: $n cases, $fails failed"
[ "$fails" = 0 ]
