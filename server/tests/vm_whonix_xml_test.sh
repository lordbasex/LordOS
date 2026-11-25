#!/bin/bash
# vm_whonix_xml_test.sh — each Whonix member's QEMU and passt arguments against Whonix's own libvirt XML of the pinned
# release (docs/VIRTUALIZATION.md 19.5, 19.10; the XMLs are fixtures in tests/vmimage/). A group is made by the test
# build's lord-vm --group-create from two tiny empty qcow2 disks (no Whonix image needed), each member is started paused
# (no guest code runs) and powered off again, and tests/vm_whonix_xml.py reads the command lines lord-vm logged: every
# element of the XML is mirrored or a known, deliberate difference; anything new fails, so a catalog move is reviewed.
# Exit 77 without KVM, QEMU, qemu-img, passt, unshare or python3.
# lord-vm runs in its own user and mount namespaces with a private home, as tests/vm_group_link.sh does.
cd "$(dirname "$0")/.." || exit 1
[ -x build/lord-vm-testpaths ] || make -s build/lord-vm-testpaths >/dev/null 2>&1
[ -x build/lord-vm-testpaths ] || { echo "skipped: needs build/lord-vm-testpaths"; exit 77; }
for t in qemu-system-x86_64 qemu-img passt unshare python3; do command -v $t >/dev/null 2>&1 || { echo "skipped: needs $t"; exit 77; }; done
[ -r /dev/kvm ] && [ -w /dev/kvm ] || { echo "skipped: needs KVM"; exit 77; }
REL=18.2.4.3
D=$(mktemp -d "${TMPDIR:-/var/tmp}/lwxml.XXXXXX") || exit 1
chmod 700 "$D"
mkdir -p "$D/home/Machines" "$D/run" "$D/fake"
chmod 700 "$D/run" "$D/home"
REALHOME=$(getent passwd "$(id -u)" | cut -d: -f6)
RT=/run/user/$(id -u)
E="env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR=$D/run HOME=$D/home LANG=C.UTF-8"
NS="unshare -Urm sh -c 'mount --bind $D/fake $REALHOME && { [ -d $RT ] && mount -t tmpfs -o mode=700 tmpfs $RT || true; } && exec unshare --user --map-user=$(id -u) --map-group=$(id -g) \"\$0\" \"\$@\"'"
ns() { eval "$NS" '"$@"'; }
G="$D/home/Machines/Whonix.lordgroup"
cleanup() { for m in Workstation Gateway; do ns $E ./build/lord-vm-testpaths --poweroff "$G/$m.lordvm" >/dev/null 2>&1; done; rm -rf "$D"; }
trap cleanup EXIT
R=0
qemu-img create -q -f qcow2 "$D/gw.qcow2" 1G && qemu-img create -q -f qcow2 "$D/ws.qcow2" 1G || { echo "FAIL: qemu-img create"; exit 1; }
ns $E LORDVM_TEST_CATALOG="$(realpath ../assets/machines/systems.json)" ./build/lord-vm-testpaths --group-create "$G" --catalog-id whonix \
    --dev-gateway "$D/gw.qcow2" --dev-workstation "$D/ws.qcow2" >"$D/create.log" 2>&1 || { echo "FAIL: lord-vm --group-create"; cat "$D/create.log"; exit 1; }
for m in Gateway Workstation; do
    role=$(echo "$m" | tr 'A-Z' 'a-z')
    ns $E ./build/lord-vm-testpaths --start "$G/$m.lordvm" --paused --detach >"$D/$m.start" 2>&1
    for i in $(seq 1 40); do grep -q "qemu: pid" "$G/$m.lordvm/log/lord-vm.log" 2>/dev/null && break; sleep 0.5; done
    grep -q "qemu: pid" "$G/$m.lordvm/log/lord-vm.log" 2>/dev/null || { echo "FAIL: $role didn't start: $(tail -3 "$G/$m.lordvm/log/lord-vm.log" 2>/dev/null) $(cat "$D/$m.start")"; R=1; continue; }
    python3 -I tests/vm_whonix_xml.py "$role" "tests/vmimage/whonix-$REL-$m.xml" "$G/$m.lordvm/log/lord-vm.log" || R=1
    ns $E ./build/lord-vm-testpaths --poweroff "$G/$m.lordvm" >/dev/null 2>&1
done
exit $R
