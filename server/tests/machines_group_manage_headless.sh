#!/bin/bash
# machines_group_manage_headless.sh — a machine group's Delete, Rename and Repair through Machines' own calls
# (docs/VIRTUALIZATION.md 19.8, 19.9) in a headless lord-server: lord.vm.groups(), groupRenamePlan, groupRename,
# groupRepair and groupDelete run the real lord-vm jobs on a throwaway library (two groups made from a fixture archive,
# as tests/vm_group_manage.sh makes them; one with a member folder removed, the other with a base removed and its
# archive in the image store). tests/machines_group_manage_scene.js drives it and says "GMAN ok|FAIL …".
# lord-server runs in its own user and mount namespaces: an empty folder over the real home, a tmpfs over the real
# runtime folder, the repository's catalog over the installed one and a scratch folder over the image store. Nothing of
# the person's is seen or touched. Run from server/ after `make` and `npm run build`. Exit 77 when something is missing.
cd "$(dirname "$0")/.." || exit 1
ROOT=$(cd .. && pwd)
THEME=$ROOT/themes/default/theme.bundle.js
[ -x build/lord-vm-testpaths ] || make -s build/lord-vm-testpaths >/dev/null 2>&1
[ -x build/lord-server ] && [ -x build/lord-vm ] && [ -x build/lord-vm-testpaths ] && [ -x build/lord-vmunpack ] && [ -f "$THEME" ] || { echo "skipped: needs build/lord-server, lord-vm, lord-vm-testpaths, lord-vmunpack and the default theme"; exit 77; }
for t in qemu-img qemu-io xz tar python3 sha512sum unshare node; do command -v $t >/dev/null 2>&1 || { echo "skipped: needs $t"; exit 77; }; done
[ -r /dev/kvm ] && [ -w /dev/kvm ] || { echo "skipped: needs /dev/kvm"; exit 77; }
[ -f /usr/lib/lordos/assets/machines/systems.json ] && [ -d /var/lib/lordos/machines/images ] || { echo "skipped: needs the installed catalog and image store to mount over"; exit 77; }
unshare -Urm true 2>/dev/null || { echo "skipped: needs user and mount namespaces"; exit 77; }
D=$(mktemp -d "${TMPDIR:-/var/tmp}/lgmh.XXXXXX") || exit 1
chmod 700 "$D"
L="$D/home/Machines"
T="$ROOT/themes/zgman/default"
mkdir -p "$L" "$D/images" "$D/fx" "$D/fake" "$D/home/.config/lordos"
P=
cleanup() {
    [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 1; kill -9 "$P" 2>/dev/null; }
    chmod -R u+w "$D" 2>/dev/null; rm -rf "$D" "$ROOT/themes/zgman"
    node "$ROOT/tools/code-manifest.mjs" >/dev/null
}
trap cleanup EXIT
CAT=$(realpath ../assets/machines/systems.json)
E="env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR=$D HOME=$D/home LANG=C.UTF-8 LORDVM_TEST_CATALOG=$CAT LORDVM_TEST_IMAGES=$D/images"
REL=99.0.0.1
for r in Gateway Workstation; do
    qemu-img create -q -f qcow2 "$D/fx/Whonix-$r-LXQt-$REL.Intel_AMD64.qcow2" 1G || exit 77
done
(cd "$D/fx" && tar --format=gnu -cf - Whonix-Gateway-LXQt-$REL.Intel_AMD64.qcow2 Whonix-Workstation-LXQt-$REL.Intel_AMD64.qcow2) | xz -0 -T1 >"$D/archive.xz"
SHA=$(sha512sum "$D/archive.xz" | cut -d' ' -f1)
cp "$D/archive.xz" "$D/images/$SHA.tar.xz"
for n in Whonix Tor2; do
    $E ./build/lord-vm-testpaths --group-create "$L/$n.lordgroup" --catalog-id whonix --archive "$D/images/$SHA.tar.xz" --sha512 "$SHA" --release $REL --name $n >"$D/create.log" 2>&1 || { echo "FAIL: --group-create"; cat "$D/create.log"; exit 1; }
done
# the damage: Whonix's workstation folder, and the workstation base (both groups name it; Tor2's card shows it missing)
rm -rf "$L/Whonix.lordgroup/Workstation.lordvm"
chmod u+w "$L/.shared"; rm -f "$L/.shared/"*-workstation-*.qcow2
ln -s "$L/Tor2.lordgroup" "$L/Link.lordgroup"
# the theme with the scene after it
rm -rf "$ROOT/themes/zgman"; mkdir -p "$T"
cp -r "$ROOT/themes/default/." "$T/"
cat "$THEME" tests/machines_group_manage_scene.js >"$T/theme.bundle.js"
node tests/perf/manifest.mjs "$ROOT" "themes/zgman/default/theme.bundle.js" || exit 1
printf '{"schema": 9, "desktopChosen": true, "language": "en"}\n' >"$D/home/.config/lordos/settings.json"
REALHOME=$(getent passwd "$(id -u)" | cut -d: -f6)
RT=/run/user/$(id -u)
unshare -Urm sh -c "mount --bind '$D/fake' '$REALHOME' && { [ -d $RT ] && mount -t tmpfs -o mode=700 tmpfs $RT || true; } && mount --bind '$CAT' /usr/lib/lordos/assets/machines/systems.json && mount --bind '$D/images' /var/lib/lordos/machines/images && exec unshare --user --map-user=$(id -u) --map-group=$(id -g) \"\$0\" \"\$@\"" \
    env -u WAYLAND_DISPLAY -u DISPLAY -u LORD_SESSIOND -u LORDOS_SESSIOND -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$D" HOME="$D/home" LORD_GPU=0 LANG=en_US.UTF-8 \
    ./build/lord-server --headless 1280x800 --theme "$T/theme.bundle.js" --timeout 240 >"$D/server.log" 2>&1 &
P=$!
for i in $(seq 1 400); do grep -aq 'GMAN done\|GMAN FAIL error' "$D/home/.cache/lordos/shell.log" 2>/dev/null && break; sleep 0.5; done
out=$(grep -ah 'GMAN' "$D/home/.cache/lordos/shell.log" 2>/dev/null | sed 's/.*GMAN /  /')
echo "$out"
if echo "$out" | grep -q 'done' && ! echo "$out" | grep -q FAIL; then echo "machines_group_manage_headless: ok"; exit 0; fi
tail -20 "$D/server.log"
echo "machines_group_manage_headless: FAILED"
exit 1
