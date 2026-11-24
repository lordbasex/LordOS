# 0.8.9.6: LordOS as a Machines guest gets its agent working after the update (docs/VIRTUALIZATION.md 6.17, "LordOS as a
# guest"). The package's 71-lordos-guest.rules now gives the agent port (org.lordos.agent) to the video group as well
# as to logind's uaccess (LordOS's own sessions aren't logind seats, so before this nobody could open it and Machines
# waited for the agent forever), and lordos-guest.service may be new to this system. Only inside a Machines guest whose
# agent is on (the port exists): udev reads the rule again and applies it to the port now, and each signed-in person's
# user manager reads its units again and starts lordos-guest, so the update doesn't wait for a restart. qemu-guest-agent
# comes as a Recommends of the package (the Store installs new Recommends); Debian's udev rule starts it at the next
# boot, and here at once when it's installed. Anywhere else (a computer, a machine without the agent) it does nothing.
# Running it again changes nothing. LORDOS_MIGRATION_ROOT is for tests only (server/tests/migration_guest_port_test.sh):
# the port is looked for there, and no command runs.
R=${LORDOS_MIGRATION_ROOT:-}
[ -e "$R/dev/virtio-ports/org.lordos.agent" ] || exit 0
[ -z "$R" ] || { echo "would apply"; exit 0; }
[ -d /run/systemd/system ] || exit 0
udevadm control --reload >/dev/null 2>&1 || true
udevadm trigger --subsystem-match=virtio-ports >/dev/null 2>&1 || true
udevadm settle --timeout=5 >/dev/null 2>&1 || true
if [ -e /dev/virtio-ports/org.qemu.guest_agent.0 ]; then systemctl start qemu-guest-agent.service >/dev/null 2>&1 || true; fi
for d in /run/user/*; do
    u=${d##*/}
    case $u in ''|*[!0-9]*) continue ;; esac
    [ "$u" -ge 1000 ] || continue
    n=$(id -nu "$u" 2>/dev/null) || continue
    systemctl --user -M "$n@" daemon-reload >/dev/null 2>&1 || continue
    systemctl --user -M "$n@" restart lordos-guest.service >/dev/null 2>&1 || true
done
exit 0
