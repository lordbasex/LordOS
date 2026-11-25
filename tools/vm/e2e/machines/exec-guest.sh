#!/bin/bash
# exec-guest.sh — runs server/tests/vm_exec_e2e.sh (machines exec, cp of files and folders, logs, mcp, apply) against a
# throwaway machine made from the harness's prepared Debian 13 image (images.py: qemu-guest-agent, the person "tester"),
# with automation on, inside ns-run.sh's private home and namespaces: never the person's library or session.
#   bash tools/vm/e2e/machines/exec-guest.sh          (from the repository; KVM, the build, the image: python3 -I images.py)
set -u
REPO=$(cd "$(dirname "$0")/../../../.." && pwd)
IMG=$(cd "$REPO" && python3 -I -c 'import sys; sys.path.insert(0, "tools/vm/e2e/machines"); import images; print(images.agent_path())')
[ -f "$IMG" ] && [ -x "$REPO/server/build/lord-vm" ] && [ -x "$REPO/server/build/machines" ] && [ -r /dev/kvm ] || { echo "skipped: needs KVM, the build and $IMG"; exit 77; }
if [ -z "${MH_NS_DIR:-}" ]; then exec sh "$REPO/tools/vm/e2e/machines/ns-run.sh" bash "$0" "$@"; fi
LV=$REPO/server/build/lord-vm
M=$HOME/Machines/exec-e2e.lordvm
mkdir -p "$HOME/Machines"
"$LV" --create "$M" --name exec-e2e --import "$IMG" --vcpus 2 --memory 1536 --agent on --network isolated >/dev/null || { echo "FAIL: create"; exit 1; }
"$LV" --set "$M" --automation on >/dev/null || { echo "FAIL: automation on"; exit 1; }
"$LV" --start "$M" --detach --keep-running >/dev/null 2>&1 || { echo "FAIL: start"; tail -5 "$M/log/lord-vm.log"; exit 1; }
for _ in $(seq 1 180); do "$LV" --status "$M" --json | grep -q '"agent":"connected"\|"agent":2' && break; grep -aq "agent: connected" "$M/log/lord-vm.log" && break; sleep 1; done
grep -aq "agent: connected" "$M/log/lord-vm.log" || { echo "FAIL: qemu-guest-agent never answered"; tail -5 "$M/log/lord-vm.log"; "$LV" --poweroff "$M"; exit 1; }
VM_EXEC_NAME=exec-e2e VM_EXEC_USER=tester bash "$REPO/server/tests/vm_exec_e2e.sh"
rc=$?
"$LV" --poweroff "$M" >/dev/null 2>&1
exit $rc
