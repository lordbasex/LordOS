# 0.8.9.6: in a LordOS Machines guest the agent port (org.lordos.agent) becomes the guest's administrators' (the sudo
# group) instead of the video group's, which every desktop account is in (security findings F9, F13; docs/VIRTUALIZATION.md
# 6.17 "LordOS as a guest"). The port opens only once and its holder gets the host's shell (as their own login shell),
# its clipboard and the files dropped on the machine, so a standard account that opened it first could read what the
# host's person typed there. The package's 71-lordos-guest.rules already says sudo; this applies it now:
#   - /etc/udev/rules.d/71-lordos-guest.rules, written by the LordOS Guest Tools disc (install.sh) or Machines' Install
#     Agent in this guest, overrides the package's rule under the same name: when it is exactly the old video-group line
#     it becomes the sudo-group line (any other content is someone's own and stays);
#   - only where the port exists (a Machines guest whose agent is on): udev reads the rules again and applies them to the
#     port, then each signed-in person's lordos-guest is stopped when they aren't an administrator (it lets the port go)
#     and restarted when they are (it takes it again). A program of a standard account other than lordos-guest that
#     holds the port keeps it until it closes it or the machine restarts.
# Anywhere else it does nothing. Running it again changes nothing. LORDOS_MIGRATION_ROOT is for tests only
# (server/tests/migration_guest_port_test.sh): the files are looked for and rewritten there, and no command runs.
R=${LORDOS_MIGRATION_ROOT:-}
OLD='SUBSYSTEM=="virtio-ports", ATTR{name}=="org.lordos.agent", TAG+="uaccess", GROUP="video", MODE="0660"'
NEW='SUBSYSTEM=="virtio-ports", ATTR{name}=="org.lordos.agent", TAG+="uaccess", GROUP="sudo", MODE="0660"'
E="$R/etc/udev/rules.d/71-lordos-guest.rules"
if [ -f "$E" ] && [ ! -L "$E" ] && [ "$(cat "$E")" = "$OLD" ]; then
    printf '%s\n' "$NEW" > "$E.new" && chmod 644 "$E.new" && mv -f "$E.new" "$E"
fi
[ -e "$R/dev/virtio-ports/org.lordos.agent" ] || exit 0
[ -z "$R" ] || { echo "would apply"; exit 0; }
[ -d /run/systemd/system ] || exit 0
udevadm control --reload >/dev/null 2>&1 || true
udevadm trigger --subsystem-match=virtio-ports >/dev/null 2>&1 || true
udevadm settle --timeout=5 >/dev/null 2>&1 || true
# the people who may no longer hold the port let it go first; then the administrators' helpers take it
for pass in stop restart; do
    for d in /run/user/*; do
        u=${d##*/}
        case $u in ''|*[!0-9]*) continue ;; esac
        [ "$u" -ge 1000 ] || continue
        n=$(id -nu "$u" 2>/dev/null) || continue
        admin=0
        case " $(id -nG "$n" 2>/dev/null) " in *" sudo "*) admin=1 ;; esac
        if [ "$pass" = stop ] && [ "$admin" = 0 ]; then
            systemctl --user -M "$n@" stop lordos-guest.service >/dev/null 2>&1 || true
        elif [ "$pass" = restart ] && [ "$admin" = 1 ]; then
            systemctl --user -M "$n@" restart lordos-guest.service >/dev/null 2>&1 || true
        fi
    done
done
exit 0
